#pragma once

#include "app/Hud.h"
#include "core/Types.h"
#include "ui/DrawList.h"

#include <span>
#include <string_view>

namespace vd {

// Parity (auras): how the original interface lays out and animates aura buttons, as pure functions (no
// drawing, no game state; unit tested): BuffFrame.lua / .xml (the player's buffs, debuffs and weapon
// enchantments), TargetFrame.lua's TargetDebuffButton_Update (the target's), PartyMemberFrame.lua's
// RefreshBuffs (the party's).

constexpr f32 kBuffWarningSeconds = 31.0f;         // BUFF_WARNING_TIME: the button flashes with less left
constexpr f32 kBuffDurationWarningSeconds = 60.0f; // BUFF_DURATION_WARNING_TIME: its duration turns white
constexpr u64 kBuffFlashMs = 750;                  // BUFF_FLASH_TIME_ON = BUFF_FLASH_TIME_OFF
constexpr f32 kBuffMinAlpha = 0.3f;                // BUFF_MIN_ALPHA
constexpr u32 kMaxPlayerBuffs = 16;                // BuffButton0..15 (HELPFUL)
constexpr u32 kMaxPlayerDebuffs = 8;               // BuffButton16..23 (HARMFUL)
constexpr u32 kMaxTempEnchants = 2;                // TempEnchant1, TempEnchant2
constexpr u32 kMaxTargetBuffs = 5;                 // MAX_TARGET_BUFFS
constexpr u32 kMaxTargetDebuffs = 16;              // MAX_TARGET_DEBUFFS
constexpr u32 kMaxPartyDebuffs = 4;                // MAX_PARTY_DEBUFFS
constexpr f32 kPartyStatusSeconds = 30.0f;         // RefreshBuffs: the party portrait's glow lasts this long

// DebuffTypeColor (BuffFrame.lua): a debuff's border by its dispel type (Spell.dbc Dispel /
// SpellDispelType.dbc: 1 Magic blue, 2 Curse purple, 3 Disease brown, 4 Poison green); "none" red for the
// rest.
u32 debuffTypeColor(u32 dispel);
// Whether the type has a colour of its own (UnitDebuff returns a debuffType: RefreshBuffs' hasDispellable).
bool hasDebuffType(u32 dispel);
// BUFF_ALPHA_VALUE (BuffFrame_OnUpdate): one 1.5 s cycle shared by every button, up from BUFF_MIN_ALPHA to 1
// in 0.75 s and back down.
f32 buffFlashAlpha(u64 nowMs);
// A button's alpha (BuffButton_OnUpdate, BuffFrame_Enchant_OnUpdate): flashing with less than
// BUFF_WARNING_TIME left; an aura without a known duration (`remaining` 0: until cancelled, or not the
// player's) never flashes.
f32 auraButtonAlpha(f32 remaining, u64 nowMs);
// SecondsToTimeAbbrev (UIParent.lua): over a day "2 d", over an hour "3 h", over a minute "15 m" (rounded
// up), else "45 s" (DAY_ / HOUR_ / MINUTE_ / SECOND_ONELETTER_ABBR).
std::string_view secondsToTimeAbbrev(std::span<char> out, f32 seconds);
// What an aura's tooltip says of its time left: "29 minutes remaining", "1 hour remaining", "12 seconds
// remaining" (SPELL_TIME_REMAINING_*, singular for 1; the unit picked and rounded up as SecondsToTimeAbbrev
// does).
std::string_view auraTimeRemaining(std::span<char> out, f32 seconds);
// BuffFrame_UpdateDuration's colour: white (HIGHLIGHT_FONT_COLOR) under a minute, gold (NORMAL_FONT_COLOR)
// above.
u32 auraDurationColor(f32 remaining);

// Where an aura button goes, in logical pixels: its icon and the rectangle that takes its taps (the icon
// grown into the gaps around it, so a row is one strip without holes: the icons are small for a finger).
struct AuraButtonPlace {
    u32 aura = 0; // index into the span laid out
    ui::Rect icon;
    ui::Rect hit;
};

// BuffFrame.xml with the durations shown (SHOW_BUFF_DURATIONS = "1": ours always show): 30-unit buttons 5
// apart, from `right` / `top` (TemporaryEnchantFrame's TOPRIGHT) leftwards. The weapon enchantments first
// (TempEnchant1 rightmost); the first eight buffs left of them (BuffFrame's TOPRIGHT 5 units left of the
// enchantments, or at `right` without them); the next eight a row lower (45 units down) from `right`; the
// debuffs another row lower (90 down) from `right`. Beyond 16 buffs, 8 debuffs and 2 enchantments nothing
// shows. `unit`: logical pixels per interface unit. Returns how many buttons were placed into `out`.
// Phase 15 (options): without the durations (SHOW_BUFF_DURATIONS "0", 1.12's default) the rows sit 35 units apart
// (BuffButtons_UpdatePositions: BuffButton8 5 under the first row, BuffButton16 70 down).
u32 buffFrameLayout(std::span<const HudAura> auras, f32 right, f32 top, f32 unit,
                    std::span<AuraButtonPlace> out, bool durations = true);

// TargetDebuffButton_Update: the first 5 buffs and 16 debuffs of the target under `frame` (the TargetFrame,
// 232 x 100 units), the first row 5 units in and 32 above its bottom. A friendly target (UnitIsFriend) shows
// its buffs first and its debuffs under them; a hostile one its debuffs first and its buffs under the second
// debuff row (under the third with a target of target and ten debuffs or more). The first debuff row wraps
// after 6, or after 5 with a target of target (its frame hangs at the right): with 6 the second row holds the
// other ten; with 5 the second row 5 and a third row the last six. Buttons are 21 units (debuffs 7 to 16:
// 17), all 17 once the debuffs reach the wrap (or with a target of target and 5 buffs); 3 apart, rows 2
// apart. `unit`: logical pixels per interface unit. Returns how many buttons were placed; `bottom` gets the
// lowest edge of a shown one (the frame's bottom when none).
u32 targetAuraLayout(std::span<const HudAura> auras, bool friendly, bool targetOfTarget,
                     const ui::Rect& frame, f32 unit, std::span<AuraButtonPlace> out, f32& bottom);

} // namespace vd
