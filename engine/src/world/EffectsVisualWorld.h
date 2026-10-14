#pragma once

#include "world/SpellVisuals.h"

namespace vd {

class Actors;
class Effects;

// SpellVisualWorld over world/Effects and world/Actors: everything but where the units are, which the online
// game (app/SpellEffects: the server's units) and the offline fight (app/CrowdFight: its own fighters) tell.
class EffectsVisualWorld : public SpellVisualWorld {
public:
    void bind(Effects* effects, Actors* actors) {
        m_effects = effects;
        m_actors = actors;
    }

    EffectHandle spawnAt(u64 model, const glm::dvec3& position, f32 yaw, f32 seconds) override;
    EffectHandle spawnOn(u64 model, u32 actor, u32 attachment, f32 seconds) override;
    void move(EffectHandle effect, const glm::dvec3& position, f32 yaw) override;
    void stop(EffectHandle effect) override;
    bool alive(EffectHandle effect) const override;
    u32 liveEffects() const override;
    bool attachmentPosition(u32 actor, u32 attachment, glm::dvec3& out) const override;
    bool hasAttachment(u32 actor, u32 attachment) const override;
    void playAction(u32 actor, u16 animation, bool loop) override;
    void stopAction(u32 actor) override;
    u16 action(u32 actor) const override;
    void preload(u64 model) override;
    void setTint(u32 actor, const glm::vec3& rgb) override; // phase 14

protected:
    // How tall an actor is (from its drawn bounds), for units without attachment points.
    f32 heightOf(u32 actor, const glm::dvec3& feet) const;

    Effects* m_effects = nullptr;
    Actors* m_actors = nullptr;
};

} // namespace vd
