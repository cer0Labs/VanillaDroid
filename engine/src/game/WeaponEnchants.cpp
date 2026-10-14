#include "game/WeaponEnchants.h"

#include "game/ItemDefs.h"
#include "game/UnitInfo.h"
#include "net/ItemMessages.h"
#include "net/UpdateFields.gen.h"

#include <initializer_list>

namespace vd::game {

namespace uf = net::uf;

void EnchantTimers::reset() {
    m_timers.fill(Timer{});
    m_next = 0;
}

bool EnchantTimers::handle(std::span<const u8> payload, u32 nowMs) {
    net::ItemEnchantTimeUpdate m;
    if (!net::parseItemEnchantTimeUpdate(payload, m)) {
        return false;
    }
    onTimeUpdate(m.item, m.slot, m.durationS, nowMs);
    return true;
}

void EnchantTimers::onTimeUpdate(u64 item, u32 slot, u32 seconds, u32 nowMs) {
    Timer* found = nullptr;
    for (Timer& t : m_timers) {
        if (t.item == item && t.slot == slot) {
            found = &t;
            break;
        }
    }
    if (seconds == 0) {
        if (found) {
            *found = Timer{};
        }
        return;
    }
    if (!found) {
        for (Timer& t : m_timers) {
            if (t.item == 0) {
                found = &t;
                break;
            }
        }
    }
    if (!found) {
        found = &m_timers[m_next];
        m_next = (m_next + 1) % kCapacity;
    }
    *found = Timer{item, slot, seconds * 1000u, nowMs};
}

std::optional<u32> EnchantTimers::remainingMs(u64 item, u32 slot, u32 nowMs) const {
    for (const Timer& t : m_timers) {
        if (t.item == item && t.item != 0 && t.slot == slot) {
            const u32 elapsed = nowMs - t.receivedMs;
            return elapsed < t.durationMs ? t.durationMs - elapsed : 0u;
        }
    }
    return std::nullopt;
}

u32 weaponEnchants(const EntityStore& entities, const Entity& player, const EnchantTimers& timers, u32 nowMs,
                   std::span<WeaponEnchant> out) {
    u32 count = 0;
    for (const u32 slot : {kEquipOffHand, kEquipMainHand}) {
        if (count == out.size()) {
            break;
        }
        const u64 guid = guidValue(player, static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slot * 2));
        const Entity* item = guid != 0 ? entities.find(guid) : nullptr;
        if (!item || item->type != ObjectType::Item || item->values.size() < uf::ITEM_END) {
            continue;
        }
        // ITEM_FIELD_ENCHANTMENT: 3 words a slot (id, duration, charges).
        const u16 base = static_cast<u16>(uf::ITEM_FIELD_ENCHANTMENT + kEnchantTemporary * 3);
        const u32 enchant = item->value(base);
        if (enchant == 0) {
            continue;
        }
        WeaponEnchant& w = out[count++];
        w = WeaponEnchant{};
        w.equipSlot = slot;
        w.item = guid;
        w.itemEntry = item->value(uf::OBJECT_FIELD_ENTRY);
        w.enchantId = enchant;
        w.charges = item->value(static_cast<u16>(base + 2));
        const std::optional<u32> left = timers.remainingMs(guid, kEnchantTemporary, nowMs);
        w.remainingMs = left ? *left : item->value(static_cast<u16>(base + 1));
    }
    return count;
}

} // namespace vd::game
