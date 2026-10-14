#pragma once

#include <string>
#include <string_view>
#include <vector>

// Phase 22 (ADR-0003): VanillaDroid Setup's texts. The tool has no packs to read them from: locale/<locale>/
// setup.lang are built into it (CMake turns them into a generated header) and parsed at startup with the
// baker's .lang rules (formats::parseLuaStrings), English first and the chosen language over it, into a
// core/Text table.
namespace vd::setup {

// The languages built in ("enUS", "esES").
std::vector<std::string> setupLocales();
// Phase 22 (gui): the name of a built-in language in that language ("English", "Español": its
// VD_SETUP_LANGUAGE_NAME), for a chooser that shows them all; the locale itself when it has none.
std::string setupLanguageName(std::string_view locale);
// The system's language among them ("esES" on a Spanish Windows or with LANG=es_ES.UTF-8), else "enUS".
std::string systemSetupLocale();

// Phase 22 (polish): who reads the texts. The core's texts speak to the console ("see vdsetup adb help",
// "--serial <device>"); the window has its own wording for those it shows: a key VD_SETUP_WINDOW_<name> of
// setupwindow.lang takes the place of VD_SETUP_<name> when the window loads the texts, so the core (which
// only knows the keys) reads the window's sentence.
enum class TextSurface : unsigned char { Console, Window };
inline constexpr std::string_view kWindowWordingPrefix = "VD_SETUP_WINDOW_";
// The key a window wording replaces ("VD_SETUP_WINDOW_ADB_NO_DEVICE" -> "VD_SETUP_ADB_NO_DEVICE"), empty when
// `key` is not one.
std::string windowWordingTarget(std::string_view key);

// Loads the texts in `locale` (any case; empty or unknown: the system's) for `surface` and returns the one
// loaded. Not thread-safe: once at startup, before any text is read.
std::string loadSetupTexts(std::string_view locale, TextSurface surface = TextSurface::Console);

// Phase 22 (polish): the language a chooser shows as chosen: the one `loaded`, or "" ("the system's") when
// the setting asks for the system's and nothing overrides it (--lang). Never a setting that did not load (an
// unknown one falls back to the system's).
std::string chooserLanguage(std::string_view loaded, std::string_view setting, bool overridden);

} // namespace vd::setup
