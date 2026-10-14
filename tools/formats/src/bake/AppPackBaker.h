#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>
#include <span>

namespace vd::bake {

// Phase 22 (apppack): app.vdpk, what is ours (pack/AppPack.h), baked from the repository alone: no WoW client
// is opened (the CI bakes it on a machine without Blizzard data, ADR-0005 §6).

// Our art (art/): single-colour SVG icons rasterized white into the atlas under a client-like path of our own
// (the HUD tints them), at `size` pixels. A new icon: its SVG under art/touch, one line in kOwnArt
// (AppPackBaker.cpp) and its hash in app/HudShared.h.
struct OwnArt {
    const char* file; // under art/
    const char* path; // the sprite's path (assetPathHash in the runtime: app/HudShared.h)
    u32 size;
};
std::span<const OwnArt> ownArt();

// Our font (art/fonts), as distance fields like the client's: "ui/font/<name>".
struct OwnFont {
    const char* file; // under art/
    const char* name;
    f32 pixelHeight;
};
std::span<const OwnFont> ownFonts();

struct AppPackStats {
    usize languages = 0;
    usize texts = 0; // in every language
    usize sprites = 0;
    usize atlasPages = 0;
    usize glyphs = 0; // of every font
    usize fontLayers = 0;
    u64 bytes = 0;
    u32 failures = 0; // missing or malformed inputs (logged); the pack is written without them
};

// <outPath>: the texts of `localeDir` (locale/<locale>/*.lang), the icons and fonts of `artDir` (art/). An
// error when the pack cannot be written at all.
Result<AppPackStats> bakeAppPack(const std::filesystem::path& outPath, const std::filesystem::path& localeDir,
                                 const std::filesystem::path& artDir, u32 bakerVersion);

} // namespace vd::bake
