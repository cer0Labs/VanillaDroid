// Retouches (friends frame): FriendsFrame at its art's proportions with the places that
// FriendsFrame.xml, RaidFrame.xml and Blizzard_RaidUI.xml give its pages (app/FriendsFrameLayout), and its bottom tabs
// turned to stand along its right edge.
#include "app/FriendsFrameLayout.h"
#include "app/QuestLogLayout.h"

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

// At one pixel a unit, in a place bigger than the art: at its top left (10, 20), the picture 2 left of what shows.
constexpr f32 kAx = 10.0f - kFriendsFrameLeft, kAy = 20.0f;

FriendsFrameLayout unitLayout() {
    return friendsFrameLayout({10.0f, 20.0f, 500.0f, 600.0f}, 1.0f, false);
}

} // namespace

TEST_CASE("Friends frame art: FriendsFrame.xml's places at its proportions") {
    const FriendsFrameLayout l = unitLayout();
    CHECK(l.k == doctest::Approx(1.0f));
    CHECK(l.art.x == doctest::Approx(kAx));
    CHECK(l.art.y == doctest::Approx(kAy));
    CHECK(l.art.w == doctest::Approx(384.0f));
    CHECK(l.art.h == doctest::Approx(512.0f));
    CHECK(l.frame.x == doctest::Approx(10.0f));
    CHECK(l.frame.w == doctest::Approx(kFriendsFrameRight - kFriendsFrameLeft));
    CHECK(l.frame.h == doctest::Approx(kFriendsFrameHeight));
    // The portrait (7, -6) 60 x 60; the title at TOP (0, -18); the close button at TOPRIGHT (-30, -8).
    CHECK(l.portrait.x == doctest::Approx(kAx + 7.0f));
    CHECK(l.portrait.y == doctest::Approx(kAy + 6.0f));
    CHECK(l.title.centerX() == doctest::Approx(kAx + 192.0f));
    CHECK(l.close.right() == doctest::Approx(kAx + 384.0f - 30.0f));
    CHECK(l.close.y == doctest::Approx(kAy + 8.0f));
    // The toggle tabs from (70, -39); the lists 296 wide with their right 67 in from the picture's, from 75 down, 304
    // and 332 high; their bars 6 right of them; the first rows at 76 and 80, 23 in.
    CHECK(l.toggleTabs.x == doctest::Approx(kAx + 70.0f));
    CHECK(l.toggleTabs.y == doctest::Approx(kAy + 39.0f));
    CHECK(l.friends.view.right() == doctest::Approx(kAx + 384.0f - 67.0f));
    CHECK(l.friends.view.y == doctest::Approx(kAy + 75.0f));
    CHECK(l.friends.view.h == doctest::Approx(304.0f));
    CHECK(l.ignore.view.h == doctest::Approx(332.0f));
    CHECK(l.friends.bar.x == doctest::Approx(l.friends.view.right() + 6.0f));
    CHECK(l.friends.bar.w == doctest::Approx(16.0f));
    CHECK(l.friends.rowX == doctest::Approx(kAx + 23.0f));
    CHECK(l.friends.rowW == doctest::Approx(298.0f));
    CHECK(l.friends.rowTop == doctest::Approx(kAy + 76.0f));
    CHECK(l.ignore.rowTop == doctest::Approx(kAy + 80.0f));
    // UI-Character-ScrollBar: 5 above the list down to 2 under it, 2 left of its right.
    CHECK(l.friends.track[0].y == doctest::Approx(l.friends.view.y - 5.0f));
    CHECK(l.friends.track[1].bottom() == doctest::Approx(l.friends.view.bottom() + 2.0f));
    CHECK(l.ignore.track[1].bottom() == doctest::Approx(l.ignore.view.bottom() + 2.0f));
    CHECK(l.friends.track[0].x == doctest::Approx(l.friends.view.right() - 2.0f));
    CHECK(l.friends.trackV == doctest::Approx(1.0f));
    // The buttons: Add Friend at BOTTOMLEFT (17, 107), Remove Friend 5 under it, the others 66 right of them; the ignore
    // list's at (17, 81) and 62 apart.
    CHECK(l.addFriend.x == doctest::Approx(kAx + 17.0f));
    CHECK(l.addFriend.bottom() == doctest::Approx(kAy + 512.0f - 107.0f));
    CHECK(l.removeFriend.y == doctest::Approx(l.addFriend.bottom() + 5.0f));
    CHECK(l.sendMessage.x == doctest::Approx(l.addFriend.right() + 66.0f));
    CHECK(l.sendMessage.y == doctest::Approx(l.addFriend.y));
    CHECK(l.groupInvite.y == doctest::Approx(l.removeFriend.y));
    CHECK(l.ignorePlayer.bottom() == doctest::Approx(kAy + 512.0f - 81.0f));
    CHECK(l.stopIgnore.x == doctest::Approx(l.ignorePlayer.right() + 62.0f));
    for (const ui::Rect& r : {l.addFriend, l.removeFriend, l.sendMessage, l.groupInvite, l.ignorePlayer, l.stopIgnore}) {
        CHECK(r.w == doctest::Approx(131.0f));
        CHECK(r.h == doctest::Approx(21.0f));
        CHECK(inside(r, l.frame));
    }
    CHECK(l.friends.view.bottom() <= l.addFriend.y);
    CHECK(l.ignore.view.bottom() <= l.ignorePlayer.y);
    CHECK(apart(l.addFriend, l.sendMessage));
    CHECK(apart(l.removeFriend, l.groupInvite));
    // Everything else inside what shows (the close button's art reaches 4 units past the border, as the original's).
    for (const ui::Rect& r : {l.portrait, l.title, l.toggleTabs, l.friends.view, l.friends.bar, l.ignore.view,
                              l.ignore.bar}) {
        CHECK(inside(r, l.frame));
    }
    CHECK(inside(l.close, l.art));
}

TEST_CASE("Friends frame art: WhoFrame's places") {
    const FriendsFrameLayout l = unitLayout();
    const WhoFrameLayout& w = l.who;
    CHECK(w.headers.x == doctest::Approx(kAx + 20.0f)); // WhoFrameColumnHeader1 at (20, -70), 24 high
    CHECK(w.headers.y == doctest::Approx(kAy + 70.0f));
    CHECK(w.headers.h == doctest::Approx(24.0f));
    CHECK(w.list.rowX == doctest::Approx(kAx + 15.0f)); // WhoFrameButton1 at (15, -95)
    CHECK(w.list.rowTop == doctest::Approx(kAy + 95.0f));
    CHECK(w.list.view.h == doctest::Approx(17.0f * 16.0f)); // WHOS_TO_DISPLAY rows of 16
    CHECK(w.list.bar.y == doctest::Approx(kAy + 96.0f));    // WhoListScrollFrame from 96 down 287
    CHECK(w.list.bar.h == doctest::Approx(287.0f));
    CHECK(w.totals.bottom() == doctest::Approx(kAy + 512.0f - 127.0f));
    CHECK(w.totals.centerX() == doctest::Approx(kAx + 192.0f - 10.0f));
    CHECK(w.edit.bottom() == doctest::Approx(kAy + 512.0f - 100.0f));
    CHECK(w.edit.centerX() == doctest::Approx(kAx + 192.0f - 20.0f));
    CHECK(w.invite.right() == doctest::Approx(kAx + 384.0f - 40.0f));
    CHECK(w.invite.bottom() == doctest::Approx(kAy + 512.0f - 82.0f));
    CHECK(w.addFriend.right() == doctest::Approx(w.invite.x));
    CHECK(w.refresh.right() == doctest::Approx(w.addFriend.x));
    CHECK(w.refresh.w == doctest::Approx(85.0f));
    CHECK(w.list.view.bottom() <= w.totals.y + 0.01f);
    CHECK(w.totals.y < w.edit.y);
    CHECK(w.edit.centerY() < w.refresh.y); // the edit box reaches 4 units into the buttons' row, as the original's
    for (const ui::Rect& r : {w.headers, w.list.view, w.totals, w.edit, w.refresh, w.addFriend, w.invite}) {
        CHECK(inside(r, l.frame));
    }
}

TEST_CASE("Friends frame art: GuildFrame's places") {
    const FriendsFrameLayout l = unitLayout();
    const GuildFrameLayout& gl = l.guild;
    CHECK(gl.headers.x == doctest::Approx(kAx + 20.0f));
    CHECK(gl.list.rowTop == doctest::Approx(kAy + 95.0f));
    CHECK(gl.list.view.h == doctest::Approx(13.0f * 16.0f)); // GUILDMEMBERS_TO_DISPLAY
    CHECK(gl.list.bar.y == doctest::Approx(kAy + 98.0f));    // GuildListScrollFrame from 98 down 237
    CHECK(gl.list.bar.h == doctest::Approx(237.0f));
    CHECK(gl.list.track[0].h == doctest::Approx(226.0f)); // its track's top part, 0.8828125 of the texture
    CHECK(gl.list.trackV == doctest::Approx(226.0f / 256.0f));
    CHECK(gl.totalsCenterX == doctest::Approx(kAx + 192.0f - 122.0f));
    CHECK(gl.totals.bottom() == doctest::Approx(kAy + 512.0f - 181.0f));
    CHECK(gl.toggle.x == doctest::Approx(kAx + 245.0f)); // LEFT (245, -67): its middle 67 under the frame's
    CHECK(gl.toggle.centerY() == doctest::Approx(kAy + 256.0f + 67.0f));
    CHECK(gl.totals.centerY() == doctest::Approx(gl.toggle.centerY()));
    CHECK(gl.motdLabel.y == doctest::Approx(kAy + 344.0f));
    CHECK(gl.motd.y == doctest::Approx(gl.motdLabel.bottom() + 3.0f));
    CHECK(gl.motd.w == doctest::Approx(315.0f));
    CHECK(gl.control.right() == doctest::Approx(kAx + 384.0f - 39.0f));
    CHECK(gl.addMember.right() == doctest::Approx(gl.control.x - 2.0f));
    CHECK(gl.information.right() == doctest::Approx(gl.addMember.x - 2.0f));
    CHECK(gl.list.view.bottom() <= gl.totals.y + 0.01f);
    CHECK(gl.motd.bottom() <= gl.control.y + 0.01f);
    for (const ui::Rect& r : {gl.headers, gl.list.view, gl.totals, gl.toggle, gl.motdLabel, gl.motd, gl.information,
                              gl.addMember, gl.control}) {
        CHECK(inside(r, l.frame));
    }
}

TEST_CASE("Friends frame art: RaidFrame's places") {
    const FriendsFrameLayout l = unitLayout();
    const RaidFrameLayout& r = l.raid;
    CHECK(r.convert.x == doctest::Approx(kAx + 72.0f));
    CHECK(r.convert.y == doctest::Approx(kAy + 37.0f));
    CHECK(r.infoAfterConvert.x == doctest::Approx(r.convert.right() + 65.0f));
    CHECK(r.addMember.x == doctest::Approx(kAx + 71.0f));
    CHECK(r.readyCheck.x == doctest::Approx(r.addMember.right() + 2.0f));
    CHECK(r.infoAfterReady.x == doctest::Approx(r.readyCheck.right() + 2.0f));
    CHECK(r.description.x == doctest::Approx(kAx + 29.0f));
    CHECK(r.description.y == doctest::Approx(kAy + 85.0f));
    // RaidGroup1 at (16, -70) with its label 14 above it; four rows of boxes 94 apart down to 432.
    CHECK(r.groups.x == doctest::Approx(kAx + 16.0f));
    CHECK(r.groups.y + r.labelH == doctest::Approx(kAy + 70.0f));
    CHECK(r.groupW == doctest::Approx(162.0f));
    CHECK(r.groupH == doctest::Approx(80.0f));
    CHECK(r.columnStep == doctest::Approx(165.0f));
    CHECK(r.rowStep == doctest::Approx(94.0f));
    CHECK(r.groups.y + 4.0f * r.rowStep == doctest::Approx(r.groups.bottom()));
    CHECK(r.groups.w == doctest::Approx(r.columnStep + r.groupW));
    for (const ui::Rect& b : {r.convert, r.infoAfterConvert, r.addMember, r.readyCheck, r.infoAfterReady,
                              r.description, r.groups}) {
        CHECK(inside(b, l.frame));
    }
    CHECK(r.convert.bottom() <= r.groups.y + r.labelH); // RaidGroup1's label grazes the buttons, as the original's
}

TEST_CASE("Friends frame art: the tabs stand along its right edge") {
    const ui::Rect place{0.0f, 0.0f, 1000.0f, 1000.0f};
    const FriendsFrameLayout l = friendsFrameLayout(place, 2.0f, false);
    // An unselected tab's top 4 units over the border (FriendsFrameTab1's 433 against its 437, turned), the column as
    // wide as the tab is high, from the lists' top down to the border's bottom.
    CHECK(l.tabs.x == doctest::Approx(l.frame.right() - 5.0f * 2.0f));
    CHECK(l.tabs.w == doctest::Approx(kFriendsTabsWidth * 2.0f));
    CHECK(l.tabs.y == doctest::Approx(l.friends.view.y));
    CHECK(l.tabs.bottom() == doctest::Approx(l.art.y + 437.0f * 2.0f));
    // The close button (above it) and every page's scroll bar stay clear of it.
    CHECK(l.close.bottom() <= l.tabs.y);
    for (const ui::Rect& bar : {l.friends.bar, l.ignore.bar, l.who.list.bar, l.guild.list.bar}) {
        CHECK(bar.right() <= l.tabs.x);
    }
    CHECK(l.tabs.right() - l.frame.x == doctest::Approx(kFriendsLayoutWidth * 2.0f));
}

TEST_CASE("Friends frame art: the side frames hang right of the tabs, or over the frame where they do not fit") {
    const FriendsFrameLayout l = friendsFrameLayout({0.0f, 0.0f, 1000.0f, 500.0f}, 1.0f, false);
    const ui::Rect screen{0.0f, 0.0f, 1200.0f, 500.0f};
    const ui::Rect side = friendsSideFrame(l, screen, 28.0f, 212.0f, 270.0f);
    CHECK(side.x >= l.tabs.right());
    CHECK(side.y == doctest::Approx(l.art.y + 28.0f));
    CHECK(side.w == doctest::Approx(212.0f));
    CHECK(side.h == doctest::Approx(270.0f));
    const ui::Rect tall = friendsSideFrame(l, screen, 65.0f, 297.0f, 900.0f);
    CHECK(tall.bottom() == doctest::Approx(l.frame.bottom())); // at most down to the frame's bottom
    const ui::Rect narrow{0.0f, 0.0f, 500.0f, 500.0f};
    const ui::Rect over = friendsSideFrame(l, narrow, 65.0f, 297.0f, 298.0f);
    CHECK(over.right() == doctest::Approx(500.0f)); // as far right as it fits, over the frame
    CHECK(inside(over, narrow));
}

TEST_CASE("Friends frame art: smaller places scale it down; over the world it is centered") {
    const f32 k = questLogScale(24.0f); // a 20:9 phone's buttons (logical pixels)
    const ui::Rect phone{6.0f, 6.0f, 600.0f, 400.0f};
    const FriendsFrameLayout l = friendsFrameLayout(phone, k, false);
    CHECK(l.k == doctest::Approx(std::min({k, 600.0f / kFriendsLayoutWidth, 400.0f / kFriendsFrameHeight})));
    CHECK(inside(l.frame, phone));
    CHECK(inside(l.tabs, phone));
    CHECK(l.frame.h >= 0.99f * 400.0f); // as tall as the slot
    const ui::Rect narrow{0.0f, 0.0f, 300.0f, 900.0f};
    const FriendsFrameLayout n = friendsFrameLayout(narrow, k, false);
    CHECK(inside(n.tabs, narrow)); // the tabs too
    CHECK(n.tabs.right() == doctest::Approx(300.0f));
    const ui::Rect screen{0.0f, 0.0f, 1200.0f, 540.0f};
    const FriendsFrameLayout c = friendsFrameLayout(screen, k, true);
    CHECK((c.frame.x + c.tabs.right()) * 0.5f == doctest::Approx(600.0f));
    CHECK(c.frame.centerY() == doctest::Approx(270.0f));
    CHECK(inside(c.frame, screen));
    // Nothing for an empty place.
    CHECK(friendsFrameLayout({0.0f, 0.0f, 0.0f, 10.0f}, k, false).k == doctest::Approx(1.0f));
}
