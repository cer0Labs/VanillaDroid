#pragma once

#include "app/Dialogs.h"

#include <string>
#include <string_view>

namespace vd {

// The Settings window's values on disk (phase 10, also the audit's pending "save the settings"): "key = value" lines
// in the app's private storage (Android: internal storage; desktop: packs/.cache), read once at start and written
// in the background a moment after they change. Values out of range are clamped; unknown keys are ignored and
// missing ones keep what `settings` held, so older files keep working. A "version" line tells the defaults a file was
// written with: settings whose default changed since keep what `settings` held when the file is older.
std::string formatSettings(const SettingsView& settings);
void parseSettings(std::string_view text, SettingsView& settings);

// Applies a preset (QualityPreset) to the video knobs and remembers it.
void applyQualityPreset(SettingsView& settings, u32 preset);
// The preset the knobs match now (QualityPreset; Custom when none).
u32 currentQualityPreset(const SettingsView& settings);

} // namespace vd
