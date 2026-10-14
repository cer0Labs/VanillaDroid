#include "anim/Animator.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace vd::anim {
namespace {

u32 nextRandom(u32& state) {
    // xorshift32: cheap, per actor, no shared state between jobs.
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// WoW.exe's table at 0x5FED90 (ranges of AnimationData ids), less 57, 58 and 118 (always the whole body).
constexpr std::pair<u16, u16> kUpperBodyRanges[] = {{2, 2},     {8, 36},    {46, 49},   {51, 56},   {59, 74},
                                                     {76, 78},   {80, 90},   {105, 113}, {117, 117}, {122, 125},
                                                     {128, 130}, {133, 134}, {136, 138}, {185, 186}, {195, 195}};

constexpr std::array<bool, animation::kCount> upperBodyTable() {
    std::array<bool, animation::kCount> out{};
    for (const auto& [first, last] : kUpperBodyRanges) {
        for (u32 id = first; id <= last; ++id) {
            out[id] = true;
        }
    }
    return out;
}
constexpr std::array<bool, animation::kCount> kUpperBody = upperBodyTable();

} // namespace

bool upperBodyAnimation(u16 id) {
    return id < animation::kCount && kUpperBody[id];
}

AnimFallbacks::AnimFallbacks() {
    next.fill(animation::kStand);
    // Movement falls back to walking before standing still (models without a run cycle).
    next[animation::kRun] = animation::kWalk;
    next[animation::kWalkBackwards] = animation::kWalk;
    next[animation::kSwim] = animation::kRun;
    next[animation::kSwimIdle] = animation::kStand;
    next[animation::kSwimBackwards] = animation::kSwim;
    next[animation::kDead] = animation::kStand;
    // Phase 18 (motion): WoW.exe's other movement animations (game/UnitMotion), moving still on models without them.
    next[animation::kSprint] = animation::kRun;
    next[animation::kStealthWalk] = animation::kWalk;
    next[animation::kFly] = animation::kRun;
    next[animation::kSwimLeft] = animation::kSwim;
    next[animation::kSwimRight] = animation::kSwim;
}

void AnimFallbacks::load(const db::DbcView& table) {
    constexpr u32 kWeaponFlagsField = 2, kBodyFlagsField = 3, kPriorityField = 4, kFallbackField = 6;
    if (table.fieldCount() <= kFallbackField) {
        return;
    }
    for (u32 r = 0; r < table.recordCount(); ++r) {
        const u32 id = table.getU32(r, 0);
        if (id < animation::kCount) {
            weaponFlags[id] = static_cast<u16>(table.getU32(r, kWeaponFlagsField));
            bodyFlags[id] = static_cast<u16>(table.getU32(r, kBodyFlagsField));
            priority[id] = static_cast<u16>(std::min<u32>(table.getU32(r, kPriorityField), 0xFFFF));
        }
        const u32 fallback = table.getU32(r, kFallbackField);
        if (id == animation::kStand || id >= animation::kCount || fallback >= animation::kCount || fallback == id) {
            continue;
        }
        if (fallback != animation::kStand || next[id] == animation::kStand) {
            next[id] = static_cast<u16>(fallback);
        }
    }
}

void Animator::reset() {
    *this = Animator{};
}

i32 Animator::pickVariation(const Skeleton& skeleton, i32 first, u32& random) const {
    if (first < 0) {
        return first;
    }
    // Variations are chained through nextVariation; each one is chosen in proportion to its frequency.
    i32 candidates[16];
    u32 weights[16];
    u32 count = 0;
    u32 total = 0;
    for (i32 s = first; s >= 0 && count < 16; s = skeleton.sequence(static_cast<u32>(s)).nextVariation) {
        candidates[count] = s;
        weights[count] = static_cast<u32>(std::max<i16>(skeleton.sequence(static_cast<u32>(s)).frequency, 0));
        total += weights[count];
        ++count;
        if (static_cast<u32>(skeleton.sequence(static_cast<u32>(s)).nextVariation) >= skeleton.sequenceCount()) {
            break;
        }
    }
    if (count <= 1 || total == 0) {
        return first;
    }
    u32 pick = nextRandom(random) % total;
    for (u32 i = 0; i < count; ++i) {
        if (pick < weights[i]) {
            return candidates[i];
        }
        pick -= weights[i];
    }
    return first;
}

void Animator::start(const Skeleton& skeleton, i32 sequence, bool blendSame) {
    if (m_pose.current.sequence >= 0 && (sequence != m_pose.current.sequence || blendSame)) {
        // Cross-fade from wherever the old sequence was (a finished one-shot stays on its last frame).
        m_pose.previous = m_pose.current;
        m_previousMs = m_timeMs;
        m_previousLoop = m_currentLoop;
        const u32 blend = sequence >= 0 ? skeleton.sequence(static_cast<u32>(sequence)).blendTime : 0;
        m_pose.blend = blend > 0 ? 0.0f : 1.0f;
        m_blendRate = blend > 0 ? 1000.0f / static_cast<f32>(blend) : 0.0f;
    }
    m_pose.current = {sequence, 0};
    m_timeMs = 0.0f;
    m_finished = false;
    m_currentLoop = m_loop;
}

u16 Animator::play(const Skeleton& skeleton, u16 animationId, bool loop, const AnimFallbacks& fallbacks, u32& random,
                   bool restart) {
    if (animationId == m_requested && loop == m_loop && m_first >= 0) {
        if (restart) {
            start(skeleton, pickVariation(skeleton, m_first, random), true);
        }
        return skeleton.sequence(static_cast<u32>(m_first)).animation;
    }
    m_requested = animationId;
    m_loop = loop;
    u16 id = animationId;
    i32 first = -1;
    for (u32 guard = 0; guard < 8; ++guard) {
        first = skeleton.findSequence(id);
        if (first >= 0 || id == animation::kStand) {
            break;
        }
        id = id < animation::kCount ? fallbacks.next[id] : animation::kStand;
    }
    if (first < 0 && skeleton.sequenceCount() > 0) {
        first = 0; // not even Stand: whatever the model has
    }
    if (first == m_first && first >= 0) {
        if (restart) {
            start(skeleton, pickVariation(skeleton, first, random), true);
        }
        return skeleton.sequence(static_cast<u32>(first)).animation; // e.g. Run falling back to the Walk playing
    }
    m_first = first;
    start(skeleton, pickVariation(skeleton, first, random));
    return first >= 0 ? skeleton.sequence(static_cast<u32>(first)).animation : animation::kStand;
}

void Animator::update(const Skeleton& skeleton, f32 dt, f32 speed, u32& random) {
    if (m_pose.blend < 1.0f) {
        m_pose.blend = std::min(1.0f, m_pose.blend + dt * m_blendRate);
        if (m_pose.previous.sequence >= 0) {
            const u32 d = skeleton.duration(static_cast<u32>(m_pose.previous.sequence));
            m_previousMs = m_previousLoop ? std::fmod(m_previousMs + dt * 1000.0f, static_cast<f32>(d))
                                          : std::min(m_previousMs + dt * 1000.0f, static_cast<f32>(d - 1));
            m_pose.previous.time = static_cast<u32>(m_previousMs);
        }
    }
    m_playedCount = 0;
    if (m_pose.current.sequence < 0) {
        return;
    }
    const i32 sequence = m_pose.current.sequence;
    const u32 from = static_cast<u32>(m_timeMs);
    const u32 duration = skeleton.duration(static_cast<u32>(sequence));
    m_timeMs += dt * 1000.0f * speed;
    if (m_timeMs >= static_cast<f32>(duration)) {
        if (!m_loop) {
            if (!m_finished) {
                m_played[m_playedCount++] = {sequence, from, duration + 1}; // its last instant too
            }
            m_timeMs = static_cast<f32>(duration - 1);
            m_finished = true;
        } else {
            m_played[m_playedCount++] = {sequence, from, duration};
            m_timeMs = std::fmod(m_timeMs, static_cast<f32>(duration));
            // Another variation for the next cycle (idle fidgets); it blends in like any change.
            const i32 next = pickVariation(skeleton, m_first, random);
            if (next != m_pose.current.sequence) {
                const f32 carried = m_timeMs;
                start(skeleton, next);
                m_timeMs = std::min(carried, static_cast<f32>(skeleton.duration(static_cast<u32>(next)) - 1));
            }
            m_played[m_playedCount++] = {m_pose.current.sequence, 0, static_cast<u32>(m_timeMs)};
        }
    } else if (static_cast<u32>(m_timeMs) > from) {
        m_played[m_playedCount++] = {sequence, from, static_cast<u32>(m_timeMs)};
    }
    m_pose.current.time = static_cast<u32>(m_timeMs);
}

} // namespace vd::anim
