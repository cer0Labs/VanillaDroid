#include "game/Languages.h"

#include "core/Log.h"
#include "db/DbcView.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <array>
#include <optional>

namespace vd::game {
namespace {

// Field numbers (1.12.1): Languages.dbc (id, name[8], flags), LanguageWords.dbc (id, language, word),
// Spell.dbc (game/GameData: Effect[0] at 61, EffectMiscValue[0] at 106), SkillLineAbility.dbc
// (game/AbilityData).
constexpr u32 kLanguageName = 1, kLanguageFields = 10;
constexpr u32 kWordLanguage = 1, kWordText = 2, kWordFields = 3;
constexpr u32 kSpellEffect0 = 61, kSpellMisc0 = 106, kSpellFields = 173;
constexpr u32 kAbilitySkill = 1, kAbilitySpell = 2, kAbilityRaceMask = 3, kAbilityClassMask = 4,
              kAbilityFields = 15;

// SStrHash's table (0x80E4E0).
constexpr std::array<u32, 16> kHashTable = {
    0x486e26ee, 0xdcaa16b3, 0xe1918eef, 0x202dafdb, 0x341c7dc7, 0x1c365303, 0x40ef2d37, 0x65fd5e49,
    0xd6057177, 0x904ece93, 0x1c38024f, 0x98fd323b, 0xe3061ae7, 0xa39b0fa1, 0x9797f25f, 0xe4444563};

constexpr i32 kEnd = -1;
constexpr i32 kInvalid = static_cast<i32>(0x80000000u);

u32 bit(u32 value) {
    return value >= 1 && value <= 32 ? 1u << (value - 1) : 0u;
}

std::optional<db::DbcView> table(const pack::PackReader& db, const char* name, u32 minFields) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    if (!asset) {
        VD_LOG_WARN("%s.dbc missing", name);
        return std::nullopt;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed || parsed.value().fieldCount() < minFields) {
        VD_LOG_WARN("%s.dbc unreadable", name);
        return std::nullopt;
    }
    return parsed.value();
}

// The next character of `text` at `at` (0x41AAB0): its code point and bytes, kEnd at the end (a NUL or a
// sequence cut by it), kInvalid for a byte that starts nothing or a sequence broken by a byte that does not
// continue it.
i32 nextChar(std::string_view text, usize at, u32& bytes) {
    bytes = 0;
    if (at >= text.size() || text[at] == 0) {
        return kEnd;
    }
    const u32 lead = static_cast<u8>(text[at]);
    bytes = 1;
    u32 value = 0;
    u32 more = 0;
    if ((lead & 0xFE) == 0xFC) {
        value = lead & 0x01, more = 5;
    } else if ((lead & 0xFC) == 0xF8) {
        value = lead & 0x03, more = 4;
    } else if ((lead & 0xF8) == 0xF0) {
        value = lead & 0x07, more = 3;
    } else if ((lead & 0xF0) == 0xE0) {
        value = lead & 0x0F, more = 2;
    } else if ((lead & 0xE0) == 0xC0) {
        value = lead & 0x1F, more = 1;
    } else {
        return (lead & 0x80) != 0 ? kInvalid : static_cast<i32>(lead);
    }
    for (u32 i = 0; i < more; ++i) {
        const usize next = at + bytes;
        if (next >= text.size() || text[next] == 0) {
            return kEnd;
        }
        const u32 c = static_cast<u8>(text[next]);
        ++bytes;
        if ((c & 0xC0) != 0x80) {
            return kInvalid;
        }
        value = (value << 6) | (c & 0x3F);
    }
    return static_cast<i32>(value);
}

// 0x49B940: letters (0x6C9C60: A-Z, a-z, 0xC0-0xDD but 0xD7, 0xDF, 0xE0-0xFF but 0xF7), digits, the
// apostrophe and anything past 0xFF (the invalid ones too: they compare as 0x80000000).
bool wordChar(i32 c) {
    const u32 u = static_cast<u32>(c);
    if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '\'') {
        return true;
    }
    if ((u >= 0xC0 && u <= 0xDD && u != 0xD7) || u == 0xDF || (u >= 0xE0 && u <= 0xFF && u != 0xF7)) {
        return true;
    }
    return u > 0xFF;
}

// SStrAppend into a buffer of maxBytes + 1.
void append(std::string& out, std::string_view s, usize maxBytes) {
    if (out.size() < maxBytes) {
        out.append(s.substr(0, maxBytes - out.size()));
    }
}

// 0x49B8B0: the replacement cased letter by letter as the original (C locale: ASCII only), as long as both
// last.
void appendCased(std::string& out, std::string_view original, std::string_view replacement, usize maxBytes) {
    for (usize i = 0; i < replacement.size() && i < original.size() && out.size() < maxBytes; ++i) {
        const char o = original[i];
        char r = replacement[i];
        if (o >= 'A' && o <= 'Z') {
            r = r >= 'a' && r <= 'z' ? static_cast<char>(r - 'a' + 'A') : r;
        } else {
            r = r >= 'A' && r <= 'Z' ? static_cast<char>(r - 'A' + 'a') : r;
        }
        out.push_back(r);
    }
}

} // namespace

bool LanguageData::init(const pack::PackReader& db) {
    m_names.clear();
    m_words.clear();
    m_groups.clear();
    m_teachers.clear();
    const std::optional<db::DbcView> languages = table(db, "Languages", kLanguageFields);
    const std::optional<db::DbcView> words = table(db, "LanguageWords", kWordFields);
    const std::optional<db::DbcView> spells = table(db, "Spell", kSpellFields);
    const std::optional<db::DbcView> abilities = table(db, "SkillLineAbility", kAbilityFields);
    if (!languages || !words) {
        return false;
    }
    for (u32 r = 0; r < languages->recordCount(); ++r) {
        m_names.emplace_back(languages->getU32(r, 0), languages->getString(r, kLanguageName));
    }
    std::sort(m_names.begin(), m_names.end());
    std::vector<Word> list;
    list.reserve(words->recordCount());
    for (u32 r = 0; r < words->recordCount(); ++r) {
        list.push_back({words->getU32(r, kWordLanguage), words->getString(r, kWordText)});
    }
    build(list);
    if (spells) {
        // The spells that teach a language (0x4B2641 reads the first effect only).
        for (u32 r = 0; r < spells->recordCount(); ++r) {
            if (spells->getU32(r, kSpellEffect0) == kSpellEffectLanguage) {
                m_teachers.push_back({spells->getU32(r, 0), spells->getU32(r, kSpellMisc0), 0, 0, 0});
            }
        }
        std::sort(m_teachers.begin(), m_teachers.end(),
                  [](const Teacher& a, const Teacher& b) { return a.spell < b.spell; });
    }
    if (abilities) {
        // Their skill lines: one row per SkillLineAbility row of the spell (the first stays the spell's own).
        const usize spellsOnly = m_teachers.size();
        for (u32 r = 0; r < abilities->recordCount(); ++r) {
            const u32 spell = abilities->getU32(r, kAbilitySpell);
            const auto it = std::lower_bound(m_teachers.begin(),
                                             m_teachers.begin() + static_cast<std::ptrdiff_t>(spellsOnly),
                                             spell, [](const Teacher& t, u32 id) { return t.spell < id; });
            if (it == m_teachers.begin() + static_cast<std::ptrdiff_t>(spellsOnly) || it->spell != spell) {
                continue;
            }
            Teacher row = *it;
            row.skillLine = abilities->getU32(r, kAbilitySkill);
            row.raceMask = abilities->getU32(r, kAbilityRaceMask);
            row.classMask = abilities->getU32(r, kAbilityClassMask);
            if (it->skillLine == 0) {
                *it = row;
            } else {
                m_teachers.push_back(row);
            }
        }
        std::stable_sort(m_teachers.begin(), m_teachers.end(),
                         [](const Teacher& a, const Teacher& b) { return a.spell < b.spell; });
    }
    VD_LOG_INFO("Languages: %u names, %u words in %u groups, %u teaching spells",
                static_cast<u32>(m_names.size()), static_cast<u32>(m_words.size()),
                static_cast<u32>(m_groups.size()), static_cast<u32>(m_teachers.size()));
    return ready();
}

void LanguageData::set(std::span<const std::pair<u32, std::string_view>> names, std::span<const Word> words,
                       std::span<const Teacher> teachers) {
    m_names.assign(names.begin(), names.end());
    std::sort(m_names.begin(), m_names.end());
    std::vector<Word> list(words.begin(), words.end());
    build(list);
    m_teachers.assign(teachers.begin(), teachers.end());
    std::stable_sort(m_teachers.begin(), m_teachers.end(),
                     [](const Teacher& a, const Teacher& b) { return a.spell < b.spell; });
}

void LanguageData::build(std::vector<Word>& words) {
    // 0x4982C0: grouped by language and byte length, in the table's order within a group; empty words left
    // out.
    std::erase_if(words, [](const Word& w) { return w.text.empty(); });
    std::stable_sort(words.begin(), words.end(), [](const Word& a, const Word& b) {
        return a.language != b.language ? a.language < b.language : a.text.size() < b.text.size();
    });
    m_words.clear();
    m_groups.clear();
    m_words.reserve(words.size());
    for (const Word& w : words) {
        const u32 length = static_cast<u32>(w.text.size());
        if (m_groups.empty() || m_groups.back().language != w.language || m_groups.back().length != length) {
            m_groups.push_back({w.language, length, static_cast<u32>(m_words.size()), 0});
        }
        m_words.push_back(w.text);
        ++m_groups.back().count;
    }
}

std::string_view LanguageData::name(u32 language) const {
    const auto it =
        std::lower_bound(m_names.begin(), m_names.end(), std::make_pair(language, std::string_view{}));
    return it != m_names.end() && it->first == language ? it->second : std::string_view{};
}

std::span<const std::string_view> LanguageData::words(u32 language, u32 length) const {
    const auto it =
        std::lower_bound(m_groups.begin(), m_groups.end(), std::make_pair(language, length),
                         [](const Group& g, const std::pair<u32, u32>& key) {
                             return g.language != key.first ? g.language < key.first : g.length < key.second;
                         });
    if (it == m_groups.end() || it->language != language || it->length != length) {
        return {};
    }
    return {m_words.data() + it->first, it->count};
}

bool LanguageData::teaches(u32 spell, u32& language) const {
    const auto it = std::lower_bound(m_teachers.begin(), m_teachers.end(), spell,
                                     [](const Teacher& t, u32 id) { return t.spell < id; });
    if (it == m_teachers.end() || it->spell != spell) {
        return false;
    }
    language = it->language;
    return true;
}

u32 LanguageData::skillLine(u32 spell, u8 race, u8 cls) const {
    auto it = std::lower_bound(m_teachers.begin(), m_teachers.end(), spell,
                               [](const Teacher& t, u32 id) { return t.spell < id; });
    for (; it != m_teachers.end() && it->spell == spell; ++it) {
        if (it->skillLine != 0 && (it->raceMask == 0 || (it->raceMask & bit(race)) != 0) &&
            (it->classMask == 0 || (it->classMask & bit(cls)) != 0)) {
            return it->skillLine;
        }
    }
    return 0;
}

u32 languageHash(std::string_view word) {
    u32 seed = 0x7FED7FEDu;
    u32 shift = 0xEEEEEEEEu;
    for (const char ch : word) {
        u32 c = static_cast<u8>(ch);
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        if (c == '/') {
            c = '\\';
        }
        seed += shift;
        shift = shift * 33 + c;
        seed ^= kHashTable[c >> 4] - kHashTable[c & 0x0F];
        shift += seed + 3;
    }
    return seed != 0 ? seed : 1;
}

void scrambleLanguage(const LanguageData& data, u32 language, u32 skill, std::string_view text,
                      std::string& out, ScrambleRules rules, usize maxBytes) {
    out.clear();
    if (language == 0 || skill >= kLanguageFullSkill) {
        append(out, text, maxBytes);
        return;
    }
    usize at = 0;
    u32 bytes = 0;
    for (;;) {
        // Between words: one space for the run (or the run itself), "<...>" as is when asked.
        bool spaced = false;
        i32 c = nextChar(text, at, bytes);
        while (c != kEnd && !wordChar(c)) {
            if (c == '<' && rules.keepTags) {
                for (;;) {
                    const i32 tag = nextChar(text, at, bytes);
                    if (tag == kEnd) {
                        break;
                    }
                    append(out, text.substr(at, bytes), maxBytes);
                    at += bytes;
                    if (tag == '>') {
                        break;
                    }
                }
                c = nextChar(text, at, bytes);
                continue;
            }
            if (rules.keepPunctuation) {
                append(out, text.substr(at, bytes), maxBytes);
            } else if (!spaced) {
                append(out, " ", maxBytes);
                spaced = true;
            }
            at += bytes;
            c = nextChar(text, at, bytes);
        }
        if (c == kEnd) {
            return;
        }
        // The word, as far as its characters go (256 bytes of it at most).
        usize end = at;
        for (i32 w = nextChar(text, end, bytes); w != kEnd && wordChar(w); w = nextChar(text, end, bytes)) {
            end += bytes;
        }
        const std::string_view word = text.substr(at, std::min<usize>(end - at, 256));
        at = end;
        const u32 hash = languageHash(word);
        if (hash % kLanguageFullSkill < skill) {
            append(out, word, maxBytes); // a word the listener knows
            continue;
        }
        u32 length = std::min<u32>(static_cast<u32>(word.size()), kLanguageMaxWordLength);
        std::span<const std::string_view> choices = data.words(language, length);
        while (choices.empty() && length > 1) {
            choices = data.words(language, --length);
        }
        if (!choices.empty()) {
            appendCased(out, word, choices[hash % choices.size()], maxBytes);
        }
    }
}

} // namespace vd::game
