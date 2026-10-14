#pragma once

#include "core/Types.h"

namespace vd {

// Phase 18 (extras): a hardware key the glue screens take (their OnKeyDown in GlueXML: AccountLogin,
// RealmList, CharacterSelect, CharacterCreate, GlueDialog): desktop Escape / Enter / the arrows, Android's
// Back and a physical keyboard. Each screen applies it on its next frame as the button it stands for.
enum class GlueKey : u8 {
    None,
    Escape, // "ESCAPE": AccountLogin_Exit, RealmList_OnCancel, CharacterSelect_Exit, CharacterCreate_Back,
            // GlueDialogButton2 (Button1 alone)
    Enter,  // "ENTER": AccountLogin_Login, RealmList_OnOk, CharacterSelect_EnterWorld, CharacterCreate_Okay,
            // GlueDialogButton1
    Previous, // "UP" / "LEFT": CharacterSelect's previous character (the last after the first)
    Next,     // "DOWN" / "RIGHT": the next one (the first after the last)
};

} // namespace vd
