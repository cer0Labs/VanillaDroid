#pragma once

#include "core/Types.h"
#include "platform/Input.h"
#include "world/Camera.h"

#include <array>

namespace vd {

// Free-flying camera for exploring the world before there is a player (phase 2 tool).
//   Desktop: WASD move, Q/E down/up, Shift faster, right mouse drag to look, wheel = speed.
//   Touch (provisional): a finger on the left half is a virtual stick (move), on the right half it looks.
class FlyCamera {
public:
    void onKey(Key key, bool down);
    void onLook(f32 dxPixels, f32 dyPixels);
    // Touches not claimed by the debug UI. viewportWidth decides the left/right half.
    void onTouch(TouchPhase phase, i32 id, f32 x, f32 y, f32 viewportWidth);
    void releaseAll();

    // Applies this frame's input. contentScale: pixels per dp (touch sensitivity in dp).
    void update(Camera& camera, f32 dt, f32 contentScale);

    f32 speed = 60.0f; // yards per second (x4 with Fast)

private:
    enum class Role : u8 { None, Move, Look };
    struct Touch {
        i32 id = -1;
        Role role = Role::None;
        f32 startX = 0.0f;
        f32 startY = 0.0f;
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 lookedX = 0.0f; // position already applied to the camera (look touches)
        f32 lookedY = 0.0f;
    };

    std::array<bool, static_cast<usize>(Key::Count)> m_keys{};
    f32 m_lookX = 0.0f; // pending mouse look, pixels
    f32 m_lookY = 0.0f;
    std::array<Touch, 4> m_touches{};
};

} // namespace vd
