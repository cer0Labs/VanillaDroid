#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "ui/Context.h"
#include "ui/DrawList.h"
#include "ui/Widgets.h"

#include <span>
#include <string_view>

namespace vd {

struct ContainerFrameView;
struct ContainerFramesResult;

// The touch windows of items (phase 8): bags, item tooltips with their actions,
// vendor, bank, reading, and the item dialogs (split, buy quantity), plus the HUD's durability figure. Pure views like
// app/Dialogs: each draws a view-model and returns what the player did this frame. Items are picked up by holding
// them (ItemDrag, shared by every window drawn in a frame) and dropped on a slot or a window.

// Which item a cell shows, as the caller needs it to act (the windows never interpret it).
struct ItemRef {
    // Window (phase 8): a cell of another module's window (mail, auction house, trade), numbered by it.
    enum class Kind : u8 { None, Slot, Vendor, Buyback, Window };
    Kind kind = Kind::None;
    u8 bag = 0;    // Slot: the item packets' bag and slot (game::ItemPos)
    u8 slot = 0;
    u32 index = 0; // Vendor: the row; Buyback: 0..11; Window: the window's own number
    bool valid() const { return kind != Kind::None; }
    bool operator==(const ItemRef&) const = default;
};

// An item cell.
struct ItemSlotView {
    ItemRef ref;
    u64 iconHash = 0;  // its icon in textures.vdpk (0: unknown yet)
    u64 emptyArt = 0;  // ui.vdpk sprite of an empty slot (UI-PaperDoll-Slot-*); 0: the plain empty slot
    u32 count = 0;     // stack size, shown when > 1
    u32 quality = 1;
    f32 cooldown = 0.0f;   // 0..1: the share of its use cooldown left
    bool empty = true;
    bool usable = true;    // false: tinted red (class, level, proficiency), as the client does
    bool broken = false;   // durability 0: red
    bool lowDurability =
        false; // retouches (windows): not drawn on the cell (1.12: DurabilityFrame's figure only)
    bool dimmed =
        false; // locked: waiting for the server (a move sent, offered in a trade or a letter), or not for
               // sale; retouches (windows): desaturated, as SetItemButtonDesaturated(locked)
    bool lit = false;      // phase 13 (spells): an item the spell waiting for one suits (the checked highlight)
    bool bagArt = false;   // phase 15 (items): over a ContainerFrame's art (an empty slot shows the bag's)
    u32 tint = 0xFFFFFFFFu; // retouches (windows): the icon's and its frame's vertex colour (a merchant's
                            // sold out grey, a bank bag slot not bought yet in red)
};

// A finger carrying an item (hold a cell to pick it up). Keep it between frames, call track() once per frame before
// the windows, pass it to every window, and finish() after them: a drop no window took is cancelled.
struct ItemDrag {
    bool active = false;
    ItemRef from;
    u64 iconHash = 0;
    u32 quality = 1;
    u32 count = 0;
    i32 touch = -1;
    u64 touchDownMs = 0;
    f32 x = 0.0f, y = 0.0f; // where the finger is (logical pixels)
    bool released = false;  // lifted this frame: the windows look for the slot under it
    bool taken = false;     // a window took the drop

    void begin(const ItemSlotView& cell, const ui::Touch& finger);
    void track(std::span<const ui::Touch> touches);
    void finish(); // after the windows: a released drag ends
    bool droppedIn(const ui::Rect& r) const { return active && released && !taken && r.contains(x, y); }
};

// What a window did this frame.
struct ItemWindowResult {
    ItemRef tapped;      // a cell tapped: show its tooltip...
    ui::Rect tappedRect; // ...beside this
    ItemRef dropFrom;    // a drag dropped on a cell or zone of this window...
    ItemRef dropOn;      // ...this cell (invalid: on the window's zone, see dropZone)
    i32 dropZone = -1;   // the zone it fell on (window-specific: see each window), -1 none
    bool closed = false;
    i32 button = -1;     // a footer / side button (window-specific)
    i32 tab = -1;        // a tab tapped
};

// The item cell (ItemButtonTemplate with the PaperDoll's empty art, durability colors, cooldown shade). Reports taps
// and drops in `result`, starts drags. `r` is the icon.
void itemCell(DialogContext& c, ui::Gui& g, ui::WidgetId id, const ui::Rect& r, const ItemSlotView& cell,
              ItemDrag& drag, ItemWindowResult& result);

// --- Bags ---------------------------------------------------------------------------------------------------

struct BagSectionView {
    std::string_view title; // "Backpack", the bag's name
    u32 first = 0;          // its cells in BagsView::cells
    u32 count = 0;
    bool special = false;   // a quiver, soul bag...: tinted
};
struct BagsView {
    std::span<const ItemSlotView> bagBar; // the backpack, then the 4 bag slots
    std::span<const BagSectionView> sections;
    std::span<const ItemSlotView> cells;
    u32 money = 0;
    u32 freeSlots = 0;
    u32 totalSlots = 0;
};
// Every bag in one scrolling grid inside `area` (no panel): the bag bar on top, money and free slots under it.
// Drops on the grid but not on a cell: zone 0.
void bagsGrid(DialogContext& c, ui::Gui& g, std::string_view name, const ui::Rect& area, const BagsView& v,
              ItemDrag& drag, ItemWindowResult& result);
// The bags window (MenuChoice::Bags): a modal panel with the grid.
ItemWindowResult bagsWindow(DialogContext& c, const BagsView& v, ItemDrag& drag);

// --- Tooltip ------------------------------------------------------------------------------------------------

struct TooltipLineView {
    std::string_view left, right;
    u32 leftColor = 0xFFFFFFFF, rightColor = 0xFFFFFFFF;
    bool wrap = false;
    u32 money = 0;
    bool gapBefore = false;
};
struct TooltipView {
    u64 iconHash = 0;
    std::span<const TooltipLineView> lines;
    std::span<const std::string_view> actions; // buttons under the text ("Use", "Equip", "Sell"...)
    ui::Rect anchor;                            // the cell it belongs to (placed beside it)
    ui::Rect placed; // phase 15 (items): w > 0: its top left goes here instead (beside its comparisons)
};
struct TooltipResult {
    i32 action = -1; // index into actions
    bool closed = false; // tapped outside
    ui::Rect panel;      // phase 15 (items): where it was drawn
};
// An item's tooltip with its action row, over everything (call it after the windows). Taps outside close it.
TooltipResult itemTooltip(DialogContext& c, const TooltipView& v);
// Phase 15 (items): the size itemTooltip gives `v` (x, y 0), to place it with its comparisons first.
ui::Rect itemTooltipSize(const DialogContext& c, const TooltipView& v);

// --- Vendor -------------------------------------------------------------------------------------------------

struct VendorRowView {
    ItemSlotView cell;
    std::string_view name;
    u32 price = 0;    // copper per purchase
    u32 stock = 0;    // limited items: how many are left (shown); 0 with `limited`: sold out
    bool limited = false;
    u32 buyCount = 1; // items per purchase ("x5")
    bool affordable = true;
};
struct VendorView {
    std::string_view npcName;
    bool buyback = false; // the "Buyback" tab
    std::span<const VendorRowView> rows;
    bool canRepair = false;
    u32 repairAllCost = 0;
    u32 junkValue = 0; // grey items in the bags (0: the button is disabled)
    u32 money = 0;
    BagsView bags;
};
// Buttons: 0 "Repair All", 1 "Sell Junk". Tabs: 0 "Buy", 1 "Buyback". Zones: 0 the vendor's list (sell what falls
// there), 1 the bags (buy what falls there).
ItemWindowResult vendorWindow(DialogContext& c, const VendorView& v, ItemDrag& drag);

// --- Bank ---------------------------------------------------------------------------------------------------

struct BankView {
    std::span<const ItemSlotView> bankBags;   // the 6 bank bag slots
    u32 bankBagSlots = 0;                     // bought
    u32 nextSlotPrice = 0;                    // 0: all bought
    std::span<const BagSectionView> sections; // the bank's own 24, then each bank bag
    std::span<const ItemSlotView> cells;
    BagsView bags;
    // Phase 15 (items): the bank bags opened from their buttons, as ContainerFrames under the bank's own slots
    // (app/ContainerWindows; their sections are then not in `sections`).
    const ContainerFrameView* bagFrames = nullptr;
    u32 bagFrameCount = 0;
};
// Button 0: buy the next bag slot. Zones: 0 the bank (store there), 1 the bags. Phase 15 (items): what the bank bags'
// frames did goes to `frames` (their cells' taps and drops to the result, as the other cells').
ItemWindowResult bankWindow(DialogContext& c, const BankView& v, ItemDrag& drag, ContainerFramesResult* frames = nullptr);

// --- Reading, quantities, durability ------------------------------------------------------------------------

struct ReadingView {
    std::string_view title;
    std::string_view text; // the page (enUS, "$B" and "\n" break lines)
    u32 page = 0;
    u32 pageCount = 0;
    bool waiting = false;
};
struct ReadingResult {
    i32 turn = 0; // -1 previous, +1 next
    bool closed = false;
};
ReadingResult readingWindow(DialogContext& c, const ReadingView& v);

// "How many?": split a stack, buy several. `value` is kept by the caller between frames.
struct QuantityView {
    std::string_view title;   // "Split", "Buy"
    std::string_view itemName;
    u32 quality = 1;
    u64 iconHash = 0;
    u32 min = 1, max = 1;
    u32 unitPrice = 0; // > 0: the total is shown
    std::string_view confirm; // the button
};
struct QuantityResult {
    bool confirmed = false;
    bool cancelled = false;
};
QuantityResult quantityDialog(DialogContext& c, const QuantityView& v, u32& value);

// The DurabilityFrame's armor figure (UI-Durability-Icons): 0 none, 1 yellow (low), 2 red (broken) per part (head,
// shoulders, chest, waist, legs, feet, wrists, hands, weapon, shield, ranged). Drawn at (x, y) top-left, `height`
// tall; not drawn when every part is fine. Returns true when tapped.
bool durabilityFigure(DialogContext& c, f32 x, f32 y, f32 height, std::span<const u8> parts, bool offhandWeapon);
// Where the HUD shows it: left of the backpack button (app/Hud's minimap placement), under the player's auras.
ui::Rect durabilityHudRect(const DialogContext& c);
// The HUD's backpack button on the minimap's rim (1.12 has it on the main menu bar), left of it, without the free
// slots. Tapped: open the bags; held: unfold the bag bar (app/MicroMenu, phase 20: the gear that unfolded it is gone).
ui::WidgetState backpackButton(DialogContext& c);
ui::Rect backpackButtonRect(const DialogContext& c);

} // namespace vd
