#include "platform/Haptics.h"

#include "core/DevEnv.h"
#include "core/Log.h"
#include "platform/PlatformInfo.h"
#include "platform/Thread.h"

#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace vd::haptics {
namespace {

constexpr const char* kNames[] = {"tap", "double", "long"};

bool traced() {
    static const bool on = [] {
        const char* v = VD_DEV_ENV("VD_TRACE_HAPTICS");
        return v && *v && *v != '0';
    }();
    return on;
}

#if defined(__ANDROID__)
// VibrationEffect constants (android.os.VibrationEffect).
constexpr jint kEffectDoubleClick = 1;
constexpr jint kEffectTick = 2;
constexpr jint kDefaultAmplitude = -1;
constexpr jlong kLongMs = 160;

struct Vibrator {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    u32 pending = 0; // Pattern + 1, 0: nothing (under `mutex`)
    bool quit = false;
    void* javaVM = nullptr; // only these of PlatformInfo (never its password)
    void* activity = nullptr;
};
Vibrator* g_vibrator = nullptr;

bool jniFailed(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return true;
    }
    return false;
}

void run(Vibrator& v) {
    setCurrentThreadName("vd-haptics");
    auto* vm = static_cast<JavaVM*>(v.javaVM);
    JNIEnv* env = nullptr;
    if (!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env || !v.activity) {
        VD_LOG_WARN("Haptics: no JNI on its thread: no vibration");
        return;
    }
    // Context.getSystemService("vibrator"), its vibrate(VibrationEffect) and the three effects, made once.
    jobject vibrator = nullptr;
    jmethodID vibrate = nullptr;
    jobject effects[3] = {};
    jclass contextClass = env->FindClass("android/content/Context");
    jclass vibratorClass = env->FindClass("android/os/Vibrator");
    jclass effectClass = env->FindClass("android/os/VibrationEffect");
    if (!jniFailed(env) && contextClass && vibratorClass && effectClass) {
        jmethodID getService =
            env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        jmethodID hasVibrator = env->GetMethodID(vibratorClass, "hasVibrator", "()Z");
        vibrate = env->GetMethodID(vibratorClass, "vibrate", "(Landroid/os/VibrationEffect;)V");
        jmethodID predefined =
            env->GetStaticMethodID(effectClass, "createPredefined", "(I)Landroid/os/VibrationEffect;");
        jmethodID oneShot = env->GetStaticMethodID(effectClass, "createOneShot", "(JI)Landroid/os/VibrationEffect;");
        if (!jniFailed(env) && getService && hasVibrator && vibrate && predefined && oneShot) {
            jstring name = env->NewStringUTF("vibrator");
            jobject service = env->CallObjectMethod(static_cast<jobject>(v.activity), getService, name);
            env->DeleteLocalRef(name);
            if (!jniFailed(env) && service && env->CallBooleanMethod(service, hasVibrator) && !jniFailed(env)) {
                vibrator = env->NewGlobalRef(service);
                jobject made[3] = {env->CallStaticObjectMethod(effectClass, predefined, kEffectTick),
                                   env->CallStaticObjectMethod(effectClass, predefined, kEffectDoubleClick),
                                   env->CallStaticObjectMethod(effectClass, oneShot, kLongMs, kDefaultAmplitude)};
                jniFailed(env);
                for (u32 i = 0; i < 3; ++i) {
                    effects[i] = made[i] ? env->NewGlobalRef(made[i]) : nullptr;
                    if (made[i]) {
                        env->DeleteLocalRef(made[i]);
                    }
                }
            }
            if (service) {
                env->DeleteLocalRef(service);
            }
        }
    }
    jniFailed(env);
    for (jclass c : {contextClass, vibratorClass, effectClass}) {
        if (c) {
            env->DeleteLocalRef(c);
        }
    }
    VD_LOG_INFO("Haptics: %s", vibrator ? "the device's vibrator" : "no vibrator");
    for (;;) {
        u32 pattern = 0;
        {
            std::unique_lock<std::mutex> lock(v.mutex);
            v.wake.wait(lock, [&] { return v.quit || v.pending != 0; });
            if (v.quit) {
                break;
            }
            pattern = v.pending - 1;
            v.pending = 0;
        }
        if (vibrator && pattern < 3 && effects[pattern]) {
            env->CallVoidMethod(vibrator, vibrate, effects[pattern]);
            jniFailed(env);
        }
    }
    for (jobject e : effects) {
        if (e) {
            env->DeleteGlobalRef(e);
        }
    }
    if (vibrator) {
        env->DeleteGlobalRef(vibrator);
    }
    vm->DetachCurrentThread();
}
#endif

} // namespace

void start(const PlatformInfo& platform) {
#if defined(__ANDROID__)
    if (g_vibrator || !platform.javaVM || !platform.activity) {
        return;
    }
    g_vibrator = new Vibrator();
    g_vibrator->javaVM = platform.javaVM;
    g_vibrator->activity = platform.activity;
    g_vibrator->thread = std::thread([] { run(*g_vibrator); });
#else
    (void)platform;
#endif
}

void stop() {
#if defined(__ANDROID__)
    if (!g_vibrator) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_vibrator->mutex);
        g_vibrator->quit = true;
    }
    g_vibrator->wake.notify_one();
    g_vibrator->thread.join();
    delete g_vibrator;
    g_vibrator = nullptr;
#endif
}

void play(Pattern pattern) {
    if (traced()) {
        VD_LOG_INFO("Haptics: %s", kNames[static_cast<u32>(pattern) % 3]);
    }
#if defined(__ANDROID__)
    if (!g_vibrator) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_vibrator->mutex);
        g_vibrator->pending = static_cast<u32>(pattern) + 1;
    }
    g_vibrator->wake.notify_one();
#endif
}

} // namespace vd::haptics
