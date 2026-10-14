#pragma once

#include "app/ContainerWindows.h"
#include "app/CharacterWindows.h"
#include "app/Dialogs.h"
#include "app/InventoryWindows.h"
#include "app/TooltipLayout.h"
#include "app/UiSound.h"
#include "core/Text.h"
#include "core/Types.h"
#include "game/GameEvents.h"
#include "game/Inventory.h"
#include "game/ItemActions.h"
#include "game/ItemTooltip.h"

#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace vd {

class CharacterScreens;
class ChatLinkInsert;
class GameSession;
class PopupScreens;
class SpellTargetingScreens;
class UiResources;
namespace game {
class GameData;
class ItemData;
} // namespace game

// The game's side of the item windows (phase 8): fills the views of app/InventoryWindows from the session's
// game::Gameplay / game::Inventory every frame and applies what the player does there (equip, move, split, destroy,
// buy, sell, repair, bank...). GameScreens owns one and calls it from drawGame: the windows in the modal chain, the
// durability figure with the HUD, the tooltip / quantity dialogs / dragged item on top of everything. Main thread;
// the view storage is reserved once (nothing allocates per frame once warmed up).
class InventoryScreens {
public:
    enum class Window : u8 { None, Bags, Character };
    // Parity pass (panels): each window of this module, drawn in the place app/UiPanels gives it.
    enum class Panel : u8 { Bags, Character, Reading, Vendor, Bank };

    void init(const game::GameData* data, const game::ItemData* items, UiResources* resources);

    // The player's windows (the micro menu's character button, the bag bar). Parity pass: the bags and the character
    // window may be open together (the bags beside the character sheet).
    void open(Window window);
    void close(Window window);
    bool isOpen(Window window) const {
        return window == Window::Bags ? m_containers.any() : window == Window::Character && m_characterOpen;
    }
    // Whether a window the server opened is up: a vendor, the bank, a book.
    bool serverPanelOpen(const GameSession& session, Panel panel) const;
    // One of the windows, in the panel place of `c` (c.panel): the tooltip, the questions and the carried item are
    // drawn by drawOverlay after every window.
    void drawPanel(GameSession& session, DialogContext& c, u32 nowMs, Panel panel);
    // Phase 15 (character): CharacterFrame's tabs in their order (CharacterFrameTab1..5). The paper doll is ours; a
    // tab shows when it has a drawer and its check (null: always) says so, as the Pet tab only with a pet (HasPetUI).
    enum class CharacterTab : u8 { Character, Pet, Reputation, Skills, Honor, Count };
    // Which shows, and switching to one (ToggleCharacter; Character: the paper doll).
    u32 characterTab() const { return m_characterTab; }
    void setCharacterTab(u32 tab);
    // Not modal, with the HUD: the durability figure under the minimap and the backpack button on the minimap's rim
    // (`backpackButton`: false hides it).
    void drawHud(GameSession& session, DialogContext& c, u32 nowMs, bool backpackButton = true);
    // Parity pass (micro menu): the bag bar's icons (the backpack, then the four bag slots; 0: empty), whether the
    // keyring holds a key, and the backpack's free slots.
    void bagBar(GameSession& session, std::array<u64, 5>& icons, bool& keyring, u32& freeSlots) const;
    // What drawHud's buttons asked for this frame (read once): the backpack button (the bags; held: the micro menu's
    // bag bar), the durability figure (the character window).
    bool takeBackpackTap() { return std::exchange(m_backpackTapped, false); }
    bool takeBackpackHold() { return std::exchange(m_backpackHeld, false); }
    bool takeDurabilityTap() { return std::exchange(m_durabilityTapped, false); }
    // Last in the frame: the item tooltip, the quantity / destroy dialogs and the item under the finger.
    void drawOverlay(GameSession& session, DialogContext& c, u32 nowMs);
    // The message an item event shows (InventoryError, BuyFailed, SellFailed, BankSlotFailed, BankSlotBought,
    // ReadItemFailed, DurabilityLoss); false when it shows none.
    bool eventMessage(GameSession& session, const game::GameEvent& e, char* out, usize size, u32& color) const;
    // The tooltip of an item template (loot, quest rewards...) beside `anchor`, without actions; drawn by
    // drawOverlay until tapped away. Phase 13: `instance` (copied) adds what that very item has: its enchantment
    // and random suffix (an inspected player's equipment).
    void showItemTooltip(u32 itemEntry, const ui::Rect& anchor, const game::ItemInstance* instance = nullptr);
    // Phase 13 (chat links): while the chat's edit box is open (c.chatLink), `item` goes into it as the client's
    // link (GetContainerItemLink & co.: its name with its suffix, its quality's colour, its permanent enchantment
    // and random property). False when the box is closed, the name is not known yet or it does not fit. A tap on
    // a cell of the bags, the bank, the character sheet or a vendor does this instead of its tooltip then (the
    // desktop's Shift + click), and so does any cell of the bags inside another window.
    bool insertItemLink(GameSession& session, const DialogContext& c, const game::ItemInstance& item);
    // GetInventorySlotInfo's empty-slot art for an equipment slot (0..18, 19 the ammo).
    static u64 equipmentSlotArt(u32 slot);
    const game::ItemData* itemData() const { return m_items; } // phase 13: the inspect frame's names
    // The character window's tabs after the paper doll: `draw` fills the frame the window drew (retouches: its page's
    // 1.12 art) when its tab is selected, and returns true to close the window. The label is a key of the texts table
    // (core/Text: "SKILLS", "REPUTATION"...), looked up when drawn.
    using TabDrawer = bool (*)(void* context, GameSession& session, DialogContext& c, const CharacterPageFrame& frame);
    using TabShown = bool (*)(void* context, GameSession& session);
    void registerCharacterTab(CharacterTab tab, text::Key label, TabDrawer draw, void* context,
                              TabShown shown = nullptr);
    // Phase 15 (character): the paper doll's model, numbers and tooltips (app/CharacterScreens; null: none).
    void setCharacterSheet(CharacterScreens* sheet) { m_sheet = sheet; }
    void closeAll();
    // Phase 8: another module's window with item cells (the mailbox, the auction house, trade) drawn instead of
    // ours. hostWindow(), once a frame before drawing it, lends it the drag every item window shares (the carried
    // item, the tooltip and the questions stay alive: drawOverlay draws them); bags() fills its bags pane as the
    // bags window's; openItemTooltip() for a tap on one of those cells (the actions of game/ItemActions:
    // "Adjuntar", "Subastar", "Comerciar"...).
    ItemDrag& hostWindow(const DialogContext& c);
    // Parity pass (audio): where the carried item's pick-up and put-down sounds go.
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }
    // Parity (popups): where DELETE_ITEM and the soulbinding questions are asked (null: destroy asks in a
    // dialog of its own, nothing is asked before binding).
    void setPopups(PopupScreens* popups) { m_popups = popups; }
    // Phase 13 (spells): while a spell waits for an item, the cells light up or dim by what it suits and a tap
    // casts it on the item (app/SpellTargetingScreens; null: never).
    void setTargeting(SpellTargetingScreens* targeting) { m_targeting = targeting; }
    void bags(GameSession& session, u32 nowMs, BagsView& out) { bagsView(session, nowMs, out); }
    void openItemTooltip(const ItemRef& ref, const ui::Rect& anchor) { openTooltip(ref, anchor); }

    // --- Phase 15 (items) ---
    // The bags as ContainerFrames (app/InventoryContainers.cpp): the open frames (isOpen(Window::Bags): any of them)
    // and what GetContainerNumSlots answers now (the keyring's size by the player's level).
    const ContainerStack& containers() const { return m_containers; }
    // The frame of container `id` on screen (open, and not one of the oldest kept out of sight for lack of room).
    bool containerShown(i32 id) const;
    ContainerSizes containerSizes(GameSession& session) const;
    // The bag bar's buttons (MainMenuBarBagButtons): `button` 0 the backpack (ToggleBackpack), 1..4 the bag slots
    // (ToggleBag), 5 the keyring (ToggleKeyRing); `shift`: their Shift + click (OpenAllBags; touch: a long press).
    void bagButton(GameSession& session, i32 button, bool shift);
    // OpenAllBags (the B key; the minimap rim's backpack button).
    void openAllBags(GameSession& session, bool force = false);
    // Once a frame in the world, before the panels: BAG_OPEN (containerOpened) and BAG_CLOSED (a bag taken off or
    // swapped closes its frame). True when it opened a frame (the panels must make room for the bags).
    bool updateContainers(GameSession& session, u32 nowMs);
    // SMSG_OPEN_CONTAINER: the bag item `guid` (the player's own: the backpack) opens once it is in its slot.
    void containerOpened(u64 guid, u32 nowMs);
    // After the windows: a carried item dropped on a button of the bag bar (`rects`: MicroMenuResult::bagRects)
    // goes there (BackpackButton / BagSlotButton OnReceiveDrag: PutItemInBackpack, PutItemInBag; a bag goes into
    // the slot; KeyRingButton: PutKeyInKeyRing).
    void dropOnBagBar(GameSession& session, const DialogContext& c, const std::array<ui::Rect, 6>& rects);
    // Phase 16 (pullouts): after the windows, a carried item let go this frame where no window took it: true with the
    // finger's point, for the frames that take items on their units (DropItemOnUnit).
    bool releasedItemAt(const DialogContext& c, f32& x, f32& y);
    // DropItemOnUnit (WoW.exe 0x48D960) with that item, on another player: into the first free slot of the trade window
    // when trading with it, else the trade with it starts (the item stays where it was; InitiateTrade's problem, if
    // any, is returned for the UIErrorsFrame). Takes the drop.
    std::string_view dropOnUnit(GameSession& session, u64 unit);
    // Ours: the comparison tooltips (app/InventoryCompare.cpp) show with every item tooltip (true), or when its
    // "Compare" button is tapped (false: the button shows only with something worn to compare it with).
    void setAlwaysCompare(bool always) { m_alwaysCompare = always; }

private:
    enum class Ask : u8 { None, Split, Buy, Destroy };
    struct CharacterTabEntry {
        text::Key label = "";
        TabDrawer draw = nullptr;
        void* context = nullptr;
        TabShown shown = nullptr;
    };

    // Views.
    ItemSlotView slotView(GameSession& session, const game::ItemInstance* item, game::ItemPos pos, u32 nowMs);
    void bagsView(GameSession& session, u32 nowMs, BagsView& out);
    void bankView(GameSession& session, u32 nowMs, BankView& out);
    void vendorRows(GameSession& session, u32 nowMs, bool buyback);
    void characterView(GameSession& session, u32 nowMs, PaperDollView& out);
    std::string_view arena(const char* format, ...); // numbers only: texts go through arenaText
    template <typename... Args> std::string_view arenaText(text::Key key, const Args&... args) {
        if (m_arenaUsed + 1 >= m_arena.size()) {
            return {};
        }
        const std::string_view written = text::format(
            std::span<char>(m_arena.data() + m_arenaUsed, m_arena.size() - m_arenaUsed), key, args...);
        m_arenaUsed += static_cast<u32>(written.size()) + 1;
        return written;
    }
    bool usable(GameSession& session, const net::ItemQueryInfo& item) const;
    // Results.
    static game::ItemPlace place(const ItemRef& ref);
    // A window's taps (tooltips) and drops (game::dropItem); `zones`: what its zone numbers are.
    void apply(GameSession& session, const ItemWindowResult& r, std::span<const game::ItemPlace::Kind> zones,
               const ui::Rect& anchor);
    void openTooltip(const ItemRef& ref, const ui::Rect& anchor);
    void tooltip(GameSession& session, DialogContext& c, u32 nowMs);
    void act(GameSession& session, game::ItemAction action);
    void quantity(GameSession& session, DialogContext& c);
    const game::ItemInstance* itemAt(GameSession& session, const ItemRef& ref) const;
    u32 entryOf(GameSession& session, const ItemRef& ref) const;

    const game::GameData* m_data = nullptr;
    const game::ItemData* m_items = nullptr;
    UiResources* m_resources = nullptr;
    bool m_trace = false;
    bool m_backpackTapped = false;
    bool m_backpackHeld = false;
    bool m_durabilityTapped = false;
    bool m_characterOpen = false;
    u32 m_characterTab = 0;
    bool m_vendorBuyback = false;
    u64 m_vendorShown = 0; // the vendor whose window is open (a new one starts on "Buy")
    ItemDrag m_drag;
    UiSoundSink m_uiSound;
    PopupScreens* m_popups = nullptr; // parity (popups)
    SpellTargetingScreens* m_targeting = nullptr; // phase 13 (spells)
    u32 m_dragEntry = 0;       // the carried item, for its sounds
    bool m_dragSounded = false; // its pick-up sound played
    u32 m_nowMs = 0;
    bool m_drawn = false; // drawWindow ran this frame

    // The tooltip: what it shows, and its lines built once per change.
    bool m_tipOpen = false;
    ItemRef m_tipRef;
    u32 m_tipEntry = 0; // a template only (showItemTooltip)
    game::ItemInstance m_tipInstance; // phase 13: ...and the very item it shows, when given
    bool m_tipHasInstance = false;
    ChatLinkInsert* m_chatLink = nullptr; // phase 13: the windows' DialogContext's, this frame
    ItemRef m_pendingLink;                // phase 13: a cell tapped with the chat open (linked in drawOverlay)
    ui::Rect m_tipAnchor;
    u64 m_tipKey = 0;
    game::ItemTooltip m_tooltip;
    std::vector<TooltipLineView> m_tipLines;
    std::array<std::string_view, 8> m_actionLabels{};
    std::array<game::ItemAction, 8> m_actions{};
    u32 m_actionCount = 0;

    // A question in progress: how many to split / buy, or the confirmation to destroy.
    Ask m_ask = Ask::None;
    ItemRef m_askRef;
    u32 m_askValue = 1;
    std::array<char, 160> m_askText{};

    // View storage (reserved in init).
    std::vector<ItemSlotView> m_bagBar;
    std::vector<ItemSlotView> m_cells;
    std::vector<BagSectionView> m_sections;
    std::vector<ItemSlotView> m_bankBags;
    std::vector<ItemSlotView> m_bankCells;
    std::vector<BagSectionView> m_bankSections;
    std::vector<VendorRowView> m_vendorRows;
    std::vector<ItemSlotView> m_equipment;
    std::array<CharacterTabEntry, static_cast<usize>(CharacterTab::Count)> m_tabs{}; // phase 15: by CharacterTab
    std::array<std::string_view, static_cast<usize>(CharacterTab::Count)> m_tabLabels{};
    std::array<u32, static_cast<usize>(CharacterTab::Count)> m_tabIds{}; // the shown tabs' CharacterTab
    u32 m_tabCount = 0;
    CharacterScreens* m_sheet = nullptr; // phase 15 (character)
    std::array<char, 4096> m_arena{};
    u32 m_arenaUsed = 0;

    // Phase 15 (items): ContainerFrames (app/InventoryContainers.cpp), the comparison tooltips
    // (app/InventoryCompare.cpp) and the tooltips' Compare.
    void drawContainers(GameSession& session, DialogContext& c, u32 nowMs); // Panel::Bags
    // The comparison tooltips of the item the tooltip shows (ShoppingTooltip1 / 2): rebuilt with its lines.
    void buildCompare(GameSession& session, const net::ItemQueryInfo& info, const game::ItemInstance* item,
                      const game::TooltipContext& context);
    void drawCompare(DialogContext& c, const TooltipPlacement& placement, u32 count);
    ContainerStack m_containers;
    std::array<u64, 4> m_frameBags{}; // the bag each open bag frame shows (BAG_CLOSED when it changes)
    u64 m_openGuid = 0;               // SMSG_OPEN_CONTAINER's bag, until it shows in a slot
    u32 m_containerMask = 0;          // the frames open last frame (their open / close sounds)
    u32 m_containersShown = 0;        // the frames on screen last frame (a bit per container; no room: not all)
    u32 m_bankFrames = 0;             // the bank bags whose frames are open (a bit per bank bag slot)
    u64 m_bankShown = 0;              // the banker of this visit
    std::vector<ContainerFrameView> m_bankFrameViews;
    u32 m_openMs = 0;
    std::vector<ContainerFrameView> m_containerViews;
    std::vector<ItemSlotView> m_containerCells;
    std::array<f32, kContainerFrames> m_containerHeights{};
    std::array<ui::Rect, kContainerFrames> m_containerRects{};
    std::array<game::ItemTooltip, 2> m_compareTips;
    std::array<std::vector<TooltipLineView>, 2> m_compareLines;
    u32 m_compareCount = 0;
    bool m_alwaysCompare = false;
    bool m_compareOpen = false; // the tooltip's "Compare" was tapped
    i32 m_compareAction = -1;   // the tooltip's button that shows the comparisons (-1: none)
};

} // namespace vd
