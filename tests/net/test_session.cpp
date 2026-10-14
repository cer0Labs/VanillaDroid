#include "game/Entities.h"
#include "net/ByteBuffer.h"
#include "net/Messages.h"
#include "net/PacketRing.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using vd::f32;
using vd::u16;
using vd::u32;
using vd::u64;
using vd::u8;

u32 bits(f32 value) {
    u32 out = 0;
    std::memcpy(&out, &value, 4);
    return out;
}

// Values block as Object::BuildValuesUpdate writes it: mask word count, mask, one u32 per set bit.
void writeValues(vd::net::ByteWriter& w, std::initializer_list<std::pair<u16, u32>> values, u32 count) {
    const u32 words = (count + 31) / 32;
    std::vector<u32> mask(words, 0);
    for (const auto& [index, value] : values) {
        (void)value;
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(words));
    for (u32 word : mask) {
        w.u32v(word);
    }
    for (u32 index = 0; index < count; ++index) {
        for (const auto& [i, value] : values) {
            if (i == index) {
                w.u32v(value);
            }
        }
    }
}

} // namespace

TEST_CASE("SMSG_UPDATE_OBJECT: create, values, out of range (vMaNGOS layout)") {
    std::vector<u8> packet;
    vd::net::ByteWriter w(packet);
    w.u32v(4).u8v(0); // 4 blocks, no transport

    // 1. A creature on a spline: create block, living movement with spline data.
    const u64 creature = 0xF130000123000456ull;
    w.u8v(2).packedGuid(creature).u8v(3); // CREATE_OBJECT, TYPEID_UNIT
    w.u8v(0x20 | 0x10);                   // UPDATEFLAG_LIVING | UPDATEFLAG_ALL
    vd::net::MovementInfo move;
    move.flags = vd::net::kMoveFlagForward | vd::net::kMoveFlagSplineEnabled;
    move.time = 1000;
    move.position = {10.0f, 20.0f, 30.0f, 1.5f};
    vd::net::writeMovementInfo(w, move);
    for (f32 speed : {2.5f, 7.0f, 4.5f, 4.72f, 2.5f, 3.14f}) {
        w.f32v(speed);
    }
    w.u32v(0x00010000); // spline flags: Final_Point
    w.f32v(1).f32v(2).f32v(3);
    w.u32v(500).u32v(2000).u32v(7); // time passed, duration, id
    w.u32v(2).f32v(10).f32v(20).f32v(30).f32v(40).f32v(20).f32v(30); // nodes
    w.f32v(40).f32v(20).f32v(30);                                     // final destination
    w.u32v(1);                                                         // UPDATEFLAG_ALL value
    writeValues(w, {{vd::net::uf::OBJECT_FIELD_ENTRY, 299}, {vd::net::uf::UNIT_FIELD_HEALTH, 42},
                    {vd::net::uf::UNIT_FIELD_BOUNDINGRADIUS, bits(0.4f)}},
                vd::net::uf::UNIT_END);

    // 2. A game object with a position.
    const u64 object = 0xF110000000000777ull;
    w.u8v(3).packedGuid(object).u8v(5); // CREATE_OBJECT2, TYPEID_GAMEOBJECT
    w.u8v(0x40 | 0x08);                 // UPDATEFLAG_HAS_POSITION | UPDATEFLAG_HIGHGUID
    w.f32v(-100.0f).f32v(50.0f).f32v(12.0f).f32v(0.25f);
    w.u32v(0);
    writeValues(w, {{vd::net::uf::OBJECT_FIELD_ENTRY, 1234}}, vd::net::uf::GAMEOBJECT_END);

    // 3. A values update of the creature.
    w.u8v(0).packedGuid(creature);
    writeValues(w, {{vd::net::uf::UNIT_FIELD_HEALTH, 17}}, vd::net::uf::UNIT_END);

    // 4. Something out of range.
    w.u8v(4).u32v(1).packedGuid(object);

    vd::game::EntityStore store;
    vd::game::UpdateStats stats;
    REQUIRE(store.applyUpdateObject(packet, 10000, &stats));
    CHECK(stats.created == 2);
    CHECK(stats.updated == 1);
    CHECK(stats.removed == 1);
    CHECK(store.size() == 1);
    CHECK(store.find(object) == nullptr);
    const vd::game::Entity* e = store.find(creature);
    REQUIRE(e != nullptr);
    CHECK(e->type == vd::game::ObjectType::Unit);
    CHECK(e->value(vd::net::uf::OBJECT_FIELD_ENTRY) == 299);
    CHECK(e->value(vd::net::uf::UNIT_FIELD_HEALTH) == 17);
    CHECK(e->speed(vd::game::SpeedType::Run) == 7.0f);
    // 500 of 2000 ms had passed at 10000: halfway at 10500.
    const glm::vec3 half = e->positionAt(10500);
    CHECK(half.x == doctest::Approx(25.0f));
    CHECK(e->positionAt(20000).x == doctest::Approx(40.0f));

    // A truncated packet is refused, but what came before the cut is kept.
    vd::game::EntityStore partial;
    CHECK_FALSE(partial.applyUpdateObject(std::span<const u8>(packet.data(), packet.size() - 3), 0));
    CHECK(partial.find(creature) != nullptr);
}

TEST_CASE("SMSG_MONSTER_MOVE and MSG_MOVE_* move known objects") {
    vd::game::EntityStore store;
    std::vector<u8> create;
    vd::net::ByteWriter c(create);
    c.u32v(1).u8v(0).u8v(2).packedGuid(55).u8v(3).u8v(0x40);
    c.f32v(0).f32v(0).f32v(0).f32v(0);
    writeValues(c, {}, vd::net::uf::UNIT_END);
    REQUIRE(store.applyUpdateObject(create, 0));

    // Linear path: start, id, type normal, flags, duration, count, destination, count - 1 packed offsets.
    std::vector<u8> move;
    vd::net::ByteWriter m(move);
    m.packedGuid(55).f32v(0).f32v(0).f32v(0).u32v(9).u8v(0).u32v(0).u32v(1000).u32v(2);
    m.f32v(100).f32v(0).f32v(0).u32v(0);
    REQUIRE(store.applyMonsterMove(move, 5000));
    CHECK(store.find(55)->positionAt(5500).x == doctest::Approx(50.0f));

    std::vector<u8> heartbeat;
    vd::net::ByteWriter h(heartbeat);
    h.packedGuid(55);
    vd::net::MovementInfo info;
    info.position = {7.0f, 8.0f, 9.0f, 0.0f};
    vd::net::writeMovementInfo(h, info);
    REQUIRE(store.applyMove(heartbeat));
    CHECK(store.find(55)->positionAt(99999).y == doctest::Approx(8.0f));
}

TEST_CASE("Remote movement: spline paths, jumps and eased corrections") {
    vd::game::EntityStore store;
    std::vector<u8> create;
    vd::net::ByteWriter c(create);
    c.u32v(1).u8v(0).u8v(2).packedGuid(77).u8v(3).u8v(0x40);
    c.f32v(0).f32v(0).f32v(0).f32v(0);
    writeValues(c, {}, vd::net::uf::UNIT_END);
    REQUIRE(store.applyUpdateObject(create, 0));

    // An L-shaped path (0,0) -> (10,0) -> (10,10), 20 yards in 2 s, facing angle 1 at the end. The points before
    // the destination come as packed quarter-yard offsets from it (the start first, as vMaNGOS writes them).
    auto pack = [](f32 x, f32 y, f32 z) {
        return (static_cast<u32>(std::lround(x * 4.0f)) & 0x7FF) | ((static_cast<u32>(std::lround(y * 4.0f)) & 0x7FF) << 11) |
               ((static_cast<u32>(std::lround(z * 4.0f)) & 0x3FF) << 22);
    };
    std::vector<u8> move;
    vd::net::ByteWriter m(move);
    m.packedGuid(77).f32v(0).f32v(0).f32v(0).u32v(1).u8v(4).f32v(1.0f).u32v(0x100).u32v(2000).u32v(3);
    m.f32v(10).f32v(10).f32v(0).u32v(pack(10, 10, 0)).u32v(pack(0, 10, 0));
    REQUIRE(store.applyMonsterMove(move, 1000));
    const vd::game::Entity* e = store.find(77);
    REQUIRE(e->path.size() == 3);
    CHECK(e->pathLength == doctest::Approx(20.0f));
    CHECK(e->positionAt(1500).x == doctest::Approx(5.0f));
    CHECK(e->positionAt(2500).x == doctest::Approx(10.0f));
    CHECK(e->positionAt(2500).y == doctest::Approx(5.0f));
    f32 heading = 0.0f;
    REQUIRE(e->headingAt(2500, heading));
    CHECK(heading == doctest::Approx(1.5707963f));
    CHECK(e->speedAt(1500) == doctest::Approx(10.0f));
    CHECK_FALSE(e->walking());
    CHECK(e->speedAt(4000) == 0.0f);
    CHECK(e->hasFinalAngle);

    // A player jumping forward at 7 yd/s: momentum and gravity until the next packet.
    std::vector<u8> jump;
    vd::net::ByteWriter j(jump);
    j.packedGuid(77);
    vd::net::MovementInfo info;
    info.flags = 0x2000 | vd::net::kMoveFlagForward;
    info.position = {100.0f, 0.0f, 0.0f, 0.0f};
    info.jumpVelocity = -7.95797334f;
    info.jumpCos = 1.0f;
    info.jumpXySpeed = 7.0f;
    vd::net::writeMovementInfo(j, info);
    REQUIRE(store.applyMove(jump, 10000));
    const glm::vec3 apex = e->positionAt(10000 + 412);
    CHECK(apex.x == doctest::Approx(100.0f + 7.0f * 0.412f).epsilon(1e-3));
    CHECK(apex.z == doctest::Approx(1.6414f).epsilon(1e-2));
    CHECK(e->airborne());

    // A heartbeat a little off from what was shown: the shown position eases to it instead of jumping.
    std::vector<u8> beat;
    vd::net::ByteWriter b(beat);
    b.packedGuid(77);
    vd::net::MovementInfo still;
    still.position = {apex.x + 1.0f, 0.0f, apex.z, 0.0f};
    vd::net::writeMovementInfo(b, still);
    REQUIRE(store.applyMove(beat, 10412));
    CHECK(e->positionAt(10412).x == doctest::Approx(apex.x).epsilon(1e-4));
    CHECK(e->positionAt(10412 + vd::game::EntityStore::kSmoothMs).x == doctest::Approx(apex.x + 1.0f));
}

TEST_CASE("SMSG_CHAR_ENUM and SMSG_LOGIN_VERIFY_WORLD parse") {
    std::vector<u8> packet;
    vd::net::ByteWriter w(packet);
    w.u8v(1).u64v(42).cstring("Arthel").u8v(1).u8v(1).u8v(0);
    for (int i = 0; i < 5; ++i) {
        w.u8v(static_cast<u8>(i));
    }
    w.u8v(1).u32v(12).u32v(0).f32v(-8922.2f).f32v(-124.7f).f32v(81.6f).u32v(0).u32v(0).u8v(1);
    w.u32v(0).u32v(0).u32v(0);
    for (int slot = 0; slot < 20; ++slot) {
        w.u32v(static_cast<u32>(slot)).u8v(0);
    }
    const auto characters = vd::net::parseCharEnum(packet);
    REQUIRE(characters.has_value());
    REQUIRE(characters->size() == 1);
    CHECK((*characters)[0].name == "Arthel");
    CHECK((*characters)[0].zone == 12);
    CHECK((*characters)[0].equipmentDisplay[19] == 19);
    CHECK_FALSE(vd::net::parseCharEnum(std::span<const u8>(packet.data(), packet.size() - 1)).has_value());
}

TEST_CASE("PacketRing: variable records, wrap-around, full, two threads") {
    vd::net::PacketRing ring(1024);
    std::vector<u8> payload(300, 7);
    // Fills up (header + 300 rounded = 312 bytes a record): three fit, the fourth does not.
    CHECK(ring.push(1, 10, payload));
    CHECK(ring.push(1, 11, payload));
    CHECK(ring.push(1, 12, payload));
    CHECK_FALSE(ring.push(1, 13, payload));
    u16 kind = 0, opcode = 0;
    std::span<const u8> data;
    REQUIRE(ring.front(kind, opcode, data));
    CHECK(opcode == 10);
    CHECK(data.size() == 300);
    ring.pop();
    // Room again, but not contiguous at the end: the record wraps to the start.
    CHECK(ring.push(2, 13, payload));
    for (u16 expected : {u16{11}, u16{12}, u16{13}}) {
        REQUIRE(ring.front(kind, opcode, data));
        CHECK(opcode == expected);
        CHECK(data[299] == 7);
        ring.pop();
    }
    CHECK_FALSE(ring.front(kind, opcode, data));

    // Producer and consumer threads: every record arrives once, in order, intact.
    vd::net::PacketRing shared(4096);
    constexpr u32 kCount = 20000;
    std::thread producer([&] {
        std::vector<u8> bytes;
        for (u32 i = 0; i < kCount; ++i) {
            bytes.assign(i % 200, static_cast<u8>(i));
            while (!shared.push(3, static_cast<u16>(i), bytes)) {
                std::this_thread::yield();
            }
        }
    });
    u32 received = 0;
    bool intact = true;
    while (received < kCount) {
        if (!shared.front(kind, opcode, data)) {
            std::this_thread::yield();
            continue;
        }
        intact = intact && opcode == static_cast<u16>(received) && data.size() == received % 200 &&
                 (data.empty() || (data.front() == static_cast<u8>(received) && data.back() == static_cast<u8>(received)));
        shared.pop();
        ++received;
    }
    producer.join();
    CHECK(intact);
}

TEST_CASE("Query responses: item, player name, creature (vMaNGOS layout)") {
    // HandleItemQuerySingleOpcode for build 5875, field by field.
    std::vector<u8> item;
    vd::net::ByteWriter w(item);
    w.u32v(2362).u32v(4).u32v(6).cstring("Weathered Buckler").cstring("").cstring("").cstring("");
    w.u32v(18662).u32v(1).u32v(0).u32v(10).u32v(2).u32v(14); // display, quality, flags, buy, sell, inventory type
    for (int i = 0; i < 14; ++i) {
        w.u32v(0); // class/race masks ... container slots
    }
    for (int i = 0; i < 10; ++i) {
        w.u32v(0).u32v(0); // stats
    }
    for (int i = 0; i < 5; ++i) {
        w.f32v(0.0f).f32v(0.0f).u32v(0); // damage
    }
    for (int i = 0; i < 7; ++i) {
        w.u32v(0); // armor, resistances
    }
    w.u32v(0).u32v(0).f32v(0.0f); // delay, ammo, ranged range
    for (int i = 0; i < 5; ++i) {
        w.u32v(0).u32v(0).u32v(0).u32v(static_cast<u32>(-1)).u32v(0).u32v(static_cast<u32>(-1));
    }
    w.u32v(0).cstring("").u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(1);
    w.u32v(4); // sheath: shield
    w.u32v(0).u32v(0).u32v(0).u32v(20).u32v(0).u32v(0); // random property, block, set, durability, area, map
    w.u32v(0);                                           // bag family
    const auto parsed = vd::net::parseItemQueryResponse(item);
    REQUIRE(parsed.has_value());
    CHECK(parsed->found);
    CHECK(parsed->entry == 2362);
    CHECK(parsed->name == "Weathered Buckler");
    CHECK(parsed->displayId == 18662);
    CHECK(parsed->inventoryType == 14);
    CHECK(parsed->sheath == 4);
    std::vector<u8> unknown;
    vd::net::ByteWriter(unknown).u32v(99999u | 0x80000000u);
    const auto missing = vd::net::parseItemQueryResponse(unknown);
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->found);
    CHECK(missing->entry == 99999);

    std::vector<u8> name;
    vd::net::ByteWriter(name).u64v(42).cstring("Arthel").cstring("").u32v(1).u32v(0).u32v(8);
    const auto player = vd::net::parseNameQueryResponse(name);
    REQUIRE(player.has_value());
    CHECK(player->guid == 42);
    CHECK(player->name == "Arthel");
    CHECK(player->cls == 8);

    std::vector<u8> creature;
    vd::net::ByteWriter(creature)
        .u32v(3309).cstring("Karus").cstring("").cstring("").cstring("").cstring("Banker")
        .u32v(0).u32v(7).u32v(0).u32v(0).u32v(0).u32v(0).u32v(3200).u32v(1).u8v(0);
    const auto npc = vd::net::parseCreatureQueryResponse(creature);
    REQUIRE(npc.has_value());
    CHECK(npc->name == "Karus");
    CHECK(npc->subname == "Banker");
    CHECK(npc->type == 7);
}
