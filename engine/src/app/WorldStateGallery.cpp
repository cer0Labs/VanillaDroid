#include "app/WorldStateGallery.h"

#include "app/MinimapView.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "net/BattlegroundMessages.h"
#include "net/PvpLogMessages.h"
#include "ui/Widgets.h"

#include <chrono>
#include <cstring>
#include <iterator>
#include <utility>

namespace vd {
namespace {

using ui::Rect;

constexpr u32 kWarsongGulch = 489, kWarsongGulchZone = 3277;
constexpr u32 kArathiBasin = 529, kArathiBasinZone = 3358;
constexpr u32 kAlteracValley = 30, kAlteracValleyZone = 2597;

// Sample players: names, races and classes as their name queries would answer.
constexpr const char* kAlliance[] = {
    "Aeryn",  "Brakk",  "Celestine", "Doran", "Arthel", "Faelan", "Gwyndor", "Halvar", "Isolde", "Jorim",
    "Kaelis", "Lunara", "Merrik",    "Nyssa", "Orwen",   "Pellan", "Quinlan", "Rowena", "Sorrel", "Thandor"};
constexpr const char* kHorde[] = {"Grok",    "Zulja",  "Mograk", "Throm",  "Varok",  "Nazgrel", "Rokhan",
                                  "Kargath", "Drakka", "Garona", "Hrogar", "Jorin",  "Kelgor",  "Lokra",
                                  "Mazog",   "Nargol", "Orgath", "Prazz",  "Rakash", "Shagra"};
constexpr u8 kAllianceRaces[] = {1, 3, 4, 7};
constexpr u8 kHordeRaces[] = {2, 5, 6, 8};
constexpr u8 kClasses[] = {1, 2, 3, 4, 5, 8, 9, 11};

std::vector<net::WorldStatePair> wsgStates(u32 flagState) {
    // BattleGroundWS::FillInitialWorldStates: the Alliance 1 capture, the Horde 2, the Horde flag carried.
    return {{1581, 1}, {1582, 2}, {1545, 0}, {1546, 1}, {1601, 3}, {2339, flagState}, {2338, 1}};
}

} // namespace

WorldStateGallery::WorldStateGallery() = default;
WorldStateGallery::~WorldStateGallery() = default;

bool WorldStateGallery::select(std::string_view page) {
    if (page == "worldstate") {
        m_page = Page::WorldState;
    } else if (page == "score") {
        m_page = Page::Score;
    } else if (page == "score2") {
        m_page = Page::Score2;
    } else if (page == "victory") {
        m_page = Page::Victory;
    } else if (page == "bgminimap") {
        m_page = Page::BgMinimap;
    } else {
        return false;
    }
    m_built = false;
    return true;
}

void WorldStateGallery::setSource(const WorldStateScreens* game, UiResources* resources) {
    m_source = game;
    m_resources = resources;
    if (game) {
        m_screens.share(*game, resources);
    }
}

void WorldStateGallery::build(u32 nowMs) {
    m_store = std::make_unique<game::EntityStore>();
    m_play = std::make_unique<game::Gameplay>(*m_store);
    game::Gameplay& play = *m_play;
    constexpr u64 kMe = 0x99;
    play.enterWorld(kMe, kWarsongGulch, true, nowMs);
    net::NameQueryInfo me; // the player: a night elf (the battle map's flag token is the enemy's flag)
    me.guid = kMe;
    me.name = "Arthel";
    me.race = 4;
    me.cls = 1;
    play.queries().onName(std::move(me));
    play.worldStateUi().setData(&m_screens.data());
    play.battlefieldScore().setData(&m_screens.data());
    play.worldStateUi().setForceZoneRows(true);
    const bool av = m_page == Page::Score2;
    const u32 map = av ? kAlteracValley : kWarsongGulch;
    // The active queue: its run time, and the victory's closing time.
    net::BattlefieldStatus status;
    status.mapId = map;
    status.bracketId = 4;
    status.instanceId = 3;
    status.status = net::kBattlefieldStatusInProgress;
    status.time1 = m_page == Page::Victory ? 104000 : 0;
    status.time2 = av ? 2412000 : 754000;
    play.battlegrounds().demoStatus(status, nowMs);
    const std::vector<net::WorldStatePair> states = wsgStates(2);
    play.worldStates().demoInit(map, av ? kAlteracValleyZone : kWarsongGulchZone, states, nowMs);
    // The score: 10 a side in Warsong Gulch, 40 in Alterac Valley.
    const u32 perSide = av ? 40 : 10;
    static net::PvpLogData log; // the sample packet (the gallery only)
    log = net::PvpLogData{};
    log.ended = m_page == Page::Victory;
    log.winner = net::kPvpLogWinnerAlliance;
    log.count = perSide * 2;
    for (u32 i = 0; i < log.count; ++i) {
        const bool alliance = i < perSide;
        const u32 k = alliance ? i : i - perSide;
        net::PvpLogScore& s = log.scores[i];
        s.guid = (alliance ? 0x1000 : 0x2000) + k;
        s.rank = k < 6 ? (k * 3) % 19 : 0;
        s.killingBlows = (k * 5 + (alliance ? 3 : 1)) % 9;
        s.honorableKills = 10 + (k * 3) % 25;
        s.deaths = (k * 2 + 1) % 7;
        s.bonusHonor = av ? 400 + (k * 37) % 300 : (alliance ? 166 : 83);
        s.extraCount = av ? 7 : 2;
        for (u32 j = 0; j < s.extraCount; ++j) {
            s.extra[j] = static_cast<i32>((k + j * 3) % (j + 3));
        }
        net::NameQueryInfo name;
        name.guid = s.guid;
        const char* base = alliance ? kAlliance[k % std::size(kAlliance)] : kHorde[k % std::size(kHorde)];
        name.name = base;
        if (k >= std::size(kAlliance)) {
            name.name += static_cast<char>('a' + k / std::size(kAlliance) - 1);
        }
        name.race = alliance ? kAllianceRaces[k % 4] : kHordeRaces[k % 4];
        name.cls = kClasses[k % std::size(kClasses)];
        play.queries().onName(std::move(name));
    }
    play.battlefieldScore().demoScore(log, nowMs);
    play.battlefieldScore().demoStats(map);
    if (m_page == Page::Victory) {
        play.battlegrounds().setWinner(1);
    }
    m_screens.scoreState().offset = av ? 30 : 0;
    // The battle map: two teammates out of the group, the Alliance carrier of the Horde flag.
    net::BattlegroundPositions positions;
    positions.count = 2;
    positions.players[0] = {0x1001, 1290.0f, 1500.0f};
    positions.players[1] = {0x1002, 1050.0f, 1400.0f};
    positions.hasCarrier = true;
    positions.carrier = {0x1003, 1180.0f, 1460.0f};
    play.battlefieldScore().demoPositions(positions);
    play.worldStateUi().update(nowMs);
    play.battlefieldScore().update(nowMs);
    m_built = true;
}

void WorldStateGallery::drawWorldState(DialogContext& c, u64 nowMs) {
    game::Gameplay& play = *m_play;
    const u32 now = static_cast<u32>(nowMs);
    const f32 unit = c.ui.px(ui::kUnitDp);
    const MinimapLayout map = minimapLayout(c.safeArea, unit, c.ui.dp());
    WorldStateHudAvoid avoid;
    avoid.cluster = {map.left, map.top, 192.0f * unit, 192.0f * unit};
    // The minimap cluster's place, shaded (the capture bars hang under it).
    c.draw.solid(avoid.cluster, ui::rgba(0, 0, 0, 90));
    // Warsong Gulch's lines (the Horde flag carried), its flag's tooltip held.
    play.worldStates().demoInit(kWarsongGulch, kWarsongGulchZone, wsgStates(2), now);
    play.worldStateUi().update(now);
    m_screens.buildHud(play, false);
    WorldStateHudPlaces places = worldStateHudPlaces(c.draw, c.safeArea, unit, avoid, m_screens.lines());
    worldStateHud(c, places, m_screens.lines(), {}, nowMs);
    if (!m_screens.lines().empty()) {
        const f32 cx = places.linesCenterX;
        const Rect dynamic{cx + 22.5f * unit + 8.0f * unit, places.linesTop + 12.0f * unit - 25.0f * unit,
                           32.0f * unit, 32.0f * unit};
        worldStateTooltip(c, dynamic, m_screens.lines()[0].dynamicTooltip);
    }
    // Arathi Basin's lines under them.
    const net::WorldStatePair ab[] = {{1779, 3}, {1778, 2}, {1776, 1240}, {1777, 980}, {1780, 2000}};
    play.worldStates().demoInit(kArathiBasin, kArathiBasinZone, ab, now);
    play.worldStateUi().update(now);
    m_screens.buildHud(play, false);
    places.linesTop += 130.0f * unit;
    worldStateHud(c, places, m_screens.lines(), {}, nowMs);
    // An Eastern Plaguelands tower's capture bar (70: the Alliance's side lit), under the minimap.
    const net::WorldStatePair ep[] = {{2327, 2}, {2328, 1}, {2426, 1}, {2427, 74}, {2428, 20}};
    play.worldStates().demoInit(0, 139, ep, now);
    play.worldStateUi().update(now);
    m_screens.buildHud(play, false);
    worldStateHud(c, places, {}, m_screens.bars(), nowMs);
    // And its two tower lines left of the minimap (they come with the bar in the Eastern Plaguelands).
    places.linesTop += 70.0f * unit;
    worldStateHud(c, places, m_screens.lines(), {}, nowMs);
}

void WorldStateGallery::draw(DialogContext& c, u64 nowMs) {
    if (!m_source) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    VD_PROFILE_ZONE("WorldStateGallery::draw");
    const u32 now = static_cast<u32>(nowMs);
    if (!m_built) {
        build(now);
    }
    game::Gameplay& play = *m_play;
    switch (m_page) {
    case Page::WorldState: drawWorldState(c, nowMs); break;
    case Page::Score:
    case Page::Score2:
    case Page::Victory: {
        DialogContext pc = c;
        if (pc.panel.w <= 0.0f) {
            pc.panel = c.safeArea.inset(c.ui.px(6.0f));
            pc.panelAlign = PanelAlign::Center;
        }
        const WorldStateScoreView view = m_screens.buildScore(play, "Arthel", now);
        const WorldStateScoreResult r = worldStateScoreFrame(pc, view, m_screens.scoreState());
        if (r.tab >= 0) {
            VD_LOG_INFO("Score gallery: tab %d", r.tab);
        }
        if (r.leave) {
            VD_LOG_INFO("Score gallery: Leave Battleground (LeaveBattlefield)");
        }
        m_screens.applyScore(play, r, now);
        break;
    }
    case Page::BgMinimap: {
        // A raid around the player in Warsong Gulch (where the server last put each member), the map's tab
        // and its options menu.
        BattlefieldMinimapView view = m_screens.buildMinimap(play, kWarsongGulchZone, true, 1460.0, 1470.0,
                                                             0.6f, "Arthel", m_settings, now);
        static const BattlefieldMapDot kRaid[] = {{0.47f, 0.18f, "Aeryn"},     {0.52f, 0.20f, "Brakk"},
                                                  {0.55f, 0.35f, "Celestine"}, {0.40f, 0.42f, "Doran"},
                                                  {0.62f, 0.52f, "Faelan"},    {0.45f, 0.60f, "Gwyndor"}};
        view.party = kRaid;
        WorldStateHudAvoid avoid;
        const f32 unit = c.ui.px(ui::kUnitDp);
        const MinimapLayout map = minimapLayout(c.safeArea, unit, c.ui.dp());
        avoid.cluster = {map.left, map.top, 192.0f * unit, 192.0f * unit};
        BattlefieldMinimapState& state = m_screens.minimapState();
        if (state.touchedMs == 0) {
            state.touchedMs = nowMs; // the tab shown, as under the finger, and its options menu open
            state.menu = true;
        }
        if (nowMs - state.touchedMs > 2500) {
            state.touchedMs = nowMs - 1000; // kept shown
        }
        const BattlefieldMinimapResult r = battlefieldMinimap(c, view, state, avoid, nowMs);
        m_screens.applyMinimap(r, m_settings);
        break;
    }
    }
    // The page's CPU time.
    const f32 ms = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - start).count();
    m_costSum += ms;
    m_costWorst = std::max(m_costWorst, ms);
    if (++m_costFrames == 600) {
        VD_LOG_INFO("World state gallery: %.3f ms a frame on average, %.3f ms at worst", m_costSum / 600.0f,
                    m_costWorst);
        m_costFrames = 0;
        m_costSum = m_costWorst = 0.0f;
    }
}

} // namespace vd
