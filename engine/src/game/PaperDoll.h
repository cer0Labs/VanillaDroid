#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/ItemTooltip.h"
#include "net/Messages.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

// The numbers of the character sheet (phase 15, PaperDollFrame.lua): what PaperDollFrame_SetStats / SetArmor
// / SetResistances / SetDamage / SetAttackPower / SetAttackBothHands / SetRangedAttack / SetRangedDamage /
// SetRangedAttackPower show and the tooltips their frames' OnEnter build (PaperDollStatTooltip,
// CharacterDamageFrame_OnEnter, MagicResistanceFrameTemplate...), from the update fields as WoW.exe's
// UnitStat (0x518600), UnitArmor (0x5192E0), UnitResistance (0x5184A0), UnitDamage (0x518C20),
// UnitAttackSpeed (0x518E50), UnitRangedDamage (0x518910), UnitAttackPower (0x518F80), UnitRangedAttackPower
// (0x5190C0), UnitAttackBothHands (0x518810) and UnitRangedAttack (0x518B90) read them. Values and tooltip
// texts carry the Lua's colour codes
// (|cff20ff20 green, |cffff2020 red, |cffffffff white). The 1.12 sheet shows no critical, dodge, parry or
// block chances. Main thread; the texts live in a fixed arena rebuilt by build().

enum class PaperDollStat : u8 {
    Strength,
    Agility,
    Stamina,
    Intellect,
    Spirit,
    Armor,
    Attack,       // CharacterAttackFrame: the melee attack rating (MELEE_ATTACK)
    AttackPower,  // CharacterAttackPowerFrame (ATTACK_POWER_COLON)
    Damage,       // CharacterDamageFrame
    RangedAttack, // CharacterRangedAttackFrame (RANGED_ATTACK)
    RangedAttackPower,
    RangedDamage,
    // MagicResFrame1..5, in the frame's order: arcane, fire, nature, frost, shadow.
    ArcaneResistance,
    FireResistance,
    NatureResistance,
    FrostResistance,
    ShadowResistance,
    Count
};
constexpr u32 kPaperDollStats = static_cast<u32>(PaperDollStat::Count);
constexpr u32 kPaperDollResistances = 5;
// The resistance index (UNIT_FIELD_RESISTANCES) of MagicResFrame1..5 (their XML ids).
constexpr u32 kPaperDollResistanceSchools[kPaperDollResistances] = {6, 2, 3, 4, 5};

// What the sheet reads besides the player's fields.
struct PaperDollInput {
    std::string_view
        classFile;          // ChrClasses.dbc ("WARRIOR"): <CLASS>_<STAT>_TOOLTIP, else DEFAULT_<STAT>_TOOLTIP
    bool relicSlot = false; // UnitHasRelicSlot: the ranged numbers show N/A
    // The equipped weapons' templates (nullptr: the slot is empty or its template is not known yet).
    const net::ItemQueryInfo* mainHand = nullptr;
    const net::ItemQueryInfo* offHand = nullptr;
    const net::ItemQueryInfo* ranged = nullptr;
    bool rangedEquipped = false; // GetInventoryItemTexture("player", 18): something in the ranged slot
};

struct PaperDollLine {
    std::string_view label; // "Strength:", "Melee Attack", "Power:"... (resistances: none)
    std::string_view value; // with colour codes
    std::array<TooltipLine, 9> tooltip{};
    u32 tooltipLines = 0; // 0: no tooltip
};

// UnitAttackBothHands / UnitRangedAttack's numbers of one weapon skill: the skill's value plus its permanent
// bonus, and its temporary bonus (never below -value).
struct WeaponRating {
    i32 base = 0;
    i32 modifier = 0;
};
WeaponRating weaponRating(const Entity& player, u32 skill);

class PaperDoll {
public:
    PaperDoll(); // its storage, once (on the heap: the screens holding it live on the stack)
    void build(const Entity& player, const PaperDollInput& in);
    const PaperDollLine& line(PaperDollStat stat) const { return m_lines[static_cast<u32>(stat)]; }
    static constexpr u32 kArenaSize = 6144;

private:
    std::string_view keep(std::string_view s);
    std::string_view print(const char* format, ...);
    void formatStat(PaperDollLine& line, std::string_view name, i32 base, i32 posBuff, i32 negBuff);
    void damage(PaperDollLine& line, std::string_view title, f32 speed, f32 minDamage, f32 maxDamage,
                i32 bonusPos, i32 bonusNeg, f32 percent, bool tooltip);
    TooltipLine& add(PaperDollLine& line, std::string_view left, u32 color, bool wrap = false);

    std::vector<PaperDollLine> m_lines; // kPaperDollStats
    std::vector<char> m_arena;          // kArenaSize
    u32 m_used = 0;
};

} // namespace vd::game
