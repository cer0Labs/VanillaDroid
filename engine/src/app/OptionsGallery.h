#pragma once

#include "app/CombatTextScreens.h"
#include "app/Dialogs.h"
#include "app/MacroScreens.h"
#include "app/OptionsScreens.h"
#include "core/Types.h"
#include "ui/TextInput.h"

namespace vd {

class UiResources;

// Phase 15 (options): the gallery's pages of the macro window and the option windows, with sample macros and the
// default options (VD_UI_GALLERY=macro, macropopup, uioptions, uioptions2, soundoptions), drawn by the same screens the
// game uses (app/MacroScreens, app/OptionsScreens) without a session. "combattext": the floating combat text with every
// kind turned on, a sample message every 300 ms, scrolling up, down and in arcs in turn (8 s each).
class OptionsGallery {
public:
    enum class Page : u8 { Macro, MacroPopup, UiOptions, UiOptionsAdvanced, SoundOptions, CombatText };
    void init(const game::GameData* data, UiResources* resources);
    void draw(DialogContext& c, ui::TextInput& text, Page page);

private:
    void drawCombatText(DialogContext& c);

    MacroScreens m_macros;
    OptionsScreens m_options;
    SettingsView m_settings;
    CombatTextScreens m_combatText;
    u32 m_nextTextMs = 0;
    u32 m_textIndex = 0;
    bool m_started = false;
    Page m_page = Page::Macro;
};

} // namespace vd
