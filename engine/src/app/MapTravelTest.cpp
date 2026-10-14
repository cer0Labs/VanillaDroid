#include "app/MapTravelTest.h"

#include "core/DevEnv.h"
#include "core/Log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vd {

bool MapTravelTest::init() {
    const char* spec = VD_DEV_ENV("VD_WORLD_TRAVEL");
    if (!spec || !*spec) {
        return false;
    }
    std::string_view text(spec);
    if (const usize at = text.rfind('@'); at != std::string_view::npos) {
        m_period = std::max(1.0f, static_cast<f32>(std::atof(std::string(text.substr(at + 1)).c_str())));
        text = text.substr(0, at);
    }
    while (!text.empty()) {
        const usize end = std::min(text.find_first_of(";+"), text.size());
        const std::string_view item = text.substr(0, end);
        text = end < text.size() ? text.substr(end + 1) : std::string_view{};
        const usize colon = item.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        Stop stop;
        stop.map.assign(item.substr(0, colon));
        std::string coordinates(item.substr(colon + 1));
        std::replace(coordinates.begin(), coordinates.end(), '/', ','); // the phones' env extra splits at commas
        if (std::sscanf(coordinates.c_str(), "%lf,%lf,%lf", &stop.position.x, &stop.position.y, &stop.position.z) == 3) {
            m_stops.push_back(std::move(stop));
        }
    }
    if (!m_stops.empty()) {
        VD_LOG_INFO("VD_WORLD_TRAVEL: %zu stops, every %.0f s", m_stops.size(), m_period);
    }
    return !m_stops.empty();
}

const MapTravelTest::Stop* MapTravelTest::update(f32 dt, bool settled) {
    if (m_stops.empty()) {
        return nullptr;
    }
    if (m_measuring) {
        if (settled) {
            finish();
        }
        return nullptr;
    }
    m_settledFor = settled ? m_settledFor + dt : 0.0f;
    if (m_settledFor < m_period && !(m_visits == 0 && m_settledFor > 1.0f)) {
        return nullptr; // the first stop as soon as the start settles, the others after the period
    }
    const Stop& stop = m_stops[m_next];
    m_next = (m_next + 1) % static_cast<u32>(m_stops.size());
    ++m_visits;
    m_settledFor = 0.0f;
    m_measuring = true;
    m_elapsedMs = 0.0;
    m_frames = 0;
    m_worstMs = 0.0f;
    m_over33 = 0;
    m_over20 = 0;
    m_from = m_to;
    m_to = stop.map;
    return &stop;
}

void MapTravelTest::record(f32 frameMs) {
    if (!m_measuring) {
        return;
    }
    m_elapsedMs += frameMs;
    ++m_frames;
    m_worstMs = std::max(m_worstMs, frameMs);
    m_over33 += frameMs > 33.0f ? 1u : 0u;
    m_over20 += frameMs > 20.0f ? 1u : 0u;
}

void MapTravelTest::finish() {
    m_measuring = false;
    VD_LOG_INFO("TRAVEL %s -> %s: ready in %.0f ms, %u frames, worst %.1f ms, %u over 33 ms, %u over 20 ms",
                m_from.empty() ? "start" : m_from.c_str(), m_to.c_str(), m_elapsedMs, m_frames, m_worstMs, m_over33,
                m_over20);
    if (m_visits > 1) { // the first travel leaves the start map, still settling: not counted
        m_allWorstMs = std::max(m_allWorstMs, m_worstMs);
        m_allOver33 += m_over33;
    }
    if (m_visits == m_stops.size() + 1) {
        VD_LOG_INFO("TRAVEL done: %u travels, worst frame %.1f ms, %u frames over 33 ms", m_visits - 1, m_allWorstMs,
                    m_allOver33);
    }
}

} // namespace vd
