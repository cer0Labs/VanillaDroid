#pragma once

#include "core/Types.h"

#include <array>
#include <span>

namespace vd {

// Retouches (windows): the prerequisite lines of a talent tree as 1.12's TalentFrame.lua lays them out
// (TALENT_BRANCH_ARRAY, TalentFrame_DrawLines, TalentFrame_Update): every cell of the grid knows which way a
// line leaves it (+1 lit, -1 grey, 0 none) and which arrows point into its talent. Pure: app/CombatWindows
// draws the cells with UI-TalentBranches and UI-TalentArrows (TALENT_BRANCH_TEXTURECOORDS,
// TALENT_ARROW_TEXTURECOORDS).

constexpr u32 kTalentColumns = 4; // NUM_TALENT_COLUMNS
constexpr u32 kTalentTiers = 8;   // MAX_NUM_TALENT_TIERS

struct TalentBranchCell {
    bool talent = false; // a talent stands there (its id in TALENT_BRANCH_ARRAY)
    i8 up = 0, down = 0, left = 0, right = 0;
    i8 topArrow = 0, leftArrow = 0, rightArrow = 0;
};

struct TalentBranchGrid {
    std::array<TalentBranchCell, kTalentTiers * kTalentColumns> cells{};

    TalentBranchCell& at(u32 tier, u32 column) { return cells[tier * kTalentColumns + column]; }
    const TalentBranchCell& at(u32 tier, u32 column) const { return cells[tier * kTalentColumns + column]; }
};

// A talent of the tree (0-based tier and column) and the one it needs.
struct TalentNode {
    u32 tier = 0;
    u32 column = 0;
    i32 prerequisite = -1; // index into the same list, -1: none
    bool lit = false;      // its requirements met (TalentFrame_SetPrereqs): its line lit, else grey
};

// TalentFrame_ResetBranches, the talents' cells, then each talent's line from its prerequisite in list order
// (TalentFrame_DrawLines: straight down, straight across, or across the prerequisite's tier and down; when a
// talent blocks that tier, down the prerequisite's column and across into the talent). Talents outside the
// grid are skipped.
void layoutTalentBranches(std::span<const TalentNode> talents, TalentBranchGrid& out);

// What TalentFrame_Update draws in a cell without a talent, from its directions (in its order of checks).
enum class TalentJoint : u8 {
    None,        // nothing (a lone direction)
    TeeUp,       // "tup": across, and up
    TeeDown,     // "tdown": across, and down
    TopRight,    // "topright": from the left, turning down
    BottomRight, // "bottomright": from the left, turning up
    TopLeft,     // "topleft": from the right, turning down
    BottomLeft,  // "bottomleft": from the right, turning up
    Across,      // left and right: straight
    Through,     // up and down: straight
};
TalentJoint talentJoint(const TalentBranchCell& cell);

} // namespace vd
