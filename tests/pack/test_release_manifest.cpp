// Phase 22 (updates): reading a release's manifest.json (pack/ReleaseManifest), its signature, and what it
// changes for a program and a packs folder.
#include "pack/ReleaseManifest.h"

#include "core/Version.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <ostream> // doctest prints std::string values through operator<<
#include <string>
#include <vector>

using vd::pack::PackManifest;
using vd::pack::Recipe;
using vd::pack::ReleaseManifest;
using vd::pack::SignedRelease;
using vd::pack::StepPlan;

namespace {

std::filesystem::path dataDir() {
    return std::filesystem::path(VD_TEST_DATA_DIR);
}

std::string readFile(const std::filesystem::path& path) {
    std::string bytes;
    std::FILE* file = nullptr;
#if defined(_WIN32)
    _wfopen_s(&file, path.c_str(), L"rb");
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (!file) {
        return bytes;
    }
    char block[4096];
    std::size_t read = 0;
    while ((read = std::fread(block, 1, sizeof(block), file)) > 0) {
        bytes.append(block, read);
    }
    std::fclose(file);
    return bytes;
}

std::vector<vd::u8> testKey() {
    std::string hex = readFile(dataDir() / "release-test-key.pub.hex");
    std::vector<vd::u8> bytes;
    for (std::size_t i = 0; i + 1 < hex.size() && hex[i] != '\n' && hex[i] != '\r'; i += 2) {
        bytes.push_back(static_cast<vd::u8>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

const std::string kDigest(64, 'a');

std::string manifestJson(const std::string& version, long long code, const std::string& packs) {
    return "{\"format\": 1, \"version\": \"" + version + "\", \"versionCode\": " + std::to_string(code) +
           ", \"date\": \"2026-10-20\", \"notes\": \"https://example.org/v\", \"apk\": {\"url\": "
           "\"https://example.org/a.apk\", \"size\": 12, \"sha256\": \"" +
           kDigest + "\"}, \"setup\": {\"windows\": {\"url\": \"https://example.org/w.zip\", \"size\": 3, " +
           "\"sha256\": \"" + kDigest + "\"}}, \"packs\": {" + packs + "}}";
}

PackManifest folderOf(std::initializer_list<std::pair<const char*, const char*>> packs) {
    PackManifest folder;
    folder.clientLocale = "enUS";
    for (const auto& [file, recipe] : packs) {
        folder.record(file).recipe = recipe;
    }
    return folder;
}

} // namespace

TEST_CASE("Release manifest: versions as version codes") {
    CHECK(vd::pack::versionCodeOf("0.2.0") == 2000u);
    CHECK(vd::pack::versionCodeOf("1.12.1") == 1012001u);
    CHECK(vd::pack::versionCodeOf("999.999.999") == 999999999u);
    CHECK(vd::pack::versionCodeOf(vd::kVersion) == vd::kVersionCode);
    CHECK(vd::pack::versionCodeOf("") == 0u);
    CHECK(vd::pack::versionCodeOf("1.2") == 0u);
    CHECK(vd::pack::versionCodeOf("1.2.3.4") == 0u);
    CHECK(vd::pack::versionCodeOf("1..3") == 0u);
    CHECK(vd::pack::versionCodeOf("1.2.3a") == 0u);
    CHECK(vd::pack::versionCodeOf("1000.0.0") == 0u);
    CHECK(vd::pack::versionCodeOf("v1.2.3") == 0u);
}

TEST_CASE("Release manifest: the test release written by scripts/release-manifest.ps1") {
    const std::string json = readFile(dataDir() / "release-test-manifest.json");
    const std::string signature = readFile(dataDir() / "release-test-manifest.json.sig");
    REQUIRE(!json.empty());
    const SignedRelease signedRelease = vd::pack::readSignedRelease(json, signature, testKey());
    REQUIRE(signedRelease.status == SignedRelease::Status::Ok);
    const ReleaseManifest& m = signedRelease.manifest;
    CHECK(m.version == "0.2.0");
    CHECK(m.versionCode == 2000u);
    CHECK(m.date == "2026-10-20");
    CHECK(m.notes == "https://github.com/cer0Labs/VanillaDroid/releases/tag/v0.2.0");
    CHECK(m.apk.present());
    CHECK(m.apk.size == 300000u);
    CHECK(m.apk.url ==
          "https://github.com/cer0Labs/VanillaDroid/releases/download/v0.2.0/VanillaDroid-0.2.0.apk");
    CHECK(m.setupWindows.present());
    CHECK(m.setupMacos.present());
    CHECK(m.setupMacos.size == 250000u);
    REQUIRE(m.packs.size() == 3);
    CHECK(m.packs[0].file == "actors.vdpk");
    CHECK(m.packs[2].file == "world_azeroth.vdpk");
    CHECK(m.packs[2].recipe == "89abcdef01234567");

    // One byte more, or another key: nothing.
    CHECK(vd::pack::readSignedRelease(json + " ", signature, testKey()).status ==
          SignedRelease::Status::BadSignature);
    CHECK(vd::pack::readSignedRelease(json, "", testKey()).status == SignedRelease::Status::BadSignature);
    std::vector<vd::u8> otherKey = testKey();
    otherKey[10] ^= 1;
    CHECK(vd::pack::readSignedRelease(json, signature, otherKey).status ==
          SignedRelease::Status::BadSignature);
}

TEST_CASE("Release manifest: what a manifest must have") {
    REQUIRE(vd::pack::parseReleaseManifest(manifestJson("0.3.1", 3001, "")));
    const ReleaseManifest m = vd::pack::parseReleaseManifest(manifestJson("0.3.1", 3001, "")).value();
    CHECK(m.versionCode == 3001u);
    CHECK(!m.tooNew());
    CHECK(m.setupWindows.present());
    CHECK(!m.setupMacos.present()); // absent: no Setup for that system in this release
    CHECK(m.packs.empty());

    CHECK(!vd::pack::parseReleaseManifest("not json"));
    CHECK(!vd::pack::parseReleaseManifest("[1, 2]"));
    CHECK(!vd::pack::parseReleaseManifest(manifestJson("0.3.1", 3002, ""))); // versionCode of another version
    CHECK(!vd::pack::parseReleaseManifest(manifestJson("0.3", 3000, "")));   // not X.Y.Z
    std::string badDigest = manifestJson("0.3.1", 3001, "");
    badDigest.replace(badDigest.find(kDigest), 64, std::string(64, 'A')); // uppercase
    CHECK(!vd::pack::parseReleaseManifest(badDigest));
    std::string noUrl = manifestJson("0.3.1", 3001, "");
    noUrl.replace(noUrl.find("https://example.org/a.apk"), 25, "ftp://example.org/a.apk..");
    CHECK(!vd::pack::parseReleaseManifest(noUrl));

    // Another format: only what tells the player where to look, and no error.
    const auto newer = vd::pack::parseReleaseManifest(
        "{\"format\": 2, \"version\": \"1.0.0\", \"notes\": \"https://example.org/n\", \"apk\": 7}");
    REQUIRE(newer);
    CHECK(newer.value().tooNew());
    CHECK(newer.value().version == "1.0.0");
    CHECK(newer.value().versionCode == 1000000u);
    CHECK(newer.value().notes == "https://example.org/n");
    // Unknown keys are ignored.
    std::string extra = manifestJson("0.3.1", 3001, "\"models.vdpk\": \"r1\"");
    extra.insert(1, "\"future\": {\"x\": [1, 2]}, ");
    REQUIRE(vd::pack::parseReleaseManifest(extra));
    CHECK(vd::pack::parseReleaseManifest(extra).value().packs.size() == 1);
}

TEST_CASE("Release manifest: bake steps of pack files") {
    const std::vector<std::string> files = {
        "world_kalimdor.vdpk", "models.vdpk", "world_azeroth.vdpk", "collision_azeroth.vdpk",
        "db.esES.vdpk",        "app.vdpk",    "unknown.bin"};
    const std::vector<StepPlan> steps = vd::pack::bakeStepsFor(files);
    REQUIRE(steps.size() == 4);
    CHECK(steps[0].step == "models");
    CHECK(steps[0].maps.empty());
    CHECK(steps[1].step == "world");
    CHECK(steps[1].maps == std::vector<std::string>{"azeroth", "kalimdor"});
    CHECK(steps[2].step == "collision");
    CHECK(steps[3].step == "locale");
    CHECK(vd::pack::describeSteps(steps) == "models, world (azeroth, kalimdor), collision (azeroth), locale");

    // liquids.vdpk is the whole world step.
    const std::vector<std::string> liquids = {"world_azeroth.vdpk", "liquids.vdpk"};
    const std::vector<StepPlan> world = vd::pack::bakeStepsFor(liquids);
    REQUIRE(world.size() == 1);
    CHECK(world[0].maps.empty());
    CHECK(vd::pack::bakeStepsFor(std::vector<std::string>{}).empty());
}

TEST_CASE("Release manifest: the data a release changes in a packs folder") {
    const std::vector<Recipe> current = {
        {"actors.vdpk", "a1"}, {"db.esES.vdpk", "e1"}, {"models.vdpk", "m1"}, {"world_azeroth.vdpk", "w1"}};
    SUBCASE("same recipes: nothing") {
        const PackManifest folder =
            folderOf({{"actors.vdpk", "a1"}, {"models.vdpk", "m1"}, {"world_azeroth.vdpk", "w1"}});
        CHECK(vd::pack::releaseDataChanges(folder, current, current).empty());
    }
    SUBCASE("a recipe changed, a pack added, a pack the folder lacked before") {
        const PackManifest folder =
            folderOf({{"actors.vdpk", "a1"}, {"world_azeroth.vdpk", "w1"}}); // no models
        const std::vector<Recipe> release = {
            {"actors.vdpk", "a2"}, {"grass.vdpk", "g1"}, {"models.vdpk", "m1"}, {"world_azeroth.vdpk", "w1"}};
        CHECK(vd::pack::releaseDataChanges(folder, release, current) ==
              std::vector<std::string>{"actors.vdpk", "grass.vdpk"});
    }
    SUBCASE("the folder's own recipes count, not this program's") {
        // Baked by an older Setup: models already differs from this program's, and from the release's.
        const PackManifest folder = folderOf({{"actors.vdpk", "a1"}, {"models.vdpk", "m0"}});
        const std::vector<Recipe> release = {{"actors.vdpk", "a1"}, {"models.vdpk", "m1"}};
        CHECK(vd::pack::releaseDataChanges(folder, release, current) ==
              std::vector<std::string>{"models.vdpk"});
    }
    SUBCASE("overlays only with their language") {
        PackManifest folder = folderOf({{"actors.vdpk", "a1"}, {"db.esES.vdpk", "e1"}});
        const std::vector<Recipe> release = {{"actors.vdpk", "a1"}, {"db.esES.vdpk", "e2"}};
        CHECK(vd::pack::releaseDataChanges(folder, release, current).empty()); // not baked with esES
        folder.overlays = {"esES"};
        CHECK(vd::pack::releaseDataChanges(folder, release, current) ==
              std::vector<std::string>{"db.esES.vdpk"});
    }
    SUBCASE("a folder without manifest: by this program's recipes") {
        const PackManifest none;
        const std::vector<Recipe> release = {{"actors.vdpk", "a1"},
                                             {"app.vdpk", "x"},
                                             {"db.esES.vdpk", "e2"},
                                             {"models.vdpk", "m2"},
                                             {"sound.vdpk", "s1"}};
        CHECK(vd::pack::releaseDataChanges(none, release, current) ==
              std::vector<std::string>{"models.vdpk", "sound.vdpk"});
        // A manifest that records app.vdpk alone says nothing about the client packs either.
        const PackManifest appOnly = folderOf({{"app.vdpk", "x"}});
        CHECK(vd::pack::releaseDataChanges(appOnly, release, current) ==
              std::vector<std::string>{"models.vdpk", "sound.vdpk"});
    }
    SUBCASE("empty recipes compare as equal") {
        const PackManifest folder = folderOf({{"actors.vdpk", ""}});
        const std::vector<Recipe> release = {{"actors.vdpk", "a9"}};
        CHECK(vd::pack::releaseDataChanges(folder, release, current).empty());
    }
}
