#include "game/Notices.h"

#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/ChatLinks.h"
#include "game/Entities.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "game/ItemTooltip.h"
#include "game/SocialText.h"
#include "net/TravelMessages.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd::game {
namespace {

namespace uf = net::uf;
using K = GameEventKind;

constexpr u32 kAttrTradeskill = 0x20;   // SPELL_ATTR_IS_TRADESKILL: a recipe
constexpr u32 kAttrDoNotDisplay = 0x80; // SPELL_ATTR_DO_NOT_DISPLAY: not in the spell book, not announced

// Talent points a level gives (Player::CalculateTalentsPoints: one a level from 10).
u32 talentPointsAt(u32 level) {
    return level >= 10 ? level - 9 : 0;
}

} // namespace

NoticePlace noticePlace(NoticeKind kind) {
    switch (kind) {
    case NoticeKind::System:
    case NoticeKind::Loot:
    case NoticeKind::Skill:
    case NoticeKind::Money: return NoticePlace::General; // ChatTypeGroup LOOT holds CHAT_MSG_MONEY
    case NoticeKind::XpGain:
    case NoticeKind::FactionChange:
    case NoticeKind::HonorGain: return NoticePlace::CombatLog; // phase 17 (honor): COMBAT_HONOR_GAIN's group
    case NoticeKind::Info:
    case NoticeKind::Error: return NoticePlace::ErrorsFrame;
    }
    return NoticePlace::General;
}

NoticeColor noticeColor(NoticeKind kind) {
    switch (kind) {
    case NoticeKind::System: return {255, 255, 0};
    case NoticeKind::Loot: return {0, 170, 0};
    case NoticeKind::Skill: return {85, 85, 255};
    case NoticeKind::Money: return {255, 255, 0};
    case NoticeKind::XpGain: return {111, 111, 255};
    case NoticeKind::FactionChange: return {128, 128, 255};
    case NoticeKind::Info: return {255, 255, 0};
    case NoticeKind::Error: return {255, 26, 26};
    case NoticeKind::HonorGain: return {224, 202, 10}; // phase 17 (honor)
    }
    return {};
}

std::string_view moneyText(std::span<char> out, u32 copper) {
    if (out.empty()) {
        return {};
    }
    const u32 coins[3] = {copper / 10000, copper / 100 % 100, copper % 100};
    static constexpr text::Key kNames[3] = {"GOLD", "SILVER", "COPPER"};
    usize used = 0;
    out[0] = 0;
    for (u32 i = 0; i < 3; ++i) {
        if (coins[i] == 0 && !(i == 2 && copper == 0)) {
            continue;
        }
        const std::string_view name = text::get(kNames[i]);
        // Decoration only (the count, the coin's name from the table, the separator), as WoW.exe's "%d %s".
        const int n = std::snprintf(out.data() + used, out.size() - used, "%s%u %.*s", used > 0 ? ", " : "",
                                    coins[i], static_cast<int>(name.size()), name.data());
        used = std::min(used + static_cast<usize>(std::max(n, 0)), out.size() - 1);
    }
    return {out.data(), used};
}

// --- NoticeFeed -----------------------------------------------------------------------------------------

void NoticeFeed::init(const GameData* data, const AbilityData* abilities) {
    m_data = data;
    m_abilities = abilities && abilities->ready() ? abilities : nullptr;
}

void NoticeFeed::restart(const Gameplay& play) {
    m_cursor = play.events().end();
    m_waitFirst = m_waitCount = 0;
    m_skillsKnown = false;
}

bool NoticeFeed::wanted(GameEventKind kind) {
    switch (kind) {
    case K::ItemReceived:
    case K::MemberItemReceived:
    case K::MoneyLooted:
    case K::XpGained:
    case K::LevelUp:
    case K::SpellLearned:
    case K::ReputationChanged:
    case K::QuestAccepted:
    case K::QuestRewarded:
    case K::InstanceSaved:
    case K::InstanceResetWarning:
    case K::InstanceReset:
    case K::InstanceResetFailed:
    case K::RaidGroupOnly:
    case K::SavedInstancesUpdated:
    case K::ReadyCheckNotice: // phase 16 (alerts)
    case K::MeetingStoneNotice:
    case K::HonorGained: // phase 17 (honor)
    case K::PlayerSkinned: return true;
    case K::BattlefieldNotice: return true; // phase 17 (queue)
    case K::BattlegroundPlayer: return true; // phase 17 (scores)
    default: return false;
    }
}

bool NoticeFeed::ready(Gameplay& play, const GameEvent& e) const {
    QueryCache& q = play.queries();
    switch (e.kind) {
    case K::ItemReceived: return e.other != 0 || q.item(e.id) != nullptr; // asks while missing
    case K::MemberItemReceived: {
        const bool item = q.item(e.id) != nullptr;
        return q.playerName(e.guid) != nullptr && item;
    }
    case K::QuestAccepted: return play.quest(e.id) != nullptr;
    case K::ReadyCheckNotice: // phase 16 (alerts): the names they need
    case K::MeetingStoneNotice: return raidAlertReady(play, e);
    case K::HonorGained: return pvpReady(play, e); // phase 17 (honor): the victim's name
    case K::BattlegroundPlayer: return q.playerName(e.guid) != nullptr; // phase 17 (scores): its name query
    case K::QuestRewarded: {
        bool known = play.quest(e.id) != nullptr;
        const net::QuestComplete& reward = play.lastQuestComplete();
        for (u32 i = 0; reward.questId == e.id && i < reward.itemCount; ++i) {
            known = q.item(reward.items[i].itemId) != nullptr && known;
        }
        return known;
    }
    default: return true;
    }
}

std::span<char> NoticeFeed::push(NoticeKind kind) {
    Notice& n = m_lines.push();
    n.kind = kind;
    n.text[0] = 0;
    return n.text;
}

std::string_view NoticeFeed::itemLink(Gameplay& play, u32 entry, std::span<char> out) const {
    const net::ItemQueryInfo* info = play.queries().findItem(entry);
    const std::string_view name = info && !info->name.empty() ? std::string_view(info->name) : "...";
    return formatItemLink(out, entry, itemQualityColor(static_cast<u8>(info ? info->quality : 1)), name);
}

void NoticeFeed::update(Gameplay& play, const Entity* player, const CombatNames& names, u32 nowMs) {
    while (const GameEvent* e = play.events().next(m_cursor)) {
        if (!wanted(e->kind)) {
            continue;
        }
        if (m_waitCount == m_waiting.size()) { // full: the oldest goes out as it is
            emit(play, m_waiting[m_waitFirst].event, player, names);
            m_waitFirst = (m_waitFirst + 1) % m_waiting.size();
            --m_waitCount;
        }
        m_waiting[(m_waitFirst + m_waitCount++) % m_waiting.size()] = {*e, nowMs};
    }
    while (m_waitCount > 0) {
        const Waiting& w = m_waiting[m_waitFirst];
        if (!ready(play, w.event) && nowMs - w.sinceMs < kNameWaitMs) {
            break; // the lines behind it wait too
        }
        emit(play, w.event, player, names);
        m_waitFirst = (m_waitFirst + 1) % m_waiting.size();
        --m_waitCount;
    }
    if (player) {
        skillUps(*player);
    }
}

void NoticeFeed::emit(Gameplay& play, const GameEvent& e, const Entity* player, const CombatNames& names) {
    char link[128];
    switch (e.kind) {
    case K::ItemReceived: {
        // SMSG_ITEM_PUSH_RESULT: looted, received from an NPC (bought, a quest's item), or created. Quest
        // rewards come with showInChat off: the quest's own lines name them.
        if (e.other != 0) {
            break;
        }
        const bool several = e.value[0] > 1;
        const text::Key key =
            e.value[2] != 0
                ? text::Key(several ? "LOOT_ITEM_CREATED_SELF_MULTIPLE" : "LOOT_ITEM_CREATED_SELF")
            : e.value[1] != 0
                ? text::Key(several ? "LOOT_ITEM_PUSHED_SELF_MULTIPLE" : "LOOT_ITEM_PUSHED_SELF")
                : text::Key(several ? "LOOT_ITEM_SELF_MULTIPLE" : "LOOT_ITEM_SELF");
        text::format(push(NoticeKind::Loot), key, itemLink(play, e.id, link), e.value[0]);
        break;
    }
    case K::MemberItemReceived: {
        const std::string* who = play.queries().findPlayerName(e.guid);
        const std::string_view name = who ? std::string_view(*who) : std::string_view("...");
        const bool several = e.value[0] > 1;
        const text::Key key = e.value[2] != 0 ? text::Key(several ? "CREATED_ITEM_MULTIPLE" : "CREATED_ITEM")
                                              : text::Key(several ? "LOOT_ITEM_MULTIPLE" : "LOOT_ITEM");
        text::format(push(NoticeKind::Loot), key, name, itemLink(play, e.id, link), e.value[0]);
        break;
    }
    case K::MoneyLooted: {
        // Alone the loot window's money (LootFrame.cpp YOU_LOOT_MONEY); in a group the share
        // (LOOT_MONEY_SPLIT).
        char money[96];
        moneyText(money, static_cast<u32>(std::max(e.value[0], 0)));
        text::format(push(NoticeKind::Money), e.value[1] != 0 ? "LOOT_MONEY_SPLIT" : "YOU_LOOT_MONEY",
                     std::string_view(money));
        break;
    }
    case K::XpGained: {
        // The rest state the player is in when the line is written (0x637B0C), as Exhaustion.dbc has it.
        ExperienceGain xp{e.guid, e.id == 1, e.value[0], e.value[1], std::bit_cast<f32>(e.value[2])};
        xp.restState = player ? player->byteValue(uf::PLAYER_BYTES_2, 3) : 0;
        if (m_data) {
            const RestStateInfo rest = m_data->restState(xp.restState);
            xp.restFactor = rest.factor;
            xp.restName = rest.name;
        }
        const std::span<char> out = push(NoticeKind::XpGain);
        formatExperience(out.data(), out.size(), xp, names);
        break;
    }
    case K::LevelUp: levelUp(play, e); break;
    case K::SpellLearned: {
        const std::optional<SpellInfo> spell = m_data ? m_data->spell(e.id) : std::nullopt;
        if (!spell || (spell->attributes & kAttrDoNotDisplay) != 0 || spell->name.empty()) {
            break;
        }
        const text::Key key = (spell->attributes & kAttrTradeskill) != 0 ? text::Key("ERR_LEARN_RECIPE_S")
                              : spell->passive()                         ? text::Key("ERR_LEARN_ABILITY_S")
                                                                         : text::Key("ERR_LEARN_SPELL_S");
        text::format(push(NoticeKind::System), key, spell->name);
        break;
    }
    case K::ReputationChanged: {
        const FactionInfo* faction = m_abilities ? m_abilities->factionByList(e.id) : nullptr;
        if (!faction) {
            break;
        }
        text::format(push(NoticeKind::FactionChange),
                     e.value[0] > 0 ? "FACTION_STANDING_INCREASED" : "FACTION_STANDING_DECREASED",
                     faction->name, std::abs(e.value[0]));
        if (player) { // a new rank: "You are now Friendly with Stormwind."
            const u32 race = player->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
            const u32 cls = player->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
            const i32 now = faction->baseReputation(race, cls) + e.value[1];
            const u32 rank = reputationRank(now);
            if (rank != reputationRank(now - e.value[0])) {
                text::format(push(NoticeKind::FactionChange), "FACTION_STANDING_CHANGED",
                             reputationRankName(rank), faction->name);
            }
        }
        break;
    }
    case K::QuestAccepted: {
        const QuestInfo* quest = play.quest(e.id);
        text::format(push(NoticeKind::System), "ERR_QUEST_ACCEPTED_S",
                     quest ? std::string_view(quest->title) : std::string_view("..."));
        break;
    }
    case K::QuestRewarded: questRewarded(play, e); break;
    case K::InstanceSaved:
    case K::InstanceResetWarning:
    case K::InstanceReset:
    case K::InstanceResetFailed:
    case K::RaidGroupOnly:
    case K::SavedInstancesUpdated: instanceLine(play, e); break;
    case K::ReadyCheckNotice: // phase 16 (alerts)
    case K::MeetingStoneNotice: raidAlertLine(play, e); break;
    case K::HonorGained: // phase 17 (honor)
    case K::PlayerSkinned: pvpLine(play, e, player, names); break;
    case K::BattlefieldNotice: battlefieldLine(play, e); break; // phase 17 (queue)
    case K::BattlegroundPlayer: battlegroundPlayerLine(play, e); break; // phase 17 (scores)
    default: break;
    }
}

void NoticeFeed::instanceLine(Gameplay& play, const GameEvent& e) {
    // Phase 12: WoW.exe's own lines (CHAT_MSG_SYSTEM) for the instance packets, with Map.dbc's name of the map
    // (its number when unknown), and ChatFrame.lua's NO_RAID_INSTANCES_SAVED for a /raidinfo with nothing saved.
    char number[16];
    const AreaData* areas = play.travel().areas();
    const MapInfo* map = areas ? areas->map(e.id) : nullptr;
    std::string_view name = map ? map->name : std::string_view{};
    if (name.empty()) {
        const int written = std::snprintf(number, sizeof(number), "%u", e.id);
        name = std::string_view(number, static_cast<usize>(std::max(written, 0)));
    }
    switch (e.kind) {
    case K::InstanceSaved: text::format(push(NoticeKind::System), "INSTANCE_SAVED"); break;
    case K::InstanceResetWarning: {
        // The time as WoW.exe cuts it: whole hours, whole minutes; GetText's plural (_P1) unless it is one.
        const u32 seconds = static_cast<u32>(std::max(e.value[1], 0));
        const std::span<char> out = push(NoticeKind::System);
        switch (static_cast<u32>(e.value[0])) {
        case net::kRaidInstanceWarningHours: {
            const u32 hours = seconds / 3600;
            text::format(out, hours == 1 ? "RAID_INSTANCE_WARNING_HOURS" : "RAID_INSTANCE_WARNING_HOURS_P1", name,
                         hours);
            break;
        }
        case net::kRaidInstanceWarningMin:
        case net::kRaidInstanceWarningMinSoon: {
            const u32 minutes = seconds / 60;
            const bool soon = e.value[0] == static_cast<i32>(net::kRaidInstanceWarningMinSoon);
            const text::Key key = soon ? text::Key(minutes == 1 ? "RAID_INSTANCE_WARNING_MIN_SOON"
                                                                : "RAID_INSTANCE_WARNING_MIN_SOON_P1")
                                       : text::Key(minutes == 1 ? "RAID_INSTANCE_WARNING_MIN"
                                                                : "RAID_INSTANCE_WARNING_MIN_P1");
            text::format(out, key, name, minutes);
            break;
        }
        default: // the welcome: days, hours and minutes left
            text::format(out, "RAID_INSTANCE_WELCOME", name, seconds / 86400, seconds % 86400 / 3600,
                         seconds % 3600 / 60);
            break;
        }
        break;
    }
    case K::InstanceReset: text::format(push(NoticeKind::System), "INSTANCE_RESET_SUCCESS", name); break;
    case K::InstanceResetFailed: {
        static constexpr const char* kReasons[] = {"INSTANCE_RESET_FAILED", "INSTANCE_RESET_FAILED_OFFLINE",
                                                   "INSTANCE_RESET_FAILED_ZONING"};
        const u32 reason = static_cast<u32>(e.value[0]);
        if (reason < std::size(kReasons)) { // others: silently, as the server means them
            text::format(push(NoticeKind::System), kReasons[reason], name);
        }
        break;
    }
    case K::RaidGroupOnly:
        text::format(push(NoticeKind::Error), e.value[0] == static_cast<i32>(net::kRaidGroupFull)
                                                  ? "ERR_RAID_GROUP_FULL"
                                                  : "ERR_RAID_GROUP_ONLY");
        break;
    case K::SavedInstancesUpdated:
        if (e.value[0] != 0 && play.travel().savedInstances().empty()) {
            text::format(push(NoticeKind::System), "NO_RAID_INSTANCES_SAVED");
        }
        break;
    default: break;
    }
}

void NoticeFeed::levelUp(Gameplay& play, const GameEvent& e) {
    // ChatFrame.lua PLAYER_LEVEL_UP: the level, health (and mana), talent points, then each stat that rose.
    const u32 level = static_cast<u32>(e.value[0]);
    text::format(push(NoticeKind::System), "LEVEL_UP", level);
    if (e.value[2] > 0) {
        text::format(push(NoticeKind::System), "LEVEL_UP_HEALTH_MANA", e.value[1], e.value[2]);
    } else {
        text::format(push(NoticeKind::System), "LEVEL_UP_HEALTH", e.value[1]);
    }
    const u32 previous = e.id; // the level the player had when the packet came (0: unknown)
    const u32 points =
        previous != 0 && level > previous ? talentPointsAt(level) - talentPointsAt(previous) : 0;
    if (points > 0) {
        text::format(push(NoticeKind::System),
                     points == 1 ? "LEVEL_UP_CHAR_POINTS" : "LEVEL_UP_CHAR_POINTS_P1", points);
    }
    const net::LevelUpInfo& info = play.lastLevelUp();
    if (info.level != level) {
        return; // a later level-up already replaced it
    }
    static constexpr text::Key kStats[] = {"SPELL_STAT0_NAME", "SPELL_STAT1_NAME", "SPELL_STAT2_NAME",
                                           "SPELL_STAT3_NAME", "SPELL_STAT4_NAME"};
    for (u32 i = 0; i < info.statGains.size(); ++i) {
        const i32 gain = static_cast<i32>(info.statGains[i]); // the server's int32 difference
        if (gain > 0) {
            text::format(push(NoticeKind::System), "LEVEL_UP_STAT", text::get(kStats[i]), gain);
        }
    }
}

void NoticeFeed::questRewarded(Gameplay& play, const GameEvent& e) {
    // SMSG_QUESTGIVER_QUEST_COMPLETE: the quest, its experience, its money and the items it gives (the chosen
    // one comes as an item push the server keeps out of the chat).
    const QuestInfo* quest = play.quest(e.id);
    text::format(push(NoticeKind::System), "ERR_QUEST_COMPLETE_S",
                 quest ? std::string_view(quest->title) : std::string_view("..."));
    if (e.value[0] > 0) {
        text::format(push(NoticeKind::System), "ERR_QUEST_REWARD_EXP_I", e.value[0]);
    }
    if (e.value[1] > 0) {
        char money[96];
        text::format(push(NoticeKind::System), "ERR_QUEST_REWARD_MONEY_S",
                     moneyText(money, static_cast<u32>(e.value[1])));
    }
    const net::QuestComplete& reward = play.lastQuestComplete();
    for (u32 i = 0; reward.questId == e.id && i < reward.itemCount; ++i) {
        char link[128];
        text::format(push(NoticeKind::System), "ERR_QUEST_REWARD_ITEM_S",
                     itemLink(play, reward.items[i].itemId, link));
    }
}

void NoticeFeed::skillUps(const Entity& player) {
    // CHAT_MSG_SKILL: a skill whose value rose while its maximum stayed (use); a level or a trainer raising
    // the maximum (and the value of the skills that always follow it) says nothing.
    for (u32 slot = 0; slot < kSkillSlots; ++slot) {
        const u16 base = static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + slot * 3);
        const u32 id = player.value(base) & 0xFFFFu;
        const u32 values = player.value(static_cast<u16>(base + 1));
        if (m_skillsKnown && id != 0 && id == m_skillIds[slot] && values != m_skillValues[slot]) {
            const u32 value = values & 0xFFFFu, max = values >> 16;
            const u32 before = m_skillValues[slot] & 0xFFFFu, maxBefore = m_skillValues[slot] >> 16;
            SkillLineInfo skill;
            if (value > before && max == maxBefore && m_abilities && m_abilities->skillLine(id, skill)) {
                text::format(push(NoticeKind::Skill), "ERR_SKILL_UP_SI", skill.name, value);
            }
        }
        m_skillIds[slot] = id;
        m_skillValues[slot] = values;
    }
    // A player without its fields yet has no skills: the first ones seen are where it starts.
    m_skillsKnown = m_skillsKnown || player.values.size() > uf::PLAYER_SKILL_INFO_1_1;
}

} // namespace vd::game
