#include "game/WhoQuery.h"

#include <algorithm>
#include <cstdio>

namespace vd::game {
namespace {

constexpr usize kTextBytes = 127; // the parser's 128-byte buffer
constexpr usize kNameBytes = 47;  // the name and guild fields

char fold(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; // _strnicmp folds A-Z only
}

bool tagged(std::string_view word, std::string_view tag) {
    if (word.size() < tag.size()) {
        return false;
    }
    for (usize i = 0; i < tag.size(); ++i) {
        if (fold(word[i]) != fold(tag[i])) {
            return false;
        }
    }
    return true;
}

// Whether `haystack` contains `needle`, A-Z folded (an empty needle is everywhere).
bool contains(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (usize at = 0; at + needle.size() <= haystack.size(); ++at) {
        usize i = 0;
        while (i < needle.size() && fold(haystack[at + i]) == fold(needle[i])) {
            ++i;
        }
        if (i == needle.size()) {
            return true;
        }
    }
    return false;
}

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

bool digit(char c) {
    return c >= '0' && c <= '9';
}

// The number at the start of `s` (its digits), and how many characters it took.
u32 leadingNumber(std::string_view s, usize& used) {
    u32 n = 0;
    used = 0;
    while (used < s.size() && digit(s[used])) {
        n = n * 10u + static_cast<u32>(s[used] - '0');
        ++used;
    }
    return n;
}

// The word's meaning; false once four words have been searched (the rest is ignored).
bool apply(std::string_view word, const WhoQueryNames& names, WhoQuery& q) {
    if (tagged(word, "n-")) {
        q.name.assign(word.substr(2, kNameBytes));
        return true;
    }
    if (tagged(word, "g-")) {
        q.guild.assign(word.substr(2, kNameBytes));
        return true;
    }
    if (tagged(word, "z-")) {
        if (q.zoneCount >= q.zones.size()) {
            return true;
        }
        const std::string_view value = word.substr(2);
        const usize n = std::min(names.zones.size(), names.zoneNames.size());
        for (usize i = 0; i < n && q.zoneCount < q.zones.size(); ++i) {
            if (contains(names.zoneNames[i], value)) {
                q.zones[q.zoneCount++] = names.zones[i];
            }
        }
        if (q.zoneCount == 0) {
            q.zones[q.zoneCount++] = 0;
        }
        return true;
    }
    const auto mask = [&](std::string_view value, std::span<const std::string_view> table, u32& m) {
        if (m == 0xFFFFFFFFu) {
            m = 0;
        }
        for (usize id = 0; id < table.size() && id < 32; ++id) {
            if (!table[id].empty() && contains(table[id], value)) {
                m |= 1u << id;
            }
        }
    };
    if (tagged(word, "r-")) {
        mask(word.substr(2), names.races, q.raceMask);
        return true;
    }
    if (tagged(word, "c-")) {
        mask(word.substr(2), names.classes, q.classMask);
        return true;
    }
    if (!word.empty() && digit(word[0])) {
        usize used = 0;
        q.minLevel = q.maxLevel = leadingNumber(word, used);
        if (used < word.size() && word[used] == '-') {
            usize more = 0;
            const u32 high = leadingNumber(word.substr(used + 1), more);
            q.maxLevel = more > 0 ? high : 100u;
        }
        return true;
    }
    if (word.size() >= 2 && word[0] == '-' && digit(word[1])) {
        usize used = 0;
        q.maxLevel = leadingNumber(word.substr(1), used);
        return true;
    }
    if (q.termCount < q.terms.size()) {
        q.terms[q.termCount++].assign(word);
    }
    return q.termCount < q.terms.size();
}

} // namespace

void parseWhoQuery(std::string_view text, const WhoQueryNames& names, WhoQuery& out) {
    out = WhoQuery{};
    text = text.substr(0, kTextBytes);
    std::string word;
    bool quoted = false;
    bool started = false; // a word (or a quote) began
    // The text and a space after it, which ends the last word unless a quote is still open.
    for (usize i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ' ';
        if (c == '"') {
            quoted = !quoted;
            started = true;
            continue;
        }
        if (space(c) && !quoted) {
            if (started && !apply(word, names, out)) {
                return;
            }
            word.clear();
            started = false;
            continue;
        }
        word.push_back(c);
        started = true;
    }
}

std::string defaultWhoQuery(std::string_view zone, u32 level) {
    // z-"<GetRealZoneText()>" <max(level - 3, 1)>-<level + 3>
    char levels[32];
    std::snprintf(levels, sizeof(levels), " %u-%u", level > 4 ? level - 3 : 1u, level + 3);
    std::string out = "z-\"";
    out.append(zone);
    out.append("\"");
    out.append(levels);
    return out;
}

} // namespace vd::game
