#pragma once

#include "core/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// Phase 18 (extras, B4): the languages of the 1.12 chat. vMaNGOS relays a /say, a /yell or a creature's line
// in the speaker's language untouched (ChatHandler.cpp never garbles); the 1.12 CLIENT turns every word of a
// language its player does not know into a word of that language (LanguageWords.dbc), picked by a hash of the
// word, before the chat frame and the chat bubble get the line (WoW.exe 0x49A970 → 0x49B560), and
// ChatFrame.lua writes the language before the text ("[Orcish] ") when it is not the player's own
// (GetDefaultLanguage) nor Universal.
//
// WoW.exe 1.12.1 (read only):
// - 0x4982C0: the words by (language, byte length) from LanguageWords.dbc (0xC0DB54), empty words skipped.
// - 0x49B560: the scrambling. Words are runs of letters (A-Z, a-z, Latin-1 letters), digits, apostrophes and
//   any character past U+00FF (UTF-8: 0x41AAB0, 0x49B940); each one hashes with SStrHash (0x64AF90:
//   case-insensitive, seed 0). hash % 300 < the listener's skill keeps the word (300 or more: the whole text
//   as is); otherwise it becomes the word of index hash % count among the language's words of its length
//   (capped at 18 bytes, one shorter while the language has none of that length; a word with none down to 1
//   byte is dropped), cut to the shorter of both and cased letter by letter as the original (0x49B8B0).
//   Between words, the chat (0x49AA7C) writes one space for any run of other characters; the gossip texts
//   (0x4E22AB) keep them, and 0x4E3856 also keeps "<...>" as is.
// - 0x5EC720: the listener's skill in a language. The spell that taught it (the last learned spell whose
//   first effect is SPELL_EFFECT_LANGUAGE with that language: 0x4B2641), its skill line for the player's race
//   and class (SkillLineAbility, 0x6DE040) and that skill in the player's fields (PLAYER_SKILL_INFO: the
//   value, plus the permanent bonus when above 0, plus the temporary one); 0 when any is missing.
// - 0x49A970: no garbling for a game master (PLAYER_FLAGS_GM) nor in Universal; the emotes, the system lines
//   and the creatures' emotes count as Universal (no garbling, no "[language]").
// The tables stay in the mapped db pack; this only indexes them. Read only after init: any thread.
constexpr u32 kLanguageFullSkill = 300;     // a skill this high understands every word (0x49B599)
constexpr u32 kLanguageMaxWordLength = 18;  // longer words are replaced by words of this length (0x49B7C1)
constexpr u32 kSpellEffectLanguage = 39;    // SPELL_EFFECT_LANGUAGE
constexpr u32 kPlayerFlagGameMaster = 0x08; // PLAYER_FLAGS_GM (.gm on): the client garbles nothing

class LanguageData {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return !m_groups.empty(); }

    // A spell that teaches a language (Spell.dbc: its first effect SPELL_EFFECT_LANGUAGE, the language in its
    // misc value) with one of its SkillLineAbility rows (skillLine 0 when it has none).
    struct Teacher {
        u32 spell = 0;
        u32 language = 0;
        u32 skillLine = 0;
        u32 raceMask = 0; // 0: any
        u32 classMask = 0;
    };
    struct Word {
        u32 language = 0;
        std::string_view text;
    };
    // Tests and tools: the tables from lists (`words` in LanguageWords.dbc's record order); the strings must
    // outlive this object.
    void set(std::span<const std::pair<u32, std::string_view>> names, std::span<const Word> words,
             std::span<const Teacher> teachers);

    // Languages.dbc's name ("Orcish"), empty for Universal and unknown languages.
    std::string_view name(u32 language) const;
    // Every language of Languages.dbc with its name, in the table's order (GetLanguageByIndex walks it).
    std::span<const std::pair<u32, std::string_view>> names() const { return m_names; }
    // The words of a language that are `length` bytes long, in the table's order (empty when none).
    std::span<const std::string_view> words(u32 language, u32 length) const;
    // The language a spell teaches, false when it teaches none.
    bool teaches(u32 spell, u32& language) const;
    // The spell's skill line for a race and class (ChrRaces / ChrClasses ids), 0 when no row fits.
    u32 skillLine(u32 spell, u8 race, u8 cls) const;

private:
    struct Group {
        u32 language = 0;
        u32 length = 0;
        u32 first = 0; // into m_words
        u32 count = 0;
    };
    void build(std::vector<Word>& words);

    std::vector<std::pair<u32, std::string_view>> m_names; // by language
    std::vector<std::string_view> m_words;                 // grouped by m_groups
    std::vector<Group> m_groups;                           // by language, then length
    std::vector<Teacher> m_teachers;                       // by spell
};

// SStrHash(word, 0, 0) as WoW.exe 0x64AF90 computes it (case-insensitive, '/' as '\').
u32 languageHash(std::string_view word);

struct ScrambleRules {
    bool keepPunctuation = false; // the gossip texts: what lies between words stays (the chat: one space)
    bool keepTags = false;        // "<...>" stays as is (0x4E3856)
};
// The text as a listener with `skill` in `language` reads it (0x49B560) into `out` (cleared first): unchanged
// in Universal or with kLanguageFullSkill. At most `maxBytes` bytes (the client's buffer less its NUL: 2047
// for the chat).
void scrambleLanguage(const LanguageData& data, u32 language, u32 skill, std::string_view text,
                      std::string& out, ScrambleRules rules = {}, usize maxBytes = 2047);

} // namespace vd::game
