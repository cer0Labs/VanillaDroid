#pragma once

#include "app/HudActionSlot.h"
#include "core/Types.h"
#include "game/TouchLayout.h"
#include "ui/DrawList.h"

#include <array>
#include <string_view>

namespace vd {

// Phase 20 (docs/MOBILE_CONTROLS.md): the touch cluster as the HUD draws it this frame (app/HudTouchBar.cpp) and
// what the player did with it. app/TouchBarScreens fills the view from the character's touch layout and the game's
// state, and applies the output. The places are game::TouchPlace's (indices into `places`).

// One round button.
struct TouchButtonView {
    HudActionSlot slot;   // icon and states, as the action bars' (cooldown, usable, range, count...)
    bool shown = false;   // drawn (a fixed button with nothing to do, an empty slot outside the edit mode: not)
    bool group = false;   // a group (§6.6): the corner mark; a hold opens its fan
    bool glow = false;    // a reactive ability usable now, the class bar's form in use...
    bool isNew = false;   // put there automatically and not used yet (§4: "New")
    bool onPress = false; // fires as the finger goes down (hostile spells, §4); else when it is lifted on it
    bool repeat = false;  // held still, it fires again whenever it can (§8: "Repetir manteniendo")
    bool aim = false;     // dragged out of it, it aims (§4: a unit frame, the ground): TouchBarOutput's drag
    bool aimGround = false; // phase 20 (assists): ...at the ground (a pad round the button), else at a unit frame
    u64 cornerIcon = 0;   // a small sprite over its top right (the pet's happiness...; atlas path hash, 0: none)
    // Our own glyph (art/: jump, mount, target) instead of the icon: an atlas sprite drawn light over the socket
    // (atlas path hash, 0: none; the icon when the pack has no such sprite).
    u64 glyph = 0;
    // Phase 20 (pets): a unit's portrait in the circle instead of the icon (the pet's; invalid: the icon), the part of
    // the corner sprite's texture it shows (UI-PetHappiness' faces), and a button that keeps working while the cluster
    // is dimmed (§5.2 row 2: what breaks the fear or the stun, the pet's commands).
    TextureHandle portrait;
    ui::TexCoords cornerUv;
    bool breaksControl = false;
};

constexpr u32 kTouchFanItems = 8;      // a group's actions (game::TouchGroup::kMaxActions)
constexpr u32 kTouchClassBarItems = 6; // stances, forms, the shaman's four totem groups (§6.7)
// TouchBarView::fanPlace / TouchBarOutput::fanOpen of the class bar's buttons: kTouchClassBarFan + their index.
constexpr i32 kTouchClassBarFan = 64;

struct TouchBarView {
    const game::TouchGeometry* geometry = nullptr; // the active template's places for this screen's profile
    bool mirrored = false;                         // left-handed: the whole cluster on the left (§3.5)
    std::array<TouchButtonView, game::kTouchPlaces> places{};
    // Combo points (rogue, cat form): gems on an arc over Main (0: none).
    u32 comboPoints = 0;
    // The class bar (§6.7): a row over the consumables (square screens) or one button with a fan (wide ones, its place
    // shows `classBarFan`'s icon); the row's buttons may be groups themselves (the shaman's totems).
    std::array<TouchButtonView, kTouchClassBarItems> classBar{};
    u32 classBarCount = 0;
    // The fan of the group being held: its place (or kTouchClassBarFan + i), its actions and the current one. The HUD
    // reports a fan opening (TouchBarOutput::fanOpen); the screens fill these from the next frame on.
    i32 fanPlace = -1;
    std::array<HudActionSlot, kTouchFanItems> fan{};
    u32 fanCount = 0;
    u32 fanCurrent = 0;
    // The templates' button: the active one's name (shown briefly after a change) and its number.
    std::string_view templateName;
    u32 templateIndex = 0;
    u32 templateCount = 1;
    u64 templateShownMs = 0; // when the name was shown last (a change): it fades after 1.5 s
    bool dimmed = false;     // the player has no control (§5.2): the cluster faint, its taps ignored
    // Phase 20 (edit): the edit mode (§9): every place outlined, also the hidden ones and empty slots; the HUD's own
    // touch handling gives way to Hud::touchEdit (app/HudTouchEdit.cpp).
    bool editing = false;
    i32 editSelected = -1;   // the place picked in the edit mode
    bool editLayout = false; // dragging moves places (else it swaps actions)
    u16 editInherited = 0;   // content slots showing the base's action in the layer edited (translucent, a chain)
    u16 editHidden = 0;      // ...showing nothing by the player's choice (a crossed circle)
    ui::Rect editPanel;      // the edit mode's panel (places are not dropped under it)
    // Phase 20 (assists, §8 "Objetivo automático"): no hostile target: a press says which unit the Target button would
    // choose (TouchBarOutput::enemy).
    bool wantsEnemy = false;
};

// What the player did with the cluster this frame (places as game::TouchPlace indices; -1 none).
struct TouchBarOutput {
    i32 pressed = -1;   // a button used (a skill or consumable slot, Mount, Reactive, Templates, Drawer, Pet)
    i32 held = -1;      // held still without a fan or a repeat: its tooltip (skills), its menu (fixed buttons)
    i32 repeating = -1; // a repeating button still held past its delay (every frame): fire again when it can
    i32 fanOpen = -1;   // a group's fan opened this frame: its place (or kTouchClassBarFan + i)
    i32 fanPlace = -1;  // ...an action chosen from the open fan: the fan's place
    i32 fanChosen = -1; // ...and its index
    i32 classBar = -1;  // a class bar button tapped (its index)
    i32 swipe = 0;      // the templates' button swiped: -1 the previous template, +1 the next one
    ui::Rect heldRect;  // where the held button is (its tooltip goes beside it)
    // §4 "Arrastrar = apuntar": a button with `aim` dragged out of its circle (phase 20, assists): where the finger is
    // (logical pixels), and the frame it is lifted (dropped: drop on a unit frame / the ground; back on the button:
    // cancelled).
    i32 dragPlace = -1;
    f32 dragX = 0.0f, dragY = 0.0f;
    bool dragDropped = false;
    // Phase 20 (assists): the finger back on the button (lifted there: cancelled), and its offset from the button as a
    // share of the ground aim's pad (x right, y down; 0 at the button's rim, length 1 at the pad's edge).
    bool dragOnButton = false;
    f32 dragNx = 0.0f, dragNy = 0.0f;
    // Phase 20 (assists): with TouchBarView::wantsEnemy, the unit the Target button would choose (Hud::nextEnemy), set
    // in the frames a button fires (0: none).
    u64 enemy = 0;
    // Phase 20 (edit): what the edit mode did (app/HudTouchEdit.cpp fills it, app/TouchEditScreens applies it).
    i32 editTapped = -1;   // a place tapped (select it / pick its action)
    i32 editMoved = -1;    // a place dragged: its new center relative to Main's, mm (layout mode)
    f32 editX = 0.0f, editY = 0.0f;
    i32 editDropOn = -1;   // actions mode: the place `editTapped` was dropped on (swap them)
    // Where places may go this frame: the safe area less the panel, in the geometry's mm (game::TouchArea).
    f32 editAreaLeft = 0.0f, editAreaRight = 0.0f, editAreaBottom = 0.0f, editAreaTop = 0.0f;
    f32 editPinch = 0.0f;  // two fingers pinching: their distance over the distance they started at (0: none)
};

} // namespace vd
