// Item, vendor, bank and reading packets (phase 8): every parser against payloads written field by field as
// vmangos-core 4641790 writes them (build 5875), every strict prefix refused, and every builder against the exact
// bytes the server reads (Server/Packets/Item.cpp, Npc.cpp, Query.cpp, Misc.cpp, Spell.cpp).
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/ItemMessages.h"
#include "net/Messages.h"

#include <doctest/doctest.h>

#include <cstring>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace {

using namespace vd;
using namespace vd::net;

using Bytes = std::vector<u8>;

void checkTruncations(const Bytes& full, const std::function<bool(std::span<const u8>)>& parse) {
    for (usize length = 0; length < full.size(); ++length) {
        INFO("truncated to " << length << " of " << full.size() << " bytes");
        CHECK_FALSE(parse(std::span<const u8>(full.data(), length)));
    }
}

Bytes bytes(std::initializer_list<int> list) {
    Bytes out;
    for (int b : list) {
        out.push_back(static_cast<u8>(b));
    }
    return out;
}

constexpr u64 kVendor = 0xF130000000000123ull;
constexpr u64 kItem = 0x4000000000000888ull;
constexpr u64 kPlayer = 0x11;

// HandleItemQuerySingleOpcode for build 5875, every field in the order the server writes it. Values chosen so a
// field read from the wrong offset shows: a two-handed sword with stats, a fire damage range, an on-equip and a
// chance-on-hit spell, a set and durability.
Bytes itemTemplate() {
    Bytes out;
    ByteWriter w(out);
    w.u32v(1000).u32v(2).u32v(8).cstring("Test Greatsword").cstring("").cstring("").cstring("");
    w.u32v(5000).u32v(3).u32v(0x20).u32v(12345).u32v(2469).u32v(17); // display, quality, flags, buy, sell, 2H
    w.u32v(0x7FFF).u32v(0x1FF).u32v(45).u32v(40);                    // classes, races, item level, level
    w.u32v(43).u32v(150).u32v(0).u32v(3).u32v(0).u32v(72).u32v(4).u32v(1); // skill, rank, spell, honor, city,
                                                                           // faction, rank, max count
    w.u32v(1).u32v(0);                                                     // stackable, container slots
    w.u32v(4).u32v(12).u32v(7).u32v(static_cast<u32>(-3));                 // +12 strength, -3 stamina
    for (u32 i = 2; i < 10; ++i) {
        w.u32v(0).u32v(0);
    }
    w.f32v(80.0f).f32v(121.0f).u32v(0).f32v(5.0f).f32v(10.0f).u32v(2); // physical, fire
    for (u32 i = 2; i < 5; ++i) {
        w.f32v(0.0f).f32v(0.0f).u32v(0);
    }
    w.u32v(150).u32v(0).u32v(10).u32v(0).u32v(0).u32v(5).u32v(0); // armor, holy, fire, nature, frost, shadow, arcane
    w.u32v(3400).u32v(0).f32v(0.0f);                             // delay, ammo, ranged modifier
    w.u32v(9139).u32v(1).u32v(0).u32v(static_cast<u32>(-1)).u32v(0).u32v(static_cast<u32>(-1)); // equip: crit
    w.u32v(18833).u32v(2).u32v(static_cast<u32>(-5)).u32v(1000).u32v(4).u32v(3000);           // chance on hit
    for (u32 i = 2; i < 5; ++i) {
        w.u32v(0).u32v(0).u32v(0).u32v(static_cast<u32>(-1)).u32v(0).u32v(static_cast<u32>(-1));
    }
    w.u32v(2).cstring("Sharp.").u32v(17).u32v(7).u32v(2); // bonding, description, page, language, material
    w.u32v(321).u32v(5).u32v(1).u32v(1);                  // quest, lock, material, sheath
    w.u32v(27).u32v(0).u32v(41).u32v(100).u32v(1519).u32v(0).u32v(9); // random property, block, set, durability,
                                                                      // area, map, bag family
    return out;
}

} // namespace

TEST_CASE("Item template: the whole SMSG_ITEM_QUERY_SINGLE_RESPONSE") {
    const Bytes full = itemTemplate();
    const std::optional<ItemQueryInfo> info = parseItemQueryResponse(full);
    REQUIRE(info.has_value());
    const ItemQueryInfo& i = *info;
    CHECK(i.found);
    CHECK(i.entry == 1000);
    CHECK(i.itemClass == 2);
    CHECK(i.subclass == 8);
    CHECK(i.name == "Test Greatsword");
    CHECK(i.displayId == 5000);
    CHECK(i.quality == 3);
    CHECK(i.flags == 0x20);
    CHECK(i.buyPrice == 12345);
    CHECK(i.sellPrice == 2469);
    CHECK(i.inventoryType == 17);
    CHECK(i.allowableClass == 0x7FFF);
    CHECK(i.allowableRace == 0x1FF);
    CHECK(i.itemLevel == 45);
    CHECK(i.requiredLevel == 40);
    CHECK(i.requiredSkill == 43);
    CHECK(i.requiredSkillRank == 150);
    CHECK(i.requiredSpell == 0);
    CHECK(i.requiredHonorRank == 3);
    CHECK(i.requiredCityRank == 0);
    CHECK(i.requiredReputationFaction == 72);
    CHECK(i.requiredReputationRank == 4);
    CHECK(i.maxCount == 1);
    CHECK(i.stackable == 1);
    CHECK(i.containerSlots == 0);
    CHECK(i.stats[0].type == 4);
    CHECK(i.stats[0].value == 12);
    CHECK(i.stats[1].type == 7);
    CHECK(i.stats[1].value == -3);
    CHECK(i.stats[2].type == 0);
    CHECK(i.damages[0].min == doctest::Approx(80.0f));
    CHECK(i.damages[0].max == doctest::Approx(121.0f));
    CHECK(i.damages[1].school == 2);
    CHECK(i.damages[1].max == doctest::Approx(10.0f));
    CHECK(i.armor == 150);
    CHECK(i.resistances[1] == 10); // fire
    CHECK(i.resistances[4] == 5);  // shadow
    CHECK(i.delayMs == 3400);
    CHECK(i.spells[0].id == 9139);
    CHECK(i.spells[0].trigger == kItemSpellTriggerOnEquip);
    CHECK(i.spells[0].cooldownMs == -1);
    CHECK(i.spells[1].id == 18833);
    CHECK(i.spells[1].trigger == kItemSpellTriggerChanceOnHit);
    CHECK(i.spells[1].charges == -5);
    CHECK(i.spells[1].cooldownMs == 1000);
    CHECK(i.spells[1].category == 4);
    CHECK(i.spells[1].categoryCooldownMs == 3000);
    CHECK(i.spells[4].id == 0);
    CHECK(i.bonding == 2);
    CHECK(i.description == "Sharp.");
    CHECK(i.pageText == 17);
    CHECK(i.language == 7);
    CHECK(i.pageMaterial == 2);
    CHECK(i.startQuest == 321);
    CHECK(i.lockId == 5);
    CHECK(i.material == 1);
    CHECK(i.sheath == 1);
    CHECK(i.randomProperty == 27);
    CHECK(i.block == 0);
    CHECK(i.itemSet == 41);
    CHECK(i.maxDurability == 100);
    CHECK(i.area == 1519);
    CHECK(i.map == 0);
    CHECK(i.bagFamily == 9);
    for (usize length = 0; length < full.size(); ++length) {
        INFO("truncated to " << length);
        CHECK_FALSE(parseItemQueryResponse(std::span<const u8>(full.data(), length)).has_value());
    }
}

TEST_CASE("Vendors: SMSG_LIST_INVENTORY, BUY_ITEM, BUY_FAILED, SELL_ITEM") {
    // SendListInventory: GUID, count, then 7 words per visible row (numbered from 1).
    Bytes list;
    ByteWriter w(list);
    w.u64v(kVendor).u8v(2);
    w.u32v(1).u32v(159).u32v(18084).u32v(kVendorUnlimited).u32v(25).u32v(0).u32v(1);
    w.u32v(2).u32v(2512).u32v(5996).u32v(3).u32v(10).u32v(0).u32v(200);
    VendorInventory v;
    REQUIRE(parseListInventory(list, v));
    CHECK(v.vendor == kVendor);
    REQUIRE(v.count == 2);
    CHECK(v.list()[0].index == 1);
    CHECK(v.list()[0].itemId == 159);
    CHECK(v.list()[0].stock == kVendorUnlimited);
    CHECK(v.list()[0].price == 25);
    CHECK(v.list()[1].itemId == 2512);
    CHECK(v.list()[1].displayId == 5996);
    CHECK(v.list()[1].stock == 3);
    CHECK(v.list()[1].buyCount == 200);
    checkTruncations(list, [](auto p) {
        VendorInventory m;
        return parseListInventory(p, m);
    });
    // The empty form: count 0, then the error byte.
    Bytes empty;
    ByteWriter(empty).u64v(kVendor).u8v(0).u8v(0);
    REQUIRE(parseListInventory(empty, v));
    CHECK(v.count == 0);
    CHECK(v.error == 0);
    checkTruncations(empty, [](auto p) {
        VendorInventory m;
        return parseListInventory(p, m);
    });
    // More rows than the server ever sends: malformed.
    Bytes tooMany;
    ByteWriter(tooMany).u64v(kVendor).u8v(129);
    CHECK_FALSE(parseListInventory(tooMany, v));

    Bytes bought;
    ByteWriter(bought).u64v(kVendor).u32v(2).u32v(2).u32v(1);
    BuyItemResult b;
    REQUIRE(parseBuyItem(bought, b));
    CHECK(b.vendor == kVendor);
    CHECK(b.vendorSlot == 2);
    CHECK(b.newStock == 2);
    CHECK(b.count == 1);
    checkTruncations(bought, [](auto p) {
        BuyItemResult m;
        return parseBuyItem(p, m);
    });

    Bytes failed;
    ByteWriter(failed).u64v(kVendor).u32v(2512).u8v(kBuyErrNotEnoughMoney);
    BuyFailed f;
    REQUIRE(parseBuyFailed(failed, f));
    CHECK(f.itemId == 2512);
    CHECK(f.reason == kBuyErrNotEnoughMoney);
    CHECK(std::string(buyResultName(f.reason)) == "NOT_ENOUGHT_MONEY");
    checkTruncations(failed, [](auto p) {
        BuyFailed m;
        return parseBuyFailed(p, m);
    });

    Bytes sold;
    ByteWriter(sold).u64v(kVendor).u64v(kItem).u8v(kSellErrCantSellItem);
    SellItemResult s;
    REQUIRE(parseSellItem(sold, s));
    CHECK(s.item == kItem);
    CHECK(s.reason == kSellErrCantSellItem);
    CHECK(std::string(sellResultName(s.reason)) == "CANT_SELL_ITEM");
    checkTruncations(sold, [](auto p) {
        SellItemResult m;
        return parseSellItem(p, m);
    });
    CHECK(std::string(bankSlotResultName(kBankSlotInsufficientFunds)) == "INSUFFICIENT_FUNDS");
    CHECK(std::string(buyResultName(99)) == "UNKNOWN");
}

TEST_CASE("Reading, cooldowns, proficiencies, enchantment timers") {
    Bytes ok;
    ByteWriter(ok).u64v(kItem).u64v(kItem);
    u64 item = 0;
    REQUIRE(parseReadItemOk(ok, item));
    CHECK(item == kItem);
    checkTruncations(ok, [](auto p) {
        u64 g;
        return parseReadItemOk(p, g);
    });
    Bytes failed;
    ByteWriter(failed).u64v(kItem).u8v(3).u64v(kItem);
    u8 reason = 0;
    REQUIRE(parseReadItemFailed(failed, item, reason));
    CHECK(reason == 3);
    checkTruncations(failed, [](auto p) {
        u64 g;
        u8 r;
        return parseReadItemFailed(p, g, r);
    });

    Bytes page;
    ByteWriter(page).u32v(17).cstring("Chapter one.$B$BIt was dark.").u32v(18);
    PageText t;
    REQUIRE(parsePageTextResponse(page, t));
    CHECK(t.pageId == 17);
    CHECK(t.text == "Chapter one.$B$BIt was dark.");
    CHECK(t.nextPageId == 18);
    checkTruncations(page, [](auto p) {
        PageText m;
        return parsePageTextResponse(p, m);
    });

    Bytes letter;
    ByteWriter(letter).u32v(55).cstring("Dear Arthel,");
    u32 textId = 0;
    std::string_view text;
    REQUIRE(parseItemTextResponse(letter, textId, text));
    CHECK(textId == 55);
    CHECK(text == "Dear Arthel,");
    checkTruncations(letter, [](auto p) {
        u32 id;
        std::string_view s;
        return parseItemTextResponse(p, id, s);
    });

    Bytes cooldown;
    ByteWriter(cooldown).u64v(kItem).u32v(8690);
    u32 spell = 0;
    REQUIRE(parseItemCooldown(cooldown, item, spell));
    CHECK(spell == 8690);
    checkTruncations(cooldown, [](auto p) {
        u64 g;
        u32 s;
        return parseItemCooldown(p, g, s);
    });

    Bytes proficiency;
    ByteWriter(proficiency).u8v(4).u32v(0x0F);
    u8 itemClass = 0;
    u32 mask = 0;
    REQUIRE(parseSetProficiency(proficiency, itemClass, mask));
    CHECK(itemClass == 4);
    CHECK(mask == 0x0F);
    checkTruncations(proficiency, [](auto p) {
        u8 c;
        u32 m;
        return parseSetProficiency(p, c, m);
    });

    Bytes enchant;
    ByteWriter(enchant).u64v(kItem).u32v(1).u32v(1800).u64v(kPlayer);
    ItemEnchantTimeUpdate e;
    REQUIRE(parseItemEnchantTimeUpdate(enchant, e));
    CHECK(e.slot == 1);
    CHECK(e.durationS == 1800);
    CHECK(e.player == kPlayer);
    checkTruncations(enchant, [](auto p) {
        ItemEnchantTimeUpdate m;
        return parseItemEnchantTimeUpdate(p, m);
    });
}

TEST_CASE("Item client packets: byte-exact bodies") {
    Bytes out;
    writeSwapInvItem(out, 23, 4); // SwapInvItem: srcslot, dstslot
    CHECK(out == bytes({23, 4}));
    out.clear();
    writeSwapItem(out, 19, 3, 255, 30); // SwapItem: dstbag, dstslot, srcbag, srcslot
    CHECK(out == bytes({19, 3, 255, 30}));
    out.clear();
    writeBagSlot(out, 255, 24); // AutoEquipItem / ReadItem / OpenItem / AutoBankItem / AutoStoreBankItem
    CHECK(out == bytes({255, 24}));
    out.clear();
    writeAutoStoreBagItem(out, 255, 25, 20);
    CHECK(out == bytes({255, 25, 20}));
    out.clear();
    writeSplitItem(out, 255, 23, 19, 0, 5);
    CHECK(out == bytes({255, 23, 19, 0, 5}));
    out.clear();
    writeDestroyItem(out, 20, 7, 0); // bag, slot, count, then 3 bytes the server skips
    CHECK(out == bytes({20, 7, 0, 0, 0, 0}));
    out.clear();
    writeAutoEquipItemSlot(out, kItem, 15);
    CHECK(out == bytes({0x88, 0x08, 0, 0, 0, 0, 0, 0x40, 15}));
    out.clear();
    writeBuyItem(out, kVendor, 159, 5); // vendor, item, count, unk
    CHECK(out == bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 159, 0, 0, 0, 5, 0}));
    out.clear();
    writeBuyItemInSlot(out, kVendor, 159, kPlayer, 30, 1); // vendor, item, bag GUID, bag slot, count
    CHECK(out == bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 159, 0, 0, 0, 0x11, 0, 0, 0, 0, 0, 0, 0, 30, 1}));
    out.clear();
    writeSellItem(out, kVendor, kItem, 0);
    CHECK(out ==
          bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 0x88, 0x08, 0, 0, 0, 0, 0, 0x40, 0}));
    out.clear();
    writeBuybackItem(out, kVendor, 69); // vendor, slot (u32, 69..80)
    CHECK(out == bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 69, 0, 0, 0}));
    out.clear();
    writeRepairItem(out, kVendor, 0); // all
    CHECK(out == bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1, 0, 0, 0, 0, 0, 0, 0, 0}));
    out.clear();
    writePageTextQuery(out, 17, kItem);
    CHECK(out == bytes({17, 0, 0, 0, 0x88, 0x08, 0, 0, 0, 0, 0, 0x40}));
    out.clear();
    writeItemTextQuery(out, 55);
    CHECK(out == bytes({55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    out.clear();
    writeGuid(out, kVendor); // CMSG_LIST_INVENTORY / CMSG_BANKER_ACTIVATE / CMSG_BUY_BANK_SLOT
    CHECK(out == bytes({0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1}));
}
