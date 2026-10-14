#include "bake/UiStrings.h"

#include "bake/UiBaker.h"
#include "core/Log.h"
#include "pack/UiFormat.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string_view>

namespace vd::bake {
namespace {

// The rules of a client's language, written as texts of ours by addLocaleRules (our translations may set them
// for a language without its own client: locale/esES).
constexpr const char* kRuleKeys[] = {"VD_GLUE_SHOW_ESRB_RATING", "VD_CREATE_SHOW_RANDOM_NAME",
                                     "VD_MINIMAP_TWENTYFOUR_HOUR_TIME"};

std::string lower(std::string s) {
    for (char& c : s) {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return s;
}

// Phase 21: what a client's Lua decides by its language, as texts of our own ("1" / "0"):
// GlueLocalization.lua's LocalizeFrames shows the ESRB rating on the login screen and the "Randomize" name
// button only in the enUS client, and GameTime.lua uses the 24-hour clock when the GlobalStrings.lua of the
// client defines TwentyFourHourTime.
void addLocaleRules(const formats::MpqArchiveSet& mpq, TextMap& texts) {
    const auto source = [&](const char* file) {
        Result<std::vector<u8>> bytes = mpq.read(file);
        return bytes ? std::string(bytes.value().begin(), bytes.value().end()) : std::string{};
    };
    const std::string glue = source("Interface\\GlueXML\\GlueLocalization.lua");
    const std::string global = source("Interface\\FrameXML\\GlobalStrings.lua");
    // A line that starts with `what` (Lua that runs: not a comment).
    const auto hasLine = [](const std::string& lua, std::string_view what) {
        usize at = 0;
        while ((at = lua.find(what, at)) != std::string::npos) {
            const usize newline = lua.rfind('\n', at);
            const usize lineStart = newline == std::string::npos ? 0 : newline + 1;
            if (lua.find_first_not_of(" \t", lineStart) == at) {
                return true;
            }
            at += what.size();
        }
        return false;
    };
    texts[kRuleKeys[0]] = hasLine(glue, "WorldOfWarcraftRating:Show()") ? "1" : "0";
    texts[kRuleKeys[1]] = hasLine(glue, "CharacterCreateRandomName:Show()") ? "1" : "0";
    texts[kRuleKeys[2]] = hasLine(global, "TwentyFourHourTime") ? "1" : "0";
    // ChatFrame.lua's EMOTE<n>_TOKEN = "WAVE": the text emote that GlobalStrings' EMOTE<n>_CMD1.. (the
    // commands of the client's language: "/vale" in esES) do.
    const std::string chatFrame = source("Interface\\FrameXML\\ChatFrame.lua");
    std::string tokens;
    usize lineStart = 0;
    while (lineStart < chatFrame.size()) {
        const usize lineEnd = std::min(chatFrame.find('\n', lineStart), chatFrame.size());
        const std::string_view line(chatFrame.data() + lineStart, lineEnd - lineStart);
        if (line.rfind("EMOTE", 0) == 0 && line.find("_TOKEN") != std::string_view::npos) {
            tokens.append(line).push_back('\n');
        }
        lineStart = lineEnd + 1;
    }
    if (Result<std::vector<formats::LuaString>> parsed = formats::parseLuaStrings(tokens); parsed) {
        for (formats::LuaString& s : parsed.value()) {
            texts[s.key] = std::move(s.text);
        }
    }
}

} // namespace

bool readClientTexts(const formats::MpqArchiveSet& mpq, TextMap& texts) {
    bool ok = true;
    for (const char* file :
         {"Interface\\GlueXML\\GlueStrings.lua", "Interface\\FrameXML\\GlobalStrings.lua"}) {
        Result<std::vector<u8>> bytes = mpq.read(file);
        Result<std::vector<formats::LuaString>> strings =
            bytes ? formats::parseLuaStrings(std::string_view(
                        reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()))
                  : Result<std::vector<formats::LuaString>>(bytes.error());
        if (!strings) {
            VD_LOG_ERROR("%s: %s", file, strings.error().message.c_str());
            ok = false;
            continue;
        }
        for (const formats::LuaString& s : strings.value()) {
            texts[s.key] = formats::restoreLostQuotes(formats::utf8FromWindows1252(s.text));
        }
    }
    addLocaleRules(mpq, texts);
    return ok;
}

std::vector<formats::LuaString> readOwnTexts(const std::filesystem::path& dir, bool& ok) {
    std::vector<std::filesystem::path> files;
    std::error_code listError;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir, listError)) {
        if (entry.path().extension() == ".lang") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<formats::LuaString> all;
    std::map<std::string, std::pair<std::string, std::string>> seen; // key -> (file, text)
    for (const std::filesystem::path& path : files) {
        std::ifstream in(path, std::ios::binary);
        const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Result<std::vector<formats::LuaString>> strings = formats::parseLuaStrings(source);
        if (!strings) {
            VD_LOG_ERROR("%s: %s", path.string().c_str(), strings.error().message.c_str());
            ok = false;
            continue;
        }
        for (formats::LuaString& s : strings.value()) {
            const auto [it, added] = seen.emplace(s.key, std::make_pair(path.filename().string(), s.text));
            if (!added && it->second.second != s.text) {
                VD_LOG_ERROR("%s: %s differs from the one in %s", path.string().c_str(), s.key.c_str(),
                             it->second.first.c_str());
                ok = false;
            }
            all.push_back(std::move(s));
        }
    }
    return all;
}

bool clientTextTables(std::span<const ClientTexts> clients, TextTables& out) {
    bool ok = true;
    for (const ClientTexts& client : clients) {
        if (!client.mpq) {
            continue;
        }
        TextMap texts;
        ok &= readClientTexts(*client.mpq, texts);
        out.emplace_back(client.locale, std::move(texts));
    }
    return ok;
}

bool ownTextTables(const std::filesystem::path& localeDir, TextTables& out) {
    bool ok = true;
    std::error_code error;
    if (!std::filesystem::is_directory(localeDir / "enUS", error)) {
        VD_LOG_ERROR("%s: missing (the texts of our own)", (localeDir / "enUS").string().c_str());
        return false;
    }
    std::vector<std::string> locales{"enUS"};
    std::vector<std::string> others;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(localeDir, error)) {
        if (entry.is_directory() && entry.path().filename() != "enUS") {
            others.push_back(entry.path().filename().string());
        }
    }
    std::sort(others.begin(), others.end());
    locales.insert(locales.end(), others.begin(), others.end());
    for (const std::string& locale : locales) {
        TextMap texts;
        for (formats::LuaString& s : readOwnTexts(localeDir / locale, ok)) {
            texts[s.key] = std::move(s.text);
        }
        if (locale != "enUS") {
            // Our keys must exist in English (the fallback); client keys and the rules of a client's language
            // can only be checked against a client (the texts test does, with VD_WOW_DIR).
            const TextMap& english = out.front().second;
            for (const auto& [key, text] : texts) {
                const bool rule =
                    std::find(std::begin(kRuleKeys), std::end(kRuleKeys), key) != std::end(kRuleKeys);
                if (key.rfind("VD_", 0) == 0 && !rule && !english.count(key)) {
                    VD_LOG_WARN("%s: %s is not a key of enUS", locale.c_str(), key.c_str());
                }
            }
        }
        out.emplace_back(locale, std::move(texts));
    }
    return ok;
}

bool stringTableBlobs(const TextTables& tables, std::vector<std::pair<std::string, std::vector<u8>>>& out) {
    bool ok = true;
    for (const auto& [locale, texts] : tables) {
        std::vector<UiText> list;
        list.reserve(texts.size());
        for (const auto& [key, text] : texts) {
            list.push_back({key, text});
        }
        Result<std::vector<u8>> blob = buildStringTable(list);
        if (!blob) {
            VD_LOG_ERROR("texts %s: %s", locale.c_str(), blob.error().message.c_str());
            ok = false;
            continue;
        }
        out.emplace_back(pack::kUiStringsPrefix + lower(locale), std::move(blob.value()));
    }
    return ok;
}

} // namespace vd::bake
