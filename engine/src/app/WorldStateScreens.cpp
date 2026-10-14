#include "app/WorldStateScreens.h"

#include "app/AuraLayout.h"
#include "app/GameSession.h"
#include "app/Hud.h"
#include "app/MinimapView.h"
#include "app/UiResources.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/AreaData.h"
#include "game/CharacterNames.h"
#include "game/CharacterSheet.h"
#include "game/Gameplay.h"
#include "game/Travel.h"
#include "game/WorldMapData.h"
#include "net/PvpLogMessages.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

constexpr u32 kHordeColor = ui::rgba(255, 26, 26);    // WorldStateFrame.lua (1.0, 0.1, 0.1)
constexpr u32 kAllianceColor = ui::rgba(0, 173, 240); // (0, 0.68, 0.94)
constexpr u64 kPartyIcon = assetPathHash("Interface\\WorldMap\\WorldMapPartyIcon.blp");
constexpr u64 kAllianceFlag = assetPathHash("Interface\\WorldStateFrame\\AllianceFlag.blp");
constexpr u64 kHordeFlag = assetPathHash("Interface\\WorldStateFrame\\HordeFlag.blp");
constexpr u32 kWarsongGulch = 489, kArathiBasin = 529, kAlteracValley = 30;
constexpr u32 kWarsongGulchZone = 3277, kArathiBasinZone = 3358, kAlteracValleyZone = 2597;

// The hash of `path` + `suffix` + ".blp", as assetPathHash would give for the joined path.
u64 texturePath(std::string_view path, std::string_view suffix = {}) {
    if (path.empty()) {
        return 0;
    }
    u64 hash = kFnvOffset;
    for (const std::string_view part : {path, suffix, std::string_view(".blp")}) {
        for (const char c : part) {
            hash ^= static_cast<u8>(normalizeAssetPathChar(c));
            hash *= kFnvPrime;
        }
    }
    return hash;
}

std::string_view copyText(std::span<char> out, std::string_view text) {
    const usize n = std::min(text.size(), out.size() - 1);
    std::memcpy(out.data(), text.data(), n);
    out[n] = 0;
    return {out.data(), n};
}

// GetText(token, nil, n): the plural "_P1" form unless n is 1.
std::string_view countText(std::span<char> out, const char* singular, const char* plural, u32 n) {
    return text::format(out, n == 1 ? singular : plural, n);
}

} // namespace

WorldStateScreens::WorldStateScreens() {
    m_rows.reserve(net::kPvpLogMaxScores);
    m_rowRaceClass.resize(net::kPvpLogMaxScores);
}

void WorldStateScreens::readDemoSwitches() {
    // In init, not the constructor: Android sets the intent's switches (bench.ps1 -EnvSwitches) after Game exists.
    if (const char* place = VD_DEV_ENV("VD_WORLDSTATE_DEMO")) {
        m_demoPlace = std::strcmp(place, "wsg") == 0  ? 1
                      : std::strcmp(place, "ab") == 0 ? 2
                      : std::strcmp(place, "av") == 0 ? 3
                      : std::strcmp(place, "ep") == 0 ? 4
                      : std::strcmp(place, "si") == 0 ? 5
                                                      : 0;
    }
    if (const char* score = VD_DEV_ENV("VD_SCORE_DEMO")) {
        m_demoScore = std::strcmp(score, "wsg") == 0       ? 1
                      : std::strcmp(score, "av") == 0      ? 2
                      : std::strcmp(score, "victory") == 0 ? 3
                                                           : 0;
    }
}

void WorldStateScreens::init(const pack::PackReader& db, const game::AreaData* areas,
                             const game::WorldMapData* maps, UiResources* resources) {
    readDemoSwitches();
    if (!m_data.init(db)) {
        VD_LOG_WARN(
            "World state UI: no WorldStateUI.dbc: the battlegrounds' lines and score columns are missing");
    }
    m_areas = areas;
    m_maps = maps;
    m_resources = resources;
}

void WorldStateScreens::share(const WorldStateScreens& game, UiResources* resources) {
    m_source = game.m_source;
    m_areas = game.m_areas;
    m_maps = game.m_maps;
    m_resources = resources;
}

void WorldStateScreens::toggleScore(GameSession& session) {
    // ToggleWorldStateScoreFrame: hidden when shown; else only while a queue is "active".
    if (m_scoreShown) {
        m_scoreShown = false;
        return;
    }
    game::Gameplay& play = session.play();
    if (play.battlegrounds().activeSlot() >= 0) {
        showScore(play, 0);
    }
}

void WorldStateScreens::toggleBattlefieldMinimap(GameSession& session) {
    // BattlefieldMinimap_Toggle: SHOW_BATTLEFIELD_MINIMAP follows (the settings' showBattlefieldMinimap:
    // update()).
    if (m_minimapShown) {
        m_minimapShown = false;
        m_minimapCvar = 0; // SHOW_BATTLEFIELD_MINIMAP = "0"
        return;
    }
    const game::Gameplay& play = session.play();
    if (play.battlegrounds().activeSlot() >= 0 || play.worldStateUi().count() > 0) {
        m_minimapShown = true;
        m_minimapCvar = 1; // "1"
    }
}

void WorldStateScreens::showScore(game::Gameplay& play, u32 nowMs) {
    if (!m_scoreShown) {
        // OnShow: Resize, Update, WorldStateScoreFrameTab_OnClick(Tab1): the All tab,
        // SetBattlefieldScoreFaction(nil).
        m_scoreTab = 0;
        m_scoreState.offset = 0;
        play.battlefieldScore().setFaction(-1, nowMs);
    }
    m_scoreShown = true;
}

void WorldStateScreens::prepareHud(const HudFrame& hud) {
    m_x = hud.position.x;
    m_y = hud.position.y;
    m_facing = hud.facing;
}

void WorldStateScreens::leaveWorld() {
    m_scoreShown = false;
    m_cursorsPlayer = 0;
    m_lineCount = m_barCount = 0;
    m_hudStates = m_hudList = ~0u;
    m_barKnown = {};
}

void WorldStateScreens::readEvents(GameSession& session, SettingsView& settings, u32 nowMs) {
    game::Gameplay& play = session.play();
    using K = game::GameEventKind;
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        switch (e->kind) {
        case K::WorldStates:
            // WorldStateScoreFrame_OnEvent: visible or a winner: Update (ShowUIPanel with a winner);
            // WorldStateFrame_ToggleMinimap: SHOW_BATTLEFIELD_MINIMAP and something to show opens the map.
        case K::BattlefieldScore:
            if (play.battlegrounds().winner() >= 0) {
                showScore(play, nowMs);
            }
            if (e->kind == K::WorldStates && settings.showBattlefieldMinimap &&
                play.worldStateUi().count() > 0) {
                m_minimapShown = true;
            }
            break;
        default: break;
        }
    }
}

void WorldStateScreens::update(GameSession& session, SettingsView& settings, bool worldMapOpen, u32 nowMs) {
    VD_PROFILE_ZONE("WorldStateScreens::update");
    game::Gameplay& play = session.play();
    play.worldStateUi().setData(m_source);
    play.battlefieldScore().setData(m_source);
    if (m_cursorsPlayer != session.playerGuid() || play.battlefieldScore().worldEntries() != m_worldEntries) {
        // PLAYER_ENTERING_WORLD: HideUIPanel(WorldStateScoreFrame); BattlefieldMinimap_OnEvent hides the map
        // with nothing to show, WorldStateFrame_ToggleMinimap shows it again when SHOW_BATTLEFIELD_MINIMAP.
        m_cursorsPlayer = session.playerGuid();
        m_worldEntries = play.battlefieldScore().worldEntries();
        m_eventCursor = play.events().end();
        m_scoreShown = false;
        m_minimapPending = true;
    }
    readEvents(session, settings, nowMs);
    if (m_minimapPending && play.worldStates().known()) {
        // The first world states of the new world decide: the active queue or a line to show.
        m_minimapPending = false;
        const bool something = play.battlegrounds().activeSlot() >= 0 || play.worldStateUi().count() > 0;
        m_minimapShown = something && (m_minimapShown || settings.showBattlefieldMinimap);
    }
    if (m_minimapCvar >= 0) { // only BattlefieldMinimap_Toggle sets SHOW_BATTLEFIELD_MINIMAP
        settings.showBattlefieldMinimap = m_minimapCvar != 0;
        m_minimapCvar = -1;
    }
    if (m_scoreShown) {
        play.battlefieldScore().requestScore(nowMs); // WorldStateScoreFrame OnUpdate
    }
    if (worldMapOpen || m_minimapShown) {
        play.battlefieldScore().requestPositions(nowMs); // WorldMapFrame OnUpdate
    }
    buildHud(play, settings.hideOutdoorWorldState);
}

void WorldStateScreens::buildHud(const game::Gameplay& play, bool hideOutdoor) {
    const game::WorldStateUi& ui = play.worldStateUi();
    const bool hidden = hideOutdoor && !play.battlegrounds().inBattleground();
    if (play.worldStates().changes() == m_hudStates && ui.changes() == m_hudList && hidden == m_hudHidden) {
        return; // WorldStateAlwaysUpFrame_Update runs on UPDATE_WORLD_STATES only
    }
    m_hudStates = play.worldStates().changes();
    m_hudList = ui.changes();
    m_hudHidden = hidden;
    m_lineCount = 0;
    u32 bars = 0;
    if (!hidden) { // HIDE_OUTDOOR_WORLD_STATE == "0" or instanceType == "pvp"
        for (u32 i = 0; i < ui.count(); ++i) {
            std::array<char, 128>& buffer = m_lineTexts[std::min(m_lineCount, kMaxLines - 1)];
            const game::WorldStateUiInfo info = ui.info(i, buffer);
            if (info.state == 0) {
                continue;
            }
            if (!info.extendedUi.empty()) {
                // ExtendedUI["CAPTUREPOINT"]: CaptureBar_Update(id, extendedState1).
                if (info.extendedUi != "CAPTUREPOINT" || bars >= kMaxBars) {
                    continue;
                }
                CaptureBarView& bar = m_bars[bars];
                bar.value = static_cast<f32>(info.extendedStates[0]);
                const f32 position = 25.0f + 124.0f * (1.0f - bar.value / 100.0f);
                const f32 old = m_barKnown[bars] ? m_barPositions[bars] : position;
                bar.moving = position < old ? -1 : position > old ? 1 : 0;
                m_barPositions[bars] = position;
                m_barKnown[bars] = true;
                ++bars;
                continue;
            }
            if (m_lineCount >= kMaxLines) {
                continue;
            }
            AlwaysUpLineView& line = m_lines[m_lineCount++];
            line.text = info.text;
            line.icon = texturePath(info.icon);
            line.dynamicIcon = texturePath(info.dynamicIcon);
            line.dynamicFlash = texturePath(info.dynamicIcon, "Flash"); // dynamicIcon.."Flash"
            line.flashing = info.state == 2;
            line.tooltip = info.tooltip;
            line.dynamicTooltip = info.dynamicTooltip;
        }
    }
    for (u32 i = bars; i < kMaxBars; ++i) {
        m_barKnown[i] =
            false; // a bar hidden starts over (its frame's oldValue stays in 1.12; a new one has none)
    }
    m_barCount = bars;
}

WorldStateHudAvoid WorldStateScreens::avoid(const DialogContext& c, const Hud& hud) const {
    WorldStateHudAvoid a;
    a.targetFrame = hud.targetFrameRect();
    a.buffs = hud.playerAurasRect();
    const f32 unit = c.ui.px(ui::kUnitDp);
    const MinimapLayout map = minimapLayout(c.safeArea, unit, c.ui.dp());
    a.cluster = {map.left, map.top, 192.0f * unit, 192.0f * unit};
    a.actions = hud.actionBarRect();
    a.stance = hud.stanceBarRect();
    a.extraBar = hud.extraBarRect();
    a.lines = m_linesRect;
    return a;
}

void WorldStateScreens::drawHud(DialogContext& c, const Hud& hud, u64 nowMs) {
    m_linesRect = {};
    if (m_lineCount == 0 && m_barCount == 0) {
        return;
    }
    VD_PROFILE_ZONE("WorldStateScreens::drawHud");
    const f32 unit = c.ui.px(ui::kUnitDp);
    const WorldStateHudPlaces places = worldStateHudPlaces(c.draw, c.safeArea, unit, avoid(c, hud), lines());
    m_linesRect = places.lines;
    const WorldStateHudResult r = worldStateHud(c, places, lines(), bars(), nowMs);
    if (!r.tooltip.empty()) {
        worldStateTooltip(c, r.tooltipAnchor, r.tooltip);
    }
}

f32 WorldStateScreens::captureBarsHeight(const DialogContext& c) const {
    return vd::captureBarsHeight(c.ui.px(ui::kUnitDp), m_barCount);
}

WorldStateScoreView WorldStateScreens::buildScore(game::Gameplay& play, std::string_view playerName,
                                                  u32 nowMs) {
    const game::BattlefieldScore& score = play.battlefieldScore();
    WorldStateScoreView v;
    if (score.changes() != m_scoreChanges || m_scoreTab != m_scoreTabBuilt) {
        m_scoreChanges = score.changes();
        m_scoreTabBuilt = m_scoreTab;
        // GetBattlefieldStatInfo: the columns.
        const u32 stats = std::min<u32>(score.statCount(), 7);
        for (u32 j = 0; j < stats; ++j) {
            const game::WorldStateUiRow* row = score.stat(j);
            ScoreColumnView& col = m_columns[j];
            col = ScoreColumnView{};
            col.text = row->text;
            col.tooltip = row->tooltip;
            col.hasIcon = !row->icon.empty();
            col.icons = {texturePath(row->icon, "0"), texturePath(row->icon, "1")};
        }
        for (u32 j = stats; j < 7; ++j) {
            m_columns[j] = ScoreColumnView{};
        }
        // GetBattlefieldScore(i) for the filter's side.
        m_rows.clear();
        u32 horde = 0, alliance = 0;
        for (u32 i = 0; i < score.count(); ++i) {
            const game::BattlefieldScoreLine& line = score.line(i);
            ScoreRowView row;
            row.name = score.name(line);
            row.faction = line.faction;
            row.nameColor = line.faction == 0 ? kHordeColor : line.faction == 1 ? kAllianceColor : ui::kGold;
            if (!row.name.empty() && row.name == playerName) {
                row.nameColor = ui::kGold; // name == UnitName("player")
            }
            // GetPVPRankInfo(rank, faction): the badge from rank number 1, the name for its tooltip.
            const i32 rankNumber = game::pvpRankNumber(line.rank);
            if (rankNumber > 0) {
                char path[64];
                std::snprintf(path, sizeof(path), "Interface\\PvPRankBadges\\PvPRank%02d.blp", rankNumber);
                row.rankBadge = assetPathHash(path);
                char key[40];
                const std::string_view k = game::pvpRankKey(key, line.rank, line.faction == 1);
                row.rankName = k.empty() ? std::string_view{} : text::get(text::Key(k));
            }
            // race.." "..class
            std::array<char, 64>& rc = m_rowRaceClass[m_rows.size()];
            const std::string_view race = game::raceName(line.race), cls = game::className(line.cls);
            const int n = std::snprintf(rc.data(), rc.size(), "%.*s %.*s", static_cast<int>(race.size()),
                                        race.data(), static_cast<int>(cls.size()), cls.data());
            row.raceClass = std::string_view(rc.data(), static_cast<usize>(std::clamp(n, 0, 63)));
            row.killingBlows = line.killingBlows;
            row.honorableKills = line.honorableKills;
            row.deaths = line.deaths;
            row.bonusHonor = line.bonusHonor;
            for (u32 j = 0; j < 7; ++j) {
                row.stats[j] = line.stats[j];
            }
            horde += line.faction == 0 ? 1 : 0;
            alliance += line.faction >= 1 ? 1 : 0;
            m_rows.push_back(row);
        }
        // PLAYER_COUNT_ALLIANCE / _HORDE (GetText's plural).
        char a[48], h[48];
        const std::string_view as =
            countText(a, "PLAYER_COUNT_ALLIANCE", "PLAYER_COUNT_ALLIANCE_P1", alliance);
        const std::string_view hs = countText(h, "PLAYER_COUNT_HORDE", "PLAYER_COUNT_HORDE_P1", horde);
        if (alliance > 0 && horde > 0) {
            std::snprintf(m_playerCount.data(), m_playerCount.size(), "%.*s / %.*s",
                          static_cast<int>(as.size()), as.data(), static_cast<int>(hs.size()), hs.data());
        } else if (alliance > 0) {
            copyText(m_playerCount, as);
        } else if (horde > 0) {
            copyText(m_playerCount, hs);
        } else {
            m_playerCount[0] = 0;
        }
        // WorldStateScoreFrameLabel: STAT_TEMPLATE with the tab's text.
        const char* tabKeys[3] = {"ALL", "FACTION_ALLIANCE", "FACTION_HORDE"};
        text::format(m_label, "STAT_TEMPLATE", text::get(tabKeys[std::min<u32>(m_scoreTab, 2)]));
    }
    v.columns = {m_columns.data(), std::min<usize>(score.statCount(), 7)};
    v.rows = m_rows;
    v.tab = m_scoreTab;
    v.label = m_label.data();
    v.playerCount = m_playerCount.data();
    const game::Battlegrounds& bgs = play.battlegrounds();
    v.winner = bgs.winner();
    if (v.winner == 0 || v.winner == 1) {
        v.winnerText = text::get(v.winner == 0 ? "VICTORY_TEXT0" : "VICTORY_TEXT1");
    }
    // TIME_ELAPSED.." "..SecondsToTime(GetBattlefieldInstanceRunTime()/1000, 1).
    {
        char time[48];
        const std::string_view t = game::secondsToTime(time, bgs.instanceRunTimeMs(nowMs) / 1000, true);
        const std::string_view label = text::get("TIME_ELAPSED");
        std::snprintf(m_runTime.data(), m_runTime.size(), "%.*s %.*s", static_cast<int>(label.size()),
                      label.data(), static_cast<int>(t.size()), t.data());
        v.runTime = m_runTime.data();
    }
    // BattlefieldFrame_OnUpdate: SecondsToTimeAbbrev(BATTLEFIELD_SHUTDOWN_TIMER).
    v.timer = secondsToTimeAbbrev(m_timer, static_cast<f32>(bgs.instanceExpirationMs(nowMs)) / 1000.0f);
    return v;
}

void WorldStateScreens::applyScore(game::Gameplay& play, const WorldStateScoreResult& r, u32 nowMs) {
    if (r.tab >= 0) {
        // WorldStateScoreFrameTab_OnClick: tab 2 the Alliance (1), 3 the Horde (0), else nil.
        m_scoreTab = static_cast<u32>(r.tab);
        play.battlefieldScore().setFaction(r.tab == 1 ? 1 : r.tab == 2 ? 0 : -1, nowMs);
        m_scoreState.offset = 0;
        m_uiSound(UiSound::CharacterTab);
    }
    if (r.leave) {
        play.battlegrounds().leave(); // LeaveBattlefield()
    }
    if (r.closed) {
        m_scoreShown = false;
    }
}

void WorldStateScreens::drawScore(GameSession& session, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("WorldStateScreens::drawScore");
    game::Gameplay& play = session.play();
    std::string_view me;
    if (const std::string* name = session.playerName(session.playerGuid())) {
        me = *name;
    }
    const WorldStateScoreView view = buildScore(play, me, nowMs);
    applyScore(play, worldStateScoreFrame(c, view, m_scoreState), nowMs);
}

BattlefieldMinimapView WorldStateScreens::buildMinimap(game::Gameplay& play, u32 zoneId, bool hasPlayer,
                                                       f64 x, f64 y, f32 facing, std::string_view playerName,
                                                       const SettingsView& settings, u32 nowMs) {
    BattlefieldMinimapView v;
    v.opacity = settings.battlefieldMinimapOpacity;
    v.locked = settings.battlefieldMinimapLocked;
    v.showPlayers = settings.battlefieldMinimapShowPlayers;
    v.playerName = playerName;
    const game::WorldMapArea* area = m_maps ? m_maps->zoneMap(zoneId) : nullptr;
    if (!area || !m_resources) {
        return v;
    }
    for (u32 i = 0; i < 12; ++i) {
        v.tiles[i] = m_resources->texture(game::WorldMapData::tileTexture(area->directory, i + 1));
    }
    v.partyIcon = m_resources->texture(kPartyIcon);
    const auto toMap = [&](f64 wx, f64 wy) { return area->toMap(wx, wy); };
    if (hasPlayer) {
        const glm::vec2 uv = toMap(x, y);
        v.hasPlayer = true;
        v.playerU = uv.x;
        v.playerV = uv.y;
        v.facing = facing;
    }
    // The party or the raid (GetPlayerMapPosition("raidN") / ("partyN")): where the server last put each
    // member.
    std::array<game::PartyMemberView, 40> members;
    const u32 count = play.social().groupMembers(members);
    u32 party = 0;
    for (u32 i = 0; i < count && party < m_party.size(); ++i) {
        const game::PartyMemberView& m = members[i];
        if (!m.online || !m.hasPosition || m.zone != zoneId) {
            continue;
        }
        const glm::vec2 uv = toMap(m.x, m.y);
        m_party[party++] = {uv.x, uv.y, m.name};
    }
    v.party = {m_party.data(), party};
    // GetBattlefieldPosition: the teammates out of the group; GetBattlefieldFlagPosition: the carrier.
    game::BattlefieldScore& score = play.battlefieldScore();
    const u32 team = std::min<u32>(score.positionCount(), static_cast<u32>(m_team.size()));
    u32 shown = 0;
    for (u32 i = 0; i < team; ++i) {
        game::BattlefieldPosition p;
        if (score.position(i, nowMs, p)) {
            const glm::vec2 uv = toMap(p.x, p.y);
            m_team[shown++] = {uv.x, uv.y, p.name};
        }
    }
    v.team = {m_team.data(), shown};
    game::BattlefieldFlag flag;
    if (score.flagCarrier(nowMs, flag) && flag.token != game::BattlefieldFlag::Token::Unknown) {
        const glm::vec2 uv = toMap(flag.x, flag.y);
        m_flags[0] = {uv.x, uv.y,
                      flag.token == game::BattlefieldFlag::Token::AllianceFlag ? kAllianceFlag : kHordeFlag};
        v.flags = {m_flags.data(), 1};
    }
    // GetCorpseMapPosition.
    const game::DeathState& death = play.death();
    if (death.corpseKnown && static_cast<u32>(death.corpseMap) == area->map) {
        const glm::vec2 uv = toMap(death.corpseX, death.corpseY);
        v.hasCorpse = true;
        v.corpseU = uv.x;
        v.corpseV = uv.y;
    }
    return v;
}

void WorldStateScreens::applyMinimap(const BattlefieldMinimapResult& r, SettingsView& settings) {
    if (r.closed) { // BattlefieldMinimapCloseButton: Hide (SHOW_BATTLEFIELD_MINIMAP stays as it was)
        m_minimapShown = false;
    }
    if (r.togglePlayers) {
        settings.battlefieldMinimapShowPlayers = !settings.battlefieldMinimapShowPlayers;
    }
    if (r.toggleLock) {
        settings.battlefieldMinimapLocked = !settings.battlefieldMinimapLocked;
    }
    if (r.opacityChanged) {
        settings.battlefieldMinimapOpacity = r.opacity;
    }
}

void WorldStateScreens::drawBattlefieldMinimap(GameSession& session, DialogContext& c, const Hud& hud,
                                               SettingsView& settings, u64 nowMs) {
    if (!m_minimapShown) {
        return;
    }
    VD_PROFILE_ZONE("WorldStateScreens::drawBattlefieldMinimap");
    game::Gameplay& play = session.play();
    std::string_view me;
    if (const std::string* name = session.playerName(session.playerGuid())) {
        me = *name;
    }
    const game::ZoneState& zone = play.travel().zone();
    const BattlefieldMinimapView view =
        buildMinimap(play, zone.zone, true, m_x, m_y, m_facing, me, settings, static_cast<u32>(nowMs));
    applyMinimap(battlefieldMinimap(c, view, m_minimapState, avoid(c, hud), nowMs), settings);
}

// --- Development
// -------------------------------------------------------------------------------------------------

namespace {

// Sample players (names, races, classes) for the demo's scores: half of each side.
constexpr const char* kAllianceNames[] = {
    "Aeryn",   "Brakk",  "Celestine", "Doran",   "Arthel",   "Faelan", "Gwyndor", "Halvar",
    "Isolde",  "Jorim",  "Kaelis",    "Lunara",  "Merrik",   "Nyssa",  "Orwen",   "Pellan",
    "Quinlan", "Rowena", "Sorrel",    "Thandor", "Ulric",    "Vesna",  "Wendell", "Xandra",
    "Yorick",  "Zalia",  "Aldric",    "Brienne", "Corwin",   "Dalia",  "Edric",   "Fiora",
    "Garret",  "Helena", "Ivor",      "Jessa",   "Kendrick", "Lyra",   "Marcus",  "Neria"};
constexpr const char* kHordeNames[] = {
    "Grok",   "Zulja", "Mograk", "Throm",  "Varok", "Nazgrel", "Rokhan", "Kargath", "Drakka", "Garona",
    "Hrogar", "Jorin", "Kelgor", "Lokra",  "Mazog", "Nargol",  "Orgath", "Prazz",   "Rakash", "Shagra",
    "Tazgul", "Urgok", "Vashra", "Wargo",  "Xazul", "Yeshka",  "Zorgal", "Brogg",   "Draka",  "Feglo",
    "Gorza",  "Hakka", "Ishka",  "Jarnok", "Karza", "Lugro",   "Morga",  "Nokka",   "Oggra",  "Puzik"};
constexpr u8 kAllianceRaces[] = {1, 3, 4, 7};
constexpr u8 kHordeRaces[] = {2, 5, 6, 8};
constexpr u8 kClasses[] = {1, 2, 3,  4, 5,
                           8, 9, 11, 1, 3}; // warrior, paladin, hunter, rogue, priest, mage, ...

net::PvpLogData& demoLog() {
    static net::PvpLogData log; // the demo's packet (built once)
    return log;
}

} // namespace

void WorldStateScreens::demoStart(GameSession& session, u32 nowMs) {
    game::Gameplay& play = session.play();
    play.worldStateUi().setData(m_source);
    play.battlefieldScore().setData(m_source);
    play.worldStateUi().setForceZoneRows(true); // the zone objectives without a LocalDefense channel
    const u8 place = m_demoPlace != 0 ? m_demoPlace : m_demoScore == 2 ? 3 : 1;
    // An active queue for the battleground, its run time and (victory) its closing time.
    net::BattlefieldStatus status;
    status.queueSlot = 0;
    status.mapId = place == 2 ? kArathiBasin : place == 3 ? kAlteracValley : kWarsongGulch;
    status.bracketId = 4;
    status.instanceId = 3;
    status.status = net::kBattlefieldStatusInProgress;
    status.time1 = m_demoScore == 3 ? 120000 : 0;
    status.time2 = 754000;
    if (place <= 3) {
        play.battlegrounds().demoStatus(status, nowMs);
    }
    std::vector<net::WorldStatePair> states;
    u32 map = 0, zone = 0;
    switch (place) {
    case 1: // BattleGroundWS::FillInitialWorldStates, the Horde flag carried
        map = kWarsongGulch;
        zone = kWarsongGulchZone;
        states = {{1581, 1}, {1582, 2}, {1545, 0}, {1546, 1}, {1601, 3}, {2339, 2}, {2338, 1}};
        break;
    case 2: // BattleGroundAB
        map = kArathiBasin;
        zone = kArathiBasinZone;
        states = {{1779, 3}, {1778, 2}, {1776, 1240}, {1777, 980}, {1780, 2000}};
        break;
    case 3:
        map = kAlteracValley;
        zone = kAlteracValleyZone;
        break;
    case 4: // OutdoorPvPEP: two towers each, the capture bar of the one the player stands at
        map = 0;
        zone = 139;
        states = {{2327, 2}, {2328, 1}, {2426, 1}, {2427, 70}, {2428, 20}};
        break;
    case 5: // OutdoorPvPSI
        map = 1;
        zone = 1377;
        states = {{2313, 37}, {2314, 112}, {2317, 200}};
        break;
    default: break;
    }
    play.worldStates().demoInit(map, zone, states, nowMs);
    net::NameQueryInfo
        me; // the offline player (a night elf: the battle map's flag token is the Horde's flag)
    me.guid = session.playerGuid();
    me.name = "Arthel";
    me.race = 4;
    me.cls = 1;
    play.queries().onName(std::move(me));
    // The score: names in the name cache (as their name queries would answer), the lines as the server sends
    // them.
    if (m_demoScore != 0) {
        const u32 perSide = m_demoScore == 2 ? 40 : 10;
        net::PvpLogData& log = demoLog();
        log = net::PvpLogData{};
        log.ended = m_demoScore == 3;
        log.winner = net::kPvpLogWinnerAlliance;
        log.count = perSide * 2;
        for (u32 i = 0; i < log.count; ++i) {
            const bool alliance = i < perSide;
            const u32 k = alliance ? i : i - perSide;
            net::PvpLogScore& s = log.scores[i];
            s.guid = (alliance ? 0x100 : 0x200) + k;
            s.rank = (k * 7) % 19;
            s.killingBlows = (k * 5 + (alliance ? 3 : 1)) % 9;
            s.honorableKills = 10 + (k * 3) % 25;
            s.deaths = (k * 2 + 1) % 7;
            s.bonusHonor = place == 3 ? 400 + (k * 37) % 300 : (alliance ? 166 : 83);
            s.extraCount = place == 3 ? 7 : 2;
            for (u32 j = 0; j < s.extraCount; ++j) {
                s.extra[j] = static_cast<i32>((k + j * 3) % (j + 3));
            }
            net::NameQueryInfo name;
            name.guid = s.guid;
            name.name = alliance ? kAllianceNames[k % std::size(kAllianceNames)]
                                 : kHordeNames[k % std::size(kHordeNames)];
            name.race = alliance ? kAllianceRaces[k % 4] : kHordeRaces[k % 4];
            name.cls = kClasses[k % std::size(kClasses)];
            play.queries().onName(std::move(name));
        }
        play.battlefieldScore().demoScore(log, nowMs);
        play.battlefieldScore().demoStats(status.mapId);
        if (m_demoScore == 3) {
            play.battlegrounds().setWinner(1);
        }
        showScore(play, nowMs);
    }
    // The battle map in Warsong Gulch: two teammates out of the group and the carrier of the Horde flag.
    if (place == 1) {
        net::BattlegroundPositions positions;
        positions.count = 2;
        positions.players[0] = {0x101, 1290.0f, 1500.0f};
        positions.players[1] = {0x102, 1050.0f, 1400.0f};
        positions.hasCarrier = true;
        positions.carrier = {0x103, 1180.0f, 1460.0f};
        play.battlefieldScore().demoPositions(positions);
        m_minimapShown = true;
    }
    m_demoStarted = true;
    m_demoStepMs = nowMs + 4000;
}

void WorldStateScreens::demoUpdate(GameSession& session, u32 nowMs) {
    game::Gameplay& play = session.play();
    if (static_cast<i32>(nowMs - m_demoStepMs) < 0) {
        return;
    }
    m_demoStepMs = nowMs + 4000;
    ++m_demoStep;
    if (m_demoPlace == 1) { // the Horde flag captured, back home, taken again...
        play.worldStates().demoSet(2339, m_demoStep % 2 == 0 ? 2 : 1, nowMs);
        play.worldStates().demoSet(1581, (m_demoStep / 2) % 4, nowMs);
    } else if (m_demoPlace == 4) { // the tower's bar moving towards the Horde and back
        const u32 value = 70 - (m_demoStep % 6) * 12;
        play.worldStates().demoSet(2427, value, nowMs);
    }
}

ui::Rect WorldStateScreens::scorePlace(const DialogContext& c, const UiPanelPlaces& places) {
    if (c.profile != ScreenProfile::Wide) {
        return places.both;
    }
    const f32 gap = c.ui.px(6.0f);
    return {c.safeArea.x + gap, c.safeArea.y + gap, std::max(0.0f, places.both.right() - c.safeArea.x - gap),
            std::max(0.0f, c.safeArea.h - 2.0f * gap)};
}

void WorldStateScreens::drawOffline(GameSession& session, DialogContext& c, const Hud& hud,
                                    SettingsView& settings, const UiPanelPlaces& places, u64 nowMs) {
    if (!demo()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    VD_PROFILE_ZONE("WorldStateScreens::drawOffline");
    game::Gameplay& play = session.play();
    const u32 now = static_cast<u32>(nowMs);
    if (!m_demoStarted) {
        demoStart(session, now);
    }
    demoUpdate(session, now);
    play.worldStateUi().update(now);
    play.battlefieldScore().update(now);
    buildHud(play, settings.hideOutdoorWorldState);
    drawHud(c, hud, nowMs);
    if (m_minimapShown) {
        const game::ZoneState& zone = play.travel().zone();
        const u32 zoneId = zone.zone != 0 ? zone.zone : play.worldStates().zoneId();
        const BattlefieldMinimapView view =
            buildMinimap(play, zoneId, true, m_x, m_y, m_facing, "Arthel", settings, now);
        applyMinimap(battlefieldMinimap(c, view, m_minimapState, avoid(c, hud), nowMs), settings);
    }
    if (m_scoreShown) {
        DialogContext pc = c;
        pc.panel = scorePlace(c, places);
        pc.panelAlign = PanelAlign::Center;
        const WorldStateScoreView view = buildScore(play, "Arthel", now);
        const WorldStateScoreResult r = worldStateScoreFrame(pc, view, m_scoreState);
        if (r.leave) {
            VD_LOG_INFO("Score demo: Leave Battleground (LeaveBattlefield)");
        }
        applyScore(play, r, now);
    }
    // The frames' CPU time.
    const f32 ms = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - start).count();
    m_costSum += ms;
    m_costWorst = std::max(m_costWorst, ms);
    if (++m_costFrames == 600) {
        VD_LOG_INFO(
            "World state demo: %.3f ms a frame on average, %.3f ms at worst (%u lines, %u bars, %zu scores)",
            m_costSum / 600.0f, m_costWorst, m_lineCount, m_barCount, m_rows.size());
        m_costFrames = 0;
        m_costSum = m_costWorst = 0.0f;
    }
}

} // namespace vd
