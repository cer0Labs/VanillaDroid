#pragma once

#include "core/Types.h"

#include <optional>
#include <span>
#include <vector>

namespace vd::net {

// Phase 13 (spells): the spell packets of aimed casts and spell modifiers, byte for byte as vMaNGOS reads and
// writes them (Server/Packets/Spell.cpp, Spells/SpellCastTargetsInfo.cpp, Objects/Player.cpp).

// SMSG_SET_FLAT_SPELL_MODIFIER / SMSG_SET_PCT_SPELL_MODIFIER (WorldPackets::Spell::SetSpellModifier, sent by
// Player::SendSpellMod for every bit of a modifier's mask): one bit of the spell family mask, the SpellModOp
// and the sum of all the player's modifiers of that type and op on that bit (the new total, not a change).
struct SpellModifierUpdate {
    u8 bit = 0; // 0..63: the bit of SpellFamilyFlags
    u8 op = 0;  // SpellModOp
    i32 value = 0;
};
std::optional<SpellModifierUpdate> parseSpellModifier(std::span<const u8> payload);

// The trade slot a TARGET_FLAG_TRADE_ITEM cast names (SpellCastTargets::setTradeItemTarget: the packed "GUID"
// is the slot number, TRADE_SLOT_NONTRADED): the other player's item that will not be traded.
constexpr u64 kTradeItemTargetSlot = 6;

} // namespace vd::net
