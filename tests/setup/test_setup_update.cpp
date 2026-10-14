// Phase 22 (updates): VanillaDroid Setup updating itself (SetupUpdate.h): the check's cache, the files it
// replaces and puts back, the leftovers of an update, and a whole check and download against a fake release
// served on 127.0.0.1 (tests/data/updates, signed with the test key).
#include "ChildProcess.h"
#include "HttpServer.h"
#include "SetupText.h"
#include "SetupUpdate.h"
#include "core/ReleaseSignature.h"
#include "core/Text.h" // phase 22 (polish)
#include "core/Sha256.h"
#include "core/Version.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <ostream> // doctest prints std::string values through operator<<
#include <sstream>
#include <string>
#include <vector>

using namespace vd::setup;

namespace {

constexpr vd::u16 kFixturePort = 47931; // the port in tests/data/updates/manifest.json's URLs

std::filesystem::path tempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vd_setup_tests" / name;
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool pathExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

std::filesystem::path fixtureDir() {
    return std::filesystem::path(VD_TEST_DATA_DIR) / "updates";
}

// The built-in release key is still the test key (tests/data/release-test-key.pub.hex): the fixture verifies.
bool builtInKeyIsTestKey() {
    std::string hex = readFile(std::filesystem::path(VD_TEST_DATA_DIR) / "release-test-key.pub.hex");
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r')) {
        hex.pop_back();
    }
    return vd::Sha256::hex(vd::releasePublicKey()) == hex;
}

// A Windows install of the Setup with fake programs, and the staged new one.
struct FakeInstall {
    std::filesystem::path dir;
    InstallLayout layout;
};
FakeInstall fakeInstall(const std::filesystem::path& root) {
    FakeInstall install;
    install.dir = root / "VanillaDroid Setup";
    writeFile(install.dir / executableName("vdsetup"), "old vdsetup");
    writeFile(install.dir / executableName("vd_baker"), "old baker");
    writeFile(install.dir / "VanillaDroid.apk", "old apk");
    writeFile(install.dir / "notes of mine.txt", "the player's own file");
    install.layout = installLayout(install.dir / executableName("vdsetup"));
    return install;
}
std::filesystem::path stagedSetup(const std::filesystem::path& root) {
    const std::filesystem::path staged =
        root / "VanillaDroid Setup" / (std::string(kStagingPrefix) + "0.9.0");
    writeFile(staged / executableName("vdsetup"), "new vdsetup");
    writeFile(staged / executableName("vd_baker"), "new baker");
    writeFile(staged / "VanillaDroid.apk", "new apk");
    writeFile(staged / "TERMS.md", "new terms");
    return staged;
}

} // namespace

TEST_CASE("Setup update: the manifest's URL and the file names of URLs") {
    if constexpr (!vd::kDistBuild) {
        CHECK(updateManifestUrl("http://127.0.0.1:8000/manifest.json") ==
              "http://127.0.0.1:8000/manifest.json");
    }
    CHECK(signatureUrlOf("https://x/manifest.json") == "https://x/manifest.json.sig");
    CHECK(fileNameOfUrl("https://github.com/a/b/releases/download/v0.2.0/VanillaDroid-0.2.0.apk", "x.apk") ==
          "VanillaDroid-0.2.0.apk");
    CHECK(fileNameOfUrl("http://h/VanillaDroidSetup-0.2.0-windows.zip?token=1#f", "x") ==
          "VanillaDroidSetup-0.2.0-windows.zip");
    CHECK(fileNameOfUrl("http://h/", "fallback.zip") == "fallback.zip");
    CHECK(fileNameOfUrl("http://h/..", "fallback.zip") == "fallback.zip");
    CHECK(fileNameOfUrl("http://h/a%2F..%2Fb.zip", "fallback.zip") == "fallback.zip");
    CHECK(fileNameOfUrl("http://h/con fig.zip", "fallback.zip") == "fallback.zip");
}

TEST_CASE("Setup update: the check's cache") {
    const std::filesystem::path dir = tempDir("update-cache");
    CHECK(loadUpdateCache(dir).checkedAt == 0); // none yet
    UpdateCache cache;
    cache.checkedAt = 1000000;
    cache.attemptedAt = 1000000;
    cache.manifestUrl = "https://x/manifest.json";
    cache.version = "0.2.0";
    cache.versionCode = 2000;
    cache.notes = "https://x/v0.2.0";
    REQUIRE(saveUpdateCache(dir, cache));
    const UpdateCache loaded = loadUpdateCache(dir);
    CHECK(loaded.checkedAt == 1000000);
    CHECK(loaded.version == "0.2.0");
    CHECK(loaded.versionCode == 2000u);
    CHECK(loaded.notes == "https://x/v0.2.0");
    CHECK(!loaded.tooNew);

    // Fresh for a few hours after a check that worked, an hour after one that failed, and only for its URL.
    CHECK(loaded.fresh(1000000 + 60, cache.manifestUrl));
    CHECK(loaded.fresh(1000000 + kUpdateCheckSeconds - 1, cache.manifestUrl));
    CHECK(!loaded.fresh(1000000 + kUpdateCheckSeconds, cache.manifestUrl));
    CHECK(!loaded.fresh(1000000 + 60, "http://other/manifest.json"));
    CHECK(!loaded.fresh(1000000 - 60, cache.manifestUrl)); // the clock went back
    UpdateCache failed;
    failed.manifestUrl = cache.manifestUrl;
    failed.attemptedAt = 5000;
    CHECK(failed.fresh(5000 + kUpdateRetrySeconds - 1, cache.manifestUrl));
    CHECK(!failed.fresh(5000 + kUpdateRetrySeconds, cache.manifestUrl));

    writeFile(dir / kUpdateCacheFile, "{ damaged");
    CHECK(loadUpdateCache(dir).checkedAt == 0);
}

TEST_CASE("Setup update: where a Setup is installed") {
    const std::filesystem::path root = tempDir("update-layout");
    const InstallLayout plain = installLayout(root / "Setup" / executableName("vdsetup"));
    CHECK(plain.dir == root / "Setup");
    CHECK(plain.bundle.empty());
    CHECK(!isInstalledSetup(plain)); // no vd_baker next to it: a development build
    writeFile(root / "Setup" / executableName("vd_baker"), "baker");
    CHECK(isInstalledSetup(plain));

    // macOS: the whole .app is what an update swaps.
    const std::filesystem::path app = root / "Apps" / "VanillaDroid Setup.app";
    const InstallLayout bundle = installLayout(app / "Contents" / "MacOS" / "vdsetup");
    CHECK(bundle.bundle == app);
    CHECK(bundle.dir == root / "Apps");
}

TEST_CASE("Setup update: replacing the installed files, and what an update leaves") {
    loadSetupTexts("enUS");
    const std::filesystem::path root = tempDir("update-replace");
    const FakeInstall install = fakeInstall(root);
    const std::filesystem::path staged = stagedSetup(root);
    REQUIRE(checkStagedSetup(staged, install.layout));

    const vd::Result<vd::u32> replaced = replaceInstall(staged, install.layout);
    REQUIRE(replaced);
    CHECK(replaced.value() == 4u);
    const std::filesystem::path& dir = install.dir;
    CHECK(readFile(dir / executableName("vdsetup")) == "new vdsetup");
    CHECK(readFile(dir / executableName("vd_baker")) == "new baker");
    CHECK(readFile(dir / "VanillaDroid.apk") == "new apk");
    CHECK(readFile(dir / "TERMS.md") == "new terms");
    CHECK(readFile(dir / "notes of mine.txt") == "the player's own file"); // not the update's
    // What was replaced waits as <name>.old (a running program cannot be deleted on Windows).
    CHECK(readFile(dir / (executableName("vdsetup") + ".old")) == "old vdsetup");
    CHECK(readFile(dir / "VanillaDroid.apk.old") == "old apk");
    CHECK(!pathExists(dir / "TERMS.md.old")); // nothing was there
    CHECK(!pathExists(staged));

    // A second update: the first one's .old goes (nothing runs from it), the file it replaces takes its name.
    writeFile(dir / "keep-me.old", "not an update's: there is no keep-me");
    const std::filesystem::path again = stagedSetup(root);
    REQUIRE(replaceInstall(again, install.layout));
    CHECK(readFile(dir / (executableName("vdsetup") + ".old")) == "new vdsetup");

    removeUpdateLeftovers(install.layout);
    CHECK(!pathExists(dir / (executableName("vdsetup") + ".old")));
    CHECK(!pathExists(dir / "VanillaDroid.apk.old"));
    CHECK(pathExists(dir / "keep-me.old"));
    CHECK(pathExists(dir / executableName("vdsetup")));
    CHECK(readFile(dir / "notes of mine.txt") == "the player's own file");

    // A staging folder an update left behind goes too.
    writeFile(dir / (std::string(kStagingPrefix) + "1.0.0") / "x", "x");
    removeUpdateLeftovers(install.layout);
    CHECK(!pathExists(dir / (std::string(kStagingPrefix) + "1.0.0")));
}

TEST_CASE("Setup update: a zip without the Setup replaces nothing") {
    loadSetupTexts("enUS");
    const std::filesystem::path root = tempDir("update-badzip");
    const FakeInstall install = fakeInstall(root);
    const std::filesystem::path staged =
        root / "VanillaDroid Setup" / (std::string(kStagingPrefix) + "0.9.0");
    writeFile(staged / executableName("vdsetup"), "new vdsetup"); // no vd_baker
    CHECK(!checkStagedSetup(staged, install.layout));
    std::filesystem::remove_all(staged);
    writeFile(staged / executableName("vd_baker"), "new baker"); // no vdsetup
    CHECK(!checkStagedSetup(staged, install.layout));
}

#if defined(_WIN32)
TEST_CASE("Setup update: a file that cannot be replaced puts everything back") {
    loadSetupTexts("enUS");
    const std::filesystem::path root = tempDir("update-rollback");
    const FakeInstall install = fakeInstall(root);
    const std::filesystem::path staged = stagedSetup(root);
    {
        // Open without FILE_SHARE_DELETE (the C runtime's way): it cannot be renamed, as a program another
        // one runs.
        std::ofstream locked(install.dir / executableName("vd_baker"), std::ios::binary | std::ios::app);
        REQUIRE(locked.is_open());
        const vd::Result<vd::u32> replaced = replaceInstall(staged, install.layout);
        CHECK(!replaced);
    }
    const std::filesystem::path& dir = install.dir;
    CHECK(readFile(dir / executableName("vdsetup")) == "old vdsetup");
    CHECK(readFile(dir / executableName("vd_baker")) == "old baker");
    CHECK(readFile(dir / "VanillaDroid.apk") == "old apk");
    CHECK(!pathExists(dir / "TERMS.md"));
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        CHECK(entry.path().extension() != ".old");
    }
    CHECK(!pathExists(staged));
}
#endif

TEST_CASE("Setup update: a macOS bundle is swapped whole") {
    const std::filesystem::path root = tempDir("update-bundle");
    const std::filesystem::path app = root / "Apps" / "VanillaDroid Setup.app";
    const std::filesystem::path macos = app / "Contents" / "MacOS";
    writeFile(macos / "vdsetup", "old vdsetup");
    writeFile(macos / executableName("vd_baker"), "old baker");
    writeFile(app / "Contents" / "Info.plist", "old plist");
    const InstallLayout layout = installLayout(macos / "vdsetup");
    REQUIRE(isInstalledSetup(layout));
    const std::filesystem::path staged = root / "Apps" / (std::string(kStagingPrefix) + "0.9.0");
    const std::filesystem::path newApp = staged / "VanillaDroid Setup.app" / "Contents";
    writeFile(newApp / "MacOS" / "vdsetup", "new vdsetup");
    writeFile(newApp / "MacOS" / executableName("vd_baker"), "new baker");
    writeFile(newApp / "Info.plist", "new plist");
    writeFile(staged / "TERMS.md", "new terms");
    REQUIRE(checkStagedSetup(staged, layout));
    REQUIRE(replaceInstall(staged, layout));
    CHECK(readFile(macos / "vdsetup") == "new vdsetup");
    CHECK(readFile(app / "Contents" / "Info.plist") == "new plist");
    CHECK(readFile(root / "Apps" / "TERMS.md") == "new terms");
    CHECK(readFile(root / "Apps" / "VanillaDroid Setup.app.old" / "Contents" / "MacOS" / "vdsetup") ==
          "old vdsetup");
    removeUpdateLeftovers(layout);
    CHECK(!pathExists(root / "Apps" / "VanillaDroid Setup.app.old"));
    CHECK(pathExists(app));
}

#if defined(_WIN32)
TEST_CASE("Setup update: the program started again and its exit code") {
    const std::filesystem::path cmd = std::filesystem::path(_wgetenv(L"SystemRoot")) / "System32" / "cmd.exe";
    const vd::Result<int> code = relaunch(cmd, {"/c", "exit 7"}, true);
    REQUIRE(code);
    CHECK(code.value() == 7);
    CHECK(!relaunch(std::filesystem::temp_directory_path() / "no such program.exe", {}, true));
}
#endif

TEST_CASE("Setup update: the APKs a Setup offers") {
    const std::filesystem::path dir = tempDir("update-apks");
    writeFile(downloadedApkDir(dir) / "broken.apk", "not an apk");
    const std::vector<std::filesystem::path> unreadable = {downloadedApkDir(dir) / "broken.apk", {}};
    CHECK(newestApk(unreadable).empty());
    const char* real = std::getenv("VD_TEST_APK"); // a release APK of the development build
    if (real && *real) {
        const std::vector<std::filesystem::path> candidates = {downloadedApkDir(dir) / "broken.apk",
                                                               std::filesystem::path(real)};
        CHECK(newestApk(candidates) == std::filesystem::path(real));
    }
    CHECK(downloadedApks(dir).size() == 1);
    removeOtherApks(dir, downloadedApkDir(dir) / "none.apk");
    CHECK(downloadedApks(dir).empty());
}

TEST_CASE("Setup update: a release checked and downloaded from a local server") {
    if (!builtInKeyIsTestKey()) {
        MESSAGE("the built-in release key is not the test key: the fixture's signature cannot verify");
        return;
    }
    if (findInPath("curl").empty() && !pathExists("C:/Windows/System32/curl.exe")) {
        MESSAGE("no curl");
        return;
    }
    loadSetupTexts("enUS");
    // The fixture's files, as GitHub would serve them; `served` counts the requests, `damage` changes the
    // APK.
    std::map<std::string, std::string> files;
    for (const char* name : {"manifest.json", "manifest.json.sig", "VanillaDroid-999.0.0.apk",
                             "VanillaDroidSetup-999.0.0-windows.zip"}) {
        files[std::string("/") + name] = readFile(fixtureDir() / name);
        REQUIRE(!files[std::string("/") + name].empty());
    }
    std::atomic<int> served{0};
    std::atomic<bool> damage{false};
    std::atomic<bool> badSignature{false};
    HttpServer server;
    const vd::Result<vd::u16> port = server.start(
        kFixturePort, 1,
        [&](const HttpRequest& request, vd::u32) {
            HttpResponse response;
            auto it = files.find(request.path);
            // Phase 22 (polish): a server that fails is not "no release".
            if (request.path == "/broken.json") {
                response.status = 500;
                return response;
            }
            if (it == files.end()) {
                response.status = 404;
                return response;
            }
            ++served;
            response.contentType = "application/octet-stream";
            response.body = it->second;
            if (damage && request.path == "/VanillaDroid-999.0.0.apk") {
                response.body[10] ^= 1;
            }
            if (badSignature && request.path == "/manifest.json.sig") {
                response.body = "MEUCIQDdamaged";
            }
            return response;
        },
        true);
    if (!port) {
        MESSAGE("port " << kFixturePort << " is busy: skipped");
        return;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(kFixturePort) + "/manifest.json";
    const std::filesystem::path data = tempDir("update-release");

    const vd::Result<UpdateCheck> check = checkForUpdate(data, url, {}, 20);
    REQUIRE(check);
    const vd::pack::ReleaseManifest& release = check.value().release.manifest;
    CHECK(check.value().newer);
    CHECK(release.version == "999.0.0");
    CHECK(release.setupWindows.present());
    CHECK(release.packs.size() == 2);
    const UpdateCache cache = loadUpdateCache(data);
    CHECK(cache.version == "999.0.0");
    CHECK(cache.fresh(unixNow(), url));

    // The APK: downloaded and checked; kept when it is already there.
    const std::filesystem::path apk = downloadedApkDir(data) / "VanillaDroid-999.0.0.apk";
    REQUIRE(downloadReleaseFile(release.apk, apk, {}, "apk"));
    CHECK(readFile(apk) == files["/VanillaDroid-999.0.0.apk"]);
    const int before = served;
    REQUIRE(downloadReleaseFile(release.apk, apk, {}, "apk"));
    CHECK(served == before);
    // Changed on the way: refused, and nothing left under the name.
    std::filesystem::remove(apk);
    damage = true;
    CHECK(!downloadReleaseFile(release.apk, apk, {}, "apk"));
    CHECK(!pathExists(apk));
    CHECK(!pathExists(apk.string() + ".part"));
    damage = false;

#if defined(_WIN32)
    // The Setup's zip unpacked next to a fake install and put in its place.
    const FakeInstall install = fakeInstall(tempDir("update-release-install"));
    const std::filesystem::path zip = data / kUpdateFolder / "setup.zip";
    REQUIRE(downloadReleaseFile(release.setupWindows, zip, {}, "setup"));
    const vd::Result<std::filesystem::path> staged = unpackSetup(zip, install.layout, release.version, {});
    REQUIRE(staged);
    REQUIRE(replaceInstall(staged.value(), install.layout));
    CHECK(readFile(install.dir / "vdsetup.exe") == "fake vdsetup 999.0.0\n");
    CHECK(readFile(install.dir / "vd_baker.exe") == "fake vd_baker 999.0.0\n");
    CHECK(readFile(install.dir / "vdsetup.exe.old") == "old vdsetup");
#endif

    // A signature that does not verify: an error, and the cache keeps no release from it.
    badSignature = true;
    const std::filesystem::path other = tempDir("update-release-bad");
    CHECK(!checkForUpdate(other, url, {}, 20));
    CHECK(loadUpdateCache(other).checkedAt == 0);
    CHECK(loadUpdateCache(other).attemptedAt > 0);
    // Phase 22 (polish): nothing at the URL (GitHub's answer before the first release) is no update, not an
    // error: a check that worked, and `status` says this is the latest version.
    const std::string missing = "http://127.0.0.1:" + std::to_string(kFixturePort) + "/missing.json";
    const vd::Result<UpdateCheck> none = checkForUpdate(other, missing, {}, 20);
    REQUIRE(none);
    CHECK(!none.value().published);
    CHECK(!none.value().newer);
    const UpdateCache noneCache = loadUpdateCache(other);
    CHECK(noneCache.checkedAt > 0);
    CHECK(noneCache.versionCode == 0u);
    CHECK(noneCache.fresh(unixNow(), missing));
    CHECK(updateStatusMessage(noneCache, missing).key == "VD_SETUP_STATUS_UPDATE_NONE");
    // A server that fails, or none at all: errors, and nothing recorded as checked.
    const std::filesystem::path failing = tempDir("update-release-failing");
    const std::string broken = "http://127.0.0.1:" + std::to_string(kFixturePort) + "/broken.json";
    CHECK(!checkForUpdate(failing, broken, {}, 20));
    CHECK(loadUpdateCache(failing).checkedAt == 0);
    server.stop();
    CHECK(!checkForUpdate(failing, url, {}, 20));
    CHECK(loadUpdateCache(failing).checkedAt == 0);
}

TEST_CASE("Setup update: what status and the window say about the last check") {
    const std::string url = "https://x/manifest.json";
    UpdateCache cache;
    CHECK(updateStatusMessage(cache, url).key == "VD_SETUP_STATUS_UPDATE_UNKNOWN"); // never checked
    cache.manifestUrl = url;
    cache.attemptedAt = 100;
    CHECK(updateStatusMessage(cache, url).key == "VD_SETUP_STATUS_UPDATE_UNKNOWN"); // only failed checks
    cache.checkedAt = 100;
    CHECK(updateStatusMessage(cache, url).key == "VD_SETUP_STATUS_UPDATE_NONE"); // no release yet
    cache.version = vd::kVersion;
    cache.versionCode = vd::kVersionCode;
    CHECK(updateStatusMessage(cache, url).key == "VD_SETUP_STATUS_UPDATE_NONE"); // this one
    CHECK(updateStatusMessage(cache, url).level == Message::Level::Info);
    cache.version = "999.0.0";
    cache.versionCode = 999000000;
    const Message available = updateStatusMessage(cache, url);
    CHECK(available.key == "VD_SETUP_STATUS_UPDATE_AVAILABLE");
    CHECK(available.level == Message::Level::Warning);
    REQUIRE(available.args.size() == 1);
    CHECK(available.args[0] == "999.0.0");
    cache.tooNew = true;
    CHECK(updateStatusMessage(cache, url).key == "VD_SETUP_STATUS_UPDATE_TOO_NEW");
    cache.versionCode = 0; // a format this program cannot read: its version may be unknown, still newer
    CHECK(updateStatusMessage(cache, url).key == "VD_SETUP_STATUS_UPDATE_TOO_NEW");
    CHECK(updateStatusMessage(cache, url).level == Message::Level::Warning);
    CHECK(updateStatusMessage(cache, "https://other/manifest.json").key == "VD_SETUP_STATUS_UPDATE_UNKNOWN");

    // The same keys in both, each in its own words: the console's names the command, the window's does not.
    loadSetupTexts("enUS");
    const std::string console = format(available);
    loadSetupTexts("enUS", TextSurface::Window);
    const std::string window = format(available);
    CHECK(console.find("999.0.0") != std::string::npos);
    CHECK(console.find("vdsetup update") != std::string::npos);
    CHECK(window.find("999.0.0") != std::string::npos);
    CHECK(window.find("vdsetup") == std::string::npos);
    vd::text::unload();
}
