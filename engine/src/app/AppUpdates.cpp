#include "app/AppUpdates.h"

#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Sha256.h"
#include "core/Version.h"
#include "platform/PlatformInfo.h"
#include "platform/Thread.h"
#include "platform/UpdatePlatform.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

namespace vd {
namespace {

constexpr u32 kOnlinePollMs = 15000;      // how often a device without a network is asked again...
constexpr u32 kOnlineWaitMs = 10 * 60000; // ...and for how long
constexpr u32 kFetchTimeoutMs = 15000;
constexpr u32 kStopWaitMs = 1500;
constexpr u32 kRetryMs = 60000; // after a failed fetch
constexpr u32 kAttempts = 3;
constexpr u32 kPollMs = 250; // a download, Android's answer to the installation
constexpr i64 kLaterSeconds = 24 * 3600;
constexpr const char* kLaterFile = "update-later.txt"; // in the cache folder: "<versionCode> <unix seconds>"

template <usize N> void copyText(std::array<char, N>& out, std::string_view text) {
    const usize n = std::min(text.size(), N - 1);
    std::memcpy(out.data(), text.data(), n);
    out[n] = '\0';
}

i64 unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string_view asText(const std::vector<u8>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

AppUpdates& appUpdates() {
    static AppUpdates instance;
    return instance;
}

AppUpdates::AppUpdates() = default;

AppUpdates::~AppUpdates() {
    stop();
}

void AppUpdates::start(const PlatformInfo& platform, const std::filesystem::path& packsDir,
                       const std::filesystem::path& cacheDir) {
    if (m_thread.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_finished) {
            // The last run's thread was left to end a fetch on its own (stop()): no check this time.
            VD_LOG_WARN("Updates: the previous check is still running");
            return;
        }
        m_finished = false;
    }
    m_packsDir = packsDir;
    m_cacheDir = cacheDir;
    m_javaVM = platform.javaVM;
    m_activity = platform.activity;
    m_platformCache = platform.cacheDir;
    // The published app asks the releases' fixed URL; a development build only a local release folder's.
    if constexpr (kDistBuild) {
        m_url = std::string(pack::kReleaseManifestUrl);
    } else if (const char* url = VD_DEV_ENV("VD_UPDATE_URL"); url && *url) {
        m_url = url;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_command = Command::None;
        m_state = State{};
        m_state.phoneBaker = static_cast<bool>(m_phoneBaker);
    }
    m_thread = std::thread([this] {
        PlatformInfo info;
        info.javaVM = m_javaVM;
        info.activity = m_activity;
        info.cacheDir = m_platformCache;
        run(info);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_finished = true;
        m_done.notify_all();
    });
}

void AppUpdates::stop() {
    if (!m_thread.joinable()) {
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_command = Command::Stop;
    m_wake.notify_all();
    // Everything but a fetch answers within half a second; a fetch in flight (up to its timeout) must not
    // hold the game's exit (Android's main thread waits for it): the thread then ends by itself.
    if (m_done.wait_for(lock, std::chrono::milliseconds(kStopWaitMs), [&] { return m_finished; })) {
        lock.unlock();
        m_thread.join();
    } else {
        lock.unlock();
        VD_LOG_WARN("Updates: a fetch is still running: left to end alone");
        m_thread.detach();
    }
}

AppUpdates::State AppUpdates::state() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

void AppUpdates::update(std::string_view downloadTitle) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_command == Command::Stop) {
            return;
        }
        m_title.assign(downloadTitle);
        m_command = Command::Update;
    }
    m_wake.notify_all();
}

#define VD_UPDATES_COMMAND(name, value)                                                                      \
    void AppUpdates::name() {                                                                                \
        {                                                                                                    \
            std::lock_guard<std::mutex> lock(m_mutex);                                                       \
            if (m_command == Command::Stop) {                                                                \
                return;                                                                                      \
            }                                                                                                \
            m_command = Command::value;                                                                      \
        }                                                                                                    \
        m_wake.notify_all();                                                                                 \
    }
VD_UPDATES_COMMAND(later, Later)
VD_UPDATES_COMMAND(openNotes, Notes)
VD_UPDATES_COMMAND(openSettings, Settings)
VD_UPDATES_COMMAND(cancel, Cancel)
#undef VD_UPDATES_COMMAND

void AppUpdates::bakeOnPhone() {
    std::vector<pack::StepPlan> steps;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        steps = m_outdatedSteps;
    }
    if (m_phoneBaker && !steps.empty()) {
        VD_LOG_INFO("Updates: the phone bakes %s", pack::describeSteps(steps).c_str());
        m_phoneBaker(steps);
    }
}

void AppUpdates::publish(const std::function<void(State&)>& change) {
    std::lock_guard<std::mutex> lock(m_mutex);
    change(m_state);
    ++m_state.serial;
}

bool AppUpdates::stopping() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_command == Command::Stop;
}

AppUpdates::Command AppUpdates::wait(u32 milliseconds) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_wake.wait_for(lock, std::chrono::milliseconds(milliseconds),
                    [&] { return m_command != Command::None; });
    const Command command = m_command;
    if (command != Command::Stop) {
        m_command = Command::None;
    }
    return command;
}

// --- The packs on the device
// --------------------------------------------------------------------------------------

void AppUpdates::checkPacks() {
    Result<pack::PackManifest> manifest = pack::PackManifest::load(m_packsDir);
    std::vector<std::string> outdated;
    if (manifest && !manifest.value().packs.empty()) {
        // Baked for another version (a recipe that is not this app's). Packs the device lacks are not this
        // notice's: the first run and the PC's download see to them.
        for (const std::string& file : pack::outdatedPacks(manifest.value(), pack::releaseRecipes())) {
            if (manifest.value().find(file)) {
                outdated.push_back(file);
            }
        }
    }
    std::error_code error;
    const bool wow = std::filesystem::is_directory(m_packsDir.parent_path() / "wow" / "Data", error);
    const std::vector<pack::StepPlan> steps = pack::bakeStepsFor(outdated);
    const std::string described = pack::describeSteps(steps);
    if (!steps.empty()) {
        VD_LOG_INFO("Updates: game data baked for another version: %s%s", described.c_str(),
                    wow ? " (the WoW is on this device)" : "");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outdatedSteps = steps;
    copyText(m_state.outdatedData, described);
    m_state.phoneHasWow = wow;
    ++m_state.serial;
}

// --- "Later"
// ----------------------------------------------------------------------------------------------------------

bool AppUpdates::dismissedToday(u32 versionCode) const {
    std::ifstream file(m_cacheDir / kLaterFile);
    unsigned long long code = 0;
    long long when = 0;
    if (!(file >> code >> when)) {
        return false;
    }
    const i64 now = unixNow();
    return code == versionCode && now >= when && now - when < kLaterSeconds;
}

void AppUpdates::rememberLater(u32 versionCode) const {
    if (m_cacheDir.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(m_cacheDir, error);
    std::ofstream file(m_cacheDir / kLaterFile, std::ios::trunc);
    file << versionCode << ' ' << unixNow() << '\n';
}

// --- The thread
// ----------------------------------------------------------------------------------------------------

void AppUpdates::run(const PlatformInfo& info) {
    setCurrentThreadName("vd-updates");
    checkPacks();
    if (const char* demo = VD_DEV_ENV("VD_UPDATE_DEMO"); demo && *demo) {
        runDemo(demo);
        return;
    }
    if (m_url.empty()) {
        return; // a development build without VD_UPDATE_URL: Off
    }
    UpdatePlatform platform;
    if (!platform.open(info)) {
        VD_LOG_INFO("Updates: not on this system");
        platform.close();
        return;
    }
    publish([](State& s) { s.stage = Stage::Checking; });
    if (check(platform)) {
        for (;;) {
            const Command command = wait(60000);
            if (command == Command::Stop) {
                break;
            }
            switch (command) {
            case Command::Update: install(platform); break;
            case Command::Later:
                rememberLater(m_release.versionCode);
                publish([](State& s) { s.stage = Stage::Dismissed; });
                break;
            case Command::Notes:
                platform.openUrl(m_release.tooNew() || m_release.notes.empty() ? pack::kReleasePageUrl
                                                                               : m_release.notes);
                break;
            case Command::Settings: platform.openInstallSettings(); break;
            default: break;
            }
        }
    }
    platform.close();
}

bool AppUpdates::check(UpdatePlatform& platform) {
    // A network first (a phone in a plane, a tunnel): asked again now and then, not forever.
    for (u32 waited = 0; !platform.online(); waited += kOnlinePollMs) {
        if (waited >= kOnlineWaitMs || wait(kOnlinePollMs) == Command::Stop) {
            publish([](State& s) { s.stage = Stage::None; });
            return false;
        }
    }
    const std::string signatureUrl = m_url + std::string(pack::kReleaseSignatureSuffix);
    for (u32 attempt = 0; attempt < kAttempts; ++attempt) {
        Result<std::vector<u8>> json = platform.fetch(m_url, pack::kMaxReleaseManifestBytes, kFetchTimeoutMs);
        if (stopping()) {
            return false; // stop() may have left this thread alone: nothing more of Java's
        }
        Result<std::vector<u8>> signature =
            json ? platform.fetch(signatureUrl, pack::kMaxReleaseSignatureBytes, kFetchTimeoutMs)
                 : Result<std::vector<u8>>(json.error());
        if (stopping()) {
            return false;
        }
        if (!json || !signature) {
            VD_LOG_WARN("Updates: %s", (!json ? json : signature).error().message.c_str());
            if (wait(kRetryMs) == Command::Stop) {
                return false;
            }
            continue;
        }
        pack::SignedRelease release =
            pack::readSignedRelease(asText(json.value()), asText(signature.value()));
        switch (release.status) {
        case pack::SignedRelease::Status::BadSignature:
            VD_LOG_ERROR("Updates: the signature of %s does not verify: ignored", m_url.c_str());
            publish([](State& s) { s.stage = Stage::None; });
            return false;
        case pack::SignedRelease::Status::Damaged:
            VD_LOG_ERROR("Updates: %s", release.error.c_str());
            publish([](State& s) { s.stage = Stage::None; });
            return false;
        case pack::SignedRelease::Status::TooNew:
        case pack::SignedRelease::Status::Ok: break;
        }
        m_release = std::move(release.manifest);
        if (m_release.versionCode <= kVersionCode && !(m_release.tooNew() && m_release.versionCode == 0)) {
            VD_LOG_INFO("Updates: VanillaDroid %s is the latest (%s)", kVersion, m_release.version.c_str());
            publish([](State& s) { s.stage = Stage::None; });
            return false;
        }
        std::string data;
        if (!m_release.tooNew()) {
            Result<pack::PackManifest> folder = pack::PackManifest::load(m_packsDir);
            const std::vector<std::string> changed = pack::releaseDataChanges(
                folder ? folder.value() : pack::PackManifest{}, m_release.packs, pack::releaseRecipes());
            data = pack::describeSteps(pack::bakeStepsFor(changed));
        }
        const bool later = dismissedToday(m_release.versionCode);
        VD_LOG_INFO("Updates: VanillaDroid %s is available (this is %s)%s%s%s", m_release.version.c_str(),
                    kVersion, data.empty() ? "" : "; it bakes again ", data.c_str(),
                    later ? "; Later today" : "");
        publish([&](State& s) {
            s.stage = m_release.tooNew() ? Stage::TooNew : later ? Stage::Dismissed : Stage::Available;
            s.versionCode = m_release.versionCode;
            copyText(s.version, m_release.version);
            copyText(s.releaseData, data);
        });
        return true;
    }
    publish([](State& s) { s.stage = Stage::None; });
    return false;
}

void AppUpdates::install(UpdatePlatform& platform) {
    const pack::ReleaseFile& apk = m_release.apk;
    const auto fail = [&](Problem problem, std::string_view detail) {
        publish([&](State& s) {
            s.stage = Stage::Failed;
            s.problem = problem;
            copyText(s.detail, detail);
        });
    };
    if (m_release.tooNew() || !apk.present()) {
        fail(Problem::Unsupported, {});
        return;
    }
    // Android lets an app install others only once the player allows it (Settings > Install unknown apps).
    if (!platform.canInstall()) {
        publish([](State& s) { s.stage = Stage::NeedPermission; });
        for (;;) {
            const Command command = wait(500);
            if (command == Command::Stop) {
                return;
            }
            if (command == Command::Cancel || command == Command::Later) {
                if (command == Command::Later) {
                    rememberLater(m_release.versionCode);
                }
                publish([&](State& s) {
                    s.stage = command == Command::Later ? Stage::Dismissed : Stage::Available;
                });
                return;
            }
            if (command == Command::Settings) {
                platform.openInstallSettings();
            }
            if (platform.canInstall()) {
                break;
            }
        }
    }
    std::string title;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        title = m_title;
    }
    const std::string fileName = "VanillaDroid-" + m_release.version + ".apk";
    if (!platform.startDownload(apk.url, fileName, title)) {
        fail(Problem::Network, {});
        return;
    }
    VD_LOG_INFO("Updates: downloading %s", apk.url.c_str());
    publish([&](State& s) {
        s.stage = Stage::Downloading;
        s.problem = Problem::None;
        s.bytes = 0;
        s.total = apk.size;
    });
    std::error_code error;
    const std::string downloaded = platform.downloadedFile();
    const std::filesystem::path file =
        std::filesystem::path(std::u8string(downloaded.begin(), downloaded.end()));
    for (;;) {
        const Command command = wait(kPollMs);
        if (command == Command::Stop || command == Command::Cancel) {
            platform.cancelDownload();
            std::filesystem::remove(file, error);
            if (command == Command::Cancel) {
                publish([](State& s) { s.stage = Stage::Available; });
            }
            return;
        }
        const UpdatePlatform::DownloadState download = platform.pollDownload();
        publish([&](State& s) {
            s.bytes = download.bytes;
            s.total = download.total > 0 ? download.total : apk.size;
        });
        if (download.status == UpdatePlatform::DownloadState::Status::Failed) {
            std::filesystem::remove(file, error);
            fail(Problem::Network, {});
            return;
        }
        if (download.status == UpdatePlatform::DownloadState::Status::Done) {
            break;
        }
    }
    // Its size and SHA-256 are the signed manifest's, or it goes.
    publish([](State& s) { s.stage = Stage::Verifying; });
    const u64 size = std::filesystem::file_size(file, error);
    Result<std::string> digest = sha256File(file);
    if (error || size != apk.size || !digest || digest.value() != apk.sha256) {
        VD_LOG_ERROR("Updates: %s is not the release's APK (%llu bytes)", platform.downloadedFile().c_str(),
                     static_cast<unsigned long long>(size));
        std::filesystem::remove(file, error);
        fail(Problem::Mismatch, {});
        return;
    }
    switch (platform.install(platform.downloadedFile(), apk.size, apk.sha256, m_release.versionCode)) {
    case UpdatePlatform::InstallStart::Started: break;
    case UpdatePlatform::InstallStart::Unsupported:
        VD_LOG_INFO("Updates: %s downloaded and checked", platform.downloadedFile().c_str());
        publish([&](State& s) {
            s.stage = Stage::Downloaded;
            copyText(s.detail, platform.downloadedFile());
        });
        return;
    case UpdatePlatform::InstallStart::Mismatch:
    case UpdatePlatform::InstallStart::NotThisApp:
        std::filesystem::remove(file, error);
        fail(Problem::Mismatch, {});
        return;
    case UpdatePlatform::InstallStart::Error: fail(Problem::Install, {}); return;
    }
    publish([](State& s) { s.stage = Stage::Confirming; });
    for (;;) {
        const Command command = wait(kPollMs);
        if (command == Command::Stop) {
            return;
        }
        if (command == Command::Later) { // Android's window never came, or the player left it open
            rememberLater(m_release.versionCode);
            publish([](State& s) { s.stage = Stage::Dismissed; });
            return;
        }
        std::string message;
        switch (platform.installStatus(message)) {
        case UpdatePlatform::InstallStatus::Waiting: continue;
        case UpdatePlatform::InstallStatus::Success:
            VD_LOG_INFO("Updates: VanillaDroid %s installed", m_release.version.c_str());
            return; // Android restarts the app as the new version
        case UpdatePlatform::InstallStatus::Aborted:
            publish([](State& s) {
                s.stage = Stage::Available;
                s.problem = Problem::Refused;
            });
            return;
        case UpdatePlatform::InstallStatus::Failed:
            VD_LOG_ERROR("Updates: the installation failed: %s", message.c_str());
            fail(Problem::Install, message);
            return;
        }
    }
}

// --- Development: VD_UPDATE_DEMO
// -------------------------------------------------------------------------------------

void AppUpdates::runDemo(std::string_view demo) {
    // Each step of the notices without a release or a phone (screenshots): available, nodata, toonew,
    // permission, downloading, verifying, confirming, refused, failed, network, mismatch, downloaded, data,
    // dataphone.
    VD_LOG_INFO("Updates: demo %.*s", static_cast<int>(demo.size()), demo.data());
    publish([&](State& s) {
        s.versionCode = 2000;
        copyText(s.version, "0.2.0");
        copyText(s.releaseData, demo == "nodata" ? "" : "models, world (azeroth, kalimdor)");
        s.stage = demo == "toonew"                                              ? Stage::TooNew
                  : demo == "permission"                                        ? Stage::NeedPermission
                  : demo == "downloading"                                       ? Stage::Downloading
                  : demo == "verifying"                                         ? Stage::Verifying
                  : demo == "confirming"                                        ? Stage::Confirming
                  : demo == "failed" || demo == "network" || demo == "mismatch" ? Stage::Failed
                  : demo == "downloaded"                                        ? Stage::Downloaded
                  : demo == "data" || demo == "dataphone"                       ? Stage::None
                                                                                : Stage::Available;
        s.problem = demo == "failed"     ? Problem::Install
                    : demo == "network"  ? Problem::Network
                    : demo == "mismatch" ? Problem::Mismatch
                    : demo == "refused"  ? Problem::Refused
                                         : Problem::None;
        s.bytes = 5600000;
        s.total = 12548774;
        copyText(s.detail, demo == "downloaded"
                               ? "packs/.cache/updates/VanillaDroid-0.2.0.apk"
                               : "INSTALL_FAILED_UPDATE_INCOMPATIBLE: signatures do not match");
        if (demo == "data" || demo == "dataphone") {
            copyText(s.outdatedData, "models, world (azeroth, kalimdor)");
            s.phoneHasWow = demo == "dataphone";
            s.phoneBaker = demo == "dataphone";
        }
    });
    for (;;) {
        const Command command = wait(60000);
        if (command == Command::Stop) {
            return;
        }
        VD_LOG_INFO("Updates: demo command %u", static_cast<u32>(command));
        if (command == Command::Later) {
            publish([](State& s) { s.stage = Stage::Dismissed; });
        } else if (command == Command::Cancel) {
            publish([](State& s) { s.stage = Stage::Available; });
        }
    }
}

} // namespace vd
