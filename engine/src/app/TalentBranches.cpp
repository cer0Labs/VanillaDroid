#include "app/TalentBranches.h"

#include <algorithm>

namespace vd {
namespace {

// TalentFrame_DrawLines: the line from the prerequisite at (tier, column) into the talent at (buttonTier,
// buttonColumn).
void drawLines(TalentBranchGrid& grid, u32 buttonTier, u32 buttonColumn, u32 tier, u32 column, bool met) {
    const i8 v = met ? 1 : -1;
    if (buttonColumn == column) {
        // Same column: down from the prerequisite, unless a talent stands between them.
        for (u32 i = tier + 1; i < buttonTier; ++i) {
            if (grid.at(i, buttonColumn).talent) {
                return; // "this layout is blocked vertically"
            }
        }
        for (u32 i = tier; i < buttonTier; ++i) {
            grid.at(i, buttonColumn).down = v;
            if (i + 1 <= buttonTier - 1) {
                grid.at(i + 1, buttonColumn).up = v;
            }
        }
        grid.at(buttonTier, buttonColumn).topArrow = v;
        return;
    }
    const u32 left = std::min(buttonColumn, column), right = std::max(buttonColumn, column);
    if (buttonTier == tier) {
        // Same tier: across, unless a talent stands between them; the arrow on the side the line comes from.
        for (u32 i = left + 1; i < right; ++i) {
            if (grid.at(tier, i).talent) {
                return; // "there's a blocker"
            }
        }
        for (u32 i = left; i < right; ++i) {
            grid.at(tier, i).right = v;
            grid.at(tier, i + 1).left = v;
        }
        if (buttonColumn < column) {
            grid.at(buttonTier, buttonColumn).rightArrow = v;
        } else {
            grid.at(buttonTier, buttonColumn).leftArrow = v;
        }
        return;
    }
    // Diagonal: across the prerequisite's tier to the talent's column, then down into it (the arrow on top).
    u32 first = left, last = right; // the prerequisite's own column is not checked
    if (first == column) {
        ++first;
    } else {
        --last;
    }
    bool blocked = false;
    for (u32 i = first; i <= last; ++i) {
        blocked = blocked || grid.at(tier, i).talent;
    }
    if (!blocked) {
        for (u32 i = tier; i < buttonTier; ++i) {
            grid.at(i, buttonColumn).down = v;
            grid.at(i + 1, buttonColumn).up = v;
        }
        for (u32 i = left; i < right; ++i) {
            grid.at(tier, i).right = v;
            grid.at(tier, i + 1).left = v;
        }
        grid.at(buttonTier, buttonColumn).topArrow = v;
        return;
    }
    // Blocked: down the prerequisite's column to the talent's tier, then across into it (the arrow on that
    // side). 1.12's code marks this column's up / down the other way round and draws no crossing (no 1.12
    // tree takes this path): here the line the comments there describe.
    first = left;
    last = right;
    if (first == buttonColumn) {
        ++first;
    } else {
        --last;
    }
    for (u32 i = first; i <= last; ++i) {
        if (grid.at(buttonTier, i).talent) {
            return; // "this layout is undrawable"
        }
    }
    for (u32 i = tier; i < buttonTier; ++i) {
        grid.at(i, column).down = v;
        grid.at(i + 1, column).up = v;
    }
    for (u32 i = left; i < right; ++i) {
        grid.at(buttonTier, i).right = v;
        grid.at(buttonTier, i + 1).left = v;
    }
    if (buttonColumn < column) {
        grid.at(buttonTier, buttonColumn).rightArrow = v;
    } else {
        grid.at(buttonTier, buttonColumn).leftArrow = v;
    }
}

bool inGrid(const TalentNode& t) {
    return t.tier < kTalentTiers && t.column < kTalentColumns;
}

} // namespace

void layoutTalentBranches(std::span<const TalentNode> talents, TalentBranchGrid& out) {
    out = TalentBranchGrid{};
    for (const TalentNode& t : talents) {
        if (inGrid(t)) {
            out.at(t.tier, t.column).talent = true;
        }
    }
    for (const TalentNode& t : talents) {
        if (!inGrid(t) || t.prerequisite < 0 || static_cast<usize>(t.prerequisite) >= talents.size()) {
            continue;
        }
        const TalentNode& p = talents[static_cast<usize>(t.prerequisite)];
        if (!inGrid(p) || p.tier > t.tier || (p.tier == t.tier && p.column == t.column)) {
            continue; // a line only runs down or across
        }
        drawLines(out, t.tier, t.column, p.tier, p.column, t.lit);
    }
}

TalentJoint talentJoint(const TalentBranchCell& c) {
    if (c.up != 0 && c.left != 0 && c.right != 0) {
        return TalentJoint::TeeUp;
    }
    if (c.down != 0 && c.left != 0 && c.right != 0) {
        return TalentJoint::TeeDown;
    }
    if (c.left != 0 && c.down != 0) {
        return TalentJoint::TopRight;
    }
    if (c.left != 0 && c.up != 0) {
        return TalentJoint::BottomRight;
    }
    if (c.left != 0 && c.right != 0) {
        return TalentJoint::Across;
    }
    if (c.right != 0 && c.down != 0) {
        return TalentJoint::TopLeft;
    }
    if (c.right != 0 && c.up != 0) {
        return TalentJoint::BottomLeft;
    }
    if (c.up != 0 && c.down != 0) {
        return TalentJoint::Through;
    }
    return TalentJoint::None;
}

} // namespace vd
