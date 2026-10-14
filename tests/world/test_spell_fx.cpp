// Phase 14 (spellfx): the beams' geometry (world/SpellBeams), the camera shakes (world/CameraShakes) and what
// the kits' procs and shakes make SpellVisuals do (beams of channels and chains, tints, flashes, shakes,
// precast transitions), against a recording world.
#include "anim/Animator.h"
#include "game/SpellKitFx.h"
#include "game/SpellVisualData.h"
#include "pack/ActorFormat.h"
#include "world/CameraShakes.h"
#include "world/SpellBeams.h"
#include "world/SpellVisuals.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <cmath>
#include <map>
#include <vector>

using namespace vd;
using namespace vd::game;

namespace {

constexpr u16 kNoAction = 0xFFFF;

glm::vec3 position(const ParticleVertex& v) {
    return glm::vec3(v.position[0], v.position[1], v.position[2]);
}
f32 u(const ParticleVertex& v) {
    return static_cast<f32>(v.uv[0]) / 65535.0f;
}
f32 v(const ParticleVertex& p) {
    return static_cast<f32>(p.uv[1]) / 65535.0f;
}

ChainEffect soulBeam() {
    ChainEffect c;
    c.id = 8;
    c.segmentLength = 2.78f;
    c.width = 0.25f;
    c.noise = 0.01f;
    c.scroll = -0.5f;
    c.segmentMs = 1000;
    c.delayMs = 200;
    c.texture = 1234;
    return c;
}

} // namespace

TEST_CASE("Beams: joints, width, texture and scroll (WoW.exe CLightning)") {
    const ChainEffect beam = soulBeam();
    std::array<glm::vec3, kMaxBeamJoints> joints;
    // 20 yards: int(20 / 2.78 + 2) = 9 segments, 10 joints; the ends exact, the rest within the noise.
    const glm::vec3 from(0.0f), to(20.0f, 0.0f, 0.0f);
    const u32 count = beamJoints(beam, from, to, 77, joints);
    REQUIRE(count == 10);
    CHECK(joints[0] == from);
    CHECK(joints[9] == to);
    for (u32 i = 1; i < 9; ++i) {
        const glm::vec3 straight = to * (static_cast<f32>(i) / 9.0f);
        const glm::vec3 off = joints[i] - straight;
        for (u32 c = 0; c < 3; ++c) {
            CHECK(std::abs(off[c]) <= 20.0f * 0.01f + 1e-4f);
        }
    }
    // A longer beam is capped at the joints there is room for.
    std::array<glm::vec3, 8> few;
    CHECK(beamJoints(beam, from, glm::vec3(500.0f, 0.0f, 0.0f), 1, few) == 8);

    // The scroll: one texture length every |scroll| seconds, towards the caster for a negative one.
    CHECK(beamScroll(beam, 0.0f) == doctest::Approx(0.0f));
    CHECK(beamScroll(beam, 0.25f) == doctest::Approx(0.5f));
    CHECK(beamScroll(beam, 0.6f) == doctest::Approx(0.2f));
    ChainEffect forward = beam;
    forward.scroll = 1.0f;
    CHECK(beamScroll(forward, 0.25f) == doctest::Approx(-0.25f));
    ChainEffect still = beam;
    still.scroll = 0.0f;
    CHECK(beamScroll(still, 3.0f) == 0.0f);

    // The strip, seen from the side (camera at -y looking +y): pointed ends, `width` to each side in the
    // middle, u over the whole beam, v across; the texture wraps once (an extra quad).
    BeamInstance instance;
    instance.effect = &beam;
    instance.from = glm::dvec3(100.0, 50.0, 10.0);
    instance.to = glm::dvec3(120.0, 50.0, 10.0);
    instance.age = 0.25f; // scrolled by half a length
    instance.seed = 9;
    std::vector<ParticleVertex> out(u64{kMaxBeamQuads} * 4);
    const glm::dvec3 camera(110.0, 30.0, 10.0);
    const glm::vec3 forwardAxis(0.0f, 1.0f, 0.0f);
    const u32 quads = writeBeam(instance, camera, forwardAxis, out);
    REQUIRE(quads == 10); // 9 segments + the split where u wraps
    // The first corner pair sits on the start (camera-relative), the last on the end.
    CHECK(glm::length(position(out[0]) - glm::vec3(-10.0f, 20.0f, 0.0f)) < 1e-3f);
    CHECK(glm::length(position(out[1]) - glm::vec3(-10.0f, 20.0f, 0.0f)) < 1e-3f);
    CHECK(glm::length(position(out[(quads - 1) * 4 + 2]) - glm::vec3(10.0f, 20.0f, 0.0f)) < 1e-3f);
    CHECK(glm::length(position(out[(quads - 1) * 4 + 3]) - glm::vec3(10.0f, 20.0f, 0.0f)) < 1e-3f);
    CHECK(v(out[0]) == doctest::Approx(0.5f).epsilon(0.001));
    // An inner joint: the two corners about 2 * width apart, across the beam (along z, facing the camera).
    const glm::vec3 top = position(out[4]), bottom = position(out[5]);
    CHECK(std::abs(top.z - bottom.z) == doctest::Approx(0.5f).epsilon(0.1));
    CHECK(v(out[4]) == 0.0f);
    CHECK(v(out[5]) == doctest::Approx(1.0f));
    // u climbs from 0.5 (scrolled), wraps to 0 once, and every corner stays within [0, 1].
    CHECK(u(out[0]) == doctest::Approx(0.5f).epsilon(0.001));
    u32 wraps = 0;
    for (u32 q = 0; q < quads; ++q) {
        CHECK(u(out[q * 4]) <= u(out[q * 4 + 2]) + 1e-4f);
        wraps += u(out[q * 4]) < 1e-4f && q > 0 ? 1 : 0;
    }
    CHECK(wraps == 1);

    // The fishing line: 64 pieces, sagging half a yard in the middle, a pixel wide.
    std::vector<ParticleVertex> line(u64{kFishingLinePieces} * 4);
    const u32 pieces =
        writeLine(glm::dvec3(0.0, 0.0, 10.0), glm::dvec3(10.0, 0.0, 10.0), kFishingLineSag,
                  glm::dvec3(5.0, -10.0, 10.0), glm::vec3(0.0f, 1.0f, 0.0f), 1000.0f, 1.0f, line);
    REQUIRE(pieces == kFishingLinePieces);
    const glm::vec3 middle = (position(line[32 * 4]) + position(line[32 * 4 + 1])) * 0.5f;
    CHECK(middle.z == doctest::Approx(-0.5f).epsilon(0.01));
    CHECK(glm::length(position(line[0]) - position(line[1])) ==
          doctest::Approx(11.18f / 1000.0f).epsilon(0.05));
}

TEST_CASE("Camera shakes: the client's sine, decay, falloff and strongest per direction") {
    CameraShake up;
    up.id = 1;
    up.decays = true;
    up.direction = CameraShake::Direction::Up;
    up.amplitude = 2.0f / 36.0f;
    up.frequency = 3.0f;
    up.duration = 0.4f;
    up.phase = 0.06f;
    up.coefficient = 1.0f;
    // amplitude * sin(2 pi f (t + phase)) * exp(-coefficient (t + phase))
    const f32 t = 0.05f;
    const f32 expected = up.amplitude * std::sin(6.2831853f * 3.0f * 0.11f) * std::exp(-0.11f);
    CHECK(cameraShakeValue(up, t, up.amplitude) == doctest::Approx(expected));
    // Falloff: full within 9 yards, 0.7 every 9 more, nothing past 80.
    CHECK(cameraShakeFalloff(1.0f, 5.0f) == 1.0f);
    CHECK(cameraShakeFalloff(1.0f, 18.0f) == doctest::Approx(0.7f));
    CHECK(cameraShakeFalloff(1.0f, 27.0f) == doctest::Approx(0.49f));
    CHECK(cameraShakeFalloff(1.0f, 81.0f) == 0.0f);

    CameraShakes shakes;
    shakes.add(up, glm::dvec3(0.0), 1000);
    CHECK(shakes.active() == 1);
    // Up: only z moves; at 1050 ms the value above.
    glm::vec3 offset = shakes.sample(1050, glm::dvec3(0.0, 0.0, 5.0), 0.0f);
    CHECK(offset.x == 0.0f);
    CHECK(offset.z == doctest::Approx(expected));
    // Far away: weaker by the falloff.
    offset = shakes.sample(1050, glm::dvec3(18.0, 0.0, 0.0), 0.0f);
    CHECK(offset.z == doctest::Approx(expected * 0.7f).epsilon(0.01));
    // The phase counts against the duration: gone at t + phase >= 0.4 s.
    shakes.sample(1339, glm::dvec3(0.0), 0.0f);
    CHECK(shakes.active() == 1);
    shakes.sample(1341, glm::dvec3(0.0), 0.0f);
    CHECK(shakes.active() == 0);

    // Forward follows the facing; of two shakes in a direction the strongest (after falloff) wins.
    CameraShake forward;
    forward.direction = CameraShake::Direction::Forward;
    forward.amplitude = 1.0f;
    forward.frequency = 1.0f;
    forward.duration = 10.0f;
    CameraShake weak = forward;
    weak.amplitude = 0.5f;
    shakes.add(weak, glm::dvec3(0.0), 0);
    shakes.add(forward, glm::dvec3(0.0), 0);
    offset =
        shakes.sample(250, glm::dvec3(0.0), 1.5707963f); // a quarter period: the full amplitude, facing +y
    CHECK(offset.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(offset.y == doctest::Approx(1.0f));
    shakes.enabled = false;
    CHECK(shakes.sample(250, glm::dvec3(0.0), 0.0f) == glm::vec3(0.0f));

    // The footsteps that shake (a kodo's $FL0 / $RR1...), not the other events.
    CHECK(footstepEvent(pack::actorEventId("$FL0")));
    CHECK(footstepEvent(pack::actorEventId("$RR1")));
    CHECK(footstepEvent(pack::actorEventId("$SR0")));
    CHECK(footstepEvent(pack::actorEventId("$WL2")));
    CHECK_FALSE(footstepEvent(pack::actorEventId("$FD1")));
    CHECK_FALSE(footstepEvent(pack::actorEventId("$DTH")));
    CHECK_FALSE(footstepEvent(pack::actorEventId("$CSL")));
}

namespace {

// Records what SpellVisuals asks for (the parts phase 14 uses).
class Recorder : public SpellVisualWorld {
public:
    std::map<u64, Unit> units;
    std::map<u32, glm::vec3> tints;
    struct Action {
        u16 animation = kNoAction;
        bool loop = false;
        u32 plays = 0;
    };
    std::map<u32, Action> actions;
    std::vector<u16> played; // every action, in order

    void addUnit(u64 key, u32 actor, glm::dvec3 feet) {
        Unit u;
        u.actor = actor;
        u.feet = feet;
        units[key] = u;
    }
    bool unit(u64 key, Unit& out) const override {
        const auto it = units.find(key);
        if (it == units.end()) {
            return false;
        }
        out = it->second;
        return true;
    }
    EffectHandle spawnAt(u64, const glm::dvec3&, f32, f32) override { return {0, 1}; }
    EffectHandle spawnOn(u64, u32, u32, f32) override { return {0, 1}; }
    void move(EffectHandle, const glm::dvec3&, f32) override {}
    void stop(EffectHandle) override {}
    bool alive(EffectHandle) const override { return false; }
    u32 liveEffects() const override { return 0; }
    bool attachmentPosition(u32 actor, u32 attachment, glm::dvec3& out) const override {
        for (const auto& [key, u] : units) {
            if (u.actor == actor) {
                // The left hand 1.5 up, the chest 1.2 up, anything else 1.
                out = u.feet + glm::dvec3(0.0, 0.0, attachment == 2 ? 1.5 : attachment == 34 ? 1.2 : 1.0);
                return true;
            }
        }
        return false;
    }
    bool hasAttachment(u32, u32) const override { return true; }
    void playAction(u32 actor, u16 animation, bool loop) override {
        Action& a = actions[actor];
        a.animation = animation;
        a.loop = loop;
        ++a.plays;
        played.push_back(animation);
    }
    void stopAction(u32 actor) override { actions[actor].animation = kNoAction; }
    u16 action(u32 actor) const override {
        const auto it = actions.find(actor);
        return it == actions.end() ? kNoAction : it->second.animation;
    }
    void setTint(u32 actor, const glm::vec3& rgb) override { tints[actor] = rgb; }
};

constexpr u64 kCaster = 0x10, kFirst = 0x20, kSecond = 0x30, kThird = 0x40;
constexpr u32 kCasterActor = 1, kFirstActor = 2, kSecondActor = 3, kThirdActor = 4;
constexpr u32 kDrain = 2001, kChain = 2002, kClap = 2003, kShot = 2004, kEvocation = 2005;

struct Fixture {
    SpellVisualData data;
    SpellKitFx fx;
    CameraShakes shakes;
    Recorder world;
    SpellVisuals visuals;
    glm::dvec3 camera{10.0, -10.0, 2.0};
    Fixture() {
        // Kits: a drain's channel (held soul beam), a chain cast (lightning, one jump at a time) and its
        // impact (a blue flash), a clap (a shake), a shot's precast (LoadBow), a channel that tints the
        // caster.
        VisualKit channel;
        channel.id = 1;
        channel.animation = anim::animation::kChannelCastDirected;
        data.addKit(channel);
        VisualKit cast;
        cast.id = 2;
        data.addKit(cast);
        VisualKit impact;
        impact.id = 3;
        data.addKit(impact);
        VisualKit clap;
        clap.id = 4;
        data.addKit(clap);
        VisualKit load;
        load.id = 5;
        load.animation = 105; // LoadBow
        data.addKit(load);
        VisualKit glow;
        glow.id = 6;
        data.addKit(glow);
        SpellVisualInfo drain;
        drain.id = 10;
        drain.channelKit = 1;
        drain.missileAttachment = 1; // the chest
        data.addVisual(drain);
        SpellVisualInfo chain;
        chain.id = 11;
        chain.castKit = 2;
        chain.impactKit = 3;
        chain.missileAttachment = 1;
        data.addVisual(chain);
        SpellVisualInfo clapVisual;
        clapVisual.id = 12;
        clapVisual.castKit = 4;
        data.addVisual(clapVisual);
        SpellVisualInfo shot;
        shot.id = 13;
        shot.precastKit = 5;
        data.addVisual(shot);
        SpellVisualInfo evocation;
        evocation.id = 14;
        evocation.channelKit = 6;
        data.addVisual(evocation);
        data.addSpell(kDrain, 10, 0.0f);
        data.addSpell(kChain, 11, 0.0f);
        data.addSpell(kClap, 12, 0.0f);
        data.addSpell(kShot, 13, 0.0f);
        data.addSpell(kEvocation, 14, 0.0f);

        ChainEffect soul;
        soul.id = 8;
        soul.scroll = -0.5f;
        fx.addChainEffect(soul);
        ChainEffect lightning;
        lightning.id = 1;
        lightning.width = 0.5f;
        lightning.noise = 0.04f;
        lightning.segmentMs = 1000;
        lightning.delayMs = 300;
        fx.addChainEffect(lightning);
        KitFx drainFx;
        drainFx.kit = 1;
        drainFx.chainEffect = 8;
        drainFx.chainBeams = 1;
        drainFx.chainHeld = true;
        fx.addKit(drainFx);
        KitFx castFx;
        castFx.kit = 2;
        castFx.chainEffect = 1;
        castFx.chainBeams = 1;
        fx.addKit(castFx);
        KitFx impactFx;
        impactFx.kit = 3;
        impactFx.flashes = true;
        impactFx.flashColor = 0x0000FF;
        impactFx.flashHoldSeconds = 0.5f;
        impactFx.flashFadeSeconds = 0.5f;
        fx.addKit(impactFx);
        KitFx clapFx;
        clapFx.kit = 4;
        clapFx.shake = 3;
        fx.addKit(clapFx);
        KitFx glowFx;
        glowFx.kit = 6;
        glowFx.tints = true;
        glowFx.tint = 0x8080FF;
        fx.addKit(glowFx);
        CameraShake jolt;
        jolt.id = 1;
        jolt.amplitude = 0.05f;
        jolt.frequency = 3.0f;
        jolt.duration = 0.4f;
        fx.addCameraShake(jolt);
        fx.addEffectShakes(3, {1, 0, 0});
        fx.addTransition(105, 109);

        world.addUnit(kCaster, kCasterActor, glm::dvec3(0.0, 0.0, 0.0));
        world.addUnit(kFirst, kFirstActor, glm::dvec3(20.0, 0.0, 0.0));
        world.addUnit(kSecond, kSecondActor, glm::dvec3(24.0, 6.0, 0.0));
        world.addUnit(kThird, kThirdActor, glm::dvec3(20.0, 12.0, 0.0));
        visuals.init(&data, &world);
        visuals.setKitFx(&fx, &shakes);
    }
};

} // namespace

TEST_CASE("Spell visuals (phase 14): a channel's held beam, from the hand to the chest") {
    Fixture f;
    f.visuals.channel(kCaster, kDrain, kFirst, 1000);
    f.visuals.update(1000, 0.016f, f.camera);
    REQUIRE(f.visuals.beams().size() == 1);
    const BeamInstance& beam = f.visuals.beams()[0];
    REQUIRE(beam.effect);
    CHECK(beam.effect->id == 8);
    CHECK(beam.from == glm::dvec3(0.0, 0.0, 1.5)); // the caster's left hand
    CHECK(beam.to == glm::dvec3(20.0, 0.0, 1.2));  // the target's chest (the visual's missile point)
    // Held while it channels (long after a segment's second), new joints every frame.
    const u32 firstSeed = beam.seed;
    f.visuals.update(5000, 0.016f, f.camera);
    REQUIRE(f.visuals.beams().size() == 1);
    CHECK(f.visuals.beams()[0].age == doctest::Approx(4.0f));
    CHECK(f.visuals.beams()[0].seed != firstSeed);
    // The channel ends: the beam goes at once.
    f.visuals.channel(kCaster, 0, 0, 5100);
    f.visuals.update(5100, 0.016f, f.camera);
    CHECK(f.visuals.beams().empty());
    // VD_BEAMS=0: none.
    f.visuals.beamsEnabled = false;
    f.visuals.channel(kCaster, kDrain, kFirst, 6000);
    f.visuals.update(6000, 0.016f, f.camera);
    CHECK(f.visuals.beams().empty());
}

TEST_CASE("Spell visuals (phase 14): a channel through the units the server names") {
    Fixture f;
    // SMSG_SPELL_UPDATE_CHAIN_TARGETS comes first (two units, in order), then the channel's fields.
    f.visuals.chainTarget(kCaster, kDrain, kFirst, 0, 2, 1000);
    f.visuals.chainTarget(kCaster, kDrain, kSecond, 1, 2, 1000);
    f.visuals.channel(kCaster, kDrain, kFirst, 1000);
    f.visuals.update(1000, 0.016f, f.camera);
    REQUIRE(f.visuals.beams().size() == 2);
    CHECK(f.visuals.beams()[0].from == glm::dvec3(0.0, 0.0, 1.5));
    CHECK(f.visuals.beams()[0].to == glm::dvec3(20.0, 0.0, 1.2));
    CHECK(f.visuals.beams()[1].from == glm::dvec3(20.0, 0.0, 1.2)); // the next leaves the previous target
    CHECK(f.visuals.beams()[1].to == glm::dvec3(24.0, 6.0, 1.2));
    // A list for another target (a stale one) is not used: the channel's own target instead.
    Fixture g;
    g.visuals.chainTarget(kCaster, kDrain, kThird, 0, 1, 1000);
    g.visuals.channel(kCaster, kDrain, kFirst, 1000);
    g.visuals.update(1000, 0.016f, g.camera);
    REQUIRE(g.visuals.beams().size() == 1);
    CHECK(g.visuals.beams()[0].to == glm::dvec3(20.0, 0.0, 1.2));
}

TEST_CASE("Spell visuals (phase 14): a chain cast jumps one unit at a time, and flashes them") {
    Fixture f;
    // SPELL_GO and its three hits in order.
    f.visuals.spellGo(kCaster, kChain, kFirst, nullptr, 1000);
    f.visuals.spellHit(kCaster, kChain, kFirst, 1000);
    f.visuals.spellHit(kCaster, kChain, kSecond, 1000);
    f.visuals.spellHit(kCaster, kChain, kThird, 1000);
    f.visuals.update(1000, 0.016f, f.camera);
    // At once: caster -> first.
    REQUIRE(f.visuals.beams().size() == 1);
    CHECK(f.visuals.beams()[0].from == glm::dvec3(0.0, 0.0, 1.5));
    CHECK(f.visuals.beams()[0].to == glm::dvec3(20.0, 0.0, 1.2));
    // 300 ms later the second jump, 600 ms the third; each lasts a second.
    f.visuals.update(1300, 0.016f, f.camera);
    CHECK(f.visuals.beams().size() == 2);
    f.visuals.update(1650, 0.016f, f.camera);
    CHECK(f.visuals.beams().size() == 3);
    f.visuals.update(2000, 0.016f, f.camera);
    CHECK(f.visuals.beams().size() == 2); // the first ended
    f.visuals.update(2600, 0.016f, f.camera);
    CHECK(f.visuals.beams().empty());

    // The impacts flashed every target blue: the color for half a second, then back over half a second.
    CHECK(f.world.tints.count(kSecondActor) == 1);
    Fixture g;
    g.visuals.spellHit(kCaster, kChain, kFirst, 1000);
    g.visuals.update(1200, 0.016f, g.camera);
    CHECK(g.world.tints[kFirstActor] == glm::vec3(0.0f, 0.0f, 1.0f));
    g.visuals.update(1750, 0.016f, g.camera);
    const glm::vec3 halfway = g.world.tints[kFirstActor];
    CHECK(halfway.x == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(halfway.z == doctest::Approx(1.0f));
    g.visuals.update(2100, 0.016f, g.camera);
    CHECK(g.world.tints[kFirstActor] == glm::vec3(1.0f));
    CHECK(g.visuals.stats().tints == 0);
}

TEST_CASE("Spell visuals (phase 14): shakes, held tints and the precast's load and hold") {
    Fixture f;
    // Thunder Clap's cast shakes the camera (its SpellEffectCameraShakes' one shake).
    f.visuals.spellGo(kCaster, kClap, 0, nullptr, 1000);
    CHECK(f.shakes.active() == 1);
    CHECK(f.visuals.stats().shakes == 1);
    // A channel whose kit tints the caster: while it lasts.
    f.visuals.channel(kCaster, kEvocation, 0, 2000);
    f.visuals.update(2000, 0.016f, f.camera);
    CHECK(f.world.tints[kCasterActor].x == doctest::Approx(128.0f / 255.0f));
    f.visuals.channel(kCaster, 0, 0, 3000);
    f.visuals.update(3000, 0.016f, f.camera);
    CHECK(f.world.tints[kCasterActor] == glm::vec3(1.0f));
    // A shot's precast: LoadBow once, then HoldBow loops when it is done.
    f.world.played.clear();
    f.visuals.spellStart(kCaster, kShot, kFirst, 3000, 4000);
    REQUIRE_FALSE(f.world.played.empty());
    CHECK(f.world.played.front() == 105);
    CHECK_FALSE(f.world.actions[kCasterActor].loop);
    f.world.stopAction(kCasterActor); // the load finished
    f.visuals.update(4500, 0.016f, f.camera);
    CHECK(f.world.action(kCasterActor) == 109);
    CHECK(f.world.actions[kCasterActor].loop);
}

TEST_CASE("Spell visuals (phase 14): the missile's landing point by the client's table") {
    CHECK(missileAttachment(0) == 20); // head
    CHECK(missileAttachment(1) == 34); // chest
    CHECK(missileAttachment(2) == 19); // base
    CHECK(missileAttachment(3) == 21); // left hand
    CHECK(missileAttachment(4) == 22); // right hand
    CHECK(missileAttachment(99) == 34);
}
