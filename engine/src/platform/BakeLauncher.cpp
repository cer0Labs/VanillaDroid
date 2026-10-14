#include "platform/BakeLauncher.h"

#include "core/Log.h"

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace vd {

#if defined(__ANDROID__)
namespace {

// MainActivity.startBake() / cancelBake() (Java): they start the foreground service BakeService (process
// :baker) with the plan at <getExternalFilesDir(null)>/bake/plan.json, the same folder as the native
// externalDataPath's.
class AndroidBakeLauncher final : public BakeLauncher {
public:
    explicit AndroidBakeLauncher(const PlatformInfo& platform)
        : m_vm(static_cast<JavaVM*>(platform.javaVM)), m_activity(static_cast<jobject>(platform.activity)) {}

    bool start(const std::filesystem::path& bakeDir) override {
        static_cast<void>(bakeDir);
        return call("startBake", "()Z");
    }
    void cancel(const std::filesystem::path& bakeDir) override {
        static_cast<void>(bakeDir);
        call("cancelBake", "()Z");
    }
    void threadEnds() override {
        if (m_env) {
            m_vm->DetachCurrentThread();
            m_env = nullptr;
        }
    }

private:
    bool call(const char* method, const char* signature) {
        if (!m_vm || !m_activity) {
            return false;
        }
        if (!m_env && m_vm->AttachCurrentThread(&m_env, nullptr) != JNI_OK) {
            m_env = nullptr;
            VD_LOG_ERROR("Phone bake: this thread cannot reach Java");
            return false;
        }
        // The activity's own class (MainActivity): its class loader, not the system's this thread would get.
        jclass activityClass = m_env->GetObjectClass(m_activity);
        jmethodID id = activityClass ? m_env->GetMethodID(activityClass, method, signature) : nullptr;
        bool ok = false;
        if (id) {
            ok = m_env->CallBooleanMethod(m_activity, id) == JNI_TRUE;
        }
        if (m_env->ExceptionCheck()) {
            m_env->ExceptionDescribe();
            m_env->ExceptionClear();
            ok = false;
        }
        if (activityClass) {
            m_env->DeleteLocalRef(activityClass);
        }
        if (!ok) {
            VD_LOG_WARN("Phone bake: MainActivity.%s failed", method);
        }
        return ok;
    }

    JavaVM* m_vm = nullptr;
    jobject m_activity = nullptr;
    JNIEnv* m_env = nullptr;
};

} // namespace

std::unique_ptr<BakeLauncher> makeAndroidBakeLauncher(const PlatformInfo& platform) {
    return std::make_unique<AndroidBakeLauncher>(platform);
}

#else

std::unique_ptr<BakeLauncher> makeAndroidBakeLauncher(const PlatformInfo& platform) {
    static_cast<void>(platform);
    return nullptr;
}

#endif

} // namespace vd
