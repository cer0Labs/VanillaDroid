#pragma once

#include "core/Types.h"

namespace vd {

// Platform-neutral input events, translated by the platform layers (Win32, GameActivity).

enum class Key : u8 {
    Forward, // W
    Back,    // S
    Left,    // A
    Right,   // D
    Down,    // Q
    Up,      // E
    Fast,    // Shift
    Jump,    // Space
    Walk,    // / (numpad or main keyboard): walk / run toggle
    Mode,    // F: free camera <-> character
    Count,
};

enum class TouchPhase : u8 { Down, Move, Up, Cancel };

// Keys that edit the text field being typed into (ui::TextInput), while the game wants the keyboard. Desktop:
// Backspace / Enter / Escape; Android: the soft keyboard's action key (Enter) and Back (Escape).
enum class TextKey : u8 { Backspace, Enter, Escape };

// Buttons of GamepadState::buttons, Xbox layout (A bottom, B right, X left, Y top).
namespace gamepad {
constexpr u32 kA = 1u << 0;
constexpr u32 kB = 1u << 1;
constexpr u32 kX = 1u << 2;
constexpr u32 kY = 1u << 3;
constexpr u32 kLB = 1u << 4; // shoulder buttons
constexpr u32 kRB = 1u << 5;
constexpr u32 kBack = 1u << 6; // View / Select
constexpr u32 kStart = 1u << 7; // Menu / Start
constexpr u32 kDpadUp = 1u << 8;
constexpr u32 kDpadDown = 1u << 9;
constexpr u32 kDpadLeft = 1u << 10;
constexpr u32 kDpadRight = 1u << 11;
constexpr u32 kL3 = 1u << 12; // stick clicks
constexpr u32 kR3 = 1u << 13;
} // namespace gamepad

// The first connected game controller, as the platform polled it this frame (desktop XInput, Android Paddleboat).
// Raw values: no dead zone applied (the consumers choose theirs).
struct GamepadState {
    f32 leftX = 0.0f, leftY = 0.0f;   // -1..1, x right, y up
    f32 rightX = 0.0f, rightY = 0.0f; // -1..1, x right, y up
    f32 leftTrigger = 0.0f, rightTrigger = 0.0f; // 0..1
    u32 buttons = 0; // gamepad::k* bits held
    bool connected = false;
};

} // namespace vd
