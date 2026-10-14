#include "game/TouchContext.h"

namespace vd::game {

TouchContext touchContextOf(u32 form, bool prowling, bool hunterMelee) {
    switch (form) {
    case kFormBattleStance: return TouchContext::BattleStance;
    case kFormDefensiveStance: return TouchContext::DefensiveStance;
    case kFormBerserkerStance: return TouchContext::BerserkerStance;
    case kFormStealth: return TouchContext::Stealth;
    case kFormCat: return prowling ? TouchContext::Prowl : TouchContext::Cat;
    case kFormBear:
    case kFormDireBear: return TouchContext::Bear;
    case kFormMoonkin: return TouchContext::Moonkin;
    case kFormTravel:
    case kFormAquatic:
    case kFormGhostWolf: return TouchContext::Travel;
    case kFormShadowform: return TouchContext::Shadowform;
    default: break;
    }
    return hunterMelee ? TouchContext::Melee : TouchContext::Base;
}

u32 touchContextBonusBar(TouchContext context) {
    switch (context) {
    case TouchContext::BattleStance:
    case TouchContext::Stealth:
    case TouchContext::Cat: return 1;
    case TouchContext::DefensiveStance: return 2;
    case TouchContext::BerserkerStance:
    case TouchContext::Bear: return 3;
    default: return 0;
    }
}

bool hunterMeleeLayer(bool wasMelee, bool haveTarget, f32 gap) {
    if (!haveTarget) {
        return false;
    }
    // Melee reach: the 5 yd "combat range" between both reaches (game/ActionBar's melee rule, without the reaches'
    // sum here: `gap` already took them off); Auto Shot works from 8 yd.
    constexpr f32 kMeleeIn = 2.0f; // the reaches of two players (1.5 each) leave about 2 yd of the 5
    constexpr f32 kRangedOut = 8.0f;
    if (gap <= kMeleeIn) {
        return true;
    }
    if (gap >= kRangedOut) {
        return false;
    }
    return wasMelee;
}

} // namespace vd::game
