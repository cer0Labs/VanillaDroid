#include "world/WmoBsp.h"

#include "core/Profile.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

constexpr u32 kMaxDepth = 64; // nodes deep (a tree is a few dozen) and portals deep (rooms behind rooms)

// Both sides count: a camera under a ceiling, over a floor (Moller-Trumbore, the segment's fraction in [0, best]).
bool segmentTriangle(const glm::vec3& from, const glm::vec3& dir, const glm::vec3& a, const glm::vec3& b,
                     const glm::vec3& c, f32& best) {
    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const glm::vec3 p = glm::cross(dir, e2);
    const f32 det = glm::dot(e1, p);
    if (std::abs(det) < 1e-12f) {
        return false;
    }
    const f32 inverse = 1.0f / det;
    const glm::vec3 s = from - a;
    const f32 u = glm::dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const glm::vec3 q = glm::cross(s, e1);
    const f32 v = glm::dot(dir, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const f32 t = glm::dot(e2, q) * inverse;
    if (t < 0.0f || t > best) {
        return false;
    }
    best = t;
    return true;
}

bool walkNode(const WmoBspView& group, u32 index, const glm::vec3& a, const glm::vec3& b, const glm::vec3& from,
              const glm::vec3& dir, f32& best, u32 depth) {
    if (index >= group.nodes.size() || depth > kMaxDepth) {
        return false;
    }
    const pack::ModelBspNode& node = group.nodes[index];
    if (node.flags & pack::kModelBspLeaf) {
        bool hit = false;
        for (u32 i = node.firstFace; i < node.firstFace + node.faceCount && i < group.refs.size(); ++i) {
            const u16 face = group.refs[i];
            if (face >= group.triangles.size()) {
                continue;
            }
            const std::array<u16, 3>& t = group.triangles[face];
            if (t[0] < group.vertices.size() && t[1] < group.vertices.size() && t[2] < group.vertices.size()) {
                hit |= segmentTriangle(from, dir, group.vertices[t[0]], group.vertices[t[1]], group.vertices[t[2]], best);
            }
        }
        return hit;
    }
    // The part of the segment on each side of the node's plane (x, y or z = planeDist) goes down that side.
    const u32 axis = node.flags & pack::kModelBspAxisMask;
    const f32 da = a[axis] - node.planeDist;
    const f32 db = b[axis] - node.planeDist;
    bool hit = false;
    if (da <= 0.0f || db <= 0.0f) {
        if (node.negChild != pack::kModelBspNoChild) {
            glm::vec3 na = a, nb = b;
            if (da > 0.0f) {
                na = a + (b - a) * (da / (da - db));
            } else if (db > 0.0f) {
                nb = a + (b - a) * (da / (da - db));
            }
            hit |= walkNode(group, node.negChild, na, nb, from, dir, best, depth + 1);
        }
    }
    if (da >= 0.0f || db >= 0.0f) {
        if (node.posChild != pack::kModelBspNoChild) {
            glm::vec3 pa = a, pb = b;
            if (da < 0.0f) {
                pa = a + (b - a) * (da / (da - db));
            } else if (db < 0.0f) {
                pb = a + (b - a) * (da / (da - db));
            }
            hit |= walkNode(group, node.posChild, pa, pb, from, dir, best, depth + 1);
        }
    }
    return hit;
}

// Inside a planar polygon (its vertices in order, convex or not), on the plane's two widest axes.
bool insidePolygon(const glm::vec3& normal, std::span<const glm::vec3> polygon, const glm::vec3& p) {
    const glm::vec3 n = glm::abs(normal);
    const u32 drop = n.x >= n.y && n.x >= n.z ? 0u : (n.y >= n.z ? 1u : 2u);
    const u32 u = drop == 0 ? 1u : 0u;
    const u32 v = drop == 2 ? 1u : 2u;
    bool inside = false;
    for (usize i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const glm::vec3& a = polygon[i];
        const glm::vec3& b = polygon[j];
        if ((a[v] > p[v]) != (b[v] > p[v]) && p[u] < (b[u] - a[u]) * (p[v] - a[v]) / (b[v] - a[v]) + a[u]) {
            inside = !inside;
        }
    }
    return inside;
}

WmoScreenRect intersect(const WmoScreenRect& a, const WmoScreenRect& b) {
    return {std::max(a.minX, b.minX), std::max(a.minY, b.minY), std::min(a.maxX, b.maxX), std::min(a.maxY, b.maxY)};
}

WmoScreenRect merge(const WmoScreenRect& a, const WmoScreenRect& b) {
    if (a.empty()) {
        return b;
    }
    return {std::min(a.minX, b.minX), std::min(a.minY, b.minY), std::max(a.maxX, b.maxX), std::max(a.maxY, b.maxY)};
}

bool contains(const WmoScreenRect& outer, const WmoScreenRect& inner) {
    return !outer.empty() && inner.minX >= outer.minX && inner.minY >= outer.minY && inner.maxX <= outer.maxX &&
           inner.maxY <= outer.maxY;
}

// The portal polygon's rectangle on the screen, clipped at the near distance: a portal wholly behind it covers nothing,
// one the camera stands in (its doorway) reaches past the screen's edges.
WmoScreenRect portalRect(std::span<const glm::vec3> polygon, const glm::mat4& clipFromModel) {
    constexpr f32 kNear = 0.05f;
    WmoScreenRect out{1e9f, 1e9f, -1e9f, -1e9f};
    auto add = [&](const glm::vec4& clip) {
        out.minX = std::min(out.minX, clip.x / clip.w);
        out.minY = std::min(out.minY, clip.y / clip.w);
        out.maxX = std::max(out.maxX, clip.x / clip.w);
        out.maxY = std::max(out.maxY, clip.y / clip.w);
    };
    glm::vec4 previous = clipFromModel * glm::vec4(polygon.back(), 1.0f);
    for (const glm::vec3& v : polygon) {
        const glm::vec4 clip = clipFromModel * glm::vec4(v, 1.0f);
        if ((previous.w >= kNear) != (clip.w >= kNear)) { // the edge crosses the near distance: where it does
            add(previous + (clip - previous) * ((kNear - previous.w) / (clip.w - previous.w)));
        }
        if (clip.w >= kNear) {
            add(clip);
        }
        previous = clip;
    }
    return out;
}

struct Walker {
    const WmoPortalGraph& graph;
    const glm::vec3& camera;
    const glm::mat4& clipFromModel;
    WmoPortalWalk& out;

    void walk(u32 group, u32 from, const WmoScreenRect& rect, u32 depth) {
        if (group >= graph.groups.size() || depth > kMaxDepth) {
            return;
        }
        // Reached again with a rectangle inside one it already had: nothing new to see from there.
        if (out.visible[group] && contains(out.groupRects[group], rect)) {
            return;
        }
        out.visible[group] = 1;
        out.groupRects[group] = merge(out.groupRects[group], rect);
        const pack::ModelGroup& info = graph.groups[group];
        if (info.flags & (pack::kModelGroupExterior | pack::kModelGroupExteriorLit)) {
            out.outdoors = true; // the walk sees out: the world beyond is drawn through this rectangle
            out.outdoorRect = merge(out.outdoorRect, rect);
        }
        for (u32 r = info.firstPortalRef; r < info.firstPortalRef + info.portalRefCount && r < graph.refs.size(); ++r) {
            const pack::ModelPortalRef& ref = graph.refs[r];
            if (ref.group == from || ref.group >= graph.groups.size() || ref.portal >= graph.portals.size()) {
                continue;
            }
            const pack::ModelPortal& portal = graph.portals[ref.portal];
            if (u64{portal.firstVertex} + portal.vertexCount > graph.portalVertices.size() || portal.vertexCount < 3) {
                continue;
            }
            ++out.portalsTested;
            // The camera looks through it from this group's side (the ref's side: where the group lies), or stands in
            // it (a yard either way: a doorway).
            const glm::vec4 plane(portal.plane[0], portal.plane[1], portal.plane[2], portal.plane[3]);
            const f32 side = wmoPortalSide(plane, camera);
            if ((ref.side > 0 && side < -1.0f) || (ref.side <= 0 && side > 1.0f)) {
                continue;
            }
            const std::span<const glm::vec3> polygon = graph.portalVertices.subspan(portal.firstVertex,
                                                                                    portal.vertexCount);
            const WmoScreenRect through = intersect(rect, portalRect(polygon, clipFromModel));
            if (through.empty()) {
                continue;
            }
            ++out.portalsPassed;
            walk(ref.group, group, through, depth + 1);
        }
    }
};

} // namespace

bool wmoBspSegment(const WmoBspView& group, const glm::vec3& from, const glm::vec3& to, f32& best) {
    if (group.nodes.empty()) {
        return false;
    }
    return walkNode(group, 0, from, to, from, to - from, best, 0);
}

bool wmoPortalSegment(const glm::vec4& plane, std::span<const glm::vec3> polygon, const glm::vec3& from,
                      const glm::vec3& to, f32& best) {
    if (polygon.size() < 3) {
        return false;
    }
    const glm::vec3 normal(plane);
    const glm::vec3 dir = to - from;
    const f32 along = glm::dot(normal, dir);
    if (std::abs(along) < 1e-6f) {
        return false; // parallel to it
    }
    const f32 t = -wmoPortalSide(plane, from) / along;
    if (t < 0.0f || t > best || !insidePolygon(normal, polygon, from + dir * t)) {
        return false;
    }
    best = t;
    return true;
}

void wmoWalkPortals(const WmoPortalGraph& graph, std::span<const u16> startGroups, const glm::vec3& camera,
                    const glm::mat4& clipFromModel, WmoPortalWalk& out) {
    VD_PROFILE_ZONE("wmoWalkPortals");
    out.groupRects.assign(graph.groups.size(), WmoScreenRect{1.0f, 1.0f, -1.0f, -1.0f});
    out.visible.assign(graph.groups.size(), 0);
    out.outdoors = false;
    out.outdoorRect = {1.0f, 1.0f, -1.0f, -1.0f};
    out.portalsTested = 0;
    out.portalsPassed = 0;
    Walker walker{graph, camera, clipFromModel, out};
    for (const u16 group : startGroups) {
        walker.walk(group, pack::kModelNoGroup, WmoScreenRect{}, 0);
    }
}

} // namespace vd
