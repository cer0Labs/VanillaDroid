#include "bake/CollisionBaker.h"

#include "bake/BakeUtil.h"
#include "bake/Portable.h"
#include "core/Hash.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>

namespace vd::bake {
namespace {

using Vec3 = std::array<f32, 3>;

// MOPY triangle flags (vMaNGOS contrib/vmap_extractor/vmapextract/wmo.h).
constexpr u8 kMopyNoCamera = 0x02;
constexpr u8 kMopyDetail = 0x04;
constexpr u8 kMopyCollision = 0x08;
constexpr u8 kMopyRender = 0x20;

constexpr u32 kSahBins = 16;

Vec3 apply(const std::array<f32, 12>& m, const Vec3& p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

} // namespace

// WMO doodad placement: translate * rotate(quaternion x, y, z, w) * scale (as the runtime places them).
std::array<f32, 12> doodadTransform(const formats::WmoDoodad& d) {
    const f32 x = d.rotation[0], y = d.rotation[1], z = d.rotation[2], w = d.rotation[3];
    const f32 s = d.scale;
    const f32 r[9] = {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - z * w),        2.0f * (x * z + y * w),
                      2.0f * (x * y + z * w),        1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - x * w),
                      2.0f * (x * z - y * w),        2.0f * (y * z + x * w),        1.0f - 2.0f * (x * x + y * y)};
    std::array<f32, 12> out{};
    for (u32 row = 0; row < 3; ++row) {
        out[row * 4 + 0] = r[row * 3 + 0] * s;
        out[row * 4 + 1] = r[row * 3 + 1] * s;
        out[row * 4 + 2] = r[row * 3 + 2] * s;
        out[row * 4 + 3] = d.position[row];
    }
    return out;
}

namespace {

f32 triangleArea2(const Vec3& a, const Vec3& b, const Vec3& c) {
    const f32 u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const f32 v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const f32 n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    return std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
}

// Merges bit-identical vertices, drops unused ones and degenerate triangles (zero area or repeated corners).
void weld(TriangleSoup& soup) {
    struct Key {
        u32 bits[3];
        bool operator==(const Key& o) const { return std::memcmp(bits, o.bits, sizeof(bits)) == 0; }
    };
    struct KeyHash {
        usize operator()(const Key& k) const { return static_cast<usize>(fnv1a64(k.bits, sizeof(k.bits))); }
    };
    std::unordered_map<Key, u32, KeyHash> index;
    index.reserve(soup.positions.size());
    std::vector<Vec3> positions;
    positions.reserve(soup.positions.size());
    std::vector<u32> remap(soup.positions.size(), ~0u);
    std::vector<u32> indices;
    std::vector<u8> tags;
    indices.reserve(soup.indices.size());
    tags.reserve(soup.tags.size());
    auto vertex = [&](u32 v) {
        if (remap[v] == ~0u) {
            Key key;
            std::memcpy(key.bits, soup.positions[v].data(), sizeof(key.bits));
            const auto [it, added] = index.emplace(key, static_cast<u32>(positions.size()));
            if (added) {
                positions.push_back(soup.positions[v]);
            }
            remap[v] = it->second;
        }
        return remap[v];
    };
    for (u32 t = 0; t < soup.triangleCount(); ++t) {
        const Vec3& a = soup.positions[soup.indices[t * 3]];
        const Vec3& b = soup.positions[soup.indices[t * 3 + 1]];
        const Vec3& c = soup.positions[soup.indices[t * 3 + 2]];
        if (triangleArea2(a, b, c) < 1e-6f) {
            continue;
        }
        const u32 i0 = vertex(soup.indices[t * 3]);
        const u32 i1 = vertex(soup.indices[t * 3 + 1]);
        const u32 i2 = vertex(soup.indices[t * 3 + 2]);
        if (i0 == i1 || i1 == i2 || i0 == i2) {
            continue;
        }
        indices.insert(indices.end(), {i0, i1, i2});
        tags.push_back(soup.tags[t]);
    }
    soup.positions = std::move(positions);
    soup.indices = std::move(indices);
    soup.tags = std::move(tags);
}

// Terrain MCLQ -> liquid triangles. A chunk entirely covered by a flat liquid (lakes, the sea) becomes two
// triangles; otherwise two per quad with liquid (low nibble of the quad flags != 0xF).
void appendTerrainLiquids(const formats::Adt& adt, u32 tx, u32 ty, TriangleSoup& out) {
    const f32 cornerX = pack::tileCornerX(ty);
    const f32 cornerY = pack::tileCornerY(tx);
    for (u32 c = 0; c < pack::kTileChunks; ++c) {
        const u32 cx = c % 16;
        const u32 cy = c / 16;
        for (const formats::AdtLiquid& liquid : adt.chunks[c].liquids) {
            const f32 fallback = liquid.minHeight < 1e30f ? (liquid.minHeight + liquid.maxHeight) * 0.5f : 0.0f;
            auto height = [&](u32 column, u32 row) {
                const f32 h = liquid.heights[row * pack::kLiquidGrid + column];
                return h < 1e30f ? h : fallback;
            };
            auto point = [&](f32 column, f32 row, f32 h) {
                const f32 s = (static_cast<f32>(cx * 8) + column) * pack::kUnitSize;
                const f32 t = (static_cast<f32>(cy * 8) + row) * pack::kUnitSize;
                return Vec3{cornerX - t, cornerY - s, h};
            };
            u32 quads = 0;
            f32 low = FLT_MAX, high = -FLT_MAX;
            for (u32 quad = 0; quad < 64; ++quad) {
                quads += (liquid.tiles[quad] & 0x0F) != 0x0F ? 1 : 0;
            }
            if (quads == 0) {
                continue;
            }
            for (u32 v = 0; v < pack::kLiquidGrid * pack::kLiquidGrid; ++v) {
                const f32 h = height(v % pack::kLiquidGrid, v / pack::kLiquidGrid);
                low = std::min(low, h);
                high = std::max(high, h);
            }
            const u8 kind = static_cast<u8>(liquid.type);
            if (quads == 64 && high - low < 0.01f) {
                const u32 base = static_cast<u32>(out.positions.size());
                out.positions.push_back(point(0.0f, 0.0f, low));
                out.positions.push_back(point(8.0f, 0.0f, low));
                out.positions.push_back(point(0.0f, 8.0f, low));
                out.positions.push_back(point(8.0f, 8.0f, low));
                out.indices.insert(out.indices.end(), {base, base + 2, base + 3, base, base + 3, base + 1});
                out.tags.insert(out.tags.end(), {kind, kind});
                continue;
            }
            const u32 base = static_cast<u32>(out.positions.size());
            for (u32 row = 0; row < pack::kLiquidGrid; ++row) {
                for (u32 column = 0; column < pack::kLiquidGrid; ++column) {
                    out.positions.push_back(
                        point(static_cast<f32>(column), static_cast<f32>(row), height(column, row)));
                }
            }
            for (u32 quad = 0; quad < 64; ++quad) {
                if ((liquid.tiles[quad] & 0x0F) == 0x0F) {
                    continue;
                }
                const u32 tl = base + (quad / 8) * pack::kLiquidGrid + quad % 8;
                const u32 tr = tl + 1;
                const u32 bl = tl + pack::kLiquidGrid;
                const u32 br = bl + 1;
                out.indices.insert(out.indices.end(), {tl, bl, br, tl, br, tr});
                out.tags.insert(out.tags.end(), {kind, kind});
            }
        }
    }
}

struct BvhItem {
    f32 min[3];
    f32 max[3];
    f32 center[3];
};

struct BvhBuilder {
    const std::vector<BvhItem>& items;
    std::vector<u32>& order;
    std::vector<pack::CollisionNode>& nodes;
    u32 depth = 0;

    static f32 area(const f32 (&min)[3], const f32 (&max)[3]) {
        const f32 x = std::max(0.0f, max[0] - min[0]);
        const f32 y = std::max(0.0f, max[1] - min[1]);
        const f32 z = std::max(0.0f, max[2] - min[2]);
        return x * y + y * z + z * x;
    }

    u32 build(u32 begin, u32 end, u32 level) {
        const u32 nodeIndex = static_cast<u32>(nodes.size());
        nodes.push_back({});
        depth = std::max(depth, level + 1);
        f32 boundsMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        f32 boundsMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        f32 centerMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        f32 centerMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (u32 i = begin; i < end; ++i) {
            const BvhItem& item = items[order[i]];
            for (u32 a = 0; a < 3; ++a) {
                boundsMin[a] = std::min(boundsMin[a], item.min[a]);
                boundsMax[a] = std::max(boundsMax[a], item.max[a]);
                centerMin[a] = std::min(centerMin[a], item.center[a]);
                centerMax[a] = std::max(centerMax[a], item.center[a]);
            }
        }
        std::memcpy(nodes[nodeIndex].boundsMin, boundsMin, sizeof(boundsMin));
        std::memcpy(nodes[nodeIndex].boundsMax, boundsMax, sizeof(boundsMax));
        const u32 count = end - begin;
        if (count <= pack::kCollisionLeafTriangles || level + 2 >= pack::kCollisionMaxDepth) {
            nodes[nodeIndex].first = begin;
            nodes[nodeIndex].count = count;
            return nodeIndex;
        }
        u32 axis = 0;
        for (u32 a = 1; a < 3; ++a) {
            if (centerMax[a] - centerMin[a] > centerMax[axis] - centerMin[axis]) {
                axis = a;
            }
        }
        const f32 extent = centerMax[axis] - centerMin[axis];
        u32 mid = begin + count / 2;
        if (extent > 1e-5f) {
            // Binned surface area heuristic along the widest axis of the centers.
            struct Bin {
                u32 count = 0;
                f32 min[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
                f32 max[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
            };
            Bin bins[kSahBins];
            auto binOf = [&](const BvhItem& item) {
                const f32 f = (item.center[axis] - centerMin[axis]) / extent * static_cast<f32>(kSahBins);
                return std::min(kSahBins - 1, static_cast<u32>(std::max(0.0f, f)));
            };
            for (u32 i = begin; i < end; ++i) {
                const BvhItem& item = items[order[i]];
                Bin& bin = bins[binOf(item)];
                ++bin.count;
                for (u32 a = 0; a < 3; ++a) {
                    bin.min[a] = std::min(bin.min[a], item.min[a]);
                    bin.max[a] = std::max(bin.max[a], item.max[a]);
                }
            }
            f32 rightArea[kSahBins] = {};
            u32 rightCount[kSahBins] = {};
            Bin accumulated;
            for (u32 b = kSahBins - 1; b > 0; --b) {
                accumulated.count += bins[b].count;
                for (u32 a = 0; a < 3; ++a) {
                    accumulated.min[a] = std::min(accumulated.min[a], bins[b].min[a]);
                    accumulated.max[a] = std::max(accumulated.max[a], bins[b].max[a]);
                }
                rightArea[b] = area(accumulated.min, accumulated.max);
                rightCount[b] = accumulated.count;
            }
            accumulated = Bin{};
            f32 bestCost = FLT_MAX;
            u32 bestSplit = 0;
            for (u32 b = 0; b + 1 < kSahBins; ++b) {
                accumulated.count += bins[b].count;
                for (u32 a = 0; a < 3; ++a) {
                    accumulated.min[a] = std::min(accumulated.min[a], bins[b].min[a]);
                    accumulated.max[a] = std::max(accumulated.max[a], bins[b].max[a]);
                }
                if (accumulated.count == 0 || rightCount[b + 1] == 0) {
                    continue;
                }
                const f32 cost = static_cast<f32>(accumulated.count) * area(accumulated.min, accumulated.max) +
                                 static_cast<f32>(rightCount[b + 1]) * rightArea[b + 1];
                if (cost < bestCost) {
                    bestCost = cost;
                    bestSplit = b + 1;
                }
            }
            // Phase 22: the same partition on every standard library (bake/Portable.h).
            if (bestSplit > 0) {
                mid = static_cast<u32>(
                    portablePartition(order.begin() + begin, order.begin() + end,
                                      [&](u32 i) { return binOf(items[i]) < bestSplit; }) -
                    order.begin());
            }
            if (mid == begin || mid == end) {
                mid = begin + count / 2;
                // A whole sort, the index breaking ties: std::nth_element leaves each half in an order of its own on
                // every standard library.
                std::sort(order.begin() + begin, order.begin() + end, [&](u32 a, u32 b) {
                    const f32 ca = items[a].center[axis], cb = items[b].center[axis];
                    return ca != cb ? ca < cb : a < b;
                });
            }
        }
        build(begin, mid, level + 1); // the first child follows its parent
        const u32 second = build(mid, end, level + 1);
        nodes[nodeIndex].first = second;
        nodes[nodeIndex].count = 0;
        return nodeIndex;
    }
};

void extend(f32 (&min)[3], f32 (&max)[3], const Vec3& p) {
    for (u32 a = 0; a < 3; ++a) {
        min[a] = std::min(min[a], p[a]);
        max[a] = std::max(max[a], p[a]);
    }
}

} // namespace

WmoCollision wmoCollision(const formats::WmoRoot& root, const std::vector<formats::WmoGroup>& groups) {
    WmoCollision out;
    for (const formats::WmoGroup& group : groups) {
        std::vector<u32> remap(group.positions.size(), ~0u);
        const usize triangles = std::min(group.indices.size() / 3, group.triangleFlags.size());
        for (usize t = 0; t < triangles; ++t) {
            const u8 flags = group.triangleFlags[t];
            const bool renderFace = (flags & kMopyRender) && !(flags & kMopyDetail);
            if (!(flags & kMopyCollision) && !renderFace) {
                continue;
            }
            const u16 corners[3] = {group.indices[t * 3], group.indices[t * 3 + 1], group.indices[t * 3 + 2]};
            if (corners[0] >= group.positions.size() || corners[1] >= group.positions.size() ||
                corners[2] >= group.positions.size()) {
                continue;
            }
            for (u16 v : corners) {
                if (remap[v] == ~0u) {
                    remap[v] = static_cast<u32>(out.solid.positions.size());
                    out.solid.positions.push_back(group.positions[v]);
                }
                out.solid.indices.push_back(remap[v]);
            }
            out.solid.tags.push_back((flags & kMopyNoCamera) ? pack::kCollisionNoCamera : 0);
        }
        const formats::WmoLiquidKind kind = formats::wmoLiquidKind(root, group);
        const formats::WmoLiquid& liquid = group.liquid;
        if (kind == formats::WmoLiquidKind::None || liquid.tilesX == 0 || liquid.tilesY == 0 ||
            liquid.heights.size() < usize{liquid.vertsX} * liquid.vertsY ||
            liquid.tiles.size() < usize{liquid.tilesX} * liquid.tilesY || liquid.vertsX < liquid.tilesX + 1 ||
            liquid.vertsY < liquid.tilesY + 1) {
            continue;
        }
        const u32 base = static_cast<u32>(out.liquid.positions.size());
        for (u32 y = 0; y < liquid.vertsY; ++y) {
            for (u32 x = 0; x < liquid.vertsX; ++x) {
                out.liquid.positions.push_back({liquid.corner[0] + static_cast<f32>(x) * formats::kWmoLiquidTileSize,
                                                liquid.corner[1] + static_cast<f32>(y) * formats::kWmoLiquidTileSize,
                                                liquid.heights[y * liquid.vertsX + x]});
            }
        }
        for (u32 y = 0; y < liquid.tilesY; ++y) {
            for (u32 x = 0; x < liquid.tilesX; ++x) {
                if ((liquid.tiles[y * liquid.tilesX + x] & 0x0F) == formats::kWmoLiquidTileEmpty) {
                    continue;
                }
                const u32 v00 = base + y * liquid.vertsX + x;
                const u32 v10 = v00 + 1;
                const u32 v01 = v00 + liquid.vertsX;
                const u32 v11 = v01 + 1;
                out.liquid.indices.insert(out.liquid.indices.end(), {v00, v01, v10, v10, v01, v11});
                out.liquid.tags.insert(out.liquid.tags.end(), {static_cast<u8>(kind), static_cast<u8>(kind)});
            }
        }
    }
    out.sets = root.doodadSets;
    for (const formats::WmoDoodad& d : root.doodads) {
        out.doodads.push_back({d.model.empty() ? std::string() : normalizeAssetPath(formats::m2Path(d.model)),
                               doodadTransform(d)});
    }
    return out;
}

TriangleSoup m2Collision(const formats::M2Model& model) {
    TriangleSoup out;
    out.positions = model.collisionPositions;
    out.indices.assign(model.collisionIndices.begin(), model.collisionIndices.end());
    out.tags.assign(out.indices.size() / 3, pack::kCollisionDoodad);
    return out;
}

std::array<f32, 12> multiplyTransforms(const std::array<f32, 12>& a, const std::array<f32, 12>& b) {
    std::array<f32, 12> out{};
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            f32 sum = column == 3 ? a[row * 4 + 3] : 0.0f;
            for (u32 k = 0; k < 3; ++k) {
                sum += a[row * 4 + k] * b[k * 4 + column];
            }
            out[row * 4 + column] = sum;
        }
    }
    return out;
}

bool transformedBounds(const TriangleSoup& soup, const std::array<f32, 12>& transform, std::array<f32, 3>& min,
                       std::array<f32, 3>& max) {
    if (soup.empty()) {
        return false;
    }
    min = {FLT_MAX, FLT_MAX, FLT_MAX};
    max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const Vec3& p : soup.positions) {
        const Vec3 w = apply(transform, p);
        for (u32 a = 0; a < 3; ++a) {
            min[a] = std::min(min[a], w[a]);
            max[a] = std::max(max[a], w[a]);
        }
    }
    return true;
}

void appendTransformed(const TriangleSoup& soup, const std::array<f32, 12>& transform, const std::array<f32, 2>& xyMin,
                       const std::array<f32, 2>& xyMax, TriangleSoup& out) {
    std::vector<Vec3> world(soup.positions.size());
    for (usize v = 0; v < world.size(); ++v) {
        world[v] = apply(transform, soup.positions[v]);
    }
    std::vector<u32> remap(world.size(), ~0u);
    for (u32 t = 0; t < soup.triangleCount(); ++t) {
        const Vec3& a = world[soup.indices[t * 3]];
        const Vec3& b = world[soup.indices[t * 3 + 1]];
        const Vec3& c = world[soup.indices[t * 3 + 2]];
        const f32 minX = std::min({a[0], b[0], c[0]}), maxX = std::max({a[0], b[0], c[0]});
        const f32 minY = std::min({a[1], b[1], c[1]}), maxY = std::max({a[1], b[1], c[1]});
        if (maxX < xyMin[0] || minX > xyMax[0] || maxY < xyMin[1] || minY > xyMax[1]) {
            continue;
        }
        for (u32 k = 0; k < 3; ++k) {
            const u32 v = soup.indices[t * 3 + k];
            if (remap[v] == ~0u) {
                remap[v] = static_cast<u32>(out.positions.size());
                out.positions.push_back(world[v]);
            }
            out.indices.push_back(remap[v]);
        }
        out.tags.push_back(soup.tags[t]);
    }
}

std::vector<pack::CollisionNode> buildCollisionBvh(const std::vector<std::array<f32, 3>>& positions,
                                                   std::vector<u32>& indices, std::vector<u8>& tags, u32* depth) {
    std::vector<pack::CollisionNode> nodes;
    const u32 count = static_cast<u32>(indices.size() / 3);
    if (depth) {
        *depth = 0;
    }
    if (count == 0) {
        return nodes;
    }
    std::vector<BvhItem> items(count);
    for (u32 t = 0; t < count; ++t) {
        BvhItem& item = items[t];
        for (u32 a = 0; a < 3; ++a) {
            const f32 p0 = positions[indices[t * 3]][a];
            const f32 p1 = positions[indices[t * 3 + 1]][a];
            const f32 p2 = positions[indices[t * 3 + 2]][a];
            item.min[a] = std::min({p0, p1, p2});
            item.max[a] = std::max({p0, p1, p2});
            item.center[a] = (item.min[a] + item.max[a]) * 0.5f;
        }
    }
    std::vector<u32> order(count);
    std::iota(order.begin(), order.end(), 0u);
    nodes.reserve(count / 2 + 1);
    BvhBuilder builder{items, order, nodes};
    builder.build(0, count, 0);
    std::vector<u32> sortedIndices(indices.size());
    std::vector<u8> sortedTags(count);
    for (u32 i = 0; i < count; ++i) {
        std::memcpy(&sortedIndices[i * 3], &indices[order[i] * 3], 3 * sizeof(u32));
        sortedTags[i] = tags[order[i]];
    }
    indices = std::move(sortedIndices);
    tags = std::move(sortedTags);
    if (depth) {
        *depth = builder.depth;
    }
    return nodes;
}

std::vector<u8> bakeCollisionTile(const CollisionTileInput& input, CollisionTileStats* stats) {
    pack::CollisionTileHeader header{};
    header.version = pack::kCollisionFormatVersion;
    header.tileX = static_cast<u16>(input.tx);
    header.tileY = static_cast<u16>(input.ty);
    f32 boundsMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    f32 boundsMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    std::vector<f32> heightBases;
    std::vector<u16> heights;
    std::vector<u16> holes;
    TriangleSoup liquid = input.liquid;
    if (input.adt && input.adt->chunks.size() == pack::kTileChunks) {
        header.flags |= pack::kCollisionTileHasTerrain;
        heightBases.resize(pack::kTileChunks);
        heights.resize(pack::kTileVertices);
        holes.resize(pack::kTileChunks);
        const f32 cornerX = pack::tileCornerX(input.ty);
        const f32 cornerY = pack::tileCornerY(input.tx);
        f32 low = FLT_MAX, high = -FLT_MAX;
        for (u32 c = 0; c < pack::kTileChunks; ++c) {
            const formats::AdtChunk& chunk = input.adt->chunks[c];
            holes[c] = chunk.holes;
            const f32 base = chunk.position[2] + *std::min_element(chunk.heights.begin(), chunk.heights.end());
            heightBases[c] = base;
            for (u32 v = 0; v < pack::kChunkVertices; ++v) {
                const f32 h = chunk.position[2] + chunk.heights[v];
                const f32 steps = std::round((h - base) / pack::kCollisionHeightStep);
                heights[c * pack::kChunkVertices + v] = static_cast<u16>(std::clamp(steps, 0.0f, 65535.0f));
                low = std::min(low, h);
                high = std::max(high, h);
            }
        }
        extend(boundsMin, boundsMax, {cornerX - pack::kTileSize, cornerY - pack::kTileSize, low});
        extend(boundsMin, boundsMax, {cornerX, cornerY, high});
        appendTerrainLiquids(*input.adt, input.tx, input.ty, liquid);
    }

    TriangleSoup solid = input.solid;
    weld(solid);
    weld(liquid);
    u32 depth = 0, liquidDepth = 0;
    const std::vector<pack::CollisionNode> nodes = buildCollisionBvh(solid.positions, solid.indices, solid.tags, &depth);
    const std::vector<pack::CollisionNode> liquidNodes =
        buildCollisionBvh(liquid.positions, liquid.indices, liquid.tags, &liquidDepth);
    for (const Vec3& p : solid.positions) {
        extend(boundsMin, boundsMax, p);
    }
    for (const Vec3& p : liquid.positions) {
        extend(boundsMin, boundsMax, p);
    }
    if (boundsMin[0] > boundsMax[0]) { // nothing at all: an empty box at the tile's corner
        const f32 corner[3] = {pack::tileCornerX(input.ty), pack::tileCornerY(input.tx), 0.0f};
        std::memcpy(boundsMin, corner, sizeof(corner));
        std::memcpy(boundsMax, corner, sizeof(corner));
    }
    std::memcpy(header.boundsMin, boundsMin, sizeof(boundsMin));
    std::memcpy(header.boundsMax, boundsMax, sizeof(boundsMax));

    BlobWriter writer(sizeof(header));
    header.heightBasesOffset = writer.append(heightBases);
    header.heightsOffset = writer.append(heights);
    header.holesOffset = writer.append(holes);
    header.vertexCount = static_cast<u32>(solid.positions.size());
    header.verticesOffset = writer.append(solid.positions);
    header.triangleCount = solid.triangleCount();
    header.indicesOffset = writer.append(solid.indices);
    header.triangleFlagsOffset = writer.append(solid.tags);
    header.nodeCount = static_cast<u32>(nodes.size());
    header.nodesOffset = writer.append(nodes);
    header.liquidVertexCount = static_cast<u32>(liquid.positions.size());
    header.liquidVerticesOffset = writer.append(liquid.positions);
    header.liquidTriangleCount = liquid.triangleCount();
    header.liquidIndicesOffset = writer.append(liquid.indices);
    header.liquidKindsOffset = writer.append(liquid.tags);
    header.liquidNodeCount = static_cast<u32>(liquidNodes.size());
    header.liquidNodesOffset = writer.append(liquidNodes);
    if (stats) {
        stats->triangles = header.triangleCount;
        stats->vertices = header.vertexCount;
        stats->nodes = header.nodeCount;
        stats->liquidTriangles = header.liquidTriangleCount;
        stats->depth = std::max(depth, liquidDepth);
    }
    return writer.finish(header);
}

} // namespace vd::bake
