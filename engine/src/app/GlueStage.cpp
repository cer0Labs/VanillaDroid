#include "app/GlueStage.h"

#include "anim/Animator.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "pack/GlueFormat.h"
#include "world/Actors.h"

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

// The models SetBackgroundModel and AccountLogin name, by GlueBackground.
constexpr const char* kScenePaths[] = {
    "",
    "Interface\\Glues\\Models\\UI_MainMenu\\UI_MainMenu.m2",
    "Interface\\Glues\\Models\\UI_Human\\UI_Human.m2",
    "Interface\\Glues\\Models\\UI_Orc\\UI_Orc.m2",
    "Interface\\Glues\\Models\\UI_Dwarf\\UI_Dwarf.m2",
    "Interface\\Glues\\Models\\UI_NightElf\\UI_NightElf.m2",
    "Interface\\Glues\\Models\\UI_Scourge\\UI_Scourge.m2",
    "Interface\\Glues\\Models\\UI_Tauren\\UI_Tauren.m2",
};
static_assert(std::size(kScenePaths) == static_cast<usize>(GlueBackground::Count));

// The fog of each scene: AccountLogin.xml's ModelFFX (fogNear 0, fogFar 1200, FogColor) and GlueParent.lua's
// CharModelFogInfo for the races (SetFogNear(0)).
struct GlueFog {
    glm::vec3 color;
    f32 far;
};
constexpr GlueFog kFogs[] = {
    {{0.0f, 0.0f, 0.0f}, 1000.0f},
    {{0.25f, 0.06f, 0.015f}, 1200.0f}, // MainMenu
    {{0.8f, 0.65f, 0.73f}, 222.0f},    // HUMAN
    {{0.5f, 0.5f, 0.5f}, 270.0f},      // ORC
    {{0.85f, 0.88f, 1.0f}, 500.0f},    // DWARF
    {{0.25f, 0.22f, 0.55f}, 611.0f},   // NIGHTELF
    {{0.0f, 0.22f, 0.22f}, 26.0f},     // SCOURGE
    {{1.0f, 0.61f, 0.42f}, 153.0f},    // TAUREN
};
static_assert(std::size(kFogs) == static_cast<usize>(GlueBackground::Count));

constexpr glm::dvec3 kOrigin{0.0, 0.0, GlueStage::kOriginZ};

// A camera diagonal field of view as the glue shows it. The scenes were made for 4:3 to 16:9 screens, and GlueParent
// puts black bars on wider ones: past their sides the models' skies end. A phone has no room for bars, so a wider
// screen (the Fold5's outer one, 23:9) shows the 16:9 picture's width and loses some of its top and bottom; a
// narrower one (the inner one, ~6:5) keeps the 4:3 picture's height and loses some of its sides.
f32 glueVerticalFov(f32 diagonal, f32 aspect) {
    constexpr f32 kWidest = 16.0f / 9.0f;
    const f32 vertical = verticalFov(diagonal, std::clamp(aspect, 4.0f / 3.0f, kWidest));
    if (aspect <= kWidest) {
        return vertical;
    }
    return 2.0f * std::atan(std::tan(vertical * 0.5f) * kWidest / aspect);
}

} // namespace

void GlueStage::open(const std::filesystem::path& packsDir) {
    if (Result<void> opened = m_pack.open(packsDir / "cinematics.vdpk"); !opened) {
        VD_LOG_INFO("No glue scenes (%s)", opened.error().message.c_str());
    }
}

bool GlueStage::load(GlueBackground background) {
    const usize index = static_cast<usize>(background);
    if (m_loaded[index]) {
        return m_scenes[index].header != nullptr;
    }
    m_loaded[index] = true;
    Scene& scene = m_scenes[index];
    const std::optional<pack::PackAsset> asset = m_pack.find(std::string_view(kScenePaths[index]));
    if (!asset || asset->type != pack::AssetType::GlueScene || asset->compression != pack::Compression::None ||
        asset->size < sizeof(pack::GlueSceneHeader)) {
        VD_LOG_WARN("Glue scene %s not baked (vd_baker bake --only cinematics)", kScenePaths[index]);
        return false;
    }
    const auto* header = reinterpret_cast<const pack::GlueSceneHeader*>(asset->data);
    if (header->version != pack::kGlueFormatVersion ||
        header->lightsOffset + header->lightCount * sizeof(pack::GlueLight) > asset->size ||
        header->cameraOffset + header->cameraSize > asset->size || header->cameraSize == 0 ||
        !scene.camera.bind(asset->data + header->cameraOffset, header->cameraSize).ok()) {
        VD_LOG_WARN("Glue scene %s: bad blob", kScenePaths[index]);
        return false;
    }
    scene.header = header;
    scene.model = assetPathHash(kScenePaths[index]);
    // The light where the character stands (or, without one, where the camera looks).
    glm::vec3 at(header->stand[0], header->stand[1], header->stand[2]);
    if ((header->flags & pack::kGlueHasStand) == 0) {
        const CinematicView view = scene.camera.sample(0, glm::dvec3(0.0), 0.0f);
        at = glm::vec3(view.target);
    }
    scene.light = lit(*header, reinterpret_cast<const pack::GlueLight*>(asset->data + header->lightsOffset), at);
    return true;
}

// The model's lights (up to four in the 1.12 scenes: a warm key, a cool fill, an ambient one, a fire) as the renderer's
// one key light and ambient: the strongest light reaching `at` (model space) is the key, the rest add to the ambient
// by how much of them reaches it.
SceneLighting GlueStage::lit(const pack::GlueSceneHeader& header, const pack::GlueLight* lights, const glm::vec3& at) {
    SceneLighting light;
    light.ambientColor = glm::vec3(0.0f);
    light.directColor = glm::vec3(0.0f);
    const auto luminance = [](const glm::vec3& c) { return 0.3f * c.x + 0.59f * c.y + 0.11f * c.z; };
    i32 key = -1;
    f32 keyStrength = 0.0f;
    std::array<glm::vec3, 8> reach{};
    const u32 count = std::min<u32>(header.lightCount, static_cast<u32>(reach.size()));
    for (u32 i = 0; i < count; ++i) {
        const pack::GlueLight& l = lights[i];
        if (!l.visible) {
            continue;
        }
        light.ambientColor += glm::vec3(l.ambient[0], l.ambient[1], l.ambient[2]);
        f32 share = 1.0f;
        if (l.type == 1) { // a point light: 1 up to its attenuation start, 0 from its end
            const f32 d = glm::length(glm::vec3(l.position[0], l.position[1], l.position[2]) - at);
            share = l.attenuationEnd > l.attenuationStart
                        ? std::clamp((l.attenuationEnd - d) / (l.attenuationEnd - l.attenuationStart), 0.0f, 1.0f)
                        : 1.0f;
        }
        reach[i] = glm::vec3(l.diffuse[0], l.diffuse[1], l.diffuse[2]) * share;
        if (luminance(reach[i]) > keyStrength) {
            keyStrength = luminance(reach[i]);
            key = static_cast<i32>(i);
        }
    }
    for (u32 i = 0; i < count; ++i) {
        if (static_cast<i32>(i) == key) {
            const pack::GlueLight& l = lights[i];
            light.directColor = reach[i];
            glm::vec3 toward(l.direction[0], l.direction[1], l.direction[2]);
            if (l.type == 1) {
                toward = glm::vec3(l.position[0], l.position[1], l.position[2]) - at;
            }
            light.lightDirection = glm::length(toward) > 1e-4f ? glm::normalize(toward) : glm::vec3(0.0f, 0.0f, 1.0f);
        } else {
            light.ambientColor += reach[i] * 0.4f; // a fill from elsewhere
        }
    }
    if (key < 0) { // no light: the model frames' default (ModelFFX SetLight: ambient 0.7, a 0.8 key)
        light.ambientColor = glm::vec3(0.7f);
        light.directColor = glm::vec3(0.8f, 0.8f, 0.64f);
        light.lightDirection = glm::normalize(glm::vec3(0.707f, 0.0f, 0.707f));
    }
    return light;
}

void GlueStage::update(const GlueSceneRequest& request, Actors& actors, Camera& camera, f32 aspect, f32 dt) {
    VD_PROFILE_ZONE("GlueStage::update");
    if (request.background == GlueBackground::None || !actors.isOpen() || !load(request.background)) {
        clear(actors);
        return;
    }
    const Scene& scene = m_scenes[static_cast<usize>(request.background)];
    if (request.background != m_background) {
        if (m_sceneActor == Actors::kNone) {
            m_sceneActor = actors.create();
        }
        game::ActorLook look;
        look.model = scene.model;
        actors.setLook(m_sceneActor, look);
        actors.setPlacement(m_sceneActor, kOrigin, 0.0f, 1.0f);
        actors.play(m_sceneActor, anim::animation::kStand, true, 1.0f); // SetSequence(0)
        m_background = request.background;
        m_time = 0.0f;
    }
    m_fade = std::clamp(request.fade, 0.0f, 1.0f);

    // The camera flies its loop (most stand still); the character stands at the scene's origin, facing the camera.
    const u32 duration = std::max(1u, scene.camera.durationMs());
    m_time = std::fmod(m_time + dt, static_cast<f32>(duration) * 0.001f);
    m_view = scene.camera.sample(static_cast<u32>(m_time * 1000.0f), kOrigin, 0.0f);
    const glm::dvec3 toward = m_view.target - m_view.eye;
    const f64 length = glm::length(toward);
    camera.position = m_view.eye;
    if (length > 1e-6) {
        const glm::dvec3 d = toward / length;
        camera.yaw = static_cast<f32>(std::atan2(d.y, d.x));
        camera.pitch = static_cast<f32>(std::asin(std::clamp(d.z, -1.0, 1.0)));
    }
    camera.roll = m_view.roll;
    camera.fovY = glueVerticalFov(m_view.fovDiagonal, aspect);
    // A screen wider than 16:9 loses part of the picture's height: a quarter of it at the top (the character's head,
    // the Dark Portal's arch) and the rest at the bottom, under the screens' buttons (the camera tilts up to it).
    if (const f32 full = verticalFov(m_view.fovDiagonal, 16.0f / 9.0f); full > camera.fovY) {
        camera.pitch += 0.25f * (full - camera.fovY);
    }
    camera.nearPlane = std::max(0.05f, m_view.nearClip);
    camera.farPlane = std::max(camera.nearPlane + 1.0f, std::min(m_view.farClip, 4000.0f));

    if (request.hasCharacter) {
        // The race scenes' attachment 0, facing as its bone does (towards the camera); without one, the origin facing
        // the camera.
        const pack::GlueSceneHeader& h = *scene.header;
        glm::dvec3 feet = kOrigin;
        f32 facing = std::atan2(static_cast<f32>(m_view.eye.y - kOrigin.y), static_cast<f32>(m_view.eye.x - kOrigin.x));
        if (h.flags & pack::kGlueHasStand) {
            feet += glm::dvec3(h.stand[0], h.stand[1], h.stand[2]);
            facing = h.standFacing;
        }
        m_character.update(actors, &request.character, feet, facing + request.characterFacing);
    } else {
        m_character.clear(actors);
    }
}

void GlueStage::clear(Actors& actors) {
    if (m_sceneActor != Actors::kNone) {
        actors.destroy(m_sceneActor);
        m_sceneActor = Actors::kNone;
    }
    m_character.clear(actors);
    m_background = GlueBackground::None;
}

SceneLighting GlueStage::lighting() const {
    const usize index = static_cast<usize>(m_background);
    SceneLighting light = m_scenes[index].light;
    const GlueFog& fog = kFogs[index];
    light.fogColor = fog.color * m_fade;
    light.fogStart = 0.0f;
    light.fogEnd = fog.far;
    light.ambientColor *= m_fade;
    light.directColor *= m_fade;
    for (glm::vec3& band : light.sky) {
        band = glm::vec3(0.0f); // where the scene's own sky ends: GlueParent's black
    }
    light.screenGlow = 0.0f;
    light.screenDeath = 0.0f;
    return light;
}

} // namespace vd
