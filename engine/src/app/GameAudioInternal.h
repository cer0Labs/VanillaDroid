#pragma once

#include "core/Types.h"

// What app/GameAudio.cpp and app/GameAudioWorld.cpp share (the same class split in two files): voice priorities,
// the unit memory's slots, item enums.
namespace vd::gameaudio {

// Voice stealing order (AudioSystem: higher survives).
constexpr u8 kPriorityMusic = 255;
constexpr u8 kPriorityAmbience = 245;
constexpr u8 kPriorityUi = 200;
constexpr u8 kPriorityPlayer = 150; // the player's own sounds (and what it does to others)
constexpr u8 kPriorityVoice = 120;  // NPCs talking to the player, emotes
constexpr u8 kPriorityWorld = 100;

// UnitMemory::lastMs slots.
constexpr u32 kWound = 0, kGrunt = 1, kAggro = 2, kWatch = 3;
constexpr u32 kWatchMs = 30000; // a unit hit within this long is watched for its death

constexpr u8 kItemClassWeapon = 2;
constexpr u8 kItemClassArmor = 4;
constexpr u8 kItemMaterialWood = 2; // ITEM_MATERIAL_WOOD (UNIT_VIRTUAL_ITEM_INFO byte 2)

// Time comparisons on the wrapping millisecond clock.
inline bool reached(u32 nowMs, u32 atMs) {
    return static_cast<i32>(nowMs - atMs) >= 0;
}

} // namespace vd::gameaudio
