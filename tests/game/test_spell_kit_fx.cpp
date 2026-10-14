// Phase 14 (spellfx): the kits' procs and shakes (game/SpellKitFx) on real rows of the 1.12.1 client, and the
// decoding of the procs' parameters.
#include "core/Hash.h"
#include "fx/EffectSet.h"
#include "game/SpellKitFx.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

using namespace vd;
using namespace vd::game;

namespace {

bool openPack(pack::PackReader& pack, const char* name) {
    const char* dir = std::getenv("VD_PACKS_DIR");
    return dir && pack.open(std::filesystem::path(dir) / name).ok();
}

u64 texture(const char* path) {
    return assetPathHash(normalizeAssetPath(path));
}

constexpr i32 kNone = -1;

} // namespace

TEST_CASE("Spell kit effects: real rows of the 1.12.1 client (VD_PACKS_DIR)") {
    pack::PackReader db;
    if (!openPack(db, "db.vdpk")) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    SpellKitFx fx;
    REQUIRE(fx.init(db));

    // SpellChainEffects: Drain Life's soul beam pulls towards the caster (scroll -0.5), Health Funnel's
    // pushes.
    const ChainEffect* soul = fx.chainEffect(8);
    REQUIRE(soul);
    CHECK(soul->segmentLength == doctest::Approx(2.78f));
    CHECK(soul->width == doctest::Approx(0.25f));
    CHECK(soul->noise == doctest::Approx(0.01f));
    CHECK(soul->scroll == doctest::Approx(-0.5f));
    CHECK(soul->segmentMs == 1000);
    CHECK(soul->delayMs == 200);
    CHECK(soul->texture == texture("Textures\\SpellChainEffects\\SoulBeam.blp"));
    CHECK(fx.chainEffect(5)->scroll == doctest::Approx(0.5f));
    const ChainEffect* lightning = fx.chainEffect(1);
    REQUIRE(lightning);
    CHECK(lightning->width == doctest::Approx(0.5f));
    CHECK(lightning->noise == doctest::Approx(0.04f));
    CHECK(lightning->scroll == doctest::Approx(1.0f));
    CHECK(lightning->delayMs == 300);
    CHECK(lightning->texture == texture("Textures\\SpellChainEffects\\Lightning.blp"));

    // The channel kits of Drain Life (402), Health Funnel (342), Drain Mana and Mind Flay (430): one held
    // beam.
    const KitFx* drain = fx.kit(402);
    REQUIRE(drain);
    CHECK(drain->chainEffect == 8);
    CHECK(drain->chainBeams == 1);
    CHECK(drain->chainHeld);
    CHECK(fx.kit(342)->chainEffect == 5);
    CHECK(fx.kit(430)->chainEffect == 4);
    CHECK(fx.kit(950)->chainEffect == 10); // Drain Soul: the purple beam
    // Chain Lightning's cast kit (321): the lightning jumps, each for its time; its impact (282) flashes
    // blue.
    const KitFx* chain = fx.kit(321);
    REQUIRE(chain);
    CHECK(chain->chainEffect == 1);
    CHECK(chain->chainBeams == 1);
    CHECK_FALSE(chain->chainHeld);
    CHECK(fx.kit(3169)->chainEffect == 2); // Chain Heal's healing beam
    const KitFx* impact = fx.kit(282);
    REQUIRE(impact);
    CHECK(impact->flashes);
    CHECK(impact->flashColor == 0x6C6CFF);
    CHECK(impact->flashHoldSeconds == doctest::Approx(0.5f));
    CHECK(impact->flashFadeSeconds == doctest::Approx(0.5f));
    // Frostbolt's chill (3531) tints blue; Evocation's channel (2489) too.
    CHECK(fx.kit(3531)->tints);
    CHECK(fx.kit(3531)->tint == 0x3C3CFF);
    CHECK(fx.kit(2489)->tints);
    // Fishing's channel (830) draws the line.
    CHECK(fx.kit(830)->line);
    CHECK(fx.kit(830)->chainEffect == 0);
    // Kits with chain procs but no effect (Death and Decay's channel, 2089: all zero) draw nothing.
    CHECK((fx.kit(2089) == nullptr || fx.kit(2089)->chainEffect == 0));

    // Thunder Clap's cast (356) shakes: SpellEffectCameraShakes 3 = CameraShakes 1, a short vertical jolt.
    REQUIRE(fx.kit(356));
    CHECK(fx.kit(356)->shake == 3);
    const auto clap = fx.effectShakes(3);
    CHECK(clap[0] == 1);
    CHECK(clap[1] == 0);
    CHECK(clap[2] == 0);
    const CameraShake* jolt = fx.cameraShake(1);
    REQUIRE(jolt);
    CHECK(jolt->decays);
    CHECK(jolt->direction == CameraShake::Direction::Up);
    CHECK(jolt->amplitude == doctest::Approx(2.0f / 36.0f));
    CHECK(jolt->frequency == doctest::Approx(3.0f));
    CHECK(jolt->duration == doctest::Approx(0.4f));
    CHECK(jolt->phase == doctest::Approx(0.06f));
    CHECK(jolt->coefficient == doctest::Approx(1.0f));
    const auto quake = fx.effectShakes(1);
    CHECK(quake[0] == 4);
    CHECK(quake[1] == 5);
    CHECK(quake[2] == 6);
    CHECK(fx.cameraShake(4)->direction == CameraShake::Direction::Forward);
    CHECK_FALSE(fx.cameraShake(4)->decays);
    CHECK(fx.cameraShake(5)->direction == CameraShake::Direction::Side);

    // The precast transitions (by AnimationData names): LoadBow 105 -> HoldBow 109, LoadRifle 106 ->
    // HoldRifle 110, LoadThrown 112 -> HoldThrown 111.
    CHECK(fx.holdAfter(105) == 109);
    CHECK(fx.holdAfter(106) == 110);
    CHECK(fx.holdAfter(112) == 111);
    CHECK(fx.holdAfter(51) == SpellKitFx::kNoAnimation);

    // CreatureModelData: a kodo's steps shake (CameraShakes 1) and its fall too (10); a human's do not.
    const u64 kodo = fx::effectModelHash("Creature\\Kodobeast\\KodoBeast.mdx");
    CHECK(fx.footstepShake(kodo) == 1);
    CHECK(fx.deathShake(kodo) == 10);
    CHECK(fx.deathShake(fx::effectModelHash("Creature\\Ogre\\Ogre.mdx")) == 10);
    CHECK(fx.footstepShake(fx::effectModelHash("Character\\Human\\Male\\HumanMale.mdx")) == 0);
}

TEST_CASE("Spell kit effects: the procs' parameters") {
    // Drain Life's channel kit: proc 0 with (8, 1, 1, 0).
    std::array<f32, 16> params{};
    params[0] = 8.0f;
    params[4] = 1.0f;
    params[8] = 1.0f;
    KitFx a = kitFxFromProcs(402, {0, kNone, kNone, kNone}, params, 0);
    CHECK(a.chainEffect == 8);
    CHECK(a.chainBeams == 1);
    CHECK(a.chainHeld);
    // Three beams at most (the client caps them), not held when p2 is 0; in the second proc slot.
    params = {};
    params[1] = 3.0f;
    params[5] = 5.0f;
    KitFx b = kitFxFromProcs(1, {kNone, 12, kNone, kNone}, params, 0);
    CHECK(b.chainEffect == 3);
    CHECK(b.chainBeams == 3);
    CHECK_FALSE(b.chainHeld);
    // A chain proc without an effect or beams is nothing.
    params = {};
    CHECK(kitFxFromProcs(2, {0, kNone, kNone, kNone}, params, 0).chainEffect == 0);
    // A color (proc 1: only p0) and a flash (proc 13: color, hold, fade), with the kit's shake.
    params = {};
    params[0] = 1945621.0f; // 0x1DB015
    params[4] = 0.8f;
    params[1] = 7105791.0f; // 0x6C6CFF
    params[5] = 0.5f;
    params[9] = 0.25f;
    KitFx c = kitFxFromProcs(3, {1, 13, kNone, kNone}, params, 26);
    CHECK(c.tints);
    CHECK(c.tint == 0x1DB015);
    CHECK(c.flashes);
    CHECK(c.flashColor == 0x6C6CFF);
    CHECK(c.flashHoldSeconds == doctest::Approx(0.5f));
    CHECK(c.flashFadeSeconds == doctest::Approx(0.25f));
    CHECK(c.shake == 26);
    // CameraShakes rows: the amplitude in yards (/36), the direction and the decay.
    const CameraShake s = cameraShakeFromRow(76, 0, 0, 2.0f, 6.0f, 20.0f, 0.0f, 0.4f);
    CHECK(s.amplitude == doctest::Approx(2.0f / 36.0f));
    CHECK_FALSE(s.decays);
    CHECK(s.direction == CameraShake::Direction::Forward);
}
