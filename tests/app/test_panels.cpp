// Parity pass (panels): the original interface's panel rules as UIParent.lua has them (UIPanelWindows,
// ShowUIPanel, HideUIPanel, CloseWindows / CloseAllWindows: which window takes the left slot, which the
// center, which pushes which aside, which replace or close the others), the bags beside them, one place on
// the cover screen, and where the slots go on the two screens of the Fold5 (app/UiPanels).
#include "app/UiPanels.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

namespace {

using namespace vd;

std::vector<UiPanel> hidden(UiPanelSlots& slots) {
    const std::span<const UiPanel> h = slots.takeHidden();
    return {h.begin(), h.end()};
}

bool contains(const std::vector<UiPanel>& list, UiPanel panel) {
    return std::find(list.begin(), list.end(), panel) != list.end();
}

UiPanelShow open(UiPanelSlots& slots, UiPanel panel, bool dead = false, bool force = false) {
    return slots.show(panel, dead, force);
}

} // namespace

TEST_CASE("UIPanelWindows: areas, priorities and whileDead as the original registers them") {
    CHECK(uiPanelInfo(UiPanel::Character).area == UiPanelArea::Left);
    CHECK(uiPanelInfo(UiPanel::Character).pushable == 2);
    CHECK(uiPanelInfo(UiPanel::Character).whileDead);
    CHECK(uiPanelInfo(UiPanel::Loot).pushable == 7);
    CHECK(uiPanelInfo(UiPanel::Bank).pushable == 6);
    CHECK(uiPanelInfo(UiPanel::Trade).pushable == 1);
    CHECK(uiPanelInfo(UiPanel::Talents).pushable == 6); // Blizzard_TalentUI
    CHECK(uiPanelInfo(UiPanel::Talents).whileDead);
    CHECK(uiPanelInfo(UiPanel::Trainer).pushable == 0); // Blizzard_TrainerUI
    CHECK(!uiPanelInfo(UiPanel::Merchant).whileDead);
    CHECK(uiPanelInfo(UiPanel::QuestLog).whileDead);
    CHECK(uiPanelInfo(UiPanel::WorldMap).area == UiPanelArea::Full);
    CHECK(uiPanelInfo(UiPanel::Auction).area == UiPanelArea::Doublewide); // Blizzard_AuctionUI
    CHECK(uiPanelInfo(UiPanel::GameMenu).area == UiPanelArea::Center);
    CHECK(uiPanelInfo(UiPanel::Options).area == UiPanelArea::Center);
    CHECK(uiPanelHostsBags(UiPanel::Merchant));
    CHECK(uiPanelHostsBags(UiPanel::Mail));
    CHECK(!uiPanelHostsBags(UiPanel::Character));
}

TEST_CASE("ShowUIPanel: two windows nobody pushes replace each other in the left slot") {
    UiPanelSlots slots;
    CHECK(open(slots, UiPanel::QuestLog) == UiPanelShow::Shown);
    CHECK(slots.left() == UiPanel::QuestLog);
    CHECK(hidden(slots).empty());
    // Spell book and quest log are both pushable 0: the book replaces the log (as in 1.12).
    CHECK(open(slots, UiPanel::Spellbook) == UiPanelShow::Shown);
    CHECK(slots.left() == UiPanel::Spellbook);
    CHECK(slots.center() == UiPanel::None);
    CHECK(hidden(slots) == std::vector<UiPanel>{UiPanel::QuestLog});
    // Already shown: nothing changes.
    CHECK(open(slots, UiPanel::Spellbook) == UiPanelShow::Shown);
    CHECK(hidden(slots).empty());
}

TEST_CASE("ShowUIPanel: the higher priority goes to the center") {
    SUBCASE("the character sheet (2) is pushed to the center by the spell book (0)") {
        UiPanelSlots slots;
        open(slots, UiPanel::Character);
        open(slots, UiPanel::Spellbook);
        CHECK(slots.left() == UiPanel::Spellbook);
        CHECK(slots.center() == UiPanel::Character);
        CHECK(hidden(slots).empty());
    }
    SUBCASE("the spell book first: the character sheet goes to the center") {
        UiPanelSlots slots;
        open(slots, UiPanel::Spellbook);
        open(slots, UiPanel::Character);
        CHECK(slots.left() == UiPanel::Spellbook);
        CHECK(slots.center() == UiPanel::Character);
    }
    SUBCASE("the talents (6) beside the spell book") {
        UiPanelSlots slots;
        open(slots, UiPanel::Spellbook);
        open(slots, UiPanel::Talents);
        CHECK(slots.left() == UiPanel::Spellbook);
        CHECK(slots.center() == UiPanel::Talents);
    }
    SUBCASE("both taken: a higher one than the center's takes the center, the center goes back left") {
        UiPanelSlots slots;
        open(slots, UiPanel::Spellbook);
        open(slots, UiPanel::Character); // spell book left, character center
        open(slots, UiPanel::Loot);      // 7 > 2
        CHECK(slots.left() == UiPanel::Character);
        CHECK(slots.center() == UiPanel::Loot);
        CHECK(hidden(slots) == std::vector<UiPanel>{UiPanel::Spellbook});
    }
    SUBCASE("both taken: a lower one replaces the left one") {
        UiPanelSlots slots;
        open(slots, UiPanel::Spellbook);
        open(slots, UiPanel::Character);
        open(slots, UiPanel::QuestLog); // 0 > 2? no
        CHECK(slots.left() == UiPanel::QuestLog);
        CHECK(slots.center() == UiPanel::Character);
        CHECK(hidden(slots) == std::vector<UiPanel>{UiPanel::Spellbook});
    }
    SUBCASE("a vendor replaces the conversation that opened it (GossipFrame and MerchantFrame are both 0)") {
        UiPanelSlots slots;
        open(slots, UiPanel::Quest);
        open(slots, UiPanel::Merchant);
        CHECK(slots.left() == UiPanel::Merchant);
        CHECK(hidden(slots) == std::vector<UiPanel>{UiPanel::Quest});
    }
}

TEST_CASE("HideUIPanel: the center window moves back to the left") {
    UiPanelSlots slots;
    open(slots, UiPanel::Character);
    open(slots, UiPanel::Spellbook); // spell book left, character center
    slots.hide(UiPanel::Spellbook);
    CHECK(slots.left() == UiPanel::Character);
    CHECK(slots.center() == UiPanel::None);
    CHECK(hidden(slots) == std::vector<UiPanel>{UiPanel::Spellbook});
    slots.hide(UiPanel::Character);
    CHECK(!slots.any());
}

TEST_CASE("Center windows close the rest and keep the others out; full-screen ones too unless forced") {
    UiPanelSlots bags;
    open(bags, UiPanel::QuestLog);
    CHECK(bags.openBags());
    CHECK(open(bags, UiPanel::GameMenu) == UiPanelShow::Shown);
    CHECK(!bags.bagsOpen()); // CloseAllBags
    CHECK(bags.left() == UiPanel::None);

    UiPanelSlots slots;
    open(slots, UiPanel::Character);
    open(slots, UiPanel::Spellbook);
    CHECK(open(slots, UiPanel::GameMenu) == UiPanelShow::Shown);
    CHECK(slots.center() == UiPanel::GameMenu);
    CHECK(slots.left() == UiPanel::None);
    const std::vector<UiPanel> closed = hidden(slots);
    CHECK(contains(closed, UiPanel::Character));
    CHECK(contains(closed, UiPanel::Spellbook));
    // CanOpenPanels: nothing but another center window while it is up, forced or not.
    CHECK(open(slots, UiPanel::QuestLog) == UiPanelShow::Refused);
    CHECK(open(slots, UiPanel::QuestLog, false, true) == UiPanelShow::Refused);
    CHECK(open(slots, UiPanel::Options) == UiPanelShow::Shown);
    CHECK(slots.center() == UiPanel::Options);
    CHECK(hidden(slots) == std::vector<UiPanel>{UiPanel::GameMenu});

    UiPanelSlots map;
    open(map, UiPanel::QuestLog);
    CHECK(open(map, UiPanel::WorldMap) == UiPanelShow::Shown);
    CHECK(map.full() == UiPanel::WorldMap);
    CHECK(map.left() == UiPanel::None);
    CHECK(hidden(map) == std::vector<UiPanel>{UiPanel::QuestLog});
    CHECK(open(map, UiPanel::Merchant) == UiPanelShow::Refused);
    CHECK(open(map, UiPanel::Character, false, true) == UiPanelShow::Shown); // forced: the map goes
    CHECK(map.full() == UiPanel::None);
    CHECK(map.left() == UiPanel::Character);
    CHECK(hidden(map) == std::vector<UiPanel>{UiPanel::WorldMap});
}

TEST_CASE("Doublewide windows take both slots and give them back") {
    UiPanelSlots slots;
    open(slots, UiPanel::Spellbook);
    open(slots, UiPanel::Character);
    hidden(slots);
    CHECK(open(slots, UiPanel::Auction) == UiPanelShow::Shown);
    CHECK(slots.doublewide() == UiPanel::Auction);
    CHECK(slots.left() == UiPanel::None);
    CHECK(slots.center() == UiPanel::None);
    CHECK(hidden(slots).size() == 2);
    open(slots, UiPanel::QuestLog);
    CHECK(slots.doublewide() == UiPanel::None);
    CHECK(slots.left() == UiPanel::QuestLog);
    CHECK(hidden(slots) == std::vector<UiPanel>{UiPanel::Auction});
}

TEST_CASE("Dead: only the windows registered whileDead open; PLAYER_DEAD keeps a center window") {
    UiPanelSlots slots;
    CHECK(open(slots, UiPanel::Merchant, true) == UiPanelShow::Dead);
    CHECK(open(slots, UiPanel::Loot, true) == UiPanelShow::Dead);
    CHECK(!slots.any());
    CHECK(open(slots, UiPanel::Character, true) == UiPanelShow::Shown);
    CHECK(open(slots, UiPanel::QuestLog, true) == UiPanelShow::Shown);
    slots.closeAll(true);
    CHECK(!slots.any());
    open(slots, UiPanel::GameMenu);
    slots.closeAll(true); // CloseAllWindows(1): the center window stays
    CHECK(slots.center() == UiPanel::GameMenu);
}

TEST_CASE("One place (the cover screen): the window just opened is the one shown") {
    UiPanelSlots slots;
    slots.setCapacity(1);
    open(slots, UiPanel::Character);
    open(slots, UiPanel::Spellbook); // the rules put the character in the center: there is none
    CHECK(slots.left() == UiPanel::Spellbook);
    CHECK(slots.center() == UiPanel::None);
    CHECK(hidden(slots) == std::vector<UiPanel>{UiPanel::Character});
    open(slots, UiPanel::Loot); // the rules put the loot in the center
    CHECK(!slots.shown(UiPanel::Spellbook));
    CHECK(slots.shown(UiPanel::Loot));
    // The bags share the place: the last one asked for stays.
    CHECK(slots.openBags());
    CHECK(!slots.shown(UiPanel::Loot));
    CHECK(slots.bagsOpen());
    open(slots, UiPanel::QuestLog);
    CHECK(!slots.bagsOpen());
    CHECK(slots.left() == UiPanel::QuestLog);
    // Folding the phone: two windows become one (the left one stays).
    UiPanelSlots two;
    open(two, UiPanel::Character);
    open(two, UiPanel::Spellbook);
    hidden(two);
    two.setCapacity(1);
    CHECK(two.left() == UiPanel::Spellbook);
    CHECK(two.center() == UiPanel::None);
    CHECK(hidden(two) == std::vector<UiPanel>{UiPanel::Character});
}

TEST_CASE("The bags beside the left window, sharing the right side with the center slot") {
    UiPanelSlots slots;
    open(slots, UiPanel::Character);
    CHECK(slots.openBags());
    CHECK(slots.left() == UiPanel::Character);
    CHECK(slots.bagsOpen());
    // A window pushed to the center takes the right side: the bags close.
    open(slots, UiPanel::Spellbook);
    CHECK(slots.center() == UiPanel::Character);
    CHECK(!slots.bagsOpen());
    // The bags back: the center window goes.
    CHECK(slots.openBags());
    CHECK(slots.center() == UiPanel::None);
    CHECK(slots.left() == UiPanel::Spellbook);
    CHECK(contains(hidden(slots), UiPanel::Character));
    // A vendor has the bags inside: they close, and do not open beside it.
    open(slots, UiPanel::Merchant);
    CHECK(!slots.bagsOpen());
    CHECK(!slots.openBags());
    // CloseAllWindows: the bags too.
    slots.hide(UiPanel::Merchant);
    CHECK(slots.openBags());
    slots.closeAll();
    CHECK(!slots.any());
}

TEST_CASE("Places on the inner screen: two side by side, clear of the stick's band and the action buttons") {
    // The Fold5's inner screen in dp (2176 x 1812 at density 2.6), the HUD's rectangles as app/Hud lays them
    // out.
    UiPanelSpace s;
    s.safe = {0.0f, 0.0f, 837.0f, 697.0f};
    s.dp = 1.0f;
    s.unit = 0.78f;
    s.framesBottom = 78.0f;
    s.leftTop = 125.0f;                           // under the target's aura rows (160 units)
    s.rightTop = 141.0f;                          // the minimap cluster
    s.cluster = {418.0f, 458.0f, 403.0f, 219.0f}; // the twelve buttons around the context button
    const UiPanelPlaces p = uiPanelPlaces(s);
    REQUIRE(p.left.w > 0.0f);
    REQUIRE(p.right.w > 0.0f);
    CHECK(p.left.y > s.leftTop);
    CHECK(p.left.right() <= p.right.x);
    CHECK(!uiPanelOverlaps(p.left, s.cluster));
    CHECK(!uiPanelOverlaps(p.right, s.cluster));
    CHECK(!uiPanelOverlaps(p.left, p.stickPad));
    CHECK(p.right.y >= s.rightTop);
    CHECK(p.right.right() <= s.safe.right());
    CHECK(p.left.w >= 380.0f); // room for a window, two side by side about as tall
    CHECK(p.left.h >= 290.0f);
    CHECK(p.right.h >= 290.0f);
    // The stick keeps the lower fourth at the left (where the left thumb rests).
    CHECK(p.stickPad.h >= s.safe.h * 0.22f);
    CHECK(p.stickPad.w >= 300.0f);
    // Both slots together (the world map, the auction house) over both columns, still clear of the cluster.
    CHECK(p.both.x == p.left.x);
    CHECK(p.both.y < s.leftTop); // up to the frames, as the original's full-screen map
    CHECK(p.both.right() == p.right.right());
    CHECK(!uiPanelOverlaps(p.both, s.cluster));
    CHECK(!uiPanelOverlaps(p.both, p.stickPad));
    CHECK_FALSE(p.overFrames);
    // The windows' wide layouts in the short, wide places; their tall ones otherwise.
    CHECK(uiPanelWide(p.both));
    CHECK(!uiPanelWide(p.left));

    SUBCASE("a stance bar over the cluster: the right place ends above it") {
        s.stance = {697.0f, 411.0f, 124.0f, 38.0f};
        const UiPanelPlaces q = uiPanelPlaces(s);
        CHECK(!uiPanelOverlaps(q.right, s.stance));
        CHECK(q.right.bottom() <= s.stance.y);
        CHECK(q.right.h >= 250.0f);
    }
}

TEST_CASE("Places on the cover screen: one, from the edges to the action buttons") {
    // The cover screen held sideways (2316 x 904 at 2.6).
    UiPanelSpace s;
    s.safe = {0.0f, 0.0f, 891.0f, 348.0f};
    s.dp = 1.0f;
    s.unit = 0.78f;
    s.wide = true;
    s.framesBottom = 78.0f;
    s.rightTop = 141.0f;
    s.cluster = {513.0f, 132.0f, 363.0f, 198.0f};
    const UiPanelPlaces p = uiPanelPlaces(s);
    CHECK(p.right.w == 0.0f); // one at a time
    REQUIRE(p.left.w > 0.0f);
    // Ours: over the stick's column and the unit frames (on purpose), opaque.
    CHECK(p.left.x < 10.0f);
    CHECK(p.left.y < 10.0f);
    CHECK(p.overFrames);
    CHECK(!uiPanelOverlaps(p.left, s.cluster));
    CHECK(p.left.h >= 300.0f);
    CHECK(p.left.w >= 480.0f);
    CHECK(uiPanelWide(p.left));
    CHECK(p.both.x == p.left.x);
    CHECK(p.both.w == p.left.w);

    CHECK(p.leftWide.x == p.left.x); // no stance bar: the quest log's place is the same
    CHECK(p.leftWide.w == p.left.w);
    CHECK(p.leftWide.h == p.left.h);

    SUBCASE("a stance bar beside the bottom row: narrower, or shorter when too narrow") {
        s.stance = {409.0f, 282.0f, 112.0f, 34.0f}; // three stances
        UiPanelPlaces q = uiPanelPlaces(s);
        CHECK(!uiPanelOverlaps(q.left, s.stance));
        // The quest log's: as wide as without it, ending over it.
        CHECK(!uiPanelOverlaps(q.leftWide, s.stance));
        CHECK(q.leftWide.w == p.left.w);
        CHECK(q.leftWide.bottom() <= s.stance.y);
        CHECK(q.leftWide.w > q.left.w);
        s.stance = {253.0f, 282.0f, 268.0f, 34.0f}; // seven druid forms
        q = uiPanelPlaces(s);
        CHECK(!uiPanelOverlaps(q.left, s.stance));
        CHECK(q.left.w >= 240.0f);
        CHECK(!uiPanelOverlaps(q.leftWide, s.stance));
    }
}
