#pragma once

#include "core/Types.h"
#include "game/TouchLayout.h"

namespace vd::game {

// Phase 20 (docs/MOBILE_CONTROLS.md §5.2): which layer of a touch template shows. Plain functions over what the update
// fields say; app/TouchBarScreens keeps what needs memory (the hunter's melee range with its hysteresis).

// SpellShapeshiftForm.dbc ids (UNIT_FIELD_BYTES_1 byte 2 while in the form).
constexpr u32 kFormCat = 1;
constexpr u32 kFormTravel = 3;
constexpr u32 kFormAquatic = 4;
constexpr u32 kFormBear = 5;
constexpr u32 kFormDireBear = 8;
constexpr u32 kFormGhostWolf = 16;
constexpr u32 kFormBattleStance = 17;
constexpr u32 kFormDefensiveStance = 18;
constexpr u32 kFormBerserkerStance = 19;
constexpr u32 kFormShadowform = 28;
constexpr u32 kFormStealth = 30;
constexpr u32 kFormMoonkin = 31;

// UNIT_FIELD_BYTES_1 byte 3 (UNIT_BYTE1_FLAG_*): stealthed (Prowl in Cat Form; Stealth is its own form).
constexpr u32 kUnitByte1Creep = 0x02;

// The layer of the player's state: the form's (Stealth's is the form 30, not the stealth flag: a night elf's
// Shadowmeld changes nothing), Prowl over Cat, the hunter's melee range over the base.
TouchContext touchContextOf(u32 form, bool prowling, bool hunterMelee);

// The 1.12 bonus bar a layer stands for (game/ActionBar's bonusBarOffset: 1 battle stance / stealth / cat, 2
// defensive, 3 berserker / bear), 0 for the base and the layers 1.12 did not have: the first template of a character
// is filled from the server's bars with it (app/TouchBarScreens).
u32 touchContextBonusBar(TouchContext context);

// The hunter's melee layer with its hysteresis (the 5-8 yd dead zone of Auto Shot): in once the target is within
// melee reach (`gap`: the distance less both combat reaches, yards), out again at 8 yd or more, unchanged between.
bool hunterMeleeLayer(bool wasMelee, bool haveTarget, f32 gap);

} // namespace vd::game
