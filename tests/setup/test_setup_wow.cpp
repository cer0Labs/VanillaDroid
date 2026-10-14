// Phase 22 (setup): checking the player's WoW folder: archives, version, language (WowCheck.h, PeVersion.h).
#include "PeVersion.h"
#include "SetupCommon.h"
#include "WowCheck.h"
#include "bake/LocaleBaker.h"
#include "formats/MpqArchiveSet.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ostream> // doctest prints std::string values through operator<<
#include <string>
#include <vector>

using namespace vd::setup;

namespace {

std::filesystem::path tempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vd_setup_tests" / name;
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

void put32(std::vector<vd::u8>& bytes, size_t at, vd::u32 value) {
    for (int i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<vd::u8>(value >> (8 * i));
    }
}

// The first bytes of an MPQ (format 0 header) whose sizes say `archiveSize`.
std::vector<vd::u8> mpqHead(vd::u32 archiveSize, vd::u32 tables = 0x100) {
    std::vector<vd::u8> head(1024, 0);
    std::memcpy(head.data(), "MPQ\x1A", 4);
    put32(head, 4, 32);
    put32(head, 8, archiveSize);
    put32(head, 16, tables);      // hash table offset
    put32(head, 20, tables + 64); // block table offset
    put32(head, 24, 4);
    put32(head, 28, 4);
    return head;
}

void writeBytes(const std::filesystem::path& path, const std::vector<vd::u8>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool hasMessage(const ClientCheck& check, std::string_view key, std::string_view arg = {}) {
    for (const Message& message : check.messages) {
        if (message.key == key && (arg.empty() || (!message.args.empty() && message.args[0] == arg))) {
            return true;
        }
    }
    return false;
}

std::filesystem::path wowDir() {
    const char* dir = std::getenv("VD_WOW_DIR");
    return dir && *dir ? std::filesystem::path(dir) : std::filesystem::path();
}

} // namespace

TEST_CASE("Setup WoW: archive kinds") {
    CHECK(officialGameArchives().size() == 12);
    CHECK(archiveKind("patch.MPQ") == ArchiveKind::Game);
    CHECK(archiveKind("PATCH-2.mpq") == ArchiveKind::Game);
    CHECK(archiveKind("texture.MPQ") == ArchiveKind::Game);
    CHECK(archiveKind("backup.MPQ") == ArchiveKind::Launcher);
    CHECK(archiveKind("base.mpq") == ArchiveKind::Launcher);
    CHECK(archiveKind("patch-3.MPQ") == ArchiveKind::Unofficial);
    CHECK(archiveKind("patch-A.MPQ") == ArchiveKind::Unofficial);
    CHECK(archiveKind("patch-enUS.MPQ") == ArchiveKind::Unofficial);
    CHECK(isSupportedLocale("enUS"));
    CHECK(isSupportedLocale("esmx"));
    CHECK(isSupportedLocale("deDE"));
    CHECK_FALSE(isSupportedLocale("koKR"));
    CHECK_FALSE(isSupportedLocale("zhCN"));
    CHECK_FALSE(isSupportedLocale("zhTW"));
}

TEST_CASE("Setup WoW: MPQ headers catch copies cut short") {
    const std::vector<vd::u8> head = mpqHead(4096);
    CHECK(inspectMpqHeader(head, 4096) == ArchiveState::Ok);
    CHECK(inspectMpqHeader(head, 5000) == ArchiveState::Ok);
    CHECK(inspectMpqHeader(head, 4095) == ArchiveState::Truncated);
    CHECK(inspectMpqHeader(mpqHead(1024, 2000), 1024) == ArchiveState::Truncated); // the tables past the end
    std::vector<vd::u8> other = head;
    std::memcpy(other.data(), "PK\x03\x04", 4);
    CHECK(inspectMpqHeader(other, 4096) == ArchiveState::NotMpq);
    // A user-data block first ("MPQ\x1B"), the header 512 bytes in.
    std::vector<vd::u8> user(2048, 0);
    std::memcpy(user.data(), "MPQ\x1B", 4);
    put32(user, 8, 512);
    const std::vector<vd::u8> inner = mpqHead(1024);
    std::memcpy(user.data() + 512, inner.data(), 32);
    CHECK(inspectMpqHeader(user, 512 + 1024) == ArchiveState::Ok);
    CHECK(inspectMpqHeader(user, 512 + 1000) == ArchiveState::Truncated);
}

TEST_CASE("Setup WoW: a folder that is not a client") {
    const std::filesystem::path dir = tempDir("wow-fake");
    CHECK(hasMessage(checkClient(dir / "missing"), "VD_SETUP_WOW_NO_FOLDER"));
    const ClientCheck noData = checkClient(dir);
    CHECK_FALSE(noData.usable);
    CHECK(hasMessage(noData, "VD_SETUP_WOW_NO_DATA"));

    // A lowercase "data" with some archives: whole, cut short, one of a private server.
    const std::filesystem::path data = dir / "data";
    std::filesystem::create_directories(data);
    writeBytes(data / "dbc.MPQ", mpqHead(1024));
    std::vector<vd::u8> cut = mpqHead(1u << 20);
    writeBytes(data / "texture.MPQ", cut); // says 1 MB, has 1 KB
    writeBytes(data / "patch-A.MPQ", mpqHead(1024));
    writeBytes(data / "readme.txt", {1, 2, 3});
    const ClientCheck check = checkClient(dir, {});
    CHECK_FALSE(check.usable);
    CHECK(check.dataDir.filename() == "data");
    REQUIRE(check.archives.size() == 3);
    CHECK(check.missing.size() == 10);
    CHECK(hasMessage(check, "VD_SETUP_WOW_MISSING_ARCHIVE", "patch.MPQ"));
    CHECK(hasMessage(check, "VD_SETUP_WOW_TRUNCATED_ARCHIVE", "texture.MPQ"));
    CHECK(hasMessage(check, "VD_SETUP_WOW_UNOFFICIAL", "patch-A.MPQ"));
    CHECK(hasMessage(check, "VD_SETUP_WOW_NO_EXE"));
    CHECK(check.skippedArchives({}) == std::vector<std::string>{"patch-A.MPQ"});
    const std::vector<std::string> include = {"PATCH-a.mpq"};
    CHECK(check.skippedArchives(include).empty());
    CHECK(hasMessage(checkClient(dir, include), "VD_SETUP_WOW_UNOFFICIAL_INCLUDED", "patch-A.MPQ"));
    CHECK(bakeFingerprint(check, nullptr, {}) != bakeFingerprint(check, nullptr, include));
    CHECK(bakeFingerprint(check, nullptr, {}) == bakeFingerprint(check, nullptr, {}));

    // Chosen by its Data folder: the client is its parent.
    CHECK(checkClient(data).folder == dir);
    // The baker finds the lowercase folder too.
    CHECK(vd::formats::findDataDir(dir) == data);
    CHECK(vd::formats::findDataDir(dir / "missing") == dir / "missing" / "Data");
}

TEST_CASE("Setup WoW: PE version resources and Info.plist") {
    CHECK_FALSE(readPeFileVersion(std::vector<vd::u8>(100, 0)).ok());
    std::vector<vd::u8> mz(512, 0);
    mz[0] = 'M';
    mz[1] = 'Z';
    CHECK_FALSE(readPeFileVersion(mz).ok());
    CHECK((FileVersion{1, 12, 1, 5875}.toString()) == "1.12.1.5875");
    const std::string plist = "<dict><key>CFBundleName</key><string>World of Warcraft</string>"
                              "<key>CFBundleVersion</key>\n  <string>1.12.1</string></dict>";
    CHECK(plistString(plist, "CFBundleVersion") == "1.12.1");
    CHECK(plistString(plist, "CFBundleName") == "World of Warcraft");
    CHECK(plistString(plist, "CFBundleShortVersionString").empty());

    const std::filesystem::path wow = wowDir();
    if (wow.empty() || !std::filesystem::exists(wow / "WoW.exe")) {
        MESSAGE("VD_WOW_DIR not set: WoW.exe's version not checked");
        return;
    }
    const vd::Result<FileVersion> version = readPeFileVersionFile(wow / "WoW.exe");
    REQUIRE(version.ok());
    CHECK(version.value() == kClientVersion);
}

TEST_CASE("Setup WoW: the real clients") {
    const std::filesystem::path wow = wowDir();
    if (wow.empty() || !std::filesystem::exists(wow)) {
        MESSAGE("VD_WOW_DIR not set: skipped");
        return;
    }
    const ClientCheck check = checkClient(wow);
    for (const Message& message : check.messages) {
        INFO(message.key);
    }
    CHECK(check.usable);
    CHECK(check.hasExe);
    CHECK(check.exeVersion == kClientVersion);
    CHECK(check.locale == "enUS");
    CHECK(check.missing.empty());
    CHECK(check.archives.size() == 14);
    CHECK(check.gameArchives().size() == 12);
    CHECK(check.skippedArchives({}).empty());
    CHECK(check.gameBytes() > 5000000000ull);
    for (const ArchiveInfo& archive : check.archives) {
        CHECK_MESSAGE(archive.state == ArchiveState::Ok, archive.name);
        CHECK_MESSAGE(archive.kind != ArchiveKind::Unofficial, archive.name);
    }
    // The Spanish client next to it, when it is there.
    const std::filesystem::path spanish = wow.parent_path() / (wow.filename().string() + " esES");
    if (std::filesystem::exists(spanish)) {
        const ClientCheck es = checkClient(spanish);
        CHECK(es.usable);
        CHECK(es.locale == "esES");
        CHECK(es.exeVersion == kClientVersion);
        CHECK(bakeFingerprint(check, &es, {}) != bakeFingerprint(check, nullptr, {}));
    }
    // The baker's --skip-archives: those archives are not opened; the language is still found.
    vd::formats::MpqArchiveSet mpq;
    const std::vector<std::string> skip = {"BACKUP.mpq", "base.MPQ"};
    REQUIRE(mpq.open(check.dataDir, skip).ok());
    CHECK(mpq.archiveNames().size() == 12);
    CHECK(vd::bake::detectClientLocale(mpq) == "enUS");
}
