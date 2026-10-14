// Phase 15 (items): the bags as ContainerFrames (app/ContainerLayout): ToggleBag / ToggleBackpack /
// OpenAllBags / CloseAllBags / ToggleKeyRing as ContainerFrame.lua runs them, GetKeyRingSize, each frame's
// background pieces and item buttons as ContainerFrame_GenerateFrame builds them,
// updateContainerFrameAnchors' stacking and shrinking; and where an item's tooltip and its comparison
// tooltips go (app/TooltipLayout).
#include "app/ContainerLayout.h"
#include "app/TooltipLayout.h"

#include <doctest/doctest.h>

#include <vector>

namespace {

using namespace vd;

ContainerSizes sizes() {
    ContainerSizes s;
    s.bags = {16, 6, 10, 0, 16}; // the third bag slot empty
    s.keyring = 4;
    return s;
}

std::vector<i32> order(const ContainerStack& stack) {
    return {stack.order().begin(), stack.order().end()};
}

bool near(f32 a, f32 b) {
    return a > b - 0.01f && a < b + 0.01f;
}

} // namespace

TEST_CASE("ContainerFrames: GetKeyRingSize") {
    CHECK(keyringSize(1) == 4);
    CHECK(keyringSize(39) == 4);
    CHECK(keyringSize(40) == 8);
    CHECK(keyringSize(50) == 12);
    CHECK(keyringSize(60) == 12);
    CHECK(keyringSize(61) == 16);
}

TEST_CASE("ContainerFrames: the bag bar's toggles") {
    const ContainerSizes s = sizes();
    ContainerStack stack;
    stack.toggleBag(2, s);
    stack.toggleBag(0, s);
    stack.toggleBag(3, s); // no bag there: nothing
    CHECK(order(stack) == std::vector<i32>{2, 0});
    stack.toggleKeyring(s);
    CHECK(order(stack) == std::vector<i32>{2, 0, kKeyringContainer});
    stack.toggleBag(2, s); // closing one closes up the rest
    CHECK(order(stack) == std::vector<i32>{0, kKeyringContainer});
    // ToggleBackpack: shown, every frame closes (the keyring too); else the backpack opens.
    stack.toggleBackpack(s);
    CHECK_FALSE(stack.any());
    stack.toggleBackpack(s);
    CHECK(order(stack) == std::vector<i32>{0});
}

TEST_CASE("ContainerFrames: OpenAllBags and CloseAllBags") {
    const ContainerSizes s = sizes();
    ContainerStack stack;
    stack.toggleKeyring(s);
    stack.toggleBag(4, s);
    stack.openAllBags(s); // not all open: everything closes (the keyring too), then the backpack and the bags
    CHECK(order(stack) == std::vector<i32>{0, 1, 2, 4});
    stack.openAllBags(s); // all open: all close
    CHECK_FALSE(stack.any());
    stack.toggleBag(1, s);
    stack.openAllBags(s, true);
    CHECK(order(stack) == std::vector<i32>{0, 1, 2, 4});
    stack.openAllBags(s, true); // forced: open again even when all were
    CHECK(order(stack) == std::vector<i32>{0, 1, 2, 4});
    // The keyring does not count as a bag, and CloseAllBags leaves it.
    stack.toggleKeyring(s);
    stack.closeAllBags();
    CHECK(order(stack) == std::vector<i32>{kKeyringContainer});
    stack.closeAll();
    CHECK_FALSE(stack.any());
}

TEST_CASE("ContainerFrames: raising a frame, BAG_OPEN of an open one") {
    ContainerSizes s;
    s.bags = {16, 6, 6, 6, 6};
    s.keyring = 4;
    ContainerStack stack;
    stack.openAllBags(s);
    stack.toggleKeyring(s);
    REQUIRE(stack.order().size() == 6);
    stack.raise(0); // the backpack becomes the newest
    CHECK(order(stack) == std::vector<i32>{1, 2, 3, 4, kKeyringContainer, 0});
    stack.raise(3);
    CHECK(order(stack) == std::vector<i32>{1, 2, 4, kKeyringContainer, 0, 3});
    stack.openBag(1, s); // BAG_OPEN of a shown bag: nothing
    CHECK(stack.order().size() == 6);
}

TEST_CASE("ContainerFrames: out of room, the oldest and the newest show") {
    const std::vector<f32> heights = {240.0f, 216.0f, 216.0f, 114.0f};
    std::vector<u32> shown(4);
    // Room for two columns of 192 (240 + 216 > 300): the backpack and the newest.
    CHECK(shownContainers(heights, {0.0f, 0.0f, 400.0f, 300.0f}, 1.0f, shown) == 2);
    CHECK(shown[0] == 0);
    CHECK(shown[1] == 3);
    // Three columns: the backpack, the next one, the newest.
    CHECK(shownContainers(heights, {0.0f, 0.0f, 600.0f, 300.0f}, 1.0f, shown) == 3);
    CHECK(shown[1] == 1);
    CHECK(shown[2] == 3);
    // Room for all.
    CHECK(shownContainers(heights, {0.0f, 0.0f, 800.0f, 300.0f}, 1.0f, shown) == 4);
    // No room at all: the newest still shows.
    CHECK(shownContainers(heights, {0.0f, 0.0f, 10.0f, 10.0f}, 1.0f, shown) == 1);
    CHECK(shown[0] == 3);
}

TEST_CASE("ContainerFrames: ContainerFrame_GenerateFrame's backgrounds") {
    const ContainerGeometry backpack = containerGeometry(0, 16);
    CHECK(backpack.art == ContainerArt::Backpack);
    CHECK(backpack.height == 240.0f);
    CHECK(backpack.firstItemBottom == 30.0f);
    // Six slots: the "+2" top (72), one middle row (41 - 9), the bottom (10).
    const ContainerGeometry six = containerGeometry(1, 6);
    CHECK(six.art == ContainerArt::Bag);
    REQUIRE(six.pieceCount == 3);
    CHECK(six.pieces[0].h == 72.0f);
    CHECK(near(six.pieces[0].v0, 0.189453125f));
    CHECK(six.pieces[1].h == 32.0f);
    CHECK(six.pieces[2].h == 10.0f);
    CHECK(six.height == 114.0f);
    // Four slots: the one-row top and the bottom.
    const ContainerGeometry four = containerGeometry(2, 4);
    REQUIRE(four.pieceCount == 2);
    CHECK(four.pieces[0].h == 86.0f);
    CHECK(four.height == 96.0f);
    // Sixteen: the full top, three rows of middle.
    const ContainerGeometry sixteen = containerGeometry(3, 16);
    CHECK(sixteen.height == 94.0f + 3.0f * 41.0f - 9.0f + 10.0f);
    // The largest bank bag: two middles, the first one six rows (plus the original's fraction).
    const ContainerGeometry big = containerGeometry(6, 36);
    CHECK(big.art == ContainerArt::Bank);
    REQUIRE(big.pieceCount == 4);
    CHECK(near(big.pieces[1].h, 6.0f * 41.0f + 0.353515625f));
    CHECK(big.pieces[2].h == 2.0f * 41.0f - 9.0f);
    CHECK(containerGeometry(kKeyringContainer, 4).art == ContainerArt::Keyring);
}

TEST_CASE("ContainerFrames: the item buttons, from the bottom right") {
    const ContainerGeometry six = containerGeometry(1, 6);
    // The last slot at the bottom right (BOTTOMRIGHT -12, 9), the first two up top on the right.
    const ui::Rect last = containerItemRect(six, 5);
    CHECK(last.x == 192.0f - 12.0f - 37.0f);
    CHECK(last.y == six.height - 9.0f - 37.0f);
    const ui::Rect first = containerItemRect(six, 0);
    const ui::Rect second = containerItemRect(six, 1);
    CHECK(second.x == last.x);          // the top row is right-aligned
    CHECK(first.x == second.x - 42.0f); // 37 + 5 apart
    CHECK(first.y == last.y - 41.0f);   // 37 + 4 apart
    const ui::Rect third = containerItemRect(six, 2);
    CHECK(third.x == last.x - 3.0f * 42.0f); // the bottom row's leftmost
    const ContainerGeometry backpack = containerGeometry(0, 16);
    CHECK(containerItemRect(backpack, 15).y == 240.0f - 30.0f - 37.0f);
}

TEST_CASE("ContainerFrames: updateContainerFrameAnchors") {
    const ui::Rect area{0.0f, 0.0f, 800.0f, 500.0f};
    const std::vector<f32> heights = {240.0f, 114.0f, 216.0f, 96.0f};
    std::vector<ui::Rect> out(heights.size());
    CHECK(layoutContainers(heights, area, 1.0f, out) == 4);
    // The backpack at the bottom right, the next one on top of it.
    CHECK(out[0].x == 800.0f - 192.0f);
    CHECK(out[0].bottom() == 500.0f);
    CHECK(out[1].x == out[0].x);
    CHECK(out[1].bottom() == out[0].y);
    // 240 + 3 + 114 + 3 leave 140: the 216 one starts a column to the left, at the bottom again.
    CHECK(out[2].x == 800.0f - 2.0f * 192.0f);
    CHECK(out[2].bottom() == 500.0f);
    CHECK(out[3].x == out[2].x);
    CHECK(out[3].bottom() == out[2].y);
    // Too narrow for the second column: two fit.
    CHECK(layoutContainers(heights, {500.0f, 0.0f, 300.0f, 500.0f}, 1.0f, out) == 2);
}

TEST_CASE("ContainerFrames: shrunk by CONTAINER_SCALE steps until they fit") {
    const std::vector<f32> heights = {240.0f, 216.0f, 216.0f};
    const ui::Rect area{0.0f, 0.0f, 300.0f, 480.0f};
    // At 1.0 the third needs a second column (384 wide).
    std::vector<ui::Rect> out(3);
    CHECK(layoutContainers(heights, area, 1.0f, out) == 2);
    const f32 scale = containerScale(heights, area, 1.0f, 0.5f);
    CHECK(scale < 1.0f);
    CHECK(layoutContainers(heights, area, scale, out) == 3);
    CHECK(near(scale, 0.729f)); // three steps of 0.9
    // Never below the smallest.
    CHECK(containerScale(heights, {0.0f, 0.0f, 50.0f, 50.0f}, 1.0f, 0.6f) == 0.6f);
}

TEST_CASE("Comparison tooltips: beside the item's tooltip") {
    const ui::Rect safe{0.0f, 0.0f, 1200.0f, 800.0f};
    const f32 heights[] = {200.0f, 150.0f};
    SUBCASE("room on the right: the tooltip right of the cell, the comparisons right of it") {
        const TooltipPlacement p =
            placeTooltips(safe, {50.0f, 100.0f, 40.0f, 40.0f}, 300.0f, 250.0f, heights, 8.0f, 10.0f);
        CHECK(p.main.x == 98.0f);
        CHECK(p.main.y == 100.0f);
        CHECK(p.compare[0].x == p.main.right());
        CHECK(p.compare[0].y == 110.0f);
        CHECK(p.compare[1].x == p.compare[0].right());
        CHECK(p.compare[1].y == 110.0f);
    }
    SUBCASE("a cell at the right edge: mirrored to the left") {
        const TooltipPlacement p =
            placeTooltips(safe, {1100.0f, 100.0f, 40.0f, 40.0f}, 300.0f, 250.0f, heights, 8.0f, 10.0f);
        CHECK(p.main.right() == 1092.0f);
        CHECK(p.compare[0].right() == p.main.x);
        CHECK(p.compare[1].right() == p.compare[0].x);
    }
    SUBCASE("no room for three across: the second comparison under the first") {
        const ui::Rect narrow{0.0f, 0.0f, 800.0f, 800.0f};
        const TooltipPlacement p =
            placeTooltips(narrow, {20.0f, 100.0f, 40.0f, 40.0f}, 300.0f, 250.0f, heights, 8.0f, 10.0f);
        CHECK(p.compare[0].x == p.main.right());
        CHECK(p.compare[1].x == p.compare[0].x);
        CHECK(p.compare[1].y == p.compare[0].bottom());
        CHECK(p.compare[1].right() <= 792.0f);
    }
    SUBCASE("inside the safe area at the bottom") {
        const TooltipPlacement p =
            placeTooltips(safe, {50.0f, 700.0f, 40.0f, 40.0f}, 300.0f, 250.0f, heights, 8.0f, 10.0f);
        CHECK(p.main.bottom() <= 792.0f);
        CHECK(p.compare[0].bottom() <= 792.0f);
    }
    SUBCASE("without comparisons: as before") {
        const TooltipPlacement p =
            placeTooltips(safe, {50.0f, 100.0f, 40.0f, 40.0f}, 300.0f, 250.0f, {}, 8.0f, 10.0f);
        CHECK(p.main.x == 98.0f);
    }
}
