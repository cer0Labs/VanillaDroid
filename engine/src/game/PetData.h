#pragma once

#include "core/Types.h"
#include "game/DbcIndex.h"

#include <span>
#include <string_view>

namespace vd::game {

// Phase 15 (pets): the client tables behind 1.12's pet windows, as WoW.exe 1.12.1 reads them for its Lua
// functions (Interface\FrameXML's GetPetLoyalty, GetPetFoodTypes, GetPetIcon, GetPetHappiness,
// GetStablePetInfo, GetNextStableSlotCost; ROADMAP F15.5): CreatureFamily.dbc (a family's name, its diet mask
// and its icon), PetLoyalty.dbc (the loyalty levels' names), ItemPetFood.dbc (the diet's food names),
// StableSlotPrices.dbc (what each stable slot costs) and PetPersonality.dbc (the happiness thresholds).
// Indexes over the mapped db pack, built once; lookups read the mapped tables (no allocation).

// GetPetHappiness: the hunter pet's mood from its happiness (UNIT_FIELD_POWER5) and what it does to its
// damage.
struct PetHappiness {
    u32 level = 0;         // 1 unhappy, 2 content, 3 happy (0: unknown)
    f32 damagePercent = 0; // PET_DAMAGE_PERCENTAGE's "%d" (75, 100, 125)
};

class PetData {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return !m_families.empty(); }

    // CreatureFamily.dbc: the family's name ("Wolf"), its diet (bit n - 1 = ItemPetFood n) and its icon
    // (assetPathHash of "Interface\Icons\Ability_Hunter_Pet_Wolf.blp", 0 none).
    std::string_view familyName(u32 family) const;
    u32 familyFoodMask(u32 family) const;
    u64 familyIconHash(u32 family) const;
    // GetPetFoodTypes + BuildListString: the names of the foods of `foodMask` in ItemPetFood.dbc's order, ",
    // " between them, into `out` (cut to fit). Returns the text written (empty: none).
    std::string_view dietList(u32 foodMask, std::span<char> out) const;
    // PetLoyalty.dbc: "(Loyalty Level 3) Submissive" (empty when unknown).
    std::string_view loyaltyName(u32 level) const;
    // StableSlotPrices.dbc: what buying stable slot `slot` (1, 2) costs, in copper (0: no such slot).
    u32 stableSlotPrice(u32 slot) const;
    // GetPetHappiness for a happiness value (PetPersonality.dbc's first record, as WoW.exe falls back to).
    PetHappiness happiness(u32 power5) const;
    // ChrClasses.dbc: HasPetSpells' token of the player's class ("PET", "DEMON": the spell book's
    // PET_TYPE_<token> tab) and a class's file name ("WARRIOR": PaperDollStatTooltip's
    // <CLASS>_<STAT>_TOOLTIP).
    std::string_view petNameToken(u32 cls) const;
    std::string_view classFileName(u32 cls) const;

private:
    DbcTable m_families;
    DbcTable m_loyalty;
    DbcTable m_foods;
    DbcTable m_slotPrices;
    DbcTable m_personality;
    DbcTable m_classes;
};

} // namespace vd::game
