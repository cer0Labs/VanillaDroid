#include "world/UnitBodyEffects.h"

#include "anim/Animator.h"
#include "core/Clock.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/AreaData.h"
#include "pack/ActorFormat.h"
#include "pack/BodyFxFormat.h"
#include "pack/ModelFormat.h"
#include "physics/CollisionWorld.h"
#include "world/Effects.h"
#include "world/Terrain.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

namespace animation = anim::animation;

constexpr u64 kShadowTexture = assetPathHash("Textures\\ShadowBlob.blp");
constexpr u64 kSplashTexture = assetPathHash("XTextures\\splash\\splash.blp");
constexpr u64 kWakeTexture = assetPathHash("XTextures\\splash\\wake.blp");

constexpr f32 kSpeedSmoothing = 8.0f;      // 1/s: how fast the measured speed follows the movement
constexpr f32 kWalkingSpeed = 5.0f;        // 0x5FC350: up to twice the walk speed (2.5 yd/s) a step is a walk
constexpr f32 kFloorReuseYards = 0.25f;    // the floor under a body is asked again past this
constexpr u32 kFloorKeepFrames = 60;       // ... or this old
constexpr f32 kFloorAbove = 0.5f;          // a floor this far over the feet still counts (sunk into a slope)
constexpr f32 kFloorBelow = 5.0f;          // floors are looked for this far under the feet
constexpr f32 kBreathCheckSeconds = 10.0f; // 0x6077A5: cold and under water are decided every 10 s
constexpr f32 kUnderwaterBreath = 5.0f; // 0x60777B: bubbles only this far (plus its height) under the surface
constexpr f32 kWadingShare = 0.5f;    // 0x5FC163: a step sprays water with less than half its height over it
constexpr f32 kSplashShare = 0.4f;    // 0x603150: going in or out of water past 0.4 of its height
constexpr f32 kOnTerrainYards = 0.5f; // feet this close to the terrain stand on it (not on a building)
constexpr f32 kShadowBoxLimit = 5.0f; // the shadow's box is clamped to +-5 yards (huge models)
constexpr f32 kShadowReach = 5.0f / 3.0f; // its projection's depth under the feet, in half heights of the box

bool envOff(const char* name) {
    const char* value = VD_DEV_ENV(name);
    return value && std::strcmp(value, "0") == 0;
}

bool walkingAnimation(u16 a) {
    return a == animation::kWalk || a == animation::kWalkBackwards;
}

bool backwardAnimation(u16 a) {
    return a == animation::kWalkBackwards || a == animation::kSwimBackwards;
}

bool movingAnimation(u16 a) {
    return a == animation::kWalk || a == animation::kRun || a == animation::kWalkBackwards ||
           a == animation::kSwim || a == animation::kSwimBackwards;
}

// The terrain's normal at (x, y) from its heights around (nullopt: not loaded there).
std::optional<glm::vec3> terrainNormal(const Terrain& terrain, f64 x, f64 y) {
    constexpr f64 kStep = 0.5;
    const std::optional<f32> east = terrain.heightAt(x + kStep, y), west = terrain.heightAt(x - kStep, y);
    const std::optional<f32> north = terrain.heightAt(x, y + kStep), south = terrain.heightAt(x, y - kStep);
    if (!east || !west || !north || !south) {
        return std::nullopt;
    }
    const glm::vec3 n(-(*east - *west) / (2.0f * static_cast<f32>(kStep)),
                      -(*north - *south) / (2.0f * static_cast<f32>(kStep)), 1.0f);
    return glm::normalize(n);
}

} // namespace

bool UnitBodyEffects::init(const pack::PackReader& db, const std::filesystem::path& packsDir) {
    m_ready = false;
    if (!m_data.init(db)) {
        VD_LOG_WARN("Body effects: the client tables are missing");
        return false;
    }
    if (m_bodyPack.open(packsDir / pack::kBodyFxPack) && m_data.openBounds(m_bodyPack)) {
        VD_LOG_INFO("Body effects: %s", pack::kBodyFxPack);
    } else {
        VD_LOG_WARN("Body effects: %s missing or old (no blob shadows; vd_baker bake --only bodyfx)",
                    pack::kBodyFxPack);
    }
    if (!m_soundPack.open(packsDir / "sound.vdpk")) {
        VD_LOG_WARN("Body effects: sound.vdpk missing (no footprints nor ground sprays)");
    }
    m_views.reserve(Actors::kMaxVisible);
    m_bodies.reserve(Actors::kMaxVisible);
    m_shadowQuads.reserve(kMaxShadows);
    m_distances.reserve(Actors::kMaxVisible);
    m_events.reserve(Actors::kMaxEvents + 64);
    m_states.reserve(4096);
    m_bodyOf.reserve(4096);
    m_scratch.reserve((kMaxShadows + FootprintPool::kPlayerSlots + FootprintPool::kOtherSlots +
                       RipplePool::kPlayerSlots + RipplePool::kOtherSlots) *
                      4);
    m_footprintTextures.clear();
    m_footprintTextures.reserve(16);
    shadows = !envOff("VD_BLOB_SHADOWS");
    blood = !envOff("VD_BLOOD");
    footsteps = !envOff("VD_FOOTSTEP_FX");
    swimming = !envOff("VD_SWIM_FX");
    breath = !envOff("VD_BREATH");
    if (const char* forced = VD_DEV_ENV("VD_BREATH")) {
        forceBreath = std::strcmp(forced, "cold") == 0 ? 1 : std::strcmp(forced, "water") == 0 ? 2 : 0;
    }
    if (const char* fake = VD_DEV_ENV("VD_FAKE_BODY_EVENTS")) {
        fakeEvents = std::strcmp(fake, "0") != 0;
    }
    trace = VD_DEV_ENV("VD_TRACE_BODYFX") != nullptr;
    if (const char* level = VD_DEV_ENV("VD_VIOLENCE")) {
        violence = std::min(2u, static_cast<u32>(std::strtoul(level, nullptr, 10)));
    }
    m_ready = true;
    return true;
}

void UnitBodyEffects::shutdown() {
    m_ground.setMap(m_soundPack, {});
    m_footprints.clear();
    m_ripples.clear();
    m_bodyPack.close();
    m_ready = false;
}

void UnitBodyEffects::setMap(std::string_view map) {
    if (m_soundPack.entryCount() > 0) {
        m_ground.setMap(m_soundPack, map);
    }
    m_footprints.clear();
    m_ripples.clear();
    for (BodyState& state : m_states) {
        state = BodyState{};
    }
}

void UnitBodyEffects::setQuality(f32 distance, f32 marks) {
    m_distanceScale = std::clamp(distance, 0.25f, 1.5f);
    m_marksScale = std::clamp(marks, 0.1f, 1.0f);
    m_footprints.setOtherSlots(static_cast<u32>(static_cast<f32>(FootprintPool::kOtherSlots) * m_marksScale));
}

UnitBodyEffects::BodyState& UnitBodyEffects::stateOf(u32 actor) {
    if (actor >= m_states.size()) {
        m_states.resize(actor + 256); // rare: more actors than ever before
        m_bodyOf.resize(actor + 256, -1);
    }
    return m_states[actor];
}

f32 UnitBodyEffects::random() {
    m_random ^= m_random << 13;
    m_random ^= m_random >> 17;
    m_random ^= m_random << 5;
    return static_cast<f32>(m_random >> 8) * (1.0f / 16777216.0f);
}

std::optional<f32> UnitBodyEffects::waterLevel(const Frame& frame, const glm::dvec3& feet, f32 above) const {
    if (!frame.collision || !frame.collision->isOpen() || !frame.collision->ready(feet.x, feet.y)) {
        return std::nullopt;
    }
    const std::optional<physics::LiquidSample> liquid =
        frame.collision->liquidAt(feet.x, feet.y, feet.z + above, feet.z - 0.05);
    return liquid ? std::optional<f32>(liquid->level) : std::nullopt;
}

void UnitBodyEffects::update(const Frame& frame) {
    VD_PROFILE_ZONE("UnitBodyEffects::update");
    const Clock::time_point start = Clock::now();
    ++m_frame;
    m_ground.update();
    m_footprints.update(frame.dt);
    m_ripples.update(frame.dt);
    m_shadowQuads.clear();
    m_bodies.clear();
    m_stats.bodies = m_stats.shadows = m_stats.floorQueries = 0;
    if (!m_ready || !frame.actors) {
        return;
    }
    frame.actors->collectBodies(m_views);
    const f32 dt = std::max(frame.dt, 1e-4f);
    u32 last = 0;
    for (const Actors::BodyView& view : m_views) {
        last = std::max(last, view.actor);
    }
    if (!m_views.empty()) {
        stateOf(last); // the states grow (rarely) before any of them is pointed at
    }
    for (const Actors::BodyView& view : m_views) {
        BodyState& state = stateOf(view.actor);
        if (state.seenFrame + 1 != m_frame || state.model != view.model ||
            glm::length(glm::dvec2(view.feet - state.feet)) > 20.0) {
            state = BodyState{}; // a new body in this slot (or one back in view, or teleported)
            state.model = view.model;
            state.feet = view.feet;
            state.breathTimer = random() * kBreathCheckSeconds; // not every body on the same frame
        }
        const f32 moved = static_cast<f32>(glm::length(glm::dvec2(view.feet - state.feet)));
        state.speed += (moved / dt - state.speed) * std::min(1.0f, kSpeedSmoothing * dt);
        state.feet = view.feet;
        state.seenFrame = m_frame;
        Body body;
        body.view = view;
        body.model = m_data.model(view.model);
        body.state = &state;
        body.distance = static_cast<f32>(glm::length(view.feet - frame.camera));
        body.height = body.model && body.model->collisionHeight > 0.0f
                          ? body.model->collisionHeight * view.scale
                          : std::max(view.top, 0.5f);
        m_bodyOf[view.actor] = static_cast<i32>(m_bodies.size());
        m_bodies.push_back(body);
    }
    m_stats.bodies = static_cast<u32>(m_bodies.size());

    // Blob shadows; with more bodies than the budget, the nearest.
    if (shadows && m_data.hasBounds()) {
        f32 reach = kShadowDistance * m_distanceScale;
        if (m_bodies.size() > kMaxShadows) {
            m_distances.clear();
            for (const Body& body : m_bodies) {
                m_distances.push_back(body.distance);
            }
            std::nth_element(m_distances.begin(), m_distances.begin() + (kMaxShadows - 1), m_distances.end());
            reach = std::min(reach, m_distances[kMaxShadows - 1]);
        }
        for (Body& body : m_bodies) {
            if (m_shadowQuads.size() >= kMaxShadows) {
                break;
            }
            const Actors::BodyView& v = body.view;
            if (v.mount != Actors::kNone || body.distance > reach || v.opacity <= 0.01f) {
                continue; // a rider: its mount's shadow is the unit's
            }
            const pack::BodyBoundsBox* box = m_data.bounds(v.model, v.animation);
            if (!box) {
                continue;
            }
            findFloor(body, frame);
            const BodyState& s = *body.state;
            if (!s.floorFound) {
                continue;
            }
            ShadowQuad quad;
            quad.feet = v.feet;
            quad.yaw = v.yaw;
            quad.scale = v.scale;
            // The box, at most 5 yards from the model's origin each way (0x6D79A3).
            quad.boxMin = glm::max(glm::vec2(box->min[0], box->min[1]), glm::vec2(-kShadowBoxLimit));
            quad.boxMax = glm::min(glm::vec2(box->max[0], box->max[1]), glm::vec2(kShadowBoxLimit));
            // The floor's plane at the feet (the floor point may be a little behind a moving body).
            const glm::vec2 d(v.feet - s.floorAt);
            const f32 nz = std::max(s.floorNormal.z, 0.3f);
            quad.floorZ = s.floorZ - (s.floorNormal.x * d.x + s.floorNormal.y * d.y) / nz;
            quad.normal = s.floorNormal;
            quad.opacity = v.opacity;
            // 0x6D7D23: the projection reaches 5/3 of the box's half height under the feet (a human's: 1.7
            // yards; jumping higher, the shadow is gone).
            const f32 halfHeight =
                (std::min(box->max[2], kShadowBoxLimit) - std::max(box->min[2], -kShadowBoxLimit)) * 0.5f *
                v.scale;
            if (quad.floorZ < v.feet.z - kShadowReach * halfHeight ||
                quad.floorZ > v.feet.z + kFloorAbove + 0.5) {
                continue; // too high over its floor, or what was found is not under it any more
            }
            m_shadowQuads.push_back(quad);
        }
        m_stats.shadows = static_cast<u32>(m_shadowQuads.size());
    }

    // The bodies' model events: steps and breaths.
    m_events.clear();
    readEvents(frame);
    if (fakeEvents) {
        makeUpEvents(frame);
    }
    for (const Event& event : m_events) {
        if (event.actor < m_bodyOf.size() && m_bodyOf[event.actor] >= 0 &&
            static_cast<u32>(m_bodyOf[event.actor]) < m_bodies.size() &&
            m_bodies[m_bodyOf[event.actor]].view.actor == event.actor) {
            onEvent(frame, m_bodies[m_bodyOf[event.actor]], event);
        }
    }

    // Development: the bleeding demo (--spawn bleed).
    for (DemoBleed& demo : m_demoBleeds) {
        if (demo.actor != Actors::kNone && frame.effects && (demo.timer -= frame.dt) <= 0.0f) {
            demo.timer = 0.6f;
            const glm::dvec3 attacker = demo.feet + glm::dvec3(std::cos(demo.yaw), std::sin(demo.yaw), 0.0) *
                                                        ((demo.hits & 1) ? -2.0 : 2.0);
            bleed(*frame.effects, *frame.actors, demo.actor, demo.feet, demo.yaw, &attacker,
                  (demo.hits & 2) != 0);
            ++demo.hits;
        }
    }

    // Development: the poof demo (--spawn poof).
    if (m_demoPoof.active && frame.effects && (m_demoPoof.timer -= frame.dt) <= 0.0f) {
        m_demoPoof.timer = 2.0f;
        mounted(*frame.effects, m_demoPoof.start, m_demoPoof.yaw, 1.0f);
    }

    // Development: the wake demo (--spawn wake).
    if (DemoWake& demo = m_demoWake; demo.active) {
        constexpr f32 kSpeed = 4.72f, kLength = 12.0f;
        demo.travelled = std::fmod(demo.travelled + kSpeed * frame.dt, 2.0f * kLength);
        const bool back = demo.travelled > kLength;
        const f32 along = back ? 2.0f * kLength - demo.travelled : demo.travelled;
        const f32 heading = demo.yaw + (back ? 3.14159265f : 0.0f);
        const glm::dvec3 at =
            demo.start + glm::dvec3(std::cos(demo.yaw), std::sin(demo.yaw), 0.0) * f64{along};
        if ((demo.timer -= frame.dt) <= 0.0f) {
            if (const std::optional<f32> level =
                    waterLevel(frame, glm::dvec3(at.x, at.y, at.z - 1.5), 10.0f)) {
                const bool turning = along < 0.5f || along > kLength - 0.5f;
                Ripple r =
                    rippleFor(turning ? RippleKind::Idle : RippleKind::Moving, glm::dvec3(at.x, at.y, *level),
                              heading, 1.0f, kSpeed, 1.5f, 2.0f, {random(), random(), random()}, demo.timer);
                m_ripples.add(r, true);
            } else {
                demo.timer = 0.25f;
            }
        }
    }

    // Water and breath, per body.
    for (Body& body : m_bodies) {
        ripples(frame, body);
        BodyState& s = *body.state;
        s.breathTimer -= frame.dt;
        if (s.breathTimer <= 0.0f) {
            // 0x607710: under water deeper than 5 yards past its height, else in a cold zone.
            s.breathTimer += kBreathCheckSeconds;
            s.breath = forceBreath;
            const glm::dvec3& feet = body.view.feet;
            const f32 deep = kUnderwaterBreath + body.view.top;
            if (s.breath != 0) {
                // development: forced
            } else if (const std::optional<f32> level = waterLevel(frame, feet, deep + 50.0f);
                       level && *level - static_cast<f32>(feet.z) > deep) {
                s.breath = 2;
            } else if (frame.areas && frame.terrain && frame.terrain->isOpen() &&
                       game::BodyData::coldArea(*frame.areas, frame.terrain->areaAt(feet.x, feet.y))) {
                s.breath = 1;
            }
        }
    }
    for (const Body& body : m_bodies) {
        m_bodyOf[body.view.actor] = -1;
    }
    m_stats.footprints = m_footprints.alive();
    m_stats.ripples = m_ripples.alive();
    m_stats.updateMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
    if (trace && m_frame % 120 == 0) {
        VD_LOG_INFO(
            "Body effects: %u bodies, %u shadows (%u floor queries), %u footprints, %u ripples, %u effects "
            "spawned, %zu events, %.3f ms",
            m_stats.bodies, m_stats.shadows, m_stats.floorQueries, m_stats.footprints, m_stats.ripples,
            m_stats.spawned, m_events.size(), static_cast<f64>(m_stats.updateMs));
    }
}

void UnitBodyEffects::findFloor(Body& body, const Frame& frame) {
    BodyState& s = *body.state;
    const glm::dvec3& feet = body.view.feet;
    const glm::dvec2 offset(feet - s.floorAt);
    const bool near = glm::dot(offset, offset) < f64{kFloorReuseYards} * kFloorReuseYards &&
                      std::abs(feet.z - s.floorAt.z) < kFloorReuseYards;
    if (s.floorFound && near && m_frame - s.floorFrame < kFloorKeepFrames) {
        return; // the plane found a moment ago still holds
    }
    if (m_stats.floorQueries >= kFloorQueriesPerFrame) {
        return; // over this frame's budget: the old plane (if any) stands in until the next one
    }
    ++m_stats.floorQueries;
    s.floorAt = feet;
    s.floorFrame = m_frame;
    s.floorFound = false;
    const f64 top = feet.z + kFloorAbove, bottom = feet.z - kFloorBelow;
    if (frame.collision && frame.collision->isOpen() && frame.collision->ready(feet.x, feet.y)) {
        // The map's collision: terrain, buildings, doodads (a crate under the feet gets the shadow too).
        m_floorSet.reset(glm::dvec3(feet.x, feet.y, top));
        frame.collision->gather(glm::dvec3(feet.x - 0.01, feet.y - 0.01, bottom),
                                glm::dvec3(feet.x + 0.01, feet.y + 0.01, top), m_floorSet);
        physics::Hit hit;
        if (physics::raycast(m_floorSet, glm::vec3(0.0f),
                             glm::vec3(0.0f, 0.0f, static_cast<f32>(bottom - top)), hit)) {
            s.floorFound = true;
            s.floorZ = top + static_cast<f64>(hit.t) * (bottom - top);
            s.floorNormal = hit.normal.z >= 0.0f ? hit.normal : -hit.normal;
            s.onTerrain = (hit.flags & physics::kTriangleTerrain) != 0;
        }
        return;
    }
    // Beyond the collision streamed around the player: the terrain alone.
    if (frame.terrain && frame.terrain->isOpen()) {
        const std::optional<f32> height = frame.terrain->heightAt(feet.x, feet.y);
        if (height && *height <= top && *height >= bottom) {
            s.floorFound = true;
            s.floorZ = *height;
            s.floorNormal =
                terrainNormal(*frame.terrain, feet.x, feet.y).value_or(glm::vec3(0.0f, 0.0f, 1.0f));
            s.onTerrain = true;
        }
    }
}

void UnitBodyEffects::readEvents(const Frame& frame) {
    // The M2 events the bodies' animations crossed this frame (within Actors::eventDistance of the camera).
    if (fakeEvents) {
        return; // made up instead (development)
    }
    for (const Actors::EventHit& hit : frame.actors->events()) {
        if (m_events.size() < m_events.capacity()) {
            m_events.push_back({hit.actor, hit.id, hit.position});
        }
    }
}

void UnitBodyEffects::makeUpEvents(const Frame& frame) {
    // Development: steps at HumanMale's cadence (Walk: a foot every 0.53 s; Run: every 0.33 s) and a breath
    // every 2.5 s standing, at the feet and the head.
    for (Body& body : m_bodies) {
        const Actors::BodyView& v = body.view;
        BodyState& s = *body.state;
        if (movingAnimation(v.animation) && v.mount == Actors::kNone) {
            s.stepTimer -= frame.dt;
            if (s.stepTimer <= 0.0f) {
                s.stepTimer += walkingAnimation(v.animation) ? 0.533f : 0.333f;
                s.leftFoot = !s.leftFoot;
                const f32 side = (s.leftFoot ? 0.12f : -0.12f) * v.scale;
                const glm::dvec3 at =
                    v.feet + glm::dvec3(-std::sin(v.yaw) * side, std::cos(v.yaw) * side, 0.0);
                const char run[5] = {'$', walkingAnimation(v.animation) ? 'F' : 'R', s.leftFoot ? 'L' : 'R',
                                     '0', 0};
                m_events.push_back({v.actor, pack::actorEventId(run), at});
            }
        } else {
            s.stepTimer = 0.0f;
        }
        if (v.animation == animation::kStand) {
            s.breathEventTimer -= frame.dt;
            if (s.breathEventTimer <= 0.0f) {
                s.breathEventTimer = 2.5f;
                m_events.push_back(
                    {v.actor, pack::actorEventId("$BTH"), v.feet + glm::dvec3(0.0, 0.0, v.top * 0.9f)});
            }
        }
    }
}

void UnitBodyEffects::onEvent(const Frame& frame, const Body& body, const Event& event) {
    const char c1 = static_cast<char>((event.id >> 8) & 0xFF);
    const char c2 = static_cast<char>((event.id >> 16) & 0xFF);
    const char c3 = static_cast<char>((event.id >> 24) & 0xFF);
    // 0x5FFBD0: $<F|B|R|S|W><L|R><0-3> are the feet coming down (walking, backwards, running, shuffling...;
    // the digit is the leg pair).
    if ((event.id & 0xFF) == '$' && (c1 == 'F' || c1 == 'B' || c1 == 'R' || c1 == 'S' || c1 == 'W') &&
        (c2 == 'L' || c2 == 'R') && c3 >= '0' && c3 <= '3') {
        footDown(frame, body, event.position, c2 == 'L');
    } else if (event.id == pack::actorEventId("$BTH")) {
        breathe(frame, body);
    }
}

void UnitBodyEffects::footDown(const Frame& frame, const Body& body, const glm::dvec3& at, bool left) {
    const Actors::BodyView& v = body.view;
    if (!footsteps || !body.model || v.mount != Actors::kNone) {
        return;
    }
    const glm::dvec3 toCamera = at - frame.camera;
    const f64 distance2 = glm::dot(toCamera, toCamera);
    const f64 printReach = kFootprintDistance;
    if (distance2 > printReach * printReach) {
        return; // 0x5FC00A
    }
    const BodyState& s = *body.state;
    const bool onTerrain =
        frame.terrain && frame.terrain->isOpen() &&
        (s.floorFound ? s.onTerrain
                      : frame.terrain->heightAt(at.x, at.y).has_value() &&
                            std::abs(*frame.terrain->heightAt(at.x, at.y) - v.feet.z) < kOnTerrainYards);
    const u32 ground =
        onTerrain ? m_ground.soundAt(at.x, at.y) : 0; // the TerrainType under the foot (-1 off it)
    const bool player = v.actor == frame.playerActor;

    // The footprint (0x5FC025 -> 0x699E70): where the ground keeps them, in the model's own print and size.
    if (ground != 0 && body.model->footprint != 0 && m_data.keepsFootprints(ground)) {
        if (const std::optional<f32> height = frame.terrain->heightAt(at.x, at.y)) {
            u32 texture = 0;
            while (texture < m_footprintTextures.size() &&
                   m_footprintTextures[texture] != body.model->footprint) {
                ++texture;
            }
            if (texture == m_footprintTextures.size()) {
                m_footprintTextures.push_back(body.model->footprint); // one per footprint texture: a handful
            }
            Footprint print;
            print.position = glm::dvec3(at.x, at.y, *height);
            print.normal = terrainNormal(*frame.terrain, at.x, at.y).value_or(glm::vec3(0.0f, 0.0f, 1.0f));
            print.facing = v.yaw;
            print.width = body.model->footprintWidth * v.scale;
            print.length = body.model->footprintLength * v.scale;
            print.texture = texture;
            print.right = !left;
            m_footprints.add(print, player);
        }
    }

    // The spray (0x5FC0B3): near enough, not backwards, from a model that kicks one up.
    const f64 sprayReach = kSprayDistance;
    if (distance2 > sprayReach * sprayReach || (body.model->flags & game::kBodyNoFootstepSprays) ||
        backwardAnimation(v.animation) || !frame.effects) {
        return;
    }
    const bool walking = walkingAnimation(v.animation) || s.speed <= kWalkingSpeed;
    // Wading (0x5FC122): water over the foot, less than half the body's height deep: the water's spray on the
    // surface; deeper, none (it swims).
    if (swimming) {
        if (const std::optional<f32> level = waterLevel(frame, v.feet, body.height * 2.0f)) {
            const f32 depth = *level - static_cast<f32>(v.feet.z);
            if (depth < body.height * kWadingShare) {
                const game::BodyEffect spray = m_data.hardcoded(
                    walking ? game::HardcodedEffect::WaterWalkSpray : game::HardcodedEffect::WaterRunSpray);
                if (EffectHandle* slot = spray.model != 0 ? effectSlot(*frame.effects) : nullptr) {
                    *slot = frame.effects->spawnAt(spray.model, glm::dvec3(at.x, at.y, *level), v.yaw,
                                                   spray.scale, Effects::kOnce);
                    m_stats.spawned += slot->valid() ? 1 : 0;
                }
            }
            return;
        }
    }
    const game::BodyEffect spray = m_data.footstepSpray(ground, walking);
    if (EffectHandle* slot = spray.model != 0 ? effectSlot(*frame.effects) : nullptr) {
        *slot = frame.effects->spawnAt(spray.model, glm::dvec3(at.x, at.y, s.floorFound ? s.floorZ : at.z),
                                       v.yaw, spray.scale, Effects::kOnce);
        m_stats.spawned += slot->valid() ? 1 : 0;
    }
}

void UnitBodyEffects::breathe(const Frame& frame, const Body& body) {
    const BodyState& s = *body.state;
    if (!breath || !frame.effects || s.breath == 0 ||
        (body.model && (body.model->flags & game::kBodyNoBreath)) || body.distance > kSprayDistance * 2.0f) {
        return;
    }
    // 0x5FFFE7: the breath on the model's Breath point (17): bubbles under water, else the cold's.
    const game::BodyEffect effect = m_data.hardcoded(s.breath == 2 ? game::HardcodedEffect::BreathUnderwater
                                                                   : game::HardcodedEffect::BreathCold);
    if (EffectHandle* slot = effect.model != 0 ? effectSlot(*frame.effects) : nullptr) {
        *slot = frame.effects->spawnOn(effect.model, body.view.actor, game::kAttachBreath, effect.scale,
                                       Effects::kOnce);
        m_stats.spawned += slot->valid() ? 1 : 0;
    }
}

void UnitBodyEffects::ripples(const Frame& frame, Body& body) {
    const Actors::BodyView& v = body.view;
    BodyState& s = *body.state;
    if (!swimming || v.mount != Actors::kNone || body.distance > kRippleDistance * m_distanceScale) {
        s.inWater = false;
        return;
    }
    const std::optional<f32> level = waterLevel(frame, v.feet, body.height * 2.0f + 1.0f);
    const f32 depth = level ? *level - static_cast<f32>(v.feet.z) : 0.0f;
    const bool player = v.actor == frame.playerActor;
    // Going in or out (0x603145: past 0.4 of its height): a ring at once (its sound is the audio's).
    const bool in = level.has_value() && depth > body.height * kSplashShare;
    if (!s.waterKnown) {
        s.waterKnown = true; // the first frame in view: where it is, no splash
        s.inWater = in;
    } else if (in != s.inWater) {
        s.inWater = in;
        if (level) {
            f32 next = 0.0f;
            m_ripples.add(rippleFor(RippleKind::Enter, glm::dvec3(v.feet.x, v.feet.y, *level), v.yaw, v.scale,
                                    s.speed, depth, body.height, {random(), random(), random()}, next),
                          player);
            s.rippleTimer = next;
        }
    }
    if (!level || !makesRipples(depth, body.height)) {
        return;
    }
    s.rippleTimer -= frame.dt;
    if (s.rippleTimer > 0.0f) {
        return;
    }
    const bool moving = s.speed > 0.1f;
    const f32 facing = v.yaw + (backwardAnimation(v.animation) ? 3.14159265f : 0.0f);
    f32 next = 0.0f;
    m_ripples.add(rippleFor(moving ? RippleKind::Moving : RippleKind::Idle,
                            glm::dvec3(v.feet.x, v.feet.y, *level), facing, v.scale, s.speed, depth,
                            body.height, {random(), random(), random()}, next),
                  player);
    s.rippleTimer = next;
}

void UnitBodyEffects::bleed(Effects& effects, const Actors& actors, u32 victim, const glm::dvec3& feet,
                            f32 yaw, const glm::dvec3* attacker, bool large) {
    if (!blood || !m_ready) {
        return;
    }
    const game::ActorLook* look = actors.look(victim);
    const game::BodyModel* model = look ? m_data.model(look->model) : nullptr;
    if (!model) {
        return;
    }
    // 0x625010: in front when the attacker stands before the victim (or is unknown), from behind otherwise.
    bool front = true;
    if (attacker) {
        const glm::vec2 toAttacker(glm::dvec2(*attacker) - glm::dvec2(feet));
        front = glm::dot(toAttacker, glm::vec2(std::cos(yaw), std::sin(yaw))) >= 0.0f;
    }
    const game::BodyEffect spurt = m_data.bloodSpurt(model->bloodLevel, front, large, violence);
    if (EffectHandle* slot = spurt.model != 0 ? effectSlot(effects) : nullptr) {
        *slot = effects.spawnOn(spurt.model, victim,
                                front ? game::kAttachChestBloodFront : game::kAttachChestBloodBack,
                                spurt.scale, Effects::kOnce);
        m_stats.spawned += slot->valid() ? 1 : 0;
    }
}

EffectHandle* UnitBodyEffects::effectSlot(const Effects& effects) {
    const u32 budget =
        std::clamp(static_cast<u32>(static_cast<f32>(kMaxEffects) * m_marksScale + 0.5f), 4u, kMaxEffects);
    for (u32 i = 0; i < budget; ++i) {
        if (!effects.alive(m_effectSlots[i])) {
            return &m_effectSlots[i];
        }
    }
    return nullptr;
}

void UnitBodyEffects::demoBleed(u32 actor, const glm::dvec3& feet, f32 yaw) {
    for (DemoBleed& demo : m_demoBleeds) {
        if (demo.actor == Actors::kNone) {
            demo = {actor, feet, yaw, 1.0f, 0};
            return;
        }
    }
}

void UnitBodyEffects::mounted(Effects& effects, const glm::dvec3& feet, f32 yaw, f32 scale) {
    if (!m_ready) {
        return;
    }
    // 0x5FFA50: the poof on the unit's Base point (19), at its feet.
    const game::BodyEffect poof = m_data.hardcoded(game::HardcodedEffect::MountPoof);
    if (EffectHandle* slot = poof.model != 0 ? effectSlot(effects) : nullptr) {
        *slot = effects.spawnAt(poof.model, feet, yaw, poof.scale * scale, Effects::kOnce);
        m_stats.spawned += slot->valid() ? 1 : 0;
    }
}

void UnitBodyEffects::meetingStoneJoined(Effects& effects, const glm::dvec3& feet, f32 yaw, f32 scale) {
    if (!m_ready) {
        return;
    }
    // MeetingStoneInfo.cpp 0x4CA338: the hardcoded kit 12 on the player, its attachment 19 (Base): at its feet.
    const game::BodyEffect join = m_data.hardcoded(game::HardcodedEffect::MeetingStoneJoin);
    if (EffectHandle* slot = join.model != 0 ? effectSlot(effects) : nullptr) {
        *slot = effects.spawnAt(join.model, feet, yaw, join.scale * scale, Effects::kOnce);
        m_stats.spawned += slot->valid() ? 1 : 0;
    }
}

u32 UnitBodyEffects::writeQuads(u32 quads, const glm::dvec3& camera, std::span<ParticleVertex> vertices,
                                TextureQuery texture, const void* textureContext,
                                std::vector<ParticleDraw>& decals, std::vector<ParticleDraw>& particles) {
    if (!m_ready || u64{quads} * 4 >= vertices.size()) {
        return quads;
    }
    VD_PROFILE_ZONE("UnitBodyEffects::writeQuads");
    // Built in cached memory and copied in one go: scattered writes into the mapped buffer are slow
    // (PERFORMANCE.md, CPU rule 1e).
    m_scratch.clear();
    const u64 room = vertices.size() / 4 - quads;
    const auto batch = [&](std::vector<ParticleDraw>& list, TextureHandle handle, pack::ModelBlend blend,
                           u8 flags, u32 first) {
        const u32 count = static_cast<u32>(m_scratch.size() / 4) - first;
        if (count > 0) {
            ParticleDraw draw;
            draw.texture = handle;
            draw.blend = static_cast<u8>(blend);
            draw.flags = flags;
            draw.firstQuad = quads + first;
            draw.quadCount = count;
            list.push_back(draw);
        }
    };
    const auto space = [&]() -> u32 {
        const u64 used = m_scratch.size() / 4;
        return used < room ? static_cast<u32>(std::min<u64>(room - used, m_scratch.capacity() / 4 - used))
                           : 0;
    };
    // Shadows: one draw, multiplied into the ground (Mod), fading towards white with the unit
    // (kParticleDrawShadow).
    if (const TextureHandle blob =
            m_shadowQuads.empty() ? TextureHandle{} : texture(textureContext, kShadowTexture);
        blob.valid()) {
        const u32 first = static_cast<u32>(m_scratch.size() / 4);
        for (const ShadowQuad& shadow : m_shadowQuads) {
            if (space() == 0) {
                break;
            }
            const usize at = m_scratch.size();
            m_scratch.resize(at + 4);
            buildShadow(shadow, camera, std::span<ParticleVertex>(m_scratch.data() + at, 4));
        }
        batch(decals, blob, pack::ModelBlend::Mod, kParticleDrawShadow, first);
    }
    // Footprints: one draw per print texture, alpha blended.
    for (u32 t = 0; t < m_footprintTextures.size() && m_footprints.alive() > 0; ++t) {
        const TextureHandle print = texture(textureContext, m_footprintTextures[t]);
        if (!print.valid() || space() == 0) {
            continue;
        }
        const u32 first = static_cast<u32>(m_scratch.size() / 4);
        const usize at = m_scratch.size();
        m_scratch.resize(at + u64{space()} * 4);
        const u32 written = m_footprints.build(
            t, camera, std::span<ParticleVertex>(m_scratch.data() + at, m_scratch.size() - at));
        m_scratch.resize(at + u64{written} * 4);
        batch(decals, print, pack::ModelBlend::Alpha, 0, first);
    }
    // Ripples: rings and wakes, added over the water in the particle pass.
    for (const bool wake : {false, true}) {
        const TextureHandle image = m_ripples.alive() > 0
                                        ? texture(textureContext, wake ? kWakeTexture : kSplashTexture)
                                        : TextureHandle{};
        if (!image.valid() || space() == 0) {
            continue;
        }
        const u32 first = static_cast<u32>(m_scratch.size() / 4);
        const usize at = m_scratch.size();
        m_scratch.resize(at + u64{space()} * 4);
        const u32 written = m_ripples.build(
            wake, camera, std::span<ParticleVertex>(m_scratch.data() + at, m_scratch.size() - at));
        m_scratch.resize(at + u64{written} * 4);
        batch(particles, image, pack::ModelBlend::Add, 0, first);
    }
    if (!m_scratch.empty()) {
        std::memcpy(vertices.data() + u64{quads} * 4, m_scratch.data(),
                    m_scratch.size() * sizeof(ParticleVertex));
    }
    if (trace && m_frame % 120 == 0) {
        const ParticleVertex* v = m_scratch.empty() ? nullptr : m_scratch.data();
        VD_LOG_INFO("Body effects: %zu quads after %u, %zu decal draws, shadow texture %s, first corner "
                    "(%.2f %.2f %.2f) "
                    "color %08x",
                    m_scratch.size() / 4, quads, decals.size(),
                    texture(textureContext, kShadowTexture).valid() ? "ready" : "not ready",
                    v ? static_cast<f64>(v->position[0]) : 0.0, v ? static_cast<f64>(v->position[1]) : 0.0,
                    v ? static_cast<f64>(v->position[2]) : 0.0, v ? v->color : 0u);
    }
    return quads + static_cast<u32>(m_scratch.size() / 4);
}

} // namespace vd
