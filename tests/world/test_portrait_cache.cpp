// Parity: the unit frames' portrait slots (world/PortraitCache): look keys, the least recently used slot for
// a new look, dirty -> rendered -> ready once the frame reached the GPU, what is kept while a unit's new look
// is drawn, invalidation; and how a portrait camera projects its model.
#include "world/PortraitCache.h"

#include <doctest/doctest.h>

#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>

using namespace vd;

namespace {

constexpr u32 kNone = PortraitCache::kNone;

game::ActorLook humanLook() {
    game::ActorLook look;
    look.model = 0x1234;
    look.textures[3] = 0x77; // hair
    look.skin = {{0x10, game::SkinRegion::Base, 0}, {0x11, game::SkinRegion::FaceLower, 1}};
    look.geosets = {0, 1, 101, 401, 501};
    look.attachments = {{11, 0x99, 0x98, 0xFF}};
    return look;
}

} // namespace

TEST_CASE("Portrait look keys: equal looks share a key, any visible change makes another") {
    const game::ActorLook a = humanLook();
    const u64 key = portraitLookKey(a);
    CHECK(key != 0);
    CHECK(portraitLookKey(humanLook()) == key);
    game::ActorLook scaled = a;
    scaled.scale = 2.5f; // the portrait camera frames the model itself: the scale does not matter
    CHECK(portraitLookKey(scaled) == key);

    game::ActorLook b = a;
    b.model = 0x1235; // another display, a shapeshift
    CHECK(portraitLookKey(b) != key);
    b = a;
    b.textures[3] = 0x78; // another hair color
    CHECK(portraitLookKey(b) != key);
    b = a;
    b.skin[1].texture = 0x12; // another face
    CHECK(portraitLookKey(b) != key);
    b = a;
    b.geosets.push_back(1501); // a cape
    CHECK(portraitLookKey(b) != key);
    b = a;
    b.attachments.clear(); // the helm hidden
    CHECK(portraitLookKey(b) != key);
    b = a;
    b.attachments[0].texture = 0x97;
    CHECK(portraitLookKey(b) != key);
    // An empty look still has a key (never 0: 0 means "nothing shown" to the frames).
    CHECK(portraitLookKey(game::ActorLook{}) != 0);
}

TEST_CASE("Portrait cache: a new look is dirty until rendered, ready once that frame was submitted") {
    PortraitCache cache(4);
    cache.beginFrame(0);
    const u32 slot = cache.request(100, 7);
    REQUIRE(slot != kNone);
    CHECK(!cache.ready(slot));
    CHECK(cache.key(slot) == 100);
    CHECK(cache.subject(slot) == 7);
    std::array<u32, 2> dirty{};
    REQUIRE(cache.dirty(dirty) == 1);
    CHECK(dirty[0] == slot);
    // Rendered into frame 1 (the one being prepared): not ready in that frame's UI yet.
    cache.rendered(slot, 1);
    CHECK(!cache.ready(slot));
    CHECK(cache.dirty(dirty) == 0);
    // The frame never reached the GPU (no surface, a swapchain rebuild): dirty again.
    cache.beginFrame(0);
    CHECK(cache.request(100, 7) == slot);
    CHECK(!cache.ready(slot));
    REQUIRE(cache.dirty(dirty) == 1);
    cache.rendered(slot, 1);
    // Submitted: ready from the next frame on, and no longer dirty.
    cache.beginFrame(1);
    CHECK(cache.request(100, 7) == slot);
    CHECK(cache.ready(slot));
    CHECK(cache.dirty(dirty) == 0);
    // Not asked for a while: it stays ready (nothing redraws a look that did not change).
    for (u64 frame = 2; frame < 10; ++frame) {
        cache.beginFrame(frame);
    }
    CHECK(cache.request(100, 7) == slot);
    CHECK(cache.ready(slot));
}

TEST_CASE("Portrait cache: identical looks share an image; the least recently used slot goes to a new look") {
    PortraitCache cache(3);
    cache.beginFrame(0);
    const u32 a = cache.request(1, 10);
    CHECK(cache.request(1, 11) == a); // a second wolf of the same display: the same image
    CHECK(cache.subject(a) == 11);    // drawn from the last one that asked
    cache.beginFrame(0);
    const u32 b = cache.request(2, 20);
    cache.beginFrame(0);
    const u32 c = cache.request(3, 30);
    CHECK(a != b);
    CHECK(b != c);
    CHECK(a != c);
    CHECK(cache.slotsInUse() == 3);
    // Look 1 is the oldest: a fourth look takes its slot.
    cache.beginFrame(0);
    CHECK(cache.request(2, 20) == b);
    const u32 d = cache.request(4, 40);
    CHECK(d == a);
    CHECK(cache.find(1) == kNone);
    CHECK(cache.key(d) == 4);
    CHECK(!cache.ready(d));
    // Every slot asked for this frame: nothing to give.
    CHECK(cache.request(3, 30) == c);
    CHECK(cache.request(5, 50) == kNone);
    CHECK(cache.find(2) == b);
}

TEST_CASE("Portrait cache: dirty slots in the order asked, as many as the frame's budget") {
    PortraitCache cache(8);
    cache.beginFrame(0);
    const u32 target = cache.request(10, 1);
    const u32 player = cache.request(11, 2);
    const u32 party = cache.request(12, 3);
    std::array<u32, 2> dirty{};
    REQUIRE(cache.dirty(dirty) == 2);
    CHECK(dirty[0] == target);
    CHECK(dirty[1] == player);
    cache.rendered(target, 1);
    cache.rendered(player, 1);
    REQUIRE(cache.dirty(dirty) == 1);
    CHECK(dirty[0] == party);
    // A dirty slot nobody asked for this frame is not drawn.
    cache.beginFrame(1);
    CHECK(cache.dirty(dirty) == 0);
    cache.request(12, 3);
    REQUIRE(cache.dirty(dirty) == 1);
    CHECK(dirty[0] == party);
}

TEST_CASE("Portrait cache: claimed slots are drawn first, this frame, within the frame's budget") {
    PortraitCache cache(8);
    cache.beginFrame(0);
    const u32 party = cache.request(10, 1);
    const u32 target = cache.request(11, 2);
    const u32 pet = cache.request(12, 3);
    // The new target's portrait is claimed (drawn before the interface shows it), then the pet's; the budget
    // is 2.
    CHECK(cache.claim(target, 2));
    CHECK(cache.claim(target, 2)); // claiming again costs nothing
    CHECK(cache.claim(pet, 2));
    CHECK(!cache.claim(party, 2));
    CHECK(cache.claimed(target));
    CHECK(!cache.claimed(party));
    std::array<u32, 3> dirty{};
    REQUIRE(cache.dirty(dirty) == 3);
    CHECK(dirty[0] == target); // claimed ones first (in the order asked), then the rest
    CHECK(dirty[1] == pet);
    CHECK(dirty[2] == party);
    // A ready slot, or one not asked for this frame, cannot be claimed.
    cache.rendered(target, 1);
    cache.rendered(pet, 1);
    cache.beginFrame(1);
    CHECK(!cache.claimed(target));
    CHECK(cache.request(11, 2) == target);
    CHECK(!cache.claim(target, 2));
    CHECK(!cache.claim(party, 2)); // dirty, but not asked this frame
    CHECK(cache.request(10, 1) == party);
    CHECK(cache.claim(party, 2));
}

TEST_CASE("Portrait cache: the previous look's image is kept while the new one is drawn") {
    PortraitCache cache(2);
    cache.beginFrame(0);
    const u32 old = cache.request(1, 5);
    cache.rendered(old, 1);
    cache.beginFrame(1);
    REQUIRE(cache.ready(old));
    // The unit put a helm on: look 2. Its old image is touched first, so the new look cannot take its slot.
    cache.beginFrame(2);
    cache.request(9, 6); // another unit fills the other slot
    cache.beginFrame(3);
    REQUIRE(cache.find(1) == old);
    cache.touch(old);
    const u32 fresh = cache.request(2, 5);
    CHECK(fresh != old);
    CHECK(cache.ready(old));
    CHECK(cache.key(old) == 1);
    // Untouched, the old image would have been the one replaced.
    PortraitCache other(2);
    other.beginFrame(0);
    const u32 o = other.request(1, 5);
    other.rendered(o, 1);
    other.beginFrame(1);
    other.request(9, 6);
    other.beginFrame(2);
    CHECK(other.request(2, 5) == o);
}

TEST_CASE("Portrait cache: lost images are drawn again; unframeable looks stay on their stand-in art") {
    PortraitCache cache(2);
    cache.beginFrame(0);
    const u32 a = cache.request(1, 1);
    const u32 b = cache.request(2, 2);
    cache.rendered(a, 1);
    cache.failed(b);
    cache.beginFrame(1);
    CHECK(cache.ready(a));
    CHECK(!cache.ready(b));
    std::array<u32, 2> dirty{};
    cache.request(1, 1);
    cache.request(2, 2);
    CHECK(cache.dirty(dirty) == 0); // a failed look is not drawn again and again
    // The renderer made its portrait images again (another swapchain format): what was ready must be redrawn.
    cache.invalidate();
    CHECK(!cache.ready(a));
    REQUIRE(cache.dirty(dirty) == 1);
    CHECK(dirty[0] == a);
}

TEST_CASE("Portrait camera: the target at the image's center, the view as wide as the diagonal field of view") {
    // HumanMale's portrait camera (actors.vdpk, from the M2): a head about 0.42 yards tall fills the image.
    pack::ActorPortrait camera{};
    camera.position[0] = 0.633f;
    camera.position[1] = -0.388f;
    camera.position[2] = 1.887f;
    camera.target[0] = 0.063f;
    camera.target[1] = 0.034f;
    camera.target[2] = 1.864f;
    camera.fov = 0.785f;
    camera.nearClip = 0.222f;
    camera.farClip = 27.78f;
    const glm::mat4 viewProj = portraitViewProjection(camera);
    const glm::vec3 eye(camera.position[0], camera.position[1], camera.position[2]);
    const glm::vec3 target(camera.target[0], camera.target[1], camera.target[2]);
    // Camera-relative, as the instances are placed.
    const auto project = [&](const glm::vec3& p) {
        const glm::vec4 clip = viewProj * glm::vec4(p - eye, 1.0f);
        return glm::vec3(clip) / clip.w;
    };
    const glm::vec3 center = project(target);
    CHECK(center.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(center.y == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(center.z > 0.0f);
    CHECK(center.z < 1.0f);
    // Up in the world is up in the image (Vulkan: y down).
    CHECK(project(target + glm::vec3(0.0f, 0.0f, 0.1f)).y < 0.0f);
    // The model's right (-Y: it faces +X, the camera in front) is on the image's left.
    CHECK(project(target + glm::vec3(0.0f, -0.1f, 0.0f)).x < 0.0f);
    // Half the image's height at the target: distance * tan(fov / 2) / sqrt(2).
    const f32 half = glm::length(eye - target) * std::tan(camera.fov * 0.5f) / std::sqrt(2.0f);
    const glm::vec3 forward = glm::normalize(target - eye);
    const glm::vec3 up = glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f) - forward * forward.z);
    CHECK(project(target + up * half).y == doctest::Approx(-1.0f).epsilon(1e-3));
    CHECK(half == doctest::Approx(0.208f).epsilon(0.01));
    // The key light comes from the camera's side, above, on the image's left.
    const glm::vec3 light = portraitKeyLight(camera);
    CHECK(glm::length(light) == doctest::Approx(1.0f));
    CHECK(glm::dot(light, -forward) > 0.5f);
    CHECK(light.z > 0.2f);
    CHECK(project(target + light * 0.1f).x < 0.0f);
}

TEST_CASE("Model frames frame the whole model at every rotation (phase 15)") {
    // A standing human's box (yards), a wolf's (long and low) and a tall thin model.
    const std::array<std::array<glm::vec3, 2>, 3> boxes{{{glm::vec3(-0.35f, -0.55f, 0.0f), glm::vec3(0.45f, 0.55f, 1.95f)},
                                                         {glm::vec3(-1.1f, -0.3f, 0.0f), glm::vec3(1.0f, 0.3f, 1.1f)},
                                                         {glm::vec3(-0.2f, -0.2f, 0.0f), glm::vec3(0.2f, 0.2f, 6.0f)}}};
    const f32 aspect = 512.0f / 640.0f;
    for (const auto& box : boxes) {
        const ModelViewCamera camera = modelViewCamera(box[0], box[1], aspect);
        // In front of the model (+X), looking straight back at it.
        CHECK(camera.eye.x > box[1].x);
        CHECK(camera.eye.y == doctest::Approx(0.0f));
        CHECK(camera.eye.z == doctest::Approx(camera.target.z));
        const auto project = [&](const glm::vec3& p) {
            const glm::vec4 clip = camera.viewProj * glm::vec4(p - camera.eye, 1.0f);
            return glm::vec3(clip) / clip.w;
        };
        f32 tallest = 0.0f;
        for (u32 step = 0; step < 16; ++step) {
            const f32 yaw = static_cast<f32>(step) * 0.3927f;
            const f32 c = std::cos(yaw), s = std::sin(yaw);
            for (u32 corner = 0; corner < 8; ++corner) {
                const glm::vec3 p((corner & 1) ? box[1].x : box[0].x, (corner & 2) ? box[1].y : box[0].y,
                                  (corner & 4) ? box[1].z : box[0].z);
                const glm::vec3 q = project(glm::vec3(c * p.x - s * p.y, s * p.x + c * p.y, p.z));
                CHECK(std::abs(q.x) <= 1.0f);
                CHECK(std::abs(q.y) <= 1.0f);
                CHECK(q.z > 0.0f);
                CHECK(q.z < 1.0f);
                tallest = std::max(tallest, std::abs(q.y));
            }
        }
        // ...and fills it: the model is not lost in the middle of the image; it stands on the image's floor.
        CHECK(tallest > 0.6f);
        const f32 feet = project(glm::vec3(0.0f, 0.0f, box[0].z)).y;
        CHECK(feet > 0.7f);
        CHECK(feet < 0.97f);
        // Closer with a zoom.
        const ModelViewCamera zoomed = modelViewCamera(box[0], box[1], aspect, 1.5f);
        CHECK(zoomed.eye.x < camera.eye.x);
        // Up is up (Vulkan: y down); the viewer's right is +Y; the light comes from the front left, above.
        CHECK(project(camera.target + glm::vec3(0.0f, 0.0f, 0.1f)).y < 0.0f);
        CHECK(project(camera.target + glm::vec3(0.0f, 0.1f, 0.0f)).x > 0.0f);
        CHECK(camera.lightDirection.x > 0.5f);
        CHECK(camera.lightDirection.y < 0.0f);
        CHECK(camera.lightDirection.z > 0.2f);
    }
}
