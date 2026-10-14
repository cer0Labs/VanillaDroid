#pragma once

#include "core/Types.h"
#include "game/AbilityData.h"
#include "game/Entities.h"
#include "game/Spells.h"

#include <span>

namespace vd::game {

// The character's progress as the talent, skill and reputation windows read it (phase 8): talent ranks from
// the known spells, what a point could buy, skill values from the update fields.

constexpr u32 kTalentPointsPerRow = 5; // a tree's row opens every 5 points spent in it (MAX_TALENT_RANK)
constexpr u32 kTalentMinLevel = 10;

// The talent's rank (0: not learned): the highest of its rank spells the player knows.
u32 talentRank(const TalentInfo& talent, const SpellBook& spells);
// Points spent in a tree: the ranks of its talents.
u32 treePoints(const AbilityData& data, u32 tabId, const SpellBook& spells);

struct TalentStatus {
    u32 rank = 0;
    u32 maxRank = 0;
    bool rowOpen = false;        // enough points in the tree for its row
    bool prerequisiteMet = true; // the talent it depends on has the rank it needs (and the spell)
    bool available = false;      // a point can go there now (free points, row, prerequisite, not full)
};
// As Player::LearnTalent decides it.
TalentStatus talentStatus(const AbilityData& data, const TalentInfo& talent, const SpellBook& spells,
                          u32 freePoints);

// One skill of the player (PLAYER_SKILL_INFO_1_1: three fields a skill, 128 skills).
struct SkillValue {
    u32 id = 0; // SkillLine.dbc
    u32 step = 0;
    u32 value = 0;
    u32 max = 0;
    i32 bonus = 0; // temporary + permanent
};
constexpr u32 kMaxSkills = 128;
// The player's skills in field order. Returns how many were written.
u32 readSkills(const Entity& player, std::span<SkillValue> out);

// The combo points on the player's combo target (PLAYER_FIELD_BYTES byte 1, PLAYER_FIELD_COMBO_TARGET); 0
// when the target is another unit.
u32 comboPoints(const Entity& player, u64 target);

} // namespace vd::game
