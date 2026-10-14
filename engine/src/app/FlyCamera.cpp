#include "app/FlyCamera.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

constexpr f32 kMouseRadiansPerPixel = 0.004f;
constexpr f32 kTouchRadiansPerDp = 0.008f;
constexpr f32 kStickRadiusDp = 70.0f; // finger travel for full speed
constexpr f32 kMaxPitch = 1.55f;      // just under 90 degrees

} // namespace

void FlyCamera::onKey(Key key, bool down) {
    if (key < Key::Count) {
        m_keys[static_cast<usize>(key)] = down;
    }
}

void FlyCamera::onLook(f32 dxPixels, f32 dyPixels) {
    m_lookX += dxPixels;
    m_lookY += dyPixels;
}

void FlyCamera::onTouch(TouchPhase phase, i32 id, f32 x, f32 y, f32 viewportWidth) {
    Touch* touch = nullptr;
    for (Touch& t : m_touches) {
        if (t.id == id) {
            touch = &t;
        }
    }
    if (phase == TouchPhase::Down) {
        if (!touch) {
            for (Touch& t : m_touches) {
                if (t.id < 0) {
                    touch = &t;
                    break;
                }
            }
        }
        if (touch) {
            *touch = Touch{id, x < viewportWidth * 0.5f ? Role::Move : Role::Look, x, y, x, y, x, y};
        }
        return;
    }
    if (!touch) {
        return;
    }
    if (phase == TouchPhase::Move) {
        touch->x = x;
        touch->y = y;
    } else {
        *touch = Touch{};
    }
}

void FlyCamera::releaseAll() {
    m_keys.fill(false);
    m_touches.fill(Touch{});
    m_lookX = 0.0f;
    m_lookY = 0.0f;
}

void FlyCamera::update(Camera& camera, f32 dt, f32 contentScale) {
    auto key = [&](Key k) { return m_keys[static_cast<usize>(k)] ? 1.0f : 0.0f; };
    f32 forward = key(Key::Forward) - key(Key::Back);
    f32 strafe = key(Key::Right) - key(Key::Left);
    const f32 lift = key(Key::Up) - key(Key::Down);

    f32 yawDelta = -m_lookX * kMouseRadiansPerPixel;
    f32 pitchDelta = -m_lookY * kMouseRadiansPerPixel;
    m_lookX = 0.0f;
    m_lookY = 0.0f;

    const f32 dp = std::max(0.5f, contentScale);
    for (Touch& touch : m_touches) {
        if (touch.role == Role::Move) {
            // Virtual stick: offset from where the finger went down, clamped to the stick radius.
            glm::vec2 offset((touch.x - touch.startX) / dp, (touch.y - touch.startY) / dp);
            const f32 length = glm::length(offset);
            if (length > kStickRadiusDp) {
                offset *= kStickRadiusDp / length;
            }
            forward += -offset.y / kStickRadiusDp;
            strafe += offset.x / kStickRadiusDp;
        } else if (touch.role == Role::Look) {
            yawDelta -= (touch.x - touch.lookedX) / dp * kTouchRadiansPerDp;
            pitchDelta -= (touch.y - touch.lookedY) / dp * kTouchRadiansPerDp;
            touch.lookedX = touch.x;
            touch.lookedY = touch.y;
        }
    }

    camera.yaw = std::remainder(camera.yaw + yawDelta, 6.2831853f);
    camera.pitch = std::clamp(camera.pitch + pitchDelta, -kMaxPitch, kMaxPitch);

    const f32 velocity = speed * (key(Key::Fast) > 0.0f ? 4.0f : 1.0f) * dt;
    const glm::vec3 move = camera.forward() * std::clamp(forward, -1.0f, 1.0f) +
                           camera.right() * std::clamp(strafe, -1.0f, 1.0f) + glm::vec3(0.0f, 0.0f, lift);
    camera.position += glm::dvec3(move * velocity);
}

} // namespace vd
