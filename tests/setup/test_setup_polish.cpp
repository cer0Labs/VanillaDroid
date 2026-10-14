// Phase 22 (polish): VanillaDroid Setup's texts as the window and the console each read them (SetupText.h):
// the window's own wording of the core's texts, both languages with the same keys in every file, and the
// language a chooser shows.
#include "SetupCommon.h"
#include "SetupText.h"
#include "core/Text.h"
#include "formats/LuaStrings.h"
#include "vd_setup_lang.gen.h"

#include <doctest/doctest.h>

#include <map>
#include <ostream> // doctest prints std::string values through operator<<
#include <set>
#include <string>
#include <string_view>

using namespace vd::setup;

namespace {

// Every key of a language, from all of its files (setup.lang, setupwindow.lang), with its text.
std::map<std::string, std::string> textsOf(std::string_view locale) {
    std::map<std::string, std::string> texts;
    for (const generated::SetupLanguage& language : generated::kSetupLanguages) {
        if (std::string_view(language.locale) != locale) {
            continue;
        }
        const std::string_view source(reinterpret_cast<const char*>(language.data), language.size);
        vd::Result<std::vector<vd::formats::LuaString>> parsed = vd::formats::parseLuaStrings(source);
        REQUIRE(parsed);
        for (vd::formats::LuaString& entry : parsed.value()) {
            CHECK_MESSAGE(!texts.count(entry.key), entry.key << " is in two files of " << locale);
            texts[entry.key] = entry.text;
        }
    }
    return texts;
}

std::string text(const char* key) {
    return std::string(vd::text::get(vd::text::Key(key)));
}

bool speaksToTheConsole(const std::string& sentence) {
    return sentence.find("vdsetup") != std::string::npos || sentence.find("--") != std::string::npos;
}

// The core's texts the window shows that speak to the console in setup.lang.
constexpr const char* kConsoleTextsTheWindowShows[] = {
    "VD_SETUP_ADB_NO_DEVICE",  "VD_SETUP_ADB_MANY_DEVICES",      "VD_SETUP_ADB_SERIAL_NOT_FOUND",
    "VD_SETUP_ADB_OFFLINE",    "VD_SETUP_ADB_NO_CURL",           "VD_SETUP_ADB_NO_TAR",
    "VD_SETUP_WOW_UNOFFICIAL", "VD_SETUP_STATUS_UPDATE_UNKNOWN", "VD_SETUP_STATUS_UPDATE_AVAILABLE",
};

} // namespace

TEST_CASE("Setup texts: both languages have the same keys in every file") {
    const std::map<std::string, std::string> english = textsOf("enUS");
    const std::map<std::string, std::string> spanish = textsOf("esES");
    CHECK(english.size() > 300);
    for (const auto& [key, value] : english) {
        CHECK_MESSAGE(spanish.count(key), key << " is not translated");
    }
    for (const auto& [key, value] : spanish) {
        CHECK_MESSAGE(english.count(key), key << " is not a key of enUS");
    }
}

TEST_CASE("Setup texts: the window's own wording of the core's texts") {
    CHECK(windowWordingTarget("VD_SETUP_WINDOW_ADB_NO_DEVICE") == "VD_SETUP_ADB_NO_DEVICE");
    CHECK(windowWordingTarget("VD_SETUP_ADB_NO_DEVICE").empty());
    CHECK(windowWordingTarget("VD_SETUP_WINDOW_").empty());
    CHECK(windowWordingTarget("VD_SETUP_GUI_NAV_HOME").empty());

    for (const char* locale : {"enUS", "esES"}) {
        CAPTURE(locale);
        // Each window wording replaces a text of the core, never one of its own kind, and speaks to no
        // console.
        const std::map<std::string, std::string> texts = textsOf(locale);
        int wordings = 0;
        for (const auto& [key, value] : texts) {
            const std::string target = windowWordingTarget(key);
            if (target.empty()) {
                continue;
            }
            ++wordings;
            CHECK_MESSAGE(texts.count(target), key << " replaces no text");
            CHECK_MESSAGE(windowWordingTarget(target).empty(), key);
            CHECK_MESSAGE(!speaksToTheConsole(value), key << ": " << value);
        }
        CHECK(wordings >= 8);

        // The console reads the core's sentence; the window, its own (also through formatText, as the core
        // formats its errors).
        loadSetupTexts(locale, TextSurface::Console);
        CHECK(speaksToTheConsole(text("VD_SETUP_ADB_NO_DEVICE")));
        CHECK(text("VD_SETUP_ADB_NO_DEVICE") == texts.at("VD_SETUP_ADB_NO_DEVICE"));
        loadSetupTexts(locale, TextSurface::Window);
        CHECK(text("VD_SETUP_ADB_NO_DEVICE") == texts.at("VD_SETUP_WINDOW_ADB_NO_DEVICE"));
        CHECK(formatText("VD_SETUP_ADB_SERIAL_NOT_FOUND", {"R5CW"}).find("R5CW") != std::string::npos);
        for (const char* key : kConsoleTextsTheWindowShows) {
            CHECK_MESSAGE(!speaksToTheConsole(text(key)), key << ": " << text(key));
        }
        // What the window has no wording of stays the core's.
        CHECK(text("VD_SETUP_WOW_SAVED") == texts.at("VD_SETUP_WOW_SAVED"));
    }
    vd::text::unload();
}

TEST_CASE("Setup texts: the language a chooser shows is the one loaded") {
    CHECK(chooserLanguage("esES", "", false).empty());       // the system's, as chosen
    CHECK(chooserLanguage("esES", "esES", false) == "esES"); // chosen by name
    CHECK(chooserLanguage("enUS", "enUS", false) == "enUS");
    CHECK(chooserLanguage("esES", "enUS", true) == "esES");  // --lang esES over a saved English
    CHECK(chooserLanguage("esES", "", true) == "esES");      // --lang esES over the system's
    CHECK(chooserLanguage("esES", "frFR", false) == "esES"); // a setting that did not load: what did
    // loadSetupTexts falls back to the system's for an unknown language: the chooser then shows that one.
    const std::string loaded = loadSetupTexts("frFR", TextSurface::Window);
    CHECK(loaded == systemSetupLocale());
    CHECK(chooserLanguage(loaded, "frFR", false) == loaded);
    vd::text::unload();
}
