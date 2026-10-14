#pragma once

#include "app/Dialogs.h"
#include "app/InventoryWindows.h"
#include "core/Types.h"

#include <array>
#include <vector>

namespace vd {

// Development (phase 8): the item windows with a level-5 human warrior's sample data, for UiGallery's pages bags,
// tooltip, vendor, bank, reading, quantity and durability (phase 15: character and inspect are app/CharacterGallery's). The windows' own texts come from the texts table
// as in the game (VD_LOCALE=esES shows them in Spanish); the sample data (names) is English. Items can be held and
// dragged; what the player does goes to the log ("UI gallery: ...").
class InventoryGallery {
public:
    enum class Page : u8 { Bags, Tooltip, Vendor, Bank, Reading, Quantity, Durability };

    void draw(DialogContext& c, Page page);

private:
    void bags(BagsView& out);
    void apply(const ItemWindowResult& r, const char* window);

    ItemDrag m_drag;
    std::vector<ItemSlotView> m_bagBar;
    std::vector<ItemSlotView> m_cells;
    std::vector<BagSectionView> m_sections;
    std::vector<ItemSlotView> m_bankBags;
    std::vector<ItemSlotView> m_bankCells;
    std::vector<BagSectionView> m_bankSections;
    std::vector<VendorRowView> m_vendor;
    bool m_buyback = false;
    bool m_tooltip = false;
    ItemRef m_tipRef;
    ui::Rect m_tipAnchor;
    u32 m_page = 0;
    u32 m_quantity = 5;
};

} // namespace vd
