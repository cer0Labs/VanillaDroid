#include "game/GameObjectAnim.h"

#include "game/UnitInfo.h"

namespace vd::game {

f32 gameObjectUseYards(const Entity& object, f32 otherwise) {
    return gameObjectType(object) == kGameObjectFishingNode ? 100.0f : otherwise;
}

u32 gameObjectSoundSlot(u16 animation) {
    switch (animation) {
    case goanim::kStand: return 0;
    case goanim::kOpen: return 1;
    case goanim::kClose: return 3;
    case goanim::kDestroy: return 4;
    case goanim::kOpened: return 5;
    case goanim::kCustom0:
    case goanim::kCustom0 + 1:
    case goanim::kCustom0 + 2:
    case goanim::kCustom0 + 3: return 6u + (animation - goanim::kCustom0);
    default: return goanim::kNone;
    }
}

void GameObjectAnimator::appear(u32 state) {
    m_state = state;
    m_oneShot = goanim::kNone;
    m_started = false;
}

void GameObjectAnimator::changeState(u32 state) {
    if (state == m_state) {
        return;
    }
    const u32 before = m_state;
    m_state = state;
    m_started = false;
    switch (state) {
    case kGoStateActive: m_oneShot = goanim::kOpen; break;
    case kGoStateActiveAlternative: m_oneShot = goanim::kDestroy; break;
    default: // back to ready: a destroyed object is rebuilt, an open one closes
        m_oneShot = before == kGoStateActiveAlternative ? goanim::kRebuild : goanim::kClose;
        break;
    }
}

void GameObjectAnimator::custom(u32 index) {
    if (index > 3) {
        return;
    }
    m_oneShot = static_cast<u16>(goanim::kCustom0 + index);
    m_started = false;
}

void GameObjectAnimator::spawn() {
    m_oneShot = goanim::kSpawn;
    m_started = false;
}

void GameObjectAnimator::despawn() {
    m_oneShot = goanim::kDespawn;
    m_started = false;
}

void GameObjectAnimator::resetState() {
    m_oneShot = goanim::kNone;
    m_started = false;
}

GameObjectAnimRequest GameObjectAnimator::next(Has has, const void* context, bool oneShotDone) {
    if (m_oneShot != goanim::kNone) {
        if (!m_started) {
            u16 pick = has(context, m_oneShot) ? m_oneShot : goanim::kNone;
            if (pick == goanim::kNone && m_oneShot == goanim::kRebuild && has(context, goanim::kClose)) {
                pick = goanim::kClose; // no rebuild of its own: it closes
            }
            m_oneShot = pick;
            if (pick != goanim::kNone) {
                m_started = true;
                return {pick, false};
            }
        } else if (!oneShotDone || m_oneShot == goanim::kDespawn) {
            return {m_oneShot, false}; // playing (a despawn holds its last frame until the object goes)
        } else {
            m_oneShot = goanim::kNone;
            m_started = false;
        }
    }
    // The state's loop, or Stand when the model has none for it.
    switch (m_state) {
    case kGoStateActive: return {has(context, goanim::kOpened) ? goanim::kOpened : goanim::kStand, true};
    case kGoStateActiveAlternative:
        if (has(context, goanim::kDestroyed)) {
            return {goanim::kDestroyed, true};
        }
        return {has(context, goanim::kOpened) ? goanim::kOpened : goanim::kStand, true};
    default: return {has(context, goanim::kClosed) ? goanim::kClosed : goanim::kStand, true};
    }
}

} // namespace vd::game
