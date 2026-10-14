#pragma once

#include "core/Types.h"

#include <glm/vec3.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace vd {

// Phase 18 (world), development: far teleports offline, one after another, each measured from the moment the map
// starts to open until the ground under the destination is there. VD_WORLD_TRAVEL=<map>:<x>,<y>,<z>[;...][@<seconds>]
// (Map.dbc directories; the list loops), e.g. "DeadminesInstance:-14.6,-385.5,62.5;Azeroth:-9457,65.7,56@8". Each
// arrival is logged as a line "TRAVEL <from> -> <to>: ready in .. ms, .. frames, worst .. ms, .. over 33 ms, .. over
// 20 ms" and, once every stop has been visited, "TRAVEL done" with the worst of all. On a phone (bench.ps1
// -EnvSwitches splits at commas, adb's shell at semicolons): "DeadminesInstance:-14.6/-385.5/62.5+Azeroth:...".
class MapTravelTest {
public:
    struct Stop {
        std::string map;
        glm::dvec3 position{0.0};
    };

    // Reads VD_WORLD_TRAVEL; false when it is not set (or names no stop).
    bool init();
    bool active() const { return !m_stops.empty(); }

    // Once per frame. `settled`: the current map stands loaded (its ground under the camera); the next stop is due
    // `period` seconds after it settled. Returns the stop to travel to now, or nullptr.
    const Stop* update(f32 dt, bool settled);
    // Every frame's time while a travel is measured (from update() returning a stop until `settled`).
    void record(f32 frameMs);
    bool measuring() const { return m_measuring; }

private:
    void finish();

    std::vector<Stop> m_stops;
    f32 m_period = 8.0f;
    u32 m_next = 0;
    u32 m_visits = 0;
    f32 m_settledFor = 0.0f;
    bool m_measuring = false;
    std::string m_from;
    std::string m_to;
    f64 m_elapsedMs = 0.0;
    u32 m_frames = 0;
    f32 m_worstMs = 0.0f;
    u32 m_over33 = 0;
    u32 m_over20 = 0;
    f32 m_allWorstMs = 0.0f;
    u32 m_allOver33 = 0;
};

} // namespace vd
