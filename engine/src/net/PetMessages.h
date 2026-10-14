#pragma once

#include "core/Types.h"
#include "net/ByteBuffer.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

// Phase 15 (pets): the stable master's packets and a pet trainer's untraining offer, as vmangos-core 4641790
// writes and reads them for build 5875 (Handlers/NPCHandler.cpp SendStablePet / SendStableResult /
// HandleStablePet / HandleUnstablePet / HandleBuyStableSlot / HandleStableSwapPet, Handlers/PetHandler.cpp
// HandlePetUnlearnOpcode / HandlePetSetAction, Objects/Player.cpp SendPetSkillWipeConfirm,
// Server/Packets/{Npc,Pet}.cpp). Same rules as net/GameMessages: parsers fill a caller-owned struct and
// return false on a malformed packet without reading past its end; strings are views into the payload.
namespace vd::net {

// --- The stable (server -> client) -------------------------------------------------------------------------

// MSG_LIST_STABLED_PETS (WorldSession::SendStablePet): the stable master, the bought slots and the hunter's
// pets: the current one (summoned and alive, or dismissed / dead from the database) and the stabled ones.
constexpr u8 kStableSlotCurrent =
    1; // the pet's `slot` byte for the current pet ("client slot 1 == current pet")
constexpr u32 kMaxStabledPets = 2; // MAX_PET_STABLES in 1.12 (NUM_PET_STABLE_SLOTS)
struct StabledPetsList {
    struct Pet {
        u32 petNumber = 0; // the character_pet id: what CMSG_UNSTABLE_PET / CMSG_STABLE_SWAP_PET name
        u32 entry = 0;     // the creature it was tamed from (its family, model and icon)
        u32 level = 0;
        std::string_view name;
        u32 loyalty = 0; // its loyalty level (PetLoyalty.dbc)
        u8 slot = 0;     // kStableSlotCurrent, or 1 + the stable slot (2, 3)
    };
    static constexpr u32 kMaxPets = 1 + kMaxStabledPets; // more is malformed
    u64 stableMaster = 0;
    u8 slots = 0; // stable slots bought
    u32 count = 0;
    std::array<Pet, kMaxPets> pets{};
};
bool parseStabledPetsList(std::span<const u8> payload, StabledPetsList& out);

// SMSG_STABLE_RESULT: one byte (StableResultCode, NPCHandler.cpp).
constexpr u8 kStableErrMoney = 0x01;        // "you don't have enough money": ERR_NOT_ENOUGH_MONEY
constexpr u8 kStableErrStable = 0x06;       // most failures: the client says nothing
constexpr u8 kStableSuccessStable = 0x08;   // stabled
constexpr u8 kStableSuccessUnstable = 0x09; // unstabled or swapped
constexpr u8 kStableSuccessBuySlot = 0x0A;  // a slot bought
bool parseStableResult(std::span<const u8> payload, u8& result);

// SMSG_PET_UNLEARN_CONFIRM (Player::SendPetSkillWipeConfirm): the pet and what untraining it costs.
struct PetUnlearnConfirm {
    u64 pet = 0;
    u32 cost = 0; // copper
};
bool parsePetUnlearnConfirm(std::span<const u8> payload, PetUnlearnConfirm& out);

// --- Client -> server --------------------------------------------------------------------------------------
// A single full GUID (net::writeGuid): MSG_LIST_STABLED_PETS (the stable master), CMSG_STABLE_PET,
// CMSG_BUY_STABLE_SLOT (the stable master), CMSG_PET_UNLEARN (the pet).

// CMSG_UNSTABLE_PET / CMSG_STABLE_SWAP_PET: the stable master and the stabled pet's number.
void writeStablePetNumber(std::vector<u8>& out, u64 stableMaster, u32 petNumber);
// CMSG_PET_SET_ACTION with two slots: a move or swap on the pet bar (HandlePetSetAction counts the actions
// from the packet's size: 24 bytes are two). Each position gets the packed button that goes there.
void writePetSetActionPair(std::vector<u8>& out, u64 pet, u32 position0, u32 button0, u32 position1,
                           u32 button1);

} // namespace vd::net
