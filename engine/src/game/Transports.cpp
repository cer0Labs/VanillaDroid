#include "game/Transports.h"

#include "core/Log.h"
#include "db/DbcView.h"
#include "pack/PackReader.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace vd::game {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;

// Catmull-Rom of the server's splines (Movement/spline/spline.cpp s_catmullRomCoeffs) over p[1]-p[2].
glm::dvec3 catmullRom(const glm::vec3* p, f64 t) {
    const f64 t2 = t * t, t3 = t2 * t;
    return glm::dvec3(p[0]) * (-0.5 * t3 + t2 - 0.5 * t) + glm::dvec3(p[1]) * (1.5 * t3 - 2.5 * t2 + 1.0) +
           glm::dvec3(p[2]) * (-1.5 * t3 + 2.0 * t2 + 0.5 * t) + glm::dvec3(p[3]) * (0.5 * t3 - 0.5 * t2);
}

glm::dvec3 catmullRomDerivative(const glm::vec3* p, f64 t) {
    const f64 t2 = t * t;
    return glm::dvec3(p[0]) * (-1.5 * t2 + 2.0 * t - 0.5) + glm::dvec3(p[1]) * (4.5 * t2 - 5.0 * t) +
           glm::dvec3(p[2]) * (-4.5 * t2 + 4.0 * t + 0.5) + glm::dvec3(p[3]) * (1.5 * t2 - t);
}

f32 normalizeOrientation(f32 o) {
    const f32 wrapped = std::fmod(o, 2.0f * kPi);
    return wrapped < 0.0f ? wrapped + 2.0f * kPi : wrapped;
}

// G3D Vector3::lerp: a + (b - a) * t.
glm::vec3 lerp(const glm::vec3& a, const glm::vec3& b, f32 t) {
    return a + (b - a) * t;
}

// A spline over `points` already holding its virtual ends, from index 1 to `last`: the server's segment
// lengths.
std::vector<f64> segmentLengths(const std::vector<glm::vec3>& points, u32 first, u32 last) {
    std::vector<f64> lengths(last + 1, 0.0);
    f64 length = 0.0;
    for (u32 i = first; i < last; ++i) {
        const glm::vec3* p = &points[i - 1];
        glm::dvec3 current(p[1]);
        for (u32 step = 1; step <= 3; ++step) { // STEPS_PER_SEGMENT
            const glm::dvec3 next = catmullRom(p, static_cast<f64>(step) / 3.0);
            length += glm::length(next - current);
            current = next;
        }
        lengths[i + 1] = length;
    }
    return lengths;
}

} // namespace

glm::vec3 TransportPath::Spline::evaluate(u32 index, f32 u) const {
    return glm::vec3(catmullRom(&points[index - 1], u));
}

glm::vec3 TransportPath::Spline::derivative(u32 index, f32 u) const {
    return glm::vec3(catmullRomDerivative(&points[index - 1], u));
}

bool TransportPath::build(std::span<const TaxiPathPoint> points, f32 speed, f32 accel) {
    m_points.assign(points.begin(), points.end());
    m_frames.clear();
    m_splines.clear();
    m_period = 0;
    if (m_points.size() < 4 || speed <= 0.0f || accel <= 0.0f) {
        return false;
    }
    const std::vector<TaxiPathPoint>& path = m_points;

    // The orientation spline over every point, with extra points so each node has a derivative.
    std::vector<glm::vec3> all;
    all.reserve(path.size() + 3);
    for (const TaxiPathPoint& p : path) {
        all.push_back(p.position);
    }
    all.insert(all.begin(), lerp(all.front(), all[1], -0.2f));
    all.push_back(lerp(all.back(), all[all.size() - 2], -0.2f));
    all.push_back(lerp(all.back(), all[all.size() - 2], -1.0f));
    // SplineRawInitializer: the points as they are, lo 1, hi size - 2.

    // Keyframes: every point but the first and the last, skipping the one after a map change.
    std::vector<glm::vec3> splinePath;
    bool mapChange = false;
    for (usize i = 1; i + 1 < path.size(); ++i) {
        if (!mapChange) {
            const TaxiPathPoint& node = path[i];
            if (i != path.size() - 1 && ((node.flags & 1) || node.map != path[i + 1].map)) {
                if (m_frames.empty()) {
                    return false; // the server would assert: no frame before the jump
                }
                m_frames.back().teleport = true;
                mapChange = true;
            } else {
                KeyFrame k;
                k.node = &node;
                const glm::dvec3 h = catmullRomDerivative(&all[i + 1 - 1], 0.0);
                k.initialOrientation = normalizeOrientation(static_cast<f32>(std::atan2(h.y, h.x)) + kPi);
                m_frames.push_back(k);
                splinePath.push_back(node.position);
            }
        } else {
            mapChange = false;
        }
    }
    if (m_frames.size() < 2) {
        return false;
    }
    m_frames.back().teleport = true; // last to first is always a jump

    m_speed = speed;
    m_accel = accel;
    m_accelDist = 0.5f * speed * speed / accel;
    m_accelTime = speed / accel;

    i32 firstStop = -1, lastStop = -1;
    m_frames[0].distFromPrev = 0.0f;
    m_frames[0].index = 1;
    if (m_frames[0].stop()) {
        firstStop = lastStop = 0;
    }
    // One spline per stretch between jumps.
    usize start = 0;
    for (usize i = 1; i < m_frames.size(); ++i) {
        if (m_frames[i - 1].teleport || i + 1 == m_frames.size()) {
            const usize extra = !m_frames[i - 1].teleport ? 1 : 0;
            const u32 count = static_cast<u32>(i - start + extra);
            Spline spline;
            // InitCatmullRom (not cyclic): a mirrored point before, the last repeated after.
            spline.points.reserve(count + 2);
            spline.points.push_back(count >= 2 ? lerp(splinePath[start], splinePath[start + 1], -1.0f)
                                               : splinePath[start]);
            for (u32 c = 0; c < count; ++c) {
                spline.points.push_back(splinePath[start + c]);
            }
            spline.points.push_back(splinePath[start + count - 1]);
            spline.lengths = segmentLengths(spline.points, 1, count);
            const u32 splineIndex = static_cast<u32>(m_splines.size());
            m_splines.push_back(std::move(spline));
            const Spline& s = m_splines.back();
            for (usize j = start; j < i + extra; ++j) {
                m_frames[j].index = static_cast<u32>(j - start + 1);
                m_frames[j].distFromPrev =
                    static_cast<f32>(s.length(static_cast<u32>(j - start), static_cast<u32>(j + 1 - start)));
                if (j > 0) {
                    m_frames[j - 1].nextDistFromPrev = m_frames[j].distFromPrev;
                }
                m_frames[j].spline = splineIndex;
            }
            if (m_frames[i - 1].teleport) {
                m_frames[i].index = static_cast<u32>(i - start + 1);
                m_frames[i].distFromPrev = 0.0f;
                m_frames[i - 1].nextDistFromPrev = 0.0f;
                m_frames[i].spline = splineIndex;
            }
            start = i;
        }
        if (m_frames[i].stop()) {
            if (firstStop == -1) {
                firstStop = static_cast<i32>(i);
            }
            lastStop = static_cast<i32>(i);
        }
    }
    m_frames.back().nextDistFromPrev = m_frames.front().distFromPrev;
    if (firstStop == -1 || lastStop == -1) {
        firstStop = lastStop = 0;
    }
    const usize n = m_frames.size();
    // Distances since the last stop and until the next one.
    f32 tmp = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const usize j = (i + static_cast<usize>(lastStop)) % n;
        if (m_frames[j].stop() || j == static_cast<usize>(lastStop)) {
            tmp = 0.0f;
        } else {
            tmp += m_frames[j].distFromPrev;
        }
        m_frames[j].distSinceStop = tmp;
    }
    tmp = 0.0f;
    for (i32 i = static_cast<i32>(n) - 1; i >= 0; --i) {
        const usize j = (static_cast<usize>(i) + static_cast<usize>(firstStop)) % n;
        tmp += m_frames[(j + 1) % n].distFromPrev;
        m_frames[j].distUntilStop = tmp;
        if (m_frames[j].stop() || j == static_cast<usize>(firstStop)) {
            tmp = 0.0f;
        }
    }
    // Time until the next stop, with the acceleration at both ends.
    for (KeyFrame& k : m_frames) {
        const f32 total = k.distSinceStop + k.distUntilStop;
        if (total < 2.0f * m_accelDist) { // never reaches full speed
            if (k.distSinceStop < k.distUntilStop) {
                const f32 segment = 2.0f * std::sqrt((k.distUntilStop + k.distSinceStop) / accel);
                k.timeTo = segment - std::sqrt(2.0f * k.distSinceStop / accel);
            } else {
                k.timeTo = std::sqrt(2.0f * k.distUntilStop / accel);
            }
        } else if (k.distSinceStop < m_accelDist) {
            const f32 segment = (k.distUntilStop + k.distSinceStop) / speed + speed / accel;
            k.timeTo = segment - std::sqrt(2.0f * k.distSinceStop / accel);
        } else if (k.distUntilStop < m_accelDist) {
            k.timeTo = std::sqrt(2.0f * k.distUntilStop / accel);
        } else {
            k.timeTo = k.distUntilStop / speed + 0.5f * speed / accel;
        }
    }
    f32 segmentTime = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const usize j = (i + static_cast<usize>(lastStop)) % n;
        if (m_frames[j].stop() || j == static_cast<usize>(lastStop)) {
            segmentTime = m_frames[j].timeTo;
        }
        m_frames[j].timeFrom = segmentTime - m_frames[j].timeTo;
    }
    // Arrival and departure times.
    m_frames[0].arriveTime = 0;
    f32 pathTime = 0.0f;
    if (m_frames[0].stop()) {
        pathTime = static_cast<f32>(m_frames[0].node->delay);
        m_frames[0].departureTime = static_cast<u32>(pathTime * 1000.0f);
    }
    for (usize i = 1; i < n; ++i) {
        pathTime += m_frames[i - 1].timeTo;
        if (m_frames[i].stop()) {
            m_frames[i].arriveTime = static_cast<u32>(pathTime * 1000.0f);
            m_frames[i - 1].nextArriveTime = m_frames[i].arriveTime;
            pathTime += static_cast<f32>(m_frames[i].node->delay);
            m_frames[i].departureTime = static_cast<u32>(pathTime * 1000.0f);
        } else {
            pathTime -= m_frames[i].timeTo;
            m_frames[i].arriveTime = static_cast<u32>(pathTime * 1000.0f);
            m_frames[i - 1].nextArriveTime = m_frames[i].arriveTime;
            m_frames[i].departureTime = m_frames[i].arriveTime;
        }
    }
    m_frames.back().nextArriveTime = m_frames.back().departureTime;
    m_period = m_frames.back().departureTime;
    return m_period != 0;
}

f32 TransportPath::segmentPosition(const KeyFrame& frame, f32 now) const {
    const f32 departed = now - static_cast<f32>(frame.departureTime) * 0.001f;
    const f32 sinceStop = frame.timeFrom + departed;
    const f32 untilStop = frame.timeTo - departed;
    f32 position = 0.0f;
    if (sinceStop < untilStop) {
        const f32 dist = sinceStop < m_accelTime ? 0.5f * m_accel * sinceStop * sinceStop
                                                 : m_accelDist + (sinceStop - m_accelTime) * m_speed;
        position = dist - frame.distSinceStop;
    } else {
        const f32 dist = untilStop < m_accelTime ? 0.5f * m_accel * untilStop * untilStop
                                                 : m_accelDist + (untilStop - m_accelTime) * m_speed;
        position = frame.distUntilStop - dist;
    }
    return frame.nextDistFromPrev > 0.0f ? position / frame.nextDistFromPrev : 0.0f;
}

TransportPlacement TransportPath::at(u32 timeMs) const {
    TransportPlacement out;
    if (!valid()) {
        return out;
    }
    const u32 t = timeMs % m_period;
    for (const KeyFrame& frame : m_frames) {
        if (t >= frame.arriveTime && t < frame.departureTime) {
            // Waiting at a stop, where it arrived.
            out.map = frame.node->map;
            out.position = frame.node->position;
            out.orientation = frame.initialOrientation;
            return out;
        }
        if (t >= frame.departureTime && t < frame.nextArriveTime) {
            out.map = frame.node->map;
            if (frame.teleport || frame.spline >= m_splines.size() || frame.nextDistFromPrev <= 0.0f) {
                out.position = frame.node->position; // about to jump to the next map
                out.orientation = frame.initialOrientation;
                return out;
            }
            const Spline& spline = m_splines[frame.spline];
            if (frame.index == 0 || frame.index + 2 > spline.points.size()) {
                out.position = frame.node->position;
                out.orientation = frame.initialOrientation;
                return out;
            }
            const f32 u = std::clamp(segmentPosition(frame, static_cast<f32>(t) * 0.001f), 0.0f, 1.0f);
            out.position = spline.evaluate(frame.index, u);
            const glm::vec3 d = spline.derivative(frame.index, u);
            out.orientation = normalizeOrientation(std::atan2(d.y, d.x) + kPi);
            out.moving = true;
            return out;
        }
    }
    // Past the last frame's departure (the jump back to the first): where the cycle starts.
    out.map = m_frames.front().node->map;
    out.position = m_frames.front().node->position;
    out.orientation = m_frames.front().initialOrientation;
    return out;
}

// --- Elevators
// ------------------------------------------------------------------------------------------------

bool TransportAnimations::init(const pack::PackReader& db) {
    m_animations.clear();
    const std::optional<pack::PackAsset> asset =
        db.find(std::string("DBFilesClient\\TransportAnimation.dbc"));
    if (!asset) {
        VD_LOG_WARN("TransportAnimation.dbc missing");
        return false;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    // TransportAnimationfmt "diifffx": id, game object entry, time (ms), x, y, z, movement id.
    if (!parsed || parsed.value().fieldCount() < 6) {
        VD_LOG_WARN("TransportAnimation.dbc: unexpected layout");
        return false;
    }
    const db::DbcView& table = parsed.value();
    for (u32 r = 0; r < table.recordCount(); ++r) {
        add(table.getU32(r, 1), table.getU32(r, 2),
            {table.getF32(r, 3), table.getF32(r, 4), table.getF32(r, 5)});
    }
    VD_LOG_INFO("Transport animations: %zu", m_animations.size());
    return true;
}

void TransportAnimations::add(u32 entry, u32 timeMs, const glm::vec3& offset) {
    Animation& a = m_animations[entry];
    const auto at = std::lower_bound(a.nodes.begin(), a.nodes.end(), timeMs,
                                     [](const Node& n, u32 t) { return n.time < t; });
    a.nodes.insert(at, Node{timeMs, offset});
    a.period = std::max(a.period, timeMs); // TransportAnimation::TotalTime
}

u32 TransportAnimations::period(u32 entry) const {
    const auto found = m_animations.find(entry);
    return found == m_animations.end() ? 0 : found->second.period;
}

bool TransportAnimations::offsetAt(u32 entry, u32 timeMs, glm::vec3& offset) const {
    const auto found = m_animations.find(entry);
    if (found == m_animations.end() || found->second.period == 0 || found->second.nodes.empty()) {
        return false;
    }
    const Animation& a = found->second;
    const u32 t = timeMs % a.period;
    // GetNextAnimNode (the first at or after t) and GetPrevAnimNode (the one before it).
    const auto next =
        std::lower_bound(a.nodes.begin(), a.nodes.end(), t, [](const Node& n, u32 v) { return n.time < v; });
    if (next == a.nodes.end() || next == a.nodes.begin()) {
        offset = next == a.nodes.end() ? a.nodes.back().offset : next->offset;
        return true;
    }
    const auto previous = next - 1;
    if (previous->offset == next->offset || next->time == previous->time) {
        offset = previous->offset;
        return true;
    }
    const f32 f = static_cast<f32>(t - previous->time) / static_cast<f32>(next->time - previous->time);
    offset = previous->offset + (next->offset - previous->offset) * f;
    return true;
}

glm::vec3 transportToWorld(const TransportPlacement& transport, const glm::vec3& offset) {
    const f32 c = std::cos(transport.orientation), s = std::sin(transport.orientation);
    return {transport.position.x + offset.x * c - offset.y * s,
            transport.position.y + offset.y * c + offset.x * s, transport.position.z + offset.z};
}

glm::vec3 worldToTransport(const TransportPlacement& transport, const glm::vec3& world) {
    const f32 c = std::cos(transport.orientation), s = std::sin(transport.orientation);
    const f32 dx = world.x - transport.position.x, dy = world.y - transport.position.y;
    return {dx * c + dy * s, dy * c - dx * s, world.z - transport.position.z};
}

} // namespace vd::game
