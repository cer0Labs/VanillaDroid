#include "AppPackInstall.h"

#include "core/Log.h"
#include "core/Version.h"
#include "pack/AppPack.h"

#include <android/asset_manager.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <cstdio>

namespace {

// PackageManager.getPackageInfo(getPackageName(), 0).lastUpdateTime: when this APK was installed (0 if
// unknown).
jlong apkInstallTime(JNIEnv* env, jobject activity) {
    if (!env || !activity) {
        return 0;
    }
    jlong time = 0;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getManager =
        env->GetMethodID(activityClass, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    jmethodID getName = env->GetMethodID(activityClass, "getPackageName", "()Ljava/lang/String;");
    jobject manager = getManager ? env->CallObjectMethod(activity, getManager) : nullptr;
    jobject name = getName ? env->CallObjectMethod(activity, getName) : nullptr;
    jclass managerClass = manager ? env->GetObjectClass(manager) : nullptr;
    jmethodID getInfo = managerClass
                            ? env->GetMethodID(managerClass, "getPackageInfo",
                                               "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;")
                            : nullptr;
    jobject info = getInfo && name ? env->CallObjectMethod(manager, getInfo, name, 0) : nullptr;
    jclass infoClass = nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    } else if (info) {
        infoClass = env->GetObjectClass(info);
        jfieldID field = env->GetFieldID(infoClass, "lastUpdateTime", "J");
        time = field && !env->ExceptionCheck() ? env->GetLongField(info, field) : 0;
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    for (jobject ref : {static_cast<jobject>(activityClass), manager, name,
                        static_cast<jobject>(managerClass), info, static_cast<jobject>(infoClass)}) {
        if (ref) {
            env->DeleteLocalRef(ref);
        }
    }
    return time;
}

bool copyAsset(void* user, std::FILE* out) {
    AAsset* asset = static_cast<AAsset*>(user);
    char buffer[64 * 1024];
    for (;;) {
        const int read = AAsset_read(asset, buffer, sizeof(buffer));
        if (read < 0) {
            return false;
        }
        if (read == 0) {
            return true;
        }
        if (std::fwrite(buffer, 1, static_cast<size_t>(read), out) != static_cast<size_t>(read)) {
            return false;
        }
    }
}

} // namespace

std::string installAppPack(android_app* app, JNIEnv* env) {
    if (!app->activity->internalDataPath || !app->activity->assetManager) {
        return {};
    }
    const std::string name(vd::pack::kAppPackFile);
    AAsset* asset = AAssetManager_open(app->activity->assetManager, name.c_str(), AASSET_MODE_STREAMING);
    if (!asset) {
        VD_LOG_ERROR("%s is not in the APK (scripts/android.ps1 bakes it: vd_baker bake --only app)",
                     name.c_str());
        return {};
    }
    const std::string stamp =
        std::to_string(vd::kVersionCode) + " " +
        std::to_string(static_cast<long long>(apkInstallTime(env, app->activity->javaGameActivity)));
    bool copied = false;
    vd::Result<std::filesystem::path> installed =
        vd::pack::installAppPack(app->activity->internalDataPath, stamp, &copyAsset, asset, &copied);
    const long long size = static_cast<long long>(AAsset_getLength64(asset));
    AAsset_close(asset);
    if (!installed) {
        VD_LOG_ERROR("%s: %s", name.c_str(), installed.error().message.c_str());
        return {};
    }
    if (copied) {
        VD_LOG_INFO("%s: copied from the APK (%lld bytes, %s)", name.c_str(), size, stamp.c_str());
    }
    return installed.value().string();
}
