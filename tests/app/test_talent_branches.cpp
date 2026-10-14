// Retouches (windows): the talent tree's prerequisite lines as TalentFrame.lua lays them out
// (TalentFrame_DrawLines into TALENT_BRANCH_ARRAY, TalentFrame_Update's pieces): straight down, down through
// an empty cell, across with the arrow on the side the line comes from, across the prerequisite's tier and
// down (its corner), a tee where two lines part, the blocked cases, and the lit / grey state each line takes
// from its talent (app/TalentBranches).
#include "app/TalentBranches.h"

#include <doctest/doctest.h>

#include <array>

namespace {

using namespace vd;

TalentBranchGrid layout(std::span<const TalentNode> nodes) {
    TalentBranchGrid grid;
    layoutTalentBranches(nodes, grid);
    return grid;
}

} // namespace

TEST_CASE("talent branches: straight down into the next tier") {
    const std::array<TalentNode, 2> nodes{{{0, 1, -1, true}, {1, 1, 0, true}}};
    const TalentBranchGrid g = layout(nodes);
    CHECK(g.at(0, 1).talent);
    CHECK(g.at(1, 1).talent);
    CHECK(g.at(0, 1).down == 1);
    // The talent itself gets the arrow, not an "up" (TalentFrame_DrawLines stops one tier short).
    CHECK(g.at(1, 1).up == 0);
    CHECK(g.at(1, 1).topArrow == 1);
    CHECK(talentJoint(g.at(0, 1)) == TalentJoint::None); // a talent's cell draws no joint
}

TEST_CASE("talent branches: down through an empty cell, grey") {
    const std::array<TalentNode, 2> nodes{{{0, 2, -1, false}, {2, 2, 0, false}}};
    const TalentBranchGrid g = layout(nodes);
    CHECK(g.at(0, 2).down == -1);
    CHECK(g.at(1, 2).up == -1);
    CHECK(g.at(1, 2).down == -1);
    CHECK(talentJoint(g.at(1, 2)) == TalentJoint::Through);
    CHECK(g.at(2, 2).topArrow == -1);
}

TEST_CASE("talent branches: a talent in between blocks a straight line") {
    const std::array<TalentNode, 3> nodes{{{0, 0, -1, true}, {1, 0, -1, true}, {2, 0, 0, true}}};
    const TalentBranchGrid g = layout(nodes);
    CHECK(g.at(0, 0).down == 0);
    CHECK(g.at(2, 0).topArrow == 0);
}

TEST_CASE("talent branches: across, the arrow on the prerequisite's side") {
    // The prerequisite on the left: the line runs right, the arrow on the talent's left.
    {
        const std::array<TalentNode, 2> nodes{{{4, 0, -1, true}, {4, 3, 0, true}}};
        const TalentBranchGrid g = layout(nodes);
        CHECK(g.at(4, 0).right == 1);
        CHECK(g.at(4, 1).left == 1);
        CHECK(g.at(4, 1).right == 1);
        CHECK(talentJoint(g.at(4, 1)) == TalentJoint::Across);
        CHECK(g.at(4, 3).left == 1);
        CHECK(g.at(4, 3).leftArrow == 1);
        CHECK(g.at(4, 3).rightArrow == 0);
    }
    // On the right: the arrow on the talent's right (rightArrow, the texture mirrored).
    {
        const std::array<TalentNode, 2> nodes{{{6, 2, -1, false}, {6, 1, 0, false}}};
        const TalentBranchGrid g = layout(nodes);
        CHECK(g.at(6, 1).right == -1);
        CHECK(g.at(6, 2).left == -1);
        CHECK(g.at(6, 1).rightArrow == -1);
    }
}

TEST_CASE("talent branches: across the prerequisite's tier, then down (a corner)") {
    const std::array<TalentNode, 2> nodes{{{2, 2, -1, true}, {3, 3, 0, true}}};
    const TalentBranchGrid g = layout(nodes);
    CHECK(g.at(2, 2).right == 1);
    CHECK(g.at(2, 3).left == 1);
    CHECK(g.at(2, 3).down == 1);
    CHECK(talentJoint(g.at(2, 3)) == TalentJoint::TopRight); // from the left, turning down
    CHECK(g.at(3, 3).up == 1);
    CHECK(g.at(3, 3).topArrow == 1);
    // Mirrored: from the right, turning down.
    const std::array<TalentNode, 2> mirrored{{{2, 2, -1, true}, {3, 1, 0, true}}};
    const TalentBranchGrid m = layout(mirrored);
    CHECK(talentJoint(m.at(2, 1)) == TalentJoint::TopLeft);
}

TEST_CASE("talent branches: two lines parting make a tee") {
    // From (4, 0): across to (4, 3), and across to column 2 then down to (5, 2).
    const std::array<TalentNode, 3> nodes{{{4, 0, -1, false}, {4, 3, 0, false}, {5, 2, 0, false}}};
    const TalentBranchGrid g = layout(nodes);
    CHECK(talentJoint(g.at(4, 2)) == TalentJoint::TeeDown);
    CHECK(talentJoint(g.at(4, 1)) == TalentJoint::Across);
    CHECK(g.at(5, 2).topArrow == -1);
    CHECK(g.at(4, 3).leftArrow == -1);
}

TEST_CASE("talent branches: a talent in the prerequisite's tier sends the line down first") {
    // (1, 1) needs (0, 0), but (0, 1) stands in tier 0: down column 0 to tier 1, then across into the talent.
    const std::array<TalentNode, 3> nodes{{{0, 0, -1, true}, {0, 1, -1, true}, {1, 1, 0, true}}};
    const TalentBranchGrid g = layout(nodes);
    CHECK(g.at(0, 0).down == 1);
    CHECK(g.at(1, 0).up == 1);
    CHECK(g.at(1, 0).right == 1);
    CHECK(talentJoint(g.at(1, 0)) == TalentJoint::BottomLeft); // from above, turning right
    CHECK(g.at(1, 1).left == 1);
    CHECK(g.at(1, 1).leftArrow == 1);
    CHECK(g.at(1, 1).topArrow == 0);
}

TEST_CASE("talent branches: what TalentFrame_Update draws in an empty cell") {
    TalentBranchCell c;
    CHECK(talentJoint(c) == TalentJoint::None);
    c.down = 1;
    CHECK(talentJoint(c) == TalentJoint::None); // a lone direction: nothing
    c.up = 1;
    c.left = 1;
    c.right = 1;
    CHECK(talentJoint(c) == TalentJoint::TeeUp); // up, left and right come first
    c = {};
    c.left = -1;
    c.up = -1;
    CHECK(talentJoint(c) == TalentJoint::BottomRight);
    c = {};
    c.right = 1;
    c.up = 1;
    CHECK(talentJoint(c) == TalentJoint::BottomLeft);
}

TEST_CASE("talent branches: out of the grid, or pointing up, nothing") {
    const std::array<TalentNode, 3> nodes{{{9, 0, -1, true}, {0, 5, 0, true}, {0, 0, 2, true}}};
    const TalentBranchGrid g = layout(nodes);
    for (const TalentBranchCell& c : g.cells) {
        CHECK(c.up == 0);
        CHECK(c.down == 0);
        CHECK(c.left == 0);
        CHECK(c.right == 0);
    }
    // A prerequisite below its talent draws nothing.
    const std::array<TalentNode, 2> upwards{{{1, 1, 1, true}, {3, 1, -1, true}}};
    const TalentBranchGrid u = layout(upwards);
    for (const TalentBranchCell& c : u.cells) {
        CHECK(c.up == 0);
        CHECK(c.down == 0);
        CHECK(c.topArrow == 0);
    }
}
