#pragma once

#include "core/Types.h"
#include "platform/PlatformInfo.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vd {

// What the device says about itself (phase 10): temperature, battery and memory. Every field may be unknown.
struct DeviceReading {
    // ADPF thermal API (Android 11+ status, 12+ headroom): the headroom is the forecast 10 s ahead, 1.0 = severe
    // throttling; the status is ATHERMAL_STATUS_* (0 none, 1 light, 2 moderate, 3 severe, 4 critical, 5 emergency,
    // 6 shutdown). `thresholds`: the headroom at which each status begins on this device (Android 15+), NaN if unknown.
    f32 thermalHeadroom = std::numeric_limits<f32>::quiet_NaN();
    i32 thermalStatus = -1;
    f32 thresholds[7] = {std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::quiet_NaN(),
                         std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::quiet_NaN(),
                         std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::quiet_NaN(),
                         std::numeric_limits<f32>::quiet_NaN()};
    // Battery (Android's BatteryManager and its sticky ACTION_BATTERY_CHANGED).
    i32 batteryPercent = -1;
    i64 chargeMicroAh = -1;     // charge counter: what is left in the battery
    i32 currentMicroA = 0;      // > 0 charging, < 0 draining; 0 unknown
    i32 voltageMv = -1;
    f32 batteryCelsius = std::numeric_limits<f32>::quiet_NaN();
    bool pluggedIn = false;     // on a charger: the battery figures say nothing about the game's draw
    // Memory: this process's resident set and the system's available memory (/proc).
    u64 residentBytes = 0;
    u64 availableBytes = 0;
    u64 totalBytes = 0;
    u32 sequence = 0; // grows with every reading
};

// A thread ("vd-monitor") that asks the system once a second (the thermal API is rate-limited and may block on a
// binder call; the battery goes through JNI; /proc is file IO): the game thread only copies the last reading,
// without ever waiting. Development: VD_FAKE_THERMAL=<headroom> holds a headroom, VD_FAKE_THERMAL=ramp:<seconds>
// climbs from 0.5 to 1.2 over that time (to watch the dynamic quality react on the desktop).
class DeviceMonitor {
public:
    DeviceMonitor() = default;
    ~DeviceMonitor();
    DeviceMonitor(const DeviceMonitor&) = delete;
    DeviceMonitor& operator=(const DeviceMonitor&) = delete;

    void start(const PlatformInfo& platform);
    void stop();
    // The newest reading, if the monitor thread is not writing one right now (then `out` keeps the previous one).
    // Never blocks.
    bool latest(DeviceReading& out);
    // The thermal API answers on this device (a session with ADPF).
    bool thermalAvailable() const { return m_thermalApi.load(std::memory_order_relaxed); }
    // Writes `text` to `path` on the monitor thread (through a temporary file, so a crash never leaves half a file):
    // the game thread never touches the disk while playing. A newer write to the same path replaces a pending one;
    // phase 15 (options): writes to other paths (the settings, the macros) wait their turn, and the file's directory
    // is made when missing.
    void writeFile(const std::filesystem::path& path, std::string text);
    // This process's resident memory read now (file IO on Android: for lifecycle events, not for frames).
    static u64 residentBytesNow();

private:
    void run();
    void sample(DeviceReading& reading, u32 tick);
    void flushWrite();

    PlatformInfo m_platform;
    std::thread m_thread;
    std::mutex m_mutex; // guards m_reading and m_stop
    std::condition_variable m_wake;
    DeviceReading m_reading;
    bool m_stop = false;
    std::vector<std::pair<std::filesystem::path, std::string>> m_writes; // pending writeFile()s, guarded by m_mutex
    bool m_writePending = false;
    std::atomic<bool> m_thermalApi{false};
    // Development: a fake thermal headroom.
    f32 m_fakeHeadroom = std::numeric_limits<f32>::quiet_NaN();
    f32 m_fakeRampSeconds = 0.0f;
    // Android: the thermal manager and JNI handles, owned by the monitor thread.
    void* m_thermal = nullptr; // AThermalManager*
    void* m_env = nullptr;     // JNIEnv* of the monitor thread
    void* m_batteryManager = nullptr; // global ref
    void* m_batteryFilter = nullptr;  // global ref: IntentFilter(ACTION_BATTERY_CHANGED)
    bool m_jniFailed = false;
};

} // namespace vd
