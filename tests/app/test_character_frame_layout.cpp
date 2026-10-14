// Retouches (character frame): CharacterFrame's art in its two arrangements (app/CharacterFrameLayout): tall,
// PaperDollFrame.xml's places; wide (not in 1.12), the slots two across a side with the numbers, the model and the
// resistances between them; the turned tabs along the right edge of both; the other pages' units and
// ReputationDetailFrame's place.
#include "app/CharacterFrameLayout.h"

#include <doctest/doctest.h>

#include <algorithm>

namespace {

using namespace vd;

bool inside(const ui::Rect& inner, const ui::Rect& outer, f32 slack = 0.01f) {
    return inner.x >= outer.x - slack && inner.y >= outer.y - slack && inner.right() <= outer.right() + slack &&
           inner.bottom() <= outer.bottom() + slack;
}

bool apart(const ui::Rect& a, const ui::Rect& b) {
    return a.right() <= b.x + 0.01f || b.right() <= a.x + 0.01f || a.bottom() <= b.y + 0.01f ||
           b.bottom() <= a.y + 0.01f;
}

// The art's pieces cover every point of a grid over the frame.
bool covered(const CharacterFrameLayout& l, f32 step) {
    for (f32 y = l.frame.y + step * 0.5f; y < l.frame.bottom(); y += step) {
        for (f32 x = l.frame.x + step * 0.5f; x < l.frame.right(); x += step) {
            bool hit = false;
            for (u32 i = 0; i < l.pieceCount && !hit; ++i) {
                hit = l.pieces[i].dst.contains(x, y);
            }
            if (!hit) {
                return false;
            }
        }
    }
    return true;
}

constexpr u8 kHead = 0, kNeck = 1, kShoulder = 2, kChest = 4, kWaist = 5, kLegs = 6, kFeet = 7, kWrist = 8, kHands = 9,
             kFinger0 = 10, kTrinket1 = 13, kBack = 14, kMainHand = 15, kOffHand = 16, kRanged = 17, kTabard = 18;

} // namespace

TEST_CASE("Character frame art: the tall arrangement is PaperDollFrame.xml's") {
    const CharacterFrameLayout l = characterFrameLayout({10.0f, 20.0f, 600.0f, 700.0f}, 1.0f, false, false);
    const f32 ax = 10.0f - 2.0f, ay = 20.0f;
    CHECK_FALSE(l.wide);
    CHECK(l.k == doctest::Approx(1.0f));
    CHECK(l.pieceCount == 4); // the four textures as they are
    CHECK(covered(l, 7.0f));
    CHECK(l.portrait.x == doctest::Approx(ax + 7.0f));
    CHECK(l.name.centerX() == doctest::Approx(ax + 198.0f)); // CharacterNameFrame's middle at CENTER (6, 232)
    CHECK(l.name.centerY() == doctest::Approx(ay + 24.0f));
    CHECK(l.level.y == doctest::Approx(l.name.bottom() + 6.0f));
    CHECK(l.close.centerX() == doctest::Approx(ax + 384.0f - 44.0f));
    CHECK(l.close.centerY() == doctest::Approx(ay + 25.0f));
    // CharacterHeadSlot at (21, -74), the left column 41 apart; CharacterHandsSlot at (305, -74); the weapons from
    // BOTTOMLEFT (122, 127) 5 apart; the ammo slot 15 right of the ranged one.
    CHECK(l.slots[kHead].x == doctest::Approx(ax + 21.0f));
    CHECK(l.slots[kHead].y == doctest::Approx(ay + 74.0f));
    CHECK(l.slots[kNeck].y == doctest::Approx(ay + 115.0f));
    CHECK(l.slots[kWrist].y == doctest::Approx(ay + 74.0f + 7.0f * 41.0f));
    CHECK(l.slots[kHands].x == doctest::Approx(ax + 305.0f));
    CHECK(l.slots[kTrinket1].y == doctest::Approx(ay + 74.0f + 7.0f * 41.0f));
    CHECK(l.slots[kMainHand].x == doctest::Approx(ax + 122.0f));
    CHECK(l.slots[kMainHand].y == doctest::Approx(ay + 512.0f - 127.0f));
    CHECK(l.slots[kOffHand].x == doctest::Approx(l.slots[kMainHand].right() + 5.0f));
    CHECK(l.slots[kCharacterAmmoCell].x == doctest::Approx(l.slots[kRanged].right() + 15.0f));
    CHECK(l.slots[kCharacterAmmoCell].w == doctest::Approx(27.0f));
    CHECK(l.cells[kHead].w == doctest::Approx(0.0f)); // the art has its cells
    // The model (65, -78) 233 x 224; the resistances (32 x 29) down from (265, -77); the numbers' boxes.
    CHECK(l.model.x == doctest::Approx(ax + 65.0f));
    CHECK(l.model.h == doctest::Approx(224.0f));
    CHECK(l.inspectModel.h == doctest::Approx(300.0f));
    CHECK(l.resistances[0].x == doctest::Approx(ax + 265.0f));
    CHECK(l.resistances[4].bottom() == doctest::Approx(ay + 77.0f + 145.0f));
    CHECK(l.statBoxes[0].x == doctest::Approx(ax + 67.0f));
    CHECK(l.statBoxes[0].y == doctest::Approx(ay + 291.0f));
    CHECK(l.statBoxes[1].x == doctest::Approx(ax + 182.0f));
    CHECK(l.statLines[0].y == doctest::Approx(ay + 294.0f));
    CHECK(l.statLines[6].x == doctest::Approx(ax + 189.0f)); // CharacterAttackFrame
    CHECK(l.statLines[9].y == doctest::Approx(ay + 336.0f)); // CharacterRangedAttackFrame
    // The ammo slot's frame around it and its arrow 22 left of its middle.
    CHECK(l.ammoFrame.centerX() == doctest::Approx(l.slots[kCharacterAmmoCell].centerX()));
    CHECK(l.ammoArrow.centerX() == doctest::Approx(l.slots[kCharacterAmmoCell].centerX() - 22.0f));
    // The tabs along the border; the close button above them; everything inside the frame.
    CHECK(l.tabs.x == doctest::Approx(l.frame.right() - 5.0f));
    CHECK(l.close.bottom() <= l.tabs.y);
    for (const ui::Rect& r : l.slots) {
        CHECK(inside(r, l.frame));
    }
    for (const ui::Rect& r : {l.model, l.statBoxes[0], l.statBoxes[1], l.statBoxes[2], l.body}) {
        CHECK(inside(r, l.frame));
    }
}

TEST_CASE("Character frame art: the wide arrangement, slots two across a side") {
    const CharacterFrameLayout l = characterFrameLayout({0.0f, 0.0f, 1200.0f, 300.0f}, 1.0f, true, false);
    CHECK(l.wide);
    CHECK(l.frame.w == doctest::Approx(kCharacterWideRight - 2.0f));
    CHECK(l.frame.h == doctest::Approx(kCharacterWideHeight));
    CHECK(covered(l, 7.0f)); // the plain frame's nine pieces cover it
    // The left column two across: head, neck, shoulders and back over the main hand; chest, shirt, tabard and wrists
    // over the off hand. The right one: hands, waist, legs and feet over the ranged slot; rings and trinkets over the
    // ammo slot.
    CHECK(l.slots[kNeck].x == doctest::Approx(l.slots[kHead].x));
    CHECK(l.slots[kNeck].y == doctest::Approx(l.slots[kHead].y + 41.0f));
    CHECK(l.slots[kBack].y == doctest::Approx(l.slots[kHead].y + 3.0f * 41.0f));
    CHECK(l.slots[kMainHand].y == doctest::Approx(l.slots[kHead].y + 4.0f * 41.0f));
    CHECK(l.slots[kMainHand].x == doctest::Approx(l.slots[kHead].x));
    CHECK(l.slots[kChest].x == doctest::Approx(l.slots[kHead].x + 42.0f));
    CHECK(l.slots[kChest].y == doctest::Approx(l.slots[kHead].y));
    CHECK(l.slots[kTabard].y == doctest::Approx(l.slots[kShoulder].y));
    CHECK(l.slots[kOffHand].x == doctest::Approx(l.slots[kChest].x));
    CHECK(l.slots[kOffHand].y == doctest::Approx(l.slots[kMainHand].y));
    CHECK(l.slots[kWaist].x == doctest::Approx(l.slots[kHands].x));
    CHECK(l.slots[kFeet].y == doctest::Approx(l.slots[kHands].y + 3.0f * 41.0f));
    CHECK(l.slots[kRanged].x == doctest::Approx(l.slots[kHands].x));
    CHECK(l.slots[kRanged].y == doctest::Approx(l.slots[kMainHand].y));
    CHECK(l.slots[kFinger0].x == doctest::Approx(l.slots[kHands].x + 42.0f));
    CHECK(l.slots[kCharacterAmmoCell].x == doctest::Approx(l.slots[kRanged].right() + 15.0f));
    CHECK(l.slots[kCharacterAmmoCell].centerY() == doctest::Approx(l.slots[kRanged].centerY()));
    // Every slot but the ammo one in its riveted cell, 9 in from the cell's top left.
    for (u32 i = 0; i < kCharacterCells; ++i) {
        if (i == kCharacterAmmoCell) {
            CHECK(l.cells[i].w == doctest::Approx(0.0f));
            continue;
        }
        CHECK(l.cells[i].x == doctest::Approx(l.slots[i].x - 9.0f));
        CHECK(l.cells[i].y == doctest::Approx(l.slots[i].y - 9.0f));
        CHECK(inside(l.cells[i], l.frame));
    }
    // Between the two blocks the numbers (one box under another), the model and the resistances (the model in the
    // middle).
    const f32 leftBlock = l.cells[kChest].right(), rightBlock = l.cells[kHands].x;
    for (const ui::Rect& r : l.statBoxes) {
        CHECK(r.x >= leftBlock);
        CHECK(r.right() <= l.model.x);
    }
    CHECK(l.statBoxes[1].y >= l.statBoxes[0].bottom());
    CHECK(l.statBoxes[2].y >= l.statBoxes[1].bottom());
    CHECK(l.model.right() <= l.resistances[0].x);
    for (const ui::Rect& r : l.resistances) {
        CHECK(r.right() <= rightBlock);
    }
    CHECK(l.ammoFrame.right() <= l.tabs.x);
    for (const ui::Rect& line : l.statLines) {
        bool inBox = false;
        for (const ui::Rect& box : l.statBoxes) {
            inBox = inBox || inside(line, box, 2.0f);
        }
        CHECK(inBox);
    }
    // The title bar's middle moves with its stretch; the close button stays in its notch above the tabs.
    CHECK(l.name.centerX() == doctest::Approx(-2.0f + 198.0f + (kCharacterWideRight - 350.0f) * 0.5f));
    CHECK(l.close.centerX() == doctest::Approx(-2.0f + 340.0f + kCharacterWideRight - 350.0f)); // its notch, moved
    CHECK(l.close.bottom() <= l.tabs.y);
    CHECK(l.tabs.x == doctest::Approx(l.frame.right() - 5.0f));
    CHECK(l.tabs.bottom() <= l.frame.bottom());
    for (const ui::Rect& r : {l.model, l.body, l.ammoFrame, l.ammoArrow}) {
        CHECK(inside(r, l.frame));
    }
    for (u32 i = 0; i < kCharacterCells; ++i) {
        for (u32 j = i + 1; j < kCharacterCells; ++j) {
            CHECK(apart(l.slots[i], l.slots[j]));
        }
    }
}

TEST_CASE("Character frame art: the other pages' units and ReputationDetailFrame's place") {
    const CharacterFrameLayout l = characterFrameLayout({10.0f, 20.0f, 600.0f, 700.0f}, 1.0f, false, false);
    // at(): the FrameXML's TOPLEFT offsets from the picture's top left (ReputationListScrollFrame's (22, -76)).
    const ui::Rect list = l.at(22.0f, 76.0f, 296.0f, 354.0f);
    CHECK(list.x == doctest::Approx(8.0f + 22.0f));
    CHECK(list.y == doctest::Approx(20.0f + 76.0f));
    CHECK(inside(list, l.frame));
    CHECK(l.art.w == doctest::Approx(384.0f));
    // ReputationDetailFrame (212 x 203, 28 under the top) right of the tabs where it fits...
    const ui::Rect beside = characterSideFrame(l, {0.0f, 0.0f, 800.0f, 700.0f}, 28.0f, 212.0f, 203.0f);
    CHECK(beside.x >= l.tabs.right());
    CHECK(beside.y == doctest::Approx(20.0f + 28.0f));
    CHECK(beside.h == doctest::Approx(203.0f));
    // ...else as far right as the bounds let it, over the frame.
    const ui::Rect over = characterSideFrame(l, {0.0f, 0.0f, 500.0f, 700.0f}, 28.0f, 212.0f, 203.0f);
    CHECK(over.right() == doctest::Approx(500.0f));
    CHECK(over.w == doctest::Approx(212.0f));
}

TEST_CASE("Character frame art: each place gets the arrangement that shows bigger") {
    const f32 k = characterFrameScale(24.0f);
    CHECK(characterFrameWide({0.0f, 0.0f, 900.0f, 380.0f}, k));        // a 20:9 phone's left slot
    CHECK_FALSE(characterFrameWide({0.0f, 0.0f, 520.0f, 700.0f}, k)); // the inner screen's
    const ui::Rect phone{6.0f, 6.0f, 900.0f, 380.0f};
    const CharacterFrameLayout w = characterFrameLayout(phone, k, true, false);
    CHECK(inside(w.frame, phone));
    CHECK(inside(w.tabs, phone));
    CHECK(w.k == doctest::Approx(std::min({k, 900.0f / (kCharacterWideRight - 5.0f + kCharacterTabsWidth - 2.0f),
                                           380.0f / kCharacterWideHeight})));
    const ui::Rect screen{0.0f, 0.0f, 1200.0f, 540.0f};
    const CharacterFrameLayout c = characterFrameLayout(screen, k, true, true);
    CHECK(c.frame.centerY() == doctest::Approx(270.0f));
    CHECK(inside(c.frame, screen));
    CHECK(characterFrameLayout({0.0f, 0.0f, 0.0f, 10.0f}, k, true, false).pieceCount == 0);
}
