#pragma once

#include "core/Types.h"
#include "game/TouchLayout.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

class AbilityData;
class GameData;
class SpellText;

// Phase 20 (docs/MOBILE_CONTROLS.md §7): the classes' presets for the touch cluster. Each slot of a preset has a list
// of candidate spells; it takes the first one the player knows that no earlier slot took (in the order Main, A, C, D,
// E, B, F, G, H), so one table serves level 1 and level 60 and every talent tree. Groups refill from the known spells
// (all the auras, the totems of an element...). Only the slots and groups the player did not change by hand
// (kTouchManual) are ever filled or refilled.

// What a preset is built from.
struct PresetInput {
    u32 cls = 0;  // ChrClasses (1 warrior .. 11 druid)
    u32 race = 0; // ChrRaces
    u32 level = 0;
    std::span<const u32> known; // the player's spells (SpellBook::known)
    const SpellText* text = nullptr;
    const AbilityData* abilities = nullptr; // the talents (the tree with the most points picks some presets)
    const GameData* data = nullptr;
    // The weapons (candidates that need one: Backstab and Ambush a dagger, Shield Bash a shield, Shoot a wand): the
    // main hand's and the off hand's item class and subclass (-1: nothing worn), the ranged slot's.
    i32 mainHandClass = -1, mainHandSubclass = -1;
    i32 offHandClass = -1, offHandSubclass = -1;
    i32 rangedClass = -1, rangedSubclass = -1;
    // Phase 20 (classes): false while a worn weapon's item query has not been answered (its requirements count as
    // met until then: the first layout of a login does not lose Backstab to an unknown dagger).
    bool weaponsKnown = true;
};

// The places a refill put a new action in (the "New" badge, §4): a bit per slot in each layer.
struct PresetChanges {
    std::array<u16, kTouchContexts> slots{};
    bool any() const {
        for (const u16 s : slots) {
            if (s != 0) {
                return true;
            }
        }
        return false;
    }
};

// A new character's touch layout: the class's templates (1-3 by role, §7: "Solo", "Sanar"...), filled for what the
// player knows now. False: no preset for the class (the caller fills a template from the server's bars).
bool buildClassLayout(const PresetInput& in, TouchLayout& out);
// The known spells changed (a spell learned, the talents reset, another weapon): the automatic slots and groups of
// `t` refilled from its preset (t.role). Returns what got a new action.
PresetChanges refreshClassTemplate(const PresetInput& in, TouchTemplate& t);

// --- Phase 20 (classes) ------------------------------------------------------------------------------------------

// The roles of the presets (TouchTemplate::role; kept in the layout file: never renumbered).
enum class PresetRole : u8 {
    None,
    WarriorArmsFury = 1,
    WarriorTank = 2,
    Rogue = 3,
    Druid = 4,
    DruidHeal = 5,
    Hunter = 6,
    Mage = 7,
    Warlock = 8,
    PriestHeal = 9,
    PriestDamage = 10,
    PaladinRetribution = 11,
    PaladinHeal = 12,
    PaladinProtect = 13,
    ShamanElemental = 14,
    ShamanEnhancement = 15,
    ShamanHeal = 16,
};

// The roles of a class's presets in their order (at most out.size()). Returns how many.
u32 classPresetRoles(u32 cls, std::span<PresetRole> out);
// The text key of a role's template name (VD_TOUCH_CLASSES_*), empty for none.
std::string_view presetRoleNameKey(PresetRole role);
// "Volver al preset de la clase" (§5.3): `t`'s actions, groups and layers as its preset makes them now (what the
// player changed is dropped; its name, places and site stay). False when t.role is not a preset of in.cls.
bool resetClassTemplate(const PresetInput& in, TouchTemplate& t);
// The talent tree (TalentTab id) with the most points among the known spells (ties: the first in the talent
// frame's order), 0 when none has any.
u32 topTalentTree(const PresetInput& in);

// The spells a character of `cls` and `race` would know at `level` among those the presets use (their first ranks,
// by Spell.dbc's level and SkillLineAbility's race and class), its forms and Attack, with `talentTab`'s talents
// (0: none) spent from the tree's top row down, one point a level from 10 (VD_TOUCH_DEMO and the tests).
// Allocates (out).
void classDemoKnown(u32 cls, u32 race, u32 level, u32 talentTab, const AbilityData& abilities,
                    std::vector<u32>& out);
// The talent tree (TalentTab id) of `cls` whose name starts with `name` (any case, spaces ignored: "arms", "feral",
// "beastmastery"), 0 if none.
u32 talentTreeByName(u32 cls, std::string_view name, const AbilityData& abilities);

// Phase 20 (assists, §5.3 / §8): the healing presets ("Heal" of the druid, priest, paladin and shaman), whose
// templates bring the party frames beside the cluster (TouchTemplate::healerFrames).
bool presetRoleHeals(PresetRole role);

} // namespace vd::game
