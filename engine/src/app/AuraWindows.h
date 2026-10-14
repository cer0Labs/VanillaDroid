#pragma once

#include "app/AuraLayout.h"
#include "app/Dialogs.h"
#include "app/Hud.h"
#include "core/Types.h"
#include "ui/Context.h"
#include "ui/DrawList.h"

#include <span>
#include <string_view>

namespace vd {

class UiResources;

// Parity (auras): the aura buttons and their tooltips, drawn over the client's art (BuffFrame.xml,
// TargetFrame.xml, PartyFrameTemplates.xml, PetFrame.xml, GameTooltip). Pure views like app/Dialogs: places
// and view-models in, what the finger did out; no game state, no network. app/HudAuras.cpp draws the player's
// and the target's rows with them, app/AuraScreens the pet's and the tooltips, app/SocialWindows the party's
// debuffs.

// How a row of buttons looks: the player's (BuffButtonTemplate: 30 units, the stack count in
// NumberFontNormal, the time left under it, flashing near the end), the target's (TargetDebuffButtonTemplate:
// the count in NumberFontNormalSmall on debuffs only, no time) or a small unit frame's
// (PartyBuffButtonTemplate: icon and border).
// Phase 15 (options): PlayerNoDuration, the player's buttons without their durations (SHOW_BUFF_DURATIONS off).
enum class AuraButtonStyle : u8 { Player, Target, Small, PlayerNoDuration };

// The border a button wears: none (buffs), the debuff overlay in its DebuffTypeColor (UI-Debuff-Overlays),
// the weapon enchantment's (UI-TempEnchant-Border).
struct AuraButtonsResult {
    i32 tapped = -1;   // index into the auras
    i32 held = -1;     // a long press
    ui::Rect rect;     // the tapped or held button's icon
    f32 bottom = 0.0f; // the lowest edge of the buttons' touch rectangles (their durations included)
};
// The buttons of `auras` at `places` (app/AuraLayout). `unit`: logical pixels per interface unit; `idName`:
// the widgets' name (indexes are the auras'). `bordered`: the Small style's buttons show a debuff border even
// on buffs (PetFrame's RefreshBuffs(this, 1): showBuffs keeps the "none" red one).
AuraButtonsResult auraButtons(ui::Context& ui, ui::DrawList& draw, UiResources& resources,
                              std::span<const HudAura> auras, std::span<const AuraButtonPlace> places,
                              f32 unit, u64 nowMs, AuraButtonStyle style, std::string_view idName,
                              bool bordered = false);

// --- Tooltip (GameTooltip:SetPlayerBuff / SetUnitBuff / SetUnitDebuff)
// ------------------------------------------

// Where it hangs from its button (GameTooltip:SetOwner): BottomLeft (BuffButton: ANCHOR_BOTTOMLEFT, the
// tooltip's top right corner at the button's bottom left), BottomRight (the target's: ANCHOR_BOTTOMRIGHT +
// (15, -25)), Right (the pet's and the party's: ANCHOR_RIGHT, its bottom left at the button's top right).
enum class AuraTooltipAnchor : u8 { BottomLeft, BottomRight, Right };
struct AuraTooltipView {
    std::string_view name;
    std::string_view type;        // a debuff's dispel type ("Magic": SpellDispelType.dbc), right of the name
    std::string_view description; // what the aura does (Spell.dbc's aura text, its numbers resolved)
    std::string_view
        timeLeft;    // "29 minutes remaining" (SPELL_TIME_REMAINING_*: the player's own timed auras)
    ui::Rect anchor; // the button
    AuraTooltipAnchor at = AuraTooltipAnchor::BottomLeft;
};
struct AuraTooltipResult {
    ui::Rect panel;
    bool tapped = false; // a tap on it (the caller closes it, as a tap anywhere else)
};
// Drawn with the spell tooltips' fonts (app/CombatWindows: the name in white, the text in gold), kept inside
// the safe area. Not modal: its taps are its own (they do not reach the world); the caller closes it on a tap
// anywhere.
AuraTooltipResult auraTooltip(DialogContext& c, const AuraTooltipView& v);

// --- A unit's aura list (PartyMemberBuffTooltip)
// ---------------------------------------------------------------

struct AuraListView {
    std::span<const HudAura>
        auras;       // the buffs (up to 16) shown first, then the debuffs (up to 8), with borders
    ui::Rect anchor; // the frame it belongs to: the list hangs over its bars (TOPLEFT + (47, -30))
    f32 unit = 1.0f; // logical pixels per interface unit of that frame
};
struct AuraListResult {
    ui::Rect panel;
    i32 tapped = -1; // an icon (index into auras): its own tooltip
    ui::Rect rect;
};
// PartyMemberBuffTooltip_Update: rows of 8 icons (15 units, 17 apart), the buffs' rows then the debuffs', in
// a tooltip backdrop (columns * 17 + 15 by rows * 17 + 15). Nothing is drawn without auras.
AuraListResult auraList(DialogContext& c, const AuraListView& v, u64 nowMs);

} // namespace vd
