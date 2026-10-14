#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/Opcodes.gen.h"

#include <span>
#include <vector>

namespace vd::game {

class Reactions;

// Phase 17 (honor): the battleground graveyards' spirit guides (UNIT_NPC_FLAG_SPIRITGUIDE) as WoW.exe 1.12.1
// follows them for a ghost (GameUI.cpp, read-only disassembly 0x4921C0..0x4924C0) against vMaNGOS'
// battleground scripts (scripts/battlegrounds/battleground.cpp npc_spirit_guide,
// Handlers/BattleGroundHandler.cpp):
//  - Every frame while the player is a ghost (0x4923B0, from the game UI's update 0x4818CA): without a
//    healer, the first spirit guide within 20 yd the player can assist (0x4924C0: UnitCanAssist, 0x6066F0)
//    becomes it; one already chosen is dropped beyond 22 yd (20 x 1.1) or when it is gone. Alive
//    (PLAYER_UNGHOST, 0x5DEA20), none.
//  - A new healer (0x4921C0) sends CMSG_AREA_SPIRIT_HEALER_QUERY with its GUID. Dropping one whose time had
//    come cancels the queue first: CancelAura(2584) (0x6E7040): CMSG_CANCEL_AURA "Waiting to Resurrect", and
//    AREA_SPIRIT_HEALER_OUT_OF_RANGE (that function fires it for 2584 whatever cancels the aura).
//  - SMSG_AREA_SPIRIT_HEALER_TIME (0x492290) for the current healer with a time: the next wave at now + ms,
//    and AREA_SPIRIT_HEALER_IN_RANGE. A time of 0 (the guide not channeling) or another unit's: nothing.
//  - GetAreaSpiritHealerTime (0x48DEE0 -> 0x4922E0): the seconds left, truncated (ms / 1000).
//  - AcceptAreaSpiritHeal (0x492310: the AREA_SPIRIT_HEAL popup's OnShow): CMSG_AREA_SPIRIT_HEALER_QUEUE with
//    the healer (the server then casts Waiting to Resurrect on the player, the wave of Spirit Heal 22012
//    resurrects it). CancelAreaSpiritHeal (0x4923A0, the popup's Cancel): CancelAura(2584) alone; the healer
//    and its time stay.
//  - Tapping a spirit guide without a gossip menu (Player_C's NPC chain, 0x5F03D5 -> 0x5DF950): for a ghost,
//    the healer is dropped and that guide chosen again (a fresh CMSG_AREA_SPIRIT_HEALER_QUERY); alive,
//    nothing.
// In a battleground the server releases the ghost at the nearest graveyard
// (BattleGround::GetClosestGraveYard) with Waiting to Resurrect already on it (Player::BuildPlayerRepop): the
// client finds its spirit guide there itself. Main thread; nothing allocates once warmed up.
//
// Events (Gameplay::events()): AreaSpiritHealerInRange (guid = the healer, value[0] = ms left),
// AreaSpiritHealerOutOfRange.
class AreaSpiritHealer {
public:
    using Events = EventRing<GameEvent, 64>;
    static constexpr u32 kWaitingToResurrect = 2584; // the queue's aura
    static constexpr f32 kFindRange = 20.0f;         // 0x4924C0 (0x8044D0)
    static constexpr f32 kKeepRange = 22.0f;         // 0x4923B0: 20 x 1.1 (0x804580)

    AreaSpiritHealer(const EntityStore& entities, Events& events);

    void setSink(PacketSink sink) { m_sink = sink; }
    void enterWorld(u64 player, u32 nowMs);
    void leaveWorld();
    static bool handles(net::Opcode opcode);
    // SMSG_AREA_SPIRIT_HEALER_TIME. False when malformed.
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    // Once per frame (0x4923B0). `reactions`: who the player may assist (null: no spirit guide qualifies).
    void update(u32 nowMs, const Reactions* reactions);

    // The tap on spirit guide `guid` (0x5DF950): for a ghost, chosen again (and queried); false alive.
    bool select(u64 guid, u32 nowMs);
    // AcceptAreaSpiritHeal: CMSG_AREA_SPIRIT_HEALER_QUEUE (false without a healer, or not sent).
    bool accept();
    // CancelAreaSpiritHeal: CMSG_CANCEL_AURA 2584 and AreaSpiritHealerOutOfRange.
    void cancel();
    // A Waiting to Resurrect aura cancelled elsewhere (the buff frame): AREA_SPIRIT_HEALER_OUT_OF_RANGE, as
    // 0x6E7040 fires it for that spell. Gameplay::cancelAura calls it after sending the packet.
    void onAuraCancelled(u32 spellId);

    u64 healer() const { return m_healer; }
    // Until the next wave (0x4922E0): 0 without a time or once it passed.
    u32 timeLeftMs(u32 nowMs) const;
    // GetAreaSpiritHealerTime: whole seconds, truncated.
    u32 timeLeftSeconds(u32 nowMs) const { return timeLeftMs(nowMs) / 1000; }

    // Development (VD_PVP_DEMO=spirit, the gallery): as if `healer` were in range with `timeMs` to the wave.
    void demoInRange(u64 healer, u32 timeMs, u32 nowMs);

private:
    void set(u64 guid, u32 nowMs); // 0x4921C0
    bool query(u64 guid);          // CMSG_AREA_SPIRIT_HEALER_QUERY
    void cancelAura();
    GameEvent& event(GameEventKind kind); // stamped with the last call's clock

    const EntityStore& m_entities;
    Events& m_events;
    PacketSink m_sink;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u64 m_player = 0;
    u64 m_healer = 0;   // 0xB4E330
    u32 m_waveAtMs = 0; // 0xB4E338: client clock of the next wave (0: no time)
    bool m_hasTime = false;
    u32 m_nowMs = 0;
    const Reactions* m_reactions = nullptr; // the last update()'s, for the packet's own look at the healer
};

} // namespace vd::game
