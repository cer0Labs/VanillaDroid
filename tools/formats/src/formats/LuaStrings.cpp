#include "formats/LuaStrings.h"

#include <cstdio>

namespace vd::formats {
namespace {

bool isKeyStart(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool isKeyChar(char c) {
    return isKeyStart(c) || (c >= '0' && c <= '9');
}

Error lineError(usize line, const char* what) {
    char message[96];
    std::snprintf(message, sizeof(message), "line %zu: %s", line, what);
    return Error{message};
}

// Windows-1252's 0x80..0x9F (0: undefined, read as Latin-1's C1 control).
constexpr u16 kWindows1252High[32] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, // 0x80
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000, // 0x88
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 0x90
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178, // 0x98
};

// The length of the well-formed UTF-8 sequence at the start of `s` (Unicode's table 3-7: no overlong forms, no
// surrogates, nothing over U+10FFFF), 0 when there is none.
usize utf8SequenceLength(std::string_view s) {
    const u8 b0 = static_cast<u8>(s[0]);
    if (b0 < 0x80) {
        return 1;
    }
    usize length = 0;
    u8 low = 0x80, high = 0xBF; // the second byte's range
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        length = 2;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        length = 3;
        low = b0 == 0xE0 ? 0xA0 : 0x80;
        high = b0 == 0xED ? 0x9F : 0xBF;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        length = 4;
        low = b0 == 0xF0 ? 0x90 : 0x80;
        high = b0 == 0xF4 ? 0x8F : 0xBF;
    } else {
        return 0;
    }
    if (s.size() < length) {
        return 0;
    }
    for (usize i = 1; i < length; ++i) {
        const u8 b = static_cast<u8>(s[i]);
        if (i == 1 ? (b < low || b > high) : (b < 0x80 || b > 0xBF)) {
            return 0;
        }
    }
    return length;
}

// The code point of the well-formed sequence of `length` bytes at the start of `s`.
u32 decodeUtf8(std::string_view s, usize length) {
    u32 c = static_cast<u8>(s[0]) & (length == 1 ? 0x7Fu : 0xFFu >> (length + 1));
    for (usize i = 1; i < length; ++i) {
        c = (c << 6) | (static_cast<u8>(s[i]) & 0x3Fu);
    }
    return c;
}

// Code points up to U+FFFF.
void appendUtf8(std::string& out, u32 codePoint) {
    if (codePoint < 0x80) {
        out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        out += static_cast<char>(0xC0 | (codePoint >> 6));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (codePoint >> 12));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

// A letter or a digit: ASCII's, and the letters of Latin-1 and Latin Extended-A and B (other languages' words).
bool isWordChar(u32 c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7);
}

} // namespace

std::string utf8FromWindows1252(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    while (!bytes.empty()) {
        if (const usize length = utf8SequenceLength(bytes); length > 0) {
            out.append(bytes.substr(0, length));
            bytes.remove_prefix(length);
            continue;
        }
        const u8 b = static_cast<u8>(bytes[0]);
        const u16 mapped = b >= 0x80 && b < 0xA0 ? kWindows1252High[b - 0x80] : 0;
        appendUtf8(out, mapped != 0 ? mapped : b);
        bytes.remove_prefix(1);
    }
    return out;
}

std::string restoreLostQuotes(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    u32 previous = 0; // the code point before (0: the text's start)
    while (!utf8.empty()) {
        const usize length = utf8SequenceLength(utf8);
        if (length == 0) { // not UTF-8: kept as it is
            out += utf8[0];
            previous = 0;
            utf8.remove_prefix(1);
            continue;
        }
        const u32 c = decodeUtf8(utf8, length);
        if (c != 0xFFFD) {
            out.append(utf8.substr(0, length));
            previous = c;
            utf8.remove_prefix(length);
            continue;
        }
        utf8.remove_prefix(length);
        const usize nextLength = utf8.empty() ? 0 : utf8SequenceLength(utf8);
        const u32 next = nextLength > 0 ? decodeUtf8(utf8, nextLength) : 0;
        const u32 quote = isWordChar(next) ? (isWordChar(previous) ? 0x2019u : 0x201Cu) : 0x201Du;
        appendUtf8(out, quote);
        previous = quote;
    }
    return out;
}

Result<std::vector<LuaString>> parseLuaStrings(std::string_view source) {
    std::vector<LuaString> out;
    if (source.size() >= 3 && static_cast<u8>(source[0]) == 0xEF && static_cast<u8>(source[1]) == 0xBB &&
        static_cast<u8>(source[2]) == 0xBF) {
        source.remove_prefix(3); // UTF-8 byte order mark
    }
    usize lineNumber = 0;
    while (!source.empty()) {
        const usize end = std::min(source.find('\n'), source.size());
        std::string_view line = source.substr(0, end);
        source.remove_prefix(std::min(end + 1, source.size()));
        ++lineNumber;
        usize i = 0;
        auto skipSpaces = [&]() {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
                ++i;
            }
        };
        skipSpaces();
        if (i == line.size() || line.substr(i, 2) == "--") {
            continue; // blank or comment
        }
        if (!isKeyStart(line[i])) {
            return lineError(lineNumber, "expected KEY = \"text\";");
        }
        const usize keyStart = i;
        while (i < line.size() && isKeyChar(line[i])) {
            ++i;
        }
        LuaString entry;
        entry.key = std::string(line.substr(keyStart, i - keyStart));
        skipSpaces();
        if (i == line.size() || line[i] != '=') {
            return lineError(lineNumber, "expected '='");
        }
        ++i;
        skipSpaces();
        if (i == line.size() || line[i] != '"') {
            continue; // a number or another value: not a text
        }
        ++i;
        bool closed = false;
        while (i < line.size()) {
            const char c = line[i++];
            if (c == '"') {
                closed = true;
                break;
            }
            if (c != '\\') {
                entry.text += c;
                continue;
            }
            if (i == line.size()) {
                break;
            }
            const char e = line[i++];
            switch (e) {
            case 'n': entry.text += '\n'; break;
            case 't': entry.text += '\t'; break;
            case 'r': entry.text += '\r'; break;
            case '"': entry.text += '"'; break;
            case '\'': entry.text += '\''; break;
            case '\\': entry.text += '\\'; break;
            default:
                if (e >= '0' && e <= '9') { // \ddd: a decimal byte, up to three digits
                    u32 value = static_cast<u32>(e - '0');
                    for (u32 digits = 1; digits < 3 && i < line.size() && line[i] >= '0' && line[i] <= '9';
                         ++digits) {
                        value = value * 10 + static_cast<u32>(line[i++] - '0');
                    }
                    if (value > 255) {
                        return lineError(lineNumber, "escape out of range");
                    }
                    entry.text += static_cast<char>(value);
                } else {
                    entry.text += e; // unknown escape: the character itself, as Lua 5.0 does
                }
            }
        }
        if (!closed) {
            return lineError(lineNumber, "unterminated string");
        }
        out.push_back(std::move(entry));
    }
    return out;
}

} // namespace vd::formats
