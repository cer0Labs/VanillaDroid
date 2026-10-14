#include "app/HudDemo.h"

#include "core/Hash.h"

namespace vd {
namespace {

HudActionSlot slot(const char* icon) {
    HudActionSlot s;
    s.iconHash = assetPathHash(icon);
    return s;
}

HudDemo makeDemo() {
    HudDemo d;
    // A warrior's main bar: the auto attack on (checked, flashing), a swing queued, cooldowns, an unusable
    // reactive ability, a spell out of range, one short of rage, a consumable's count.
    d.actions[0] = slot("Interface\\Icons\\Ability_SteelMelee.blp");
    d.actions[0].checked = d.actions[0].flashing = true;
    d.actions[1] = slot("Interface\\Icons\\Ability_Rogue_Ambush.blp");
    d.actions[1].checked = true;
    d.actions[2] = slot("Interface\\Icons\\Ability_Warrior_Charge.blp");
    d.actions[2].inRange = false;
    d.actions[3] = slot("Interface\\Icons\\Ability_Gouge.blp");
    d.actions[4] = slot("Interface\\Icons\\Ability_Warrior_Revenge.blp");
    d.actions[4].usable = false;
    d.actions[5] = slot("Interface\\Icons\\Ability_Warrior_BattleShout.blp");
    d.actions[5].cooldownLeft = 4.0f;
    d.actions[5].cooldownTotal = 6.0f;
    d.actions[6] = slot("Interface\\Icons\\Spell_Nature_ThunderClap.blp");
    d.actions[6].usable = false;
    d.actions[6].noMana = true;
    d.actions[7] = slot("Interface\\Icons\\Ability_ShockWave.blp");
    d.actions[8] = slot("Interface\\Icons\\Ability_Warrior_Sunder.blp");
    d.actions[9] = slot("Interface\\Icons\\Ability_MeleeDamage.blp");
    d.actions[9].usable = false;
    d.actions[10] = slot("Interface\\Icons\\INV_Misc_Food_19.blp");
    d.actions[10].count = 5;
    // Twelve: left empty (the empty slot's art).
    // The bottom left bar: potions on their shared cooldown, a bandage, the Hearthstone, an equipped trinket.
    d.extraActions[0] = slot("Interface\\Icons\\INV_Potion_51.blp");
    d.extraActions[0].count = 4;
    d.extraActions[0].cooldownLeft = 95.0f;
    d.extraActions[0].cooldownTotal = 120.0f;
    d.extraActions[1] = slot("Interface\\Icons\\INV_Potion_54.blp");
    d.extraActions[1].count = 2;
    d.extraActions[1].cooldownLeft = 95.0f;
    d.extraActions[1].cooldownTotal = 120.0f;
    d.extraActions[2] = slot("Interface\\Icons\\INV_Misc_Bandage_15.blp");
    d.extraActions[2].count = 12;
    d.extraActions[3] = slot("Interface\\Icons\\INV_Misc_Rune_01.blp");
    d.extraActions[3].cooldownLeft = 1800.0f;
    d.extraActions[3].cooldownTotal = 3600.0f;
    d.extraActions[4] = slot("Interface\\Icons\\INV_Jewelry_Talisman_07.blp");
    d.extraActions[4].equipped = true;
    // The stance bar: Battle Stance in use, the others on the stances' shared second.
    d.stances[0] = slot("Interface\\Icons\\Ability_Warrior_OffensiveStance.blp");
    d.stances[0].checked = true;
    d.stances[1] = slot("Interface\\Icons\\Ability_Warrior_DefensiveStance.blp");
    d.stances[2] = slot("Interface\\Icons\\Ability_Racial_Avatar.blp");
    d.stances[2].usable = false;
    return d;
}

} // namespace

const HudDemo& hudDemo() {
    static const HudDemo demo = makeDemo();
    return demo;
}

} // namespace vd
