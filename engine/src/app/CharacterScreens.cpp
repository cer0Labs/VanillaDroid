#include "app/CharacterScreens.h"

#include "app/GameSession.h"
#include "app/ModelViews.h"
#include "app/PopupScreens.h"
#include "app/Portraits.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/CharacterNames.h"
#include "game/CharacterSheet.h"
#include "net/UpdateFields.gen.h"
#include "world/Actors.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;
constexpr u32 kErrorColor = ui::rgba(255, 26, 26); // UIErrorsFrame: UI_ERROR_MESSAGE
constexpr u32 kMaxPlayerLevel = 60;                // ReputationFrame.lua's MAX_PLAYER_LEVEL
constexpr u32 kTextShownMs = 4000;                 // a tap on the watch bar: its text this long (its OnEnter)
constexpr u32 kItemClassWeapon = 2;

std::string_view copy(std::span<char> out, std::string_view s) {
    const usize n = std::min(s.size(), out.size() - 1);
    std::memcpy(out.data(), s.data(), n);
    out[n] = '\0';
    return {out.data(), n};
}

// GetText(key, gender): the "_FEMALE" variant for a woman when the table has it.
std::string_view genderText(std::string_view key, bool female) {
    if (female) {
        char name[64];
        const int n =
            std::snprintf(name, sizeof(name), "%.*s_FEMALE", static_cast<int>(key.size()), key.data());
        if (n > 0 && text::has(text::Key(std::string_view(name, static_cast<usize>(n))))) {
            return text::get(text::Key(std::string_view(name, static_cast<usize>(n))));
        }
    }
    return text::get(text::Key(key));
}

std::string_view standingLabel(u32 standing, bool female) {
    static constexpr const char* kKeys[8] = {"FACTION_STANDING_LABEL1", "FACTION_STANDING_LABEL2",
                                             "FACTION_STANDING_LABEL3", "FACTION_STANDING_LABEL4",
                                             "FACTION_STANDING_LABEL5", "FACTION_STANDING_LABEL6",
                                             "FACTION_STANDING_LABEL7", "FACTION_STANDING_LABEL8"};
    return genderText(kKeys[std::clamp<u32>(standing, 1, 8) - 1], female);
}

// GetPVPRankInfo's name (NONE for rank 0).
std::string_view rankName(u32 rank, bool alliance, bool female = false) {
    char key[40];
    const std::string_view k = game::pvpRankKey(key, rank, alliance, female);
    return k.empty() ? text::get("NONE") : text::get(text::Key(k));
}

// HonorFrame_Update / InspectHonorFrame_Update: the rank title, its number, the badge and the bar.
void fillHonor(HonorView& v, const game::HonorStats& h, std::span<char> title, std::span<char> number,
               std::span<char> highest) {
    v.rankTitle = copy(title, rankName(h.rank, h.alliance));
    const i32 rankNumber = game::pvpRankNumber(h.rank);
    v.rankNumber = text::format(number, "VD_CHAR_RANK_NUMBER", text::get("RANK"), rankNumber);
    if (rankNumber > 0) {
        char path[64];
        std::snprintf(path, sizeof(path), "Interface\\PvPRankBadges\\PvPRank%02d.blp", rankNumber);
        v.rankIcon = assetPathHash(path);
    }
    v.progress = h.progress;
    v.alliance = h.alliance;
    v.todayHonorable = h.todayHonorable;
    v.todayDishonorable = h.todayDishonorable;
    v.yesterdayHonorable = h.yesterdayHonorable;
    v.yesterdayHonor = h.yesterdayHonor;
    v.thisWeekHonorable = h.thisWeekHonorable;
    v.thisWeekHonor = h.thisWeekHonor;
    v.lastWeekHonorable = h.lastWeekHonorable;
    v.lastWeekHonor = h.lastWeekHonor;
    v.lastWeekStanding = h.lastWeekStanding;
    v.lifetimeHonorable = h.lifetimeHonorable;
    v.lifetimeDishonorable = h.lifetimeDishonorable;
    v.highestRank = copy(highest, rankName(h.highestRank, h.alliance));
}

} // namespace

void CharacterScreens::init(const pack::PackReader& db, const game::GameData* data,
                            const game::AbilityData* abilities, const game::ItemData* items) {
    m_data = data;
    m_abilities = abilities;
    m_items = items;
    m_tables.init(db);
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    // The views' storage, once.
    m_factionViews.resize(net::kFactionListSize * 2 + 2);
    m_skillSources.resize(game::kSkillSlots);
    m_skillViews.resize(game::kSkillSlots * 2);
    m_skillRanks.resize(game::kSkillSlots * 2);
}

void CharacterScreens::message(u32 color, std::string_view line) {
    if (m_message) {
        char buffer[256];
        m_message(m_messageContext, color, false, copy(buffer, line).data());
    }
}

TextureHandle CharacterScreens::model(ModelViewId id, u64 guid, const ModelFrameState& state) {
    if (!m_views || !m_actors || !m_actorQuery) {
        return {};
    }
    const u32 actor = m_actorQuery(m_actorContext, guid);
    const game::ActorLook* look = actor != ~0u ? m_actors->look(actor) : nullptr;
    return look && look->model != 0 ? m_views->show(id, *look, state.facing) : TextureHandle{};
}

// -------------------------------------------------------------------------------------------------------
// The paper doll.

void CharacterScreens::fillPaperDoll(GameSession& session, PaperDollView& view) {
    VD_PROFILE_ZONE("CharacterScreens::paperDoll");
    const game::Entity* me = session.entities().find(session.playerGuid());
    if (!me) {
        return;
    }
    if (me->guid != m_player) {
        m_player = me->guid;
        m_characterModel = ModelFrameState{};
        m_statShown = -1;
        m_factions.reset();
        m_skills.reset();
        m_factionDetail = false;
    }
    const u8 race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
    const u8 cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
    const bool female = me->byteValue(uf::UNIT_FIELD_BYTES_0, 2) == 1;
    const u32 level = me->value(uf::UNIT_FIELD_LEVEL);
    // CharacterNameText: UnitPVPName("player") (the rank's title before the name).
    const std::string* name = session.playerName(session.playerGuid());
    const std::string_view plain = name ? std::string_view(*name) : text::get("CHARACTER");
    const u32 rank = me->byteValue(uf::PLAYER_BYTES_3, 3);
    view.title = rank != 0 ? text::format(m_title, "UNIT_PVP_NAME",
                                          rankName(rank, game::allianceRace(race), female), plain)
                           : copy(m_title, plain);
    view.level = text::format(m_level, "PLAYER_LEVEL", level, game::raceName(race), game::className(cls));
    // CharacterGuildText: GUILD_TITLE_TEMPLATE with the rank's and the guild's names.
    const game::GuildState& guild = session.play().social().guild();
    view.guild = guild.id != 0 && guild.known
                     ? text::format(m_guild, "GUILD_TITLE_TEMPLATE", guild.rankName(guild.rank), guild.name)
                     : std::string_view{};
    // The numbers: the weapons' templates (UnitAttackBothHands, UnitRangedDamage's school...).
    const game::Inventory& inv = session.play().inventory();
    const auto templateAt = [&](u8 slot) -> const net::ItemQueryInfo* {
        const game::ItemInstance* item = inv.equipped(slot);
        return item ? session.item(item->entry) : nullptr;
    };
    game::PaperDollInput in;
    in.classFile = m_tables.classFile(cls);
    in.relicSlot = m_tables.relicSlot(cls);
    in.mainHand = templateAt(15);
    in.offHand = templateAt(16);
    in.ranged = templateAt(17);
    in.rangedEquipped = inv.equipped(17) != nullptr;
    if (in.offHand && in.offHand->itemClass != kItemClassWeapon) {
        in.offHand =
            nullptr; // a shield or a held item: no off-hand block (UnitAttackSpeed has no off-hand speed)
    }
    m_doll.build(*me, in);
    for (u32 i = 0; i < game::kPaperDollStats; ++i) {
        const game::PaperDollLine& line = m_doll.line(static_cast<game::PaperDollStat>(i));
        m_statViews[i] = {line.label, line.value, {line.tooltip.data(), line.tooltipLines}};
    }
    view.stats = m_statViews;
    view.statShown = m_statShown;
    view.ammoSlot = !in.relicSlot; // PaperDollFrame_OnShow: CharacterAmmoSlot hides with a relic slot
    view.modelState = &m_characterModel;
    view.model = model(ModelViewId::Character, session.playerGuid(), m_characterModel);
}

bool CharacterScreens::relicSlot(const GameSession& session) const {
    const game::Entity* me = session.entities().find(session.playerGuid());
    return me && m_tables.relicSlot(me->byteValue(uf::UNIT_FIELD_BYTES_0, 1));
}

void CharacterScreens::paperDollDone(const PaperDollResult& result) {
    m_statShown = result.statTapped;
    if (result.rotated) {
        m_uiSound(UiSound::InventoryRotate); // Model_RotateLeft / Right
    }
}

// -------------------------------------------------------------------------------------------------------
// Reputation.

void CharacterScreens::reputationTab(GameSession& session, DialogContext& c, const CharacterPageFrame& frame) {
    VD_PROFILE_ZONE("CharacterScreens::reputation");
    game::Gameplay& play = session.play();
    const game::Entity* me = session.entities().find(session.playerGuid());
    u32 count = 0;
    bool female = false;
    if (me && m_abilities) {
        const u32 race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
        const u32 cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
        female = me->byteValue(uf::UNIT_FIELD_BYTES_0, 2) == 1;
        m_factions.build(play.abilities().reputation(), game::FactionList::sourceOf(*m_abilities), race, cls,
                         play.character().watchedFaction());
    }
    const std::span<const game::FactionRow> rows = m_factions.rows();
    const i32 selected = m_factions.selected();
    if (selected < 0) {
        m_factionDetail = false; // ReputationFrame_Update: GetSelectedFaction() == 0 hides the detail
    }
    for (const game::FactionRow& row : rows) {
        if (count == m_factionViews.size()) {
            break;
        }
        FactionRowView& v = m_factionViews[count];
        v = FactionRowView{};
        v.header = row.header;
        v.collapsed = row.collapsed;
        v.name = row.faction == game::kFactionOther            ? text::get("FACTION_OTHER")
                 : row.faction == game::kFactionInactiveHeader ? text::get("FACTION_INACTIVE")
                                                               : row.name;
        v.standingId = row.standing;
        v.standing = standingLabel(row.standing, female);
        v.max = std::max(1, row.barMax - row.barMin);
        v.value = row.barValue - row.barMin;
        v.atWar = row.atWar;
        v.watched = row.watched;
        v.selected = static_cast<i32>(count) == selected;
        ++count;
    }
    FactionDetailView detail;
    if (m_factionDetail && selected >= 0 && static_cast<u32>(selected) < rows.size()) {
        const game::FactionRow& row = rows[static_cast<u32>(selected)];
        detail.shown = true;
        detail.name = row.name;
        detail.description = row.description;
        detail.atWar = row.atWar;
        detail.canToggleAtWar = row.canToggleAtWar;
        detail.inactive = row.inactive;
        detail.watched = row.watched;
    }
    const ReputationPanelResult r =
        reputationPanel(c, frame, {m_factionViews.data(), count}, detail, m_reputationTip);
    if (r.headerTapped >= 0 && static_cast<u32>(r.headerTapped) < rows.size()) {
        if (rows[static_cast<u32>(r.headerTapped)].collapsed) {
            m_factions.expand(r.headerTapped); // ExpandFactionHeader
        } else {
            m_factions.collapse(r.headerTapped);
        }
    }
    if (r.rowTapped >= 0) { // ReputationBar_OnClick
        if (m_factionDetail && r.rowTapped == selected) {
            m_factionDetail = false;
        } else {
            m_factions.select(r.rowTapped);
            m_factionDetail = true;
        }
    }
    if (r.closeDetail) {
        m_factionDetail = false;
    }
    if (detail.shown) {
        const game::FactionRow& row = rows[static_cast<u32>(selected)];
        game::CharacterSheet& sheet = play.character();
        if (r.toggleAtWar && row.listId >= 0) { // FactionToggleAtWar
            const game::CharacterSheet::WarResult war =
                sheet.toggleAtWar(static_cast<u32>(row.listId), row.barValue);
            if (war == game::CharacterSheet::WarResult::InCombat) {
                message(kErrorColor, text::get("ERR_NOT_IN_COMBAT"));
            } else if (war == game::CharacterSheet::WarResult::Sent) {
                m_uiSound(row.atWar ? UiSound::CheckBoxOff : UiSound::CheckBoxOn);
            }
        }
        if (r.toggleInactive && row.listId >= 0) { // SetFactionInactive / SetFactionActive
            sheet.setFactionInactive(static_cast<u32>(row.listId), !row.inactive);
            m_uiSound(row.inactive ? UiSound::CheckBoxOff : UiSound::CheckBoxOn);
        }
        if (r.toggleWatched && row.listId >= 0) { // SetWatchedFactionIndex(index or 0)
            sheet.setWatchedFaction(row.watched ? -1 : row.listId);
            m_uiSound(row.watched ? UiSound::CheckBoxOff : UiSound::CheckBoxOn);
        }
        if (m_trace && (r.toggleAtWar || r.toggleInactive || r.toggleWatched)) {
            VD_LOG_INFO("Character: faction %d (list %d) war %d inactive %d watched %d", row.faction,
                        row.listId, r.toggleAtWar ? 1 : 0, r.toggleInactive ? 1 : 0, r.toggleWatched ? 1 : 0);
        }
    }
}

// -------------------------------------------------------------------------------------------------------
// Skills.

bool CharacterScreens::skillsTab(GameSession& session, DialogContext& c, const CharacterPageFrame& frame) {
    VD_PROFILE_ZONE("CharacterScreens::skills");
    const game::Entity* me = session.entities().find(session.playerGuid());
    u32 sources = 0;
    if (me && m_abilities) {
        sources = game::readSkillSources(*me, *m_abilities, m_tables, m_skillSources);
        m_skills.build({m_skillSources.data(), sources}, me->value(uf::UNIT_FIELD_LEVEL));
    }
    const std::span<const game::SkillLineRow> rows =
        me ? m_skills.rows() : std::span<const game::SkillLineRow>{};
    const i32 selected = m_skills.selected();
    // SkillFrame_SetStatusBar / SkillDetailFrame_SetStatusBar's rank text: "rank/max", "rank (+mod)/max".
    const auto rankText = [](std::span<char> out, const game::SkillLineRow& row) -> std::string_view {
        if (row.maxRank <= 1) {
            return {}; // a proficiency: no numbers
        }
        int n = 0;
        if (row.modifier == 0) {
            n = std::snprintf(out.data(), out.size(), "%u/%u", row.rank, row.maxRank);
        } else {
            n = std::snprintf(out.data(), out.size(),
                              row.modifier > 0 ? "%u (|cff20ff20+%d|r)/%u" : "%u (|cffff2020%d|r)/%u",
                              row.rank, row.modifier, row.maxRank);
        }
        return n > 0 ? std::string_view(out.data(), std::min<usize>(static_cast<usize>(n), out.size() - 1))
                     : std::string_view{};
    };
    const auto fillOf = [](const game::SkillLineRow& row) {
        return row.maxRank > 1 ? static_cast<f32>(row.rank) / static_cast<f32>(row.maxRank) : 1.0f;
    };
    u32 count = 0;
    for (const game::SkillLineRow& row : rows) {
        if (count == m_skillViews.size()) {
            break;
        }
        SkillLineView& v = m_skillViews[count];
        v = SkillLineView{};
        v.header = row.header;
        v.expanded = row.expanded;
        v.name = row.name;
        v.rank = row.header ? std::string_view{} : rankText(m_skillRanks[count], row);
        v.fill = fillOf(row);
        v.proficiency = row.maxRank == 1;
        v.selected = static_cast<i32>(count) == selected;
        ++count;
    }
    SkillDetailView detail;
    const game::SkillLineRow* chosen =
        selected >= 0 && static_cast<u32>(selected) < rows.size() && !rows[static_cast<u32>(selected)].header
            ? &rows[static_cast<u32>(selected)]
            : nullptr;
    if (chosen) {
        detail.shown = true;
        detail.name = chosen->name;
        detail.rank = m_skillViews[static_cast<u32>(selected)].rank;
        detail.fill = fillOf(*chosen);
        detail.proficiency = chosen->maxRank == 1;
        detail.abandonable = chosen->abandonable;
        detail.description = text::format(m_skillDescription, "SKILL_DESCRIPTION", "", chosen->description);
    }
    const SkillPanelResult r =
        skillPanel(c, frame, {m_skillViews.data(), count}, m_skills.allExpanded(), detail, m_skillTip);
    if (r.headerTapped >= 0 && static_cast<u32>(r.headerTapped) < rows.size()) {
        if (rows[static_cast<u32>(r.headerTapped)].expanded) {
            m_skills.collapse(r.headerTapped); // CollapseSkillHeader: an UNLEARN_SKILL of another line goes
            if (m_popups) {
                m_popups->hideUnlearnSkill(chosen ? chosen->skill : 0);
            }
        } else {
            m_skills.expand(r.headerTapped);
        }
    }
    if (r.allTapped) {
        if (m_skills.allExpanded()) {
            m_skills.collapse(-1);
            if (m_popups) {
                m_popups->hideUnlearnSkill(chosen ? chosen->skill : 0);
            }
        } else {
            m_skills.expand(-1);
        }
    }
    if (r.rowTapped >= 0) {
        m_skills.select(r.rowTapped); // SkillBar_OnClick
    }
    if (r.unlearn && chosen && m_popups) {
        m_popups->askUnlearnSkill(session, chosen->skill,
                                  chosen->name); // StaticPopup_Show("UNLEARN_SKILL", name)
    }
    return r.close; // SkillFrameCancelButton: HideUIPanel(CharacterFrame)
}

// -------------------------------------------------------------------------------------------------------
// Honor.

void CharacterScreens::honorTab(GameSession& session, DialogContext& c, const CharacterPageFrame& frame) {
    VD_PROFILE_ZONE("CharacterScreens::honor");
    const game::Entity* me = session.entities().find(session.playerGuid());
    HonorView v;
    if (me) {
        const u8 race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0),
                 cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
        v.level = text::format(m_level, "PLAYER_LEVEL", me->value(uf::UNIT_FIELD_LEVEL), game::raceName(race),
                               game::className(cls)); // HonorLevelText
        const game::GuildState& guild = session.play().social().guild();
        v.guild = guild.id != 0 && guild.known
                      ? text::format(m_guild, "GUILD_TITLE_TEMPLATE", guild.rankName(guild.rank), guild.name)
                      : std::string_view{};
        fillHonor(v, game::honorStats(*me), m_rankTitle, m_rankNumber, m_highestRank);
    }
    honorPanel(c, frame, v, m_honorTip);
}

// -------------------------------------------------------------------------------------------------------
// The watch bar.

void CharacterScreens::drawWatchBar(GameSession& session, DialogContext& c, const ui::Rect& xpBar,
                                    u32 nowMs) {
    VD_PROFILE_ZONE("CharacterScreens::watchBar");
    const game::Entity* me = session.entities().find(session.playerGuid());
    if (!me || !m_abilities || xpBar.w <= 0.0f) {
        return;
    }
    // GetWatchedFactionInfo (FactionList keeps it from its last build; built here when the window is closed).
    const u32 race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0), cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
    m_factions.build(session.play().abilities().reputation(), game::FactionList::sourceOf(*m_abilities), race,
                     cls, session.play().character().watchedFaction());
    const game::FactionRow* watched = m_factions.watched();
    ReputationWatchView v;
    if (watched) {
        const u32 level = me->value(uf::UNIT_FIELD_LEVEL);
        const i32 max = std::max(1, watched->barMax - watched->barMin);
        const i32 value = watched->barValue - watched->barMin;
        v.shown = true;
        v.maxLevel = level >= kMaxPlayerLevel;
        v.fill = static_cast<f32>(value) / static_cast<f32>(max);
        v.standingId = watched->standing;
        char out[96];
        std::snprintf(out, sizeof(out), "%.*s %d / %d", static_cast<int>(watched->name.size()),
                      watched->name.data(), value, max);
        v.text = copy(m_watchText, out);
        // OnEnter, or ShowWatchedReputationBarText while the character window shows at the top level.
        v.showText = nowMs < m_watchTextUntilMs || (v.maxLevel && m_characterShown);
    }
    ui::Gui g{c.ui, c.draw};
    if (reputationWatchBar(g, xpBar, v)) {
        m_watchTextUntilMs = nowMs < m_watchTextUntilMs ? nowMs : nowMs + kTextShownMs;
    }
}

// -------------------------------------------------------------------------------------------------------
// Inspect.

TextureHandle CharacterScreens::portrait(u64 guid) {
    return m_portraits && guid != 0 ? m_portraits->unit(guid) : TextureHandle{};
}

PaperDollResult CharacterScreens::inspectWindow(GameSession& session, DialogContext& c, u64 unit,
                                                PaperDollView& view, ItemDrag& drag) {
    VD_PROFILE_ZONE("CharacterScreens::inspect");
    if (unit != m_inspected) {
        m_inspected = unit;
        m_inspectTab = 0; // InspectFrame_Show of another unit starts on InspectPaperDollFrame
        m_inspectModel = ModelFrameState{};
    }
    game::CharacterSheet& sheet = session.play().character();
    sheet.setInspected(unit);                                                      // NotifyInspect's player
    const std::string_view tabs[2] = {text::get("CHARACTER"), text::get("HONOR")}; // InspectFrameTab1 / 2
    view.tabs = tabs;
    view.tab = m_inspectTab;
    view.page = m_inspectTab == 1 ? CharacterPage::Honor : CharacterPage::PaperDoll; // InspectHonorFrame: its art
    view.ammoSlot = false;
    view.portrait = portrait(unit); // InspectFrame_UnitChanged: SetPortraitTexture(InspectFramePortrait, unit)
    view.modelState = &m_inspectModel;
    view.model = model(ModelViewId::Inspect, unit, m_inspectModel);
    PaperDollResult r = paperDollWindow(c, view, drag);
    if (r.rotated) {
        m_uiSound(UiSound::InventoryRotate);
    }
    if (r.items.tab >= 0 && static_cast<u32>(r.items.tab) != m_inspectTab) {
        m_inspectTab = static_cast<u32>(r.items.tab);
        m_uiSound(UiSound::CharacterTab); // InspectFrameTab_OnClick: igCharacterInfoTab
    }
    if (m_inspectTab == 1 && r.items.tab < 0) {
        // InspectHonorFrame_OnShow: asks for the data once; shows it when it comes (INSPECT_HONOR_UPDATE).
        HonorView v;
        const net::InspectHonorStats* data = sheet.inspectHonor();
        if (!data) {
            sheet.requestInspectHonor();
            v.waiting = true;
        }
        const game::Entity* other = session.entities().find(unit);
        const game::HonorStats h =
            data ? game::honorStats(*data, other) : game::honorStats(net::InspectHonorStats{}, other);
        fillHonor(v, h, m_rankTitle, m_rankNumber, m_highestRank);
        honorPanel(c, r.frame, v, m_inspectHonorTip);
    }
    return r;
}

void CharacterScreens::inspectClosed() {
    m_inspected = 0;
    m_inspectTab = 0;
}

void CharacterScreens::update(GameSession& session, bool characterShown, bool inspectShown) {
    m_characterShown = characterShown;
    if (!inspectShown && m_inspected != 0) {
        inspectClosed();
        session.play().character().setInspected(0); // ClearInspectPlayer
    }
}

void CharacterScreens::drawWatchBarDemo(DialogContext& c, const ui::Rect& xpBar) {
    const char* demo = VD_DEV_ENV("VD_WATCH_BAR_DEMO");
    ReputationWatchView v;
    v.shown = true;
    v.maxLevel = demo && std::strcmp(demo, "60") == 0;
    v.fill = 0.62f;
    v.standingId = 6; // Honored with Stormwind: 7440 / 12000
    v.text = "Stormwind 7440 / 12000";
    v.showText = true;
    ui::Gui g{c.ui, c.draw};
    reputationWatchBar(g, xpBar, v);
}

} // namespace vd
