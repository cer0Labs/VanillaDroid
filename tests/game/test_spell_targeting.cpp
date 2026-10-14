// Phase 13 (spells): spells aimed at a place or an item and the spell modifiers. The flows go end to end
// through game::Gameplay with a capturing packet sink: server packets as vMaNGOS writes them
// (Player::SendSpellMod), the client's casts byte for byte as SpellCastTargets::read and HandleUseItemOpcode
// read them. The rules are checked against the real Spell.dbc when VD_PACKS_DIR is set.
#include "TestData.h"
#include "core/Text.h"
#include "game/Entities.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "game/ItemDefs.h"
#include "game/SpellMods.h"
#include "game/SpellTargeting.h"
#include "game/SpellText.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/SpellMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using game::GameEventKind;
using game::SpellModOp;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x21;
constexpr u64 kPoison = 0x4000000000000101ull;  // Instant Poison in the backpack's first slot
constexpr u64 kDagger = 0x4000000000000102ull;  // a dagger in the main hand
constexpr u64 kRobe = 0x4000000000000103ull;    // a cloth chest in the backpack's second slot
constexpr u64 kLockbox = 0x4000000000000104ull; // a locked box in the third
constexpr u64 kChest = 0xF110000000000777ull;
constexpr u32 kPoisonEntry = 6947, kDaggerEntry = 2092, kRobeEntry = 2470, kLockboxEntry = 4632;
constexpr u32 kBlizzard = 10, kInstantPoison = 8679, kPickLock = 1804, kFireball = 133;

u32 low(u64 guid) {
    return static_cast<u32>(guid);
}
u32 high(u64 guid) {
    return static_cast<u32>(guid >> 32);
}
u32 floatBits(f32 value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
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

Bytes created(u64 guid, game::ObjectType type, const Fields& fields, f32 x = 0.0f, f32 y = 0.0f) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(x).f32v(y).f32v(0.0f).f32v(0.0f);
    writeValues(w, fields);
    return out;
}

Bytes itemObject(u64 guid, u32 entry, u32 flags = 0, u32 tempEnchant = 0) {
    return created(guid, game::ObjectType::Item,
                   {{uf::OBJECT_FIELD_ENTRY, entry},
                    {uf::ITEM_FIELD_OWNER, low(kPlayer)},
                    {uf::ITEM_FIELD_STACK_COUNT, 1},
                    {uf::ITEM_FIELD_FLAGS, flags},
                    {static_cast<u16>(uf::ITEM_FIELD_ENCHANTMENT + 3), tempEnchant}});
}

void slot(Fields& f, u8 slotIndex, u64 guid) {
    f.push_back({static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slotIndex * 2), low(guid)});
    f.push_back({static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slotIndex * 2 + 1), high(guid)});
}

// HandleItemQuerySingleOpcode's layout with the fields these tests need.
struct ItemTemplate {
    u32 entry = 0;
    u32 itemClass = 0, subclass = 0, quality = 1, inventoryType = 0, itemLevel = 1;
    u32 spell = 0; // its on-use spell
    u32 lockId = 0;
};
Bytes itemQuery(const ItemTemplate& t) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(t.entry).u32v(t.itemClass).u32v(t.subclass).cstring("Item").cstring("").cstring("").cstring("");
    w.u32v(1000).u32v(t.quality).u32v(0).u32v(0).u32v(0).u32v(t.inventoryType);
    w.u32v(0xFFFFFFFFu).u32v(0xFFFFFFFFu).u32v(t.itemLevel).u32v(0);
    for (u32 i = 0; i < 8; ++i) {
        w.u32v(0);
    }
    w.u32v(1).u32v(0);
    for (u32 i = 0; i < 10; ++i) {
        w.u32v(0).u32v(0);
    }
    for (u32 i = 0; i < 5; ++i) {
        w.f32v(0.0f).f32v(0.0f).u32v(0);
    }
    for (u32 i = 0; i < 7; ++i) {
        w.u32v(0);
    }
    w.u32v(0).u32v(0).f32v(0.0f);
    for (u32 i = 0; i < 5; ++i) {
        if (i == 0 && t.spell != 0) {
            w.u32v(t.spell).u32v(0).u32v(0xFFFFFFFFu).u32v(0xFFFFFFFFu).u32v(0).u32v(0xFFFFFFFFu);
        } else {
            w.u32v(0).u32v(0).u32v(0).u32v(0xFFFFFFFFu).u32v(0).u32v(0xFFFFFFFFu);
        }
    }
    w.u32v(0).cstring("").u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(t.lockId).u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    return out;
}

// A table of spells.
struct SpellTable {
    std::vector<game::SpellInfo> spells;
    game::SpellSource source() const {
        game::SpellSource s;
        s.context = this;
        s.lookup = [](const void* context, u32 id, game::SpellInfo& out) {
            for (const game::SpellInfo& info : static_cast<const SpellTable*>(context)->spells) {
                if (info.id == id) {
                    out = info;
                    return true;
                }
            }
            return false;
        };
        return s;
    }
};

// What these tests know of Spell.dbc (1.12.1 values).
game::SpellInfo blizzard() {
    game::SpellInfo s;
    s.id = kBlizzard;
    s.targets = 0x40; // TARGET_FLAG_DEST_LOCATION
    s.effects[0] = 27;
    s.implicitTargetA[0] = 28;
    s.effectRadius[0] = 8.0f;
    s.maxRange = 30.0f;
    s.gcdCategory = 133;
    s.gcdMs = 1500;
    s.spellFamily = 3;
    s.familyFlags = 0x80080;
    return s;
}
game::SpellInfo instantPoison() {
    game::SpellInfo s;
    s.id = kInstantPoison;
    s.targets = 0x10;  // TARGET_FLAG_ITEM
    s.effects[0] = 54; // ENCHANT_ITEM_TEMPORARY
    s.effectMisc[0] = 323;
    s.equippedItemClass = 2;
    s.equippedItemSubClassMask = 0x2a5f3;
    return s;
}
game::SpellInfo pickLock() {
    game::SpellInfo s;
    s.id = kPickLock;
    s.effects[0] = 33;         // OPEN_LOCK
    s.implicitTargetA[0] = 26; // TARGET_LOCKED
    s.effectMisc[0] = 1;
    return s;
}
game::SpellInfo fireball() {
    game::SpellInfo s;
    s.id = kFireball;
    s.effects[0] = 2;
    s.implicitTargetA[0] = 6;
    s.castTimeMs = 1500;
    s.maxRange = 35.0f;
    s.powerCost = 30;
    s.spellFamily = 3;
    s.familyFlags = 0x40000001;
    return s;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    SpellTable table;
    std::vector<Sent> sent;
    std::vector<u64> createdGuids;
    u32 now = 1000;
    u32 cursor = 0;

    explicit Harness(bool enter = true) {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        table.spells = {blizzard(), instantPoison(), pickLock(), fireball()};
        play.setSpellSource(table.source());
        if (enter) {
            play.enterWorld(kPlayer, 0, true, now);
        }
    }
    void apply(const Bytes& update) {
        createdGuids.clear();
        game::UpdateStats stats;
        stats.createdGuids = &createdGuids;
        REQUIRE(store.applyUpdateObject(update, now, &stats));
        play.afterUpdate(createdGuids, now);
        play.update(now);
        play.inventory().refresh(now, true);
    }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
    }
    std::vector<Bytes> take(Opcode opcode) {
        std::vector<Bytes> out;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out.push_back(s.body);
            }
        }
        sent.clear();
        return out;
    }
    std::vector<game::GameEvent> events() {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            out.push_back(*e);
        }
        return out;
    }
    static const game::GameEvent* find(const std::vector<game::GameEvent>& list, GameEventKind kind) {
        for (const game::GameEvent& e : list) {
            if (e.kind == kind) {
                return &e;
            }
        }
        return nullptr;
    }
    // A level-30 mage at the origin with Instant Poison and a cloth robe in the backpack, a dagger in the
    // main hand and a locked box; the templates known.
    void character(u32 daggerTempEnchant = 0, u32 daggerFlags = 0) {
        apply(itemObject(kPoison, kPoisonEntry));
        apply(itemObject(kDagger, kDaggerEntry, daggerFlags, daggerTempEnchant));
        apply(itemObject(kRobe, kRobeEntry));
        apply(itemObject(kLockbox, kLockboxEntry));
        Fields f = {{uf::UNIT_FIELD_HEALTH, 100},
                    {uf::UNIT_FIELD_LEVEL, 30},
                    {uf::UNIT_FIELD_BYTES_0, 8u << 8}, // a mage
                    {uf::UNIT_FIELD_BOUNDINGRADIUS, floatBits(0.389f)}};
        slot(f, 15, kDagger);
        slot(f, 23, kPoison);
        slot(f, 24, kRobe);
        slot(f, 25, kLockbox);
        apply(created(kPlayer, game::ObjectType::Player, f));
        REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                       itemQuery({kPoisonEntry, 0, 0, 1, 0, 24, kInstantPoison})) ==
                game::Gameplay::Handled::Yes);
        REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                       itemQuery({kDaggerEntry, 2, 15, 2, 13, 25})) == game::Gameplay::Handled::Yes);
        REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery({kRobeEntry, 4, 1, 1, 20, 20})) ==
                game::Gameplay::Handled::Yes);
        REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                       itemQuery({kLockboxEntry, 15, 0, 1, 0, 25, 0, 5})) == game::Gameplay::Handled::Yes);
        sent.clear();
        events();
    }
};

Bytes modifierPacket(u8 bit, u8 op, i32 value) {
    Bytes out;
    net::ByteWriter(out).u8v(bit).u8v(op).u32v(static_cast<u32>(value));
    return out;
}

} // namespace

TEST_CASE("SpellMessages: SMSG_SET_FLAT_SPELL_MODIFIER / _PCT_ as Player::SendSpellMod writes them") {
    const Bytes body = {0x00, 0x0A, 0x9C, 0xFF, 0xFF, 0xFF}; // bit 0, SPELLMOD_CASTING_TIME, -100
    const std::optional<net::SpellModifierUpdate> m = net::parseSpellModifier(body);
    REQUIRE(m.has_value());
    CHECK(m->bit == 0);
    CHECK(m->op == 10);
    CHECK(m->value == -100);
    CHECK_FALSE(net::parseSpellModifier(Bytes{0x00, 0x0A, 0x9C, 0xFF, 0xFF}).has_value());       // truncated
    CHECK_FALSE(net::parseSpellModifier(Bytes{0x40, 0x0A, 0x00, 0x00, 0x00, 0x00}).has_value()); // bit 64
}

TEST_CASE("SpellMods: the client's tables and its rule to apply them") {
    game::SpellMods mods;
    const game::SpellModTarget fire = game::modTarget(fireball());
    // Not the player's class family yet: nothing applies.
    mods.set(true, 0, static_cast<u32>(SpellModOp::CastingTime), -100);
    CHECK(mods.apply(fire, SpellModOp::CastingTime, 1500) == 1500);
    mods.setClassFamily(3);
    // Improved Fireball rank 1: -0.1 s on Fireball's bit 0.
    CHECK(mods.apply(fire, SpellModOp::CastingTime, 1500) == 1400);
    // Another family's spell with the same bit, and a spell that ignores caster modifiers.
    game::SpellModTarget other = fire;
    other.family = 4;
    CHECK(mods.apply(other, SpellModOp::CastingTime, 1500) == 1500);
    game::SpellModTarget ignoring = fire;
    ignoring.attributesEx3 = 0x20000000;
    CHECK(mods.apply(ignoring, SpellModOp::CastingTime, 1500) == 1500);
    // Percentages add up over 100 and multiply what the flat ones left: (1500 - 100) * 50 / 100.
    mods.set(false, 30, static_cast<u32>(SpellModOp::CastingTime), -50); // Fireball's bit 30 too
    CHECK(mods.apply(fire, SpellModOp::CastingTime, 1500) == 700);
    // -100 % (Presence of Mind, Nature's Swiftness): instant; never below 0 %.
    mods.set(false, 30, static_cast<u32>(SpellModOp::CastingTime), -150);
    CHECK(mods.apply(fire, SpellModOp::CastingTime, 1500) == 0);
    // Integers truncate towards zero; floats are exact (Arctic Reach: +10 % range).
    mods.set(false, 0, static_cast<u32>(SpellModOp::Cost), 7);
    CHECK(mods.apply(fire, SpellModOp::Cost, 30) == 32); // 32.1
    mods.set(false, 0, static_cast<u32>(SpellModOp::Range), 10);
    CHECK(mods.apply(fire, SpellModOp::Range, 35.0f) == doctest::Approx(38.5f));
    const u32 version = mods.version();
    mods.set(false, 0, static_cast<u32>(SpellModOp::Range), 10); // the same total again
    CHECK(mods.version() == version);
    // The families of the classes when ChrClasses.dbc is not at hand.
    CHECK(game::classSpellFamily(8) == 3);  // mage
    CHECK(game::classSpellFamily(1) == 4);  // warrior
    CHECK(game::classSpellFamily(11) == 7); // druid
}

TEST_CASE("Gameplay: the modifiers come before the world, stay across teleports and go at logout") {
    Harness h(false);
    // Player::_LoadAuras / _LoadSpells send them while the character loads, before SMSG_LOGIN_VERIFY_WORLD.
    REQUIRE(h.packet(Opcode::SMSG_SET_FLAT_SPELL_MODIFIER, modifierPacket(0, 10, -100)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_SET_PCT_SPELL_MODIFIER, modifierPacket(5, 5, 10)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.packet(Opcode::SMSG_SET_PCT_SPELL_MODIFIER, Bytes{0x05}) == game::Gameplay::Handled::Malformed);
    h.play.enterWorld(kPlayer, 0, true, h.now);
    h.character();
    CHECK(h.play.spellMods().classFamily() == 3);
    CHECK(h.play.spellMods().flat(0, SpellModOp::CastingTime) == -100);
    CHECK(h.play.spellMods().percent(5, SpellModOp::Range) == 10);
    h.play.enterWorld(kPlayer, 1, false, h.now); // SMSG_NEW_WORLD: the same character
    CHECK(h.play.spellMods().flat(0, SpellModOp::CastingTime) == -100);
    // The tooltip's numbers: Fireball's cast time, its cost with the school's power cost fields.
    game::SpellDetails d;
    d.id = kFireball;
    d.castTimeMs = 1500;
    d.powerCost = 30;
    d.maxRange = 35.0f;
    d.spellFamily = 3;
    d.familyFlags = 0x40000001;
    game::applySpellMods(d, nullptr, &h.play.spellMods());
    CHECK(d.castTimeMs == 1400);
    CHECK(d.powerCost == 30);
    h.play.leaveWorld();
    CHECK(h.play.spellMods().flat(0, SpellModOp::CastingTime) == 0);
}

TEST_CASE("Gameplay: a spell aimed at a place waits for it, and the place is checked for range") {
    Harness h;
    h.character();
    // Blizzard from the action bar or the book: nothing is sent, the targeting starts.
    REQUIRE(h.play.castSpell(kBlizzard, 0));
    CHECK(h.sent.empty());
    REQUIRE(h.play.targeting().active());
    CHECK(h.play.targeting().aim() == game::SpellAim::Location);
    CHECK(game::spellAreaRadius(h.play.targeting().spell()) == doctest::Approx(8.0f));
    std::vector<game::GameEvent> ev = h.events();
    const game::GameEvent* started = Harness::find(ev, GameEventKind::SpellTargetingStarted);
    REQUIRE(started != nullptr);
    CHECK(started->id == kBlizzard);
    // Too far: the client says so and keeps waiting.
    CHECK(h.play.targetLocation(40.0f, 0.0f, 0.0f) == game::Gameplay::TargetResult::Refused);
    ev = h.events();
    const game::GameEvent* failed = Harness::find(ev, GameEventKind::CastFailed);
    REQUIRE(failed != nullptr);
    CHECK(net::spellCastResultName(static_cast<u8>(failed->value[0])) == "OUT_OF_RANGE");
    CHECK(h.play.targeting().active());
    CHECK(h.sent.empty());
    // Arctic Reach (+10 % range on Blizzard's bit 7): 33 yards, plus 1.25 and the caster's bounding radius.
    REQUIRE(h.packet(Opcode::SMSG_SET_PCT_SPELL_MODIFIER, modifierPacket(7, 5, 10)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.play.targetLocation(34.5f, 0.0f, 0.0f) == game::Gameplay::TargetResult::Sent);
    Bytes expected;
    net::ByteWriter(expected).u32v(kBlizzard).u16v(0x0040).f32v(34.5f).f32v(0.0f).f32v(0.0f);
    CHECK(h.take(Opcode::CMSG_CAST_SPELL) == std::vector<Bytes>{expected});
    CHECK_FALSE(h.play.targeting().active());
    CHECK(Harness::find(h.events(), GameEventKind::SpellTargetingStopped) != nullptr);
    CHECK(h.play.spells().globalCooldown(h.now).active(h.now)); // the cast went out: its global cooldown
    // Another spell while waiting replaces it; stopTargeting drops it.
    REQUIRE(h.play.castSpell(kBlizzard, 0));
    REQUIRE(h.play.castSpell(kFireball, 0));
    CHECK_FALSE(h.play.targeting().active());
    REQUIRE(h.play.castSpell(kBlizzard, 0));
    h.play.stopTargeting();
    CHECK_FALSE(h.play.targeting().active());
    // castSpellAt does not wait.
    h.sent.clear();
    REQUIRE(h.play.castSpellAt(kBlizzard, 1.0f, 2.0f, 3.0f));
    CHECK(h.take(Opcode::CMSG_CAST_SPELL).size() == 1);
}

TEST_CASE("Gameplay: a poison waits for a weapon, used with CMSG_USE_ITEM on it") {
    Harness h;
    h.character();
    REQUIRE(h.play.useItemAt({255, 23}));
    CHECK(h.sent.empty());
    REQUIRE(h.play.targeting().active());
    CHECK(h.play.targeting().aim() == game::SpellAim::Item);
    CHECK(h.play.targeting().fromItem());
    CHECK(h.play.targeting().itemGuid() == kPoison);
    h.events();
    // A cloth robe: the client itself refuses it ("Invalid target"), the poison still waits.
    CHECK(h.play.targetItem({255, 24}) == game::Gameplay::TargetResult::Refused);
    const std::vector<game::GameEvent> ev = h.events();
    const game::GameEvent* failed = Harness::find(ev, GameEventKind::CastFailed);
    REQUIRE(failed != nullptr);
    CHECK(net::spellCastResultName(static_cast<u8>(failed->value[0])) == "BAD_TARGETS");
    CHECK(h.play.targeting().active());
    // The dagger: CMSG_USE_ITEM with the poison's place and spell index, the dagger as the item target.
    REQUIRE(h.play.targetItem({255, 15}) == game::Gameplay::TargetResult::Sent);
    Bytes expected;
    net::ByteWriter(expected).u8v(255).u8v(23).u8v(0).u16v(0x0010).packedGuid(kDagger);
    CHECK(h.take(Opcode::CMSG_USE_ITEM) == std::vector<Bytes>{expected});
    CHECK_FALSE(h.play.targeting().active());
    // How the windows light the items for it.
    const game::SpellInfo poison = instantPoison();
    const net::ItemQueryInfo* dagger = h.play.queries().item(kDaggerEntry);
    const net::ItemQueryInfo* robe = h.play.queries().item(kRobeEntry);
    REQUIRE(dagger != nullptr);
    REQUIRE(robe != nullptr);
    CHECK(game::itemFitsSpell(poison, *dagger, nullptr) == game::ItemFit::Fits);
    CHECK(game::itemFitsSpell(poison, *robe, nullptr) == game::ItemFit::Refused);
}

TEST_CASE("Gameplay: Pick Lock waits for a locked box or a locked object; casts without waiting") {
    Harness h;
    h.character();
    REQUIRE(h.play.castSpell(kPickLock, 0));
    CHECK(h.play.targeting().aim() == game::SpellAim::Locked);
    CHECK(game::spellLockType(h.play.targeting().spell()) == 1);
    const net::ItemQueryInfo* box = h.play.queries().item(kLockboxEntry);
    const net::ItemQueryInfo* robe = h.play.queries().item(kRobeEntry);
    REQUIRE(box != nullptr);
    CHECK(game::itemFitsSpell(pickLock(), *box, nullptr) == game::ItemFit::Fits);
    CHECK(game::itemFitsSpell(pickLock(), *robe, nullptr) == game::ItemFit::Unlikely);
    REQUIRE(h.play.targetItem({255, 25}) == game::Gameplay::TargetResult::Sent);
    Bytes expected;
    net::ByteWriter(expected).u32v(kPickLock).u16v(0x0010).packedGuid(kLockbox);
    CHECK(h.take(Opcode::CMSG_CAST_SPELL) == std::vector<Bytes>{expected});
    // A locked chest in the world.
    h.apply(created(kChest, game::ObjectType::GameObject, {{uf::OBJECT_FIELD_ENTRY, 4096}}, 3.0f, 0.0f));
    REQUIRE(h.play.castSpell(kPickLock, 0));
    REQUIRE(h.play.targetObject(kChest) == game::Gameplay::TargetResult::Sent);
    expected.clear();
    net::ByteWriter(expected).u32v(kPickLock).u16v(0x0800).packedGuid(kChest);
    CHECK(h.take(Opcode::CMSG_CAST_SPELL) == std::vector<Bytes>{expected});
    // Without waiting: on the player's item, on the trader's item that will not be traded (slot 6).
    REQUIRE(h.play.castSpellOnItem(kInstantPoison, {255, 15}));
    expected.clear();
    net::ByteWriter(expected).u32v(kInstantPoison).u16v(0x0010).packedGuid(kDagger);
    CHECK(h.take(Opcode::CMSG_CAST_SPELL) == std::vector<Bytes>{expected});
    REQUIRE(h.play.castSpellOnTradeItem(7418));
    expected.clear();
    net::ByteWriter(expected).u32v(7418).u16v(0x1000).u8v(0x01).u8v(0x06);
    CHECK(h.take(Opcode::CMSG_CAST_SPELL) == std::vector<Bytes>{expected});
    // Dead: nothing waits.
    REQUIRE(h.play.castSpell(kPickLock, 0));
    Bytes dead;
    net::ByteWriter w(dead);
    w.u32v(1).u8v(0).u8v(0).packedGuid(kPlayer);
    writeValues(w, {{uf::UNIT_FIELD_HEALTH, 0}});
    h.apply(dead);
    CHECK_FALSE(h.play.targeting().active());
}

TEST_CASE("Spell targeting and modifiers against the real Spell.dbc") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    game::GameData data;
    REQUIRE(data.init(db));
    const auto aim = [&](u32 id) {
        const std::optional<game::SpellInfo> s = data.spell(id);
        return s ? game::spellAim(*s) : game::SpellAim::None;
    };
    // Every player spell with a place to choose (SkillLineAbility.dbc): Blizzard, Flamestrike, Rain of Fire,
    // Hurricane, Volley, Inferno, Flare, Distract, Far Sight, Eagle Eye; and the engineering bombs.
    for (const u32 id :
         {10u,   6141u, 8427u,  10185u, 10186u, 10187u, 2120u,  2121u, 8422u,  8423u,  10215u, 10216u,
          5740u, 6219u, 11677u, 11678u, 16914u, 17401u, 17402u, 1510u, 14294u, 14295u, 1122u,  1543u,
          1725u, 6196u, 6197u,  4064u,  4065u,  4066u,  4067u,  4068u, 4069u,  12543u, 23063u, 19821u}) {
        CAPTURE(id);
        CHECK(aim(id) == game::SpellAim::Location);
    }
    // Items: Instant Poison, Sharpen Blade, Enchant Bracer - Minor Health, Feed Pet, Disenchant.
    for (const u32 id : {8679u, 2828u, 7418u, 6991u, 13262u}) {
        CAPTURE(id);
        CHECK(aim(id) == game::SpellAim::Item);
    }
    CHECK(aim(1804) == game::SpellAim::Locked); // Pick Lock (TARGET_LOCKED)
    CHECK(aim(1842) == game::SpellAim::Locked); // Disarm Trap (TARGET_FLAG_LOCKED)
    CHECK(aim(133) == game::SpellAim::None);    // Fireball
    CHECK(aim(1449) == game::SpellAim::None);   // Arcane Explosion: around the caster
    CHECK(aim(13241) == game::SpellAim::None);  // Goblin Sapper Charge: around the caster
    const std::optional<game::SpellInfo> blizzard = data.spell(10);
    REQUIRE(blizzard.has_value());
    CHECK(game::spellAreaRadius(*blizzard) == doctest::Approx(8.0f));
    CHECK(blizzard->spellFamily == 3);
    CHECK(blizzard->familyFlags == 0x80080);
    const std::optional<game::SpellInfo> poison = data.spell(8679);
    REQUIRE(poison.has_value());
    CHECK(poison->equippedItemClass == 2);
    CHECK(poison->equippedItemSubClassMask == 0x2a5f3);
    const std::optional<game::SpellInfo> bracer = data.spell(7418);
    REQUIRE(bracer.has_value());
    CHECK(bracer->equippedItemClass == 4);
    CHECK(bracer->equippedItemInventoryTypeMask == 0x200); // INVTYPE_WRISTS
    // The enchantments the questions name, and the classes' spell families.
    REQUIRE(data.enchantment(323).has_value());
    CHECK(data.enchantment(323)->name == "Instant Poison");
    CHECK(data.enchantment(323)->flags == 1); // ENCHANTMENT_CAN_SOULBOUND
    CHECK(data.enchantment(41)->flags == 0);
    for (const u32 cls : {1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 11u}) {
        CAPTURE(cls);
        CHECK(data.classSpellFamily(cls) == game::classSpellFamily(cls));
    }

    // The questions before an enchantment (WoW.exe): Instant Poison binds an unbound weapon, then replaces.
    net::ItemQueryInfo dagger;
    dagger.itemClass = 2;
    dagger.subclass = 15;
    dagger.inventoryType = 13;
    dagger.itemLevel = 25;
    game::ItemInstance item;
    item.guid = kDagger;
    item.entry = kDaggerEntry;
    CHECK(game::enchantQuestion(*poison, item, dagger, false, &data).ask == game::EnchantAsk::Bind);
    CHECK(game::enchantQuestion(*poison, item, dagger, true, &data).ask == game::EnchantAsk::None);
    item.enchantments[1] = 7; // a temporary enchantment already there (Deadly Poison)
    const game::EnchantQuestion replace = game::enchantQuestion(*poison, item, dagger, true, &data);
    CHECK(replace.ask == game::EnchantAsk::Replace);
    CHECK(replace.oldEnchant == 7);
    CHECK(replace.newEnchant == 323);
    item.flags = game::kItemDynFlagBound; // soulbound: only the replacement is asked
    CHECK(game::enchantQuestion(*poison, item, dagger, false, &data).ask == game::EnchantAsk::Replace);
    // A bracer enchantment (no ENCHANTMENT_CAN_SOULBOUND) on a bracer without one: nothing to ask.
    net::ItemQueryInfo bracers;
    bracers.itemClass = 4;
    bracers.subclass = 2;
    bracers.inventoryType = 9;
    bracers.itemLevel = 10;
    game::ItemInstance wrist;
    CHECK(game::itemFitsSpell(*bracer, bracers, &wrist) == game::ItemFit::Fits);
    CHECK(game::itemFitsSpell(*bracer, dagger, &wrist) == game::ItemFit::Refused);
    CHECK(game::enchantQuestion(*bracer, wrist, bracers, false, &data).ask == game::EnchantAsk::None);
    wrist.enchantments[0] = 41;
    CHECK(game::enchantQuestion(*bracer, wrist, bracers, false, &data).ask == game::EnchantAsk::Replace);
    CHECK(game::tradeEnchantQuestion(*bracer, 41, &data).ask == game::EnchantAsk::TradeReplace);
    CHECK(game::tradeEnchantQuestion(*poison, 41, &data).ask ==
          game::EnchantAsk::None); // permanent ones only

    // Talents in the tooltips, with the values the server sends for them (Spell.dbc: BasePoints + 1, the
    // masks in EffectItemType): Improved Fireball rank 1 (-0.1 s on bit 0), Improved Frostbolt rank 5 (-0.5 s
    // on bit 5), Arctic Reach rank 1 (+10 % range on bits 5 and 7).
    game::SpellText text;
    REQUIRE(text.init(db));
    game::SpellMods mods;
    mods.setClassFamily(data.classSpellFamily(8));
    mods.set(true, 0, static_cast<u32>(SpellModOp::CastingTime), -100); // Improved Fireball rank 1
    mods.set(true, 5, static_cast<u32>(SpellModOp::CastingTime), -500); // Improved Frostbolt rank 5
    mods.set(false, 5, static_cast<u32>(SpellModOp::Range), 10);        // Arctic Reach rank 1
    mods.set(false, 7, static_cast<u32>(SpellModOp::Range), 10);
    game::SpellDetails fire;
    REQUIRE(text.details(133, fire));
    CHECK(fire.castTimeMs == 1500);
    game::applySpellMods(fire, nullptr, &mods);
    CHECK(fire.castTimeMs == 1400);
    game::SpellDetails frost;
    REQUIRE(text.details(7322, frost)); // Frostbolt rank 4
    const u32 frostCast = frost.castTimeMs;
    CHECK(frostCast == 2600);
    game::applySpellMods(frost, nullptr, &mods);
    CHECK(frost.castTimeMs == frostCast - 500);
    CHECK(frost.maxRange == doctest::Approx(33.0f));
    game::SpellDetails blizz;
    REQUIRE(text.details(10, blizz));
    game::applySpellMods(blizz, nullptr, &mods);
    CHECK(blizz.maxRange == doctest::Approx(33.0f));
    // The description: $s2 of Frostbolt rank 1 (18 to 20) with +10 % SPELLMOD_DAMAGE, $d with +1 s
    // SPELLMOD_DURATION.
    mods.set(false, 5, static_cast<u32>(SpellModOp::Damage), 10);
    mods.set(true, 5, static_cast<u32>(SpellModOp::Duration), 1000);
    REQUIRE(test::loadTexts()); // "5 sec": the client's duration texts
    game::SpellTextContext context;
    context.mods = &mods;
    std::string plain, modded;
    game::SpellTextContext none;
    REQUIRE(game::spellDescription(text, 116, plain, none));
    REQUIRE(game::spellDescription(text, 116, modded, context));
    CHECK(plain.find("18") != std::string::npos);
    CHECK(modded.find("19.8") != std::string::npos);
    CHECK(modded.find("22") != std::string::npos);
    CHECK(plain.find("5 sec") != std::string::npos);
    CHECK(modded.find("6 sec") != std::string::npos);
    text::unload();
}
