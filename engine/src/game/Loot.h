#pragma once

#include "core/Types.h"
#include "net/GameMessages.h"

#include <span>
#include <vector>

namespace vd::game {

// One item of the loot window.
struct LootSlot {
    u8 slot = 0; // what CMSG_AUTOSTORE_LOOT_ITEM asks for (not the position in the list)
    u32 itemId = 0;
    u32 count = 0;
    u32 displayId = 0;
    u32 randomPropertyId = 0;
    // LootSlotType: 0 allowed, 1 roll ongoing, 2 master looter, 3 locked, 4 owner.
    u8 slotType = net::kLootSlotAllowLoot;
    bool taken = false; // SMSG_LOOT_REMOVED

    // The player may take it now.
    bool lootable() const { return !taken && (slotType == net::kLootSlotAllowLoot || slotType == 4); }
};

// The loot window (SMSG_LOOT_RESPONSE until SMSG_LOOT_RELEASE_RESPONSE or the player closes it).
class LootWindow {
public:
    LootWindow() { m_items.reserve(48); } // MAX_NR_LOOT_ITEMS + MAX_NR_QUEST_ITEMS

    bool open() const { return m_open; }
    u64 guid() const { return m_guid; }    // the corpse, chest...
    u8 lootType() const { return m_type; } // net::kLootType*
    u32 gold() const { return m_gold; }    // copper still there
    std::span<const LootSlot> items() const { return m_items; }
    const LootSlot* find(u8 slot) const;
    // Nothing left to take.
    bool empty() const;

    // Opens it (normal form; an error form leaves it as it was and returns false).
    bool onResponse(const net::LootResponse& m);
    void onRemoved(u8 slot);
    // Phase 8: a group roll on `slot` of `object`'s loot ended with everybody passing: the item may be taken.
    void onRollEnded(u64 object, u8 slot);
    void onMoneyCleared() { m_gold = 0; }
    void onReleased(u64 guid);
    void close();

private:
    bool m_open = false;
    u64 m_guid = 0;
    u8 m_type = 0;
    u32 m_gold = 0;
    std::vector<LootSlot> m_items;
};

} // namespace vd::game
