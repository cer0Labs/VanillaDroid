// Phase 18 (create): where CharacterCreate's frames go on the phones' screens (app/CharCreateLayout): the
// original's arrangement on the Fold5's inner screen, the two-column one on the wide screens (the Fold5's
// cover, 20:9 phones), finger-sized targets, the middle free for the character, nothing overlapping, and the
// name above the soft keyboard.
#include "app/CharCreateLayout.h"

#include <doctest/doctest.h>

#include <ostream>
#include <vector>

namespace vd::ui {
// For the checks' messages (found by argument-dependent lookup).
std::ostream& operator<<(std::ostream& os, const Rect& r) {
    return os << "{" << r.x << ", " << r.y << ", " << r.w << ", " << r.h << "}";
}
} // namespace vd::ui

namespace {

using namespace vd;
using ui::Rect;

bool inside(const Rect& r, const Rect& area) {
    return r.x >= area.x - 0.5f && r.y >= area.y - 0.5f && r.right() <= area.right() + 0.5f &&
           r.bottom() <= area.bottom() + 0.5f;
}

bool apart(const Rect& a, const Rect& b) {
    return a.right() <= b.x + 0.5f || b.right() <= a.x + 0.5f || a.bottom() <= b.y + 0.5f ||
           b.bottom() <= a.y + 0.5f;
}

CharCreateLayoutInput input(f32 w, f32 h, bool wide, f32 dp = 1.0f) {
    CharCreateLayoutInput in;
    in.area = {0.0f, 0.0f, w, h};
    in.dp = dp;
    in.wide = wide;
    in.classCount = 6;
    in.randomizeEm = 5.2f;
    return in;
}

// Every target a finger needs, the frames that must not overlap, the free middle.
void checkScreen(const CharCreateLayoutInput& in, f32 minTargetDp) {
    const CharCreateLayout l = charCreateLayout(in);
    const Rect& a = in.area;
    INFO("screen " << a.w << " x " << a.h << (l.wide ? " wide" : " square") << ", unit " << l.unit);
    std::vector<Rect> targets;
    for (u32 i = 0; i < 8; ++i) {
        targets.push_back(l.races[i]);
    }
    for (u32 i = 0; i < in.classCount; ++i) {
        targets.push_back(l.classes[i]);
    }
    for (const Rect& r : l.genders) {
        targets.push_back(r);
    }
    for (u32 i = 0; i < 5; ++i) {
        targets.push_back(l.leftArrows[i]);
        targets.push_back(l.rightArrows[i]);
    }
    for (const Rect& r : targets) {
        INFO("target " << r);
        CHECK(r.w >= minTargetDp * in.dp - 0.01f);
        CHECK(r.h >= minTargetDp * in.dp - 0.01f);
        CHECK(inside(r, a));
        CHECK(inside(r, l.config));
    }
    // Buttons tall enough (their hit rectangles grow to 44 dp) and on the screen.
    for (const Rect& r : {l.randomize, l.randomName, l.name, l.accept, l.back}) {
        INFO("button " << r);
        CHECK(r.h >= 40.0f * in.dp - 0.01f);
        CHECK(inside(r, a));
    }
    // No two targets of the configuration frame overlap.
    for (usize i = 0; i < targets.size(); ++i) {
        for (usize j = i + 1; j < targets.size(); ++j) {
            INFO(targets[i] << " and " << targets[j]);
            CHECK(apart(targets[i], targets[j]));
        }
    }
    // The frames: the configuration, the three texts and the bottom's buttons keep apart.
    const Rect frames[] = {l.config, l.info[0], l.info[1], l.info[2], l.name, l.randomName, l.accept, l.back};
    for (usize i = 0; i < std::size(frames); ++i) {
        CHECK(inside(frames[i], a));
        for (usize j = i + 1; j < std::size(frames); ++j) {
            INFO(i << " " << frames[i] << " and " << j << " " << frames[j]);
            CHECK(apart(frames[i], frames[j]));
        }
    }
    CHECK(inside(l.randomize, l.config));
    // The character in the middle: the screen's centre is free, with room around it.
    CHECK(l.scene.w >= 200.0f * in.dp);
    CHECK(l.config.right() < a.centerX() - 40.0f * in.dp);
    CHECK(l.info[0].x > a.centerX() + 40.0f * in.dp);
    // The scene's middle third (where app/GlueStage stands the character) stays nearly free.
    CHECK(l.config.right() <= a.x + a.w * 0.38f + 0.5f);
    CHECK(l.info[0].x >= a.right() - a.w * 0.38f - 0.5f);
}

} // namespace

TEST_CASE("Character creation layout: the Fold5's inner screen keeps the original's arrangement") {
    const CharCreateLayoutInput in = input(829.0f, 690.0f, false);
    checkScreen(in, 44.0f);
    const CharCreateLayout l = charCreateLayout(in);
    CHECK_FALSE(l.wide);
    CHECK(l.unit > 0.85f);
    // One column down the left: the races over the gender, the classes, the rows and Randomize.
    CHECK(l.races[3].bottom() < l.genders[0].y);
    CHECK(l.genders[0].bottom() < l.classes[0].y);
    CHECK(l.classes[3].bottom() < l.rows[0].y);
    CHECK(l.rows[4].bottom() < l.randomize.y);
    // The Alliance's races on the left, the Horde's on the right, in the banners.
    CHECK(l.races[0].right() < l.races[4].x);
    CHECK(l.races[0].x > l.banners[0].x);
    CHECK(l.races[4].right() < l.banners[1].right());
    // The three texts down the right, all open; Accept over Back under them.
    for (u32 i = 0; i < 3; ++i) {
        CHECK(l.infoOpen[i]);
    }
    CHECK(l.info[0].bottom() < l.info[1].y);
    CHECK(l.info[2].bottom() < l.accept.y);
    CHECK(l.accept.bottom() < l.back.y);
    // The name at the bottom centre, "Name" over it, the random name under it.
    CHECK(l.nameLabel.bottom() <= l.name.y + 0.5f);
    CHECK(l.randomName.y >= l.name.bottom());
    CHECK(l.name.centerX() > 400.0f);
    CHECK(l.name.centerX() < 440.0f);
}

TEST_CASE("Character creation layout: the wide screens use two columns and an accordion of texts") {
    for (const auto& [w, h] :
         {std::pair{884.0f, 345.0f}, std::pair{873.0f, 393.0f}, std::pair{800.0f, 360.0f}}) {
        CharCreateLayoutInput in = input(w, h, true);
        checkScreen(in, 44.0f);
        const CharCreateLayout l = charCreateLayout(in);
        CHECK(l.wide);
        // The races beside the classes and rows.
        CHECK(l.races[4].right() < l.classes[0].x);
        CHECK(l.races[4].right() < l.rows[0].x);
        // The configuration frame from top to bottom with the same small margin at each end, its content
        // in the middle.
        const f32 above = l.config.y - in.area.y, below = in.area.bottom() - l.config.bottom();
        CHECK(above == doctest::Approx(below).epsilon(0.001));
        CHECK(above >= 0.0f);
        CHECK(above <= 8.0f * l.unit + 0.5f);
        CHECK(l.allianceLabel.y > l.config.y);
        CHECK(l.randomize.bottom() < l.config.bottom());
        CHECK(inside(l.races[0], l.config));
        CHECK(inside(l.rows[4], l.config));
        // One text open (the race's), the others reduced to their header.
        CHECK(l.infoOpen[1]);
        CHECK_FALSE(l.infoOpen[0]);
        CHECK_FALSE(l.infoOpen[2]);
        CHECK(l.info[1].h > 2.0f * l.info[0].h);
        CHECK(l.info[0].h >= 44.0f); // a closed one is a finger's target
        CHECK(l.info[2].h >= 44.0f);
        in.expanded = 2;
        const CharCreateLayout other = charCreateLayout(in);
        CHECK(other.infoOpen[2]);
        CHECK(other.info[2].h > 2.0f * other.info[1].h);
    }
}

TEST_CASE("Character creation layout: a short square screen falls back to the wide arrangement") {
    const CharCreateLayout l = charCreateLayout(input(600.0f, 450.0f, false));
    CHECK(l.wide);
}

TEST_CASE("Character creation layout: phone densities and the desktop window") {
    checkScreen(input(2176.0f, 1812.0f, false, 2.625f), 44.0f); // the Fold5's inner screen in pixels
    checkScreen(input(2316.0f, 904.0f, true, 2.625f), 44.0f);   // its cover screen
    checkScreen(input(1088.0f, 906.0f, false), 44.0f);          // the desktop's --screen inner
    checkScreen(input(1158.0f, 452.0f, true), 44.0f);           // --screen cover
}

TEST_CASE("Character creation layout: the name rises over the soft keyboard") {
    CharCreateLayoutInput in = input(829.0f, 690.0f, false);
    const CharCreateLayout before = charCreateLayout(in);
    in.keyboardTop = 690.0f * 0.55f;
    const CharCreateLayout l = charCreateLayout(in);
    CHECK(l.name.bottom() <= in.keyboardTop);
    CHECK(l.nameLabel.bottom() <= l.name.y + 0.5f);
    CHECK(l.nameLabel.y >= 0.0f);
    // The rest stays where it was.
    CHECK(l.config.y == before.config.y);
    CHECK(l.accept.y == before.accept.y);
    CHECK(l.races[0].y == before.races[0].y);
    // A keyboard below the name moves nothing.
    in.keyboardTop = 689.0f;
    CHECK(charCreateLayout(in).name.y == before.name.y);
}

TEST_CASE("Character creation layout: the rows hang under the second row of classes") {
    for (u32 classes = 4; classes <= 6; ++classes) {
        CharCreateLayoutInput in = input(829.0f, 690.0f, false);
        in.classCount = classes;
        const CharCreateLayout l = charCreateLayout(in);
        CHECK(l.rows[0].y >= l.classes[3].bottom());
        // Three to a row, the second under the first.
        CHECK(l.classes[1].x > l.classes[0].right());
        CHECK(l.classes[3].x == l.classes[0].x);
        CHECK(l.classes[3].y == l.classes[0].bottom());
    }
}
