#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// Phase 22 (ADR-0005 §4): the version of a client without running it. WoW.exe (a 32-bit PE) carries it in its
// version resource (VS_FIXEDFILEINFO: 1.12.1.5875 for the client we play); a Mac client in its Info.plist.
// Portable code: the files are parsed byte by byte, never loaded as a program.
namespace vd::setup {

struct FileVersion {
    u16 major = 0, minor = 0, patch = 0, build = 0;

    bool operator==(const FileVersion&) const = default;
    std::string toString() const; // "1.12.1.5875"
};

inline constexpr FileVersion kClientVersion{1, 12, 1, 5875};

// The file version of a PE image's version resource (RT_VERSION, its first name and language).
Result<FileVersion> readPeFileVersion(std::span<const u8> image);
Result<FileVersion> readPeFileVersionFile(const std::filesystem::path& exe);

// The string value of `key` in an XML property list (<key>CFBundleVersion</key><string>1.12.1</string>);
// empty when it is not there.
std::string plistString(std::string_view plist, std::string_view key);

} // namespace vd::setup
