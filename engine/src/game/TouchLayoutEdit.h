#pragma once

#include "core/Types.h"
#include "game/TouchLayout.h"

#include <array>
#include <span>
#include <string_view>

namespace vd::game {

// Phase 20 (edit, docs/MOBILE_CONTROLS.md §5.3, §9): what the template list and the edit mode change in a touch layout.
// Plain functions over the data model (app/TouchEditScreens calls them; tests/app/test_touch_edit.cpp). What the
// player puts in a slot is his: kTouchManual, the class presets never change it again. No allocation.

constexpr f32 kTouchGridMm = 1.0f;          // §9: places move on a 1 mm grid
constexpr f32 kTouchGapMm = 0.5f;           // ...and never closer than this to another
constexpr f32 kTouchMinSizePercent = 70.0f; // §3.3: an element between 70 and 150 % of its default size
constexpr f32 kTouchMaxSizePercent = 150.0f;
constexpr f32 kTouchMinScale = 0.7f; // the whole cluster between 70 and 130 %
constexpr f32 kTouchMaxScale = 1.3f;
constexpr f32 kTouchTooSmallMm = 7.0f; // §3.3: smaller than this is too small for a finger (a warning)

// The screen's safe area in a geometry's millimetres (Main's center at 0, 0; x toward the screen's middle as the
// geometry's, y up): what the HUD lays the cluster in. Empty (all 0): not checked.
struct TouchArea {
    f32 left = 0.0f, right = 0.0f, bottom = 0.0f, top = 0.0f;
    bool empty() const { return left >= right || bottom >= top; }
};

// --- Places (§9 "Disposición") ---

// Moves `place` so its center is near (x, y) mm: snapped to the grid, its circle inside `area`, not overlapping
// another element (the nearest free point within a few mm of it, else it stays). Main is the cluster's anchor: moving
// it moves the whole cluster (mainRight / mainBottom), kept inside `area`. Returns true when something moved.
bool moveTouchPlace(TouchGeometry& g, TouchPlace place, f32 x, f32 y, const TouchArea& area);
// A place's size as a share of its default one for `profile` (100: the reference picture's).
f32 touchPlaceSizePercent(const TouchGeometry& g, TouchProfile profile, TouchPlace place);
// Sets it (clamped to 70..150 %), never growing over a neighbour. Returns the percent it got.
f32 resizeTouchPlace(TouchGeometry& g, TouchProfile profile, TouchPlace place, f32 percent);
// The whole cluster's scale, clamped to 70..130 %.
void setTouchScale(TouchGeometry& g, f32 scale);
// The default places of `profile` (§9 "Restablecer").
void resetTouchGeometry(TouchGeometry& g, TouchProfile profile);
// §5.3 "Copiar esta disposición a todas mis plantillas": both profiles' places of template `from` to the others.
void copyTouchGeometryToAll(TouchLayout& layout, u32 from);
// Two elements overlap (circles closer than kTouchGapMm; places never shown do not count).
bool touchPlacesOverlap(const TouchGeometry& g, TouchPlace a, TouchPlace b);

// --- Actions (§9 "Acciones") ---

// `action` into slot `slot` (< kTouchSlots) of `layer`, by hand.
void putTouchAction(TouchTemplate& t, TouchContext layer, u32 slot, TouchAction action);
// The base: nothing there, by hand (kept empty: the presets leave it). A layer: nothing there either (not the base's).
void hideTouchSlot(TouchTemplate& t, TouchContext layer, u32 slot);
// A layer's slot shows the base's again (§5.2: the layer holds only what changes). Nothing for the base.
void inheritTouchSlot(TouchTemplate& t, TouchContext layer, u32 slot);
// A slot dragged onto another (§9): what each shows in `layer` is swapped. In a layer, a slot that ends up showing the
// base's own action inherits it again instead of keeping a copy.
void swapTouchSlots(TouchTemplate& t, TouchContext layer, u32 a, u32 b);
// What `layer`'s slot holds itself (not inherited): the layer's own entry, `inherited` set when it shows the base's.
TouchAction touchSlotShown(const TouchTemplate& t, TouchContext layer, u32 slot, bool& inherited);

// --- Groups (§6.6, §9 "Grupo nuevo") ---

// The group a slot holds itself in `layer` (-1: none).
i32 touchSlotGroup(const TouchTemplate& t, TouchContext layer, u32 slot);
// Some slot of some layer or the class bar uses group `index`.
bool touchGroupUsed(const TouchTemplate& t, u32 index);
// A group index free for a new group: never used, or a hand-made one nothing uses any more (-1: all sixteen taken).
i32 freeTouchGroup(const TouchTemplate& t);
// The slot gets a group of `actions` (up to eight; empty ones, hidden ones and groups are skipped), made by hand: the
// slot's own group is changed in place (an automatic one becomes the player's), else a free one is taken. The first
// action is the current. Returns the group's index (-1: no action to make it of, or no free group).
i32 putTouchGroup(TouchTemplate& t, TouchContext layer, u32 slot, std::span<const TouchAction> actions);

// --- Templates (§5.3) ---

// Appends a template (at most kMaxTouchTemplates). Returns its index, -1 when full.
i32 addTouchTemplate(TouchLayout& layout, const TouchTemplate& t);
// Deletes one; the last one is never deleted. The active one stays the same template (or the next when it went).
bool removeTouchTemplate(TouchLayout& layout, u32 index);
// A name no template of `layout` has: `base`, else `base` and a number ("Solo 2"), within kTouchNameBytes - 1 bytes.
void uniqueTouchTemplateName(const TouchLayout& layout, std::string_view base, std::array<char, kTouchNameBytes>& out);
// §5.3 "Volver al preset de la clase": `t`'s actions, groups and layers from the preset template of its role in
// `presets` (game::buildClassLayout's), its name, places and site kept. A template of no role takes the first one.
// False when `presets` has none.
bool restoreTouchTemplate(TouchTemplate& t, const TouchLayout& presets);

// --- The server's copy (§5.5) ---

// Account data type 6 (PER_CHARACTER_LAYOUT) holds this client's layout file, or 1.12's own layout-cache.txt when the
// character was last played with the PC's client (it shares the type): the server's text is ours when it starts with
// "VDTOUCH" (then it replaces the local file when it is newer); otherwise the local copy stays and goes up again.
bool isTouchLayoutText(std::string_view text);

} // namespace vd::game
