#include "game/Honor.h"

#include "core/Text.h"
#include "game/AreaData.h"
#include "game/Chat.h"
#include "game/Reactions.h"
#include "game/Social.h"
#include "game/Travel.h"
#include "game/UnitInfo.h"
#include "net/PvpMessages.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace vd::game {
namespace {

using net::Opcode;
namespace uf = net::uf;

// ChatChannels.dbc's names of the two defense channels (enUS, as the zone channels are joined): "LocalDefense
// - %s" up to its zone, "WorldDefense" whole.
constexpr std::string_view kLocalDefensePrefix = "LocalDefense - ";
constexpr std::string_view kWorldDefense = "WorldDefense";

bool startsWithNoCase(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), s.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

// GetText of a title key: the female form for a female player when the table has one, else the key's own;
// empty when the table has neither (a rank without a title).
std::string_view titleText(std::span<char> keyBuffer, usize keyLength, bool female) {
    if (female && keyLength + 7 < keyBuffer.size()) {
        std::snprintf(keyBuffer.data() + keyLength, keyBuffer.size() - keyLength, "_FEMALE");
        const text::Key key(std::string_view(keyBuffer.data(), keyLength + 7));
        if (text::has(key)) {
            return text::get(key);
        }
        keyBuffer[keyLength] = '\0';
    }
    const text::Key key(std::string_view(keyBuffer.data(), keyLength));
    return text::has(key) ? text::get(key) : std::string_view{};
}

} // namespace

u32 defenseChannelKind(std::string_view name) {
    if (startsWithNoCase(name, kLocalDefensePrefix)) {
        return kChannelLocalDefense;
    }
    if (name.size() == kWorldDefense.size() && startsWithNoCase(name, kWorldDefense)) {
        return kChannelWorldDefense;
    }
    return 0;
}

u32 defenseChannelKind(const ChatChannel& channel) {
    if (channel.zoneChannel == kChannelLocalDefense || channel.zoneChannel == kChannelWorldDefense) {
        return channel.zoneChannel;
    }
    return channel.zoneChannel == 0 ? defenseChannelKind(channel.name) : 0;
}

std::string_view honorRankTitle(std::span<char> keyBuffer, const GameEvent& credit, bool playerFemale) {
    if (keyBuffer.size() < 32) {
        return {};
    }
    int n = 0;
    switch (static_cast<HonorCredit>(credit.value[1])) {
    case HonorCredit::Awarded: return {};
    case HonorCredit::Dishonorable:
        n = std::snprintf(keyBuffer.data(), keyBuffer.size(), "PVP_RANK_CIVILIAN");
        break;
    case HonorCredit::Honorable:
        // 'PVP_RANK_%d_%d' (0x62532C): the victim's rank and side.
        n = std::snprintf(keyBuffer.data(), keyBuffer.size(), "PVP_RANK_%d_%d", static_cast<i32>(credit.id),
                          credit.value[2]);
        break;
    }
    return n > 0 ? titleText(keyBuffer, static_cast<usize>(n), playerFemale) : std::string_view{};
}

std::string_view formatHonorLine(std::span<char> out, const GameEvent& credit, std::string_view victimName,
                                 bool playerFemale) {
    char key[48];
    const std::string_view title = honorRankTitle(key, credit, playerFemale);
    switch (static_cast<HonorCredit>(credit.value[1])) {
    case HonorCredit::Awarded: return text::format(out, "COMBATLOG_HONORAWARD", credit.value[0]);
    case HonorCredit::Honorable:
        return text::format(out, "COMBATLOG_HONORGAIN", victimName, title, credit.value[0]);
    case HonorCredit::Dishonorable:
        return text::format(out, "COMBATLOG_DISHONORGAIN", victimName, title, credit.value[0]);
    }
    return {};
}

std::string_view formatHonorWorldText(std::span<char> out, const GameEvent& credit, bool playerFemale) {
    if (out.empty() || static_cast<HonorCredit>(credit.value[1]) == HonorCredit::Awarded) {
        return {};
    }
    char key[48];
    const std::string_view title = honorRankTitle(key, credit, playerFemale);
    const std::string_view kill = text::get(credit.value[0] > 0 ? "HK" : "DK");
    // WoW.exe's own "%s: %s" (0x85EAC4) around the two texts.
    const int n = std::snprintf(out.data(), out.size(), "%.*s: %.*s", static_cast<int>(kill.size()),
                                kill.data(), static_cast<int>(title.size()), title.data());
    return {out.data(), static_cast<usize>(std::clamp(n, 0, static_cast<int>(out.size()) - 1))};
}

Honor::Honor(const EntityStore& entities, const Travel& travel, const Social& social, ChatLog& chat,
             Events& events)
    : m_entities(entities), m_travel(travel), m_social(social), m_chat(chat), m_events(events) {}

void Honor::enterWorld(u64 player) {
    m_player = player;
}

void Honor::leaveWorld() {
    m_player = 0;
}

bool Honor::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::SMSG_PVP_CREDIT:
    case Opcode::SMSG_PLAYER_SKINNED:
    case Opcode::SMSG_ZONE_UNDER_ATTACK:
    case Opcode::SMSG_DEFENSE_MESSAGE: return true;
    default: return false;
    }
}

bool Honor::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs, const Reactions* reactions) {
    switch (opcode) {
    case Opcode::SMSG_PVP_CREDIT: {
        bool ok = true;
        credit(payload, nowMs, reactions, ok);
        return ok;
    }
    case Opcode::SMSG_PLAYER_SKINNED: {
        net::PlayerSkinned m;
        if (!net::parsePlayerSkinned(payload, m)) {
            return false;
        }
        event(GameEventKind::PlayerSkinned, nowMs).value[0] = m.freeRepop ? 1 : 0; // PLAYER_SKINNED
        return true;
    }
    case Opcode::SMSG_ZONE_UNDER_ATTACK: {
        net::ZoneUnderAttack m;
        if (!net::parseZoneUnderAttack(payload, m)) {
            return false;
        }
        const AreaData* areas = m_travel.areas();
        const AreaInfo* area = areas ? areas->area(m.areaId) : nullptr;
        if (!area) {
            return true; // 0x49DCF9: an area AreaTable lacks says nothing
        }
        char line[256];
        const std::string_view text = text::format(line, "ZONE_UNDER_ATTACK", area->name);
        defenseLine(area->parent != 0 ? area->parent : area->id, text, nowMs);
        return true;
    }
    case Opcode::SMSG_DEFENSE_MESSAGE: {
        net::DefenseMessage m;
        if (!net::parseDefenseMessage(payload, m)) {
            return false;
        }
        const AreaData* areas = m_travel.areas();
        const AreaInfo* area = areas ? areas->area(m.zoneId) : nullptr;
        defenseLine(area && area->parent != 0 ? area->parent : m.zoneId, m.text, nowMs);
        return true;
    }
    default: return true;
    }
}

void Honor::credit(std::span<const u8> payload, u32 nowMs, const Reactions* reactions, bool& ok) {
    net::PvpCredit m;
    ok = net::parsePvpCredit(payload, m);
    if (!ok) {
        return;
    }
    if (m.victim == 0) { // COMBATLOG_HONORAWARD
        GameEvent& e = event(GameEventKind::HonorGained, nowMs);
        e.value[0] = m.honor;
        e.value[1] = static_cast<i32>(HonorCredit::Awarded);
        return;
    }
    const Entity* victim = m_entities.find(m.victim);
    if (!victim || !isUnit(*victim)) {
        return; // 0x6252AB: the client no longer has the victim: no line, no combat text
    }
    HonorCredit kind = HonorCredit::Dishonorable;
    i32 side = 0;
    u32 rank = 0;
    if (m.honor > 0) {
        // The victim's side from its FactionTemplate's own group (0x6252DE): Horde first, then Alliance, else
        // nothing.
        const u32 group = reactions ? reactions->groupMask(victim->value(uf::UNIT_FIELD_FACTIONTEMPLATE)) : 0;
        if ((group & kFactionGroupHorde) != 0) {
            side = 0;
        } else if ((group & kFactionGroupAlliance) != 0) {
            side = 1;
        } else {
            return;
        }
        kind = HonorCredit::Honorable;
        rank = static_cast<u32>(std::max(m.rank, 0));
    }
    GameEvent& e = event(GameEventKind::HonorGained, nowMs);
    e.guid = m.victim;
    e.id = rank;
    e.value[0] = m.honor;
    e.value[1] = static_cast<i32>(kind);
    e.value[2] = side;
}

void Honor::defenseLine(u32 zoneId, std::string_view text, u32 nowMs) {
    const u32 playerZone = m_travel.zone().zone;
    for (const ChatChannel& channel : m_social.channels()) {
        if (channel.name.empty() || !channel.joined) {
            continue;
        }
        const u32 kind = defenseChannelKind(channel);
        if (kind == 0 || (kind == kChannelLocalDefense && zoneId != playerZone)) {
            continue; // LocalDefense (zone dependent, 0x49DD9C) only for the player's own zone
        }
        net::ChatMessage m;
        m.type = net::ChatType::Channel;
        m.channel = channel.name;
        m.text = text;
        m_chat.add(m, nowMs); // no sender: ChatFrame writes "[2. LocalDefense] : <text>"
        ++m_defenseLines;
    }
}

GameEvent& Honor::event(GameEventKind kind, u32 nowMs) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = kind;
    e.timeMs = nowMs;
    return e;
}

} // namespace vd::game
