#include "app/UiPanels.h"

#include <algorithm>
#include <initializer_list>

namespace vd {

using ui::Rect;

UiPanelInfo uiPanelInfo(UiPanel panel) {
    // UIParent.lua's UIPanelWindows, plus TalentFrame (Blizzard_TalentUI), ClassTrainerFrame
    // (Blizzard_TrainerUI) and AuctionFrame (Blizzard_AuctionUI).
    switch (panel) {
    case UiPanel::GameMenu: return {UiPanelArea::Center, 0, true};
    case UiPanel::Options: return {UiPanelArea::Center, 0, true};
    case UiPanel::Character: return {UiPanelArea::Left, 2, true};
    case UiPanel::ItemText: return {UiPanelArea::Left, 0, false};
    case UiPanel::Spellbook: return {UiPanelArea::Left, 0, true};
    case UiPanel::Loot: return {UiPanelArea::Left, 7, false};
    case UiPanel::Taxi: return {UiPanelArea::Left, 0, false};
    case UiPanel::Quest: return {UiPanelArea::Left, 0, false};
    case UiPanel::QuestLog: return {UiPanelArea::Left, 0, true};
    case UiPanel::Merchant: return {UiPanelArea::Left, 0, false};
    case UiPanel::Trade: return {UiPanelArea::Left, 1, false};
    case UiPanel::Bank: return {UiPanelArea::Left, 6, false};
    case UiPanel::Friends: return {UiPanelArea::Left, 0, true};
    case UiPanel::WorldMap: return {UiPanelArea::Full, 0, true};
    case UiPanel::Mail: return {UiPanelArea::Left, 0, false};
    case UiPanel::Talents: return {UiPanelArea::Left, 6, true};
    case UiPanel::Trainer: return {UiPanelArea::Left, 0, false};
    case UiPanel::Auction: return {UiPanelArea::Doublewide, 0, false};
    case UiPanel::TradeSkill: return {UiPanelArea::Left, 3, false}; // phase 13: Blizzard_TradeSkillUI
    case UiPanel::Craft: return {UiPanelArea::Left, 4, false};      // phase 13: Blizzard_CraftUI
    case UiPanel::Inspect: return {UiPanelArea::Left, 0, false}; // phase 13: Blizzard_InspectUI's entry
    case UiPanel::PetStable: return {UiPanelArea::Left, 0, false}; // phase 15 (pets): PetStableFrame
    case UiPanel::DressUp: return {UiPanelArea::Left, 2, false}; // phase 15 (items)
    case UiPanel::GuildRegistrar: return {UiPanelArea::Left, 0, false}; // phase 15 (guild)
    case UiPanel::Petition: return {UiPanelArea::Left, 0, false};
    case UiPanel::Tabard: return {UiPanelArea::Left, 0, false};
    case UiPanel::Help: return {UiPanelArea::Center, 0, true}; // phase 15 (help)
    case UiPanel::GmSurvey: return {UiPanelArea::Center, 0, true};
    case UiPanel::Suggest: return {UiPanelArea::Center, 0, true};
    case UiPanel::SoundOptions: return {UiPanelArea::Center, 0, true}; // phase 15 (options)
    case UiPanel::UIOptions: return {UiPanelArea::Center, 0, true};
    case UiPanel::Macro: return {UiPanelArea::Left, 5, true}; // Blizzard_MacroUI's entry
    case UiPanel::Battlefield: return {UiPanelArea::Left, 0, false}; // phase 17 (queue)
    case UiPanel::WorldStateScore: return {UiPanelArea::Center, 0, true}; // phase 17 (scores)
    default: return {};
    }
}

bool uiPanelHostsBags(UiPanel panel) {
    return panel == UiPanel::Merchant || panel == UiPanel::Bank || panel == UiPanel::Mail ||
           panel == UiPanel::Trade || panel == UiPanel::Auction;
}

// --- Slots -------------------------------------------------------------------------------------------------

void UiPanelSlots::setCapacity(u32 places) {
    m_capacity = std::clamp(places, 1u, 2u);
    if (m_capacity == 1) {
        if (m_left != UiPanel::None && m_center != UiPanel::None) {
            setCenter(UiPanel::None); // the left one stays
        }
        if (m_bags && (m_left != UiPanel::None || m_center != UiPanel::None)) {
            m_bags = false;
        }
    }
}

void UiPanelSlots::hidden(UiPanel panel) {
    if (panel == UiPanel::None) {
        return;
    }
    for (u32 i = 0; i < m_hiddenCount; ++i) {
        if (m_hidden[i] == panel) {
            return;
        }
    }
    if (m_hiddenCount < m_hidden.size()) {
        m_hidden[m_hiddenCount++] = panel;
    }
}

std::span<const UiPanel> UiPanelSlots::takeHidden() {
    const u32 n = m_hiddenCount;
    for (u32 i = 0; i < n; ++i) {
        m_taken[i] = m_hidden[i];
    }
    m_hiddenCount = 0;
    return {m_taken.data(), n};
}

void UiPanelSlots::setLeft(UiPanel panel) { // SetLeftFrame
    const UiPanel old = m_left;
    m_left = panel;
    if (old != panel) {
        hidden(old);
    }
}

void UiPanelSlots::setCenter(UiPanel panel) { // SetCenterFrame
    const UiPanel old = m_center;
    m_center = panel;
    if (old != panel) {
        hidden(old);
    }
}

void UiPanelSlots::moveToCenter() { // MovePanelToCenter
    if (m_left != UiPanel::None) {
        setCenter(UiPanel::None);
        m_center = m_left;
        m_left = UiPanel::None;
    }
}

void UiPanelSlots::moveToLeft() { // MovePanelToLeft
    if (m_center != UiPanel::None) {
        setLeft(UiPanel::None);
        m_left = m_center;
        m_center = UiPanel::None;
    }
}

bool UiPanelSlots::shown(UiPanel panel) const {
    return panel != UiPanel::None &&
           (m_left == panel || m_center == panel || m_doublewide == panel || m_full == panel);
}

void UiPanelSlots::closeWindows(bool ignoreCenter) { // CloseWindows
    setLeft(UiPanel::None);
    hidden(m_full);
    m_full = UiPanel::None;
    hidden(m_doublewide);
    m_doublewide = UiPanel::None;
    if (m_center != UiPanel::None && (!ignoreCenter || uiPanelInfo(m_center).area != UiPanelArea::Center)) {
        setCenter(UiPanel::None);
    }
}

void UiPanelSlots::closeAll(bool ignoreCenter) { // CloseAllWindows
    m_bags = false;
    closeWindows(ignoreCenter);
}

UiPanelShow UiPanelSlots::show(UiPanel panel, bool dead, bool force) {
    if (panel == UiPanel::None || panel == UiPanel::Count) {
        return UiPanelShow::Refused;
    }
    if (shown(panel)) {
        return UiPanelShow::Shown; // frame:IsVisible(): nothing changes
    }
    const UiPanelInfo info = uiPanelInfo(panel);
    // CanOpenPanels(): a center window ("center" area) lets only other center windows open.
    const bool centerOpen = m_center != UiPanel::None && uiPanelInfo(m_center).area == UiPanelArea::Center;
    if (centerOpen && info.area != UiPanelArea::Center) {
        return UiPanelShow::Refused;
    }
    if (dead && !info.whileDead) {
        return UiPanelShow::Dead;
    }
    // A full-screen window ignores the others' requests, unless forced.
    if (m_full != UiPanel::None && info.area != UiPanelArea::Full) {
        if (!force) {
            return UiPanelShow::Refused;
        }
        hidden(m_full);
        m_full = UiPanel::None;
    }
    switch (info.area) {
    case UiPanelArea::Full: // full-screen windows replace each other
        closeAll();
        m_full = panel;
        return UiPanelShow::Shown;
    case UiPanelArea::Center: // center windows replace each other and close the rest
        closeWindows(false);
        m_bags = false;
        setCenter(panel);
        return UiPanelShow::Shown;
    case UiPanelArea::Doublewide: // both slots
        setLeft(UiPanel::None);
        setCenter(UiPanel::None);
        if (m_doublewide != panel) {
            hidden(m_doublewide);
        }
        m_doublewide = panel;
        m_bags = false; // ours has the bags inside it
        return UiPanelShow::Shown;
    default: break;
    }
    if (m_doublewide != UiPanel::None) {
        hidden(m_doublewide);
        m_doublewide = UiPanel::None;
    }
    if (m_left == UiPanel::None) {
        setLeft(panel);
    } else if (m_center == UiPanel::None) {
        const UiPanelInfo left = uiPanelInfo(m_left);
        if (left.pushable == 0 && info.pushable == 0) {
            setLeft(panel); // neither is pushable: it replaces the left one
        } else if (left.pushable > info.pushable) {
            moveToCenter(); // the higher priority goes to the center
            setLeft(panel);
        } else {
            setCenter(panel);
        }
    } else if (info.pushable > uiPanelInfo(m_center).pushable) {
        moveToLeft(); // the center one goes back to the left, this one to the center
        setCenter(panel);
    } else {
        setLeft(panel);
    }
    // One place (the cover screen): the window just opened is the one shown.
    if (m_capacity == 1 && m_left != UiPanel::None && m_center != UiPanel::None) {
        if (m_left == panel) {
            setCenter(UiPanel::None);
        } else {
            setLeft(UiPanel::None);
        }
    }
    // The bags share the right side with the center slot (and the only place with everything).
    if (m_bags && (m_capacity == 1 || m_center != UiPanel::None || uiPanelHostsBags(panel))) {
        m_bags = false;
    }
    return UiPanelShow::Shown;
}

void UiPanelSlots::hide(UiPanel panel) {
    if (panel == UiPanel::None) {
        return;
    }
    if (panel == m_full) {
        m_full = UiPanel::None;
        hidden(panel);
    } else if (panel == m_center) {
        setCenter(UiPanel::None);
    } else if (panel == m_left) {
        // The center window moves back to the left, unless it is a center window of its own.
        if (m_center != UiPanel::None && uiPanelInfo(m_center).area == UiPanelArea::Left) {
            moveToLeft();
        } else {
            setLeft(UiPanel::None);
        }
    } else if (panel == m_doublewide) {
        m_doublewide = UiPanel::None;
        hidden(panel);
    }
}

bool UiPanelSlots::openBags() {
    for (const UiPanel shownPanel : {m_left, m_center, m_doublewide}) {
        if (uiPanelHostsBags(shownPanel)) {
            return false; // that window shows the bags already
        }
    }
    if (m_capacity == 1) {
        closeWindows(false); // the one place
    } else {
        setCenter(UiPanel::None); // the right side
        hidden(m_doublewide);
        m_doublewide = UiPanel::None;
        hidden(m_full);
        m_full = UiPanel::None;
    }
    m_bags = true;
    return true;
}

void UiPanelSlots::closeBags() {
    m_bags = false;
}

// --- Places ------------------------------------------------------------------------------------------------

bool uiPanelWide(const Rect& place) {
    return place.w >= place.h * 1.4f;
}

bool uiPanelOverlaps(const Rect& a, const Rect& b) {
    return a.w > 0.0f && a.h > 0.0f && b.w > 0.0f && b.h > 0.0f && a.x < b.right() && b.x < a.right() &&
           a.y < b.bottom() && b.y < a.bottom();
}

UiPanelPlaces uiPanelPlaces(const UiPanelSpace& s) {
    const Rect& safe = s.safe;
    const f32 gap = 6.0f * s.dp;
    const f32 bottom = safe.bottom() - 13.0f * s.unit - gap; // over MainMenuExpBar along the bottom edge
    const f32 top = std::max(safe.y, s.framesBottom) + gap;
    UiPanelPlaces p;
    if (s.wide) {
        // Ours: one place as big as the screen allows (the windows read better than the stick reaches), from the left
        // and top edges down to the experience bar, left of the action buttons; over the stick's column and the unit
        // frames (they hide under the window).
        const f32 x = safe.x + gap, y = safe.y + gap;
        f32 right = (s.cluster.w > 0.0f ? s.cluster.x : safe.right()) - gap;
        f32 low = bottom;
        p.leftWide = {x, y, std::max(0.0f, right - x), std::max(0.0f, low - y)};
        // The stance bar beside the cluster's bottom row: narrower when that leaves enough, shorter otherwise.
        if (s.stance.w > 0.0f && s.stance.x < right && s.stance.right() > x && s.stance.bottom() > y &&
            s.stance.y < low) {
            p.leftWide.h = std::max(0.0f, s.stance.y - gap - y);
            if (s.stance.x - gap - x >= 260.0f * s.dp) {
                right = s.stance.x - gap;
            } else {
                low = s.stance.y - gap;
            }
        }
        p.left = {x, y, std::max(0.0f, right - x), std::max(0.0f, low - y)};
        p.both = p.left;
        // Where the stick floats with no window up: a column along the left edge, under the unit frames.
        const f32 pad = std::clamp(safe.w * 0.15f, 120.0f * s.dp, 200.0f * s.dp);
        p.stickPad = {safe.x, top, pad, safe.bottom() - top};
        p.overFrames = true;
        return p;
    }
    // Two places side by side over the stick's band (the lower fourth: the left thumb rests in it) and the
    // action buttons.
    const f32 stickTop = safe.bottom() - std::max(95.0f * s.unit, safe.h * 0.23f);
    const f32 split = s.cluster.w > 0.0f ? std::min(safe.centerX(), s.cluster.x) : safe.centerX();
    const f32 leftX = safe.x + gap;
    const f32 leftTop = std::max(top, s.leftTop + gap);
    p.left = {leftX, leftTop, std::max(0.0f, split - gap * 0.5f - leftX), std::max(0.0f, stickTop - leftTop)};
    p.leftWide = p.left;
    p.stickPad = {safe.x, stickTop, std::max(0.0f, split - safe.x), safe.bottom() - stickTop};
    // The right one: under the minimap and the micro menu, over the cluster and the stance bar.
    const f32 rightX = split + gap * 0.5f;
    const f32 rightTop = std::max(top, s.rightTop + gap);
    f32 rightBottom = s.cluster.w > 0.0f ? s.cluster.y - gap : bottom;
    if (s.stance.w > 0.0f && s.stance.right() > rightX) {
        rightBottom = std::min(rightBottom, s.stance.y - gap);
    }
    const Rect right{rightX, rightTop, std::max(0.0f, safe.right() - gap - rightX),
                     std::max(0.0f, rightBottom - rightTop)};
    if (right.w >= 280.0f * s.dp && right.h >= 170.0f * s.dp) {
        p.right = right;
    }
    // Both slots together reach up to the frames' line (the original's full-screen map hides the minimap and
    // the micro menu too).
    const f32 bothBottom = p.right.w > 0.0f ? std::min(p.left.bottom(), p.right.bottom()) : p.left.bottom();
    const f32 bothRight = p.right.w > 0.0f ? p.right.right() : p.left.right();
    p.both = {p.left.x, top, std::max(0.0f, bothRight - p.left.x), std::max(0.0f, bothBottom - top)};
    return p;
}

} // namespace vd
