#pragma once

#include "core/Types.h"

#include <span>

// The mirror timer packets (breath, fatigue, feign death) as vmangos-core 4641790 writes them for build 5875
// (Server/Packets/Misc: StartMirrorTimer, PauseMirrorTimer, StopMirrorTimer; Objects/MirrorTimer.h; sent by
// Player::SendMirrorTimer*). Same rules as net/GameMessages: parsers return false on a malformed packet and
// never read past its end.
namespace vd::net {

// MirrorTimer::Type: the client's three bars. The server's own ENVIRONMENTAL timer (3, lava and slime) never
// reaches the client (Player::SendMirrorTimers stops at NUM_CLIENT_TIMERS).
constexpr u32 kMirrorTimerFatigue = 0;
constexpr u32 kMirrorTimerBreath = 1;
constexpr u32 kMirrorTimerFeignDeath = 2;
constexpr u32 kMirrorTimerTypes = 3;

// SMSG_START_MIRROR_TIMER (21 bytes): also every change of a running timer (the server resends it whole
// instead of SMSG_PAUSE_MIRROR_TIMER, whose default UI handler is broken).
struct MirrorTimerStart {
    u32 type = 0;        // kMirrorTimer*
    u32 remainingMs = 0; // time left
    u32 durationMs = 0;  // the whole bar
    i32 scale = 0;       // how fast the bar moves: -1 runs out in real time, 10 refills ten times as fast
    bool paused = false; // frozen (MirrorTimer::IsFrozen: on a taxi, a transport...)
    u32 spellId = 0;     // the aura behind the timer (feign death), 0 none
};
bool parseMirrorTimerStart(std::span<const u8> payload, MirrorTimerStart& out);

// SMSG_PAUSE_MIRROR_TIMER (5 bytes): a timer frozen or running again.
struct MirrorTimerPause {
    u32 type = 0;
    bool paused = false;
};
bool parseMirrorTimerPause(std::span<const u8> payload, MirrorTimerPause& out);

// SMSG_STOP_MIRROR_TIMER (4 bytes): the bar goes away.
bool parseMirrorTimerStop(std::span<const u8> payload, u32& type);

} // namespace vd::net
