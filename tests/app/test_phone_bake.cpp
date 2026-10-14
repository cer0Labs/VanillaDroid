// Phase 22 (phonebake, ROADMAP 22.7): baking the packs on the phone. The rules without Android: which steps
// (a first bake, an update with outdated recipes), what they cost and need (the free-space estimate),
// plan.json and status.json, the progress from the baker's lines, the thermal policy, the runner that the
// service follows (resume after a killed service, a failure, a cancel, a restart with fewer threads, a wait
// while hot), the WoW's archives on the phone, a step's packs into packs/, and the game's side (PhoneBake)
// with a fake service.
#include "app/Pairing.h"
#include "app/PhoneBake.h"
#include "app/PhoneBakePlan.h"
#include "app/PhoneBakeRunner.h"
#include "pack/PackInstall.h"
#include "pack/PackManifest.h"
#include "pack/WowArchives.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream> // doctest prints std::string values through operator<<
#include <string>
#include <thread>
#include <vector>

using namespace vd;

namespace {

std::filesystem::path tempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vd_phone_bake_tests" / name;
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// An MPQ header (format 0) of a file of `fileSize` bytes that says the archive is `archiveSize` long.
std::string mpqBytes(u32 fileSize, u32 archiveSize) {
    std::string bytes(fileSize, '\0');
    const auto put = [&](usize at, u32 value) {
        for (usize i = 0; i < 4; ++i) {
            bytes[at + i] = static_cast<char>((value >> (8 * i)) & 0xff);
        }
    };
    put(0, 0x1A51504Du); // "MPQ\x1A"
    put(4, 32);          // header size
    put(8, archiveSize);
    put(16, 32); // hash table
    put(20, 96); // block table
    put(24, 4);  // hash entries (16 bytes each)
    put(28, 4);  // block entries
    return bytes;
}

PhoneBakePlaces places(const std::filesystem::path& root) {
    return {root / "packs", root / "wow", root / "bake"};
}

PhoneBakePlan planOf(std::span<const PhoneBakeJob> jobs, const std::filesystem::path& root,
                     const std::map<std::string, u64>& existing = {}) {
    PhoneBakePlanInputs inputs;
    inputs.places = places(root);
    inputs.id = "plan-1";
    inputs.nowMs = 1000;
    inputs.cores = 8;
    inputs.existing = existing;
    inputs.texts.title = "Preparing";
    return makePhoneBakePlan(jobs, inputs);
}

bool contains(const std::vector<std::string>& args, const std::string& what) {
    return std::find(args.begin(), args.end(), what) != args.end();
}

std::string after(const std::vector<std::string>& args, const std::string& option) {
    const auto it = std::find(args.begin(), args.end(), option);
    return it != args.end() && it + 1 != args.end() ? *(it + 1) : std::string();
}

// A fake vd_baker for the runner: records each command, prints a progress line, answers with an exit code.
struct FakeBaker {
    std::vector<std::vector<std::string>> calls;
    std::string failStep; // "bake --only <this>" fails
    u32 ticksPerBake = 3; // how many times it calls tick() while it "works"
    std::function<void(const std::vector<std::string>&)> during; // called while a bake works

    i32 run(const std::vector<std::string>& args, const std::function<void(std::string_view)>& onLine,
            const std::function<bool()>& tick) {
        calls.push_back(args);
        if (args.empty() || args[0] != "bake") {
            onLine("[INFO] install: 1 packs, 1.0 MB -> packs");
            return 0;
        }
        const std::string step = after(args, "--only");
        onLine("[INFO] Client in enUS: wow");
        for (u32 i = 0; i < ticksPerBake; ++i) {
            onLine("[INFO] " + step + ": " + std::to_string((i + 1) * 10) + " / 100 (1 s)");
            if (during) {
                during(args);
            }
            if (tick()) {
                return -1; // stopped
            }
        }
        if (step == failStep) {
            onLine("[ERROR] " + step + ": cannot read something.blp");
            onLine("[INFO] bake: 1.0 s, CPU 1.0 s");
            return 1;
        }
        onLine("[INFO] bake: 1.0 s, CPU 1.0 s (1.0 threads busy), peak memory 10 MB");
        return 0;
    }
};

struct FakeClock {
    i64 ms = 1'000'000;
};

PhoneBakeRunnerHooks hooksFor(FakeBaker& baker, FakeClock& clock, const std::atomic<bool>* cancel = nullptr) {
    PhoneBakeRunnerHooks hooks;
    hooks.runBaker = [&baker, &clock](const std::vector<std::string>& args,
                                      const std::function<void(std::string_view)>& onLine,
                                      const std::function<bool()>& tick) {
        return baker.run(args, onLine, [&] {
            clock.ms += 60'000; // a minute per tick
            return tick();
        });
    };
    hooks.nowMs = [&clock] { return clock.ms; };
    hooks.sleep = [&clock](u32 ms) { clock.ms += ms; };
    hooks.cancel = cancel;
    return hooks;
}

PhoneBakeRun statusOf(const std::filesystem::path& root) {
    Result<PhoneBakeRun> run = readPhoneBakeRun(readFile(places(root).statusFile()));
    REQUIRE(run.ok());
    return run.value();
}

void writePlan(const std::filesystem::path& root, const PhoneBakePlan& plan) {
    writeFile(places(root).planFile(), writePhoneBakePlan(plan));
}

std::vector<std::string> bakedSteps(const FakeBaker& baker) {
    std::vector<std::string> steps;
    for (const std::vector<std::string>& call : baker.calls) {
        if (!call.empty() && call[0] == "bake") {
            steps.push_back(after(call, "--only"));
        }
    }
    return steps;
}

} // namespace

TEST_CASE("Phone bake: the steps of a first bake and of an update") {
    const std::vector<PhoneBakeJob> first = firstBakeJobs();
    REQUIRE(first.size() == 13);
    CHECK(first[0].step == "db");       // a client that cannot be read fails in seconds
    CHECK(first[1].step == "textures"); // half the time, while the phone is cool
    CHECK(first.back().step == "bodyfx");
    for (const PhoneBakeJob& job : first) {
        CHECK(job.maps.empty());
        CHECK(job.step != "locale");
        CHECK(job.step != "app");
    }

    // An update: the packs whose recipe changed (pack::outdatedPacks) -> their steps, in the first bake's
    // order, the maps merged; liquids.vdpk is the whole world step; app.vdpk and the overlays are not the
    // phone's.
    const std::vector<std::string> outdated = {
        "world_kalimdor.vdpk", "sound.vdpk", "collision_azeroth.vdpk", "world_azeroth.vdpk",
        "db.esES.vdpk",        "app.vdpk",   "collision_objects.vdpk", "db.vdpk"};
    std::vector<PhoneBakeJob> jobs = jobsForPacks(outdated);
    REQUIRE(jobs.size() == 5);
    CHECK(jobs[0].step == "db");
    CHECK(jobs[1].step == "world");
    CHECK(jobs[1].maps == std::vector<std::string>{"azeroth", "kalimdor"});
    CHECK(jobs[2].step == "collision");
    CHECK(jobs[2].maps == std::vector<std::string>{"azeroth"});
    CHECK(jobs[3].step == "objects");
    CHECK(jobs[4].step == "sound");
    const std::vector<std::string> liquids = {"world_azeroth.vdpk", "liquids.vdpk"};
    jobs = jobsForPacks(liquids);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].step == "world");
    CHECK(jobs[0].maps.empty()); // every map: the shared liquids are rewritten by any world bake
    CHECK(jobsForPacks(std::vector<std::string>{"app.vdpk", "sound.esES.vdpk"}).empty());
    // What the update flow's notice hands over (pack::StepPlan): only the steps the phone bakes.
    CHECK(isPhoneBakeStep("textures"));
    CHECK(isPhoneBakeStep("objects"));
    CHECK_FALSE(isPhoneBakeStep("locale"));
    CHECK_FALSE(isPhoneBakeStep("app"));
}

TEST_CASE("Phone bake: what a step costs and the room a plan needs") {
    // Nothing on the phone: the reference (Mi 11X seconds, the enUS packs' bytes).
    const PhoneBakeCost textures = phoneBakeCost({"textures", {}}, {});
    CHECK(textures.seconds == doctest::Approx(1835.0));
    CHECK(textures.bytes == 1'318'239'516ull);
    CHECK(textures.replaces == 0);
    // The step's packs already there: their size (an update replaces them).
    std::map<std::string, u64> existing = {{"textures.vdpk", 659'119'758ull},
                                           {"world_azeroth.vdpk", 100'000'000ull},
                                           {"world_kalimdor.vdpk", 150'000'000ull},
                                           {"liquids.vdpk", 5'000'000ull}};
    const PhoneBakeCost again = phoneBakeCost({"textures", {}}, existing);
    CHECK(again.bytes == 659'119'758ull);
    CHECK(again.replaces == 659'119'758ull);
    CHECK(again.seconds == doctest::Approx(1835.0 / 2.0).epsilon(0.01));
    const PhoneBakeCost world = phoneBakeCost({"world", {}}, existing);
    CHECK(world.replaces == 255'000'000ull); // its maps and liquids.vdpk
    const PhoneBakeCost maps = phoneBakeCost({"world", {"azeroth", "zulgurub"}}, existing);
    CHECK(maps.replaces == 100'000'000ull);
    CHECK(maps.bytes == 100'000'000ull + 1'776'129'257ull / 4); // a map never baked here: a quarter
    CHECK(phoneBakeCost({"nonsense", {}}, {}).seconds > 0.0);

    // A first bake on an empty phone: the peak is all the packs (textures' resume cache is gone by then), and
    // the measured costs add up to the 42 minutes of a Mi 11X with every map.
    const std::filesystem::path root = tempDir("space");
    const std::vector<PhoneBakeJob> jobs = firstBakeJobs();
    const PhoneBakePlan plan = planOf(jobs, root);
    u64 total = 0;
    for (const PhoneBakePlanStep& step : plan.steps) {
        total += step.bytes;
    }
    CHECK(total > 4'700'000'000ull);
    CHECK(phoneBakeSpaceNeeded(plan) == total + kPhoneBakeSpareBytes);
    CHECK(phoneBakeSeconds(plan) == doctest::Approx(2512.0).epsilon(0.01));
    // From the third step on: what is left.
    CHECK(phoneBakeSpaceNeeded(plan, 2) ==
          total - plan.steps[0].bytes - plan.steps[1].bytes + kPhoneBakeSpareBytes);

    // Textures alone, with its cache: twice its packs at the peak.
    const PhoneBakeJob texturesJob{"textures", {}};
    const PhoneBakePlan texturesPlan = planOf(std::span(&texturesJob, 1), root);
    CHECK(phoneBakeSpaceNeeded(texturesPlan) == 2 * textures.bytes + kPhoneBakeSpareBytes);
    // An update that replaces packs: the new ones in staging while the old are still there, one step at a
    // time.
    const std::vector<PhoneBakeJob> update = {{"textures", {}}, {"world", {"azeroth"}}};
    const PhoneBakePlan updatePlan = planOf(update, root, existing);
    CHECK(phoneBakeSpaceNeeded(updatePlan) == 2 * 659'119'758ull + kPhoneBakeSpareBytes);
}

TEST_CASE("Phone bake: plan.json, the baker's arguments") {
    const std::filesystem::path root = tempDir("plan");
    PhoneBakePlanInputs inputs;
    inputs.places = places(root);
    inputs.id = "1759900000000-1";
    inputs.kind = PhoneBakeKind::Update;
    inputs.nowMs = 1759900000000;
    inputs.cores = 8;
    inputs.skipArchives = {"patch-3.MPQ", "patch-A.MPQ"};
    inputs.texts.channel = "Channel";
    inputs.texts.title = "Preparing \"data\"";
    inputs.texts.cancel = "Stop";
    inputs.label = [](std::string_view step, u32 index, u32 count) {
        return std::string(step) + " " + std::to_string(index + 1) + "/" + std::to_string(count);
    };
    const std::vector<PhoneBakeJob> jobs = {{"db", {}}, {"textures", {}}, {"world", {"azeroth", "kalimdor"}}};
    const PhoneBakePlan plan = makePhoneBakePlan(jobs, inputs);
    REQUIRE(plan.steps.size() == 3);
    const PhoneBakePlanStep& db = plan.steps[0];
    CHECK(db.label == "db 1/3");
    CHECK(db.bake[0] == "bake");
    CHECK(after(db.bake, "--only") == "db");
    CHECK(after(db.bake, "--out") == plan.stagingDir);
    CHECK(after(db.bake, "--cache") == "off");
    CHECK(after(db.bake, "--locale-clients") == ";");
    CHECK(after(db.bake, "--skip-archives") == "patch-3.MPQ;patch-A.MPQ");
    CHECK_FALSE(contains(db.bake, "--jobs")); // the service adds it
    CHECK_FALSE(db.restartable);
    CHECK(db.install ==
          std::vector<std::string>{"install", "--staging", plan.stagingDir, "--packs", plan.packsDir});
    const PhoneBakePlanStep& textures = plan.steps[1];
    CHECK(textures.restartable);
    CHECK(std::filesystem::path(after(textures.bake, "--cache")) == places(root).stagingDir() / ".cache");
    CHECK(after(plan.steps[2].bake, "--maps") == "azeroth,kalimdor");
    CHECK(plan.thermal.maxJobs == 7);

    Result<PhoneBakePlan> read = readPhoneBakePlan(writePhoneBakePlan(plan));
    REQUIRE(read.ok());
    const PhoneBakePlan& again = read.value();
    CHECK(again.id == plan.id);
    CHECK(again.kind == PhoneBakeKind::Update);
    CHECK(again.createdMs == plan.createdMs);
    CHECK(again.packsDir == plan.packsDir);
    CHECK(again.statusFile == plan.statusFile);
    CHECK(again.texts.title == "Preparing \"data\"");
    CHECK(again.texts.channel == "Channel");
    REQUIRE(again.steps.size() == 3);
    for (usize i = 0; i < 3; ++i) {
        CHECK(again.steps[i].name == plan.steps[i].name);
        CHECK(again.steps[i].maps == plan.steps[i].maps);
        CHECK(again.steps[i].bake == plan.steps[i].bake);
        CHECK(again.steps[i].install == plan.steps[i].install);
        CHECK(again.steps[i].restartable == plan.steps[i].restartable);
        CHECK(again.steps[i].bytes == plan.steps[i].bytes);
    }
    CHECK(again.thermal.headroom == plan.thermal.headroom);
    CHECK(again.thermal.status == plan.thermal.status);
    CHECK_FALSE(readPhoneBakePlan("{\"format\": 2}").ok());
    CHECK_FALSE(readPhoneBakePlan("{\"format\": 1, \"id\": \"x\", \"steps\": []}").ok());
}

TEST_CASE("Phone bake: the thermal policy") {
    const PhoneBakeThermal thermal = phoneBakeThermal(8);
    const f32 unknown = std::nanf("");
    CHECK(phoneBakeJobs(thermal, unknown, -1) == 7); // nothing known: every core but one, as the baker
    CHECK(phoneBakeJobs(thermal, 0.3f, 0) == 7);
    CHECK(phoneBakeJobs(thermal, 0.75f, 1) == 3);
    CHECK(phoneBakeJobs(thermal, 0.92f, 2) == 2);
    CHECK(phoneBakeJobs(thermal, 1.05f, 2) == 1);
    CHECK(phoneBakeJobs(thermal, 0.3f, 3) == 1);    // severe
    CHECK(phoneBakeJobs(thermal, unknown, 4) == 0); // critical: wait
    CHECK(phoneBakeThermal(1).maxJobs == 1);
    CHECK(phoneBakeJobs(phoneBakeThermal(2), 0.8f, 0) == 1);

    // A restartable step starts again with fewer threads at half or fewer, not before 3 minutes; when too hot
    // to go on, at once.
    CHECK_FALSE(phoneBakeRestart(7, 4, 600.0));
    CHECK(phoneBakeRestart(7, 3, 600.0));
    CHECK_FALSE(phoneBakeRestart(7, 3, 60.0));
    CHECK(phoneBakeRestart(7, 0, 1.0));
}

TEST_CASE("Phone bake: status.json, a service that is gone, where it goes on") {
    PhoneBakeRun run;
    run.plan = "plan-1";
    run.state = PhoneBakeState::Running;
    run.finished = 3;
    run.step = 3;
    run.planStartedMs = 1'000;
    run.stepStartedMs = 5'000;
    run.updatedMs = 9'000;
    run.fraction = 0.25f;
    run.line = "[INFO] textures: 25 / 100 (3 s)";
    run.jobs = 4;
    run.headroom = 0.8f;
    run.thermalStatus = 1;
    run.seconds = {11.0, 1800.5, 79.0};
    Result<PhoneBakeRun> read = readPhoneBakeRun(writePhoneBakeRun(run));
    REQUIRE(read.ok());
    CHECK(read.value().plan == "plan-1");
    CHECK(read.value().state == PhoneBakeState::Running);
    CHECK(read.value().finished == 3);
    CHECK(read.value().stepStartedMs == 5'000);
    CHECK(read.value().fraction == doctest::Approx(0.25f));
    CHECK(read.value().line == run.line);
    CHECK(read.value().jobs == 4);
    CHECK(read.value().seconds.size() == 3);
    CHECK(read.value().seconds[1] == doctest::Approx(1800.5));
    for (const PhoneBakeState state :
         {PhoneBakeState::Running, PhoneBakeState::Cooling, PhoneBakeState::Done, PhoneBakeState::Failed,
          PhoneBakeState::Cancelled, PhoneBakeState::Interrupted}) {
        CHECK(phoneBakeStateFrom(phoneBakeStateName(state)) == state);
    }

    // Not written for 30 s while running: Android stopped the service.
    CHECK_FALSE(phoneBakeStale(run, 9'000 + kPhoneBakeStaleMs));
    CHECK(phoneBakeStale(run, 9'001 + kPhoneBakeStaleMs));
    run.state = PhoneBakeState::Cancelled;
    CHECK_FALSE(phoneBakeStale(run, 1'000'000));

    const std::filesystem::path root = tempDir("resume");
    const std::vector<PhoneBakeJob> jobs = firstBakeJobs();
    const PhoneBakePlan plan = planOf(jobs, root);
    CHECK(phoneBakeResumeStep(plan, run) == 3);
    run.plan = "another plan";
    CHECK(phoneBakeResumeStep(plan, run) == 0);
}

TEST_CASE("Phone bake: the baker's lines") {
    CHECK(phoneBakeLineFraction("[INFO] textures: 1200 / 58502 (40 s)", "textures") ==
          doctest::Approx(1200.0 / 58502.0));
    CHECK(phoneBakeLineFraction("textures: 58502 / 58502 (797 s)", "textures") == doctest::Approx(1.0));
    CHECK(phoneBakeLineFraction("[INFO] textures: 1200 / 58502 (40 s)", "models") < 0.0f);
    CHECK(phoneBakeLineFraction("[INFO] textures: 58502 baked (astc4x4 1), 0 failed | 1 MB -> x",
                                "textures") < 0.0f);
    CHECK(phoneBakeLineFraction("[INFO] Client in enUS: /storage/emulated/0/x / y", "textures") < 0.0f);
    CHECK(phoneBakeLineFraction("[INFO] textures: 12 / 0 (1 s)", "textures") < 0.0f);
    CHECK(phoneBakeLineFraction("[INFO] textures: a / 10", "textures") < 0.0f);
    CHECK(phoneBakeLineFraction("[INFO] textures esES: 10 / 20 (1 s)", "textures") < 0.0f);
    CHECK(phoneBakeErrorText("[ERROR] FAILED interface/x.blp: bad") == "FAILED interface/x.blp: bad");
    CHECK(phoneBakeErrorText("[INFO] fine").empty());
}

TEST_CASE("Phone bake: the progress and the time left") {
    const std::filesystem::path root = tempDir("progress");
    const std::vector<PhoneBakeJob> jobs = {{"db", {}}, {"textures", {}}, {"models", {}}};
    const PhoneBakePlan plan = planOf(jobs, root);
    const f64 total = phoneBakeSeconds(plan); // 11 + 1835 + 79
    PhoneBakeRun run;
    run.plan = plan.id;
    run.planStartedMs = 1'000;

    // Nothing yet.
    PhoneBakeProgress p = phoneBakeProgress(plan, run, 0);
    CHECK(p.fraction == doctest::Approx(0.0));
    CHECK(p.secondsLeft == doctest::Approx(total));

    // db took twice what it took on the Mi 11X, but it is too short to judge this phone by.
    run.state = PhoneBakeState::Running;
    run.finished = 1;
    run.step = 1;
    run.seconds = {22.0};
    run.stepStartedMs = 22'000;
    run.updatedMs = 22'000;
    p = phoneBakeProgress(plan, run, 22'000);
    CHECK(p.fraction == doctest::Approx(11.0 / total));
    CHECK(p.secondsLeft == doctest::Approx(1835.0 + 79.0).epsilon(0.01));

    // Textures counts its work: a quarter in 600 s -> 1800 s more for it.
    run.fraction = 0.25f;
    p = phoneBakeProgress(plan, run, 622'000);
    CHECK(p.stepFraction == doctest::Approx(0.25));
    CHECK(p.fraction == doctest::Approx((11.0 + 1835.0 * 0.25) / total));
    CHECK(p.secondsLeft == doctest::Approx(1800.0 + 79.0).epsilon(0.01));
    CHECK(p.elapsedSeconds == doctest::Approx(621.0));

    // Without counts (models), by the time it should take, never past 95 %.
    run.finished = 2;
    run.step = 2;
    run.seconds = {22.0, 3670.0}; // this phone is twice as slow
    run.fraction = -1.0f;
    run.stepStartedMs = 4'000'000;
    p = phoneBakeProgress(plan, run, 4'000'000 + 79'000);
    CHECK(p.stepFraction == doctest::Approx(0.5).epsilon(0.02)); // 79 s of the 158 it should take here
    CHECK(p.secondsLeft == doctest::Approx(79.0 * 2.0 - 79.0).epsilon(0.02));
    p = phoneBakeProgress(plan, run, 4'000'000 + 1'000'000);
    CHECK(p.stepFraction == doctest::Approx(0.95));

    run.state = PhoneBakeState::Done;
    run.finished = 3;
    p = phoneBakeProgress(plan, run, 9'000'000);
    CHECK(p.fraction == doctest::Approx(1.0));
    CHECK(p.secondsLeft == doctest::Approx(0.0));

    // Stopped after two steps: what is left, at this phone's pace.
    run.state = PhoneBakeState::Cancelled;
    run.finished = 2;
    p = phoneBakeProgress(plan, run, 9'000'000);
    CHECK(p.fraction == doctest::Approx((11.0 + 1835.0) / total));
    CHECK(p.secondsLeft == doctest::Approx(79.0 * 2.0).epsilon(0.02));
}

TEST_CASE("Phone bake: the runner, from the first step to the last") {
    const std::filesystem::path root = tempDir("runner");
    const std::vector<PhoneBakeJob> jobs = {{"db", {}}, {"textures", {}}, {"models", {}}};
    writePlan(root, planOf(jobs, root));
    FakeBaker baker;
    FakeClock clock;
    // What a step cut short left in the staging folder is gone before the next run; its .cache stays.
    writeFile(places(root).stagingDir() / "half.vdpk", "half");
    writeFile(places(root).stagingDir() / ".cache" / "textures" / "0001.bin", "cached");
    CHECK(runPhoneBakePlan(places(root), hooksFor(baker, clock)) == PhoneBakeState::Done);
    CHECK_FALSE(std::filesystem::exists(places(root).stagingDir() / "half.vdpk"));
    CHECK(std::filesystem::exists(places(root).stagingDir() / ".cache" / "textures" / "0001.bin"));
    REQUIRE(baker.calls.size() == 6); // bake and install of each step
    CHECK(bakedSteps(baker) == std::vector<std::string>{"db", "textures", "models"});
    CHECK(after(baker.calls[0], "--jobs") == "7"); // nothing known of the heat: every core but one
    CHECK(baker.calls[1][0] == "install");
    const PhoneBakeRun run = statusOf(root);
    CHECK(run.state == PhoneBakeState::Done);
    CHECK(run.finished == 3);
    CHECK(run.seconds.size() == 3);
    CHECK(run.seconds[0] == doctest::Approx(180.0)); // three ticks of a minute
    // Every line of the baker in bake.log.
    const std::string log = readFile(places(root).logFile());
    CHECK(log.find("textures: 30 / 100") != std::string::npos);
    CHECK(log.find("=== done") != std::string::npos);
    // Done is done: run again, nothing runs.
    CHECK(runPhoneBakePlan(places(root), hooksFor(baker, clock)) == PhoneBakeState::Done);
    CHECK(baker.calls.size() == 6);
}

TEST_CASE("Phone bake: the runner after a failure, a killed service and a cancel") {
    const std::filesystem::path root = tempDir("runner2");
    const std::vector<PhoneBakeJob> jobs = {{"db", {}}, {"textures", {}}, {"models", {}}, {"ui", {}}};
    writePlan(root, planOf(jobs, root));
    FakeBaker baker;
    FakeClock clock;

    // textures fails: db stays done, the error line is kept.
    baker.failStep = "textures";
    CHECK(runPhoneBakePlan(places(root), hooksFor(baker, clock)) == PhoneBakeState::Failed);
    PhoneBakeRun run = statusOf(root);
    CHECK(run.state == PhoneBakeState::Failed);
    CHECK(run.finished == 1);
    CHECK(run.step == 1);
    CHECK(run.exitCode == 1);
    CHECK(run.error == "textures: cannot read something.blp");

    // Try again: from textures, not from db. The player stops it during models.
    baker.failStep.clear();
    baker.calls.clear();
    std::atomic<bool> cancel{false};
    baker.during = [&](const std::vector<std::string>& args) {
        if (after(args, "--only") == "models") {
            cancel = true;
        }
    };
    CHECK(runPhoneBakePlan(places(root), hooksFor(baker, clock, &cancel)) == PhoneBakeState::Cancelled);
    CHECK(bakedSteps(baker) == std::vector<std::string>{"textures", "models"});
    run = statusOf(root);
    CHECK(run.state == PhoneBakeState::Cancelled);
    CHECK(run.finished == 2);
    CHECK(run.seconds.size() == 2);

    // Resumed, then killed by Android during models: what it leaves is a status that says "running" and is
    // never written again.
    run.state = PhoneBakeState::Running;
    run.step = 2;
    run.updatedMs = clock.ms - 120'000;
    writeFile(places(root).statusFile(), writePhoneBakeRun(run));
    CHECK(phoneBakeStale(run, clock.ms));
    // Started again (Android restarts the service, or the player resumes): from models.
    cancel = false;
    baker.during = nullptr;
    baker.calls.clear();
    CHECK(runPhoneBakePlan(places(root), hooksFor(baker, clock, &cancel)) == PhoneBakeState::Done);
    CHECK(bakedSteps(baker) == std::vector<std::string>{"models", "ui"});
    run = statusOf(root);
    CHECK(run.finished == 4);
    CHECK(run.seconds.size() == 4);
    // A new plan (another id) starts from the beginning.
    PhoneBakePlan other = planOf(jobs, root);
    other.id = "plan-2";
    writePlan(root, other);
    baker.calls.clear();
    CHECK(runPhoneBakePlan(places(root), hooksFor(baker, clock)) == PhoneBakeState::Done);
    CHECK(bakedSteps(baker).size() == 4);
    CHECK(runPhoneBakePlan(places(tempDir("noplan")), hooksFor(baker, clock)) == PhoneBakeState::None);
}

TEST_CASE("Phone bake: the runner when the phone heats") {
    const std::filesystem::path root = tempDir("runner3");
    const std::vector<PhoneBakeJob> jobs = {{"db", {}}, {"textures", {}}};
    writePlan(root, planOf(jobs, root));
    FakeBaker baker;
    baker.ticksPerBake = 12;
    FakeClock clock;
    PhoneBakeRunnerHooks hooks = hooksFor(baker, clock);
    // Critical when it starts (it waits), cool for db, then hot from the fourth minute of textures on.
    bool critical = true;
    u32 texturesTicks = 0;
    baker.during = [&](const std::vector<std::string>& args) {
        if (after(args, "--only") == "textures" && after(args, "--jobs") == "7") {
            ++texturesTicks;
        }
    };
    hooks.thermal = [&](f32& headroom, i32& status) {
        status = critical ? 4 : 1;
        critical = false;
        headroom = texturesTicks >= 4 ? 0.95f : 0.3f;
    };
    CHECK(runPhoneBakePlan(places(root), hooks) == PhoneBakeState::Done);
    const std::vector<std::string> steps = bakedSteps(baker);
    REQUIRE(steps.size() == 3); // db, textures, textures again with fewer threads
    CHECK(steps[1] == "textures");
    CHECK(steps[2] == "textures");
    std::vector<std::string> threads;
    for (const std::vector<std::string>& call : baker.calls) {
        if (call[0] == "bake") {
            threads.push_back(after(call, "--jobs"));
        }
    }
    CHECK(threads == std::vector<std::string>{"7", "7", "2"});
    const std::string log = readFile(places(root).logFile());
    CHECK(log.find("too hot") != std::string::npos);
    CHECK(log.find("again with 2 threads") != std::string::npos);
    // A step without a resume cache is never stopped for the heat.
    const std::filesystem::path other = tempDir("runner4");
    const std::vector<PhoneBakeJob> models = {{"models", {}}};
    writePlan(other, planOf(models, other));
    FakeBaker hot;
    hot.ticksPerBake = 12;
    PhoneBakeRunnerHooks hotHooks = hooksFor(hot, clock);
    hotHooks.thermal = [](f32& headroom, i32& status) {
        headroom = 0.99f;
        status = 2;
    };
    CHECK(runPhoneBakePlan(places(other), hotHooks) == PhoneBakeState::Done);
    CHECK(bakedSteps(hot).size() == 1);
}

TEST_CASE("Phone bake: a step's packs into packs/") {
    const std::filesystem::path root = tempDir("install");
    const std::filesystem::path staging = root / "staging";
    const std::filesystem::path packs = root / "packs";
    // packs/ had an older db.vdpk and models.vdpk; the step baked db.vdpk again.
    writeFile(packs / "db.vdpk", "old db");
    writeFile(packs / "models.vdpk", "models");
    pack::PackManifest before;
    before.bakedBy = "0.0.9";
    before.clientBuild = 5875;
    before.clientLocale = "enUS";
    before.record("db.vdpk").recipe = "old";
    before.record("db.vdpk").size = 6;
    before.record("models.vdpk").recipe = "m";
    before.record("models.vdpk").size = 6;
    REQUIRE(before.save(packs).ok());

    // Nothing baked in the staging folder: an error, nothing moves.
    CHECK_FALSE(pack::installStagedPacks(staging, packs).ok());

    writeFile(staging / "db.vdpk", "new db!");
    writeFile(staging / ".cache" / "textures" / "a.bin", "cache");
    pack::PackManifest bake;
    bake.bakedBy = "0.2.0";
    bake.clientBuild = 5875;
    bake.clientLocale = "enUS";
    bake.record("db.vdpk").recipe = "new";
    bake.record("db.vdpk").size = 7;
    bake.record("sound.vdpk").recipe = "s";
    bake.record("sound.vdpk").size = 3;
    REQUIRE(bake.save(staging).ok());
    // sound.vdpk is in its manifest but not there: nothing moves.
    CHECK_FALSE(pack::installStagedPacks(staging, packs).ok());
    CHECK(readFile(packs / "db.vdpk") == "old db");

    bake.remove("sound.vdpk");
    REQUIRE(bake.save(staging).ok());
    Result<pack::InstallResult> installed = pack::installStagedPacks(staging, packs);
    REQUIRE(installed.ok());
    CHECK(installed.value().packs == 1);
    CHECK(installed.value().bytes == 7);
    CHECK(readFile(packs / "db.vdpk") == "new db!");
    CHECK_FALSE(std::filesystem::exists(staging / "db.vdpk"));
    CHECK_FALSE(std::filesystem::exists(staging / "manifest.json"));
    CHECK_FALSE(std::filesystem::exists(staging / ".cache")); // textures' resume cache, no longer needed
    Result<pack::PackManifest> after = pack::PackManifest::load(packs);
    REQUIRE(after.ok());
    CHECK(after.value().bakedBy == "0.2.0");
    REQUIRE(after.value().find("db.vdpk"));
    CHECK(after.value().find("db.vdpk")->recipe == "new");
    CHECK(after.value().find("db.vdpk")->size == 7);
    REQUIRE(after.value().find("models.vdpk"));
    CHECK(after.value().find("models.vdpk")->recipe == "m"); // the others stay as they were
}

TEST_CASE("Phone bake: the WoW copied to the phone") {
    // The header against the file.
    const std::string whole = mpqBytes(1024, 1024);
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(whole.data()), whole.size());
    CHECK(pack::inspectMpqHeader(bytes, 1024) == pack::MpqState::Ok);
    CHECK(pack::inspectMpqHeader(bytes, 900) == pack::MpqState::Truncated); // a copy cut short
    const std::string zeros(1024, '\0');
    CHECK(pack::inspectMpqHeader(std::span(reinterpret_cast<const u8*>(zeros.data()), zeros.size()), 1024) ==
          pack::MpqState::NotMpq);
    // After a user-data block ("MPQ\x1B", the header 512 bytes in).
    std::string userData(512, '\0');
    userData[0] = 'M';
    userData[1] = 'P';
    userData[2] = 'Q';
    userData[3] = 0x1B;
    userData[8] = 0x00;
    userData[9] = 0x02; // 512
    userData += mpqBytes(1024, 1024);
    CHECK(pack::inspectMpqHeader(std::span(reinterpret_cast<const u8*>(userData.data()), userData.size()),
                                 userData.size()) == pack::MpqState::Ok);

    const std::filesystem::path root = tempDir("wow");
    CHECK(pack::findWowDataDir(root / "wow").empty());
    const std::filesystem::path data = root / "wow" / "DATA"; // any case
    for (const std::string_view name : pack::wowGameArchives()) {
        writeFile(data / std::string(name), mpqBytes(1024, 1024));
    }
    writeFile(data / "base.MPQ", "launcher only"); // official, never read: not checked
    writeFile(data / "patch-3.MPQ", mpqBytes(1024, 1024));
    REQUIRE(pack::findWowDataDir(root / "wow") == data);
    pack::WowDataCheck check = pack::checkWowData(data);
    CHECK(check.complete());
    CHECK(check.archives == 14);
    CHECK(check.gameBytes == 12 * 1024);
    CHECK(check.unofficial == std::vector<std::string>{"patch-3.MPQ"});

    std::filesystem::remove(data / "speech.MPQ");
    writeFile(data / "texture.MPQ", mpqBytes(1024, 8192));
    check = pack::checkWowData(data);
    CHECK_FALSE(check.complete());
    CHECK(check.missing == std::vector<std::string>{"speech.MPQ"});
    CHECK(check.damaged == std::vector<std::string>{"texture.MPQ"});
    CHECK_FALSE(pack::checkWowData(root / "nothing").complete());
}

TEST_CASE("Phone bake: the first-run screen until a first bake ends") {
    const std::filesystem::path root = tempDir("firstrun");
    const std::filesystem::path packs = root / "packs";
    for (const std::string_view pack : kRequiredPacks) {
        writeFile(packs / std::string(pack), "x");
    }
    writeFile(packs / "world_azeroth.vdpk", "x");
    CHECK_FALSE(needsFirstRun(packs, root / "internal"));
    CHECK_FALSE(phoneFirstBakeUnfinished(root / "bake"));

    // A first bake on the phone: its first packs are there, the rest is coming.
    const std::vector<PhoneBakeJob> jobs = firstBakeJobs();
    PhoneBakePlan plan = planOf(jobs, root);
    writePlan(root, plan);
    CHECK(phoneFirstBakeUnfinished(root / "bake"));
    CHECK(needsFirstRun(packs, root / "internal"));
    PhoneBakeRun run;
    run.plan = plan.id;
    run.state = PhoneBakeState::Running;
    run.finished = 7;
    writeFile(places(root).statusFile(), writePhoneBakeRun(run));
    CHECK(needsFirstRun(packs, root / "internal"));
    run.state = PhoneBakeState::Done;
    writeFile(places(root).statusFile(), writePhoneBakeRun(run));
    CHECK_FALSE(needsFirstRun(packs, root / "internal"));
    // An update baking: the game has its data.
    plan.kind = PhoneBakeKind::Update;
    plan.id = "update";
    writePlan(root, plan);
    CHECK_FALSE(phoneFirstBakeUnfinished(root / "bake"));
}

namespace {

// The platform's side for the tests: it "runs" the plan with the runner and a fake baker on a thread, as the
// service would.
class FakeLauncher final : public BakeLauncher {
public:
    std::atomic<u32> starts{0};
    std::atomic<u32> cancels{0};
    std::atomic<bool> stopRequested{false};
    bool refuse = false;

    ~FakeLauncher() override {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
    bool start(const std::filesystem::path& bakeDir) override {
        ++starts;
        if (refuse) {
            return false;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        stopRequested = false;
        m_thread = std::thread([this, bakeDir] {
            PhoneBakeRunnerHooks hooks;
            hooks.runBaker = [this](const std::vector<std::string>& args,
                                    const std::function<void(std::string_view)>& onLine,
                                    const std::function<bool()>& tick) {
                return m_baker.run(args, onLine, [&] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // a baker takes its time
                    return tick();
                });
            };
            hooks.cancel = &stopRequested;
            runPhoneBakePlan(PhoneBakePlaces{{}, {}, bakeDir}, hooks);
        });
        return true;
    }
    void cancel(const std::filesystem::path&) override {
        ++cancels;
        stopRequested = true;
    }

private:
    FakeBaker m_baker;
    std::thread m_thread;
};

template <typename F> bool waitFor(PhoneBake& bake, F&& done) {
    for (int i = 0; i < 400; ++i) {
        if (done(bake.status())) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

} // namespace

TEST_CASE("Phone bake: the game's side, with a fake service") {
    const std::filesystem::path root = tempDir("game");
    const std::filesystem::path data = root / "wow" / "Data";
    for (const std::string_view name : pack::wowGameArchives()) {
        writeFile(data / std::string(name), mpqBytes(1024, 1024));
    }
    writeFile(data / "patch-A.MPQ", mpqBytes(1024, 1024));
    PhoneBake bake;
    PhoneBake::Config config;
    config.places = places(root);
    auto launcher = std::make_unique<FakeLauncher>();
    FakeLauncher* fake = launcher.get();
    config.launcher = std::move(launcher);
    config.cores = 4;
    bake.init(std::move(config));
    REQUIRE(waitFor(bake, [](const PhoneBakeStatus& s) { return s.checked; }));
    PhoneBakeStatus s = bake.status();
    CHECK(s.wowComplete);
    CHECK(s.state == PhoneBakeState::None);
    CHECK(s.firstBakeBytes > 4'700'000'000ull);
    CHECK(s.firstBakeSeconds == doctest::Approx(2512.0).epsilon(0.01));

    const std::vector<PhoneBakeJob> jobs = {{"db", {}}, {"cinematics", {}}};
    REQUIRE(bake.start(jobs, PhoneBakeKind::First));
    CHECK(bake.status().active());
    CHECK_FALSE(bake.start(jobs, PhoneBakeKind::First)); // one at a time
    REQUIRE(waitFor(bake, [](const PhoneBakeStatus& s) { return s.state == PhoneBakeState::Done; }));
    CHECK(fake->starts == 1);
    s = bake.status();
    CHECK(s.finished == 2);
    CHECK(s.stepCount == 2);
    CHECK(s.fraction == doctest::Approx(1.0));
    CHECK(s.maxJobs == 3);
    // The plan it wrote: the unofficial archive left out, the steps in order.
    Result<PhoneBakePlan> plan = readPhoneBakePlan(readFile(places(root).planFile()));
    REQUIRE(plan.ok());
    CHECK(plan.value().kind == PhoneBakeKind::First);
    REQUIRE(plan.value().steps.size() == 2);
    CHECK(after(plan.value().steps[0].bake, "--skip-archives") == "patch-A.MPQ");
    CHECK_FALSE(plan.value().texts.title.empty()); // a text (or its key, without a table)

    // Discarded: the plan, its status and the staging folder go.
    bake.discard();
    REQUIRE(waitFor(bake, [](const PhoneBakeStatus& s) { return s.state == PhoneBakeState::None; }));
    CHECK_FALSE(std::filesystem::exists(places(root).planFile()));
    CHECK_FALSE(std::filesystem::exists(places(root).statusFile()));

    // A platform that cannot start it: stopped, to try again.
    fake->refuse = true;
    REQUIRE(bake.start(jobs, PhoneBakeKind::First));
    REQUIRE(waitFor(bake, [](const PhoneBakeStatus& s) { return !s.starting && s.launchFailed; }));
    s = bake.status();
    CHECK(s.state == PhoneBakeState::Interrupted);
    CHECK(s.resumable());
    fake->refuse = false;
    REQUIRE(bake.resume());
    REQUIRE(waitFor(bake, [](const PhoneBakeStatus& s) { return s.state == PhoneBakeState::Done; }));
    CHECK_FALSE(bake.status().launchFailed);
    bake.shutdown();
}
