#include "world/WorldObjects.h"

#include "core/Clock.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "world/DoodadFade.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

constexpr u64 kTagModel = 1ull << 32;
constexpr u64 kTagTexture = 2ull << 32;
constexpr u32 kMaxUploadsPerFrame = 32;
constexpr u64 kUploadBytesPerFrame = 16ull << 20; // at least one asset per frame regardless
constexpr f64 kUploadBudgetMs = 2.0;              // main thread time per frame, idem
constexpr f64 kSpawnBudgetMs = 1.0;               // WMO doodad sets, at least 64 doodads per frame

// out = a * b for 3x4 affine matrices (rows of [rotation | translation]).
void multiply(const f32 (&a)[12], const f32 (&b)[12], f32 (&out)[12]) {
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            f32 sum = column == 3 ? a[row * 4 + 3] : 0.0f;
            for (u32 k = 0; k < 3; ++k) {
                sum += a[row * 4 + k] * b[k * 4 + column];
            }
            out[row * 4 + column] = sum;
        }
    }
}

// WMO doodad placement (model space of the WMO): translate * rotate(quaternion x, y, z, w) * scale.
void doodadTransform(const pack::ModelDoodad& d, f32 (&out)[12]) {
    const f32 x = d.rotation[0], y = d.rotation[1], z = d.rotation[2], w = d.rotation[3];
    const f32 s = d.scale;
    const f32 r[9] = {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - z * w),        2.0f * (x * z + y * w),
                      2.0f * (x * y + z * w),        1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - x * w),
                      2.0f * (x * z - y * w),        2.0f * (y * z + x * w),        1.0f - 2.0f * (x * x + y * y)};
    for (u32 row = 0; row < 3; ++row) {
        out[row * 4 + 0] = r[row * 3 + 0] * s;
        out[row * 4 + 1] = r[row * 3 + 1] * s;
        out[row * 4 + 2] = r[row * 3 + 2] * s;
        out[row * 4 + 3] = d.position[row];
    }
}

} // namespace

WorldObjects::~WorldObjects() {
    m_streamer.stop();
}

Result<void> WorldObjects::open(const std::filesystem::path& packsDir, const pack::PackReader& textures,
                                Renderer& renderer) {
    close(renderer);
    VD_TRY(m_models.open(packsDir / "models.vdpk"));
    Result<StagingBlock> staging = renderer.createStagingBlock(kStagingBytes);
    if (!staging) {
        m_models.close();
        return staging.error();
    }
    m_staging = staging.value();
    m_textures = &textures;
    m_renderer = &renderer;
    m_instances.reserve(65536);
    m_spheres.reserve(65536);
    m_instanceModel.reserve(65536);
    m_classRadius.reserve(65536); // phase 14
    m_instanceFade.reserve(65536);
    m_fadeLimits.reserve(65536);
    m_modelDrawable.reserve(4096);
    m_modelEffects.reserve(4096);
    m_visibleEffects.reserve(4096);
    m_drawModels.reserve(2 * kMaxVisible);
    m_drawPoses.reserve(2 * kMaxVisible); // phase 14
    m_visible.reserve(kMaxVisible);
    m_pendingSpawns.reserve(1024);
    m_modelEntries.reserve(4096);
    m_textureEntries.reserve(8192);
    m_streamer.start(m_staging);
    VD_LOG_INFO("World objects: %u models in the pack", m_models.entryCount());
    return {};
}

void WorldObjects::close(Renderer& renderer) {
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
        delete[] model.effectBlob;
    }
    m_animator.clear(); // phase 14
    m_classRadius.clear();
    m_instanceFade.clear();
    m_fadeLimits.clear();
    for (TextureEntry& texture : m_textureEntries) {
        if (texture.handle.valid()) {
            renderer.destroyTexture(texture.handle);
        }
    }
    m_modelEntries.clear();
    m_freeModels.clear();
    m_modelIndex.clear();
    m_textureEntries.clear();
    m_freeTextures.clear();
    m_textureIndex.clear();
    m_requestQueue.clear();
    m_instances.clear();
    m_freeInstances.clear();
    m_spheres.clear();
    m_instanceModel.clear();
    m_modelDrawable.clear();
    m_modelEffects.clear();
    m_visibleEffects.clear();
    m_drawModels.clear();
    m_wmos.clear();
    m_wmoByInstance.clear();
    m_dynamicWmos.clear();
    m_pendingSpawns.clear();
    m_tiles.clear();
    renderer.destroyStagingBlock(m_staging);
    m_models.close();
    m_textures = nullptr;
    m_renderer = nullptr;
    m_stats = {};
}

u32 WorldObjects::acquireModel(u64 hash) {
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
        m_modelDrawable.push_back(kNotDrawable);
        m_modelEffects.push_back(0);
    }
    ModelEntry& entry = m_modelEntries[index];
    entry = ModelEntry{};
    m_modelDrawable[index] = kNotDrawable;
    m_modelEffects[index] = 0;
    entry.hash = hash;
    entry.refs = 1;
    m_modelIndex.emplace(hash, index);
    m_requestQueue.push_back(kTagModel | index);
    return index;
}

void WorldObjects::releaseModel(u32 index) {
    ModelEntry& entry = m_modelEntries[index];
    if (--entry.refs > 0 || entry.state == State::Requested) {
        return; // still used, or still loading: freed when its IO result arrives
    }
    if (entry.handle.valid()) {
        m_renderer->destroyModel(entry.handle);
    }
    for (u32 texture : entry.textures) {
        if (texture != kNone) {
            releaseTexture(texture);
        }
    }
    for (u32 texture : entry.effectTextures) {
        if (texture != kNone) {
            releaseTexture(texture);
        }
    }
    if (entry.skeleton != kNone) {
        m_animator.remove(entry.skeleton); // phase 14: before its blob goes
    }
    delete[] entry.effectBlob;
    m_modelIndex.erase(entry.hash);
    entry = ModelEntry{};
    m_modelDrawable[index] = kNotDrawable;
    m_modelEffects[index] = 0;
    m_freeModels.push_back(index);
}

u32 WorldObjects::acquireTexture(u64 hash) {
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

void WorldObjects::releaseTexture(u32 index) {
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

u32 WorldObjects::addInstance(u32 model, const f32 (&transform)[12], bool wmo) {
    u32 index = 0;
    if (!m_freeInstances.empty()) {
        index = m_freeInstances.back();
        m_freeInstances.pop_back();
    } else {
        index = static_cast<u32>(m_instances.size());
        m_instances.emplace_back();
        m_spheres.emplace_back(0.0f);
        m_instanceModel.push_back(model);
        m_classRadius.push_back(0.0f); // phase 14
        m_instanceFade.push_back(1.0f);
        m_fadeLimits.emplace_back(-1.0f);
        m_instanceOwner.push_back(kNone); // phase 18 (world)
        m_instanceDoodad.push_back(kNone);
    }
    Instance& instance = m_instances[index];
    instance = Instance{};
    instance.model = model;
    m_spheres[index] = glm::vec4(0.0f);
    m_instanceModel[index] = model;
    m_instanceOwner[index] = kNone; // phase 18 (world): set by spawnDoodads() for a building's doodads
    m_instanceDoodad[index] = kNone;
    std::memcpy(instance.transform, transform, sizeof(instance.transform));
    instance.scale = glm::length(glm::vec3(transform[0], transform[4], transform[8]));
    instance.alive = true;
    instance.wmo = wmo;
    instance.serial = m_nextSerial++;
    instance.spawnMs = animationMs; // phase 14
    ++m_stats.instances;
    return index;
}

u32 WorldObjects::addDynamic(u64 modelHash, const f32 (&transform)[12], bool wmo) {
    if (!isOpen() || modelHash == 0) {
        return kNoInstance;
    }
    const u32 index = addInstance(acquireModel(modelHash), transform, wmo);
    m_instances[index].dynamic = true; // phase 14: game objects keep their own range (1.12 does not fade them)
    if (wmo) {
        WmoInstance& building = m_dynamicWmos[index];
        building = WmoInstance{};
        building.instance = index;
        building.doodadsSpawned = true; // its doodads would not follow it
        building.spawnStage = 2;
        m_wmoByInstance[index] = &building;
        computeGroupSpheres(building); // when its model is already here
    }
    return index;
}

void WorldObjects::moveDynamic(u32 index, const f32 (&transform)[12]) {
    if (index >= m_instances.size() || !m_instances[index].alive) {
        return;
    }
    Instance& instance = m_instances[index];
    std::memcpy(instance.transform, transform, sizeof(instance.transform));
    instance.scale = glm::length(glm::vec3(transform[0], transform[4], transform[8]));
    m_spheres[index] = glm::vec4(0.0f); // recomputed when next culled
    if (const auto building = m_dynamicWmos.find(index); building != m_dynamicWmos.end()) {
        building->second.groupSpheres.clear(); // their capacity stays: no allocation while it moves
        building->second.portalSpheres.clear();
        computeGroupSpheres(building->second);
    }
}

void WorldObjects::removeDynamic(u32 index) {
    if (index < m_instances.size()) {
        if (m_dynamicWmos.erase(index) > 0) {
            m_wmoByInstance.erase(index);
        }
        removeInstance(index);
    }
}

void WorldObjects::removeInstance(u32 index) {
    Instance& instance = m_instances[index];
    if (!instance.alive) {
        return;
    }
    releaseModel(instance.model);
    instance = Instance{};
    m_spheres[index] = glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
    m_freeInstances.push_back(index);
    --m_stats.instances;
}

void WorldObjects::updateSphere(u32 index) {
    const Instance& instance = m_instances[index];
    const ModelEntry& model = m_modelEntries[instance.model];
    const f32* m = instance.transform;
    const glm::vec3 c = model.center;
    m_spheres[index] = glm::vec4(m[0] * c.x + m[1] * c.y + m[2] * c.z + m[3],
                                 m[4] * c.x + m[5] * c.y + m[6] * c.z + m[7],
                                 m[8] * c.x + m[9] * c.y + m[10] * c.z + m[11],
                                 std::max(0.01f, model.radius * instance.scale));
    // Phase 14: the radius 1.12 measures a doodad's range with (the model file's), the sphere's without one; < 0 for
    // the objects the server places, which keep the old range.
    m_classRadius[index] =
        instance.dynamic ? -1.0f : (model.fileRadius > 0.0f ? model.fileRadius : model.radius) * instance.scale;
    updateFadeLimits(index);
}

void WorldObjects::updateFadeLimits(u32 index) {
    // Squared horizontal distances from the camera to the sphere centre: the end of the range, and where the fade
    // begins. x < 0: no range of its own (a server object, a doodad larger than 7 yards).
    const f32 radius = m_classRadius[index];
    const DoodadRange range = radius >= 0.0f ? doodadRange(radius, m_fadeScale, m_fadeFar) : DoodadRange{};
    if (range.end <= 0.0f) {
        m_fadeLimits[index] = glm::vec2(-1.0f);
        return;
    }
    const f32 last = range.end + radius;
    const f32 opaque = std::max(0.0f, range.end - range.band + radius);
    m_fadeLimits[index] = glm::vec2(last * last, opaque * opaque);
}

std::optional<glm::vec4> WorldObjects::instanceSphere(u32 index) const {
    if (index >= m_instances.size() || !m_instances[index].alive ||
        m_modelDrawable[m_instances[index].model] == kNotDrawable) {
        return std::nullopt;
    }
    const Instance& instance = m_instances[index];
    const ModelEntry& model = m_modelEntries[instance.model];
    const f32* m = instance.transform;
    const glm::vec3 c = model.center;
    return glm::vec4(m[0] * c.x + m[1] * c.y + m[2] * c.z + m[3], m[4] * c.x + m[5] * c.y + m[6] * c.z + m[7],
                     m[8] * c.x + m[9] * c.y + m[10] * c.z + m[11],
                     std::max(0.01f, model.radius * instance.scale));
}

void WorldObjects::refreshDrawable(u32 model) {
    const ModelEntry& entry = m_modelEntries[model];
    const bool drawable = entry.state == State::Ready && entry.pendingTextures == 0 && entry.handle.valid();
    m_modelDrawable[model] = drawable ? (entry.wmo ? kDrawableWmo : kDrawableM2) : kNotDrawable;
}

void WorldObjects::computeGroupSpheres(WmoInstance& wmo) {
    const Instance& building = m_instances[wmo.instance];
    const ModelEntry& model = m_modelEntries[building.model];
    if (!wmo.groupSpheres.empty() || model.state != State::Ready || !model.wmo) {
        return;
    }
    const f32* m = building.transform;
    wmo.groupSpheres.reserve(model.groups.size());
    for (const pack::ModelGroup& g : model.groups) {
        const glm::vec3 min(g.boundsMin[0], g.boundsMin[1], g.boundsMin[2]);
        const glm::vec3 max(g.boundsMax[0], g.boundsMax[1], g.boundsMax[2]);
        const glm::vec3 c = (min + max) * 0.5f;
        const glm::vec3 center(m[0] * c.x + m[1] * c.y + m[2] * c.z + m[3], m[4] * c.x + m[5] * c.y + m[6] * c.z + m[7],
                               m[8] * c.x + m[9] * c.y + m[10] * c.z + m[11]);
        wmo.groupSpheres.emplace_back(center, std::max(0.5f, glm::length(max - min) * 0.5f * building.scale));
    }
    wmo.portalSpheres.reserve(model.portalSpheres.size());
    for (const glm::vec4& p : model.portalSpheres) {
        wmo.portalSpheres.emplace_back(m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
                                       m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                                       m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11], p.w * building.scale);
    }
}

WorldObjects::Spawn WorldObjects::spawnDoodads(WmoInstance& wmo, Clock::time_point deadline) {
    if (wmo.doodadsSpawned) {
        return Spawn::Done;
    }
    const u32 modelIndex = m_instances[wmo.instance].model;
    if (m_modelEntries[modelIndex].state == State::Requested) {
        return Spawn::NotReady; // queued again when the model arrives
    }
    if (m_modelEntries[modelIndex].state == State::Failed || !m_modelEntries[modelIndex].wmo) {
        wmo.doodadsSpawned = true;
        return Spawn::Done;
    }
    f32 wmoTransform[12];
    std::memcpy(wmoTransform, m_instances[wmo.instance].transform, sizeof(wmoTransform));
    // The default set (0) is always shown, then the placement's own. m_modelEntries is indexed afresh on every
    // doodad: acquireModel may grow it.
    for (u32 processed = 1; wmo.spawnStage < 2; ++processed) {
        const ModelEntry& model = m_modelEntries[modelIndex];
        const u32 set = wmo.spawnStage == 0 ? 0 : wmo.doodadSet;
        if ((wmo.spawnStage == 1 && set == 0) || set >= model.doodadSets.size()) {
            ++wmo.spawnStage;
            wmo.spawnNext = 0;
            continue;
        }
        const pack::ModelDoodadSet& range = model.doodadSets[set];
        const u32 end = std::min<u32>(range.first + range.count, static_cast<u32>(model.doodads.size()));
        wmo.spawnNext = std::max(wmo.spawnNext, range.first);
        if (wmo.spawnNext >= end) {
            ++wmo.spawnStage;
            wmo.spawnNext = 0;
            continue;
        }
        const u32 doodadIndex = wmo.spawnNext++;
        const pack::ModelDoodad doodad = model.doodads[doodadIndex];
        if (doodad.model != 0) {
            f32 local[12];
            doodadTransform(doodad, local);
            f32 world[12];
            multiply(wmoTransform, local, world);
            // Phase 11: in a true interior its own color (MODD) lights it, not the sun.
            std::array<u32, 2> light{};
            const u16 group =
                doodadIndex < model.doodadGroups.size() ? model.doodadGroups[doodadIndex] : pack::kModelNoGroup;
            if (interiorLighting && group < model.groups.size() && group < model.groupLighting.size() &&
                (model.groups[group].flags & (pack::kModelGroupInterior | pack::kModelGroupExteriorLit)) ==
                    pack::kModelGroupInterior) {
                light = interiorLightAt(model, wmoTransform, group, unpackRgb8(doodad.color),
                                        glm::vec3(doodad.position[0], doodad.position[1], doodad.position[2]));
            }
            const u32 instance = addInstance(acquireModel(doodad.model), world, false);
            m_instances[instance].light = light;
            m_instanceOwner[instance] = wmo.instance; // phase 18 (world): drawn with its groups, seen through portals
            m_instanceDoodad[instance] = doodadIndex;
            wmo.doodads.push_back(instance);
        }
        if (processed % 64 == 0 && Clock::now() > deadline) {
            return Spawn::Partial;
        }
    }
    wmo.doodadsSpawned = true;
    return Spawn::Done;
}

void WorldObjects::onTileLoaded(u32 tile, const u8* blob, usize size) {
    if (!isOpen() || size < sizeof(pack::TerrainTileHeader)) {
        return;
    }
    pack::TerrainTileHeader header;
    std::memcpy(&header, blob, sizeof(header));
    if (header.doodadsOffset + u64{header.doodadCount} * sizeof(pack::TerrainDoodadInstance) > size ||
        header.wmosOffset + u64{header.wmoCount} * sizeof(pack::TerrainWmoInstance) > size) {
        VD_LOG_ERROR("Tile %u: corrupt placements", tile);
        return;
    }
    TileObjects& objects = m_tiles[tile];
    for (u32 i = 0; i < header.doodadCount; ++i) {
        pack::TerrainDoodadInstance d;
        std::memcpy(&d, blob + header.doodadsOffset + i * sizeof(d), sizeof(d));
        objects.doodads.push_back(addInstance(acquireModel(d.model), d.transform, false));
    }
    for (u32 i = 0; i < header.wmoCount; ++i) {
        pack::TerrainWmoInstance w;
        std::memcpy(&w, blob + header.wmosOffset + i * sizeof(w), sizeof(w));
        objects.wmos.push_back(w.uniqueId);
        const auto [it, inserted] = m_wmos.try_emplace(w.uniqueId);
        WmoInstance& wmo = it->second;
        ++wmo.refs;
        if (inserted) {
            wmo.instance = addInstance(acquireModel(w.model), w.transform, true);
            wmo.uniqueId = w.uniqueId;
            wmo.doodadSet = w.doodadSet;
            m_wmoByInstance[wmo.instance] = &wmo;
            m_pendingSpawns.push_back(w.uniqueId); // its doodads, once the building's model is there
            computeGroupSpheres(wmo);
        }
    }
}

void WorldObjects::onTileUnloaded(u32 tile) {
    const auto it = m_tiles.find(tile);
    if (it == m_tiles.end()) {
        return;
    }
    for (u32 instance : it->second.doodads) {
        removeInstance(instance);
    }
    for (u32 uniqueId : it->second.wmos) {
        const auto wmo = m_wmos.find(uniqueId);
        if (wmo == m_wmos.end() || --wmo->second.refs > 0) {
            continue;
        }
        for (u32 doodad : wmo->second.doodads) {
            removeInstance(doodad);
        }
        m_wmoByInstance.erase(wmo->second.instance);
        removeInstance(wmo->second.instance);
        m_wmos.erase(wmo);
    }
    m_tiles.erase(it);
}

void WorldObjects::requestPending() {
    usize done = 0;
    for (; done < m_requestQueue.size(); ++done) {
        const u64 tag = m_requestQueue[done];
        const u32 index = static_cast<u32>(tag);
        std::optional<pack::PackAsset> asset;
        if (tag & kTagModel) {
            ModelEntry& entry = m_modelEntries[index];
            asset = m_models.find(entry.hash);
            if (!asset || asset->type != pack::AssetType::Model) {
                entry.state = State::Failed; // not in the pack: nothing to draw
                if (entry.refs == 0) {
                    ++entry.refs; // released through the normal path below
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
                    refreshDrawable(model);
                }
                entry.waiting.clear();
                continue;
            }
        }
        // Phase 18 (world): textures for a GPU without ASTC (desktop) are decoded on the IO thread.
        const bool decode = !(tag & kTagModel) && m_renderer && !m_renderer->nativeAstc();
        if (!m_streamer.request(tag, *asset, decode)) {
            break; // queue full: the rest waits for the next frame
        }
    }
    m_requestQueue.erase(m_requestQueue.begin(), m_requestQueue.begin() + static_cast<std::ptrdiff_t>(done));
}

void WorldObjects::onModelReady(u32 index, AssetStreamer::Done& done, Renderer& renderer) {
    ModelEntry& entry = m_modelEntries[index];
    Result<ModelHandle> handle = renderer.createModel(m_staging, done.stagingOffset, done.data, done.size);
    if (!handle) {
        VD_LOG_ERROR("Model %016llx: %s", static_cast<unsigned long long>(entry.hash), handle.error().message.c_str());
        entry.state = State::Failed;
        return;
    }
    entry.handle = handle.value();
    pack::ModelHeader header;
    std::memcpy(&header, done.data, sizeof(header));
    const glm::vec3 min(header.boundsMin[0], header.boundsMin[1], header.boundsMin[2]);
    const glm::vec3 max(header.boundsMax[0], header.boundsMax[1], header.boundsMax[2]);
    entry.center = (min + max) * 0.5f;
    entry.radius = glm::length(max - min) * 0.5f;
    entry.wmo = header.kind == static_cast<u32>(pack::ModelKind::Wmo);
    entry.fileRadius = header.fileRadius; // phase 14
    entry.readyMs = animationMs;
    loadLighting(entry, done.data, done.size, header);
    if (entry.wmo) {
        entry.skybox = u64{header.reserved2[0]} | (u64{header.reserved2[1]} << 32); // phase 11: its MOSB
        const auto* sets = reinterpret_cast<const pack::ModelDoodadSet*>(done.data + header.doodadSetsOffset);
        const auto* doodads = reinterpret_cast<const pack::ModelDoodad*>(done.data + header.doodadsOffset);
        if (header.doodadSetsOffset + u64{header.doodadSetCount} * sizeof(*sets) <= done.size &&
            header.doodadsOffset + u64{header.doodadCount} * sizeof(*doodads) <= done.size) {
            entry.doodadSets.assign(sets, sets + header.doodadSetCount);
            entry.doodads.assign(doodads, doodads + header.doodadCount);
        }
        const auto* groups = reinterpret_cast<const pack::ModelGroup*>(done.data + header.groupsOffset);
        const auto* portals = reinterpret_cast<const pack::ModelPortal*>(done.data + header.portalsOffset);
        const auto* portalVertices = reinterpret_cast<const f32*>(done.data + header.portalVerticesOffset);
        const auto* refs = reinterpret_cast<const pack::ModelPortalRef*>(done.data + header.portalRefsOffset);
        if (header.groupsOffset + u64{header.groupCount} * sizeof(*groups) <= done.size &&
            header.portalsOffset + u64{header.portalCount} * sizeof(*portals) <= done.size &&
            header.portalVerticesOffset + u64{header.portalVertexCount} * 12 <= done.size &&
            header.portalRefsOffset + u64{header.portalRefCount} * sizeof(*refs) <= done.size) {
            entry.groups.assign(groups, groups + header.groupCount);
            entry.portalRefs.assign(refs, refs + header.portalRefCount);
            entry.portalSpheres.reserve(header.portalCount);
            for (u32 p = 0; p < header.portalCount; ++p) {
                glm::vec3 low(FLT_MAX), high(-FLT_MAX);
                for (u32 v = portals[p].firstVertex;
                     v < portals[p].firstVertex + portals[p].vertexCount && v < header.portalVertexCount; ++v) {
                    const glm::vec3 point(portalVertices[v * 3], portalVertices[v * 3 + 1], portalVertices[v * 3 + 2]);
                    low = glm::min(low, point);
                    high = glm::max(high, point);
                }
                entry.portalSpheres.push_back(low.x <= high.x
                                                  ? glm::vec4((low + high) * 0.5f, glm::length(high - low) * 0.5f)
                                                  : glm::vec4(0.0f, 0.0f, 0.0f, -1.0f)); // no vertices: never seen
            }
            // A group whose portal ranges don't fit is treated as outdoors (always drawn when in view).
            for (pack::ModelGroup& g : entry.groups) {
                if (u64{g.firstPortalRef} + g.portalRefCount > entry.portalRefs.size()) {
                    g.portalRefCount = 0;
                    g.flags |= pack::kModelGroupExteriorLit;
                }
            }
            for (pack::ModelPortalRef& r : entry.portalRefs) {
                if (r.portal >= header.portalCount) {
                    r.portal = 0xFFFF;
                }
            }
            // Phase 18 (world): the planes and polygons the portal walk projects.
            entry.portals.assign(portals, portals + header.portalCount);
            entry.portalVertices.resize(header.portalVertexCount);
            std::memcpy(entry.portalVertices.data(), portalVertices, u64{header.portalVertexCount} * 12);
        }
        loadBsp(entry, done.data, done.size, header); // phase 18 (world)
    }
    entry.state = State::Ready;

    // Textures: request them; the model is drawn once they are all there.
    const auto* materials = reinterpret_cast<const pack::ModelMaterial*>(done.data + header.materialsOffset);
    std::vector<u32> textures(header.materialCount, kNone);
    for (u32 m = 0; m < header.materialCount; ++m) {
        if (materials[m].texture == 0) {
            continue;
        }
        const u32 texture = acquireTexture(materials[m].texture);
        textures[m] = texture;
        TextureEntry& t = m_textureEntries[texture];
        if (t.state == State::Ready) {
            renderer.setModelTexture(entry.handle, m, t.handle);
        } else if (t.state == State::Requested) {
            t.waiting.emplace_back(index, m);
            ++m_modelEntries[index].pendingTextures;
        }
    }
    m_modelEntries[index].textures = std::move(textures);
    if (header.effectsOffset != 0 && !m_modelEntries[index].wmo) {
        bindEffects(index, done, renderer);
    }
    if (header.skeletonOffset != 0 && !m_modelEntries[index].wmo) {
        // Phase 14: an animated doodad. Its skeleton points into the blob, kept like the effects' (small models).
        ModelEntry& animated = m_modelEntries[index];
        if (!animated.effectBlob) {
            animated.effectBlob = done.data;
            done.data = nullptr;
        }
        animated.skeleton = m_animator.add(animated.effectBlob, done.size);
        pack::ModelSkeletonHeader skeleton{};
        if (header.skeletonOffset + u64{sizeof(skeleton)} <= done.size) {
            std::memcpy(&skeleton, animated.effectBlob + header.skeletonOffset, sizeof(skeleton));
        }
        animated.skinnedMesh = animated.skeleton != DoodadAnimator::kNone && skeleton.skinOffset != 0;
        if (animated.skeleton == DoodadAnimator::kNone) {
            VD_LOG_WARN("Model %016llx: corrupt skeleton, drawn as baked", static_cast<unsigned long long>(animated.hash));
        }
    }
    refreshDrawable(index);

    if (m_modelEntries[index].wmo) {
        for (auto& [uniqueId, wmo] : m_wmos) {
            if (!wmo.doodadsSpawned && m_instances[wmo.instance].model == index) {
                m_pendingSpawns.push_back(uniqueId);
                computeGroupSpheres(wmo);
            }
        }
        for (auto& [instance, wmo] : m_dynamicWmos) {
            if (m_instances[instance].model == index) {
                computeGroupSpheres(wmo);
            }
        }
    }
}

void WorldObjects::bindEffects(u32 index, AssetStreamer::Done& done, Renderer& renderer) {
    ModelEntry& entry = m_modelEntries[index];
    entry.effects = std::make_unique<fx::EffectSet>();
    if (Result<void> bound = entry.effects->bindModel(done.data, done.size); !bound) {
        VD_LOG_ERROR("Model %016llx effects: %s", static_cast<unsigned long long>(entry.hash),
                     bound.error().message.c_str());
        entry.effects.reset();
        return;
    }
    // The effects point into the blob: keep it (models with effects are small doodads).
    entry.effectBlob = done.data;
    done.data = nullptr;
    m_modelEffects[index] = entry.effects->hasParticles() ? 1 : 0;
    if (VD_DEV_ENV("VD_TRACE_FX")) {
        // Development: what each loaded model emits.
        const std::optional<pack::PackAsset> asset = m_models.find(entry.hash);
        const std::string_view name = asset ? asset->name : std::string_view{};
        for (const pack::ParticleEmitter& e : entry.effects->emitters()) {
            VD_LOG_INFO("fx %.*s: emitter flags 0x%x blend %u shape %u style %u cells %ux%u rate %.1f life %.1f "
                        "scales %.2f/%.2f/%.2f texture %016llx reach %.1f",
                        static_cast<int>(name.size()), name.data(), e.flags, e.blend, e.shape, e.style, e.rows,
                        e.columns, e.values[pack::kEmitterRate], e.values[pack::kEmitterLifespan], e.scales[0],
                        e.scales[1], e.scales[2], static_cast<unsigned long long>(e.texture), e.reach);
        }
    }
    // Emitter textures load with the model (it is drawn once they are all in).
    const u32 emitters = static_cast<u32>(entry.effects->emitters().size());
    std::vector<u32> textures(emitters, kNone);
    for (u32 e = 0; e < emitters; ++e) {
        const u64 hash = entry.effects->emitterTexture(e);
        if (hash == 0) {
            continue;
        }
        if (!m_textures->find(hash)) {
            VD_LOG_WARN("Model %016llx: emitter %u texture %016llx not in the pack",
                        static_cast<unsigned long long>(m_modelEntries[index].hash), e,
                        static_cast<unsigned long long>(hash));
        }
        const u32 texture = acquireTexture(hash);
        textures[e] = texture;
        TextureEntry& t = m_textureEntries[texture];
        if (t.state == State::Requested) {
            t.waiting.emplace_back(index, kEffectTexture | e);
            ++m_modelEntries[index].pendingTextures;
        }
    }
    m_modelEntries[index].effectTextures = std::move(textures);
    static_cast<void>(renderer);
}

const fx::EffectSet* WorldObjects::instanceEffects(u32 instance) const {
    if (instance >= m_instances.size() || !m_instances[instance].alive) {
        return nullptr;
    }
    const ModelEntry& model = m_modelEntries[m_instances[instance].model];
    return model.effects && model.effects->valid() ? model.effects.get() : nullptr;
}

std::string_view WorldObjects::instanceModelName(u32 instance) const {
    if (instance >= m_instances.size() || !m_instances[instance].alive) {
        return {};
    }
    const std::optional<pack::PackAsset> asset = m_models.find(m_modelEntries[m_instances[instance].model].hash);
    return asset ? asset->name : std::string_view{};
}

void WorldObjects::effectTextures(u32 instance, std::span<TextureHandle> out) const {
    const ModelEntry& model = m_modelEntries[m_instances[instance].model];
    for (usize i = 0; i < out.size(); ++i) {
        const u32 texture = i < model.effectTextures.size() ? model.effectTextures[i] : kNone;
        out[i] = texture != kNone && m_textureEntries[texture].state == State::Ready ? m_textureEntries[texture].handle
                                                                                     : TextureHandle{};
    }
}

void WorldObjects::collectUvTransforms(u32 globalMs, std::span<ModelDraw> draws, std::vector<UvTransform>& out) {
    VD_PROFILE_ZONE("WorldObjects::collectUvTransforms");
    for (usize i = 0; i < draws.size() && i < m_drawModels.size(); ++i) {
        const fx::EffectSet* effects = m_modelEntries[m_drawModels[i]].effects.get();
        const usize count = effects ? effects->uvAnimations().size() : 0;
        if (count == 0 || out.size() + count > out.capacity()) {
            continue;
        }
        draws[i].firstUvTransform = static_cast<u32>(out.size());
        out.resize(out.size() + count); // capacity reserved by the owner
        const u32 local = effects->loopMs() ? globalMs % effects->loopMs() : 0;
        effects->computeUvTransforms(0, local, globalMs, out.data() + draws[i].firstUvTransform);
    }
}

void WorldObjects::collectSelfLights(u32 globalMs, std::span<ModelDraw> draws) const {
    VD_PROFILE_ZONE("WorldObjects::collectSelfLights");
    for (usize i = 0; i < draws.size() && i < m_drawModels.size(); ++i) {
        const ModelEntry& model = m_modelEntries[m_drawModels[i]];
        if (model.wmo || model.lights.empty() || draws[i].groupCount != 0) {
            continue;
        }
        // The first point light (1.12's doodads carry one: the fire of braziers and campfires).
        for (const pack::ModelLight& light : model.lights) {
            if (light.type != pack::kModelLightOmni || light.attenEnd <= 0.0f) {
                continue;
            }
            const f32 intensity = lightIntensity(light, model.lightKeys, globalMs);
            const glm::vec3 color = unpackRgb8(light.color) * intensity;
            glm::vec3 at(light.position[0], light.position[1], light.position[2]);
            if (i < m_drawPoses.size() && m_drawPoses[i] != kNone && model.skeleton != kNone &&
                light.bone < m_animator.boneCount(model.skeleton)) {
                // Phase 14: on an animated doodad it rides its bone (posed as the draw's first instance).
                const anim::BoneMatrix& m = m_animator.pose(m_drawPoses[i])[light.bone];
                at = glm::vec3(m.rows[0] * at.x + m.rows[1] * at.y + m.rows[2] * at.z + m.rows[3],
                               m.rows[4] * at.x + m.rows[5] * at.y + m.rows[6] * at.z + m.rows[7],
                               m.rows[8] * at.x + m.rows[9] * at.y + m.rows[10] * at.z + m.rows[11]);
            }
            draws[i].selfLight = {at.x,    at.y,    at.z,    light.attenEnd,
                                  color.x, color.y, color.z, light.attenStart};
            break;
        }
    }
}

void WorldObjects::onTextureReady(u32 index, const AssetStreamer::Done& done, Renderer& renderer) {
    TextureEntry& entry = m_textureEntries[index];
    Result<TextureHandle> handle = renderer.createTexture(m_staging, done.stagingOffset, done.data, done.size);
    if (handle) {
        entry.handle = handle.value();
        entry.state = State::Ready;
    } else {
        VD_LOG_ERROR("Texture %016llx: %s", static_cast<unsigned long long>(entry.hash), handle.error().message.c_str());
        entry.state = State::Failed;
    }
    for (const auto& [model, material] : entry.waiting) {
        ModelEntry& m = m_modelEntries[model];
        if (m.state == State::Ready && m.refs > 0) {
            if (entry.state == State::Ready && (material & kEffectTexture) == 0) {
                renderer.setModelTexture(m.handle, material, entry.handle);
            }
            --m.pendingTextures;
            refreshDrawable(model);
        }
    }
    entry.waiting.clear();
}

void WorldObjects::update(Renderer& renderer) {
    if (!isOpen()) {
        return;
    }
    VD_PROFILE_ZONE("WorldObjects::update");
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
        if (next.tag & kTagModel) {
            ModelEntry& entry = m_modelEntries[index];
            if (entry.refs == 0) {
                entry.state = State::Failed; // nobody wants it any more
                ++entry.refs;
                releaseModel(index);
            } else if (!next.ok) {
                entry.state = State::Failed;
            } else {
                onModelReady(index, next, renderer);
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
                    refreshDrawable(model);
                }
                entry.waiting.clear();
            } else {
                onTextureReady(index, next, renderer);
                frame = renderer.frameNumber() + 1;
            }
        }
        ++uploads;
        bytes += next.size;
        m_streamer.release(next, frame);
    }
    m_waiting.erase(m_waiting.begin(), m_waiting.begin() + static_cast<std::ptrdiff_t>(processed));
    m_streamer.update(renderer.completedFrames());

    // Doodad sets of the buildings, a slice per frame (a city has thousands of doodads).
    const Clock::time_point spawnDeadline =
        Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<f64, std::milli>(kSpawnBudgetMs));
    usize kept = 0;
    for (usize i = 0; i < m_pendingSpawns.size(); ++i) {
        const auto wmo = m_wmos.find(m_pendingSpawns[i]);
        if (wmo == m_wmos.end()) {
            continue; // unloaded meanwhile
        }
        if (Clock::now() > spawnDeadline || spawnDoodads(wmo->second, spawnDeadline) == Spawn::Partial) {
            m_pendingSpawns[kept++] = m_pendingSpawns[i];
        }
    }
    m_pendingSpawns.resize(kept);

    if (uploads > 0) {
        m_stats.lastUploadMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
        m_stats.worstUploadMs = std::max(m_stats.worstUploadMs, m_stats.lastUploadMs);
    }
    m_stats.pendingAssets = m_streamer.pending() + static_cast<u32>(m_requestQueue.size() + m_pendingSpawns.size());
    m_stats.modelsLoaded = renderer.modelCount();
    m_stats.texturesLoaded = static_cast<u32>(m_textureIndex.size());
}

void WorldObjects::collect(const glm::dvec3& camera, const Frustum& frustum, f32 drawDistance,
                           std::vector<ModelDraw>& draws, std::vector<ModelInstanceData>& instances,
                           std::vector<u16>& groups) {
    VD_PROFILE_ZONE("WorldObjects::collect");
    draws.clear();
    instances.clear();
    groups.clear();
    m_visible.clear();
    m_visibleWmos.clear();
    m_visibleEffects.clear();
    m_drawModels.clear();
    m_drawPoses.clear();
    m_stats.visibleGroups = 0;
    m_stats.totalGroups = 0;
    m_stats.portalCulledGroups = 0;
    m_stats.animatedInstances = 0;
    m_stats.fadingInstances = 0;
    if (!isOpen()) {
        return;
    }
    m_animator.beginFrame(animationMs, m_renderer->doodadBoneMatrices()); // phase 14
    m_lastCamera = camera;
    // Phase 14: the doodads' squared ranges follow the distance scale and a far clip under the largest range.
    const f32 fadeFar = std::min(drawDistance, kLargestDoodadRange * doodadDistanceScale);
    if (fadeFar != m_fadeFar || doodadDistanceScale != m_fadeScale) {
        m_fadeFar = fadeFar;
        m_fadeScale = doodadDistanceScale;
        for (u32 i = 0; i < m_spheres.size(); ++i) {
            if (m_spheres[i].w > 0.0f) {
                updateFadeLimits(i);
            }
        }
    }
    const glm::vec3 cam(static_cast<f32>(camera.x), static_cast<f32>(camera.y), static_cast<f32>(camera.z));
    const u32 count = static_cast<u32>(m_spheres.size());
    const u32* models = m_instanceModel.data();
    const u8* drawable = m_modelDrawable.data();
    // Phase 18 (world): inside a building, what its portals let through (beginView()): its own doodads with their
    // groups, and the world outside only when a portal leads out.
    const u32 cameraWmo = m_cameraLocation.instance;
    const bool inBuilding = m_cameraLocation.inside() && cameraWmo < m_instances.size();
    const ModelEntry* cameraEntry = inBuilding ? &m_modelEntries[m_instances[cameraWmo].model] : nullptr;
    for (u32 i = 0; i < count && m_visible.size() < m_visible.capacity(); ++i) {
        glm::vec4 sphere = m_spheres[i];
        const u8 kind = sphere.w < 0.0f ? kNotDrawable : drawable[models[i]];
        if (kind == kNotDrawable) {
            continue;
        }
        if (inBuilding && i != cameraWmo) {
            if (m_instanceOwner[i] == cameraWmo) {
                if (!doodadSeen(*cameraEntry, m_instanceDoodad[i])) {
                    continue;
                }
            } else if (!m_outdoorsVisible && !m_instances[i].dynamic) {
                continue; // the map's doodads and buildings (not the server's objects, drawn wherever they stand)
            }
        }
        if (sphere.w == 0.0f) {
            updateSphere(i);
            sphere = m_spheres[i];
        }
        if (m_modelEffects[models[i]] && m_visibleEffects.size() < m_visibleEffects.capacity()) {
            // Its particles reach further than its mesh (and emitter-only models have none).
            const f32* m = m_instances[i].transform;
            const glm::vec3 origin(static_cast<f32>(static_cast<f64>(m[3]) - camera.x),
                                   static_cast<f32>(static_cast<f64>(m[7]) - camera.y),
                                   static_cast<f32>(static_cast<f64>(m[11]) - camera.z));
            const f32 reach = m_modelEntries[models[i]].effects->reach() * m_instances[i].scale + 1.0f;
            const f32 limit = effectDistance + reach;
            // Phase 14: 1.12 leaves a doodad beyond its range out of the scene, and its particles with it.
            const glm::vec2 flat(sphere.x - cam.x, sphere.y - cam.y);
            const bool inRange = !fadeDoodads || m_fadeLimits[i].x < 0.0f || glm::dot(flat, flat) <= m_fadeLimits[i].x;
            if (inRange && glm::dot(origin, origin) <= limit * limit && frustum.intersectsSphere(origin, reach)) {
                m_visibleEffects.push_back(i);
            }
        }
        // Distance to the sphere <= limit, without the square root.
        const glm::vec3 relative = glm::vec3(sphere) - cam;
        f32 fade = 1.0f;
        // Phase 14 (14.10a): an M2 doodad's range and fade as 1.12 measures them (world/DoodadFade.h): horizontally,
        // from its centre minus the model file's radius, against the squared limits kept per instance (no branches on
        // its size here). The square root only inside the fade band.
        const glm::vec2 limits = m_fadeLimits[i];
        if (kind == kDrawableM2 && fadeDoodads && limits.x >= 0.0f) {
            const f32 horizontal = relative.x * relative.x + relative.y * relative.y;
            if (horizontal > limits.x || !frustum.intersectsSphere(relative, sphere.w)) {
                continue;
            }
            if (horizontal > limits.y) {
                const f32 radius = m_classRadius[i];
                fade = doodadFade(doodadRange(radius, doodadDistanceScale, m_fadeFar), std::sqrt(horizontal) - radius);
                if (fade <= 0.0f) {
                    continue;
                }
            }
        } else {
            const f32 limit = kind == kDrawableWmo || (fadeDoodads && m_classRadius[i] >= 0.0f)
                                  ? drawDistance
                                  : std::min(drawDistance, doodadBaseDistance + sphere.w * doodadDistancePerRadius);
            const f32 reach = limit + sphere.w;
            if (glm::dot(relative, relative) > reach * reach || !frustum.intersectsSphere(relative, sphere.w)) {
                continue;
            }
        }
        if (kind == kDrawableWmo) {
            m_visibleWmos.push_back(i); // drawn alone, with its visible groups
        } else {
            m_instanceFade[i] = m_classRadius[i] >= 0.0f ? std::min(fade, doodadFadeCeiling) : fade; // phase 14
            m_visible.emplace_back(models[i], i);
        }
    }
    std::sort(m_visible.begin(), m_visible.end());

    for (usize i = 0; i < m_visible.size() && instances.size() < instances.capacity();) {
        const u32 model = m_visible[i].first;
        const ModelEntry& entry = m_modelEntries[model];
        const bool skinned = animateDoodads && entry.skinnedMesh;
        ModelDraw draw;
        draw.model = entry.handle;
        draw.firstInstance = static_cast<u32>(instances.size());
        draw.doodadFlags = skinned ? ModelDraw::kDoodadSkinned : 0u;
        u32 firstPose = kNone;
        u32 fading = 0;
        for (; i < m_visible.size() && m_visible[i].first == model && instances.size() < instances.capacity(); ++i) {
            const Instance& placed = m_instances[m_visible[i].second];
            const f32* m = placed.transform;
            ModelInstanceData data;
            std::memcpy(data.rows, m, sizeof(data.rows));
            data.light[0] = placed.light[0];
            data.light[1] = placed.light[1];
            data.rows[3] = static_cast<f32>(static_cast<f64>(m[3]) - camera.x);
            data.rows[7] = static_cast<f32>(static_cast<f64>(m[7]) - camera.y);
            data.rows[11] = static_cast<f32>(static_cast<f64>(m[11]) - camera.z);
            data.fade = m_instanceFade[m_visible[i].second];
            fading += data.fade < 1.0f ? 1u : 0u;
            if (skinned) {
                // Phase 14: its phase's pose (its loop started when it, or its model, appeared).
                const f32 distance = std::sqrt(data.rows[3] * data.rows[3] + data.rows[7] * data.rows[7] +
                                               data.rows[11] * data.rows[11]);
                const u32 pose = m_animator.touch(entry.skeleton, std::max(placed.spawnMs, entry.readyMs), distance);
                const u32 base = m_animator.paletteBase(pose);
                data.boneBase = base != DoodadAnimator::kNone ? base : 0u;
                firstPose = firstPose == kNone ? pose : firstPose;
            }
            instances.push_back(data);
        }
        draw.instanceCount = static_cast<u32>(instances.size()) - draw.firstInstance;
        // Phase 14: a model with an instance in the fade band dithers its opaque parts in this one draw (the others,
        // at fade 1, never discard): one more draw per model would cost the tiled GPU more than the early depth
        // writes those parts give up.
        draw.doodadFlags |= fading > 0 ? ModelDraw::kDoodadFading : 0u;
        m_stats.animatedInstances += skinned ? draw.instanceCount : 0u;
        m_stats.fadingInstances += fading;
        draws.push_back(draw);
        m_drawModels.push_back(model);
        m_drawPoses.push_back(firstPose);
    }
    // Buildings: one draw each, restricted to the groups in view.
    for (u32 index : m_visibleWmos) {
        if (instances.size() == instances.capacity()) {
            break;
        }
        const auto wmo = m_wmoByInstance.find(index);
        if (wmo == m_wmoByInstance.end() || wmo->second->groupSpheres.empty()) {
            continue;
        }
        ModelDraw draw;
        draw.model = m_modelEntries[m_instances[index].model].handle;
        draw.firstInstance = static_cast<u32>(instances.size());
        draw.instanceCount = 1;
        draw.firstGroup = static_cast<u32>(groups.size());
        const std::vector<glm::vec4>& spheres = wmo->second->groupSpheres;
        const std::vector<glm::vec4>& portals = wmo->second->portalSpheres;
        const ModelEntry& entry = m_modelEntries[m_instances[index].model];
        m_stats.totalGroups += static_cast<u32>(spheres.size());
        // The camera in model space, for the "inside this interior" test: rows are rotation * scale.
        const f32* m = m_instances[index].transform;
        const f32 inverseScale2 = 1.0f / (m_instances[index].scale * m_instances[index].scale);
        const glm::vec3 d(static_cast<f32>(camera.x - static_cast<f64>(m[3])),
                          static_cast<f32>(camera.y - static_cast<f64>(m[7])),
                          static_cast<f32>(camera.z - static_cast<f64>(m[11])));
        const glm::vec3 local = glm::vec3(m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z,
                                          m[2] * d.x + m[6] * d.y + m[10] * d.z) *
                                inverseScale2;
        const f32 margin = interiorBoxMargin / m_instances[index].scale;
        for (u32 g = 0; g < spheres.size() && groups.size() < groups.capacity(); ++g) {
            const f32 radius = spheres[g].w;
            const glm::vec3 relative = glm::vec3(spheres[g]) - cam;
            const f32 distance = glm::length(relative) - radius;
            const f32 limit = std::min(drawDistance, groupBaseDistance + radius * groupDistancePerRadius);
            if (distance > limit || !frustum.intersectsSphere(relative, radius)) {
                continue;
            }
            const pack::ModelGroup& info = entry.groups[g];
            if (pack::modelGroupNeverDrawn(info.flags)) {
                continue;
            }
            const u32 kind = info.flags & (pack::kModelGroupInterior | pack::kModelGroupExteriorLit |
                                           pack::kModelGroupExterior);
            if (inBuilding && index == cameraWmo && g < m_walk.visible.size()) {
                // Phase 18 (world): the camera's building, the 1.12 way: the groups its portals reach, and its outdoor
                // groups while a portal leads out.
                if (!m_walk.visible[g] &&
                    !(m_outdoorsVisible && (info.flags & (pack::kModelGroupExterior | pack::kModelGroupExteriorLit)))) {
                    ++m_stats.portalCulledGroups;
                    continue;
                }
            } else if (kind == pack::kModelGroupInterior && info.portalRefCount > 0) {
                const bool inside = local.x >= info.boundsMin[0] - margin && local.x <= info.boundsMax[0] + margin &&
                                    local.y >= info.boundsMin[1] - margin && local.y <= info.boundsMax[1] + margin &&
                                    local.z >= info.boundsMin[2] - margin && local.z <= info.boundsMax[2] + margin;
                bool throughPortal = false;
                for (u32 r = info.firstPortalRef; !inside && !throughPortal && r < info.firstPortalRef + info.portalRefCount;
                     ++r) {
                    const u16 portal = entry.portalRefs[r].portal;
                    if (portal < portals.size() && portals[portal].w >= 0.0f) {
                        const glm::vec3 toPortal = glm::vec3(portals[portal]) - cam;
                        const f32 reach =
                            std::min(drawDistance, portalBaseDistance + portals[portal].w * portalDistancePerRadius) +
                            portals[portal].w;
                        throughPortal = glm::dot(toPortal, toPortal) <= reach * reach &&
                                        frustum.intersectsSphere(toPortal, portals[portal].w);
                    }
                }
                if (!inside && !throughPortal) {
                    ++m_stats.portalCulledGroups;
                    continue;
                }
            }
            groups.push_back(static_cast<u16>(g));
        }
        draw.groupCount = static_cast<u32>(groups.size()) - draw.firstGroup;
        if (draw.groupCount == 0) {
            continue;
        }
        m_stats.visibleGroups += draw.groupCount;
        m_drawModels.push_back(m_instances[index].model);
        m_drawPoses.push_back(kNone); // phase 14
        ModelInstanceData data;
        std::memcpy(data.rows, m, sizeof(data.rows));
        data.rows[3] = static_cast<f32>(static_cast<f64>(m[3]) - camera.x);
        data.rows[7] = static_cast<f32>(static_cast<f64>(m[7]) - camera.y);
        data.rows[11] = static_cast<f32>(static_cast<f64>(m[11]) - camera.z);
        instances.push_back(data);
        draws.push_back(draw);
    }
    m_stats.visibleInstances = static_cast<u32>(instances.size());
    if (traceDoodads && ++m_traceFrames % 300 == 0) { // phase 14 (VD_TRACE_DOODADS=1): every 300 frames
        const DoodadAnimator::Stats& a = m_animator.stats();
        VD_LOG_INFO("doodads: %u animated and %u fading of %u drawn | %u poses (%u sampled), %u palette matrices",
                    m_stats.animatedInstances, m_stats.fadingInstances, m_stats.visibleInstances, a.poses, a.sampled,
                    a.bones);
    }
    m_stats.modelsDrawn = static_cast<u32>(draws.size());
}

const anim::BoneMatrix* WorldObjects::instancePose(u32 index, u32& boneCount) {
    boneCount = 0;
    if (!animateDoodads || index >= m_instances.size() || !m_instances[index].alive) {
        return nullptr;
    }
    const Instance& instance = m_instances[index];
    const ModelEntry& model = m_modelEntries[instance.model];
    if (model.skeleton == kNone) {
        return nullptr;
    }
    const glm::dvec3 offset(instance.transform[3] - m_lastCamera.x, instance.transform[7] - m_lastCamera.y,
                            instance.transform[11] - m_lastCamera.z);
    const u32 pose = m_animator.touch(model.skeleton, std::max(instance.spawnMs, model.readyMs),
                                      static_cast<f32>(glm::length(offset)));
    boneCount = m_animator.boneCount(model.skeleton);
    return m_animator.pose(pose);
}

// --- Phase 18 (world): the group the camera is in, the 1.12 way --------------------------------------------------------

namespace {

constexpr f64 kLocateDepth = 1760.0; // WoW.exe 0x80fec8: how far down from the camera the client looks
constexpr f32 kPortalFirst = 1.05f;  // 0x6be411: portals are searched a little beyond the segment
constexpr f32 kPortalEpsilon = 0.0001f; // 0x80c4f4: a portal that close to the nearest face still wins

// The segment [a, b] meets the box (slabs, any direction).
bool segmentHitsBox(const glm::vec3& a, const glm::vec3& b, const f32 (&min)[3], const f32 (&max)[3]) {
    f32 t0 = 0.0f, t1 = 1.0f;
    for (u32 axis = 0; axis < 3; ++axis) {
        const f32 d = b[axis] - a[axis];
        if (std::abs(d) < 1e-9f) {
            if (a[axis] < min[axis] || a[axis] > max[axis]) {
                return false;
            }
            continue;
        }
        f32 near = (min[axis] - a[axis]) / d;
        f32 far = (max[axis] - a[axis]) / d;
        if (near > far) {
            std::swap(near, far);
        }
        t0 = std::max(t0, near);
        t1 = std::min(t1, far);
        if (t0 > t1) {
            return false;
        }
    }
    return true;
}

} // namespace

glm::vec3 WorldObjects::toModel(const Instance& instance, const glm::dvec3& p) {
    const f32* m = instance.transform;
    const glm::vec3 d(static_cast<f32>(p.x - static_cast<f64>(m[3])), static_cast<f32>(p.y - static_cast<f64>(m[7])),
                      static_cast<f32>(p.z - static_cast<f64>(m[11])));
    return glm::vec3(m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z,
                     m[2] * d.x + m[6] * d.y + m[10] * d.z) /
           (instance.scale * instance.scale);
}

void WorldObjects::loadBsp(ModelEntry& entry, const u8* blob, usize size, const pack::ModelHeader& header) {
    if (header.bspOffset == 0 || u64{header.bspOffset} + sizeof(pack::ModelBspHeader) > size) {
        return;
    }
    pack::ModelBspHeader h;
    std::memcpy(&h, blob + header.bspOffset, sizeof(h));
    auto fits = [&](u32 offset, u32 count, usize stride) { return count == 0 || u64{offset} + u64{count} * stride <= size; };
    if (h.groupCount != entry.groups.size() || !fits(h.groupsOffset, h.groupCount, sizeof(pack::ModelBspGroup)) ||
        !fits(h.nodesOffset, h.nodeCount, sizeof(pack::ModelBspNode)) || !fits(h.refsOffset, h.refCount, 2) ||
        !fits(h.trianglesOffset, h.triangleCount, 6) || !fits(h.verticesOffset, h.vertexCount, 12)) {
        return; // a corrupt section is dropped, not trusted: the boxes then decide
    }
    entry.bspGroups.resize(h.groupCount);
    std::memcpy(entry.bspGroups.data(), blob + h.groupsOffset, u64{h.groupCount} * sizeof(pack::ModelBspGroup));
    entry.bspNodes.resize(h.nodeCount);
    std::memcpy(entry.bspNodes.data(), blob + h.nodesOffset, u64{h.nodeCount} * sizeof(pack::ModelBspNode));
    entry.bspRefs.resize(h.refCount);
    std::memcpy(entry.bspRefs.data(), blob + h.refsOffset, u64{h.refCount} * 2);
    entry.bspTriangles.resize(h.triangleCount);
    std::memcpy(entry.bspTriangles.data(), blob + h.trianglesOffset, u64{h.triangleCount} * 6);
    entry.bspVertices.resize(h.vertexCount);
    std::memcpy(entry.bspVertices.data(), blob + h.verticesOffset, u64{h.vertexCount} * 12);
    if (h.doodadCount == entry.doodads.size() && fits(h.doodadRangesOffset, h.doodadCount, 8) &&
        fits(h.doodadGroupsOffset, h.doodadGroupCount, 2)) {
        entry.bspDoodadRanges.resize(h.doodadCount);
        std::memcpy(entry.bspDoodadRanges.data(), blob + h.doodadRangesOffset, u64{h.doodadCount} * 8);
        entry.bspDoodadGroups.resize(h.doodadGroupCount);
        std::memcpy(entry.bspDoodadGroups.data(), blob + h.doodadGroupsOffset, u64{h.doodadGroupCount} * 2);
        for (u16& g : entry.bspDoodadGroups) {
            g = g < entry.groups.size() ? g : pack::kModelNoGroup;
        }
    }
    for (pack::ModelBspGroup& g : entry.bspGroups) {
        if (u64{g.firstNode} + g.nodeCount > h.nodeCount || u64{g.firstRef} + g.refCount > h.refCount ||
            u64{g.firstTriangle} + g.triangleCount > h.triangleCount ||
            u64{g.firstVertex} + g.vertexCount > h.vertexCount) {
            g = pack::ModelBspGroup{}; // this group's tree is out of bounds: it holds nothing
        }
    }
}

WmoBspView WorldObjects::bspView(const ModelEntry& entry, u32 group) const {
    const pack::ModelBspGroup& g = entry.bspGroups[group];
    return {std::span<const pack::ModelBspNode>(entry.bspNodes).subspan(g.firstNode, g.nodeCount),
            std::span<const u16>(entry.bspRefs).subspan(g.firstRef, g.refCount),
            std::span<const std::array<u16, 3>>(entry.bspTriangles).subspan(g.firstTriangle, g.triangleCount),
            std::span<const glm::vec3>(entry.bspVertices).subspan(g.firstVertex, g.vertexCount)};
}

WorldObjects::Location WorldObjects::locate(const glm::dvec3& p, std::optional<f32> terrainBelow,
                                            bool exteriorOutdoors) const {
    VD_PROFILE_ZONE("WorldObjects::locate");
    Location best;
    f32 bestT = 1.0f; // the client's fraction of the segment, shared by every building (0x682264)
    bool bestExterior = false;
    for (const auto& [index, wmo] : m_wmoByInstance) {
        (void)wmo;
        const Instance& instance = m_instances[index];
        const ModelEntry& entry = m_modelEntries[instance.model];
        if (entry.state != State::Ready || !entry.wmo || entry.bspGroups.size() != entry.groups.size()) {
            continue;
        }
        const glm::vec4& bounds = m_spheres[index]; // the whole building, once culled (w <= 0: not yet)
        if (bounds.w > 0.0f) {
            const f64 dx = p.x - bounds.x, dy = p.y - bounds.y;
            if (dx * dx + dy * dy > f64{bounds.w} * bounds.w || f64{bounds.z} - bounds.w > p.z) {
                continue;
            }
        }
        const glm::vec3 from = toModel(instance, p);
        const glm::vec3 to = toModel(instance, p - glm::dvec3(0.0, 0.0, kLocateDepth));
        // 0x6b92b0 for each group the segment reaches: the nearest of its BSP's faces.
        f32 t = bestT;
        u16 group = pack::kModelNoGroup;
        for (u32 g = 0; g < entry.groups.size(); ++g) {
            if (segmentHitsBox(from, to, entry.groups[g].boundsMin, entry.groups[g].boundsMax) &&
                wmoBspSegment(bspView(entry, g), from, to, t)) {
                group = static_cast<u16>(g);
            }
        }
        // 0x6a3f80: the nearest portal crossed, if it comes before that face: the group on this side of it.
        f32 portalT = kPortalFirst;
        u16 near = pack::kModelNoGroup, far = pack::kModelNoGroup;
        for (u32 g = 0; g < entry.groups.size(); ++g) {
            const pack::ModelGroup& info = entry.groups[g];
            if (!segmentHitsBox(from, to, info.boundsMin, info.boundsMax)) {
                continue;
            }
            for (u32 r = info.firstPortalRef; r < info.firstPortalRef + info.portalRefCount && r < entry.portalRefs.size();
                 ++r) {
                const pack::ModelPortalRef& ref = entry.portalRefs[r];
                if (ref.portal >= entry.portals.size()) {
                    continue;
                }
                const pack::ModelPortal& portal = entry.portals[ref.portal];
                if (u64{portal.firstVertex} + portal.vertexCount > entry.portalVertices.size()) {
                    continue;
                }
                const glm::vec4 plane(portal.plane[0], portal.plane[1], portal.plane[2], portal.plane[3]);
                if (wmoPortalSegment(plane,
                                     std::span<const glm::vec3>(entry.portalVertices)
                                         .subspan(portal.firstVertex, portal.vertexCount),
                                     from, to, portalT)) {
                    // The ref's side says where its group lies; the point is in it on that side, beyond it otherwise.
                    const bool positive = wmoPortalSide(plane, from) >= 0.0f;
                    const bool groupSide = positive == (ref.side > 0);
                    near = groupSide ? static_cast<u16>(g) : ref.group;
                    far = groupSide ? ref.group : static_cast<u16>(g);
                }
            }
        }
        u16 other = pack::kModelNoGroup;
        if (near != pack::kModelNoGroup && portalT - t < kPortalEpsilon && near < entry.groups.size()) {
            t = portalT;
            group = near;
            other = far < entry.groups.size() && !(entry.groups[far].flags & pack::kModelGroupExterior)
                        ? far
                        : pack::kModelNoGroup;
        }
        if (group != pack::kModelNoGroup && t <= bestT) {
            bestT = t;
            best = Location{index, group, other};
            bestExterior = (entry.groups[group].flags & pack::kModelGroupExterior) != 0;
        }
    }
    // A hit in an exterior group is outdoors (0x6be477); so is terrain nearer than the building's floor (0x6822a2).
    if ((bestExterior && exteriorOutdoors) ||
        (terrainBelow && p.z >= *terrainBelow && (p.z - static_cast<f64>(*terrainBelow)) / kLocateDepth < bestT)) {
        return Location{};
    }
    return best;
}

void WorldObjects::beginView(const glm::dvec3& camera, const glm::mat4& viewProj) {
    VD_PROFILE_ZONE("WorldObjects::beginView");
    const Clock::time_point start = Clock::now();
    m_cameraLocation = {};
    m_outdoorsVisible = true;
    m_stats.cameraInside = false;
    m_stats.cameraGroup = pack::kModelNoGroup;
    m_stats.outdoorsVisible = true;
    m_stats.portalWalkGroups = 0;
    m_stats.portalsTested = 0;
    if (!isOpen() || !portalCulling) {
        return;
    }
    m_cameraLocation = locate(camera, terrainBelow(camera));
    if (m_cameraLocation.inside()) {
        const Instance& building = m_instances[m_cameraLocation.instance];
        const ModelEntry& entry = m_modelEntries[building.model];
        // Model -> camera-relative world (the rows, translation minus the camera), then the frame's projection.
        const f32* m = building.transform;
        glm::mat4 toCamera(1.0f);
        for (u32 row = 0; row < 3; ++row) {
            for (u32 column = 0; column < 3; ++column) {
                toCamera[column][row] = m[row * 4 + column];
            }
        }
        toCamera[3] = glm::vec4(static_cast<f32>(static_cast<f64>(m[3]) - camera.x),
                                static_cast<f32>(static_cast<f64>(m[7]) - camera.y),
                                static_cast<f32>(static_cast<f64>(m[11]) - camera.z), 1.0f);
        const WmoPortalGraph graph{entry.groups, entry.portalRefs, entry.portals, entry.portalVertices};
        m_walkStart[0] = m_cameraLocation.group;
        m_walkStart[1] = m_cameraLocation.other;
        const u32 starts = m_cameraLocation.other != pack::kModelNoGroup ? 2u : 1u;
        wmoWalkPortals(graph, std::span<const u16>(m_walkStart.data(), starts), toModel(building, camera),
                       viewProj * toCamera, m_walk);
        m_outdoorsVisible = m_walk.outdoors;
        m_stats.cameraInside = true;
        m_stats.cameraGroup = m_cameraLocation.group;
        m_stats.outdoorsVisible = m_walk.outdoors;
        for (const u8 v : m_walk.visible) {
            m_stats.portalWalkGroups += v;
        }
        m_stats.portalsTested = m_walk.portalsTested;
    }
    m_stats.locateMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
}

void WorldObjects::compareGroupRules(const glm::dvec3& around, f32 radius, u32 maxReports) const {
    u32 points = 0, inside = 0, differ = 0, reported = 0;
    for (const auto& [index, wmo] : m_wmoByInstance) {
        (void)wmo;
        const Instance& instance = m_instances[index];
        const ModelEntry& entry = m_modelEntries[instance.model];
        if (entry.state != State::Ready || entry.bspGroups.size() != entry.groups.size() ||
            glm::length(glm::dvec3(instance.transform[3], instance.transform[7], instance.transform[11]) - around) >
                static_cast<f64>(radius + entry.radius * instance.scale)) {
            continue;
        }
        const f32* m = instance.transform;
        // A person standing on the building's floors: over every 13th face that looks up, a yard and a half.
        for (u32 g = 0; g < entry.groups.size(); ++g) {
            const WmoBspView view = bspView(entry, g);
            for (u32 t = 0; t < view.triangles.size(); t += 13) {
                const std::array<u16, 3>& tri = view.triangles[t];
                const glm::vec3 a = view.vertices[tri[0]], b = view.vertices[tri[1]], c = view.vertices[tri[2]];
                const glm::vec3 n = glm::cross(b - a, c - a);
                if (n.z < 0.7f * glm::length(n)) {
                    continue;
                }
                const glm::vec3 local = (a + b + c) / 3.0f;
                const glm::dvec3 world(m[0] * local.x + m[1] * local.y + m[2] * local.z + m[3],
                                       m[4] * local.x + m[5] * local.y + m[6] * local.z + m[7],
                                       m[8] * local.x + m[9] * local.y + m[10] * local.z + m[11] + 1.5);
                if (glm::length(world - around) > static_cast<f64>(radius)) {
                    continue;
                }
                ++points;
                const Location where = locate(world, terrainBelow(world));
                const std::optional<GroupHit> box = findInteriorByBoxes(world);
                const bool bspInterior =
                    where.inside() && (m_modelEntries[m_instances[where.instance].model].groups[where.group].flags &
                                       (pack::kModelGroupInterior | pack::kModelGroupExteriorLit)) ==
                                          pack::kModelGroupInterior;
                inside += bspInterior ? 1u : 0u;
                const bool same = bspInterior == box.has_value() &&
                                  (!box || (box->instance == where.instance && box->group == where.group));
                if (!same) {
                    ++differ;
                    if (reported++ < maxReports) {
                        VD_LOG_INFO("group rules differ at %.2f,%.2f,%.2f: boxes %s %d, BSP %s %d (standing in group %u)",
                                    world.x, world.y, world.z, box ? "interior" : "none",
                                    box ? static_cast<i32>(box->group) : -1, bspInterior ? "interior" : "not",
                                    where.inside() ? static_cast<i32>(where.group) : -1, g);
                    }
                }
            }
        }
    }
    VD_LOG_INFO("group rules: %u points on floors, %u in true interiors by the BSP, %u where the boxes say otherwise",
                points, inside, differ);
}

bool WorldObjects::doodadSeen(const ModelEntry& building, u32 doodad) const {
    auto seen = [&](u32 g) {
        return g < m_walk.visible.size() &&
               (m_walk.visible[g] || (m_outdoorsVisible && (building.groups[g].flags & (pack::kModelGroupExterior |
                                                                                         pack::kModelGroupExteriorLit))));
    };
    if (doodad < building.bspDoodadRanges.size()) {
        const std::array<u32, 2>& range = building.bspDoodadRanges[doodad];
        for (u32 r = range[0]; r < range[0] + range[1] && r < building.bspDoodadGroups.size(); ++r) {
            if (seen(building.bspDoodadGroups[r])) {
                return true;
            }
        }
        return range[1] == 0 && m_outdoorsVisible; // listed by no group: with the world outside
    }
    const u16 group = doodad < building.doodadGroups.size() ? building.doodadGroups[doodad] : pack::kModelNoGroup;
    return group == pack::kModelNoGroup ? m_outdoorsVisible : seen(group);
}

void WorldObjects::traceCulledDoodads(const glm::dvec3& camera, f32 radius) const {
    if (!m_cameraLocation.inside()) {
        return;
    }
    char reached[512];
    usize used = 0;
    for (u32 g = 0; g < m_walk.visible.size() && used + 8 < sizeof(reached); ++g) {
        if (m_walk.visible[g]) {
            used += static_cast<usize>(std::snprintf(reached + used, sizeof(reached) - used, " %u", g));
        }
    }
    reached[used] = '\0';
    VD_LOG_INFO("portals: camera group %u (other %d), reached:%s", m_cameraLocation.group,
                m_cameraLocation.other == pack::kModelNoGroup ? -1 : static_cast<i32>(m_cameraLocation.other), reached);
    const glm::vec3 cam(camera);
    for (u32 i = 0; i < m_spheres.size(); ++i) {
        const f32* m = m_instances[i].transform;
        const glm::vec4 sphere(m[3], m[7], m[11], std::max(0.0f, m_spheres[i].w));
        if (!m_instances[i].alive || m_instances[i].wmo || glm::length(glm::vec3(sphere) - cam) > radius + sphere.w) {
            continue;
        }
        const u32 owner = m_instanceOwner[i];
        const ModelEntry& building = m_modelEntries[m_instances[m_cameraLocation.instance].model];
        const u32 doodad = m_instanceDoodad[i];
        const u16 g = doodad < building.doodadGroups.size() ? building.doodadGroups[doodad] : pack::kModelNoGroup;
        const bool culled = owner == m_cameraLocation.instance ? !doodadSeen(building, doodad)
                                                               : !m_outdoorsVisible && !m_instances[i].dynamic;
        if (culled) {
            const std::string_view name = instanceModelName(i);
            VD_LOG_INFO("culled doodad %.1f,%.1f,%.1f r %.1f %s group %d: %.*s", sphere.x, sphere.y, sphere.z, sphere.w,
                        owner == m_cameraLocation.instance ? "of the building," : "of the map,",
                        g == pack::kModelNoGroup ? -1 : static_cast<i32>(g), static_cast<int>(name.size()), name.data());
        }
    }
}

bool WorldObjects::anyBsp() const {
    for (const auto& [index, wmo] : m_wmoByInstance) {
        (void)wmo;
        if (!m_modelEntries[m_instances[index].model].bspGroups.empty()) {
            return true;
        }
    }
    return false;
}

bool WorldObjects::trueInterior(const Location& where) const {
    if (!where.inside()) {
        return false;
    }
    const pack::ModelGroup& info = m_modelEntries[m_instances[where.instance].model].groups[where.group];
    const u32 kind =
        info.flags & (pack::kModelGroupInterior | pack::kModelGroupExteriorLit | pack::kModelGroupExterior);
    return kind == pack::kModelGroupInterior && info.portalRefCount > 0; // lit by its vertex colors, with portals
}

u32 WorldObjects::groupFlags(const Location& where) const {
    return where.inside() ? m_modelEntries[m_instances[where.instance].model].groups[where.group].flags : 0u;
}

WorldObjects::Standing WorldObjects::standingNow(const glm::dvec3& feet) const {
    Standing standing;
    if (!isOpen() || !portalCulling || !anyBsp()) {
        return standing; // buildings without the BSP section (an older models.vdpk) answer by their boxes
    }
    standing.bsp = true;
    // Phase 18 (world): the group 1.12 finds under the point, not the first box that holds it; an exterior one too (not
    // an interior, but a room: a city's districts).
    const glm::dvec3 probe = feet + glm::dvec3(0.0, 0.0, kStandingProbe);
    standing.location = locate(probe, terrainBelow(probe), false);
    if (!standing.location.inside()) {
        return standing;
    }
    const Instance& instance = m_instances[standing.location.instance];
    const auto building = m_wmoByInstance.find(standing.location.instance);
    standing.placed = !instance.dynamic && building != m_wmoByInstance.end();
    standing.uniqueId = standing.placed ? building->second->uniqueId : 0u;
    standing.groupCount = static_cast<u32>(m_modelEntries[instance.model].groups.size());
    standing.interior = trueInterior(standing.location);
    return standing;
}

const WorldObjects::Standing& WorldObjects::standingAt(const glm::dvec3& feet) {
    const glm::dvec3 moved = feet - m_standingFeet;
    if (glm::dot(moved, moved) < 0.0625 && ++m_standingAge < 8) {
        return m_standing;
    }
    VD_PROFILE_ZONE("WorldObjects::standingAt");
    m_standingFeet = feet;
    m_standingAge = 0;
    m_standing = standingNow(feet);
    if (m_standing.bsp) {
        m_interiorPlace.wmo = 0;
    }
    if (m_standing.interior) {
        const Instance& instance = m_instances[m_standing.location.instance];
        m_interiorPlace.wmo = m_modelEntries[instance.model].hash; // phase 15: the indoor minimap's building
        std::copy(std::begin(instance.transform), std::end(instance.transform), m_interiorPlace.transform);
        m_interiorPlace.scale = instance.scale;
    }
    return m_standing;
}

void WorldObjects::floorSamples(const glm::dvec3& around, f32 radius, u32 stride, std::vector<FloorSample>& out) const {
    for (const auto& [index, wmo] : m_wmoByInstance) {
        const Instance& instance = m_instances[index];
        const ModelEntry& entry = m_modelEntries[instance.model];
        if (entry.state != State::Ready || instance.dynamic || entry.bspGroups.size() != entry.groups.size() ||
            glm::length(glm::dvec3(instance.transform[3], instance.transform[7], instance.transform[11]) - around) >
                static_cast<f64>(radius + entry.radius * instance.scale)) {
            continue;
        }
        const f32* m = instance.transform;
        for (u32 g = 0; g < entry.groups.size(); ++g) {
            if (!trueInterior(Location{index, static_cast<u16>(g), pack::kModelNoGroup})) {
                continue;
            }
            const WmoBspView view = bspView(entry, g);
            for (u32 t = 0; t < view.triangles.size(); t += std::max(stride, 1u)) {
                const std::array<u16, 3>& tri = view.triangles[t];
                const glm::vec3 a = view.vertices[tri[0]], b = view.vertices[tri[1]], c = view.vertices[tri[2]];
                const glm::vec3 n = glm::cross(b - a, c - a);
                if (n.z < 0.7f * glm::length(n)) {
                    continue; // not a floor
                }
                const glm::vec3 local = (a + b + c) / 3.0f;
                out.push_back({glm::dvec3(m[0] * local.x + m[1] * local.y + m[2] * local.z + m[3],
                                          m[4] * local.x + m[5] * local.y + m[6] * local.z + m[7],
                                          m[8] * local.x + m[9] * local.y + m[10] * local.z + m[11]),
                               index, static_cast<u16>(g), wmo->uniqueId,
                               static_cast<u32>(entry.groups.size())});
            }
        }
    }
}

bool WorldObjects::insideInterior(const glm::dvec3& p) {
    if (const Standing& standing = standingAt(p); standing.bsp) {
        return standing.interior;
    }
    const glm::dvec3 moved = p - m_interiorPoint;
    if (glm::dot(moved, moved) < 1.0 && ++m_interiorAge < 8) {
        return m_interiorInside;
    }
    VD_PROFILE_ZONE("WorldObjects::insideInterior");
    m_interiorPoint = p;
    m_interiorAge = 0;
    m_interiorInside = false;
    m_interiorPlace.wmo = 0;
    const glm::vec3 point(p);
    for (const auto& [index, wmo] : m_wmoByInstance) {
        const glm::vec4& bounds = m_spheres[index]; // the whole building (w <= 0: not computed yet)
        const glm::vec3 fromCenter = point - glm::vec3(bounds);
        if (bounds.w > 0.0f && glm::dot(fromCenter, fromCenter) > bounds.w * bounds.w) {
            continue;
        }
        const std::vector<glm::vec4>& spheres = wmo->groupSpheres;
        const Instance& instance = m_instances[index];
        const ModelEntry& entry = m_modelEntries[instance.model];
        if (spheres.empty() || entry.groups.size() != spheres.size()) {
            continue;
        }
        for (u32 g = 0; g < spheres.size(); ++g) {
            const pack::ModelGroup& info = entry.groups[g];
            const u32 kind = info.flags & (pack::kModelGroupInterior | pack::kModelGroupExteriorLit |
                                           pack::kModelGroupExterior);
            const glm::vec3 offset = point - glm::vec3(spheres[g]);
            if (kind != pack::kModelGroupInterior || info.portalRefCount == 0 ||
                glm::dot(offset, offset) > spheres[g].w * spheres[g].w) {
                continue;
            }
            // The point in model space (rows are rotation * scale), against the group's box, as collect() tests the
            // camera.
            const f32* m = instance.transform;
            const glm::vec3 d(static_cast<f32>(p.x - static_cast<f64>(m[3])), static_cast<f32>(p.y - static_cast<f64>(m[7])),
                              static_cast<f32>(p.z - static_cast<f64>(m[11])));
            const glm::vec3 local = glm::vec3(m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z,
                                              m[2] * d.x + m[6] * d.y + m[10] * d.z) /
                                    (instance.scale * instance.scale);
            if (local.x >= info.boundsMin[0] && local.x <= info.boundsMax[0] && local.y >= info.boundsMin[1] &&
                local.y <= info.boundsMax[1] && local.z >= info.boundsMin[2] && local.z <= info.boundsMax[2]) {
                m_interiorInside = true;
                m_interiorPlace.wmo = entry.hash; // phase 15: the indoor minimap's building
                std::copy(std::begin(instance.transform), std::end(instance.transform), m_interiorPlace.transform);
                m_interiorPlace.scale = instance.scale;
                return true;
            }
        }
    }
    return false;
}


// --- Phase 11: the light of interiors -------------------------------------------------------------------------------

void WorldObjects::loadLighting(ModelEntry& entry, const u8* blob, usize size, const pack::ModelHeader& header) {
    entry.ambient = header.ambientColor;
    if (header.lightingOffset == 0 || u64{header.lightingOffset} + sizeof(pack::ModelLightingHeader) > size) {
        return;
    }
    pack::ModelLightingHeader h;
    std::memcpy(&h, blob + header.lightingOffset, sizeof(h));
    // Each table only if it lies inside the blob (a corrupt one is dropped, not trusted).
    auto copy = [&](auto& out, u32 offset, u32 count) {
        using T = typename std::remove_reference_t<decltype(out)>::value_type;
        if (count == 0 || u64{offset} + u64{count} * sizeof(T) > size) {
            return;
        }
        out.resize(count);
        std::memcpy(out.data(), blob + offset, u64{count} * sizeof(T));
    };
    copy(entry.lights, h.lightsOffset, h.lightCount);
    copy(entry.lightKeys, h.lightKeysOffset, h.lightKeyCount);
    copy(entry.lightRefs, h.lightRefsOffset, h.lightRefCount);
    copy(entry.fogs, h.fogsOffset, h.fogCount);
    copy(entry.groupLighting, h.groupsOffset, h.groupCount);
    copy(entry.lightCells, h.cellsOffset, h.cellCount);
    copy(entry.doodadGroups, h.doodadGroupsOffset, h.doodadCount);
    for (u16& ref : entry.lightRefs) {
        ref = ref < entry.lights.size() ? ref : 0;
    }
    if (entry.lights.empty()) {
        entry.lightRefs.clear();
    }
    for (pack::ModelGroupLighting& g : entry.groupLighting) {
        if (u64{g.firstLightRef} + g.lightRefCount > entry.lightRefs.size()) {
            g.lightRefCount = 0;
        }
        for (u8& fog : g.fogs) {
            fog = fog < entry.fogs.size() ? fog : 0;
        }
    }
}

std::array<u32, 2> WorldObjects::interiorLightAt(const ModelEntry& model, const f32* m, u32 group, const glm::vec3& baked,
                                                 const glm::vec3& local) const {
    // The strongest of the group's lamps at that point (not the one it hangs from: a lantern's own light).
    const pack::ModelGroupLighting& lighting = model.groupLighting[group];
    glm::vec3 towards(0.0f);
    f32 best = 0.01f;
    for (u32 r = lighting.firstLightRef; r < lighting.firstLightRef + lighting.lightRefCount; ++r) {
        const pack::ModelLight& light = model.lights[model.lightRefs[r]];
        if (light.type == pack::kModelLightAmbient || light.type == pack::kModelLightDirectional) {
            continue;
        }
        const glm::vec3 offset = glm::vec3(light.position[0], light.position[1], light.position[2]) - local;
        const f32 distance = glm::length(offset);
        const f32 strength = distance > 0.75f ? lightStrength(light, distance) : 0.0f;
        if (strength > best) {
            best = strength;
            towards = offset / distance;
        }
    }
    // Model -> world direction (rows of the placement: rotation times scale); from above when no lamp reaches.
    glm::vec3 world(0.0f, 0.0f, 1.0f);
    if (best > 0.01f) {
        world = glm::vec3(m[0] * towards.x + m[1] * towards.y + m[2] * towards.z,
                          m[4] * towards.x + m[5] * towards.y + m[6] * towards.z,
                          m[8] * towards.x + m[9] * towards.y + m[10] * towards.z);
    }
    return packInstanceLight(interiorLight(baked, unpackRgb8(model.ambient), world));
}

std::optional<WorldObjects::GroupHit> WorldObjects::findInterior(const glm::dvec3& p) const {
    if (portalCulling) {
        // Phase 18 (world): the group 1.12 finds under the point (its BSP and portals), a true interior; the boxes
        // (below) only for buildings without the BSP section.
        const Location where = locate(p, terrainBelow(p));
        if (where.inside()) {
            const Instance& instance = m_instances[where.instance];
            const ModelEntry& entry = m_modelEntries[instance.model];
            const pack::ModelGroup& info = entry.groups[where.group];
            if ((info.flags & (pack::kModelGroupInterior | pack::kModelGroupExteriorLit)) != pack::kModelGroupInterior ||
                entry.groupLighting.size() != entry.groups.size()) {
                return std::nullopt;
            }
            return GroupHit{where.instance, where.group, toModel(instance, p)};
        }
        if (anyBsp()) {
            return std::nullopt;
        }
    }
    return findInteriorByBoxes(p);
}

std::optional<WorldObjects::GroupHit> WorldObjects::findInteriorByBoxes(const glm::dvec3& p) const {
    std::optional<GroupHit> best;
    f32 bestVolume = FLT_MAX;
    const glm::vec3 point(p);
    for (const auto& [index, wmo] : m_wmoByInstance) {
        const glm::vec4& bounds = m_spheres[index]; // the whole building (w <= 0: not computed yet)
        const glm::vec3 fromCenter = point - glm::vec3(bounds);
        if (bounds.w > 0.0f && glm::dot(fromCenter, fromCenter) > bounds.w * bounds.w) {
            continue;
        }
        const std::vector<glm::vec4>& spheres = wmo->groupSpheres;
        const Instance& instance = m_instances[index];
        const ModelEntry& entry = m_modelEntries[instance.model];
        if (spheres.empty() || entry.groups.size() != spheres.size() || entry.groupLighting.size() != spheres.size()) {
            continue;
        }
        const f32* m = instance.transform;
        const glm::vec3 d(static_cast<f32>(p.x - static_cast<f64>(m[3])), static_cast<f32>(p.y - static_cast<f64>(m[7])),
                          static_cast<f32>(p.z - static_cast<f64>(m[11])));
        const glm::vec3 local = glm::vec3(m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z,
                                          m[2] * d.x + m[6] * d.y + m[10] * d.z) /
                                (instance.scale * instance.scale);
        for (u32 g = 0; g < spheres.size(); ++g) {
            const pack::ModelGroup& info = entry.groups[g];
            const glm::vec3 offset = point - glm::vec3(spheres[g]);
            if ((info.flags & (pack::kModelGroupInterior | pack::kModelGroupExteriorLit)) != pack::kModelGroupInterior ||
                glm::dot(offset, offset) > spheres[g].w * spheres[g].w) {
                continue;
            }
            if (local.x < info.boundsMin[0] || local.x > info.boundsMax[0] || local.y < info.boundsMin[1] ||
                local.y > info.boundsMax[1] || local.z < info.boundsMin[2] || local.z > info.boundsMax[2]) {
                continue;
            }
            // Nested boxes (a room inside a hall's box): the smallest is where `p` really is.
            const f32 volume = (info.boundsMax[0] - info.boundsMin[0]) * (info.boundsMax[1] - info.boundsMin[1]) *
                               (info.boundsMax[2] - info.boundsMin[2]);
            if (volume < bestVolume) {
                bestVolume = volume;
                best = GroupHit{index, g, local};
            }
        }
    }
    return best;
}

bool WorldObjects::lightAt(const glm::dvec3& position, std::array<u32, 2>& packed) {
    if (!isOpen() || !interiorLighting) {
        return false;
    }
    VD_PROFILE_ZONE("WorldObjects::lightAt");
    // Sampled at the chest, a yard over the feet: the floor's vertex colors and the walls' around it.
    const std::optional<GroupHit> hit = findInterior(position + glm::dvec3(0.0, 0.0, 1.0));
    if (!hit) {
        return false;
    }
    const Instance& building = m_instances[hit->instance];
    const ModelEntry& model = m_modelEntries[building.model];
    const pack::ModelGroupLighting& group = model.groupLighting[hit->group];
    if (group.cellSize <= 0.0f && group.average == 0) {
        return false; // no vertex colors: nothing baked to light it with
    }
    packed = interiorLightAt(model, building.transform, hit->group, gridLight(group, model.lightCells, hit->local),
                             hit->local);
    return true;
}

std::optional<WorldObjects::FogSample> WorldObjects::fogAt(const glm::dvec3& p, bool underwater) {
    if (!isOpen() || !interiorLighting) {
        return std::nullopt;
    }
    const glm::dvec3 moved = p - m_fogPoint;
    if (glm::dot(moved, moved) < 0.0625 && underwater == m_fogUnderwater && ++m_fogAge < 8) {
        return m_fog;
    }
    VD_PROFILE_ZONE("WorldObjects::fogAt");
    m_fogPoint = p;
    m_fogUnderwater = underwater;
    m_fogAge = 0;
    m_fog.reset();
    // The camera's building and group: 1.12's rule (the group and the one across the portal the segment crossed), or
    // the boxes' interior without the BSP.
    u32 index = kNone, group = 0;
    bool interior = false;
    if (portalCulling && anyBsp()) {
        const Location where = locate(p, terrainBelow(p));
        if (where.inside()) {
            const ModelEntry& entry = m_modelEntries[m_instances[where.instance].model];
            const auto inside = [&](u16 g) {
                return g < entry.groups.size() &&
                       !(entry.groups[g].flags & (pack::kModelGroupExterior | pack::kModelGroupExteriorLit));
            };
            index = where.instance;
            group = where.group;
            interior = inside(where.group) || inside(where.other);
        }
    } else if (const std::optional<GroupHit> hit = findInteriorByBoxes(p)) {
        index = hit->instance;
        group = hit->group;
        interior = true;
    }
    if (!interior) {
        return m_fog;
    }
    const Instance& instance = m_instances[index];
    const ModelEntry& model = m_modelEntries[instance.model];
    if (model.fogs.empty() || model.groupLighting.size() != model.groups.size()) {
        return m_fog;
    }
    const InteriorFog fog = interiorFog(model.fogs, model.groupLighting[group].fogs, toModel(instance, p), underwater);
    if (fog.end <= 0.0f) {
        return m_fog;
    }
    // The underwater fogs' negative start scales: the fog starts right at the camera.
    m_fog = FogSample{fog.color, fog.end, std::clamp(fog.startScale, 0.0f, 1.0f)};
    return m_fog;
}

bool WorldObjects::segmentHitsDoodad(const glm::dvec3& from, const glm::vec3& direction, f32 length,
                                     f32 minRadius, u32 first, u32 count) const {
    const glm::vec3 origin(from);
    const usize end = std::min<usize>(m_spheres.size(), usize{first} + count);
    for (usize i = first; i < end; ++i) {
        const glm::vec4& sphere = m_spheres[i];
        if (sphere.w < minRadius || m_modelDrawable[m_instanceModel[i]] != kDrawableM2) {
            continue; // small, a free slot (w < 0), not culled yet (w = 0), or a building
        }
        const glm::vec3 toCenter = glm::vec3(sphere) - origin;
        const f32 along = glm::dot(toCenter, direction);
        if (along <= 0.0f || along > length + sphere.w) {
            continue; // behind the camera (standing beside a tree is not being under it) or past the segment
        }
        const glm::vec3 closest = toCenter - direction * std::min(along, length);
        if (glm::dot(closest, closest) < sphere.w * sphere.w) {
            return true;
        }
    }
    return false;
}

u64 WorldObjects::skyboxAt(const glm::dvec3& p) const {
    for (const auto& [index, wmo] : m_wmoByInstance) {
        const Instance& instance = m_instances[index];
        const ModelEntry& entry = m_modelEntries[instance.model];
        if (entry.skybox == 0) {
            continue;
        }
        // The point in model space (rows are rotation * scale), against every group's box.
        const f32* m = instance.transform;
        const glm::vec3 d(static_cast<f32>(p.x - static_cast<f64>(m[3])),
                          static_cast<f32>(p.y - static_cast<f64>(m[7])),
                          static_cast<f32>(p.z - static_cast<f64>(m[11])));
        const glm::vec3 local = glm::vec3(m[0] * d.x + m[4] * d.y + m[8] * d.z,
                                          m[1] * d.x + m[5] * d.y + m[9] * d.z,
                                          m[2] * d.x + m[6] * d.y + m[10] * d.z) /
                                (instance.scale * instance.scale);
        for (const pack::ModelGroup& group : entry.groups) {
            const bool inside = local.x >= group.boundsMin[0] && local.x <= group.boundsMax[0] &&
                                local.y >= group.boundsMin[1] && local.y <= group.boundsMax[1] &&
                                local.z >= group.boundsMin[2] && local.z <= group.boundsMax[2];
            if (inside) {
                return entry.skybox;
            }
        }
    }
    return 0;
}

} // namespace vd
