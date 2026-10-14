#include "BakeRunner.h"

#include "ChildProcess.h"
#include "HashCache.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <set>

namespace vd::setup {
namespace {

struct StepInfo {
    std::string_view name;
    f64 weight; // seconds of a full bake without the cache on the development PC (i7-6700K), roughly
};
constexpr std::array<StepInfo, 14> kSteps = {{{"db", 5},
                                              {"textures", 420},
                                              {"models", 60},
                                              {"actors", 30},
                                              {"world", 180},
                                              {"collision", 60},
                                              {"objects", 3},
                                              {"ui", 30},
                                              {"sound", 50},
                                              {"cinematics", 2},
                                              {"grass", 70},
                                              {"worldsound", 30},
                                              {"bodyfx", 3},
                                              {"locale", 60}}};

std::array<std::string_view, kSteps.size()> stepNames() {
    std::array<std::string_view, kSteps.size()> names{};
    for (usize i = 0; i < kSteps.size(); ++i) {
        names[i] = kSteps[i].name;
    }
    return names;
}

f64 stepWeight(const BakeJob& job) {
    for (const StepInfo& step : kSteps) {
        if (step.name == job.step) {
            // A few maps of world / collision: Azeroth and Kalimdor are most of it, the rest are small.
            return job.maps.empty() ? step.weight
                                    : step.weight * std::min(1.0, 0.25 * static_cast<f64>(job.maps.size()));
        }
    }
    return 10;
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// The packs a step writes, as a test of presence: one file, or a prefix for the per-map steps.
bool stepPresent(std::string_view step, std::string_view overlayLocale, const std::set<std::string>& names) {
    auto has = [&](const std::string& name) { return names.count(name) != 0; };
    auto hasPrefix = [&](std::string_view prefix, std::string_view except) {
        return std::any_of(names.begin(), names.end(), [&](const std::string& name) {
            return startsWith(name, prefix) && name != except;
        });
    };
    if (step == "world") {
        return hasPrefix("world_", "") && has("liquids.vdpk");
    }
    if (step == "collision") {
        return hasPrefix("collision_", "collision_objects.vdpk");
    }
    if (step == "objects") {
        return has("collision_objects.vdpk");
    }
    if (step == "locale") {
        if (overlayLocale.empty()) {
            return true;
        }
        const std::string locale(overlayLocale);
        return has("db." + locale + ".vdpk") && has("textures." + locale + ".vdpk") &&
               has("sound." + locale + ".vdpk");
    }
    return has(std::string(step) + ".vdpk");
}

// "textures: 1200 / 58502 (40 s)" -> 1200 / 58502.
bool parseCount(std::string_view line, f64& fraction) {
    const usize slash = line.find(" / ");
    if (slash == std::string_view::npos || slash == 0) {
        return false;
    }
    usize begin = slash;
    while (begin > 0 && line[begin - 1] >= '0' && line[begin - 1] <= '9') {
        --begin;
    }
    usize end = slash + 3;
    while (end < line.size() && line[end] >= '0' && line[end] <= '9') {
        ++end;
    }
    if (begin == slash || end == slash + 3) {
        return false;
    }
    const f64 done = std::strtod(std::string(line.substr(begin, slash - begin)).c_str(), nullptr);
    const f64 total = std::strtod(std::string(line.substr(slash + 3, end - slash - 3)).c_str(), nullptr);
    if (total <= 0) {
        return false;
    }
    fraction = std::clamp(done / total, 0.0, 1.0);
    return true;
}

std::string joined(const std::vector<std::string>& items, char separator) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out.push_back(separator);
        }
        out += item;
    }
    return out;
}

} // namespace

std::span<const std::string_view> bakeSteps() {
    static const std::array<std::string_view, kSteps.size()> names = stepNames();
    return names;
}

bool isBakeStep(std::string_view step) {
    const std::span<const std::string_view> steps = bakeSteps();
    return std::find(steps.begin(), steps.end(), step) != steps.end();
}

std::vector<PackFile> listPackFiles(const std::filesystem::path& packsDir) {
    std::vector<PackFile> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(packsDir, error)) {
        const std::string name = utf8FromPath(entry.path().filename());
        if (entry.is_regular_file(error) && pack::isClientPack(name)) {
            files.push_back({name, entry.file_size(error)});
        }
    }
    std::sort(files.begin(), files.end(),
              [](const PackFile& a, const PackFile& b) { return a.name < b.name; });
    return files;
}

BakePlan planBake(const PlanInputs& inputs, const pack::PackManifest& manifest,
                  std::span<const pack::Recipe> recipes, std::span<const PackFile> files) {
    BakePlan plan;
    std::set<std::string> names;
    std::map<std::string, u64> sizes;
    for (const PackFile& file : files) {
        names.insert(file.name);
        sizes[file.name] = file.size;
    }
    plan.noManifest = !files.empty() && manifest.packs.empty();
    plan.inputsChanged = inputs.inputsChanged ||
                         (!manifest.clientLocale.empty() && manifest.clientLocale != inputs.baseLocale);

    // step -> maps (an empty string in the set: the whole step).
    std::map<std::string, std::set<std::string>> wanted;
    auto want = [&](const std::string& step, const std::string& map) { wanted[step].insert(map); };
    const bool everything = inputs.force || plan.inputsChanged;
    for (const std::string_view step : bakeSteps()) {
        if (step == "locale" && inputs.overlayLocale.empty()) {
            continue;
        }
        if (everything) {
            want(std::string(step), "");
        } else if (!stepPresent(step, inputs.overlayLocale, names)) {
            plan.missing.emplace_back(step);
            want(std::string(step), "");
        }
    }
    if (!everything) {
        // Recipes of another release, and packs the manifest knows that are gone or cut short.
        std::vector<std::string> outdated =
            pack::outdatedPacks(manifest, std::vector<pack::Recipe>(recipes.begin(), recipes.end()));
        for (const pack::PackManifest::Entry& entry : manifest.packs) {
            if (!pack::isClientPack(entry.file)) {
                continue;
            }
            const auto size = sizes.find(entry.file);
            if (size == sizes.end() || (entry.size != 0 && size->second != entry.size)) {
                outdated.push_back(entry.file);
            }
        }
        std::sort(outdated.begin(), outdated.end());
        outdated.erase(std::unique(outdated.begin(), outdated.end()), outdated.end());
        for (const std::string& file : outdated) {
            const pack::BakeStep step = pack::bakeStepOf(file);
            if (step.step.empty() || step.step == "app" ||
                (step.step == "locale" && inputs.overlayLocale.empty())) {
                continue;
            }
            plan.outdated.push_back(file);
            want(step.step, step.map);
        }
    }
    for (const std::string_view step : bakeSteps()) {
        if (!inputs.only.empty() && step != inputs.only) {
            continue;
        }
        const auto it = wanted.find(std::string(step));
        if (it == wanted.end()) {
            continue;
        }
        BakeJob job;
        job.step = std::string(step);
        if (!it->second.count("")) {
            job.maps.assign(it->second.begin(), it->second.end());
        }
        plan.jobs.push_back(std::move(job));
    }
    return plan;
}

std::vector<std::string> bakerArguments(const BakeRun& run, const BakeJob& job) {
    std::vector<std::string> args = {
        "bake", "--wow-dir", utf8FromPath(run.wow), "--out", utf8FromPath(run.packsDir), "--only", job.step};
    if (!job.maps.empty()) {
        args.insert(args.end(), {"--maps", joined(job.maps, ',')});
    }
    // ";" is an empty list: without it the baker falls back to $VD_WOW_LOCALE_DIRS and its built-in folders.
    args.insert(args.end(),
                {"--locale-clients", run.localeClient.empty() ? ";" : utf8FromPath(run.localeClient)});
    if (!run.skipArchives.empty()) {
        args.insert(args.end(), {"--skip-archives", joined(run.skipArchives, ';')});
    }
    if (!run.localeClient.empty() && !run.localeSkipArchives.empty()) {
        args.insert(args.end(), {"--locale-skip-archives", joined(run.localeSkipArchives, ';')});
    }
    return args;
}

std::filesystem::path findBaker(const std::filesystem::path& programDir) {
    const std::filesystem::path name = pathFromUtf8(executableName("vd_baker"));
    std::error_code error;
    if (std::filesystem::is_regular_file(programDir / name, error)) {
        return programDir / name;
    }
    // Development: build/<preset>/tools/setup/<config>/vdsetup ->
    // build/<preset>/tools/baker/<config>/vd_baker (multi-config generators), or
    // build/<preset>/tools/setup/vdsetup -> build/<preset>/tools/baker/vd_baker.
    const std::filesystem::path config = programDir.filename();
    for (const std::filesystem::path& candidate :
         {programDir.parent_path().parent_path() / "baker" / config / name,
          programDir.parent_path() / "baker" / name}) {
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

Result<BakeOutcome> runBake(std::span<const BakeJob> jobs, const BakeRun& run, const Operation& operation) {
    BakeOutcome outcome;
    f64 total = 0;
    for (const BakeJob& job : jobs) {
        total += stepWeight(job);
    }
    std::error_code error;
    std::filesystem::create_directories(run.packsDir, error);
    f64 done = 0;
    for (usize i = 0; i < jobs.size(); ++i) {
        const BakeJob& job = jobs[i];
        const f64 weight = stepWeight(job);
        const std::string label =
            formatText("VD_SETUP_BAKE_STEP", {job.step, std::to_string(i + 1), std::to_string(jobs.size())});
        operation.report("bake", total > 0 ? done / total : 0.0, label);
        const std::filesystem::file_time_type start = std::filesystem::file_time_type::clock::now();

        ProcessOptions process;
        process.program = run.baker;
        process.args = bakerArguments(run, job);
        process.cancel = operation.cancel;
        process.onLine = [&](std::string_view line) {
            operation.report("bake-log", -1.0, line);
            f64 inStep = 0;
            if (parseCount(line, inStep)) {
                operation.report("bake", total > 0 ? (done + weight * inStep) / total : 0.0, label);
            }
        };
        Result<ProcessResult> result = runProcess(process);
        if (!result) {
            return result.error();
        }
        if (result.value().cancelled) {
            // What this run was writing is incomplete: delete it, the next bake redoes the step.
            for (const auto& entry : std::filesystem::directory_iterator(run.packsDir, error)) {
                const std::string name = utf8FromPath(entry.path().filename());
                if (endsWith(name, ".vdpk") && entry.last_write_time(error) >= start) {
                    std::filesystem::remove(entry.path(), error);
                }
            }
            outcome.cancelled = true;
            return outcome;
        }
        if (result.value().exitCode != 0) {
            outcome.failed.push_back(job.step);
        }
        done += weight;
    }
    operation.report("bake", 1.0, {});
    return outcome;
}

} // namespace vd::setup
