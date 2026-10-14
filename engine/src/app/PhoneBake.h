#pragma once

#include "app/PhoneBakePlan.h"
#include "platform/BakeLauncher.h"
#include "platform/PlatformInfo.h"

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

// Phase 22 (phonebake, ROADMAP 22.7, ADR-0005 §8): the game's side of baking the packs on the phone. A thread
// of its own (vd-bake), never the game's: it checks the WoW copied to the phone (pack::checkWowData) and the
// free room, writes the plan (app/PhoneBakePlan.h) and asks the platform to run it (BakeLauncher: the
// foreground service BakeService in the process :baker on Android, PhoneBakeRunner on the desktop), then
// follows its status.json. The game reads status() every frame (a copy, fixed size) and never waits.
//
// The first-run screen starts a first bake (firstBakeJobs()); after an update, the update flow starts the
// steps whose recipes changed: start(jobsForPacks(pack::outdatedPacks(manifest, pack::releaseRecipes())),
// PhoneBakeKind::Update) when the WoW is on the phone (status().wowComplete).
namespace vd {

// A step's name for the player ("textures" -> "Textures"; core/Text, VD_PHONE_STEP_*).
std::string_view phoneBakeStepText(std::string_view step);

struct PhoneBakeStatus {
    u32 sequence = 0; // grows with every change
    // The WoW in <app files>/wow/Data and the room (check()).
    bool checked = false;
    bool wowFound = false;    // some *.MPQ there
    bool wowComplete = false; // the 12 game archives, whole
    u32 wowMissing = 0;
    u32 wowDamaged = 0;
    std::array<char, 96> wowProblem{}; // the archives missing or damaged, comma-separated (cut short)
    u64 wowBytes = 0;
    u64 freeBytes = 0;        // free where the packs go
    u64 firstBakeBytes = 0;   // what a first bake needs at its peak (phoneBakeSpaceNeeded)
    f64 firstBakeSeconds = 0; // what it takes on a Mi 11X
    // The bake in <app files>/bake.
    PhoneBakeState state = PhoneBakeState::None;
    bool starting = false; // asked to run; its service has not answered yet
    bool stale = false;    // its status stopped changing: Android stopped the service (state: Interrupted)
    PhoneBakeKind kind = PhoneBakeKind::First;
    u32 step = 0; // the step running, or the one that failed
    u32 stepCount = 0;
    u32 finished = 0;
    std::array<char, 16> stepName{}; // "textures"
    f32 fraction = 0.0f;             // of the whole bake
    f32 stepFraction = 0.0f;
    f64 secondsLeft = 0.0;
    f64 elapsedSeconds = 0.0;
    i32 jobs = 0; // threads the step runs with
    i32 maxJobs = 0;
    i32 exitCode = 0;
    std::array<char, 160> error{}; // why it failed (the baker's error line)
    bool launchFailed = false;     // the platform could not start it (Interrupted)
    u64 neededBytes = 0;           // the room the steps left need
    bool active() const {
        return starting || state == PhoneBakeState::Running || state == PhoneBakeState::Cooling;
    }
    // Stopped half way (failed, cancelled, interrupted): it can go on from its last finished step.
    bool resumable() const {
        return !active() && stepCount > 0 && finished < stepCount &&
               (state == PhoneBakeState::Failed || state == PhoneBakeState::Cancelled ||
                state == PhoneBakeState::Interrupted);
    }
};

class PhoneBake {
public:
    struct Config {
        PhoneBakePlaces places;
        std::unique_ptr<BakeLauncher> launcher; // null: nothing can run a plan (status says so)
        u32 cores = 0;                          // 0: this device's
    };
    // The places and the launcher of this platform: <packs>/../wow, <packs>/../bake; Android's service, the
    // desktop's runner.
    static Config configFor(const PlatformInfo& platform);

    PhoneBake() = default;
    ~PhoneBake();
    PhoneBake(const PhoneBake&) = delete;
    PhoneBake& operator=(const PhoneBake&) = delete;

    void init(Config config); // the thread: checks the WoW, reads the bake left in <bakeDir>
    void shutdown();          // the thread ends; a bake running in its service goes on

    // A new bake of these steps (false while one runs). The notification's texts are taken in the player's
    // language now.
    bool start(std::vector<PhoneBakeJob> jobs, PhoneBakeKind kind = PhoneBakeKind::Update);
    bool resume();  // the bake stopped half way, from its last finished step
    void cancel();  // stops it; what is finished stays
    void discard(); // forgets a bake that is not running (its plan, its status, the staging folder)
    void check();   // the WoW and the room again
    PhoneBakeStatus status() const;
    bool started() const { return m_thread.joinable(); }

private:
    enum class Command : u8 { None, Start, Resume, Cancel, Discard, Check };
    struct Request {
        Command command = Command::None;
        std::vector<PhoneBakeJob> jobs;
        PhoneBakeKind kind = PhoneBakeKind::Update;
    };

    void run();
    void execute(Request& request);
    void startPlan(Request& request);
    void checkDevice();
    void refresh();
    void publish();

    Config m_config;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<Request> m_requests;
    bool m_quit = false;
    PhoneBakeStatus m_status; // what status() returns

    // The thread's own.
    std::optional<PhoneBakePlan> m_plan;
    PhoneBakeRun m_run;
    PhoneBakeStatus m_working;
    i64 m_requestedMs = 0; // when the platform was asked to run the plan (0: not waiting for it)
    bool m_launchFailed = false;
    std::vector<std::string> m_skipArchives;
    u32 m_cores = 1;
};

} // namespace vd
