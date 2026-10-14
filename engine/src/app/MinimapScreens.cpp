#include "app/MinimapScreens.h"

#include "app/GameSession.h"
#include "app/PopupScreens.h"
#include "app/UiResources.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/GameData.h"
#include "game/MinimapData.h"
#include "game/Travel.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace vd {
namespace {

namespace uf = net::uf;
using game::BlipKind;
using ui::rgba;

constexpr f32 kRimArrow = 0.84f;  // rim arrows sit just inside the ring (radii), clear of its art
constexpr f32 kTapSlopDp = 14.0f; // a tap on the map names the blips this close to the finger
constexpr u32 kDeadTint = rgba(160, 160, 160); // a party member dead or a ghost (a touch-first cue)
constexpr u32 kFindHerbs = 2383;               // the development demo's tracking spell

MinimapBlipView blipView(const game::MinimapBlip& b, f32 centerX, f32 centerY, f32 radiusYards, u32 poiIcon) {
    const game::MinimapPoint p = game::minimapPoint(centerX, centerY, radiusYards, b.x, b.y, 1.0f);
    MinimapBlipView v;
    v.x = p.x;
    v.y = p.y;
    v.angle = p.angle;
    if (b.kind == BlipKind::GossipPoi) { // phase 13: its icon on the map, the guide arrow on the rim beyond it
        v.art = p.outside ? MinimapBlipArt::GuideArrow : MinimapBlipArt::Poi;
        v.cell = static_cast<u8>(poiIcon);
        if (p.outside) {
            v.x *= kRimArrow;
            v.y *= kRimArrow;
        }
        return v;
    }
    if (b.kind == BlipKind::Corpse || p.outside) {
        // The corpse is an arrow wherever it lies (pointing away from the player); beyond the map, it and the
        // party members ride the rim.
        v.art = b.kind == BlipKind::Corpse ? MinimapBlipArt::CorpseArrow : MinimapBlipArt::GroupArrow;
        if (p.outside) {
            v.x *= kRimArrow;
            v.y *= kRimArrow;
        }
    } else {
        v.art = MinimapBlipArt::Dot;
        v.cell = static_cast<u8>(game::blipCell(b.kind));
    }
    if (b.dead) {
        v.tint = kDeadTint;
    }
    return v;
}

} // namespace

void MinimapScreens::init(const game::GameData* data, const game::MinimapData* minimap, UiResources* resources) {
    m_data = data;
    m_minimapData = minimap;
    m_resources = resources;
    m_demo = VD_DEV_ENV("VD_HUD_DEMO") != nullptr;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    if (const char* demo = VD_DEV_ENV("VD_MEETINGSTONE_DEMO")) { // phase 16 (alerts)
        m_meetingStoneDemo = static_cast<u32>(std::max(1, std::atoi(demo)));
    }
}

// --- Phase 15: the indoor minimap -------------------------------------------------------------------------

// Indoors, the building's own map (pack::UiWmoMinimapTile) instead of the terrain's: the client's window around the
// player in the building's space ([p - r, p + r] across, [p.z - 1.5 r, p.z + 0.5 r] in height, r the minimap's
// radius: WoW.exe 0x6D95F8), each tile's corners turned into the world with the building. A building without tiles
// keeps the terrain's map.
void MinimapScreens::interiorTiles(const HudFrame& hud) {
    m_interiorTileCount = 0;
    if (!m_indoors || hud.interiorWmo == 0 || !hud.interiorTransform || !m_resources || !m_resources->ready()) {
        return;
    }
    const std::span<const pack::UiWmoMinimapTile> tiles = m_resources->assets().wmoMinimap(hud.interiorWmo);
    if (tiles.empty()) {
        return;
    }
    VD_PROFILE_ZONE("MinimapScreens::interiorTiles");
    const f32* m = hud.interiorTransform; // rows: rotation * scale | translation
    const f64 dx = hud.position.x - m[3], dy = hud.position.y - m[7], dz = hud.position.z - m[11];
    const f64 s2 = static_cast<f64>(hud.interiorScale) * hud.interiorScale;
    const glm::dvec3 p((m[0] * dx + m[4] * dy + m[8] * dz) / s2, (m[1] * dx + m[5] * dy + m[9] * dz) / s2,
                       (m[2] * dx + m[6] * dy + m[10] * dz) / s2);
    const f64 r = m_radiusYards / std::max(hud.interiorScale, 0.01f);
    for (const pack::UiWmoMinimapTile& t : tiles) {
        if (t.boundsMax[0] < p.x - r || t.boundsMin[0] > p.x + r || t.boundsMax[1] < p.y - r ||
            t.boundsMin[1] > p.y + r || t.boundsMax[2] < p.z - 1.5 * r || t.boundsMin[2] > p.z + 0.5 * r) {
            continue;
        }
        const TextureHandle texture = m_resources->texture(t.texture);
        if (!texture.valid() || m_interiorTileCount == kMaxInteriorTiles) {
            continue; // still loading (it shows in a frame or two)
        }
        MinimapInteriorTile& out = m_interiorTiles[m_interiorTileCount++];
        out.texture = texture;
        // The texture's top-left is the tile's (min x, max y): columns along +X, rows along -Y.
        const f32 local[4][2] = {{t.boundsMin[0], t.boundsMax[1]},
                                 {t.boundsMax[0], t.boundsMax[1]},
                                 {t.boundsMax[0], t.boundsMin[1]},
                                 {t.boundsMin[0], t.boundsMin[1]}};
        for (u32 i = 0; i < 4; ++i) {
            out.corners[2 * i] = m[0] * local[i][0] + m[1] * local[i][1] + m[2] * t.boundsMin[2] + m[3];
            out.corners[2 * i + 1] = m[4] * local[i][0] + m[5] * local[i][1] + m[6] * t.boundsMin[2] + m[7];
        }
    }
    m_view.interior = true;
    m_view.interiorTiles = {m_interiorTiles.data(), m_interiorTileCount};
}

// --- Before the HUD ----------------------------------------------------------------------------------------

void MinimapScreens::prepareHud(GameSession& session, HudFrame& hud, const SettingsView& settings,
                                u32 nowMs) {
    VD_PROFILE_ZONE("MinimapScreens::prepareHud");
    m_indoors = hud.indoors;
    const u32 zoom =
        std::min(m_indoors ? settings.minimapInsideZoom : settings.minimapZoom, game::kMinimapZoomLevels - 1);
    m_radiusYards = game::minimapRadius(m_indoors, zoom);

    MinimapView& v = m_view;
    v = MinimapView{};
    v.map = hud.map;
    v.position = hud.position;
    v.facing = hud.facing;
    v.radiusYards = m_radiusYards;
    v.shown = m_shown;
    v.zoneName = hud.zoneName;
    v.zoneColor = hud.zoneColor != 0 ? hud.zoneColor : ui::kGold;
    v.canZoomIn = zoom + 1 < game::kMinimapZoomLevels;
    v.canZoomOut = zoom > 0;
    v.hasTime = hud.gameTime >= 0.0f;
    m_minutes = v.hasTime ? static_cast<u32>(hud.gameTime * 0.5f) % (24 * 60) : 0;
    v.night = game::gameTimeNight(m_minutes);
    interiorTiles(hud);

    // What the game state puts on it.
    m_blipCount = 0;
    m_trackingSpell = 0;
    const game::MinimapPingState* ping = &m_localPing;
    u32 poiIcon = 0;
    if (session.inWorld()) {
        game::Gameplay& play = session.play();
        poiIcon = play.minimap().gossipPoint().icon;
        collectBlips(session, hud, nowMs);
        if (m_minimapData) {
            m_trackingSpell = game::activeTrackingSpell(play, session.playerGuid(), *m_minimapData);
        }
        v.newMail = play.exchange().mail().hasNewMail();
        v.meetingStone = play.meetingStone().inQueue(); // phase 16 (alerts): MEETINGSTONE_CHANGED's Show / Hide
        ping = &play.minimap().lastPing();
    } else if (m_demo) {
        demoBlips(nowMs);
        poiIcon = 6; // the red flag every points_of_interest row of vMaNGOS uses
        m_trackingSpell = kFindHerbs;
        v.newMail = true;
    }
    if (!session.inWorld() && m_meetingStoneDemo != 0) {
        v.meetingStone = true; // VD_MEETINGSTONE_DEMO
    }
    if (m_trackingSpell != 0 && m_data) {
        if (const std::optional<game::SpellInfo> spell = m_data->spell(m_trackingSpell)) {
            v.trackingIcon = spell->iconHash;
        }
    }
    const f32 centerX = static_cast<f32>(v.position.x), centerY = static_cast<f32>(v.position.y);
    m_blipViewCount = 0;
    u32 kept = 0;
    for (u32 i = 0; i < m_blipCount; ++i) {
        const game::MinimapPoint p =
            game::minimapPoint(centerX, centerY, m_radiusYards, m_blips[i].x, m_blips[i].y);
        if (p.outside && !game::blipOnRim(m_blips[i].kind)) {
            continue; // only party members and the corpse ride the rim
        }
        m_blips[kept++] = m_blips[i]; // blips and their views stay parallel (a tap names them)
        m_blipViews[m_blipViewCount++] = blipView(m_blips[i], centerX, centerY, m_radiusYards, poiIcon);
    }
    m_blipCount = kept;
    v.blips = {m_blipViews.data(), m_blipViewCount};

    // The ping: the sound of each new one (Minimap_OnEvent: PlaySound("MapPing")), shown while it lasts on
    // the map.
    if (ping->serial != m_lastPingSerial) {
        m_lastPingSerial = ping->serial;
        if (ping->active) {
            m_uiSound(UiSound::MapPing);
        }
    }
    v.pingAlpha = game::minimapPingAlpha(*ping, nowMs);
    if (v.pingAlpha > 0.0f) {
        const game::MinimapPoint p = game::minimapPoint(centerX, centerY, m_radiusYards, ping->x, ping->y);
        v.pingX = p.x;
        v.pingY = p.y;
        v.pingSeconds = static_cast<f32>(nowMs - ping->startMs) * 0.001f;
        if (p.outside) {
            v.pingAlpha = 0.0f; // Minimap_SetPing hides it beyond the map
        }
    }
    hud.minimap = &m_view;
}

void MinimapScreens::collectBlips(GameSession& session, const HudFrame& hud, u32 nowMs) {
    const game::Entity* me = session.entities().find(session.playerGuid());
    game::BlipQuery q;
    q.x = static_cast<f32>(m_view.position.x);
    q.y = static_cast<f32>(m_view.position.y);
    q.radius = m_radiusYards;
    q.trackCreatures = me ? me->value(uf::PLAYER_TRACK_CREATURES) : 0;
    q.trackResources = me ? me->value(uf::PLAYER_TRACK_RESOURCES) : 0;
    q.map = session.map();
    q.nowMs = nowMs;
    m_blipCount = game::collectMinimapBlips(session.entities(), session.play(), m_minimapData, hud.reactions,
                                            q, m_blips);
}

void MinimapScreens::demoBlips(u32 nowMs) {
    // Development (VD_HUD_DEMO without a server): one blip of each kind around the camera, a party member and
    // the corpse beyond the map (on the rim), and a ping every seven seconds.
    struct Sample {
        BlipKind kind;
        f32 north, west; // yards from the centre
        bool dead;
    };
    static constexpr Sample kSamples[] = {
        {BlipKind::PartyMember, 40.0f, -20.0f, false},     {BlipKind::PartyMember, -30.0f, 35.0f, true},
        {BlipKind::PartyMember, 420.0f, -380.0f, false},   {BlipKind::QuestReward, 15.0f, 60.0f, false},
        {BlipKind::TrackedHostile, -75.0f, -10.0f, false}, {BlipKind::TrackedNeutral, -50.0f, -70.0f, false},
        {BlipKind::TrackedFriendly, 70.0f, 55.0f, false},  {BlipKind::TrackedResource, 95.0f, 40.0f, false},
        {BlipKind::Corpse, -520.0f, 90.0f, false},         {BlipKind::GossipPoi, 25.0f, -55.0f, false},
        {BlipKind::GossipPoi, 300.0f, 400.0f, false}, // phase 13: a guard's point on the map, and its arrow
    };
    const f32 x = static_cast<f32>(m_view.position.x), y = static_cast<f32>(m_view.position.y);
    m_blipCount = 0;
    for (const Sample& s : kSamples) {
        m_blips[m_blipCount++] = game::MinimapBlip{s.kind, 0, x + s.north, y + s.west, s.dead};
    }
    const u32 period = nowMs / 7000;
    if (m_localPing.serial == 0 || m_localPing.startMs / 7000 != period) {
        m_localPing.serial = m_localPing.serial + 1;
        m_localPing.active = true;
        m_localPing.x = x + 45.0f;
        m_localPing.y = y + 30.0f;
        m_localPing.startMs = period * 7000;
    }
}

// --- After the HUD -----------------------------------------------------------------------------------------

void MinimapScreens::drawGame(GameSession& session, const MinimapResult& r, DialogContext& c,
                              SettingsView& settings, u32 nowMs) {
    VD_PROFILE_ZONE("MinimapScreens::drawGame");
    const MinimapLayout layout = minimapLayout(c.safeArea, c.ui.px(ui::kUnitDp), c.ui.dp());
    const bool online = session.inWorld();
    if (r.zoom != 0) {
        u32& zoom = m_indoors ? settings.minimapInsideZoom : settings.minimapZoom;
        const u32 before = std::min(zoom, game::kMinimapZoomLevels - 1);
        zoom = game::minimapZoomStep(before, r.zoom);
        if (zoom != before) {
            m_uiSound(zoom > before ? UiSound::MinimapZoomIn : UiSound::MinimapZoomOut);
        }
        if (m_trace) {
            VD_LOG_INFO("Minimap: %s zoom %u -> %u (%.0f yd across)", m_indoors ? "indoor" : "outdoor",
                        before, zoom, game::minimapRadius(m_indoors, zoom) * 2.0f);
        }
    }
    if (r.toggle) {
        m_shown = !m_shown;
        m_uiSound(m_shown ? UiSound::MinimapOpen : UiSound::MinimapClose);
        if (!m_shown && m_tip != MinimapTip::Zone && m_tip != MinimapTip::Time) {
            m_tip = MinimapTip::None;
        }
    }
    if (r.meetingStoneClicked) { // phase 16 (alerts): OnClick: StaticPopup_Show("CONFIRM_LEAVE_QUEUE")
        if (online && m_popups) {
            m_popups->askLeaveMeetingStoneQueue(session);
        } else {
            VD_LOG_INFO("Minimap: meeting stone button: CONFIRM_LEAVE_QUEUE (offline: no popup)");
        }
    }
    if (r.cancelTracking && online && m_trackingSpell != 0) {
        session.play().cancelAura(m_trackingSpell); // the client's right click: CancelTrackingBuff
    }
    if (r.tappedMap) {
        // Retouches (ui models): Minimap_OnClick's CURSOR_OFFSET_X / _Y (-7, -9 units, y up, of the 70-unit radius):
        // the ping goes that far from the point, and MinimapPing.m2 draws its centre as far the other way
        // (app/MinimapView): the ping shows under the finger, where a 1.12 player's would have gone for a click there.
        constexpr f32 kCursorOffsetX = -7.0f / 70.0f, kCursorOffsetY = 9.0f / 70.0f; // radii, y down
        f32 x = 0.0f, y = 0.0f;
        game::minimapWorldPoint(static_cast<f32>(m_view.position.x), static_cast<f32>(m_view.position.y),
                                m_radiusYards, r.tapX + kCursorOffsetX, r.tapY + kCursorOffsetY, x, y);
        if (online) {
            game::Gameplay& play = session.play();
            const bool sent = play.minimap().ping(x, y, play.social().group().active, nowMs);
            if (m_trace) {
                VD_LOG_INFO("Minimap: ping at %.1f, %.1f (%s)", x, y, sent ? "sent to the group" : "local");
            }
        } else {
            m_localPing.serial = m_localPing.serial + 1;
            m_localPing.active = true;
            m_localPing.x = x;
            m_localPing.y = y;
            m_localPing.startMs = nowMs;
        }
        // The blips under the finger: their names (the client's tooltip over its blips).
        const f32 slop = c.ui.px(kTapSlopDp) / layout.radius;
        m_tappedBlipCount = 0;
        for (u32 i = 0; i < m_blipViewCount && m_tappedBlipCount < m_tappedBlips.size(); ++i) {
            const f32 dx = m_blipViews[i].x - r.tapX, dy = m_blipViews[i].y - r.tapY;
            if (dx * dx + dy * dy <= slop * slop) {
                m_tappedBlips[m_tappedBlipCount++] = {m_blips[i].kind, m_blips[i].guid};
            }
        }
        if (m_tappedBlipCount > 0) {
            openTooltip(MinimapTip::Blips, nowMs);
            const f32 tx = layout.cx + r.tapX * layout.radius, ty = layout.cy + r.tapY * layout.radius;
            m_tipAnchor = {tx - c.ui.px(6.0f), ty - c.ui.px(6.0f), c.ui.px(12.0f), c.ui.px(12.0f)};
        }
    }
    if (r.tapped != MinimapTip::None) {
        if (r.tapped == m_tip) {
            m_tip = MinimapTip::None; // a second tap closes it
        } else {
            openTooltip(r.tapped, nowMs);
            switch (r.tapped) {
            case MinimapTip::Zone: m_tipAnchor = layout.zoneButton(); break;
            case MinimapTip::Time: m_tipAnchor = layout.time(); break;
            case MinimapTip::Tracking: m_tipAnchor = layout.tracking(); break;
            case MinimapTip::Mail: m_tipAnchor = layout.mail(); break;
            case MinimapTip::MeetingStone: m_tipAnchor = layout.meetingStone(); break; // phase 16 (alerts)
            default: break;
            }
        }
    }
    if (m_tip != MinimapTip::None && static_cast<i32>(nowMs - m_tipUntilMs) >= 0) {
        m_tip = MinimapTip::None;
    }
    if (m_tip == MinimapTip::None) {
        return;
    }
    tooltipLines(session);
    drawMinimapTooltip(c.ui, c.draw, m_tipAnchor, {m_tipLines.data(), m_tipLineCount}, c.safeArea);
}

void MinimapScreens::openTooltip(MinimapTip tip, u32 nowMs) {
    m_tip = tip;
    m_tipUntilMs = nowMs + kTooltipMs;
}

void MinimapScreens::tooltipLines(GameSession& session) {
    m_tipLineCount = 0;
    const auto line = [&](std::string_view text) {
        if (!text.empty() && m_tipLineCount < m_tipLines.size()) {
            m_tipLines[m_tipLineCount++] = {text, ui::kWhite};
        }
    };
    switch (m_tip) {
    case MinimapTip::Zone: {
        // MinimapZoneTextButton's OnEnter: the name, then who holds the place (or that it is contested / free
        // for all).
        const game::Travel& travel = session.play().travel();
        const game::ZoneState& zone = travel.zone();
        line(!zone.subzoneName.empty() ? zone.subzoneName : zone.zoneName);
        line(game::zonePvpText(tipBuffer(0), zone.pvp, travel.playerTeam()));
        break;
    }
    case MinimapTip::Time:
        // GameTimeFrame_UpdateTooltip; the 24-hour clock where the locale's client uses it
        // (TwentyFourHourTime).
        line(
            game::gameTimeText(tipBuffer(0), m_minutes, text::get("VD_MINIMAP_TWENTYFOUR_HOUR_TIME") == "1"));
        break;
    case MinimapTip::Tracking:
        if (m_trackingSpell != 0 && m_data) {
            if (const std::optional<game::SpellInfo> spell = m_data->spell(m_trackingSpell)) {
                line(spell->name);
            }
        }
        break;
    case MinimapTip::Mail: line(text::get("HAVE_MAIL")); break;
    case MinimapTip::MeetingStone: { // phase 16 (alerts): GetMeetingStoneStatusText
        const bool online = session.inWorld();
        const game::MeetingStone* stone = online ? &session.play().meetingStone() : nullptr;
        const u32 area = stone ? stone->areaId() : m_meetingStoneDemo;
        if (area == 0) {
            break; // out of the queue: the button is gone
        }
        // UNKNOWN until this world's SMSG_MEETINGSTONE_SETQUEUE (0x4C9F40), then MEETINGSTONE_TOOLTIP with the
        // area.
        const std::string_view name = m_data ? m_data->areaName(area) : std::string_view{};
        if (stone && !stone->statusKnown()) {
            line(text::get("UNKNOWN"));
        } else {
            line(text::format(tipBuffer(0), "MEETINGSTONE_TOOLTIP",
                              name.empty() ? text::get("UNKNOWN") : name));
        }
        break;
    }
    case MinimapTip::Blips:
        for (u32 i = 0; i < m_tappedBlipCount; ++i) {
            const game::MinimapBlip blip{m_tappedBlips[i].kind, m_tappedBlips[i].guid, 0.0f, 0.0f, false};
            line(game::minimapBlipName(session.entities(), session.play(), blip));
        }
        break;
    default: break;
    }
    if (m_tipLineCount == 0) {
        m_tip = MinimapTip::None; // nothing to say (a name not known yet, the tracking gone)
    }
}

} // namespace vd
