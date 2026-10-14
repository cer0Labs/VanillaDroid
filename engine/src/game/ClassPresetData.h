#pragma once

#include "core/Types.h"
#include "game/ClassPresets.h"
#include "game/TouchLayout.h"

#include <array>
#include <span>

// Phase 20 (classes): the tables of the class presets (game/ClassPresetData.cpp) as game/ClassPresets.cpp reads them.
// Spells are their first rank's id (a slot casts the highest rank the player knows: game/SpellLines); every id was
// checked against the client's Spell.dbc, SkillLineAbility.dbc and Talent.dbc (docs/MOBILE_CONTROLS.md §7).
namespace vd::game::presets {

enum class CandKind : u8 {
    None,
    Spell,      // id: a spell (first rank)
    Group,      // id: a GroupKind: the template's group of that kind, filled with the known members
    Consumable, // id: a TouchConsumable
    SameAs,     // id: a TouchContext: what that layer's slot holds (the druid's Prowl takes Cat Form's)
    Hide,       // nothing here (a layer whose base actions cannot be used in it: the druid's animal forms)
};

constexpr u8 kIfTopTree = 0x01;    // the spell needs `tree` to be the talent tree with the most points
constexpr u8 kOrUntalented = 0x02; // ...or no talent points at all

struct Cand {
    CandKind kind = CandKind::None;
    u8 flags = 0;
    u16 tree = 0; // TalentTab id (kIfTopTree)
    u32 id = 0;
};

constexpr u32 kMaxCands = 5;
// A slot: its candidates, the first that resolves wins. None at all: the layer inherits the base there.
struct Slot {
    std::array<Cand, kMaxCands> cands{};
};
// A layer's slots by TouchPlace: Main, A, B, C, D, E, F, G, H, the three consumables.
using Table = std::array<Slot, kTouchSlots>;

struct LayerDef {
    TouchContext context = TouchContext::Base;
    const Table* table = nullptr;
};

// The group kinds (TouchGroup::autoKind; kept in the layout file: never renumbered).
enum class GroupKind : u8 {
    None = 0,
    WarriorBattleArea = 1,
    WarriorShouts = 2,
    WarriorBattleDefense = 3,
    WarriorCooldowns = 4,
    WarriorTaunts = 5,
    WarriorShieldWall = 6,
    WarriorBerserkerDefense = 7,
    WarriorTankShouts = 8,
    RogueFinishers = 10,
    RogueInterrupts = 11,
    RogueCooldowns = 12,
    RogueDefense = 13,
    RogueVanish = 14,
    RogueStealthExit = 15,
    DruidControl = 20,
    DruidBuffs = 21,
    DruidDefense = 22,
    DruidCooldowns = 23,
    DruidBearTaunts = 24,
    DruidCures = 25,
    DruidHealCooldowns = 26,
    DruidBearDefense = 27,
    DruidHealDefense = 28,
    HunterStings = 30,
    HunterArea = 31,
    HunterControl = 32,
    HunterAspects = 33,
    HunterTraps = 34,
    HunterCooldowns = 35,
    HunterMeleeDefense = 36,
    MageArea = 40,
    MagePolymorphs = 41,
    MageBuffs = 42,
    MageDefense = 43,
    MageCooldowns = 44,
    WarlockFire = 50,
    WarlockArea = 51,
    WarlockControl = 52,
    WarlockCurses = 53,
    WarlockBuffs = 54,
    WarlockLife = 55,
    WarlockDrain = 56,
    PriestDispels = 60,
    PriestBuffs = 61,
    PriestHealDefense = 62,
    PriestHealCooldowns = 63,
    PriestDamageDefense = 64,
    PriestWand = 65,
    PaladinSeals = 70,
    PaladinHeals = 71,
    PaladinHoly = 72,
    PaladinBlessings = 73,
    PaladinAuras = 74,
    PaladinDefense = 75,
    PaladinCooldowns = 76,
    PaladinCleanse = 77,
    PaladinHealCooldowns = 78,
    ShamanEarthTotems = 80,
    ShamanFireTotems = 81,
    ShamanWaterTotems = 82,
    ShamanAirTotems = 83,
    ShamanShocks = 84,
    ShamanWeapons = 85,
    ShamanHeals = 86,
    ShamanCooldowns = 87,
    ShamanFrostShocks = 88,
    ShamanCures = 89,
    ShamanHealShocks = 90,
    ShamanManaTide = 91,
    Racial = 100,
};

// A member of a group: the first of its spells the player knows (Ice Armor over Frost Armor; one racial of each
// race).
constexpr u32 kMaxAlternatives = 5;
struct Member {
    std::array<u32, kMaxAlternatives> spells{};
};
struct GroupDef {
    GroupKind kind = GroupKind::None;
    std::array<Member, TouchGroup::kMaxActions> members{};
};

constexpr u32 kMaxPresetLayers = 4;
constexpr u32 kMaxPresetTrees = 2;
constexpr u32 kClassBarKinds = 4;
struct TemplateDef {
    PresetRole role = PresetRole::None;
    u8 cls = 0;
    const char* nameKey = "";
    // The talent trees this template is made for (TalentTab ids): it is the active one when the player has the most
    // points in one of them.
    std::array<u16, kMaxPresetTrees> trees{};
    std::array<LayerDef, kMaxPresetLayers>
        layers{};           // the base first; a layer may copy one before it (SameAs)
    u16 disabledLayers = 0; // TouchTemplate::disabledLayers of a new template
    std::array<GroupKind, kClassBarKinds> classBar{}; // the class bar's groups (the shaman's totems)
};

std::span<const TemplateDef> templates();
const TemplateDef* templateOf(PresetRole role);
std::span<const GroupDef> groups();
const GroupDef* groupOf(GroupKind kind);
// The class whose presets use a group kind (0: any class's: the racials).
u32 groupClass(GroupKind kind);
// The races that learn a spell (bit race - 1), for the spells of the presets only some races have (the racials, the
// priest's race spells: SkillLineAbility.dbc's race masks); 0: every race.
u32 raceMaskOf(u32 spell);
// Spells the demo's known list adds for a class besides the presets' (its forms: the class bar).
std::span<const u32> demoExtraSpells(u32 cls);

} // namespace vd::game::presets
