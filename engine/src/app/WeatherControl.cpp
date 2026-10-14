#include "app/WeatherControl.h"

#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "pack/CollisionFormat.h"
#include "pack/PackReader.h"
#include "physics/CollisionWorld.h"
#include "render/Renderer.h"
#include "world/Camera.h"
#include "world/Terrain.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>

namespace vd {
namespace {

using game::WeatherType;

// WeatherTexture order.
constexpr const char* kTexturePaths[kWeatherTextures] = {
    "Textures\\Weather\\RainDrop01.blp", "Textures\\Weather\\RainDropSplash01.blp",
    "Textures\\Weather\\SnowFlake01.blp", "Textures\\Weather\\SnowMist01.blp",
    "Textures\\Weather\\WeatherMistGrainy01.blp"};

constexpr const char* kTypeNames[game::kWeatherTypes] = {"fine", "rain", "snow", "sand"};

} // namespace

void WeatherControl::init(const pack::PackReader& textures, Renderer& renderer, std::string_view forced) {
    std::array<std::span<const u8>, kWeatherTextures> blobs{};
    for (u32 i = 0; i < kWeatherTextures; ++i) {
        if (const std::optional<pack::PackAsset> asset = textures.find(std::string_view(kTexturePaths[i]));
            asset && asset->type == pack::AssetType::Texture) {
            blobs[i] = {asset->data, asset->size};
        } else {
            VD_LOG_WARN("Weather: %s missing from textures.vdpk", kTexturePaths[i]);
        }
    }
    if (Result<void> set = renderer.setWeatherTextures(blobs); !set) {
        VD_LOG_WARN("Weather textures: %s", set.error().message.c_str());
    }
    const std::string forcedSpec(forced);
    const char* spec = forcedSpec.empty() ? VD_DEV_ENV("VD_WEATHER") : forcedSpec.c_str();
    if (spec) {
        if (game::parseWeatherSpec(spec, m_forcedState)) {
            m_forced = true;
            m_forcedPending = true;
            m_panelType = static_cast<i32>(m_forcedState.type);
            m_panelGrade = m_forcedState.grade;
            VD_LOG_INFO("Weather forced: %s %.2f%s", kTypeNames[static_cast<u32>(m_forcedState.type)],
                        static_cast<f64>(m_forcedState.grade), m_forcedState.instant ? " at once" : "");
        } else {
            VD_LOG_WARN("VD_WEATHER=%s: expected <rain|snow|sand|fine>[:grade][:instant]", spec);
        }
    }
}

void WeatherControl::update(const Inputs& in) {
    VD_PROFILE_ZONE("WeatherControl::update");
    m_collision = in.collision;
    m_terrain = in.terrain;
    const u64 map = assetPathHash(in.map);
    if (map != m_mapHash) {
        // Another map: what was known of the ground is not there any more.
        m_mapHash = map;
        m_effect.reset();
        m_serverChanges = ~0u;
        m_forcedPending = m_forced;
    }
    if (m_forced) {
        if (m_forcedPending) {
            m_forcedPending = false;
            m_effect.set(m_forcedState);
        }
    } else if (in.server) {
        if (in.server->changes() != m_serverChanges || !m_online || m_released) {
            m_serverChanges = in.server->changes();
            m_effect.set(in.server->state());
        }
        m_online = true;
    } else if (m_online || m_released) {
        // Out of the world (or the forced weather let go offline): none, at once out of the world.
        m_effect.set(game::WeatherState{WeatherType::Fine, 0.0f, 0, m_online});
        m_online = false;
    }
    m_released = false;

    WeatherFrame frame;
    frame.dt = in.dt;
    if (in.camera) {
        frame.camera = in.camera->position;
        frame.cameraForward = in.camera->forward();
        frame.cameraRight = in.camera->right();
        frame.pixelAngle = in.camera->fovY / std::max(in.screenHeight, 1.0f);
        frame.fovY = in.camera->fovY;
    }
    frame.indoors = in.indoors;
    frame.underwater = in.underwater;
    frame.density = in.density;
    frame.effectScale = in.effectScale;
    if (in.light) {
        frame.ambient = in.light->ambientColor;
        frame.direct = in.light->directColor;
        frame.fog = in.light->fogColor;
    }
    m_effect.update(frame, &WeatherControl::sampleGround, this);
    const WeatherEffect::Stats& stats = m_effect.stats();
    m_worstMs = std::max(m_worstMs * 0.995f, stats.updateMs);
    if (m_effect.fade().type() == WeatherType::Fine) {
        m_logSeconds = 0.0f;
        return;
    }
    // While there is weather: its cost on the main thread (development log, like the frame stats).
    ++m_logFrames;
    m_logMs += stats.updateMs;
    m_logMaxMs = std::max(m_logMaxMs, stats.updateMs);
    m_logSamples += stats.groundSampled;
    m_logGroundMs += stats.groundMs;
    m_logSlow += stats.updateMs > 0.25f ? 1u : 0u;
    m_logSeconds += in.dt;
    if (m_logSeconds >= kLogSeconds) {
        const WeatherFade& fade = m_effect.fade();
        VD_LOG_INFO("Weather: %s %.2f (seen %.2f), %u particles, %u mists | update %.3f ms avg, %.3f max, "
                    "%u frames over 0.25 ms | ground %.1f samples a frame, %.2f us each, %u unknown",
                    kTypeNames[static_cast<u32>(fade.type())], static_cast<f64>(fade.intensity()),
                    static_cast<f64>(m_effect.visibility()), stats.particles, stats.mists,
                    m_logMs / m_logFrames, static_cast<f64>(m_logMaxMs), m_logSlow,
                    static_cast<f64>(m_logSamples) / m_logFrames,
                    m_logSamples > 0 ? m_logGroundMs * 1000.0 / static_cast<f64>(m_logSamples) : 0.0,
                    m_effect.ground().unknown());
        m_logSeconds = 0.0f;
        m_logFrames = 0;
        m_logMs = 0.0;
        m_logMaxMs = 0.0f;
        m_logSamples = 0;
        m_logGroundMs = 0.0;
        m_logSlow = 0;
    }
}

f32 WeatherControl::sampleGround(const void* self, f64 x, f64 y, f64 top, f64 bottom) {
    const WeatherControl& c = *static_cast<const WeatherControl*>(self);
    f32 ground = std::numeric_limits<f32>::quiet_NaN();
    if (c.m_collision && c.m_collision->isOpen() && c.m_collision->ready(x, y)) {
        // The highest of the terrain, the buildings and the water; through what the camera goes through and
        // through the doodads (trees, tents, stalls: the client's rain falls through them).
        c.m_triangles.reset(glm::dvec3(x, y, top));
        c.m_collision->gather(glm::dvec3(x - 0.01, y - 0.01, bottom), glm::dvec3(x + 0.01, y + 0.01, top),
                              c.m_triangles);
        physics::Hit hit;
        if (physics::raycast(c.m_triangles, glm::vec3(0.0f),
                             glm::vec3(0.0f, 0.0f, static_cast<f32>(bottom - top)), hit,
                             pack::kCollisionDoodad | pack::kCollisionNoCamera)) {
            ground = static_cast<f32>(top + static_cast<f64>(hit.t) * (bottom - top));
        }
        if (const std::optional<physics::LiquidSample> liquid = c.m_collision->liquidAt(x, y, top, bottom);
            liquid && (std::isnan(ground) || liquid->level > ground)) {
            ground = liquid->level;
        }
    } else if (c.m_terrain) {
        if (const std::optional<f32> height = c.m_terrain->heightAt(x, y);
            height && static_cast<f64>(*height) <= top && static_cast<f64>(*height) >= bottom) {
            ground = *height;
        }
    }
    return ground;
}

void WeatherControl::drawPanel() {
    const f32 font = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(font * 40.0f, font * 13.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Weather")) {
        ImGui::End();
        return;
    }
    const WeatherFade& fade = m_effect.fade();
    const WeatherEffect::Stats& stats = m_effect.stats();
    ImGui::Text("shown: %s %.2f -> %s %.2f%s", kTypeNames[static_cast<u32>(fade.type())],
                static_cast<f64>(fade.intensity()), kTypeNames[static_cast<u32>(fade.targetType())],
                static_cast<f64>(fade.targetGrade()), m_forced ? " (forced)" : "");
    ImGui::Text("sound %u at %.2f | storm light %.2f | seen %.2f", fade.sound(),
                static_cast<f64>(m_effect.soundVolume()), static_cast<f64>(m_effect.storm()),
                static_cast<f64>(m_effect.visibility()));
    ImGui::Text("%u particles, %u mists | ground: %u sampled, %u unknown | %.3f ms (worst %.3f)",
                stats.particles, stats.mists, stats.groundSampled, m_effect.ground().unknown(),
                static_cast<f64>(stats.updateMs), static_cast<f64>(m_worstMs));
    ImGui::Separator();
    ImGui::Combo("type", &m_panelType, kTypeNames, static_cast<int>(game::kWeatherTypes));
    ImGui::SliderFloat("grade", &m_panelGrade, 0.0f, 1.0f);
    ImGui::Checkbox("at once", &m_panelInstant);
    if (ImGui::Button("Force")) {
        m_forced = true;
        m_forcedPending = true;
        m_forcedState.type = static_cast<WeatherType>(std::clamp(m_panelType, 0, 3));
        m_forcedState.grade = m_forcedState.type == WeatherType::Fine ? 0.0f : m_panelGrade;
        m_forcedState.sound = game::weatherSound(m_forcedState.type, m_forcedState.grade);
        m_forcedState.instant = m_panelInstant;
    }
    ImGui::SameLine();
    if (ImGui::Button("Server's") && m_forced) {
        m_forced = false;
        m_released = true; // the server's again, from where it is (offline: none)
    }
    ImGui::End();
}

} // namespace vd
