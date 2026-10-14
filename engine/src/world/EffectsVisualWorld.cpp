#include "world/EffectsVisualWorld.h"

#include "world/Actors.h"
#include "world/Effects.h"

#include <algorithm>

namespace vd {

EffectHandle EffectsVisualWorld::spawnAt(u64 model, const glm::dvec3& position, f32 yaw, f32 seconds) {
    return m_effects->spawnAt(model, position, yaw, 1.0f, seconds);
}

EffectHandle EffectsVisualWorld::spawnOn(u64 model, u32 actor, u32 attachment, f32 seconds) {
    return m_effects->spawnOn(model, actor, attachment, 1.0f, seconds);
}

void EffectsVisualWorld::move(EffectHandle effect, const glm::dvec3& position, f32 yaw) {
    m_effects->move(effect, position, yaw);
}

void EffectsVisualWorld::stop(EffectHandle effect) {
    m_effects->stop(effect);
}

bool EffectsVisualWorld::alive(EffectHandle effect) const {
    return m_effects->alive(effect);
}

u32 EffectsVisualWorld::liveEffects() const {
    return m_effects->spellCount();
}

bool EffectsVisualWorld::attachmentPosition(u32 actor, u32 attachment, glm::dvec3& out) const {
    const std::optional<glm::dvec3> at = m_actors->attachmentPosition(actor, attachment);
    if (at) {
        out = *at;
    }
    return at.has_value();
}

bool EffectsVisualWorld::hasAttachment(u32 actor, u32 attachment) const {
    return m_actors->hasAttachment(actor, attachment);
}

void EffectsVisualWorld::playAction(u32 actor, u16 animation, bool loop) {
    m_actors->playAction(actor, animation, loop);
}

void EffectsVisualWorld::stopAction(u32 actor) {
    m_actors->stopAction(actor);
}

u16 EffectsVisualWorld::action(u32 actor) const {
    return m_actors->action(actor);
}

void EffectsVisualWorld::preload(u64 model) {
    m_actors->preload(model);
}

void EffectsVisualWorld::setTint(u32 actor, const glm::vec3& rgb) {
    m_actors->setTint(actor, rgb);
}

f32 EffectsVisualWorld::heightOf(u32 actor, const glm::dvec3& feet) const {
    if (const std::optional<glm::dvec3> top = m_actors->headPosition(actor)) {
        return std::max(0.5f, static_cast<f32>(top->z - feet.z));
    }
    return 2.0f;
}

} // namespace vd
