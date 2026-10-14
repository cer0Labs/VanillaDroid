#include "game/ChatLinks.h"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace vd::game {
namespace {

// The next ':'-separated field of `rest` (consumed).
std::string_view field(std::string_view& rest) {
    const usize colon = rest.find(':');
    const std::string_view f = rest.substr(0, colon);
    rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
    return f;
}

bool number(std::string_view s, u32& out) {
    if (s.empty()) {
        return false;
    }
    const std::from_chars_result r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool hexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Appends `s` to `out` (NUL-terminated), cut to fit.
void put(std::span<char> out, usize& used, std::string_view s) {
    if (out.empty() || used + 1 >= out.size()) {
        return;
    }
    const usize n = std::min(s.size(), out.size() - 1 - used);
    std::memcpy(out.data() + used, s.data(), n);
    used += n;
    out[used] = 0;
}

} // namespace

ChatLinkKind parseChatLink(std::string_view link, ChatLink& out) {
    out.kind = ChatLinkKind::Other;
    out.id = 0;
    out.values = {};
    out.player = {};
    std::string_view rest = link;
    const std::string_view type = field(rest);
    if (type == "player") {
        // ItemRef.lua takes the name after "player:" (SetItemRef: strsub(link, 8)).
        if (!rest.empty()) {
            out.kind = ChatLinkKind::Player;
            out.player = rest;
        }
        return out.kind;
    }
    u32 id = 0;
    if (!number(field(rest), id) || id == 0) {
        return out.kind;
    }
    // The numbers after the id (missing ones are 0, as the client reads them).
    for (u32 i = 0; i < out.values.size() && !rest.empty(); ++i) {
        if (!number(field(rest), out.values[i])) {
            out.values = {};
            return out.kind;
        }
    }
    out.id = id;
    if (type == "item") {
        out.kind = ChatLinkKind::Item;
    } else if (type == "enchant") {
        out.kind = ChatLinkKind::Enchant;
    } else if (type == "quest") {
        out.kind = ChatLinkKind::Quest;
    }
    return out.kind;
}

bool nextChatLink(std::string_view line, usize from, ChatLink& out) {
    for (usize start = from; start + 1 < line.size(); ++start) {
        if (line[start] != '|') {
            continue;
        }
        if (line[start + 1] != 'H') {
            ++start; // an escape ("||", "|c", "|h"...): its second character is not a bar that starts one
            continue;
        }
        // "|H" + link + "|h" + text + "|h": neither part may hold another bar (an escape inside breaks it).
        const usize linkEnd = line.find('|', start + 2);
        if (linkEnd == std::string_view::npos) {
            return false;
        }
        const usize textEnd =
            linkEnd + 2 <= line.size() ? line.find('|', linkEnd + 2) : std::string_view::npos;
        if (linkEnd > start + 2 && linkEnd + 1 < line.size() && line[linkEnd + 1] == 'h' &&
            textEnd != std::string_view::npos && textEnd + 1 < line.size() && line[textEnd + 1] == 'h') {
            out = ChatLink{};
            out.link = line.substr(start + 2, linkEnd - start - 2);
            out.text = line.substr(linkEnd + 2, textEnd - linkEnd - 2);
            out.begin = start;
            out.end = textEnd + 2;
            parseChatLink(out.link, out);
            return true;
        }
        ++start;
    }
    return false;
}

bool chatLinkAt(std::string_view line, usize at, ChatLink& out) {
    usize from = 0;
    while (nextChatLink(line, from, out)) {
        if (at >= out.begin && at < out.end) {
            return true;
        }
        if (out.begin > at) {
            return false;
        }
        from = out.end;
    }
    return false;
}

void appendChatText(std::string& out, std::string_view text) {
    for (usize i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c != '|') {
            out.push_back(c);
            continue;
        }
        const char code = i + 1 < text.size() ? text[i + 1] : 0;
        if (code == '|') { // an escaped bar
            out.append("||");
            ++i;
            continue;
        }
        if (code == 'r') {
            out.append("|r");
            ++i;
            continue;
        }
        if (code == 'c' && i + 10 <= text.size() &&
            std::all_of(text.begin() + static_cast<std::ptrdiff_t>(i + 2),
                        text.begin() + static_cast<std::ptrdiff_t>(i + 10), hexDigit)) {
            out.append(text.substr(i, 10));
            i += 9;
            continue;
        }
        ChatLink link;
        if (code == 'H' && nextChatLink(text, i, link) && link.begin == i) {
            out.append(text.substr(i, link.end - i));
            i = link.end - 1;
            continue;
        }
        out.append("||"); // anything else is shown as typed: "|H" without its "|h", "|n", "|T"...
    }
}

std::string_view formatItemLink(std::span<char> out, u32 entry, std::string_view colorCode,
                                std::string_view name) {
    if (out.empty()) {
        return {};
    }
    out[0] = 0;
    usize used = 0;
    char number[16];
    const std::to_chars_result r = std::to_chars(number, number + sizeof(number), entry);
    put(out, used, colorCode);
    put(out, used, "|Hitem:");
    put(out, used, std::string_view(number, static_cast<usize>(r.ptr - number)));
    put(out, used, ":0:0:0|h[");
    put(out, used, name);
    put(out, used, "]|h|r");
    return {out.data(), used};
}

std::string_view formatItemLink(std::span<char> out, u32 entry, std::string_view colorCode, std::string_view name,
                                u32 enchant, u32 randomProperty, std::string_view suffix) {
    if (out.empty()) {
        return {};
    }
    out[0] = 0;
    usize used = 0;
    const auto number = [&](u32 value) {
        char digits[16];
        const std::to_chars_result r = std::to_chars(digits, digits + sizeof(digits), value);
        put(out, used, std::string_view(digits, static_cast<usize>(r.ptr - digits)));
    };
    put(out, used, colorCode);
    put(out, used, "|Hitem:");
    number(entry);
    put(out, used, ":");
    number(enchant);
    put(out, used, ":");
    number(randomProperty);
    put(out, used, ":0|h[");
    put(out, used, name);
    if (!suffix.empty()) {
        put(out, used, " "); // ITEM_SUFFIX_TEMPLATE "%s %s"
        put(out, used, suffix);
    }
    put(out, used, "]|h");
    if (!colorCode.empty()) {
        put(out, used, "|r");
    }
    return {out.data(), used};
}

void escapeChatInput(std::string& out, std::string_view typed, usize maxBytes) {
    out.clear();
    const auto fits = [&](usize bytes) { return out.size() + bytes <= maxBytes; };
    for (usize i = 0; i < typed.size(); ++i) {
        const char c = typed[i];
        if (c != '|') {
            if (!fits(1)) {
                return;
            }
            out.push_back(c);
            continue;
        }
        // A link as Insert put it: "|c" + 8 hex digits, its "|H..|h[..]|h", then "|r".
        ChatLink link;
        if (i + 10 < typed.size() && typed[i + 1] == 'c' &&
            std::all_of(typed.begin() + static_cast<std::ptrdiff_t>(i + 2),
                        typed.begin() + static_cast<std::ptrdiff_t>(i + 10), hexDigit) &&
            nextChatLink(typed, i + 10, link) && link.begin == i + 10 && link.end + 1 < typed.size() &&
            typed[link.end] == '|' && typed[link.end + 1] == 'r') {
            const usize length = link.end + 2 - i;
            if (!fits(length)) {
                return;
            }
            out.append(typed.substr(i, length));
            i += length - 1;
            continue;
        }
        if (!fits(2)) {
            return;
        }
        out.append("||");
    }
}

} // namespace vd::game
