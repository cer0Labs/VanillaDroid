#pragma once

#include "app/Dialogs.h"
#include "app/TouchBarView.h"
#include "app/TouchEditWindows.h"
#include "core/Types.h"
#include "game/SpellBookTabs.h"
#include "game/Spells.h"
#include "game/TouchLayout.h"
#include "ui/DrawList.h"
#include "ui/TextInput.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd {

class CombatScreens;
class GameSession;
class TouchBarScreens;
class UiResources;
struct HudFrame;

// Phase 20 (edit, docs/MOBILE_CONTROLS.md §5.3, §6.9, §9): the touch cluster's windows: the list of templates (the
// templates' button), the edit mode (move and resize places, put actions in the slots of each layer, groups, ranks),
// the drawer (the 1.12 bars), a held button's tooltip and the spell book's "Place on bar" onto a cluster slot.
// app/TouchBarScreens owns it and calls it at fixed points of the frame; it changes the layout through TouchBarScreens
// (layout(), changed()) with game/TouchLayoutEdit; the views are app/TouchEditWindows. Main thread; the picker's lists
// are rebuilt when what they show changes (a tab, the known spells, the bags), not every frame.
// Development: VD_TOUCH_EDIT_DEMO=list|rename|delete|edit[:<layer>]|layout|picker[:<layer>]|group|ranks|drawer|placing
// (with VD_TOUCH_DEMO=rogue|warrior) opens them offline on a sample layout of two templates.
class TouchEditScreens {
public:
    void init(UiResources* resources);
    // Before draw(): the platform's keyboard (renaming) and where the cluster is (the windows go beside it).
    void setFrame(ui::TextInput* input, const ui::Rect& cluster) {
        m_input = input;
        m_cluster = cluster;
    }
    // The templates' button tapped (§5.3: its list, again: closed), the drawer's (§6.9).
    void openTemplates();
    void toggleDrawer();
    // A cluster button held still (TouchBarOutput::held): its tooltip, with "Edit" (§9).
    void showTooltip(i32 place, const ui::Rect& anchor);
    bool editing() const { return m_editing; }
    // A window of this module is up that takes the screen (the edit mode, the template list).
    bool wantsModal() const { return m_editing || m_listOpen; }
    // The view's edit fields (TouchBarView::editing, editSelected, editLayout...).
    void prepareView(TouchBarView& view) const;
    // After the bar filled the view: the edit mode's (the layer edited, every place shown), and the slots lit while a
    // spell from the book waits for one.
    void prepareHud(GameSession& session, TouchBarScreens& bar, CombatScreens& combat, TouchBarView& view, u32 nowMs);
    // After the HUD: what the edit mode, the placing and the drawer did. True when it took the frame (the bar does not
    // use its buttons then); `aim`: the unit a drawer's spell was aimed at.
    bool actOnHud(GameSession& session, TouchBarScreens& bar, CombatScreens& combat, const TouchBarOutput& out,
                  u32 nowMs, u64& aim);
    // Over the HUD: the template list, the edit mode's panel and picker, the drawer, the tooltip.
    void draw(GameSession& session, TouchBarScreens& bar, CombatScreens& combat, DialogContext& c, u32 nowMs);
    // Escape (and Android's Back) closes what is open, the top one first: true when something was.
    bool escape();

    // Without a world (VD_TOUCH_EDIT_DEMO): the sample layout's cluster in `view` (true when the switch is on), what
    // the HUD did with it, the windows.
    bool prepareOffline(TouchBarScreens& bar, HudFrame& hud, TouchBarView& view, u32 nowMs);
    void actOffline(TouchBarScreens& bar, const TouchBarOutput& out, u32 nowMs);
    void drawOffline(TouchBarScreens& bar, DialogContext& c, u32 nowMs);

private:
    enum class Ask : u8 { None, Delete, Restore, CopyLayout };
    // What a picker entry puts in the slot.
    struct Entry {
        game::TouchAction action;
        u64 iconHash = 0;
    };

    // The frame's world (null offline).
    struct World {
        GameSession* session = nullptr;
        CombatScreens* combat = nullptr;
    };
    void openEdit(TouchBarScreens& bar, World world, i32 place, bool picker);
    void openPicker(TouchBarScreens& bar, World world, u32 slot);
    void closeEdit(TouchBarScreens& bar, bool keep);
    void editView(TouchBarScreens& bar, World world, TouchBarView& view, u32 nowMs);
    bool applyEdit(TouchBarScreens& bar, World world, const TouchBarOutput& out);
    void drawWindows(TouchBarScreens& bar, World world, DialogContext& c, u32 nowMs);
    void drawList(TouchBarScreens& bar, World world, DialogContext& c, u32 nowMs);
    void drawPanel(TouchBarScreens& bar, World world, DialogContext& c);
    void drawPicker(TouchBarScreens& bar, World world, DialogContext& c, u32 nowMs);
    void drawDrawer(TouchBarScreens& bar, World world, DialogContext& c, u32 nowMs);
    void drawAsk(TouchBarScreens& bar, World world, DialogContext& c);
    void drawOfflineTooltip(TouchBarScreens& bar, DialogContext& c);
    // The picker's entries for its tab (rebuilt when the tab or what it lists changed).
    void buildEntries(TouchBarScreens& bar, World world);
    void buildRanks(TouchBarScreens& bar, World world);
    // The known spells, class and race (the player's, or the demo's).
    std::span<const u32> knownSpells(World world) const;
    u32 playerClass(World world) const;
    u32 playerRace(World world) const;
    bool inCombat(World world) const;
    // The layers that make sense for the class (and any with actions of its own).
    void listLayers(TouchBarScreens& bar, World world);
    std::string_view layerName(TouchBarScreens& bar, World world, game::TouchContext context) const;
    std::string_view placeName(game::TouchPlace place);
    // A touch action's icon without the world (the offline demo, the picker's group strip).
    u64 iconOf(TouchBarScreens& bar, World world, const game::TouchAction& action) const;
    game::ActionButton buttonOf(TouchBarScreens& bar, World world, const game::TouchAction& action, u32 nowMs) const;
    // The side of the screen away from the cluster, at most `maxWidthDp` wide (against the screen's far edge).
    ui::Rect sideArea(const DialogContext& c, f32 maxWidthDp) const;
    void stopRenaming();
    void closeAll(); // every window, without changing the layout (another character, the classic bars)
    bool canRestore(TouchBarScreens& bar, World world);

    UiResources* m_resources = nullptr;
    ui::TextInput* m_input = nullptr;
    ui::Rect m_cluster;
    bool m_mirrored = false;

    // The template list.
    bool m_listOpen = false;
    i32 m_menuRow = -1;
    i32 m_renaming = -1;
    FormText<game::kTouchNameBytes - 1> m_renameText;
    std::array<TouchTemplateRowView, game::kMaxTouchTemplates> m_rows{};
    bool m_restorable = false;
    bool m_restoreChecked = false;
    Ask m_ask = Ask::None;
    i32 m_askIndex = -1;
    game::TouchLayout m_scratch; // the class presets, for "Restore"

    // The edit mode.
    bool m_editing = false;
    bool m_closeWanted = false; // Escape: closed (keeping what was done) at the next act
    bool m_layoutMode = false;
    i32 m_selected = -1;
    game::TouchContext m_layer = game::TouchContext::Base;
    game::TouchTemplate m_backup; // Cancel: the template as it was
    std::array<game::TouchContext, game::kTouchContexts> m_layers{};
    std::array<TouchLayerView, game::kTouchContexts> m_layerViews{};
    u32 m_layerCount = 0;
    bool m_pinching = false;
    f32 m_pinchStart = 100.0f;
    ui::Rect m_panelRect; // the panel or the picker drawn last (places are not dropped under it)
    std::array<char, 64> m_placeName{};

    // The picker.
    bool m_picking = false;
    u32 m_pickSlot = 0;
    TouchPickerTab m_tab = TouchPickerTab::Spells;
    u32 m_subTab = 0;
    bool m_grouping = false;
    bool m_ranks = false;
    std::array<game::TouchAction, game::TouchGroup::kMaxActions> m_group{};
    u32 m_groupCount = 0;
    std::array<TouchPickerEntry, game::TouchGroup::kMaxActions> m_groupViews{};
    std::vector<Entry> m_entries;
    std::vector<TouchPickerEntry> m_entryViews;
    std::vector<std::string_view> m_subTabNames;
    u64 m_entriesKey = 0; // what the entries were built for
    game::SpellBookTabs m_book;
    game::SpellBookTabs m_allRanks;
    std::array<char, 96> m_title{};
    std::vector<std::array<char, 32>> m_names; // names made here ("Pet Bar 3", "Rank 2")

    // The drawer.
    bool m_drawerOpen = false;
    bool m_drawerKeepOpen = false;
    std::array<HudActionSlot, kTouchDrawerSlots> m_drawerSlots{};
    u64 m_drawerAim = 0;

    // A held button's tooltip, the placing from the book.
    i32 m_tipPlace = -1;
    ui::Rect m_tipAnchor;
    bool m_tipPending = false;
    bool m_tipEditAction = false; // the tooltip's action opens the edit mode (else: the rank picker, edit mode)
    i32 m_tipEditPlace = -1;
    bool m_placingShown = false;
    u64 m_player = 0; // the character the windows were opened for (another one closes them)

    // VD_TOUCH_EDIT_DEMO.
    std::array<char, 32> m_demo{};
    bool m_demoReady = false;
    bool m_demoWarrior = false;
    std::vector<u32> m_demoKnown;
    bool m_demoCombat = false; // VD_TOUCH_EDIT_DEMO=combat: the list as in combat
    i32 m_demoTip = -1;        // the offline tooltip's place
    std::string m_demoTipText;
};

} // namespace vd
