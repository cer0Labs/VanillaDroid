#include "pack/PackManifest.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <ostream> // doctest prints std::string values through operator<<
#include <string>

using vd::pack::bakeStepOf;
using vd::pack::PackManifest;

TEST_CASE("Pack manifest: the bake step of each pack file") {
    CHECK(bakeStepOf("db.vdpk").step == "db");
    CHECK(bakeStepOf("textures.vdpk").step == "textures");
    CHECK(bakeStepOf("models.vdpk").step == "models");
    CHECK(bakeStepOf("ui.vdpk").step == "ui");
    CHECK(bakeStepOf("bodyfx.vdpk").step == "bodyfx");
    CHECK(bakeStepOf("world_azeroth.vdpk").step == "world");
    CHECK(bakeStepOf("world_azeroth.vdpk").map == "azeroth");
    CHECK(bakeStepOf("world_zul_gurub.vdpk").map == "zul_gurub");
    CHECK(bakeStepOf("liquids.vdpk").step == "world");
    CHECK(bakeStepOf("liquids.vdpk").map.empty());
    CHECK(bakeStepOf("collision_kalimdor.vdpk").step == "collision");
    CHECK(bakeStepOf("collision_kalimdor.vdpk").map == "kalimdor");
    CHECK(bakeStepOf("collision_objects.vdpk").step == "objects");
    CHECK(bakeStepOf("db.esES.vdpk").step == "locale");
    CHECK(bakeStepOf("sound.esES.vdpk").step == "locale");
    CHECK(bakeStepOf("app.vdpk").step == "app");
    CHECK(bakeStepOf("manifest.json").step.empty());
    CHECK(bakeStepOf("unknown.vdpk").step.empty());
    CHECK(vd::pack::isClientPack("world_kalimdor.vdpk"));
    CHECK_FALSE(vd::pack::isClientPack("app.vdpk"));
    CHECK_FALSE(vd::pack::isClientPack("manifest.json"));
}

TEST_CASE("Pack manifest: recipes read back sorted") {
    const std::string text = vd::pack::writeRecipes({{"world_azeroth.vdpk", "b2"}, {"db.vdpk", "a1"}});
    const vd::Result<std::vector<vd::pack::Recipe>> recipes = vd::pack::parseRecipes(text);
    REQUIRE(recipes.ok());
    REQUIRE(recipes.value().size() == 2);
    CHECK(recipes.value()[0].file == "db.vdpk");
    CHECK(recipes.value()[1].recipe == "b2");
    const char* unknownFormat = R"({"format": 9, "packs": {}})";
    CHECK_FALSE(vd::pack::parseRecipes(unknownFormat).ok());
    // The build's own recipes always parse (release/packs.json may still be empty in development).
    for (const vd::pack::Recipe& recipe : vd::pack::releaseRecipes()) {
        CHECK(vd::pack::releaseRecipeOf(recipe.file) == recipe.recipe);
    }
    CHECK(vd::pack::releaseRecipeOf("no-such.vdpk").empty());
}

TEST_CASE("Pack manifest: save, load and the outdated packs") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vd_test_pack_manifest";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    PackManifest manifest;
    CHECK(PackManifest::load(dir).ok()); // no file: an empty manifest
    manifest.bakedBy = "0.2.0";
    manifest.clientBuild = 5875;
    manifest.clientLocale = "enUS";
    manifest.overlays = {"esES"};
    manifest.record("world_azeroth.vdpk").recipe = "aaa";
    manifest.record("db.vdpk").recipe = "bbb";
    manifest.record("models.vdpk").recipe = "";
    manifest.record("db.esES.vdpk").recipe = "ccc";
    manifest.record("db.vdpk").size = 26783400;
    CHECK(manifest.packs.size() == 4);
    CHECK(manifest.packs.front().file == "db.esES.vdpk"); // sorted by name
    REQUIRE(manifest.save(dir).ok());

    const vd::Result<PackManifest> loaded = PackManifest::load(dir);
    REQUIRE(loaded.ok());
    CHECK(loaded.value().bakedBy == "0.2.0");
    CHECK(loaded.value().clientBuild == 5875);
    CHECK(loaded.value().clientLocale == "enUS");
    REQUIRE(loaded.value().overlays.size() == 1);
    REQUIRE(loaded.value().find("db.vdpk") != nullptr);
    CHECK(loaded.value().find("db.vdpk")->size == 26783400);
    CHECK(loaded.value().find("world_azeroth.vdpk")->recipe == "aaa");

    const std::vector<vd::pack::Recipe> release = {
        {"world_azeroth.vdpk", "aaa"}, // same
        {"db.vdpk", "zzz"},            // another recipe: outdated
        {"models.vdpk", "mmm"},        // recorded without a recipe: nothing to compare
        {"sound.vdpk", "sss"},         // missing from the folder
        {"db.esES.vdpk", "ccc"},       // the overlay of a baked language, same
        {"db.deDE.vdpk", "ddd"},       // an overlay of a language the folder was not baked with: not missing
        {"app.vdpk", "xxx"},           // not a client pack
    };
    const std::vector<std::string> outdated = vd::pack::outdatedPacks(loaded.value(), release);
    REQUIRE(outdated.size() == 2);
    CHECK(outdated[0] == "db.vdpk");
    CHECK(outdated[1] == "sound.vdpk");

    std::ofstream(dir / "manifest.json") << "{ broken";
    CHECK_FALSE(PackManifest::load(dir).ok());
    std::filesystem::remove_all(dir);
}
