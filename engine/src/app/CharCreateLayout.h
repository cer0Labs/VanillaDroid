#pragma once

#include "core/Types.h"
#include "ui/DrawList.h"

#include <array>

namespace vd {

// Phase 18 (create): where the frames of GlueXML's CharacterCreate go on a phone. The original lays them out
// in the 1024 x 768 units of a 4:3 screen (CharacterCreate.xml): the configuration frame down the left (the
// WoW logo, the Alliance and Horde banners with the race buttons, the gender and class buttons, the five
// customization rows and Randomize), the faction, race and class texts down the right, the name at the bottom
// centre with the random name under it, the rotate buttons at the bottom left and Back / Accept at the bottom
// right, the character in the middle. The rotate buttons are left out: a finger on the scene turns it. Here the same frames are sized in those units scaled to the phone
// (`unit`: as large as the screen's height allows) with every finger target at least 44 dp (the original's
// 32-unit arrows and 30-unit buttons grow; the icons are 48 units already), over the whole safe area
// (GlueParent's 16:9 bars are no use on a phone):
//  - square screens (the Fold5's inner ~6:5, tablets): the original's arrangement; the WoW logo is left out
//  (its 74
//    units above the column hold the touch rows), the three texts share the right column's height.
//  - wide screens (20:9 phones, the Fold5's 23:9 cover) and any screen too short for the column: the
//  configuration
//    frame is two columns side by side (races, gender and Randomize; classes and the customization rows), and
//    the three texts are an accordion (one open, the others reduced to their icon and title).
// Either way the configuration frame runs the screen's height (a margin from the edges), its content centred.
// The middle stays free for the character: a drag there turns it (CharacterCreateFrame_OnMouseDown). While
// the name is edited the soft keyboard covers the bottom: the name box (and its label) rise above it, the
// rest stays put. Pure arithmetic: no drawing, no state.
struct CharCreateLayoutInput {
    ui::Rect area;          // the safe area without the keyboard (logical pixels)
    f32 keyboardTop = 0.0f; // where the soft keyboard begins while the name is edited (0: no keyboard)
    f32 dp = 1.0f;          // logical pixels per dp
    bool wide = false;  // the wide arrangement asked for (it is also used when the square one does not fit)
    u32 classCount = 6; // GetClassesForRace's buttons (1..8)
    bool facialHair =
        true;         // CharacterCustomizationButtonFrame5 shown (GetFacialHairCustomization() ~= "NONE")
    u32 expanded = 1; // wide: the text frame open (0 faction, 1 race, 2 class)
    // The width of the "Randomize" text at a font size of 1 (GlueButtonSmall: SetWidth(GetTextWidth() + 50)).
    f32 randomizeEm = 5.0f;
};

struct CharCreateLayout {
    f32 unit = 1.0f;     // logical pixels per UI unit of the original
    f32 iconUnit = 1.0f; // the banners' and icons' scale: unit, or more where 48 units are less than 44 dp
    bool wide = false;   // the arrangement used
    // CharacterCreateConfigurationFrame.
    ui::Rect config;                   // the outer border's box (UI-CharacterCreate-OuterBorder, -Background)
    std::array<ui::Rect, 2> banners{}; // UI-CharacterCreate-Banners' halves (the Alliance's, the Horde's)
    ui::Rect allianceLabel, hordeLabel;
    std::array<ui::Rect, 8>
        races{}; // CharacterCreateRaceButton1..8 (1-4 the Alliance's column, 5-8 the Horde's)
    std::array<ui::Rect, 2> genders{}; // CharacterCreateGenderButtonMale, -Female
    std::array<ui::Rect, 8> classes{}; // CharacterCreateClassButton1..8, three to a row
    std::array<ui::Rect, 5> rows{};    // CharacterCustomizationButtonFrame1..5
    std::array<ui::Rect, 5>
        plates{}; // their CharacterCreate-LabelFrame plates (the label is centred in them)
    std::array<ui::Rect, 5> leftArrows{}, rightArrows{};
    ui::Rect randomize; // CharCreateRandomizeButton
    // CharacterCreateCharacterFaction / Race / Class.
    std::array<ui::Rect, 3> info{};
    std::array<bool, 3> infoOpen{}; // its text shows (square: all three; wide: the open one)
    // The bottom.
    ui::Rect nameLabel, name, randomName; // "Name", CharacterCreateNameEdit, CharacterCreateRandomName
    ui::Rect back, accept;                // CharCreateBackButton, CharCreateOkayButton
    ui::Rect scene;                       // the free middle (the character stands there)
};

CharCreateLayout charCreateLayout(const CharCreateLayoutInput& in);

} // namespace vd
