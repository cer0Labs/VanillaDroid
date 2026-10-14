#include "anim/Animator.h"
#include "anim/Skeleton.h"
#include "bake/ActorBaker.h"
#include "formats/M2.h"
#include "pack/ActorFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace {

using vd::f32;
using vd::i32;
using vd::u16;
using vd::u32;
using vd::u8;

// A baked blob in 16-byte aligned memory, as the runtime gets it from the pack reader.
struct AlignedBlob {
    std::unique_ptr<u8[]> storage;
    u8* data = nullptr;
    vd::usize size = 0;
    explicit AlignedBlob(const std::vector<u8>& bytes) : storage(new u8[bytes.size() + 16]), size(bytes.size()) {
        data = storage.get() + (16 - reinterpret_cast<uintptr_t>(storage.get()) % 16) % 16;
        std::memcpy(data, bytes.data(), bytes.size());
    }
};

// A run cycle's foot downs: "$RL0" and "$RR0" (walking ones are "$FL0" / "$FR0").
bool runFoot(u32 id) {
    return id == vd::pack::actorEventId("$RL0") || id == vd::pack::actorEventId("$RR0");
}

} // namespace

TEST_CASE("M2 events: ids as stored") {
    static_assert(vd::pack::actorEventId("$FD1") == 0x31444624u);
    CHECK(vd::pack::actorEventId("$CSD") == (u32('$') | u32('C') << 8 | u32('S') << 16 | u32('D') << 24));
}

TEST_CASE("M2 events: HumanMale's run foot downs, baked per sequence and crossed by the animator") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<std::vector<u8>> bytes = mpq->read("Character\\Human\\Male\\HumanMale.m2");
    REQUIRE(bytes.ok());
    vd::Result<vd::formats::M2Model> model = vd::formats::parseM2(bytes.value());
    REQUIRE(model.ok());
    const vd::formats::M2Model& m = model.value();
    REQUIRE(!m.events.empty());
    for (const vd::formats::M2Event& e : m.events) {
        CHECK((e.id & 0xFF) == '$'); // every id starts with '$' when the record layout is right
        CHECK(e.bone < m.bones.size());
    }

    vd::Result<std::vector<u8>> baked = vd::bake::bakeActor(m);
    REQUIRE(baked.ok());
    const AlignedBlob blob(baked.value());
    vd::anim::Skeleton skeleton;
    REQUIRE(skeleton.bind(blob.data, blob.size).ok());
    REQUIRE(skeleton.events().size() == m.events.size());
    CHECK(skeleton.hasTimedEvents());

    // Foot downs: the run cycle (667 ms) has one for each foot, the right one at 33 ms and the left at 367 ms.
    const i32 run = skeleton.findSequence(vd::anim::animation::kRun);
    REQUIRE(run >= 0);
    const u32 duration = skeleton.duration(static_cast<u32>(run));
    u32 feet = 0;
    for (u32 e = 0; e < skeleton.events().size(); ++e) {
        const std::span<const u32> times = skeleton.eventTimes(e, static_cast<u32>(run));
        for (u32 k = 0; k < times.size(); ++k) {
            CHECK(times[k] <= duration);
            if (k > 0) {
                CHECK(times[k] > times[k - 1]);
            }
        }
        if (runFoot(skeleton.events()[e].id)) {
            feet += static_cast<u32>(times.size());
            CHECK(times.size() == 1);
            if (!times.empty()) {
                CHECK(times[0] == (skeleton.events()[e].id == vd::pack::actorEventId("$RR0") ? 33u : 367u));
            }
        }
    }
    CHECK(feet == 2);

    // Playing the run over three cycles in uneven steps crosses each foot down exactly three times.
    vd::anim::Animator animator;
    vd::anim::AnimFallbacks fallbacks;
    u32 random = 1;
    animator.play(skeleton, vd::anim::animation::kRun, true, fallbacks, random);
    u32 crossed = 0;
    f32 played = 0.0f;
    const f32 total = 3.0f * static_cast<f32>(duration) / 1000.0f;
    for (u32 step = 0; played + 1e-4f < total; ++step) {
        const f32 dt = std::min(total - played, step % 3 == 0 ? 0.031f : 0.0127f);
        played += dt;
        animator.update(skeleton, dt, 1.0f, random);
        for (const vd::anim::Animator::Played& piece : animator.played()) {
            CHECK(piece.sequence == run);
            CHECK(piece.from <= piece.to);
            for (u32 e = 0; e < skeleton.events().size(); ++e) {
                if (!runFoot(skeleton.events()[e].id)) {
                    continue;
                }
                for (const u32 t : skeleton.eventTimes(e, static_cast<u32>(run))) {
                    crossed += t >= piece.from && t < piece.to ? 1 : 0;
                }
            }
        }
    }
    CHECK(crossed == 3 * feet);
}

TEST_CASE("Animator: a one-shot plays its last instant once, then nothing") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<std::vector<u8>> bytes = mpq->read("Character\\Human\\Male\\HumanMale.m2");
    REQUIRE(bytes.ok());
    vd::Result<vd::formats::M2Model> model = vd::formats::parseM2(bytes.value());
    REQUIRE(model.ok());
    vd::Result<std::vector<u8>> baked = vd::bake::bakeActor(model.value());
    REQUIRE(baked.ok());
    const AlignedBlob blob(baked.value());
    vd::anim::Skeleton skeleton;
    REQUIRE(skeleton.bind(blob.data, blob.size).ok());
    vd::anim::Animator animator;
    vd::anim::AnimFallbacks fallbacks;
    u32 random = 1;
    animator.play(skeleton, vd::anim::animation::kDeath, false, fallbacks, random);
    const i32 death = skeleton.findSequence(vd::anim::animation::kDeath);
    REQUIRE(death >= 0);
    const u32 duration = skeleton.duration(static_cast<u32>(death));
    u32 reached = 0;
    for (u32 step = 0; step < 400; ++step) {
        animator.update(skeleton, 0.02f, 1.0f, random);
        for (const vd::anim::Animator::Played& piece : animator.played()) {
            reached += piece.to > duration ? 1 : 0;
        }
    }
    CHECK(animator.finished());
    CHECK(reached == 1);
    animator.update(skeleton, 0.02f, 1.0f, random);
    CHECK(animator.played().empty());
}
