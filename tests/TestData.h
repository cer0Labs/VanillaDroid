#pragma once

#include "core/Text.h"

#include <cstdlib>
#include <filesystem>
#include <optional>

namespace vd::test {

// Data directory of the original client, from $VD_WOW_DIR (set by CTest from the CMake cache).
// Tests that need real data return early with a message when it is not available.
inline std::optional<std::filesystem::path> wowDataDir() {
    const char* root = std::getenv("VD_WOW_DIR");
    if (!root) {
        return std::nullopt;
    }
    std::filesystem::path data = std::filesystem::path(root) / "Data";
    std::error_code error;
    if (!std::filesystem::is_directory(data, error)) {
        return std::nullopt;
    }
    return data;
}

// The interface's texts (core/Text) from $VD_PACKS_DIR/ui.vdpk in English, for tests that check what the player
// reads. False (and the texts show as their keys) when the packs are not available.
inline bool loadTexts() {
    const char* dir = std::getenv("VD_PACKS_DIR");
    return dir && ::vd::text::load(std::filesystem::path(dir) / "ui.vdpk", "enUS");
}

// Phase 21: with the esES table loaded, whether its client keys are the esES client's own (ui.vdpk baked with that
// client: Blizzard's wording) or our translations over the English client (locale/esES). Tests of client keys in
// Spanish expect one or the other.
inline bool esESClientTexts() {
    return ::vd::text::get("SLASH_WHO1") == "/quien";
}

} // namespace vd::test

#define VD_REQUIRE_WOW_DATA(var)                                                                         \
    const auto var = ::vd::test::wowDataDir();                                                           \
    if (!var) {                                                                                          \
        MESSAGE("VD_WOW_DIR not set or missing: skipping test that needs the original client");         \
        return;                                                                                          \
    }
