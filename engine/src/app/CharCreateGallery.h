#pragma once

#include "app/CharCreateWindows.h"
#include "app/Dialogs.h"
#include "core/Types.h"
#include "ui/TextInput.h"

#include <array>
#include <string>

namespace vd {

// Development (phase 18, create): GlueXML's CharacterCreate with sample data as VD_UI_GALLERY pages, drawn by
// the real view (app/CharCreateWindows) over the offline world (the glue scene itself shows with
// VD_CREATE_DEMO=1):
//  - "create": as it opens, a human male warrior named Arthel (taps change the race, gender and class as the
//    screen's rules do; the rest goes to the log);
//  - "create2": a tauren female druid (the Horde's backdrops, horns and hair for the rows, the class text);
//  - "create3": GlueDialog's "Creating character" with Cancel over it;
//  - "create4": the server's answer in an "OKAY" dialog (CHAR_CREATE_NAME_IN_USE).
// Use --screen inner / cover for the two layouts and --fake-keyboard 0.45 (a tap on the name) for the
// keyboard.
class CharCreateGallery {
public:
    enum class Page : u8 { Opening, Tauren, Creating, Error };
    // A page by its VD_UI_GALLERY name; false for another name.
    bool select(const char* name);
    void draw(DialogContext& c, ui::TextInput& text);

private:
    void fill();

    Page m_page = Page::Opening;
    bool m_filled = false;
    CharCreateView m_view;
    CharCreateUiState m_ui;
    FormText<48> m_name;
    std::array<u8, 8> m_classes{};
    std::string m_abilities;
};

} // namespace vd
