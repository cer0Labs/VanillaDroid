// Retouches (quest log): QuestLogFrame drawn with its 1.12 art cut into pieces (app/QuestLogLayout): unstretched it is
// the original picture, piece by piece; stretched (the original's arrangement in a tall place, our own arrangement in
// a wide one) its pieces cover the frame and its controls keep the places QuestLogFrame.xml gives them. Retouches
// (quest windows): QuestFrame and GossipFrame at their art's proportions (app/QuestFrameLayout).
#include "app/QuestFrameLayout.h"
#include "app/QuestLogLayout.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

namespace {

using namespace vd;

// The textures' sizes and where they sit in the frame's 384 x 512 picture (and EmptyQuestLogFrame's 320 x 384).
struct TextureSpot {
    f32 x, y, w, h;
};
constexpr TextureSpot kSpots[kQuestLogTextures] = {
    {0.0f, 0.0f, 256.0f, 256.0f},   {256.0f, 0.0f, 128.0f, 256.0f}, {0.0f, 256.0f, 256.0f, 256.0f},
    {256.0f, 256.0f, 128.0f, 256.0f}, {0.0f, 0.0f, 256.0f, 256.0f},   {256.0f, 0.0f, 64.0f, 256.0f},
    {0.0f, 256.0f, 256.0f, 128.0f}, {256.0f, 256.0f, 64.0f, 128.0f},
};

bool inside(const ui::Rect& inner, const ui::Rect& outer, f32 slack = 0.01f) {
    return inner.x >= outer.x - slack && inner.y >= outer.y - slack && inner.right() <= outer.right() + slack &&
           inner.bottom() <= outer.bottom() + slack;
}

// Whether the frame's pieces cover every point of a grid over it.
bool covers(const QuestLogLayout& l, f32 step) {
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

} // namespace

TEST_CASE("Quest log art: unstretched, the tall arrangement is the original picture piece by piece") {
    // The art's visible 346 x 462 units at one pixel each, the list its original 96 of the 364 between the header and
    // the buttons' bar.
    const ui::Rect place{100.0f, 50.0f, 346.0f, 462.0f};
    const QuestLogLayout l = questLogLayout(place, 1.0f, false, 96.0f / 364.0f);
    CHECK_FALSE(l.wide);
    CHECK(l.k == doctest::Approx(1.0f));
    CHECK(l.frame.w == doctest::Approx(346.0f));
    CHECK(l.frame.h == doctest::Approx(462.0f));
    REQUIRE(l.pieceCount > 0);
    for (u32 i = 0; i < l.pieceCount; ++i) {
        const QuestLogPiece& p = l.pieces[i];
        const u32 t = static_cast<u32>(p.texture);
        REQUIRE(t < 4);
        // Where its texels are in the picture is where it is drawn (the frame's 0 is the picture's 3, 2).
        const TextureSpot& s = kSpots[t];
        CHECK(p.dst.x - place.x == doctest::Approx(s.x + p.uv.u0 * s.w - 3.0f).epsilon(0.001));
        CHECK(p.dst.y - place.y == doctest::Approx(s.y + p.uv.v0 * s.h - 2.0f).epsilon(0.001));
        CHECK(p.dst.w == doctest::Approx((p.uv.u1 - p.uv.u0) * s.w).epsilon(0.001));
        CHECK(p.dst.h == doctest::Approx((p.uv.v1 - p.uv.v0) * s.h).epsilon(0.001));
    }
    CHECK(covers(l, 4.0f));
    // QuestLogFrame.xml's places: the portrait (4, -4), the close button (TOPRIGHT -30, -8), the All button (70,
    // -50), the radio (129, -44), the count's box (TOPRIGHT -47, -41), the list (19, -75), the detail 7 under it, the
    // buttons.
    CHECK(l.portrait.x == doctest::Approx(place.x + 1.0f));
    CHECK(l.portrait.y == doctest::Approx(place.y + 2.0f));
    CHECK(l.close.x == doctest::Approx(place.x + 319.0f));
    CHECK(l.expand.x == doctest::Approx(place.x + 67.0f));
    CHECK(l.track.x == doctest::Approx(place.x + 126.0f));
    CHECK(l.countRight == doctest::Approx(place.x + 334.0f));
    CHECK(l.list.x == doctest::Approx(place.x + 16.0f));
    CHECK(l.list.y == doctest::Approx(place.y + 73.0f));
    CHECK(l.list.w == doctest::Approx(300.0f));
    CHECK(l.detail.y == doctest::Approx(place.y + 173.0f));
    CHECK(l.abandon.x == doctest::Approx(place.x + 14.0f));
    CHECK(l.abandon.y == doctest::Approx(place.y + 435.0f));
    CHECK(l.share.x == doctest::Approx(place.x + 138.0f));
    CHECK(l.exit.x == doctest::Approx(place.x + 261.0f));
    CHECK(l.share.right() == doctest::Approx(l.exit.x));
    // The title at the 384-unit frame's middle.
    CHECK(l.titleCenterX == doctest::Approx(place.x + 189.0f));
    // The scroll bars 6 right of the scroll frames, in the art's columns.
    CHECK(l.listBar.x == doctest::Approx(l.list.right() + 6.0f));
    CHECK(l.detailBar.x == doctest::Approx(l.detail.right() + 6.0f));
}

TEST_CASE("Quest log art: the wide arrangement in a wide place") {
    // A 20:9 phone's place (logical pixels) at the scale of 20-pixel buttons.
    const ui::Rect place{6.0f, 6.0f, 700.0f, 500.0f};
    const f32 k = questLogScale(20.0f);
    REQUIRE(questLogWide(place, k));
    const QuestLogLayout l = questLogLayout(place, k, true, 0.5f);
    CHECK(l.wide);
    CHECK(l.frame.x == doctest::Approx(place.x));
    CHECK(l.frame.y == doctest::Approx(place.y));
    CHECK(l.frame.w == doctest::Approx(place.w));
    CHECK(l.frame.h == doctest::Approx(place.h));
    CHECK(l.pieceCount <= l.pieces.size());
    CHECK(covers(l, 3.0f));
    for (u32 i = 0; i < l.pieceCount; ++i) {
        CHECK(inside(l.pieces[i].dst, l.frame));
        CHECK(static_cast<u32>(l.pieces[i].texture) < 4);
    }
    // The list at the left, the parchment right of it, its scroll bar in the column at the frame's right edge.
    CHECK(l.list.right() < l.detail.x);
    CHECK(l.detail.right() < l.detailBar.x);
    CHECK(l.detailBar.right() < l.frame.right());
    CHECK(std::abs(l.list.w - l.detail.w) < 0.1f * l.frame.w); // about half each
    // A phone's place over a stance bar: the parchment keeps its rewards' two columns (2 x 147 + 1 units of the
    // original's 300), the list what is left.
    const QuestLogLayout narrow = questLogLayout({6.0f, 6.0f, 600.0f, 422.0f}, k, true, 0.5f);
    CHECK(narrow.detail.w >= 295.0f * narrow.k);
    CHECK(narrow.list.w >= 180.0f * narrow.k);
    CHECK(inside(l.listBar, l.list));
    // The header, the content and the buttons' bar one under the other.
    CHECK(l.track.bottom() <= l.list.y);
    CHECK(l.list.bottom() <= l.abandon.y);
    CHECK(l.detail.bottom() <= l.exit.y);
    // The buttons in their slots: Abandon at the left, Share against Exit at the right.
    CHECK(l.abandon.x == doctest::Approx(place.x + 14.0f * k));
    CHECK(l.share.right() == doctest::Approx(l.exit.x));
    CHECK(l.exit.right() == doctest::Approx(l.frame.right() - 8.0f * k));
    CHECK(l.abandon.h == doctest::Approx(20.0f));
    // The title in the middle of the title bar (the frame's TOP: the 384-unit frame's middle when unstretched).
    CHECK(l.titleCenterX == doctest::Approx(l.frame.x + ((l.frame.w / k + 38.0f) * 0.5f - 3.0f) * k));
    CHECK(std::abs(l.titleCenterX - l.frame.centerX()) < 20.0f * k);
    // The empty log's cobwebs over the list, its book over the parchment, from their own textures.
    REQUIRE(l.emptyPieceCount > 0);
    for (u32 i = 0; i < l.emptyPieceCount; ++i) {
        CHECK(static_cast<u32>(l.emptyPieces[i].texture) >= 4);
        CHECK(inside(l.emptyPieces[i].dst, l.frame));
    }
}

TEST_CASE("Quest log art: which places are wide, and the scale in small and big ones") {
    const f32 k = questLogScale(20.0f);
    CHECK(questLogWide({0.0f, 0.0f, 700.0f, 500.0f}, k));  // a phone held sideways
    CHECK(questLogWide({0.0f, 0.0f, 640.0f, 400.0f}, k));  // the cover screen
    CHECK_FALSE(questLogWide({0.0f, 0.0f, 530.0f, 600.0f}, k)); // a slot of the inner screen
    CHECK(questLogWide({0.0f, 0.0f, 600.0f, 422.0f}, k));  // a phone's over a stance bar
    CHECK_FALSE(questLogWide({0.0f, 0.0f, 440.0f, 380.0f}, k)); // too narrow for two columns

    // Smaller than the art's fixed parts: the scale shrinks so it fits.
    const ui::Rect small{0.0f, 0.0f, 300.0f, 240.0f};
    const QuestLogLayout s = questLogLayout(small, 1.0f, false, 0.4f);
    CHECK(s.k == doctest::Approx(std::min(300.0f / kQuestLogMinWidth, 240.0f / kQuestLogMinHeight)));
    CHECK(inside(s.frame, small));
    CHECK(covers(s, 2.0f));

    // A big place: the frame grows to its arrangement's most, from the place's top left.
    const ui::Rect big{10.0f, 20.0f, 3000.0f, 2000.0f};
    const QuestLogLayout w = questLogLayout(big, 1.0f, true, 0.5f);
    CHECK(w.frame.w == doctest::Approx(kQuestLogMaxWideWidth));
    CHECK(w.frame.h == doctest::Approx(kQuestLogMaxWideHeight));
    CHECK(w.frame.x == doctest::Approx(10.0f));
    const QuestLogLayout t = questLogLayout(big, 1.0f, false, 0.4f);
    CHECK(t.frame.w == doctest::Approx(kQuestLogMaxTallWidth));
    CHECK(t.frame.h == doctest::Approx(kQuestLogMaxTallHeight));
    CHECK(covers(t, 4.0f));
}

TEST_CASE("Quest frame art: QuestFrame.xml's places at its proportions") {
    // At one pixel a unit, in a place bigger than the art: at its top left, what the art shows of its 384 x 512.
    const ui::Rect place{10.0f, 20.0f, 500.0f, 600.0f};
    const QuestFrameLayout l = questFrameLayout(place, 1.0f, false);
    CHECK(l.k == doctest::Approx(1.0f));
    CHECK(l.frame.x == doctest::Approx(10.0f));
    CHECK(l.frame.y == doctest::Approx(20.0f));
    CHECK(l.frame.w == doctest::Approx(kQuestFrameWidth));
    CHECK(l.frame.h == doctest::Approx(kQuestFrameHeight));
    CHECK(l.art.x == doctest::Approx(8.0f)); // the picture's 2 left of what shows
    CHECK(l.art.w == doctest::Approx(384.0f));
    CHECK(l.art.h == doctest::Approx(512.0f));
    // The portrait (7, -6) 60 x 60; the close button centered at TOPRIGHT (-42, -31); the name in the 300-unit frame
    // at TOP (0, -23); the scroll frame (23, -81) 300 x 334 and its bar 6 right of it; the buttons at the bottom.
    CHECK(l.portrait.x == doctest::Approx(8.0f + 7.0f));
    CHECK(l.portrait.y == doctest::Approx(20.0f + 6.0f));
    CHECK(l.close.centerX() == doctest::Approx(8.0f + 342.0f));
    CHECK(l.close.centerY() == doctest::Approx(20.0f + 31.0f));
    CHECK(l.title.centerX() == doctest::Approx(8.0f + 192.0f));
    CHECK(l.scroll.x == doctest::Approx(8.0f + 23.0f));
    CHECK(l.scroll.y == doctest::Approx(20.0f + 81.0f));
    CHECK(l.scroll.w == doctest::Approx(300.0f));
    CHECK(l.scrollBar.x == doctest::Approx(l.scroll.right() + 6.0f));
    CHECK(l.scrollBar.h == doctest::Approx(334.0f));
    CHECK(l.leftButton.bottom() == doctest::Approx(20.0f + 512.0f - 72.0f));
    CHECK(l.rightButton.right() == doctest::Approx(8.0f + 384.0f - 39.0f));
    CHECK(l.accept.w == doctest::Approx(77.0f));
    CHECK(l.patch.bottom() == doctest::Approx(20.0f + 512.0f - 68.0f));
    // Everything it draws inside what shows.
    for (const ui::Rect& r : {l.portrait, l.scroll, l.scrollBar, l.leftButton, l.rightButton, l.accept}) {
        CHECK(inside(r, l.frame));
    }
    // A rewards' row fits its scroll frame: two 147-unit items 1 apart, from 2 in.
    CHECK(2.0f + 147.0f * 2.0f + 1.0f <= l.scroll.w);
}

TEST_CASE("Quest frame art: smaller places scale it down; over the world it is centered") {
    const f32 k = questLogScale(24.0f); // a 20:9 phone's buttons (logical pixels)
    const ui::Rect phone{6.0f, 6.0f, 413.0f, 511.0f}; // its left slot beside a stance bar
    const QuestFrameLayout l = questFrameLayout(phone, k, false);
    CHECK(l.k == doctest::Approx(std::min({k, 413.0f / kQuestFrameWidth, 511.0f / kQuestFrameHeight})));
    CHECK(inside(l.frame, phone));
    CHECK(l.frame.h >= 0.99f * 511.0f); // about as tall as the slot
    const ui::Rect screen{0.0f, 0.0f, 1200.0f, 540.0f};
    const QuestFrameLayout c = questFrameLayout(screen, k, true);
    CHECK(c.frame.centerX() == doctest::Approx(600.0f));
    CHECK(c.frame.centerY() == doctest::Approx(270.0f));
    CHECK(inside(c.frame, screen));
}
