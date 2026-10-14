#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "pack/PackReader.h"
#include "pack/SkyFormat.h"
#include "physics/Collision.h"
#include "render/Renderer.h"
#include "world/Camera.h"
#include "world/Lighting.h"

#include <array>
#include <filesystem>
#include <vector>

namespace vd {

namespace physics {
class CollisionWorld;
}
class WorldObjects;

// Phase 11: the sky of the 1.12 client around the camera (world/Celestial.h has its curves): the sun, the
// White Lady and the Blue Child where WoW.exe puts them, colored by LightIntBand 9 and hidden by storms; the
// stars model at night; the clouds (a noise made here once, shaped by the cloud bands); a skybox when
// Light.dbc (the ghost's DeathClouds) or the WMO around the camera (MOSB) asks for one; and the glare around
// the sun and the moon, which fades while the world hides them (a ray through the collision world a few times
// a second). Everything is loaded at open() from models.vdpk and textures.vdpk; update() only computes. Game
// thread only.
class Sky {
public:
    Result<void> open(const std::filesystem::path& packsDir, const pack::PackReader& textures,
                      const WorldLighting& lighting, Renderer& renderer);
    bool isOpen() const { return m_open; }

    // Development overrides of the light conditions, after the game set them (VD_STORM=0..1: the weather's
    // intensity until the weather drives it).
    void overrideConditions(LightConditions& conditions) const;

    // Per frame, after the scene's lighting. dayFraction: 0 = midnight; day: whole game days (the Blue
    // Child); objects (WorldObjects::skyboxAt, the doodads' crowns) and collision (may be closed): what can
    // stand in front of the sun or the moon, for the glare (either may be null).
    void update(f32 dt, f32 dayFraction, u32 day, const Camera& camera, const SceneLighting& lighting,
                const WorldObjects* objects, const physics::CollisionWorld* collision, f32 viewDistance);
    const SkyFrame& frame() const { return m_frame; }

    // Development: the skybox forced by VD_SKYBOX (a LightSkybox.dbc id; 0 = none forced).
    u32 forcedSkybox() const { return m_forcedSkybox; }

private:
    struct Model {
        u64 hash = 0; // pack::skyModelPath
        SkyModelHandle handle;
        std::vector<pack::SkyRotationKey> rotation;
        u32 loop = 0;
    };
    struct Glare {
        f32 visibility = 0.0f; // eased towards its target, as WoW.exe does
        bool occluded = false;
    };
    // The glare's occlusion ray, one leg of the collision world per frame so no frame pays for the whole ray.
    struct Probe {
        bool running = false;
        u32 glare = 0;
        glm::dvec3 from{0.0};
        glm::vec3 direction{0.0f, 0.0f, 1.0f};
        f32 reach = 0.0f;
        u32 nextDoodad = 0; // the doodads' spheres first, kOcclusionDoodads instance slots per frame
        f32 next = 0.0f;    // then the collision world: where the next leg starts, yards along the ray
    };

    Result<void> loadModel(const pack::PackAsset& asset, u64 hash, const pack::PackReader& textures,
                           Renderer& renderer);
    TextureHandle loadTexture(const pack::PackReader& textures, std::string_view path, Renderer& renderer);
    Result<void> makeCloudNoise(Renderer& renderer);
    const Model* findModel(u64 hash) const;
    glm::vec4 rotationAt(const Model& model) const;
    // Advances the probe one leg; starts one every kOcclusionPeriod for the glare that shows most.
    void probeOcclusion(f32 dt, const glm::dvec3& camera, const std::array<glm::vec3, 2>& directions,
                        const std::array<f32, 2>& fades, const WorldObjects* objects,
                        const physics::CollisionWorld* collision, f32 viewDistance);

    bool m_open = false;
    std::vector<Model> m_models;
    std::vector<u64> m_lightSkyboxes; // LightSkybox.dbc id -> its model's hash (0: none)
    u64 m_starsHash = 0;
    TextureHandle m_sun, m_sunGlare, m_moon, m_blueChild, m_moonGlare, m_clouds;
    SkyFrame m_frame;
    std::array<Glare, 2> m_glares{}; // the sun's, the White Lady's
    f32 m_occlusionTimer = 0.0f;
    Probe m_probe;
    f64 m_time = 0.0; // seconds, for the skyboxes' rotation and the clouds' drift
    physics::TriangleSet m_rayTriangles{2048};
    // Development switches.
    u32 m_forcedSkybox = 0;
    f32 m_forcedStorm = -1.0f;   // < 0: not forced
    bool m_glareEnabled = true;  // the client's SkySunGlare (VD_SUN_GLARE=0)
    u32 m_dayOffset = 0;         // VD_SKY_DAY: days added to the game's (the Blue Child's cycle)
    bool m_cloudsEnabled = true; // VD_SKY_CLOUDS=0: none, to compare
};

} // namespace vd
