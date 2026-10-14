#pragma once

#include "core/Types.h"
#include "formats/Adt.h"
#include "formats/M2.h"
#include "formats/Wmo.h"
#include "pack/CollisionFormat.h"

#include <array>
#include <string>
#include <vector>

namespace vd::bake {

// Triangles with one byte per triangle: collision flags (pack::kCollision*) or a LiquidKind.
struct TriangleSoup {
    std::vector<std::array<f32, 3>> positions;
    std::vector<u32> indices;
    std::vector<u8> tags;

    u32 triangleCount() const { return static_cast<u32>(indices.size() / 3); }
    bool empty() const { return indices.empty(); }
};

// A doodad of a WMO doodad set, for collision: its M2 and its placement in the WMO's model space.
struct WmoCollisionDoodad {
    std::string model; // canonical M2 path
    std::array<f32, 12> transform;
};

// What a WMO contributes, in its model space: the triangles vMaNGOS' extractor keeps (MOPY collision, or
// rendered and not detail), flagged kCollisionNoCamera where MOPY says so; the group liquids (MLIQ tiles
// with liquid, tagged by LiquidKind); and the doodad sets.
struct WmoCollision {
    TriangleSoup solid;
    TriangleSoup liquid;
    std::vector<formats::WmoDoodadSet> sets;
    std::vector<WmoCollisionDoodad> doodads;
};
WmoCollision wmoCollision(const formats::WmoRoot& root, const std::vector<formats::WmoGroup>& groups);
// An M2's bounding triangles (the only part of a doodad the player bumps into), tagged kCollisionDoodad.
TriangleSoup m2Collision(const formats::M2Model& model);

// A WMO doodad's placement in the WMO's model space: translate * rotate(quaternion) * scale (phase 14: also the doodads'
// sounds).
std::array<f32, 12> doodadTransform(const formats::WmoDoodad& d);
// out = a * b for 3x4 row-major affine transforms.
std::array<f32, 12> multiplyTransforms(const std::array<f32, 12>& a, const std::array<f32, 12>& b);
// Model-space box of `soup` through `transform`: the world box of an instance. False when the soup is empty.
bool transformedBounds(const TriangleSoup& soup, const std::array<f32, 12>& transform, std::array<f32, 3>& min,
                       std::array<f32, 3>& max);
// Appends `soup` through `transform`, keeping only the triangles whose XY box overlaps [xyMin, xyMax].
void appendTransformed(const TriangleSoup& soup, const std::array<f32, 12>& transform, const std::array<f32, 2>& xyMin,
                       const std::array<f32, 2>& xyMax, TriangleSoup& out);

// Everything that goes into one collision tile, in world coordinates.
struct CollisionTileInput {
    u32 tx = 0;
    u32 ty = 0;
    const formats::Adt* adt = nullptr; // terrain heights, holes and MCLQ liquids; null: objects only
    TriangleSoup solid;
    TriangleSoup liquid;
};
struct CollisionTileStats {
    u32 triangles = 0;
    u32 vertices = 0;
    u32 nodes = 0;
    u32 liquidTriangles = 0;
    u32 depth = 0;
};
// CollisionTileHeader blob (pack/CollisionFormat.h). Vertices are welded, degenerate triangles dropped.
std::vector<u8> bakeCollisionTile(const CollisionTileInput& input, CollisionTileStats* stats = nullptr);

// BVH over `indices` (triangles into `positions`): reorders the triangles (and their tags) into leaf order.
// Node 0 is the root; the first child of an inner node follows it, the second is at `first`.
std::vector<pack::CollisionNode> buildCollisionBvh(const std::vector<std::array<f32, 3>>& positions,
                                                   std::vector<u32>& indices, std::vector<u8>& tags,
                                                   u32* depth = nullptr);

} // namespace vd::bake
