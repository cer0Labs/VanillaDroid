#include "physics/Collision.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vd::physics {
namespace {

constexpr f32 kInsideTolerance = 1e-5f; // barycentric slack: rays and spheres don't slip between neighbors

bool outsideBox(const Triangle& t, const glm::vec3& min, const glm::vec3& max) {
    return std::max({t.a.x, t.b.x, t.c.x}) < min.x || std::min({t.a.x, t.b.x, t.c.x}) > max.x ||
           std::max({t.a.y, t.b.y, t.c.y}) < min.y || std::min({t.a.y, t.b.y, t.c.y}) > max.y ||
           std::max({t.a.z, t.b.z, t.c.z}) < min.z || std::min({t.a.z, t.b.z, t.c.z}) > max.z;
}

// p on the plane of `t`: inside the triangle (edges included, with a little slack scaled to its size).
bool insideTriangle(const glm::vec3& p, const Triangle& t) {
    const glm::vec3& n = t.normal;
    const f32 slack = -kInsideTolerance * glm::dot(t.b - t.a, t.b - t.a);
    return glm::dot(glm::cross(t.b - t.a, p - t.a), n) >= slack && glm::dot(glm::cross(t.c - t.b, p - t.b), n) >= slack &&
           glm::dot(glm::cross(t.a - t.c, p - t.c), n) >= slack;
}

// Earliest t in [0, maxT] at which a point moving from `c` by `d` comes within `r` of `v`. A point already
// inside that moves closer counts at t = 0.
bool sweepPointSphere(const glm::vec3& c, const glm::vec3& d, const glm::vec3& v, f32 r, f32 maxT, f32& t) {
    const glm::vec3 m = c - v;
    const f32 cc = glm::dot(m, m) - r * r;
    const f32 b = glm::dot(m, d);
    if (cc <= 0.0f) {
        if (b >= 0.0f) {
            return false; // inside and leaving
        }
        t = 0.0f;
        return true;
    }
    const f32 a = glm::dot(d, d);
    if (b >= 0.0f || a < 1e-12f) {
        return false;
    }
    const f32 disc = b * b - a * cc;
    if (disc < 0.0f) {
        return false;
    }
    const f32 hit = (-b - std::sqrt(disc)) / a;
    if (hit < 0.0f || hit > maxT) {
        return false;
    }
    t = hit;
    return true;
}

// Same against the segment p0-p1 (a capsule of radius r around it, without its end caps).
bool sweepPointCapsule(const glm::vec3& c, const glm::vec3& d, const glm::vec3& p0, const glm::vec3& p1, f32 r, f32 maxT,
                       f32& t, glm::vec3& contact) {
    const glm::vec3 e = p1 - p0;
    const f32 ee = glm::dot(e, e);
    if (ee < 1e-12f) {
        return false;
    }
    const glm::vec3 m = c - p0;
    const glm::vec3 mPerp = m - e * (glm::dot(m, e) / ee);
    const glm::vec3 dPerp = d - e * (glm::dot(d, e) / ee);
    const f32 a = glm::dot(dPerp, dPerp);
    const f32 b = glm::dot(mPerp, dPerp);
    const f32 cc = glm::dot(mPerp, mPerp) - r * r;
    f32 hit = 0.0f;
    if (cc <= 0.0f) {
        if (b >= 0.0f) {
            return false;
        }
        hit = 0.0f;
    } else {
        if (b >= 0.0f || a < 1e-12f) {
            return false;
        }
        const f32 disc = b * b - a * cc;
        if (disc < 0.0f) {
            return false;
        }
        hit = (-b - std::sqrt(disc)) / a;
        if (hit < 0.0f || hit > maxT) {
            return false;
        }
    }
    const f32 s = glm::dot(m + d * hit, e) / ee;
    if (s < 0.0f || s > 1.0f) {
        return false; // the end caps are the vertex spheres
    }
    t = hit;
    contact = p0 + e * s;
    return true;
}

bool sweepTriangle(const Triangle& tri, const glm::vec3& c, f32 r, const glm::vec3& d, f32 maxT, f32& outT,
                   glm::vec3& outNormal, glm::vec3& outPoint) {
    glm::vec3 n = tri.normal;
    f32 distance = glm::dot(c - tri.a, n);
    if (distance < 0.0f) {
        n = -n;
        distance = -distance;
    }
    const f32 dn = glm::dot(d, n);
    if (distance >= r) {
        if (dn >= 0.0f) {
            return false; // above the plane and not approaching it: the edges lie in the plane too
        }
        const f32 t = (distance - r) / -dn;
        if (t > maxT) {
            return false;
        }
        const glm::vec3 p = c + d * t - n * r;
        if (insideTriangle(p, tri)) {
            outT = t;
            outNormal = n;
            outPoint = p;
            return true;
        }
    } else {
        const glm::vec3 p = c - n * distance;
        if (insideTriangle(p, tri)) {
            if (dn >= 0.0f) {
                return false;
            }
            outT = 0.0f;
            outNormal = n;
            outPoint = p;
            return true;
        }
    }
    // The face was missed: the first edge or corner met.
    f32 best = maxT;
    bool found = false;
    glm::vec3 contact{0.0f};
    const glm::vec3* corners[3] = {&tri.a, &tri.b, &tri.c};
    for (u32 i = 0; i < 3; ++i) {
        f32 t = 0.0f;
        glm::vec3 point{0.0f};
        if (sweepPointCapsule(c, d, *corners[i], *corners[(i + 1) % 3], r, best, t, point) && t <= best) {
            best = t;
            contact = point;
            found = true;
        }
        if (sweepPointSphere(c, d, *corners[i], r, best, t) && t <= best) {
            best = t;
            contact = *corners[i];
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    const glm::vec3 away = c + d * best - contact;
    const f32 length = glm::length(away);
    outT = best;
    outNormal = length > 1e-6f ? away / length : n;
    outPoint = contact;
    return true;
}

} // namespace

TriangleSet::TriangleSet(u32 capacity) : m_capacity(capacity) {
    m_triangles.reserve(capacity);
}

void TriangleSet::reset(const glm::dvec3& origin) {
    m_origin = origin;
    m_triangles.clear();
    m_overflowed = false;
}

bool TriangleSet::add(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, u8 flags) {
    if (m_triangles.size() >= m_capacity) {
        m_overflowed = true;
        return false;
    }
    const glm::vec3 n = glm::cross(b - a, c - a);
    const f32 length = glm::length(n);
    if (length < 1e-8f) {
        return true;
    }
    m_triangles.push_back({a, b, c, n / length, flags});
    return true;
}

bool raycast(const TriangleSet& set, const glm::vec3& from, const glm::vec3& delta, Hit& hit, u8 ignore) {
    hit = Hit{};
    const glm::vec3 rayMin = glm::min(from, from + delta) - glm::vec3(1e-4f);
    const glm::vec3 rayMax = glm::max(from, from + delta) + glm::vec3(1e-4f);
    for (const Triangle& tri : set.triangles()) {
        if ((tri.flags & ignore) || outsideBox(tri, rayMin, rayMax)) {
            continue;
        }
        const glm::vec3 e1 = tri.b - tri.a;
        const glm::vec3 e2 = tri.c - tri.a;
        const glm::vec3 p = glm::cross(delta, e2);
        const f32 det = glm::dot(e1, p);
        if (std::abs(det) < 1e-12f) {
            continue;
        }
        const f32 inv = 1.0f / det;
        const glm::vec3 s = from - tri.a;
        const f32 u = glm::dot(s, p) * inv;
        if (u < -kInsideTolerance || u > 1.0f + kInsideTolerance) {
            continue;
        }
        const glm::vec3 q = glm::cross(s, e1);
        const f32 v = glm::dot(delta, q) * inv;
        if (v < -kInsideTolerance || u + v > 1.0f + kInsideTolerance) {
            continue;
        }
        const f32 t = glm::dot(e2, q) * inv;
        if (t < 0.0f || t > hit.t) {
            continue;
        }
        hit.t = t;
        hit.normal = glm::dot(tri.normal, delta) > 0.0f ? -tri.normal : tri.normal;
        hit.point = from + delta * t;
        hit.flags = tri.flags;
        hit.hit = true;
    }
    return hit.hit;
}

bool sweepSphere(const TriangleSet& set, const glm::vec3& center, f32 radius, const glm::vec3& delta, Hit& hit,
                 u8 ignore) {
    hit = Hit{};
    const glm::vec3 sweepMin = glm::min(center, center + delta) - glm::vec3(radius);
    const glm::vec3 sweepMax = glm::max(center, center + delta) + glm::vec3(radius);
    for (const Triangle& tri : set.triangles()) {
        if ((tri.flags & ignore) || outsideBox(tri, sweepMin, sweepMax)) {
            continue;
        }
        f32 t = 0.0f;
        glm::vec3 normal{0.0f}, point{0.0f};
        if (sweepTriangle(tri, center, radius, delta, hit.t, t, normal, point) && (!hit.hit || t < hit.t)) {
            hit.t = t;
            hit.normal = normal;
            hit.point = point;
            hit.flags = tri.flags;
            hit.hit = true;
        }
    }
    return hit.hit;
}

glm::vec3 closestPointOnTriangle(const glm::vec3& p, const Triangle& t) {
    // Ericson, Real-Time Collision Detection 5.1.5.
    const glm::vec3 ab = t.b - t.a;
    const glm::vec3 ac = t.c - t.a;
    const glm::vec3 ap = p - t.a;
    const f32 d1 = glm::dot(ab, ap);
    const f32 d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return t.a;
    }
    const glm::vec3 bp = p - t.b;
    const f32 d3 = glm::dot(ab, bp);
    const f32 d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return t.b;
    }
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return t.a + ab * (d1 / (d1 - d3));
    }
    const glm::vec3 cp = p - t.c;
    const f32 d5 = glm::dot(ab, cp);
    const f32 d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return t.c;
    }
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return t.a + ac * (d2 / (d2 - d6));
    }
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return t.b + (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const f32 denom = 1.0f / (va + vb + vc);
    return t.a + ab * (vb * denom) + ac * (vc * denom);
}

glm::vec3 depenetrateSphere(const TriangleSet& set, glm::vec3& center, f32 radius, u8 ignore) {
    const glm::vec3 start = center;
    for (u32 pass = 0; pass < 4; ++pass) {
        bool moved = false;
        for (const Triangle& tri : set.triangles()) {
            if (tri.flags & ignore) {
                continue;
            }
            const glm::vec3 away = center - closestPointOnTriangle(center, tri);
            const f32 distance2 = glm::dot(away, away);
            if (distance2 >= radius * radius) {
                continue;
            }
            const f32 distance = std::sqrt(distance2);
            const glm::vec3 direction = distance > 1e-6f ? away / distance : tri.normal;
            center += direction * (radius - distance + 1e-4f);
            moved = true;
        }
        if (!moved) {
            break;
        }
    }
    return center - start;
}

} // namespace vd::physics
