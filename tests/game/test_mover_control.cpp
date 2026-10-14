// Phase 13 (combat): who the client moves (game::MoverControl through game::Gameplay) as WoW.exe 1.12.1 decides it,
// with the packets vmangos-core sends for a fear, a Mind Control, Eyes of the Beast and a mob charming the player,
// and what the client must answer in the order MovementHandler.cpp expects; the time skipped; the far sight.
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/MoverControl.h"
#include "net/ByteBuffer.h"
#include "net/ControlMessages.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <ostream>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using game::GameEventKind;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x21;
constexpr u64 kKobold = 0xF130000000000456ull; // a creature to Mind Control
constexpr u64 kPet = 0xF140000000000042ull;    // the hunter's pet

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    std::vector<u32> mask(maxIndex / 32 + 1, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(mask.size()));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
}

Bytes created(u64 guid, game::ObjectType type, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(10.0f).f32v(20.0f).f32v(30.0f).f32v(0.0f);
    writeValues(w, fields);
    return out;
}

Bytes values(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

// PLAYER_FARSIGHT (a GUID in two words).
Fields farsight(u64 guid) {
    return {{uf::PLAYER_FARSIGHT, static_cast<u32>(guid)}, {static_cast<u16>(uf::PLAYER_FARSIGHT + 1),
                                                             static_cast<u32>(guid >> 32)}};
}

Bytes controlUpdate(u64 guid, bool allow) {
    Bytes out;
    net::ByteWriter(out).packedGuid(guid).u8v(allow ? 1 : 0);
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
    u32 now = 1000;
    u32 eventCursor = 0;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kPlayer, 0, true, now);
        update(created(kPlayer, game::ObjectType::Player, {{uf::UNIT_FIELD_HEALTH, 100}}));
        update(created(kKobold, game::ObjectType::Unit, {{uf::UNIT_FIELD_HEALTH, 50}}));
        update(created(kPet, game::ObjectType::Unit, {{uf::UNIT_FIELD_HEALTH, 80}}));
        eventCursor = play.events().end();
        sent.clear();
    }
    void update(const Bytes& payload) {
        std::vector<u64> guids;
        game::UpdateStats stats;
        stats.createdGuids = &guids;
        REQUIRE(store.applyUpdateObject(payload, now, &stats));
        play.afterUpdate(guids, now);
    }
    void control(u64 guid, bool allow) {
        REQUIRE(play.handle(Opcode::SMSG_CLIENT_CONTROL_UPDATE, controlUpdate(guid, allow), now) ==
                game::Gameplay::Handled::Yes);
    }
    // A frame: the packets came, then Gameplay::update, then the controller switches if it must.
    void frame(const game::MoverControl::Switch& s = {}) {
        now += 16;
        play.update(now);
        if (play.control().switchPending()) {
            play.control().switchMover(s);
        }
    }
    bool event(GameEventKind kind) {
        while (const game::GameEvent* e = play.events().next(eventCursor)) {
            if (e->kind == kind) {
                return true;
            }
        }
        return false;
    }
    std::vector<Opcode> opcodes() const {
        std::vector<Opcode> out;
        for (const Sent& s : sent) {
            out.push_back(s.opcode);
        }
        return out;
    }
};

net::MovementInfo at(f32 x, f32 y, u32 flags = 0) {
    net::MovementInfo info;
    info.flags = flags;
    info.time = 5000;
    info.position = {x, y, 30.0f, 1.0f};
    return info;
}

u64 guidOf(const Bytes& body) {
    net::ByteReader r(body);
    return r.u64v();
}

} // namespace

TEST_CASE("Fear: the player stops being the mover, then gets it back with its last spline answered") {
    Harness h;
    CHECK(h.play.control().activeMover() == kPlayer);
    CHECK(h.play.control().playerHasControl());
    // FearMovementGenerator::Initialize -> Unit::UpdateControl -> SetClientControl(player, 0).
    h.control(kPlayer, false);
    CHECK(h.event(GameEventKind::ControlLost));
    CHECK_FALSE(h.play.control().playerHasControl());
    const net::MovementInfo running = at(10.0f, 20.0f, 0x1);
    game::MoverControl::Switch lost;
    lost.oldInfo = &running;
    h.frame(lost);
    // SetActiveMover(0): CMSG_MOVE_NOT_ACTIVE_MOVER with the player's last movement, no CMSG_SET_ACTIVE_MOVER.
    REQUIRE(h.opcodes() == std::vector<Opcode>{Opcode::CMSG_MOVE_NOT_ACTIVE_MOVER});
    Bytes expected;
    net::writeMoveNotActiveMover(expected, kPlayer, running);
    CHECK(h.sent[0].body == expected);
    CHECK(h.play.control().activeMover() == 0);
    CHECK(h.play.control().viewTarget() == kPlayer); // the camera stays on the player
    h.sent.clear();
    h.frame();
    CHECK(h.sent.empty()); // nothing more while feared

    // FearMovementGenerator::Finalize -> StopMoving (a stop spline, id 7) -> SetClientControl(player, 1).
    h.control(kPlayer, true);
    CHECK(h.event(GameEventKind::ControlGained));
    const net::MovementInfo stopped = at(14.0f, 22.0f);
    game::MoverControl::Switch back;
    back.newInfo = &stopped;
    back.newFinishedSpline = 7;
    h.frame(back);
    // SetActiveMover(player): CMSG_SET_ACTIVE_MOVER, then the spline that ended meanwhile is answered (the server
    // only clears HasPendingSplineDone for the mover it was told: WorldSession::HandleMoveSplineDoneOpcode).
    REQUIRE(h.opcodes() == std::vector<Opcode>{Opcode::CMSG_SET_ACTIVE_MOVER, Opcode::CMSG_MOVE_SPLINE_DONE});
    CHECK(guidOf(h.sent[0].body) == kPlayer);
    net::ByteReader r(h.sent[1].body);
    const net::MovementInfo done = net::readMovementInfo(r);
    CHECK(done.position.x == 14.0f);
    CHECK(r.u32v() == 7);
    CHECK(h.play.control().activeMover() == kPlayer);
}

TEST_CASE("Mind Control: the possessed unit becomes the mover once the camera is on it, and back") {
    Harness h;
    // Unit::ModPossess: SetView (PLAYER_FARSIGHT, a batched field update), SetCharm, SetMover; the target's
    // UpdateControl sends SetClientControl(target, 1) at once.
    h.control(kKobold, true);
    h.frame();
    CHECK(h.sent.empty()); // the camera is not on it yet: nothing changes (CGUnit_C::SetClientControl)
    CHECK(h.play.control().activeMover() == kPlayer);

    h.update(values(kPlayer, farsight(kKobold)));
    const net::MovementInfo me = at(10.0f, 20.0f);
    const net::MovementInfo kobold = at(30.0f, 40.0f);
    game::MoverControl::Switch possess;
    possess.oldInfo = &me;
    possess.newInfo = &kobold;
    possess.newFinishedSpline = 3; // its StopMoving() as the possession began
    h.frame(possess);
    // CMSG_FAR_SIGHT 1 (the view moves), NOT_ACTIVE_MOVER for the player, SET_ACTIVE_MOVER for the kobold, then its
    // stop spline answered.
    REQUIRE(h.opcodes() == std::vector<Opcode>{Opcode::CMSG_FAR_SIGHT, Opcode::CMSG_MOVE_NOT_ACTIVE_MOVER,
                                               Opcode::CMSG_SET_ACTIVE_MOVER, Opcode::CMSG_MOVE_SPLINE_DONE});
    CHECK(h.sent[0].body == Bytes{1});
    CHECK(guidOf(h.sent[1].body) == kPlayer);
    CHECK(guidOf(h.sent[2].body) == kKobold);
    CHECK(h.play.control().viewTarget() == kKobold);
    CHECK(h.play.control().activeMover() == kKobold);
    CHECK(h.play.control().playerHasControl()); // Mind Control does not take the caster's own control
    CHECK_FALSE(h.event(GameEventKind::ControlLost));

    // The time skipped is the mover's.
    h.sent.clear();
    h.play.control().timeSkipped(900);
    REQUIRE(h.opcodes() == std::vector<Opcode>{Opcode::CMSG_MOVE_TIME_SKIPPED});
    Bytes expected;
    net::writeMoveTimeSkipped(expected, kKobold, 900);
    CHECK(h.sent[0].body == expected);

    // ModPossess removal: UpdateControl (player, 1), SetClientControl(target, 0), ResetView (FARSIGHT 0 at once).
    h.sent.clear();
    h.control(kPlayer, true);
    h.control(kKobold, false);
    h.update(values(kPlayer, farsight(0)));
    const net::MovementInfo player = at(10.0f, 20.0f);
    game::MoverControl::Switch release;
    release.oldInfo = &kobold;
    release.newInfo = &player;
    h.frame(release);
    // CMSG_FAR_SIGHT 0, then SET_ACTIVE_MOVER(player): no NOT_ACTIVE_MOVER for a unit that is not the player
    // (CGUnit_C::SetActiveMover only sends it for the local player).
    REQUIRE(h.opcodes() == std::vector<Opcode>{Opcode::CMSG_FAR_SIGHT, Opcode::CMSG_SET_ACTIVE_MOVER});
    CHECK(h.sent[0].body == Bytes{0});
    CHECK(guidOf(h.sent[1].body) == kPlayer);
    CHECK(h.play.control().activeMover() == kPlayer);
    CHECK(h.play.control().viewTarget() == kPlayer);
}

TEST_CASE("Eyes of the Beast ends without a control packet: the far sight going back is enough") {
    Harness h;
    // Player::ModPossessPet: SetView(pet), SetCharm, SetMover, pet->UpdateControl -> SetClientControl(pet, 1).
    h.control(kPet, true);
    h.update(values(kPlayer, farsight(kPet)));
    h.frame();
    CHECK(h.play.control().activeMover() == kPet);
    // Removal: SetCharm(nullptr) first, so the pet's UpdateControl sends nothing; ResetView -> FARSIGHT 0. The
    // client goes back to the player, whose control flag never changed (HandleSetActiveMoverOpcode then clears the
    // pet's possessed state).
    h.sent.clear();
    h.update(values(kPlayer, farsight(0)));
    h.frame();
    REQUIRE(h.opcodes() == std::vector<Opcode>{Opcode::CMSG_FAR_SIGHT, Opcode::CMSG_SET_ACTIVE_MOVER});
    CHECK(guidOf(h.sent[1].body) == kPlayer);
}

TEST_CASE("Charmed by a mob, far sight on an object the client cannot move, a far teleport") {
    Harness h;
    // Aura::HandleModCharm on the player: UpdateControl -> SetClientControl(player, 0); the AI moves it.
    h.control(kPlayer, false);
    h.frame();
    CHECK(h.play.control().activeMover() == 0);
    CHECK(h.play.control().wantedMover() == 0);
    // A new map (the charm ends with it): the player moves itself again and the UI hears it.
    h.play.enterWorld(kPlayer, 1, false, h.now);
    CHECK(h.event(GameEventKind::ControlGained));
    CHECK(h.play.control().activeMover() == kPlayer);
    CHECK(h.play.control().playerHasControl());

    // Far Sight (a dynamic object the camera looks through): the player stays the mover.
    Harness f;
    constexpr u64 kEye = 0xF100000000000777ull; // HIGHGUID_DYNAMICOBJECT
    f.update(created(kEye, game::ObjectType::DynamicObject, {}));
    f.update(values(kPlayer, farsight(kEye)));
    f.frame();
    REQUIRE(f.opcodes() == std::vector<Opcode>{Opcode::CMSG_FAR_SIGHT});
    CHECK(f.play.control().viewTarget() == kEye);
    CHECK(f.play.control().activeMover() == kPlayer);
    CHECK_FALSE(f.play.control().switchPending());
    // Its object not here yet: the view stays on the player until it comes.
    Harness g;
    g.update(values(kPlayer, farsight(0xF100000000000999ull)));
    g.frame();
    CHECK(g.sent.empty());
    CHECK(g.play.control().viewTarget() == kPlayer);
}

TEST_CASE("Another client's time skipped: its unit's dead reckoning waits that long") {
    game::EntityStore store;
    Bytes created;
    net::ByteWriter w(created);
    // A player created running forward at (0, 0, 0) facing +X (UPDATEFLAG_LIVING: MovementInfo + 6 speeds).
    w.u32v(1).u8v(0).u8v(2).packedGuid(0x42).u8v(static_cast<u8>(game::ObjectType::Player)).u8v(0x20);
    net::MovementInfo running;
    running.flags = 0x1;
    running.time = 100;
    net::writeMovementInfo(w, running);
    for (const f32 speed : {2.5f, 7.0f, 4.5f, 4.72f, 2.5f, 3.14f}) {
        w.f32v(speed);
    }
    w.u8v(1).u32v(0);
    REQUIRE(store.applyUpdateObject(created, 1000));
    const game::Entity* e = store.find(0x42);
    REQUIRE(e);
    CHECK(e->positionAt(1500).x == doctest::Approx(3.5f)); // half a second at 7 yd/s
    // Its client paused 2 s (relayed MSG_MOVE_TIME_SKIPPED): its last packet now counts as 2 s later.
    Bytes skipped;
    net::ByteWriter(skipped).packedGuid(0x42).u32v(2000);
    REQUIRE(store.applyTimeSkipped(skipped));
    CHECK(e->positionAt(1500).x == doctest::Approx(0.0f));
    CHECK(e->positionAt(3500).x == doctest::Approx(3.5f));
    CHECK_FALSE(store.applyTimeSkipped(std::span<const u8>(skipped.data(), skipped.size() - 1)));
}
