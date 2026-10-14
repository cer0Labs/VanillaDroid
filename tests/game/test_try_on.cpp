// Phase 15 (items): what an item is compared with (ShoppingTooltip1 / 2), the dressing room's TryOn rules
// (WoW.exe 0x504540 and its weapon pairing 0x504BC0), a player's look from its update fields, the master
// looter's candidates as GetMasterLootCandidate numbers them (WoW.exe 0x61C550) and SMSG_OPEN_CONTAINER's
// BAG_OPEN, through game::Gameplay.
#include "game/Entities.h"
#include "game/GameEvents.h"
#include "game/Gameplay.h"
#include "game/ItemCompare.h"
#include "game/ItemDefs.h"
#include "game/MasterLoot.h"
#include "game/Queries.h"
#include "game/Social.h"
#include "game/TryOn.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <vector>

namespace {

using namespace vd;
using namespace vd::game;
namespace uf = net::uf;

CompareSlots slots(u32 type, u32 offhand = 0) {
    return compareSlots(type, offhand);
}

TryOnItem item(u32 display, u8 type) {
    return {display, type, 0};
}

net::ItemQueryInfo itemInfo(u32 entry, u32 display, u32 type) {
    net::ItemQueryInfo i;
    i.entry = entry;
    i.found = true;
    i.name = "Item";
    i.displayId = display;
    i.inventoryType = type;
    return i;
}

} // namespace

TEST_CASE("Item comparison: the slots ShoppingTooltip1 and 2 show") {
    CHECK(slots(kInvTypeHead).count == 1);
    CHECK(slots(kInvTypeHead).slot[0] == kSlotHead);
    CHECK(slots(kInvTypeRobe).slot[0] == kSlotChest); // robes and chest pieces share the chest
    CHECK(slots(kInvTypeCloak).slot[0] == kSlotBack);
    // Both fingers, both trinkets.
    const CompareSlots ring = slots(kInvTypeFinger);
    REQUIRE(ring.count == 2);
    CHECK(ring.slot[0] == kSlotFinger1);
    CHECK(ring.slot[1] == kSlotFinger2);
    CHECK(slots(kInvTypeTrinket).count == 2);
    // A one-hand weapon: the main hand, and the off hand only when it holds a weapon (dual wielding).
    CHECK(slots(kInvTypeWeapon).count == 1);
    CHECK(slots(kInvTypeWeapon, kInvTypeShield).count == 1);
    CHECK(slots(kInvTypeWeapon, kInvTypeWeaponOffHand).count == 2);
    CHECK(slots(kInvTypeWeapon, kInvTypeWeapon).slot[1] == kSlotOffHand);
    // A two-hander: the main hand and whatever the off hand holds.
    CHECK(slots(kInvType2HWeapon).count == 1);
    CHECK(slots(kInvType2HWeapon, kInvTypeHoldable).count == 2);
    // The off hand's items, the ranged slot's.
    CHECK(slots(kInvTypeShield).slot[0] == kSlotOffHand);
    CHECK(slots(kInvTypeHoldable).slot[0] == kSlotOffHand);
    CHECK(slots(kInvTypeRangedRight).slot[0] == kSlotRanged);
    CHECK(slots(kInvTypeThrown).slot[0] == kSlotRanged);
    // Nothing for what is not compared.
    CHECK(slots(kInvTypeNonEquip).count == 0);
    CHECK(slots(kInvTypeBag).count == 0);
    CHECK(slots(kInvTypeAmmo).count == 0);
    CHECK(slots(kInvTypeQuiver).count == 0);
    CHECK(slots(kInvTypeRelic).count == 0);
}

TEST_CASE("Dressing room: what TryOn puts on") {
    CHECK(canTryOn(kInvTypeHead));
    CHECK(canTryOn(kInvTypeTabard));
    CHECK(canTryOn(kInvTypeHoldable));
    CHECK_FALSE(canTryOn(kInvTypeNeck));
    CHECK_FALSE(canTryOn(kInvTypeFinger));
    CHECK_FALSE(canTryOn(kInvTypeTrinket));
    CHECK_FALSE(canTryOn(kInvTypeBag));
    CHECK_FALSE(canTryOn(kInvTypeAmmo));

    CharacterLook look;
    CHECK(tryOn(look, item(100, kInvTypeChest), false));
    CHECK(look.items[kSlotChest] == 100);
    CHECK(tryOn(look, item(101, kInvTypeRobe), false));
    CHECK(look.items[kSlotChest] == 101);
    CHECK(look.inventoryTypes[kSlotChest] == kInvTypeRobe);
    CHECK(tryOn(look, item(102, kInvTypeCloak), false));
    CHECK(look.items[kSlotBack] == 102);
    // Rings change nothing; nor does an item without a display.
    const CharacterLook before = look;
    CHECK_FALSE(tryOn(look, item(103, kInvTypeFinger), false));
    CHECK_FALSE(tryOn(look, item(0, kInvTypeHead), false));
    CHECK(look == before);
}

TEST_CASE("Dressing room: weapons go to the hands WoW.exe gives them") {
    SUBCASE("a one-hand weapon with a shield keeps the shield") {
        CharacterLook look;
        tryOn(look, item(200, kInvTypeShield), false);
        tryOn(look, item(201, kInvTypeWeapon), false);
        CHECK(look.items[kSlotMainHand] == 201);
        CHECK(look.items[kSlotOffHand] == 200);
    }
    SUBCASE("a second one-hand weapon: to the off hand when dual wielding, else it replaces the first") {
        CharacterLook dual;
        tryOn(dual, item(300, kInvTypeWeapon), true);
        tryOn(dual, item(301, kInvTypeWeapon), true);
        CHECK(dual.items[kSlotMainHand] == 300);
        CHECK(dual.items[kSlotOffHand] == 301);
        CharacterLook single;
        tryOn(single, item(300, kInvTypeWeapon), false);
        tryOn(single, item(301, kInvTypeWeapon), false);
        CHECK(single.items[kSlotMainHand] == 301);
        CHECK(single.items[kSlotOffHand] == 0);
    }
    SUBCASE("a two-hander takes off what the off hand holds; a shield then takes off the two-hander") {
        CharacterLook look;
        tryOn(look, item(400, kInvTypeHoldable), false);
        tryOn(look, item(401, kInvType2HWeapon), false);
        CHECK(look.items[kSlotMainHand] == 401);
        CHECK(look.items[kSlotOffHand] == 0);
        tryOn(look, item(402, kInvTypeShield), false);
        CHECK(look.items[kSlotMainHand] == 0);
        CHECK(look.items[kSlotOffHand] == 402);
    }
    SUBCASE("ranged weapons: a bow in the off (left) hand, a gun in the main (right) hand, alone") {
        CharacterLook look;
        tryOn(look, item(500, kInvTypeWeapon), false);
        tryOn(look, item(501, kInvTypeRanged), false);
        CHECK(look.items[kSlotMainHand] == 0);
        CHECK(look.items[kSlotOffHand] == 501);
        tryOn(look, item(502, kInvTypeRangedRight), false);
        CHECK(look.items[kSlotMainHand] == 502);
        CHECK(look.items[kSlotOffHand] == 0);
    }
    SUBCASE("an off-hand weapon needs dual wield to stay with a main-hand one") {
        CharacterLook look;
        tryOn(look, item(600, kInvTypeWeaponMainHand), false);
        tryOn(look, item(601, kInvTypeWeaponOffHand), false);
        CHECK(look.items[kSlotMainHand] == 0);
        CHECK(look.items[kSlotOffHand] == 601);
        CharacterLook dual;
        tryOn(dual, item(600, kInvTypeWeaponMainHand), true);
        tryOn(dual, item(601, kInvTypeWeaponOffHand), true);
        CHECK(dual.items[kSlotMainHand] == 600);
        CHECK(dual.items[kSlotOffHand] == 601);
    }
}

TEST_CASE("Dressing room: a player's look from its update fields") {
    Entity player;
    player.guid = 0x11;
    player.type = ObjectType::Player;
    player.values.assign(uf::PLAYER_END, 0);
    player.values[uf::UNIT_FIELD_BYTES_0] = 4u | (1u << 8) | (1u << 16); // night elf warrior, female
    player.values[uf::PLAYER_BYTES] = 3u | (2u << 8) | (5u << 16) | (6u << 24);
    player.values[uf::PLAYER_BYTES_2] = 1;
    constexpr u32 kStride = uf::PLAYER_VISIBLE_ITEM_2_0 - uf::PLAYER_VISIBLE_ITEM_1_0;
    player.values[uf::PLAYER_VISIBLE_ITEM_1_0 + kSlotChest * kStride] = 2000;
    player.values[uf::PLAYER_VISIBLE_ITEM_1_0 + kSlotMainHand * kStride] = 2001;
    QueryCache queries;
    CharacterLook look;
    CHECK_FALSE(characterLookOf(player, queries, look)); // the items' templates are not known yet
    CHECK(look.race == 4);
    CHECK(look.gender == 1);
    CHECK(look.skin == 3);
    CHECK(look.face == 2);
    CHECK(look.hairStyle == 5);
    CHECK(look.hairColor == 6);
    CHECK(look.facialHair == 1);
    queries.onItem(itemInfo(2000, 7000, kInvTypeChest));
    queries.onItem(itemInfo(2001, 7001, kInvTypeWeapon));
    CHECK(characterLookOf(player, queries, look));
    CHECK(look.items[kSlotChest] == 7000);
    CHECK(look.items[kSlotMainHand] == 7001);
    CHECK(look.inventoryTypes[kSlotMainHand] == kInvTypeWeapon);
}

TEST_CASE("Master loot: GetMasterLootCandidate's numbering") {
    GroupState party;
    party.active = true;
    const std::vector<u64> list = {0x21, 0x11, 0x22};
    const MasterLootSlots a = masterLootSlots(list, party, 0x11);
    CHECK_FALSE(a.raid);
    CHECK(a.guid[0] == 0x21); // a party: the list's order
    CHECK(a.guid[1] == 0x11);
    CHECK(a.guid[2] == 0x22);

    GroupState raid;
    raid.active = true;
    raid.raid = true;
    raid.ownSubgroup = 1;
    raid.members.push_back({0x21, "A", 0, 0});
    raid.members.push_back({0x22, "B", 0, 2});
    raid.members.push_back({0x23, "C", 0, 1});
    const std::vector<u64> raidList = {0x22, 0x11, 0x23, 0x21, 0x99}; // 0x99: not in the raid
    const MasterLootSlots b = masterLootSlots(raidList, raid, 0x11);
    CHECK(b.raid);
    CHECK(b.guid[0] == 0x21);  // group 1
    CHECK(b.guid[5] == 0x11);  // group 2: the player first (it came first in the list)...
    CHECK(b.guid[6] == 0x23);  // ...then C
    CHECK(b.guid[10] == 0x22); // group 3
    CHECK(b.groupHasCandidates(0));
    CHECK(b.groupHasCandidates(1));
    CHECK(b.groupHasCandidates(2));
    CHECK_FALSE(b.groupHasCandidates(3));
    for (const u64 g : b.guid) {
        CHECK(g != 0x99);
    }
}

TEST_CASE("BAG_OPEN: SMSG_OPEN_CONTAINER tells the screens which bag to open") {
    EntityStore store;
    Gameplay play{store};
    play.enterWorld(0x11, 0, true, 1000);
    u32 cursor = play.events().end();
    std::vector<u8> body;
    net::writeGuid(body, 0x4000000000000902ull);
    CHECK(play.handle(net::Opcode::SMSG_OPEN_CONTAINER, body, 1000) == Gameplay::Handled::Yes);
    bool found = false;
    while (const GameEvent* e = play.events().next(cursor)) {
        if (e->kind == GameEventKind::ContainerOpened) {
            found = true;
            CHECK(e->guid == 0x4000000000000902ull);
        }
    }
    CHECK(found);
    // A truncated packet is rejected.
    body.resize(4);
    CHECK(play.handle(net::Opcode::SMSG_OPEN_CONTAINER, body, 1000) == Gameplay::Handled::Malformed);
}
