#pragma once

#include "core/Types.h"
#include "pack/ModelFormat.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <span>
#include <vector>

namespace vd {

// Phase 18 (world): what 1.12 does with a WMO's BSP trees and portals, as pure functions over the baked tables
// (pack::ModelBspHeader, ModelPortal, ModelPortalRef). Model space throughout. WorldObjects uses them to find the group
// the camera is in and to walk the portals from there; the tests use them on their own.

// One group's BSP as baked.
struct WmoBspView {
    std::span<const pack::ModelBspNode> nodes; // [0] is the root
    std::span<const u16> refs;
    std::span<const std::array<u16, 3>> triangles;
    std::span<const glm::vec3> vertices;
};

// The nearest face of the group the segment [from, to] crosses, as a fraction of the segment below `best` (both sides
// of a face count): true, and `best` lowered to it. WoW.exe 0x6b92b0: the segment walks the tree (0x6bc370: a node's
// plane is x, y or z = planeDist; below it the negative child, above it the positive one, both when it straddles),
// then each leaf's faces are tested.
bool wmoBspSegment(const WmoBspView& group, const glm::vec3& from, const glm::vec3& to, f32& best);

// Whether the segment [from, to] crosses the portal (its plane: n . p + plane.w = 0, and inside its polygon) at a
// fraction of the segment in [0, best]: true, and `best` lowered to it. WoW.exe 0x6a3f80 (the plane, then 0x7c23e0
// for the polygon).
bool wmoPortalSegment(const glm::vec4& plane, std::span<const glm::vec3> polygon, const glm::vec3& from,
                      const glm::vec3& to, f32& best);

// The side of the portal plane `p` lies on: the plane's equation there (> 0 in front).
inline f32 wmoPortalSide(const glm::vec4& plane, const glm::vec3& p) {
    return plane.x * p.x + plane.y * p.y + plane.z * p.z + plane.w;
}

// A WMO's portal graph for the walk.
struct WmoPortalGraph {
    std::span<const pack::ModelGroup> groups;
    std::span<const pack::ModelPortalRef> refs;
    std::span<const pack::ModelPortal> portals;
    std::span<const glm::vec3> portalVertices;
};

// A screen rectangle in normalized device coordinates (x, y in -1..1).
struct WmoScreenRect {
    f32 minX = -1.0f;
    f32 minY = -1.0f;
    f32 maxX = 1.0f;
    f32 maxY = 1.0f;
    bool empty() const { return minX >= maxX || minY >= maxY; }
};

// What the walk found: the groups seen (their rectangles' union, a group not reached has an empty one) and whether a
// portal leads outdoors (an exterior group, MOGP 0x8 / 0x40), with the union of what of the screen it covers.
struct WmoPortalWalk {
    std::vector<WmoScreenRect> groupRects; // per group
    std::vector<u8> visible;               // per group: 1 when reached
    bool outdoors = false;
    WmoScreenRect outdoorRect{1.0f, 1.0f, -1.0f, -1.0f};
    u32 portalsTested = 0;
    u32 portalsPassed = 0;
};

// 1.12's walk from the groups the camera is in (WoW.exe 0x6b3b20 -> 0x6b41c0): from a group, through each of its
// portals that does not lead back where the walk came from, that the camera sees from the group's side, and whose
// polygon on the screen overlaps the rectangle the walk got there with; into the group beyond, with the overlap.
// `clipFromModel` takes model space to clip space; `camera` is the camera in model space. `out` is reset first
// (its vectors keep their capacity: no allocation once warmed up).
void wmoWalkPortals(const WmoPortalGraph& graph, std::span<const u16> startGroups, const glm::vec3& camera,
                    const glm::mat4& clipFromModel, WmoPortalWalk& out);

} // namespace vd
