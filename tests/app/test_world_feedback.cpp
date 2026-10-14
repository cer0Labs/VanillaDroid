// Parity (world feedback): chat bubbles, the second-tap rule, the selection circle's geometry.

#include "app/ChatBubbles.h"
#include "app/SelectionCircle.h"
#include "app/WorldTap.h"
#include "game/Chat.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"
#include "ui/DrawList.h"

#include <doctest/doctest.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace vd;
using net::ChatType;

namespace {

game::ChatLine chatLine(ChatType type, u64 sender, std::string text) {
    game::ChatLine line;
    line.source = game::ChatSource::Message;
    line.type = type;
    line.sender = sender;
    line.text = std::move(text);
    return line;
}

constexpr ChatBubbles::Options kAll{true, true};

} // namespace

TEST_CASE("ChatBubbles: which lines become bubbles, in their chat colors") {
    ChatBubbles bubbles;
    CHECK(bubbles.add(chatLine(ChatType::Say, 1, "hello"), 1000, kAll));
    CHECK(bubbles.add(chatLine(ChatType::Yell, 2, "For the Alliance!"), 1000, kAll));
    CHECK(bubbles.add(chatLine(ChatType::MonsterSay, 3, "Welcome!"), 1000, kAll));
    CHECK(bubbles.add(chatLine(ChatType::MonsterYell, 4, "You no take candle!"), 1000, kAll));
    CHECK(bubbles.add(chatLine(ChatType::Party, 5, "pull"), 1000, kAll));
    // Whispers, emotes, guild, channels, system lines and lines without a speaker: never.
    CHECK_FALSE(bubbles.add(chatLine(ChatType::Whisper, 6, "psst"), 1000, kAll));
    CHECK_FALSE(bubbles.add(chatLine(ChatType::Emote, 6, "waves"), 1000, kAll));
    CHECK_FALSE(bubbles.add(chatLine(ChatType::Guild, 6, "hi guild"), 1000, kAll));
    CHECK_FALSE(bubbles.add(chatLine(ChatType::Channel, 6, "WTS"), 1000, kAll));
    CHECK_FALSE(bubbles.add(chatLine(ChatType::Say, 0, "nobody"), 1000, kAll));
    game::ChatLine notice = chatLine(ChatType::Say, 6, "You are too far away.");
    notice.source = game::ChatSource::Notification;
    CHECK_FALSE(bubbles.add(notice, 1000, kAll));
    CHECK(bubbles.count() == 5);
    REQUIRE(bubbles.of(1) != nullptr);
    CHECK(bubbles.of(1)->view() == "hello");
    CHECK(bubbles.of(1)->color == ui::rgba(255, 255, 255));
    CHECK(bubbles.of(2)->color == ui::rgba(255, 64, 64));
    CHECK(bubbles.of(4)->color == ui::rgba(255, 64, 64));
    CHECK(bubbles.of(5)->color == ui::rgba(170, 170, 255));

    // The options: ChatBubbles (say, yell, monsters) and ChatBubblesParty apart.
    ChatBubbles onlyParty;
    CHECK_FALSE(onlyParty.add(chatLine(ChatType::Say, 1, "hello"), 1000, {false, true}));
    CHECK(onlyParty.add(chatLine(ChatType::Party, 1, "hello"), 1000, {false, true}));
    ChatBubbles noParty;
    CHECK(noParty.add(chatLine(ChatType::Say, 1, "hello"), 1000, {true, false}));
    CHECK_FALSE(noParty.add(chatLine(ChatType::Party, 2, "hello"), 1000, {true, false}));
}

TEST_CASE("ChatBubbles: one per speaker, at most kMax, the oldest gives way") {
    ChatBubbles bubbles;
    CHECK(bubbles.add(chatLine(ChatType::Say, 1, "first"), 1000, kAll));
    CHECK(bubbles.add(chatLine(ChatType::Say, 1, "second"), 1500, kAll));
    CHECK(bubbles.count() == 1);
    CHECK(bubbles.of(1)->view() == "second");
    CHECK(bubbles.of(1)->startMs == 1500); // restarted

    for (u64 speaker = 2; speaker <= ChatBubbles::kMax; ++speaker) {
        CHECK(
            bubbles.add(chatLine(ChatType::Say, speaker, "hi"), 1500 + static_cast<u32>(speaker) * 10, kAll));
    }
    CHECK(bubbles.count() == ChatBubbles::kMax);
    // A new speaker takes the oldest bubble's place (speaker 1, said at 1500).
    CHECK(bubbles.add(chatLine(ChatType::Say, 99, "new"), 2000, kAll));
    CHECK(bubbles.count() == ChatBubbles::kMax);
    CHECK(bubbles.of(1) == nullptr);
    REQUIRE(bubbles.of(99) != nullptr);
    CHECK(bubbles.of(2) != nullptr);
    // A line with nothing to show (only a color code) leaves nothing over the head.
    CHECK_FALSE(bubbles.add(chatLine(ChatType::Say, 99, "|cffffffff|r"), 2100, kAll));
    CHECK(bubbles.of(99) == nullptr);
}

TEST_CASE("ChatBubbles: life by length, fading at the end, expiring") {
    CHECK(ChatBubbles::lifetimeMs(0) == 4000);
    CHECK(ChatBubbles::lifetimeMs(5) == 4300);
    CHECK(ChatBubbles::lifetimeMs(100) == 10000);
    CHECK(ChatBubbles::lifetimeMs(255) == 15000); // capped
    ChatBubbles bubbles;
    REQUIRE(bubbles.add(chatLine(ChatType::Say, 7, "hello"), 10000, kAll)); // lives 4300 ms
    const ChatBubbles::Bubble& b = *bubbles.of(7);
    CHECK(ChatBubbles::alpha(b, 10000) == doctest::Approx(1.0f));
    CHECK(ChatBubbles::alpha(b, 10000 + 4300 - ChatBubbles::kFadeMs) == doctest::Approx(1.0f));
    CHECK(ChatBubbles::alpha(b, 10000 + 4300 - ChatBubbles::kFadeMs / 2) == doctest::Approx(0.5f));
    CHECK(ChatBubbles::alpha(b, 10000 + 4300) == doctest::Approx(0.0f));
    bubbles.expire(10000 + 4299);
    CHECK(bubbles.count() == 1);
    bubbles.expire(10000 + 4300);
    CHECK(bubbles.count() == 0);
}

TEST_CASE("ChatBubbles: links keep their [text], color codes go") {
    ChatBubbles bubbles;
    REQUIRE(bubbles.add(
        chatLine(ChatType::Say, 1, "WTS |cffffffff|Hitem:2589:0:0:0|h[Linen Cloth]|h|r cheap"), 0, kAll));
    CHECK(bubbles.of(1)->view() == "WTS [Linen Cloth] cheap");
    // A line cut inside a link drops the rest; a long line fits the bubble's buffer.
    REQUIRE(bubbles.add(chatLine(ChatType::Say, 2, "see |Hitem:2589"), 0, kAll));
    CHECK(bubbles.of(2)->view() == "see ");
    REQUIRE(bubbles.add(chatLine(ChatType::Say, 3, std::string(300, 'a')), 0, kAll));
    CHECK(bubbles.of(3)->view().size() == ChatBubbles::kTextBytes);
}

TEST_CASE("ChatBubbles: follows the session's chat from the first update on") {
    constexpr u64 kPlayer = 0x10;
    constexpr u64 kOther = 0x11;
    game::EntityStore store;
    game::Gameplay play{store};
    game::PacketSink sink;
    sink.send = [](void*, net::Opcode, std::span<const u8>) { return true; };
    play.setSink(sink);
    play.enterWorld(kPlayer, 0, true, 1000);
    const auto say = [&](const char* text) {
        std::vector<u8> body;
        net::ByteWriter(body)
            .u8v(0x00)
            .u32v(7)
            .u64v(kOther)
            .u64v(kOther)
            .u32v(static_cast<u32>(std::string_view(text).size() + 1))
            .cstring(text)
            .u8v(0);
        REQUIRE(play.handle(net::Opcode::SMSG_MESSAGECHAT, body, 1000) == game::Gameplay::Handled::Yes);
    };
    say("before");
    ChatBubbles bubbles;
    bubbles.update(play, kPlayer, 1000, kAll); // the lines already there are not shown
    CHECK(bubbles.count() == 0);
    say("hello there");
    bubbles.update(play, kPlayer, 1100, kAll);
    REQUIRE(bubbles.of(kOther) != nullptr);
    CHECK(bubbles.of(kOther)->view() == "hello there");
    // Both options off: nothing stays over the heads.
    bubbles.update(play, kPlayer, 1200, {false, false});
    CHECK(bubbles.count() == 0);
    // Another character in the world: a fresh start.
    say("again");
    bubbles.update(play, kPlayer + 1, 1300, kAll);
    CHECK(bubbles.count() == 0);
}

TEST_CASE("World tap: select, then act like a right click") {
    CHECK(worldTap(0, 0) == WorldTap::None);
    CHECK(worldTap(0, 42) == WorldTap::None);   // empty ground keeps the target
    CHECK(worldTap(42, 0) == WorldTap::Select); // a first tap selects
    CHECK(worldTap(42, 7) == WorldTap::Select); // another unit: selects it
    CHECK(worldTap(42, 42) == WorldTap::Act);   // the selected one (or the second tap of a double tap): acts
    // The act runs the context button's action only when the button works on that unit (not on an object at
    // hand the button offers because the target has nothing to do).
    CHECK(tapRunsContext(42, 42));
    CHECK_FALSE(tapRunsContext(42, 99));
    CHECK_FALSE(tapRunsContext(0, 0));
}

TEST_CASE("World tap: the game objects a tap uses, as the client's right click") {
    const auto object = [](u32 type, u32 flags = 0, u32 dynamic = 0) {
        game::Entity go;
        go.guid = 0xF110000000001234ull;
        go.type = game::ObjectType::GameObject;
        go.values.assign(net::uf::GAMEOBJECT_END, 0);
        go.values[net::uf::GAMEOBJECT_TYPE_ID] = type;
        go.values[net::uf::GAMEOBJECT_FLAGS] = flags;
        go.values[net::uf::GAMEOBJECT_DYN_FLAGS] = dynamic;
        return go;
    };
    // Doors, buttons, quest objects, chests (herbs and veins too), chairs, plaques (phase 13: text objects the client
    // reads itself), goobers, bobbers, rituals, mailboxes, portals, meeting stones and flags.
    for (u32 type : {0u, 1u, 2u, 3u, 7u, 9u, 10u, 13u, 17u, 18u, 19u, 22u, 23u, 24u, 26u}) {
        CHECK(worldObjectTappable(object(type)));
    }
    // Generic objects, traps, spell foci (anvils, forges), transports, fishing holes...
    for (u32 type : {4u, 5u, 6u, 8u, 11u, 12u, 14u, 15u, 16u, 20u, 21u, 25u, 29u}) {
        CHECK_FALSE(worldObjectTappable(object(type)));
    }
    CHECK_FALSE(worldObjectTappable(object(3, 0x10)));  // GO_FLAG_NO_INTERACT
    CHECK_FALSE(worldObjectTappable(object(3, 0x04)));  // GO_FLAG_INTERACT_COND: a quest's, not for us
    CHECK(worldObjectTappable(object(3, 0x04, 0x01)));  // ... until the server activates it for this player
    CHECK(worldObjectTappable(object(0, 0x01 | 0x02))); // in use / locked: still clicked (the server answers)
    game::Entity unit = object(3);
    unit.type = game::ObjectType::Unit;
    CHECK_FALSE(worldObjectTappable(unit));
}

namespace {

struct Floor {
    f32 slope = 0.0f; // height = base + slope * x
    f32 base = 100.0f;
    bool none = false; // no floor anywhere
    mutable u32 queries = 0;
};

std::optional<f32> floorQuery(const void* context, f32 x, f32, f32 top, f32 bottom) {
    const Floor& f = *static_cast<const Floor*>(context);
    ++f.queries;
    const f32 h = f.base + f.slope * x;
    if (f.none || h > top || h < bottom) {
        return std::nullopt;
    }
    return h;
}

} // namespace

TEST_CASE("SelectionCircle: size and tint") {
    CHECK(SelectionCircle::ringRadius(0.306f) == doctest::Approx(0.612f)); // a human
    CHECK(SelectionCircle::ringRadius(1.5f) == doctest::Approx(3.0f));
    CHECK(SelectionCircle::ringRadius(0.1f) == doctest::Approx(0.45f)); // never smaller than a critter's
    CHECK(SelectionCircle::ringRadius(0.0f) ==
          doctest::Approx(0.778f)); // no field: the server's default size
    CHECK(SelectionCircle::color(game::Reaction::Hostile) == ui::rgba(255, 0, 0));
    CHECK(SelectionCircle::color(game::Reaction::Neutral) == ui::rgba(255, 255, 0));
    CHECK(SelectionCircle::color(game::Reaction::Friendly) == ui::rgba(0, 255, 0));
}

TEST_CASE("SelectionCircle: a grid on the floor, camera-relative, bright side to the camera") {
    std::vector<ParticleVertex> out(SelectionCircle::kQuads * 4);
    Floor flat;
    SelectionCircle circle;
    const glm::dvec3 feet(10.0, 20.0, 100.0);
    const glm::dvec3 camera(0.0, 20.0, 105.0); // south of the unit (x is north), looking north
    const f32 ring = 0.88f;                    // half the quad: 1 yard
    REQUIRE(circle.build(feet, ring, 0xFF00FF00u, camera, {1.0f, 0.0f, 0.0f}, out, &floorQuery, &flat) ==
            SelectionCircle::kQuads);
    CHECK(flat.queries == (SelectionCircle::kCells + 1) * (SelectionCircle::kCells + 1));
    f32 minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;
    for (const ParticleVertex& v : out) {
        minX = std::min(minX, v.position[0]);
        maxX = std::max(maxX, v.position[0]);
        minZ = std::min(minZ, v.position[2]);
        maxZ = std::max(maxZ, v.position[2]);
        CHECK(v.color == 0xFF00FF00u);
    }
    CHECK(minX == doctest::Approx(9.0f)); // feet - camera = (10, 0, -5), +- 1 yard
    CHECK(maxX == doctest::Approx(11.0f));
    // On the floor, lifted a little (more far away): 0.03 + 0.0015 * 11.18 yards.
    CHECK(minZ == doctest::Approx(-5.0f + 0.03f + 0.0015f * std::sqrt(125.0f)));
    CHECK(maxZ == doctest::Approx(minZ));
    // The texture's bottom (v = 1, its bright side) is the edge nearest the camera (south: low x).
    for (const ParticleVertex& v : out) {
        if (v.position[0] < 9.01f) {
            CHECK(v.uv[1] == 65535);
        }
        if (v.position[0] > 10.99f) {
            CHECK(v.uv[1] == 0);
        }
    }

    // Standing still: the floor is not sampled again; moving, it is.
    flat.queries = 0;
    REQUIRE(circle.build(feet, ring, 0xFF00FF00u, camera, {1.0f, 0.0f, 0.0f}, out, &floorQuery, &flat) > 0);
    CHECK(flat.queries == 0);
    REQUIRE(circle.build(feet + glm::dvec3(0.5, 0.0, 0.0), ring, 0xFF00FF00u, camera, {1.0f, 0.0f, 0.0f}, out,
                         &floorQuery, &flat) > 0);
    CHECK(flat.queries > 0);
    // No room, no size: nothing.
    std::vector<ParticleVertex> small(4);
    CHECK(circle.build(feet, ring, 0, camera, {1.0f, 0.0f, 0.0f}, small, &floorQuery, &flat) == 0);
    CHECK(circle.build(feet, 0.0f, 0, camera, {1.0f, 0.0f, 0.0f}, out, &floorQuery, &flat) == 0);
}

TEST_CASE("SelectionCircle: follows a slope, lies flat with no floor at the feet") {
    std::vector<ParticleVertex> out(SelectionCircle::kQuads * 4);
    Floor slope;
    slope.slope = 0.5f;
    slope.base = 95.0f; // floor at x = 10: 100, the feet
    SelectionCircle circle;
    const glm::dvec3 feet(10.0, 20.0, 100.0);
    REQUIRE(circle.build(feet, 0.88f, 0, feet + glm::dvec3(-10.0, 0.0, 5.0), {1.0f, 0.0f, 0.0f}, out,
                         &floorQuery, &slope) > 0);
    for (const ParticleVertex& v : out) {
        const f32 x = 10.0f + v.position[0] - 10.0f; // camera at x 0: relative x = world x - 0
        const f32 expected = 95.0f + 0.5f * x - 105.0f;
        CHECK(v.position[2] == doctest::Approx(expected).epsilon(0.02));
    }
    // Swimming or jumping: no floor near the feet, the circle stays at the feet.
    Floor none;
    none.none = true;
    SelectionCircle flat;
    REQUIRE(flat.build(feet, 0.88f, 0, feet + glm::dvec3(-10.0, 0.0, 5.0), {1.0f, 0.0f, 0.0f}, out,
                       &floorQuery, &none) > 0);
    CHECK(none.queries == 1); // only the centre was looked at
    for (const ParticleVertex& v : out) {
        CHECK(v.position[2] == doctest::Approx(-5.0f + 0.03f + 0.0015f * std::sqrt(125.0f)));
    }
}
