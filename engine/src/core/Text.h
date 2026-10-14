#pragma once

#include "core/Hash.h"
#include "core/Types.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace vd::text {

// The interface's texts: nothing the player reads is written in the code, only its key. Keys are the client's own
// (Interface\FrameXML\GlobalStrings.lua and Interface\GlueXML\GlueStrings.lua: ERR_INV_FULL, LOOT_ITEM_SELF,
// SPELLLOGSELFOTHER...), so the texts are the original ones in the client's language, or ours (VD_*) for what only
// this client has (touch controls...), written in English in locale/enUS/*.lang. Other languages translate keys of
// both kinds in locale/<locale>/*.lang; whatever is missing falls back to English.
//
// Phase 22 (apppack, ADR-0005 §2): two packs hold them ("ui/strings/<locale>", pack/UiFormat.h): ui.vdpk the tables of
// the player's clients (the base client's, in the language of the pack's header, and one per client of another
// language, ADR-0004) and app.vdpk ours. For the chosen language a key is looked up in: the table of that language's
// own client (unless it is English: our English texts fix the client's) -> our table of that language -> our English
// table -> the base client's table. The tables are merged once at load into one sorted table: a lookup is one binary
// search, as before.
//
//     text::get("ERR_INV_FULL")                                    -> "Inventory is full."
//     text::format(buffer, "LOOT_ITEM_SELF", itemName)             -> "You receive loot: Linen Cloth."
//
// The table is process-wide: loaded once at startup on the main thread, read-only afterwards (any thread).

struct Key {
    u64 hash;
    std::string_view name;
    constexpr Key(std::string_view key) : hash(stringHash(key)), name(key) {}
    constexpr Key(const char* key) : Key(std::string_view(key)) {}
};

// Maps `uiPack` (the clients' tables) and `appPack` (ours) and merges their tables for `locale` ("enUS", "esES"; any
// case) in the order above; either pack may be missing (an empty path skips it: app.vdpk alone on a phone without
// data). False when neither has a table (every key then shows as itself).
bool load(const std::filesystem::path& uiPack, const std::filesystem::path& appPack, std::string_view locale);
// The same with the app.vdpk next to `uiPack` (desktop, tests).
bool load(const std::filesystem::path& uiPack, std::string_view locale);
// The tables of one language, as load() finds them (blobs as the baker writes them; any may be empty).
struct Tables {
    std::string_view locale;       // the chosen language
    std::span<const u8> client;     // its own client's table (ui.vdpk), when it has one
    std::span<const u8> clientBase; // the base client's table (ui.vdpk)
    std::span<const u8> own;        // our table of that language (app.vdpk)
    std::span<const u8> ownEnglish; // our English table (app.vdpk)
};
// Tests and tools: merges `tables` as load() does (the blobs are kept by reference: they must outlive their use).
void setTables(const Tables& tables);
// Tests and tools: a table blob as the baker writes it (kept by reference: it must outlive its use).
void setTable(std::span<const u8> blob);
void unload();
bool loaded();
// Phase 21 (languages): the player's gender (0 male, 1 female), for the "$g<male>:<female>;" some translations put in
// their texts (formats only). Any thread.
void setGender(u8 gender);

bool has(Key key);
// The text of `key`; the key's own name when the table lacks it (shown as is: a visible bug, warned in the log once).
std::string_view get(Key key);

// One argument of a format.
struct Arg {
    enum class Kind : u8 { Int, Uint, Float, Str, Char };
    Kind kind = Kind::Int;
    i64 i = 0;
    u64 u = 0;
    f64 f = 0.0;
    std::string_view s;

    Arg(int v) : kind(Kind::Int), i(v) {}
    Arg(long v) : kind(Kind::Int), i(v) {}
    Arg(long long v) : kind(Kind::Int), i(v) {}
    Arg(unsigned v) : kind(Kind::Uint), u(v) {}
    Arg(unsigned long v) : kind(Kind::Uint), u(v) {}
    Arg(unsigned long long v) : kind(Kind::Uint), u(v) {}
    Arg(f32 v) : kind(Kind::Float), f(v) {}
    Arg(f64 v) : kind(Kind::Float), f(v) {}
    Arg(char v) : kind(Kind::Char), i(static_cast<u8>(v)) {}
    Arg(std::string_view v) : kind(Kind::Str), s(v) {}
    Arg(const char* v) : kind(Kind::Str), s(v ? std::string_view(v) : std::string_view{}) {}
    Arg(const std::string& v) : kind(Kind::Str), s(v) {}
};

// Formats `format` (a text of the table, never code's own printf format) into `out`, NUL-terminated and cut to fit;
// returns what was written. Specifiers as the client's strings use them: %s %d %i %u %c %f %g %x %%, with flags,
// width and precision (%.1f, %02d), and positional ones (%2$s: translations may reorder the arguments). A missing
// argument prints nothing; a number for %s prints the number, a string for %d prints the string.
std::string_view vformat(std::span<char> out, std::string_view format, std::span<const Arg> args);

template <typename... Args> std::string_view format(std::span<char> out, Key key, const Args&... args) {
    if constexpr (sizeof...(Args) == 0) {
        return vformat(out, get(key), {});
    } else {
        const Arg list[] = {Arg(args)...};
        return vformat(out, get(key), list);
    }
}

// Appends the formatted text to `out` (allocates: for texts built once, not per frame).
void vappend(std::string& out, std::string_view format, std::span<const Arg> args);
template <typename... Args> void append(std::string& out, Key key, const Args&... args) {
    if constexpr (sizeof...(Args) == 0) {
        vappend(out, get(key), {});
    } else {
        const Arg list[] = {Arg(args)...};
        vappend(out, get(key), list);
    }
}

} // namespace vd::text
