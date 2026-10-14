#pragma once

#include "core/Types.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/BattlefieldListMessages.h"
#include "net/Opcodes.gen.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

class Battlegrounds;
class EntityStore;
class Social;
class Travel;

// UNIT_NPC_FLAG_BATTLEMASTER (vMaNGOS UnitDefines.h NPCFlags).
constexpr u32 kNpcFlagBattlemaster = 0x0800;

// The client's own lines about the queue (BattlefieldNotice events), worded by game::NoticeFeed
// (game/BattlefieldLines.cpp). WoW.exe's error table (0x496720: entries 439..442) gives the group's lines
// their place: the first two are UI_INFO_MESSAGE (yellow, UIErrorsFrame), the next two UI_ERROR_MESSAGE
// (red); BattlefieldFrame_OnUpdate writes the closing messages into the chat as SYSTEM lines.
enum class BattlefieldLine : u8 {
    GroupJoined,      // ERR_GROUP_JOIN_BATTLEGROUND_S: id = the map the group queued for
    GroupDeserters,   // ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS (entry 439)
    GroupFailed,      // ERR_GROUP_JOIN_BATTLEGROUND_FAIL (441): a member not eligible (or an unknown map)
    GroupTooMany,     // ERR_GROUP_JOIN_BATTLEGROUND_TOO_MANY (442): JoinBattlefield's own check
    InstanceShutdown, // INSTANCE_SHUTDOWN_MESSAGE: value[1] = the seconds it says (no winner yet)
    InstanceComplete, // INSTANCE_COMPLETE_MESSAGE: ... after the victory
};
// Its GlobalStrings key, and where it goes: 0 the chat (SYSTEM), 1 UI_INFO_MESSAGE, 2 UI_ERROR_MESSAGE.
const char* battlefieldLineKey(BattlefieldLine line);
u32 battlefieldLinePlace(BattlefieldLine line);

// GetBattlefieldInfo (WoW.exe 0x4AB0B0) of the list shown: Map.dbc's name and the description for the
// player's faction, the battleground's levels and players per side, the bracket's levels (0..0 when its row
// has no bracket size, Alterac Valley's) and whether a group may join it together (CanJoinBattlefieldAsGroup,
// 0x4AC380).
struct BattlefieldInfo {
    std::string_view name;
    std::string_view description;
    u32 minLevel = 0, maxLevel = 0, maxPlayers = 0;
    u32 bracketMinLevel = 0, bracketMaxLevel = 0;
    bool groupJoin = false;
};

// Phase 17 (queue): the battlemaster's list and joining a queue as WoW.exe 1.12.1 does them (read-only
// disassembly, the Lua API of BattlefieldFrame.lua in brackets), against vMaNGOS' BattleGroundHandler:
//  - A battlemaster's right click (0x5F0130, the NPC flags in order): one with UNIT_NPC_FLAG_GOSSIP gets
//    CMSG_GOSSIP_HELLO (its GOSSIP_OPTION_BATTLEFIELD answers the list: every battlemaster of vMaNGOS' world
//    database is GOSSIP | BATTLEMASTER, npc_flags 2049); one without, CMSG_BATTLEMASTER_HELLO (0x5E01A0) ->
//    hello().
//  - SMSG_BATTLEFIELD_LIST (0x4AA6C0) keeps the GUID (0xB6EBB0), the map (0xB6EBA4), the bracket's levels
//    (0xB6EBA8 / AC: Map.dbc MinLevel + bracket * its 39th field, at most 60) and the instance ids, then
//    BATTLEFIELDS_SHOW, and the player's position (0xB6E870): walking 5.56 yd away from it (0x4AA090, each
//    frame; the threshold is 0x806574 squared) fires BATTLEFIELDS_CLOSED once. A portal's area trigger, the
//    GM's .go warsong and ShowBattlefieldList answer the same list with the PLAYER's GUID.
//  - SetSelectedBattlefield(i) keeps the id of instance i (0: "First Available", any i out of the list: 0);
//    GetSelectedBattlefield finds it again (0 when not in the list). CloseBattlefield sends nothing.
//  - JoinBattlefield(i, asGroup) (0x4A9F60): as a group, more party (GetNumPartyMembers) or raid members than
//    the map's MaxPlayers is ERR_GROUP_JOIN_BATTLEGROUND_TOO_MANY and nothing is sent; otherwise
//    CMSG_BATTLEMASTER_JOIN (the list's GUID, the map, instance i's id or 0, asGroup), CMSG_BATTLEFIELD_JOIN
//    without a GUID.
//  - ShowBattlefieldList(i) (0x4AB8C0): CMSG_BATTLEFIELD_LIST with the map of queue i, only while "queued".
//  - SMSG_GROUP_JOINED_BATTLEGROUND (0x4AACC0): -2 the deserters' line, a known map
//    ERR_GROUP_JOIN_BATTLEGROUND_S with its name, anything else the FAIL line.
//  - Every world entry (0x4A9DB0) forgets the list, the selection and the position.
// And what BattlefieldFrame.lua does with game::Battlegrounds' statuses (BattlefieldFrame_UpdateStatus on
// each UPDATE_BATTLEFIELD_STATUS, here once per frame that had any): a queue "queued" that was not at the
// last update (IsAlreadyInQueue: its map and instance) is a BattlefieldQueueJoined (PVPENTERQUEUE, the
// minimap's shine), every queue in "confirm" a BattlefieldConfirm (PVPTHROUGHQUEUE and
// CONFIRM_BATTLEFIELD_ENTRY shown again), an "active" one starts BATTLEFIELD_SHUTDOWN_TIMER at
// GetBattlefieldInstanceExpiration; BattlefieldFrame_OnUpdate (UIParent's OnUpdate) counts it down and every
// 3 s writes INSTANCE_SHUTDOWN_MESSAGE / INSTANCE_COMPLETE_MESSAGE (GetBattlefieldWinner) as it crosses the
// thresholds 600, 300, 60 and 15 s. A world entry stops it (WorldStateScoreFrame's PLAYER_ENTERING_WORLD); a
// new character starts the IsAlreadyInQueue list over (a fresh interface: its queues play PVPENTERQUEUE
// again). Main thread; nothing allocates once warmed up (the list's vector keeps its capacity).
//
// Events (Gameplay::events()): BattlefieldsShow (id = the map), BattlefieldsClosed, BattlefieldQueueJoined
// (id = the queue slot), BattlefieldConfirm (id = the queue slot), BattlefieldNotice (value[0] =
// BattlefieldLine, id = the map, value[1] = seconds).
class BattlefieldQueueing {
public:
    using Events = EventRing<GameEvent, 64>;
    // BATTLEFIELD_TIMER_THRESHOLDS (seconds) and BATTLEFIELD_TIMER_DELAY.
    static constexpr std::array<u32, 4> kShutdownThresholdsMs = {600000, 300000, 60000, 15000};
    static constexpr u32 kShutdownCheckMs = 3000;
    // 0x4AA090: BATTLEFIELDS_CLOSED beyond this from where the list came (0x806574: 5.5555553 yd).
    static constexpr f32 kCloseDistance = 5.5555553f;

    BattlefieldQueueing(const EntityStore& entities, const Social& social, const Travel& travel,
                        const Battlegrounds& battlegrounds, Events& events);

    void setSink(PacketSink sink) { m_sink = sink; }
    // Every world entry (also a new map); `newCharacter` starts the queues' sounds over.
    void enterWorld(u64 player, bool newCharacter, u32 nowMs);
    void leaveWorld();
    static bool handles(net::Opcode opcode);
    // SMSG_BATTLEFIELD_LIST and SMSG_GROUP_JOINED_BATTLEGROUND. False when malformed.
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    // Each frame: the walk-away check, the statuses that changed, the closing messages.
    void update(u32 nowMs);

    // A battlemaster without a gossip menu tapped (Gameplay::interact): CMSG_BATTLEMASTER_HELLO.
    bool hello(u64 battlemaster);

    // The list (BATTLEFIELDS_SHOW): its map (0: none), bracket and GUID.
    u32 mapId() const { return m_list.mapId; }
    u8 bracketId() const { return m_list.bracketId; }
    u64 battlemaster() const { return m_list.battlemaster; }
    // GetNumBattlefields and GetBattlefieldInstanceInfo(index + 1) (0 out of the list).
    u32 count() const { return static_cast<u32>(m_list.instanceIds.size()); }
    u32 instanceId(u32 index) const { return index < count() ? m_list.instanceIds[index] : 0; }
    // GetBattlefieldInfo; false without a list (or an unknown map).
    bool info(BattlefieldInfo& out) const;
    // CanJoinBattlefieldAsGroup, and whether Join as Group is enabled ((GetNumPartyMembers() > 0 or
    // GetNumRaidMembers() > 0) and IsPartyLeader()).
    bool canJoinAsGroup() const;
    bool groupLeader() const;
    // SetSelectedBattlefield(index) / GetSelectedBattlefield: 0 "First Available", n the n-th instance.
    void select(u32 index);
    u32 selected() const;
    // JoinBattlefield(index, asGroup). False when nothing was sent (no list, too many, not in the world).
    bool join(u32 index, bool asGroup, u32 nowMs);
    // ShowBattlefieldList(slot + 1). False when not sent (the queue is not "queued").
    bool showList(u32 slot);

    // BATTLEFIELD_SHUTDOWN_TIMER (ms; 0: not counting).
    i32 shutdownMs() const { return m_shutdownMs; }
    // Lists received so far (a view rebuilds when it moves).
    u32 changes() const { return m_changes; }

    // Development (gallery pages, VD_BATTLEFIELD_* switches): as if the server had sent this list.
    void demoList(const net::BattlefieldList& list, u32 nowMs);

private:
    void applyList(u32 nowMs);     // m_incoming becomes the list
    void syncStatus();             // BattlefieldFrame_UpdateStatus (not its tooltip part)
    void countDown(u32 elapsedMs); // BattlefieldFrame_OnUpdate
    void notice(BattlefieldLine line, u32 mapId, u32 seconds, u32 nowMs);
    GameEvent& event(GameEventKind kind, u32 nowMs);
    bool sendBody(net::Opcode opcode);
    bool playerPosition(f32& x, f32& y, f32& z, u32 nowMs) const;

    const EntityStore& m_entities;
    const Social& m_social;
    const Travel& m_travel;
    const Battlegrounds& m_battlegrounds;
    Events& m_events;
    PacketSink m_sink;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u64 m_player = 0;
    u32 m_nowMs = 0;

    net::BattlefieldList m_list;     // 0xB6EBB0 / A4 / the ids at 0xB6E868
    net::BattlefieldList m_incoming; // parsed into, then swapped with m_list
    u32 m_bracketMinLevel = 0;       // 0xB6EBA8
    u32 m_bracketMaxLevel = 0;       // 0xB6EBAC
    u32 m_selectedInstance = 0;      // 0xB6EBA0
    bool m_watching = false;         // 0xB6E870..78 not zero: the walk-away check runs
    f32 m_listX = 0.0f, m_listY = 0.0f, m_listZ = 0.0f;
    u32 m_changes = 0;

    // BattlefieldFrame_UpdateStatus' CURRENT_BATTLEFIELD_QUEUES: the queued ones' map and instance.
    struct QueuedKey {
        u32 mapId = 0;
        u32 instanceId = 0;
    };
    std::array<QueuedKey, 3> m_queued{};
    u32 m_queuedCount = 0;
    u32 m_statusChanges = 0;  // game::Battlegrounds::changes() at the last update
    i32 m_shutdownMs = 0;     // BATTLEFIELD_SHUTDOWN_TIMER
    u32 m_thresholdIndex = 0; // BATTLEFIELD_TIMER_THRESHOLD_INDEX - 1
    u32 m_previousMod = 0;    // PREVIOUS_BATTLEFIELD_MOD
    u32 m_checkDelayMs = 0;   // BattlefieldFrame.timerDelay
    u32 m_lastUpdateMs = 0;
    bool m_updated = false;
};

} // namespace vd::game
