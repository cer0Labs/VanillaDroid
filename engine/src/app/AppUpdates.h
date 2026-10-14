#pragma once

#include "core/Types.h"
#include "pack/ReleaseManifest.h"

#include <array>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vd {

struct PlatformInfo;

// Phase 22 (updates, ADR-0005 §7): the app tells the player about a new release and installs it. At start, on
// a thread of its own ("vd-updates": the game never waits for it), once the device has a network: the latest
// release's manifest.json and its signature over HTTPS (platform/UpdatePlatform), verified
// (core/ReleaseSignature) and read (pack/ReleaseManifest). A newer versionCode becomes a notice on the glue
// screens (app/UpdateScreens): Update, Later (for a day), What's new (the release's page in the browser).
// Update: Android's "install unknown apps" permission first when it is off, then the APK with
// DownloadManager, its size and SHA-256 checked, and a PackageInstaller session that Android asks the player
// to confirm. Only distribution builds check by themselves; development builds only with VD_UPDATE_URL (a
// local release folder: docs/ANDROID.md).
//
// Also at start: the packs baked for another version (their recipes are not this app's, pack::outdatedPacks:
// right after an update that changes game data), told once on the glue screens with what to do: VanillaDroid
// Setup on the PC, or the phone itself when the player's WoW was copied to it (files/wow/Data; the baking is
// phase 22.7's, hooked through setPhoneBaker).
//
// One instance for the process (appUpdates()). state() and the commands are for the main thread and never
// block or allocate; the thread does the work.
class AppUpdates {
public:
    enum class Stage : u8 {
        Off,            // no check in this build or system
        Checking,       // waiting for a network, or reading the manifest
        None,           // nothing newer (or the check failed: the log says why)
        Available,      // a newer release (the notice)
        TooNew,         // a newer release whose manifest this app cannot read: update by hand
        Dismissed,      // Later: no notice for this release today
        NeedPermission, // Android does not let the app install yet: its settings screen
        Downloading,
        Verifying,
        Confirming, // Android asked the player (its dialog); the app restarts updated
        Failed,
        Downloaded, // desktop (development): downloaded and checked; an APK installs on the phone
    };
    enum class Problem : u8 { None, Network, Mismatch, Refused, Install, Unsupported };

    // A copy of where it stands, for the main thread (fixed size).
    struct State {
        Stage stage = Stage::Off;
        Problem problem = Problem::None;
        u32 versionCode = 0;
        std::array<char, 24> version{}; // the release's ("0.2.0")
        std::array<char, 160>
            releaseData{}; // the game data the release bakes again ("world (azeroth), models")
        u64 bytes = 0;     // Downloading
        u64 total = 0;
        std::array<char, 160> detail{}; // Failed: Android's words; Downloaded: the file
        // The packs on the device now: those baked for another version, and whether the WoW is here to bake
        // them.
        std::array<char, 160> outdatedData{};
        bool phoneHasWow = false;
        bool phoneBaker = false; // a phone baker is hooked (22.7)
        u32 serial = 0;          // changes with every change of the rest
    };

    AppUpdates();
    ~AppUpdates();
    AppUpdates(const AppUpdates&) = delete;
    AppUpdates& operator=(const AppUpdates&) = delete;

    // Starts the thread: the check (when this build checks) and the look at the packs. `packsDir`: the packs;
    // `cacheDir`: where "Later" is remembered and the desktop's downloads go.
    void start(const PlatformInfo& platform, const std::filesystem::path& packsDir,
               const std::filesystem::path& cacheDir);
    // Before core/Text unloads. Waits a moment for the thread; one stuck in a fetch is left to end by itself.
    void stop();

    State state() const;
    // The release's notes page (copied: main thread, on a tap).
    // Commands.
    void update(std::string_view downloadTitle); // the title of Android's download notification
    void later();                                // the notice hides for a day
    void openNotes();                            // the release's page (or the releases', when too new)
    void openSettings();                         // NeedPermission: Android's screen for this app
    void cancel();                               // a download
    void bakeOnPhone();                          // the outdated packs, by the phone baker (22.7)

    // Phase 22.7's hook: bakes these steps on the phone with files/wow/Data (in its own process). Set before
    // start().
    using PhoneBaker = std::function<void(const std::vector<pack::StepPlan>& steps)>;
    void setPhoneBaker(PhoneBaker baker) { m_phoneBaker = std::move(baker); }

private:
    enum class Command : u8 { None, Update, Later, Notes, Settings, Cancel, Stop };
    struct Shared;

    void run(const PlatformInfo& platform);
    void publish(const std::function<void(State&)>& change);
    Command wait(u32 milliseconds); // the next command, or None after the time
    bool stopping() const;
    void checkPacks();
    bool check(class UpdatePlatform& platform);
    void install(class UpdatePlatform& platform);
    bool dismissedToday(u32 versionCode) const;
    void rememberLater(u32 versionCode) const;
    void runDemo(std::string_view demo);

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done; // the thread ended (m_finished)
    bool m_finished = true;         // no thread of this instance runs (under m_mutex)
    State m_state;
    Command m_command = Command::None;
    std::string m_title; // for Android's download notification (under m_mutex)
    std::thread m_thread;
    std::filesystem::path m_packsDir;
    std::filesystem::path m_cacheDir;
    std::string m_url;
    pack::ReleaseManifest m_release; // the thread's
    std::vector<pack::StepPlan> m_outdatedSteps;
    PhoneBaker m_phoneBaker;
    void* m_javaVM = nullptr; // only these of PlatformInfo
    void* m_activity = nullptr;
    std::string m_platformCache;
};

AppUpdates& appUpdates();

} // namespace vd
