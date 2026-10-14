#include "platform/DeviceMonitor.h"

#include "core/DevEnv.h"
#include "core/Log.h"
#include "platform/Thread.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <jni.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace vd {
namespace {

constexpr u32 kBatteryTicks = 5; // seconds between battery readings
constexpr u32 kMemoryTicks = 2;
constexpr i32 kForecastSeconds = 10;

#if defined(__ANDROID__)
// android/thermal.h, looked up at run time (minSdk 30; the headroom is API 31, its thresholds API 35).
struct HeadroomThreshold {
    float headroom;
    int32_t status;
};
struct ThermalApi {
    void* (*acquire)() = nullptr;
    void (*release)(void*) = nullptr;
    int (*status)(void*) = nullptr;
    float (*headroom)(void*, int) = nullptr;
    int (*thresholds)(void*, const HeadroomThreshold**, size_t*) = nullptr;
};

const ThermalApi& thermalApi() {
    static const ThermalApi api = [] {
        ThermalApi a;
        if (void* library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL)) {
            a.acquire = reinterpret_cast<decltype(a.acquire)>(dlsym(library, "AThermal_acquireManager"));
            a.release = reinterpret_cast<decltype(a.release)>(dlsym(library, "AThermal_releaseManager"));
            a.status = reinterpret_cast<decltype(a.status)>(dlsym(library, "AThermal_getCurrentThermalStatus"));
            a.headroom = reinterpret_cast<decltype(a.headroom)>(dlsym(library, "AThermal_getThermalHeadroom"));
            a.thresholds = reinterpret_cast<decltype(a.thresholds)>(
                dlsym(library, "AThermal_getThermalHeadroomThresholds"));
        }
        return a;
    }();
    return api;
}

// BatteryManager and the sticky ACTION_BATTERY_CHANGED intent, for the monitor thread only.
struct BatteryJni {
    jmethodID getIntProperty = nullptr;
    jmethodID registerReceiver = nullptr;
    jmethodID getIntExtra = nullptr;
};
BatteryJni g_battery;

constexpr jint kChargeCounter = 1; // BatteryManager.BATTERY_PROPERTY_*
constexpr jint kCurrentNow = 2;
constexpr jint kCapacity = 4;

bool jniFailed(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return true;
    }
    return false;
}

// "Key:   12345 kB" lines of /proc/meminfo.
u64 meminfoValue(const char* text, const char* key) {
    const char* at = std::strstr(text, key);
    return at ? std::strtoull(at + std::strlen(key), nullptr, 10) * 1024ull : 0;
}
#endif

} // namespace

DeviceMonitor::~DeviceMonitor() {
    stop();
}

void DeviceMonitor::start(const PlatformInfo& platform) {
    stop();
    m_platform = platform;
    if (const char* fake = VD_DEV_ENV("VD_FAKE_THERMAL")) {
        if (std::strncmp(fake, "ramp:", 5) == 0) {
            m_fakeRampSeconds = std::max(1.0f, static_cast<f32>(std::atof(fake + 5)));
        } else {
            m_fakeHeadroom = static_cast<f32>(std::atof(fake));
        }
        VD_LOG_WARN("VD_FAKE_THERMAL=%s: the thermal headroom is simulated", fake);
    }
    m_stop = false;
    m_thread = std::thread([this] { run(); });
}

void DeviceMonitor::stop() {
    if (!m_thread.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    m_thread.join();
    flushWrite(); // asked for right before stopping
}

void DeviceMonitor::writeFile(const std::filesystem::path& path, std::string text) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto same = std::find_if(m_writes.begin(), m_writes.end(), [&](const auto& w) { return w.first == path; });
        if (same != m_writes.end()) {
            same->second = std::move(text);
        } else {
            m_writes.emplace_back(path, std::move(text));
        }
        m_writePending = true;
    }
    if (!m_thread.joinable()) {
        flushWrite(); // not running (shutdown): write it here
        return;
    }
    m_wake.notify_all();
}

void DeviceMonitor::flushWrite() {
    std::vector<std::pair<std::filesystem::path, std::string>> writes;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_writePending) {
            return;
        }
        writes.swap(m_writes);
        m_writePending = false;
    }
    for (const auto& [path, text] : writes) {
        std::error_code error;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), error); // phase 15: the macros' WTF folders
        }
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        bool written = false;
        if (FILE* file = std::fopen(temporary.string().c_str(), "wb")) {
            written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
            written = std::fclose(file) == 0 && written;
        }
        error.clear();
        if (written) {
            std::filesystem::rename(temporary, path, error);
        }
        if (!written || error) {
            VD_LOG_WARN("Could not write %s", path.string().c_str());
        }
    }
}

u64 DeviceMonitor::residentBytesNow() {
#if defined(__ANDROID__)
    u64 bytes = 0;
    if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
        unsigned long long size = 0, resident = 0;
        if (std::fscanf(statm, "%llu %llu", &size, &resident) == 2) {
            bytes = static_cast<u64>(resident) * static_cast<u64>(sysconf(_SC_PAGESIZE));
        }
        std::fclose(statm);
    }
    return bytes;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.WorkingSetSize : 0;
#else
    return 0;
#endif
}

bool DeviceMonitor::latest(DeviceReading& out) {
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    out = m_reading;
    return true;
}

void DeviceMonitor::run() {
    setCurrentThreadName("vd-monitor");
#if defined(__ANDROID__)
    const ThermalApi& thermal = thermalApi();
    if (thermal.acquire) {
        m_thermal = thermal.acquire();
    }
    m_thermalApi.store(m_thermal != nullptr && thermal.headroom != nullptr, std::memory_order_relaxed);
    auto* vm = static_cast<JavaVM*>(m_platform.javaVM);
    JNIEnv* env = nullptr;
    if (vm && vm->AttachCurrentThread(&env, nullptr) == JNI_OK && env && m_platform.activity) {
        m_env = env;
        auto activity = static_cast<jobject>(m_platform.activity);
        jclass contextClass = env->FindClass("android/content/Context");
        jclass managerClass = env->FindClass("android/os/BatteryManager");
        jclass filterClass = env->FindClass("android/content/IntentFilter");
        jclass intentClass = env->FindClass("android/content/Intent");
        if (!jniFailed(env) && contextClass && managerClass && filterClass && intentClass) {
            jmethodID getService =
                env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
            g_battery.registerReceiver = env->GetMethodID(
                contextClass, "registerReceiver",
                "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;");
            g_battery.getIntProperty = env->GetMethodID(managerClass, "getIntProperty", "(I)I");
            g_battery.getIntExtra = env->GetMethodID(intentClass, "getIntExtra", "(Ljava/lang/String;I)I");
            jmethodID filterInit = env->GetMethodID(filterClass, "<init>", "(Ljava/lang/String;)V");
            if (!jniFailed(env) && getService && g_battery.registerReceiver && g_battery.getIntProperty &&
                g_battery.getIntExtra && filterInit) {
                jstring service = env->NewStringUTF("batterymanager");
                jobject manager = env->CallObjectMethod(activity, getService, service);
                jstring action = env->NewStringUTF("android.intent.action.BATTERY_CHANGED");
                jobject filter = env->NewObject(filterClass, filterInit, action);
                if (!jniFailed(env) && manager && filter) {
                    m_batteryManager = env->NewGlobalRef(manager);
                    m_batteryFilter = env->NewGlobalRef(filter);
                }
                env->DeleteLocalRef(service);
                env->DeleteLocalRef(action);
                if (manager) env->DeleteLocalRef(manager);
                if (filter) env->DeleteLocalRef(filter);
            }
        }
        jniFailed(env);
        for (jclass c : {contextClass, managerClass, filterClass, intentClass}) {
            if (c) env->DeleteLocalRef(c);
        }
        m_jniFailed = m_batteryManager == nullptr;
    }
    {
        // The device's own thresholds, once (Android 15+).
        DeviceReading reading;
        if (m_thermal && thermal.thresholds) {
            const HeadroomThreshold* thresholds = nullptr;
            size_t count = 0;
            if (thermal.thresholds(m_thermal, &thresholds, &count) == 0 && thresholds) {
                for (size_t i = 0; i < count; ++i) {
                    if (thresholds[i].status >= 0 && thresholds[i].status < 7) {
                        reading.thresholds[thresholds[i].status] = thresholds[i].headroom;
                    }
                }
                delete[] thresholds;
            }
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        std::copy(std::begin(reading.thresholds), std::end(reading.thresholds), std::begin(m_reading.thresholds));
    }
    VD_LOG_INFO("Device monitor: thermal %s (thresholds light %.2f, moderate %.2f, severe %.2f), battery %s",
                m_thermal ? (thermal.headroom ? "status + headroom" : "status") : "unavailable",
                static_cast<f64>(m_reading.thresholds[1]), static_cast<f64>(m_reading.thresholds[2]),
                static_cast<f64>(m_reading.thresholds[3]), m_batteryManager ? "BatteryManager" : "unavailable");
#else
    m_thermalApi.store(false, std::memory_order_relaxed);
#endif
    const auto started = std::chrono::steady_clock::now();
    for (u32 tick = 0;; ++tick) {
        DeviceReading reading;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            reading = m_reading;
        }
        flushWrite();
        sample(reading, tick);
        if (!std::isnan(m_fakeHeadroom) || m_fakeRampSeconds > 0.0f) {
            const f32 seconds = std::chrono::duration<f32>(std::chrono::steady_clock::now() - started).count();
            reading.thermalHeadroom = m_fakeRampSeconds > 0.0f
                                          ? 0.5f + 0.7f * std::min(1.0f, seconds / m_fakeRampSeconds)
                                          : m_fakeHeadroom;
            // Status as a Galaxy's thresholds would give it.
            const f32 h = reading.thermalHeadroom;
            reading.thermalStatus = h >= 1.07f ? 4 : h >= 1.0f ? 3 : h >= 0.9f ? 2 : h >= 0.83f ? 1 : 0;
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        reading.sequence = m_reading.sequence + 1;
        m_reading = reading;
        // Once a second, or at once for a file to write (then the readings carry on where they were).
        const auto next = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!m_stop && std::chrono::steady_clock::now() < next) {
            m_wake.wait_until(lock, next, [this] { return m_stop || m_writePending; });
            if (m_writePending && !m_stop) {
                lock.unlock();
                flushWrite();
                lock.lock();
            }
        }
        if (m_stop) {
            break;
        }
    }
#if defined(__ANDROID__)
    if (m_thermal && thermal.release) {
        thermal.release(m_thermal);
    }
    m_thermal = nullptr;
    if (m_env) {
        auto* jni = static_cast<JNIEnv*>(m_env);
        if (m_batteryManager) jni->DeleteGlobalRef(static_cast<jobject>(m_batteryManager));
        if (m_batteryFilter) jni->DeleteGlobalRef(static_cast<jobject>(m_batteryFilter));
        m_batteryManager = nullptr;
        m_batteryFilter = nullptr;
        m_env = nullptr;
        vm->DetachCurrentThread();
    }
#endif
}

void DeviceMonitor::sample(DeviceReading& reading, u32 tick) {
#if defined(__ANDROID__)
    const ThermalApi& thermal = thermalApi();
    if (m_thermal) {
        if (thermal.status) {
            reading.thermalStatus = thermal.status(m_thermal);
        }
        if (thermal.headroom) {
            const float headroom = thermal.headroom(m_thermal, kForecastSeconds);
            if (!std::isnan(headroom)) { // NaN: asked too soon, or not supported; keep the last one
                reading.thermalHeadroom = headroom;
            }
        }
    }
    if (tick % kBatteryTicks == 0 && m_env && m_batteryManager && !m_jniFailed) {
        auto* env = static_cast<JNIEnv*>(m_env);
        auto manager = static_cast<jobject>(m_batteryManager);
        const jint capacity = env->CallIntMethod(manager, g_battery.getIntProperty, kCapacity);
        const jint charge = env->CallIntMethod(manager, g_battery.getIntProperty, kChargeCounter);
        const jint current = env->CallIntMethod(manager, g_battery.getIntProperty, kCurrentNow);
        if (!jniFailed(env)) {
            reading.batteryPercent = capacity;
            reading.chargeMicroAh = charge > 0 ? charge : -1;
            // Microamperes by the documentation, but some devices (Samsung) report milliamperes: no phone running
            // a game moves less than 20 mA.
            const i64 magnitude = current < 0 ? -static_cast<i64>(current) : current;
            reading.currentMicroA = current == INT32_MIN ? 0 : (magnitude < 20000 ? current * 1000 : current);
        }
        jobject intent = env->CallObjectMethod(static_cast<jobject>(m_platform.activity), g_battery.registerReceiver,
                                               nullptr, static_cast<jobject>(m_batteryFilter));
        if (!jniFailed(env) && intent) {
            struct Extra {
                const char* name;
                jint value = -1;
            } extras[] = {{"voltage"}, {"temperature"}, {"plugged"}};
            for (Extra& extra : extras) {
                jstring name = env->NewStringUTF(extra.name);
                extra.value = env->CallIntMethod(intent, g_battery.getIntExtra, name, -1);
                env->DeleteLocalRef(name);
            }
            if (!jniFailed(env)) {
                reading.voltageMv = extras[0].value;
                reading.batteryCelsius =
                    extras[1].value >= 0 ? static_cast<f32>(extras[1].value) / 10.0f : reading.batteryCelsius;
                reading.pluggedIn = extras[2].value > 0;
            }
            env->DeleteLocalRef(intent);
        }
    }
    if (tick % kMemoryTicks == 0) {
        if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
            unsigned long long size = 0, resident = 0;
            if (std::fscanf(statm, "%llu %llu", &size, &resident) == 2) {
                reading.residentBytes = static_cast<u64>(resident) * static_cast<u64>(sysconf(_SC_PAGESIZE));
            }
            std::fclose(statm);
        }
        if (FILE* meminfo = std::fopen("/proc/meminfo", "r")) {
            char text[1024];
            const usize read = std::fread(text, 1, sizeof(text) - 1, meminfo);
            text[read] = '\0';
            std::fclose(meminfo);
            reading.totalBytes = meminfoValue(text, "MemTotal:");
            reading.availableBytes = meminfoValue(text, "MemAvailable:");
        }
    }
#elif defined(_WIN32)
    if (tick % kMemoryTicks == 0) {
        PROCESS_MEMORY_COUNTERS counters{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
            reading.residentBytes = counters.WorkingSetSize;
        }
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) {
            reading.availableBytes = status.ullAvailPhys;
            reading.totalBytes = status.ullTotalPhys;
        }
    }
#else
    static_cast<void>(reading);
    static_cast<void>(tick);
#endif
}

} // namespace vd
