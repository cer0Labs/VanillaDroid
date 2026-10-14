#include "app/FarSightView.h"

#include "app/GameSession.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "world/Camera.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vd {
namespace {

constexpr f64 kAboveGround = 0.3;  // yards the camera keeps over the ground at a far sight object
constexpr f64 kObjectHeight = 1.0; // the view's pivot over a far sight object (PlayerController's)

} // namespace

void FarSightView::init() {
    const char* spec = VD_DEV_ENV("VD_MOTION_FARSIGHT");
    if (!spec || !*spec) {
        return;
    }
    f64 dx = 0.0, dy = 0.0;
    u32 seconds = 20, after = 4;
    if (std::sscanf(spec, "%lf,%lf,%u,%u", &dx, &dy, &seconds, &after) < 2) {
        VD_LOG_WARN("VD_MOTION_FARSIGHT=%s: <dx>,<dy>[,<seconds>[,<after seconds>]]", spec);
        return;
    }
    m_demo = true;
    m_offset = glm::dvec3(dx, dy, 0.0);
    m_demoMs = std::max(1u, seconds) * 1000;
    m_delayMs = after * 1000;
}

void FarSightView::update(const GameSession& session, bool online, const glm::dvec3& pivot, Camera& camera,
                          u32 nowMs, HeightQuery height, const void* context) {
    VD_PROFILE_ZONE("FarSightView::update");
    m_viewing = false;
    if (online) {
        // The camera's target is the far sight object when it is not the unit the client moves (a possessed
        // unit, Eyes of the Beast: moved and collided with by the controller).
        const game::MoverControl& control = session.play().control();
        m_viewing = control.viewTarget() != 0 && control.viewTarget() != session.playerGuid() &&
                    control.viewTarget() != control.activeMover();
    } else if (m_demo && !m_demoDone) {
        if (m_startMs == 0) {
            m_startMs = nowMs + m_delayMs;
        } else if (static_cast<i32>(nowMs - m_startMs) >= 0) {
            if (nowMs - m_startMs >= m_demoMs) {
                m_demoDone = true;
                VD_LOG_INFO("VD_MOTION_FARSIGHT: back to the character");
            } else {
                if (m_point == glm::dvec3(0.0)) {
                    m_point = pivot + m_offset;
                    VD_LOG_INFO("VD_MOTION_FARSIGHT: viewing (%.1f, %.1f) for %u s", m_point.x, m_point.y,
                                m_demoMs / 1000);
                }
                if (const std::optional<f32> ground =
                        height ? height(context, m_point.x, m_point.y) : std::nullopt) {
                    m_point.z = *ground + kObjectHeight;
                }
                // As PlayerController's camera around a far sight object: the same angles and zoom, around
                // it.
                camera.position = m_point + (camera.position - pivot);
                m_viewing = true;
            }
        }
    }
    if (m_viewing && height) {
        if (const std::optional<f32> ground = height(context, camera.position.x, camera.position.y)) {
            camera.position.z = std::max(camera.position.z, static_cast<f64>(*ground) + kAboveGround);
        }
    }
}

} // namespace vd
