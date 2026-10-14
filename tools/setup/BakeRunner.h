#pragma once

#include "SetupCommon.h"
#include "core/Result.h"
#include "pack/PackManifest.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (ADR-0005 §3-§4): which bake steps a packs folder needs, and running them. The baker is a child
// process (vd_baker next to the program: the same binary the phone runs, a crash cannot take the Setup down,
// and its log lines are the progress). A step is baked when its packs are missing, when the manifest's recipe
// of one of them is not this release's (pack::outdatedPacks; per map for world_* and collision_*), when a
// bake was cut short (a pack whose size is not the manifest's), or when the inputs changed (another client
// language or other archives).
namespace vd::setup {

// The steps of `vd_baker bake --only`, in the order a full bake runs them (app.vdpk travels in the APK: not
// here).
std::span<const std::string_view> bakeSteps();
bool isBakeStep(std::string_view step);

struct BakeJob {
    std::string step;
    std::vector<std::string> maps; // world / collision: these maps only (--maps); empty: the whole step
};

struct PackFile {
    std::string name; // "models.vdpk"
    u64 size = 0;
};

struct PlanInputs {
    std::string baseLocale;     // the WoW's language
    std::string overlayLocale;  // the second client's, empty when none
    bool inputsChanged = false; // the fingerprint of the clients is not the last bake's
    bool force = false;         // every step (or `only`) whatever its state
    std::string only;           // one step; empty: all
};

struct BakePlan {
    std::vector<BakeJob> jobs;
    std::vector<std::string> outdated; // packs whose recipe is not this release's, or cut short
    std::vector<std::string> missing;  // steps with no pack in the folder
    bool inputsChanged = false;        // everything again: other clients or archives than the last bake
    bool noManifest = false; // packs without manifest.json (baked before phase 22): taken as up to date
};

// The plan for a folder holding `files` (its *.vdpk with their sizes) and `manifest`, against `recipes`
// (normally pack::releaseRecipes()).
BakePlan planBake(const PlanInputs& inputs, const pack::PackManifest& manifest,
                  std::span<const pack::Recipe> recipes, std::span<const PackFile> files);
// The client packs of a folder (every *.vdpk but app.vdpk), sorted by name.
std::vector<PackFile> listPackFiles(const std::filesystem::path& packsDir);

struct BakeRun {
    std::filesystem::path baker; // the vd_baker executable
    std::filesystem::path wow;   // the base client's folder
    std::vector<std::string> skipArchives;
    std::filesystem::path localeClient; // empty: none
    std::vector<std::string> localeSkipArchives;
    std::filesystem::path packsDir;
};

struct BakeOutcome {
    std::vector<std::string> failed; // steps whose baker run did not end well
    bool cancelled = false;
};

// The arguments of one baker run (exposed for tests).
std::vector<std::string> bakerArguments(const BakeRun& run, const BakeJob& job);

// The baker next to this program, or in the development build's layout
// (build/<preset>/tools/baker/<config>/).
std::filesystem::path findBaker(const std::filesystem::path& programDir);

// Runs the jobs one by one. Progress: "bake" with the whole plan's fraction (by each step's usual share of a
// full bake, and the "N / M" counts the baker logs) and the text VD_SETUP_BAKE_STEP; every baker line as step
// "bake-log" (its own words, English). A cancelled job's half-written packs are deleted (the next bake redoes
// them).
Result<BakeOutcome> runBake(std::span<const BakeJob> jobs, const BakeRun& run, const Operation& operation);

} // namespace vd::setup
