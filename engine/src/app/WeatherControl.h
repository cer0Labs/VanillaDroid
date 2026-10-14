#pragma once

#include "core/Types.h"
#include "game/Weather.h"
#include "physics/Collision.h"
#include "world/WeatherEffect.h"

#include <glm/vec3.hpp>

#include <string_view>

namespace vd {

class Renderer;
class Terrain;
struct Camera;
struct SceneLighting;
namespace pack {
class PackReader;
}
namespace physics {
class CollisionWorld;
}

// The weather of the game (phase 11): what the server says (game::Weather) or what development forces, shown
// by world/WeatherEffect over the ground the collision knows (terrain, buildings and water; not the doodads:
// trees and tents let the rain through), loaded with the client's textures. Game glue: Game calls update()
// once a frame with this frame's light and reads what it gives the renderer, the light and the sound.
class WeatherControl {
public:
    // The client's weather textures from textures.vdpk into the renderer, and the forced weather: `forced`
    // (PlatformInfo::weather) or else VD_WEATHER.
    void init(const pack::PackReader& textures, Renderer& renderer, std::string_view forced = {});

    struct Inputs {
        f32 dt = 0.0f;
        const Camera* camera = nullptr;
        f32 screenHeight = 1.0f; // pixels
        std::string_view map;    // the map shown: the ground is forgotten when it changes
        bool indoors = false;
        bool underwater = false;
        u32 density = 3;        // WEATHER_DETAIL
        f32 effectScale = 1.0f; // the dynamic quality's
        const SceneLighting* light = nullptr;
        const game::Weather* server = nullptr; // online in the world; null offline
        const physics::CollisionWorld* collision = nullptr;
        const Terrain* terrain = nullptr;
    };
    void update(const Inputs& in);

    const WeatherUniforms* uniforms() const { return m_effect.uniforms(); }
    f32 storm() const { return m_effect.storm(); } // Light.dbc's storm blend
    u32 sound() const { return m_effect.sound(); }
    f32 soundVolume() const { return m_effect.soundVolume(); }
    const WeatherEffect& effect() const { return m_effect; }

    void drawPanel(); // development (ImGui): the weather shown, forcing one, its cost

private:
    static f32 sampleGround(const void* self, f64 x, f64 y, f64 top, f64 bottom);

    WeatherEffect m_effect;
    bool m_forced = false; // development: VD_WEATHER or the panel, over the server's
    game::WeatherState m_forcedState;
    bool m_forcedPending = false;
    bool m_released = false; // the forced weather was let go this frame
    u32 m_serverChanges = ~0u;
    bool m_online = false;
    u64 m_mapHash = 0;
    const physics::CollisionWorld* m_collision = nullptr;
    const Terrain* m_terrain = nullptr;
    mutable physics::TriangleSet m_triangles{256};
    // The panel's choice.
    i32 m_panelType = 1;
    f32 m_panelGrade = 0.7f;
    bool m_panelInstant = false;
    f32 m_worstMs = 0.0f;
    // Logged every kLogSeconds while there is weather: what it cost the main thread.
    static constexpr f32 kLogSeconds = 10.0f;
    f32 m_logSeconds = 0.0f;
    u32 m_logFrames = 0;
    f64 m_logMs = 0.0;
    f32 m_logMaxMs = 0.0f;
    u64 m_logSamples = 0;
    f64 m_logGroundMs = 0.0;
    u32 m_logSlow = 0; // frames over kSlowMs
};

} // namespace vd
