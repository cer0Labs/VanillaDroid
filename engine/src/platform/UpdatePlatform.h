#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vd {

struct PlatformInfo;

// Phase 22 (updates, ADR-0005 §7): what the app's updates need from the system, which C++ cannot do on
// Android: HTTPS with the system's TLS, DownloadManager, PackageInstaller and the browser, through the Java
// class dev.cer.vanilladroid.Updates (apps/android) over JNI. On Windows (development only: VD_UPDATE_URL)
// WinHTTP downloads and the shell opens links; nothing installs there. Elsewhere nothing works. Every call
// blocks: only the updates thread (app/AppUpdates) calls them, after open() on that thread.
class UpdatePlatform {
public:
    UpdatePlatform();
    ~UpdatePlatform();
    UpdatePlatform(const UpdatePlatform&) = delete;
    UpdatePlatform& operator=(const UpdatePlatform&) = delete;

    // Attaches this thread to the JVM and finds the Java class (Android). False when updates cannot work
    // here.
    bool open(const PlatformInfo& platform);
    void close(); // detaches (on the same thread)

    // A network that reaches the internet now (desktop: always true).
    bool online();
    // A small file (the manifest, its signature) in memory; an error past `maxBytes` or after `timeoutMs`.
    Result<std::vector<u8>> fetch(std::string_view url, usize maxBytes, u32 timeoutMs);

    // A download to a file the platform chooses for `fileName` (Android: the app's external files, updates/).
    struct DownloadState {
        enum class Status : u8 { Running, Done, Failed };
        Status status = Status::Running;
        u64 bytes = 0;
        u64 total = 0; // 0 while unknown
    };
    // Starts it (`title`: the system's notification, Android). False when it cannot start.
    bool startDownload(std::string_view url, std::string_view fileName, std::string_view title);
    DownloadState pollDownload(); // where the one started is
    void cancelDownload();
    std::string downloadedFile() const { return m_downloadPath; } // its path (known once started)

    // Installing an APK (Android): may this app install packages (Settings > Install unknown apps)?
    bool canInstall();
    void openInstallSettings(); // that settings screen, for this app
    enum class InstallStart : u8 { Started, Mismatch, NotThisApp, Error, Unsupported };
    // A PackageInstaller session from the file, its bytes checked against `sha256` and `size` as they are
    // written and its package and versionCode against this app and `versionCode`; Android then asks the
    // player.
    InstallStart install(std::string_view file, u64 size, std::string_view sha256, u32 versionCode);
    enum class InstallStatus : u8 { Waiting, Success, Aborted, Failed };
    InstallStatus installStatus(std::string& message);

    // The browser on `url`.
    void openUrl(std::string_view url);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_downloadPath;
};

} // namespace vd
