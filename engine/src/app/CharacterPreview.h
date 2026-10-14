#pragma once

#include "core/Types.h"
#include "game/Appearance.h"

#include <glm/vec3.hpp>

namespace vd {

class Actors;

// The character being created (phase 8), standing in the world in front of the camera while the creation screen is
// up: one actor whose look follows the screen's choices (rebuilt only when they change). Game thread only.
class CharacterPreview {
public:
    // Shows `look` with its feet at `feet`, facing `yaw` (radians, 0 = +X north); nullptr removes it.
    void update(Actors& actors, const game::CharacterLook* look, const glm::dvec3& feet, f32 yaw);
    void clear(Actors& actors);
    bool active() const { return m_actor != ~0u; }
    // Where the camera frames a character of `race` whole between the screen's panels: its distance in front of
    // the camera and the camera's height over the ground (yards).
    static f32 cameraDistance(u8 race);
    static f32 cameraHeight(u8 race);

private:
    u32 m_actor = ~0u;
    game::CharacterLook m_look;
    bool m_hasLook = false;
};

} // namespace vd
