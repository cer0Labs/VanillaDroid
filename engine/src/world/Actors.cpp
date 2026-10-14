#include "world/Actors.h"

#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "db/DbcView.h"
#include "fx/ParticleSystem.h"
#include "jobs/JobSystem.h"
#include "pack/ActorFormat.h"
#include "world/PortraitCache.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vd {
namespace {

constexpr u64 kTagModel = 1ull << 32;
constexpr u64 kTagTexture = 2ull << 32;
constexpr u32 kMaxUploadsPerFrame = 16;
constexpr u64 kUploadBytesPerFrame = 8ull << 20; // at least one asset per frame regardless
constexpr f64 kUploadBudgetMs = 1.5;
constexpr u32 kMaxComposesPerFrame = 4;
constexpr u32 kPaletteSlackBones = 256;
// Phase 9: unused spell effect models stay loaded this many frames (a minute at 60 fps), at most this many.
constexpr u32 kIdleModelFrames = 3600;
constexpr usize kMaxIdleModels = 160;
// TextureEntry::waiting entries of effect textures (phase 9) carry this bit with the slot, not a material.
constexpr u32 kEffectTexture = 0x80000000u;

// Rotation about +Z by yaw, uniform scale, translation.
anim::BoneMatrix placement(const glm::vec3& translation, f32 yaw, f32 scale) {
    const f32 c = std::cos(yaw) * scale;
    const f32 s = std::sin(yaw) * scale;
    return anim::BoneMatrix{{c, -s, 0.0f, translation.x, s, c, 0.0f, translation.y, 0.0f, 0.0f, scale, translation.z}};
}

anim::BoneMatrix translation(const glm::vec3& t) {
    return anim::BoneMatrix{{1.0f, 0.0f, 0.0f, t.x, 0.0f, 1.0f, 0.0f, t.y, 0.0f, 0.0f, 1.0f, t.z}};
}

// Removes the scale of the rotation part, then applies `scale`.
anim::BoneMatrix rescale(anim::BoneMatrix m, f32 scale) {
    for (u32 column = 0; column < 3; ++column) {
        const f32 length = std::sqrt(m.rows[column] * m.rows[column] + m.rows[4 + column] * m.rows[4 + column] +
                                     m.rows[8 + column] * m.rows[8 + column]);
        const f32 k = length > 1e-6f ? scale / length : scale;
        m.rows[column] *= k;
        m.rows[4 + column] *= k;
        m.rows[8 + column] *= k;
    }
    return m;
}

ActorInstanceData instanceOf(const anim::BoneMatrix& transform, u32 boneBase, const std::array<u32, 2>& light = {}) {
    ActorInstanceData data{};
    std::memcpy(data.rows, transform.rows, sizeof(data.rows));
    data.boneBase = boneBase;
    data.light[0] = light[0];
    data.light[1] = light[1];
    return data;
}

u64 skinKeyOf(const std::vector<game::SkinLayer>& layers) {
    u64 hash = kFnvOffset;
    for (const game::SkinLayer& layer : layers) {
        hash = fnv1a64(&layer.texture, sizeof(layer.texture), hash);
        const u8 region = static_cast<u8>(layer.region);
        hash = fnv1a64(&region, 1, hash);
    }
    return hash == 0 ? 1 : hash;
}

} // namespace

Actors::~Actors() {
    m_streamer.stop();
}

Result<void> Actors::open(const std::filesystem::path& packsDir, const pack::PackReader& db,
                          const pack::PackReader& textures, Renderer& renderer) {
    close(renderer);
    VD_TRY(m_models.open(packsDir / "actors.vdpk"));
    if (Result<void> appearance = m_appearance.init(db, textures, m_models); !appearance) {
        m_models.close();
        return appearance.error();
    }
    if (const std::optional<pack::PackAsset> table = db.find("DBFilesClient\\AnimationData.dbc")) {
        if (Result<db::DbcView> view = db::DbcView::parse(table->data, table->size); view) {
            m_fallbacks.load(view.value());
        }
    }
    Result<StagingBlock> staging = renderer.createStagingBlock(kStagingBytes);
    if (!staging) {
        m_models.close();
        return staging.error();
    }
    m_staging = staging.value();
    m_textures = &textures;
    m_renderer = &renderer;
    m_actors.reserve(2048);
    m_modelEntries.reserve(1024);
    m_textureEntries.reserve(4096);
    m_visible.reserve(kMaxVisible);
    m_eventHits.reserve(kMaxEvents);
    m_ordered.reserve(kMaxVisible);
    m_stillBones.reserve(anim::kMaxBones);
    m_stillItemBones.reserve(anim::kMaxBones);
    m_pendingLooks.reserve(2048);
    m_streamer.start(m_staging, "vd-actors");
    VD_LOG_INFO("Actors: %u models in the pack", m_models.entryCount());
    return {};
}

void Actors::close(Renderer& renderer) {
    if (!isOpen()) {
        return;
    }
    m_streamer.stop();
    for (AssetStreamer::Done& done : m_waiting) {
        delete[] done.data;
    }
    m_waiting.clear();
    for (ModelEntry& model : m_modelEntries) {
        if (model.handle.valid()) {
            renderer.destroyModel(model.handle);
        }
        delete[] model.blob;
    }
    for (TextureEntry& texture : m_textureEntries) {
        if (texture.handle.valid()) {
            renderer.destroyTexture(texture.handle);
        }
    }
    for (auto& [key, skin] : m_skins) {
        renderer.destroyTexture(skin.handle);
    }
    m_skins.clear();
    for (auto& [key, mesh] : m_meshes) {
        renderer.destroyActorMesh(mesh.handle);
    }
    m_meshes.clear();
    m_modelEntries.clear();
    m_freeModels.clear();
    m_modelIndex.clear();
    m_textureEntries.clear();
    m_freeTextures.clear();
    m_textureIndex.clear();
    m_requestQueue.clear();
    m_actors.clear();
    m_freeActors.clear();
    m_pendingLooks.clear();
    m_idleModels.clear();
    m_preloads.clear();
    renderer.destroyStagingBlock(m_staging);
    m_models.close();
    m_textures = nullptr;
    m_renderer = nullptr;
    m_stats = {};
}

// --- Assets -------------------------------------------------------------------------------------------------

u32 Actors::acquireModel(u64 hash) {
    if (const auto it = m_modelIndex.find(hash); it != m_modelIndex.end()) {
        ++m_modelEntries[it->second].refs;
        return it->second;
    }
    u32 index = 0;
    if (!m_freeModels.empty()) {
        index = m_freeModels.back();
        m_freeModels.pop_back();
    } else {
        index = static_cast<u32>(m_modelEntries.size());
        m_modelEntries.emplace_back();
    }
    ModelEntry& entry = m_modelEntries[index];
    entry = ModelEntry{};
    entry.hash = hash;
    entry.refs = 1;
    m_modelIndex.emplace(hash, index);
    m_requestQueue.push_back(kTagModel | index);
    return index;
}

void Actors::releaseModel(u32 index) {
    if (index == kNone) {
        return;
    }
    ModelEntry& entry = m_modelEntries[index];
    if (--entry.refs > 0 || entry.state == State::Requested) {
        return; // still used, or still loading: freed when its IO result arrives
    }
    if (entry.cached && entry.state == State::Ready) {
        entry.idleFrame = m_frame; // kept a while (evictIdleModels)
        if (!entry.idle) {
            entry.idle = true;
            m_idleModels.push_back(index);
        }
        return;
    }
    freeModel(index);
}

void Actors::preload(u64 model) {
    if (!isOpen() || model == 0) {
        return;
    }
    const u32 index = acquireModel(model);
    m_modelEntries[index].cached = true;
    m_preloads.push_back(index);
}

void Actors::evictIdleModels() {
    // Used again: off the list; unused for too long: freed.
    for (usize i = 0; i < m_idleModels.size();) {
        const u32 index = m_idleModels[i];
        ModelEntry& entry = m_modelEntries[index];
        const bool revived = entry.refs > 0;
        const bool stale = !revived && m_frame - entry.idleFrame > kIdleModelFrames;
        if (!revived && !stale) {
            ++i;
            continue;
        }
        entry.idle = false;
        m_idleModels[i] = m_idleModels.back();
        m_idleModels.pop_back();
        if (stale) {
            freeModel(index);
        }
    }
    // Too many: the least recently used go first.
    while (m_idleModels.size() > kMaxIdleModels) {
        usize oldest = 0;
        for (usize i = 1; i < m_idleModels.size(); ++i) {
            if (static_cast<i32>(m_modelEntries[m_idleModels[i]].idleFrame -
                                 m_modelEntries[m_idleModels[oldest]].idleFrame) < 0) {
                oldest = i;
            }
        }
        const u32 index = m_idleModels[oldest];
        m_modelEntries[index].idle = false;
        m_idleModels[oldest] = m_idleModels.back();
        m_idleModels.pop_back();
        freeModel(index);
    }
}

u32 Actors::trimIdleModels() {
    // Phase 10 (memory pressure): every cached effect model nothing uses now goes; the next cast streams it again.
    u32 freed = 0;
    for (const u32 index : m_idleModels) {
        ModelEntry& entry = m_modelEntries[index];
        entry.idle = false;
        if (entry.refs == 0) {
            freeModel(index);
            ++freed;
        }
    }
    m_idleModels.clear();
    return freed;
}

void Actors::freeModel(u32 index) {
    ModelEntry& entry = m_modelEntries[index];
    if (entry.handle.valid()) {
        m_renderer->destroyModel(entry.handle);
    }
    // Textures still loading forget it: the slot may hold another model when they arrive.
    const auto forget = [&](u32 texture) {
        if (texture != kNone) {
            std::vector<std::pair<u32, u32>>& waiting = m_textureEntries[texture].waiting;
            waiting.erase(std::remove_if(waiting.begin(), waiting.end(), [&](const auto& w) { return w.first == index; }),
                          waiting.end());
        }
    };
    for (u32 texture : entry.textures) {
        forget(texture);
    }
    for (u32 texture : entry.effectTextures) {
        forget(texture);
    }
    for (u32 texture : entry.textures) {
        if (texture != kNone) {
            releaseTexture(texture);
        }
    }
    for (u32 texture : entry.effectTextures) {
        releaseTexture(texture);
    }
    delete[] entry.blob;
    m_modelIndex.erase(entry.hash);
    entry = ModelEntry{};
    m_freeModels.push_back(index);
}

u32 Actors::acquireTexture(u64 hash) {
    if (hash == 0) {
        return kNone;
    }
    if (const auto it = m_textureIndex.find(hash); it != m_textureIndex.end()) {
        ++m_textureEntries[it->second].refs;
        return it->second;
    }
    u32 index = 0;
    if (!m_freeTextures.empty()) {
        index = m_freeTextures.back();
        m_freeTextures.pop_back();
    } else {
        index = static_cast<u32>(m_textureEntries.size());
        m_textureEntries.emplace_back();
    }
    TextureEntry& entry = m_textureEntries[index];
    entry = TextureEntry{};
    entry.hash = hash;
    entry.refs = 1;
    m_textureIndex.emplace(hash, index);
    m_requestQueue.push_back(kTagTexture | index);
    return index;
}

void Actors::releaseTexture(u32 index) {
    if (index == kNone) {
        return;
    }
    TextureEntry& entry = m_textureEntries[index];
    if (--entry.refs > 0 || entry.state == State::Requested) {
        return;
    }
    if (entry.handle.valid()) {
        m_renderer->destroyTexture(entry.handle);
    }
    m_textureIndex.erase(entry.hash);
    entry = TextureEntry{};
    m_freeTextures.push_back(index);
}

void Actors::requestPending() {
    usize done = 0;
    for (; done < m_requestQueue.size(); ++done) {
        const u64 tag = m_requestQueue[done];
        const u32 index = static_cast<u32>(tag);
        std::optional<pack::PackAsset> asset;
        if (tag & kTagModel) {
            ModelEntry& entry = m_modelEntries[index];
            asset = m_models.find(entry.hash);
            if (!asset || asset->type != pack::AssetType::ActorModel) {
                entry.state = State::Failed;
                if (entry.refs == 0) {
                    ++entry.refs;
                    releaseModel(index);
                }
                continue;
            }
        } else {
            TextureEntry& entry = m_textureEntries[index];
            asset = m_textures->find(entry.hash);
            if (!asset || asset->type != pack::AssetType::Texture) {
                entry.state = State::Failed;
                for (const auto& [model, material] : entry.waiting) {
                    (void)material;
                    --m_modelEntries[model].pendingTextures;
                }
                entry.waiting.clear();
                if (entry.refs == 0) {
                    ++entry.refs;
                    releaseTexture(index);
                }
                continue;
            }
        }
        if (!m_streamer.request(tag, *asset)) {
            break; // queue full: the rest waits for the next frame
        }
    }
    m_requestQueue.erase(m_requestQueue.begin(), m_requestQueue.begin() + static_cast<std::ptrdiff_t>(done));
}

void Actors::onModelReady(u32 index, AssetStreamer::Done& done, Renderer& renderer) {
    ModelEntry& entry = m_modelEntries[index];
    if (Result<void> bound = entry.skeleton.bind(done.data, done.size); !bound) {
        VD_LOG_ERROR("Actor model %016llx: %s", static_cast<unsigned long long>(entry.hash),
                     bound.error().message.c_str());
        entry.state = State::Failed;
        return;
    }
    Result<ModelHandle> handle = renderer.createActorModel(m_staging, done.stagingOffset, done.data, done.size);
    if (!handle) {
        VD_LOG_ERROR("Actor model %016llx: %s", static_cast<unsigned long long>(entry.hash),
                     handle.error().message.c_str());
        entry.skeleton = anim::Skeleton{};
        entry.state = State::Failed;
        return;
    }
    entry.handle = handle.value();
    entry.blob = done.data; // kept: the skeleton points into it
    entry.size = done.size;
    done.data = nullptr;
    entry.state = State::Ready;
    const pack::ActorModelHeader& header = entry.skeleton.header();
    const glm::vec3 min(header.boundsMin[0], header.boundsMin[1], header.boundsMin[2]);
    const glm::vec3 max(header.boundsMax[0], header.boundsMax[1], header.boundsMax[2]);
    entry.center = (min + max) * 0.5f;
    entry.radius = std::max(0.5f, header.radius);
    entry.top = std::max(max.z, 0.5f);
    const auto* geosets = reinterpret_cast<const u16*>(entry.blob + header.geosetsOffset);
    if (header.geosetsOffset + u64{header.geosetCount} * 2 <= entry.size) {
        entry.geosets.assign(geosets, geosets + header.geosetCount);
    }
    const std::span<const pack::ActorMaterial> materials = entry.skeleton.materials();
    entry.textures.assign(materials.size(), kNone);
    for (u32 m = 0; m < materials.size(); ++m) {
        entry.animatedColors |= materials[m].colorTrack != pack::kActorNoColorTrack ||
                                materials[m].alphaTrack != pack::kActorNoColorTrack ||
                                materials[m].transparencyTrack != pack::kActorNoColorTrack;
        if (materials[m].slot != static_cast<u8>(pack::ActorTextureSlot::File) || materials[m].texture == 0) {
            continue;
        }
        const u32 texture = acquireTexture(materials[m].texture);
        m_modelEntries[index].textures[m] = texture;
        TextureEntry& t = m_textureEntries[texture];
        if (t.state == State::Ready) {
            renderer.setModelTexture(m_modelEntries[index].handle, m, t.handle);
        } else if (t.state == State::Requested) {
            t.waiting.emplace_back(index, m);
            ++m_modelEntries[index].pendingTextures;
        }
    }

    // Phase 9: particles, ribbons, texture animations. Their textures load with the model.
    ModelEntry& model = m_modelEntries[index];
    auto effects = std::make_unique<fx::EffectSet>();
    if (Result<void> bound =
            effects->bindActor(model.blob, model.size, model.skeleton.tracks(), model.skeleton.boneCount());
        !bound) {
        VD_LOG_ERROR("Actor model %016llx effects: %s", static_cast<unsigned long long>(model.hash),
                     bound.error().message.c_str());
        return;
    }
    if (!effects->valid()) {
        return;
    }
    model.radius = std::max(model.radius, effects->reach());
    const usize emitters = effects->emitters().size();
    std::vector<u32> textures(emitters + effects->ribbons().size(), kNone);
    for (usize slot = 0; slot < textures.size(); ++slot) {
        const u64 hash =
            slot < emitters ? effects->emitters()[slot].texture : effects->ribbons()[slot - emitters].texture;
        const u32 texture = acquireTexture(hash);
        textures[slot] = texture;
        if (texture != kNone && m_textureEntries[texture].state == State::Requested) {
            m_textureEntries[texture].waiting.emplace_back(index, kEffectTexture | static_cast<u32>(slot));
            ++m_modelEntries[index].pendingTextures;
        }
    }
    m_modelEntries[index].effectTextures = std::move(textures);
    m_modelEntries[index].effects = std::move(effects);
}

void Actors::onTextureReady(u32 index, const AssetStreamer::Done& done, Renderer& renderer) {
    TextureEntry& entry = m_textureEntries[index];
    Result<TextureHandle> handle = renderer.createTexture(m_staging, done.stagingOffset, done.data, done.size);
    if (handle) {
        entry.handle = handle.value();
        entry.state = State::Ready;
    } else {
        VD_LOG_ERROR("Actor texture %016llx: %s", static_cast<unsigned long long>(entry.hash),
                     handle.error().message.c_str());
        entry.state = State::Failed;
    }
    for (const auto& [model, material] : entry.waiting) {
        ModelEntry& m = m_modelEntries[model];
        if (m.state == State::Ready && (m.refs > 0 || m.idle)) { // phase 9: or kept in the effect model cache
            if (entry.state == State::Ready && (material & kEffectTexture) == 0) {
                renderer.setModelTexture(m.handle, material, entry.handle);
            }
            --m.pendingTextures;
        }
    }
    entry.waiting.clear();
}

// --- Actors -------------------------------------------------------------------------------------------------

void Actors::resetActor(Actor& actor) {
    // Phase 9: spell effects come and go many times a second: the per-frame arrays keep their memory.
    std::vector<anim::BoneMatrix> bones = std::move(actor.bones);
    std::vector<glm::vec4> colors = std::move(actor.colors);
    std::vector<UvTransform> uvTransforms = std::move(actor.uvTransforms);
    actor = Actor{};
    bones.clear();
    colors.clear();
    uvTransforms.clear();
    actor.bones = std::move(bones);
    actor.colors = std::move(colors);
    actor.uvTransforms = std::move(uvTransforms);
}

u32 Actors::create(bool effect) {
    u32 index = 0;
    if (!m_freeActors.empty()) {
        index = m_freeActors.back();
        m_freeActors.pop_back();
    } else {
        index = static_cast<u32>(m_actors.size());
        m_actors.emplace_back();
    }
    Actor& actor = m_actors[index];
    resetActor(actor);
    actor.alive = true;
    actor.effect = effect;
    actor.slotTextures.fill(kNone);
    actor.random = 0x9E3779B9u ^ (index * 2654435761u);
    if (actor.random == 0) {
        actor.random = 1;
    }
    ++m_stats.actors;
    return index;
}

void Actors::releaseLook(Actor& actor) {
    releaseModel(actor.model);
    actor.model = kNone;
    for (u32& texture : actor.slotTextures) {
        releaseTexture(texture);
        texture = kNone;
    }
    for (u32 texture : actor.skinLayers) {
        releaseTexture(texture);
    }
    actor.skinLayers.clear();
    for (Attached& a : actor.attachments) {
        releaseModel(a.model);
        releaseTexture(a.texture);
    }
    actor.attachments.clear();
    actor.geosetIndices.clear();
    actor.geosetsResolved = false;
    actor.skinKey = 0;
}

void Actors::releaseShown(Actor::Shown& shown) {
    releaseModel(shown.model);
    shown.model = kNone;
    for (u32& texture : shown.slotTextures) {
        releaseTexture(texture);
        texture = kNone;
    }
    for (TextureHandle& handle : shown.textures) {
        handle = {};
    }
    if (m_particles) {
        m_particles->destroy(shown.fx);
    }
    shown.fx = kNone;
    for (Attached& a : shown.attachments) {
        if (m_particles) {
            m_particles->destroy(a.fx);
        }
        releaseModel(a.model);
        releaseTexture(a.texture);
    }
    shown.attachments.clear();
    shown.geosetIndices.clear();
    if (shown.skinKey != 0) {
        if (const auto it = m_skins.find(shown.skinKey); it != m_skins.end() && --it->second.refs == 0) {
            m_renderer->destroyTexture(it->second.handle);
            m_skins.erase(it);
        }
        shown.skinKey = 0;
    }
    if (shown.meshKey != 0) {
        if (const auto it = m_meshes.find(shown.meshKey); it != m_meshes.end() && --it->second.refs == 0) {
            m_renderer->destroyActorMesh(it->second.handle);
            m_meshes.erase(it);
        }
        shown.meshKey = 0;
        shown.mesh = {};
    }
}

void Actors::destroy(u32 index) {
    if (index >= m_actors.size() || !m_actors[index].alive) {
        return;
    }
    Actor& actor = m_actors[index];
    releaseLook(actor);
    releaseShown(actor.shown);
    for (Actor& other : m_actors) {
        if (other.alive && other.mount == index) {
            other.mount = kNone;
        }
    }
    resetActor(actor);
    m_freeActors.push_back(index);
    --m_stats.actors;
}

void Actors::setLook(u32 index, const game::ActorLook& look) {
    if (index >= m_actors.size() || !m_actors[index].alive) {
        return;
    }
    Actor& actor = m_actors[index];
    if (actor.look == look && (actor.model != kNone || actor.shown.model != kNone || look.model == 0)) {
        return;
    }
    releaseLook(actor);
    actor.look = look;
    if (look.model == 0) {
        return;
    }
    actor.model = acquireModel(look.model);
    m_modelEntries[actor.model].cached = m_modelEntries[actor.model].cached || actor.effect;
    for (u32 slot = 1; slot < kActorTextureSlots; ++slot) {
        if (slot == static_cast<u32>(pack::ActorTextureSlot::Body) && !look.skin.empty()) {
            continue; // composed
        }
        actor.slotTextures[slot] = acquireTexture(look.textures[slot]);
    }
    for (const game::SkinLayer& layer : look.skin) {
        actor.skinLayers.push_back(acquireTexture(layer.texture));
    }
    for (const game::AttachedModel& a : look.attachments) {
        Attached attached;
        attached.attachment = a.attachment;
        attached.slot = a.slot;
        attached.model = acquireModel(a.model);
        attached.texture = acquireTexture(a.texture);
        actor.attachments.push_back(std::move(attached));
    }
    m_pendingLooks.push_back(index);
}

const game::ActorLook* Actors::look(u32 index) const {
    return index < m_actors.size() && m_actors[index].alive ? &m_actors[index].look : nullptr;
}

void Actors::setPlacement(u32 index, const glm::dvec3& position, f32 yaw, f32 scale) {
    if (index < m_actors.size() && m_actors[index].alive) {
        Actor& actor = m_actors[index];
        actor.position = position;
        actor.yaw = yaw;
        actor.scale = scale > 0.0f ? scale : 1.0f;
    }
}

void Actors::play(u32 index, u16 animation, bool loop, f32 speed, f32 groundSpeed, bool legsHeld) {
    if (index < m_actors.size() && m_actors[index].alive) {
        Actor& actor = m_actors[index];
        actor.animation = animation;
        actor.loop = loop;
        actor.speed = std::clamp(speed, 0.0f, 4.0f);
        actor.groundSpeed = std::max(0.0f, groundSpeed);
        actor.legsHeld = legsHeld;
    }
}

void Actors::playAction(u32 index, u16 animation, bool loop, f32 speed) {
    if (index < m_actors.size() && m_actors[index].alive && animation != kNoAction) {
        Actor& actor = m_actors[index];
        actor.action = animation;
        actor.actionLoop = loop;
        actor.actionSpeed = std::clamp(speed, 0.1f, 4.0f);
        ++actor.actionSerial;
    }
}

void Actors::stopAction(u32 index) {
    if (index < m_actors.size() && m_actors[index].alive && m_actors[index].action != kNoAction) {
        m_actors[index].actionEnding = true;
    }
}

u16 Actors::action(u32 index) const {
    return index < m_actors.size() && m_actors[index].alive && !m_actors[index].actionEnding ? m_actors[index].action
                                                                                             : kNoAction;
}

u16 Actors::playing(u32 index) const {
    return index < m_actors.size() && m_actors[index].alive ? m_actors[index].playing : kNoAction;
}

void Actors::animateAction(Actor& actor, const anim::Skeleton& skeleton, u16 base, f32 dt) {
    constexpr f32 kFadeIn = 1.0f / 0.1f; // weight per second
    constexpr f32 kFadeOut = 1.0f / 0.2f;
    if (actor.actionStarted != actor.actionSerial) {
        actor.actionStarted = actor.actionSerial;
        actor.actionEnding = false;
        actor.actionShown =
            actor.actionAnimator.play(skeleton, actor.action, actor.actionLoop, m_fallbacks, actor.random, true);
        if (actor.actionShown == anim::animation::kStand && actor.action != anim::animation::kStand) {
            actor.actionEnding = true; // the model has nothing for it
            actor.actionWeight = 0.0f;
        }
    }
    // Over what the actor does (WoW.exe 0x5FE2F0): on the upper body while the legs move or keep a stand state (the
    // animations of its table; EmoteEat always: eating and drinking seated), else the whole body when it outranks the
    // base animation.
    const u16 basePriority = m_fallbacks.priorityOf(base);
    const u16 priority = m_fallbacks.priorityOf(actor.actionShown);
    const bool legsBusy = (m_fallbacks.bodyFlagsOf(base) & anim::bodyflag::kLegs) != 0 && basePriority > 0;
    const bool eat = actor.actionShown == anim::animation::kEmoteEat;
    bool shown = priority >= basePriority;
    actor.actionUpper = false;
    if (legsBusy || actor.legsHeld || eat) {
        actor.actionUpper = (eat || anim::upperBodyAnimation(actor.actionShown)) && skeleton.hasUpperBody();
        shown = actor.actionUpper || (legsBusy ? priority > basePriority : priority >= basePriority);
    }
    if (!shown && !actor.actionLoop) {
        actor.actionEnding = true;
    }
    actor.actionAnimator.update(skeleton, dt, actor.actionSpeed, actor.random);
    // A one-shot blends out over its last moments.
    const anim::AnimPose& pose = actor.actionAnimator.pose();
    if (!actor.actionLoop && pose.current.sequence >= 0) {
        const u32 duration = skeleton.duration(static_cast<u32>(pose.current.sequence));
        const f32 left = static_cast<f32>(duration - std::min(duration, pose.current.time));
        if (actor.actionAnimator.finished() || left < 1000.0f * actor.actionSpeed / kFadeOut) {
            actor.actionEnding = true;
        }
    }
    const f32 target = actor.actionEnding || !shown ? 0.0f : 1.0f;
    if (actor.actionWeight < target) {
        actor.actionWeight = std::min(target, actor.actionWeight + dt * kFadeIn);
    } else {
        actor.actionWeight = std::max(target, actor.actionWeight - dt * kFadeOut);
    }
    if (actor.actionEnding && actor.actionWeight <= 0.0f) {
        actor.action = kNoAction;
        actor.actionShown = kNoAction;
        actor.actionEnding = false;
        actor.actionAnimator.reset();
    }
}

void Actors::collectEvents(u32 index, const Actor& actor, const anim::Skeleton& skeleton) {
    const std::span<const pack::ActorEvent> events = skeleton.events();
    const auto add = [&](std::span<const anim::Animator::Played> played) {
        for (const anim::Animator::Played& piece : played) {
            if (piece.sequence < 0) {
                continue;
            }
            for (u32 e = 0; e < events.size(); ++e) {
                if ((events[e].flags & pack::kActorEventTimed) == 0) {
                    continue;
                }
                for (const u32 t : skeleton.eventTimes(e, static_cast<u32>(piece.sequence))) {
                    if (t >= piece.to) {
                        break;
                    }
                    if (t >= piece.from && m_eventHits.size() < kMaxEvents) {
                        m_eventHits.push_back({index, events[e].id, events[e].data, e, actor.position});
                    }
                }
            }
        }
    };
    // An action shown over the whole body (an emote, a cast) replaces the base animation: only its events fire. On
    // the upper body (a swing while running), the legs keep stepping.
    const bool action = actor.action != kNoAction && actor.actionWeight > 0.5f;
    if (!action || actor.actionUpper) {
        add(actor.animator.played());
    }
    if (action) {
        add(actor.actionAnimator.played());
    }
}

glm::dvec3 Actors::eventWorld(const Actor& actor, const pack::ActorEvent& event) const {
    const glm::vec3 point(event.position[0], event.position[1], event.position[2]);
    if (actor.visibleNow && !actor.unplaced && event.bone < actor.bones.size()) {
        const glm::vec3 posed = anim::transformPoint(actor.bones[event.bone], point);
        return actor.drawnCamera + glm::dvec3(anim::transformPoint(actor.transform, posed));
    }
    return actor.position + glm::dvec3(anim::transformPoint(placement(glm::vec3(0.0f), actor.yaw, actor.scale), point));
}

std::span<const pack::ActorEvent> Actors::modelEvents(u32 index) const {
    if (index >= m_actors.size() || !m_actors[index].alive || m_actors[index].shown.model == kNone) {
        return {};
    }
    const ModelEntry& model = m_modelEntries[m_actors[index].shown.model];
    return model.state == State::Ready ? model.skeleton.events() : std::span<const pack::ActorEvent>{};
}

std::optional<glm::dvec3> Actors::eventPosition(u32 index, u32 event) const {
    const std::span<const pack::ActorEvent> events = modelEvents(index);
    if (event >= events.size()) {
        return std::nullopt;
    }
    return eventWorld(m_actors[index], events[event]);
}

void Actors::setMount(u32 index, u32 mount) {
    attach(index, mount, game::attach::kShield);
}

void Actors::setCameraFade(u32 index, f32 alpha) {
    if (index < m_actors.size() && m_actors[index].alive) {
        m_actors[index].cameraAlpha = std::clamp(alpha, 0.0f, 1.0f);
    }
}

void Actors::setFade(u32 index, const glm::vec4& fade) {
    if (index < m_actors.size() && m_actors[index].alive) {
        m_actors[index].fade = fade;
    }
}

void Actors::setTint(u32 index, const glm::vec3& rgb) {
    if (index < m_actors.size() && m_actors[index].alive) {
        m_actors[index].tint = rgb;
    }
}

void Actors::attachToItem(u32 index, u32 parent, u8 equipSlot, u32 itemAttachmentId) {
    attach(index, parent, itemAttachmentId);
    if (index < m_actors.size() && m_actors[index].alive) {
        m_actors[index].mountItemSlot = equipSlot;
    }
}

void Actors::attach(u32 index, u32 parent, u32 attachmentId) {
    if (index < m_actors.size() && m_actors[index].alive && parent != index) {
        m_actors[index].mount = parent < m_actors.size() && m_actors[parent].alive ? parent : kNone;
        m_actors[index].mountAttachment = attachmentId;
        m_actors[index].mountItemSlot = 0xFF;
    }
}

u32 Actors::createEffects(u32 model, u32 seed, bool warm) {
    if (!m_particles || model == kNone || m_modelEntries[model].state != State::Ready ||
        !m_modelEntries[model].effects || !m_modelEntries[model].effects->hasParticles()) {
        return kNone;
    }
    const ModelEntry& entry = m_modelEntries[model];
    std::array<TextureHandle, 256> textures{};
    const usize count = std::min(entry.effectTextures.size(), textures.size());
    for (usize i = 0; i < count; ++i) {
        const u32 t = entry.effectTextures[i];
        textures[i] =
            t != kNone && m_textureEntries[t].state == State::Ready ? m_textureEntries[t].handle : TextureHandle{};
    }
    return m_particles->create(*entry.effects, {textures.data(), count}, seed, warm);
}

void Actors::stopEffects(u32 index) {
    if (index >= m_actors.size() || !m_actors[index].alive) {
        return;
    }
    Actor& actor = m_actors[index];
    actor.hidden = true;
    if (m_particles) {
        m_particles->stop(actor.shown.fx);
        for (const Attached& a : actor.shown.attachments) {
            m_particles->stop(a.fx);
        }
    }
}

bool Actors::effectsAlive(u32 index) const {
    if (index >= m_actors.size() || !m_actors[index].alive || !m_particles) {
        return false;
    }
    const Actor& actor = m_actors[index];
    bool alive = m_particles->alive(actor.shown.fx);
    for (const Attached& a : actor.shown.attachments) {
        alive = alive || m_particles->alive(a.fx);
    }
    return alive;
}

bool Actors::ready(u32 index) const {
    return index < m_actors.size() && m_actors[index].alive && m_actors[index].shown.model != kNone &&
           m_modelEntries[m_actors[index].shown.model].state == State::Ready;
}

bool Actors::visible(u32 index) const {
    return index < m_actors.size() && m_actors[index].alive && m_actors[index].visibleNow;
}

bool Actors::animationFinished(u32 index) const {
    return index < m_actors.size() && m_actors[index].alive && m_actors[index].animator.finished();
}

f32 Actors::animationSeconds(u32 index) const {
    if (index >= m_actors.size() || !m_actors[index].alive || m_actors[index].shown.model == kNone) {
        return 0.0f;
    }
    const Actor& actor = m_actors[index];
    const ModelEntry& model = m_modelEntries[actor.shown.model];
    const i32 sequence = actor.animator.pose().current.sequence;
    if (model.state != State::Ready || sequence < 0) {
        return 0.0f;
    }
    return static_cast<f32>(model.skeleton.duration(static_cast<u32>(sequence))) * 0.001f;
}

bool Actors::hasAnimation(u32 index, u16 animation) const {
    if (!ready(index)) {
        return false;
    }
    return m_modelEntries[m_actors[index].shown.model].skeleton.findSequence(animation) >= 0;
}

std::optional<glm::vec4> Actors::bounds(u32 index) const {
    if (!visible(index)) {
        return std::nullopt;
    }
    const Actor& actor = m_actors[index];
    const ModelEntry& model = m_modelEntries[actor.shown.model];
    const glm::dvec3 center = actor.drawnCamera + glm::dvec3(anim::transformPoint(actor.transform, model.center));
    return glm::vec4(glm::vec3(center), model.radius * actor.scale);
}

void Actors::setBody(u32 index, bool body) {
    if (index < m_actors.size() && m_actors[index].alive) {
        m_actors[index].body = body;
    }
}

void Actors::collectBodies(std::vector<BodyView>& out) const {
    out.clear();
    for (u32 index : m_visible) {
        const Actor& actor = m_actors[index];
        if (!actor.body || !actor.visibleNow || actor.hidden) {
            continue;
        }
        const ModelEntry& model = m_modelEntries[actor.shown.model];
        BodyView view;
        view.actor = index;
        view.mount = actor.mount;
        view.model = model.hash;
        view.feet = actor.position;
        view.yaw = actor.yaw;
        view.scale = actor.scale;
        view.top = model.top * actor.scale;
        view.opacity = actor.fade.w;
        view.animation = actor.playing;
        out.push_back(view); // capacity reserved by the caller
    }
}

std::optional<glm::dvec3> Actors::attachmentPosition(u32 index, u32 attachmentId) const {
    if (!visible(index)) {
        return std::nullopt;
    }
    const Actor& actor = m_actors[index];
    const ModelEntry& model = m_modelEntries[actor.shown.model];
    const pack::ActorAttachment* a = model.skeleton.attachment(attachmentId);
    if (!a || actor.bones.size() <= a->bone) {
        return std::nullopt;
    }
    const glm::vec3 local = anim::transformPoint(actor.bones[a->bone], glm::vec3(a->position[0], a->position[1], a->position[2]));
    return actor.drawnCamera + glm::dvec3(anim::transformPoint(actor.transform, local));
}

bool Actors::hasAttachment(u32 index, u32 attachmentId) const {
    if (index >= m_actors.size() || !m_actors[index].alive) {
        return false;
    }
    const u32 model = m_actors[index].shown.model != kNone ? m_actors[index].shown.model : m_actors[index].model;
    if (model == kNone || m_modelEntries[model].state != State::Ready) {
        return true; // not known yet
    }
    return m_modelEntries[model].skeleton.attachment(attachmentId) != nullptr;
}

std::optional<glm::dvec3> Actors::headPosition(u32 index) const {
    if (!visible(index)) {
        return std::nullopt;
    }
    const Actor& actor = m_actors[index];
    const ModelEntry& model = m_modelEntries[actor.shown.model];
    return actor.drawnCamera + glm::dvec3(anim::transformPoint(actor.transform, glm::vec3(0.0f, 0.0f, model.top)));
}

std::optional<glm::dvec3> Actors::itemTip(u32 index, u8 equipSlot) const {
    if (!visible(index)) {
        return std::nullopt;
    }
    const Actor& actor = m_actors[index];
    for (const Attached& a : actor.shown.attachments) {
        if (a.slot != equipSlot || !a.placed || a.model == kNone ||
            m_modelEntries[a.model].state != State::Ready) {
            continue;
        }
        // Phase 14 (spellfx): the item model's $CCH event, where the client's fishing line leaves the pole
        // (WoW.exe 0x61F780); without it, the far end of the item along its length (+X).
        const anim::Skeleton& skeleton = m_modelEntries[a.model].skeleton;
        glm::vec3 point(skeleton.header().boundsMax[0], 0.0f, 0.0f);
        for (const pack::ActorEvent& event : skeleton.events()) {
            if (event.id == pack::actorEventId("$CCH")) {
                point = glm::vec3(event.position[0], event.position[1], event.position[2]);
                if (event.bone < a.bones.size()) {
                    point = anim::transformPoint(a.bones[event.bone], point);
                }
                break;
            }
        }
        return actor.drawnCamera + glm::dvec3(anim::transformPoint(a.transform, point));
    }
    return std::nullopt;
}

bool Actors::lookLoaded(const Actor& actor) const {
    auto modelLoaded = [&](u32 model) {
        if (model == kNone) {
            return true;
        }
        const ModelEntry& m = m_modelEntries[model];
        return m.state == State::Failed || (m.state == State::Ready && m.pendingTextures == 0);
    };
    auto textureLoaded = [&](u32 texture) {
        return texture == kNone || m_textureEntries[texture].state != State::Requested;
    };
    if (!modelLoaded(actor.model)) {
        return false;
    }
    for (u32 texture : actor.slotTextures) {
        if (!textureLoaded(texture)) {
            return false;
        }
    }
    for (u32 texture : actor.skinLayers) {
        if (!textureLoaded(texture)) {
            return false;
        }
    }
    for (const Attached& a : actor.attachments) {
        if (!modelLoaded(a.model) || !textureLoaded(a.texture)) {
            return false;
        }
    }
    return true;
}

void Actors::resolveGeosets(Actor& actor) {
    actor.geosetIndices.clear();
    if (actor.model == kNone || actor.look.geosets.empty()) {
        actor.geosetsResolved = true;
        return;
    }
    const std::vector<u16>& table = m_modelEntries[actor.model].geosets;
    for (u16 id : actor.look.geosets) {
        const auto it = std::lower_bound(table.begin(), table.end(), id);
        if (it != table.end() && *it == id) {
            actor.geosetIndices.push_back(static_cast<u16>(it - table.begin()));
        }
    }
    if (actor.geosetIndices.empty()) {
        actor.geosetIndices.push_back(0xFFFF); // nothing matched: an index no part has (draw nothing but keep masked)
    }
    actor.geosetsResolved = true;
}

void Actors::acquireMesh(Actor& actor, Renderer& renderer) {
    Actor::Shown& shown = actor.shown;
    if (shown.model == kNone || shown.geosetIndices.empty() || m_modelEntries[shown.model].state != State::Ready) {
        return;
    }
    const ModelEntry& model = m_modelEntries[shown.model];
    u64 key = fnv1a64(&model.hash, sizeof(model.hash));
    key = fnv1a64(shown.geosetIndices.data(), shown.geosetIndices.size() * sizeof(u16), key);
    key = key == 0 ? 1 : key;
    if (const auto it = m_meshes.find(key); it != m_meshes.end()) {
        ++it->second.refs;
        shown.meshKey = key;
        shown.mesh = it->second.handle;
        return;
    }
    // The visible parts' indices back to back, one run per material (parts of a material are adjacent).
    const pack::ActorModelHeader& header = model.skeleton.header();
    const auto* parts = reinterpret_cast<const pack::ActorPart*>(model.blob + header.partsOffset);
    const auto* indices = reinterpret_cast<const u16*>(model.blob + header.indicesOffset);
    m_geosetEnabled.assign(std::max(1u, header.geosetCount), 0);
    for (u16 g : shown.geosetIndices) {
        if (g < m_geosetEnabled.size()) {
            m_geosetEnabled[g] = 1;
        }
    }
    m_meshIndices.clear();
    m_meshRuns.clear();
    for (u32 p = 0; p < header.partCount; ++p) {
        const pack::ActorPart& part = parts[p];
        if (part.geosetIndex >= m_geosetEnabled.size() || !m_geosetEnabled[part.geosetIndex] ||
            u64{part.firstIndex} + part.indexCount > header.indexCount) {
            continue;
        }
        if (m_meshRuns.empty() || m_meshRuns.back().material != part.material) {
            m_meshRuns.push_back({static_cast<u32>(m_meshIndices.size()), 0, part.material});
        }
        m_meshIndices.insert(m_meshIndices.end(), indices + part.firstIndex, indices + part.firstIndex + part.indexCount);
        m_meshRuns.back().indexCount += part.indexCount;
    }
    if (m_meshIndices.empty()) {
        return;
    }
    Result<ActorMeshHandle> mesh = renderer.createActorMesh(model.handle, m_meshIndices, m_meshRuns);
    if (!mesh) {
        return; // drawn part by part meanwhile
    }
    m_meshes.emplace(key, SharedMesh{mesh.value(), 1});
    shown.meshKey = key;
    shown.mesh = mesh.value();
}

void Actors::promote(Actor& actor, Renderer& renderer) {
    // Compose (or share) the skin first: its layers are released right after.
    u64 skinKey = 0;
    if (!actor.look.skin.empty()) {
        skinKey = skinKeyOf(actor.look.skin);
        auto it = m_skins.find(skinKey);
        if (it == m_skins.end()) {
            std::array<ComposeLayer, 32> layers{};
            u32 count = 0;
            for (usize i = 0; i < actor.look.skin.size() && count < layers.size(); ++i) {
                const u32 texture = actor.skinLayers[i];
                if (texture == kNone || m_textureEntries[texture].state != State::Ready) {
                    continue;
                }
                const game::SkinRect rect = game::skinRegionRect(actor.look.skin[i].region);
                layers[count++] = {m_textureEntries[texture].handle, rect.x, rect.y, rect.width, rect.height};
            }
            Result<TextureHandle> composed =
                count > 0 ? renderer.composeTexture(game::kSkinSize, game::kSkinSize, {layers.data(), count})
                          : Result<TextureHandle>(Error{"no skin layer loaded"});
            if (composed) {
                it = m_skins.emplace(skinKey, SharedSkin{composed.value(), 0}).first;
            } else {
                skinKey = 0;
            }
        }
        if (skinKey != 0) {
            ++it->second.refs;
        }
    }
    for (u32 texture : actor.skinLayers) {
        releaseTexture(texture);
    }
    actor.skinLayers.clear();

    // The loading look becomes the shown one: its references move over, the old shown look lets go of its own.
    resolveGeosets(actor);
    releaseShown(actor.shown);
    actor.shown.model = actor.model;
    actor.shown.slotTextures = actor.slotTextures;
    actor.shown.geosetIndices = actor.geosetIndices;
    actor.shown.attachments = std::move(actor.attachments);
    actor.shown.skinKey = skinKey;
    actor.shown.portraitKey = portraitLookKey(actor.look);
    for (u32 slot = 0; slot < kActorTextureSlots; ++slot) {
        const u32 t = actor.shown.slotTextures[slot];
        actor.shown.textures[slot] = t != kNone && m_textureEntries[t].state == State::Ready ? m_textureEntries[t].handle
                                                                                             : TextureHandle{};
    }
    if (skinKey != 0) {
        actor.shown.textures[static_cast<u32>(pack::ActorTextureSlot::Body)] = m_skins[skinKey].handle;
    }
    actor.model = kNone;
    actor.slotTextures.fill(kNone);
    actor.attachments.clear();
    actor.geosetIndices.clear();
    acquireMesh(actor, renderer);

    // A new model has other sequences: restart the animation. Size the per-frame arrays here, not in the jobs.
    const ModelEntry& model = m_modelEntries[actor.shown.model];
    actor.animator.reset();
    actor.actionAnimator.reset(); // its sequences were the old model's
    actor.action = kNoAction;
    actor.actionShown = kNoAction;
    actor.actionWeight = 0.0f;
    actor.actionEnding = false;
    if (model.state == State::Ready) {
        actor.bones.assign(model.skeleton.boneCount(), anim::BoneMatrix{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}});
        actor.colors.assign(model.animatedColors ? model.skeleton.materials().size() : 0, glm::vec4(1.0f));
        actor.uvTransforms.assign(model.effects ? model.effects->uvAnimations().size() : 0, UvTransform{});
        actor.animator.play(model.skeleton, actor.animation, actor.loop, m_fallbacks, actor.random);
        actor.sampledFrame = 0;
    }
    actor.shown.fx = actor.offscreen ? kNone : createEffects(actor.shown.model, actor.random, !actor.effect);
    for (Attached& a : actor.shown.attachments) {
        if (a.model != kNone && m_modelEntries[a.model].state == State::Ready) {
            const ModelEntry& item = m_modelEntries[a.model];
            const anim::Skeleton& skeleton = item.skeleton;
            a.bones.assign(skeleton.boneCount(), anim::BoneMatrix{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}});
            a.uvTransforms.assign(item.effects ? item.effects->uvAnimations().size() : 0, UvTransform{});
            a.animator.play(skeleton, anim::animation::kStand, true, m_fallbacks, actor.random);
            a.fx = actor.offscreen ? kNone : createEffects(a.model, actor.random ^ a.attachment, !actor.effect);
        }
    }
}

void Actors::update(Renderer& renderer) {
    if (!isOpen()) {
        return;
    }
    VD_PROFILE_ZONE("Actors::update");
    evictIdleModels();
    requestPending();
    AssetStreamer::Done done;
    while (m_waiting.size() < 512 && m_streamer.poll(done)) {
        m_waiting.push_back(done);
    }
    const Clock::time_point start = Clock::now();
    u32 uploads = 0;
    u64 bytes = 0;
    usize processed = 0;
    for (; processed < m_waiting.size(); ++processed) {
        AssetStreamer::Done& next = m_waiting[processed];
        if (uploads >= kMaxUploadsPerFrame ||
            (uploads > 0 && (bytes + next.size > kUploadBytesPerFrame ||
                             secondsBetween(start, Clock::now()) * 1000.0 > kUploadBudgetMs))) {
            break;
        }
        const u32 index = static_cast<u32>(next.tag);
        u64 frame = 0;
        bool keep = false;
        if (next.tag & kTagModel) {
            ModelEntry& entry = m_modelEntries[index];
            if (entry.refs == 0) {
                entry.state = State::Failed;
                ++entry.refs;
                releaseModel(index);
            } else if (!next.ok) {
                entry.state = State::Failed;
            } else {
                onModelReady(index, next, renderer);
                keep = next.data == nullptr; // the entry took the CPU copy
                frame = renderer.frameNumber() + 1;
            }
        } else {
            TextureEntry& entry = m_textureEntries[index];
            if (entry.refs == 0) {
                entry.state = State::Failed;
                ++entry.refs;
                releaseTexture(index);
            } else if (!next.ok) {
                entry.state = State::Failed;
                for (const auto& [model, material] : entry.waiting) {
                    (void)material;
                    --m_modelEntries[model].pendingTextures;
                }
                entry.waiting.clear();
            } else {
                onTextureReady(index, next, renderer);
                frame = renderer.frameNumber() + 1;
            }
        }
        ++uploads;
        bytes += next.size;
        m_streamer.release(next, frame, keep);
    }
    m_waiting.erase(m_waiting.begin(), m_waiting.begin() + static_cast<std::ptrdiff_t>(processed));
    // Phase 9: preloaded models wait in the idle cache once loaded (or go, if they failed).
    for (usize i = 0; i < m_preloads.size();) {
        if (m_modelEntries[m_preloads[i]].state == State::Requested) {
            ++i;
            continue;
        }
        releaseModel(m_preloads[i]);
        m_preloads[i] = m_preloads.back();
        m_preloads.pop_back();
    }
    m_streamer.update(renderer.completedFrames());
    if (uploads > 0) {
        m_stats.worstUploadMs =
            std::max(m_stats.worstUploadMs, static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0));
    }

    // Looks whose assets are all in: they replace what the actor showed.
    u32 composes = 0;
    usize kept = 0;
    for (usize i = 0; i < m_pendingLooks.size(); ++i) {
        const u32 index = m_pendingLooks[i];
        if (index >= m_actors.size() || !m_actors[index].alive || m_actors[index].model == kNone) {
            continue; // destroyed, or its look was replaced (the newer request is also in the list)
        }
        Actor& actor = m_actors[index];
        if (!lookLoaded(actor) || (!actor.look.skin.empty() && composes >= kMaxComposesPerFrame)) {
            m_pendingLooks[kept++] = index;
            continue;
        }
        composes += actor.look.skin.empty() ? 0 : 1;
        promote(actor, renderer);
    }
    m_pendingLooks.resize(kept);

    m_stats.pendingAssets = m_streamer.pending() + static_cast<u32>(m_requestQueue.size() + m_pendingLooks.size());
    m_stats.modelsLoaded = static_cast<u32>(m_modelIndex.size());
    m_stats.texturesLoaded = static_cast<u32>(m_textureIndex.size());
    m_stats.composedSkins = static_cast<u32>(m_skins.size());
}

// --- Per frame ----------------------------------------------------------------------------------------------

void Actors::sample(Actor& actor, u32 nowMs, const glm::vec3& cameraForward) {
    VD_PROFILE_ZONE("Actors::sample");
    const ModelEntry& model = m_modelEntries[actor.shown.model];
    // Billboards face the camera: its direction in the model's space (yaw only; actors stand upright). Ours (quest
    // marks): one on another's point turns with that point, so from its last placement's axes.
    const f32 c = std::cos(-actor.yaw);
    const f32 s = std::sin(-actor.yaw);
    const glm::vec3 toCamera = -cameraForward;
    glm::vec3 inModel(c * toCamera.x - s * toCamera.y, s * toCamera.x + c * toCamera.y, toCamera.z);
    if (actor.mount != kNone) {
        const anim::BoneMatrix& m = actor.transform;
        glm::vec3 axes[3];
        bool placed = true;
        for (u32 i = 0; i < 3; ++i) {
            axes[i] = glm::vec3(m.rows[i], m.rows[4 + i], m.rows[8 + i]);
            placed = placed && glm::dot(axes[i], axes[i]) > 1e-8f;
        }
        if (placed) {
            inModel = glm::vec3(glm::dot(glm::normalize(axes[0]), toCamera), glm::dot(glm::normalize(axes[1]), toCamera),
                                glm::dot(glm::normalize(axes[2]), toCamera));
        }
    }
    {
        VD_PROFILE_ZONE("Actors::bones");
        anim::ActionPose action;
        if (actor.action != kNoAction && actor.actionWeight > 0.0f) {
            action.pose = actor.actionAnimator.pose();
            action.weight = actor.actionWeight;
            action.upperBody = actor.actionUpper;
        }
        anim::computeBones(model.skeleton, actor.animator.pose(), nowMs, inModel, actor.bones.data(),
                           action.weight > 0.0f ? &action : nullptr);
    }
    if (model.animatedColors && !actor.colors.empty()) {
        anim::computeMaterialColors(model.skeleton, actor.animator.pose(), nowMs, actor.colors.data());
    }
    if (!actor.uvTransforms.empty()) {
        const anim::AnimLayer& layer = actor.animator.pose().current;
        model.effects->computeUvTransforms(layer.sequence >= 0 ? static_cast<u32>(layer.sequence) : 0, layer.time,
                                           nowMs, actor.uvTransforms.data());
    }
    {
        VD_PROFILE_ZONE("Actors::attachments");
        for (Attached& a : actor.shown.attachments) {
            if (a.model != kNone && !a.bones.empty()) {
                anim::computeBones(m_modelEntries[a.model].skeleton, a.animator.pose(), nowMs, glm::vec3(0.0f),
                                   a.bones.data());
                if (!a.uvTransforms.empty()) {
                    const anim::AnimLayer& layer = a.animator.pose().current;
                    m_modelEntries[a.model].effects->computeUvTransforms(
                        layer.sequence >= 0 ? static_cast<u32>(layer.sequence) : 0, layer.time, nowMs,
                        a.uvTransforms.data());
                }
            }
        }
    }
}

void Actors::place(Actor& actor, const glm::dvec3& camera) {
    const ModelEntry& model = m_modelEntries[actor.shown.model];
    actor.unplaced = false;
    if (actor.mount != kNone && m_actors[actor.mount].visibleNow && actor.mountItemSlot != 0xFF) {
        // Phase 9: on a point of the item the parent holds in that slot (placed by the parent just before).
        actor.unplaced = true;
        for (const Attached& item : m_actors[actor.mount].shown.attachments) {
            if (item.slot != actor.mountItemSlot || !item.placed) {
                continue;
            }
            const pack::ActorAttachment* point = m_modelEntries[item.model].skeleton.attachment(actor.mountAttachment);
            if (point && point->bone < item.bones.size()) {
                actor.transform = rescale(
                    anim::multiply(item.transform,
                                   anim::multiply(item.bones[point->bone],
                                                  translation(glm::vec3(point->position[0], point->position[1],
                                                                        point->position[2])))),
                    actor.scale);
                actor.unplaced = false;
            }
            break;
        }
        if (actor.unplaced) {
            actor.transform = m_actors[actor.mount].transform;
        }
    } else if (actor.mount != kNone && m_actors[actor.mount].visibleNow) {
        // On the mount's saddle (attachment 0), keeping the rider's own scale.
        const Actor& mount = m_actors[actor.mount];
        const ModelEntry& mountModel = m_modelEntries[mount.shown.model];
        const pack::ActorAttachment* saddle = mountModel.skeleton.attachment(actor.mountAttachment);
        if (saddle && saddle->bone < mount.bones.size()) {
            const anim::BoneMatrix seat = anim::multiply(
                mount.transform,
                anim::multiply(mount.bones[saddle->bone],
                               translation(glm::vec3(saddle->position[0], saddle->position[1], saddle->position[2]))));
            actor.transform = rescale(seat, actor.scale);
        } else if (actor.effect) {
            actor.transform = rescale(mount.transform, actor.scale); // phase 9: no such point: at its feet
        } else {
            actor.transform = placement(glm::vec3(actor.position - camera), actor.yaw, actor.scale);
        }
    } else {
        actor.transform = placement(glm::vec3(actor.position - camera), actor.yaw, actor.scale);
    }
    actor.drawnCamera = camera;
    for (Attached& a : actor.shown.attachments) {
        a.placed = false;
        const pack::ActorAttachment* point = model.skeleton.attachment(a.attachment);
        if (!point || point->bone >= actor.bones.size() || a.model == kNone || a.bones.empty()) {
            continue;
        }
        a.transform = anim::multiply(
            actor.transform,
            anim::multiply(actor.bones[point->bone],
                           translation(glm::vec3(point->position[0], point->position[1], point->position[2]))));
        a.placed = true;
    }
}

void Actors::beginCollect(const glm::dvec3& camera, const Frustum& frustum, const glm::vec3& cameraForward, f32 dt,
                          u32 nowMs, JobSystem& jobs, Renderer& renderer, ActorFrame& out) {
    out.draws.clear();
    out.instances.clear();
    out.geosets.clear();
    out.colors.clear();
    out.uvTransforms.clear();
    out.boneCount = 0;
    m_visible.clear();
    m_eventHits.clear();
    m_stats.visible = 0;
    m_stats.sampled = 0;
    m_stats.events = 0;
    m_stats.bones = 0;
    m_stats.draws = 0;
    m_collect.active = false;
    if (!isOpen()) {
        return;
    }
    VD_PROFILE_ZONE("Actors::beginCollect");
    const Clock::time_point start = Clock::now();
    ++m_frame;
    const std::span<anim::BoneMatrix> palette = renderer.boneMatrices();
    const u32 paletteCapacity = palette.size() > kPaletteSlackBones ? static_cast<u32>(palette.size()) : 0;

    // Animate everyone (cheap), cull, and pick who is sampled this frame.
    u32 riders = 0;
    for (u32 index = 0; index < m_actors.size(); ++index) {
        Actor& actor = m_actors[index];
        actor.visibleNow = false;
        if (!actor.alive || actor.shown.model == kNone) {
            continue;
        }
        const ModelEntry& model = m_modelEntries[actor.shown.model];
        if (model.state != State::Ready || actor.bones.empty()) {
            continue;
        }
        actor.playing = actor.animator.play(model.skeleton, actor.animation, actor.loop, m_fallbacks, actor.random);
        f32 playback = actor.speed;
        if (actor.groundSpeed > 0.0f && actor.animator.pose().current.sequence >= 0) {
            const f32 authored =
                model.skeleton.sequence(static_cast<u32>(actor.animator.pose().current.sequence)).moveSpeed * actor.scale;
            playback = authored > 0.1f ? std::clamp(actor.groundSpeed / authored, 0.25f, 3.0f) : actor.speed;
        }
        actor.animator.update(model.skeleton, dt, playback, actor.random);
        if (actor.action != kNoAction) {
            animateAction(actor, model.skeleton, actor.playing, dt);
        }
        for (Attached& a : actor.shown.attachments) {
            if (a.model != kNone && !a.bones.empty()) {
                a.animator.update(m_modelEntries[a.model].skeleton, dt, 1.0f, actor.random);
            }
        }
        if (actor.offscreen) {
            continue; // phase 15: a model frame's (appendPosed): animated, never drawn in the world
        }
        glm::dvec3 where = actor.position;
        if (actor.mount != kNone && actor.mount < m_actors.size() && m_actors[actor.mount].alive) {
            where = m_actors[actor.mount].position;
        }
        if (model.skeleton.hasTimedEvents() && m_eventHits.size() < kMaxEvents) { // phase 14
            const glm::dvec3 offset = where - camera;
            if (glm::dot(offset, offset) <= static_cast<f64>(eventDistance) * eventDistance) {
                collectEvents(index, actor, model.skeleton);
            }
        }
        const f32 radius = model.radius * actor.scale;
        const glm::vec3 center = glm::vec3(where - camera) + glm::vec3(0.0f, 0.0f, model.center.z * actor.scale);
        const f32 distance = glm::length(center);
        // Small creatures vanish sooner than dragons.
        const f32 limit = std::min(drawDistance, 60.0f + radius * 40.0f);
        if (distance - radius > limit || !frustum.intersectsSphere(center, radius) || m_visible.size() >= kMaxVisible) {
            continue;
        }
        u32 rate = 8;
        if (distance < fullRateDistance) {
            rate = 1;
        } else if (distance < halfRateDistance) {
            rate = 2;
        } else if (distance < halfRateDistance * 2.0f) {
            rate = 4;
        }
        actor.sampleNow = actor.sampledFrame == 0 || (m_frame + index) % rate == 0;
        actor.visibleNow = true;
        m_visible.push_back(index);
        riders += actor.mount != kNone ? 1 : 0;
    }
    // Riders after their mounts (they sit on the mount's pose), and what rides a rider (phase 9: a spell effect on
    // a mounted player) after the riders.
    const auto level = [&](u32 i) -> u32 {
        const u32 parent = m_actors[i].mount;
        return parent == kNone ? 0u : m_actors[parent].mount == kNone ? 1u : 2u;
    };
    if (riders > 0) {
        m_ordered.clear(); // reserved: no allocation
        for (u32 l = 0; l < 3; ++l) {
            for (u32 i : m_visible) {
                if (level(i) == l) {
                    m_ordered.push_back(i);
                }
            }
        }
        m_visible.swap(m_ordered);
    }
    // Bone palette ranges.
    u32 bones = 0;
    usize kept = 0;
    for (u32 index : m_visible) {
        Actor& actor = m_actors[index];
        if (actor.effect && actor.mount != kNone && !m_actors[actor.mount].visibleNow) {
            actor.visibleNow = false; // on a unit not drawn this frame: nowhere to place it
            continue;
        }
        u32 need = static_cast<u32>(actor.bones.size());
        for (const Attached& a : actor.shown.attachments) {
            need += static_cast<u32>(a.bones.size());
        }
        if (bones + need > paletteCapacity) {
            actor.visibleNow = false; // palette full this frame
            continue;
        }
        actor.boneBase = bones;
        bones += need;
        if (!actor.colors.empty()) {
            actor.colorBase = static_cast<u32>(out.colors.size());
            out.colors.resize(out.colors.size() + actor.colors.size()); // capacity reserved by the owner
        }
        if (!actor.uvTransforms.empty()) {
            actor.uvBase = static_cast<u32>(out.uvTransforms.size());
            out.uvTransforms.resize(out.uvTransforms.size() + actor.uvTransforms.size()); // idem
        }
        for (Attached& a : actor.shown.attachments) {
            if (!a.uvTransforms.empty()) {
                a.uvBase = static_cast<u32>(out.uvTransforms.size());
                out.uvTransforms.resize(out.uvTransforms.size() + a.uvTransforms.size());
            }
        }
        m_visible[kept++] = index;
    }
    m_visible.resize(kept);
    const u32 firstRider = static_cast<u32>(
        std::find_if(m_visible.begin(), m_visible.end(), [&](u32 i) { return m_actors[i].mount != kNone; }) -
        m_visible.begin());
    m_collect.firstDeep = static_cast<u32>(
        std::find_if(m_visible.begin() + firstRider, m_visible.end(), [&](u32 i) { return level(i) == 2; }) -
        m_visible.begin());

    // Sample (or reuse) each pose, place it and its attachments, and copy the matrices into the palette: on the
    // workers, while the caller goes on with its frame until finishCollect(). Riders wait for their mounts there.
    m_collect.camera = camera;
    m_collect.cameraForward = cameraForward;
    m_collect.nowMs = nowMs;
    m_collect.palette = palette.data();
    m_collect.out = &out;
    m_collect.firstRider = firstRider;
    m_collect.bones = bones;
    m_collect.active = true;
    m_collect.sampleStart = Clock::now();
    m_work = {this, 0};
    if (firstRider > 0 && sampleOnWorkers) {
        jobs.begin(firstRider, 1, m_work);
    }
    m_collect.mainMs = secondsBetween(start, Clock::now()) * 1000.0;
}

void Actors::sampleRange(u32 begin, u32 end) {
    VD_PROFILE_ZONE("Actors::sampleRange");
    for (u32 i = begin; i < end; ++i) {
        Actor& actor = m_actors[m_visible[i]];
        if (actor.sampleNow) {
            sample(actor, m_collect.nowMs, m_collect.cameraForward);
            actor.sampledFrame = m_frame;
        }
        place(actor, m_collect.camera);
        anim::BoneMatrix* dst = m_collect.palette + actor.boneBase;
        std::memcpy(dst, actor.bones.data(), actor.bones.size() * sizeof(anim::BoneMatrix));
        dst += actor.bones.size();
        for (const Attached& a : actor.shown.attachments) {
            std::memcpy(dst, a.bones.data(), a.bones.size() * sizeof(anim::BoneMatrix));
            dst += a.bones.size();
        }
        if (!actor.colors.empty()) {
            std::memcpy(&m_collect.out->colors[actor.colorBase], actor.colors.data(),
                        actor.colors.size() * sizeof(glm::vec4));
        }
        if (!actor.uvTransforms.empty()) {
            std::memcpy(&m_collect.out->uvTransforms[actor.uvBase], actor.uvTransforms.data(),
                        actor.uvTransforms.size() * sizeof(UvTransform));
        }
        for (const Attached& a : actor.shown.attachments) {
            if (!a.uvTransforms.empty()) {
                std::memcpy(&m_collect.out->uvTransforms[a.uvBase], a.uvTransforms.data(),
                            a.uvTransforms.size() * sizeof(UvTransform));
            }
        }
    }
}

// Phase 9: the particle instances of a visible actor's models, where its last pose put them.
void Actors::placeEffects(const Actor& actor) {
    if (!m_particles) {
        return;
    }
    if (actor.shown.fx != kNone) {
        const anim::AnimLayer& layer = actor.animator.pose().current;
        fx::Placement placement;
        placement.origin = actor.drawnCamera;
        placement.toWorld = actor.transform;
        placement.bones = actor.bones.data();
        placement.boneCount = static_cast<u32>(actor.bones.size());
        placement.sequence = layer.sequence >= 0 ? static_cast<u32>(layer.sequence) : 0;
        placement.localMs = layer.time;
        placement.globalMs = m_collect.nowMs;
        m_particles->place(actor.shown.fx, placement);
    }
    for (const Attached& a : actor.shown.attachments) {
        if (a.fx == kNone || !a.placed) {
            continue;
        }
        const anim::AnimLayer& layer = a.animator.pose().current;
        fx::Placement placement;
        placement.origin = actor.drawnCamera;
        placement.toWorld = a.transform;
        placement.bones = a.bones.data();
        placement.boneCount = static_cast<u32>(a.bones.size());
        placement.sequence = layer.sequence >= 0 ? static_cast<u32>(layer.sequence) : 0;
        placement.localMs = layer.time;
        placement.globalMs = m_collect.nowMs;
        m_particles->place(a.fx, placement);
    }
}

void Actors::finishCollect(JobSystem& jobs, ActorFrame& out) {
    if (!m_collect.active) {
        return;
    }
    VD_PROFILE_ZONE("Actors::finishCollect");
    m_collect.active = false;
    const Clock::time_point start = Clock::now();
    if (sampleOnWorkers) {
        jobs.wait();
    } else {
        sampleRange(0, m_collect.firstRider);
    }
    if (m_collect.firstRider < m_collect.firstDeep) {
        m_riderWork = {this, m_collect.firstRider};
        const u32 riders = m_collect.firstDeep - m_collect.firstRider;
        if (sampleOnWorkers) {
            jobs.parallelFor(riders, 1, m_riderWork);
        } else {
            m_riderWork(0, riders);
        }
    }
    if (m_collect.firstDeep < m_visible.size()) { // phase 9: effects on riders, once the riders are placed
        m_deepWork = {this, m_collect.firstDeep};
        const u32 deep = static_cast<u32>(m_visible.size()) - m_collect.firstDeep;
        if (sampleOnWorkers) {
            jobs.parallelFor(deep, 1, m_deepWork);
        } else {
            m_deepWork(0, deep);
        }
    }
    m_stats.sampleMs = static_cast<f32>(secondsBetween(m_collect.sampleStart, Clock::now()) * 1000.0);
    const u32 bones = m_collect.bones;
    // Phase 14: where this frame's events happened, now that the drawn actors are posed.
    for (EventHit& hit : m_eventHits) {
        const Actor& actor = m_actors[hit.actor];
        hit.position = eventWorld(actor, m_modelEntries[actor.shown.model].skeleton.events()[hit.index]);
    }
    m_stats.events = static_cast<u32>(m_eventHits.size());

    // Draw lists: the actor's model, then what it carries.
    for (u32 index : m_visible) {
        Actor& actor = m_actors[index];
        m_stats.sampled += actor.sampleNow ? 1 : 0;
        if (actor.unplaced) {
            continue; // phase 9: the item it glows on is not held now
        }
        placeEffects(actor);
        if (actor.hidden || actor.cameraAlpha <= 0.0f) {
            continue; // a stopped effect: only its last particles are left; phase 14: the player in first person
        }
        const ModelEntry& model = m_modelEntries[actor.shown.model];
        // Phase 11: inside a building's interior the light baked there lights it, not the sun. Asked again when it
        // moves half a yard, or every 32 frames (buildings stream in).
        if (lightSource) {
            const glm::dvec3 feet =
                actor.drawnCamera + glm::dvec3(actor.transform.rows[3], actor.transform.rows[7], actor.transform.rows[11]);
            const glm::dvec3 moved = feet - actor.lightPoint;
            if (glm::dot(moved, moved) > 0.25 || ++actor.lightAge >= 32) {
                actor.lightPoint = feet;
                actor.lightAge = 0;
                actor.light = {};
                lightSource->lightAt(feet, actor.light);
            }
        }
        ActorDraw draw;
        draw.model = model.handle;
        draw.firstInstance = static_cast<u32>(out.instances.size());
        draw.instanceCount = 1;
        out.instances.push_back(instanceOf(actor.transform, actor.boneBase, actor.light));
        if (!actor.shown.geosetIndices.empty()) {
            draw.firstGeoset = static_cast<u32>(out.geosets.size());
            draw.geosetCount = static_cast<u32>(actor.shown.geosetIndices.size());
            out.geosets.insert(out.geosets.end(), actor.shown.geosetIndices.begin(), actor.shown.geosetIndices.end());
        }
        draw.firstColor = actor.colors.empty() ? ActorDraw::kNoColors : actor.colorBase;
        draw.firstUvTransform = actor.uvTransforms.empty() ? ModelDraw::kNoUvTransforms : actor.uvBase;
        draw.textures = actor.shown.textures;
        draw.mesh = actor.shown.mesh;
        // Phase 14: the camera's fade (lead) and the kits' tint (spellfx).
        const glm::vec4 fade = actor.fade * glm::vec4(actor.tint, actor.cameraAlpha);
        draw.fade = fade;
        out.draws.push_back(draw);
        u32 base = actor.boneBase + static_cast<u32>(actor.bones.size());
        for (const Attached& a : actor.shown.attachments) {
            if (a.placed) {
                ActorDraw item;
                item.model = m_modelEntries[a.model].handle;
                item.firstInstance = static_cast<u32>(out.instances.size());
                item.instanceCount = 1;
                out.instances.push_back(instanceOf(a.transform, base, actor.light));
                if (a.texture != kNone && m_textureEntries[a.texture].state == State::Ready) {
                    item.textures[static_cast<u32>(pack::ActorTextureSlot::Cape)] = m_textureEntries[a.texture].handle;
                }
                item.firstUvTransform = a.uvTransforms.empty() ? ModelDraw::kNoUvTransforms : a.uvBase;
                item.fade = fade; // phase 11: its weapons, helm and shoulders go with it
                out.draws.push_back(item);
            }
            base += static_cast<u32>(a.bones.size());
        }
    }
    out.boneCount = bones;
    m_stats.visible = static_cast<u32>(m_visible.size());
    m_stats.bones = bones;
    m_stats.draws = static_cast<u32>(out.draws.size());
    // Main thread time only: the sampling overlapped with the caller's own work is not counted.
    m_stats.animateMs = static_cast<f32>(m_collect.mainMs + secondsBetween(start, Clock::now()) * 1000.0);
}

// --- Portraits (parity)
// -------------------------------------------------------------------------------------

u64 Actors::portraitKey(u32 index) const {
    if (index >= m_actors.size() || !m_actors[index].alive || m_actors[index].shown.model == kNone ||
        m_modelEntries[m_actors[index].shown.model].state != State::Ready) {
        return 0;
    }
    return m_actors[index].shown.portraitKey;
}

const pack::ActorPortrait* Actors::portraitCamera(u32 index) const {
    if (portraitKey(index) == 0) {
        return nullptr;
    }
    const ModelEntry& model = m_modelEntries[m_actors[index].shown.model];
    const u32 offset = model.skeleton.header().portraitOffset;
    if (offset == 0 || offset % 4 != 0 || u64{offset} + sizeof(pack::ActorPortrait) > model.size) {
        return nullptr;
    }
    return reinterpret_cast<const pack::ActorPortrait*>(model.blob + offset);
}

u32 Actors::appendStill(u32 index, const glm::vec3& eye, const glm::vec3& target, Renderer& renderer,
                        ActorFrame& out, std::vector<ActorDraw>& draws) {
    return appendModel(index, eye, target, 0.0f, false, renderer, out, draws);
}

// --- Phase 15: the interface's model frames ---------------------------------------------------------------------

void Actors::setOffscreen(u32 index, bool offscreen) {
    if (index < m_actors.size() && m_actors[index].alive) {
        m_actors[index].offscreen = offscreen;
    }
}

bool Actors::shownBounds(u32 index, glm::vec3& min, glm::vec3& max) const {
    if (portraitKey(index) == 0) {
        return false;
    }
    const pack::ActorModelHeader& header = m_modelEntries[m_actors[index].shown.model].skeleton.header();
    min = glm::vec3(header.boundsMin[0], header.boundsMin[1], header.boundsMin[2]);
    max = glm::vec3(header.boundsMax[0], header.boundsMax[1], header.boundsMax[2]);
    return true;
}

f32 Actors::selectionHalfSize(u32 index) const {
    if (index >= m_actors.size() || !m_actors[index].alive || m_actors[index].shown.model == kNone ||
        m_modelEntries[m_actors[index].shown.model].state != State::Ready) {
        return 0.0f;
    }
    const pack::ActorModelHeader& header = m_modelEntries[m_actors[index].shown.model].skeleton.header();
    return header.selectionHalfSize > 0.0f ? header.selectionHalfSize
                                           : pack::actorSelectionHalfSize(header.boundsMin, header.boundsMax);
}

u32 Actors::appendPosed(u32 index, const glm::vec3& eye, const glm::vec3& target, f32 yaw, Renderer& renderer,
                        ActorFrame& out, std::vector<ActorDraw>& draws) {
    return appendModel(index, eye, target, yaw, true, renderer, out, draws);
}

u32 Actors::appendModel(u32 index, const glm::vec3& eye, const glm::vec3& target, f32 yaw, bool posed,
                        Renderer& renderer, ActorFrame& out, std::vector<ActorDraw>& draws) {
    if (portraitKey(index) == 0) {
        return 0;
    }
    VD_PROFILE_ZONE("Actors::appendModel");
    const Actor& actor = m_actors[index];
    const ModelEntry& model = m_modelEntries[actor.shown.model];
    const anim::Skeleton& skeleton = model.skeleton;
    const std::span<anim::BoneMatrix> palette = renderer.boneMatrices();
    u32 need = skeleton.boneCount();
    for (const Attached& a : actor.shown.attachments) {
        if (a.model != kNone && m_modelEntries[a.model].state == State::Ready) {
            need += m_modelEntries[a.model].skeleton.boneCount();
        }
    }
    if (u64{out.boneCount} + need > palette.size()) {
        return 0;
    }
    // Billboards face the camera (in the model's space: the model is turned by `yaw`).
    const glm::vec3 toEyeWorld =
        glm::length(eye - target) > 1e-4f ? glm::normalize(eye - target) : glm::vec3(1.0f, 0.0f, 0.0f);
    const f32 c = std::cos(yaw);
    const f32 s = std::sin(yaw);
    const glm::vec3 toEye(c * toEyeWorld.x + s * toEyeWorld.y, -s * toEyeWorld.x + c * toEyeWorld.y, toEyeWorld.z);
    anim::AnimPose still;
    still.current.sequence = skeleton.findSequence(anim::animation::kStand);
    const anim::AnimPose& pose = posed ? actor.animator.pose() : still;
    const u32 globalMs = posed ? m_collect.nowMs : 0;
    m_stillBones.resize(skeleton.boneCount()); // reserved: kMaxBones
    anim::computeBones(skeleton, pose, globalMs, toEye, m_stillBones.data());
    const u32 boneBase = out.boneCount;
    std::memcpy(palette.data() + boneBase, m_stillBones.data(), m_stillBones.size() * sizeof(anim::BoneMatrix));
    out.boneCount += static_cast<u32>(m_stillBones.size());

    const usize first = draws.size();
    const anim::BoneMatrix place = placement(-eye, yaw, 1.0f);
    ActorDraw draw;
    draw.model = model.handle;
    draw.firstInstance = static_cast<u32>(out.instances.size());
    draw.instanceCount = 1;
    out.instances.push_back(instanceOf(place, boneBase));
    if (!actor.shown.geosetIndices.empty()) {
        draw.firstGeoset = static_cast<u32>(out.geosets.size());
        draw.geosetCount = static_cast<u32>(actor.shown.geosetIndices.size());
        out.geosets.insert(out.geosets.end(), actor.shown.geosetIndices.begin(), actor.shown.geosetIndices.end());
    }
    if (model.animatedColors) {
        draw.firstColor = static_cast<u32>(out.colors.size());
        out.colors.resize(out.colors.size() + skeleton.materials().size());
        anim::computeMaterialColors(skeleton, pose, globalMs, &out.colors[draw.firstColor]);
    }
    const u32 sequence = pose.current.sequence >= 0 ? static_cast<u32>(pose.current.sequence) : 0;
    if (model.effects && !model.effects->uvAnimations().empty()) {
        draw.firstUvTransform = static_cast<u32>(out.uvTransforms.size());
        out.uvTransforms.resize(out.uvTransforms.size() + model.effects->uvAnimations().size());
        model.effects->computeUvTransforms(sequence, posed ? pose.current.time : 0, globalMs,
                                           &out.uvTransforms[draw.firstUvTransform]);
    }
    draw.textures = actor.shown.textures;
    draw.mesh = actor.shown.mesh;
    draws.push_back(draw);

    // What it carries (helm, shoulders, weapons) on the model's points, each in its own pose.
    for (const Attached& a : actor.shown.attachments) {
        const pack::ActorAttachment* point = skeleton.attachment(a.attachment);
        if (a.model == kNone || m_modelEntries[a.model].state != State::Ready || !point ||
            point->bone >= m_stillBones.size()) {
            continue;
        }
        const ModelEntry& item = m_modelEntries[a.model];
        anim::AnimPose itemStill;
        itemStill.current.sequence = item.skeleton.findSequence(anim::animation::kStand);
        const anim::AnimPose& itemPose = posed && !a.bones.empty() ? a.animator.pose() : itemStill;
        m_stillItemBones.resize(item.skeleton.boneCount());
        anim::computeBones(item.skeleton, itemPose, globalMs, glm::vec3(0.0f), m_stillItemBones.data());
        const u32 itemBase = out.boneCount;
        std::memcpy(palette.data() + itemBase, m_stillItemBones.data(),
                    m_stillItemBones.size() * sizeof(anim::BoneMatrix));
        out.boneCount += static_cast<u32>(m_stillItemBones.size());
        const anim::BoneMatrix transform =
            anim::multiply(place, anim::multiply(m_stillBones[point->bone],
                                                 translation(glm::vec3(point->position[0], point->position[1],
                                                                       point->position[2]))));
        ActorDraw itemDraw;
        itemDraw.model = item.handle;
        itemDraw.firstInstance = static_cast<u32>(out.instances.size());
        itemDraw.instanceCount = 1;
        out.instances.push_back(instanceOf(transform, itemBase));
        if (a.texture != kNone && m_textureEntries[a.texture].state == State::Ready) {
            itemDraw.textures[static_cast<u32>(pack::ActorTextureSlot::Cape)] = m_textureEntries[a.texture].handle;
        }
        if (item.effects && !item.effects->uvAnimations().empty()) {
            const i32 itemSequence = itemPose.current.sequence;
            itemDraw.firstUvTransform = static_cast<u32>(out.uvTransforms.size());
            out.uvTransforms.resize(out.uvTransforms.size() + item.effects->uvAnimations().size());
            item.effects->computeUvTransforms(itemSequence >= 0 ? static_cast<u32>(itemSequence) : 0,
                                              posed ? itemPose.current.time : 0, globalMs,
                                              &out.uvTransforms[itemDraw.firstUvTransform]);
        }
        draws.push_back(itemDraw);
    }
    return static_cast<u32>(draws.size() - first);
}

} // namespace vd
