#pragma once

#include "core/Types.h"

#include <glm/vec3.hpp>

#include <optional>

namespace vd {

class GameSession;
struct Camera;

// Phase 18 (motion): the camera through a far sight object. Far Sight, Eagle Eye and Ryson's Eye in the Sky
// (Spell.dbc effect 72, SPELL_EFFECT_ADD_FARSIGHT, range 50000 yd) make a dynamic object where the player
// aims and set PLAYER_FARSIGHT to it (vMaNGOS EffectAddFarsight, Camera::SetView); WoW.exe makes it the
// camera's target (CGPlayer_C 0x5EE290 -> CGWorldFrame 0x4841A0) and says so with CMSG_FAR_SIGHT
// (game::MoverControl), the player still the mover: moving or acting ends the channel, the field goes back to
// 0 and the camera to the character. Online, PlayerController orbits that object like the character (its zoom
// and angles); the object can be far beyond the collision kept around the player, so this keeps the camera
// above the ground there (the terrain streams around the camera). Offline,
// VD_MOTION_FARSIGHT=<dx>,<dy>[,<seconds>] views that far from the character (yards east and north) for that
// long (20 s by default) a few seconds after it stands, then comes back. Game thread.
class FarSightView {
public:
    using HeightQuery = std::optional<f32> (*)(const void* context, f64 x, f64 y);

    void init(); // reads VD_MOTION_FARSIGHT
    // After PlayerController::updateCamera. `pivot`: the point the camera turned around (the character's
    // head).
    void update(const GameSession& session, bool online, const glm::dvec3& pivot, Camera& camera, u32 nowMs,
                HeightQuery height, const void* context);
    // The camera looks through a far sight object this frame.
    bool viewing() const { return m_viewing; }

private:
    bool m_viewing = false;
    // Development (VD_MOTION_FARSIGHT).
    bool m_demo = false;
    glm::dvec3 m_offset{0.0};
    u32 m_demoMs = 20000;
    u32 m_delayMs = 4000; // after the character stands
    u32 m_startMs = 0;    // 0: not started
    glm::dvec3 m_point{0.0};
    bool m_demoDone = false;
};

} // namespace vd
