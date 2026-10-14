// Inventory (phase 8): the model decoded from SMSG_UPDATE_OBJECT (player slots, bags, bank, buyback, money,
// durability), the item actions byte for byte, the vendor (list, buy, sell, buy back, repair, junk), the bank (open,
// move, bag slots), reading books and letters, end to end through game::Gameplay with a capturing packet sink; and
// the client tables (repair costs, bank slot prices, spell texts, names) against the real db.vdpk, with the client's
// texts of ui.vdpk in English (VD_PACKS_DIR).
#include "TestData.h"
#include "core/Text.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Inventory.h"
#include "game/ItemActions.h"
#include "game/ItemData.h"
#include "game/ItemTooltip.h"
#include "game/SpellText.h"
#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/ItemMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using game::GameEventKind;
using game::ItemPos;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x11;
constexpr u64 kVendor = 0xF130000000000123ull;
constexpr u64 kBanker = 0xF130000000000124ull;
constexpr u64 kSword = 0x4000000000000901ull;
constexpr u64 kBag = 0x4000000000000902ull;
constexpr u64 kBread = 0x4000000000000903ull;
constexpr u64 kJunk = 0x4000000000000904ull;
constexpr u64 kBanked = 0x4000000000000905ull;
constexpr u64 kSold = 0x4000000000000906ull;
constexpr u64 kBook = 0x4000000000000907ull;
constexpr u64 kChest = 0x4000000000000908ull;

constexpr u32 kSwordEntry = 25; // Worn Shortsword
constexpr u32 kBagEntry = 4496; // Small Brown Pouch (6 slots)
constexpr u32 kBreadEntry = 4540;
constexpr u32 kJunkEntry = 7073; // Broken Fang
constexpr u32 kBookEntry = 2794;
constexpr u32 kChestEntry = 6125;

u32 low(u64 guid) {
    return static_cast<u32>(guid);
}
u32 high(u64 guid) {
    return static_cast<u32>(guid >> 32);
}

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    const u32 words = maxIndex / 32 + 1;
    std::vector<u32> mask(words, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(words));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
}

// SMSG_UPDATE_OBJECT with one create block (UPDATEFLAG_HAS_POSITION).
Bytes created(u64 guid, game::ObjectType type, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f);
    writeValues(w, fields);
    return out;
}

// SMSG_UPDATE_OBJECT with one values block.
Bytes values(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

// An item object (the owner's create block).
Bytes itemObject(u64 guid, u32 entry, u32 count, u32 durability = 0, u32 maxDurability = 0, u32 flags = 0) {
    return created(guid, game::ObjectType::Item,
                   {{uf::OBJECT_FIELD_ENTRY, entry},
                    {uf::ITEM_FIELD_OWNER, low(kPlayer)},
                    {uf::ITEM_FIELD_STACK_COUNT, count},
                    {uf::ITEM_FIELD_DURABILITY, durability},
                    {uf::ITEM_FIELD_MAXDURABILITY, maxDurability},
                    {uf::ITEM_FIELD_FLAGS, flags}});
}

// A player slot (PLAYER_FIELD_INV_SLOT_HEAD + 2 * slot) holding `guid`.
void slot(Fields& f, u8 slotIndex, u64 guid) {
    f.push_back({static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slotIndex * 2), low(guid)});
    f.push_back({static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slotIndex * 2 + 1), high(guid)});
}

// HandleItemQuerySingleOpcode's layout, from a template.
Bytes itemQuery(const net::ItemQueryInfo& i) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(i.entry).u32v(i.itemClass).u32v(i.subclass).cstring(i.name).cstring("").cstring("").cstring("");
    w.u32v(i.displayId).u32v(i.quality).u32v(i.flags).u32v(i.buyPrice).u32v(i.sellPrice).u32v(i.inventoryType);
    w.u32v(i.allowableClass).u32v(i.allowableRace).u32v(i.itemLevel).u32v(i.requiredLevel);
    w.u32v(i.requiredSkill).u32v(i.requiredSkillRank).u32v(i.requiredSpell).u32v(i.requiredHonorRank);
    w.u32v(i.requiredCityRank).u32v(i.requiredReputationFaction).u32v(i.requiredReputationRank).u32v(i.maxCount);
    w.u32v(i.stackable).u32v(i.containerSlots);
    for (const auto& s : i.stats) {
        w.u32v(s.type).u32v(static_cast<u32>(s.value));
    }
    for (const auto& d : i.damages) {
        w.f32v(d.min).f32v(d.max).u32v(d.school);
    }
    w.u32v(static_cast<u32>(i.armor));
    for (const i32 r : i.resistances) {
        w.u32v(static_cast<u32>(r));
    }
    w.u32v(i.delayMs).u32v(i.ammoType).f32v(i.rangedModRange);
    for (const auto& s : i.spells) {
        w.u32v(s.id).u32v(s.trigger).u32v(static_cast<u32>(s.charges)).u32v(static_cast<u32>(s.cooldownMs));
        w.u32v(s.category).u32v(static_cast<u32>(s.categoryCooldownMs));
    }
    w.u32v(i.bonding).cstring(i.description).u32v(i.pageText).u32v(i.language).u32v(i.pageMaterial);
    w.u32v(i.startQuest).u32v(i.lockId).u32v(static_cast<u32>(i.material)).u32v(i.sheath).u32v(i.randomProperty);
    w.u32v(i.block).u32v(i.itemSet).u32v(i.maxDurability).u32v(i.area).u32v(i.map).u32v(i.bagFamily);
    return out;
}

net::ItemQueryInfo templateOf(u32 entry, const char* name, u32 itemClass, u32 subclass, u32 quality, u32 sellPrice) {
    net::ItemQueryInfo i;
    i.entry = entry;
    i.found = true;
    i.name = name;
    i.itemClass = itemClass;
    i.subclass = subclass;
    i.quality = quality;
    i.sellPrice = sellPrice;
    i.stackable = 1;
    return i;
}

Bytes guidBody(u64 guid) {
    Bytes out;
    net::writeGuid(out, guid);
    return out;
}

Bytes bytes(std::initializer_list<int> list) {
    Bytes out;
    for (int b : list) {
        out.push_back(static_cast<u8>(b));
    }
    return out;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    std::vector<Sent> sent;
    std::vector<u64> createdGuids;
    std::vector<game::GameEvent> seen;
    u32 now = 1000;
    u32 cursor = 0;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kPlayer, 0, true, now);
    }
    game::Inventory& inv() { return play.inventory(); }
    void apply(const Bytes& update) {
        createdGuids.clear();
        game::UpdateStats stats;
        stats.createdGuids = &createdGuids;
        REQUIRE(store.applyUpdateObject(update, now, &stats));
        play.afterUpdate(createdGuids, now);
        play.update(now);
        play.inventory().refresh(now, true); // the tests read the model right away (it is rebuilt 10 times a second)
    }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) { return play.handle(opcode, body, now); }
    // The last body sent with `opcode`; everything sent is forgotten.
    Bytes last(Opcode opcode) {
        Bytes out;
        bool found = false;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out = s.body;
                found = true;
            }
        }
        CHECK_MESSAGE(found, "nothing sent with opcode " << static_cast<u32>(opcode));
        sent.clear();
        return out;
    }
    u32 count(Opcode opcode) const {
        return static_cast<u32>(
            std::count_if(sent.begin(), sent.end(), [&](const Sent& s) { return s.opcode == opcode; }));
    }
    // The first event of `kind` since the last call (nullptr if none).
    const game::GameEvent* event(GameEventKind kind) {
        seen.clear();
        while (const game::GameEvent* e = play.events().next(cursor)) {
            seen.push_back(*e);
        }
        for (const game::GameEvent& e : seen) {
            if (e.kind == kind) {
                return &e;
            }
        }
        return nullptr;
    }
    // A character with a sword (main hand, 18 / 20), a 6-slot bag (slot 19) holding 5 breads in its third slot, a
    // grey item in the backpack's first slot, 2 breads in the bank and one bank bag slot bought.
    void character(u32 money = 5000) {
        apply(itemObject(kSword, kSwordEntry, 1, 18, 20));
        apply(itemObject(kBread, kBreadEntry, 5));
        apply(itemObject(kJunk, kJunkEntry, 3));
        apply(itemObject(kBanked, kBreadEntry, 2));
        apply(created(kBag, game::ObjectType::Container,
                      {{uf::OBJECT_FIELD_ENTRY, kBagEntry},
                       {uf::ITEM_FIELD_STACK_COUNT, 1},
                       {uf::CONTAINER_FIELD_NUM_SLOTS, 6},
                       {static_cast<u16>(uf::CONTAINER_FIELD_SLOT_1 + 2 * 2), low(kBread)},
                       {static_cast<u16>(uf::CONTAINER_FIELD_SLOT_1 + 2 * 2 + 1), high(kBread)}}));
        Fields f = {{uf::UNIT_FIELD_HEALTH, 100},
                    {uf::UNIT_FIELD_LEVEL, 5},
                    {uf::PLAYER_FIELD_COINAGE, money},
                    {uf::PLAYER_BYTES_2, 1u << 16}};
        slot(f, game::kSlotMainHand, kSword);
        slot(f, 19, kBag);
        slot(f, 23, kJunk);
        slot(f, 39, kBanked);
        apply(created(kPlayer, game::ObjectType::Player, f));
    }
    void templates() {
        net::ItemQueryInfo sword = templateOf(kSwordEntry, "Worn Shortsword", 2, 7, 1, 7);
        sword.inventoryType = 21;
        sword.maxDurability = 20;
        sword.itemLevel = 2;
        net::ItemQueryInfo bag = templateOf(kBagEntry, "Small Brown Pouch", 1, 0, 1, 25);
        bag.containerSlots = 6;
        net::ItemQueryInfo bread = templateOf(kBreadEntry, "Tough Hunk of Bread", 0, 0, 1, 1);
        bread.stackable = 20;
        net::ItemQueryInfo junk = templateOf(kJunkEntry, "Broken Fang", 15, 0, 0, 3);
        junk.stackable = 5;
        for (const net::ItemQueryInfo* t : {&sword, &bag, &bread, &junk}) {
            REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(*t)) == game::Gameplay::Handled::Yes);
        }
    }
};

} // namespace

TEST_CASE("Inventory: equipment, backpack, bags, bank and money from the update fields") {
    Harness h;
    h.character();
    const game::Inventory& inv = h.inv();
    const game::ItemInstance* sword = inv.equipped(game::kSlotMainHand);
    REQUIRE(sword);
    CHECK(sword->guid == kSword);
    CHECK(sword->entry == kSwordEntry);
    CHECK(sword->durability == 18);
    CHECK(sword->maxDurability == 20);
    CHECK(inv.containerSize(game::kContainerBackpack) == 16);
    CHECK(inv.containerSize(1) == 6);
    CHECK(inv.containerSize(2) == 0); // no bag there
    CHECK(inv.containerSize(game::kContainerBank) == 24);
    REQUIRE(inv.bagOf(1));
    CHECK(inv.bagOf(1)->guid == kBag);
    REQUIRE(inv.containerItem(1, 2));
    CHECK(inv.containerItem(1, 2)->entry == kBreadEntry);
    CHECK(inv.containerItem(1, 2)->count == 5);
    CHECK(inv.containerPos(1, 2) == ItemPos{19, 2});
    CHECK(inv.containerItem(game::kContainerBackpack, 0)->guid == kJunk);
    CHECK(inv.containerItem(game::kContainerBank, 0)->guid == kBanked);
    CHECK(inv.find(kBread)->pos == ItemPos{19, 2});
    CHECK(inv.containerOf({19, 2}) == 1);
    CHECK(inv.containerOf({255, 23}) == game::kContainerBackpack);
    CHECK(inv.containerOf({255, 40}) == game::kContainerBank);
    CHECK(inv.containerOf({255, 15}) == 99);
    CHECK(inv.money() == 5000);
    CHECK(inv.bankBagSlots() == 1);
    CHECK(inv.freeSlots() == 15 + 5);
    CHECK(inv.totalSlots() == 22);
    CHECK(inv.count(kBreadEntry) == 5);
    CHECK(inv.count(kBreadEntry, true) == 7);
    // The phase-7 helper still sees the same items.
    CHECK(game::inventoryCount(h.store, *h.store.find(kPlayer), kBreadEntry) == 5);
    // Every item's template is asked for once.
    CHECK(h.count(Opcode::CMSG_ITEM_QUERY_SINGLE) == 4);

    // A move (the server rewrites both slots) bumps the version; an unrelated update does not.
    const u32 version = inv.version();
    Fields move;
    slot(move, 23, 0);
    slot(move, 30, kJunk);
    h.apply(values(kPlayer, move));
    CHECK(inv.version() != version);
    CHECK_FALSE(inv.at({255, 23}));
    CHECK(inv.at({255, 30})->guid == kJunk);
    const u32 moved = inv.version();
    h.apply(values(kPlayer, {{uf::UNIT_FIELD_HEALTH, 90}}));
    CHECK(inv.version() == moved);

    // Into the bag: the container's slot changes.
    Fields out;
    slot(out, 30, 0);
    h.apply(values(kPlayer, out));
    h.apply(values(kBag, {{static_cast<u16>(uf::CONTAINER_FIELD_SLOT_1 + 2 * 5), low(kJunk)},
                          {static_cast<u16>(uf::CONTAINER_FIELD_SLOT_1 + 2 * 5 + 1), high(kJunk)}}));
    CHECK(inv.find(kJunk)->pos == ItemPos{19, 5});
    CHECK(inv.freeSlots() == 16 + 4);

    // Destroyed: the object goes and the slot empties.
    Bytes destroy;
    net::ByteWriter(destroy).u64v(kBanked);
    REQUIRE(h.store.applyDestroy(destroy));
    Fields gone;
    slot(gone, 39, 0);
    h.apply(values(kPlayer, gone));
    CHECK_FALSE(inv.containerItem(game::kContainerBank, 0));
    CHECK(inv.count(kBreadEntry, true) == 5);

    // An item seen in its slot before its object arrives: no entry yet, filled in later.
    Fields early;
    slot(early, 24, kChest);
    h.apply(values(kPlayer, early));
    REQUIRE(inv.at({255, 24}));
    CHECK(inv.at({255, 24})->entry == 0);
    h.apply(itemObject(kChest, kChestEntry, 1));
    CHECK(inv.at({255, 24})->entry == kChestEntry);

    // Durability: yellow at 20% or less, red when broken (DurabilityFrame); part 8 is the weapon.
    CHECK_FALSE(inv.durability().any());
    h.apply(values(kSword, {{uf::ITEM_FIELD_DURABILITY, 4}}));
    CHECK(inv.durability().parts[8] == game::DurabilityAlert::Low);
    h.apply(values(kSword, {{uf::ITEM_FIELD_DURABILITY, 0}}));
    CHECK(inv.durability().parts[8] == game::DurabilityAlert::Broken);
    CHECK(inv.equipped(game::kSlotMainHand)->broken());
}

TEST_CASE("Inventory: item actions as the server reads them") {
    Harness h;
    h.character();
    h.templates();
    h.sent.clear();
    game::Inventory& inv = h.inv();
    REQUIRE(inv.swap({255, 23}, {255, 24})); // both in bag 255: CMSG_SWAP_INV_ITEM
    CHECK(h.last(Opcode::CMSG_SWAP_INV_ITEM) == bytes({23, 24}));
    REQUIRE(inv.swap({255, 23}, {19, 0})); // into a bag: CMSG_SWAP_ITEM, destination first
    CHECK(h.last(Opcode::CMSG_SWAP_ITEM) == bytes({19, 0, 255, 23}));
    CHECK_FALSE(inv.swap({255, 25}, {255, 26})); // nothing there
    CHECK_FALSE(inv.swap({255, 23}, {255, 23}));
    REQUIRE(inv.equip({19, 2}));
    CHECK(h.last(Opcode::CMSG_AUTOEQUIP_ITEM) == bytes({19, 2}));
    REQUIRE(inv.equipTo({255, 23}, game::kSlotFinger2));
    CHECK(h.last(Opcode::CMSG_AUTOEQUIP_ITEM_SLOT) == bytes({0x04, 0x09, 0, 0, 0, 0, 0, 0x40, 11}));
    REQUIRE(inv.store({255, game::kSlotMainHand})); // unequip into the bags: dstbag NULL_BAG
    CHECK(h.last(Opcode::CMSG_AUTOSTORE_BAG_ITEM) == bytes({255, 15, 0}));
    REQUIRE(inv.store({19, 2}, 255)); // into the backpack
    CHECK(h.last(Opcode::CMSG_AUTOSTORE_BAG_ITEM) == bytes({19, 2, 255}));
    // Split: two of the five breads to the first empty slot (the backpack's second).
    REQUIRE(inv.split({19, 2}, 2));
    CHECK(h.last(Opcode::CMSG_SPLIT_ITEM) == bytes({19, 2, 255, 24, 2}));
    CHECK_FALSE(inv.split({19, 2}, 5)); // the whole stack is not a split
    CHECK_FALSE(inv.split({19, 2}, 0));
    REQUIRE(inv.destroy({255, 23}));
    CHECK(h.last(Opcode::CMSG_DESTROYITEM) == bytes({255, 23, 0, 0, 0, 0}));
    REQUIRE(inv.destroy({19, 2}, 2));
    CHECK(h.last(Opcode::CMSG_DESTROYITEM) == bytes({19, 2, 2, 0, 0, 0}));
    REQUIRE(inv.open({255, 23}));
    CHECK(h.last(Opcode::CMSG_OPEN_ITEM) == bytes({255, 23}));
    REQUIRE(inv.read({255, 23}));
    CHECK(h.last(Opcode::CMSG_READ_ITEM) == bytes({255, 23}));
    REQUIRE(inv.setAmmo(2512));
    CHECK(h.last(Opcode::CMSG_SET_AMMO) == bytes({0xD0, 0x09, 0, 0}));

    // Using an item at a bag position: its on-use spell, else read / open (the client's right click).
    net::ItemQueryInfo book = templateOf(kBookEntry, "A Letter", 12, 0, 1, 0);
    book.pageText = 17;
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(book)) == game::Gameplay::Handled::Yes);
    net::ItemQueryInfo chest = templateOf(kChestEntry, "Clam", 15, 0, 1, 0);
    chest.flags = game::kItemFlagLootable;
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(chest)) == game::Gameplay::Handled::Yes);
    h.apply(itemObject(kBook, kBookEntry, 1));
    h.apply(itemObject(kChest, kChestEntry, 1));
    Fields more;
    slot(more, 25, kBook);
    slot(more, 26, kChest);
    h.apply(values(kPlayer, more));
    h.sent.clear();
    REQUIRE(h.play.useItemAt({255, 25}));
    CHECK(h.last(Opcode::CMSG_READ_ITEM) == bytes({255, 25}));
    REQUIRE(h.play.useItemAt({255, 26}));
    CHECK(h.last(Opcode::CMSG_OPEN_ITEM) == bytes({255, 26}));
    net::ItemQueryInfo bread = templateOf(kBreadEntry, "Tough Hunk of Bread", 0, 0, 1, 1);
    bread.spells[0].id = 433;
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(bread)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.play.useItemAt({19, 2}));
    CHECK(h.last(Opcode::CMSG_USE_ITEM) == bytes({19, 2, 0, 0, 0})); // bag, slot, spell index, self
}

TEST_CASE("Vendor: open, buy, sell, junk, buy back, repair") {
    Harness h;
    h.character();
    h.templates();
    h.apply(created(kVendor, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 100},
                     {uf::UNIT_NPC_FLAGS, game::kNpcFlagVendor | game::kNpcFlagRepair},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, 12}}));
    h.sent.clear();
    h.event(GameEventKind::VendorOpened);
    // A vendor without a gossip menu: CMSG_LIST_INVENTORY straight away.
    CHECK(h.play.interact(kVendor) == game::Interaction::Vendor);
    CHECK(h.last(Opcode::CMSG_LIST_INVENTORY) == guidBody(kVendor));
    Bytes list;
    net::ByteWriter w(list);
    w.u64v(kVendor).u8v(2);
    w.u32v(1).u32v(159).u32v(18084).u32v(net::kVendorUnlimited).u32v(25).u32v(0).u32v(1);
    w.u32v(2).u32v(2512).u32v(5996).u32v(3).u32v(10).u32v(0).u32v(200);
    REQUIRE(h.packet(Opcode::SMSG_LIST_INVENTORY, list) == game::Gameplay::Handled::Yes);
    const game::VendorWindow& vendor = h.inv().vendor();
    REQUIRE(vendor.open());
    CHECK(vendor.npc == kVendor);
    CHECK(vendor.canRepair);
    CHECK(vendor.items().size() == 2);
    CHECK(h.event(GameEventKind::VendorOpened));
    CHECK(h.count(Opcode::CMSG_ITEM_QUERY_SINGLE) == 2); // names and icons of the rows

    // Three purchases of the arrows' row (200 each); the stock drops.
    h.sent.clear();
    REQUIRE(h.inv().buy(1, 3));
    CHECK(h.last(Opcode::CMSG_BUY_ITEM) == bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 0xD0, 0x09, 0, 0, 3, 0}));
    Bytes bought;
    net::ByteWriter(bought).u64v(kVendor).u32v(2).u32v(0).u32v(3);
    REQUIRE(h.packet(Opcode::SMSG_BUY_ITEM, bought) == game::Gameplay::Handled::Yes);
    CHECK(vendor.items()[1].stock == 0);
    const game::GameEvent* got = h.event(GameEventKind::ItemBought);
    REQUIRE(got);
    CHECK(got->id == 2512);
    CHECK(got->value[0] == 600);
    Bytes failed;
    net::ByteWriter(failed).u64v(kVendor).u32v(159).u8v(net::kBuyErrNotEnoughMoney);
    REQUIRE(h.packet(Opcode::SMSG_BUY_FAILED, failed) == game::Gameplay::Handled::Yes);
    const game::GameEvent* refused = h.event(GameEventKind::BuyFailed);
    REQUIRE(refused);
    CHECK(refused->value[0] == net::kBuyErrNotEnoughMoney);

    // Sell the bread stack; a refusal is an event.
    REQUIRE(h.inv().sell({19, 2}));
    CHECK(h.last(Opcode::CMSG_SELL_ITEM) ==
          bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 0x03, 0x09, 0, 0, 0, 0, 0, 0x40, 0}));
    Bytes sellFailed;
    net::ByteWriter(sellFailed).u64v(kVendor).u64v(kBread).u8v(net::kSellErrCantSellItem);
    REQUIRE(h.packet(Opcode::SMSG_SELL_ITEM, sellFailed) == game::Gameplay::Handled::Yes);
    REQUIRE(h.event(GameEventKind::SellFailed));

    // Grey items with a price: their value, then one CMSG_SELL_ITEM each.
    u32 stacks = 0;
    CHECK(h.inv().junkValue(&stacks) == 9);
    CHECK(stacks == 1);
    CHECK(h.inv().sellJunk() == 1);
    CHECK(h.last(Opcode::CMSG_SELL_ITEM) ==
          bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 0x04, 0x09, 0, 0, 0, 0, 0, 0x40, 0}));

    // The sold item waits in the first buyback slot with its price.
    h.apply(itemObject(kSold, kJunkEntry, 3));
    Fields buyback;
    slot(buyback, game::kBuybackSlotStart, kSold);
    buyback.push_back({uf::PLAYER_FIELD_BUYBACK_PRICE_1, 9});
    slot(buyback, 23, 0);
    h.apply(values(kPlayer, buyback));
    CHECK(h.inv().buybackPrice(0) == 9);
    CHECK(h.inv().buybackPrice(1) == 0);
    REQUIRE(h.inv().buyback(0));
    CHECK(h.last(Opcode::CMSG_BUYBACK_ITEM) == bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 69, 0, 0, 0}));
    CHECK_FALSE(h.inv().buyback(1));

    // Repair one item, or everything.
    REQUIRE(h.inv().repair({255, game::kSlotMainHand}));
    CHECK(h.last(Opcode::CMSG_REPAIR_ITEM) ==
          bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 0x01, 0x09, 0, 0, 0, 0, 0, 0x40}));
    REQUIRE(h.inv().repairAll());
    CHECK(h.last(Opcode::CMSG_REPAIR_ITEM) == bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 0, 0, 0, 0, 0, 0, 0, 0}));

    h.inv().closeVendor();
    CHECK_FALSE(h.inv().vendor().open());
    CHECK_FALSE(h.inv().buy(0));
    CHECK_FALSE(h.inv().sell({19, 2}));

    // From a gossip menu ("I want to browse your goods"): the vendor replaces the menu.
    Bytes gossip;
    net::ByteWriter(gossip).u64v(kVendor).u32v(1).u32v(1).u32v(0).u8v(1).u8v(0).cstring("Browse").u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_GOSSIP_MESSAGE, gossip) == game::Gameplay::Handled::Yes);
    REQUIRE(h.play.dialog().open());
    REQUIRE(h.packet(Opcode::SMSG_LIST_INVENTORY, list) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(h.play.dialog().open());
    CHECK(h.inv().vendor().open());

    // A vendor that also gives quests: CMSG_GOSSIP_HELLO (its menu lists both).
    h.apply(values(kVendor, {{uf::UNIT_NPC_FLAGS, game::kNpcFlagVendor | game::kNpcFlagQuestGiver}}));
    h.sent.clear();
    CHECK(h.play.interact(kVendor) == game::Interaction::Gossip);
    CHECK(h.last(Opcode::CMSG_GOSSIP_HELLO) == guidBody(kVendor));
    // Malformed: refused, nothing read past the end.
    CHECK(h.packet(Opcode::SMSG_LIST_INVENTORY, bytes({1, 2, 3})) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Bank: open, move items, buy a bag slot") {
    Harness h;
    h.character();
    h.apply(created(kBanker, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_NPC_FLAGS, game::kNpcFlagBanker}}));
    h.sent.clear();
    h.event(GameEventKind::BankOpened);
    CHECK(h.play.interact(kBanker) == game::Interaction::Bank);
    CHECK(h.last(Opcode::CMSG_BANKER_ACTIVATE) == guidBody(kBanker));
    CHECK_FALSE(h.inv().toBank({19, 2})); // not open yet
    REQUIRE(h.packet(Opcode::SMSG_SHOW_BANK, guidBody(kBanker)) == game::Gameplay::Handled::Yes);
    CHECK(h.inv().banker() == kBanker);
    CHECK(h.event(GameEventKind::BankOpened));
    REQUIRE(h.inv().toBank({19, 2}));
    CHECK(h.last(Opcode::CMSG_AUTOBANK_ITEM) == bytes({19, 2}));
    REQUIRE(h.inv().fromBank({255, 39}));
    CHECK(h.last(Opcode::CMSG_AUTOSTORE_BANK_ITEM) == bytes({255, 39}));
    REQUIRE(h.inv().swap({255, 39}, {255, 24})); // bank <-> backpack by hand
    CHECK(h.last(Opcode::CMSG_SWAP_INV_ITEM) == bytes({39, 24}));
    REQUIRE(h.inv().buyBankSlot());
    CHECK(h.last(Opcode::CMSG_BUY_BANK_SLOT) == guidBody(kBanker));
    Bytes poor;
    net::writeU32(poor, net::kBankSlotInsufficientFunds);
    REQUIRE(h.packet(Opcode::SMSG_BUY_BANK_SLOT_RESULT, poor) == game::Gameplay::Handled::Yes);
    const game::GameEvent* e = h.event(GameEventKind::BankSlotFailed);
    REQUIRE(e);
    CHECK(e->value[0] == static_cast<i32>(net::kBankSlotInsufficientFunds));
    // Bought: only PLAYER_BYTES_2 changes.
    h.apply(values(kPlayer, {{uf::PLAYER_BYTES_2, 2u << 16}}));
    CHECK(h.inv().bankBagSlots() == 2);
    const game::GameEvent* bought = h.event(GameEventKind::BankSlotBought);
    REQUIRE(bought);
    CHECK(bought->value[0] == 2);
    h.inv().closeBank();
    CHECK_FALSE(h.inv().buyBankSlot());
}

TEST_CASE("Reading: a book's pages, a letter, a refusal") {
    Harness h;
    h.character();
    net::ItemQueryInfo book = templateOf(kBookEntry, "Old Book", 12, 0, 1, 0);
    book.pageText = 17;
    book.pageMaterial = 2;
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(book)) == game::Gameplay::Handled::Yes);
    h.apply(itemObject(kBook, kBookEntry, 1));
    Fields f;
    slot(f, 25, kBook);
    h.apply(values(kPlayer, f));
    h.sent.clear();
    REQUIRE(h.inv().read({255, 25}));
    CHECK(h.last(Opcode::CMSG_READ_ITEM) == bytes({255, 25}));
    Bytes ok;
    net::ByteWriter(ok).u64v(kBook).u64v(kBook);
    REQUIRE(h.packet(Opcode::SMSG_READ_ITEM_OK, ok) == game::Gameplay::Handled::Yes);
    const game::ReadingWindow& reading = h.inv().reading();
    REQUIRE(reading.open());
    CHECK(reading.material == 2);
    CHECK(reading.waiting);
    CHECK(h.last(Opcode::CMSG_PAGE_TEXT_QUERY) == bytes({17, 0, 0, 0, 0x07, 0x09, 0, 0, 0, 0, 0, 0x40}));
    Bytes page1, page2;
    net::ByteWriter(page1).u32v(17).cstring("First page.").u32v(18);
    net::ByteWriter(page2).u32v(18).cstring("Second page.").u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE, page1) == game::Gameplay::Handled::Yes);
    CHECK(reading.pageCount == 1);
    CHECK(reading.waiting);
    REQUIRE(h.packet(Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE, page2) == game::Gameplay::Handled::Yes);
    CHECK(reading.pageCount == 2);
    CHECK_FALSE(reading.waiting);
    CHECK(reading.pages[0] == "First page.");
    h.inv().turnPage(1);
    CHECK(reading.page == 1);
    h.inv().turnPage(5);
    CHECK(reading.page == 1);
    h.inv().closeReading();
    CHECK_FALSE(reading.open());
    // Read again: the pages are known, nothing is asked.
    REQUIRE(h.packet(Opcode::SMSG_READ_ITEM_OK, ok) == game::Gameplay::Handled::Yes);
    CHECK(h.count(Opcode::CMSG_PAGE_TEXT_QUERY) == 0);
    CHECK(reading.pageCount == 2);

    // A letter (an item text from the mail).
    h.apply(values(kBook, {{uf::ITEM_FIELD_ITEM_TEXT_ID, 55}}));
    REQUIRE(h.packet(Opcode::SMSG_READ_ITEM_OK, ok) == game::Gameplay::Handled::Yes);
    CHECK(h.last(Opcode::CMSG_ITEM_TEXT_QUERY) == bytes({55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    Bytes letter;
    net::ByteWriter(letter).u32v(55).cstring("Dear Arthel,");
    REQUIRE(h.packet(Opcode::SMSG_ITEM_TEXT_QUERY_RESPONSE, letter) == game::Gameplay::Handled::Yes);
    CHECK(reading.pageCount == 1);
    CHECK(reading.pages[0] == "Dear Arthel,");

    Bytes failed;
    net::ByteWriter(failed).u64v(kBook).u8v(1).u64v(kBook);
    REQUIRE(h.packet(Opcode::SMSG_READ_ITEM_FAILED, failed) == game::Gameplay::Handled::Yes);
    CHECK(h.event(GameEventKind::ReadItemFailed));
}

TEST_CASE("Inventory: proficiencies, equip cooldowns, death durability, bag families") {
    Harness h;
    h.character();
    h.templates();
    game::Inventory& inv = h.inv();
    CHECK(inv.proficient(game::kItemClassArmor, 4)); // unknown yet: allowed
    Bytes armor;
    net::ByteWriter(armor).u8v(4).u32v(0x0F); // misc, cloth, leather, mail
    REQUIRE(h.packet(Opcode::SMSG_SET_PROFICIENCY, armor) == game::Gameplay::Handled::Yes);
    CHECK(inv.proficient(game::kItemClassArmor, 2));
    CHECK_FALSE(inv.proficient(game::kItemClassArmor, 4)); // plate
    CHECK(inv.proficient(game::kItemClassConsumable, 0));
    // vMaNGOS casts the proficiency passives while loading the character: they arrive before SMSG_LOGIN_VERIFY_WORLD
    // and outlive entering the world; logging out forgets them.
    h.play.leaveWorld();
    CHECK(inv.proficient(game::kItemClassArmor, 4));
    REQUIRE(h.packet(Opcode::SMSG_SET_PROFICIENCY, armor) == game::Gameplay::Handled::Yes);
    h.play.enterWorld(kPlayer, 0, true, h.now);
    CHECK_FALSE(inv.proficient(game::kItemClassArmor, 4));
    Bytes cooldown;
    net::ByteWriter(cooldown).u64v(kSword).u32v(8690);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_COOLDOWN, cooldown) == game::Gameplay::Handled::Yes);
    CHECK(inv.itemCooldownLeft(8690, h.now + 1000) == 29000);
    CHECK(inv.itemCooldownLeft(8690, h.now + 31000) == 0);
    h.event(GameEventKind::DurabilityLoss);
    REQUIRE(h.packet(Opcode::SMSG_DURABILITY_DAMAGE_DEATH, {}) == game::Gameplay::Handled::Yes);
    CHECK(h.event(GameEventKind::DurabilityLoss));

    // A quiver in bag slot 20 takes arrows only; arrows go there first, other items skip it.
    constexpr u64 kQuiver = 0x4000000000000990ull;
    net::ItemQueryInfo quiver = templateOf(2101, "Light Quiver", game::kItemClassQuiver, game::kQuiverQuiver, 1, 1);
    quiver.containerSlots = 4;
    quiver.bagFamily = game::kBagFamilyArrows;
    net::ItemQueryInfo arrow = templateOf(2512, "Rough Arrow", game::kItemClassProjectile, 2, 1, 0);
    arrow.bagFamily = game::kBagFamilyArrows;
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(quiver)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(arrow)) == game::Gameplay::Handled::Yes);
    h.apply(created(kQuiver, game::ObjectType::Container,
                    {{uf::OBJECT_FIELD_ENTRY, 2101}, {uf::CONTAINER_FIELD_NUM_SLOTS, 4}}));
    Fields f;
    slot(f, 20, kQuiver);
    h.apply(values(kPlayer, f));
    CHECK(inv.containerFamily(2) == game::kBagFamilyArrows);
    CHECK(inv.freeSlots() == 15 + 5 + 4);
    CHECK(inv.freeSlots(true) == 15 + 5);
    ItemPos where;
    REQUIRE(inv.findEmptySlot(2512, false, where));
    CHECK(where == ItemPos{20, 0});
    REQUIRE(inv.findEmptySlot(kBreadEntry, false, where));
    CHECK(where == ItemPos{255, 24});
    REQUIRE(inv.findEmptySlot(kBreadEntry, true, where));
    CHECK(where == ItemPos{255, 40}); // the bank's second slot (the first holds an item)
}

TEST_CASE("Item tables: repair costs, bank slot prices, sets, enchantments, names, spell texts (db.vdpk)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    REQUIRE(test::loadTexts()); // the words spell texts add ("70 to 90", "6 sec") are the client's
    game::ItemData data;
    REQUIRE(data.init(db));
    // BankBagSlotPrices.dbc: 10 s, 1 g, 10 g, 25 g, 50 g, 100 g.
    CHECK(data.bankSlotPrice(1) == 1000);
    CHECK(data.bankSlotPrice(2) == 10000);
    CHECK(data.bankSlotPrice(6) == 1000000);
    // Repair: DurabilityCosts[item level][subclass column] x DurabilityQuality[(quality + 1) * 2], read by hand.
    const std::optional<pack::PackAsset> costsAsset = db.find("DBFilesClient\\DurabilityCosts.dbc");
    const std::optional<pack::PackAsset> qualityAsset = db.find("DBFilesClient\\DurabilityQuality.dbc");
    REQUIRE(costsAsset);
    REQUIRE(qualityAsset);
    const db::DbcView costs = db::DbcView::parse(costsAsset->data, costsAsset->size).value();
    const db::DbcView quality = db::DbcView::parse(qualityAsset->data, qualityAsset->size).value();
    const u32 swordColumn = costs.getU32(costs.findById(5), 1 + 7);      // weapon subclass 7
    const u32 mailColumn = costs.getU32(costs.findById(20), 1 + 21 + 3); // armor subclass 3
    const f32 common = quality.getF32(quality.findById(4), 1);           // quality 1
    const f32 uncommon = quality.getF32(quality.findById(6), 1);         // quality 2
    CHECK(data.repairCost(2, 7, 1, 5, 10) == static_cast<u32>(static_cast<f32>(10 * swordColumn) * common));
    CHECK(data.repairCost(4, 3, 2, 20, 35) == static_cast<u32>(static_cast<f32>(35 * mailColumn) * uncommon));
    CHECK(data.repairCost(2, 7, 1, 5, 10) == 8);
    CHECK(data.repairCost(4, 3, 2, 20, 35) == 175);
    CHECK(data.repairCost(2, 7, 1, 5, 0) == 0);
    CHECK(data.repairCost(2, 7, 0, 1, 1) >= 1); // never free once something is lost

    // Through the inventory: the sword lost 2 of 20 (item level 2, common).
    Harness h;
    h.inv().setData(&data);
    h.character();
    h.templates();
    const game::ItemInstance* sword = h.inv().equipped(game::kSlotMainHand);
    REQUIRE(sword);
    CHECK(h.inv().repairCost(*sword) == data.repairCost(2, 7, 1, 2, 2));
    CHECK(h.inv().repairAllCost() == h.inv().repairCost(*sword));
    CHECK(h.inv().nextBankSlotPrice() == 10000); // one slot bought

    const std::optional<game::ItemSetInfo> set = data.itemSet(41);
    REQUIRE(set.has_value());
    CHECK(set->name == "Dal'Rend's Arms");
    CHECK(set->itemCount == 2);
    CHECK(set->thresholds[0] == 2);
    CHECK(data.enchantmentText(1883) == "Intellect +7");
    CHECK(data.randomSuffix(5) == "of Intellect");
    CHECK(data.skillName(43) == "Swords");
    CHECK(data.factionName(72) == "Stormwind");
    // ChrClasses / ChrRaces.dbc (DBCfmt.h: name at 5 / 17) and ItemSubClass.dbc (not in vMaNGOS: display name at 10).
    CHECK(data.className(1) == "Warrior");
    CHECK(data.className(11) == "Druid");
    CHECK(data.className(6).empty());
    CHECK(data.raceName(4) == "Night Elf");
    CHECK(data.raceName(5) == "Undead");
    CHECK(data.hordeRace(2));
    CHECK(data.hordeRace(8));
    CHECK_FALSE(data.hordeRace(1));
    CHECK_FALSE(data.hordeRace(7));
    CHECK(data.subclassName(game::kItemClassWeapon, 7) == "Sword");
    CHECK(data.subclassName(game::kItemClassWeapon, 20) == "Fishing Pole");
    CHECK(data.subclassName(game::kItemClassArmor, 4) == "Plate");
    CHECK(data.subclassName(game::kItemClassArmor, 0) == "Miscellaneous");
    CHECK(data.subclassName(game::kItemClassProjectile, 2) == "Arrow");
    CHECK(data.subclassName(game::kItemClassContainer, 0) == "Bag");
    CHECK(data.subclassName(game::kItemClassContainer, 1) == "Soul Bag");
    CHECK(data.subclassName(game::kItemClassQuiver, 3) == "Ammo Pouch");
    CHECK(data.subclassName(game::kItemClassWeapon, 99).empty());

    // "Use:" / "Equip:" texts as the client's tooltips read them: Spell.dbc's own template (read through
    // game::SpellText) with each token replaced by the value the item data has to give it.
    game::SpellText spells;
    REQUIRE(spells.init(db));
    const auto expected = [&](u32 spell, std::initializer_list<std::pair<std::string_view, std::string_view>> tokens) {
        game::SpellDetails details;
        REQUIRE(spells.details(spell, details));
        std::string out(details.description);
        for (const auto& [token, value] : tokens) {
            const usize at = out.find(token);
            REQUIRE_MESSAGE(at != std::string::npos, std::string(token));
            out.replace(at, token.size(), value);
        }
        return out;
    };
    std::string text;
    REQUIRE(data.spellDescription(746, text)); // Linen Bandage
    CHECK(text == expected(746, {{"$o1", "66"}, {"$d", "6 sec"}}));
    REQUIRE(data.spellDescription(439, text)); // Minor Healing Potion: dice
    CHECK(text == expected(439, {{"$s1", "70 to 90"}}));
    REQUIRE(data.spellDescription(433, text)); // Tough Hunk of Bread: health per 5 sec over 18 sec
    CHECK(text == expected(433, {{"$o1", "61"}, {"$d", "18 sec"}}));
    REQUIRE(data.spellDescription(8690, text)); // Hearthstone
    CHECK(text == expected(8690, {{"$z", "your home"}}));
    REQUIRE(data.spellDescription(7597, text));
    CHECK(text == expected(7597, {{"$s1", "1"}}));
    REQUIRE(data.spellDescription(18833, text)); // a chance-on-hit Firebolt
    CHECK(text == expected(18833, {{"$s1", "75 to 105"}}));
    CHECK_FALSE(data.spellDescription(999999, text));
}

namespace {

// The tooltip's lines as "left | right" strings, for comparing whole tooltips.
std::vector<std::string> tooltipText(const game::ItemTooltip& t) {
    std::vector<std::string> out;
    for (const game::TooltipLine& line : t.lines()) {
        std::string s(line.left);
        if (!line.right.empty()) {
            s += " | ";
            s += line.right;
        }
        if (line.money) {
            s += " " + std::to_string(line.money);
        }
        out.push_back(s);
    }
    return out;
}

} // namespace

TEST_CASE("Item tooltips: the client's lines (ui.vdpk's texts in English, db.vdpk's names and spell texts)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    REQUIRE(test::loadTexts());
    game::ItemData data;
    REQUIRE(data.init(db));
    Harness h;
    h.inv().setData(&data);
    h.character();
    game::TooltipContext c;
    c.level = 5;
    c.cls = 1; // warrior
    c.race = 1;
    c.inventory = &h.inv();
    c.queries = &h.play.queries();
    c.data = &data;
    game::ItemTooltip t;

    // Worn Shortsword, worn and a little damaged.
    net::ItemQueryInfo sword = templateOf(25, "Worn Shortsword", 2, 7, 1, 7);
    sword.inventoryType = game::kInvTypeWeaponMainHand;
    sword.damages[0] = {1.0f, 3.0f, 0};
    sword.delayMs = 1900;
    sword.maxDurability = 20;
    sword.itemLevel = 2;
    game::buildItemTooltip(sword, h.inv().equipped(game::kSlotMainHand), c, t);
    CHECK(t.quality() == 1);
    CHECK(tooltipText(t) == std::vector<std::string>{"Worn Shortsword", "Main Hand | Sword",
                                                     "1 - 3 Damage | Speed 1.90", "(1.1 damage per second)",
                                                     "Durability 18 / 20", "Sell Price: 7"});
    CHECK(t.lines()[0].leftColor == game::itemQualityColor(1));

    // Linen Bandage: a Use line with the spell's text, wrapped, in green.
    net::ItemQueryInfo bandage = templateOf(1251, "Linen Bandage", 0, 0, 1, 10);
    bandage.spells[0].id = 746;
    bandage.requiredSkill = 129; // First Aid
    bandage.requiredSkillRank = 1;
    game::buildItemTooltip(bandage, nullptr, c, t);
    CHECK(tooltipText(t) == std::vector<std::string>{"Linen Bandage", "Requires First Aid (1)",
                                                     "Use: Heals 66 damage over 6 sec.", "Sell Price: 10"});
    CHECK(t.lines()[2].leftColor == game::kTooltipGreen);
    CHECK(t.lines()[2].wrap);

    // Hearthstone: binds on pickup, unique, a Use line.
    net::ItemQueryInfo stone = templateOf(6948, "Hearthstone", 15, 0, 1, 0);
    stone.bonding = game::kBindOnPickup;
    stone.maxCount = 1;
    stone.spells[0].id = 8690;
    stone.description = "";
    game::buildItemTooltip(stone, nullptr, c, t);
    const std::vector<std::string> hearth = tooltipText(t);
    REQUIRE(hearth.size() == 4);
    CHECK(hearth[1] == "Binds when picked up");
    CHECK(hearth[2] == "Unique");
    CHECK(hearth[3].rfind("Use: Returns you to your home.", 0) == 0);

    // Armor for another class, needing a higher level: those lines red; plate the warrior cannot wear yet red too.
    Bytes armor;
    net::ByteWriter(armor).u8v(4).u32v(0x0F);
    REQUIRE(h.packet(Opcode::SMSG_SET_PROFICIENCY, armor) == game::Gameplay::Handled::Yes);
    net::ItemQueryInfo robe = templateOf(5000, "Robe of the Magi", 4, 4, 3, 1200);
    robe.inventoryType = game::kInvTypeRobe;
    robe.armor = 48;
    robe.stats[0] = {game::kItemModIntellect, 7};
    robe.stats[1] = {game::kItemModStamina, -2};
    robe.resistances[1] = 5;
    robe.allowableClass = 1u << (8 - 1); // mage
    robe.requiredLevel = 12;
    robe.bonding = game::kBindOnEquip;
    robe.description = "Smells of old books.";
    game::buildItemTooltip(robe, nullptr, c, t);
    CHECK(tooltipText(t) == std::vector<std::string>{"Robe of the Magi", "Binds when equipped", "Chest | Plate",
                                                     "48 Armor", "+7 Intellect", "-2 Stamina", "+5 Fire Resistance",
                                                     "Classes: Mage", "Requires Level 12", "\"Smells of old books.\"",
                                                     "Sell Price: 1200"});
    CHECK(t.lines()[0].leftColor == game::itemQualityColor(3));
    CHECK(t.lines()[2].rightColor == game::kTooltipRed); // plate: not proficient
    CHECK(t.lines()[7].leftColor == game::kTooltipRed);  // classes
    CHECK(t.lines()[8].leftColor == game::kTooltipRed);  // level 12 at 5
    CHECK(t.lines()[9].leftColor == game::kTooltipGold);

    // A bag, a stack's price, a soulbound and broken item, a book.
    net::ItemQueryInfo pouch = templateOf(kBagEntry, "Small Brown Pouch", 1, 0, 1, 25);
    pouch.containerSlots = 6;
    pouch.inventoryType = game::kInvTypeBag;
    game::buildItemTooltip(pouch, nullptr, c, t);
    CHECK(tooltipText(t) == std::vector<std::string>{"Small Brown Pouch", "6 Slot Bag", "Sell Price: 25"});
    net::ItemQueryInfo quiver = templateOf(kBagEntry, "Light Quiver", game::kItemClassQuiver, 2, 1, 1);
    quiver.containerSlots = 8;
    quiver.inventoryType = game::kInvTypeQuiver;
    game::buildItemTooltip(quiver, nullptr, c, t);
    CHECK(tooltipText(t)[1] == "8 Slot Quiver");
    net::ItemQueryInfo bread = templateOf(kBreadEntry, "Tough Hunk of Bread", 0, 0, 1, 1);
    bread.spells[0].id = 433;
    game::buildItemTooltip(bread, h.inv().find(kBread), c, t);
    CHECK(tooltipText(t).back() == "Sell Price: 5"); // the whole stack of 5
    h.apply(values(kSword, {{uf::ITEM_FIELD_DURABILITY, 0}, {uf::ITEM_FIELD_FLAGS, game::kItemDynFlagBound}}));
    game::buildItemTooltip(sword, h.inv().equipped(game::kSlotMainHand), c, t);
    CHECK(tooltipText(t)[1] == "Soulbound");
    const game::TooltipLine& durability = t.lines()[5];
    CHECK(durability.left == "Durability 0 / 20");
    CHECK(durability.leftColor == game::kTooltipRed);
    net::ItemQueryInfo book = templateOf(kBookEntry, "Old Book", 12, 0, 1, 0);
    book.pageText = 17;
    book.bonding = game::kBindQuest;
    book.startQuest = 123;
    game::buildItemTooltip(book, nullptr, c, t);
    CHECK(tooltipText(t) == std::vector<std::string>{"Old Book", "Quest Item", "This Item Begins a Quest",
                                                     "<Tap to Read>"});

    // Ammo; a ring with every resistance, an honor rank and a race; a two-handed fire weapon.
    net::ItemQueryInfo arrow = templateOf(2512, "Rough Arrow", game::kItemClassProjectile, 2, 1, 0);
    arrow.inventoryType = game::kInvTypeAmmo;
    arrow.damages[0] = {2.0f, 3.0f, 0};
    game::buildItemTooltip(arrow, nullptr, c, t);
    char ammo[64];
    CHECK(tooltipText(t) == std::vector<std::string>{"Rough Arrow", "Projectile | Arrow",
                                                     std::string(text::format(ammo, "AMMO_DAMAGE_TEMPLATE", 2.5f))});
    net::ItemQueryInfo ring = templateOf(5001, "Band of the Marshal", game::kItemClassArmor, 0, 4, 0);
    ring.inventoryType = game::kInvTypeFinger;
    ring.resistances = {0, 10, 10, 10, 10, 10};
    ring.requiredHonorRank = 11;
    ring.allowableRace = (1u << 0) | (1u << 2); // human, dwarf
    game::buildItemTooltip(ring, nullptr, c, t);
    CHECK(tooltipText(t) == std::vector<std::string>{"Band of the Marshal", "Finger", "+10 to All Resistances",
                                                     "Races: Human, Dwarf", "Requires Knight-Lieutenant"});
    c.race = 2; // an orc: the Horde's title, the race in red
    game::buildItemTooltip(ring, nullptr, c, t);
    CHECK(tooltipText(t)[3] == "Races: Human, Dwarf");
    CHECK(t.lines()[3].leftColor == game::kTooltipRed);
    CHECK(tooltipText(t)[4] == "Requires Blood Guard");
    c.race = 1;
    net::ItemQueryInfo staff = templateOf(5002, "Flaming Staff", game::kItemClassWeapon, 10, 2, 0);
    staff.inventoryType = game::kInvType2HWeapon;
    staff.damages[0] = {10.0f, 20.0f, 2};
    staff.damages[1] = {5.0f, 5.0f, 0};
    staff.delayMs = 3000;
    game::buildItemTooltip(staff, nullptr, c, t);
    CHECK(tooltipText(t) == std::vector<std::string>{"Flaming Staff", "Two-Hand | Staff",
                                                     "10 - 20 Fire Damage | Speed 3.00", "+ 5 Damage",
                                                     "(6.7 damage per second)"});

    // A set piece: its name with the pieces worn, the bonus grey until enough are worn.
    net::ItemQueryInfo arms = templateOf(12940, "Dal'Rend's Sacred Charge", 2, 7, 3, 20000);
    arms.inventoryType = game::kInvTypeWeaponMainHand;
    arms.itemSet = 41;
    game::buildItemTooltip(arms, nullptr, c, t);
    bool setLine = false, bonus = false;
    for (const game::TooltipLine& line : t.lines()) {
        setLine = setLine || (line.left == "Dal'Rend's Arms (0/2)" && line.leftColor == game::kTooltipGold);
        bonus = bonus || (line.left.rfind("(2) Set: ", 0) == 0 && line.leftColor == game::kTooltipGray);
    }
    CHECK(setLine);
    CHECK(bonus);
    CHECK(game::inventoryTypeName(game::kInvType2HWeapon) == "Two-Hand");
    CHECK(game::inventoryTypeName(game::kInvTypeThrown) == "Thrown");
    CHECK(game::inventoryTypeName(game::kInvTypeNonEquip).empty());
    CHECK(game::reputationRankName(7) == "Exalted");
    CHECK(game::reputationRankName(8).empty());
}

TEST_CASE("Item actions and drops: what a tap offers and where a carried item goes") {
    using game::ItemAction;
    using game::ItemPlace;
    using Kind = game::ItemPlace::Kind;
    Harness h;
    h.character();
    h.templates();
    std::array<ItemAction, 8> actions{};
    const auto offered = [&](const ItemPlace& at) {
        const u32 n = game::itemActions(h.play, at, actions);
        return std::vector<ItemAction>(actions.begin(), actions.begin() + n);
    };
    // Out of town: the equipped sword can be taken off or destroyed; the bread used, split, destroyed.
    CHECK(offered(ItemPlace::slot({255, game::kSlotMainHand})) ==
          std::vector<ItemAction>{ItemAction::Unequip, ItemAction::Destroy});
    net::ItemQueryInfo bread = templateOf(kBreadEntry, "Tough Hunk of Bread", 0, 0, 1, 1);
    bread.stackable = 20;
    bread.spells[0].id = 433;
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(bread)) == game::Gameplay::Handled::Yes);
    CHECK(offered(ItemPlace::slot({19, 2})) ==
          std::vector<ItemAction>{ItemAction::Use, ItemAction::Split, ItemAction::Destroy});
    CHECK(game::useLabel(h.play, ItemPlace::slot({19, 2})) == text::get("USE"));
    CHECK(offered(ItemPlace::slot({255, 25})).empty()); // nothing there

    // At a vendor that repairs: sell, repair the damaged sword; its rows buy.
    h.apply(created(kVendor, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_NPC_FLAGS, game::kNpcFlagVendor | game::kNpcFlagRepair}}));
    Bytes list;
    net::ByteWriter w(list);
    w.u64v(kVendor).u8v(1).u32v(1).u32v(kBreadEntry).u32v(0).u32v(net::kVendorUnlimited).u32v(25).u32v(0).u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_LIST_INVENTORY, list) == game::Gameplay::Handled::Yes);
    CHECK(offered(ItemPlace::slot({255, game::kSlotMainHand})) ==
          std::vector<ItemAction>{ItemAction::Unequip, ItemAction::Sell, ItemAction::Repair, ItemAction::Destroy});
    CHECK(offered(ItemPlace::of(Kind::Vendor, 0)) == std::vector<ItemAction>{ItemAction::Buy, ItemAction::BuyMany});
    h.sent.clear();
    REQUIRE(game::doItemAction(h.play, ItemPlace::slot({255, game::kSlotMainHand}), ItemAction::Repair));
    CHECK(h.last(Opcode::CMSG_REPAIR_ITEM) ==
          bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 0x01, 0x09, 0, 0, 0, 0, 0, 0x40}));
    REQUIRE(game::doItemAction(h.play, ItemPlace::of(Kind::Vendor, 0), ItemAction::Buy));
    CHECK(h.last(Opcode::CMSG_BUY_ITEM) == bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 0xBC, 0x11, 0, 0, 1, 0}));
    CHECK_FALSE(game::doItemAction(h.play, ItemPlace::slot({19, 2}), ItemAction::Split)); // asks first

    // Drops.
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({255, 23}), ItemPlace::slot({255, 30})));
    CHECK(h.last(Opcode::CMSG_SWAP_INV_ITEM) == bytes({23, 30}));
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({255, 23}), ItemPlace::slot({255, 19}))); // onto the worn bag
    CHECK(h.last(Opcode::CMSG_AUTOSTORE_BAG_ITEM) == bytes({255, 23, 19}));
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({255, 19}), ItemPlace::slot({255, 20}))); // a bag to a free bag slot
    CHECK(h.last(Opcode::CMSG_SWAP_INV_ITEM) == bytes({19, 20}));
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({19, 2}), ItemPlace::of(Kind::Backpack)));
    CHECK(h.last(Opcode::CMSG_AUTOSTORE_BAG_ITEM) == bytes({19, 2, 255}));
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({255, game::kSlotMainHand}), ItemPlace::of(Kind::Bags)));
    CHECK(h.last(Opcode::CMSG_AUTOSTORE_BAG_ITEM) == bytes({255, 15, 0}));
    CHECK_FALSE(game::dropItem(h.play, ItemPlace::slot({255, 23}), ItemPlace::of(Kind::Bags))); // already there
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({255, 23}), ItemPlace::of(Kind::Equip)));
    CHECK(h.last(Opcode::CMSG_AUTOEQUIP_ITEM) == bytes({255, 23}));
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({255, 23}), ItemPlace::of(Kind::Sell)));
    CHECK(h.last(Opcode::CMSG_SELL_ITEM).size() == 17);
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({19, 2}), ItemPlace::of(Kind::Vendor, 0))); // on a row: sell
    CHECK(h.last(Opcode::CMSG_SELL_ITEM).size() == 17);
    REQUIRE(game::dropItem(h.play, ItemPlace::of(Kind::Vendor, 0), ItemPlace::of(Kind::Bags))); // a row: buy
    CHECK(h.last(Opcode::CMSG_BUY_ITEM).size() == 14);
    // Into an empty cell: CMSG_BUY_ITEM_IN_SLOT with the bag named by GUID (the player's for the backpack).
    REQUIRE(game::dropItem(h.play, ItemPlace::of(Kind::Vendor, 0), ItemPlace::slot({255, 30})));
    CHECK(h.last(Opcode::CMSG_BUY_ITEM_IN_SLOT) ==
          bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 0xBC, 0x11, 0, 0, 0x11, 0, 0, 0, 0, 0, 0, 0, 30, 1}));
    REQUIRE(game::dropItem(h.play, ItemPlace::of(Kind::Vendor, 0), ItemPlace::slot({19, 0})));
    CHECK(h.last(Opcode::CMSG_BUY_ITEM_IN_SLOT) ==
          bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 0xBC, 0x11, 0, 0, 0x02, 0x09, 0, 0, 0, 0, 0, 0x40, 0, 1}));
    REQUIRE(game::dropItem(h.play, ItemPlace::of(Kind::Vendor, 0), ItemPlace::slot({19, 2}))); // taken: anywhere
    CHECK(h.last(Opcode::CMSG_BUY_ITEM).size() == 14);
    CHECK_FALSE(game::dropItem(h.play, ItemPlace::of(Kind::Vendor, 0), ItemPlace::of(Kind::Sell)));
    Bytes ammo;
    net::ItemQueryInfo arrow = templateOf(2512, "Rough Arrow", game::kItemClassProjectile, 2, 1, 0);
    arrow.inventoryType = game::kInvTypeAmmo;
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(arrow)) == game::Gameplay::Handled::Yes);
    h.apply(itemObject(kChest, 2512, 200));
    Fields f;
    slot(f, 26, kChest);
    h.apply(values(kPlayer, f));
    CHECK(offered(ItemPlace::slot({255, 26}))[0] == ItemAction::SetAmmo);
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({255, 26}), ItemPlace::of(Kind::Ammo)));
    CHECK(h.last(Opcode::CMSG_SET_AMMO) == bytes({0xD0, 0x09, 0, 0}));

    // At the bank: to the bank from the bags, back to the bags from it.
    h.inv().closeVendor();
    h.apply(created(kBanker, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_NPC_FLAGS, game::kNpcFlagBanker}}));
    REQUIRE(h.packet(Opcode::SMSG_SHOW_BANK, guidBody(kBanker)) == game::Gameplay::Handled::Yes);
    CHECK(offered(ItemPlace::slot({255, 39})) ==
          std::vector<ItemAction>{ItemAction::FromBank, ItemAction::Split, ItemAction::Destroy});
    const std::vector<ItemAction> carried = offered(ItemPlace::slot({19, 2}));
    CHECK(std::find(carried.begin(), carried.end(), ItemAction::ToBank) != carried.end());
    h.sent.clear();
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({19, 2}), ItemPlace::of(Kind::Bank)));
    CHECK(h.last(Opcode::CMSG_AUTOBANK_ITEM) == bytes({19, 2}));
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({255, 39}), ItemPlace::of(Kind::Bags)));
    CHECK(h.last(Opcode::CMSG_AUTOSTORE_BANK_ITEM) == bytes({255, 39}));
    REQUIRE(game::dropItem(h.play, ItemPlace::slot({255, 39}), ItemPlace::slot({19, 0}))); // by hand onto a bag cell
    CHECK(h.last(Opcode::CMSG_SWAP_ITEM) == bytes({19, 0, 255, 39}));
    CHECK(game::actionLabel(ItemAction::FromBank) == text::get("VD_INV_TO_BAGS"));
    if (test::loadTexts()) {
        CHECK(game::actionLabel(ItemAction::FromBank) == "To Bags");
        CHECK(game::actionLabel(ItemAction::Buyback) == "Buyback");
        CHECK(game::useLabel(h.play, ItemPlace::slot({255, 39})) == "Use");
    }
}

TEST_CASE("Inventory: rebuilt at once after a quiet spell, at most every 100 ms under a stream of updates") {
    Harness h;
    h.character();
    game::Inventory& inv = h.inv();
    const u32 version = inv.version();
    Fields move;
    slot(move, 23, 0);
    slot(move, 30, kJunk);
    // An update in the middle of a stream: not yet...
    h.store.applyUpdateObject(values(kPlayer, move), h.now);
    h.play.afterUpdate({}, h.now);
    inv.refresh(h.now + 50);
    CHECK(inv.version() == version);
    CHECK(inv.at({255, 23}));
    // ...100 ms after the last rebuild it is.
    inv.refresh(h.now + 100);
    CHECK(inv.version() != version);
    CHECK(inv.at({255, 30}));
    // After a quiet spell a change shows the next frame.
    Fields back;
    slot(back, 30, 0);
    slot(back, 23, kJunk);
    h.store.applyUpdateObject(values(kPlayer, back), h.now + 900);
    h.play.afterUpdate({}, h.now + 900);
    h.play.update(h.now + 900);
    CHECK(inv.at({255, 23}));
}
