#include "game/TaxiData.h"

#include "core/Log.h"
#include "db/DbcView.h"
#include "pack/PackReader.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace vd::game {
namespace {

// TaxiNodes.dbc (TaxiNodesEntryfmt "nifffssssssssxii"): id, map, x, y, z, name (enUS at 5), 14 horde mount,
// 15 alliance mount. TaxiPath.dbc: id, from, to, price. TaxiPathNode.dbc: id, path, index, map, x, y, z,
// flags, delay.
namespace node {
constexpr u32 kMap = 1, kX = 2, kName = 5, kMountHorde = 14, kMountAlliance = 15, kFieldCount = 16;
}
constexpr u32 kPathFieldCount = 4;
constexpr u32 kPointFieldCount = 9;
constexpr u32 kMaxNodeId = 256; // TaxiMask bits

std::optional<db::DbcView> table(const pack::PackReader& db, const char* name, u32 minFields) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    if (!asset) {
        VD_LOG_WARN("%s.dbc missing", name);
        return std::nullopt;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed || parsed.value().fieldCount() < minFields) {
        VD_LOG_WARN("%s.dbc: unexpected layout", name);
        return std::nullopt;
    }
    return parsed.value();
}

} // namespace

bool TaxiData::init(const pack::PackReader& db) {
    const std::optional<db::DbcView> nodeTable = table(db, "TaxiNodes", node::kFieldCount);
    const std::optional<db::DbcView> pathTable = table(db, "TaxiPath", kPathFieldCount);
    const std::optional<db::DbcView> pointTable = table(db, "TaxiPathNode", kPointFieldCount);
    if (!nodeTable || !pathTable || !pointTable) {
        return false;
    }
    std::vector<TaxiNodeInfo> nodes;
    for (u32 r = 0; r < nodeTable->recordCount(); ++r) {
        TaxiNodeInfo n;
        n.id = nodeTable->getU32(r, 0);
        n.map = nodeTable->getU32(r, node::kMap);
        n.x = nodeTable->getF32(r, node::kX);
        n.y = nodeTable->getF32(r, node::kX + 1);
        n.z = nodeTable->getF32(r, node::kX + 2);
        n.name = nodeTable->getString(r, node::kName);
        n.mountHorde = nodeTable->getU32(r, node::kMountHorde);
        n.mountAlliance = nodeTable->getU32(r, node::kMountAlliance);
        nodes.push_back(n);
    }
    std::vector<TaxiPathPoint> points;
    points.reserve(pointTable->recordCount());
    for (u32 r = 0; r < pointTable->recordCount(); ++r) {
        TaxiPathPoint p;
        p.path = pointTable->getU32(r, 1);
        p.index = pointTable->getU32(r, 2);
        p.map = pointTable->getU32(r, 3);
        p.position = {pointTable->getF32(r, 4), pointTable->getF32(r, 5), pointTable->getF32(r, 6)};
        p.flags = pointTable->getU32(r, 7);
        p.delay = pointTable->getU32(r, 8);
        points.push_back(p);
    }
    std::vector<TaxiPathInfo> paths;
    paths.reserve(pathTable->recordCount());
    for (u32 r = 0; r < pathTable->recordCount(); ++r) {
        TaxiPathInfo p;
        p.id = pathTable->getU32(r, 0);
        p.from = pathTable->getU32(r, 1);
        p.to = pathTable->getU32(r, 2);
        p.cost = pathTable->getU32(r, 3);
        paths.push_back(p);
    }
    set(std::move(nodes), std::move(paths), std::move(points));
    VD_LOG_INFO("Taxi: %zu nodes, %zu paths, %zu points", m_nodes.size(), m_paths.size(), m_points.size());
    return true;
}

void TaxiData::set(std::vector<TaxiNodeInfo> nodes, std::vector<TaxiPathInfo> paths,
                   std::vector<TaxiPathPoint> points) {
    m_nodes.clear();
    for (const TaxiNodeInfo& n : nodes) {
        if (n.id < kMaxNodeId) {
            m_nodes.push_back(n);
        }
    }
    m_points = std::move(points);
    std::sort(m_points.begin(), m_points.end(), [](const TaxiPathPoint& a, const TaxiPathPoint& b) {
        return a.path != b.path ? a.path < b.path : a.index < b.index;
    });
    m_paths = std::move(paths);
    for (TaxiPathInfo& p : m_paths) {
        const auto first = std::lower_bound(m_points.begin(), m_points.end(), p.id,
                                            [](const TaxiPathPoint& a, u32 id) { return a.path < id; });
        auto last = first;
        while (last != m_points.end() && last->path == p.id) {
            ++last;
        }
        p.firstPoint = static_cast<u32>(first - m_points.begin());
        p.pointCount = static_cast<u32>(last - first);
        p.length = 0.0f;
        for (auto it = first; it != last && it + 1 != last; ++it) {
            p.length += glm::length((it + 1)->position - it->position);
        }
    }
    std::sort(m_paths.begin(), m_paths.end(), [](const TaxiPathInfo& a, const TaxiPathInfo& b) {
        return a.from != b.from ? a.from < b.from : a.to < b.to;
    });
    m_distance.assign(kMaxNodeId, 0.0f);
    m_previous.assign(kMaxNodeId, 0);
    m_done.assign(kMaxNodeId, 0);
}

const TaxiNodeInfo* TaxiData::node(u32 id) const {
    for (const TaxiNodeInfo& n : m_nodes) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

const TaxiPathInfo* TaxiData::path(u32 from, u32 to) const {
    const auto found =
        std::lower_bound(m_paths.begin(), m_paths.end(), std::pair<u32, u32>{from, to},
                         [](const TaxiPathInfo& p, const std::pair<u32, u32>& key) {
                             return p.from != key.first ? p.from < key.first : p.to < key.second;
                         });
    return found != m_paths.end() && found->from == from && found->to == to ? &*found : nullptr;
}

const TaxiPathInfo* TaxiData::pathById(u32 id) const {
    for (const TaxiPathInfo& p : m_paths) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

std::span<const TaxiPathPoint> TaxiData::points(const TaxiPathInfo& path) const {
    return {m_points.data() + path.firstPoint, path.pointCount};
}

bool TaxiData::route(u32 from, u32 to, const TaxiMask& known, TaxiRoute& out) const {
    out = TaxiRoute{};
    const TaxiNodeInfo* start = node(from);
    const TaxiNodeInfo* end = node(to);
    if (!start || !end || from == to || start->map != end->map || !taxiNodeKnown(known, to) ||
        m_distance.size() < kMaxNodeId) {
        return false;
    }
    // Dijkstra over the paths between known nodes (a few hundred edges: linear scans are fine).
    constexpr f32 kUnreached = std::numeric_limits<f32>::max();
    std::fill(m_distance.begin(), m_distance.end(), kUnreached);
    std::fill(m_done.begin(), m_done.end(), u8{0});
    m_distance[from] = 0.0f;
    while (true) {
        u32 current = kMaxNodeId;
        for (u32 n = 0; n < kMaxNodeId; ++n) {
            if (!m_done[n] && m_distance[n] < kUnreached &&
                (current == kMaxNodeId || m_distance[n] < m_distance[current])) {
                current = n;
            }
        }
        if (current == kMaxNodeId || current == to) {
            break;
        }
        m_done[current] = 1;
        const auto first = std::lower_bound(m_paths.begin(), m_paths.end(), current,
                                            [](const TaxiPathInfo& p, u32 id) { return p.from < id; });
        for (auto it = first; it != m_paths.end() && it->from == current; ++it) {
            const u32 next = it->to;
            if (next >= kMaxNodeId || m_done[next] || !taxiNodeKnown(known, next) || it->pointCount < 2) {
                continue;
            }
            const TaxiNodeInfo* n = node(next);
            if (!n || n->map != start->map) {
                continue;
            }
            const f32 d = m_distance[current] + it->length;
            if (d < m_distance[next]) {
                m_distance[next] = d;
                m_previous[next] = current;
            }
        }
    }
    if (m_distance[to] == kUnreached) {
        return false;
    }
    // Walk back from the destination.
    std::array<u32, 16> reversed{};
    u32 count = 0;
    for (u32 n = to; count < reversed.size(); n = m_previous[n]) {
        reversed[count++] = n;
        if (n == from) {
            break;
        }
    }
    if (reversed[count - 1] != from) {
        return false; // more hops than a route holds
    }
    for (u32 i = 0; i < count; ++i) {
        out.nodes[i] = reversed[count - 1 - i];
    }
    out.count = count;
    out.length = m_distance[to];
    for (u32 i = 0; i + 1 < count; ++i) {
        if (const TaxiPathInfo* p = path(out.nodes[i], out.nodes[i + 1])) {
            out.cost += p->cost;
        }
    }
    return true;
}

} // namespace vd::game
