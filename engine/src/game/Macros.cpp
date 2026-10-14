#include "game/Macros.h"

#include "core/Hash.h"
#include "core/Text.h"
#include "game/GameData.h"

#include <algorithm>
#include <cstdio>

namespace vd::game {
namespace {

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// stricmp's order (WoW.exe 0x64A4C0 with no length limit): the tabs' sort.
bool lessNoCase(std::string_view a, std::string_view b) {
    const usize n = std::min(a.size(), b.size());
    for (usize i = 0; i < n; ++i) {
        const char x = lower(a[i]), y = lower(b[i]);
        if (x != y) {
            return static_cast<u8>(x) < static_cast<u8>(y);
        }
    }
    return a.size() < b.size();
}

bool sameNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

bool startsNoCase(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && sameNoCase(s.substr(0, prefix.size()), prefix);
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// SLASH_CAST1..4 of GlobalStrings.lua ("/cast", "/spell") and this client's Spanish alias (game/ChatCommands).
constexpr std::string_view kCastCommands[] = {"/cast ", "/spell ", "/lanzar "};
constexpr std::string_view kCastSpellByName = "CastSpellByName(\"";

} // namespace

u64 macroIconHash(std::string_view iconName) {
    u64 hash = kFnvOffset;
    const auto add = [&hash](std::string_view part) {
        for (const char c : part) {
            hash ^= static_cast<u8>(normalizeAssetPathChar(c));
            hash *= kFnvPrime;
        }
    };
    add("Interface\\Icons\\");
    add(iconName);
    add(".blp");
    return hash;
}

ActionButton macroButton(u32 id) {
    // packed = 0x40000000 | id: the character's 0x1000000 bit becomes the type's low bit (ACTION_BUTTON_CMACRO).
    const u32 packed = 0x40000000u | id;
    return ActionButton::unpack(packed);
}

u32 macroIdOf(const ActionButton& button) {
    if ((button.type & 0xF0u) != 0x40u) { // ACTION_BUTTON_MACRO / _CMACRO (WoW.exe: packed & 0xF0000000)
        return 0;
    }
    return button.packed() & 0xBFFFFFFFu;
}

std::string_view clampUtf8(std::string_view text, usize maxBytes) {
    if (text.size() <= maxBytes) {
        return text;
    }
    usize end = maxBytes;
    while (end > 0 && (static_cast<u8>(text[end]) & 0xC0u) == 0x80u) {
        --end; // not inside a character
    }
    return text.substr(0, end);
}

std::string macroBodyText(std::string_view typed) {
    std::string body;
    body.reserve(std::min<usize>(typed.size(), kMacroBodyBytes));
    for (const char c : typed) {
        if (c != '\r') {
            body.push_back(c);
        }
    }
    body.resize(clampUtf8(body, kMacroBodyBytes).size());
    return body;
}

std::string_view macroCastText(std::string_view body) {
    std::string_view found;
    forEachMacroLine(body, [&found](std::string_view line) {
        if (!found.empty()) {
            return;
        }
        for (const std::string_view command : kCastCommands) {
            if (startsNoCase(line, command)) {
                found = trimmed(line.substr(command.size()));
                return;
            }
        }
        // Phase 21 (languages): the names of the client's language (SLASH_CAST1..: "/lanzar", "/hechizo" in esES).
        for (int n = 1; n <= 10 && text::loaded(); ++n) {
            char key[16];
            std::snprintf(key, sizeof(key), "SLASH_CAST%d", n);
            if (!text::has(key)) {
                continue;
            }
            const std::string_view command = text::get(key);
            if (!command.empty() && startsNoCase(line, command) && line.size() > command.size() &&
                line[command.size()] == ' ') {
                found = trimmed(line.substr(command.size() + 1));
                return;
            }
        }
    });
    if (!found.empty()) {
        return found;
    }
    // A /script line's CastSpellByName("Name(Rank n)").
    forEachMacroLine(body, [&found](std::string_view line) {
        const usize at = found.empty() ? line.find(kCastSpellByName) : std::string_view::npos;
        if (at == std::string_view::npos) {
            return;
        }
        std::string_view rest = line.substr(at + kCastSpellByName.size());
        const usize quote = rest.find('"');
        if (quote != std::string_view::npos) {
            found = trimmed(rest.substr(0, quote));
        }
    });
    return found;
}

u32 knownSpellByName(std::span<const u32> known, const GameData& data, std::string_view text) {
    text = trimmed(text);
    std::string_view name = text, rank;
    const usize open = text.find('(');
    if (open != std::string_view::npos && text.back() == ')') {
        name = trimmed(text.substr(0, open));
        rank = trimmed(text.substr(open + 1, text.size() - open - 2));
    }
    if (name.empty()) {
        return 0;
    }
    u32 best = 0;
    for (const u32 id : known) {
        const std::optional<SpellInfo> info = data.spell(id);
        if (!info || !sameNoCase(info->name, name) || (!rank.empty() && !sameNoCase(info->rank, rank))) {
            continue;
        }
        best = id; // the book keeps the ranks in order: the last one is the highest
    }
    return best;
}

// --- MacroStore ----------------------------------------------------------------------------------------------

void MacroStore::clear() {
    clear(MacroTab::Account);
    clear(MacroTab::Character);
}

void MacroStore::clear(MacroTab tab) {
    const u32 t = static_cast<u32>(tab);
    for (u32 i = 0; i < kMacrosPerTab; ++i) {
        m_slots[t * kMacrosPerTab + i] = Macro{};
    }
    m_count[t] = 0;
    m_lastId[t] = t == 0 ? 0 : kCharacterMacroIdBase;
    ++m_version;
}

const Macro* MacroStore::slot(u32 index) const {
    if (index >= kMacroSlots) {
        return nullptr;
    }
    const u32 t = index / kMacrosPerTab;
    return index % kMacrosPerTab < m_count[t] ? &m_slots[index] : nullptr;
}

const Macro* MacroStore::find(u32 id) const {
    const i32 index = slotOf(id);
    return index >= 0 ? &m_slots[static_cast<u32>(index)] : nullptr;
}

i32 MacroStore::slotOf(u32 id) const {
    if (id == 0) {
        return -1;
    }
    for (u32 t = 0; t < 2; ++t) {
        for (u32 i = 0; i < m_count[t]; ++i) {
            if (m_slots[t * kMacrosPerTab + i].id == id) {
                return static_cast<i32>(t * kMacrosPerTab + i);
            }
        }
    }
    return -1;
}

void MacroStore::sortTab(MacroTab tab) {
    const u32 t = static_cast<u32>(tab);
    const auto first = m_slots.begin() + t * kMacrosPerTab;
    std::stable_sort(first, first + m_count[t],
                     [](const Macro& a, const Macro& b) { return lessNoCase(a.name, b.name); });
}

i32 MacroStore::add(MacroTab tab, u32 id, std::string_view name, std::string_view icon, std::string_view body) {
    const u32 t = static_cast<u32>(tab);
    if (m_count[t] >= kMacrosPerTab) {
        return -1;
    }
    Macro& m = m_slots[t * kMacrosPerTab + m_count[t]];
    m = Macro{};
    m.id = id;
    m.name.assign(clampUtf8(name, kMacroNameBytes));
    m.icon.assign(icon);
    m.body = macroBodyText(body);
    m.iconHash = icon.empty() ? 0 : macroIconHash(icon);
    ++m_count[t];
    sortTab(tab);
    ++m_version;
    return slotOf(id);
}

i32 MacroStore::create(MacroTab tab, std::string_view name, std::string_view icon, std::string_view body) {
    const u32 t = static_cast<u32>(tab);
    if (trimmed(name).empty() || m_count[t] >= kMacrosPerTab) {
        return -1; // "CreateMacro() failed, no name specified" / "already have %d macros"
    }
    return add(tab, ++m_lastId[t], name, icon, body);
}

i32 MacroStore::edit(u32 index, std::string_view name, std::string_view icon, bool setBody, std::string_view body) {
    if (!slot(index)) {
        return -1;
    }
    Macro& m = m_slots[index];
    if (!trimmed(name).empty()) {
        m.name.assign(clampUtf8(name, kMacroNameBytes));
    }
    if (!icon.empty()) {
        m.icon.assign(icon);
        m.iconHash = macroIconHash(icon);
    }
    if (setBody) {
        m.body = macroBodyText(body);
    }
    const u32 id = m.id;
    sortTab(tabOf(index));
    ++m_version;
    return slotOf(id);
}

void MacroStore::remove(u32 index) {
    if (!slot(index)) {
        return;
    }
    const u32 t = index / kMacrosPerTab;
    const u32 last = t * kMacrosPerTab + m_count[t] - 1;
    for (u32 i = index; i < last; ++i) {
        m_slots[i] = std::move(m_slots[i + 1]);
    }
    m_slots[last] = Macro{};
    --m_count[t];
    ++m_version;
}

std::string MacroStore::format(MacroTab tab) const {
    const u32 t = static_cast<u32>(tab);
    std::string text;
    char head[160];
    for (u32 i = 0; i < m_count[t]; ++i) {
        const Macro& m = m_slots[t * kMacrosPerTab + i];
        std::snprintf(head, sizeof(head), "MACRO %u \"%.*s\" %.*s\r\n", m.id, static_cast<int>(m.name.size()),
                      m.name.data(), static_cast<int>(m.icon.size()), m.icon.data());
        text += head;
        forEachMacroLine(m.body, [&text](std::string_view line) {
            text += line;
            text += "\r\n";
        });
        text += "END\r\n";
    }
    return text;
}

u32 MacroStore::parse(MacroTab tab, std::string_view text) {
    clear(tab);
    const u32 t = static_cast<u32>(tab);
    const u32 base = t == 0 ? 0 : kCharacterMacroIdBase;
    u32 read = 0;
    bool inMacro = false;
    u32 id = 0;
    std::string_view name, icon;
    std::string body;
    while (!text.empty()) {
        const usize end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        while (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!inMacro) {
            // MACRO <id> "<name>" <icon>
            if (!startsNoCase(line, "MACRO ")) {
                continue;
            }
            std::string_view rest = line.substr(6);
            u32 value = 0;
            usize digits = 0;
            while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9') {
                value = value * 10 + static_cast<u32>(rest[digits] - '0');
                ++digits;
            }
            const usize open = rest.find('"');
            const usize close = open == std::string_view::npos ? open : rest.find('"', open + 1);
            if (digits == 0 || value == 0 || close == std::string_view::npos) {
                continue;
            }
            id = value < base ? value + base : value;
            name = rest.substr(open + 1, close - open - 1);
            icon = trimmed(rest.substr(close + 1));
            body.clear();
            inMacro = true;
            continue;
        }
        if (sameNoCase(trimmed(line), "END")) {
            inMacro = false;
            if (find(id) == nullptr && add(tab, id, name, icon, body) >= 0) {
                m_lastId[t] = std::max(m_lastId[t], id);
                ++read;
            }
            continue;
        }
        if (!body.empty()) {
            body += '\n';
        }
        body += line;
    }
    ++m_version;
    return read;
}

} // namespace vd::game
