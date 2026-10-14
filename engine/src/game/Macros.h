#pragma once

#include "core/Types.h"
#include "game/Spells.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace vd::game {

class GameData;

// Phase 15 (options): the macros of Blizzard_MacroUI as WoW.exe keeps them (UIMacros.cpp, read from its disassembly:
// CreateMacro 0x4F15B0 -> 0x4F1010, the slots 0x4F0D30, the files 0x4F0600 / 0x4F0AB0, UseAction 0x4E608B ->
// 0x4F1460). Client-side only: the server keeps nothing but the action buttons that point at them.
//
// - Two tabs of MAX_MACROS (18): the account's ("General Macros", WTF\Account\<ACCOUNT>\macros-cache.txt) and the
//   character's ("<name> Specific Macros", WTF\Account\<ACCOUNT>\<realm>\<character>\macros-local.txt). The 36 slots
//   are GetMacroInfo's indices: the account's first, then the character's, each tab sorted by name (stricmp).
// - Ids never change: the account's count up from 1, the character's from 0x1000001 (two counters that keep the
//   highest id read). An action button holds ACTION_BUTTON_MACRO (0x40) and the id's low 24 bits; the character's
//   0x1000000 bit lands in the type's low bit, ACTION_BUTTON_CMACRO (0x41): WoW.exe reads `packed & 0xBFFFFFFF`.
// - The icon is a file name of Interface\Icons (GetMacroIconInfo: "Interface\Icons\" .. name), the name at most 16
//   bytes (MacroPopupEditBox), the body at most 255 (MacroFrameText), its lines split at line breaks (empty ones
//   are not kept: the writer tokenizes on "\r\n").
// - The body's first "/cast" (or CastSpellByName("...")) line names the macro's spell: an action button holding the
//   macro shows that spell's cooldown, power, range and state (WoW.exe 0x4EFE00 / 0x4E5A50); its icon and name are
//   the macro's (GetActionTexture, GetActionText).
// - Pressing it runs every line in order as if typed in the chat (EXECUTE_CHAT_LINE: ChatEdit_SendText): slash
//   commands, emotes, and plain text said in the chat's current channel. 1.12 has no "one cast per press" rule (that
//   came with 2.0's secure macros): a second cast in the same press is refused by the cast in progress or the global
//   cooldown, as the server answers it.
// Main thread; changes allocate (they are rare: the player edits a macro), lookups do not.

constexpr u32 kMacrosPerTab = 18;                     // MAX_MACROS
constexpr u32 kMacroSlots = 2 * kMacrosPerTab;        // the account's 0..17, the character's 18..35
constexpr u32 kMacroNameBytes = 16;                   // MacroPopupEditBox letters="16"
constexpr u32 kMacroBodyBytes = 255;                  // MacroFrameText letters="255"
constexpr u32 kCharacterMacroIdBase = 0x1000000;      // WoW.exe's second id counter (0x807B90)

enum class MacroTab : u8 { Account, Character };

struct Macro {
    u32 id = 0; // 0: an empty slot
    std::string name;
    std::string icon; // a file of Interface\Icons without its extension ("Ability_Warrior_Charge")
    std::string body; // lines separated by '\n'
    u64 iconHash = 0; // assetPathHash of Interface\Icons\<icon>.blp (textures.vdpk)
};

// The texture of a macro icon (Interface\Icons\<name>.blp), hashed without building the path.
u64 macroIconHash(std::string_view iconName);

// The action button that holds macro `id`, and the macro a button holds (0: none).
ActionButton macroButton(u32 id);
u32 macroIdOf(const ActionButton& button);
inline MacroTab macroTabOf(u32 id) { return id > kCharacterMacroIdBase ? MacroTab::Character : MacroTab::Account; }

// What the field takes: a name cut at kMacroNameBytes, a body without carriage returns cut at kMacroBodyBytes (both
// on a whole UTF-8 character).
std::string_view clampUtf8(std::string_view text, usize maxBytes);
std::string macroBodyText(std::string_view typed);

// The text of the body's first cast (WoW.exe 0x4EFE00): the rest of the first line starting with SLASH_CAST
// ("/cast ", "/spell ", and this client's Spanish alias "/lanzar ") or the quoted name of a CastSpellByName("...")
// line; empty when none ("Fireball(Rank 3)" names a rank).
std::string_view macroCastText(std::string_view body);

// CastSpellByName's lookup: the known spell of that name, that rank ("Fireball(Rank 3)") or the highest one the book
// has; 0 when not known.
u32 knownSpellByName(std::span<const u32> known, const GameData& data, std::string_view text);

// Calls f(line) for each line of `body` to run, in order (blank ones are not lines: the writer drops them).
template <typename F> void forEachMacroLine(std::string_view body, F&& f);

class MacroStore {
public:
    void clear();
    void clear(MacroTab tab);

    u32 count(MacroTab tab) const { return m_count[static_cast<u32>(tab)]; }
    // GetMacroInfo(slot + 1): the macro in a slot (0..35), null when empty.
    const Macro* slot(u32 index) const;
    const Macro* find(u32 id) const;
    i32 slotOf(u32 id) const; // -1 when not there
    static MacroTab tabOf(u32 slot) { return slot < kMacrosPerTab ? MacroTab::Account : MacroTab::Character; }

    // CreateMacro: a new macro in `tab` (refused: -1, when the tab is full or the name empty). Returns its slot.
    i32 create(MacroTab tab, std::string_view name, std::string_view icon, std::string_view body);
    // EditMacro: an empty name or icon keeps the old one; the body only changes with `setBody`. Returns its slot
    // (the tab is sorted again by name), -1 when the slot is empty.
    i32 edit(u32 slot, std::string_view name, std::string_view icon, bool setBody, std::string_view body);
    // DeleteMacro.
    void remove(u32 slot);

    // macros-cache.txt / macros-local.txt as WoW.exe writes them ("MACRO <id> \"<name>\" <icon>\r\n", the body's
    // lines each ended by "\r\n", "END\r\n"), and read back: every macro of `tab` replaced by the file's. Ids below the
    // tab's base get it added (as the client's reader does); macros past MAX_MACROS are dropped. Returns how many were
    // read.
    std::string format(MacroTab tab) const;
    u32 parse(MacroTab tab, std::string_view text);

    // Bumped on every change (the action bars and the window rebuild what they show from the macros).
    u32 version() const { return m_version; }

private:
    void sortTab(MacroTab tab);
    i32 add(MacroTab tab, u32 id, std::string_view name, std::string_view icon, std::string_view body);

    std::array<Macro, kMacroSlots> m_slots{};
    std::array<u32, 2> m_count{};
    std::array<u32, 2> m_lastId{0, kCharacterMacroIdBase};
    u32 m_version = 1;
};

// -------------------------------------------------------------------------------------------------------

template <typename F> void forEachMacroLine(std::string_view body, F&& f) {
    while (!body.empty()) {
        const usize end = body.find('\n');
        std::string_view line = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        if (!line.empty()) {
            f(line);
        }
    }
}

} // namespace vd::game
