#include "app/Benchmark.h"

#include "core/Log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace vd {
namespace {

// Phase 2 acceptance routes (ROADMAP F2) plus a short one for quick checks.
constexpr BenchmarkWaypoint kElwynn[] = {
    {-9464.0, 62.0},   // Goldshire
    {-9800.0, 500.0},  // towards Westfall
    {-10628.0, 1037.0}, // Sentinel Hill
    {-10600.0, 0.0},
    {-10560.0, -1180.0}, // Darkshire
};
constexpr BenchmarkWaypoint kDurotar[] = {
    {1370.0, -4380.0}, // Orgrimmar gates
    {315.0, -4743.0},  // Razor Hill
    {280.0, -3800.0},  // bridge into the Barrens
    {-441.0, -2596.0}, // the Crossroads
};
constexpr BenchmarkWaypoint kBarrens[] = {
    {-441.0, -2596.0},  // the Crossroads
    {-2375.0, -1880.0}, // Camp Taurajo
    {-4200.0, -1900.0}, // southern Barrens
};
// Phase 3 acceptance (ROADMAP F3): the cities, at street level, and the Stranglethorn jungle. Floors come
// from vMaNGOS game_tele.
constexpr BenchmarkWaypoint kStormwind[] = {
    {-9110.0, 420.0, 93.0f},    // gate bridge
    {-8833.4, 628.6, 94.0f},    // Trade District
    {-8976.2, 876.1, 106.5f},   // Mage Quarter
    {-8778.0, 1066.6, 90.8f},   // the Park
    {-8623.5, 778.2, 96.7f},    // Cathedral Square
    {-8448.9, 591.9, 93.8f},    // Dwarven District
    {-8452.0, 346.9, 120.9f},   // the Keep
    {-8722.4, 412.8, 98.9f},    // Old Town
    {-8833.4, 628.6, 94.0f},    // Trade District
};
constexpr BenchmarkWaypoint kOrgrimmar[] = {
    {1370.0, -4380.0},         // gates
    {1548.1, -4417.2, 9.5f},    // Valley of Strength
    {1913.5, -4490.1, 23.2f},   // the Drag
    {1936.5, -4673.7, 26.4f},   // Valley of Honor
    {1913.5, -4490.1, 23.2f},   // the Drag
    {1931.3, -4284.5, 28.4f},   // Valley of Wisdom
    {1924.9, -4205.2, 42.0f},   // Grommash Hold
    {1529.7, -4210.2, 41.4f},   // Valley of Spirits
    {1629.4, -4373.4, 31.3f},   // Valley of Strength
};
constexpr BenchmarkWaypoint kIronforge[] = {
    {-5040.5, -814.8, 495.1f},  // gates
    {-4918.9, -940.4, 501.6f},  // the Commons
    {-4662.1, -952.7, 500.4f},  // Mystic Ward
    {-4675.8, -1256.3, 502.0f}, // Hall of Explorers
    {-4831.6, -1279.2, 501.9f}, // Tinker Town
    {-4968.9, -1225.5, 501.7f}, // Military Ward
    {-4793.0, -1119.1, 498.8f}, // the Great Forge
    {-4918.9, -940.4, 501.6f},  // the Commons
};
constexpr BenchmarkWaypoint kStranglethorn[] = {
    {-11322.4, -202.5},        // Rebel Camp
    {-11609.3, -53.0},         // Nesingwary's Expedition
    {-12135.0, 17.9},          // Kal'ai Ruins
    {-12388.9, 172.6},         // Grom'gol
    {-12424.8, -148.7},        // Mizjah Ruins
    {-12644.3, -377.4},
    {-13304.6, -428.9},        // Crystalvein Mine
    {-14297.2, 531.0, 8.8f},    // Booty Bay (over the water: the piers are the floor)
};
constexpr BenchmarkWaypoint kGoldshire[] = {
    {-9464.0, 62.0},
    {-9300.0, 300.0},
    {-9100.0, 0.0},
    {-9464.0, 62.0},
};

// Phase 5 acceptance (ROADMAP F5): 40 animated characters in front of the Orgrimmar bank, the camera circling
// them at street level.
// The square in front of the bank (the bankers stand at 1622.9, -4369.1, 11.8; guards around it at z 9.7-12.4,
// from vMaNGOS' creature spawns): the characters never go below this floor.
constexpr f64 kBankX = 1605.0;
constexpr f64 kBankY = -4388.0;
constexpr f32 kBankFloor = 9.6f;
constexpr BenchmarkWaypoint kCrowd[] = {
    {kBankX + 26.0, kBankY + 0.0, kBankFloor},  {kBankX + 18.4, kBankY + 18.4, kBankFloor},
    {kBankX + 0.0, kBankY + 26.0, kBankFloor},  {kBankX - 18.4, kBankY + 18.4, kBankFloor},
    {kBankX - 26.0, kBankY + 0.0, kBankFloor},  {kBankX - 18.4, kBankY - 18.4, kBankFloor},
    {kBankX + 0.0, kBankY - 26.0, kBankFloor},  {kBankX + 18.4, kBankY - 18.4, kBankFloor},
    {kBankX + 26.0, kBankY + 0.0, kBankFloor},
};

constexpr f32 kMountSpeed = 28.0f; // yd/s, a little faster than an epic mount
constexpr f32 kCameraHeight = 30.0f;
constexpr f32 kCitySpeed = 14.0f;  // epic mount
constexpr f32 kStreetHeight = 6.0f; // third-person camera over a player on foot

// Phase 10 acceptance: half an hour in Orgrimmar, the city at street level and then the group fight of 40 in front of
// the bank (which keeps fighting while the camera tours the city), over and over.
constexpr const char* kSoakLegs[] = {"orgrimmar", "fight"};
constexpr f32 kSoakMinutes = 30.0f;

constexpr BenchmarkRoute kRoutes[] = {
    {"elwynn", "Elwynn > Westfall > Duskwood", "Azeroth", kMountSpeed, kCameraHeight, kElwynn},
    {"durotar", "Durotar > The Barrens", "Kalimdor", kMountSpeed, kCameraHeight, kDurotar},
    {"barrens", "The Barrens, north > south", "Kalimdor", kMountSpeed, kCameraHeight, kBarrens},
    {"stormwind", "Stormwind", "Azeroth", kCitySpeed, kStreetHeight, kStormwind},
    {"orgrimmar", "Orgrimmar", "Kalimdor", kCitySpeed, kStreetHeight, kOrgrimmar},
    {"ironforge", "Ironforge", "Azeroth", kCitySpeed, kStreetHeight, kIronforge, true},
    {"stranglethorn", "Stranglethorn Vale > Booty Bay", "Azeroth", kMountSpeed, 12.0f, kStranglethorn},
    {"goldshire", "Goldshire (short)", "Azeroth", kMountSpeed, kCameraHeight, kGoldshire},
    {"crowd", "Crowd: 40 characters (Orgrimmar's bank)", "Kalimdor", 2.5f, 5.0f, kCrowd, false, kBankX, kBankY, 40},
    {"crowd0", "Crowd: the same lap without characters (reference)", "Kalimdor", 2.5f, 5.0f, kCrowd, false, kBankX,
     kBankY, 0},
    {"fxcrowd", "Effects: 40 spell effects and fire elementals (Orgrimmar's bank)", "Kalimdor", 2.5f, 5.0f, kCrowd,
     false, kBankX, kBankY, 0, 40},
    {"fight", "Group fight: 40 fighters with spells (Orgrimmar's bank)", "Kalimdor", 2.5f, 5.0f, kCrowd, false, kBankX,
     kBankY, 0, 0, 40},
    {"fight20", "Group fight: 20 fighters with spells (Orgrimmar's bank)", "Kalimdor", 2.5f, 5.0f, kCrowd, false,
     kBankX, kBankY, 0, 0, 20},
    {"soak", "Soak: Orgrimmar and a fight of 40, round and round (30 min)", "Kalimdor", 2.5f, 5.0f, kCrowd, false,
     kBankX, kBankY, 0, 0, 40, kSoakLegs, kSoakMinutes},
};

f64 segmentLength(const BenchmarkWaypoint& a, const BenchmarkWaypoint& b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}

f32 percentile(const std::vector<f32>& sorted, f32 p) {
    if (sorted.empty()) {
        return 0.0f;
    }
    const usize index = std::min(sorted.size() - 1, static_cast<usize>(p * static_cast<f32>(sorted.size() - 1) + 0.5f));
    return sorted[index];
}

} // namespace

std::span<const BenchmarkRoute> benchmarkRoutes() {
    return kRoutes;
}

const BenchmarkRoute* findBenchmarkRoute(std::string_view id) {
    for (const BenchmarkRoute& route : kRoutes) {
        if (id == route.id) {
            return &route;
        }
    }
    return nullptr;
}

void Benchmark::start(const BenchmarkRoute& route) {
    m_route = &route;
    m_leg = &route;
    m_legIndex = 0;
    m_duration = 0.0f;
    if (!route.legs.empty()) {
        m_duration = (m_minutesOverride > 0.0f ? m_minutesOverride : route.minutes) * 60.0f;
        if (const BenchmarkRoute* leg = findBenchmarkRoute(route.legs[0])) {
            m_leg = leg;
        }
    }
    m_elapsed = 0.0f;
    m_segment = 0;
    m_along = 0.0;
    m_travelled = 0.0;
    m_warmup = kWarmupSeconds;
    m_z = 0.0f;
    m_totalLength = 0.0;
    for (usize i = 0; i + 1 < m_leg->waypoints.size(); ++i) {
        m_totalLength += segmentLength(m_leg->waypoints[i], m_leg->waypoints[i + 1]);
    }
    m_samples.clear();
    // No growth mid-run, up to 200 fps (144 Hz desktop monitors included).
    const f64 seconds = m_duration > 0.0f ? static_cast<f64>(m_duration) : m_totalLength / m_leg->speed;
    m_samples.reserve(static_cast<usize>(seconds * 200.0) + 2048);
    m_minuteFrames.clear();
    m_minuteFrames.reserve(60 * 200);
    m_minuteSeconds = 0.0f;
    m_minuteGpu = 0.0;
    m_minuteCpu = 0.0;
    m_minuteIndex = 0;
    m_minuteReady = false;
    if (m_duration > 0.0f) {
        VD_LOG_INFO("Benchmark %s: %zu routes in a loop for %.0f min", route.id, route.legs.size(),
                    static_cast<f64>(m_duration) / 60.0);
    } else {
        VD_LOG_INFO("Benchmark %s: %.0f yards at %.0f yd/s", route.id, m_totalLength, route.speed);
    }
}

void Benchmark::stop() {
    m_route = nullptr;
    m_leg = nullptr;
}

f32 Benchmark::progress() const {
    if (m_duration > 0.0f) {
        return std::min(1.0f, m_elapsed / m_duration);
    }
    return m_totalLength > 0.0 ? static_cast<f32>(m_travelled / m_totalLength) : 0.0f;
}

bool Benchmark::takeMinute(BenchmarkMinute& out) {
    if (!m_minuteReady) {
        return false;
    }
    out = m_minute;
    m_minuteReady = false;
    return true;
}

bool Benchmark::nextLeg() {
    // A looping route goes on with its next leg until its time is up.
    if (m_duration <= 0.0f || m_elapsed >= m_duration) {
        return false;
    }
    m_legIndex = (m_legIndex + 1) % static_cast<u32>(m_route->legs.size());
    if (const BenchmarkRoute* leg = findBenchmarkRoute(m_route->legs[m_legIndex])) {
        m_leg = leg;
    }
    m_segment = 0;
    m_along = 0.0;
    m_z = 0.0f; // a new start: the height snaps to its ground
    return true;
}

bool Benchmark::update(Camera& camera, f32 dt, const f32* groundHeight) {
    if (!m_route) {
        return false;
    }
    if (m_warmup > 0.0f) {
        m_warmup -= dt;
    } else {
        m_elapsed += dt;
        const f64 step = static_cast<f64>(m_leg->speed) * dt;
        m_along += step;
        m_travelled += step;
        const std::span<const BenchmarkWaypoint> legPoints = m_leg->waypoints;
        while (m_segment + 1 < legPoints.size() &&
               m_along >= segmentLength(legPoints[m_segment], legPoints[m_segment + 1])) {
            m_along -= segmentLength(legPoints[m_segment], legPoints[m_segment + 1]);
            ++m_segment;
        }
        const bool timeUp = m_duration > 0.0f && m_elapsed >= m_duration;
        if (timeUp || (m_segment + 1 >= legPoints.size() && !nextLeg())) {
            finish();
            return false;
        }
    }
    const std::span<const BenchmarkWaypoint> points = m_leg->waypoints;
    const BenchmarkWaypoint& a = points[m_segment];
    const BenchmarkWaypoint& b = points[std::min<usize>(m_segment + 1, points.size() - 1)];
    const f64 length = std::max(1.0, segmentLength(a, b));
    const f64 t = std::min(1.0, m_along / length);
    camera.position.x = a.x + (b.x - a.x) * t;
    camera.position.y = a.y + (b.y - a.y) * t;
    // Follow the ground (and the floors) smoothly, without jitter over bumps; keep the altitude while
    // the tile streams in.
    f32 floor = BenchmarkWaypoint::kNoFloor;
    if (a.floor != BenchmarkWaypoint::kNoFloor && b.floor != BenchmarkWaypoint::kNoFloor) {
        floor = a.floor + (b.floor - a.floor) * static_cast<f32>(t);
    } else {
        floor = std::max(a.floor, b.floor);
    }
    const bool haveGround = groundHeight && !m_leg->ignoreGround;
    if (haveGround || floor != BenchmarkWaypoint::kNoFloor) {
        const f32 target = std::max(haveGround ? *groundHeight : BenchmarkWaypoint::kNoFloor, floor) + m_leg->height;
        m_z = m_z == 0.0f ? target : m_z + (target - m_z) * std::min(1.0f, dt * 2.0f);
    }
    camera.position.z = m_z != 0.0f ? m_z : camera.position.z;
    if (m_leg->lookX != 0.0 || m_leg->lookY != 0.0) {
        camera.yaw = static_cast<f32>(std::atan2(m_leg->lookY - camera.position.y, m_leg->lookX - camera.position.x));
        camera.pitch = -0.12f;
    } else {
        camera.yaw = static_cast<f32>(std::atan2(b.y - a.y, b.x - a.x));
        camera.pitch = -0.15f;
    }
    return true;
}

void Benchmark::record(const BenchmarkSample& sample) {
    if (!m_route || m_warmup > 0.0f) {
        return;
    }
    if (m_samples.size() < m_samples.capacity()) {
        m_samples.push_back(sample);
    }
    if (m_duration > 0.0f) {
        if (m_minuteFrames.size() < m_minuteFrames.capacity()) {
            m_minuteFrames.push_back(sample.frameMs);
        }
        m_minuteSeconds += sample.frameMs / 1000.0f;
        m_minuteGpu += sample.gpuMs;
        m_minuteCpu += sample.cpuMs;
        if (m_minuteSeconds >= 60.0f) {
            closeMinute();
        }
    }
}

void Benchmark::closeMinute() {
    BenchmarkMinute& m = m_minute;
    m = {};
    m.index = ++m_minuteIndex;
    m.frames = static_cast<u32>(m_minuteFrames.size());
    if (m.frames > 0) {
        m.fps = static_cast<f32>(m.frames) / std::max(0.001f, m_minuteSeconds);
        for (const f32 frame : m_minuteFrames) {
            m.over20 += frame > 20.0f ? 1u : 0u;
            m.over33 += frame > 33.0f ? 1u : 0u;
            m.maxMs = std::max(m.maxMs, frame);
        }
        std::sort(m_minuteFrames.begin(), m_minuteFrames.end()); // ~3600 values, once a minute
        m.p50Ms = percentile(m_minuteFrames, 0.5f);
        m.p99Ms = percentile(m_minuteFrames, 0.99f);
        m.gpuMs = static_cast<f32>(m_minuteGpu / m.frames);
        m.cpuMs = static_cast<f32>(m_minuteCpu / m.frames);
    }
    m_minuteReady = true;
    m_minuteFrames.clear();
    m_minuteSeconds = 0.0f;
    m_minuteGpu = 0.0;
    m_minuteCpu = 0.0;
}

void Benchmark::finish() {
    const BenchmarkRoute* route = m_route;
    if (m_duration > 0.0f && m_minuteSeconds >= 10.0f) {
        closeMinute(); // the last, shorter one
    }
    m_route = nullptr;
    m_leg = nullptr;
    if (m_samples.empty()) {
        m_report = std::string(route->id) + ": no samples";
        return;
    }
    std::vector<f32> frames;
    frames.reserve(m_samples.size());
    std::vector<f32> cpu;
    cpu.reserve(m_samples.size());
    f64 totalMs = 0.0, gpuSum = 0.0, cpuSum = 0.0;
    f32 gpuMax = 0.0f;
    u32 over20 = 0, over33 = 0, drawsMax = 0, trisMax = 0;
    for (const BenchmarkSample& s : m_samples) {
        frames.push_back(s.frameMs);
        cpu.push_back(s.cpuMs);
        totalMs += s.frameMs;
        gpuSum += s.gpuMs;
        cpuSum += s.cpuMs;
        gpuMax = std::max(gpuMax, s.gpuMs);
        over20 += s.frameMs > 20.0f ? 1u : 0u;
        over33 += s.frameMs > 33.0f ? 1u : 0u;
        drawsMax = std::max(drawsMax, s.drawCalls);
        trisMax = std::max(trisMax, s.triangles);
    }
    std::sort(frames.begin(), frames.end());
    std::sort(cpu.begin(), cpu.end());
    const f64 n = static_cast<f64>(m_samples.size());
    char text[512];
    std::snprintf(text, sizeof(text),
                  "%s: %zu frames in %.0f s | %.1f fps | frame p50 %.2f p99 %.2f max %.2f ms | >20 ms: %u, >33 ms: %u | "
                  "GPU avg %.2f max %.2f ms | CPU avg %.2f p99 %.2f ms | draws max %u | tris max %uk",
                  route->id, m_samples.size(), totalMs / 1000.0, n * 1000.0 / totalMs, percentile(frames, 0.5f),
                  percentile(frames, 0.99f), frames.back(), over20, over33, gpuSum / n, gpuMax, cpuSum / n,
                  percentile(cpu, 0.99f), drawsMax,
                  trisMax / 1000);
    m_report = text;
    VD_LOG_INFO("BENCH %s", m_report.c_str());
}

} // namespace vd
