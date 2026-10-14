#pragma once

#include "core/Types.h"
#include "fx/ParticleSystem.h"
#include "render/Renderer.h"
#include "world/Camera.h"

#include <glm/vec3.hpp>

#include <string_view>
#include <vector>

namespace vd {

class Actors;
class JobSystem;
class WorldObjects;

// A spell effect played by Effects (spawnAt / spawnOn). The generation tells a stale handle from the slot's
// next user.
struct EffectHandle {
    u32 index = ~0u;
    u32 generation = 0;
    bool valid() const { return index != ~0u; }
};

// Phase 9: the particles and ribbons of the world (doodads), of actors (through Actors, which places them
// with their skeletons) and of spell effects. Owns the particle system; every frame it keeps a particle
// instance for each doodad with emitters that WorldObjects found in view (created warm, released a while
// after it leaves the view), then simulates everything placed and writes the quads straight into the
// renderer's buffer. Game thread only.
class Effects {
public:
    // `actors` (may be null) shows the spell effects and gets the particle system for its own models.
    void init(Actors* actors, const fx::ParticleSystem::Settings& settings = {});
    void shutdown();
    fx::ParticleSystem& particles() { return m_particles; }
    const fx::ParticleSystem& particles() const { return m_particles; }

    // Doodads of WorldObjects::visibleEffects() (after its collect()): their instances, placed for this
    // frame.
    void placeDoodads(WorldObjects& objects, u32 nowMs);
    // Simulates whatever was placed this frame (doodads, actors, spells) and writes its quads into the
    // renderer's particle buffer; fills `draws`. Returns the quads written (FrameParams::particleQuads).
    u32 update(f32 dt, const Camera& camera, const Frustum& frustum, f32 viewportHeight, JobSystem& jobs,
               Renderer& renderer, std::vector<ParticleDraw>& draws);
    // Drops every doodad instance (map change).
    void clearDoodads();

    // --- Spell visuals (the API of f9/spellfx)
    // ------------------------------------------------------------------ An effect model of actors.vdpk
    // (Spells\*.m2 and every model SpellVisualEffectName.dbc names; see effectModelHash) played by an actor
    // of its own: its meshes, skeleton animation (Stand, looping), texture animations, particles and ribbons.
    // `seconds` > 0: it stops by itself after that long (one-shot impacts and casts); 0: until stop(); kOnce:
    // its animation plays once (the one-shot kits of SpellVisualKit), between kOnceMinSeconds and
    // kOnceMaxSeconds. Stopping lets the particles already flying die out, then the effect goes away. Invalid
    // handle when there is no actors pack or no free slot.
    static constexpr f32 kOnce = -1.0f;
    static constexpr f32 kOnceMinSeconds = 0.6f;
    static constexpr f32 kOnceMaxSeconds = 5.0f;
    EffectHandle spawnAt(u64 model, const glm::dvec3& position, f32 yaw, f32 scale = 1.0f,
                         f32 seconds = 0.0f);
    // The same on an actor's attachment point (game::attach::kHandRight, kHandLeft, kHelm, kShield..., the
    // ids of the M2 attachments: 0 shield/mount saddle, 1-2 hands, 5-6 shoulders, 11 helm, 18 over the
    // head...), following it.
    EffectHandle spawnOn(u64 model, u32 actor, u32 attachmentId, f32 scale = 1.0f, f32 seconds = 0.0f);
    // Phase 9 (f9/spellfx): on a point of what the actor holds in an equipment slot (game::kSlotMainHand,
    // kSlotOffHand: a weapon's points 0-4 along its blade), following it; hidden while nothing is held there.
    EffectHandle spawnOnItem(u64 model, u32 actor, u8 equipSlot, u32 itemAttachmentId, f32 scale = 1.0f,
                             f32 seconds = 0.0f);
    void move(EffectHandle effect, const glm::dvec3& position, f32 yaw); // missiles
    void stop(EffectHandle effect);
    bool alive(EffectHandle effect) const;
    // Lifetimes and fading out; once per frame, before Actors::beginCollect.
    void updateSpells(f32 dt);
    u32 spellCount() const { return m_spellCount; }
    // actors.vdpk asset hash of a model path as the client tables spell it ("Spells\Frostbolt_Missile.mdx",
    // with a leading backslash or ".mdl"): the .m2 path, canonical.
    static u64 effectModelHash(std::string_view clientPath);

    // Development (VD_FX_OFF=1, the debug panel): no particles and no texture animations, for before/after
    // captures.
    bool enabled = true;

    // Frames a doodad instance survives out of view (or beyond the distance) before its particles are
    // released.
    u32 doodadGraceFrames = 90;
    u32 doodadInstances() const { return static_cast<u32>(m_doodads.size()); }

private:
    struct Doodad {
        u32 instance = 0; // WorldObjects instance
        u32 serial = 0;   // WorldObjects::instanceSerial when created (a reused slot is another doodad)
        u32 fx = fx::ParticleSystem::kNone;
        u32 seenFrame = 0;
    };

    struct Spell {
        u32 actor = ~0u;
        u32 parent = ~0u; // spawnOn: the actor it follows (stops when that one goes away)
        u32 generation = 0;
        f32 remaining = 0.0f; // seconds until it stops (<= 0 with `timed`: stop now)
        f32 fading = 0.0f;    // seconds since it stopped
        f32 age = 0.0f;       // kOnce: seconds waiting for its model (to know how long its animation is)
        bool once = false;    // kOnce until its model is loaded, then timed
        bool timed = false;
        bool stopping = false;
        bool alive = false;
    };
    static constexpr u32 kMaxSpells = 512;
    static constexpr f32 kMaxFadeSeconds =
        10.0f; // a stopped effect is removed after this even if particles remain

    void endSpell(u32 index);

    fx::ParticleSystem m_particles;
    Actors* m_actors = nullptr;
    std::vector<Spell> m_spells;
    std::vector<u32> m_freeSpells;
    u32 m_spellCount = 0;
    std::vector<Doodad> m_doodads;
    std::vector<u32> m_doodadOf;           // WorldObjects instance -> index into m_doodads (kNone)
    std::vector<TextureHandle> m_textures; // scratch
    u32 m_frame = 0;
};

} // namespace vd
