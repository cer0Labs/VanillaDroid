#include "world/WeatherEffect.h"

#include "core/Clock.h"
#include "core/Profile.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace vd {
namespace {

using game::WeatherType;

// How each weather moves and looks. The client's vertex programs say how a particle moves once it is born
// (start + velocity * age), how it fades and how a drop and its splash are built (Vertex_rain's vtxCoordsY =
// +-0.05 and vtxCoordsZ = 2 yards; Vertex_patter's 4 x 4 frames at 15.96 a second; Vertex_snowpoint's first
// second fading in and quarter of a second settling; Vertex_sand's fifth of a second at both ends); the
// speeds, sizes and boxes are MapWeather's CPU side, which is not in them: chosen by looking at the result
// next to the client's look.
struct Kind {
    f32 box = 42.0f;      // horizontal side of the box around the camera (yards; landing needs it within
                          // WeatherGround's known radius)
    f32 height = 20.0f;   // its height
    f32 bottom = 5.0f;    // how far its bottom is under the camera
    f32 fall = 0.0f;      // yd/s down
    f32 wind = 0.0f;      // yd/s along this weather's wind
    f32 jitter = 0.0f;    // random horizontal yd/s of each particle
    f32 jitterZ = 0.0f;   // random vertical yd/s
    f32 life = 0.0f;      // s (0: the time to fall through the box)
    f32 lifeExtra = 0.0f; // s, random
    f32 size = 0.0f;      // yards across
    f32 minPixels = 1.5f; // smallest size on screen
    f32 maxScreen = 0.0f; // largest, a share of the screen's height (0: none)
    f32 opacity = 1.0f;
    f32 fadeIn = 0.0f;    // s
    f32 fadeOut = 0.0f;   // s
    f32 landFade = 0.0f;  // s; 0: does not land
    glm::vec3 tint{1.0f}; // over the scene's light
};
constexpr Kind kRain = {.box = 42.0f,
                        .height = 32.0f,
                        .bottom = 10.0f,
                        .fall = 24.0f,
                        .wind = 1.5f,
                        .jitter = 0.3f,
                        .minPixels = 2.0f,
                        .opacity = 0.9f};
constexpr Kind kSnow = {.box = 32.0f,
                        .height = 20.0f,
                        .bottom = 5.0f,
                        .fall = 2.2f,
                        .wind = 0.6f,
                        .jitter = 0.8f,
                        .jitterZ = 0.4f,
                        .life = 8.0f,
                        .lifeExtra = 4.0f,
                        .size = 0.15f,
                        .maxScreen = 0.035f,
                        .fadeIn = 1.0f,
                        .fadeOut = 0.5f,
                        .landFade = 0.25f};
constexpr Kind kSand = {.box = 42.0f,
                        .height = 16.0f,
                        .bottom = 4.0f,
                        .wind = 14.0f,
                        .jitter = 3.0f,
                        .jitterZ = 1.0f,
                        .life = 1.5f,
                        .lifeExtra = 1.0f,
                        .size = 1.6f,
                        // Vertex_sand's grains are point sprites: their size saturates at a few dozen pixels (64 at
                        // most on the client's hardware, a twelfth of a 768-line screen), never a quarter of it.
                        .maxScreen = 0.08f,
                        .fadeIn = 0.2f,
                        .fadeOut = 0.2f,
                        .tint = glm::vec3(0.86f, 0.72f, 0.52f)};

// The mists over the ground: snow's SnowMist01, sand's bigger puffs of its grains' texture.
struct Mist {
    u32 count = 0;
    f32 box = 50.0f;
    f32 halfSize = 6.0f; // yards
    f32 height = 0.5f; // of its rounded-off lowest point over the ground (plus up to as much again): a sprite
                       // through the ground would show a hard line where it goes under
    f32 wind = 1.0f;   // share of the weather's wind
    f32 life = 10.0f;  // s
    f32 lifeExtra = 4.0f;
    f32 opacity = 0.8f;
    f32 fade = 3.0f; // s at both ends
};
constexpr Mist kSnowMist = {.count = 14,
                            .halfSize = 6.0f,
                            .height = 0.5f,
                            .wind = 1.0f,
                            .life = 10.0f,
                            .lifeExtra = 4.0f,
                            .opacity = 0.8f,
                            .fade = 3.0f};
constexpr Mist kSandMist = {.count = 12,
                            .halfSize = 8.0f,
                            .height = 1.0f,
                            .wind = 0.9f,
                            .life = 4.0f,
                            .lifeExtra = 2.0f,
                            .opacity = 0.9f,
                            .fade = 1.0f};

constexpr f32 kDropHalfWidth = 0.05f; // Vertex_rain's vtxCoordsY
constexpr f32 kDropTail = 2.0f;       // and vtxCoordsZ
constexpr f32 kSplashSize = 0.4f;     // yards across a splash's frame
constexpr f32 kSplashOpacity = 0.8f;
constexpr f32 kSplashTime = 4.0f / 15.96f; // its four frames (Vertex_patter)
constexpr f32 kMaxTailStretch = 2.0f;  // a camera moving through the rain stretches the drops up to this much
constexpr f32 kCameraSmoothing = 0.1f; // s: the camera's velocity for the motion blur
constexpr f32 kCameraJump = 30.0f;     // yards in a frame: a teleport, not a motion
constexpr f32 kMaxCameraSpeed = 40.0f;
constexpr f32 kDensity[4] = {0.25f, 0.5f, 0.75f, 1.0f}; // weatherDensity 0..3

const Kind* kindOf(WeatherType type) {
    switch (type) {
    case WeatherType::Rain: return &kRain;
    case WeatherType::Snow: return &kSnow;
    case WeatherType::Sandstorm: return &kSand;
    case WeatherType::Fine: break;
    }
    return nullptr;
}

const Mist* mistOf(WeatherType type) {
    switch (type) {
    case WeatherType::Snow: return &kSnowMist;
    case WeatherType::Sandstorm: return &kSandMist;
    default: break;
    }
    return nullptr;
}

f32 approach(f32 value, f32 target, f32 step) {
    return value < target ? std::min(target, value + step) : std::max(target, value - step);
}

f32 positiveMod(f64 value, f64 span) {
    const f64 m = std::fmod(value, span);
    return static_cast<f32>(m < 0.0 ? m + span : m);
}

u32 ticks(f32 seconds) {
    return static_cast<u32>(std::max(1.0f, seconds * 10000.0f));
}

u32 nextSeed(u32 seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed != 0 ? seed : 1u;
}

} // namespace

// --- The fade ----------------------------------------------------------------------------------------------

void WeatherFade::set(WeatherType type, f32 grade, u32 sound, bool instant) {
    m_target = type;
    m_targetGrade = type == WeatherType::Fine ? 0.0f : std::clamp(grade, 0.0f, 1.0f);
    m_targetSound = type == WeatherType::Fine ? 0u : sound;
    if (instant) {
        m_type = type;
        m_intensity = m_targetGrade;
    }
    if (instant || m_type == type) {
        m_sound = m_targetSound;
        m_soundGrade = m_targetGrade;
    }
}

void WeatherFade::update(f32 dt) {
    const f32 step = kRate * std::max(dt, 0.0f);
    if (m_type != m_target) {
        if (m_type != WeatherType::Fine && m_intensity > 0.0f) {
            m_intensity = std::max(0.0f, m_intensity - step); // the old weather goes first
            return;
        }
        m_type = m_target;
        m_intensity = 0.0f;
        m_sound = m_targetSound;
        m_soundGrade = m_targetGrade;
    }
    m_intensity = approach(m_intensity, m_targetGrade, step);
}

f32 WeatherFade::soundVolume() const {
    if (m_sound == 0 || m_type == WeatherType::Fine) {
        return 0.0f;
    }
    return std::clamp(m_intensity / std::max(m_soundGrade, 0.05f), 0.0f, 1.0f);
}

// --- The ground --------------------------------------------------------------------------------------------

namespace {

u32 slotOf(i32 x, i32 y) {
    constexpr u32 kMask = WeatherGround::kSide - 1;
    return (static_cast<u32>(y) & kMask) * WeatherGround::kSide + (static_cast<u32>(x) & kMask);
}

// The window: points [c - (side/2 - 1), c + side/2] around the camera's point c = floor(camera / spacing).
constexpr i32 kWindowLow = -static_cast<i32>(WeatherGround::kSide / 2 - 1);

} // namespace

WeatherGround::WeatherGround() {
    std::array<u16, kSide * kSide> order;
    std::iota(order.begin(), order.end(), u16{0});
    const auto distance = [](u16 i) {
        // The camera lies between its point and the next: the nearest points are around (0.5, 0.5).
        const f32 dx = static_cast<f32>(static_cast<i32>(i % kSide) + kWindowLow) - 0.5f;
        const f32 dy = static_cast<f32>(static_cast<i32>(i / kSide) + kWindowLow) - 0.5f;
        return dx * dx + dy * dy;
    };
    std::stable_sort(order.begin(), order.end(), [&](u16 a, u16 b) { return distance(a) < distance(b); });
    for (u32 k = 0; k < order.size(); ++k) {
        m_order[k * 2] = static_cast<i8>(static_cast<i32>(order[k] % kSide) + kWindowLow);
        m_order[k * 2 + 1] = static_cast<i8>(static_cast<i32>(order[k] / kSide) + kWindowLow);
    }
    clear();
}

void WeatherGround::clear() {
    m_heights.fill(kNone);
    m_slots.fill(Slot{});
    m_refresh = 0;
    m_unknown = kSide * kSide;
}

u32 WeatherGround::update(const glm::dvec3& camera, WeatherGroundSampler sampler, const void* context,
                          u32 budget) {
    const i32 cx = static_cast<i32>(std::floor(camera.x / kSpacing));
    const i32 cy = static_cast<i32>(std::floor(camera.y / kSpacing));
    const f32 cz = static_cast<f32>(camera.z);
    u32 sampled = 0;
    m_unknown = 0;
    const auto sample = [&](u32 slot, i32 x, i32 y) {
        const f32 h = sampler(context, static_cast<f64>(x) * kSpacing, static_cast<f64>(y) * kSpacing,
                              camera.z + kAbove, camera.z - kBelow);
        m_heights[slot] = std::isnan(h) ? kNone : h;
        m_slots[slot].sampled = true;
        m_slots[slot].sampledFrom = cz;
        ++sampled;
    };
    for (u32 k = 0; k < kSide * kSide; ++k) {
        const i32 x = cx + m_order[k * 2];
        const i32 y = cy + m_order[k * 2 + 1];
        const u32 slot = slotOf(x, y);
        Slot& s = m_slots[slot];
        if (s.x != x || s.y != y) {
            // Another point of the world now: unknown until sampled.
            s = Slot{x, y, 0.0f, false};
            m_heights[slot] = kNone;
        }
        if (s.sampled && std::fabs(cz - s.sampledFrom) <= kRestale) {
            continue;
        }
        if (sampler && sampled < budget) {
            sample(slot, x, y);
        } else if (!s.sampled) {
            ++m_unknown;
        }
    }
    // The rest again, slowly: the collision streams in after the terrain.
    for (u32 i = 0; sampler && i < kRefreshPerFrame; ++i) {
        const u32 slot = m_refresh++ % (kSide * kSide);
        if (m_slots[slot].sampled) {
            sample(slot, m_slots[slot].x, m_slots[slot].y);
        }
    }
    return sampled;
}

f32 WeatherGround::at(i32 x, i32 y) const {
    const u32 slot = slotOf(x, y);
    const Slot& s = m_slots[slot];
    return s.sampled && s.x == x && s.y == y ? m_heights[slot] : kNone;
}

glm::vec2 WeatherGround::phase(const glm::dvec3& camera) {
    constexpr f64 kSpan = static_cast<f64>(kSide) * kSpacing;
    return {positiveMod(camera.x, kSpan), positiveMod(camera.y, kSpan)};
}

// --- The frame ---------------------------------------------------------------------------------------------

u32 weatherParticleCount(WeatherType type, f32 intensity, u32 density, f32 effectScale) {
    if (!kindOf(type)) {
        return 0;
    }
    const f32 share = std::clamp(intensity, 0.0f, 1.0f) * kDensity[std::min(density, 3u)] *
                      std::clamp(effectScale, 0.0f, 1.0f);
    return static_cast<u32>(std::lround(static_cast<f32>(kWeatherMaxParticles) * share));
}

void WeatherEffect::set(const game::WeatherState& state) {
    if (state.type != WeatherType::Fine && state.type != m_seededType) {
        // A new kind of weather: a new wind (direction) and new particles.
        m_seed = nextSeed(m_seed + 0x9E3779B9u);
        m_seededType = state.type;
    }
    m_fade.set(state.type, state.grade, state.sound, state.instant);
}

void WeatherEffect::reset() {
    m_fade.set(WeatherType::Fine, 0.0f, 0, true);
    m_ground.clear();
    m_hasLastCamera = false;
    m_cameraVelocity = glm::vec3(0.0f);
}

void WeatherEffect::motionBlur(const glm::vec3& fall) {
    // The drops as the moving camera sees them: their fall minus its own motion. The client turns each drop's
    // shape from its fall towards that (c_motionBlurRotateMtx) and stretches its tail (c_tailLength).
    const glm::vec3 seen = fall - m_cameraVelocity;
    const f32 fallSpeed = glm::length(fall);
    const f32 seenSpeed = glm::length(seen);
    m_uniforms.blur[0] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    m_uniforms.blur[1] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    m_uniforms.blur[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    m_uniforms.rain.y = kDropTail;
    if (fallSpeed < 1.0e-3f || seenSpeed < 1.0e-3f) {
        return;
    }
    m_uniforms.rain.y = kDropTail * std::clamp(seenSpeed / fallSpeed, 1.0f, kMaxTailStretch);
    const glm::vec3 a = fall / fallSpeed;
    const glm::vec3 b = seen / seenSpeed;
    const glm::vec3 axis = glm::cross(a, b);
    const f32 s = glm::length(axis);
    const f32 c = glm::dot(a, b);
    if (s < 1.0e-4f) {
        return; // straight along the fall (or right against it: the tail is then the head, as good)
    }
    // Rodrigues: the rotation by the angle between them about their common perpendicular.
    const glm::vec3 k = axis / s;
    const f32 t = 1.0f - c;
    m_uniforms.blur[0] = glm::vec4(c + t * k.x * k.x, t * k.x * k.y - s * k.z, t * k.x * k.z + s * k.y, 0.0f);
    m_uniforms.blur[1] = glm::vec4(t * k.x * k.y + s * k.z, c + t * k.y * k.y, t * k.y * k.z - s * k.x, 0.0f);
    m_uniforms.blur[2] = glm::vec4(t * k.x * k.z - s * k.y, t * k.y * k.z + s * k.x, c + t * k.z * k.z, 0.0f);
}

void WeatherEffect::update(const WeatherFrame& frame, WeatherGroundSampler sampler, const void* context) {
    VD_PROFILE_ZONE("WeatherEffect::update");
    const Clock::time_point start = Clock::now();
    const f32 dt = std::max(frame.dt, 0.0f);
    m_stats = {};
    m_fade.update(dt);

    // Not inside buildings nor under water; out of a door it comes back in a moment.
    m_visible =
        frame.underwater ? 0.0f : approach(m_visible, frame.indoors ? 0.0f : 1.0f, kIndoorFadeRate * dt);
    m_soundVolume =
        frame.underwater ? 0.0f : m_fade.soundVolume() * (kIndoorVolume + (1.0f - kIndoorVolume) * m_visible);

    // The particles' clock, and the camera's motion (the drops' motion blur).
    m_tickRemainder += static_cast<f64>(dt) * 10000.0;
    const f64 whole = std::floor(m_tickRemainder);
    m_ticks += static_cast<u64>(whole);
    m_tickRemainder -= whole;
    if (m_hasLastCamera && dt > 0.0f) {
        const glm::dvec3 moved = frame.camera - m_lastCamera;
        if (glm::length(moved) > static_cast<f64>(kCameraJump)) {
            m_cameraVelocity = glm::vec3(0.0f);
        } else {
            glm::vec3 velocity = glm::vec3(moved / static_cast<f64>(dt));
            const f32 speed = glm::length(velocity);
            if (speed > kMaxCameraSpeed) {
                velocity *= kMaxCameraSpeed / speed;
            }
            const f32 blend = 1.0f - std::exp(-dt / kCameraSmoothing);
            m_cameraVelocity += (velocity - m_cameraVelocity) * blend;
        }
    }
    m_lastCamera = frame.camera;
    m_hasLastCamera = true;

    m_uniforms.time = glm::uvec4(static_cast<u32>(m_ticks), 0u, 0u, 0u);
    const WeatherType type = m_fade.type();
    const Kind* kind = kindOf(type);
    if (!kind || (m_fade.targetType() == WeatherType::Fine && m_fade.intensity() <= 0.0f)) {
        m_stats.updateMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
        return;
    }
    // The ground under the box (only while there is weather: nothing to land otherwise).
    const Clock::time_point groundStart = Clock::now();
    m_stats.groundSampled = m_ground.update(frame.camera, sampler, context, kGroundBudget);
    m_stats.groundMs = static_cast<f32>(secondsBetween(groundStart, Clock::now()) * 1000.0);
    const u32 count = weatherParticleCount(type, m_fade.intensity(), frame.density, frame.effectScale);
    if (count == 0 || m_visible <= 0.0f) {
        m_stats.updateMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
        return;
    }

    const glm::vec3 lit = glm::min(frame.ambient + frame.direct, glm::vec3(1.0f));
    const glm::vec3 right = frame.cameraRight;
    const glm::vec3 up = glm::normalize(glm::cross(right, frame.cameraForward));
    // This weather's wind: a direction of its own (the seed), flat.
    const f32 angle = static_cast<f32>(m_seed % 3600u) * (6.2831853f / 3600.0f);
    const glm::vec3 wind(std::cos(angle), std::sin(angle), 0.0f);
    const glm::vec3 fall = wind * kind->wind - glm::vec3(0.0f, 0.0f, kind->fall);

    m_uniforms.time.y = static_cast<u32>(type);
    m_uniforms.cameraRight = glm::vec4(right, kSplashOpacity * m_visible);
    m_uniforms.cameraUp = glm::vec4(up, static_cast<f32>(frame.camera.z));
    m_uniforms.rain = glm::vec4(kDropHalfWidth, kDropTail, kSplashSize, kSplashTime);
    motionBlur(fall);
    m_uniforms.ground =
        glm::vec4(WeatherGround::phase(frame.camera), WeatherGround::kSpacing, WeatherGround::kKnownRadius);
    std::memcpy(m_uniforms.heights, m_ground.heights().data(), sizeof(m_uniforms.heights));

    WeatherParticles& p = m_uniforms.particles;
    p.box = glm::vec4(kind->box, kind->box, kind->height, kind->bottom);
    p.phase = glm::vec4(positiveMod(frame.camera.x, kind->box), positiveMod(frame.camera.y, kind->box),
                        positiveMod(frame.camera.z, kind->height), 0.0f);
    p.velocity = glm::vec4(fall, kind->jitter);
    p.shape =
        glm::vec4(kind->jitterZ, kind->size, kind->minPixels * frame.pixelAngle, kind->opacity * m_visible);
    p.fades =
        glm::vec4(kind->fadeIn, kind->fadeOut, kind->landFade, type == WeatherType::Sandstorm ? 1.0f : 0.0f);
    p.color = glm::vec4(kind->tint * lit, kind->maxScreen * frame.fovY);
    // A drop falls through the whole box in one life: wherever it is, it is somewhere in its fall.
    const f32 life = kind->fall > 0.0f && kind->life <= 0.0f ? kind->height / kind->fall : kind->life;
    p.life = glm::uvec4(count, ticks(life), kind->lifeExtra > 0.0f ? ticks(kind->lifeExtra) : 0u, m_seed);
    m_stats.particles = count;

    WeatherParticles& m = m_uniforms.mist;
    m = {};
    if (const Mist* mist = mistOf(type)) {
        m.box = glm::vec4(mist->box, mist->box, 1.0f, 0.0f);
        m.phase = glm::vec4(positiveMod(frame.camera.x, mist->box), positiveMod(frame.camera.y, mist->box),
                            0.0f, 0.0f);
        m.velocity = glm::vec4(wind * (kind->wind * mist->wind), kind->jitter * 0.5f);
        m.shape = glm::vec4(0.0f, mist->halfSize, 0.0f,
                            mist->opacity * std::min(1.0f, m_fade.intensity() * 1.5f) * m_visible);
        m.fades = glm::vec4(mist->fade, mist->fade, 0.0f, mist->height);
        m.color = glm::vec4(kind->tint * lit, 0.0f);
        m.life = glm::uvec4(mist->count, ticks(mist->life), ticks(mist->lifeExtra), m_seed ^ 0xA5A5A5A5u);
        m_stats.mists = mist->count;
    }
    m_stats.updateMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
}

} // namespace vd
