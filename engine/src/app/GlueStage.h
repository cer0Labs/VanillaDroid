#pragma once

#include "app/CharacterPreview.h"
#include "app/GlueScene.h"
#include "core/Types.h"
#include "pack/GlueFormat.h"
#include "pack/PackReader.h"
#include "render/Renderer.h"
#include "world/Camera.h"
#include "world/CinematicCamera.h"

#include <glm/vec3.hpp>

#include <array>
#include <filesystem>

namespace vd {

class Actors;

// Phase 18: draws what the glue screens ask for (app/GlueScene): the background model of Interface\Glues\Models as an
// actor (its bones, particles and texture animations playing its first sequence, SetSequence(0)), seen through its
// first camera (SetCamera(0): pack/GlueFormat.h in cinematics.vdpk), with GlueParent's fog and the scene's light, and
// the character standing in it. The scene stands far above every map (kOrigin), so the world's objects never light
// or hide it. Game thread; nothing allocates once the actors are made.
class GlueStage {
public:
    // cinematics.vdpk's glue scenes (missing: the glue screens draw over a plain backdrop).
    void open(const std::filesystem::path& packsDir);
    bool available() const { return m_pack.entryCount() > 0; }

    // This frame's request: the scene and the character (made or changed as needed), and `camera` on the scene's
    // camera for a picture `aspect` (width / height) wide. A request without a scene clears it.
    void update(const GlueSceneRequest& request, Actors& actors, Camera& camera, f32 aspect, f32 dt);
    void clear(Actors& actors);
    bool active() const { return m_background != GlueBackground::None; }
    GlueBackground background() const { return m_background; }
    // The scene's light and fog (the fog also clears the screen).
    SceneLighting lighting() const;

    // Where the scenes stand in the world: model space + kOrigin.
    static constexpr f64 kOriginZ = 4000.0;

private:
    struct Scene {
        const pack::GlueSceneHeader* header = nullptr;
        CinematicCamera camera;
        u64 model = 0; // actors.vdpk
        SceneLighting light; // its lights folded into the renderer's key light and ambient (lit())
    };
    static SceneLighting lit(const pack::GlueSceneHeader& header, const pack::GlueLight* lights, const glm::vec3& at);
    bool load(GlueBackground background);

    pack::PackReader m_pack;
    std::array<Scene, static_cast<usize>(GlueBackground::Count)> m_scenes{};
    std::array<bool, static_cast<usize>(GlueBackground::Count)> m_loaded{};
    GlueBackground m_background = GlueBackground::None;
    u32 m_sceneActor = ~0u;
    CharacterPreview m_character;
    f32 m_time = 0.0f; // seconds into the scene's camera loop
    f32 m_fade = 1.0f;
    CinematicView m_view;
};

} // namespace vd
