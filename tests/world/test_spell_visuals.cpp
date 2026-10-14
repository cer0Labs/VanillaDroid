// Phase 9: the scheduling of spell visuals (world/SpellVisuals) against a recording world: precast -> cast ->
// missile -> impact timing, interruptions, aura states, channels, ground areas, distance and budget limits.
#include "anim/Animator.h"
#include "game/SpellVisualData.h"
#include "world/SpellVisuals.h"

#include <doctest/doctest.h>

#include <map>
#include <vector>

using namespace vd;
using namespace vd::game;
namespace animation = vd::anim::animation;

namespace {

constexpr u16 kNone = 0xFFFF;

// Records what SpellVisuals asks for.
class Recorder : public SpellVisualWorld {
public:
    struct Effect {
        u64 model = 0;
        u32 actor = ~0u; // ~0u: in the world
        u32 attachment = 0;
        glm::dvec3 position{0.0};
        f32 seconds = 0.0f;
        bool stopped = false;
        u32 moves = 0;
    };
    struct Action {
        u16 animation = kNone;
        bool loop = false;
        u32 plays = 0;
    };
    std::map<u64, Unit> units;
    std::vector<Effect> effects;
    std::map<u32, Action> actions;

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
    EffectHandle spawnAt(u64 model, const glm::dvec3& position, f32, f32 seconds) override {
        effects.push_back({model, ~0u, 0, position, seconds});
        return {static_cast<u32>(effects.size() - 1), 1};
    }
    EffectHandle spawnOn(u64 model, u32 actor, u32 attachment, f32 seconds) override {
        effects.push_back({model, actor, attachment, glm::dvec3(0.0), seconds});
        return {static_cast<u32>(effects.size() - 1), 1};
    }
    void move(EffectHandle e, const glm::dvec3& position, f32) override {
        effects.at(e.index).position = position;
        ++effects.at(e.index).moves;
    }
    void stop(EffectHandle e) override {
        if (e.valid()) {
            effects.at(e.index).stopped = true;
        }
    }
    bool alive(EffectHandle e) const override { return e.valid() && !effects.at(e.index).stopped; }
    u32 liveEffects() const override {
        u32 n = 0;
        for (const Effect& e : effects) {
            n += e.stopped || e.seconds != 0.0f ? 0 : 1; // one-shots count until they end; here: never
        }
        return n + extraLive;
    }
    bool attachmentPosition(u32 actor, u32 attachment, glm::dvec3& out) const override {
        for (const auto& [key, u] : units) {
            if (u.actor == actor) {
                out = u.feet + glm::dvec3(0.0, 0.0, attachment == 19 ? 0.0 : 1.2);
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
    }
    void stopAction(u32 actor) override { actions[actor].animation = kNone; }
    u16 action(u32 actor) const override {
        const auto it = actions.find(actor);
        return it == actions.end() ? kNone : it->second.animation;
    }

    u32 extraLive = 0;
    // Effects alive (not stopped) with this model.
    u32 live(u64 model) const {
        u32 n = 0;
        for (const Effect& e : effects) {
            n += e.model == model && !e.stopped ? 1 : 0;
        }
        return n;
    }
    u32 spawned(u64 model) const {
        u32 n = 0;
        for (const Effect& e : effects) {
            n += e.model == model ? 1 : 0;
        }
        return n;
    }
};

// Model hashes of the hand-made tables.
constexpr u64 kPrecastHand = 101, kCastHand = 102, kMissile = 103, kImpactChest = 104, kStateHead = 105,
              kChannelHand = 106, kArea = 107, kStateWorld = 108;
constexpr u32 kBolt = 1001, kBlizzard = 1002, kStun = 1003, kShield = 1004, kBuff = 1005;

SpellVisualData tables() {
    SpellVisualData data;
    VisualKit precast;
    precast.id = 1;
    precast.animation = animation::kReadySpellDirected;
    precast.models[static_cast<u32>(KitSlot::LeftHand)] = kPrecastHand;
    precast.models[static_cast<u32>(KitSlot::RightHand)] = kPrecastHand;
    precast.modelCount = 2;
    data.addKit(precast);
    VisualKit cast;
    cast.id = 2;
    cast.animation = animation::kSpellCastDirected;
    cast.models[static_cast<u32>(KitSlot::RightHand)] = kCastHand;
    cast.modelCount = 1;
    data.addKit(cast);
    VisualKit impact;
    impact.id = 3;
    impact.animation = animation::kCombatWound;
    impact.models[static_cast<u32>(KitSlot::Chest)] = kImpactChest;
    impact.modelCount = 1;
    data.addKit(impact);
    VisualKit stun;
    stun.id = 4;
    stun.animation = 14; // Stun
    stun.models[static_cast<u32>(KitSlot::Head)] = kStateHead;
    stun.modelCount = 1;
    data.addKit(stun);
    VisualKit channel;
    channel.id = 5;
    channel.animation = animation::kChannelCastOmni;
    channel.models[static_cast<u32>(KitSlot::LeftHand)] = kChannelHand;
    channel.modelCount = 1;
    data.addKit(channel);
    VisualKit shield; // a state with a model in the world and no animation
    shield.id = 6;
    shield.models[static_cast<u32>(KitSlot::World)] = kStateWorld;
    shield.modelCount = 1;
    data.addKit(shield);

    SpellVisualInfo bolt;
    bolt.id = 10;
    bolt.precastKit = 1;
    bolt.castKit = 2;
    bolt.impactKit = 3;
    bolt.missileModel = kMissile;
    bolt.missileAttachment = 1;
    data.addVisual(bolt);
    SpellVisualInfo blizzard;
    blizzard.id = 11;
    blizzard.precastKit = 1;
    blizzard.channelKit = 5;
    blizzard.areaModel = kArea;
    data.addVisual(blizzard);
    SpellVisualInfo stunned;
    stunned.id = 12;
    stunned.stateKit = 4;
    data.addVisual(stunned);
    SpellVisualInfo shielded;
    shielded.id = 13;
    shielded.stateKit = 6;
    data.addVisual(shielded);
    SpellVisualInfo buff; // an instant with an impact and no animation of its own on the caster
    buff.id = 14;
    buff.impactKit = 3;
    data.addVisual(buff);
    data.addSpell(kBolt, 10, 28.0f);
    data.addSpell(kBlizzard, 11, 0.0f);
    data.addSpell(kStun, 12, 0.0f);
    data.addSpell(kShield, 13, 0.0f);
    data.addSpell(kBuff, 14, 0.0f);
    return data;
}

constexpr u64 kCaster = 0x10, kTarget = 0x20, kFar = 0x30;
constexpr u32 kCasterActor = 1, kTargetActor = 2, kFarActor = 3;

struct Fixture {
    SpellVisualData data = tables();
    Recorder world;
    SpellVisuals visuals;
    glm::dvec3 camera{0.0, -10.0, 2.0};
    Fixture() {
        world.addUnit(kCaster, kCasterActor, glm::dvec3(0.0, 0.0, 0.0));
        world.addUnit(kTarget, kTargetActor, glm::dvec3(28.0, 0.0, 0.0)); // one second of flight at 28 yd/s
        world.addUnit(kFar, kFarActor, glm::dvec3(500.0, 0.0, 0.0));
        visuals.init(&data, &world);
    }
};

} // namespace

TEST_CASE("Spell visuals: precast, cast, missile and impact in time") {
    Fixture f;
    f.visuals.spellStart(kCaster, kBolt, kTarget, 2500, 1000);
    f.visuals.update(1000, 0.016f, f.camera);
    // Precast: a model on each hand (the spell hands 21 and 22), held, and the caster readies the spell.
    CHECK(f.world.live(kPrecastHand) == 2);
    for (const Recorder::Effect& e : f.world.effects) {
        CHECK(e.actor == kCasterActor);
        CHECK((e.attachment == 21 || e.attachment == 22));
        CHECK(e.seconds == 0.0f);
    }
    CHECK(f.world.action(kCasterActor) == animation::kReadySpellDirected);
    CHECK(f.world.actions[kCasterActor].loop);
    CHECK(f.visuals.stats().holds == 1);

    // It goes off at 3500: the precast stops, the cast kit plays once with its animation, a missile leaves.
    f.visuals.spellGo(kCaster, kBolt, kTarget, nullptr, 3500);
    f.visuals.spellHit(kCaster, kBolt, kTarget, 3500);
    CHECK(f.world.live(kPrecastHand) == 0);
    CHECK(f.world.spawned(kCastHand) == 1);
    CHECK(f.world.effects.back().model == kMissile);
    CHECK(f.world.action(kCasterActor) == animation::kSpellCastDirected);
    CHECK_FALSE(f.world.actions[kCasterActor].loop);
    for (const Recorder::Effect& e : f.world.effects) {
        if (e.model == kCastHand) {
            CHECK(e.seconds == Effects::kOnce);
        }
    }
    // Halfway at 4000 (28 yd at 28 yd/s: one second), no impact yet.
    f.visuals.update(4000, 0.5f, f.camera);
    CHECK(f.visuals.stats().missiles == 1);
    const Recorder::Effect* missile = nullptr;
    for (const Recorder::Effect& e : f.world.effects) {
        missile = e.model == kMissile ? &e : missile;
    }
    REQUIRE(missile);
    CHECK(missile->position.x == doctest::Approx(14.0).epsilon(0.05));
    CHECK(f.world.spawned(kImpactChest) == 0);
    CHECK(f.world.action(kTargetActor) == kNone);
    // Lands at 4500: the missile stops, the impact plays on the target's chest with its wound.
    f.visuals.update(4499, 0.5f, f.camera);
    CHECK(f.world.spawned(kImpactChest) == 0);
    f.visuals.update(4500, 0.001f, f.camera);
    CHECK(f.world.live(kMissile) == 0);
    CHECK(f.world.spawned(kImpactChest) == 1);
    CHECK(f.world.effects.back().actor == kTargetActor);
    CHECK(f.world.effects.back().attachment == 34);
    CHECK(f.world.action(kTargetActor) == animation::kCombatWound);
    CHECK(f.visuals.stats().missiles == 0);
    CHECK(f.visuals.stats().holds == 0);
}

TEST_CASE("Spell visuals: an interrupted cast stops its precast, no cast follows") {
    Fixture f;
    f.visuals.spellStart(kCaster, kBolt, kTarget, 2500, 1000);
    f.visuals.update(1000, 0.016f, f.camera);
    REQUIRE(f.world.live(kPrecastHand) == 2);
    f.visuals.spellInterrupted(kCaster, 1800);
    f.visuals.update(1800, 0.016f, f.camera);
    CHECK(f.world.live(kPrecastHand) == 0);
    CHECK(f.world.action(kCasterActor) == kNone);
    CHECK(f.world.spawned(kCastHand) == 0);
    // A precast whose cast never comes is given up after its cast time and a grace.
    f.visuals.spellStart(kCaster, kBolt, kTarget, 1500, 5000);
    f.visuals.update(5000, 0.016f, f.camera);
    CHECK(f.world.live(kPrecastHand) == 2);
    f.visuals.update(8400, 0.016f, f.camera); // a pushback may delay the cast
    CHECK(f.world.live(kPrecastHand) == 2);
    f.visuals.update(8600, 0.016f, f.camera);
    CHECK(f.world.live(kPrecastHand) == 0);
    // Instants show no precast.
    f.visuals.spellStart(kCaster, kBolt, kTarget, 0, 9000);
    f.visuals.update(9000, 0.016f, f.camera);
    CHECK(f.visuals.stats().holds == 0);
}

TEST_CASE("Spell visuals: a held loop comes back after a one-shot took the unit") {
    Fixture f;
    f.visuals.spellStart(kCaster, kBolt, kTarget, 3000, 0);
    f.visuals.update(0, 0.016f, f.camera);
    REQUIRE(f.world.action(kCasterActor) == animation::kReadySpellDirected);
    // Hit while casting: a wound takes over (Units plays it)...
    f.world.playAction(kCasterActor, animation::kCombatWound, false);
    f.visuals.update(100, 0.016f, f.camera);
    CHECK(f.world.action(kCasterActor) == animation::kCombatWound);
    // ... and once it is over, the precast loop returns.
    f.world.stopAction(kCasterActor);
    f.visuals.update(900, 0.016f, f.camera);
    CHECK(f.world.action(kCasterActor) == animation::kReadySpellDirected);
}

TEST_CASE("Spell visuals: impacts without a missile, far units and the budget") {
    Fixture f;
    // No missile: the impact plays at once.
    f.visuals.spellGo(kCaster, kBuff, kTarget, nullptr, 0);
    f.visuals.spellHit(kCaster, kBuff, kTarget, 0);
    CHECK(f.world.spawned(kImpactChest) == 1);
    // Far from the camera: nothing (counted as skipped)...
    const u32 skipped = f.visuals.stats().skipped;
    f.visuals.spellHit(kCaster, kBuff, kFar, 0);
    CHECK(f.world.spawned(kImpactChest) == 1);
    CHECK(f.visuals.stats().skipped == skipped + 1);
    // ... unless it is on the player.
    f.visuals.setPlayer(kFar);
    f.visuals.spellHit(kCaster, kBuff, kFar, 0);
    CHECK(f.world.spawned(kImpactChest) == 2);
    // Over the budget of live effects only the player's own still show.
    f.world.extraLive = 10000;
    f.visuals.spellHit(kCaster, kBuff, kTarget, 0);
    CHECK(f.world.spawned(kImpactChest) == 2);
    CHECK(f.world.action(kTargetActor) == animation::kCombatWound); // the animation still plays
    f.visuals.spellHit(kCaster, kBuff, kFar, 0);
    CHECK(f.world.spawned(kImpactChest) == 3);
}

TEST_CASE("Spell visuals: aura states while the aura lasts and while near") {
    Fixture f;
    f.visuals.auraAdded(kTarget, kStun);
    f.visuals.auraAdded(kTarget, kStun); // the same aura again: one state
    f.visuals.auraAdded(kTarget, kBolt); // no state kit: nothing
    f.visuals.update(0, 0.016f, f.camera);
    CHECK(f.visuals.stats().auras == 1);
    CHECK(f.world.live(kStateHead) == 1);
    CHECK(f.world.effects.back().attachment == 20); // over the head
    CHECK(f.world.action(kTargetActor) == 14);      // stunned
    CHECK(f.world.actions[kTargetActor].loop);
    // The unit walks away from the camera: its models go, they come back when it is near again.
    f.world.units[kTarget].feet = glm::dvec3(400.0, 0.0, 0.0);
    f.visuals.update(100, 0.016f, f.camera);
    CHECK(f.world.live(kStateHead) == 0);
    f.world.units[kTarget].feet = glm::dvec3(28.0, 0.0, 0.0);
    f.visuals.update(200, 0.016f, f.camera);
    CHECK(f.world.live(kStateHead) == 1);
    f.visuals.auraRemoved(kTarget, kStun);
    CHECK(f.world.live(kStateHead) == 0);
    CHECK(f.world.action(kTargetActor) == kNone);
    // A state in the world (not following the unit); the unit leaving ends it.
    f.visuals.auraAdded(kCaster, kShield);
    f.visuals.update(300, 0.016f, f.camera);
    CHECK(f.world.live(kStateWorld) == 1);
    CHECK(f.world.effects.back().actor == ~0u);
    f.visuals.unitGone(kCaster);
    CHECK(f.world.live(kStateWorld) == 0);
    CHECK(f.visuals.stats().auras == 1); // counted at the last update
    f.visuals.update(400, 0.016f, f.camera);
    CHECK(f.visuals.stats().auras == 0);
}

TEST_CASE("Spell visuals: channels and ground areas") {
    Fixture f;
    // Blizzard: a cast time, then the channel and its dynamic object over the ground.
    f.visuals.spellStart(kCaster, kBlizzard, 0, 1000, 0);
    f.visuals.update(0, 0.016f, f.camera);
    CHECK(f.world.live(kPrecastHand) == 2);
    CHECK(f.world.action(kCasterActor) == animation::kReadySpellDirected); // the precast kit's own animation
    const glm::dvec3 ground(10.0, 5.0, 0.0);
    f.visuals.spellGo(kCaster, kBlizzard, 0, &ground, 1000);
    f.visuals.channel(kCaster, kBlizzard, 0, 1000);
    f.visuals.area(0x99, kBlizzard, ground, 8.0f, 1000);
    f.visuals.update(1000, 0.016f, f.camera);
    CHECK(f.world.live(kPrecastHand) == 0);
    CHECK(f.world.live(kChannelHand) == 1);
    // The cast animation first (a cast time and no cast kit: the generic omni cast), then the channel's loop.
    CHECK(f.world.action(kCasterActor) == animation::kSpellCastOmni);
    f.world.stopAction(kCasterActor);
    f.visuals.update(1050, 0.016f, f.camera);
    CHECK(f.world.action(kCasterActor) == animation::kChannelCastOmni);
    // The destination got a pass of the area model; over the dynamic object it falls at random points of its
    // radius, one every 300 ms (8 yd), at most six alive at once.
    const auto rained = [&]() {
        u32 n = 0;
        for (const Recorder::Effect& e : f.world.effects) {
            if (e.model == kArea) {
                ++n;
                CHECK(e.seconds == Effects::kOnce);
                CHECK(glm::length(glm::dvec2(e.position - ground)) <= 8.0);
            }
        }
        return n;
    };
    CHECK(rained() == 2); // the pass at the destination and the first one
    for (u32 t = 1100; t <= 3000; t += 100) {
        f.visuals.channel(kCaster, kBlizzard, 0, t); // each frame from the unit's fields: nothing new
        f.visuals.area(0x99, kBlizzard, ground, 8.0f, t);
        f.visuals.update(t, 0.1f, f.camera);
    }
    CHECK(rained() == 7); // the pass and six: the slots are full while they live
    CHECK(f.world.live(kChannelHand) == 1);
    for (Recorder::Effect& e : f.world.effects) {
        e.stopped = e.stopped || e.model == kArea; // they played their animation
    }
    f.visuals.area(0x99, kBlizzard, ground, 8.0f, 3300);
    f.visuals.update(3300, 0.1f, f.camera);
    CHECK(rained() == 8);
    // It ends: the channel stops; the area goes when the object is no longer seen.
    f.visuals.channel(kCaster, 0, 0, 5000);
    CHECK(f.world.live(kChannelHand) == 0);
    CHECK(f.world.action(kCasterActor) == kNone);
    f.visuals.update(5400, 0.016f, f.camera);
    CHECK(rained() == 8);
    CHECK(f.visuals.stats().areas == 0);
}
