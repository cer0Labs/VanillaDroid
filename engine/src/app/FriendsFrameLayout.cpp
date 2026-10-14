#include "app/FriendsFrameLayout.h"

#include <algorithm>

namespace vd {

FriendsFrameLayout friendsFrameLayout(const ui::Rect& place, f32 k, bool center) {
    FriendsFrameLayout l;
    if (place.w <= 0.0f || place.h <= 0.0f || k <= 0.0f) {
        return l;
    }
    k = std::min({k, place.w / kFriendsLayoutWidth, place.h / kFriendsFrameHeight});
    l.k = k;
    const f32 w = kFriendsLayoutWidth * k, h = kFriendsFrameHeight * k;
    const f32 x = center ? place.centerX() - w * 0.5f : place.x;
    const f32 y = center ? place.centerY() - h * 0.5f : place.y;
    l.art = {x - kFriendsFrameLeft * k, y, 384.0f * k, 512.0f * k};
    // The picture's units (from its top left, y down) in logical pixels; its bottom is 512.
    const auto at = [&](f32 ux, f32 uy, f32 uw, f32 uh) {
        return ui::Rect{l.art.x + ux * k, l.art.y + uy * k, uw * k, uh * k};
    };
    // A scroll frame 296 wide with its right 67 in from the picture's (all of the pages'), `top` down and `height`
    // high: its bar 6 right of it, its track around the bar.
    constexpr f32 kListX = 384.0f - 67.0f - 296.0f;
    const auto list = [&](ui::Rect view, f32 rowX, f32 rowTop, f32 top, f32 height, f32 trackTopH) {
        FriendsListLayout s;
        s.view = view;
        s.rowX = l.art.x + rowX * k;
        s.rowW = 298.0f * k;
        s.rowTop = l.art.y + rowTop * k;
        s.bar = at(kListX + 296.0f + 6.0f, top, 16.0f, height);
        s.track = {at(kListX + 296.0f - 2.0f, top - 5.0f, 31.0f, trackTopH),
                   at(kListX + 296.0f - 2.0f, top + height + 2.0f - 106.0f, 31.0f, 106.0f)};
        s.trackV = trackTopH / 256.0f;
        return s;
    };
    l.frame = at(kFriendsFrameLeft, 0.0f, kFriendsFrameRight - kFriendsFrameLeft, kFriendsFrameHeight);
    l.portrait = at(7.0f, 6.0f, 60.0f, 60.0f);
    l.title = at(192.0f - 117.5f, 16.0f, 235.0f, 16.0f); // a 12-unit line from 18 down, centered across
    l.close = at(384.0f - 30.0f - 32.0f, 8.0f, 32.0f, 32.0f);
    l.tabs = at(kFriendsTabsLeft, 75.0f, kFriendsTabsWidth, 437.0f - 75.0f);

    // FriendsListFrame and IgnoreListFrame.
    l.toggleTabs = at(70.0f, 39.0f, kListX + 296.0f - 70.0f, 32.0f);
    l.friends = list(at(kListX, 75.0f, 296.0f, 304.0f), 23.0f, 76.0f, 75.0f, 304.0f, 256.0f);
    l.ignore = list(at(kListX, 75.0f, 296.0f, 332.0f), 23.0f, 80.0f, 75.0f, 332.0f, 256.0f);
    l.addFriend = at(17.0f, 512.0f - 107.0f - 21.0f, 131.0f, 21.0f);
    l.removeFriend = at(17.0f, 512.0f - 107.0f + 5.0f, 131.0f, 21.0f);
    l.sendMessage = at(17.0f + 131.0f + 66.0f, 512.0f - 107.0f - 21.0f, 131.0f, 21.0f);
    l.groupInvite = at(17.0f + 131.0f + 66.0f, 512.0f - 107.0f + 5.0f, 131.0f, 21.0f);
    l.ignorePlayer = at(17.0f, 512.0f - 81.0f - 21.0f, 131.0f, 21.0f);
    l.stopIgnore = at(17.0f + 131.0f + 62.0f, 512.0f - 81.0f - 21.0f, 131.0f, 21.0f);

    // WhoFrame: the 17 rows (16 high) under the headers, the scroll frame from 96 down 287 (beside the totals too).
    l.who.headers = at(20.0f, 70.0f, kListX + 296.0f - 20.0f, 24.0f);
    l.who.list = list(at(15.0f, 95.0f, kListX + 296.0f - 15.0f, 17.0f * 16.0f), 15.0f, 95.0f, 96.0f, 287.0f, 256.0f);
    l.who.totals = at(192.0f - 10.0f - 149.0f, 512.0f - 127.0f - 16.0f, 298.0f, 16.0f);
    l.who.edit = at(192.0f - 20.0f - 148.0f, 512.0f - 100.0f - 32.0f, 296.0f, 32.0f);
    l.who.invite = at(384.0f - 40.0f - 120.0f, 512.0f - 82.0f - 22.0f, 120.0f, 22.0f);
    l.who.addFriend = at(384.0f - 40.0f - 240.0f, 512.0f - 82.0f - 22.0f, 120.0f, 22.0f);
    l.who.refresh = at(384.0f - 40.0f - 240.0f - 85.0f, 512.0f - 82.0f - 22.0f, 85.0f, 22.0f);

    // GuildFrame: 13 rows of 16 under the headers, the scroll frame from 98 down 237 (its track's top part 226 high);
    // the totals and the view's toggle on one line at the list's foot, the message of the day under the list.
    l.guild.headers = at(20.0f, 70.0f, kListX + 296.0f - 20.0f, 24.0f);
    l.guild.list = list(at(15.0f, 95.0f, kListX + 296.0f - 15.0f, 13.0f * 16.0f), 15.0f, 95.0f, 98.0f, 237.0f, 226.0f);
    l.guild.totalsCenterX = l.art.x + (192.0f - 122.0f) * k;
    l.guild.totals = at(15.0f, 512.0f - 181.0f - 16.0f, 230.0f, 16.0f);
    l.guild.toggle = at(245.0f, 256.0f + 67.0f - 16.0f, 32.0f, 32.0f);
    l.guild.motdLabel = at(23.0f, 344.0f, 225.0f, 11.0f);
    l.guild.motd = at(23.0f, 344.0f + 11.0f + 3.0f, 315.0f, 45.0f);
    l.guild.control = at(384.0f - 39.0f - 104.0f, 512.0f - 82.0f - 22.0f, 104.0f, 22.0f);
    l.guild.addMember = at(384.0f - 39.0f - 104.0f - 2.0f - 98.0f, 512.0f - 82.0f - 22.0f, 98.0f, 22.0f);
    l.guild.information = at(384.0f - 39.0f - 104.0f - 2.0f - 98.0f - 2.0f - 123.0f, 512.0f - 82.0f - 22.0f, 123.0f, 22.0f);

    // RaidFrame: the buttons in the band under the title, the description or the eight groups under them.
    l.raid.convert = at(72.0f, 37.0f, 115.0f, 21.0f);
    l.raid.infoAfterConvert = at(72.0f + 115.0f + 65.0f, 37.0f, 90.0f, 21.0f);
    l.raid.addMember = at(71.0f, 37.0f, 90.0f, 21.0f);
    l.raid.readyCheck = at(71.0f + 90.0f + 2.0f, 37.0f, 90.0f, 21.0f);
    l.raid.infoAfterReady = at(71.0f + 90.0f + 2.0f + 90.0f + 2.0f, 37.0f, 90.0f, 21.0f);
    l.raid.description = at(29.0f, 85.0f, 300.0f, 432.0f - 85.0f);
    l.raid.groups = at(16.0f, 70.0f - 14.0f, 162.0f * 2.0f + 3.0f, 432.0f - 70.0f + 14.0f);
    l.raid.groupW = 162.0f * k;
    l.raid.groupH = 80.0f * k;
    l.raid.columnStep = (162.0f + 3.0f) * k;
    l.raid.rowStep = (80.0f + 14.0f) * k;
    l.raid.labelH = 14.0f * k;
    return l;
}

ui::Rect friendsSideFrame(const FriendsFrameLayout& l, const ui::Rect& bounds, f32 top, f32 w, f32 h) {
    const f32 y = l.art.y + top * l.k;
    h = std::min(h, l.frame.bottom() - y);
    w = std::min(w, bounds.w);
    f32 x = l.tabs.right() + 2.0f * l.k;
    if (x + w > bounds.right()) {
        x = std::max(bounds.x, bounds.right() - w);
    }
    return {x, y, w, std::max(0.0f, h)};
}

} // namespace vd
