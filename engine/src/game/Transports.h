#pragma once

#include "core/Types.h"
#include "game/TaxiData.h"

#include <glm/vec3.hpp>

#include <span>
#include <unordered_map>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// Game object types that carry passengers (GameobjectTypes).
constexpr u32 kGameObjectTransport = 11;   // elevators and trams: TransportAnimation.dbc
constexpr u32 kGameObjectMoTransport = 15; // ships and zeppelins: a TaxiPathNode path

// Where a transport is at a moment of its cycle.
struct TransportPlacement {
    u32 map = 0;
    glm::vec3 position{0.0f};
    f32 orientation = 0.0f; // radians, as the server's (the ship's bow points along -X of its model)
    bool moving = false;    // false: waiting at a stop (or about to change maps)
};

// The cycle of a ship or zeppelin (MO_TRANSPORT): the TaxiPathNode path of its game object (data[0]) walked
// at data[1] yards per second with data[2] yards per second² of acceleration, stopping `delay` seconds at the
// points flagged 2 and jumping between maps at the points flagged 1 (or where the map changes). The server
// only tells the time into the cycle (the create block's transport time); the position is computed on both
// sides the same way: this follows vMaNGOS TransportMgr::GenerateWaypoints and ShipTransport::Update /
// CalculateSegmentPos.
class TransportPath {
public:
    // False when the path is too short to form a cycle.
    bool build(std::span<const TaxiPathPoint> points, f32 speed, f32 accel);
    bool valid() const { return m_period != 0; }
    // Length of one cycle, ms (vMaNGOS' computed one; the server may override it from its `transports`
    // table).
    u32 period() const { return m_period; }
    u32 keyFrameCount() const { return static_cast<u32>(m_frames.size()); }
    // At `timeMs` into the cycle (any value: taken modulo the period).
    TransportPlacement at(u32 timeMs) const;

private:
    // A Catmull-Rom spline over a run of keyframe points (one per stretch between map changes), with the
    // server's virtual end points and its segment lengths (SplineBase::SegLengthCatmullRom, 3 steps).
    struct Spline {
        std::vector<glm::vec3> points; // virtual, controls..., virtual
        std::vector<f64> lengths;      // lengths[i]: from the first control to control i - 1
        glm::vec3 evaluate(u32 index, f32 u) const;
        glm::vec3 derivative(u32 index, f32 u) const;
        f64 length(u32 first, u32 last) const { return lengths[last] - lengths[first]; }
    };
    struct KeyFrame {
        const TaxiPathPoint* node = nullptr;
        u32 index = 0; // segment of its spline that leaves it
        u32 spline = ~0u;
        f32 initialOrientation = 0.0f;
        f32 distSinceStop = -1.0f, distUntilStop = -1.0f, distFromPrev = -1.0f;
        f32 timeFrom = 0.0f, timeTo = 0.0f;
        bool teleport = false;
        u32 arriveTime = 0, departureTime = 0;
        f32 nextDistFromPrev = 0.0f;
        u32 nextArriveTime = 0;
        bool stop() const { return node->flags == 2; }
    };
    f32 segmentPosition(const KeyFrame& frame, f32 nowSeconds) const;

    std::vector<TaxiPathPoint> m_points; // owned copy (KeyFrame::node points in here)
    std::vector<KeyFrame> m_frames;
    std::vector<Spline> m_splines;
    f32 m_speed = 0.0f, m_accel = 0.0f, m_accelTime = 0.0f, m_accelDist = 0.0f;
    u32 m_period = 0;
};

// Elevators and trams (GAMEOBJECT_TYPE_TRANSPORT): TransportAnimation.dbc offsets from the object's own place
// by time, as vMaNGOS ElevatorTransport::Update moves them (linear between the nodes).
class TransportAnimations {
public:
    bool init(const pack::PackReader& db);
    // Tests and tools.
    void add(u32 entry, u32 timeMs, const glm::vec3& offset);
    bool has(u32 entry) const { return m_animations.find(entry) != m_animations.end(); }
    // Length of an entry's cycle, ms (0: none).
    u32 period(u32 entry) const;
    // Offset of `entry` at `timeMs` into its cycle (modulo the period), before the object's rotation.
    bool offsetAt(u32 entry, u32 timeMs, glm::vec3& offset) const;

private:
    struct Node {
        u32 time = 0;
        glm::vec3 offset{0.0f};
    };
    struct Animation {
        std::vector<Node> nodes; // by time
        u32 period = 0;          // the last node's time
    };
    std::unordered_map<u32, Animation> m_animations; // game object entry -> its nodes
};

// A passenger's world position from its offset on a transport (GenericTransport::CalculatePassengerPosition),
// and back (CalculatePassengerOffset).
glm::vec3 transportToWorld(const TransportPlacement& transport, const glm::vec3& offset);
glm::vec3 worldToTransport(const TransportPlacement& transport, const glm::vec3& world);

} // namespace vd::game
