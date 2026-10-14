#pragma once

#include "core/Types.h"
#include "formats/LuaStrings.h"
#include "formats/MpqArchiveSet.h"

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace vd::bake {

// Phase 22 (apppack, ADR-0005 §2): the interface's texts split by where they come from. ui.vdpk keeps what
// the player's clients bring (clientTextTables: GlobalStrings.lua, GlueStrings.lua and the rules derived from
// them, ADR-0004) and app.vdpk what is ours (ownTextTables: locale/<locale>/*.lang), baked without the WoW.
// core/Text looks a key up in both (text::Tables). Each table becomes a "ui/strings/<locale>" blob
// (stringTableBlobs, pack/UiFormat.h).

using TextMap = std::map<std::string, std::string>;
using TextTables = std::vector<std::pair<std::string, TextMap>>; // locale ("enUS", "esES") -> its texts

// A client's texts: GlueStrings first (the in-game table wins the two keys both define with different texts: SECONDS,
// MINUTES), then GlobalStrings, then the rules of its language (VD_GLUE_SHOW_ESRB_RATING, VD_CREATE_SHOW_RANDOM_NAME,
// VD_MINIMAP_TWENTYFOUR_HOUR_TIME and ChatFrame.lua's EMOTE<n>_TOKEN). The texts come out as UTF-8 with their quotes
// (formats::utf8FromWindows1252, formats::restoreLostQuotes). False when a Lua file is missing or malformed (logged);
// what could be read is still in `texts`.
bool readClientTexts(const formats::MpqArchiveSet& mpq, TextMap& texts);

// Every *.lang of one language's folder (locale/<locale>: one per area of the interface), in file name order.
// Two areas may need the same key: fine when they agree, an error (logged, `ok` false) when they do not.
std::vector<formats::LuaString> readOwnTexts(const std::filesystem::path& dir, bool& ok);

// A client and the language of its texts.
struct ClientTexts {
    std::string locale;
    const formats::MpqArchiveSet* mpq = nullptr;
};
// ui.vdpk: one table per client (the base client first, under its own language; then each client of another
// language), with only what that client defines.
bool clientTextTables(std::span<const ClientTexts> clients, TextTables& out);

// app.vdpk: one table per folder of `localeDir` (enUS first), with only our texts: VD_* keys in English and
// their translations, our fixes of client texts (enUS) and our translations of client keys for a language
// without its own client (esES). A VD_* key translated but missing from enUS is a typo: warned. False when
// enUS is missing or a file is malformed.
bool ownTextTables(const std::filesystem::path& localeDir, TextTables& out);

// The "ui/strings/<locale in lowercase>" blob of each table (pack::UiStringsHeader, keys sorted by
// stringHash).
bool stringTableBlobs(const TextTables& tables, std::vector<std::pair<std::string, std::vector<u8>>>& out);

} // namespace vd::bake
