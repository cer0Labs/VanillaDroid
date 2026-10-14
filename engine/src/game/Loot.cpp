#include "game/Loot.h"

namespace vd::game {

const LootSlot* LootWindow::find(u8 slot) const {
    for (const LootSlot& item : m_items) {
        if (item.slot == slot) {
            return &item;
        }
    }
    return nullptr;
}

bool LootWindow::empty() const {
    if (m_gold != 0) {
        return false;
    }
    for (const LootSlot& item : m_items) {
        if (!item.taken) {
            return false;
        }
    }
    return true;
}

bool LootWindow::onResponse(const net::LootResponse& m) {
    if (m.lootType == 0) {
        return false; // error form
    }
    m_open = true;
    m_guid = m.guid;
    m_type = m.lootType;
    m_gold = m.gold;
    m_items.clear();
    for (const net::LootResponse::Item& from : m.items) {
        LootSlot& item = m_items.emplace_back();
        item.slot = from.slot;
        item.itemId = from.itemId;
        item.count = from.count;
        item.displayId = from.displayId;
        item.randomPropertyId = from.randomPropertyId;
        item.slotType = from.slotType;
        item.taken = false;
    }
    return true;
}

void LootWindow::onRemoved(u8 slot) {
    for (LootSlot& item : m_items) {
        if (item.slot == slot) {
            item.taken = true;
        }
    }
}

void LootWindow::onRollEnded(u64 object, u8 slot) {
    for (LootSlot& item : m_items) {
        if (object == m_guid && item.slot == slot && item.slotType == 1) { // LOOT_SLOT_TYPE_ROLL_ONGOING
            item.slotType = net::kLootSlotAllowLoot;
        }
    }
}

void LootWindow::onReleased(u64 guid) {
    if (guid == m_guid || guid == 0) {
        close();
    }
}

void LootWindow::close() {
    m_open = false;
    m_guid = 0;
    m_type = 0;
    m_gold = 0;
    m_items.clear();
}

} // namespace vd::game
