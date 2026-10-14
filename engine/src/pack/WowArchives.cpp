#include "pack/WowArchives.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace vd::pack {
namespace {

// The enUS and esES 1.12.1 clients hold the same 14 archives; their sizes differ by language (only fonts.MPQ
// is the same), so sizes are not checked against a list: each header is checked against its own file.
constexpr std::array<std::string_view, 12> kGameArchives = {
    "dbc.MPQ",     "fonts.MPQ", "interface.MPQ", "misc.MPQ",    "model.MPQ",   "patch.MPQ",
    "patch-2.MPQ", "sound.MPQ", "speech.MPQ",    "terrain.MPQ", "texture.MPQ", "wmo.MPQ"};
constexpr std::array<std::string_view, 2> kLauncherArchives = {"backup.MPQ", "base.MPQ"};

constexpr usize kHeadBytes = 64 * 1024;
constexpr u32 kMpqSignature = 0x1A51504Du;      // "MPQ\x1A"
constexpr u32 kUserDataSignature = 0x1B51504Du; // "MPQ\x1B"

char lowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

u32 readU32(std::span<const u8> bytes, usize offset) {
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    return static_cast<u32>(bytes[offset]) | (static_cast<u32>(bytes[offset + 1]) << 8) |
           (static_cast<u32>(bytes[offset + 2]) << 16) | (static_cast<u32>(bytes[offset + 3]) << 24);
}

} // namespace

std::span<const std::string_view> wowGameArchives() {
    return kGameArchives;
}

bool isWowGameArchive(std::string_view fileName) {
    return std::any_of(kGameArchives.begin(), kGameArchives.end(),
                       [&](std::string_view name) { return equalsNoCase(name, fileName); });
}

bool isWowLauncherArchive(std::string_view fileName) {
    return std::any_of(kLauncherArchives.begin(), kLauncherArchives.end(),
                       [&](std::string_view name) { return equalsNoCase(name, fileName); });
}

MpqState inspectMpqHeader(std::span<const u8> head, u64 fileSize) {
    // The header is at offset 0, or where a user-data block ("MPQ\x1B") says (always a multiple of 512).
    u64 offset = 0;
    u32 signature = readU32(head, 0);
    if (signature == kUserDataSignature) {
        offset = readU32(head, 8); // dwHeaderOffs
        signature = offset + 32 <= head.size() ? readU32(head, static_cast<usize>(offset)) : 0;
    }
    if (signature != kMpqSignature) {
        return MpqState::NotMpq;
    }
    const usize at = static_cast<usize>(offset);
    if (at + 32 > head.size()) {
        return fileSize < offset + 32 ? MpqState::Truncated : MpqState::NotMpq;
    }
    const u32 headerSize = readU32(head, at + 4);
    const u32 archiveSize = readU32(head, at + 8);
    const u32 hashTable = readU32(head, at + 16);
    const u32 blockTable = readU32(head, at + 20);
    const u32 hashEntries = readU32(head, at + 24);
    const u32 blockEntries = readU32(head, at + 28);
    if (headerSize < 32) {
        return MpqState::NotMpq;
    }
    const u64 available = fileSize > offset ? fileSize - offset : 0;
    // 1.12's archives (format 0 and 1, < 4 GB) state their size; the two tables must fit too.
    if (u64{archiveSize} > available || u64{hashTable} + u64{hashEntries} * 16 > available ||
        u64{blockTable} + u64{blockEntries} * 16 > available) {
        return MpqState::Truncated;
    }
    return MpqState::Ok;
}

MpqState inspectMpqFile(const std::filesystem::path& file) {
    std::error_code error;
    const u64 size = std::filesystem::file_size(file, error);
    if (error) {
        return MpqState::Unreadable;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return MpqState::Unreadable;
    }
    std::vector<u8> head(static_cast<usize>(std::min<u64>(size, kHeadBytes)));
    if (!in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()))) {
        return MpqState::Unreadable;
    }
    return inspectMpqHeader(head, size);
}

std::filesystem::path findWowDataDir(const std::filesystem::path& clientDir) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(clientDir, error)) {
        if (entry.is_directory(error) && equalsNoCase(entry.path().filename().string(), "Data")) {
            return entry.path();
        }
    }
    return {};
}

WowDataCheck checkWowData(const std::filesystem::path& dataDir) {
    WowDataCheck check;
    std::vector<std::string> present;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(dataDir, error)) {
        const std::string name = entry.path().filename().string();
        if (name.size() <= 4 || !equalsNoCase(std::string_view(name).substr(name.size() - 4), ".mpq") ||
            !entry.is_regular_file(error)) {
            continue;
        }
        ++check.archives;
        if (isWowLauncherArchive(name)) {
            continue; // official, never read
        }
        if (!isWowGameArchive(name)) {
            check.unofficial.push_back(name);
            continue;
        }
        present.push_back(name);
        if (inspectMpqFile(entry.path()) != MpqState::Ok) {
            check.damaged.push_back(name);
        } else {
            check.gameBytes += entry.file_size(error);
        }
    }
    for (const std::string_view archive : kGameArchives) {
        if (std::none_of(present.begin(), present.end(),
                         [&](const std::string& name) { return equalsNoCase(name, archive); })) {
            check.missing.emplace_back(archive);
        }
    }
    std::sort(check.damaged.begin(), check.damaged.end());
    std::sort(check.unofficial.begin(), check.unofficial.end());
    return check;
}

} // namespace vd::pack
