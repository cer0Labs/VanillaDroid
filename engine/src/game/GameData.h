#pragma once

#include "core/Types.h"
#include "db/DbcView.h"

#include <array>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// What a spell needs as its explicit target (the effects' implicit targets and the Targets mask): how a tap on its
// button picks the target.
enum class SpellTargetKind : u8 {
    None,       // the caster or around it: sent as a self cast
    Enemy,      // a hostile unit (TARGET_UNIT_ENEMY, TARGET_LOCATION_CASTER_TARGET_POSITION)
    Friend,     // a friendly unit; the caster when the target is not one (TARGET_UNIT_FRIEND, _PARTY, _RAID...)
    AnyUnit,    // any unit (TARGET_UNIT)
    GameObject, // a game object (TARGET_GAMEOBJECT, TARGET_LOCKED)
    Location,   // a point on the ground (TARGET_FLAG_DEST_LOCATION)
};

// What a spell looks like and costs, from Spell.dbc and its side tables (1.12.1 layout, field numbers as in
// vMaNGOS' SpellEntry.h).
struct SpellInfo {
    u32 id = 0;
    std::string_view name; // enUS
    std::string_view rank;
    u64 iconHash = 0;      // assetPathHash of its icon texture (SpellIcon.dbc path + ".blp"); 0 if none
    u32 school = 0;
    u32 attributes = 0;
    u32 castTimeMs = 0;    // base (SpellCastTimes.dbc)
    f32 minRange = 0.0f;   // yards (SpellRange.dbc)
    f32 maxRange = 0.0f;
    u32 powerType = 0;     // 0 mana, 1 rage, 2 focus, 3 energy
    u32 powerCost = 0;
    u32 powerCostPercent = 0; // of the base mana
    u32 recoveryMs = 0;       // own cooldown
    u32 categoryRecoveryMs = 0;
    u32 category = 0;
    u32 gcdCategory = 0;      // StartRecoveryCategory (133 = the global cooldown)
    u32 gcdMs = 0;            // StartRecoveryTime
    u32 durationMs = 0;       // of its auras (SpellDuration.dbc base), 0 = none / permanent
    u32 targets = 0;          // TARGET_FLAG_* the spell asks for (0 = implicit)
    u32 effects[3] = {};         // SpellEffects (0 = unused slot)
    u32 implicitTargetA[3] = {}; // SpellTarget of each effect
    u32 implicitTargetB[3] = {};
    i32 effectMisc[3] = {};      // EffectMiscValue (the lock type of OPEN_LOCK effects...)
    // Phase 13 (spells): what the item and place targets and the spell modifiers need.
    u32 attributesEx2 = 0;
    u32 attributesEx3 = 0;
    u32 baseLevel = 0;
    i32 equippedItemClass = -1;         // EquippedItemClass (-1: any)
    u32 equippedItemSubClassMask = 0;   // bit = item subclass (0: any)
    u32 equippedItemInventoryTypeMask = 0; // bit = INVTYPE_* (0: any)
    u32 effectAura[3] = {};             // EffectApplyAuraName
    f32 effectRadius[3] = {};           // yards (SpellRadius.dbc), 0 = none
    u32 spellFamily = 0;                // SpellFamilyName (SpellClassSet: 3 mage, 4 warrior...)
    u64 familyFlags = 0;                // SpellFamilyFlags: the bits the class's spell modifiers name
    bool passive() const { return (attributes & 0x40) != 0; } // SPELL_ATTR_PASSIVE
    SpellTargetKind targetKind() const;
};

// Phase 13 (spells): an enchantment as the enchanting questions name it (SpellItemEnchantment.dbc).
struct EnchantmentInfo {
    std::string_view name; // "+5 Agility", "Instant Poison"
    u32 flags = 0;         // m_flags: 0x01 ENCHANTMENT_CAN_SOULBOUND (enchanting binds the item)
};

// What a spell needs besides its power (Spell.dbc), for the texts of its cast failures (SPELL_FAILED_REAGENTS,
// _TOTEMS, _REQUIRES_SPELL_FOCUS, _ONLY_SHAPESHIFT name what is missing).
struct SpellRequirements {
    u32 stances = 0;                   // shapeshift forms it is cast in (bit = SpellShapeshiftForm id - 1)
    u32 spellFocus = 0;                // SpellFocusObject.dbc: a forge, a cooking fire...
    std::array<u32, 2> totems{};       // items carried (item entries)
    std::array<u32, 8> reagents{};     // items consumed
    std::array<u32, 8> reagentCounts{};
    u32 targetCreatureType = 0;        // phase 20 (classes): bit (CreatureType.dbc id - 1) its target must be, 0 any
};

// Phase 20 (retouches): a rest state (PLAYER_BYTES_2 byte 3) as Exhaustion.dbc has it, read by WoW.exe for
// the experience lines (0x637B2A) and GetRestState.
struct RestStateInfo {
    f32 factor = 0.0f;     // the experience from monsters (2 rested, 1 normal); 0: the state is unknown
    std::string_view name; // "Rested", "Normal"
};

// Static client tables the game UI needs (phase 7): spells and item icons. The DBCs stay in the mapped db pack; this
// only indexes them.
class GameData {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return m_spells.recordCount() > 0; }

    // nullopt when the spell is not in the client's table.
    std::optional<SpellInfo> spell(u32 id) const;
    // Icon texture of an item display (ItemDisplayInfo.dbc inventory icon), 0 if unknown.
    u64 itemIconHash(u32 displayId) const;
    // The LockType (LockType.dbc) a spell must open for this lock (Lock.dbc), as the client picks the "Opening"
    // spell of a locked chest or quest object; 0 when the lock needs none (or a key item).
    u32 lockOpenType(u32 lockId) const;

    // For the texts of cast failures (rare: linear searches, no index). Names in the client's language, "" if unknown.
    std::optional<SpellRequirements> requirements(u32 spellId) const;
    std::string_view spellFocusName(u32 id) const;     // SpellFocusObject.dbc ("Anvil", "Cooking Fire"...)
    std::string_view shapeshiftFormName(u32 id) const; // SpellShapeshiftForm.dbc ("Cat Form", "Battle Stance"...)
    std::string_view areaName(u32 id) const;           // AreaTable.dbc ("Elwynn Forest")
    // Phase 13: ServerMessages.dbc's text of a SMSG_SERVER_MESSAGE type ("[SERVER] Shutdown in %s"), "" if none.
    std::string_view serverMessage(u32 id) const;
    // Phase 13 (spells): an enchantment's name and flags (nullopt if unknown), and the spell family of a class's
    // spells (ChrClasses.dbc m_spellClassSet: the family its spell modifiers apply to; 0 unknown).
    std::optional<EnchantmentInfo> enchantment(u32 id) const;
    u32 classSpellFamily(u32 cls) const;
    RestStateInfo restState(u32 id) const; // phase 20 (retouches): Exhaustion.dbc

private:
    db::DbcView m_spells;
    db::DbcView m_icons;
    db::DbcView m_castTimes;
    db::DbcView m_ranges;
    db::DbcView m_durations;
    db::DbcView m_itemDisplays;
    db::DbcView m_locks;
    db::DbcView m_spellFocus;
    db::DbcView m_shapeshiftForms;
    db::DbcView m_areas;
    db::DbcView m_serverMessages; // phase 13
    std::unordered_map<u32, u32> m_spellRecords; // spell id -> record
    std::unordered_map<u32, u32> m_iconRecords;
    std::unordered_map<u32, u32> m_castTimeRecords;
    std::unordered_map<u32, u32> m_rangeRecords;
    std::unordered_map<u32, u32> m_durationRecords;
    std::unordered_map<u32, u32> m_itemDisplayRecords;
    std::unordered_map<u32, u32> m_lockRecords;
    // Phase 13 (spells).
    db::DbcView m_radii;
    db::DbcView m_enchantments;
    db::DbcView m_classes;
    db::DbcView m_exhaustion; // phase 20 (retouches)
    std::unordered_map<u32, u32> m_radiusRecords;
    std::unordered_map<u32, u32> m_enchantmentRecords;
};

} // namespace vd::game
