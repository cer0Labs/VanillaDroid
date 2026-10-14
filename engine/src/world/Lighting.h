#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "db/DbcView.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <string_view>
#include <vector>

namespace vd {

namespace pack {
class PackReader;
}

// Which of the five LightParams sets of every light applies (Light.dbc keeps one per condition: clear, clear
// underwater, storm, storm underwater and death).
struct LightConditions {
    f32 storm = 0.0f;        // weather intensity 0..1: each light blends from its clear set towards its storm set
    bool underwater = false; // the camera is under a liquid's surface: the underwater sets
    bool dead = false;       // the player is a ghost: the death sets replace all the others
};

// Outdoor light at one place and time, from Light/LightParams/LightIntBand/LightFloatBand.dbc.
// Colors are gamma-space like the original client (no sRGB conversion anywhere in the pipeline).
struct LightingState {
    glm::vec3 direct{0.8f};  // "global diffuse": sun (day) or moon (night) light
    glm::vec3 ambient{0.4f};
    std::array<glm::vec3, 5> sky{}; // zenith -> horizon
    glm::vec3 fog{0.5f};
    glm::vec3 shadow{0.3f};  // band 8: the color of shadows
    glm::vec3 sun{1.0f};     // band 9: the sun's (or the moon's) disc
    glm::vec3 sunHalo{1.0f}; // band 10: the glow around it
    glm::vec3 cloudEmissive{0.0f}; // band 11
    glm::vec3 cloudLayer1{0.0f};   // band 12: the clouds' ambient color, first layer
    glm::vec3 cloudLayer2{0.0f};   // band 13: second layer
    glm::vec3 riverClose{0.1f}, riverFar{0.1f}, oceanClose{0.1f}, oceanFar{0.05f};
    f32 fogEnd = 500.0f;      // yards
    f32 fogStartScale = 0.25f; // fog start = end * this (negative: it starts behind the camera)
    f32 celestialGlow = 1.0f;  // float band 2: how much of the sun and the moon shows through the sky
    f32 cloudDensity = 0.5f;   // float band 3
    f32 riverShallowAlpha = 0.5f, riverDeepAlpha = 1.0f;
    f32 oceanShallowAlpha = 0.75f, oceanDeepAlpha = 1.0f;
    // From the LightParams set that weighs most here (the global one unless a local light covers over half).
    u32 skybox = 0;            // LightSkybox.dbc id, 0 = none (the death sets' DeathClouds)
    bool highlightSky = false; // LightParams.highlightSky
    f32 glow = 0.5f;           // LightParams.glow: strength of the client's full-screen glow
    glm::vec3 lightDirection{0.0f, 0.0f, 1.0f}; // unit vector towards the sun/moon
};

// Evaluates the client's outdoor lighting for a position and time of day. The global light of the map
// is blended with the local lights in range (full inside the inner radius, fading to the outer one).
class WorldLighting {
public:
    // Game time in half-minutes, like the client (0..2880, 1440 = noon).
    static constexpr f32 kDayLength = 2880.0f;

    Result<void> init(const pack::PackReader& dbPack);
    void setMap(u32 mapId);
    bool ready() const { return m_ready; }

    LightingState evaluate(const glm::dvec3& position, f32 timeOfDay, const LightConditions& conditions = {}) const;
    // LightSkybox.dbc: the model of a skybox id ("Environments\Stars\DeathClouds.mdx"), empty when unknown.
    std::string_view skyboxModel(u32 skybox) const;

private:
    // Light.dbc's parameter sets per condition.
    enum ParamsSet : u32 { kClear = 0, kClearUnderwater = 1, kStorm = 2, kStormUnderwater = 3, kDeath = 4 };
    struct Light {
        glm::vec2 xy; // world X, Y
        f32 z;
        f32 inner;
        f32 outer;
        std::array<u32, 5> params; // LightParams per ParamsSet
    };
    struct Bands {
        std::array<glm::vec3, 18> colors;
        std::array<f32, 6> floats;
        std::array<f32, 4> waterAlphas;
    };

    Bands evaluateParams(u32 params, f32 time) const;
    // The bands of one light under `conditions` (its clear or underwater set, blended towards the storm one).
    Bands evaluateLight(const std::array<u32, 5>& params, f32 time, const LightConditions& conditions) const;
    static void blend(Bands& into, const Bands& from, f32 weight);
    static u32 dominantParams(const std::array<u32, 5>& params, const LightConditions& conditions);

    db::DbcView m_light;
    db::DbcView m_params;
    db::DbcView m_intBand;
    db::DbcView m_floatBand;
    db::DbcView m_skybox; // optional
    std::vector<u32> m_paramsIndex; // id -> record, ~0u if absent
    std::vector<u32> m_intIndex;
    std::vector<u32> m_floatIndex;
    std::vector<Light> m_lights; // local lights of the current map
    std::array<u32, 5> m_globalParams{};
    bool m_ready = false;
};

} // namespace vd
