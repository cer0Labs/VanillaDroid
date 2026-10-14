#pragma once

#include "app/ContainerLayout.h"
#include "app/ContainerWindows.h"
#include "app/Dialogs.h"
#include "app/DressUpWindows.h"
#include "app/InventoryWindows.h"
#include "app/MicroMenu.h"
#include "app/ModelFrame.h"
#include "app/ModelViews.h"
#include "app/PopupStack.h"
#include "app/PopupWindows.h"
#include "core/Types.h"

#include <array>
#include <vector>

namespace vd {

class ModelFrameGallery;

// Development (phase 15, items): its windows with sample data, for UiGallery's pages containers (the bag bar
// and the ContainerFrames: tap the bag bar's buttons, hold the backpack for all), compare (a ring's tooltip
// with both equipped rings), dressup (DressUpFrame; the model needs a map: --map Azeroth --pos -9050,440),
// auctiondressup (with UiGallery's auction house), masterloot / masterloot2 (the LootFrame and
// GroupLootDropDown in a party / a raid) and lootpopups (CONFIRM_LOOT_ROLL, CONFIRM_LOOT_DISTRIBUTION); also
// the HUD demo's bags (app/PanelDemo). What the player does goes to the log ("Items gallery: ...").
class ItemsGallery {
public:
    enum class Page : u8 {
        Containers,
        Compare,
        DressUp,
        AuctionDressUp,
        MasterLoot,
        MasterLootRaid,
        LootPopups,
        BankBags
    };

    // The sample models (UiGallery's app/ModelFrameGallery).
    void setModelFrames(ModelFrameGallery* frames) { m_modelFrames = frames; }
    void draw(DialogContext& c, Page page);
    // The auction house's dressing room in `place` (UiGallery draws the auction house beside it).
    void drawAuctionDock(DialogContext& c, const ui::Rect& place);

    // The HUD demo's bags: the bag bar's buttons, the frames in the bags' place.
    void bagButton(i32 button, bool shift);
    bool bagsOpen() const { return m_stack.any(); }
    void closeBags() { m_stack.closeAll(); }
    void drawContainers(DialogContext& c);
    void bagBarState(MicroMenuView& v) const;

private:
    void drawPage(DialogContext& c, Page page);
    void sample();
    void containersPage(DialogContext& c);
    void comparePage(DialogContext& c);
    void dressUpPage(DialogContext& c);
    void masterLootPage(DialogContext& c, bool raid);
    void lootPopupsPage(DialogContext& c);
    void bankBagsPage(DialogContext& c);
    TextureHandle model(ModelViewId id, f32 facing);

    ModelFrameGallery* m_modelFrames = nullptr;
    bool m_sampled = false;
    ContainerSizes m_sizes;
    ContainerStack m_stack;
    std::array<std::vector<ItemSlotView>, 6> m_cells; // the backpack, the four bags, the keyring
    std::array<ContainerFrameView, kContainerFrames> m_views{};
    std::array<ui::Rect, kContainerFrames> m_rects{};
    u32 m_shownCount = 0; // the frames on screen last frame (m_views' first ones)
    ItemDrag m_drag;
    MicroMenuState m_micro;
    ModelFrameState m_dressUpFrame, m_auctionFrame;
    bool m_opened = false;
    i32 m_masterGroup = 0;
    u32 m_bankFrames = 3; // the bank page's open bank bags (a bit each)
    bool m_menuOpen = true;
    PopupStack m_popups;
    std::array<f32, PopupStack::kSlots> m_popupHeights{};
    std::array<PopupView, PopupStack::kSlots> m_popupViews{};
    std::array<std::array<char, 200>, PopupStack::kSlots> m_popupTexts{};
    f64 m_timeSum = 0.0, m_timeMax = 0.0; // VD_TRACE_ITEMS
    u32 m_timeFrames = 0;
};

} // namespace vd
