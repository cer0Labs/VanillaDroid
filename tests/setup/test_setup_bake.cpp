// Phase 22 (setup): what a packs folder needs baked (BakeRunner.h).
#include "BakeRunner.h"
#include "SetupCommon.h"
#include "pack/PackManifest.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ostream> // doctest prints std::string values through operator<<
#include <string>
#include <vector>

using namespace vd::setup;
using vd::pack::PackManifest;
using vd::pack::Recipe;

namespace {

// Every pack a whole bake leaves, with the maps cut to two of each.
std::vector<PackFile> fullFolder(bool overlays) {
    std::vector<PackFile> files = {{"actors.vdpk", 10},
                                   {"bodyfx.vdpk", 10},
                                   {"cinematics.vdpk", 10},
                                   {"collision_azeroth.vdpk", 10},
                                   {"collision_kalimdor.vdpk", 10},
                                   {"collision_objects.vdpk", 10},
                                   {"db.vdpk", 10},
                                   {"grass.vdpk", 10},
                                   {"liquids.vdpk", 10},
                                   {"models.vdpk", 10},
                                   {"sound.vdpk", 10},
                                   {"textures.vdpk", 10},
                                   {"ui.vdpk", 10},
                                   {"world_azeroth.vdpk", 10},
                                   {"world_kalimdor.vdpk", 10},
                                   {"worldsound.vdpk", 10}};
    if (overlays) {
        files.insert(files.end(),
                     {{"db.esES.vdpk", 10}, {"sound.esES.vdpk", 10}, {"textures.esES.vdpk", 10}});
    }
    std::sort(files.begin(), files.end(),
              [](const PackFile& a, const PackFile& b) { return a.name < b.name; });
    return files;
}

PackManifest manifestOf(const std::vector<PackFile>& files, const std::string& recipe) {
    PackManifest manifest;
    manifest.bakedBy = "0.1.0";
    manifest.clientBuild = 5875;
    manifest.clientLocale = "enUS";
    for (const PackFile& file : files) {
        PackManifest::Entry& entry = manifest.record(file.name);
        entry.size = file.size;
        entry.recipe = recipe;
    }
    return manifest;
}

std::vector<Recipe> recipesOf(const std::vector<PackFile>& files, const std::string& recipe) {
    std::vector<Recipe> recipes;
    for (const PackFile& file : files) {
        recipes.push_back({file.name, recipe});
    }
    return recipes;
}

std::vector<std::string> stepsOf(const BakePlan& plan) {
    std::vector<std::string> steps;
    for (const BakeJob& job : plan.jobs) {
        steps.push_back(job.step);
    }
    return steps;
}

const BakeJob* jobOf(const BakePlan& plan, const std::string& step) {
    for (const BakeJob& job : plan.jobs) {
        if (job.step == step) {
            return &job;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Setup bake: the steps") {
    CHECK(bakeSteps().size() == 14);
    CHECK(bakeSteps().front() == "db");
    CHECK(isBakeStep("textures"));
    CHECK(isBakeStep("locale"));
    CHECK_FALSE(isBakeStep("app")); // in the APK
    CHECK_FALSE(isBakeStep("all"));
}

TEST_CASE("Setup bake: an empty folder bakes everything") {
    PlanInputs inputs;
    inputs.baseLocale = "enUS";
    BakePlan plan = planBake(inputs, PackManifest{}, {}, {});
    CHECK(plan.jobs.size() == 13); // no second client: no locale
    CHECK(plan.missing.size() == 13);
    CHECK_FALSE(plan.noManifest);
    inputs.overlayLocale = "esES";
    plan = planBake(inputs, PackManifest{}, {}, {});
    CHECK(plan.jobs.size() == 14);
    CHECK(plan.jobs.back().step == "locale");
    CHECK(plan.jobs.front().maps.empty());
    inputs.only = "grass";
    plan = planBake(inputs, PackManifest{}, {}, {});
    CHECK(stepsOf(plan) == std::vector<std::string>{"grass"});
}

TEST_CASE("Setup bake: a whole folder is up to date") {
    const std::vector<PackFile> files = fullFolder(true);
    PlanInputs inputs;
    inputs.baseLocale = "enUS";
    inputs.overlayLocale = "esES";
    // Recipes of this release: the same.
    BakePlan plan = planBake(inputs, manifestOf(files, "r1"), recipesOf(files, "r1"), files);
    CHECK(plan.jobs.empty());
    CHECK(plan.outdated.empty());
    // Development: no recipes in release/packs.json.
    plan = planBake(inputs, manifestOf(files, ""), {}, files);
    CHECK(plan.jobs.empty());
    // Baked before phase 22: no manifest.
    plan = planBake(inputs, PackManifest{}, {}, files);
    CHECK(plan.jobs.empty());
    CHECK(plan.noManifest);
    // Without its overlays the second client's step is missing.
    plan = planBake(inputs, manifestOf(fullFolder(false), "r1"), {}, fullFolder(false));
    CHECK(stepsOf(plan) == std::vector<std::string>{"locale"});
    // --force: all; --only with --force: that one.
    inputs.force = true;
    CHECK(planBake(inputs, manifestOf(files, "r1"), {}, files).jobs.size() == 14);
    inputs.only = "ui";
    CHECK(stepsOf(planBake(inputs, manifestOf(files, "r1"), {}, files)) == std::vector<std::string>{"ui"});
}

TEST_CASE("Setup bake: other recipes re-bake their steps, per map") {
    const std::vector<PackFile> files = fullFolder(true);
    PlanInputs inputs;
    inputs.baseLocale = "enUS";
    inputs.overlayLocale = "esES";
    std::vector<Recipe> recipes = recipesOf(files, "r1");
    for (Recipe& recipe : recipes) {
        if (recipe.file == "world_azeroth.vdpk" || recipe.file == "collision_kalimdor.vdpk" ||
            recipe.file == "models.vdpk" || recipe.file == "textures.esES.vdpk") {
            recipe.recipe = "r2";
        }
    }
    BakePlan plan = planBake(inputs, manifestOf(files, "r1"), recipes, files);
    CHECK(stepsOf(plan) == std::vector<std::string>{"models", "world", "collision", "locale"});
    REQUIRE(jobOf(plan, "world"));
    CHECK(jobOf(plan, "world")->maps == std::vector<std::string>{"azeroth"});
    CHECK(jobOf(plan, "collision")->maps == std::vector<std::string>{"kalimdor"});
    CHECK(jobOf(plan, "models")->maps.empty());
    CHECK(plan.outdated.size() == 4);
    // The shared liquids: the whole world step.
    for (Recipe& recipe : recipes) {
        if (recipe.file == "liquids.vdpk") {
            recipe.recipe = "r2";
        }
    }
    plan = planBake(inputs, manifestOf(files, "r1"), recipes, files);
    CHECK(jobOf(plan, "world")->maps.empty());
    // A pack the release lists and the folder lacks.
    recipes.push_back({"world_newmap.vdpk", "r1"});
    plan = planBake(inputs, manifestOf(files, "r1"), recipes, files);
    CHECK(jobOf(plan, "world")->maps.empty()); // still whole: liquids
}

TEST_CASE("Setup bake: a bake cut short, a deleted pack, another client") {
    std::vector<PackFile> files = fullFolder(false);
    PlanInputs inputs;
    inputs.baseLocale = "enUS";
    const PackManifest manifest = manifestOf(files, "");
    // textures.vdpk half written (killed): not the size the manifest recorded.
    for (PackFile& file : files) {
        if (file.name == "textures.vdpk") {
            file.size = 3;
        }
    }
    BakePlan plan = planBake(inputs, manifest, {}, files);
    CHECK(stepsOf(plan) == std::vector<std::string>{"textures"});
    // world_kalimdor.vdpk deleted.
    files = fullFolder(false);
    files.erase(std::remove_if(files.begin(), files.end(),
                               [](const PackFile& f) { return f.name == "world_kalimdor.vdpk"; }),
                files.end());
    plan = planBake(inputs, manifest, {}, files);
    REQUIRE(jobOf(plan, "world"));
    CHECK(jobOf(plan, "world")->maps == std::vector<std::string>{"kalimdor"});
    // A client in another language than the packs': everything.
    inputs.baseLocale = "esES";
    plan = planBake(inputs, manifest, {}, fullFolder(false));
    CHECK(plan.inputsChanged);
    CHECK(plan.jobs.size() == 13);
    inputs.baseLocale = "enUS";
    inputs.inputsChanged = true; // other archives (Settings::lastBake)
    CHECK(planBake(inputs, manifest, {}, fullFolder(false)).jobs.size() == 13);
}

TEST_CASE("Setup bake: the baker's arguments and where it is") {
    BakeRun run;
    run.baker = "vd_baker.exe";
    run.wow = "C:/World of Warcraft 1.12";
    run.packsDir = "C:/packs";
    BakeJob job{"world", {"azeroth", "kalimdor"}};
    std::vector<std::string> args = bakerArguments(run, job);
    CHECK(args == std::vector<std::string>{"bake", "--wow-dir", "C:/World of Warcraft 1.12", "--out",
                                           "C:/packs", "--only", "world", "--maps", "azeroth,kalimdor",
                                           "--locale-clients", ";"});
    run.skipArchives = {"patch-A.MPQ", "patch-3.MPQ"};
    run.localeClient = "D:/WoW esES";
    run.localeSkipArchives = {"patch-B.MPQ"};
    args = bakerArguments(run, BakeJob{"locale", {}});
    CHECK(std::find(args.begin(), args.end(), "D:/WoW esES") != args.end());
    CHECK(std::find(args.begin(), args.end(), "patch-A.MPQ;patch-3.MPQ") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--locale-skip-archives") != args.end());

    // The development layout: build/<preset>/tools/setup/<config>/ next to
    // build/<preset>/tools/baker/<config>/.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "vd_setup_tests" / "layout";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "tools" / "setup" / "Release");
    std::filesystem::create_directories(root / "tools" / "baker" / "Release");
    CHECK(findBaker(root / "tools" / "setup" / "Release").empty());
    const std::filesystem::path baker = root / "tools" / "baker" / "Release" /
                                        pathFromUtf8(
#if defined(_WIN32)
                                            "vd_baker.exe"
#else
                                            "vd_baker"
#endif
                                        );
    std::ofstream(baker) << "x";
    CHECK(findBaker(root / "tools" / "setup" / "Release") == baker);

    // listPackFiles: client packs only.
    const std::filesystem::path packs = root / "packs";
    std::filesystem::create_directories(packs / ".cache");
    for (const char* name : {"db.vdpk", "app.vdpk", "manifest.json", "world_azeroth.vdpk", "x.tmp"}) {
        std::ofstream(packs / name) << "data";
    }
    const std::vector<PackFile> listed = listPackFiles(packs);
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].name == "db.vdpk");
    CHECK(listed[1].name == "world_azeroth.vdpk");
    CHECK(listed[1].size == 4);
}
