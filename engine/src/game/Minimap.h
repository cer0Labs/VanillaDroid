#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/GameEvents.h"
#include "net/MinimapMessages.h"
#include "net/MiscMessages.h"
#include "net/Opcodes.gen.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::game {

class Gameplay;
class MinimapData;
class Reactions;

// The minimap of the 1.12 client (parity pass): what it shows and how far, as its Minimap frame (FrameXML
// Minimap.xml / Minimap.lua, GameTime.lua) and the engine behind it do. Pure functions over the game state
// plus the group's ping (MSG_MINIMAP_PING), which game::Gameplay owns. The HUD draws it (app/MinimapView);
// app/MinimapScreens fills it.

// --- Zoom --------------------------------------------------------------------------------------------------

// Six zoom levels (Minimap:GetZoomLevels), 0 the farthest; the client keeps one for the outdoors (CVar
// minimapZoom) and one inside buildings (minimapInsideZoom), both 3 by default (their CVar::Register defaults
// in WoW.exe 5875).
constexpr u32 kMinimapZoomLevels = 6;
constexpr u32 kMinimapDefaultZoom = 3;
// The yards the map spans across at each level: the 1.12 client's sizes as the map addons of the time
// measured them (Astrolabe's MinimapSize, indoor and outdoor; the client computes them in code, not in a
// table).
constexpr f32 kMinimapOutdoorYards[kMinimapZoomLevels] = {466.0f + 2.0f / 3.0f, 400.0f, 333.0f + 1.0f / 3.0f,
                                                          266.0f + 2.0f / 3.0f, 200.0f, 133.0f + 1.0f / 3.0f};
constexpr f32 kMinimapIndoorYards[kMinimapZoomLevels] = {300.0f, 240.0f, 180.0f, 120.0f, 80.0f, 50.0f};
// Radius in yards shown at `zoom` (clamped to the levels).
f32 minimapRadius(bool indoors, u32 zoom);
// Minimap_ZoomInClick / ZoomOutClick: `steps` levels in (> 0) or out (< 0), kept within 0..5.
u32 minimapZoomStep(u32 zoom, i32 steps);

// --- Where things fall on it -------------------------------------------------------------------------------

// A world point on the minimap (north up, west left: world +X is up, +Y left): its offset from the centre in
// radii, x to the right and y down, clamped to `rim` radii when beyond it (`outside` then set: the client's
// rim arrows), and the screen angle from the centre towards it (radians clockwise from up) the rim arrows
// point along.
struct MinimapPoint {
    f32 x = 0.0f, y = 0.0f;
    f32 angle = 0.0f;
    bool outside = false;
};
MinimapPoint minimapPoint(f32 centerX, f32 centerY, f32 radiusYards, f32 worldX, f32 worldY, f32 rim = 1.0f);
// The world point under an offset (radii, x right, y down) from the centre: where a tap on the map pings.
void minimapWorldPoint(f32 centerX, f32 centerY, f32 radiusYards, f32 offsetX, f32 offsetY, f32& worldX,
                       f32& worldY);

// --- Blips -------------------------------------------------------------------------------------------------

// What a blip stands for. The client draws them with Interface\Minimap\ObjectIcons.blp, a 128 x 128 sheet of
// 32 x 32 cells of which five are drawn (0 yellow, 1 red, 2 green, 3 yellow again, 4 blue) and no table says
// which is which, so the choice follows the evidence there is:
// - quest givers: vMaNGOS QuestDef.h annotates DIALOG_STATUS_REWARD2 (7, a quest to turn in) "yellow dot on
//   minimap" and DIALOG_STATUS_REWARD_OLD (6) "red dot on minimap"; no other status (an available quest, "!")
//   has one, so the 1.12 minimap shows only where quests can be turned in. vMaNGOS never sends 6.
// - tracked creatures (PLAYER_TRACK_CREATURES): colored by how they look to the player, like their names:
//   hostile red, neutral yellow, friendly green (the three colors of the sheet).
// - tracked resources (herbs, minerals, treasure): yellow, the sheet's second yellow cell.
// - party members: the blue cell; beyond the rim, Rotating-MinimapGroupArrow pointing at them.
// - the player's corpse while a ghost: ROTATING-MINIMAPCORPSEARROW, on the rim when beyond it.
enum class BlipKind : u8 {
    PartyMember,
    QuestReward,    // DIALOG_STATUS_REWARD2
    QuestRewardOld, // DIALOG_STATUS_REWARD_OLD
    TrackedHostile,
    TrackedNeutral,
    TrackedFriendly,
    TrackedResource,
    Corpse,
    // Phase 13: the gossip point (SMSG_GOSSIP_POI: a guard's directions), its Interface\Minimap\POIIcons cell;
    // beyond the map the rotating guide arrow on the rim (the Minimap's minimapArrowModel, like the corpse's).
    GossipPoi,
};
constexpr u32 kNoBlipCell = 0xFFFFFFFFu;
// The ObjectIcons.blp cell of a kind (row-major, 4 per row), kNoBlipCell for the corpse (its own arrow).
u32 blipCell(BlipKind kind);
// Kinds that stay on the rim, as an arrow, when beyond the map (the others are left out there).
bool blipOnRim(BlipKind kind);

struct MinimapBlip {
    BlipKind kind = BlipKind::PartyMember;
    u64 guid = 0;           // the unit, object or member (0: the corpse)
    f32 x = 0.0f, y = 0.0f; // world
    bool dead = false;      // a party member dead or a ghost
};

struct BlipQuery {
    f32 x = 0.0f, y = 0.0f; // the minimap's centre: the player
    f32 radius = 0.0f;      // yards shown: blips beyond are left out, but for those that ride the rim
    u32 trackCreatures = 0; // PLAYER_TRACK_CREATURES: bit (CreatureType.dbc id - 1)
    u32 trackResources = 0; // PLAYER_TRACK_RESOURCES: bit (LockType.dbc id - 1)
    u32 map = 0;            // Map.dbc id shown (the corpse must lie on it)
    u32 nowMs = 0;
};
constexpr u32 kCreatureTypeHumanoid = 7; // CreatureType.dbc: players are humanoids for Track Humanoids
// The blips around the player up to out.size(), the corpse and the party members first, then what stands
// within the map (quest givers with a quest to turn in, tracked creatures and resources); returns the count
// written. Templates not known yet (creature types, chest locks) leave their blip out until the server
// answered (nothing is asked here). `data` / `reactions` may be null (no resources tracked / everything
// neutral).
u32 collectMinimapBlips(const EntityStore& entities, const Gameplay& play, const MinimapData* data,
                        const Reactions* reactions, const BlipQuery& q, std::span<MinimapBlip> out);
// The name a tap on a blip shows (the client's tooltip over its blips): the member's, player's, creature's or
// object's; CORPSE for the corpse; empty while unknown.
std::string_view minimapBlipName(const EntityStore& entities, const Gameplay& play, const MinimapBlip& blip);
// The tracking spell among the player's auras (the client's GetTrackingTexture), 0 when none.
u32 activeTrackingSpell(const Gameplay& play, u64 player, const MinimapData& data);

// --- The day / night dial (GameTimeFrame) ------------------------------------------------------------------

// GameTime.lua: the moon from 21:00 (GAMETIME_DUSK) until 5:30 (GAMETIME_DAWN), the sun the rest of the day.
// The 1.12 dial does not turn: it swaps between the two halves of UI-TOD-Indicator.
constexpr u32 kGameTimeDawn = 5 * 60 + 30;
constexpr u32 kGameTimeDusk = 21 * 60;
bool gameTimeNight(u32 minutesOfDay);
// What its tooltip shows (GameTimeFrame_UpdateTooltip): TIME_TWENTYFOURHOURS, or TIME_TWELVEHOURAM / PM with
// 12 for the hours 0 and 12. Written into `out`.
std::string_view gameTimeText(std::span<char> out, u32 minutesOfDay, bool twentyFourHours);

// --- The ping (MSG_MINIMAP_PING) ---------------------------------------------------------------------------

constexpr u32 kMinimapPingMs = 5000;    // MINIMAPPING_TIMER: shown this long...
constexpr u32 kMinimapPingFadeMs = 500; // ...then MINIMAPPING_FADE_TIMER more (its fade: see minimapPingAlpha)
struct MinimapPingState {
    u32 serial = 0;         // bumped by every ping (0: none yet); the HUD plays MapPing when it changes
    bool active = false;    // false after a world change (its ping is gone)
    u64 sender = 0;         // the player's own GUID for its own pings
    f32 x = 0.0f, y = 0.0f; // world
    u32 startMs = 0;
};
// The ping's opacity `nowMs` into its life (Minimap_OnUpdate): 1, also through its "fade" (retouches, ui models:
// MiniMapPing:SetAlpha(255 * timer / MINIMAPPING_FADE_TIMER) is above 1 until its last 2 ms, and Frame:SetAlpha clamps
// it, WoW.exe 0x774E90); 0 once over (or none).
f32 minimapPingAlpha(const MinimapPingState& ping, u32 nowMs);

// Phase 13: the minimap's gossip point (SMSG_GOSSIP_POI). The 1.12 client keeps one (its second minimap POI slot,
// the first being the corpse): a new one replaces it; it stays until another comes or the world changes.
struct GossipPointState {
    bool active = false;
    u32 serial = 0;         // bumped by every new point
    f32 x = 0.0f, y = 0.0f; // world, on the map the player was on
    u32 icon = 0;           // POIIcons cell (an 8 x 8 grid)
    std::string name;
};

// The group's minimap pings: the last one shown (the client moves a single MiniMapPing model to each new
// one). Owned by game::Gameplay, which hands it MSG_MINIMAP_PING. Main thread.
class Minimap {
public:
    Minimap() { m_body.reserve(8); }

    void setSink(PacketSink sink) { m_sink = sink; }
    void enterWorld(u64 player);
    void leaveWorld();
    static bool handles(net::Opcode opcode) {
        return opcode == net::Opcode::MSG_MINIMAP_PING || opcode == net::Opcode::SMSG_GOSSIP_POI;
    }
    // False when malformed.
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    // The player pings the world point (x, y) (Minimap:PingLocation): shown at once; sent when `grouped`, the
    // only case in which the server relays it (to the others, never back). Returns whether it was sent.
    bool ping(f32 x, f32 y, bool grouped, u32 nowMs);
    const MinimapPingState& lastPing() const { return m_ping; }
    const GossipPointState& gossipPoint() const { return m_gossipPoint; } // phase 13

private:
    PacketSink m_sink;
    u64 m_player = 0;
    MinimapPingState m_ping;
    GossipPointState m_gossipPoint;
    net::GossipPoi m_poi; // parsed, reused
    std::vector<u8> m_body;
};

} // namespace vd::game
