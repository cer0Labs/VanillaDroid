#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/Opcodes.gen.h"
#include "net/PetMessages.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

// A pet as the stable master lists it (MSG_LIST_STABLED_PETS), WoW.exe's StableInfo record.
struct StabledPet {
    u32 petNumber = 0; // 0: no pet
    u32 entry = 0;     // the creature it was tamed from (CreatureQuery: family, model)
    u32 level = 0;
    u32 loyalty = 0;
    std::array<char, 64> name{};
    std::string_view nameView() const { return name.data(); }
};

// Phase 15 (pets): the hunter's stable (PetStableFrame) and a pet trainer's offer to untrain the pet
// (CONFIRM_PET_UNLEARN), with WoW.exe 1.12.1's rules (StableInfo.cpp: the list handler 0x4CAB10, the result
// handler 0x4CACB0, ClickStablePet 0x4CB420, PickupStablePet 0x4CB7A0, GetSelectedStablePet 0x4CB810,
// BuyStableSlot 0x4CB080; ConfirmPetUnlearn 0x48DCA0). Owned by Gameplay, which hands it its packets; its
// requests go through the same PacketSink, its news to the same event ring (PET_STABLE_SHOW / _UPDATE /
// _CLOSED, CONFIRM_PET_UNLEARN). Indexes are the Lua's: 0 the current pet, 1..2 the stabled ones (in the
// order the server listed them, as WoW.exe keeps them, whatever their slot). Main thread; no allocation.
class PetStable {
public:
    using Events = EventRing<GameEvent, 64>;
    static constexpr u32 kStabledSlots = net::kMaxStabledPets; // NUM_PET_STABLE_SLOTS

    PetStable(const EntityStore& entities, Events& events) : m_entities(entities), m_events(events) {}

    void setSink(PacketSink sink) { m_sink = sink; }
    void enterWorld(u64 player, u32 nowMs);
    void leaveWorld();

    // A packet of this module (see Gameplay::handle): false when malformed.
    static bool handles(net::Opcode opcode);
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);

    // --- The stable (open from MSG_LIST_STABLED_PETS until closed) ---
    bool open() const { return m_master != 0; }
    u64 stableMaster() const { return m_master; }
    u32 slots() const { return m_slots; }               // GetNumStableSlots
    u32 stabledCount() const { return m_stabledCount; } // GetNumStablePets
    // GetStablePetInfo(index): nullptr when that index holds no pet.
    const StabledPet* pet(u32 index) const;
    // GetSelectedStablePet: -1 none, 0 the current pet, 1.. a stabled one.
    i32 selected() const;
    // PetStable_Update's "If no selected pet try to set one": the current pet when the player has a hunter
    // pet out (GetPetIcon), else the first index with a pet. Returns the selection.
    i32 autoSelect(bool petOut);
    // ClickStablePet(index): with a stable pet on the cursor, moves it there (CMSG_STABLE_PET,
    // CMSG_UNSTABLE_PET or CMSG_STABLE_SWAP_PET, as WoW.exe picks them; `petOut`: the player has a pet,
    // UNIT_FIELD_SUMMON) and empties the cursor; without one, selects it. True when the window should update
    // (always, as the client returns 1).
    bool click(i32 index, bool petOut);
    // PickupStablePet(index): that pet on the cursor (nothing when it holds no pet).
    void pickup(i32 index);
    i32 picked() const { return m_picked; } // the index on the cursor, -1 none
    void dropPicked() { m_picked = -1; }    // ClearCursor
    // GetNextStableSlotCost needs StableSlotPrices.dbc (game::PetData): `price` is it for slots() + 1.
    // BuyStableSlot asks only while a slot is left and the money covers it (CMSG_BUY_STABLE_SLOT); false when
    // nothing was sent.
    bool buySlot(u32 price, u32 money);
    // ClosePetStables: the window is gone (PET_STABLE_CLOSED).
    void close();

    // --- A pet trainer's untraining offer (SMSG_PET_UNLEARN_CONFIRM: CONFIRM_PET_UNLEARN) ---
    // The NPC whose gossip option was chosen last (Gameplay::selectGossipOption): the "untrainer" the popup
    // keeps its distance to (CheckPetUntrainerDist), as the server closes the gossip before it asks.
    void noteGossipNpc(u64 npc) { m_gossipNpc = npc; }
    u64 unlearnPet() const { return m_unlearnPet; } // 0: no offer
    u32 unlearnCost() const { return m_unlearnCost; }
    u64 unlearnTrainer() const { return m_unlearnTrainer; }
    // ConfirmPetUnlearn (CMSG_PET_UNLEARN with the pet) or the popup's cancel: the offer is gone.
    void answerUnlearn(bool accept);

private:
    bool sendGuid(net::Opcode opcode, u64 guid);
    bool sendPetNumber(net::Opcode opcode, u32 petNumber);
    GameEvent& event(GameEventKind kind, u32 id = 0, u64 guid = 0);
    void onList(const net::StabledPetsList& m);
    void onResult(u8 result);

    [[maybe_unused]] const EntityStore& m_entities;
    Events& m_events;
    PacketSink m_sink;
    bool m_inWorld = false;
    u64 m_player = 0;
    u32 m_nowMs = 0;
    std::vector<u8> m_body;

    u64 m_master = 0;
    u32 m_slots = 0;
    StabledPet m_current;                              // index 0 (petNumber 0: none)
    std::array<StabledPet, kStabledSlots> m_stabled{}; // indexes 1..
    u32 m_stabledCount = 0;
    // The selected pet's number as WoW.exe keeps it: 0 none, ~0u the current pet.
    u32 m_selected = 0;
    i32 m_picked = -1;

    u64 m_gossipNpc = 0;
    u64 m_unlearnPet = 0;
    u32 m_unlearnCost = 0;
    u64 m_unlearnTrainer = 0;

    net::StabledPetsList m_list; // parsed, reused
};

} // namespace vd::game
