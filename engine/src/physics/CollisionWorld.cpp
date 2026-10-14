#include "physics/CollisionWorld.h"

#include "core/Log.h"
#include "core/Profile.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vd::physics {
namespace {

constexpr u32 kTileCount = pack::kMapTiles * pack::kMapTiles;
constexpr f32 kUnloadMargin = 64.0f; // yards beyond the load radius before a tile is dropped
constexpr u32 kQuadsPerSide = 128;   // terrain quads along a tile side

template <typename T>
bool section(u32 offset, u64 count, u32 size) {
    return count == 0 || (offset % alignof(T) == 0 && u64{offset} + count * sizeof(T) <= size);
}

bool validNodes(const pack::CollisionNode* nodes, u32 nodeCount, u32 triangleCount) {
    for (u32 i = 0; i < nodeCount; ++i) {
        const pack::CollisionNode& n = nodes[i];
        if (n.count > 0 ? u64{n.first} + n.count > triangleCount : (n.first <= i + 1 || n.first >= nodeCount)) {
            return false;
        }
    }
    return nodeCount > 0 || triangleCount == 0;
}

bool validIndices(const u32* indices, u32 triangleCount, u32 vertexCount) {
    for (u64 i = 0; i < u64{triangleCount} * 3; ++i) {
        if (indices[i] >= vertexCount) {
            return false;
        }
    }
    return true;
}

// IO thread: every offset, count and index of a tile blob in bounds (the game thread then trusts it).
bool validateTile(const u8* data, u32 size) {
    if (size < sizeof(pack::CollisionTileHeader)) {
        return false;
    }
    pack::CollisionTileHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.version != pack::kCollisionFormatVersion || h.tileX >= pack::kMapTiles || h.tileY >= pack::kMapTiles) {
        return false;
    }
    const bool terrain = (h.flags & pack::kCollisionTileHasTerrain) != 0;
    if (terrain && (!section<u16>(h.heightsOffset, pack::kTileVertices, size) || h.heightsOffset == 0 ||
                    !section<f32>(h.heightBasesOffset, pack::kTileChunks, size) || h.heightBasesOffset == 0 ||
                    !section<u16>(h.holesOffset, pack::kTileChunks, size) || h.holesOffset == 0)) {
        return false;
    }
    if (!section<f32>(h.verticesOffset, u64{h.vertexCount} * 3, size) ||
        !section<u32>(h.indicesOffset, u64{h.triangleCount} * 3, size) ||
        !section<u8>(h.triangleFlagsOffset, h.triangleCount, size) ||
        !section<pack::CollisionNode>(h.nodesOffset, h.nodeCount, size) ||
        !section<f32>(h.liquidVerticesOffset, u64{h.liquidVertexCount} * 3, size) ||
        !section<u32>(h.liquidIndicesOffset, u64{h.liquidTriangleCount} * 3, size) ||
        !section<u8>(h.liquidKindsOffset, h.liquidTriangleCount, size) ||
        !section<pack::CollisionNode>(h.liquidNodesOffset, h.liquidNodeCount, size)) {
        return false;
    }
    const auto* indices = reinterpret_cast<const u32*>(data + h.indicesOffset);
    const auto* nodes = reinterpret_cast<const pack::CollisionNode*>(data + h.nodesOffset);
    const auto* liquidIndices = reinterpret_cast<const u32*>(data + h.liquidIndicesOffset);
    const auto* liquidNodes = reinterpret_cast<const pack::CollisionNode*>(data + h.liquidNodesOffset);
    return validIndices(indices, h.triangleCount, h.vertexCount) &&
           validNodes(nodes, h.nodeCount, h.triangleCount) &&
           validIndices(liquidIndices, h.liquidTriangleCount, h.liquidVertexCount) &&
           validNodes(liquidNodes, h.liquidNodeCount, h.liquidTriangleCount);
}

bool boxesOverlap(const f32 (&aMin)[3], const f32 (&aMax)[3], const glm::vec3& bMin, const glm::vec3& bMax) {
    return aMin[0] <= bMax.x && aMax[0] >= bMin.x && aMin[1] <= bMax.y && aMax[1] >= bMin.y && aMin[2] <= bMax.z &&
           aMax[2] >= bMin.z;
}

// Square of tile (tx, ty) in world X/Y.
void tileSquare(u32 tx, u32 ty, f64& minX, f64& maxX, f64& minY, f64& maxY) {
    maxX = pack::tileCornerX(ty);
    minX = maxX - pack::kTileSize;
    maxY = pack::tileCornerY(tx);
    minY = maxY - pack::kTileSize;
}

// Tile ranges overlapping the XY box (clamped to the grid); false when the box misses the grid.
bool tileRange(f64 minX, f64 maxX, f64 minY, f64 maxY, i32& tx0, i32& tx1, i32& ty0, i32& ty1) {
    const f64 size = pack::kTileSize;
    ty0 = static_cast<i32>(std::floor(32.0 - maxX / size));
    ty1 = static_cast<i32>(std::floor(32.0 - minX / size));
    tx0 = static_cast<i32>(std::floor(32.0 - maxY / size));
    tx1 = static_cast<i32>(std::floor(32.0 - minY / size));
    if (tx1 < 0 || ty1 < 0 || tx0 >= static_cast<i32>(pack::kMapTiles) || ty0 >= static_cast<i32>(pack::kMapTiles)) {
        return false;
    }
    tx0 = std::max(tx0, 0);
    ty0 = std::max(ty0, 0);
    tx1 = std::min(tx1, static_cast<i32>(pack::kMapTiles) - 1);
    ty1 = std::min(ty1, static_cast<i32>(pack::kMapTiles) - 1);
    return true;
}

} // namespace

CollisionWorld::CollisionWorld() : m_assets(kTileCount), m_tiles(kTileCount) {
    m_state.fill(State::Absent);
    m_loaded.reserve(64);
}

CollisionWorld::~CollisionWorld() {
    closeMap();
    for (u32 index : m_loaded) { // tiles added by addTile() without a map
        delete[] m_tiles[index].data;
    }
    for (MovingObject& object : m_objects) {
        delete[] object.tile.data;
    }
}

bool CollisionWorld::tileAt(f64 x, f64 y, i32& tx, i32& ty) {
    ty = static_cast<i32>(std::floor(32.0 - x / pack::kTileSize));
    tx = static_cast<i32>(std::floor(32.0 - y / pack::kTileSize));
    return tx >= 0 && ty >= 0 && tx < static_cast<i32>(pack::kMapTiles) && ty < static_cast<i32>(pack::kMapTiles);
}

Result<u32> CollisionWorld::indexMap(const std::filesystem::path& packsDir, std::string_view directory,
                                     pack::PackReader& pack, std::vector<pack::PackAsset>& assets) {
    VD_TRY(pack.open(packsDir / pack::collisionPackFileName(directory)));
    assets.assign(kTileCount, pack::PackAsset{});
    u32 tiles = 0;
    for (u32 ty = 0; ty < pack::kMapTiles; ++ty) {
        for (u32 tx = 0; tx < pack::kMapTiles; ++tx) {
            const std::optional<pack::PackAsset> asset = pack.find(pack::collisionTilePath(directory, tx, ty));
            if (asset && asset->type == pack::AssetType::CollisionTile) {
                assets[ty * pack::kMapTiles + tx] = *asset;
                ++tiles;
            }
        }
    }
    return tiles;
}

void CollisionWorld::takeIndex(u32 tiles) {
    for (u32 index = 0; index < kTileCount; ++index) {
        m_state[index] = m_assets[index].data ? State::Unloaded : State::Absent;
    }
    m_stats = {};
    m_reader.start("vd-collision");
    VD_LOG_INFO("Collision %s: %u tiles", m_directory.c_str(), tiles);
}

Result<void> CollisionWorld::openMap(const std::filesystem::path& packsDir, std::string_view directory) {
    closeMap();
    Result<u32> tiles = indexMap(packsDir, directory, m_pack, m_assets);
    if (!tiles) {
        m_pack.close();
        std::fill(m_assets.begin(), m_assets.end(), pack::PackAsset{});
        return tiles.error();
    }
    m_directory = directory;
    m_open = true;
    takeIndex(tiles.value());
    return {};
}

void CollisionWorld::beginOpenMap(const std::filesystem::path& packsDir, std::string_view directory) {
    closeMap();
    m_directory = directory;
    m_open = true; // the map is this one; nothing is ready until the index is taken (update())
    m_indexing = true;
    m_indexed = false;
    m_indexer = std::thread([this, packsDir, dir = std::string(directory)] {
        m_indexResult = indexMap(packsDir, dir, m_indexPack, m_indexAssets);
        m_indexed.store(true, std::memory_order_release);
    });
}

void CollisionWorld::closeMap() {
    if (m_indexer.joinable()) {
        m_indexer.join(); // a few ms at most: the index of a map nobody waits for any more
    }
    m_indexing = false;
    m_indexPack.close();
    m_reader.stop();
    while (!m_loaded.empty()) {
        unload(m_loaded.back());
    }
    m_state.fill(State::Absent);
    std::fill(m_assets.begin(), m_assets.end(), pack::PackAsset{});
    m_pack.close();
    m_open = false;
    m_directory.clear();
    m_stats.residentTiles = 0;
    m_stats.loadingTiles = 0;
    m_stats.residentBytes = 0;
}

bool CollisionWorld::bind(Tile& tile, u8* data, u32 size) {
    // Validated already (validateTile, on the IO thread or in addTile): only the header is looked at again.
    if (size < sizeof(pack::CollisionTileHeader) ||
        reinterpret_cast<const pack::CollisionTileHeader*>(data)->version != pack::kCollisionFormatVersion) {
        return false;
    }
    const auto* h = reinterpret_cast<const pack::CollisionTileHeader*>(data);
    tile.data = data;
    tile.size = size;
    tile.header = h;
    const bool terrain = (h->flags & pack::kCollisionTileHasTerrain) != 0;
    tile.heights = terrain ? reinterpret_cast<const u16*>(data + h->heightsOffset) : nullptr;
    tile.heightBases = terrain ? reinterpret_cast<const f32*>(data + h->heightBasesOffset) : nullptr;
    tile.holes = terrain ? reinterpret_cast<const u16*>(data + h->holesOffset) : nullptr;
    tile.vertices = reinterpret_cast<const f32*>(data + h->verticesOffset);
    tile.indices = reinterpret_cast<const u32*>(data + h->indicesOffset);
    tile.flags = data + h->triangleFlagsOffset;
    tile.nodes = reinterpret_cast<const pack::CollisionNode*>(data + h->nodesOffset);
    tile.liquidVertices = reinterpret_cast<const f32*>(data + h->liquidVerticesOffset);
    tile.liquidIndices = reinterpret_cast<const u32*>(data + h->liquidIndicesOffset);
    tile.liquidKinds = data + h->liquidKindsOffset;
    tile.liquidNodes = reinterpret_cast<const pack::CollisionNode*>(data + h->liquidNodesOffset);
    return true;
}

void CollisionWorld::unload(u32 index) {
    Tile& tile = m_tiles[index];
    m_stats.residentBytes -= tile.size;
    delete[] tile.data;
    tile = Tile{};
    m_state[index] = m_assets[index].data ? State::Unloaded : State::Absent;
    m_loaded.erase(std::find(m_loaded.begin(), m_loaded.end(), index));
    m_stats.residentTiles = static_cast<u32>(m_loaded.size());
}

Result<void> CollisionWorld::addTile(std::vector<u8> blob) {
    if (!validateTile(blob.data(), static_cast<u32>(blob.size()))) {
        return Error{"Invalid collision tile"};
    }
    pack::CollisionTileHeader h;
    std::memcpy(&h, blob.data(), sizeof(h));
    const u32 index = u32{h.tileY} * pack::kMapTiles + h.tileX;
    if (m_state[index] == State::Loaded) {
        unload(index);
    }
    u8* data = new u8[blob.size()];
    std::memcpy(data, blob.data(), blob.size());
    bind(m_tiles[index], data, static_cast<u32>(blob.size()));
    m_state[index] = State::Loaded;
    m_loaded.push_back(index);
    m_stats.residentTiles = static_cast<u32>(m_loaded.size());
    m_stats.residentBytes += blob.size();
    return {};
}

void CollisionWorld::update(const glm::dvec3& focus, f32 radius) {
    if (!m_open) {
        return;
    }
    VD_PROFILE_ZONE("CollisionWorld::update");
    if (m_indexing) { // phase 18 (world): beginOpenMap()'s index
        if (!m_indexed.load(std::memory_order_acquire)) {
            return;
        }
        m_indexer.join();
        m_indexing = false;
        if (!m_indexResult) {
            VD_LOG_WARN("No collision for %s (vd_baker bake --only collision): %s", m_directory.c_str(),
                        m_indexResult.error().message.c_str());
            m_indexPack.close();
            closeMap(); // no collision: nothing to wait for
            return;
        }
        m_pack = std::move(m_indexPack);
        m_assets.swap(m_indexAssets);
        takeIndex(m_indexResult.value());
    }
    pack::AsyncReader::Done done;
    while (m_reader.poll(done)) {
        const u32 index = static_cast<u32>(done.tag);
        if (m_state[index] != State::Requested) {
            delete[] done.data;
            continue;
        }
        if (!done.data || !bind(m_tiles[index], done.data, done.size)) {
            delete[] done.data;
            m_state[index] = State::Absent; // broken: never asked again
            VD_LOG_ERROR("Collision tile %u_%u could not be loaded", index % pack::kMapTiles, index / pack::kMapTiles);
            continue;
        }
        m_state[index] = State::Loaded;
        m_loaded.push_back(index);
        m_stats.residentBytes += done.size;
        ++m_stats.tilesLoaded;
    }
    // Drop what is far, then ask for what is near (nearest first: the one under the focus before the others).
    for (usize i = m_loaded.size(); i-- > 0;) {
        const u32 index = m_loaded[i];
        f64 minX, maxX, minY, maxY;
        tileSquare(index % pack::kMapTiles, index / pack::kMapTiles, minX, maxX, minY, maxY);
        const f64 dx = std::max({minX - focus.x, 0.0, focus.x - maxX});
        const f64 dy = std::max({minY - focus.y, 0.0, focus.y - maxY});
        if (std::max(dx, dy) > radius + kUnloadMargin) {
            unload(index);
        }
    }
    i32 tx0, tx1, ty0, ty1;
    if (tileRange(focus.x - radius, focus.x + radius, focus.y - radius, focus.y + radius, tx0, tx1, ty0, ty1)) {
        i32 ftx, fty;
        tileAt(focus.x, focus.y, ftx, fty);
        for (u32 pass = 0; pass < 2; ++pass) { // the tile under the focus first, then its neighbors
            for (i32 ty = ty0; ty <= ty1; ++ty) {
                for (i32 tx = tx0; tx <= tx1; ++tx) {
                    if ((tx == ftx && ty == fty) != (pass == 0)) {
                        continue;
                    }
                    const u32 index = static_cast<u32>(ty) * pack::kMapTiles + static_cast<u32>(tx);
                    if (m_state[index] == State::Unloaded && m_reader.request(index, m_assets[index], validateTile)) {
                        m_state[index] = State::Requested;
                    }
                }
            }
        }
    }
    m_stats.residentTiles = static_cast<u32>(m_loaded.size());
    m_stats.loadingTiles = m_reader.pending();
}

bool CollisionWorld::ready(f64 x, f64 y, f32 radius) const {
    if (m_indexing) {
        return false; // phase 18 (world): which tiles exist is not known yet
    }
    i32 tx0, tx1, ty0, ty1;
    if (!tileRange(x - radius, x + radius, y - radius, y + radius, tx0, tx1, ty0, ty1)) {
        return true;
    }
    for (i32 ty = ty0; ty <= ty1; ++ty) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            const State state = m_state[static_cast<u32>(ty) * pack::kMapTiles + static_cast<u32>(tx)];
            if (state != State::Loaded && state != State::Absent) {
                return false;
            }
        }
    }
    return true;
}

void CollisionWorld::gatherTerrain(const Tile& tile, u32 tx, u32 ty, const glm::dvec3& min, const glm::dvec3& max,
                                   TriangleSet& out) const {
    const f64 cornerX = pack::tileCornerX(ty);
    const f64 cornerY = pack::tileCornerY(tx);
    const f64 unit = pack::kUnitSize;
    // s runs along -Y, t along -X (TerrainFormat.h).
    const f64 s0 = (cornerY - max.y) / unit, s1 = (cornerY - min.y) / unit;
    const f64 t0 = (cornerX - max.x) / unit, t1 = (cornerX - min.x) / unit;
    if (s1 < 0.0 || t1 < 0.0 || s0 >= kQuadsPerSide || t0 >= kQuadsPerSide) {
        return;
    }
    const u32 qs0 = static_cast<u32>(std::max(0.0, std::floor(s0)));
    const u32 qs1 = static_cast<u32>(std::min<f64>(kQuadsPerSide - 1, std::floor(s1)));
    const u32 qt0 = static_cast<u32>(std::max(0.0, std::floor(t0)));
    const u32 qt1 = static_cast<u32>(std::min<f64>(kQuadsPerSide - 1, std::floor(t1)));
    const glm::dvec3& origin = out.origin();
    for (u32 qt = qt0; qt <= qt1; ++qt) {
        for (u32 qs = qs0; qs <= qs1; ++qs) {
            const u32 cx = qs / 8, column = qs % 8;
            const u32 cy = qt / 8, row = qt % 8;
            const u32 chunk = cy * 16 + cx;
            if ((tile.holes[chunk] >> ((row / 2) * 4 + column / 2)) & 1u) {
                continue;
            }
            const u16* h = tile.heights + chunk * pack::kChunkVertices;
            const f32 base = tile.heightBases[chunk];
            auto height = [&](u32 v) { return base + static_cast<f32>(h[v]) * pack::kCollisionHeightStep; };
            const f32 tl = height(row * 17 + column);
            const f32 tr = height(row * 17 + column + 1);
            const f32 bl = height((row + 1) * 17 + column);
            const f32 br = height((row + 1) * 17 + column + 1);
            const f32 center = height(row * 17 + 9 + column);
            const f32 low = std::min({tl, tr, bl, br, center});
            const f32 high = std::max({tl, tr, bl, br, center});
            if (high < min.z || low > max.z) {
                continue;
            }
            auto local = [&](f64 s, f64 t, f32 z) {
                return glm::vec3(static_cast<f32>(cornerX - t * unit - origin.x),
                                 static_cast<f32>(cornerY - s * unit - origin.y), static_cast<f32>(z - origin.z));
            };
            const glm::vec3 vtl = local(qs, qt, tl);
            const glm::vec3 vtr = local(qs + 1.0, qt, tr);
            const glm::vec3 vbl = local(qs, qt + 1.0, bl);
            const glm::vec3 vbr = local(qs + 1.0, qt + 1.0, br);
            const glm::vec3 vc = local(qs + 0.5, qt + 0.5, center);
            out.add(vc, vtr, vtl, kTriangleTerrain);
            out.add(vc, vbr, vtr, kTriangleTerrain);
            out.add(vc, vbl, vbr, kTriangleTerrain);
            out.add(vc, vtl, vbl, kTriangleTerrain);
        }
    }
}

void CollisionWorld::gatherObjects(const Tile& tile, const glm::dvec3& min, const glm::dvec3& max,
                                   TriangleSet& out) const {
    const pack::CollisionTileHeader& h = *tile.header;
    if (h.nodeCount == 0) {
        return;
    }
    const glm::vec3 boxMin(min), boxMax(max);
    const glm::dvec3& origin = out.origin();
    u32 stack[pack::kCollisionMaxDepth + 2];
    u32 top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const u32 index = stack[--top];
        const pack::CollisionNode& node = tile.nodes[index];
        if (!boxesOverlap(node.boundsMin, node.boundsMax, boxMin, boxMax)) {
            continue;
        }
        if (node.count == 0) {
            if (top + 2 > std::size(stack)) {
                continue; // deeper than the baker ever writes
            }
            stack[top++] = node.first;
            stack[top++] = index + 1;
            continue;
        }
        for (u32 t = node.first; t < node.first + node.count; ++t) {
            const f32* a = tile.vertices + tile.indices[t * 3] * 3;
            const f32* b = tile.vertices + tile.indices[t * 3 + 1] * 3;
            const f32* c = tile.vertices + tile.indices[t * 3 + 2] * 3;
            const f32 triMin[3] = {std::min({a[0], b[0], c[0]}), std::min({a[1], b[1], c[1]}), std::min({a[2], b[2], c[2]})};
            const f32 triMax[3] = {std::max({a[0], b[0], c[0]}), std::max({a[1], b[1], c[1]}), std::max({a[2], b[2], c[2]})};
            if (!boxesOverlap(triMin, triMax, boxMin, boxMax)) {
                continue;
            }
            auto local = [&](const f32* p) {
                return glm::vec3(static_cast<f32>(p[0] - origin.x), static_cast<f32>(p[1] - origin.y),
                                 static_cast<f32>(p[2] - origin.z));
            };
            out.add(local(a), local(b), local(c), tile.flags[t]);
        }
    }
}

void CollisionWorld::gather(const glm::dvec3& min, const glm::dvec3& max, TriangleSet& out) const {
    for (const MovingObject& object : m_objects) {
        if (object.alive && object.placed && object.worldMin.x <= max.x && object.worldMax.x >= min.x &&
            object.worldMin.y <= max.y && object.worldMax.y >= min.y && object.worldMin.z <= max.z &&
            object.worldMax.z >= min.z) {
            gatherObject(object, min, max, out);
        }
    }
    i32 tx0, tx1, ty0, ty1;
    if (!tileRange(min.x, max.x, min.y, max.y, tx0, tx1, ty0, ty1)) {
        return;
    }
    for (i32 ty = ty0; ty <= ty1; ++ty) {
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            const u32 index = static_cast<u32>(ty) * pack::kMapTiles + static_cast<u32>(tx);
            if (m_state[index] != State::Loaded) {
                continue;
            }
            const Tile& tile = m_tiles[index];
            if (tile.heights) {
                gatherTerrain(tile, static_cast<u32>(tx), static_cast<u32>(ty), min, max, out);
            }
            gatherObjects(tile, min, max, out);
        }
    }
}

// --- Phase 8: moving objects ------------------------------------------------------------------------------------

u32 CollisionWorld::addObject(std::vector<u8> blob) {
    if (!validateTile(blob.data(), static_cast<u32>(blob.size()))) {
        return kNoObject;
    }
    u32 slot = 0;
    while (slot < m_objects.size() && m_objects[slot].alive) {
        ++slot;
    }
    if (slot == m_objects.size()) {
        m_objects.emplace_back();
    }
    MovingObject& object = m_objects[slot];
    u8* data = new u8[blob.size()];
    std::memcpy(data, blob.data(), blob.size());
    object = MovingObject{};
    bind(object.tile, data, static_cast<u32>(blob.size()));
    object.alive = true;
    ++m_objectCount;
    return slot;
}

void CollisionWorld::placeObject(u32 index, const glm::dvec3& position, f32 orientation) {
    if (index >= m_objects.size() || !m_objects[index].alive) {
        return;
    }
    MovingObject& object = m_objects[index];
    object.placed = true;
    object.position = position;
    object.cos = std::cos(orientation);
    object.sin = std::sin(orientation);
    // World bounds: the model box's corners turned (a circle around it would be looser).
    const pack::CollisionTileHeader& h = *object.tile.header;
    object.worldMin = glm::dvec3(1e30);
    object.worldMax = glm::dvec3(-1e30);
    for (u32 corner = 0; corner < 4; ++corner) {
        const f64 x = (corner & 1) ? h.boundsMax[0] : h.boundsMin[0];
        const f64 y = (corner & 2) ? h.boundsMax[1] : h.boundsMin[1];
        const glm::dvec3 world(position.x + x * object.cos - y * object.sin, position.y + y * object.cos + x * object.sin,
                               0.0);
        object.worldMin = glm::min(object.worldMin, world);
        object.worldMax = glm::max(object.worldMax, world);
    }
    object.worldMin.z = position.z + h.boundsMin[2];
    object.worldMax.z = position.z + h.boundsMax[2];
}

void CollisionWorld::removeObject(u32 index) {
    if (index >= m_objects.size() || !m_objects[index].alive) {
        return;
    }
    delete[] m_objects[index].tile.data;
    m_objects[index] = MovingObject{};
    --m_objectCount;
}

void CollisionWorld::gatherObject(const MovingObject& object, const glm::dvec3& min, const glm::dvec3& max,
                                  TriangleSet& out) const {
    const Tile& tile = object.tile;
    const pack::CollisionTileHeader& h = *tile.header;
    if (h.nodeCount == 0) {
        return;
    }
    // The query box in the model's space (its turned corners' bounds).
    glm::vec3 boxMin(1e30f), boxMax(-1e30f);
    for (u32 corner = 0; corner < 4; ++corner) {
        const f64 dx = ((corner & 1) ? max.x : min.x) - object.position.x;
        const f64 dy = ((corner & 2) ? max.y : min.y) - object.position.y;
        const glm::vec3 local(static_cast<f32>(dx * object.cos + dy * object.sin),
                              static_cast<f32>(dy * object.cos - dx * object.sin), 0.0f);
        boxMin = glm::min(boxMin, local);
        boxMax = glm::max(boxMax, local);
    }
    boxMin.z = static_cast<f32>(min.z - object.position.z);
    boxMax.z = static_cast<f32>(max.z - object.position.z);
    const glm::dvec3 offset = object.position - out.origin();
    const auto toOut = [&](const f32* p) {
        return glm::vec3(static_cast<f32>(offset.x + p[0] * object.cos - p[1] * object.sin),
                         static_cast<f32>(offset.y + p[1] * object.cos + p[0] * object.sin),
                         static_cast<f32>(offset.z + p[2]));
    };
    u32 stack[pack::kCollisionMaxDepth + 2];
    u32 top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const u32 index = stack[--top];
        const pack::CollisionNode& node = tile.nodes[index];
        if (!boxesOverlap(node.boundsMin, node.boundsMax, boxMin, boxMax)) {
            continue;
        }
        if (node.count == 0) {
            if (top + 2 > std::size(stack)) {
                continue;
            }
            stack[top++] = node.first;
            stack[top++] = index + 1;
            continue;
        }
        for (u32 t = node.first; t < node.first + node.count; ++t) {
            const f32* a = tile.vertices + tile.indices[t * 3] * 3;
            const f32* b = tile.vertices + tile.indices[t * 3 + 1] * 3;
            const f32* c = tile.vertices + tile.indices[t * 3 + 2] * 3;
            const f32 triMin[3] = {std::min({a[0], b[0], c[0]}), std::min({a[1], b[1], c[1]}), std::min({a[2], b[2], c[2]})};
            const f32 triMax[3] = {std::max({a[0], b[0], c[0]}), std::max({a[1], b[1], c[1]}), std::max({a[2], b[2], c[2]})};
            if (!boxesOverlap(triMin, triMax, boxMin, boxMax)) {
                continue;
            }
            out.add(toOut(a), toOut(b), toOut(c), static_cast<u8>(tile.flags[t] | kTriangleMoving));
        }
    }
}

u32 CollisionWorld::objectBelow(f64 x, f64 y, f64 top, f64 bottom, f64* height) const {
    u32 best = kNoObject;
    f64 bestHeight = bottom;
    for (u32 i = 0; i < m_objects.size(); ++i) {
        const MovingObject& object = m_objects[i];
        if (!object.alive || !object.placed || x < object.worldMin.x || x > object.worldMax.x ||
            y < object.worldMin.y || y > object.worldMax.y || object.worldMin.z > top || object.worldMax.z < bottom) {
            continue;
        }
        m_scratch.reset(glm::dvec3(x, y, top));
        gatherObject(object, glm::dvec3(x - 0.01, y - 0.01, bottom), glm::dvec3(x + 0.01, y + 0.01, top), m_scratch);
        Hit hit;
        if (raycast(m_scratch, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, static_cast<f32>(bottom - top)), hit)) {
            const f64 h = top + static_cast<f64>(hit.t) * (bottom - top);
            if (best == kNoObject || h > bestHeight) {
                best = i;
                bestHeight = h;
            }
        }
    }
    if (height && best != kNoObject) {
        *height = bestHeight;
    }
    return best;
}

std::optional<f64> CollisionWorld::groundHeight(f64 x, f64 y, f64 top, f64 bottom) const {
    if (top <= bottom) {
        return std::nullopt;
    }
    m_scratch.reset(glm::dvec3(x, y, top));
    gather(glm::dvec3(x - 0.01, y - 0.01, bottom), glm::dvec3(x + 0.01, y + 0.01, top), m_scratch);
    Hit hit;
    if (!raycast(m_scratch, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, static_cast<f32>(bottom - top)), hit)) {
        return std::nullopt;
    }
    return top + static_cast<f64>(hit.t) * (bottom - top);
}

std::optional<f64> CollisionWorld::terrainHeight(f64 x, f64 y, f64 top, f64 bottom) const {
    i32 tx, ty;
    if (top <= bottom || !tileAt(x, y, tx, ty)) {
        return std::nullopt;
    }
    const u32 index = static_cast<u32>(ty) * pack::kMapTiles + static_cast<u32>(tx);
    if (m_state[index] != State::Loaded || !m_tiles[index].heights) {
        return std::nullopt;
    }
    m_scratch.reset(glm::dvec3(x, y, top));
    gatherTerrain(m_tiles[index], static_cast<u32>(tx), static_cast<u32>(ty), glm::dvec3(x - 0.01, y - 0.01, bottom),
                  glm::dvec3(x + 0.01, y + 0.01, top), m_scratch);
    Hit hit;
    if (!raycast(m_scratch, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, static_cast<f32>(bottom - top)), hit)) {
        return std::nullopt;
    }
    return top + static_cast<f64>(hit.t) * (bottom - top);
}

std::optional<LiquidSample> CollisionWorld::liquidAt(f64 x, f64 y, f64 top, f64 bottom) const {
    i32 tx, ty;
    if (!tileAt(x, y, tx, ty)) {
        return std::nullopt;
    }
    const u32 index = static_cast<u32>(ty) * pack::kMapTiles + static_cast<u32>(tx);
    if (m_state[index] != State::Loaded || m_tiles[index].header->liquidNodeCount == 0) {
        return std::nullopt;
    }
    const Tile& tile = m_tiles[index];
    const f32 px = static_cast<f32>(x), py = static_cast<f32>(y);
    const f32 zMin = static_cast<f32>(bottom), zMax = static_cast<f32>(top);
    std::optional<LiquidSample> best;
    u32 stack[pack::kCollisionMaxDepth + 2];
    u32 depth = 0;
    stack[depth++] = 0;
    while (depth > 0) {
        const pack::CollisionNode& node = tile.liquidNodes[stack[--depth]];
        const u32 nodeIndex = static_cast<u32>(&node - tile.liquidNodes);
        if (px < node.boundsMin[0] || px > node.boundsMax[0] || py < node.boundsMin[1] || py > node.boundsMax[1] ||
            node.boundsMax[2] < zMin || node.boundsMin[2] > zMax) {
            continue;
        }
        if (node.count == 0) {
            if (depth + 2 <= std::size(stack)) {
                stack[depth++] = node.first;
                stack[depth++] = nodeIndex + 1;
            }
            continue;
        }
        for (u32 t = node.first; t < node.first + node.count; ++t) {
            const f32* a = tile.liquidVertices + tile.liquidIndices[t * 3] * 3;
            const f32* b = tile.liquidVertices + tile.liquidIndices[t * 3 + 1] * 3;
            const f32* c = tile.liquidVertices + tile.liquidIndices[t * 3 + 2] * 3;
            // Barycentric coordinates of (x, y) in the triangle's XY projection.
            const f32 det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
            if (std::abs(det) < 1e-9f) {
                continue;
            }
            const f32 u = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / det;
            const f32 v = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / det;
            const f32 w = 1.0f - u - v;
            if (u < -1e-4f || v < -1e-4f || w < -1e-4f) {
                continue;
            }
            const f32 z = u * a[2] + v * b[2] + w * c[2];
            if (z < zMin || z > zMax || (best && best->level >= z)) {
                continue;
            }
            best = LiquidSample{z, tile.liquidKinds[t]};
        }
    }
    return best;
}

} // namespace vd::physics
