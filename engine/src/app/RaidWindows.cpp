#include "app/RaidWindows.h"

#include "app/DialogKit.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::widgetId;
using namespace kit;

constexpr u64 kGroupOutline = assetPathHash("Interface\\RaidFrame\\UI-RaidFrame-GroupOutline.blp");
constexpr u64 kGroupButton = assetPathHash("Interface\\RaidFrame\\UI-RaidFrame-GroupButton.blp");
constexpr ui::TexCoords kOutlineUv{0.0f, 0.0f, 0.6640625f, 0.625f}; // RaidGroupTemplate's texture
constexpr ui::TexCoords kButtonUv{0.0f, 0.0f, 0.640625f, 0.4375f};  // the button's NormalTexture
constexpr ui::TexCoords kHighlightUv{0.0f, 0.46875f, 0.640625f,
                                     0.90625f}; // its and the slots' HighlightTexture
constexpr u32 kEmptyText = rgba(89, 89, 89);    // GameFontDarkGraySmall (0.35)

// The templates' sizes in interface units: RaidGroupTemplate 162 x 80, its slots 156 x 14 from (3, 3) one
// unit apart (UI-RaidFrame-GroupOutline draws their five frames: the boxes keep its proportions when they
// grow).
constexpr f32 kGroupW = 162.0f, kGroupH = 80.0f, kSlotW = 156.0f, kSlotH = 14.0f, kSlotStep = 15.0f,
              kInset = 3.0f;
constexpr u32 kGroups = 8, kPerGroup = 5;

constexpr ui::WidgetId kListId = widgetId("raid.list");
constexpr ui::WidgetId kDescriptionId = widgetId("raid.description");

// Where the boxes are this frame (logical pixels, the scroll offset included).
struct Layout {
    u32 columns = 2;
    f32 groupW = 0.0f, groupH = 0.0f, labelH = 0.0f, rowStep = 0.0f, colGap = 0.0f;
    f32 left = 0.0f, top = 0.0f;

    Rect group(u32 g) const {
        const u32 col = g % columns, row = g / columns;
        return {left + static_cast<f32>(col) * (groupW + colGap),
                top + static_cast<f32>(row) * rowStep + labelH, groupW, groupH};
    }
    // RaidGroupNSlotM (slot = group * 5 + place).
    Rect slot(u32 s) const {
        const Rect g = group(s / kPerGroup);
        const f32 kx = g.w / kGroupW, ky = g.h / kGroupH;
        return {g.x + kInset * kx, g.y + (kInset + kSlotStep * static_cast<f32>(s % kPerGroup)) * ky,
                kSlotW * kx, kSlotH * ky};
    }
    // RaidGroupNLabel: 156 x 14 with its TOP 14 units over the box's.
    Rect label(u32 g) const {
        const Rect r = group(g);
        return {r.x + kInset * (r.w / kGroupW), r.y - labelH, kSlotW * (r.w / kGroupW), labelH};
    }
    f32 height() const { return static_cast<f32>(kGroups / columns) * rowStep; }
};

// RaidGroup1..8 two across 3 units apart, each box 14 under the one above (its label in the gap); a slot a finger
// high at least (the box taller, its art stretched).
Layout layoutFor(const Gui& g, const FriendsFrameLayout& frame, const QuestArtSizes& sizes, const Rect& viewport,
                 f32 top) {
    Layout l;
    const RaidFrameLayout& r = frame.raid;
    l.columns = 2;
    l.colGap = r.columnStep - r.groupW;
    l.groupW = r.groupW;
    const f32 slotH = std::max(kSlotH * frame.k, std::max(g.px(24.0f), sizes.small * 1.6f));
    l.groupH = slotH * kGroupH / kSlotH;
    l.labelH = std::max(r.labelH, sizes.small * 1.4f);
    l.rowStep = l.labelH + l.groupH;
    l.left = viewport.x;
    l.top = top;
    return l;
}

// RaidGroupButtonTemplate: its art, then the rank token, the name, the level and the class at the template's
// offsets (Rank 18 wide at 1, Name 60 wide 2 after it, Level 22 wide 5 after that, centered; Class from 110).
void memberButton(Gui& g, const Rect& s, const RaidMemberButtonView& m, f32 font, bool lit) {
    g.draw.sprite(s, kGroupButton, ui::kWhite, kButtonUv);
    if (lit) {
        g.draw.spriteAdditive(s, kGroupButton, ui::kWhite, kHighlightUv);
    }
    const f32 k = s.w / kSlotW, up = s.h / kSlotH; // the rank and the class sit one unit up
    const u32 color = m.color != 0 ? m.color : ui::kGold;
    const TextStyle text = ui::gameFont(font, color);
    ui::label(g, {s.x + 1.0f * k, s.y - up, 18.0f * k, s.h}, m.rank, ui::gameFont(font));
    ui::label(g, {s.x + 21.0f * k, s.y, 60.0f * k, s.h}, m.name, text);
    TextStyle level = text;
    level.align = Align::Center;
    ui::label(g, {s.x + 86.0f * k, s.y, 22.0f * k, s.h}, m.level, level);
    ui::label(g, {s.x + 110.0f * k, s.y - up, s.w - 110.0f * k, s.h}, m.className, text);
}

// The class text's own button (RaidGroupButtonNClass: 50 wide from 110, over the member's button).
Rect classRect(const Rect& s) {
    const f32 k = s.w / kSlotW;
    return {s.x + 110.0f * k, s.y, s.w - 110.0f * k, s.h};
}

i32 memberByKey(std::span<const RaidMemberButtonView> members, u64 key) {
    for (u32 i = 0; i < members.size(); ++i) {
        if (members[i].key == key) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

// "Group N" (RaidGroupTemplate's OnLoad: GROUP.." "..id).
std::string_view groupLabel(std::span<char> out, u32 group) {
    const std::string_view word = text::get("GROUP");
    const int n = std::snprintf(out.data(), out.size(), "%.*s %u", static_cast<int>(word.size()), word.data(),
                                group + 1);
    return {out.data(), static_cast<usize>(std::clamp(n, 0, static_cast<int>(out.size()) - 1))};
}

// What a lifted label or class text looks like under the finger: the pullout's name over a small backdrop
// (RaidPulloutFrameTemplate's MenuBackdrop and its Name), its top centre above the finger.
void pulloutGhost(Gui& g, f32 x, f32 y, std::string_view name, f32 font) {
    const f32 w = std::max(g.unit(72.0f) * 1.5f, g.draw.measure(name, ui::FontId::Friz, font) + g.px(16.0f));
    const Rect r{x - w * 0.5f, y - g.px(56.0f), w, font * 2.2f};
    ui::tooltipPanel(g, r);
    TextStyle s = ui::gameFont(font);
    s.align = Align::Center;
    ui::label(g, r, name, s);
}

} // namespace

RaidTabResult raidTab(DialogContext& c, const FriendsFrameLayout& frame, const QuestArtSizes& sizes, const Rect& window,
                      const RaidTabView& v, RaidTabDrag& drag) {
    VD_PROFILE_ZONE("RaidWindows::raidTab");
    Gui g{c.ui, c.draw};
    const RaidFrameLayout& rl = frame.raid;
    RaidTabResult result;
    const u64 nowMs = c.ui.nowMs();

    // The finger of a lifted button, label or class text: where it is, whether it moved since.
    const ui::Touch* finger = nullptr;
    if (drag.kind != RaidTabDrag::Kind::None) {
        for (const ui::Touch& t : c.ui.touches()) {
            if (t.id == drag.touch && t.downMs == drag.touchDownMs) {
                finger = &t;
            }
        }
        if (!finger || finger->cancelled || !v.inRaid ||
            (drag.kind != RaidTabDrag::Kind::Group && memberByKey(v.members, drag.key) < 0)) {
            drag.reset(); // gone without a release, out of the raid, or the member left
        } else {
            drag.x = finger->x;
            drag.y = finger->y;
            drag.moving = drag.moving || finger->moved;
        }
    }

    // The buttons in the band under the title. Out of a raid: Convert To Raid, then Raid Info 65 units right of it.
    // In a raid: Add Member, Ready Check (the leader) and Raid Info right after it (RaidGroupFrame_Update moves it
    // there), or where it was without Ready Check.
    {
        Rect info = rl.infoAfterConvert;
        if (!v.inRaid) {
            result.convert =
                ui::button(g, widgetId("raid.convert"), rl.convert, text::get("CONVERT_TO_RAID"), v.convertEnabled);
        } else {
            result.addMember =
                ui::button(g, widgetId("raid.add"), rl.addMember, text::get("ADD_RAID_MEMBER"), v.addMemberEnabled);
            if (v.readyCheck) {
                result.readyCheck = ui::button(g, widgetId("raid.readyCheck"), rl.readyCheck, text::get("READY_CHECK"));
                info = rl.infoAfterReady;
            }
        }
        result.raidInfo = ui::button(g, widgetId("raid.info"), info, text::get("RAID_INFO"), v.raidInfoEnabled);
    }

    // Out of a raid: the explanation (RaidFrameRaidDescription, GameFontNormal with its white lines, 300 wide).
    if (!v.inRaid) {
        const Rect& box = rl.description;
        const ui::ScrollView view = ui::beginScroll(g, kDescriptionId, box);
        const TextStyle style = ui::gameFont(sizes.normal);
        const f32 h = g.draw.wrappedText({box.x, view.top, box.w, 1e6f}, text::get("RAID_DESCRIPTION"), style);
        ui::endScroll(g, view, h + sizes.normal);
        return result;
    }

    // In a raid: the eight boxes.
    const Rect viewport = rl.groups;
    const ui::ScrollView view = ui::beginScroll(g, kListId, viewport);
    const Layout l = layoutFor(g, frame, sizes, viewport, view.top);
    const f32 font = std::min(sizes.small, l.slot(0).h * 0.72f);
    const bool draggingMember = drag.kind == RaidTabDrag::Kind::Member && drag.moving;
    drag.targetSlot = -1;
    char label[48];
    for (u32 group = 0; group < kGroups; ++group) {
        const Rect box = l.group(group);
        if (box.bottom() < viewport.y - l.labelH || box.y - l.labelH > viewport.bottom()) {
            continue; // scrolled out
        }
        g.draw.sprite(box, kGroupOutline, ui::kWhite, kOutlineUv);
        // The label: GameFontNormalSmall, lifted to drag a pullout of the group out.
        const Rect labelRect = l.label(group);
        const ui::WidgetState ls = ui::hit(g, widgetId("raid.label", group), labelRect);
        TextStyle labelStyle = ui::gameFont(font, ui::held(ls, labelRect) ? ui::kWhite : ui::kGold);
        labelStyle.align = Align::Center;
        ui::label(g, labelRect, groupLabel(label, group), labelStyle);
        if (ls.longPress && ls.touch && drag.kind == RaidTabDrag::Kind::None) {
            drag.kind = RaidTabDrag::Kind::Group;
            drag.group = static_cast<u8>(group);
            drag.touch = ls.touch->id;
            drag.touchDownMs = ls.touch->downMs;
            drag.x = ls.touch->x;
            drag.y = ls.touch->y;
        }
        for (u32 place = 0; place < kPerGroup; ++place) {
            const u32 slot = group * kPerGroup + place;
            const Rect s = l.slot(slot);
            const i32 member = slot < v.slots.size() ? v.slots[slot] : -1;
            const bool lifted = member >= 0 && drag.kind == RaidTabDrag::Kind::Member &&
                                v.members[static_cast<u32>(member)].key == drag.key;
            // RaidGroupFrame_OnUpdate: the slot under the moving button lights (LockHighlight).
            if (draggingMember && viewport.contains(drag.x, drag.y) && s.contains(drag.x, drag.y)) {
                drag.targetSlot = static_cast<i32>(slot);
            }
            if (member < 0 || static_cast<u32>(member) >= v.members.size() || (lifted && drag.moving)) {
                TextStyle empty = ui::gameFont(font, kEmptyText); // RaidGroupSlotTemplate's EMPTY
                empty.align = Align::Center;
                ui::label(g, s, text::get("EMPTY"), empty);
            } else {
                const RaidMemberButtonView& mv = v.members[static_cast<u32>(member)];
                const ui::WidgetState st = ui::hit(g, widgetId("raid.member", mv.key), s);
                const Rect classArea = classRect(s);
                const ui::WidgetState cs = ui::hit(g, widgetId("raid.class", mv.key), classArea);
                memberButton(g, s, mv, font, lifted || ui::held(st, s));
                if (ui::tapped(st) && drag.kind == RaidTabDrag::Kind::None) {
                    result.tapped = member;
                }
                if (st.longPress && st.touch && drag.kind == RaidTabDrag::Kind::None) {
                    if (v.canMove) { // RaidGroupButton_OnDragStart: the leader's
                        drag.kind = RaidTabDrag::Kind::Member;
                        drag.key = mv.key;
                        drag.touch = st.touch->id;
                        drag.touchDownMs = st.touch->downMs;
                        drag.x = st.touch->x;
                        drag.y = st.touch->y;
                        drag.from = s;
                    } else {
                        result.menu = member;
                        result.menuRect = s;
                    }
                }
                if (cs.longPress && cs.touch && drag.kind == RaidTabDrag::Kind::None &&
                    !mv.className.empty()) {
                    drag.kind = RaidTabDrag::Kind::Class;
                    drag.key = mv.key;
                    drag.touch = cs.touch->id;
                    drag.touchDownMs = cs.touch->downMs;
                    drag.x = cs.touch->x;
                    drag.y = cs.touch->y;
                }
            }
            if (static_cast<i32>(slot) == drag.targetSlot) {
                g.draw.spriteAdditive(s, kGroupButton, ui::kWhite, kHighlightUv);
            }
        }
    }
    ui::endScroll(g, view, l.height());

    // A member dragged to the top or the bottom edge of the list (or past it) scrolls it, once held there a
    // moment (a drop on the last row shown does not scroll it away).
    f32 into = 0.0f;
    if (draggingMember && drag.x >= viewport.x && drag.x <= viewport.right()) {
        const f32 edge = std::min(g.px(22.0f), viewport.h * 0.15f);
        into = drag.y < viewport.y + edge          ? drag.y - (viewport.y + edge)
               : drag.y > viewport.bottom() - edge ? drag.y - (viewport.bottom() - edge)
                                                   : 0.0f;
        into = std::clamp(into / edge, -1.0f, 1.0f);
    }
    if (into == 0.0f) {
        drag.edgeSinceMs = 0;
    } else if (drag.edgeSinceMs == 0) {
        drag.edgeSinceMs = nowMs;
    } else if (nowMs - drag.edgeSinceMs >= 250 && drag.lastMs != 0 && nowMs > drag.lastMs) {
        const f32 dt = std::min(0.1f, static_cast<f32>(nowMs - drag.lastMs) / 1000.0f);
        ui::scrollTo(kListId, view.offset + into * g.px(420.0f) * dt);
    }
    drag.lastMs = nowMs;

    // What is being dragged, over everything.
    if (drag.kind == RaidTabDrag::Kind::Member && drag.moving) {
        const i32 member = memberByKey(v.members, drag.key);
        if (member >= 0) {
            const Rect s = l.slot(0);
            memberButton(g, {drag.x - s.w * 0.5f, drag.y - s.h * 0.5f, s.w, s.h},
                         v.members[static_cast<u32>(member)], font, true);
        }
    } else if (drag.kind == RaidTabDrag::Kind::Group) { // lifted: what will come out, moving or not yet
        pulloutGhost(g, drag.x, drag.y, groupLabel(label, drag.group), font);
    } else if (drag.kind == RaidTabDrag::Kind::Class) {
        const i32 member = memberByKey(v.members, drag.key);
        if (member >= 0) {
            pulloutGhost(g, drag.x, drag.y, v.members[static_cast<u32>(member)].className, font);
        }
    }

    // The finger lifted: the drop (RaidGroupButton_OnDragStop, RaidPulloutStopMoving), or the menu of a
    // button only lifted.
    if (finger && finger->released && drag.kind != RaidTabDrag::Kind::None) {
        const i32 member = drag.kind == RaidTabDrag::Kind::Group ? -1 : memberByKey(v.members, drag.key);
        const bool outside = !window.contains(drag.x, drag.y);
        switch (drag.kind) {
        case RaidTabDrag::Kind::Member:
            if (!drag.moving) {
                result.menu = member;
                result.menuRect = drag.from;
            } else {
                result.dropMember = member;
                result.dropSlot = drag.targetSlot;
            }
            break;
        case RaidTabDrag::Kind::Group:
            if (drag.moving && outside) {
                result.pulloutGroup = drag.group;
            }
            break;
        case RaidTabDrag::Kind::Class:
            if (drag.moving && outside) {
                result.pulloutMember = member;
            }
            break;
        case RaidTabDrag::Kind::None: break;
        }
        result.pulloutX = drag.x;
        result.pulloutY = drag.y;
        drag.reset();
    }
    return result;
}

} // namespace vd
