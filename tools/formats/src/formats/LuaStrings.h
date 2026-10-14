#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vd::formats {

// The client's string tables (Interface\FrameXML\GlobalStrings.lua, Interface\GlueXML\GlueStrings.lua) and our own
// locale/<locale>.lang files, which use the same syntax: one `KEY = "text";` per line, `--` comments, blank lines.
// Escapes in the text: \n \t \" \\ and \ddd (decimal byte, as Lua: GlobalStrings writes a trailing space as \32).
// Values that are not strings (numbers) are skipped. UTF-8, CRLF or LF.
struct LuaString {
    std::string key;
    std::string text;
};

// Every string assignment in file order. Errors name the first malformed line.
Result<std::vector<LuaString>> parseLuaStrings(std::string_view source);

// The client's texts made fit for the interface, which reads UTF-8 and shows "?" for a byte that is not UTF-8 and for
// U+FFFD. Both give the same bytes on every platform.
//
// A Lua string file saved by a Windows-1252 editor carries bytes that are not UTF-8 (curly quotes and apostrophes
// 0x91..0x94, dashes, the ellipsis...). Returns `bytes` as UTF-8: every well-formed UTF-8 sequence as it is (another
// language's accents), every other byte read as Windows-1252 (0x80..0x9F its punctuation; the five it leaves undefined
// and 0xA0..0xFF as Latin-1).
std::string utf8FromWindows1252(std::string_view bytes);
// The 1.12.1 GlobalStrings.lua (enUS and esES) has U+FFFD, the replacement character, in a few values where its
// source had Windows-1252 quotes and apostrophes, lost when it was converted to UTF-8. Each one in `utf8` becomes the
// punctuation its place calls for: between two letters or digits an apostrophe (U+2019), before one an opening
// quote (U+201C), otherwise a closing quote (U+201D).
std::string restoreLostQuotes(std::string_view utf8);

} // namespace vd::formats
