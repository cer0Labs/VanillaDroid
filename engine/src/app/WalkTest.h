#pragma once

#include "core/Types.h"

#include <glm/vec3.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd {

class PlayerController;

// A walking route for the character controller (phase 6 acceptance): points of NPC patrols of the server's
// database (vMaNGOS creature_movement), which the server knows to be walkable, and their height there.
struct WalkPoint {
    f32 x;
    f32 y;
    f32 z;
};

struct WalkRoute {
    const char* id; // command line / intent name ("walk_stormwind")
    const char* name;
    const char* map; // Map.dbc directory
    std::span<const WalkPoint> points;
};

std::span<const WalkRoute> walkRoutes();
const WalkRoute* findWalkRoute(std::string_view id);

// A bot that walks a route with the real controller and input (like a player holding forward and turning
// towards the next point), and reports what matters for the collision: points reached, how far the feet are
// from the height the server has there, where it got stuck, falls. Also frame statistics, as a benchmark.
class WalkTest {
public:
    static constexpr f32 kWarmupSeconds = 6.0f; // the tiles and textures around the start stream in

    // `online`: the server's character (never carried past an obstacle: that would be a teleport to the server).
    void start(const WalkRoute& route, bool online = false);
    void stop();
    bool running() const { return m_route != nullptr; }
    const WalkRoute* route() const { return m_route; }
    f32 progress() const;

    // Drives `player` (its bot input) for this frame. Returns false once the route is done (report ready).
    bool update(PlayerController& player, f32 dt);
    void record(f32 frameMs, f32 cpuMs, f32 gpuMs, f32 playerMs);
    const std::string& report() const { return m_report; }

private:
    void finish(PlayerController& player);

    const WalkRoute* m_route = nullptr;
    u32 m_next = 1;
    f32 m_warmup = 0.0f;
    f32 m_time = 0.0f;
    f32 m_best = 1e9f;     // closest to the current point so far
    f32 m_sinceBest = 0.0f; // seconds without getting closer
    u32 m_detour = 0;       // blocked: 1 around the right, 2 around the left, 3 jumping
    bool m_online = false;
    u32 m_first = 1; // first point walked to (online: the closest to where the character was)
    f32 m_sinceJump = 0.0f;
    u32 m_jumps = 0;
    u32 m_reached = 0;
    u32 m_stuck = 0;
    u32 m_falls = 0;
    f32 m_worstFall = 0.0f;
    f32 m_airTop = -1e9f;
    bool m_inAir = false;
    f64 m_travelled = 0.0;
    glm::dvec3 m_last{0.0};
    f32 m_errorSum = 0.0f;
    f32 m_errorMax = 0.0f;
    u32 m_errorCount = 0;
    u32 m_farPoints = 0; // reached with the feet more than a yard off the server's height
    struct Sample {
        f32 frameMs, cpuMs, gpuMs, playerMs;
    };
    std::vector<Sample> m_samples;
    std::string m_report;
};

} // namespace vd
