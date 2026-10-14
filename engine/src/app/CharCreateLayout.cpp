#include "app/CharCreateLayout.h"

#include <algorithm>

namespace vd {
namespace {

using ui::Rect;

// CharacterCreate.xml, in UI units.
constexpr f32 kIcon = 48.0f;   // CharacterCreateIconButtonTemplate
constexpr f32 kRaceGap = 5.0f; // race buttons under each other (0, -5)
// A race column's centre from the frame's (the two columns 46 units apart); wide: closer together.
constexpr f32 kColumnSpread = 47.0f, kColumnSpreadWide = 40.0f;
// Each banner's middle in its half of the art (texels 55-113 and 149-207 of 256).
constexpr f32 kBannerCentre[2] = {84.0f, 50.0f};
constexpr f32 kBannerAbove = 8.0f; // the banners' top over the first race button (TOP -60 against -68)
constexpr f32 kBannerW = 256.0f, kBannerH = 259.0f;
constexpr f32 kBannerShift = 2.0f;    // the banners are anchored 2 units left of the frame's centre
constexpr f32 kLabelShift = 50.0f;    // ALLIANCE / HORDE over the banners' top (-50 / +50, +3)
constexpr f32 kLabelH = 18.0f;        // GlueFontNormal's line
constexpr f32 kGenderGap = 28.0f;     // Male under race button 4 (+20, -28): over the banners' tails
constexpr f32 kGenderGapWide = 12.0f; // wide: closer (the column is short)
constexpr f32 kGenderSpace = 5.0f;    // Female right of Male
constexpr f32 kClassGap = 4.0f;       // class buttons side by side; the rows touch
constexpr f32 kClassUnder = 18.0f;    // the class grid under the gender (Banners BOTTOM, -50)
constexpr f32 kRowsUnder = 15.0f;     // CharacterCustomizationButtonFrame1 under class button 5
constexpr f32 kRow = 32.0f, kRowW = 198.0f;
constexpr f32 kPlateWide = 112.0f; // wide: the label plates narrower (the original's are 159 units)
// The label plate starts 5 units left of the row and ends 17 units into the left arrow (RIGHT -44 of 198
// units).
constexpr f32 kPlateLeft = 5.0f, kPlateIntoArrow = 17.0f;
// CharacterCreate_UpdateFacialHairCustomization: Randomize's TOP to frame 5's BOTTOM, -5.
constexpr f32 kRandomizeUnder = 5.0f;
constexpr f32 kSmallButtonH = 30.0f, kSmallButtonPad = 50.0f, kSmallFont = 12.0f; // GlueButtonSmallTemplate
constexpr f32 kBorderW = 224.0f; // UI-CharacterCreate-OuterBorder
constexpr f32 kInfoW = 240.0f, kInfoGap = 10.0f, kInfoMargin = 20.0f;
constexpr f32 kInfoH[3] = {160.0f, 260.0f, 210.0f};
constexpr f32 kInfoHeader = 34.0f; // wide: a closed text frame (its icon and title)
constexpr f32 kNameW = 156.0f, kNameH = 40.0f, kNameBottom = 50.0f, kNameShift = 8.0f;
constexpr f32 kNameLabelH = 22.0f; // GlueFontNormalLarge over the box
constexpr f32 kBackW = 120.0f, kBackH = 30.0f, kAcceptW = 160.0f, kAcceptH = 35.0f;
constexpr f32 kButtonsBottom = 20.0f, kButtonsGap = 5.0f;
constexpr f32 kMargin = 8.0f; // our margins (the original's frames sit 20-28 units in from 4:3 edges)
// Touch, in dp.
constexpr f32 kTargetDp = 44.0f;   // the smallest finger target
constexpr f32 kSqueezedDp = 40.0f; // what is left when even that does not fit
constexpr f32 kButtonDp = 40.0f;   // text buttons (their hit rectangle still grows to 44 dp)
constexpr f32 kMiddleDp = 220.0f;  // the free middle, at least
constexpr f32 kSideShare = 0.38f;  // the most of the width a side's frames take (the middle third: the scene)
constexpr f32 kGapDp = 6.0f;

struct Sizes {
    f32 k = 1.0f;  // logical pixels per unit
    f32 kb = 1.0f; // the race and class block's scale (icons at least the target)
    f32 icon = 48.0f, row = 32.0f, button = 30.0f;
};

Sizes sizes(f32 k, f32 dp, f32 targetDp) {
    Sizes s;
    s.k = k;
    s.icon = std::max(kIcon * k, targetDp * dp);
    s.kb = s.icon / kIcon;
    s.row = std::max(kRow * k, targetDp * dp);
    s.button = std::max(kSmallButtonH * k, kButtonDp * dp);
    return s;
}

u32 classRows(u32 classes) {
    return std::clamp((classes + 2u) / 3u, 1u, 3u);
}

f32 randomizeWidth(const CharCreateLayoutInput& in, f32 k) {
    return in.randomizeEm * kSmallFont * k + kSmallButtonPad * k;
}

// The race block from `top`: the faction labels, the banners and the race buttons in two columns `spread`
// units either side of `fc`. Returns the y under the last race button.
f32 placeRaces(CharCreateLayout& l, const Sizes& s, f32 fc, f32 top, f32 spread) {
    const f32 kb = s.kb;
    const f32 shift = spread - kColumnSpread; // the labels follow their columns
    l.allianceLabel = {fc - (kBannerShift + kLabelShift + shift + 60.0f) * kb, top, 120.0f * kb,
                       kLabelH * s.k};
    l.hordeLabel = {fc + (kLabelShift - kBannerShift + shift - 60.0f) * kb, top, 120.0f * kb, kLabelH * s.k};
    const f32 bannerTop = top + kLabelH * s.k + 3.0f * s.k;
    // Each half of UI-CharacterCreate-Banners (the Alliance's left, the Horde's right) behind its column.
    for (u32 side = 0; side < 2; ++side) {
        const f32 cx = side == 0 ? fc - spread * kb : fc + spread * kb;
        l.banners[side] = {cx - kBannerCentre[side] * kb, bannerTop, kBannerW * 0.5f * kb, kBannerH * kb};
    }
    const f32 racesTop = bannerTop + kBannerAbove * kb;
    for (u32 i = 0; i < 8; ++i) {
        const f32 cx = i < 4 ? fc - spread * kb : fc + spread * kb;
        l.races[i] = {cx - s.icon * 0.5f, racesTop + static_cast<f32>(i % 4) * (s.icon + kRaceGap * kb),
                      s.icon, s.icon};
    }
    return racesTop + 4.0f * s.icon + 3.0f * kRaceGap * kb;
}

void placeGenders(CharCreateLayout& l, const Sizes& s, f32 fc, f32 top) {
    l.genders[0] = {fc - kGenderSpace * 0.5f * s.kb - s.icon, top, s.icon, s.icon};
    l.genders[1] = {fc + kGenderSpace * 0.5f * s.kb, top, s.icon, s.icon};
}

// The class grid from `top`, three to a row around `fc`. Returns the y where the customization rows begin.
f32 placeClasses(CharCreateLayout& l, const Sizes& s, f32 fc, f32 top, u32 count) {
    const f32 pitch = s.icon + kClassGap * s.kb;
    for (u32 i = 0; i < 8; ++i) {
        const f32 cx = fc + static_cast<f32>(static_cast<i32>(i % 3) - 1) * pitch;
        l.classes[i] = {cx - s.icon * 0.5f, top + static_cast<f32>(i / 3) * s.icon, s.icon, s.icon};
    }
    // Under class button 5 (the second row); a race of more than six classes (none in 1.12) pushes them down.
    return top + static_cast<f32>(std::max(2u, classRows(count))) * s.icon + kRowsUnder * s.k;
}

// The row's width for a label plate `plate` units wide (the plate from 5 units left of the row to 17 units
// into the left arrow).
f32 rowWidth(const Sizes& s, f32 plate) {
    return (plate - kPlateLeft - kPlateIntoArrow) * s.k + 2.0f * s.row;
}

// The five customization rows `rowW` wide from `top` around `fc`, then Randomize. Returns the y under
// Randomize.
f32 placeRows(CharCreateLayout& l, const CharCreateLayoutInput& in, const Sizes& s, f32 fc, f32 top,
              f32 rowW) {
    const f32 k = s.k;
    for (u32 i = 0; i < 5; ++i) {
        const Rect row{fc - rowW * 0.5f, top + static_cast<f32>(i) * s.row, rowW, s.row};
        l.rows[i] = row;
        l.rightArrows[i] = {row.right() - s.row, row.y, s.row, s.row};
        // The original's left arrow is 3 units into the right one: side by side here, two fingers' targets.
        l.leftArrows[i] = {l.rightArrows[i].x - s.row, row.y, s.row, s.row};
        const f32 plateLeft = row.x - kPlateLeft * k;
        l.plates[i] = {plateLeft, row.y, l.leftArrows[i].x + kPlateIntoArrow * k - plateLeft, s.row};
    }
    // Under where frame 5 is, shown or not (the Lua anchors Randomize to it either way).
    const f32 y = top + 5.0f * s.row + kRandomizeUnder * k;
    const f32 w = randomizeWidth(in, k);
    l.randomize = {fc - w * 0.5f, y, w, s.button};
    return l.randomize.bottom();
}

// The square arrangement at scale `k`; returns the height the left column needs.
f32 square(CharCreateLayout& l, const CharCreateLayoutInput& in, const Sizes& s) {
    const Rect& a = in.area;
    const f32 k = s.k;
    const f32 top = a.y + kMargin * 0.75f * k;
    const f32 width = std::max(kBorderW * k, 2.0f * (kColumnSpread * s.kb + s.icon * 0.5f) + 24.0f * k);
    const f32 fc = a.x + kMargin * k + width * 0.5f;
    f32 y = placeRaces(l, s, fc, top + kMargin * k, kColumnSpread);
    placeGenders(l, s, fc, y + kGenderGap * s.kb);
    y = placeClasses(l, s, fc, l.genders[0].bottom() + kClassUnder * s.kb, in.classCount);
    y = placeRows(l, in, s, fc, y, kRowW * k);
    l.config = {fc - width * 0.5f, top, width, y + kMargin * k - top};
    return l.config.bottom() - a.y;
}

// The wide arrangement at scale `k`; returns the height its taller column needs.
f32 wide(CharCreateLayout& l, const CharCreateLayoutInput& in, const Sizes& s) {
    const Rect& a = in.area;
    const f32 k = s.k;
    const f32 top = a.y + kMargin * 0.75f * k;
    // Column A: the races, the gender, Randomize; column B: the classes and the customization rows. Narrower
    // than the original's column so that they keep to the screen's left third or so.
    const f32 rowW = rowWidth(s, kPlateWide);
    // 12 units either side: the Alliance banner clear of the frame's border (TextPanel-Border).
    const f32 widthA =
        std::max(2.0f * (kColumnSpreadWide * s.kb + s.icon * 0.5f) + 24.0f * k, randomizeWidth(in, k));
    const f32 widthB = std::max(rowW + kPlateLeft * k, 3.0f * s.icon + 2.0f * kClassGap * s.kb);
    const f32 left = a.x + kMargin * 0.5f * k;
    const f32 fcA = left + kMargin * 0.75f * k + widthA * 0.5f;
    const f32 fcB = left + kMargin * 0.75f * k + widthA + 6.0f * k + widthB * 0.5f;
    f32 yA = placeRaces(l, s, fcA, top + kMargin * 0.5f * k, kColumnSpreadWide);
    placeGenders(l, s, fcA, yA + kGenderGapWide * s.kb);
    yA = l.genders[0].bottom() + kMargin * k;
    const f32 w = randomizeWidth(in, k);
    l.randomize = {fcA - w * 0.5f, yA, w, s.button};
    yA = l.randomize.bottom();
    f32 yB = placeClasses(l, s, fcB, top + kMargin * k, in.classCount);
    yB = placeRows(l, in, s, fcB, yB - kRowsUnder * k + kMargin * k, rowW);
    // placeRows put Randomize under the rows: column A has it.
    l.randomize = {fcA - w * 0.5f, l.genders[0].bottom() + kMargin * k, w, s.button};
    yB -= kRandomizeUnder * k + s.button;
    const f32 bottom = std::max(yA, yB) + kMargin * 0.75f * k;
    l.config = {left, top, fcB + widthB * 0.5f + kMargin * 0.75f * k - left, bottom - top};
    return l.config.bottom() - a.y;
}

// The whole configuration frame `dy` lower.
void shiftConfig(CharCreateLayout& l, f32 dy) {
    auto down = [dy](Rect& r) { r.y += dy; };
    down(l.config);
    down(l.allianceLabel);
    down(l.hordeLabel);
    down(l.randomize);
    for (auto* group : {&l.banners, &l.genders}) {
        std::for_each(group->begin(), group->end(), down);
    }
    for (auto* group : {&l.races, &l.classes}) {
        std::for_each(group->begin(), group->end(), down);
    }
    for (auto* group : {&l.rows, &l.plates, &l.leftArrows, &l.rightArrows}) {
        std::for_each(group->begin(), group->end(), down);
    }
}

// The largest scale whose arrangement fits the area: its height, and the middle free between the columns for
// the character and the name box at the bottom centre.
f32 solveScale(CharCreateLayout& l, const CharCreateLayoutInput& in, bool wideArrangement, f32 targetDp,
               bool& fits) {
    const f32 dp = in.dp;
    const Rect& a = in.area;
    auto fitsAt = [&](f32 k) {
        const Sizes s = sizes(k, dp, targetDp);
        if ((wideArrangement ? wide(l, in, s) : square(l, in, s)) > a.h) {
            return false;
        }
        const f32 infoX = a.right() - (wideArrangement ? kMargin : kInfoMargin) * k - kInfoW * k;
        const f32 nameLeft = a.centerX() + (kNameShift - kNameW * 0.5f) * k;
        const f32 nameRight = a.centerX() + (kNameShift + kNameW * 0.5f) * k;
        const f32 gap = kGapDp * dp;
        // The character stands in the middle third (app/GlueStage): the columns keep to the sides, give or
        // take.
        const f32 side = a.w * kSideShare;
        return l.config.right() + gap <= nameLeft && nameRight + gap <= infoX &&
               infoX - l.config.right() >= kMiddleDp * dp && l.config.right() <= a.x + side &&
               infoX >= a.right() - side;
    };
    f32 lo = 0.5f * dp, hi = 1.5f * dp;
    fits = fitsAt(lo);
    if (!fits) {
        return lo;
    }
    for (u32 i = 0; i < 24; ++i) {
        const f32 mid = 0.5f * (lo + hi);
        (fitsAt(mid) ? lo : hi) = mid;
    }
    return lo;
}

} // namespace

CharCreateLayout charCreateLayout(const CharCreateLayoutInput& in) {
    CharCreateLayout l;
    const Rect& a = in.area;
    const f32 dp = in.dp;
    // The original's arrangement where it fits; the wide one otherwise; the touch minimum relaxed last.
    bool fits = false;
    bool wideArrangement = in.wide;
    f32 targetDp = kTargetDp;
    f32 k = solveScale(l, in, wideArrangement, targetDp, fits);
    if (!fits && !wideArrangement) {
        wideArrangement = true;
        k = solveScale(l, in, wideArrangement, targetDp, fits);
    }
    if (!fits) {
        targetDp = kSqueezedDp;
        k = solveScale(l, in, wideArrangement, targetDp, fits);
    }
    const Sizes s = sizes(k, dp, targetDp);
    if (wideArrangement) {
        wide(l, in, s);
    } else {
        square(l, in, s);
    }
    // The configuration frame from top to bottom, a margin from each edge, what it holds in its middle (the
    // original's hangs under the WoW logo, left out here). Taller than that, it keeps its height, centred.
    const f32 frameH = std::max(a.h - 2.0f * kMargin * k, l.config.h);
    const f32 frameTop = a.y + (a.h - frameH) * 0.5f;
    shiftConfig(l, frameTop + (frameH - l.config.h) * 0.5f - l.config.y);
    l.config.y = frameTop;
    l.config.h = frameH;
    l.unit = k;
    l.iconUnit = s.kb;
    l.wide = wideArrangement;

    // The right column: the three texts, Back and Accept under them.
    const f32 infoW = kInfoW * k;
    const f32 infoX = a.right() - (wideArrangement ? kMargin : kInfoMargin) * k - infoW;
    const f32 buttonsX = infoX + infoW * 0.5f;
    const f32 backH = std::max(kBackH * k, kTargetDp * dp), acceptH = std::max(kAcceptH * k, kTargetDp * dp);
    const f32 backW = kBackW * k, acceptW = kAcceptW * k;
    const f32 bottomMargin = (wideArrangement ? kMargin : kButtonsBottom) * k;
    l.back = {buttonsX - backW * 0.5f, a.bottom() - bottomMargin - backH, backW, backH};
    l.accept = {buttonsX - acceptW * 0.5f, l.back.y - kButtonsGap * k - acceptH, acceptW, acceptH};
    const f32 infoTop = a.y + (wideArrangement ? kMargin * 1.5f : kInfoMargin) * k;
    const f32 infoBottom = l.accept.y - kInfoGap * k;
    const f32 room = std::max(0.0f, infoBottom - infoTop - 2.0f * kInfoGap * k);
    f32 y = infoTop;
    for (u32 i = 0; i < 3; ++i) {
        f32 h = room * kInfoH[i] / (kInfoH[0] + kInfoH[1] + kInfoH[2]);
        l.infoOpen[i] = true;
        if (wideArrangement) {
            const u32 open = std::min(in.expanded, 2u);
            const f32 header = std::max(kInfoHeader * k, kTargetDp * dp); // a finger taps it open
            l.infoOpen[i] = i == open;
            h = i == open ? room - 2.0f * header : header;
        }
        l.info[i] = {infoX, y, infoW, std::max(0.0f, h)};
        y += l.info[i].h + kInfoGap * k;
    }

    // The name at the bottom centre (BOTTOM 8, 50), "Name" over it, the random name under it.
    const f32 nameW = kNameW * k, nameH = std::max(kNameH * k, kTargetDp * dp);
    const f32 randomW = randomizeWidth(in, k);
    const f32 nameBottom = a.bottom() - std::max(kNameBottom * k, s.button + 2.0f * kGapDp * dp);
    l.name = {a.centerX() + kNameShift * k - nameW * 0.5f, nameBottom - nameH, nameW, nameH};
    l.randomName = {l.name.centerX() - randomW * 0.5f, l.name.bottom() + kGapDp * 0.5f * dp, randomW,
                    s.button};

    // While the name is edited, the box and its label rise above the keyboard.
    const f32 gap = kGapDp * dp;
    if (in.keyboardTop > 0.0f && l.name.bottom() + gap > in.keyboardTop) {
        l.name.y = std::max(a.y + kNameLabelH * k, in.keyboardTop - gap - nameH);
    }
    l.nameLabel = {l.name.centerX() - 128.0f * k, l.name.y - kNameLabelH * k, 256.0f * k, kNameLabelH * k};

    l.scene = {l.config.right(), a.y, std::max(0.0f, infoX - l.config.right()), a.h};
    return l;
}

} // namespace vd
