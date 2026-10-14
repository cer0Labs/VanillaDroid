// Retouches (ui models): the 1.12 interface's models drawn by ui/UiModels from the baked ui.vdpk ($VD_PACKS_DIR).

#include "ui/DrawList.h"
#include "ui/UiAssets.h"
#include "ui/UiModels.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

using namespace vd;
using namespace vd::ui;

namespace {

bool openPack(Assets& assets) {
    const char* dir = std::getenv("VD_PACKS_DIR");
    const std::filesystem::path pack = dir ? std::filesystem::path(dir) / "ui.vdpk" : std::filesystem::path();
    return dir && std::filesystem::exists(pack) && assets.open(pack);
}

f32 cross(f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

bool inside(const UiVertex& a, const UiVertex& b, const UiVertex& c, f32 x, f32 y) {
    const f32 d0 = cross(a.x, a.y, b.x, b.y, x, y), d1 = cross(b.x, b.y, c.x, c.y, x, y);
    const f32 d2 = cross(c.x, c.y, a.x, a.y, x, y);
    const bool negative = d0 < 0.0f || d1 < 0.0f || d2 < 0.0f, positive = d0 > 0.0f || d1 > 0.0f || d2 > 0.0f;
    return !(negative && positive);
}

// The most opaque of the non-additive quads over a point (0..1; 0: none): how dark the cooldown leaves it.
f32 darkness(const DrawList& draw, f32 x, f32 y) {
    const std::span<const UiVertex> v = draw.vertices();
    f32 best = 0.0f;
    for (usize q = 0; q + 3 < v.size(); q += 4) {
        if ((v[q].params >> 24) & 1u) {
            continue;
        }
        if (inside(v[q], v[q + 1], v[q + 2], x, y) || inside(v[q], v[q + 2], v[q + 3], x, y)) {
            const f32 alpha = static_cast<f32>(v[q].color >> 24) / 255.0f;
            best = std::max(best, alpha);
        }
    }
    return best;
}

u32 additiveQuads(const DrawList& draw) {
    u32 n = 0;
    const std::span<const UiVertex> v = draw.vertices();
    for (usize q = 0; q + 3 < v.size(); q += 4) {
        n += (v[q].params >> 24) & 1u;
    }
    return n;
}

} // namespace

TEST_CASE("UI models: the cooldown's model sweeps clockwise from twelve o'clock (VD_PACKS_DIR)") {
    Assets assets;
    if (!openPack(assets)) {
        MESSAGE("VD_PACKS_DIR not set or ui.vdpk missing: skipping");
        return;
    }
    DrawList draw;
    draw.init(&assets, 4096);
    UiModels& models = draw.models();
    REQUIRE(models.ready());
    // A 36-unit Cooldown frame at 10 pixels a unit, its centre (180, 180), y down.
    const Rect frame{0.0f, 0.0f, 360.0f, 360.0f};
    const auto at = [&](f32 remaining) {
        draw.reset();
        models.cooldown(draw, 7, frame, 10.0f, remaining, remaining * 10.0f, 1000);
    };
    // cooldown.blp's dark texels: black at 0x99 alpha.
    const f32 dark = 153.0f / 255.0f;
    at(1.0f); // the whole button
    CHECK(darkness(draw, 90.0f, 90.0f) == doctest::Approx(dark).epsilon(0.02));
    CHECK(darkness(draw, 270.0f, 90.0f) == doctest::Approx(dark).epsilon(0.02));
    CHECK(darkness(draw, 270.0f, 270.0f) == doctest::Approx(dark).epsilon(0.02));
    CHECK(darkness(draw, 90.0f, 270.0f) == doctest::Approx(dark).epsilon(0.02));
    at(0.875f); // an eighth gone: from twelve to half past one
    CHECK(darkness(draw, 200.0f, 60.0f) == 0.0f);   // near twelve o'clock
    CHECK(darkness(draw, 300.0f, 160.0f) > 0.5f);   // near three
    at(0.75f); // a quarter: the top right uncovered
    CHECK(darkness(draw, 270.0f, 90.0f) == 0.0f);
    CHECK(darkness(draw, 270.0f, 270.0f) > 0.5f);
    CHECK(darkness(draw, 90.0f, 270.0f) > 0.5f);
    CHECK(darkness(draw, 90.0f, 90.0f) > 0.5f);
    at(0.5f); // half: the right half
    CHECK(darkness(draw, 270.0f, 90.0f) == 0.0f);
    CHECK(darkness(draw, 270.0f, 270.0f) == 0.0f);
    CHECK(darkness(draw, 90.0f, 270.0f) > 0.5f);
    CHECK(darkness(draw, 90.0f, 90.0f) > 0.5f);
    at(0.25f); // three quarters: only the top left
    CHECK(darkness(draw, 90.0f, 270.0f) == 0.0f);
    CHECK(darkness(draw, 90.0f, 90.0f) > 0.5f);
    at(0.04f); // nearly done: almost nothing
    CHECK(darkness(draw, 90.0f, 90.0f) == 0.0f);
    CHECK(darkness(draw, 172.0f, 20.0f) > 0.5f); // the last sliver left of twelve
}

TEST_CASE("UI models: a cooldown that runs out flashes, one cut short does not (VD_PACKS_DIR)") {
    Assets assets;
    if (!openPack(assets)) {
        MESSAGE("VD_PACKS_DIR not set or ui.vdpk missing: skipping");
        return;
    }
    DrawList draw;
    draw.init(&assets, 4096);
    UiModels& models = draw.models();
    REQUIRE(models.ready());
    const Rect frame{0.0f, 0.0f, 36.0f, 36.0f};
    // Ran out: the seconds known.
    draw.reset();
    models.cooldown(draw, 1, frame, 1.0f, 0.5f, 1.0f, 10000);
    CHECK(additiveQuads(draw) == 0);
    draw.reset();
    models.cooldown(draw, 1, frame, 1.0f, 0.0f, 0.0f, 11000);
    draw.reset();
    models.cooldown(draw, 1, frame, 1.0f, 0.0f, 0.0f, 11400); // the blue star of its Death sequence
    CHECK(additiveQuads(draw) > 0);
    draw.reset();
    models.cooldown(draw, 1, frame, 1.0f, 0.0f, 0.0f, 12100); // over (1000 ms): hidden
    CHECK(draw.quadCount() == 0);
    // Cut short (CooldownFrame_SetTimer with start 0: Hide).
    draw.reset();
    models.cooldown(draw, 2, frame, 1.0f, 0.5f, 5.0f, 10000);
    draw.reset();
    models.cooldown(draw, 2, frame, 1.0f, 0.0f, 0.0f, 10100);
    draw.reset();
    models.cooldown(draw, 2, frame, 1.0f, 0.0f, 0.0f, 10400);
    CHECK(draw.quadCount() == 0);
    // Only the share known (the bags, the spell book): the end comes from how it falls.
    draw.reset();
    models.cooldown(draw, 3, frame, 1.0f, 0.5f, -1.0f, 10000);
    models.cooldown(draw, 3, frame, 1.0f, 0.45f, -1.0f, 10100); // 1 s left
    draw.reset();
    models.cooldown(draw, 3, frame, 1.0f, 0.0f, -1.0f, 11050);
    draw.reset();
    models.cooldown(draw, 3, frame, 1.0f, 0.0f, -1.0f, 11400);
    CHECK(additiveQuads(draw) > 0);
}

TEST_CASE("UI models: the autocast's particles and the minimap's ping (VD_PACKS_DIR)") {
    Assets assets;
    if (!openPack(assets)) {
        MESSAGE("VD_PACKS_DIR not set or ui.vdpk missing: skipping");
        return;
    }
    DrawList draw;
    draw.init(&assets, 16384);
    UiModels& models = draw.models();
    REQUIRE(models.ready());
    // Four emitters, 300 particles a second living 1 s: after a second, every one alive (the HUD draws a third).
    const Rect button{100.0f, 100.0f, 30.0f, 30.0f};
    models.setParticleLimit(0);
    draw.reset();
    models.autocast(draw, 9, button, 1.0f, 1.2f, 50000);
    CHECK(draw.quadCount() <= 4);
    draw.reset();
    models.autocast(draw, 9, button, 1.0f, 1.2f, 50200);
    const u32 early = draw.quadCount();
    for (u64 t = 50300; t <= 51200; t += 100) { // drawn every frame: the same start
        draw.reset();
        models.autocast(draw, 9, button, 1.0f, 1.2f, t);
    }
    CHECK(early > 200);
    CHECK(draw.quadCount() >= 1150);
    CHECK(draw.quadCount() <= 1210);
    CHECK(additiveQuads(draw) == draw.quadCount());
    models.setParticleLimit(UiModels::kParticleLimit);
    draw.reset();
    models.autocast(draw, 9, button, 1.0f, 1.2f, 51300);
    CHECK(draw.quadCount() <= 4 * UiModels::kParticleLimit + 4);
    // Their sparks stay round the button's edge (the bones' square path, 0..0.02 model units at scale 1.2: 30.7 units).
    for (const UiVertex& v : draw.vertices()) {
        CHECK(v.x > button.x - 10.0f);
        CHECK(v.x < button.right() + 12.0f);
    }

    // The ping: nothing at its very start (its Birth fades the arrows and the dot in), then the ripple in the Stand.
    draw.reset();
    models.minimapPing(draw, 300.0f, 300.0f, 2.0f, 0.0f);
    CHECK(draw.quadCount() == 0);
    draw.reset();
    models.minimapPing(draw, 300.0f, 300.0f, 2.0f, 700.0f);
    const u32 birth = draw.quadCount();
    CHECK(birth > 0);
    draw.reset();
    models.minimapPing(draw, 300.0f, 300.0f, 2.0f, 1750.0f);
    CHECK(draw.quadCount() > birth);
    CHECK(additiveQuads(draw) == draw.quadCount());
    draw.reset();
    models.minimapPing(draw, 300.0f, 300.0f, 2.0f, 1000.0f, 0.0f); // the frame hidden
    CHECK(draw.quadCount() == 0);
}
