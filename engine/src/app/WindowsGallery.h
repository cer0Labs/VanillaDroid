#pragma once

#include "app/CombatWindows.h"
#include "app/Dialogs.h"
#include "app/ExchangeWindows.h"
#include "app/InventoryWindows.h"
#include "app/PopupWindows.h"
#include "core/Types.h"

#include <array>

namespace vd {

// Development (retouches, windows): the states of the windows' 1.12 art the other pages do not show, as
// VD_UI_GALLERY pages:
//  - "talents2": a tree whose lines take every shape TalentFrame.lua draws (down, through an empty cell,
//    across both ways, across then down with its corner, a tee where two lines part), lit and grey, full and
//    empty ranks.
//  - "mailinvoice" / "mailinvoice2": an auction house invoice open in the mailbox, the seller's and the
//    buyer's (OpenMailInvoiceFrame's money frames).
//  - "codpopup": COD_CONFIRMATION with its money frame and COD_ALERT, as StaticPopups.
//  - "slots": item buttons as they can look: an icon, the question mark while one loads, locked
//    (desaturated) with and without its icon, a merchant's sold out and unusable tints, a broken item; money
//    frames white and red.
// Taps go to the log.
class WindowsGallery {
public:
    enum class Page : u8 { Talents2, MailInvoice, MailInvoice2, CodPopup, Slots };
    bool select(const char* name);
    void draw(DialogContext& c, u64 nowMs);

private:
    void drawTalents(DialogContext& c);
    void drawInvoice(DialogContext& c, bool buyer);
    void drawPopups(DialogContext& c);
    void drawSlots(DialogContext& c);

    Page m_page = Page::Talents2;
    i32 m_talent = -1;
    ItemDrag m_drag;
    std::array<PopupView, 2> m_popups{};
    std::array<f32, 2> m_popupHeights{};
};

} // namespace vd
