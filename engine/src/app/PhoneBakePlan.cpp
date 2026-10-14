#include "app/PhoneBakePlan.h"

#include "core/Json.h"
#include "pack/PackManifest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>

namespace vd {
namespace {

constexpr i64 kFormat = 1;

// The steps of a first bake, in their order, with what they cost: seconds on a Mi 11X (vd_baker for Android, 7 threads,
// no cache; world and collision measured on Azeroth alone, 83 s and 36 s, and scaled to every map by the size of their
// packs) and the bytes of the packs they write from an enUS client, every map. The prefix: the packs of the step that
// are one per map.
struct StepReference {
    std::string_view name;
    f64 seconds;
    u64 bytes;
    std::string_view mapPrefix;
};
constexpr std::array<StepReference, 13> kSteps = {{
    {"db", 11, 28'084'422, {}},
    {"textures", 1835, 1'318'239'516, {}},
    {"models", 79, 408'350'584, {}},
    {"actors", 13, 120'150'643, {}},
    {"world", 260, 1'776'129'257, "world_"}, // 42 maps and liquids.vdpk
    {"collision", 106, 515'728'738, "collision_"},
    {"objects", 9, 8'771'758, {}},
    {"ui", 15, 13'980'970, {}},
    {"sound", 64, 509'301'705, {}},
    {"cinematics", 4, 28'028, {}},
    {"grass", 32, 15'973'361, {}},
    {"worldsound", 79, 5'291'873, {}},
    {"bodyfx", 5, 267'502, {}},
}};

const StepReference* reference(std::string_view step) {
    for (const StepReference& r : kSteps) {
        if (r.name == step) {
            return &r;
        }
    }
    return nullptr;
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string joined(std::span<const std::string> items, char separator) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out.push_back(separator);
        }
        out += item;
    }
    return out;
}

json::Value stringArray(std::span<const std::string> items) {
    json::Value array = json::Value::array();
    for (const std::string& item : items) {
        array.push(json::Value::string(item));
    }
    return array;
}

std::vector<std::string> readStrings(const json::Value& array) {
    std::vector<std::string> items;
    for (const json::Value& item : array.items()) {
        items.emplace_back(item.asString());
    }
    return items;
}

std::string_view kindName(PhoneBakeKind kind) {
    return kind == PhoneBakeKind::Update ? "update" : "first";
}

Result<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Error{"Cannot open " + utf8(path)};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool parseCount(std::string_view text, u64& value) {
    if (text.empty()) {
        return false;
    }
    value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<u64>(c - '0');
    }
    return true;
}

} // namespace

std::vector<PhoneBakeJob> firstBakeJobs() {
    std::vector<PhoneBakeJob> jobs;
    for (const StepReference& step : kSteps) {
        jobs.push_back({std::string(step.name), {}});
    }
    return jobs;
}

std::vector<PhoneBakeJob> jobsForPacks(std::span<const std::string> packs) {
    std::vector<PhoneBakeJob> jobs;
    for (const StepReference& ref : kSteps) {
        PhoneBakeJob job;
        job.step = std::string(ref.name);
        bool wanted = false;
        bool whole = false;
        for (const std::string& pack : packs) {
            const pack::BakeStep step = pack::bakeStepOf(pack);
            if (step.step != ref.name) {
                continue;
            }
            wanted = true;
            if (step.map.empty()) {
                whole = true;
            } else if (std::find(job.maps.begin(), job.maps.end(), step.map) == job.maps.end()) {
                job.maps.push_back(step.map);
            }
        }
        if (!wanted) {
            continue;
        }
        if (whole) {
            job.maps.clear();
        }
        std::sort(job.maps.begin(), job.maps.end());
        jobs.push_back(std::move(job));
    }
    return jobs;
}

bool isPhoneBakeStep(std::string_view step) {
    return reference(step) != nullptr;
}

std::map<std::string, u64> listClientPacks(const std::filesystem::path& packsDir) {
    std::map<std::string, u64> packs;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(packsDir, error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(error) && pack::isClientPack(name)) {
            packs[name] = entry.file_size(error);
        }
    }
    return packs;
}

PhoneBakeCost phoneBakeCost(const PhoneBakeJob& job, const std::map<std::string, u64>& existing) {
    PhoneBakeCost cost;
    const StepReference* ref = reference(job.step);
    if (!ref) {
        cost.seconds = 10.0;
        return cost;
    }
    if (job.maps.empty()) {
        for (const auto& [file, bytes] : existing) {
            if (pack::bakeStepOf(file).step == job.step) {
                cost.replaces += bytes;
            }
        }
        cost.bytes = cost.replaces > 0 ? cost.replaces : ref->bytes;
    } else {
        u64 unknownMaps = 0;
        for (const std::string& map : job.maps) {
            const auto it = existing.find(std::string(ref->mapPrefix) + map + ".vdpk");
            if (it != existing.end()) {
                cost.replaces += it->second;
            } else {
                ++unknownMaps;
            }
        }
        // A map never baked here: a quarter of the step (Azeroth and Kalimdor are most of it).
        cost.bytes = cost.replaces + std::min(ref->bytes, unknownMaps * (ref->bytes / 4));
    }
    cost.seconds = ref->bytes > 0 ? ref->seconds * static_cast<f64>(cost.bytes) / static_cast<f64>(ref->bytes)
                                  : ref->seconds;
    cost.seconds = std::max(cost.seconds, 1.0);
    return cost;
}

PhoneBakeThermal phoneBakeThermal(u32 cores) {
    PhoneBakeThermal thermal;
    thermal.maxJobs = std::max<i32>(1, static_cast<i32>(cores) - 1);
    thermal.headroom = {{0.0f, thermal.maxJobs},
                        {0.75f, std::max<i32>(1, thermal.maxJobs / 2)},
                        {0.9f, std::min<i32>(2, thermal.maxJobs)},
                        {1.0f, 1}};
    thermal.status = {{3, 1}, {4, 0}}; // severe: one thread; critical: wait
    return thermal;
}

i32 phoneBakeJobs(const PhoneBakeThermal& thermal, f32 headroom, i32 status) {
    i32 jobs = thermal.maxJobs;
    if (!std::isnan(headroom) && headroom >= 0.0f) {
        for (const auto& [from, threads] : thermal.headroom) {
            if (headroom >= from) {
                jobs = std::min(thermal.maxJobs, threads);
            }
        }
    }
    if (status >= 0) {
        for (const auto& [from, threads] : thermal.status) {
            if (status >= from) {
                jobs = std::min(jobs, threads);
            }
        }
    }
    return std::max(0, jobs);
}

bool phoneBakeRestart(i32 runningJobs, i32 wantedJobs, f64 secondsSinceStart) {
    if (wantedJobs <= 0) {
        return true; // too hot to go on: wait
    }
    return wantedJobs * 2 <= runningJobs && secondsSinceStart >= kPhoneBakeRestartSeconds;
}

PhoneBakePlan makePhoneBakePlan(std::span<const PhoneBakeJob> jobs, const PhoneBakePlanInputs& inputs) {
    PhoneBakePlan plan;
    plan.id = inputs.id;
    plan.kind = inputs.kind;
    plan.createdMs = inputs.nowMs;
    const PhoneBakePlaces& places = inputs.places;
    plan.packsDir = utf8(places.packsDir);
    plan.stagingDir = utf8(places.stagingDir());
    plan.statusFile = utf8(places.statusFile());
    plan.logFile = utf8(places.logFile());
    plan.texts = inputs.texts;
    plan.thermal = phoneBakeThermal(inputs.cores);
    const u32 count = static_cast<u32>(jobs.size());
    for (u32 i = 0; i < count; ++i) {
        const PhoneBakeJob& job = jobs[i];
        PhoneBakePlanStep step;
        step.name = job.step;
        step.maps = job.maps;
        step.label = inputs.label ? inputs.label(job.step, i, count) : job.step;
        const PhoneBakeCost cost = phoneBakeCost(job, inputs.existing);
        step.seconds = cost.seconds;
        step.bytes = cost.bytes;
        step.replaces = cost.replaces;
        // Textures (half the bake) keeps what it baked in the staging folder's .cache: a step stopped
        // (Android, the heat, the player) goes on from the last texture. `vd_baker install` deletes it once
        // the step is done.
        step.restartable = job.step == "textures";
        step.bake = {"bake", "--wow-dir", utf8(places.wowDir), "--out", plan.stagingDir, "--only", job.step};
        if (!job.maps.empty()) {
            step.bake.insert(step.bake.end(), {"--maps", joined(job.maps, ',')});
        }
        // ";" is an empty list: no client in another language (the phone has one; without it the baker looks
        // in its built-in folders).
        step.bake.insert(step.bake.end(), {"--locale-clients", ";"});
        step.bake.insert(step.bake.end(), {"--cache", step.restartable ? utf8(places.stagingDir() / ".cache")
                                                                       : std::string("off")});
        if (!inputs.skipArchives.empty()) {
            step.bake.insert(step.bake.end(), {"--skip-archives", joined(inputs.skipArchives, ';')});
        }
        step.install = {"install", "--staging", plan.stagingDir, "--packs", plan.packsDir};
        plan.steps.push_back(std::move(step));
    }
    return plan;
}

std::string writePhoneBakePlan(const PhoneBakePlan& plan) {
    json::Value root = json::Value::object();
    root.set("format", json::Value::number(kFormat));
    root.set("id", json::Value::string(plan.id));
    root.set("kind", json::Value::string(std::string(kindName(plan.kind))));
    root.set("createdMs", json::Value::number(static_cast<f64>(plan.createdMs)));
    root.set("packs", json::Value::string(plan.packsDir));
    root.set("staging", json::Value::string(plan.stagingDir));
    root.set("status", json::Value::string(plan.statusFile));
    root.set("log", json::Value::string(plan.logFile));
    json::Value& texts = root.set("texts", json::Value::object());
    texts.set("channel", json::Value::string(plan.texts.channel));
    texts.set("title", json::Value::string(plan.texts.title));
    texts.set("cancel", json::Value::string(plan.texts.cancel));
    texts.set("cooling", json::Value::string(plan.texts.cooling));
    texts.set("done", json::Value::string(plan.texts.done));
    texts.set("failed", json::Value::string(plan.texts.failed));
    texts.set("stopped", json::Value::string(plan.texts.stopped));
    json::Value& thermal = root.set("thermal", json::Value::object());
    thermal.set("maxJobs", json::Value::number(plan.thermal.maxJobs));
    json::Value& headroom = thermal.set("headroom", json::Value::array());
    for (const auto& [from, jobs] : plan.thermal.headroom) {
        json::Value& row = headroom.push(json::Value::array());
        row.push(json::Value::number(from));
        row.push(json::Value::number(jobs));
    }
    json::Value& status = thermal.set("status", json::Value::array());
    for (const auto& [from, jobs] : plan.thermal.status) {
        json::Value& row = status.push(json::Value::array());
        row.push(json::Value::number(from));
        row.push(json::Value::number(jobs));
    }
    json::Value& steps = root.set("steps", json::Value::array());
    for (const PhoneBakePlanStep& step : plan.steps) {
        json::Value& value = steps.push(json::Value::object());
        value.set("name", json::Value::string(step.name));
        value.set("maps", stringArray(step.maps));
        value.set("label", json::Value::string(step.label));
        value.set("seconds", json::Value::number(std::round(step.seconds * 10.0) / 10.0));
        value.set("bytes", json::Value::number(static_cast<f64>(step.bytes)));
        value.set("replaces", json::Value::number(static_cast<f64>(step.replaces)));
        value.set("restartable", json::Value::boolean(step.restartable));
        value.set("bake", stringArray(step.bake));
        value.set("install", stringArray(step.install));
    }
    return json::write(root);
}

Result<PhoneBakePlan> readPhoneBakePlan(std::string_view text) {
    Result<json::Value> document = json::parse(text);
    if (!document) {
        return Error{"plan.json: " + document.error().message};
    }
    const json::Value& root = document.value();
    if (root.get("format").asInt() != kFormat) {
        return Error{"plan.json: unknown format"};
    }
    PhoneBakePlan plan;
    plan.id = root.get("id").asString();
    plan.kind = root.get("kind").asString() == "update" ? PhoneBakeKind::Update : PhoneBakeKind::First;
    plan.createdMs = root.get("createdMs").asInt();
    plan.packsDir = root.get("packs").asString();
    plan.stagingDir = root.get("staging").asString();
    plan.statusFile = root.get("status").asString();
    plan.logFile = root.get("log").asString();
    const json::Value& texts = root.get("texts");
    plan.texts.channel = texts.get("channel").asString();
    plan.texts.title = texts.get("title").asString();
    plan.texts.cancel = texts.get("cancel").asString();
    plan.texts.cooling = texts.get("cooling").asString();
    plan.texts.done = texts.get("done").asString();
    plan.texts.failed = texts.get("failed").asString();
    plan.texts.stopped = texts.get("stopped").asString();
    const json::Value& thermal = root.get("thermal");
    plan.thermal.maxJobs = static_cast<i32>(thermal.get("maxJobs").asInt(1));
    for (const json::Value& row : thermal.get("headroom").items()) {
        if (row.size() == 2) {
            plan.thermal.headroom.emplace_back(static_cast<f32>(row.items()[0].asNumber()),
                                               static_cast<i32>(row.items()[1].asInt()));
        }
    }
    for (const json::Value& row : thermal.get("status").items()) {
        if (row.size() == 2) {
            plan.thermal.status.emplace_back(static_cast<i32>(row.items()[0].asInt()),
                                             static_cast<i32>(row.items()[1].asInt()));
        }
    }
    for (const json::Value& value : root.get("steps").items()) {
        PhoneBakePlanStep step;
        step.name = value.get("name").asString();
        step.maps = readStrings(value.get("maps"));
        step.label = value.get("label").asString();
        step.seconds = value.get("seconds").asNumber();
        step.bytes = static_cast<u64>(value.get("bytes").asNumber());
        step.replaces = static_cast<u64>(value.get("replaces").asNumber());
        step.restartable = value.get("restartable").asBool();
        step.bake = readStrings(value.get("bake"));
        step.install = readStrings(value.get("install"));
        if (step.name.empty() || step.bake.empty() || step.install.empty()) {
            return Error{"plan.json: a step without its commands"};
        }
        plan.steps.push_back(std::move(step));
    }
    if (plan.id.empty() || plan.steps.empty()) {
        return Error{"plan.json: no steps"};
    }
    return plan;
}

u64 phoneBakeSpaceNeeded(const PhoneBakePlan& plan, u32 fromStep) {
    i64 installed = 0; // what the steps before have added to the folder
    i64 peak = 0;
    for (usize i = fromStep; i < plan.steps.size(); ++i) {
        const PhoneBakePlanStep& step = plan.steps[i];
        const i64 bytes = static_cast<i64>(step.bytes);
        // The step's packs in the staging folder (the ones they replace still in packs/), and its resume
        // cache.
        peak = std::max(peak, installed + bytes + (step.restartable ? bytes : 0));
        installed += bytes - static_cast<i64>(step.replaces);
    }
    return static_cast<u64>(std::max<i64>(peak, 0)) + kPhoneBakeSpareBytes;
}

f64 phoneBakeSeconds(const PhoneBakePlan& plan, u32 fromStep) {
    f64 seconds = 0.0;
    for (usize i = fromStep; i < plan.steps.size(); ++i) {
        seconds += plan.steps[i].seconds;
    }
    return seconds;
}

std::string_view phoneBakeStateName(PhoneBakeState state) {
    switch (state) {
    case PhoneBakeState::None: return "none";
    case PhoneBakeState::Running: return "running";
    case PhoneBakeState::Cooling: return "cooling";
    case PhoneBakeState::Done: return "done";
    case PhoneBakeState::Failed: return "failed";
    case PhoneBakeState::Cancelled: return "cancelled";
    case PhoneBakeState::Interrupted: return "interrupted";
    }
    return "none";
}

PhoneBakeState phoneBakeStateFrom(std::string_view name) {
    for (const PhoneBakeState state :
         {PhoneBakeState::Running, PhoneBakeState::Cooling, PhoneBakeState::Done, PhoneBakeState::Failed,
          PhoneBakeState::Cancelled, PhoneBakeState::Interrupted}) {
        if (phoneBakeStateName(state) == name) {
            return state;
        }
    }
    return PhoneBakeState::None;
}

std::string writePhoneBakeRun(const PhoneBakeRun& run) {
    json::Value root = json::Value::object();
    root.set("format", json::Value::number(kFormat));
    root.set("plan", json::Value::string(run.plan));
    root.set("state", json::Value::string(std::string(phoneBakeStateName(run.state))));
    root.set("finished", json::Value::number(run.finished));
    root.set("step", json::Value::number(run.step));
    root.set("planStartedMs", json::Value::number(static_cast<f64>(run.planStartedMs)));
    root.set("stepStartedMs", json::Value::number(static_cast<f64>(run.stepStartedMs)));
    root.set("updatedMs", json::Value::number(static_cast<f64>(run.updatedMs)));
    root.set("fraction", json::Value::number(std::round(static_cast<f64>(run.fraction) * 1e4) / 1e4));
    root.set("line", json::Value::string(run.line));
    root.set("error", json::Value::string(run.error));
    root.set("exit", json::Value::number(run.exitCode));
    root.set("jobs", json::Value::number(run.jobs));
    root.set("headroom", json::Value::number(std::round(static_cast<f64>(run.headroom) * 1e3) / 1e3));
    root.set("thermalStatus", json::Value::number(run.thermalStatus));
    json::Value& seconds = root.set("seconds", json::Value::array());
    for (const f64 s : run.seconds) {
        seconds.push(json::Value::number(std::round(s * 10.0) / 10.0));
    }
    return json::write(root, false);
}

Result<PhoneBakeRun> readPhoneBakeRun(std::string_view text) {
    Result<json::Value> document = json::parse(text);
    if (!document) {
        return Error{"status.json: " + document.error().message};
    }
    const json::Value& root = document.value();
    if (root.get("format").asInt() != kFormat) {
        return Error{"status.json: unknown format"};
    }
    PhoneBakeRun run;
    run.plan = root.get("plan").asString();
    run.state = phoneBakeStateFrom(root.get("state").asString());
    run.finished = static_cast<u32>(std::max<i64>(0, root.get("finished").asInt()));
    run.step = static_cast<u32>(std::max<i64>(0, root.get("step").asInt()));
    run.planStartedMs = root.get("planStartedMs").asInt();
    run.stepStartedMs = root.get("stepStartedMs").asInt();
    run.updatedMs = root.get("updatedMs").asInt();
    run.fraction = static_cast<f32>(root.get("fraction").asNumber(-1.0));
    run.line = root.get("line").asString();
    run.error = root.get("error").asString();
    run.exitCode = static_cast<i32>(root.get("exit").asInt());
    run.jobs = static_cast<i32>(root.get("jobs").asInt());
    run.headroom = static_cast<f32>(root.get("headroom").asNumber(-1.0));
    run.thermalStatus = static_cast<i32>(root.get("thermalStatus").asInt(-1));
    for (const json::Value& s : root.get("seconds").items()) {
        run.seconds.push_back(s.asNumber());
    }
    return run;
}

bool phoneBakeStale(const PhoneBakeRun& run, i64 nowMs) {
    return (run.state == PhoneBakeState::Running || run.state == PhoneBakeState::Cooling) &&
           nowMs - run.updatedMs > kPhoneBakeStaleMs;
}

u32 phoneBakeResumeStep(const PhoneBakePlan& plan, const PhoneBakeRun& run) {
    if (run.plan != plan.id) {
        return 0;
    }
    return std::min<u32>(run.finished, static_cast<u32>(plan.steps.size()));
}

f32 phoneBakeLineFraction(std::string_view line, std::string_view step) {
    constexpr std::string_view kInfo = "[INFO] ";
    if (line.substr(0, kInfo.size()) == kInfo) {
        line.remove_prefix(kInfo.size());
    }
    if (step.empty() || line.size() <= step.size() + 2 || line.substr(0, step.size()) != step ||
        line.substr(step.size(), 2) != ": ") {
        return -1.0f;
    }
    line.remove_prefix(step.size() + 2);
    const usize slash = line.find(" / ");
    if (slash == std::string_view::npos) {
        return -1.0f;
    }
    usize end = slash + 3;
    while (end < line.size() && line[end] >= '0' && line[end] <= '9') {
        ++end;
    }
    u64 done = 0;
    u64 total = 0;
    if (!parseCount(line.substr(0, slash), done) ||
        !parseCount(line.substr(slash + 3, end - slash - 3), total) || total == 0) {
        return -1.0f;
    }
    return std::clamp(static_cast<f32>(static_cast<f64>(done) / static_cast<f64>(total)), 0.0f, 1.0f);
}

std::string_view phoneBakeErrorText(std::string_view line) {
    constexpr std::string_view kError = "[ERROR] ";
    return line.substr(0, kError.size()) == kError ? line.substr(kError.size()) : std::string_view{};
}

PhoneBakeProgress phoneBakeProgress(const PhoneBakePlan& plan, const PhoneBakeRun& run, i64 nowMs) {
    PhoneBakeProgress progress;
    const u32 count = static_cast<u32>(plan.steps.size());
    const f64 total = std::max(phoneBakeSeconds(plan), 1.0);
    const bool ours = run.plan == plan.id;
    const u32 finished = ours ? std::min(run.finished, count) : 0;
    f64 doneEstimate = 0.0;
    for (u32 i = 0; i < finished; ++i) {
        doneEstimate += plan.steps[i].seconds;
    }
    // How this phone does against the measured costs: the steps it finished (the long ones count most).
    f64 factor = 1.0;
    if (ours) {
        f64 actual = 0.0;
        f64 estimated = 0.0;
        for (u32 i = 0; i < finished && i < run.seconds.size(); ++i) {
            actual += run.seconds[i];
            estimated += plan.steps[i].seconds;
        }
        if (estimated >= 30.0) {
            factor = std::clamp(actual / estimated, 0.5, 4.0);
        }
    }
    const i64 end = ours && (run.state == PhoneBakeState::Running || run.state == PhoneBakeState::Cooling)
                        ? nowMs
                        : run.updatedMs;
    progress.elapsedSeconds =
        ours && run.planStartedMs > 0 ? std::max<f64>(0.0, (end - run.planStartedMs) / 1e3) : 0.0;
    if (ours && run.state == PhoneBakeState::Done) {
        progress.fraction = 1.0f;
        progress.stepFraction = 1.0f;
        return progress;
    }
    f64 later = 0.0;
    const bool running = ours && run.state == PhoneBakeState::Running && run.step < count;
    for (u32 i = running ? run.step + 1 : finished; i < count; ++i) {
        later += plan.steps[i].seconds;
    }
    if (!running) {
        progress.fraction = static_cast<f32>(doneEstimate / total);
        progress.secondsLeft = factor * later;
        return progress;
    }
    const PhoneBakePlanStep& step = plan.steps[run.step];
    const f64 elapsed = std::max<f64>(0.0, (nowMs - run.stepStartedMs) / 1e3);
    const f64 expected = std::max(step.seconds * factor, 1.0);
    f64 stepLeft = 0.0;
    if (run.fraction >= 0.02f) {
        // The baker counts this step's work: its own pace says what is left.
        progress.stepFraction = std::min(run.fraction, 1.0f);
        stepLeft = elapsed / progress.stepFraction - elapsed;
    } else {
        progress.stepFraction = static_cast<f32>(std::min(elapsed / expected, 0.95));
        stepLeft = std::max(expected - elapsed, expected * 0.05);
    }
    progress.fraction =
        static_cast<f32>(std::min((doneEstimate + step.seconds * progress.stepFraction) / total, 1.0));
    progress.secondsLeft = std::max(stepLeft, 0.0) + factor * later;
    return progress;
}

bool phoneFirstBakeUnfinished(const std::filesystem::path& bakeDir) {
    const PhoneBakePlaces places{{}, {}, bakeDir};
    std::error_code error;
    if (!std::filesystem::exists(places.planFile(), error)) {
        return false;
    }
    Result<std::string> planText = readFile(places.planFile());
    if (!planText) {
        return false;
    }
    Result<PhoneBakePlan> plan = readPhoneBakePlan(planText.value());
    if (!plan || plan.value().kind != PhoneBakeKind::First) {
        return false;
    }
    Result<std::string> runText = readFile(places.statusFile());
    if (!runText) {
        return true; // planned, its service not started yet
    }
    Result<PhoneBakeRun> run = readPhoneBakeRun(runText.value());
    return !run || run.value().plan != plan.value().id || run.value().state != PhoneBakeState::Done;
}

} // namespace vd
