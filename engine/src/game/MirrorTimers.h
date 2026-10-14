#pragma once

#include "core/Types.h"
#include "net/Opcodes.gen.h"

#include <array>
#include <span>

namespace vd::game {

// One of the client's three mirror timer bars (MirrorTimer.lua): breath under water, fatigue in deep water,
// feign death. What the server last said, and the value it moves from: the bar shows seconds(nowMs) out of
// maxSeconds, moving `scale` seconds per second (-1 runs out, 10 refills) unless paused.
struct MirrorTimer {
    bool active = false;
    u32 type = 0;    // net::kMirrorTimer*
    u32 spellId = 0; // the aura behind it (feign death), 0 none
    i32 scale = 0;
    bool paused = false;
    f32 maxSeconds = 0.0f;
    f32 baseSeconds = 0.0f; // the value at sinceMs
    u32 sinceMs = 0;

    // MirrorTimerFrame_OnUpdate: value + scale * elapsed, frozen while paused; kept within the bar.
    f32 seconds(u32 nowMs) const;
    f32 fill(u32 nowMs) const; // 0..1 of the bar
};

// The client's timer name of a type (MirrorTimerColors' keys: EXHAUSTION, BREATH, FEIGNDEATH), nullptr if
// unknown.
const char* mirrorTimerName(u32 type);
// The label's key as WoW.exe builds it ("%s_LABEL": EXHAUSTION_LABEL, BREATH_LABEL, FEIGNDEATH_LABEL).
// The 1.12 GlobalStrings.lua has no FEIGNDEATH_LABEL: callers name the timer's spell instead when the key is
// missing.
const char* mirrorTimerLabelKey(u32 type);

// SMSG_START_MIRROR_TIMER / SMSG_PAUSE_MIRROR_TIMER / SMSG_STOP_MIRROR_TIMER (vmangos
// Player::SendMirrorTimer*) as the client's MirrorTimer frames take them (MirrorTimer.lua MirrorTimer_Show):
// a timer keeps its frame while shown, a new one takes the first free frame of three, and frames keep their
// place (MirrorTimer2 stays under MirrorTimer1's spot when the first goes away). Everything hides when a
// world is entered (PLAYER_ENTERING_WORLD). Main thread.
class MirrorTimers {
public:
    static constexpr u32 kFrames = 3; // MIRRORTIMER_NUMTIMERS

    static bool handles(net::Opcode opcode);
    // False when the packet is malformed (nothing changes then).
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    void reset();

    // The frames in order (MirrorTimer1..3): inactive ones are hidden.
    const std::array<MirrorTimer, kFrames>& frames() const { return m_frames; }
    // The shown timer of `type`, nullptr if none.
    const MirrorTimer* find(u32 type) const;
    bool any() const;

private:
    MirrorTimer* shown(u32 type);

    std::array<MirrorTimer, kFrames> m_frames{};
};

} // namespace vd::game
