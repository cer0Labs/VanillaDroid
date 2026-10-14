// Phase 22 (setup): VanillaDroid Setup's settings, its built-in texts and its pairing secrets.
#include "SetupCommon.h"
#include "SetupSettings.h"
#include "SetupText.h"
#include "core/Text.h"
#include "formats/LuaStrings.h"
#include "vd_setup_lang.gen.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ostream> // doctest prints std::string values through operator<<
#include <set>
#include <string>

namespace {

std::filesystem::path tempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vd_setup_tests" / name;
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

std::set<std::string> keysOf(const vd::setup::generated::SetupLanguage& language) {
    const std::string_view source(reinterpret_cast<const char*>(language.data), language.size);
    const vd::Result<std::vector<vd::formats::LuaString>> parsed = vd::formats::parseLuaStrings(source);
    REQUIRE(parsed.ok());
    std::set<std::string> keys;
    for (const vd::formats::LuaString& entry : parsed.value()) {
        CHECK_MESSAGE(keys.insert(entry.key).second, entry.key << " twice in " << language.locale);
    }
    return keys;
}

} // namespace

TEST_CASE("Setup: settings round trip, with paths in any language") {
    using vd::setup::Settings;
    const std::filesystem::path dir = tempDir("settings");
    const std::filesystem::path file = dir / "setup.json";
    {
        const vd::Result<Settings> missing = Settings::load(file);
        REQUIRE(missing.ok());
        CHECK(missing.value().appId == vd::setup::kDistAppId);
        CHECK(missing.value().port == vd::setup::kDefaultPort);
        CHECK(missing.value().wow.empty());
    }
    Settings settings;
    settings.language = "esES";
    settings.wow =
        vd::setup::pathFromUtf8("C:/Juegos/M\xC3\xBAsica y \xE6\xB8\xB8\xE6\x88\x8F/World of Warcraft");
    settings.localeClient = "D:/WoW esES";
    settings.includeArchives = {"patch-4.MPQ", "patch-A.MPQ"};
    settings.packs = "E:/packs";
    settings.port = 47811;
    settings.pairingKey = "00112233445566778899aabbccddeeff";
    settings.adb = "C:/tools/adb.exe";
    settings.apk = "C:/VanillaDroid.apk";
    settings.appId = vd::setup::kDevAppId;
    settings.serial = "adb-R5CT00TEST-AbCdEf._adb-tls-connect._tcp";
    settings.lastBake = "abc";
    REQUIRE(settings.save(file).ok());
    const vd::Result<Settings> loaded = Settings::load(file);
    REQUIRE(loaded.ok());
    const Settings& back = loaded.value();
    CHECK(back.language == "esES");
    CHECK(back.wow == settings.wow);
    CHECK(vd::setup::utf8FromPath(back.wow) == vd::setup::utf8FromPath(settings.wow));
    CHECK(back.localeClient == settings.localeClient);
    CHECK(back.includeArchives == settings.includeArchives);
    CHECK(back.packs == settings.packs);
    CHECK(back.port == 47811);
    CHECK(back.pairingKey == settings.pairingKey);
    CHECK(back.adb == settings.adb);
    CHECK(back.apk == settings.apk);
    CHECK(back.appId == vd::setup::kDevAppId);
    CHECK(back.serial == settings.serial);
    CHECK(back.lastBake == "abc");
    CHECK(vd::setup::packsDir(back, dir) == "E:/packs");
    CHECK(vd::setup::packsDir(Settings{}, dir) == dir / "packs");
    CHECK(vd::setup::dataDir(dir) == dir);

    {
        std::ofstream damaged(file, std::ios::binary | std::ios::trunc);
        damaged << "{\"format\": 1, \"wow\": ";
    }
    CHECK_FALSE(Settings::load(file).ok());
}

TEST_CASE("Setup: pairing key and code") {
    const std::string key = vd::setup::newPairingKey();
    CHECK(key.size() == 32);
    CHECK(key.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(vd::setup::newPairingKey() != key);
    std::set<std::string> codes;
    for (int i = 0; i < 50; ++i) {
        const std::string code = vd::setup::newPairingCode();
        CHECK(code.size() == 6);
        CHECK(code.find_first_not_of("0123456789") == std::string::npos);
        codes.insert(code);
    }
    CHECK(codes.size() > 40); // random, not a counter
}

TEST_CASE("Setup: the built-in texts, English and Spanish with the same keys") {
    using vd::setup::generated::kSetupLanguages;
    const vd::setup::generated::SetupLanguage* english = nullptr;
    const vd::setup::generated::SetupLanguage* spanish = nullptr;
    for (const auto& language : kSetupLanguages) {
        if (std::string_view(language.locale) == "enUS") {
            english = &language;
        }
        if (std::string_view(language.locale) == "esES") {
            spanish = &language;
        }
    }
    REQUIRE(english != nullptr);
    REQUIRE(spanish != nullptr);
    const std::set<std::string> enKeys = keysOf(*english);
    const std::set<std::string> esKeys = keysOf(*spanish);
    CHECK(enKeys.size() > 100);
    for (const std::string& key : enKeys) {
        CHECK_MESSAGE(esKeys.count(key), key << " is not translated");
        CHECK(key.rfind("VD_SETUP_", 0) == 0);
    }
    for (const std::string& key : esKeys) {
        CHECK_MESSAGE(enKeys.count(key), key << " is not a key of enUS");
    }

    CHECK(vd::setup::loadSetupTexts("enUS") == "enUS");
    CHECK(vd::text::get("VD_SETUP_WOW_SAVED") == "Saved.");
    CHECK(vd::setup::formatText("VD_SETUP_SERVE_BAD_PORT", {"x"}) == "Not a port: x");
    CHECK(vd::setup::loadSetupTexts("eses") == "esES");
    CHECK(vd::text::get("VD_SETUP_WOW_SAVED") == "Guardado.");
    const std::vector<std::string> locales = vd::setup::setupLocales();
    const std::string system = vd::setup::systemSetupLocale();
    CHECK(std::find(locales.begin(), locales.end(), system) != locales.end());
    vd::setup::loadSetupTexts("enUS");
    vd::text::unload();
}

TEST_CASE("Setup: helpers") {
    CHECK(vd::setup::equalsNoCase("Data", "dATA"));
    CHECK_FALSE(vd::setup::equalsNoCase("Data", "Dat"));
    CHECK(vd::setup::toLowerAscii("Patch-A.MPQ") == "patch-a.mpq");
    vd::setup::loadSetupTexts("enUS"); // phase 22 (polish): sizes are texts of the language
    CHECK(vd::setup::formatBytes(999) == "999 B");
    CHECK(vd::setup::formatBytes(4500000000ull) == "4.50 GB");
    CHECK(vd::setup::formatBytes(12300000) == "12.3 MB");
    CHECK(vd::setup::formatBytes(5400000000ull) == "5.40 GB");
    CHECK(vd::setup::formatBytes(1234) == "1 KB");
    CHECK(vd::setup::formatBytes(999499) == "999 KB");
    CHECK(vd::setup::formatBytes(999500) == "1.0 MB");        // never "1000 KB"
    CHECK(vd::setup::formatBytes(999949999) == "999.9 MB");
    CHECK(vd::setup::formatBytes(999950000) == "1.00 GB");    // never "1000.0 MB"
    CHECK(vd::setup::formatBytes(1005000000ull) == "1.01 GB"); // rounded half up
    CHECK(vd::setup::formatBytes(0) == "0 B");
    vd::setup::loadSetupTexts("esES"); // the language's decimal sign
    CHECK(vd::setup::formatBytes(5400000000ull) == "5,40 GB");
    CHECK(vd::setup::formatBytes(12300000) == "12,3 MB");
    CHECK(vd::setup::formatBytes(4500) == "5 KB");
    vd::text::unload();
    const std::filesystem::path dir = tempDir("child");
    std::filesystem::create_directories(dir / "data");
    const std::filesystem::path found = vd::setup::findChildNoCase(dir, "Data");
    REQUIRE_FALSE(found.empty());
    CHECK(found.filename() == "data");
    CHECK(vd::setup::findChildNoCase(dir, "Interface").empty());
    CHECK_FALSE(vd::setup::executableDir().empty());
}
