#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Phase 22 (phonebake, ROADMAP 22.7, ADR-0005 §8): the phone bakes its own packs from the WoW copied to it
// (<app files>/wow/Data). The baker (vd_baker built for Android, shipped in the APK as
// lib/arm64-v8a/libvdbaker.so) runs in a foreground service of its own process (:baker, BakeService.java),
// one step per process; the game never runs it and never waits for it. The two talk through two small files
// in <app files>/bake:
//
//   plan.json    written by the game (PhoneBake): the steps, each with its vd_baker arguments (the bake into
//                bake/staging, then `vd_baker install` into packs/), its cost as measured, its label, and the
//                notification's texts (in the player's language: the service reads no text table) and the
//                thermal policy (how many threads at each thermal headroom and status).
//   status.json  written by the service (and by PhoneBakeRunner, its C++ twin on the desktop): the steps
//                finished, the step running, the baker's last line, the threads, the temperature; every
//                couple of seconds while it runs (a status that stops changing is a service Android killed).
//
// A step that ends well is never repeated: after a killed service, a cancel or a failure the bake goes on
// from the next step (and textures, the longest, from the last texture it finished: its resume cache). This
// file holds the rules, without Android (the tests): which steps, what they cost and need, the two files, the
// progress.
namespace vd {

// One step of `vd_baker bake --only`.
struct PhoneBakeJob {
    std::string step;              // "db", "textures", "world"...
    std::vector<std::string> maps; // world / collision: these maps only (--maps); empty: every map
};

enum class PhoneBakeKind : u8 {
    First,  // the phone has no packs: every step
    Update, // a new version changed some recipes: only those steps
};

// Every step of a first bake, in the order they run: db first (11 s: a client that cannot be read fails at
// once), then textures (half the time, on a phone that is still cool, and while little else is on the disk:
// its resume cache never raises the peak of space), then the rest as measured. The game cannot start with
// part of the packs in a useful way (without collision the character walks through the world; without sound
// it is silent), so the order is not chosen to play sooner.
std::vector<PhoneBakeJob> firstBakeJobs();
// The steps that bake these packs again (pack::outdatedPacks against this app's pack::releaseRecipes()), in
// the order of a first bake, the maps of world_* and collision_* merged per step (liquids.vdpk: the whole
// world step). Packs no step of the phone writes (app.vdpk travels in the APK; the language overlays need a
// second client) are left out.
std::vector<PhoneBakeJob> jobsForPacks(std::span<const std::string> packs);
// A step the phone bakes (those of firstBakeJobs(); not app, which travels in the APK, nor locale, which needs a
// second client).
bool isPhoneBakeStep(std::string_view step);

// What a step costs, as measured on a Mi 11X (Snapdragon 870, 7 threads, no cache: 2265 s with Azeroth alone)
// and the size of what it writes from an enUS client (every map), scaled for a step of a few maps.
struct PhoneBakeCost {
    f64 seconds = 0.0;
    u64 bytes = 0;    // the packs it writes
    u64 replaces = 0; // the packs of the folder it replaces (an update)
};
// `existing`: the packs already in the folder (file name -> bytes). A step's output is the size of the packs
// it replaces when they are there, else the reference (a few maps: a quarter of the step per map, at most
// all).
PhoneBakeCost phoneBakeCost(const PhoneBakeJob& job, const std::map<std::string, u64>& existing);
// The packs of `packsDir` (the client packs: *.vdpk but app.vdpk) with their sizes.
std::map<std::string, u64> listClientPacks(const std::filesystem::path& packsDir);

// The thermal policy of the service: at most `jobs` threads from each headroom up (the ADPF forecast 10 s
// ahead, 1.0 = severe throttling) and from each thermal status up (PowerManager.THERMAL_STATUS_*: 3 severe, 4
// critical); 0 threads: wait for the phone to cool down. Rows ascending.
struct PhoneBakeThermal {
    std::vector<std::pair<f32, i32>> headroom;
    std::vector<std::pair<i32, i32>> status;
    i32 maxJobs = 1;
};
// The policy for a device with `cores` cores: every core but one while cool (the baker's own default), half
// from a headroom of 0.75, 2 from 0.9, 1 from 1.0 or the severe status, none from the critical status.
PhoneBakeThermal phoneBakeThermal(u32 cores);
// The threads for a reading (headroom NaN or status < 0: unknown, that row is skipped).
i32 phoneBakeJobs(const PhoneBakeThermal& thermal, f32 headroom, i32 status);
// A step that keeps a resume cache (textures) is stopped and started again with fewer threads when the phone
// heats: when it must wait, or when the threads fall to half or fewer, at most once every 3 minutes.
inline constexpr f64 kPhoneBakeRestartSeconds = 180.0;
bool phoneBakeRestart(i32 runningJobs, i32 wantedJobs, f64 secondsSinceStart);

// Where the bake lives: the packs folder, the WoW (the client folder: wow, holding Data) and <app
// files>/bake.
struct PhoneBakePlaces {
    std::filesystem::path packsDir;
    std::filesystem::path wowDir;
    std::filesystem::path bakeDir;
    std::filesystem::path planFile() const { return bakeDir / "plan.json"; }
    std::filesystem::path statusFile() const { return bakeDir / "status.json"; }
    std::filesystem::path logFile() const { return bakeDir / "bake.log"; }
    std::filesystem::path stagingDir() const { return bakeDir / "staging"; }
};

// The notification's texts, in the player's language (the service process reads no text table).
struct PhoneBakeTexts {
    std::string channel; // the notification channel's name (Android's settings)
    std::string title;   // while it runs
    std::string cancel;  // the notification's button
    std::string cooling; // waiting for the phone to cool down
    std::string done;    // it ended well: tap to play
    std::string failed;  // a step failed: open the app
    std::string stopped; // Android stopped it (the app shows how to go on)
};

struct PhoneBakePlanStep {
    std::string name;
    std::vector<std::string> maps;
    std::string label; // "Textures (2 of 13)", for the notification
    f64 seconds = 0.0;
    u64 bytes = 0;
    u64 replaces = 0;
    bool restartable = false;         // keeps a resume cache in the staging folder
    std::vector<std::string> bake;    // vd_baker's arguments, without --jobs (the service adds it)
    std::vector<std::string> install; // vd_baker install: the staging folder's packs into packs/
};

struct PhoneBakePlan {
    std::string id; // tells this plan's status.json from an older one's
    PhoneBakeKind kind = PhoneBakeKind::First;
    i64 createdMs = 0; // wall clock (ms since 1970), as the service writes its times
    std::string packsDir, stagingDir, statusFile, logFile; // UTF-8 paths
    PhoneBakeTexts texts;
    PhoneBakeThermal thermal;
    std::vector<PhoneBakePlanStep> steps;
};

struct PhoneBakePlanInputs {
    PhoneBakePlaces places;
    PhoneBakeKind kind = PhoneBakeKind::First;
    std::string id;
    i64 nowMs = 0;
    u32 cores = 8;
    std::vector<std::string> skipArchives; // the WoW's unofficial archives (pack::checkWowData)
    std::map<std::string, u64> existing;   // the packs already in the folder (listClientPacks)
    PhoneBakeTexts texts;
    // The label of step `index` (0-based) of `count`, e.g. text::format of VD_PHONE_STEP_OF.
    std::string (*label)(std::string_view step, u32 index, u32 count) = nullptr;
};
PhoneBakePlan makePhoneBakePlan(std::span<const PhoneBakeJob> jobs, const PhoneBakePlanInputs& inputs);
std::string writePhoneBakePlan(const PhoneBakePlan& plan);
Result<PhoneBakePlan> readPhoneBakePlan(std::string_view text);

// The room a plan needs beyond what the folder holds now, at its peak (the packs of the steps done so far,
// the one being baked into the staging folder while the one it replaces is still there, and textures' resume
// cache), plus kPhoneBakeSpareBytes for the system and the game's own files.
inline constexpr u64 kPhoneBakeSpareBytes = 256ull << 20;
u64 phoneBakeSpaceNeeded(const PhoneBakePlan& plan, u32 fromStep = 0);
f64 phoneBakeSeconds(const PhoneBakePlan& plan, u32 fromStep = 0);

enum class PhoneBakeState : u8 {
    None,        // no bake (no status.json, or one of another plan)
    Running,     // a step runs
    Cooling,     // waiting for the phone to cool down
    Done,        // every step finished
    Failed,      // step `step` failed (`exitCode`, `error`)
    Cancelled,   // stopped by the player: it goes on from `finished`
    Interrupted, // stopped by Android (a time limit, not allowed to run): it goes on from `finished`
};
std::string_view phoneBakeStateName(PhoneBakeState state);
PhoneBakeState phoneBakeStateFrom(std::string_view name);

// status.json.
struct PhoneBakeRun {
    std::string plan; // the plan's id
    PhoneBakeState state = PhoneBakeState::None;
    u32 finished = 0; // steps that ended well, in order
    u32 step = 0;     // the step running (or the one that failed)
    i64 planStartedMs = 0;
    i64 stepStartedMs = 0;
    i64 updatedMs = 0;
    f32 fraction = -1.0f; // the running step's own progress from the baker's lines; < 0: unknown
    std::string line;     // the baker's last line
    std::string error;    // why it failed or was interrupted
    i32 exitCode = 0;
    i32 jobs = 0;
    f32 headroom = -1.0f;
    i32 thermalStatus = -1;
    std::vector<f64> seconds; // what each finished step took
};
std::string writePhoneBakeRun(const PhoneBakeRun& run);
Result<PhoneBakeRun> readPhoneBakeRun(std::string_view text);

// A running or cooling status not written for this long belongs to a service that is gone (Android killed it,
// the phone restarted): the bake goes on when the player says so.
inline constexpr i64 kPhoneBakeStaleMs = 30'000;
bool phoneBakeStale(const PhoneBakeRun& run, i64 nowMs);
// The steps left: from `finished` of a status of this plan, else all.
u32 phoneBakeResumeStep(const PhoneBakePlan& plan, const PhoneBakeRun& run);

// The fraction of a step in one of the baker's lines: "[INFO] textures: 1200 / 58502 (40 s)" for the step
// "textures" -> 0.0205; < 0 for any other line.
f32 phoneBakeLineFraction(std::string_view line, std::string_view step);
// An error line of the baker ("[ERROR] ..."): its text; empty for any other line.
std::string_view phoneBakeErrorText(std::string_view line);

struct PhoneBakeProgress {
    f32 fraction = 0.0f;     // of the whole plan, by the steps' measured costs
    f32 stepFraction = 0.0f; // of the step running
    f64 secondsLeft = 0.0;   // the estimate, scaled by how this phone did the steps already done
    f64 elapsedSeconds = 0.0;
};
PhoneBakeProgress phoneBakeProgress(const PhoneBakePlan& plan, const PhoneBakeRun& run, i64 nowMs);

// Whether <bakeDir> holds a first bake that has not ended (the game opens on the first-run screen until it
// does: its first packs are in packs/ long before the last).
bool phoneFirstBakeUnfinished(const std::filesystem::path& bakeDir);

} // namespace vd
