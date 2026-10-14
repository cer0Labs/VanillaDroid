#pragma once

#include "anim/BoneMatrix.h"
#include "core/Types.h"
#include "fx/EffectSet.h"
#include "render/Renderer.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <span>
#include <vector>

namespace vd {
class JobSystem;
}

namespace vd::fx {

struct ParticleStats {
    u32 instances = 0; // live effect instances
    u32 placed = 0;    // placed this frame
    u32 emitters = 0;  // emitters simulated this frame (in view, near enough)
    u32 culled = 0;    // emitters placed but off screen, too far or too small
    u32 ribbons = 0;   // ribbons simulated this frame
    u32 particles = 0; // alive in the simulated emitters
    u32 quads = 0;     // written this frame (particles and ribbon segments)
    u32 draws = 0;
    u32 starved = 0;       // emitters without room in the particle pools (not drawn)
    u32 clipped = 0;       // emitters beyond the frame's quad capacity (simulated, not drawn)
    f32 updateMs = 0.0f;   // wall time of update() on the calling thread
    f32 prepareMs = 0.0f;  // of which: placing the emitters and sampling their parameters
    f32 simulateMs = 0.0f; // simulating and writing the quads (workers included)
    f32 rateScale = 1.0f;  // emission scale the CPU budget currently allows
};

// The camera the particles are built for.
struct ParticleView {
    glm::dvec3 camera{0.0};
    glm::vec3 right{0.0f, -1.0f, 0.0f}; // camera axes, world
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    glm::vec3 forward{1.0f, 0.0f, 0.0f};
    std::array<glm::vec4, 6>
        frustum{};               // camera-relative planes (Frustum::planes): xyz inward normal, w distance
    f32 pixelsPerYard = 1000.0f; // on-screen pixels of one yard at one yard: height / (2 tan(fovY / 2))
};

// Where an effect instance is this frame. `toWorld`: model -> world relative to `origin` (any nearby world
// point: the camera, the model's placement), rows of [rotation * scale | translation]. Actors give their
// skinning matrices (model space, valid until update()) and the sequence they play; doodads loop their first
// sequence on `globalMs`.
struct Placement {
    glm::dvec3 origin{0.0};
    anim::BoneMatrix toWorld{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
    const anim::BoneMatrix* bones = nullptr;
    u32 boneCount = 0;
    u32 sequence = 0;
    u32 localMs = 0; // time in the sequence; doodads: ignored (globalMs % loop)
    u32 globalMs = 0;
};

// Particles and ribbons of placed models (doodads, actors, spell effects), simulated with the 1.12 client's
// rules: emitters spawn `rate` particles per second at their bone (plane, sphere or spline shapes), which fly
// with gravity, wind and drag, live `lifespan` seconds and go through three colors, opacities and sizes;
// ribbons trail their bone. Each emitter is deterministic (its own random sequence from the instance seed).
//
// Budgets: emitters only run while placed, in view and near enough (further ones emit less; ones smaller than
// a pixel or off screen are frozen), particles live in fixed pools reserved by init() (an emitter that finds
// no room does not emit), the frame's quads are capped by the renderer's buffer, and the emission rate scales
// down when update() goes over its CPU budget. Nothing allocates after init(). Game thread only (update() may
// fan out to the job workers).
class ParticleSystem {
public:
    static constexpr u32 kNone = ~0u;

    struct Settings {
        f32 fullRateDistance = 25.0f; // yards: closer emitters spawn at their full rate
        f32 maxDistance = 100.0f;     // further (plus the effect's reach) nothing runs
        f32 minRateScale = 0.3f;      // rate at maxDistance
        f32 minPixels = 1.0f;         // emitters whose largest particle covers less are not run
        f32 budgetMs = 1.0f;          // update() time per frame; above it the emission rate scales down
        f32 density = 1.0f;           // phase 10 (quality, temperature): every emission rate times this
        u32 maxInstances = 4096;
        u32 maxEmitters = 8192;
        u32 maxRibbons = 512;
        u32 maxQuads = 16384; // per frame (the renderer's buffer holds as many)
        bool workers = true;  // simulate on the job workers (only where they run on fast cores: see Actors)
        bool meshes = true;   // phase 14 (VD_MODEL_PARTICLES=0): emitters of models drawn (else left out)
    };

    ParticleSystem();
    ~ParticleSystem();
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    void init(const Settings& settings);
    void shutdown();
    Settings& settings() { return m_settings; }

    // An instance of a model's effects. `textures`: one per emitter of `set`, then one per ribbon (invalid
    // handles draw white). Ribbons only run for actors. `warm`: start as if it had been running for a while
    // (doodads; a spell effect starts empty). The set must outlive the instance. kNone when the instance
    // table is full.
    u32 create(const EffectSet& set, std::span<const TextureHandle> textures, u32 seed, bool warm);
    void setTexture(u32 instance, u32 slot, TextureHandle texture);
    // No new particles; the instance frees itself once its last particle has died (keep placing it until
    // then).
    void stop(u32 instance);
    void destroy(u32 instance); // now, with its particles
    bool alive(u32 instance) const;
    u32 particleCount(u32 instance) const;

    // Places an instance for this frame. Only placed instances run (the others are frozen).
    void place(u32 instance, const Placement& placement);
    // Forgets this frame's placements without running them (effects switched off).
    void skipFrame() { m_placed.clear(); }

    // Simulates the placed instances by dt and writes their quads into `vertices` (4 per quad; the renderer's
    // mapped memory, write-only) and the draw list (cleared here); returns the number of quads written.
    u32 update(f32 dt, const ParticleView& view, JobSystem* jobs, std::span<ParticleVertex> vertices,
               std::vector<ParticleDraw>& draws);

    const ParticleStats& stats() const { return m_stats; }

    // Test hooks: particles of an instance's emitter (anchor space), oldest first.
    struct ParticleState {
        glm::vec3 position;
        glm::vec3 velocity;
        f32 age;
        f32 life;
    };
    u32 debugParticles(u32 instance, u32 emitter, std::span<ParticleState> out) const;

private:
    struct Particle {
        glm::vec3 position; // anchor space, or emitter space (kParticleModelSpace)
        f32 age;
        glm::vec3 velocity;
        f32 life;
        u32 seed;
        f32 reserved;
    };
    struct Emitter;
    struct Ribbon;
    struct Instance;
    struct Work;
    struct WorkFn;

    u32 allocateSlab(u32 capacity, u32& granted);
    void freeSlab(u32 slab, u32 capacity);
    void releaseInstance(u32 instance);
    void prepareEmitter(Instance& instance, u32 emitterIndex, const ParticleView& view, f32 dt);
    void prepareRibbon(Instance& instance, u32 ribbonIndex, const ParticleView& view, f32 dt);
    void runWork(u32 begin, u32 end);
    void simulate(Emitter& e, f32 dt);
    void spawn(Emitter& e, f32 dt, u32 count);
    void writeQuads(const Emitter& e, const Work& work);
    // Phase 14 (spellfx): an emitter of models writes each particle as the model's triangles (one quad each).
    void writeMeshParticles(const Emitter& e, const Work& work, const pack::ParticleMesh& mesh);
    void simulateRibbon(Ribbon& r, f32 dt);
    void writeRibbon(const Ribbon& r, const Work& work);

    Settings m_settings;
    std::vector<Instance> m_instances;
    std::vector<u32> m_freeInstances;
    std::vector<Emitter> m_emitters;
    std::vector<u32> m_freeEmitters;
    std::vector<Ribbon> m_ribbons;
    std::vector<u32> m_freeRibbons;
    // Particle storage: slabs of 8 << c particles in pool c (power-of-two size classes).
    static constexpr u32 kSizeClasses = 7;
    std::vector<Particle> m_particles;
    std::array<u32, kSizeClasses> m_classBase{};
    std::array<std::vector<u32>, kSizeClasses> m_freeSlabs; // offsets into m_particles
    std::vector<u32> m_placed;                              // instances placed this frame
    std::vector<Work> m_work; // this frame's emitters and ribbons, in draw order
    // The quads are built in cached memory and copied to the renderer's mapped (write-combined, sometimes
    // uncached) buffer in one go: scattered small writes straight into it were 100x slower on the desktop
    // GPU.
    std::vector<ParticleVertex> m_scratch;
    f32 m_dt = 0.0f;
    glm::vec3 m_right{0.0f}, m_up{0.0f}, m_forward{0.0f};
    u32 m_frame = 0;
    f32 m_rateScale = 1.0f;
    ParticleStats m_stats;
};

} // namespace vd::fx
