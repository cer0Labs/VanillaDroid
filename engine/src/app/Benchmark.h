#pragma once

#include "core/Types.h"
#include "world/Camera.h"

#include <span>
#include <string>
#include <vector>

namespace vd {

// Recorded camera flights over fixed routes (docs/PERFORMANCE.md), so every run renders exactly the same
// thing and results can be compared before/after a change and between devices.
struct BenchmarkWaypoint {
    static constexpr f32 kNoFloor = -1.0e6f;
    f64 x;
    f64 y;
    f32 floor = kNoFloor; // walkable height here (a WMO floor, a pier): the camera never goes below it
};

struct BenchmarkRoute {
    const char* id; // command line / intent name
    const char* name;
    const char* map; // Map.dbc directory
    f32 speed;       // yards per second
    f32 height;      // yards above the ground (or the waypoint floors)
    std::span<const BenchmarkWaypoint> waypoints;
    bool ignoreGround = false; // under a mountain (Ironforge): follow only the waypoint floors
    // Orbits: the camera looks at this point instead of along the route (0, 0 = along the route).
    f64 lookX = 0.0;
    f64 lookY = 0.0;
    u32 crowd = 0; // synthetic characters spawned around the look-at point for the run (phase 5 acceptance)
    u32 effects = 0; // phase 9: spell effects and fiery creatures spawned around the look-at point instead
    u32 fight = 0;   // phase 9: a group fight with spells (app/CrowdFight) of this many fighters around it
    // Phase 10 (soak): other routes flown one after the other, round and round, for `minutes` (PlatformInfo::
    // benchMinutes overrides it). This route's own waypoints then only give the start and where its crowd or fight
    // stands; a report comes every minute (Benchmark::takeMinute).
    std::span<const char* const> legs = {};
    f32 minutes = 0.0f;
};

std::span<const BenchmarkRoute> benchmarkRoutes();
const BenchmarkRoute* findBenchmarkRoute(std::string_view id);

struct BenchmarkSample {
    f32 frameMs;
    f32 cpuMs;
    f32 gpuMs;
    u32 drawCalls;
    u32 triangles;
};

// Phase 10: one minute of a looping route.
struct BenchmarkMinute {
    u32 index = 0; // 1, 2...
    u32 frames = 0;
    f32 fps = 0.0f;
    f32 p50Ms = 0.0f;
    f32 p99Ms = 0.0f;
    f32 maxMs = 0.0f;
    u32 over20 = 0;
    u32 over33 = 0;
    f32 gpuMs = 0.0f; // averages
    f32 cpuMs = 0.0f;
};

class Benchmark {
public:
    // Warm-up seconds at the start position before measuring (tiles stream in, pipelines warm up).
    static constexpr f32 kWarmupSeconds = 8.0f; // cities: the largest WMOs and their textures stream in

    void start(const BenchmarkRoute& route);
    void stop();
    bool running() const { return m_route != nullptr; }
    bool warmingUp() const { return m_warmup > 0.0f; }
    const BenchmarkRoute* route() const { return m_route; }
    f32 progress() const; // 0..1
    // Phase 10: minutes of the next looping route instead of its own (0 = its own).
    void setMinutes(f32 minutes) { m_minutesOverride = minutes; }
    bool looping() const { return m_duration > 0.0f; }
    f32 measuredSeconds() const { return m_elapsed; } // since the warm-up ended
    // A minute of a looping route has just ended: its numbers, once.
    bool takeMinute(BenchmarkMinute& out);

    // Moves the camera along the route. groundHeight: terrain under the camera, if known. Returns false
    // when the route has finished (the report is ready).
    bool update(Camera& camera, f32 dt, const f32* groundHeight);
    void record(const BenchmarkSample& sample);

    // Text summary of the last finished run ("" if none).
    const std::string& report() const { return m_report; }

private:
    void finish();
    void closeMinute();
    bool nextLeg();

    const BenchmarkRoute* m_route = nullptr;
    const BenchmarkRoute* m_leg = nullptr; // the route flown now: m_route, or one of its legs
    u32 m_legIndex = 0;
    f32 m_duration = 0.0f; // seconds a looping route lasts (0: once)
    f32 m_elapsed = 0.0f;
    f32 m_minutesOverride = 0.0f;
    // The minute being measured.
    std::vector<f32> m_minuteFrames;
    f32 m_minuteSeconds = 0.0f;
    f64 m_minuteGpu = 0.0;
    f64 m_minuteCpu = 0.0;
    u32 m_minuteIndex = 0;
    BenchmarkMinute m_minute;
    bool m_minuteReady = false;
    u32 m_segment = 0;
    f64 m_along = 0.0; // yards along the current segment
    f32 m_warmup = 0.0f;
    f32 m_z = 0.0f;
    f64 m_totalLength = 0.0;
    f64 m_travelled = 0.0;
    std::vector<BenchmarkSample> m_samples;
    std::string m_report;
};

} // namespace vd
