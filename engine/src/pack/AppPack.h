#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "pack/PackManifest.h" // kAppPackFile

#include <cstdio>
#include <filesystem>
#include <string_view>

namespace vd::pack {

// app.vdpk (kAppPackFile, phase 22, ADR-0005 §2): what is ours, baked by `vd_baker bake --only app` without
// the WoW client and shipped inside the APK, so that a change of our texts or art reaches the players with
// the APK, without re-baking anything from their client. Same formats as ui.vdpk (pack/UiFormat.h):
//
//   "ui/strings/<loc>"   our texts in one language: the VD_* keys, our fixes of client texts (enUS) and
//                        our translations (esES...). core/Text looks a key up here and in the clients'
//                        tables of ui.vdpk.
//   "ui/atlas"           our art (art/: the touch cluster's SVG icons) in pages of kUiAtlasSize, LZ4; the
//   "ui/sprites"         runtime puts them after ui.vdpk's pages (ui/UiAssets), under client-like paths of
//                        our own ("Interface\VanillaDroid\Touch-Jump.blp": app/HudShared.h)
//   "ui/fonts"           the distance fields of our font (R8 layers of kUiFontAtlasSize), after ui.vdpk's
//   "ui/font/droidsans"  its glyphs: DroidSans (Apache 2.0, art/fonts), for the screens shown before there
//                        is any data (ui::FontId::App) and for every text while ui.vdpk is missing
constexpr const char* kAppFontAsset = "ui/font/droidsans";

// Android: app.vdpk travels in the APK's assets and is copied to the app's internal storage `dir` when it is
// missing there or when `stamp` (the APK's versionCode and install time) differs from the one recorded next
// to it ("app.vdpk.stamp"). `write` copies the asset's bytes into `out`, a temporary file renamed over
// app.vdpk once complete: a copy cut short never leaves half a pack. Returns the installed pack's path;
// `copied` says whether it was copied now.
using AppPackWriter = bool (*)(void* user, std::FILE* out);
Result<std::filesystem::path> installAppPack(const std::filesystem::path& dir, std::string_view stamp,
                                             AppPackWriter write, void* user, bool* copied = nullptr);

} // namespace vd::pack
