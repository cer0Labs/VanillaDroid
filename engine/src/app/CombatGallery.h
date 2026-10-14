#pragma once

#include "app/CombatWindows.h"
#include "core/Types.h"

#include <string_view>

namespace vd {

// Development (phase 8): the windows of app/CombatWindows with sample data of a level-10 mage and a hunter's
// pet, for UiGallery (VD_UI_GALLERY=spellbook, trainer, talents, pet, actionedit, itempicker, combatlog,
// actiontooltip; phase 15: skills and reputation are app/CharacterGallery's). What the player does goes to the log
// ("UI gallery: ...").
class CombatGallery {
public:
    // Takes the page named `name`; false when it is not one of these.
    bool select(std::string_view name);
    // The next page; true when it wrapped around to the first ("all" mode moves on then).
    bool next();
    std::string_view pageName() const;
    void draw(DialogContext& c);

private:
    enum class Page : u8 {
        Spellbook,
        Trainer,
        Talents,
        Pet,
        ActionEdit,
        ItemPicker,
        CombatLog,
        ActionTooltip, // parity pass: a held action button's tooltip with its "Edit" action
        Count
    };

    void spellbook(DialogContext& c);
    void trainer(DialogContext& c);
    void talents(DialogContext& c);
    void pet(DialogContext& c);

    Page m_page = Page::Spellbook;
    u32 m_tab = 1;
    i32 m_selected = 2;
    bool m_allRanks = false;
    i32 m_service = 0;
    bool m_filters[3] = {true, true, false};
    u32 m_tree = 1;
    i32 m_talent = 4;
    bool m_onlyMine = true;
    u32 m_reaction = 1;
    bool m_autocast[2] = {true, false};
};

} // namespace vd
