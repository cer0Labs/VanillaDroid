#include "game/Inventory.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "game/ItemData.h"
#include "game/Spells.h"
#include "game/UnitInfo.h"
#include "net/GameMessages.h"
#include "net/MiscMessages.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cstring>

namespace vd::game {
namespace {

using net::Opcode;
namespace uf = net::uf;

// The DurabilityFrame turns a part yellow at this share of its durability or less (GetInventoryAlertStatus 3; red,
// 4, when broken).
constexpr u32 kLowDurabilityPercent = 20;
constexpr u32 kItemCooldownMs = 30000; // SMSG_ITEM_COOLDOWN: Player::ApplyEquipCooldown

// The worn bag slots, then the bank bag slots: the order of Inventory::m_bags.
constexpr u8 kBagSlots[10] = {19, 20, 21, 22, 63, 64, 65, 66, 67, 68};
// DurabilityFrame.lua INVENTORY_ALERT_STATUS_SLOTS: head, shoulders, chest, waist, legs, feet, wrists, hands, weapon,
// shield, ranged.
constexpr u8 kAlertSlots[DurabilityAlerts::kParts] = {kSlotHead,  kSlotShoulders, kSlotChest,    kSlotWaist,
                                                      kSlotLegs,  kSlotFeet,      kSlotWrists,   kSlotHands,
                                                      kSlotMainHand, kSlotOffHand, kSlotRanged};

f32 floatField(const Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// PLAYER_FIELD_POSSTAT / NEGSTAT / RESISTANCEBUFFMODS*: floats the server sends converted to integers (Object.cpp).
i32 intField(const Entity& e, u16 index) {
    return static_cast<i32>(e.value(index));
}

void mix(u64& hash, u64 value) {
    hash = (hash ^ value) * 1099511628211ull;
}

// ItemCanGoIntoBag (Objects/Item.cpp): a plain bag takes anything; a special one only its family, and no bags.
bool fitsBag(const net::ItemQueryInfo& item, const net::ItemQueryInfo& bag) {
    if (bag.bagFamily != kBagFamilyNone && item.containerSlots != 0) {
        return false;
    }
    if (bag.itemClass == kItemClassContainer) {
        switch (bag.subclass) {
        case 0: return true;
        case 1: return item.bagFamily == kBagFamilySoulShards;
        case 2: return item.bagFamily == kBagFamilyHerbs;
        case 3: return item.bagFamily == kBagFamilyEnchanting;
        default: return false;
        }
    }
    if (bag.itemClass == kItemClassQuiver) {
        return (bag.subclass == kQuiverQuiver && item.bagFamily == kBagFamilyArrows) ||
               (bag.subclass == kQuiverAmmoPouch && item.bagFamily == kBagFamilyBullets);
    }
    return false;
}

} // namespace

i32 CharacterStats::totalAttackPower() const {
    const f32 ap = static_cast<f32>(attackPower + attackPowerPositive + attackPowerNegative);
    return static_cast<i32>(ap * (1.0f + attackPowerMultiplier));
}

i32 CharacterStats::totalRangedAttackPower() const {
    const f32 ap = static_cast<f32>(rangedAttackPower + rangedAttackPowerPositive + rangedAttackPowerNegative);
    return static_cast<i32>(ap * (1.0f + rangedAttackPowerMultiplier));
}

CharacterStats characterStats(const Entity& player) {
    CharacterStats s;
    s.level = player.value(uf::UNIT_FIELD_LEVEL);
    for (u16 i = 0; i < 5; ++i) {
        s.stat[i] = intField(player, static_cast<u16>(uf::UNIT_FIELD_STAT0 + i));
        s.statPositive[i] = intField(player, static_cast<u16>(uf::PLAYER_FIELD_POSSTAT0 + i));
        s.statNegative[i] = intField(player, static_cast<u16>(uf::PLAYER_FIELD_NEGSTAT0 + i));
    }
    for (u16 i = 0; i < 7; ++i) {
        s.resistance[i] = intField(player, static_cast<u16>(uf::UNIT_FIELD_RESISTANCES + i));
        s.resistancePositive[i] = intField(player, static_cast<u16>(uf::PLAYER_FIELD_RESISTANCEBUFFMODSPOSITIVE + i));
        s.resistanceNegative[i] = intField(player, static_cast<u16>(uf::PLAYER_FIELD_RESISTANCEBUFFMODSNEGATIVE + i));
    }
    // UNIT_FIELD_*ATTACK_POWER_MODS: two int16, positive then negative.
    const u32 mods = player.value(uf::UNIT_FIELD_ATTACK_POWER_MODS);
    s.attackPower = intField(player, uf::UNIT_FIELD_ATTACK_POWER);
    s.attackPowerPositive = static_cast<i16>(mods & 0xFFFF);
    s.attackPowerNegative = static_cast<i16>(mods >> 16);
    s.attackPowerMultiplier = floatField(player, uf::UNIT_FIELD_ATTACK_POWER_MULTIPLIER);
    const u32 rangedMods = player.value(uf::UNIT_FIELD_RANGED_ATTACK_POWER_MODS);
    s.rangedAttackPower = intField(player, uf::UNIT_FIELD_RANGED_ATTACK_POWER);
    s.rangedAttackPowerPositive = static_cast<i16>(rangedMods & 0xFFFF);
    s.rangedAttackPowerNegative = static_cast<i16>(rangedMods >> 16);
    s.rangedAttackPowerMultiplier = floatField(player, uf::UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER);
    s.minDamage = floatField(player, uf::UNIT_FIELD_MINDAMAGE);
    s.maxDamage = floatField(player, uf::UNIT_FIELD_MAXDAMAGE);
    s.minOffhandDamage = floatField(player, uf::UNIT_FIELD_MINOFFHANDDAMAGE);
    s.maxOffhandDamage = floatField(player, uf::UNIT_FIELD_MAXOFFHANDDAMAGE);
    s.minRangedDamage = floatField(player, uf::UNIT_FIELD_MINRANGEDDAMAGE);
    s.maxRangedDamage = floatField(player, uf::UNIT_FIELD_MAXRANGEDDAMAGE);
    // Attack times travel as integers (milliseconds).
    s.mainHandSpeedMs = player.value(uf::UNIT_FIELD_BASEATTACKTIME);
    s.offHandSpeedMs = player.value(static_cast<u16>(uf::UNIT_FIELD_BASEATTACKTIME + 1));
    s.rangedSpeedMs = player.value(uf::UNIT_FIELD_RANGEDATTACKTIME);
    s.critChance = floatField(player, uf::PLAYER_CRIT_PERCENTAGE);
    s.rangedCritChance = floatField(player, uf::PLAYER_RANGED_CRIT_PERCENTAGE);
    s.dodgeChance = floatField(player, uf::PLAYER_DODGE_PERCENTAGE);
    s.parryChance = floatField(player, uf::PLAYER_PARRY_PERCENTAGE);
    s.blockChance = floatField(player, uf::PLAYER_BLOCK_PERCENTAGE);
    return s;
}

bool DurabilityAlerts::any() const {
    for (const DurabilityAlert a : parts) {
        if (a != DurabilityAlert::None) {
            return true;
        }
    }
    return false;
}

Inventory::Inventory(const EntityStore& entities, QueryCache& queries, Events& events)
    : m_entities(entities), m_queries(queries), m_events(events) {
    m_body.reserve(64);
    m_pages.reserve(64);
    m_letters.reserve(8);
}

// -------------------------------------------------------------------------------------------------------
// Session.

void Inventory::enterWorld(u64 player, bool newLogin, u32 nowMs) {
    m_nowMs = nowMs;
    m_inWorld = true;
    if (newLogin || player != m_player) {
        m_cooldowns = {};
        m_hash = 0;
        m_itemTimes.clear(); // phase 13
    }
    m_player = player;
    m_dirty = true;
    m_built = false;
    m_vendor.npc = 0;
    m_banker = 0;
    m_reading = ReadingWindow{};
}

void Inventory::leaveWorld() {
    m_inWorld = false;
    // The next login sends its proficiencies again, before SMSG_LOGIN_VERIFY_WORLD (vMaNGOS casts the passive
    // proficiency spells while loading the character): forgotten on entering, they were never known and every
    // item looked usable (no red mail in a druid's bags, the auction's "Usable Items" filter let everything through).
    m_proficiency = {};
    m_proficiencyKnown = false;
    m_vendor.npc = 0;
    m_banker = 0;
    m_reading = ReadingWindow{};
}

// -------------------------------------------------------------------------------------------------------
// The model.

void Inventory::refresh(u32 nowMs, bool force) {
    m_nowMs = nowMs;
    if (m_dirty && (force || !m_built || nowMs - m_builtMs >= kRebuildMs)) {
        m_dirty = false;
        m_built = true;
        m_builtMs = nowMs;
        rebuild(nowMs);
    }
}

void Inventory::readItem(u64 guid, ItemPos pos, ItemInstance& out) {
    out = ItemInstance{};
    out.guid = guid;
    out.pos = pos;
    const Entity* e = m_entities.find(guid);
    if (!e || (e->type != ObjectType::Item && e->type != ObjectType::Container)) {
        out.entry = 0; // its object comes later
        return;
    }
    out.entry = e->value(uf::OBJECT_FIELD_ENTRY);
    out.count = std::max<u32>(e->value(uf::ITEM_FIELD_STACK_COUNT), 1);
    out.durability = e->value(uf::ITEM_FIELD_DURABILITY);
    out.maxDurability = e->value(uf::ITEM_FIELD_MAXDURABILITY);
    out.flags = e->value(uf::ITEM_FIELD_FLAGS);
    for (u32 i = 0; i < kEnchantmentSlots; ++i) {
        out.enchantments[i] = e->value(static_cast<u16>(uf::ITEM_FIELD_ENCHANTMENT + i * 3));
        out.enchantmentDurations[i] = e->value(static_cast<u16>(uf::ITEM_FIELD_ENCHANTMENT + i * 3 + 1));
    }
    for (u32 i = 0; i < out.spellCharges.size(); ++i) {
        out.spellCharges[i] = static_cast<i32>(e->value(static_cast<u16>(uf::ITEM_FIELD_SPELL_CHARGES + i)));
    }
    out.creator = guidValue(*e, uf::ITEM_FIELD_CREATOR);
    out.giftCreator = guidValue(*e, uf::ITEM_FIELD_GIFTCREATOR);
    out.randomPropertyId = e->value(uf::ITEM_FIELD_RANDOM_PROPERTIES_ID);
    out.itemTextId = e->value(uf::ITEM_FIELD_ITEM_TEXT_ID);
    out.durationS = e->value(uf::ITEM_FIELD_DURATION);
    if (const auto told = m_itemTimes.find(guid); told != m_itemTimes.end()) { // phase 13: SMSG_ITEM_TIME_UPDATE
        const u32 elapsed = (m_nowMs - told->second.atMs) / 1000;
        out.durationS = told->second.seconds > elapsed ? told->second.seconds - elapsed : 0;
    }
    if (e->type == ObjectType::Container) {
        out.containerSlots = std::min<u32>(e->value(uf::CONTAINER_FIELD_NUM_SLOTS), kMaxBagSize);
    }
}

void Inventory::rebuild(u32 nowMs) {
    VD_PROFILE_ZONE("Inventory::rebuild");
    (void)nowMs;
    const Entity* p = me();
    m_slots.fill(ItemInstance{});
    for (Bag& bag : m_bags) {
        bag.item = ItemInstance{};
        bag.slots.fill(ItemInstance{});
    }
    const u32 bankSlotsBefore = m_bankBagSlots;
    const bool baseline = m_hash == 0;
    m_money = m_bankBagSlots = m_ammo = 0;
    if (!p) {
        if (m_hash != 0) {
            m_hash = 0;
            ++m_version;
        }
        m_alerts = DurabilityAlerts{};
        return;
    }
    for (u8 slot = 0; slot < kPlayerSlotCount; ++slot) {
        const u64 guid = guidValue(*p, static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slot * 2));
        if (guid != 0) {
            readItem(guid, {255, slot}, m_slots[slot]);
        }
    }
    for (u32 i = 0; i < m_bags.size(); ++i) {
        Bag& bag = m_bags[i];
        const ItemInstance& held = m_slots[kBagSlots[i]];
        if (held.empty()) {
            continue;
        }
        bag.item = held;
        const Entity* container = m_entities.find(held.guid);
        if (!container || container->type != ObjectType::Container) {
            continue;
        }
        for (u32 s = 0; s < held.containerSlots; ++s) {
            const u64 guid = guidValue(*container, static_cast<u16>(uf::CONTAINER_FIELD_SLOT_1 + s * 2));
            if (guid != 0) {
                readItem(guid, {kBagSlots[i], static_cast<u8>(s)}, bag.slots[s]);
            }
        }
    }
    m_money = p->value(uf::PLAYER_FIELD_COINAGE);
    m_bankBagSlots = p->byteValue(uf::PLAYER_BYTES_2, 2); // PLAYER_BYTES_2_OFFSET_BANK_BAG_SLOTS
    m_ammo = p->value(uf::PLAYER_AMMO_ID);
    computeAlerts();

    // A hash of what the windows show: views rebuild only when it changes.
    u64 hash = 1469598103934665603ull;
    const auto hashItem = [&](const ItemInstance& item) {
        if (item.empty()) {
            return;
        }
        mix(hash, item.guid);
        mix(hash, (static_cast<u64>(item.entry) << 32) | item.count);
        mix(hash, (static_cast<u64>(item.durability) << 32) | item.maxDurability);
        mix(hash, (static_cast<u64>(item.flags) << 32) | item.containerSlots);
        mix(hash, (static_cast<u64>(item.enchantments[0]) << 32) | item.enchantments[1]);
        mix(hash, (static_cast<u64>(item.pos.bag) << 8) | item.pos.slot);
    };
    for (const ItemInstance& item : m_slots) {
        hashItem(item);
    }
    for (const Bag& bag : m_bags) {
        for (u32 s = 0; s < bag.item.containerSlots; ++s) {
            hashItem(bag.slots[s]);
        }
    }
    mix(hash, m_money);
    mix(hash, (static_cast<u64>(m_bankBagSlots) << 32) | m_ammo);
    for (u32 i = 0; i < 12; ++i) {
        mix(hash, p->value(static_cast<u16>(uf::PLAYER_FIELD_BUYBACK_PRICE_1 + i)));
    }
    hash |= 1; // never 0 (0 = nothing known yet)
    if (hash != m_hash) {
        m_hash = hash;
        ++m_version;
        // Names, icons and tooltips of everything shown: asked for once.
        for (const ItemInstance& item : m_slots) {
            if (item.entry != 0) {
                m_queries.item(item.entry);
            }
        }
        for (const Bag& bag : m_bags) {
            for (u32 s = 0; s < bag.item.containerSlots; ++s) {
                if (bag.slots[s].entry != 0) {
                    m_queries.item(bag.slots[s].entry);
                }
            }
        }
    }
    if (!baseline && m_bankBagSlots > bankSlotsBefore) {
        event(GameEventKind::BankSlotBought).value[0] = static_cast<i32>(m_bankBagSlots);
    }
}

void Inventory::computeAlerts() {
    m_alerts = DurabilityAlerts{};
    for (u32 i = 0; i < DurabilityAlerts::kParts; ++i) {
        const ItemInstance& item = m_slots[kAlertSlots[i]];
        if (item.empty() || item.maxDurability == 0) {
            continue;
        }
        if (item.durability == 0) {
            m_alerts.parts[i] = DurabilityAlert::Broken;
        } else if (item.durability * 100 <= item.maxDurability * kLowDurabilityPercent) {
            m_alerts.parts[i] = DurabilityAlert::Low;
        }
    }
    const ItemInstance& offhand = m_slots[kSlotOffHand];
    if (!offhand.empty()) {
        const net::ItemQueryInfo* info = m_queries.findItem(offhand.entry);
        m_alerts.offhandWeapon = info && info->itemClass == kItemClassWeapon;
    }
}

Inventory::Bag* Inventory::bagAt(u8 bagSlot) {
    for (u32 i = 0; i < m_bags.size(); ++i) {
        if (kBagSlots[i] == bagSlot) {
            return &m_bags[i];
        }
    }
    return nullptr;
}

const Inventory::Bag* Inventory::bagAt(u8 bagSlot) const {
    return const_cast<Inventory*>(this)->bagAt(bagSlot);
}

const ItemInstance* Inventory::at(ItemPos pos) const {
    const ItemInstance* item = nullptr;
    if (pos.bag == 255) {
        item = pos.slot < kPlayerSlotCount ? &m_slots[pos.slot] : nullptr;
    } else if (const Bag* bag = bagAt(pos.bag); bag && pos.slot < bag->item.containerSlots) {
        item = &bag->slots[pos.slot];
    }
    return item && !item->empty() ? item : nullptr;
}

const ItemInstance* Inventory::find(u64 guid) const {
    if (guid == 0) {
        return nullptr;
    }
    for (const ItemInstance& item : m_slots) {
        if (item.guid == guid) {
            return &item;
        }
    }
    for (const Bag& bag : m_bags) {
        for (u32 s = 0; s < bag.item.containerSlots; ++s) {
            if (bag.slots[s].guid == guid) {
                return &bag.slots[s];
            }
        }
    }
    return nullptr;
}

u32 Inventory::containerSize(i32 container) const {
    if (container == kContainerBackpack) {
        return kBackpackSize;
    }
    if (container == kContainerBank) {
        return kBankSize;
    }
    if (container == kContainerKeyring) {
        return kKeyringSlotEnd - kKeyringSlotStart;
    }
    const ItemInstance* bag = bagOf(container);
    return bag ? bag->containerSlots : 0;
}

const ItemInstance* Inventory::bagOf(i32 container) const {
    u8 slot = 0;
    if (container >= 1 && container <= kContainerLastBag) {
        slot = static_cast<u8>(18 + container);
    } else if (container >= kContainerFirstBankBag && container <= kContainerLastBankBag) {
        slot = static_cast<u8>(kBankBagSlotStart + container - kContainerFirstBankBag);
    } else {
        return nullptr;
    }
    return at({255, slot});
}

ItemPos Inventory::containerPos(i32 container, u32 index) const {
    switch (container) {
    case kContainerBackpack: return {255, static_cast<u8>(23 + index)};
    case kContainerBank: return {255, static_cast<u8>(kBankItemSlotStart + index)};
    case kContainerKeyring: return {255, static_cast<u8>(kKeyringSlotStart + index)};
    default: break;
    }
    if (container >= 1 && container <= kContainerLastBag) {
        return {static_cast<u8>(18 + container), static_cast<u8>(index)};
    }
    return {static_cast<u8>(kBankBagSlotStart + container - kContainerFirstBankBag), static_cast<u8>(index)};
}

const ItemInstance* Inventory::containerItem(i32 container, u32 index) const {
    return index < containerSize(container) ? at(containerPos(container, index)) : nullptr;
}

i32 Inventory::containerOf(ItemPos pos) const {
    if (pos.bag == 255) {
        if (pos.slot >= 23 && pos.slot < 39) {
            return kContainerBackpack;
        }
        if (pos.slot >= kBankItemSlotStart && pos.slot < kBankItemSlotEnd) {
            return kContainerBank;
        }
        if (pos.slot >= kKeyringSlotStart && pos.slot < kKeyringSlotEnd) {
            return kContainerKeyring;
        }
        return 99;
    }
    if (pos.bag >= 19 && pos.bag < 23) {
        return pos.bag - 18;
    }
    if (pos.bag >= kBankBagSlotStart && pos.bag < kBankBagSlotEnd) {
        return kContainerFirstBankBag + (pos.bag - kBankBagSlotStart);
    }
    return 99;
}

u32 Inventory::containerFamily(i32 container) const {
    const ItemInstance* bag = bagOf(container);
    if (!bag) {
        return kBagFamilyNone;
    }
    const net::ItemQueryInfo* info = m_queries.item(bag->entry);
    return info ? info->bagFamily : kBagFamilyNone;
}

u32 Inventory::freeSlots(bool generalOnly) const {
    u32 free = 0;
    for (i32 c = kContainerBackpack; c <= kContainerLastBag; ++c) {
        if (generalOnly && containerFamily(c) != kBagFamilyNone) {
            continue;
        }
        const u32 size = containerSize(c);
        for (u32 i = 0; i < size; ++i) {
            free += containerItem(c, i) ? 0 : 1;
        }
    }
    return free;
}

u32 Inventory::totalSlots() const {
    u32 total = 0;
    for (i32 c = kContainerBackpack; c <= kContainerLastBag; ++c) {
        total += containerSize(c);
    }
    return total;
}

u32 Inventory::count(u32 entry, bool withBank) const {
    u32 total = 0;
    const auto add = [&](i32 container) {
        const u32 size = containerSize(container);
        for (u32 i = 0; i < size; ++i) {
            const ItemInstance* item = containerItem(container, i);
            total += item && item->entry == entry ? item->count : 0;
        }
    };
    for (i32 c = kContainerBackpack; c <= kContainerLastBag; ++c) {
        add(c);
    }
    add(kContainerKeyring);
    if (withBank) {
        add(kContainerBank);
        for (i32 c = kContainerFirstBankBag; c <= kContainerLastBankBag; ++c) {
            add(c);
        }
    }
    return total;
}

bool Inventory::findEmptySlot(u32 entry, bool bank, ItemPos& out) const {
    const net::ItemQueryInfo* item = m_queries.findItem(entry);
    const auto search = [&](i32 container) {
        if (const ItemInstance* bag = bagOf(container)) {
            const net::ItemQueryInfo* bagInfo = m_queries.findItem(bag->entry);
            if (!bagInfo || (item && !fitsBag(*item, *bagInfo)) || (!item && bagInfo->bagFamily != kBagFamilyNone)) {
                return false;
            }
        }
        const u32 size = containerSize(container);
        for (u32 i = 0; i < size; ++i) {
            if (!containerItem(container, i)) {
                out = containerPos(container, i);
                return true;
            }
        }
        return false;
    };
    if (bank) {
        if (search(kContainerBank)) {
            return true;
        }
        for (i32 c = kContainerFirstBankBag; c <= kContainerLastBankBag; ++c) {
            if (search(c)) {
                return true;
            }
        }
        return false;
    }
    // Special bags first (as the server stores): an arrow goes to the quiver.
    for (i32 pass = 0; pass < 2; ++pass) {
        for (i32 c = kContainerBackpack; c <= kContainerLastBag; ++c) {
            const bool special = containerFamily(c) != kBagFamilyNone;
            if ((pass == 0) == special && search(c)) {
                return true;
            }
        }
    }
    return false;
}

u32 Inventory::nextBankSlotPrice() const {
    return m_data && m_bankBagSlots < 6 ? m_data->bankSlotPrice(m_bankBagSlots + 1) : 0;
}

u32 Inventory::buybackPrice(u32 index) const {
    const Entity* p = me();
    if (!p || index >= 12 || m_slots[kBuybackSlotStart + index].empty()) {
        return 0;
    }
    return p->value(static_cast<u16>(uf::PLAYER_FIELD_BUYBACK_PRICE_1 + index));
}

u32 Inventory::repairCost(const ItemInstance& item) const {
    if (!m_data || item.maxDurability == 0 || item.durability >= item.maxDurability) {
        return 0;
    }
    const net::ItemQueryInfo* info = m_queries.findItem(item.entry);
    if (!info) {
        return 0;
    }
    return m_data->repairCost(info->itemClass, info->subclass, info->quality, info->itemLevel,
                              item.maxDurability - item.durability);
}

u32 Inventory::repairAllCost() const {
    // Player::DurabilityRepairAll: equipment, worn bags and backpack, then the bags' contents (not the bank).
    u32 total = 0;
    for (u8 slot = 0; slot < 39; ++slot) {
        total += m_slots[slot].empty() ? 0 : repairCost(m_slots[slot]);
    }
    for (u32 i = 0; i < 4; ++i) {
        for (u32 s = 0; s < m_bags[i].item.containerSlots; ++s) {
            total += m_bags[i].slots[s].empty() ? 0 : repairCost(m_bags[i].slots[s]);
        }
    }
    return total;
}

bool Inventory::proficient(u32 itemClass, u32 subclass) const {
    if (itemClass != kItemClassWeapon && itemClass != kItemClassArmor) {
        return true;
    }
    if (!m_proficiencyKnown || subclass >= 32) {
        return true;
    }
    if (itemClass == kItemClassArmor && subclass == kArmorMisc) {
        return true; // rings, necklaces, trinkets...
    }
    return (m_proficiency[itemClass] & (1u << subclass)) != 0;
}

bool Inventory::usable(const net::ItemQueryInfo& item) const {
    const Entity* me = m_entities.find(m_player);
    if (!me) {
        return true;
    }
    const u32 race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0), cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
    if ((cls > 0 && cls <= 32 && item.allowableClass != 0 && (item.allowableClass & (1u << (cls - 1))) == 0) ||
        (race > 0 && race <= 32 && item.allowableRace != 0 && (item.allowableRace & (1u << (race - 1))) == 0) ||
        item.requiredLevel > me->value(uf::UNIT_FIELD_LEVEL)) {
        return false;
    }
    return proficient(item.itemClass, item.subclass);
}

u32 Inventory::itemCooldownLeft(u32 spellId, u32 nowMs) const {
    for (const Cooldown& c : m_cooldowns) {
        if (c.spellId == spellId && nowMs - c.startMs < kItemCooldownMs) {
            return kItemCooldownMs - (nowMs - c.startMs);
        }
    }
    return 0;
}

bool Inventory::isEquipped(u32 entry) const {
    if (entry == 0) {
        return false;
    }
    for (u32 slot = 0; slot < kEquipmentSlotEnd; ++slot) {
        if (m_slots[slot].entry == entry) {
            return true;
        }
    }
    return false;
}

bool Inventory::itemCastCooldown(const net::SpellGo& m, ItemSpellCooldown& out) const {
    if (m.unitCaster == 0 || m.unitCaster != m_player || m.caster == m.unitCaster) {
        return false;
    }
    // SMSG_SPELL_GO comes before the server takes the item (Spell::cast: SendSpellGo, then TakeCastItem): its
    // object is still known, even for the last potion of a stack.
    const Entity* item = m_entities.find(m.caster);
    const net::ItemQueryInfo* info = item ? m_queries.findItem(item->value(uf::OBJECT_FIELD_ENTRY)) : nullptr;
    if (!info) {
        return false;
    }
    for (const net::ItemQueryInfo::Spell& spell : info->spells) {
        if (spell.id == m.spellId) {
            out.category = spell.category;
            out.recoveryMs = spell.cooldownMs;
            out.categoryRecoveryMs = spell.categoryCooldownMs;
            return true;
        }
    }
    return false;
}

// -------------------------------------------------------------------------------------------------------
// Packets.

bool Inventory::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::SMSG_LIST_INVENTORY:
    case Opcode::SMSG_BUY_ITEM:
    case Opcode::SMSG_BUY_FAILED:
    case Opcode::SMSG_SELL_ITEM:
    case Opcode::SMSG_SHOW_BANK:
    case Opcode::SMSG_BUY_BANK_SLOT_RESULT:
    case Opcode::SMSG_READ_ITEM_OK:
    case Opcode::SMSG_READ_ITEM_FAILED:
    case Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE:
    case Opcode::SMSG_ITEM_TEXT_QUERY_RESPONSE:
    case Opcode::SMSG_DURABILITY_DAMAGE_DEATH:
    case Opcode::SMSG_ITEM_COOLDOWN:
    case Opcode::SMSG_SET_PROFICIENCY:
    case Opcode::SMSG_OPEN_CONTAINER:
    case Opcode::SMSG_ITEM_ENCHANT_TIME_UPDATE:
    case Opcode::SMSG_ITEM_TIME_UPDATE: return true; // phase 13
    default: return false;
    }
}

bool Inventory::handle(Opcode opcode, std::span<const u8> p, u32 nowMs) {
    m_nowMs = nowMs;
    switch (opcode) {
    case Opcode::SMSG_LIST_INVENTORY: {
        if (!net::parseListInventory(p, m_vendor.list)) {
            m_vendor.list.count = 0;
            return false;
        }
        m_vendor.npc = m_vendor.list.vendor;
        const Entity* npc = m_entities.find(m_vendor.npc);
        m_vendor.canRepair = npc && (npcFlags(*npc) & kNpcFlagRepair) != 0;
        for (const net::VendorInventory::Item& item : m_vendor.items()) {
            m_queries.item(item.itemId);
        }
        m_banker = 0;
        event(GameEventKind::VendorOpened, 0, m_vendor.npc);
        return true;
    }
    case Opcode::SMSG_BUY_ITEM: {
        if (!net::parseBuyItem(p, m_buy)) {
            return false;
        }
        u32 itemId = 0, received = m_buy.count;
        if (m_vendor.npc == m_buy.vendor) {
            for (u32 i = 0; i < m_vendor.list.count; ++i) {
                net::VendorInventory::Item& row = m_vendor.list.items[i];
                if (row.index == m_buy.vendorSlot) {
                    row.stock = m_buy.newStock;
                    itemId = row.itemId;
                    received = m_buy.count * std::max<u32>(row.buyCount, 1);
                }
            }
        }
        event(GameEventKind::ItemBought, itemId, m_buy.vendor).value[0] = static_cast<i32>(received);
        return true;
    }
    case Opcode::SMSG_BUY_FAILED: {
        net::BuyFailed m;
        if (!net::parseBuyFailed(p, m)) {
            return false;
        }
        event(GameEventKind::BuyFailed, m.itemId, m.vendor).value[0] = m.reason;
        return true;
    }
    case Opcode::SMSG_SELL_ITEM: {
        net::SellItemResult m;
        if (!net::parseSellItem(p, m)) {
            return false;
        }
        GameEvent& e = event(GameEventKind::SellFailed, 0, m.vendor);
        e.other = m.item;
        e.value[0] = m.reason;
        return true;
    }
    case Opcode::SMSG_SHOW_BANK: {
        u64 banker = 0;
        if (!net::parseGuid(p, banker)) {
            return false;
        }
        m_banker = banker;
        m_vendor.npc = 0;
        event(GameEventKind::BankOpened, 0, banker);
        return true;
    }
    case Opcode::SMSG_BUY_BANK_SLOT_RESULT: {
        u32 result = 0;
        if (!net::parseU32(p, result)) {
            return false;
        }
        if (result != net::kBankSlotOk) {
            event(GameEventKind::BankSlotFailed).value[0] = static_cast<i32>(result);
        }
        return true;
    }
    case Opcode::SMSG_READ_ITEM_OK: {
        u64 guid = 0;
        if (!net::parseReadItemOk(p, guid)) {
            return false;
        }
        refresh(nowMs, true);
        if (const ItemInstance* item = find(guid)) {
            openReading(*item);
        }
        return true;
    }
    case Opcode::SMSG_READ_ITEM_FAILED: {
        u64 guid = 0;
        u8 reason = 0;
        if (!net::parseReadItemFailed(p, guid, reason)) {
            return false;
        }
        GameEvent& e = event(GameEventKind::ReadItemFailed);
        e.other = guid;
        e.value[0] = reason;
        return true;
    }
    case Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE: {
        net::PageText page;
        if (!net::parsePageTextResponse(p, page)) {
            return false;
        }
        Page& stored = m_pages[page.pageId];
        stored.text.assign(page.text);
        stored.next = page.nextPageId;
        showPages();
        return true;
    }
    case Opcode::SMSG_ITEM_TEXT_QUERY_RESPONSE: {
        u32 textId = 0;
        std::string_view text;
        if (!net::parseItemTextResponse(p, textId, text)) {
            return false;
        }
        m_letters[textId].assign(text);
        showPages();
        return true;
    }
    case Opcode::SMSG_DURABILITY_DAMAGE_DEATH: event(GameEventKind::DurabilityLoss); return true;
    case Opcode::SMSG_ITEM_COOLDOWN: {
        u64 guid = 0;
        u32 spellId = 0;
        if (!net::parseItemCooldown(p, guid, spellId)) {
            return false;
        }
        Cooldown* slot = &m_cooldowns[0];
        for (Cooldown& c : m_cooldowns) {
            if (c.spellId == spellId || nowMs - c.startMs >= kItemCooldownMs) {
                slot = &c;
                break;
            }
            slot = c.startMs < slot->startMs ? &c : slot;
        }
        *slot = {spellId, nowMs};
        return true;
    }
    case Opcode::SMSG_SET_PROFICIENCY: {
        u8 itemClass = 0;
        u32 mask = 0;
        if (!net::parseSetProficiency(p, itemClass, mask)) {
            return false;
        }
        if (itemClass < m_proficiency.size()) {
            m_proficiency[itemClass] = mask;
            m_proficiencyKnown = true;
        }
        return true;
    }
    case Opcode::SMSG_OPEN_CONTAINER: {
        u64 guid = 0;
        if (!net::parseGuid(p, guid)) {
            return false;
        }
        event(GameEventKind::ContainerOpened, 0, guid); // phase 15 (items): a bag just equipped opens (BAG_OPEN)
        return true;
    }
    case Opcode::SMSG_ITEM_ENCHANT_TIME_UPDATE: {
        net::ItemEnchantTimeUpdate m;
        return net::parseItemEnchantTimeUpdate(p, m); // the item's fields carry the duration too
    }
    case Opcode::SMSG_ITEM_TIME_UPDATE: { // phase 13: what an expiring item has left, for its tooltip
        net::ItemTimeUpdate m;
        if (!net::parseItemTimeUpdate(p, m)) {
            return false;
        }
        m_itemTimes[m.item] = ItemTime{m.seconds, nowMs};
        m_dirty = true;
        return true;
    }
    default: return false;
    }
}

GameEvent& Inventory::event(GameEventKind kind, u32 id, u64 guid) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = kind;
    e.timeMs = m_nowMs;
    e.id = id;
    e.guid = guid;
    return e;
}

// -------------------------------------------------------------------------------------------------------
// Actions.

bool Inventory::send(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

bool Inventory::swap(ItemPos from, ItemPos to) {
    if (from == to || !at(from)) {
        return false;
    }
    m_body.clear();
    if (from.inPlayerRow() && to.inPlayerRow()) {
        net::writeSwapInvItem(m_body, from.slot, to.slot);
        return send(Opcode::CMSG_SWAP_INV_ITEM);
    }
    net::writeSwapItem(m_body, to.bag, to.slot, from.bag, from.slot);
    return send(Opcode::CMSG_SWAP_ITEM);
}

bool Inventory::equip(ItemPos from) {
    if (!at(from)) {
        return false;
    }
    m_body.clear();
    net::writeBagSlot(m_body, from.bag, from.slot);
    return send(Opcode::CMSG_AUTOEQUIP_ITEM);
}

bool Inventory::equipTo(ItemPos from, u8 equipSlot) {
    const ItemInstance* item = at(from);
    if (!item || equipSlot >= kEquipmentSlotEnd) {
        return false;
    }
    m_body.clear();
    net::writeAutoEquipItemSlot(m_body, item->guid, equipSlot);
    return send(Opcode::CMSG_AUTOEQUIP_ITEM_SLOT);
}

bool Inventory::store(ItemPos from, u8 dstBag) {
    if (!at(from)) {
        return false;
    }
    m_body.clear();
    net::writeAutoStoreBagItem(m_body, from.bag, from.slot, dstBag);
    return send(Opcode::CMSG_AUTOSTORE_BAG_ITEM);
}

bool Inventory::split(ItemPos from, u32 count) {
    const ItemInstance* item = at(from);
    ItemPos to;
    if (!item || count == 0 || count >= item->count || count > 255 || !findEmptySlot(item->entry, from.bank(), to)) {
        return false;
    }
    m_body.clear();
    net::writeSplitItem(m_body, from.bag, from.slot, to.bag, to.slot, static_cast<u8>(count));
    return send(Opcode::CMSG_SPLIT_ITEM);
}

bool Inventory::destroy(ItemPos from, u32 count) {
    const ItemInstance* item = at(from);
    if (!item) {
        return false;
    }
    m_body.clear();
    net::writeDestroyItem(m_body, from.bag, from.slot, count >= item->count ? 0 : static_cast<u8>(std::min(count, 255u)));
    return send(Opcode::CMSG_DESTROYITEM);
}

bool Inventory::open(ItemPos from) {
    if (!at(from)) {
        return false;
    }
    m_body.clear();
    net::writeBagSlot(m_body, from.bag, from.slot);
    return send(Opcode::CMSG_OPEN_ITEM);
}

bool Inventory::read(ItemPos from) {
    if (!at(from)) {
        return false;
    }
    m_body.clear();
    net::writeBagSlot(m_body, from.bag, from.slot);
    return send(Opcode::CMSG_READ_ITEM);
}

bool Inventory::setAmmo(u32 entry) {
    m_body.clear();
    net::writeU32(m_body, entry);
    return send(Opcode::CMSG_SET_AMMO);
}

// -------------------------------------------------------------------------------------------------------
// Vendor and bank.

void Inventory::requestVendor(u64 npc) {
    m_body.clear();
    net::writeGuid(m_body, npc);
    send(Opcode::CMSG_LIST_INVENTORY);
}

bool Inventory::buy(u32 row, u32 count) {
    if (!m_vendor.open() || row >= m_vendor.list.count || count == 0) {
        return false;
    }
    m_body.clear();
    net::writeBuyItem(m_body, m_vendor.npc, m_vendor.list.items[row].itemId, static_cast<u8>(std::min(count, 255u)));
    return send(Opcode::CMSG_BUY_ITEM);
}

bool Inventory::buyInto(u32 row, ItemPos dst, u32 count) {
    if (!m_vendor.open() || row >= m_vendor.list.count || count == 0 || at(dst)) {
        return false;
    }
    // The bag is named by GUID: the player's for its own slots (backpack, equipment), else the worn bag's.
    u64 bag = 0;
    if (dst.bag == 255 && (dst.slot < kEquipmentSlotEnd || (dst.slot >= 23 && dst.slot < 39))) {
        bag = m_player;
    } else if (dst.bag >= 19 && dst.bag < 23) {
        const ItemInstance* worn = at({255, dst.bag});
        bag = worn ? worn->guid : 0;
    }
    if (bag == 0) {
        return false; // the bank and the rest are not for buying into
    }
    m_body.clear();
    net::writeBuyItemInSlot(m_body, m_vendor.npc, m_vendor.list.items[row].itemId, bag, dst.slot,
                            static_cast<u8>(std::min(count, 255u)));
    return send(Opcode::CMSG_BUY_ITEM_IN_SLOT);
}

bool Inventory::sell(ItemPos from, u32 count) {
    const ItemInstance* item = at(from);
    if (!m_vendor.open() || !item) {
        return false;
    }
    m_body.clear();
    net::writeSellItem(m_body, m_vendor.npc, item->guid, count >= item->count ? 0 : static_cast<u8>(count));
    return send(Opcode::CMSG_SELL_ITEM);
}

u32 Inventory::junkValue(u32* stacks) const {
    u32 value = 0, count = 0;
    for (i32 c = kContainerBackpack; c <= kContainerLastBag; ++c) {
        const u32 size = containerSize(c);
        for (u32 i = 0; i < size; ++i) {
            const ItemInstance* item = containerItem(c, i);
            const net::ItemQueryInfo* info = item ? m_queries.findItem(item->entry) : nullptr;
            if (info && info->quality == 0 && info->sellPrice > 0) {
                value += info->sellPrice * item->count;
                ++count;
            }
        }
    }
    if (stacks) {
        *stacks = count;
    }
    return value;
}

u32 Inventory::sellJunk() {
    if (!m_vendor.open()) {
        return 0;
    }
    u32 sold = 0;
    for (i32 c = kContainerBackpack; c <= kContainerLastBag; ++c) {
        const u32 size = containerSize(c);
        for (u32 i = 0; i < size; ++i) {
            const ItemInstance* item = containerItem(c, i);
            const net::ItemQueryInfo* info = item ? m_queries.findItem(item->entry) : nullptr;
            if (info && info->quality == 0 && info->sellPrice > 0 && sell(item->pos)) {
                ++sold;
            }
        }
    }
    return sold;
}

bool Inventory::buyback(u32 index) {
    if (!m_vendor.open() || index >= 12 || m_slots[kBuybackSlotStart + index].empty()) {
        return false;
    }
    m_body.clear();
    net::writeBuybackItem(m_body, m_vendor.npc, kBuybackSlotStart + index);
    return send(Opcode::CMSG_BUYBACK_ITEM);
}

bool Inventory::repair(ItemPos pos) {
    const ItemInstance* item = at(pos);
    if (!m_vendor.open() || !m_vendor.canRepair || !item) {
        return false;
    }
    m_body.clear();
    net::writeRepairItem(m_body, m_vendor.npc, item->guid);
    return send(Opcode::CMSG_REPAIR_ITEM);
}

bool Inventory::repairAll() {
    if (!m_vendor.open() || !m_vendor.canRepair) {
        return false;
    }
    m_body.clear();
    net::writeRepairItem(m_body, m_vendor.npc, 0);
    return send(Opcode::CMSG_REPAIR_ITEM);
}

void Inventory::closeVendor() {
    m_vendor.npc = 0; // the server keeps nothing open
}

void Inventory::requestBank(u64 npc) {
    m_body.clear();
    net::writeGuid(m_body, npc);
    send(Opcode::CMSG_BANKER_ACTIVATE);
}

bool Inventory::toBank(ItemPos from) {
    if (m_banker == 0 || !at(from)) {
        return false;
    }
    m_body.clear();
    net::writeBagSlot(m_body, from.bag, from.slot);
    return send(Opcode::CMSG_AUTOBANK_ITEM);
}

bool Inventory::fromBank(ItemPos from) {
    if (m_banker == 0 || !at(from)) {
        return false;
    }
    m_body.clear();
    net::writeBagSlot(m_body, from.bag, from.slot);
    return send(Opcode::CMSG_AUTOSTORE_BANK_ITEM);
}

bool Inventory::buyBankSlot() {
    if (m_banker == 0 || m_bankBagSlots >= 6) {
        return false;
    }
    m_body.clear();
    net::writeGuid(m_body, m_banker);
    return send(Opcode::CMSG_BUY_BANK_SLOT);
}

// -------------------------------------------------------------------------------------------------------
// Reading.

void Inventory::openReading(const ItemInstance& item) {
    m_reading = ReadingWindow{};
    m_reading.item = item.guid;
    m_reading.entry = item.entry;
    const net::ItemQueryInfo* info = m_queries.findItem(item.entry);
    m_reading.material = info ? info->pageMaterial : 0;
    if (item.itemTextId != 0) {
        if (!m_letters.contains(item.itemTextId)) {
            m_body.clear();
            net::writeItemTextQuery(m_body, item.itemTextId);
            send(Opcode::CMSG_ITEM_TEXT_QUERY);
        }
    } else if (info && info->pageText != 0 && !m_pages.contains(info->pageText)) {
        m_body.clear();
        net::writePageTextQuery(m_body, info->pageText, item.guid);
        send(Opcode::CMSG_PAGE_TEXT_QUERY); // the server answers the whole chain
    }
    showPages();
    event(GameEventKind::ReadingOpened).other = item.guid;
}

void Inventory::showPages() {
    if (!m_reading.open()) {
        return;
    }
    m_reading.pageCount = 0;
    m_reading.waiting = false;
    const ItemInstance* item = find(m_reading.item);
    const u32 textId = item ? item->itemTextId : 0;
    if (textId != 0) {
        const auto letter = m_letters.find(textId);
        if (letter == m_letters.end()) {
            m_reading.waiting = true;
        } else {
            m_reading.pages[m_reading.pageCount++] = letter->second;
        }
    } else {
        const net::ItemQueryInfo* info = m_reading.firstPage == 0 ? m_queries.findItem(m_reading.entry) : nullptr;
        u32 page = m_reading.firstPage != 0 ? m_reading.firstPage : info ? info->pageText : 0; // phase 13: objects
        while (page != 0 && m_reading.pageCount < m_reading.pages.size()) {
            const auto found = m_pages.find(page);
            if (found == m_pages.end()) {
                m_reading.waiting = true;
                break;
            }
            m_reading.pages[m_reading.pageCount++] = found->second.text;
            page = found->second.next;
        }
    }
    if (m_reading.pageCount > 0) {
        m_reading.page = std::min(m_reading.page, m_reading.pageCount - 1);
    }
}

void Inventory::turnPage(i32 delta) {
    if (m_reading.pageCount == 0) {
        return;
    }
    const i32 page = static_cast<i32>(m_reading.page) + delta;
    m_reading.page = static_cast<u32>(std::clamp(page, 0, static_cast<i32>(m_reading.pageCount) - 1));
}

void Inventory::closeReading() {
    m_reading = ReadingWindow{};
}

void Inventory::openPages(u64 object, u32 firstPage, u32 material, std::string_view title) {
    m_reading = ReadingWindow{};
    m_reading.item = object;
    m_reading.firstPage = firstPage;
    m_reading.material = material;
    m_reading.title = title;
    if (!m_pages.contains(firstPage)) {
        m_body.clear();
        net::writePageTextQuery(m_body, firstPage, object);
        send(Opcode::CMSG_PAGE_TEXT_QUERY); // the server answers the whole chain
    }
    showPages();
    event(GameEventKind::ReadingOpened).other = object;
}

std::string_view Inventory::pageText(u32 pageId) const {
    const auto found = m_pages.find(pageId);
    return found == m_pages.end() ? std::string_view{} : std::string_view(found->second.text);
}

std::string_view Inventory::letterText(u32 textId) const {
    const auto found = m_letters.find(textId);
    return found == m_letters.end() ? std::string_view{} : std::string_view(found->second);
}

bool Inventory::requestLetter(u32 textId) {
    if (textId == 0 || m_letters.contains(textId)) {
        return false;
    }
    m_body.clear();
    net::writeItemTextQuery(m_body, textId);
    return send(Opcode::CMSG_ITEM_TEXT_QUERY);
}

} // namespace vd::game
