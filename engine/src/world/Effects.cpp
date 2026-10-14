#include "world/Effects.h"

#include "core/Profile.h"
#include "jobs/JobSystem.h"
#include "world/Actors.h"
#include "world/WorldObjects.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

constexpr u32 kNone = fx::ParticleSystem::kNone;

} // namespace

void Effects::init(Actors* actors, const fx::ParticleSystem::Settings& settings) {
    m_particles.init(settings);
    m_actors = actors;
    if (m_actors) {
        m_actors->setParticles(&m_particles);
    }
    m_spells.assign(kMaxSpells, Spell{});
    m_freeSpells.clear();
    for (u32 i = kMaxSpells; i-- > 0;) {
        m_freeSpells.push_back(i);
    }
    m_spellCount = 0;
    m_doodads.clear();
    m_doodads.reserve(settings.maxInstances);
    m_doodadOf.assign(65536, kNone);
    m_textures.reserve(256);
}

void Effects::shutdown() {
    if (m_actors) {
        for (u32 i = 0; i < m_spells.size(); ++i) {
            if (m_spells[i].alive) {
                endSpell(i);
            }
        }
        m_actors->setParticles(nullptr);
    }
    m_particles.shutdown();
    m_doodads.clear();
    m_doodadOf.clear();
}

void Effects::clearDoodads() {
    for (const Doodad& d : m_doodads) {
        m_particles.destroy(d.fx);
        if (d.instance < m_doodadOf.size()) {
            m_doodadOf[d.instance] = kNone;
        }
    }
    m_doodads.clear();
}

void Effects::placeDoodads(WorldObjects& objects, u32 nowMs) {
    VD_PROFILE_ZONE("Effects::placeDoodads");
    ++m_frame;
    if (!enabled) {
        return;
    }
    for (u32 instance : objects.visibleEffects()) {
        const fx::EffectSet* set = objects.instanceEffects(instance);
        if (!set) {
            continue;
        }
        if (instance >= m_doodadOf.size()) {
            m_doodadOf.resize(instance + 4096, kNone); // rare: more placed instances than ever before
        }
        u32 slot = m_doodadOf[instance];
        const u32 serial = objects.instanceSerial(instance);
        if (slot == kNone || m_doodads[slot].serial != serial) {
            // New in view, or the slot now holds another doodad: its particles start warm.
            m_textures.resize(set->emitters().size());
            objects.effectTextures(instance, m_textures);
            const u32 fx = m_particles.create(*set, m_textures, serial * 2654435761u, true);
            if (slot == kNone) {
                if (fx == kNone) {
                    continue; // tables full: no particles for it
                }
                slot = static_cast<u32>(m_doodads.size());
                m_doodads.push_back({instance, serial, fx, m_frame});
                m_doodadOf[instance] = slot;
            } else {
                m_particles.destroy(m_doodads[slot].fx);
                m_doodads[slot].fx = fx;
                m_doodads[slot].serial = serial;
            }
        }
        Doodad& d = m_doodads[slot];
        d.seenFrame = m_frame;
        if (d.fx == kNone) {
            continue;
        }
        const f32* m = objects.instanceTransform(instance);
        fx::Placement placement;
        placement.origin = glm::dvec3(m[3], m[7], m[11]);
        placement.toWorld =
            anim::BoneMatrix{{m[0], m[1], m[2], 0.0f, m[4], m[5], m[6], 0.0f, m[8], m[9], m[10], 0.0f}};
        placement.globalMs = nowMs;
        placement.bones = objects.instancePose(instance, placement.boneCount); // phase 14: emitters on moving bones
        m_particles.place(d.fx, placement);
    }
    // Out of view for a while (or unloaded): their particles go back to the pools.
    for (usize i = 0; i < m_doodads.size();) {
        Doodad& d = m_doodads[i];
        if (m_frame - d.seenFrame <= doodadGraceFrames) {
            ++i;
            continue;
        }
        m_particles.destroy(d.fx);
        m_doodadOf[d.instance] = kNone;
        if (i + 1 < m_doodads.size()) {
            d = m_doodads.back();
            m_doodadOf[d.instance] = static_cast<u32>(i);
        }
        m_doodads.pop_back();
    }
}

u32 Effects::update(f32 dt, const Camera& camera, const Frustum& frustum, f32 viewportHeight, JobSystem& jobs,
                    Renderer& renderer, std::vector<ParticleDraw>& draws) {
    VD_PROFILE_ZONE("Effects::update");
    if (!enabled) {
        draws.clear();
        m_particles.skipFrame();
        return 0;
    }
    fx::ParticleView view;
    view.camera = camera.position;
    view.forward = camera.forward();
    view.right = glm::normalize(glm::cross(view.forward, glm::vec3(0.0f, 0.0f, 1.0f)));
    if (!std::isfinite(view.right.x)) {
        view.right = camera.right(); // looking straight up or down
    }
    view.up = glm::cross(view.right, view.forward);
    view.frustum = frustum.planes;
    view.pixelsPerYard = viewportHeight / (2.0f * std::tan(camera.fovY * 0.5f));
    return m_particles.update(dt, view, &jobs, renderer.particleVertices(), draws);
}

// --- Spell visuals
// --------------------------------------------------------------------------------------------

u64 Effects::effectModelHash(std::string_view clientPath) {
    return fx::effectModelHash(clientPath);
}

EffectHandle Effects::spawnAt(u64 model, const glm::dvec3& position, f32 yaw, f32 scale, f32 seconds) {
    if (!m_actors || !m_actors->isOpen() || model == 0 || m_freeSpells.empty()) {
        return {};
    }
    const u32 index = m_freeSpells.back();
    m_freeSpells.pop_back();
    Spell& spell = m_spells[index];
    spell.actor = m_actors->create(true);
    ++spell.generation;
    spell.remaining = seconds;
    spell.timed = seconds > 0.0f;
    spell.once = seconds < 0.0f;
    spell.age = 0.0f;
    spell.stopping = false;
    spell.fading = 0.0f;
    spell.alive = true;
    spell.parent = ~0u;
    game::ActorLook look;
    look.model = model;
    look.scale = scale;
    m_actors->setLook(spell.actor, look);
    m_actors->setPlacement(spell.actor, position, yaw, scale);
    m_actors->play(spell.actor, anim::animation::kStand, !spell.once, 1.0f);
    ++m_spellCount;
    return {index, spell.generation};
}

EffectHandle Effects::spawnOn(u64 model, u32 actor, u32 attachmentId, f32 scale, f32 seconds) {
    const EffectHandle handle = spawnAt(model, glm::dvec3(0.0), 0.0f, scale, seconds);
    if (handle.valid()) {
        m_actors->attach(m_spells[handle.index].actor, actor, attachmentId);
        m_spells[handle.index].parent = actor;
    }
    return handle;
}

EffectHandle Effects::spawnOnItem(u64 model, u32 actor, u8 equipSlot, u32 itemAttachmentId, f32 scale, f32 seconds) {
    const EffectHandle handle = spawnAt(model, glm::dvec3(0.0), 0.0f, scale, seconds);
    if (handle.valid()) {
        m_actors->attachToItem(m_spells[handle.index].actor, actor, equipSlot, itemAttachmentId);
        m_spells[handle.index].parent = actor;
    }
    return handle;
}

void Effects::move(EffectHandle effect, const glm::dvec3& position, f32 yaw) {
    if (alive(effect)) {
        const Spell& spell = m_spells[effect.index];
        const game::ActorLook* look = m_actors->look(spell.actor);
        m_actors->setPlacement(spell.actor, position, yaw, look ? look->scale : 1.0f);
    }
}

void Effects::stop(EffectHandle effect) {
    if (alive(effect) && !m_spells[effect.index].stopping) {
        Spell& spell = m_spells[effect.index];
        spell.stopping = true;
        spell.fading = 0.0f;
        m_actors->stopEffects(spell.actor);
    }
}

bool Effects::alive(EffectHandle effect) const {
    return effect.index < m_spells.size() && m_spells[effect.index].alive &&
           m_spells[effect.index].generation == effect.generation;
}

void Effects::endSpell(u32 index) {
    Spell& spell = m_spells[index];
    m_actors->destroy(spell.actor);
    spell.alive = false;
    spell.actor = ~0u;
    m_freeSpells.push_back(index);
    --m_spellCount;
}

void Effects::updateSpells(f32 dt) {
    if (!m_actors || m_spellCount == 0) {
        return;
    }
    VD_PROFILE_ZONE("Effects::updateSpells");
    for (u32 i = 0; i < m_spells.size(); ++i) {
        Spell& spell = m_spells[i];
        if (!spell.alive) {
            continue;
        }
        if (!spell.stopping && spell.parent != ~0u &&
            (!m_actors->look(spell.parent) || m_actors->parent(spell.actor) != spell.parent)) {
            spell.stopping = true; // what it was on went away
            m_actors->stopEffects(spell.actor);
        }
        if (!spell.stopping && spell.once) {
            // Its animation plays once: how long that is, once the model is there.
            spell.age += dt;
            const f32 length = m_actors->animationSeconds(spell.actor);
            if (length > 0.0f) {
                spell.once = false;
                spell.timed = true;
                spell.remaining = std::clamp(length, kOnceMinSeconds, kOnceMaxSeconds);
            } else if (spell.age > kOnceMaxSeconds) {
                spell.stopping = true; // never loaded
                m_actors->stopEffects(spell.actor);
            }
        }
        if (!spell.stopping && spell.timed) {
            spell.remaining -= dt;
            if (spell.remaining <= 0.0f) {
                spell.stopping = true;
                m_actors->stopEffects(spell.actor);
            }
            continue;
        }
        if (spell.stopping) {
            // Gone once its last particle has died (or after a while, when nobody looked at it).
            spell.fading += dt;
            if ((spell.fading > 0.1f && !m_actors->effectsAlive(spell.actor)) ||
                spell.fading > kMaxFadeSeconds) {
                endSpell(i);
            }
        }
    }
}

} // namespace vd
