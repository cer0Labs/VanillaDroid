#include "game/MirrorTimers.h"

#include "net/MirrorTimerMessages.h"

#include <algorithm>

namespace vd::game {

using net::Opcode;

f32 MirrorTimer::seconds(u32 nowMs) const {
    f32 value = baseSeconds;
    if (!paused) {
        value += static_cast<f32>(scale) * static_cast<f32>(nowMs - sinceMs) * 0.001f;
    }
    return std::clamp(value, 0.0f, maxSeconds);
}

f32 MirrorTimer::fill(u32 nowMs) const {
    return maxSeconds > 0.0f ? seconds(nowMs) / maxSeconds : 0.0f;
}

const char* mirrorTimerName(u32 type) {
    static constexpr const char* kNames[] = {"EXHAUSTION", "BREATH", "FEIGNDEATH"};
    return type < std::size(kNames) ? kNames[type] : nullptr;
}

const char* mirrorTimerLabelKey(u32 type) {
    static constexpr const char* kKeys[] = {"EXHAUSTION_LABEL", "BREATH_LABEL", "FEIGNDEATH_LABEL"};
    return type < std::size(kKeys) ? kKeys[type] : nullptr;
}

bool MirrorTimers::handles(Opcode opcode) {
    return opcode == Opcode::SMSG_START_MIRROR_TIMER || opcode == Opcode::SMSG_PAUSE_MIRROR_TIMER ||
           opcode == Opcode::SMSG_STOP_MIRROR_TIMER;
}

MirrorTimer* MirrorTimers::shown(u32 type) {
    for (MirrorTimer& t : m_frames) {
        if (t.active && t.type == type) {
            return &t;
        }
    }
    return nullptr;
}

const MirrorTimer* MirrorTimers::find(u32 type) const {
    return const_cast<MirrorTimers*>(this)->shown(type);
}

bool MirrorTimers::any() const {
    return std::any_of(m_frames.begin(), m_frames.end(), [](const MirrorTimer& t) { return t.active; });
}

void MirrorTimers::reset() {
    m_frames = {};
}

bool MirrorTimers::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    switch (opcode) {
    case Opcode::SMSG_START_MIRROR_TIMER: {
        net::MirrorTimerStart m;
        if (!net::parseMirrorTimerStart(payload, m)) {
            return false;
        }
        if (!mirrorTimerName(m.type)) {
            return true; // no bar of that kind in the client (MirrorTimerColors has no entry)
        }
        // The frame already showing this timer, else the first free one; none free: not shown.
        MirrorTimer* t = shown(m.type);
        for (u32 i = 0; i < kFrames && !t; ++i) {
            t = m_frames[i].active ? nullptr : &m_frames[i];
        }
        if (!t) {
            return true;
        }
        t->active = true;
        t->type = m.type;
        t->spellId = m.spellId;
        t->scale = m.scale;
        t->paused = m.paused;
        t->maxSeconds = static_cast<f32>(m.durationMs) * 0.001f;
        t->baseSeconds = static_cast<f32>(m.remainingMs) * 0.001f;
        t->sinceMs = nowMs;
        return true;
    }
    case Opcode::SMSG_PAUSE_MIRROR_TIMER: {
        net::MirrorTimerPause m;
        if (!net::parseMirrorTimerPause(payload, m)) {
            return false;
        }
        if (MirrorTimer* t = shown(m.type); t && t->paused != m.paused) {
            t->baseSeconds = t->seconds(nowMs); // frozen where it is, or moving on from there
            t->sinceMs = nowMs;
            t->paused = m.paused;
        }
        return true;
    }
    case Opcode::SMSG_STOP_MIRROR_TIMER: {
        u32 type = 0;
        if (!net::parseMirrorTimerStop(payload, type)) {
            return false;
        }
        if (MirrorTimer* t = shown(type)) {
            *t = MirrorTimer{};
        }
        return true;
    }
    default: return true;
    }
}

} // namespace vd::game
