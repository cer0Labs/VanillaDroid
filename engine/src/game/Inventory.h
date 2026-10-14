#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "game/ItemDefs.h"
#include "game/Queries.h"
#include "net/ItemMessages.h"
#include "net/Opcodes.gen.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vd::net {
struct SpellGo;
}

namespace vd::game {

class ItemData;
struct ItemSpellCooldown;

// Where an item is, as the item packets name it: bag 255 (the player's own row of slots: equipment, worn bags,
// backpack, bank, bank bags, buyback, keyring) and a slot in it, or a worn / bank bag's slot (19..22, 63..68) and the
// index inside that bag.
struct ItemPos {
    u8 bag = 255;
    u8 slot = 0;
    bool operator==(const ItemPos&) const = default;
    bool inPlayerRow() const { return bag == 255; }
    bool equipment() const { return bag == 255 && slot < kEquipmentSlotEnd; }
    bool wornBagSlot() const { return bag == 255 && slot >= 19 && slot < 23; }
    bool bankBagSlot() const { return bag == 255 && slot >= kBankBagSlotStart && slot < kBankBagSlotEnd; }
    // In the bank: its 24 slots, the bank bag slots or a bank bag's contents.
    bool bank() const {
        return (bag == 255 && slot >= kBankItemSlotStart && slot < kBankBagSlotEnd) ||
               (bag >= kBankBagSlotStart && bag < kBankBagSlotEnd);
    }
    bool buyback() const { return bag == 255 && slot >= kBuybackSlotStart && slot < kBuybackSlotEnd; }
};

// An item the player owns, from its object's update fields.
struct ItemInstance {
    u64 guid = 0; // 0: an empty slot
    u32 entry = 0; // 0 while its object has not arrived yet
    u32 count = 1;
    u32 durability = 0;
    u32 maxDurability = 0; // 0: the item does not wear
    u32 flags = 0;         // ITEM_FIELD_FLAGS (kItemDynFlag*)
    std::array<u32, kEnchantmentSlots> enchantments{};       // SpellItemEnchantment ids by EnchantmentSlot
    std::array<u32, kEnchantmentSlots> enchantmentDurations{}; // ms left (temporary enchantments)
    std::array<i32, 5> spellCharges{};
    u64 creator = 0; // "<Made by ...>"
    u64 giftCreator = 0;
    u32 randomPropertyId = 0;
    u32 itemTextId = 0; // a letter's text (CMSG_ITEM_TEXT_QUERY)
    u32 durationS = 0;  // seconds left of an item that expires
    u32 containerSlots = 0; // a bag object: its size
    ItemPos pos;

    bool empty() const { return guid == 0; }
    bool soulbound() const { return (flags & kItemDynFlagBound) != 0; }
    bool broken() const { return maxDurability > 0 && durability == 0; }
};

// The client's numbering of containers (ContainerFrame.lua): 0 the backpack, 1..4 the worn bags, 5..10 the bank bags,
// -1 the bank's own 24 slots, -2 the keyring.
constexpr i32 kContainerBank = -1;
constexpr i32 kContainerKeyring = -2;
constexpr i32 kContainerBackpack = 0;
constexpr i32 kContainerLastBag = 4;
constexpr i32 kContainerFirstBankBag = 5;
constexpr i32 kContainerLastBankBag = 10;
constexpr u32 kBackpackSize = 16;
constexpr u32 kBankSize = 24;

// The character window's numbers (UNIT_FIELD_* and PLAYER_FIELD_* of the player).
struct CharacterStats {
    u32 level = 0;
    std::array<i32, 5> stat{};         // strength, agility, stamina, intellect, spirit (with everything)
    std::array<i32, 5> statPositive{}; // the part buffs and items add (PLAYER_FIELD_POSSTAT*)
    std::array<i32, 5> statNegative{}; // the part debuffs take (PLAYER_FIELD_NEGSTAT*, <= 0)
    std::array<i32, 7> resistance{};   // armor, holy, fire, nature, frost, shadow, arcane
    std::array<i32, 7> resistancePositive{};
    std::array<i32, 7> resistanceNegative{};
    i32 attackPower = 0; // base; the total is base + positive + negative, times (1 + multiplier)
    i32 attackPowerPositive = 0, attackPowerNegative = 0;
    f32 attackPowerMultiplier = 0.0f;
    i32 rangedAttackPower = 0;
    i32 rangedAttackPowerPositive = 0, rangedAttackPowerNegative = 0;
    f32 rangedAttackPowerMultiplier = 0.0f;
    f32 minDamage = 0.0f, maxDamage = 0.0f;
    f32 minOffhandDamage = 0.0f, maxOffhandDamage = 0.0f;
    f32 minRangedDamage = 0.0f, maxRangedDamage = 0.0f;
    u32 mainHandSpeedMs = 0, offHandSpeedMs = 0, rangedSpeedMs = 0;
    f32 critChance = 0.0f, rangedCritChance = 0.0f, dodgeChance = 0.0f, parryChance = 0.0f, blockChance = 0.0f;

    i32 totalAttackPower() const;
    i32 totalRangedAttackPower() const;
};
CharacterStats characterStats(const Entity& player);

// The DurabilityFrame's figure: one part per alert slot (head, shoulders, chest, waist, legs, feet, wrists, hands,
// weapon, shield / off hand, ranged).
enum class DurabilityAlert : u8 { None, Low, Broken };
struct DurabilityAlerts {
    static constexpr u32 kParts = 11;
    std::array<DurabilityAlert, kParts> parts{};
    bool offhandWeapon = false; // the off-hand part is a weapon, not a shield
    bool any() const;
};

// The open vendor (SMSG_LIST_INVENTORY).
struct VendorWindow {
    u64 npc = 0; // 0: closed
    bool canRepair = false; // UNIT_NPC_FLAG_REPAIR
    net::VendorInventory list;
    bool open() const { return npc != 0; }
    std::span<const net::VendorInventory::Item> items() const { return list.list(); }
};

// The open book or letter: its pages as they arrive (the server sends a book's whole chain at once).
struct ReadingWindow {
    u64 item = 0; // 0: closed
    u32 entry = 0;
    u32 material = 0; // PageTextMaterial: the background
    u32 page = 0;     // the one shown
    u32 pageCount = 0;
    bool waiting = false; // a page is still on its way
    std::array<std::string_view, 32> pages{};
    // Phase 13 (quests): a game object's pages (a plaque, a book on a lectern): `item` is the object, `firstPage` its
    // template's page and `title` its name (the item's name otherwise).
    u32 firstPage = 0;
    std::string_view title;
    bool open() const { return item != 0; }
};

// Everything the player carries and wears, the vendor, the bank and the book being read (phase 8): decoded from
// the update fields of the player and its item objects, plus the item, vendor, bank and reading packets
// (Gameplay::handle delegates them here). Actions send the client packets; the server answers with update fields
// (moves, counts, money) or errors (GameEventKind::InventoryError and the events below). Main thread; the model
// lives in fixed arrays (no allocation per frame).
class Inventory {
public:
    using Events = EventRing<GameEvent, 64>;

    Inventory(const EntityStore& entities, QueryCache& queries, Events& events);

    void setSink(PacketSink sink) { m_sink = sink; }
    void setData(const ItemData* data) { m_data = data; }
    // Session (Gameplay calls these).
    void enterWorld(u64 player, bool newLogin, u32 nowMs);
    void leaveWorld();
    // The update fields changed (after each SMSG_UPDATE_OBJECT): rebuilt on the next refresh().
    void markDirty() { m_dirty = true; }
    // Rebuilds the model if anything changed (Gameplay::update calls it once per frame): at once after a quiet
    // spell, at most every kRebuildMs while updates keep coming (a city sends them every frame); `force`: now.
    void refresh(u32 nowMs, bool force = false);
    static constexpr u32 kRebuildMs = 100;
    // One of the packets listed in handles(). False when malformed.
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    static bool handles(net::Opcode opcode);

    // --- The model (as of the last refresh) ---
    // Bumped whenever an item, a count, a bag or the money changed: views may cache what they built.
    u32 version() const { return m_version; }
    u64 player() const { return m_player; }
    u32 money() const { return m_money; }
    const ItemInstance* at(ItemPos pos) const;      // nullptr: empty or not a slot
    const ItemInstance* equipped(u8 slot) const { return at({255, slot}); }
    const ItemInstance* find(u64 guid) const;
    // Containers by the client's numbers (kContainer*): size (0 without a bag there), contents, the bag item itself
    // (nullptr for the backpack, the bank and the keyring) and where its slot `index` is for the packets.
    u32 containerSize(i32 container) const;
    const ItemInstance* containerItem(i32 container, u32 index) const;
    const ItemInstance* bagOf(i32 container) const;
    ItemPos containerPos(i32 container, u32 index) const;
    // The container an item position belongs to (kContainer*), or 99 for equipment, worn bag slots, buyback...
    i32 containerOf(ItemPos pos) const;
    // The family a container holds (kBagFamily*; 0 any item), from its template (asked for when missing).
    u32 containerFamily(i32 container) const;
    // Free slots in the backpack and worn bags: all of them / those any item fits in.
    u32 freeSlots(bool generalOnly = false) const;
    u32 totalSlots() const;
    // How many of `entry` the player carries (and keeps in the bank).
    u32 count(u32 entry, bool withBank = false) const;
    // The first empty slot that can hold `entry` (by its bag family) in the bags or the bank; false if none.
    bool findEmptySlot(u32 entry, bool bank, ItemPos& out) const;
    // Bank bag slots bought (PLAYER_BYTES_2 byte 2) and what the next one costs (0: all bought or unknown).
    u32 bankBagSlots() const { return m_bankBagSlots; }
    u32 nextBankSlotPrice() const;
    u32 ammo() const { return m_ammo; } // PLAYER_AMMO_ID
    // The price a buyback slot (0..11) costs, 0 when empty.
    u32 buybackPrice(u32 index) const;
    const DurabilityAlerts& durability() const { return m_alerts; }
    // What repairing an item / everything worn and carried costs (DurabilityCosts x DurabilityQuality), 0 without
    // the tables or the templates.
    u32 repairCost(const ItemInstance& item) const;
    u32 repairAllCost() const;
    // Subclasses of a class the player may use (SMSG_SET_PROFICIENCY): weapons (2) and armor (4).
    bool proficient(u32 itemClass, u32 subclass) const;
    // The client's isUsable / canUse of an item (the windows tint what is not red): its classes and races, its
    // required level and the player's proficiencies. True while the player is unknown.
    bool usable(const net::ItemQueryInfo& item) const;
    // A cooldown of an item's on-use spell from SMSG_ITEM_COOLDOWN (30 s after equipping): ms left.
    u32 itemCooldownLeft(u32 spellId, u32 nowMs) const;
    // Parity pass (action bars). The item tables given by setData (spell texts, sets...), for views that build
    // item tooltips; whether an item of `entry` is worn (its action button gets the green border:
    // IsEquippedAction); the cooldown values of an item cast for SpellBook::onSpellGo (SMSG_SPELL_GO's caster is
    // the item, its unit caster the player: the item's query values for that spell; false for any other cast or
    // while the item or its query is unknown).
    const ItemData* data() const { return m_data; }
    bool isEquipped(u32 entry) const;
    bool itemCastCooldown(const net::SpellGo& m, ItemSpellCooldown& out) const;

    // --- Moving items ---
    bool swap(ItemPos from, ItemPos to);       // CMSG_SWAP_INV_ITEM / CMSG_SWAP_ITEM (stacks merge)
    bool equip(ItemPos from);                  // CMSG_AUTOEQUIP_ITEM (bags go to a free bag slot)
    bool equipTo(ItemPos from, u8 equipSlot);  // CMSG_AUTOEQUIP_ITEM_SLOT (a ring to the second finger...)
    bool store(ItemPos from, u8 dstBag = kNullBag); // CMSG_AUTOSTORE_BAG_ITEM: into `dstBag` (255 backpack,
                                                    // 19..22 a bag) or anywhere
    bool split(ItemPos from, u32 count);       // CMSG_SPLIT_ITEM to the first empty slot that takes it
    bool destroy(ItemPos from, u32 count = 0); // CMSG_DESTROYITEM (0: the whole stack); confirm first
    bool open(ItemPos from);                   // CMSG_OPEN_ITEM: lockboxes, clams (loot window on the answer)
    bool read(ItemPos from);                   // CMSG_READ_ITEM: books and letters (reading() opens)
    bool setAmmo(u32 entry);                   // CMSG_SET_AMMO (0 removes)

    // --- Vendor ---
    const VendorWindow& vendor() const { return m_vendor; }
    // CMSG_LIST_INVENTORY (Gameplay::interact sends it for vendors without a gossip menu).
    void requestVendor(u64 npc);
    bool buy(u32 row, u32 count = 1); // `count` purchases of vendor().items()[row]
    // CMSG_BUY_ITEM_IN_SLOT: into an empty backpack, bag or equipment slot (a weapon straight into the hand).
    bool buyInto(u32 row, ItemPos dst, u32 count = 1);
    bool sell(ItemPos from, u32 count = 0);
    // Sells every grey item that has a price; returns how many stacks went.
    u32 sellJunk();
    // The value of what sellJunk() would sell (copper) and how many stacks.
    u32 junkValue(u32* stacks = nullptr) const;
    bool buyback(u32 index); // 0..11
    bool repair(ItemPos item);
    bool repairAll();
    void closeVendor();

    // --- Bank ---
    u64 banker() const { return m_banker; } // 0: closed
    void requestBank(u64 npc);              // CMSG_BANKER_ACTIVATE
    bool toBank(ItemPos from);              // CMSG_AUTOBANK_ITEM
    bool fromBank(ItemPos from);            // CMSG_AUTOSTORE_BANK_ITEM
    bool buyBankSlot();                     // CMSG_BUY_BANK_SLOT
    void closeBank() { m_banker = 0; }

    // --- Reading ---
    const ReadingWindow& reading() const { return m_reading; }
    void turnPage(i32 delta);
    void closeReading();
    // Phase 13 (quests): the pages of a game object from `firstPage` on (CMSG_PAGE_TEXT_QUERY unless known);
    // `title` must outlive the window (the object template's name in the QueryCache).
    void openPages(u64 object, u32 firstPage, u32 material, std::string_view title);
    // Page text by id (the server's; empty until it arrives) for other windows (mail...).
    std::string_view pageText(u32 pageId) const;
    // A letter's text by its item text id (mail bodies, letters copied from the mail): empty until it arrives;
    // requestLetter() asks for it (CMSG_ITEM_TEXT_QUERY) unless it is known.
    std::string_view letterText(u32 textId) const;
    bool requestLetter(u32 textId);

private:
    struct Bag {
        ItemInstance item; // the bag object (guid 0: no bag)
        std::array<ItemInstance, kMaxBagSize> slots{};
    };
    struct Page {
        std::string text;
        u32 next = 0;
    };
    struct Cooldown {
        u32 spellId = 0;
        u32 startMs = 0;
    };

    const Entity* me() const { return m_entities.find(m_player); }
    void rebuild(u32 nowMs);
    void readItem(u64 guid, ItemPos pos, ItemInstance& out);
    Bag* bagAt(u8 bagSlot);
    const Bag* bagAt(u8 bagSlot) const;
    void computeAlerts();
    bool send(net::Opcode opcode);
    GameEvent& event(GameEventKind kind, u32 id = 0, u64 guid = 0);
    void openReading(const ItemInstance& item);
    void showPages();

    const EntityStore& m_entities;
    QueryCache& m_queries;
    Events& m_events;
    const ItemData* m_data = nullptr;
    PacketSink m_sink;
    std::vector<u8> m_body;
    u64 m_player = 0;
    bool m_inWorld = false;
    bool m_dirty = true;
    bool m_built = false;
    u32 m_builtMs = 0;
    u32 m_nowMs = 0;
    u32 m_version = 1;
    u64 m_hash = 0;

    std::array<ItemInstance, kPlayerSlotCount> m_slots{};
    std::array<Bag, 10> m_bags{}; // worn bags (19..22), then bank bags (63..68)
    u32 m_money = 0;
    u32 m_bankBagSlots = 0;
    u32 m_ammo = 0;
    DurabilityAlerts m_alerts;
    std::array<u32, 16> m_proficiency{}; // subclass mask per item class
    bool m_proficiencyKnown = false;
    std::array<Cooldown, 8> m_cooldowns{};

    VendorWindow m_vendor;
    net::BuyItemResult m_buy;
    u64 m_banker = 0;
    ReadingWindow m_reading;
    std::unordered_map<u32, Page> m_pages;          // page id -> text (session cache)
    std::unordered_map<u32, std::string> m_letters; // item text id -> text
    // Phase 13: SMSG_ITEM_TIME_UPDATE (an expiring item's seconds left, and when they were told): newer than its
    // ITEM_FIELD_DURATION, which the server only sends with the item's other changes.
    struct ItemTime {
        u32 seconds = 0;
        u32 atMs = 0;
    };
    std::unordered_map<u64, ItemTime> m_itemTimes;
};

} // namespace vd::game
