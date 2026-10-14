#pragma once

#include "core/Types.h"
#include "game/Appearance.h"

namespace vd::net {
struct CharacterInfo;
}

namespace vd {

// Phase 18: the 3D scene behind the glue screens (GlueXML). AccountLogin is a ModelFFX of
// Interface\Glues\Models\UI_MainMenu (the Dark Portal); CharacterSelect and CharacterCreate show the background of a
// race (GlueParent.lua SetBackgroundModel: UI_<race>, gnomes in the dwarves', trolls in the orcs'), each through the
// model's camera 0 with GlueParent's fog (CharModelFogInfo), and the character standing in it. The screens say what
// they want each frame (GlueSceneRequest, filled by app/GameScreens' login and creation screens); Game draws it
// instead of the world.
enum class GlueBackground : u8 {
    None,     // no glue scene: the world (or nothing)
    MainMenu, // AccountLogin: UI_MainMenu
    Human,    // UI_Human
    Orc,      // UI_Orc (also trolls)
    Dwarf,    // UI_Dwarf (also gnomes)
    NightElf, // UI_NightElf
    Scourge,  // UI_Scourge
    Tauren,   // UI_Tauren
    Count
};

// SetBackgroundModel's choice for a ChrRaces id (0 or unknown: Orc, as its "not race" case).
GlueBackground glueBackgroundOfRace(u8 race);

// What the glue screens want shown this frame.
struct GlueSceneRequest {
    GlueBackground background = GlueBackground::None;
    // A character stands in the scene: CharacterSelect's selected one, or the one CharacterCreate is making.
    bool hasCharacter = false;
    game::CharacterLook character;
    // Radians about the vertical added to where the scene faces the character (the rotate buttons, a drag);
    // positive turns it counterclockwise seen from above (SetFacing).
    f32 characterFacing = 0.0f;
    // 0 black .. 1 the scene as lit (GlueParent's fades: LOGIN_FADE_IN, CHARACTER_SELECT_FADE_IN).
    f32 fade = 1.0f;
};

// The look of a character of the list (SMSG_CHAR_ENUM): its appearance and the display of each worn item.
game::CharacterLook lookOfCharacter(const net::CharacterInfo& character);

} // namespace vd
