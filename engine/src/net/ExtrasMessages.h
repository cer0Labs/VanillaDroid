#pragma once

#include "core/Types.h"

#include <span>
#include <string_view>
#include <vector>

// Phase 18 (extras): small packets vMaNGOS sends that the client used to ignore, as vmangos-core 4641790
// writes and reads them for build 5875: Server/Packets/Movement.cpp (MountSpecialAnim), Pet.cpp
// (PetActionSound), Misc.cpp (InvalidatePlayer, SetRestStart), Item.cpp (ItemNameQueryResponse) and Query.cpp
// (ItemNameQuery). Same rules as net/GameMessages: parsers fill a caller-owned value and return false on a
// malformed packet, never reading past its end; builders append the body of a client packet.
namespace vd::net {

// Server SMSG_MOUNTSPECIAL_ANIM (WorldSession::HandleMountSpecialAnimOpcode, to the players around but the
// sender): u64 the mounted player whose mount rears. Client CMSG_MOUNTSPECIAL_ANIM has no body.
bool parseMountSpecialAnim(std::span<const u8> payload, u64& unit);

// Server SMSG_PET_ACTION_SOUND (Unit::SendPetTalk, to the pet's owner, one time in ten that a summoned pet is
// told to attack or casts a spell it was ordered to): u64 the pet, u32 PetTalk (0 PET_TALK_SPECIAL_SPELL, 1
// PET_TALK_ATTACK).
constexpr u32 kPetTalkSpecialSpell = 0;
constexpr u32 kPetTalkAttack = 1;
struct PetActionSound {
    u64 pet = 0;
    u32 talk = 0;
};
bool parsePetActionSound(std::span<const u8> payload, PetActionSound& out);

// Server SMSG_INVALIDATE_PLAYER (World::InvalidatePlayerDataToAllClient, to every client: a character
// renamed, or its race changed by a GM): u64 the player whose cached name and look are stale.
bool parseInvalidatePlayer(std::span<const u8> payload, u64& player);

// Server SMSG_SET_REST_START (Player::SendInitialPacketsBeforeAddToMap, always 0): u32 the rest's start time.
bool parseSetRestStart(std::span<const u8> payload, u32& time);

// Client CMSG_ITEM_NAME_QUERY: u32 the item entry, u64 a GUID the server skips (0). Server
// SMSG_ITEM_NAME_QUERY_RESPONSE (WorldSession::HandleItemNameQueryOpcode, nothing for an unknown item): u32
// the entry, the name (its locale's when the server has one) with its NUL.
void writeItemNameQuery(std::vector<u8>& out, u32 entry);
struct ItemNameQueryResponse {
    u32 entry = 0;
    std::string_view name;
};
bool parseItemNameQueryResponse(std::span<const u8> payload, ItemNameQueryResponse& out);

} // namespace vd::net
