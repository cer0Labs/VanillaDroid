#pragma once

#include "core/Types.h"
#include "game/Entities.h"

#include <array>
#include <optional>
#include <span>

namespace vd::game {

// Parity (auras): the temporary enchantments of the player's weapons (poisons, oils, sharpening stones: the
// item's TEMP_ENCHANTMENT_SLOT), as BuffFrame's TempEnchant buttons show them (GetWeaponEnchantInfo). The
// item's ITEM_FIELD_ENCHANTMENT duration is only written when the server saves the item; how long one has
// left comes in SMSG_ITEM_ENCHANT_TIME_UPDATE, sent when it is applied and at login
// (Player::AddEnchantmentDuration, Player::SendEnchantmentDurations), and the client counts it down.
class EnchantTimers {
public:
    static constexpr u32 kCapacity = 16; // the oldest is replaced past this (only the weapons' matter)

    void reset();
    // SMSG_ITEM_ENCHANT_TIME_UPDATE (net::parseItemEnchantTimeUpdate). False when malformed.
    bool handle(std::span<const u8> payload, u32 nowMs);
    // `seconds` 0: the enchantment is gone.
    void onTimeUpdate(u64 item, u32 slot, u32 seconds, u32 nowMs);
    // What the enchantment in `slot` of `item` has left, nullopt when the server never said.
    std::optional<u32> remainingMs(u64 item, u32 slot, u32 nowMs) const;

private:
    struct Timer {
        u64 item = 0;
        u32 slot = 0;
        u32 durationMs = 0;
        u32 receivedMs = 0;
    };
    std::array<Timer, kCapacity> m_timers{};
    u32 m_next = 0;
};

constexpr u32 kEquipMainHand = 15; // EQUIPMENT_SLOT_MAINHAND (the client's inventory slot 16)
constexpr u32 kEquipOffHand = 16;  // EQUIPMENT_SLOT_OFFHAND (17)

// A weapon with a temporary enchantment.
struct WeaponEnchant {
    u32 equipSlot = 0; // kEquipMainHand / kEquipOffHand
    u64 item = 0;      // the weapon
    u32 itemEntry = 0;
    u32 enchantId = 0;   // SpellItemEnchantment.dbc
    u32 remainingMs = 0; // 0: unknown (no time update yet, and nothing in the item's field)
    u32 charges = 0;     // poisons: applications left
};

// The player's weapons with a temporary enchantment, as BuffFrame_Enchant_OnUpdate orders them: the off hand
// first (TempEnchant1, the rightmost button), then the main hand. Returns how many were written (0..2).
u32 weaponEnchants(const EntityStore& entities, const Entity& player, const EnchantTimers& timers, u32 nowMs,
                   std::span<WeaponEnchant> out);

} // namespace vd::game
