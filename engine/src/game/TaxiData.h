#pragma once

#include "core/Types.h"

#include <glm/vec3.hpp>

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// Known taxi nodes: one bit per TaxiNodes.dbc id (SMSG_SHOWTAXINODES' mask, 8 words).
using TaxiMask = std::array<u32, 8>;
constexpr bool taxiNodeKnown(const TaxiMask& mask, u32 node) {
    return node < 256 && (mask[node / 32] & (1u << (node % 32))) != 0;
}

// One TaxiNodes.dbc row (16 fields): a flight master's node.
struct TaxiNodeInfo {
    u32 id = 0;
    u32 map = 0;
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    std::string_view name; // enUS: "Stormwind, Elwynn"
    u32 mountHorde = 0;    // creature entries of the taxi mount (0: not this faction's node)
    u32 mountAlliance = 0;
};

// One TaxiPath.dbc row plus its TaxiPathNode.dbc points (sorted by index).
struct TaxiPathInfo {
    u32 id = 0;
    u32 from = 0, to = 0; // TaxiNodes ids
    u32 cost = 0;         // copper
    f32 length = 0.0f;    // yards along its points
    u32 firstPoint = 0;   // into TaxiData::points()
    u32 pointCount = 0;
};

// One TaxiPathNode.dbc row (9 fields).
struct TaxiPathPoint {
    u32 path = 0;
    u32 index = 0;
    u32 map = 0;
    glm::vec3 position{0.0f};
    u32 flags = 0; // 1: a map change follows (transports), 2: a stop (transports wait `delay` seconds)
    u32 delay = 0; // seconds
};

// A flight from one node to another through known nodes (the client's TaxiFrame route).
struct TaxiRoute {
    std::array<u32, 16> nodes{}; // start, hops..., destination
    u32 count = 0;
    u32 cost = 0;      // copper: the sum of the paths' prices (the server charges the same)
    f32 length = 0.0f; // yards flown
    std::span<const u32> list() const { return {nodes.data(), count}; }
};

// Flight master tables (phase 8): nodes, paths with their prices and points. Transports (boats, zeppelins)
// fly TaxiPathNode paths too. Indexes only; names are views into the mapped db pack.
class TaxiData {
public:
    bool init(const pack::PackReader& db);
    // The tables from elsewhere (tests, tools): paths need only id, from, to and cost (their points and
    // lengths are looked up here).
    void set(std::vector<TaxiNodeInfo> nodes, std::vector<TaxiPathInfo> paths,
             std::vector<TaxiPathPoint> points);
    bool ready() const { return !m_nodes.empty(); }

    std::span<const TaxiNodeInfo> nodes() const { return m_nodes; }
    const TaxiNodeInfo* node(u32 id) const;
    // The direct path between two nodes, nullptr if there is none.
    const TaxiPathInfo* path(u32 from, u32 to) const;
    const TaxiPathInfo* pathById(u32 id) const;
    std::span<const TaxiPathPoint> points(const TaxiPathInfo& path) const;
    std::span<const TaxiPathPoint> points() const { return m_points; }

    // The shortest flight (in yards) from `from` to `to` hopping only through nodes of `known` on the same
    // map (the destination must be known too). False when there is none.
    bool route(u32 from, u32 to, const TaxiMask& known, TaxiRoute& out) const;

private:
    std::vector<TaxiNodeInfo> m_nodes;
    std::vector<TaxiPathInfo> m_paths;   // sorted by (from, to)
    std::vector<TaxiPathPoint> m_points; // by path, then index
    // Route search scratch (mutable: route() is const and allocation-free once warmed up).
    mutable std::vector<f32> m_distance;
    mutable std::vector<u32> m_previous;
    mutable std::vector<u8> m_done;
};

} // namespace vd::game
