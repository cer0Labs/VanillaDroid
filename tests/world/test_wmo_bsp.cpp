// Phase 18 (world): a WMO's BSP trees and portals the way 1.12 uses them (world/WmoBsp): the segment under the camera,
// the portal it crosses, the walk through the portals; and the baked trees of a real city ($VD_PACKS_DIR).

#include "core/Hash.h"
#include "pack/ModelFormat.h"
#include "pack/PackReader.h"
#include "world/WmoBsp.h"

#include <doctest/doctest.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <cstdlib>
#include <cstring>
#include <vector>

using namespace vd;

namespace {

// A 4 x 4 floor at z = 0 around the origin, split by a node on x = 0 into two leaves of two triangles each.
struct Floor {
    std::vector<pack::ModelBspNode> nodes;
    std::vector<u16> refs;
    std::vector<std::array<u16, 3>> triangles;
    std::vector<glm::vec3> vertices;
    WmoBspView view() const { return {nodes, refs, triangles, vertices}; }
};

Floor makeFloor() {
    Floor f;
    f.vertices = {{-2, -2, 0}, {0, -2, 0}, {2, -2, 0}, {-2, 2, 0}, {0, 2, 0}, {2, 2, 0}};
    f.triangles = {{0, 1, 4}, {0, 4, 3}, {1, 2, 5}, {1, 5, 4}};
    f.refs = {0, 1, 2, 3};
    f.nodes = {
        {0 /* x */, 1, 2, 0, 0, 0.0f},
        {pack::kModelBspLeaf, pack::kModelBspNoChild, pack::kModelBspNoChild, 2, 0, 0.0f}, // x < 0
        {pack::kModelBspLeaf, pack::kModelBspNoChild, pack::kModelBspNoChild, 2, 2, 0.0f}, // x > 0
    };
    return f;
}

} // namespace

TEST_CASE("WMO BSP (phase 18): the segment under the camera meets the nearest face, from either side") {
    const Floor floor = makeFloor();
    f32 best = 1.0f;
    CHECK(wmoBspSegment(floor.view(), {-1.0f, 0.5f, 10.0f}, {-1.0f, 0.5f, -10.0f}, best));
    CHECK(best == doctest::Approx(0.5f));
    best = 1.0f;
    CHECK(wmoBspSegment(floor.view(), {1.5f, -1.0f, 2.0f}, {1.5f, -1.0f, -18.0f}, best)); // the other leaf
    CHECK(best == doctest::Approx(0.1f));
    best = 1.0f;
    CHECK(wmoBspSegment(floor.view(), {1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, -9.0f}, best) == false); // already below it
    best = 1.0f;
    CHECK(wmoBspSegment(floor.view(), {0.5f, 0.5f, -1.0f}, {0.5f, 0.5f, 1.0f}, best)); // from below: both sides count
    CHECK(best == doctest::Approx(0.5f));
    best = 1.0f;
    CHECK_FALSE(wmoBspSegment(floor.view(), {3.0f, 0.0f, 1.0f}, {3.0f, 0.0f, -1.0f}, best)); // beside it
    best = 0.25f; // something nearer was found already
    CHECK_FALSE(wmoBspSegment(floor.view(), {-1.0f, 0.5f, 10.0f}, {-1.0f, 0.5f, -10.0f}, best));
    CHECK(best == doctest::Approx(0.25f));
    // A segment straddling the node's plane walks down both sides, each with its part.
    best = 1.0f;
    CHECK(wmoBspSegment(floor.view(), {-1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, -1.0f}, best));
    CHECK(best == doctest::Approx(0.5f));
}

TEST_CASE("WMO BSP (phase 18): a portal crossed inside its polygon, on its plane n.p + d = 0") {
    const std::vector<glm::vec3> square = {{-1, -1, 5}, {1, -1, 5}, {1, 1, 5}, {-1, 1, 5}};
    const glm::vec4 plane(0.0f, 0.0f, 1.0f, -5.0f); // as MOPT stores them: n . v = -d for its vertices
    f32 best = 1.05f;
    CHECK(wmoPortalSegment(plane, square, {0.2f, 0.3f, 10.0f}, {0.2f, 0.3f, 0.0f}, best));
    CHECK(best == doctest::Approx(0.5f));
    best = 1.05f;
    CHECK_FALSE(wmoPortalSegment(plane, square, {1.5f, 0.0f, 10.0f}, {1.5f, 0.0f, 0.0f}, best)); // beside it
    best = 0.4f;
    CHECK_FALSE(wmoPortalSegment(plane, square, {0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f}, best)); // farther than best
    best = 1.05f;
    CHECK_FALSE(wmoPortalSegment(plane, square, {0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 6.0f}, best)); // stops short
    CHECK(wmoPortalSide(plane, {0.0f, 0.0f, 10.0f}) > 0.0f);
    CHECK(wmoPortalSide(plane, {0.0f, 0.0f, 0.0f}) < 0.0f);
}

TEST_CASE("WMO portals (phase 18): the walk sees through the portals in view, and out of the building") {
    // Three rooms along +X: A (x < 0), B (0 < x < 10), C outdoors (x > 10). Portal 0 on x = 0 between A and B,
    // portal 1 on x = 10 between B and C; each a 2 x 2 square around y = 0, z = 1.
    std::vector<pack::ModelGroup> groups(3);
    groups[0].flags = pack::kModelGroupInterior;
    groups[0].firstPortalRef = 0;
    groups[0].portalRefCount = 1;
    groups[1].flags = pack::kModelGroupInterior;
    groups[1].firstPortalRef = 1;
    groups[1].portalRefCount = 2;
    groups[2].flags = pack::kModelGroupExterior;
    groups[2].firstPortalRef = 3;
    groups[2].portalRefCount = 1;
    // Plane x = 0: n = (1, 0, 0), d = 0 (A on the negative side); plane x = 10: n = (1, 0, 0), d = -10.
    std::vector<pack::ModelPortal> portals = {{{1.0f, 0.0f, 0.0f, 0.0f}, 0, 4}, {{1.0f, 0.0f, 0.0f, -10.0f}, 4, 4}};
    std::vector<glm::vec3> vertices = {{0, -1, 0}, {0, 1, 0}, {0, 1, 2}, {0, -1, 2},
                                       {10, -1, 0}, {10, 1, 0}, {10, 1, 2}, {10, -1, 2}};
    std::vector<pack::ModelPortalRef> refs = {
        {0, 1, -1, 0}, // A: portal 0 to B, A on its negative side
        {0, 0, 1, 0},  // B: portal 0 back to A, B on its positive side
        {1, 2, -1, 0}, // B: portal 1 to C
        {1, 1, 1, 0},  // C: portal 1 back to B
    };
    const WmoPortalGraph graph{groups, refs, portals, vertices};
    const glm::mat4 projection = glm::perspective(1.0f, 1.0f, 0.2f, 1000.0f);
    const glm::vec3 camera(-5.0f, 0.0f, 1.0f);
    const std::array<u16, 1> start{0};
    WmoPortalWalk walk;

    // Looking down the rooms, through both doors: all three, and the outdoors.
    glm::mat4 view = glm::lookAt(camera, camera + glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    wmoWalkPortals(graph, start, camera, projection * view, walk);
    CHECK(walk.visible[0] == 1);
    CHECK(walk.visible[1] == 1);
    CHECK(walk.visible[2] == 1);
    CHECK(walk.outdoors);
    CHECK_FALSE(walk.outdoorRect.empty());
    CHECK(walk.outdoorRect.maxX - walk.outdoorRect.minX < walk.groupRects[1].maxX - walk.groupRects[1].minX + 1e-4f);

    // Turned away from the doors: only the room it stands in.
    view = glm::lookAt(camera, camera + glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    wmoWalkPortals(graph, start, camera, projection * view, walk);
    CHECK(walk.visible[0] == 1);
    CHECK(walk.visible[1] == 0);
    CHECK(walk.visible[2] == 0);
    CHECK_FALSE(walk.outdoors);

    // Looking at the first door from the side, the second out of view through it: two rooms, not the outdoors.
    view = glm::lookAt(camera, glm::vec3(0.0f, 0.0f, 1.0f) + glm::vec3(0.0f, -3.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    wmoWalkPortals(graph, start, camera, projection * view, walk);
    CHECK(walk.visible[0] == 1);
    CHECK(walk.visible[2] == walk.outdoors);

    // Standing beyond portal 0 (in B by mistake for A): portal 0 is seen from behind and not walked.
    const glm::vec3 beyond(5.0f, 0.0f, 1.0f);
    view = glm::lookAt(beyond, beyond + glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    wmoWalkPortals(graph, start, beyond, projection * view, walk);
    CHECK(walk.visible[1] == 0);
}

TEST_CASE("WMO BSP (phase 18): every kept face of Ironforge's groups is found under a point over it (VD_PACKS_DIR)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    if (!dir) {
        MESSAGE("VD_PACKS_DIR not set: skipping");
        return;
    }
    pack::PackReader models;
    if (!models.open(std::filesystem::path(dir) / "models.vdpk").ok()) {
        MESSAGE("models.vdpk missing: skipping");
        return;
    }
    const std::optional<pack::PackAsset> asset = models.find("world/wmo/khazmodan/cities/ironforge/ironforge.wmo");
    REQUIRE(asset);
    std::vector<u8> blob(asset->rawSize);
    REQUIRE(pack::PackReader::read(*asset, blob.data(), blob.size()).ok());
    pack::ModelHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    if (header.version != pack::kModelFormatVersion || header.bspOffset == 0) {
        MESSAGE("models.vdpk older than v8 (no BSP): skipping (vd_baker bake --only models)");
        return;
    }
    pack::ModelBspHeader h;
    std::memcpy(&h, blob.data() + header.bspOffset, sizeof(h));
    REQUIRE(h.groupCount == header.groupCount);
    auto array = [&](u32 offset) { return blob.data() + offset; };
    const auto* groups = reinterpret_cast<const pack::ModelBspGroup*>(array(h.groupsOffset));
    const auto* nodes = reinterpret_cast<const pack::ModelBspNode*>(array(h.nodesOffset));
    const auto* refs = reinterpret_cast<const u16*>(array(h.refsOffset));
    const auto* triangles = reinterpret_cast<const std::array<u16, 3>*>(array(h.trianglesOffset));
    const auto* vertices = reinterpret_cast<const glm::vec3*>(array(h.verticesOffset));
    u32 tested = 0, found = 0, groupsWithTrees = 0;
    for (u32 g = 0; g < h.groupCount; ++g) {
        const pack::ModelBspGroup& group = groups[g];
        if (group.nodeCount == 0) {
            continue;
        }
        ++groupsWithTrees;
        const WmoBspView view{{nodes + group.firstNode, group.nodeCount},
                              {refs + group.firstRef, group.refCount},
                              {triangles + group.firstTriangle, group.triangleCount},
                              {vertices + group.firstVertex, group.vertexCount}};
        for (u32 t = 0; t < group.triangleCount; t += 7) { // a sample of each group's faces
            const std::array<u16, 3>& tri = view.triangles[t];
            const glm::vec3 center = (view.vertices[tri[0]] + view.vertices[tri[1]] + view.vertices[tri[2]]) / 3.0f;
            const glm::vec3 normal = glm::cross(view.vertices[tri[1]] - view.vertices[tri[0]],
                                                view.vertices[tri[2]] - view.vertices[tri[0]]);
            if (std::abs(normal.z) < 0.3f * glm::length(normal)) {
                continue; // a wall: a vertical segment grazes it
            }
            f32 best = 1.0f;
            ++tested;
            found += wmoBspSegment(view, center + glm::vec3(0.0f, 0.0f, 0.05f), center - glm::vec3(0.0f, 0.0f, 0.05f),
                                   best)
                         ? 1u
                         : 0u;
        }
    }
    MESSAGE("Ironforge: " << groupsWithTrees << " of " << h.groupCount << " groups with a tree, " << found << " of "
                          << tested << " faces found");
    CHECK(groupsWithTrees > 90);
    CHECK(tested > 1000);
    CHECK(found == tested);
}
