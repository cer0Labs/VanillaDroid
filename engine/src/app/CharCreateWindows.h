#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "ui/Context.h"
#include "ui/DrawList.h"
#include "ui/TextInput.h"
#include "ui/Widgets.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

// Phase 18 (create): GlueXML's CharacterCreate (CharacterCreate.xml / .lua) as a pure view over the glue
// scene (the race's place and the character being made, drawn by app/GlueStage behind it): the configuration
// frame (the faction banners with the race buttons, the gender buttons, the class buttons of the race, the
// five customization rows and Randomize), the faction, race and class texts with their icons, the name box
// with "Randomize" for a random name (shown by the enUS client's GlueLocalization.lua), the rotate buttons,
// Back and Accept, and the GlueDialog the C side opens over it (creating, the name's and the server's
// errors). Laid out by app/CharCreateLayout. Same rules as app/Dialogs: a view in, what the player did out;
// no game state, no network (app/CharCreateScreens fills it and applies the result). Texts by key (core/Text:
// GlueStrings', VD_CREATE_* of locale/*/charcreate.lang); names as given.
//
// Touch: a tap is the mouse's click; a long press on a race, class or gender button is its hover (OnEnter's
// GlueTooltip with the name: none for the chosen one); a horizontal drag on the free middle turns the
// character (CharacterCreateFrame_OnMouseDown / OnUpdate: 0.6 degree per unit); holding a rotate button turns
// it as their OnUpdate does (2 degrees a frame of 60 fps). The texts scroll by dragging them.

// What GlueDialog shows over the screen.
enum class CharCreateDialog : u8 {
    None,
    Creating, // OPEN_STATUS_DIALOG "CANCEL" with CHAR_CREATE_IN_PROGRESS (WoW.exe 0x46B290)
    Message,  // "OKAY" with a name's error (0x4711D0) or the server's answer (0x46C0F8)
};

struct CharCreateRowView {
    std::string_view label; // CHAR_CUSTOMIZATIONn_DESC, HAIR_*_STYLE / _COLOR, FACIAL_HAIR_*
    bool shown = true;      // CharacterCustomizationButtonFrame5 hides for "NONE"
};

struct CharCreateView {
    u8 race = 1, cls = 1, gender = 0; // ChrRaces / ChrClasses ids, 0 male
    std::span<const u8> races;        // GetAvailableRaces: the Alliance's four (left), then the Horde's
    std::span<const u8> classes;      // GetClassesForRace, in its order
    std::array<std::string_view, 16> raceNames{}, classNames{}; // by id (ChrRaces, ChrClasses)
    // CharacterCreateCharacterFaction / Race / Class (SetCharacterRace / SetCharacterClass): titles and
    // texts.
    bool alliance = true;
    std::string_view factionName, factionText;
    std::string_view raceText, abilityText; // RACE_INFO_*, the ABILITY_INFO_*n lines
    std::string_view classText;             // CLASS_*
    std::array<CharCreateRowView, 5> rows{};
    CharCreateDialog dialog = CharCreateDialog::None;
    std::string_view dialogText;
    // The safe area without the soft keyboard (empty: the context's): the screen keeps its place while the
    // name is edited and only the name rises above the keyboard.
    ui::Rect layoutArea;
};

struct CharCreateResult {
    i32 race = -1;             // a race button tapped (its id)
    i32 cls = -1;              // a class button tapped
    i32 gender = -1;           // 0 / 1
    i32 row = -1;              // a customization row's arrow (0..4)...
    i32 step = 0;              // ...-1 or +1
    bool randomize = false;    // CharCreateRandomizeButton
    bool randomName = false;   // CharacterCreateRandomName
    bool accept = false;       // CharCreateOkayButton, or Enter in the name box
    bool back = false;         // CharCreateBackButton
    bool dialogButton = false; // GlueDialog's button (Cancel / Okay)
    bool scrollButton = false; // a text's scroll button scrolled it (UChatScrollButton)
    f32 turnDegrees = 0.0f;    // this frame's turn (SetCharacterCreateFacing's degrees)
};

// What the view keeps between frames (the caller owns it).
struct CharCreateUiState {
    u32 expanded = 1;                 // the wide screens' open text frame (0 faction, 1 race, 2 class)
    u8 shownRace = 0, shownClass = 0; // the texts' scroll frames go back to the top when these change
    bool shownAlliance = true;
    std::array<ui::ScrollState, 3> scrolls{}; // the faction's, the race's and the class's texts
};

// The whole screen, modal. The name edits through `text` (12 letters: CharacterCreateNameEdit letters="12").
CharCreateResult characterCreateScreen(DialogContext& c, ui::TextInput& text, FormText<48>& name,
                                       const CharCreateView& v, CharCreateUiState& state);

} // namespace vd
