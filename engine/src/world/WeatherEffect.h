#pragma once

#include "core/Types.h"
#include "game/Weather.h"
#include "render/Renderer.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>

namespace vd {

// --- The fade ----------------------------------------------------------------------------------------------

// What the world shows of the weather the server asks for (phase 11). A smooth change ramps the intensity by
// kRate a second (light to heavy rain, 0.33 -> 1, in under 7 s); a change of type first takes the old weather
// away the same way, then brings the new one; an instant change (the first weather of a new world) jumps. The
// sound follows the weather being shown: its loop changes when the type does, and when the server names
// another loop for the same type (light, medium, heavy), at once.
class WeatherFade {
public:
    static constexpr f32 kRate = 0.1f;

    void set(game::WeatherType type, f32 grade, u32 sound, bool instant);
    void update(f32 dt);

    game::WeatherType type() const { return m_type; } // being shown (Fine: nothing)
    f32 intensity() const { return m_intensity; }     // of type(), 0..1
    game::WeatherType targetType() const { return m_target; }
    f32 targetGrade() const { return m_targetGrade; }
    bool settled() const { return m_type == m_target && m_intensity == m_targetGrade; }
    u32 sound() const { return m_sound; } // SoundEntries of the shown weather's loop, 0 none
    // How much of its full volume the loop plays at: the intensity shown over the one the loop was named for.
    f32 soundVolume() const;

private:
    game::WeatherType m_type = game::WeatherType::Fine;
    f32 m_intensity = 0.0f;
    u32 m_sound = 0;
    f32 m_soundGrade = 1.0f;
    game::WeatherType m_target = game::WeatherType::Fine;
    f32 m_targetGrade = 0.0f;
    u32 m_targetSound = 0;
};

// --- The ground --------------------------------------------------------------------------------------------

// The ground's height at a world point, NaN where there is none between `top` and `bottom` (the game's
// collision: terrain, buildings, water surfaces).
using WeatherGroundSampler = f32 (*)(const void* context, f64 x, f64 y, f64 top, f64 bottom);

// Where the drops and the flakes land: kSide x kSide world points every kSpacing yards around the camera,
// kept as the camera moves (each slot holds the world point that is congruent to it modulo the side, so
// moving one row only samples that row). Points that come into the window are sampled first, nearest to the
// camera first, up to a budget a frame; then those sampled from a camera height far from today's (the column
// the sampler looks down moved with the camera), and slowly every other one again (the collision streams in
// after the terrain).
class WeatherGround {
public:
    static constexpr u32 kSide = kWeatherGroundSide;
    static constexpr f32 kSpacing = 1.5f;
    static constexpr f32 kNone = -1.0e4f;
    // How far from the camera the points around a position are all in the window: drops and flakes land only
    // there.
    static constexpr f32 kKnownRadius = (kSide / 2 - 2) * kSpacing;
    static constexpr f32 kAbove = 30.0f; // the sampler looks down from this far over the camera
    static constexpr f32 kBelow = 40.0f; // to this far under it
    // A point sampled from a camera this much higher or lower is sampled again.
    static constexpr f32 kRestale = 8.0f;
    static constexpr u32 kRefreshPerFrame = 2;

    WeatherGround();
    void clear();
    // Moves the window to the camera and samples up to `budget` points. Returns how many it sampled.
    u32 update(const glm::dvec3& camera, WeatherGroundSampler sampler, const void* context, u32 budget);
    // A world point's height (kNone: unknown or out of the window).
    f32 at(i32 x, i32 y) const;
    const std::array<f32, kSide * kSide>& heights() const { return m_heights; }
    // The camera's world position modulo the window's span (what the shader adds to find a point's slot).
    static glm::vec2 phase(const glm::dvec3& camera);
    u32 unknown() const { return m_unknown; } // points of the window still to sample

private:
    struct Slot {
        i32 x = 0;
        i32 y = 0;
        f32 sampledFrom = 0.0f; // the camera's z then
        bool sampled = false;
    };
    std::array<f32, kSide * kSide> m_heights{};
    std::array<Slot, kSide * kSide> m_slots{};
    std::array<i8, kSide * kSide * 2> m_order{}; // window offsets nearest first
    u32 m_refresh = 0;
    u32 m_unknown = kSide * kSide;
};

// --- The frame ---------------------------------------------------------------------------------------------

// Particles at the "Weather Intensity" option (WEATHER_DETAIL, the CVar weatherDensity 0..3) for a weather of
// this type at this intensity, scaled by the dynamic quality's effects (QualityGovernor). Full: 6144, the
// client's packet size (MapWeather's Packet<Drop@Rain, 0x1800, 3>, Packet<Flake@Snow, 0x1800, 1>,
// Packet<Grain@Sand, 0x1800, 1>).
constexpr u32 kWeatherMaxParticles = 0x1800;
u32 weatherParticleCount(game::WeatherType type, f32 intensity, u32 density, f32 effectScale);

// What the weather needs from the game each frame.
struct WeatherFrame {
    f32 dt = 0.0f;
    glm::dvec3 camera{0.0};
    glm::vec3 cameraForward{1.0f, 0.0f, 0.0f};
    glm::vec3 cameraRight{0.0f, -1.0f, 0.0f};
    f32 pixelAngle = 0.001f; // radians a pixel covers (vertical field of view over the screen's height)
    f32 fovY = 1.0f;         // the vertical field of view (radians)
    bool indoors = false;    // inside a building's interior: no weather (the client hides it there)
    bool underwater = false; // the camera under a liquid's surface: none either, and no sound
    u32 density = 3;         // WEATHER_DETAIL 0..3
    f32 effectScale = 1.0f;  // the dynamic quality's
    glm::vec3 ambient{0.4f}; // the scene's light (gamma space): the particles take it
    glm::vec3 direct{0.8f};
    glm::vec3 fog{0.5f};
};

// The weather around the camera (phase 11): the fade from the server's weather, whether it can be seen from
// where the camera is, the ground it lands on, and the frame's WeatherUniforms for render/WeatherRenderer.
// Also what the light (Light.dbc's storm sets) and the sound take from it. Main thread; nothing allocates.
class WeatherEffect {
public:
    // A new weather from the server (or the development switch). A new seed (wind) comes with a new type.
    void set(const game::WeatherState& state);
    void reset(); // another map: fine at once, the ground forgotten

    // Once per frame. `sampler` may be null (no ground: drops fall through, hidden by the depth test).
    void update(const WeatherFrame& frame, WeatherGroundSampler sampler, const void* context);

    // The frame's weather for the renderer; null when there is nothing to draw.
    const WeatherUniforms* uniforms() const { return m_uniforms.time.y != 0 ? &m_uniforms : nullptr; }
    const WeatherFade& fade() const { return m_fade; }
    const WeatherGround& ground() const { return m_ground; }
    // Light.dbc's storm blend: the intensity shown, whatever the type (also indoors, where the light is the
    // building's).
    f32 storm() const { return m_fade.type() == game::WeatherType::Fine ? 0.0f : m_fade.intensity(); }
    // The weather's loop and its volume (0..1 over its SoundEntries volume): the fade, quieter indoors,
    // silent under water.
    u32 sound() const { return m_fade.sound(); }
    f32 soundVolume() const { return m_soundVolume; }
    f32 visibility() const { return m_visible; } // 0 indoors and under water, 1 outdoors (faded)

    // Development: the cost of the last update and of the ground's sampling.
    struct Stats {
        u32 particles = 0;
        u32 mists = 0;
        u32 groundSampled = 0; // points sampled this frame
        f32 groundMs = 0.0f;   // sampling them
        f32 updateMs = 0.0f;   // all of it
    };
    const Stats& stats() const { return m_stats; }

    // Per second: in or out of a building in a quarter of a second.
    static constexpr f32 kIndoorFadeRate = 4.0f;
    static constexpr f32 kIndoorVolume = 0.35f; // the weather heard from inside a building
    static constexpr u32 kGroundBudget = 48;    // points sampled a frame

private:
    void motionBlur(const glm::vec3& fall);

    WeatherFade m_fade;
    WeatherGround m_ground;
    WeatherUniforms m_uniforms{};
    Stats m_stats;
    f32 m_visible = 1.0f;
    f32 m_soundVolume = 0.0f;
    u32 m_seed = 1;
    game::WeatherType m_seededType = game::WeatherType::Fine;
    u64 m_ticks = 0; // the particles' clock (0.1 ms)
    f64 m_tickRemainder = 0.0;
    glm::dvec3 m_lastCamera{0.0};
    bool m_hasLastCamera = false;
    glm::vec3 m_cameraVelocity{0.0f}; // smoothed, for the motion blur
};

} // namespace vd
