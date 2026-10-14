#include "WowCheck.h"

#include "bake/LocaleBaker.h"
#include "core/Locale.h"
#include "core/Sha256.h"
#include "formats/MpqArchiveSet.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>

namespace vd::setup {
namespace {

// The enUS and esES 1.12.1 clients hold the same 14 archives; their sizes differ by language (only fonts.MPQ
// is the same), so sizes are not checked against a list: each header is checked against its own file.
constexpr std::array<std::string_view, 12> kGameArchives = {
    "dbc.MPQ",     "fonts.MPQ", "interface.MPQ", "misc.MPQ",    "model.MPQ",   "patch.MPQ",
    "patch-2.MPQ", "sound.MPQ", "speech.MPQ",    "terrain.MPQ", "texture.MPQ", "wmo.MPQ"};
// Only the launcher's own files (.exe, .html, .url; docs/FORMATS.md): official, but nothing the baker reads.
constexpr std::array<std::string_view, 2> kLauncherArchives = {"backup.MPQ", "base.MPQ"};

constexpr usize kHeadBytes = 64 * 1024;         // the MPQ header is looked for in 512-byte steps up to here
constexpr u32 kMpqSignature = 0x1A51504Du;      // "MPQ\x1A"
constexpr u32 kUserDataSignature = 0x1B51504Du; // "MPQ\x1B"

u32 readU32(std::span<const u8> bytes, usize offset) {
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    return static_cast<u32>(bytes[offset]) | (static_cast<u32>(bytes[offset + 1]) << 8) |
           (static_cast<u32>(bytes[offset + 2]) << 16) | (static_cast<u32>(bytes[offset + 3]) << 24);
}

bool contains(std::span<const std::string> names, std::string_view name) {
    return std::any_of(names.begin(), names.end(),
                       [&](const std::string& n) { return equalsNoCase(n, name); });
}

Message message(Message::Level level, std::string key, std::vector<std::string> args = {}) {
    return Message{level, std::move(key), std::move(args)};
}

std::string readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

std::span<const std::string_view> officialGameArchives() {
    return kGameArchives;
}

std::span<const std::string_view> officialLauncherArchives() {
    return kLauncherArchives;
}

ArchiveKind archiveKind(std::string_view fileName) {
    for (const std::string_view name : kGameArchives) {
        if (equalsNoCase(name, fileName)) {
            return ArchiveKind::Game;
        }
    }
    for (const std::string_view name : kLauncherArchives) {
        if (equalsNoCase(name, fileName)) {
            return ArchiveKind::Launcher;
        }
    }
    return ArchiveKind::Unofficial;
}

ArchiveState inspectMpqHeader(std::span<const u8> head, u64 fileSize) {
    // The header is at offset 0, or where a user-data block ("MPQ\x1B") says (always a multiple of 512).
    u64 offset = 0;
    u32 signature = readU32(head, 0);
    if (signature == kUserDataSignature) {
        offset = readU32(head, 8); // dwHeaderOffs
        signature = offset + 32 <= head.size() ? readU32(head, static_cast<usize>(offset)) : 0;
    }
    if (signature != kMpqSignature) {
        return ArchiveState::NotMpq;
    }
    const usize at = static_cast<usize>(offset);
    if (at + 32 > head.size()) {
        return fileSize < offset + 32 ? ArchiveState::Truncated : ArchiveState::NotMpq;
    }
    const u32 headerSize = readU32(head, at + 4);
    const u32 archiveSize = readU32(head, at + 8);
    const u32 hashTable = readU32(head, at + 16);
    const u32 blockTable = readU32(head, at + 20);
    const u32 hashEntries = readU32(head, at + 24);
    const u32 blockEntries = readU32(head, at + 28);
    if (headerSize < 32) {
        return ArchiveState::NotMpq;
    }
    const u64 available = fileSize > offset ? fileSize - offset : 0;
    // 1.12's archives (format 0 and 1, < 4 GB) state their size; the two tables must fit too.
    if (u64{archiveSize} > available || u64{hashTable} + u64{hashEntries} * 16 > available ||
        u64{blockTable} + u64{blockEntries} * 16 > available) {
        return ArchiveState::Truncated;
    }
    return ArchiveState::Ok;
}

ArchiveState inspectMpqFile(const std::filesystem::path& file) {
    std::error_code error;
    const u64 size = std::filesystem::file_size(file, error);
    if (error) {
        return ArchiveState::Unreadable;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return ArchiveState::Unreadable;
    }
    std::vector<u8> head(static_cast<usize>(std::min<u64>(size, kHeadBytes)));
    if (!in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()))) {
        return ArchiveState::Unreadable;
    }
    return inspectMpqHeader(head, size);
}

bool isSupportedLocale(std::string_view locale) {
    static constexpr std::string_view kSupported[] = {"enUS", "enGB", "esES", "esMX", "frFR", "deDE"};
    return std::any_of(std::begin(kSupported), std::end(kSupported),
                       [&](std::string_view s) { return equalsNoCase(s, locale); });
}

std::vector<std::string> ClientCheck::skippedArchives(std::span<const std::string> include) const {
    std::vector<std::string> skipped;
    for (const ArchiveInfo& archive : archives) {
        if (archive.kind == ArchiveKind::Unofficial && !contains(include, archive.name)) {
            skipped.push_back(archive.name);
        }
    }
    return skipped;
}

std::vector<ArchiveInfo> ClientCheck::gameArchives() const {
    std::vector<ArchiveInfo> game;
    for (const ArchiveInfo& archive : archives) {
        if (archive.kind == ArchiveKind::Game && archive.state == ArchiveState::Ok) {
            game.push_back(archive);
        }
    }
    return game;
}

u64 ClientCheck::gameBytes() const {
    u64 total = 0;
    for (const ArchiveInfo& archive : gameArchives()) {
        total += archive.size;
    }
    return total;
}

ClientCheck checkClient(const std::filesystem::path& chosen, std::span<const std::string> include) {
    using Level = Message::Level;
    // "C:/WoW/" -> "C:/WoW": the folder's own name decides below whether it is Data itself.
    const std::filesystem::path folder =
        !chosen.has_filename() && chosen.has_relative_path() ? chosen.parent_path() : chosen;
    ClientCheck check;
    check.folder = folder;
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) {
        check.messages.push_back(message(Level::Error, "VD_SETUP_WOW_NO_FOLDER", {utf8FromPath(folder)}));
        return check;
    }
    check.dataDir = findChildNoCase(folder, "Data");
    if (check.dataDir.empty() && equalsNoCase(utf8FromPath(folder.filename()), "Data") &&
        folder.has_parent_path()) {
        // The player chose the Data folder itself.
        check.dataDir = folder;
        check.folder = folder.parent_path();
    }
    if (check.dataDir.empty() || !std::filesystem::is_directory(check.dataDir, error)) {
        check.dataDir.clear();
        check.messages.push_back(message(Level::Error, "VD_SETUP_WOW_NO_DATA", {utf8FromPath(folder)}));
        return check;
    }

    // The archives.
    for (const auto& entry : std::filesystem::directory_iterator(check.dataDir, error)) {
        const std::string name = utf8FromPath(entry.path().filename());
        if (!entry.is_regular_file(error) || name.size() <= 4 ||
            !equalsNoCase(name.substr(name.size() - 4), ".mpq")) {
            continue;
        }
        ArchiveInfo archive;
        archive.name = name;
        archive.size = entry.file_size(error);
        archive.kind = archiveKind(name);
        archive.state = inspectMpqFile(entry.path());
        check.archives.push_back(std::move(archive));
    }
    std::sort(check.archives.begin(), check.archives.end(), [](const ArchiveInfo& a, const ArchiveInfo& b) {
        return toLowerAscii(a.name) < toLowerAscii(b.name);
    });
    bool fatal = false;
    for (const std::string_view name : kGameArchives) {
        const bool present = std::any_of(check.archives.begin(), check.archives.end(),
                                         [&](const ArchiveInfo& a) { return equalsNoCase(a.name, name); });
        if (!present) {
            check.missing.emplace_back(name);
            check.messages.push_back(
                message(Level::Error, "VD_SETUP_WOW_MISSING_ARCHIVE", {std::string(name)}));
            fatal = true;
        }
    }
    for (const ArchiveInfo& archive : check.archives) {
        const bool baked = archive.kind != ArchiveKind::Unofficial || contains(include, archive.name);
        if (archive.state != ArchiveState::Ok && baked) {
            const char* key = archive.state == ArchiveState::Truncated ? "VD_SETUP_WOW_TRUNCATED_ARCHIVE"
                                                                       : "VD_SETUP_WOW_DAMAGED_ARCHIVE";
            check.messages.push_back(message(
                archive.kind == ArchiveKind::Launcher ? Level::Warning : Level::Error, key, {archive.name}));
            fatal |= archive.kind != ArchiveKind::Launcher;
        }
        if (archive.kind == ArchiveKind::Unofficial) {
            check.messages.push_back(message(
                baked ? Level::Info : Level::Warning,
                baked ? "VD_SETUP_WOW_UNOFFICIAL_INCLUDED" : "VD_SETUP_WOW_UNOFFICIAL", {archive.name}));
        }
    }

    // The version: WoW.exe's resource, a Mac client's Info.plist, or neither (a copy of Data alone).
    const std::filesystem::path exe = findChildNoCase(check.folder, "WoW.exe");
    if (!exe.empty()) {
        check.hasExe = true;
        Result<FileVersion> version = readPeFileVersionFile(exe);
        if (!version) {
            check.messages.push_back(
                message(Level::Warning, "VD_SETUP_WOW_VERSION_UNREADABLE", {version.error().message}));
        } else {
            check.exeVersionRead = true;
            check.exeVersion = version.value();
            if (check.exeVersion == kClientVersion) {
                check.messages.push_back(
                    message(Level::Info, "VD_SETUP_WOW_VERSION_OK", {check.exeVersion.toString()}));
            } else {
                check.messages.push_back(
                    message(Level::Error, "VD_SETUP_WOW_VERSION_WRONG", {check.exeVersion.toString()}));
                fatal = true;
            }
        }
    } else {
        std::filesystem::path plist;
        for (const auto& entry : std::filesystem::directory_iterator(check.folder, error)) {
            const std::string name = toLowerAscii(utf8FromPath(entry.path().filename()));
            if (entry.is_directory(error) && name.size() > 4 && name.ends_with(".app") &&
                name.find("world of warcraft") != std::string::npos) {
                plist = entry.path() / "Contents" / "Info.plist";
                break;
            }
        }
        const std::string text = plist.empty() ? std::string() : readText(plist);
        check.macVersion = plistString(text, "CFBundleVersion");
        if (check.macVersion.empty()) {
            check.macVersion = plistString(text, "CFBundleShortVersionString");
        }
        if (check.macVersion.empty()) {
            check.messages.push_back(message(Level::Warning, "VD_SETUP_WOW_NO_EXE"));
        } else if (check.macVersion.find("1.12.1") != std::string::npos ||
                   check.macVersion.find("5875") != std::string::npos) {
            check.messages.push_back(message(Level::Info, "VD_SETUP_WOW_VERSION_OK", {check.macVersion}));
        } else {
            check.messages.push_back(message(Level::Error, "VD_SETUP_WOW_VERSION_WRONG", {check.macVersion}));
            fatal = true;
        }
    }

    // The language, from the DBC texts of the archives the baker will read. Only dbc.MPQ and the patches can
    // hold DBFilesClient: the big archives (textures, terrain...) are left closed, which saves seconds.
    if (!fatal) {
        std::vector<std::string> skipped = check.skippedArchives(include);
        for (const ArchiveInfo& archive : check.archives) {
            const std::string name = toLowerAscii(archive.name);
            if (archive.kind != ArchiveKind::Unofficial && name != "dbc.mpq" && name.rfind("patch", 0) != 0) {
                skipped.push_back(archive.name);
            }
        }
        formats::MpqArchiveSet mpq;
        if (Result<void> opened = mpq.open(check.dataDir, skipped); !opened) {
            check.messages.push_back(
                message(Level::Error, "VD_SETUP_WOW_CANNOT_OPEN", {opened.error().message}));
            fatal = true;
        } else {
            check.locale = bake::detectClientLocale(mpq);
            if (check.locale.empty()) {
                check.messages.push_back(message(Level::Error, "VD_SETUP_WOW_LOCALE_UNKNOWN"));
                fatal = true;
            } else if (!isSupportedLocale(check.locale)) {
                check.messages.push_back(
                    message(Level::Error, "VD_SETUP_WOW_LOCALE_UNSUPPORTED", {check.locale}));
                fatal = true;
            } else {
                check.messages.push_back(message(Level::Info, "VD_SETUP_WOW_LOCALE", {check.locale}));
            }
        }
    }
    check.usable = !fatal;
    if (check.usable) {
        check.messages.push_back(message(Level::Info, "VD_SETUP_WOW_OK", {formatBytes(check.gameBytes())}));
    }
    return check;
}

std::string bakeFingerprint(const ClientCheck& base, const ClientCheck* localeClient,
                            std::span<const std::string> include) {
    std::string text;
    auto add = [&](const ClientCheck& client) {
        text += client.locale;
        text += ':';
        const std::vector<std::string> skipped = client.skippedArchives(include);
        for (const ArchiveInfo& archive : client.archives) {
            if (!contains(skipped, archive.name)) {
                text += toLowerAscii(archive.name) + '=' + std::to_string(archive.size) + ';';
            }
        }
    };
    add(base);
    if (localeClient) {
        text += '|';
        add(*localeClient);
    }
    return sha256Hex(std::span(reinterpret_cast<const u8*>(text.data()), text.size())).substr(0, 32);
}

} // namespace vd::setup
