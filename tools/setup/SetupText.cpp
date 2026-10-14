#include "SetupText.h"

#include "SetupCommon.h"
#include "bake/UiBaker.h"
#include "core/Locale.h"
#include "core/Log.h"
#include "core/Text.h"
#include "formats/LuaStrings.h"
#include "vd_setup_lang.gen.h" // kSetupLanguages: locale/<locale>/setup.lang at build time

#include <algorithm>
#include <cstdlib>
#include <map>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace vd::setup {
namespace {

const generated::SetupLanguage* findLanguage(std::string_view locale) {
    for (const generated::SetupLanguage& language : generated::kSetupLanguages) {
        if (equalsNoCase(language.locale, locale)) {
            return &language;
        }
    }
    return nullptr;
}

void addTexts(const generated::SetupLanguage& language, std::map<std::string, std::string>& texts) {
    const std::string_view source(reinterpret_cast<const char*>(language.data), language.size);
    Result<std::vector<formats::LuaString>> parsed = formats::parseLuaStrings(source);
    if (!parsed) {
        VD_LOG_ERROR("setup.lang %s: %s", language.locale, parsed.error().message.c_str());
        return;
    }
    for (formats::LuaString& entry : parsed.value()) {
        texts[entry.key] = std::move(entry.text);
    }
}

// Phase 22 (gui): a language's texts are in several files (setup.lang, setupwindow.lang): all of them.
void addLocaleTexts(std::string_view locale, std::map<std::string, std::string>& texts) {
    for (const generated::SetupLanguage& language : generated::kSetupLanguages) {
        if (equalsNoCase(language.locale, locale)) {
            addTexts(language, texts);
        }
    }
}

// "es-ES", "es_ES.UTF-8", "es" -> a built-in language of the same tongue.
std::string matchLocale(std::string_view name) {
    if (name.size() < 2) {
        return {};
    }
    const std::string language = toLowerAscii(name.substr(0, 2));
    std::string region;
    if (name.size() >= 5 && (name[2] == '-' || name[2] == '_')) {
        region = std::string(name.substr(3, 2));
    }
    if (const generated::SetupLanguage* exact = findLanguage(language + region)) {
        return exact->locale;
    }
    for (const generated::SetupLanguage& candidate : generated::kSetupLanguages) {
        if (toLowerAscii(std::string_view(candidate.locale).substr(0, 2)) == language) {
            return candidate.locale; // esMX -> esES while there is no esMX translation
        }
    }
    return {};
}

} // namespace

std::vector<std::string> setupLocales() {
    std::vector<std::string> list;
    for (const generated::SetupLanguage& language : generated::kSetupLanguages) {
        if (std::find(list.begin(), list.end(), language.locale) == list.end()) { // one per language
            list.emplace_back(language.locale);
        }
    }
    return list;
}

std::string setupLanguageName(std::string_view locale) {
    std::map<std::string, std::string> texts;
    addLocaleTexts(locale, texts);
    const auto name = texts.find("VD_SETUP_LANGUAGE_NAME");
    return name != texts.end() ? name->second : std::string(locale);
}

std::string systemSetupLocale() {
#if defined(_WIN32)
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) {
        std::string ascii;
        for (const wchar_t* c = name; *c; ++c) {
            ascii.push_back(*c < 128 ? static_cast<char>(*c) : '?');
        }
        if (std::string match = matchLocale(ascii); !match.empty()) {
            return match;
        }
    }
#else
    for (const char* variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(variable);
        if (value && *value) {
            if (std::string match = matchLocale(value); !match.empty()) {
                return match;
            }
        }
    }
#endif
    return "enUS";
}

std::string windowWordingTarget(std::string_view key) {
    if (key.size() <= kWindowWordingPrefix.size() ||
        key.substr(0, kWindowWordingPrefix.size()) != kWindowWordingPrefix) {
        return {};
    }
    return "VD_SETUP_" + std::string(key.substr(kWindowWordingPrefix.size()));
}

std::string chooserLanguage(std::string_view loaded, std::string_view setting, bool overridden) {
    return !overridden && setting.empty() ? std::string() : std::string(loaded);
}

std::string loadSetupTexts(std::string_view locale, TextSurface surface) {
    static std::vector<u8> table; // core/Text keeps a reference to it
    std::string chosen;
    if (const generated::SetupLanguage* language = findLanguage(locale)) {
        chosen = language->locale;
    } else {
        chosen = systemSetupLocale();
    }
    std::map<std::string, std::string> texts;
    addLocaleTexts("enUS", texts);
    if (chosen != "enUS") {
        addLocaleTexts(chosen, texts);
    }
    // Phase 22 (polish): the window's wording of the core's texts in their place (after both languages: a
    // translated console sentence never wins over the window's).
    if (surface == TextSurface::Window) {
        for (const auto& [key, text] : texts) {
            if (const std::string target = windowWordingTarget(key); !target.empty()) {
                const auto replaced = texts.find(target);
                if (replaced != texts.end()) {
                    replaced->second = text;
                } else {
                    VD_LOG_WARN("setupwindow.lang: %s replaces no text", key.c_str());
                }
            }
        }
    }
    std::vector<bake::UiText> list;
    list.reserve(texts.size());
    for (auto& [key, text] : texts) {
        list.push_back({key, text});
    }
    Result<std::vector<u8>> blob = bake::buildStringTable(list);
    if (!blob) {
        VD_LOG_ERROR("setup texts: %s", blob.error().message.c_str());
        return chosen;
    }
    text::unload();
    table = std::move(blob.value());
    text::setTable(table);
    return chosen;
}

} // namespace vd::setup
