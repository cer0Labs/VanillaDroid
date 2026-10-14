#include "platform/UpdatePlatform.h"

#include "core/Log.h"
#include "platform/PlatformInfo.h"

#include <atomic>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#if defined(__ANDROID__)
#include <jni.h>
#elif defined(_WIN32)
#include <windows.h>

#include <shellapi.h> // after windows.h
#include <winhttp.h>
#endif

namespace vd {

#if defined(__ANDROID__)

// --- Android: dev.cer.vanilladroid.Updates
// ---------------------------------------------------------------------

namespace {

constexpr const char* kUpdatesClass = "dev.cer.vanilladroid.Updates";
// DownloadManager.STATUS_* and PackageInstaller.STATUS_* (Updates.installStatus() adds kInstallWaiting).
constexpr jlong kDownloadSuccessful = 8;
constexpr jlong kDownloadFailed = 16;
constexpr jint kInstallWaiting = -100;
constexpr jint kInstallPendingUserAction = -1;
constexpr jint kInstallSuccess = 0;
constexpr jint kInstallAborted = 3;

bool cleared(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe(); // the Java stack in logcat
        env->ExceptionClear();
        return true;
    }
    return false;
}

std::string toUtf8(JNIEnv* env, jstring text) {
    if (!text) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(text, nullptr);
    std::string out = chars ? chars : "";
    if (chars) {
        env->ReleaseStringUTFChars(text, chars);
    }
    return out;
}

} // namespace

struct UpdatePlatform::Impl {
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    jobject activity = nullptr; // PlatformInfo's global reference: not ours to delete
    jclass updates = nullptr;
    jmethodID online = nullptr, fetch = nullptr, download = nullptr, downloadPath = nullptr,
              downloadState = nullptr, cancelDownload = nullptr, canInstall = nullptr,
              openInstallSettings = nullptr, install = nullptr, installStatus = nullptr,
              installMessage = nullptr, openUrl = nullptr;
    jlong downloadId = -1;

    jstring string(std::string_view text) const { return env->NewStringUTF(std::string(text).c_str()); }
};

UpdatePlatform::UpdatePlatform() : m_impl(std::make_unique<Impl>()) {}
UpdatePlatform::~UpdatePlatform() = default;

bool UpdatePlatform::open(const PlatformInfo& platform) {
    Impl& j = *m_impl;
    j.vm = static_cast<JavaVM*>(platform.javaVM);
    j.activity = static_cast<jobject>(platform.activity);
    if (!j.vm || !j.activity || j.vm->AttachCurrentThread(&j.env, nullptr) != JNI_OK || !j.env) {
        return false;
    }
    JNIEnv* env = j.env;
    // A thread the JVM did not start sees the system's class loader: the app's class comes from the
    // activity's.
    jclass activityClass = env->GetObjectClass(j.activity);
    jmethodID getClassLoader = env->GetMethodID(activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = getClassLoader ? env->CallObjectMethod(j.activity, getClassLoader) : nullptr;
    jclass loaderClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass =
        loaderClass ? env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;")
                    : nullptr;
    jstring name = env->NewStringUTF(kUpdatesClass);
    auto local =
        static_cast<jclass>(loader && loadClass ? env->CallObjectMethod(loader, loadClass, name) : nullptr);
    if (cleared(env) || !local) {
        VD_LOG_WARN("Updates: no %s class", kUpdatesClass);
        return false;
    }
    j.updates = static_cast<jclass>(env->NewGlobalRef(local));
    for (jobject ref : {static_cast<jobject>(local), static_cast<jobject>(name), loader,
                        static_cast<jobject>(loaderClass), static_cast<jobject>(activityClass)}) {
        if (ref) {
            env->DeleteLocalRef(ref);
        }
    }
    const auto method = [&](const char* method, const char* signature) {
        jmethodID id = env->GetStaticMethodID(j.updates, method, signature);
        cleared(env);
        return id;
    };
    j.online = method("online", "(Landroid/app/Activity;)Z");
    j.fetch = method("fetch", "(Ljava/lang/String;II)[B");
    j.download =
        method("download", "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J");
    j.downloadPath = method("downloadPath", "(Landroid/app/Activity;Ljava/lang/String;)Ljava/lang/String;");
    j.downloadState = method("downloadState", "(Landroid/app/Activity;J)[J");
    j.cancelDownload = method("cancelDownload", "(Landroid/app/Activity;J)V");
    j.canInstall = method("canInstall", "(Landroid/app/Activity;)Z");
    j.openInstallSettings = method("openInstallSettings", "(Landroid/app/Activity;)V");
    j.install = method("install", "(Landroid/app/Activity;Ljava/lang/String;JLjava/lang/String;J)I");
    j.installStatus = method("installStatus", "()I");
    j.installMessage = method("installMessage", "()Ljava/lang/String;");
    j.openUrl = method("openUrl", "(Landroid/app/Activity;Ljava/lang/String;)V");
    const bool complete = j.online && j.fetch && j.download && j.downloadPath && j.downloadState &&
                          j.cancelDownload && j.canInstall && j.openInstallSettings && j.install &&
                          j.installStatus && j.installMessage && j.openUrl;
    if (!complete) {
        VD_LOG_WARN("Updates: %s lacks methods", kUpdatesClass);
    }
    return complete;
}

void UpdatePlatform::close() {
    Impl& j = *m_impl;
    if (j.env && j.updates) {
        j.env->DeleteGlobalRef(j.updates);
        j.updates = nullptr;
    }
    if (j.vm && j.env) {
        j.vm->DetachCurrentThread();
    }
    j.env = nullptr;
}

bool UpdatePlatform::online() {
    Impl& j = *m_impl;
    const bool up = j.env->CallStaticBooleanMethod(j.updates, j.online, j.activity);
    return !cleared(j.env) && up;
}

Result<std::vector<u8>> UpdatePlatform::fetch(std::string_view url, usize maxBytes, u32 timeoutMs) {
    Impl& j = *m_impl;
    jstring jurl = j.string(url);
    auto bytes = static_cast<jbyteArray>(j.env->CallStaticObjectMethod(
        j.updates, j.fetch, jurl, static_cast<jint>(maxBytes), static_cast<jint>(timeoutMs)));
    j.env->DeleteLocalRef(jurl);
    if (cleared(j.env) || !bytes) {
        return Error{"cannot fetch " + std::string(url)};
    }
    std::vector<u8> out(static_cast<usize>(j.env->GetArrayLength(bytes)));
    j.env->GetByteArrayRegion(bytes, 0, static_cast<jsize>(out.size()), reinterpret_cast<jbyte*>(out.data()));
    j.env->DeleteLocalRef(bytes);
    return out;
}

bool UpdatePlatform::startDownload(std::string_view url, std::string_view fileName, std::string_view title) {
    Impl& j = *m_impl;
    jstring jurl = j.string(url);
    jstring jname = j.string(fileName);
    jstring jtitle = j.string(title);
    j.downloadId = j.env->CallStaticLongMethod(j.updates, j.download, j.activity, jurl, jname, jtitle);
    auto path =
        static_cast<jstring>(j.env->CallStaticObjectMethod(j.updates, j.downloadPath, j.activity, jname));
    m_downloadPath = toUtf8(j.env, path);
    for (jobject ref : {static_cast<jobject>(jurl), static_cast<jobject>(jname), static_cast<jobject>(jtitle),
                        static_cast<jobject>(path)}) {
        if (ref) {
            j.env->DeleteLocalRef(ref);
        }
    }
    if (cleared(j.env)) {
        j.downloadId = -1;
    }
    return j.downloadId >= 0 && !m_downloadPath.empty();
}

UpdatePlatform::DownloadState UpdatePlatform::pollDownload() {
    Impl& j = *m_impl;
    DownloadState state;
    auto values = static_cast<jlongArray>(
        j.env->CallStaticObjectMethod(j.updates, j.downloadState, j.activity, j.downloadId));
    if (cleared(j.env) || !values || j.env->GetArrayLength(values) < 3) {
        state.status = DownloadState::Status::Failed;
        return state;
    }
    jlong v[3] = {};
    j.env->GetLongArrayRegion(values, 0, 3, v);
    j.env->DeleteLocalRef(values);
    state.status = v[0] == kDownloadSuccessful           ? DownloadState::Status::Done
                   : v[0] == kDownloadFailed || v[0] < 0 ? DownloadState::Status::Failed
                                                         : DownloadState::Status::Running;
    state.bytes = v[1] > 0 ? static_cast<u64>(v[1]) : 0;
    state.total = v[2] > 0 ? static_cast<u64>(v[2]) : 0;
    return state;
}

void UpdatePlatform::cancelDownload() {
    Impl& j = *m_impl;
    if (j.downloadId >= 0) {
        j.env->CallStaticVoidMethod(j.updates, j.cancelDownload, j.activity, j.downloadId);
        cleared(j.env);
        j.downloadId = -1;
    }
}

bool UpdatePlatform::canInstall() {
    Impl& j = *m_impl;
    const bool can = j.env->CallStaticBooleanMethod(j.updates, j.canInstall, j.activity);
    return !cleared(j.env) && can;
}

void UpdatePlatform::openInstallSettings() {
    Impl& j = *m_impl;
    j.env->CallStaticVoidMethod(j.updates, j.openInstallSettings, j.activity);
    cleared(j.env);
}

UpdatePlatform::InstallStart UpdatePlatform::install(std::string_view file, u64 size, std::string_view sha256,
                                                     u32 versionCode) {
    Impl& j = *m_impl;
    jstring jfile = j.string(file);
    jstring jsha = j.string(sha256);
    const jint result =
        j.env->CallStaticIntMethod(j.updates, j.install, j.activity, jfile, static_cast<jlong>(size), jsha,
                                   static_cast<jlong>(versionCode));
    j.env->DeleteLocalRef(jfile);
    j.env->DeleteLocalRef(jsha);
    if (cleared(j.env)) {
        return InstallStart::Error;
    }
    switch (result) { // Updates.INSTALL_*
    case 0: return InstallStart::Started;
    case 1: return InstallStart::Mismatch;
    case 2: return InstallStart::NotThisApp;
    default: return InstallStart::Error;
    }
}

UpdatePlatform::InstallStatus UpdatePlatform::installStatus(std::string& message) {
    Impl& j = *m_impl;
    const jint status = j.env->CallStaticIntMethod(j.updates, j.installStatus);
    if (cleared(j.env)) {
        return InstallStatus::Failed;
    }
    if (status == kInstallWaiting || status == kInstallPendingUserAction) {
        return InstallStatus::Waiting;
    }
    if (status == kInstallSuccess) {
        return InstallStatus::Success;
    }
    if (status == kInstallAborted) {
        return InstallStatus::Aborted;
    }
    auto text = static_cast<jstring>(j.env->CallStaticObjectMethod(j.updates, j.installMessage));
    message = cleared(j.env) ? std::string() : toUtf8(j.env, text);
    if (text) {
        j.env->DeleteLocalRef(text);
    }
    return InstallStatus::Failed;
}

void UpdatePlatform::openUrl(std::string_view url) {
    Impl& j = *m_impl;
    jstring jurl = j.string(url);
    j.env->CallStaticVoidMethod(j.updates, j.openUrl, j.activity, jurl);
    j.env->DeleteLocalRef(jurl);
    cleared(j.env);
}

#elif defined(_WIN32)

// --- Windows (development): WinHTTP and the shell
// -------------------------------------------------------------

namespace {

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<usize>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

// A GET with WinHTTP (the system's TLS; redirects followed). `sink` gets the body in pieces with the total
// size (0: unknown) and returns false to stop.
template <typename Sink>
Result<void> httpGet(std::string_view url, u32 timeoutMs, Sink&& sink, const std::atomic<bool>* stop) {
    std::wstring wide = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.dwExtraInfoLength = 1; // the query is appended to the path
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) {
        return Error{"bad URL " + std::string(url)};
    }
    std::wstring target(path, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0) {
        target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET session = WinHttpOpen(L"VanillaDroid", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return Error{"WinHttpOpen failed"};
    }
    const int timeout = static_cast<int>(timeoutMs);
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request =
        connection ? WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                   : nullptr;
    Result<void> result = Error{"cannot reach " + std::string(url)};
    if (request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        wchar_t lengthText[32] = {};
        size = sizeof(lengthText);
        u64 total = 0;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                lengthText, &size, WINHTTP_NO_HEADER_INDEX)) {
            total = std::wcstoull(lengthText, nullptr, 10);
        }
        if (status != 200) {
            result = Error{"HTTP " + std::to_string(status) + " for " + std::string(url)};
        } else {
            result = Result<void>{};
            std::vector<u8> block(1u << 16);
            for (;;) {
                if (stop && stop->load()) {
                    result = Error{"cancelled"};
                    break;
                }
                DWORD got = 0;
                if (!WinHttpReadData(request, block.data(), static_cast<DWORD>(block.size()), &got)) {
                    result = Error{"cut while reading " + std::string(url)};
                    break;
                }
                if (got == 0) {
                    break;
                }
                if (!sink(block.data(), static_cast<usize>(got), total)) {
                    result = Error{"too big or not written: " + std::string(url)};
                    break;
                }
            }
        }
    }
    if (request) {
        WinHttpCloseHandle(request);
    }
    if (connection) {
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return result;
}

} // namespace

struct UpdatePlatform::Impl {
    std::filesystem::path folder; // <cacheDir>/updates
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<u64> bytes{0};
    std::atomic<u64> total{0};
    std::atomic<u32> status{0}; // DownloadState::Status

    void join() {
        stop = true;
        if (worker.joinable()) {
            worker.join();
        }
        stop = false;
    }
};

UpdatePlatform::UpdatePlatform() : m_impl(std::make_unique<Impl>()) {}
UpdatePlatform::~UpdatePlatform() {
    m_impl->join();
}

bool UpdatePlatform::open(const PlatformInfo& platform) {
    m_impl->folder = std::filesystem::path(platform.cacheDir.empty() ? "." : platform.cacheDir) / "updates";
    return true;
}

void UpdatePlatform::close() {
    m_impl->join();
}

bool UpdatePlatform::online() {
    return true;
}

Result<std::vector<u8>> UpdatePlatform::fetch(std::string_view url, usize maxBytes, u32 timeoutMs) {
    std::vector<u8> out;
    Result<void> got = httpGet(
        url, timeoutMs,
        [&](const u8* data, usize size, u64) {
            if (out.size() + size > maxBytes) {
                return false;
            }
            out.insert(out.end(), data, data + size);
            return true;
        },
        nullptr);
    if (!got) {
        return got.error();
    }
    return out;
}

bool UpdatePlatform::startDownload(std::string_view url, std::string_view fileName, std::string_view) {
    Impl& w = *m_impl;
    w.join();
    std::error_code error;
    std::filesystem::create_directories(w.folder, error);
    const std::filesystem::path file =
        w.folder / std::filesystem::path(std::u8string(fileName.begin(), fileName.end()));
    const std::u8string text = file.u8string();
    m_downloadPath.assign(text.begin(), text.end());
    w.bytes = 0;
    w.total = 0;
    w.status = static_cast<u32>(DownloadState::Status::Running);
    w.worker = std::thread([&w, file, url = std::string(url)] {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        Result<void> got = httpGet(
            url, 30000,
            [&](const u8* data, usize size, u64 total) {
                out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
                w.total = total;
                w.bytes += size;
                return static_cast<bool>(out);
            },
            &w.stop);
        out.close();
        w.status = static_cast<u32>(got ? DownloadState::Status::Done : DownloadState::Status::Failed);
        if (!got) {
            VD_LOG_WARN("Updates: %s", got.error().message.c_str());
        }
    });
    return true;
}

UpdatePlatform::DownloadState UpdatePlatform::pollDownload() {
    DownloadState state;
    state.status = static_cast<DownloadState::Status>(m_impl->status.load());
    state.bytes = m_impl->bytes.load();
    state.total = m_impl->total.load();
    return state;
}

void UpdatePlatform::cancelDownload() {
    m_impl->join();
}

bool UpdatePlatform::canInstall() {
    return true;
}

void UpdatePlatform::openInstallSettings() {}

UpdatePlatform::InstallStart UpdatePlatform::install(std::string_view, u64, std::string_view, u32) {
    return InstallStart::Unsupported; // an APK is installed on the phone
}

UpdatePlatform::InstallStatus UpdatePlatform::installStatus(std::string&) {
    return InstallStatus::Failed;
}

void UpdatePlatform::openUrl(std::string_view url) {
    const std::wstring wide = widen(url);
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

#else

// --- Elsewhere: no updates
// --------------------------------------------------------------------------------------

struct UpdatePlatform::Impl {};

UpdatePlatform::UpdatePlatform() : m_impl(std::make_unique<Impl>()) {}
UpdatePlatform::~UpdatePlatform() = default;

bool UpdatePlatform::open(const PlatformInfo&) {
    return false;
}
void UpdatePlatform::close() {}
bool UpdatePlatform::online() {
    return false;
}
Result<std::vector<u8>> UpdatePlatform::fetch(std::string_view, usize, u32) {
    return Error{"no updates on this system"};
}
bool UpdatePlatform::startDownload(std::string_view, std::string_view, std::string_view) {
    return false;
}
UpdatePlatform::DownloadState UpdatePlatform::pollDownload() {
    return {DownloadState::Status::Failed, 0, 0};
}
void UpdatePlatform::cancelDownload() {}
bool UpdatePlatform::canInstall() {
    return false;
}
void UpdatePlatform::openInstallSettings() {}
UpdatePlatform::InstallStart UpdatePlatform::install(std::string_view, u64, std::string_view, u32) {
    return InstallStart::Unsupported;
}
UpdatePlatform::InstallStatus UpdatePlatform::installStatus(std::string&) {
    return InstallStatus::Failed;
}
void UpdatePlatform::openUrl(std::string_view) {}

#endif

} // namespace vd
