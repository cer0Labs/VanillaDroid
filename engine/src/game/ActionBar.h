#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/GameData.h"
#include "game/SpellMods.h"
#include "game/SpellText.h"

#include <array>
#include <span>

namespace vd::game {

class AbilityData;

// The rules of the client's action bars (parity pass): which of the 120 action slots a button shows
// (ActionButton.lua, MultiActionBars.lua, BonusActionBarFrame.lua), when a button is usable, short of power or out
// of range (ActionButton_UpdateUsable / ActionButton_OnUpdate: IsUsableAction, IsActionInRange) and the forms of
// the stance / shapeshift bar (ShapeshiftBar_Update: GetShapeshiftFormInfo). Plain functions over the update
// fields and the client tables: app/CombatScreens fills the HUD's buttons with them.

// --- Slots -------------------------------------------------------------------------------------------------

constexpr u32 kActionBarButtons = 12;       // NUM_ACTIONBAR_BUTTONS
constexpr u32 kActionBarPages = 6;          // NUM_ACTIONBAR_PAGES
constexpr u32 kBottomLeftActionBarPage = 6; // BOTTOMLEFT_ACTIONBAR_PAGE (1-based): slots 60..71
constexpr u32 kAttackSpell = 6603;          // "Attack": the auto attack toggle (IsAttackAction)

// The bonus bar a form swaps in on the first page (GetBonusBarOffset: SpellShapeshiftForm's m_bonusActionBar;
// 1 cat / battle stance / stealth, 2 defensive stance, 3 bear / berserker stance), 0 none. Without the table
// (tests, no packs) the 1.12 values of those forms.
u32 bonusBarOffset(u32 form, const AbilityData* data);
// ActionButton_GetPagedID: the action slot (0..119) of main bar button `button` (0..11) on `page` (0..5). The
// first page shows the form's bonus bar when `bonusOffset` > 0 (BonusActionBarFrame over the main bar):
// (NUM_ACTIONBAR_PAGES + offset - 1) * 12.
u32 mainBarSlot(u32 page, u32 button, u32 bonusOffset);
// A MultiActionBar's slot: button `button` (0..11) of the bar that shows page `page` (1-based:
// kBottomLeftActionBarPage...).
u32 multiBarSlot(u32 page, u32 button);

// --- Button states -----------------------------------------------------------------------------------------

// IsUsableAction: usable, or not only because the power is short (ActionButton_UpdateUsable tints those blue,
// the rest grey).
struct ActionUsable {
    bool usable = true;
    bool noMana = false;
};
// The power `spell` costs the player now: its flat cost plus its share of the base mana (Spell::CalculatePowerCost:
// ManaCostPercentage of the creation mana, UNIT_FIELD_BASE_MANA). Phase 13 (spells): with the school's power cost
// fields and the spell modifiers as WoW.exe adds them (game/SpellMods: modifiedPowerCost).
u32 spellPowerCost(const SpellInfo& spell, const Entity& me, const SpellMods* mods = nullptr);
// A spell's button: power (short: noMana), the aura state the caster and the target need (Revenge, Overpower,
// Execute), combo points (finishers) and the form it is cast in (`forms` decides; null: any form), a dead caster.
// `details` may be null (only the power then). Phase 20 (classes): with `needs` and `state`, also what the spell
// needs besides (spellNeedMissing below: stealth, combat, the target's type, equipped items, reagents, totems).
struct SpellNeedsState;
ActionUsable spellUsable(const SpellInfo& spell, const SpellDetails* details, const Entity& me, const Entity* target,
                         u32 comboPoints, const AbilityData* forms, const SpellMods* mods = nullptr,
                         const SpellRequirements* needs = nullptr, const SpellNeedsState* state = nullptr);

// Where a spell lands, the unit IsActionInRange measures to (Gameplay::spellTarget casts with the same rule): an
// enemy spell on the target; a spell for any unit on the target, or on the player without one; a helpful spell on a
// friendly target (`targetFriendly`: the target is the player or friendly to it), else on the player (the client's
// auto self cast); other spells on no unit (0).
u64 spellLandsOn(SpellTargetKind kind, u64 player, u64 target, bool targetFriendly);

// IsActionInRange: nothing to say (no range, cast on the player, no unit to measure), in range or out of it.
enum class ActionRange : u8 { None, In, Out };
// Whether `spell` reaches `unit` (where it would land: Gameplay::spellTarget; null or the player: None), as
// Spell::CheckRange measures: melee abilities (5 yd "combat range") by both combat reaches in 2D
// (CanReachWithMeleeSpellAttack), the rest by the 3D distance less both combat reaches, against the maximum range
// (plus the server's 1.25 yd) and the minimum one (Auto Shot's dead zone). Phase 13 (spells): the maximum with
// SPELLMOD_RANGE (`mods` may be null).
ActionRange spellRange(const SpellInfo& spell, const Entity& me, const Entity* unit, u32 nowMs,
                       const SpellMods* mods = nullptr);

// --- Stance / shapeshift bar -------------------------------------------------------------------------------

constexpr u32 kMaxShapeshiftForms = 10; // NUM_SHAPESHIFT_SLOTS
struct ShapeshiftForm {
    u32 spellId = 0;
    u32 form = 0;  // SpellShapeshiftForm id (UNIT_FIELD_BYTES_1 byte 2 while in it)
    u32 level = 0; // the spell's level: the bar's order
};
// The forms the player can take: known spells that apply SPELL_AURA_MOD_SHAPESHIFT (warrior stances, druid forms,
// Stealth, Shadowform, Ghost Wolf), one button per form (its highest rank), in the order the client's spell book
// learned them (by level; Dire Bear Form keeps Bear Form's place, as the rank that superseded it). Returns how many
// (at most out.size()).
u32 shapeshiftForms(std::span<const u32> knownSpells, const SpellText& spells, std::span<ShapeshiftForm> out);
// What a tap on a form's button does (CastShapeshiftForm): cast it; leave it when it is the form in use and can be
// left (CMSG_CANCEL_AURA: druid forms, Stealth, Shadowform); nothing for the warrior stance in use (the server
// ignores cancelling an aura without an icon: SPELL_ATTR_EX_NO_AURA_ICON).
enum class FormTap : u8 { Cast, Cancel, Nothing };
FormTap formTap(const SpellDetails& spell, bool active);

// --- Phase 20 (classes): what else a spell needs (docs/MOBILE_CONTROLS.md §10.3) ---------------------------------
// IsUsableAction leaves these to the tooltip's red lines, and the touch cluster has no tooltip to read: the rules of
// vMaNGOS' Spell::CheckCast, Spell::CheckItems and Spell::CheckTargetCreatureType the client can see. The classic
// bars use them too.

constexpr u32 kWornSlots = 19; // EQUIPMENT_SLOT_HEAD .. EQUIPMENT_SLOT_TABARD
// A worn item as a spell's equipped item requirement sees it.
struct WornItem {
    u32 entry = 0;
    u32 count = 0;       // its stack (a thrown weapon's)
    i32 itemClass = -1;  // ItemClass; -1 nothing worn, or broken (Item::IsBroken: it fits nothing)
    i32 subclass = 0;
    bool unknown = false; // worn, its item query not answered yet: it fits (nothing greys for a missing answer)
};
// Player::HasItemFitToSpellReqirements (Item::IsFitToSpellRequirements): a weapon in the main hand, the off hand or
// the ranged slot, as the spell's attack type allows (GetWeaponAttackType: a main hand ability ignores the off
// hand's weapon, an off hand one the main hand's); armor in any other slot, a shield in the off hand. A spell aimed
// at an item (a sharpening stone, a poison) needs nothing worn.
bool wornFitsSpell(const SpellInfo& spell, std::span<const WornItem> worn);

// What a frame's buttons share for the checks below.
struct SpellNeedsState {
    std::span<const WornItem> worn; // by equipment slot (kWornSlots); empty: the equipped items are not checked
    u32 targetCreatureType = 0;     // the target creature's CreatureType.dbc id (its query); 0: unknown, a player
    const void* context = nullptr;
    // Items carried in the bags (reagents, totems, ammunition); null: those are not checked.
    u32 (*carried)(const void* context, u32 entry) = nullptr;
};

enum class SpellNeed : u8 {
    Met,
    Stealth,        // SPELL_FAILED_ONLY_STEALTHED
    NotInCombat,    // SPELL_FAILED_AFFECTING_COMBAT
    PeacefulTarget, // SPELL_FAILED_TARGET_AFFECTING_COMBAT
    CreatureType,   // SPELL_FAILED_BAD_TARGETS / _TARGET_IS_PLAYER
    EquippedItem,   // SPELL_FAILED_EQUIPPED_ITEM_CLASS
    Ammo,           // SPELL_FAILED_NO_AMMO
    Reagents,       // SPELL_FAILED_ITEM_NOT_READY (CheckItems' reagents)
    Totem,          // SPELL_FAILED_ITEM_GONE (CheckItems' totems)
};
// The first thing `spell` lacks (Met: nothing): SPELL_ATTR_ONLY_STEALTHED (Ambush, Ravage, Pounce: any
// SPELL_AURA_MOD_STEALTH, which sets the creep flag: Stealth, Prowl, Shadowmeld),
// SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL in combat (Stealth, Charge, food), SPELL_ATTR_EX_ONLY_PEACEFUL_TARGETS on a
// target in combat (Sap), the target's creature type (TargetCreatureType: Exorcism, Banish, Hibernate, Shackle
// Undead; Curse of Doom never on players), the equipped item, the ammunition of a ranged weapon's shot (CheckItems'
// weapon damage: Auto Shot, Shoot Bow, Throw), the reagents and the totems carried. `details` may be null (its
// attributesEx rules are then skipped).
SpellNeed spellNeedMissing(const SpellInfo& spell, const SpellDetails* details, const SpellRequirements& needs,
                           const Entity& me, const Entity* target, const SpellNeedsState& state);
// A player's creature type (Unit::GetCreatureType): its form's (SpellShapeshiftForm.dbc: a beast in Cat, Bear,
// Dire Bear, Travel, Aquatic Form and Ghost Wolf), else its race's (humanoid for every race in ChrRaces.dbc).
u32 playerCreatureType(u32 form);
// The count a touch button shows on a spell (§4 "Cargas / cuentas"): its first reagent carried (soul shards, runes,
// Symbol of Divinity, Ankh), the ammunition of a ranged weapon's shot (Auto Shot), the thrown weapon's stack; 0: none.
u32 spellShownCount(const SpellInfo& spell, const SpellRequirements& needs, const Entity& me,
                    const SpellNeedsState& state);

class Inventory;
// Counts of carried items for those checks, kept until the bags change (Inventory::version): no bag scan per frame.
class CarriedCounts {
public:
    u32 count(const Inventory& inventory, u32 entry) const;

private:
    static constexpr u32 kEntries = 16;
    mutable u32 m_version = 0;
    mutable std::array<u32, kEntries> m_entries{};
    mutable std::array<u32, kEntries> m_counts{};
    mutable u32 m_used = 0;
    mutable u32 m_next = 0; // the next to replace when full
};

} // namespace vd::game
