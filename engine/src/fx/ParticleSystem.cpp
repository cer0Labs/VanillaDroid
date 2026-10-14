#include "fx/ParticleSystem.h"

#include "anim/Skeleton.h"
#include "core/Clock.h"
#include "core/Profile.h"
#include "fx/ParticleMath.h"
#include "jobs/JobSystem.h"
#include "pack/ModelFormat.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vd::fx {
namespace {

// Particle pools: class c holds slabs of 8 << c particles (powers of two: rings index with a mask).
constexpr std::array<u32, 7> kSlabCounts = {1024, 1024, 1024, 384, 128, 48, 16};
constexpr u32 kSlabBase = 8;
constexpr u32 kRibbonEdges = 96; // edges a ribbon keeps (its rate drops if its life needs more)
constexpr f32 kMaxStep = 0.1f;   // seconds: longer frames are simulated in steps of at most this
constexpr f32 kWarmSeconds = 3.0f;
constexpr f32 kNearFade = 2.0f; // particles fade out closer to the camera than this many times their size
constexpr u32 kParallelQuads = 2048; // frames with fewer quads run on the calling thread
constexpr u32 kMaxMeshQuads = 1024;  // phase 14: the triangles one emitter of models writes at most

// Random table of the twinkle (shared, fixed: the same particle twinkles the same way every run).
struct RandTable {
    std::array<f32, 128> values{};
    RandTable() {
        u32 state = 0x2545F491u;
        for (f32& v : values) {
            state = state * 1664525u + 1013904223u;
            v = static_cast<f32>(state >> 8) * (1.0f / 16777216.0f);
        }
    }
};
const RandTable kRandTable;

u32 nextRandom(u32& state) {
    // xorshift32: never 0 once seeded with a nonzero value.
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
f32 uniform(u32& state) { // [-1, 1)
    return static_cast<f32>(nextRandom(state) >> 8) * (2.0f / 16777216.0f) - 1.0f;
}
f32 uniformPos(u32& state) { // [0, 1)
    return static_cast<f32>(nextRandom(state) >> 8) * (1.0f / 16777216.0f);
}

glm::vec4 unpackColor(u32 rgba) {
    return glm::vec4(static_cast<f32>(rgba & 0xFF), static_cast<f32>((rgba >> 8) & 0xFF),
                     static_cast<f32>((rgba >> 16) & 0xFF), static_cast<f32>(rgba >> 24)) *
           (1.0f / 255.0f);
}

u32 packColor(const glm::vec4& c) {
    auto byte = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (byte(c.w) << 24);
}

u16 unorm16(f32 v) {
    return static_cast<u16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

glm::vec3 translationOf(const anim::BoneMatrix& m) {
    return {m.rows[3], m.rows[7], m.rows[11]};
}
glm::vec3 rotate(const anim::BoneMatrix& m, const glm::vec3& v) {
    return {m.rows[0] * v.x + m.rows[1] * v.y + m.rows[2] * v.z,
            m.rows[4] * v.x + m.rows[5] * v.y + m.rows[6] * v.z,
            m.rows[8] * v.x + m.rows[9] * v.y + m.rows[10] * v.z};
}
glm::vec3 column(const anim::BoneMatrix& m, u32 c) {
    return {m.rows[c], m.rows[4 + c], m.rows[8 + c]};
}
anim::BoneMatrix translationMatrix(const f32 (&t)[3]) {
    return anim::BoneMatrix{{1.0f, 0.0f, 0.0f, t[0], 0.0f, 1.0f, 0.0f, t[1], 0.0f, 0.0f, 1.0f, t[2]}};
}
anim::BoneMatrix fromRows(const f32 (&rows)[12]) {
    anim::BoneMatrix m;
    std::memcpy(m.rows, rows, sizeof(m.rows));
    return m;
}

bool sphereInFrustum(const std::array<glm::vec4, 6>& planes, const glm::vec3& center, f32 radius) {
    for (const glm::vec4& plane : planes) {
        if (plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w < -radius) {
            return false;
        }
    }
    return true;
}

void writeQuad(ParticleVertex* dst, const glm::vec3& center, const glm::vec3& m0, const glm::vec3& m1, f32 u0,
               f32 v0, f32 u1, f32 v1, u32 color) {
    const u16 a = unorm16(u0), b = unorm16(v0), c = unorm16(u1), d = unorm16(v1);
    const glm::vec3 p0 = center - m0 + m1;
    const glm::vec3 p1 = center - m0 - m1;
    const glm::vec3 p2 = center + m0 + m1;
    const glm::vec3 p3 = center + m0 - m1;
    // Mapped (write-combined) memory: whole vertices in order, never read back.
    dst[0] = {{p0.x, p0.y, p0.z}, {a, b}, color};
    dst[1] = {{p1.x, p1.y, p1.z}, {a, d}, color};
    dst[2] = {{p2.x, p2.y, p2.z}, {c, b}, color};
    dst[3] = {{p3.x, p3.y, p3.z}, {c, d}, color};
}

void writeEmptyQuads(ParticleVertex* dst, u32 quads) {
    const ParticleVertex empty{{0.0f, 0.0f, 0.0f}, {0, 0}, 0};
    for (u32 i = 0; i < quads * 4; ++i) {
        dst[i] = empty; // zero area: nothing rasterized
    }
}

bool alphaBlended(u8 blend) {
    return blend <= static_cast<u8>(pack::ModelBlend::Alpha);
}

} // namespace

struct ParticleSystem::Emitter {
    const pack::ParticleEmitter* desc = nullptr;
    u32 instance = kNone;
    u32 next = kNone; // next emitter of the instance
    u32 index = 0;    // in the effect set
    TextureHandle texture;
    u32 slab = kNone;
    u32 capacity = 0;
    u32 head = 0; // ring of `capacity` particles at `slab`: the oldest
    u32 count = 0;
    f32 emission = 0.0f; // particles owed (fraction carried over)
    u32 random = 1;
    bool warm = false;
    bool hasPrevious = false;
    // This frame (prepareEmitter).
    anim::BoneMatrix toAnchor{};
    glm::vec3 origin{0.0f};
    glm::vec3 previousOrigin{0.0f};
    glm::vec3 cameraOffset{0.0f}; // anchor - camera
    glm::vec3 deltaPosition{0.0f};
    glm::vec3 burst{0.0f};
    f32 burstTime = 0.0f;
    f32 scale = 1.0f;
    f32 rate = 0.0f;
    std::array<f32, pack::kEmitterParamCount> values{};
};

struct ParticleSystem::Ribbon {
    struct Edge {
        glm::vec3 top;
        f32 age;
        glm::vec3 bottom;
        f32 fall; // yards per second downwards (gravity)
    };
    const pack::RibbonEmitter* desc = nullptr;
    u32 instance = kNone;
    u32 next = kNone;
    TextureHandle texture;
    std::array<Edge, kRibbonEdges> edges{};
    u32 head = 0;
    u32 count = 0;
    f32 sinceEdge = 0.0f;
    // This frame.
    glm::vec3 top{0.0f};
    glm::vec3 bottom{0.0f};
    glm::vec3 cameraOffset{0.0f};
    f32 alpha = 1.0f;
    bool emitting = true;
};

struct ParticleSystem::Instance {
    const EffectSet* set = nullptr;
    bool alive = false;
    bool stopping = false;
    bool anchored = false;
    bool warm = false;
    glm::dvec3 anchor{0.0}; // world origin of its particles' coordinates
    u32 firstEmitter = kNone;
    u32 firstRibbon = kNone;
    u32 placedFrame = 0;
    u32 simulatedFrame = 0; // some emitter in view this frame
    Placement placement;
};

struct ParticleSystem::Work {
    u64 key = 0;
    u32 index = 0;
    u32 firstQuad = 0;
    u32 quadCapacity = 0;
    bool ribbon = false;
    u8 blend = 0;
    u8 flags = 0;
    TextureHandle texture;
};

struct ParticleSystem::WorkFn {
    ParticleSystem* self = nullptr;
    void operator()(u32 begin, u32 end) const { self->runWork(begin, end); }
};

ParticleSystem::ParticleSystem() = default;
ParticleSystem::~ParticleSystem() = default;

void ParticleSystem::init(const Settings& settings) {
    m_settings = settings;
    m_instances.assign(settings.maxInstances, Instance{});
    m_freeInstances.clear();
    m_freeInstances.reserve(settings.maxInstances);
    for (u32 i = settings.maxInstances; i-- > 0;) {
        m_freeInstances.push_back(i);
    }
    m_emitters.assign(settings.maxEmitters, Emitter{});
    m_freeEmitters.clear();
    m_freeEmitters.reserve(settings.maxEmitters);
    for (u32 i = settings.maxEmitters; i-- > 0;) {
        m_freeEmitters.push_back(i);
    }
    m_ribbons.assign(settings.maxRibbons, Ribbon{});
    m_freeRibbons.clear();
    m_freeRibbons.reserve(settings.maxRibbons);
    for (u32 i = settings.maxRibbons; i-- > 0;) {
        m_freeRibbons.push_back(i);
    }
    u32 total = 0;
    for (u32 c = 0; c < kSizeClasses; ++c) {
        m_classBase[c] = total;
        total += kSlabCounts[c] * (kSlabBase << c);
    }
    m_particles.assign(total, Particle{});
    for (u32 c = 0; c < kSizeClasses; ++c) {
        m_freeSlabs[c].clear();
        m_freeSlabs[c].reserve(kSlabCounts[c]);
        for (u32 s = kSlabCounts[c]; s-- > 0;) {
            m_freeSlabs[c].push_back(m_classBase[c] + s * (kSlabBase << c));
        }
    }
    m_scratch.assign(u64{settings.maxQuads} * 4, ParticleVertex{});
    m_placed.clear();
    m_placed.reserve(settings.maxInstances);
    m_work.clear();
    m_work.reserve(settings.maxEmitters + settings.maxRibbons);
    m_rateScale = 1.0f;
    m_stats = {};
}

void ParticleSystem::shutdown() {
    m_scratch.clear();
    m_instances.clear();
    m_freeInstances.clear();
    m_emitters.clear();
    m_freeEmitters.clear();
    m_ribbons.clear();
    m_freeRibbons.clear();
    m_particles.clear();
    for (std::vector<u32>& slabs : m_freeSlabs) {
        slabs.clear();
    }
    m_placed.clear();
    m_work.clear();
}

u32 ParticleSystem::allocateSlab(u32 capacity, u32& granted) {
    u32 wanted = 0;
    while (wanted + 1 < kSizeClasses && (kSlabBase << wanted) < capacity) {
        ++wanted;
    }
    // The class that fits, a larger one, or (pools short) a smaller one: fewer particles rather than none.
    for (u32 c = wanted; c < kSizeClasses; ++c) {
        if (!m_freeSlabs[c].empty()) {
            const u32 slab = m_freeSlabs[c].back();
            m_freeSlabs[c].pop_back();
            granted = kSlabBase << c;
            return slab;
        }
    }
    for (u32 c = wanted; c-- > 0;) {
        if (!m_freeSlabs[c].empty()) {
            const u32 slab = m_freeSlabs[c].back();
            m_freeSlabs[c].pop_back();
            granted = kSlabBase << c;
            return slab;
        }
    }
    granted = 0;
    return kNone;
}

void ParticleSystem::freeSlab(u32 slab, u32 capacity) {
    for (u32 c = 0; c < kSizeClasses; ++c) {
        if ((kSlabBase << c) == capacity) {
            m_freeSlabs[c].push_back(slab);
            return;
        }
    }
}

u32 ParticleSystem::create(const EffectSet& set, std::span<const TextureHandle> textures, u32 seed,
                           bool warm) {
    if (m_freeInstances.empty() || !set.hasParticles()) {
        return kNone;
    }
    const u32 emitters = static_cast<u32>(set.emitters().size());
    const u32 ribbons = set.actor() ? static_cast<u32>(set.ribbons().size()) : 0;
    if (m_freeEmitters.size() < emitters || m_freeRibbons.size() < ribbons) {
        return kNone;
    }
    const u32 index = m_freeInstances.back();
    m_freeInstances.pop_back();
    Instance& instance = m_instances[index];
    instance = Instance{};
    instance.set = &set;
    instance.alive = true;
    instance.warm = warm;
    u32* link = &instance.firstEmitter;
    for (u32 i = 0; i < emitters; ++i) {
        const u32 e = m_freeEmitters.back();
        m_freeEmitters.pop_back();
        Emitter& emitter = m_emitters[e];
        emitter = Emitter{};
        emitter.desc = &set.emitters()[i];
        emitter.instance = index;
        emitter.index = i;
        emitter.texture = i < textures.size() ? textures[i] : TextureHandle{};
        emitter.random = (seed ^ (0x9E3779B9u * (i + 1))) | 1u;
        emitter.warm = warm;
        *link = e;
        link = &emitter.next;
    }
    link = &instance.firstRibbon;
    for (u32 i = 0; i < ribbons; ++i) {
        const u32 r = m_freeRibbons.back();
        m_freeRibbons.pop_back();
        Ribbon& ribbon = m_ribbons[r];
        ribbon.desc = &set.ribbons()[i];
        ribbon.instance = index;
        ribbon.next = kNone;
        ribbon.texture = emitters + i < textures.size() ? textures[emitters + i] : TextureHandle{};
        ribbon.head = 0;
        ribbon.count = 0;
        ribbon.sinceEdge = 0.0f;
        *link = r;
        link = &ribbon.next;
    }
    ++m_stats.instances;
    return index;
}

void ParticleSystem::setTexture(u32 instance, u32 slot, TextureHandle texture) {
    if (!alive(instance)) {
        return;
    }
    u32 n = 0;
    for (u32 e = m_instances[instance].firstEmitter; e != kNone; e = m_emitters[e].next, ++n) {
        if (n == slot) {
            m_emitters[e].texture = texture;
            return;
        }
    }
    for (u32 r = m_instances[instance].firstRibbon; r != kNone; r = m_ribbons[r].next, ++n) {
        if (n == slot) {
            m_ribbons[r].texture = texture;
            return;
        }
    }
}

void ParticleSystem::stop(u32 instance) {
    if (alive(instance)) {
        m_instances[instance].stopping = true;
    }
}

void ParticleSystem::destroy(u32 instance) {
    if (alive(instance)) {
        releaseInstance(instance);
    }
}

bool ParticleSystem::alive(u32 instance) const {
    return instance < m_instances.size() && m_instances[instance].alive;
}

u32 ParticleSystem::particleCount(u32 instance) const {
    if (!alive(instance)) {
        return 0;
    }
    u32 total = 0;
    for (u32 e = m_instances[instance].firstEmitter; e != kNone; e = m_emitters[e].next) {
        total += m_emitters[e].count;
    }
    for (u32 r = m_instances[instance].firstRibbon; r != kNone; r = m_ribbons[r].next) {
        total += m_ribbons[r].count;
    }
    return total;
}

void ParticleSystem::releaseInstance(u32 index) {
    Instance& instance = m_instances[index];
    for (u32 e = instance.firstEmitter; e != kNone;) {
        Emitter& emitter = m_emitters[e];
        const u32 next = emitter.next;
        if (emitter.slab != kNone) {
            freeSlab(emitter.slab, emitter.capacity);
        }
        emitter = Emitter{};
        m_freeEmitters.push_back(e);
        e = next;
    }
    for (u32 r = instance.firstRibbon; r != kNone;) {
        Ribbon& ribbon = m_ribbons[r];
        const u32 next = ribbon.next;
        ribbon.desc = nullptr;
        ribbon.instance = kNone;
        ribbon.next = kNone;
        ribbon.count = 0;
        m_freeRibbons.push_back(r);
        r = next;
    }
    instance = Instance{};
    m_freeInstances.push_back(index);
    --m_stats.instances;
}

void ParticleSystem::place(u32 index, const Placement& placement) {
    if (!alive(index)) {
        return;
    }
    Instance& instance = m_instances[index];
    instance.placement = placement;
    if (!instance.anchored) {
        // Particle coordinates stay small and precise around the instance's first position.
        instance.anchor = placement.origin + glm::dvec3(translationOf(placement.toWorld));
        instance.anchored = true;
    }
    if (instance.placedFrame != m_frame + 1) {
        instance.placedFrame = m_frame + 1;
        m_placed.push_back(index);
    }
}

void ParticleSystem::prepareEmitter(Instance& instance, u32 index, const ParticleView& view, f32 dt) {
    Emitter& e = m_emitters[index];
    const pack::ParticleEmitter& d = *e.desc;
    const Placement& p = instance.placement;
    const EffectSet& set = *instance.set;
    const pack::ParticleMesh* mesh = set.mesh(e.index); // phase 14: its particles are a model
    if (mesh && !m_settings.meshes) {
        return;
    }

    // Emitter -> anchor space this frame.
    anim::BoneMatrix local;
    if (set.actor() || (p.bones && d.bone < p.boneCount)) { // phase 14: an animated doodad gives its bones too
        local = translationMatrix(d.position);
        if (p.bones && d.bone < p.boneCount) {
            local = anim::multiply(p.bones[d.bone], local);
        }
    } else {
        local = fromRows(d.transform);
    }
    e.toAnchor = anim::multiply(p.toWorld, local);
    const glm::vec3 shift(p.origin - instance.anchor);
    e.toAnchor.rows[3] += shift.x;
    e.toAnchor.rows[7] += shift.y;
    e.toAnchor.rows[11] += shift.z;
    e.origin = translationOf(e.toAnchor);
    e.cameraOffset = glm::vec3(instance.anchor - view.camera);
    e.scale = glm::length(column(e.toAnchor, 0));
    const glm::vec3 delta = e.hasPrevious ? e.origin - e.previousOrigin : glm::vec3(0.0f);
    e.previousOrigin = e.origin;
    e.hasPrevious = true;

    // Parameters at this time.
    const u32 sequence = set.actor() ? p.sequence : 0;
    const u32 localMs = set.actor() ? p.localMs : (set.loopMs() ? p.globalMs % set.loopMs() : 0);
    const anim::TrackTable& tracks = set.tracks();
    for (u32 param = 0; param < pack::kEmitterParamCount; ++param) {
        e.values[param] =
            d.tracks[param] == pack::kEffectNoTrack
                ? d.values[param]
                : tracks.sampleScalar(d.tracks[param], sequence, localMs, p.globalMs, d.values[param]);
    }

    // In view, near enough, big enough on screen?
    const glm::vec3 center = e.cameraOffset + e.origin;
    const f32 distance = glm::length(center);
    const f32 reach = d.reach * std::max(e.scale, 0.01f) + 1.0f;
    const f32 size = std::max({d.scales[0], d.scales[1], d.scales[2]}) * std::max(1.0f, d.twinkleMax) *
                     ((d.flags & pack::kParticleInheritScale) ? e.scale : 1.0f);
    const bool visible = distance - reach < m_settings.maxDistance &&
                         sphereInFrustum(view.frustum, center, reach) &&
                         (distance < 1.0f || size / distance * view.pixelsPerYard >= m_settings.minPixels);
    if (!visible) {
        ++m_stats.culled;
        return; // frozen
    }
    if (e.slab == kNone) {
        e.slab = allocateSlab(set.capacity(e.index), e.capacity);
        if (e.slab == kNone) {
            ++m_stats.starved;
            return;
        }
        e.head = 0;
        e.count = 0;
    }

    // Emission, fewer with distance and under CPU pressure.
    const f32 farthest = std::max(m_settings.maxDistance, m_settings.fullRateDistance + 1.0f);
    const f32 lod = distance <= m_settings.fullRateDistance
                        ? 1.0f
                        : 1.0f + (m_settings.minRateScale - 1.0f) *
                                     std::min(1.0f, (distance - m_settings.fullRateDistance) /
                                                        (farthest - m_settings.fullRateDistance));
    const bool emitting = !instance.stopping && e.values[pack::kEmitterEnabled] > 0.5f;
    e.rate = emitting ? std::max(0.0f, e.values[pack::kEmitterRate]) * lod * m_rateScale * m_settings.density : 0.0f;

    // The emitter's own motion: followed by its particles, or a burst of speed for new ones.
    if (d.flags & pack::kParticleFollow) {
        const f32 den = d.followSpeed2 - d.followSpeed1;
        const f32 mult = std::fabs(den) > 1e-6f ? (d.followScale2 - d.followScale1) / den : 0.0f;
        const f32 base = d.followScale1 - d.followSpeed1 * mult;
        f32 x = mult * (dt > 0.0f ? glm::length(delta) / dt : 0.0f) + base;
        x = x >= 0.0f ? std::min(x, 1.0f) : x;
        e.deltaPosition = delta * x;
    } else {
        e.deltaPosition = glm::vec3(0.0f);
    }
    if (d.flags & pack::kParticleBurst) {
        constexpr f32 kBurstFrame = 0.03f;
        e.burstTime += dt;
        if (e.burstTime > kBurstFrame) {
            e.burst =
                e.count == 0 ? delta * (kBurstFrame / e.burstTime) * d.burstMultiplier : glm::vec3(0.0f);
            e.burstTime = 0.0f;
        }
    }

    Work work;
    work.index = index;
    work.ribbon = false;
    work.blend = d.blend == static_cast<u8>(pack::ModelBlend::AlphaKey)
                     ? static_cast<u8>(pack::ModelBlend::Alpha)
                     : d.blend;
    work.flags =
        static_cast<u8>(((d.flags & pack::kParticleUnfogged) ? kParticleDrawUnfogged : 0) |
                        ((d.flags & pack::kParticleLit) && alphaBlended(work.blend) ? kParticleDrawLit : 0));
    work.texture = e.texture;
    const u32 perParticle = mesh                                                    ? mesh->triangleCount
                            : d.style == static_cast<u8>(pack::ParticleStyle::Both) ? 2
                                                                                    : 1;
    const f32 owed = e.emission + e.rate * std::min(dt, kMaxStep * 4.0f);
    const u32 upper = e.warm ? e.capacity : std::min(e.capacity, e.count + static_cast<u32>(owed) + 1);
    work.quadCapacity = mesh ? std::min(upper * perParticle, kMaxMeshQuads) : upper * perParticle;
    // Blended ones back to front, then the order-free blends grouped by state.
    if (alphaBlended(work.blend)) {
        work.key = static_cast<u64>(0xFFFFFFFFu - static_cast<u32>(std::min(distance, 60000.0f) * 64.0f));
    } else {
        work.key = (1ull << 63) | (static_cast<u64>(work.blend) << 56) |
                   (static_cast<u64>(work.flags) << 48) | static_cast<u64>(work.texture.index);
    }
    m_work.push_back(work);
    instance.simulatedFrame = m_frame + 1;
}

void ParticleSystem::prepareRibbon(Instance& instance, u32 index, const ParticleView& view, f32 dt) {
    (void)dt;
    Ribbon& r = m_ribbons[index];
    const pack::RibbonEmitter& d = *r.desc;
    const Placement& p = instance.placement;
    const EffectSet& set = *instance.set;
    anim::BoneMatrix local = translationMatrix(d.position);
    if (p.bones && d.bone < p.boneCount) {
        local = anim::multiply(p.bones[d.bone], local);
    }
    anim::BoneMatrix toAnchor = anim::multiply(p.toWorld, local);
    const glm::vec3 shift(p.origin - instance.anchor);
    const glm::vec3 position = translationOf(toAnchor) + shift;
    r.cameraOffset = glm::vec3(instance.anchor - view.camera);
    f32 values[pack::kRibbonParamCount];
    for (u32 param = 0; param < pack::kRibbonParamCount; ++param) {
        values[param] = d.tracks[param] == pack::kEffectNoTrack
                            ? d.values[param]
                            : set.tracks().sampleScalar(d.tracks[param], p.sequence, p.localMs, p.globalMs,
                                                        d.values[param]);
    }
    // The strip spreads along the bone's Z.
    glm::vec3 axis = rotate(toAnchor, glm::vec3(0.0f, 0.0f, 1.0f));
    const f32 length = glm::length(axis);
    axis = length > 1e-6f ? axis / length : glm::vec3(0.0f, 0.0f, 1.0f);
    const f32 scale = length > 1e-6f ? length : 1.0f;
    r.top = position + axis * values[pack::kRibbonAbove] * scale;
    r.bottom = position - axis * values[pack::kRibbonBelow] * scale;
    r.alpha = std::clamp(values[pack::kRibbonAlpha], 0.0f, 1.0f);
    r.emitting = !instance.stopping && values[pack::kRibbonVisibility] > 0.5f;

    const glm::vec3 center = r.cameraOffset + position;
    const f32 distance = glm::length(center);
    const f32 reach = set.reach() + 2.0f;
    if (distance - reach > m_settings.maxDistance || !sphereInFrustum(view.frustum, center, reach)) {
        ++m_stats.culled;
        return;
    }
    Work work;
    work.index = index;
    work.ribbon = true;
    work.blend = d.blend == static_cast<u8>(pack::ModelBlend::AlphaKey)
                     ? static_cast<u8>(pack::ModelBlend::Alpha)
                     : d.blend;
    work.flags = 0;
    work.texture = r.texture;
    work.quadCapacity = std::min(r.count + 1, kRibbonEdges);
    if (alphaBlended(work.blend)) {
        work.key = static_cast<u64>(0xFFFFFFFFu - static_cast<u32>(std::min(distance, 60000.0f) * 64.0f));
    } else {
        work.key = (1ull << 63) | (static_cast<u64>(work.blend) << 56) | static_cast<u64>(work.texture.index);
    }
    m_work.push_back(work);
    instance.simulatedFrame = m_frame + 1;
}

u32 ParticleSystem::update(f32 dt, const ParticleView& view, JobSystem* jobs,
                           std::span<ParticleVertex> vertices, std::vector<ParticleDraw>& draws) {
    VD_PROFILE_ZONE("ParticleSystem::update");
    const Clock::time_point start = Clock::now();
    draws.clear();
    m_work.clear();
    m_stats.placed = static_cast<u32>(m_placed.size());
    m_stats.emitters = m_stats.culled = m_stats.ribbons = m_stats.particles = m_stats.quads = 0;
    m_stats.draws = m_stats.starved = m_stats.clipped = 0;
    m_dt = std::clamp(dt, 0.0f, 0.25f);
    m_right = view.right;
    m_up = view.up;
    m_forward = view.forward;

    // Where every emitter of the placed instances is, what it does this frame, and whether it is drawn.
    for (u32 index : m_placed) {
        Instance& instance = m_instances[index];
        if (!instance.alive) {
            continue;
        }
        for (u32 e = instance.firstEmitter; e != kNone; e = m_emitters[e].next) {
            prepareEmitter(instance, e, view, m_dt);
        }
        for (u32 r = instance.firstRibbon; r != kNone; r = m_ribbons[r].next) {
            prepareRibbon(instance, r, view, m_dt);
        }
    }

    const Clock::time_point prepared = Clock::now();
    m_stats.prepareMs = static_cast<f32>(secondsBetween(start, prepared) * 1000.0);
    // Draw order; each emitter gets a range of quads (an upper bound: what it does not fill stays empty).
    std::sort(m_work.begin(), m_work.end(), [](const Work& a, const Work& b) { return a.key < b.key; });
    const u32 maxQuads = static_cast<u32>(std::min(vertices.size(), m_scratch.size()) / 4);
    u32 quads = 0;
    for (Work& work : m_work) {
        if (quads + work.quadCapacity > maxQuads) {
            work.quadCapacity = 0; // simulated, not drawn this frame
            ++m_stats.clipped;
        }
        work.firstQuad = quads;
        quads += work.quadCapacity;
        if (work.quadCapacity == 0) {
            continue;
        }
        if (!draws.empty() && draws.back().texture.index == work.texture.index &&
            draws.back().blend == work.blend && draws.back().flags == work.flags &&
            draws.back().firstQuad + draws.back().quadCount == work.firstQuad) {
            draws.back().quadCount += work.quadCapacity;
        } else {
            draws.push_back({work.texture, work.blend, work.flags, work.firstQuad, work.quadCapacity});
        }
    }

    // Simulate and write the quads, on the workers when they are worth it.
    // Waking the workers costs more than a few hundred particles: only big frames go wide.
    const u32 count = static_cast<u32>(m_work.size());
    if (count > 0) {
        WorkFn fn{this};
        if (jobs && m_settings.workers && quads >= kParallelQuads && count > 1) {
            const u32 grain = std::max(1u, count / (4 * std::max(1u, jobs->workerCount())));
            jobs->parallelFor(count, grain, fn);
        } else {
            fn(0, count);
        }
    }

    if (quads > 0) {
        std::memcpy(vertices.data(), m_scratch.data(), u64{quads} * 4 * sizeof(ParticleVertex));
    }
    m_stats.simulateMs = static_cast<f32>(secondsBetween(prepared, Clock::now()) * 1000.0);
    // Instances that were stopped and have nothing left (or nothing anyone sees) go away.
    for (u32 index : m_placed) {
        Instance& instance = m_instances[index];
        if (instance.alive && instance.stopping &&
            (particleCount(index) == 0 || instance.simulatedFrame != m_frame + 1)) {
            releaseInstance(index);
        }
    }
    m_placed.clear();
    ++m_frame;

    for (const Work& work : m_work) {
        if (work.ribbon) {
            ++m_stats.ribbons;
            m_stats.particles += m_ribbons[work.index].count;
        } else {
            ++m_stats.emitters;
            m_stats.particles += m_emitters[work.index].count;
        }
    }
    m_stats.quads = quads;
    m_stats.draws = static_cast<u32>(draws.size());
    m_stats.updateMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
    // Over budget: emit less (the visible change is gradual); back to full rate when there is room again.
    if (m_stats.updateMs > m_settings.budgetMs) {
        m_rateScale = std::max(0.25f, m_rateScale * 0.95f);
    } else if (m_stats.updateMs < m_settings.budgetMs * 0.7f) {
        m_rateScale = std::min(1.0f, m_rateScale + 0.01f);
    }
    m_stats.rateScale = m_rateScale;
    return quads;
}

void ParticleSystem::runWork(u32 begin, u32 end) {
    VD_PROFILE_ZONE("ParticleSystem::runWork");
    for (u32 i = begin; i < end; ++i) {
        const Work& work = m_work[i];
        if (work.ribbon) {
            Ribbon& r = m_ribbons[work.index];
            simulateRibbon(r, m_dt);
            writeRibbon(r, work);
            continue;
        }
        Emitter& e = m_emitters[work.index];
        if (e.warm) {
            // As if it had been running: a few seconds in coarse steps.
            e.warm = false;
            const f32 life = std::max(e.values[pack::kEmitterLifespan], 0.0f);
            const u32 steps = std::min(30u, static_cast<u32>(std::min(life, kWarmSeconds) / kMaxStep));
            for (u32 s = 0; s < steps; ++s) {
                simulate(e, kMaxStep);
            }
        }
        f32 left = m_dt;
        while (left > kMaxStep) {
            simulate(e, kMaxStep);
            left -= kMaxStep;
        }
        simulate(e, left);
        writeQuads(e, work);
    }
}

void ParticleSystem::simulate(Emitter& e, f32 dt) {
    if (dt <= 0.0f) {
        return;
    }
    const pack::ParticleEmitter& d = *e.desc;
    Particle* ring = m_particles.data() + e.slab;
    // Age, dropping the oldest that died (all live about as long: the ring stays in spawn order).
    for (u32 i = 0; i < e.count; ++i) {
        Particle& p = ring[(e.head + i) & (e.capacity - 1)];
        p.age += dt;
    }
    while (e.count > 0 && ring[e.head].age >= ring[e.head].life) {
        e.head = (e.head + 1) & (e.capacity - 1);
        --e.count;
    }

    // New ones.
    e.emission += e.rate * dt;
    u32 spawnCount = static_cast<u32>(e.emission);
    e.emission -= static_cast<f32>(spawnCount);
    if (e.emission > 1.0f) {
        e.emission = 1.0f;
    }
    spawnCount = std::min(spawnCount, e.capacity - e.count);
    spawn(e, dt, spawnCount);

    // Forces: wind, gravity (straight down in the particles' space), drag.
    const glm::vec3 gravity(0.0f, 0.0f, -e.values[pack::kEmitterGravity]);
    const glm::vec3 drift = glm::vec3(d.wind[0], d.wind[1], d.wind[2]) * dt;
    const glm::vec3 fall = gravity * dt;
    const glm::vec3 fallPosition = gravity * (0.5f * dt * dt);
    const f32 keep = 1.0f - std::min(d.drag * dt, 1.0f);
    const bool modelSpace = (d.flags & pack::kParticleModelSpace) != 0;
    const bool follow = (d.flags & pack::kParticleFollow) != 0 && !modelSpace;
    const bool implode =
        (d.flags & pack::kParticleImplode) != 0 && d.shape == static_cast<u8>(pack::EmitterShape::Sphere);
    const glm::vec3 center = modelSpace ? glm::vec3(0.0f) : e.origin;
    for (u32 i = 0; i < e.count; ++i) {
        Particle& p = ring[(e.head + i) & (e.capacity - 1)];
        if (p.age >= p.life) {
            continue;
        }
        p.velocity += drift;
        if (follow && 2.0f * dt < p.age) {
            p.position += e.deltaPosition;
        }
        const glm::vec3 step = p.velocity * dt;
        p.velocity = (p.velocity + fall) * keep;
        p.position += step + fallPosition;
        if (implode && glm::dot(p.position - center, step) > 0.0f) {
            p.age = p.life; // passed the center
        }
    }
}

void ParticleSystem::spawn(Emitter& e, f32 dt, u32 count) {
    const pack::ParticleEmitter& d = *e.desc;
    Particle* ring = m_particles.data() + e.slab;
    const f32* v = e.values.data();
    const f32 life = std::max(v[pack::kEmitterLifespan], 0.001f);
    const bool modelSpace = (d.flags & pack::kParticleModelSpace) != 0;
    const glm::vec3 zSource(0.0f, 0.0f, v[pack::kEmitterZSource]);
    const std::span<const glm::vec3> spline =
        m_instances[e.instance].set->splinePoints().subspan(d.firstSplinePoint, d.splinePointCount);
    for (u32 n = 0; n < count; ++n) {
        Particle& p = ring[(e.head + e.count) & (e.capacity - 1)];
        ++e.count;
        p.life = life;
        p.age = std::fmod(dt * uniformPos(e.random), life);
        p.seed = nextRandom(e.random);
        p.reserved = 0.0f;
        const f32 speed =
            v[pack::kEmitterSpeed] * (1.0f + v[pack::kEmitterSpeedVariation] * uniform(e.random));
        glm::vec3 direction;
        if (d.shape == static_cast<u8>(pack::EmitterShape::Sphere)) {
            const f32 radius =
                v[pack::kEmitterAreaLength] +
                (v[pack::kEmitterAreaWidth] - v[pack::kEmitterAreaLength]) * uniformPos(e.random);
            const f32 polar = v[pack::kEmitterVerticalRange] * uniform(e.random);
            const f32 azimuth = v[pack::kEmitterHorizontalRange] * uniform(e.random);
            const f32 cosPolar = std::cos(polar);
            direction =
                glm::vec3(cosPolar * std::cos(azimuth), cosPolar * std::sin(azimuth), std::sin(polar));
            p.position = direction * radius;
            if (d.flags & pack::kParticleGoUp) {
                direction = glm::vec3(0.0f, 0.0f, 1.0f);
            }
        } else {
            if (d.shape == static_cast<u8>(pack::EmitterShape::Spline) && spline.size() >= 2) {
                const f32 t = uniformPos(e.random) * static_cast<f32>(spline.size() - 1);
                const u32 k = std::min(static_cast<u32>(t), static_cast<u32>(spline.size() - 2));
                p.position = spline[k] + (spline[k + 1] - spline[k]) * (t - static_cast<f32>(k));
            } else {
                p.position = glm::vec3(uniform(e.random) * v[pack::kEmitterAreaLength] * 0.5f,
                                       uniform(e.random) * v[pack::kEmitterAreaWidth] * 0.5f, 0.0f);
            }
            const f32 polar = v[pack::kEmitterVerticalRange] * uniform(e.random);
            const f32 azimuth = v[pack::kEmitterHorizontalRange] * uniform(e.random);
            const f32 sinPolar = std::sin(polar);
            direction =
                glm::vec3(std::cos(azimuth) * sinPolar, std::sin(azimuth) * sinPolar, std::cos(polar));
        }
        if (zSource.z >= 0.001f) {
            const glm::vec3 away = p.position - zSource;
            const f32 length = glm::length(away);
            direction = length > 1e-4f ? away / length : glm::vec3(0.0f, 0.0f, 1.0f);
        }
        p.velocity = direction * speed;
        if (!modelSpace) {
            p.position = anim::transformPoint(e.toAnchor, p.position);
            p.velocity = rotate(e.toAnchor, p.velocity);
        }
        if (d.flags & pack::kParticleBurst) {
            p.velocity += e.burst * (1.0f + v[pack::kEmitterSpeedVariation] * uniform(e.random));
        }
    }
}

void ParticleSystem::writeQuads(const Emitter& e, const Work& work) {
    if (work.quadCapacity == 0) {
        return;
    }
    if (const pack::ParticleMesh* mesh = m_instances[e.instance].set->mesh(e.index)) {
        writeMeshParticles(e, work, *mesh);
        return;
    }
    const pack::ParticleEmitter& d = *e.desc;
    ParticleVertex* out = m_scratch.data() + u64{work.firstQuad} * 4;
    u32 written = 0;
    const Particle* ring = m_particles.data() + e.slab;
    const glm::vec4 colors[3] = {unpackColor(d.colors[0]), unpackColor(d.colors[1]),
                                 unpackColor(d.colors[2])};
    const u32 cells = u32{d.rows} * d.columns;
    const f32 cellU = 1.0f / static_cast<f32>(d.columns);
    const f32 cellV = 1.0f / static_cast<f32>(d.rows);
    const bool modelSpace = (d.flags & pack::kParticleModelSpace) != 0;
    const f32 sizeScale = (d.flags & pack::kParticleInheritScale) ? e.scale : 1.0f;
    const bool head = d.style != static_cast<u8>(pack::ParticleStyle::Tail);
    const bool tail = d.style != static_cast<u8>(pack::ParticleStyle::Head);
    const bool spins = d.spin != 0.0f;
    const bool xyQuad = (d.flags & pack::kParticleXYQuad) != 0;
    const bool velocityOrient = (d.flags & pack::kParticleVelocityOrient) != 0;
    const bool twinkles = d.twinklePercent < 1.0f || d.twinkleMin != d.twinkleMax;
    // XY quads lie in the particles' own plane: the world's, or the emitter's (scale removed).
    glm::vec3 planeX(1.0f, 0.0f, 0.0f), planeY(0.0f, 1.0f, 0.0f);
    if (xyQuad && modelSpace) {
        planeX = column(e.toAnchor, 0) / std::max(e.scale, 1e-6f);
        planeY = column(e.toAnchor, 1) / std::max(e.scale, 1e-6f);
    }
    for (u32 i = 0; i < e.count && written < work.quadCapacity; ++i) {
        const Particle& p = ring[(e.head + i) & (e.capacity - 1)];
        if (p.age >= p.life) {
            continue;
        }
        const f32 t = std::clamp(p.age / p.life, 0.0f, 1.0f);
        f32 size = lifeRamp(t, d.midPoint, d.scales[0], d.scales[1], d.scales[2]) * sizeScale;
        if (twinkles) {
            const f32 r = kRandTable.values[(static_cast<u32>(p.age * d.twinkleSpeed) + p.seed) & 0x7F];
            if (d.twinklePercent < r) {
                continue;
            }
            size *= d.twinkleMin + (d.twinkleMax - d.twinkleMin) * r;
        }
        if (size <= 0.0f) {
            continue;
        }
        glm::vec4 color = lifeRamp(t, d.midPoint, colors[0], colors[1], colors[2]);
        if (color.w <= 0.002f) {
            continue;
        }
        const glm::vec3 position = modelSpace ? anim::transformPoint(e.toAnchor, p.position) : p.position;
        const glm::vec3 velocity = modelSpace ? rotate(e.toAnchor, p.velocity) : p.velocity;
        const glm::vec3 center = e.cameraOffset + position;
        // Near the camera a particle fades out: a quad around the eye would cover the screen (overdraw on a
        // tiled GPU) and look like a flat sheet anyway.
        const f32 nearDistance = size * kNearFade;
        const f32 distance2 = glm::dot(center, center);
        if (distance2 < nearDistance * nearDistance) {
            const f32 f = std::sqrt(distance2) / nearDistance;
            color.w *= f * f;
            if (color.w <= 0.002f) {
                continue;
            }
        }
        const u32 packed = packColor(color);
        if (head) {
            u32 cell = 0;
            if (cells > 1) {
                cell = (d.flags & pack::kParticleRandomCell) ? p.seed % cells : headCell(d, t, cells);
            }
            const f32 u0 = static_cast<f32>(cell % d.columns) * cellU;
            const f32 v0 = static_cast<f32>(cell / d.columns) * cellV;
            f32 theta = spins ? d.spin * p.age : 0.0f;
            if ((d.flags & pack::kParticleRandomSpinSign) && (p.seed & 1u)) {
                theta = -theta;
            }
            glm::vec3 axisX = m_right, axisY = m_up;
            if (xyQuad) {
                axisX = planeX;
                axisY = planeY;
                if (velocityOrient) {
                    // Turned in its plane to point backwards along the velocity.
                    const glm::vec2 flat(-glm::dot(velocity, planeX), -glm::dot(velocity, planeY));
                    const f32 length = glm::length(flat);
                    if (length > 1e-4f) {
                        const glm::vec2 dir = flat / length;
                        axisX = planeX * dir.x + planeY * dir.y;
                        axisY = -planeX * dir.y + planeY * dir.x;
                    }
                }
            } else if (velocityOrient && glm::dot(velocity, velocity) > 2.4e-7f) {
                // Stretched along the velocity as seen on screen.
                glm::vec3 back = -velocity - m_forward * glm::dot(-velocity, m_forward);
                const f32 length = glm::length(back);
                if (length > 1e-5f) {
                    axisX = back / length;
                    axisY = glm::cross(m_forward, axisX);
                }
            }
            glm::vec3 m0 = axisX * size;
            glm::vec3 m1 = axisY * size;
            if (spins) {
                const f32 c = std::cos(theta), s = std::sin(theta);
                m0 = (axisX * c + axisY * s) * size;
                m1 = (axisY * c - axisX * s) * size;
            }
            writeQuad(out + written * 4, center, m0, m1, u0, v0, u0 + cellU, v0 + cellV, packed);
            ++written;
        }
        if (tail && written < work.quadCapacity) {
            u32 cell = 0;
            if (cells > 1) {
                cell = tailCell(d, t, cells);
            }
            const f32 u0 = static_cast<f32>(cell % d.columns) * cellU;
            const f32 v0 = static_cast<f32>(cell / d.columns) * cellV;
            f32 trail = d.tailLength;
            if (d.flags & pack::kParticleClampTail) {
                trail = std::min(trail, p.age);
            }
            const glm::vec3 behind = -velocity * trail;
            const glm::vec3 side = glm::cross(m_forward, behind);
            const f32 sideLength = glm::length(side);
            glm::vec3 m0, m1;
            glm::vec3 middle = center;
            if (sideLength > 0.01f) {
                // From the particle back along its path, as wide as its size.
                m0 = behind * 0.5f;
                m1 = side * (size / sideLength);
                middle += m0;
            } else {
                m0 = m_right * (size * 0.05f);
                m1 = m_up * (size * 0.05f);
            }
            writeQuad(out + written * 4, middle, m0, m1, u0, v0, u0 + cellU, v0 + cellV, packed);
            ++written;
        }
    }
    writeEmptyQuads(out + written * 4, work.quadCapacity - written);
}

void ParticleSystem::writeMeshParticles(const Emitter& e, const Work& work, const pack::ParticleMesh& mesh) {
    const pack::ParticleEmitter& d = *e.desc;
    const std::span<const pack::ParticleMeshVertex> corners = m_instances[e.instance].set->meshVertices(mesh);
    ParticleVertex* out = m_scratch.data() + u64{work.firstQuad} * 4;
    u32 written = 0;
    const Particle* ring = m_particles.data() + e.slab;
    const glm::vec4 colors[3] = {unpackColor(d.colors[0]), unpackColor(d.colors[1]),
                                 unpackColor(d.colors[2])};
    const bool modelSpace = (d.flags & pack::kParticleModelSpace) != 0;
    const f32 sizeScale = (d.flags & pack::kParticleInheritScale) ? e.scale : 1.0f;
    const bool twinkles = d.twinklePercent < 1.0f || d.twinkleMin != d.twinkleMax;
    const bool randomSigns = (d.flags & pack::kParticleMeshRandomSigns) != 0;
    for (u32 i = 0; i < e.count && written + mesh.triangleCount <= work.quadCapacity; ++i) {
        const Particle& p = ring[(e.head + i) & (e.capacity - 1)];
        if (p.age >= p.life) {
            continue;
        }
        // Size and color over its life as a quad's (WoW.exe 0x7B4510 scales the model by them and colors it).
        const f32 t = std::clamp(p.age / p.life, 0.0f, 1.0f);
        f32 size = lifeRamp(t, d.midPoint, d.scales[0], d.scales[1], d.scales[2]) * sizeScale;
        if (twinkles) {
            const f32 r = kRandTable.values[(static_cast<u32>(p.age * d.twinkleSpeed) + p.seed) & 0x7F];
            if (d.twinklePercent < r) {
                continue;
            }
            size *= d.twinkleMin + (d.twinkleMax - d.twinkleMin) * r;
        }
        const glm::vec4 color = lifeRamp(t, d.midPoint, colors[0], colors[1], colors[2]);
        if (size <= 0.0f || color.w <= 0.002f) {
            continue;
        }
        const glm::vec3 position = modelSpace ? anim::transformPoint(e.toAnchor, p.position) : p.position;
        const glm::vec3 center = e.cameraOffset + position;
        // A random start and a random tumble rate between the mesh's limits, from the particle's seed (the
        // client keeps a quaternion and an angular velocity per particle, 0x7B2540).
        u32 random = p.seed | 1u;
        const f32 u1 = uniformPos(random);
        const f32 u2 = uniformPos(random) * 6.2831853f, u3 = uniformPos(random) * 6.2831853f;
        const glm::quat start(std::sqrt(u1) * std::cos(u3), std::sqrt(1.0f - u1) * std::sin(u2),
                              std::sqrt(1.0f - u1) * std::cos(u2), std::sqrt(u1) * std::sin(u3));
        glm::vec3 spin;
        for (u32 c = 0; c < 3; ++c) {
            spin[c] = mesh.tumbleMin[c] + (mesh.tumbleMax[c] - mesh.tumbleMin[c]) * uniformPos(random);
            if (randomSigns && (nextRandom(random) & 1u)) {
                spin[c] = -spin[c];
            }
        }
        const f32 rate = glm::length(spin);
        const glm::quat turned =
            rate * p.age > 1e-5f ? start * glm::angleAxis(rate * p.age, spin / rate) : start;
        const glm::mat3 m = glm::mat3_cast(glm::normalize(turned)) * size;
        const u32 packed = packColor(color);
        for (u32 k = 0; k < mesh.triangleCount; ++k) {
            ParticleVertex* dst = out + (written + k) * 4;
            for (u32 c = 0; c < 3; ++c) {
                const pack::ParticleMeshVertex& v = corners[k * 3 + c];
                const glm::vec3 at = center + m * glm::vec3(v.position[0], v.position[1], v.position[2]);
                dst[c] = {{at.x, at.y, at.z}, {unorm16(v.uv[0]), unorm16(v.uv[1])}, packed};
            }
            dst[3] = dst[2]; // the quad's second triangle collapses
        }
        written += mesh.triangleCount;
    }
    writeEmptyQuads(out + written * 4, work.quadCapacity - written);
}

void ParticleSystem::simulateRibbon(Ribbon& r, f32 dt) {
    const pack::RibbonEmitter& d = *r.desc;
    // Age and fall; the oldest drop off the tail.
    for (u32 i = 0; i < r.count; ++i) {
        Ribbon::Edge& edge = r.edges[(r.head + i) % kRibbonEdges];
        edge.age += dt;
        edge.fall += d.gravity * dt;
        edge.top.z -= edge.fall * dt;
        edge.bottom.z -= edge.fall * dt;
    }
    while (r.count > 0 && r.edges[r.head].age >= d.edgeLifetime) {
        r.head = (r.head + 1) % kRibbonEdges;
        --r.count;
    }
    if (!r.emitting) {
        return;
    }
    // The newest edge follows the bone; a new one is left behind every 1 / edgesPerSecond seconds (fewer when
    // the ribbon's life would need more edges than it keeps).
    const f32 spacing =
        std::max(1.0f / d.edgesPerSecond, d.edgeLifetime / static_cast<f32>(kRibbonEdges - 2));
    r.sinceEdge += dt;
    if (r.count == 0 || r.sinceEdge >= spacing) {
        if (r.count == kRibbonEdges) {
            r.head = (r.head + 1) % kRibbonEdges;
            --r.count;
        }
        r.sinceEdge = r.count == 0 ? 0.0f : r.sinceEdge - spacing;
        ++r.count;
    }
    Ribbon::Edge& newest = r.edges[(r.head + r.count - 1) % kRibbonEdges];
    newest = {r.top, 0.0f, r.bottom, 0.0f};
    if (r.count == 1) {
        // A second point to draw from as soon as it moves.
        r.head = (r.head + kRibbonEdges - 1) % kRibbonEdges;
        r.edges[r.head] = newest;
        ++r.count;
    }
}

void ParticleSystem::writeRibbon(const Ribbon& r, const Work& work) {
    if (work.quadCapacity == 0) {
        return;
    }
    const pack::RibbonEmitter& d = *r.desc;
    ParticleVertex* out = m_scratch.data() + u64{work.firstQuad} * 4;
    glm::vec4 color = unpackColor(d.color);
    color.w *= r.alpha;
    const u32 packed = packColor(color);
    const f32 inverseLife = 1.0f / d.edgeLifetime;
    u32 written = 0;
    for (u32 i = 0; i + 1 < r.count && written < work.quadCapacity; ++i) {
        const Ribbon::Edge& a = r.edges[(r.head + i) % kRibbonEdges];
        const Ribbon::Edge& b = r.edges[(r.head + i + 1) % kRibbonEdges];
        const u16 ua = unorm16(1.0f - a.age * inverseLife);
        const u16 ub = unorm16(1.0f - b.age * inverseLife);
        const glm::vec3 at = r.cameraOffset + a.top, ab = r.cameraOffset + a.bottom;
        const glm::vec3 bt = r.cameraOffset + b.top, bb = r.cameraOffset + b.bottom;
        ParticleVertex* dst = out + written * 4;
        dst[0] = {{at.x, at.y, at.z}, {ua, 0}, packed};
        dst[1] = {{ab.x, ab.y, ab.z}, {ua, 65535}, packed};
        dst[2] = {{bt.x, bt.y, bt.z}, {ub, 0}, packed};
        dst[3] = {{bb.x, bb.y, bb.z}, {ub, 65535}, packed};
        ++written;
    }
    writeEmptyQuads(out + written * 4, work.quadCapacity - written);
}

u32 ParticleSystem::debugParticles(u32 instance, u32 emitter, std::span<ParticleState> out) const {
    if (!alive(instance)) {
        return 0;
    }
    u32 n = 0;
    for (u32 e = m_instances[instance].firstEmitter; e != kNone; e = m_emitters[e].next, ++n) {
        if (n != emitter) {
            continue;
        }
        const Emitter& em = m_emitters[e];
        u32 written = 0;
        for (u32 i = 0; i < em.count && written < out.size(); ++i) {
            const Particle& p = m_particles[em.slab + ((em.head + i) & (em.capacity - 1))];
            out[written++] = {p.position, p.velocity, p.age, p.life};
        }
        return written;
    }
    return 0;
}

} // namespace vd::fx
