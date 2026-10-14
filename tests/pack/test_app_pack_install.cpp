// Phase 22 (apppack): app.vdpk copied from the APK to the app's internal storage (pack/AppPack's
// installAppPack): only when it is missing or the APK changed, through a temporary file.
#include "pack/AppPack.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace vd;

struct Source {
    std::string bytes;
    int copies = 0;
    bool fail = false;
};

bool writeSource(void* user, std::FILE* out) {
    Source& source = *static_cast<Source*>(user);
    ++source.copies;
    if (source.fail) {
        std::fwrite(source.bytes.data(), 1, source.bytes.size() / 2, out); // cut short
        return false;
    }
    return std::fwrite(source.bytes.data(), 1, source.bytes.size(), out) == source.bytes.size();
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("App pack: copied to internal storage when missing or when the APK changes") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vd_test_app_install";
    std::error_code error;
    std::filesystem::remove_all(dir, error);

    Source source{"VDPK app pack v1"};
    bool copied = false;
    Result<std::filesystem::path> installed =
        pack::installAppPack(dir, "1000 1", &writeSource, &source, &copied);
    REQUIRE(installed.ok());
    CHECK(copied);
    CHECK(installed.value() == dir / pack::kAppPackFile);
    CHECK(readFile(installed.value()) == source.bytes);

    // The same APK: nothing copied.
    REQUIRE(pack::installAppPack(dir, "1000 1", &writeSource, &source, &copied).ok());
    CHECK_FALSE(copied);
    CHECK(source.copies == 1);

    // A new APK (versionCode or install time): copied again.
    source.bytes = "VDPK app pack v2, longer";
    REQUIRE(pack::installAppPack(dir, "1001 2", &writeSource, &source, &copied).ok());
    CHECK(copied);
    CHECK(readFile(dir / pack::kAppPackFile) == source.bytes);

    // A copy cut short keeps the previous pack and leaves no temporary file; the next start tries again.
    const std::string previous = source.bytes;
    source.bytes = "VDPK app pack v3";
    source.fail = true;
    CHECK_FALSE(pack::installAppPack(dir, "1002 3", &writeSource, &source, &copied).ok());
    CHECK_FALSE(copied);
    CHECK(readFile(dir / pack::kAppPackFile) == previous);
    CHECK_FALSE(std::filesystem::exists(dir / "app.vdpk.tmp", error));
    source.fail = false;
    REQUIRE(pack::installAppPack(dir, "1002 3", &writeSource, &source, &copied).ok());
    CHECK(copied);
    CHECK(readFile(dir / pack::kAppPackFile) == source.bytes);

    // A pack deleted by hand (the stamp still there): copied again.
    std::filesystem::remove(dir / pack::kAppPackFile, error);
    REQUIRE(pack::installAppPack(dir, "1002 3", &writeSource, &source, &copied).ok());
    CHECK(copied);
    std::filesystem::remove_all(dir, error);
}
