#pragma once

#include "app/Dialogs.h"
#include "app/HudActionSlot.h"
#include "core/Types.h"
#include "ui/DrawList.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 20 (edit, docs/MOBILE_CONTROLS.md §5.3, §6.9, §9): the touch cluster's windows as pure views (like the other
// *Windows): the template list, the edit mode's panel, the action picker and the drawer. Each draws a view-model that
// app/TouchEditScreens fills and returns what the player did this frame. They go on the side of the screen away from
// the cluster (`area`), as panels: the cluster stays usable beside them.

// --- The template list (§5.3): the templates' button tapped ---

struct TouchTemplateRowView {
    std::string_view name;
    bool active = false;
};
struct TouchTemplateListView {
    std::span<const TouchTemplateRowView> rows;
    i32 menuRow = -1;            // the row whose actions (rename, duplicate, delete) are open
    i32 renaming = -1;           // the row whose name is edited (a text field)
    std::string_view renameText; // its text now
    bool canAdd = true;          // fewer than six templates
    bool canEdit = true;         // not in combat (§9)
    bool canRestore = false;     // the class has a preset for the active template
    bool canDelete = true;       // more than one template
    std::string_view note;       // under the list: why something is grey (in combat)
    ui::Rect anchor;             // the templates' button
    ui::Rect area;               // where the list may go
};
struct TouchTemplateListResult {
    i32 select = -1; // a row tapped: that template from now on
    i32 menu = -1;   // a row's menu button tapped
    i32 rename = -1; // ...its actions
    i32 duplicate = -1;
    i32 remove = -1;
    bool renameField = false; // the name field tapped (the keyboard)
    bool renameDone = false;  // its OK button
    bool edit = false;
    bool add = false;
    bool restore = false;
    bool copyLayout = false;
    bool closed = false; // tapped outside, or its close button
    ui::Rect panel;      // where it was drawn
};
TouchTemplateListResult touchTemplateList(DialogContext& c, const TouchTemplateListView& v);

// --- The edit mode's panel (§9) ---

struct TouchLayerView {
    std::string_view name;
    bool own = false;    // the layer holds actions of its own (else it shows the base)
    bool enabled = true; // TouchTemplate::layerEnabled
};
struct TouchEditPanelView {
    std::string_view templateName;
    bool wide = false; // the screen's profile, the one edited
    std::span<const TouchLayerView> layers;
    u32 layer = 0;             // the one edited (index into layers; 0 the base)
    bool layout = false;       // the layout mode (else the actions mode)
    std::string_view selected; // the selected place's name (empty: none)
    f32 size = 100.0f;         // its size, % of the default
    f32 scale = 100.0f;        // the cluster's, %
    bool tooSmall = false;     // the selected place is under 7 mm (§3.3)
    std::string_view hint;     // what a tap and a drag do now
    ui::Rect area;             // where the panel may go
    // Phase 20 (assists): the template's site (TouchSite) and healer frames, in the Places mode (app/TouchAssists).
    u32 site = 0;
    bool healerFrames = false;
};
struct TouchEditPanelResult {
    i32 layer = -1;           // a layer chosen
    bool toggleLayer = false; // the layer's "use it" box
    i32 mode = -1;            // 1 layout, 0 actions
    f32 size = -1.0f;         // the size slider moved (%)
    f32 scale = -1.0f;        // the scale slider moved (%)
    bool reset = false;
    bool done = false;
    bool cancel = false;
    ui::Rect panel;
    i32 site = -1;             // phase 20 (assists): another site chosen
    bool toggleHealer = false; // ...the healer frames' box tapped
};
TouchEditPanelResult touchEditPanel(DialogContext& c, const TouchEditPanelView& v);

// --- The action picker (§9): a slot tapped in the actions mode ---

enum class TouchPickerTab : u8 { Spells, Items, Macros, Pet, Special, Count };
struct TouchPickerEntry {
    u64 iconHash = 0;
    std::string_view name;
    std::string_view detail; // a rank, a count
    bool marked = false;     // in the group being made; the rank kept
};
struct TouchPickerView {
    std::string_view title;
    TouchPickerTab tab = TouchPickerTab::Spells;
    std::span<const std::string_view> subTabs; // the spell book's pages (Spells)
    u32 subTab = 0;
    std::span<const TouchPickerEntry> entries;
    bool layer = false;                      // a layer's slot: Inherit and Hidden (the base: Empty)
    bool grouping = false;                   // making a group: a tap adds (or takes out) an entry
    std::span<const TouchPickerEntry> group; // the group so far
    bool canRank = false;                    // the slot holds a spell: Fixed rank
    bool ranks = false;     // choosing the rank: `entries` are the ranks (the first: the highest, always)
    std::string_view empty; // shown when there are no entries
    ui::Rect area;
};
struct TouchPickerResult {
    i32 tab = -1;
    i32 subTab = -1;
    i32 entry = -1;     // tapped
    i32 entryHeld = -1; // held: its tooltip
    ui::Rect entryRect;
    i32 groupEntry = -1; // a group's action tapped (taken out)
    bool empty = false;
    bool inherit = false;
    bool hide = false;
    bool group = false; // "New group" / its "Save" while grouping
    bool rank = false;  // "Fixed rank"
    bool closed = false;
    ui::Rect panel;
};
TouchPickerResult touchActionPicker(DialogContext& c, const TouchPickerView& v);

// --- The drawer (§6.9): the six pages of the 1.12 bars ---

constexpr u32 kTouchDrawerSlots = 72; // the server's action slots 0..71: the bars' six pages of twelve
struct TouchDrawerView {
    std::span<const HudActionSlot> slots; // kTouchDrawerSlots
    bool keepOpen = false;
    bool placing = false; // a spell from the book: a tap puts it on the slot
    ui::Rect area;
};
struct TouchDrawerResult {
    i32 used = -1; // a slot tapped
    i32 held = -1; // a slot held still
    ui::Rect heldRect;
    bool keepOpen = false; // its box toggled
    bool closed = false;
    ui::Rect panel;
};
TouchDrawerResult touchDrawer(DialogContext& c, const TouchDrawerView& v);

} // namespace vd
