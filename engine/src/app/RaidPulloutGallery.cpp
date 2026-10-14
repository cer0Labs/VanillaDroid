#include "app/RaidPulloutGallery.h"

#include "app/MicroMenu.h"
#include "app/MinimapView.h"
#include "app/RaidPulloutSamples.h"
#include "app/RaidPulloutWindows.h"
#include "app/SocialWindows.h"
#include "app/UiPanels.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/CharacterNames.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

// The HUD's controls as the game places them (app/UiGallery's estimates of app/Hud's formulas): the action
// cluster in the bottom right corner, and the stick's pad.
struct HudGuess {
    Rect cluster;
    Rect stickPad;
};
HudGuess guessHud(const DialogContext& c) {
    const Rect& safe = c.safeArea;
    const f32 k =
        std::clamp(std::min(safe.h / c.ui.px(430.0f), safe.w * 0.5f / c.ui.px(466.0f)), 0.72f, 1.0f);
    const f32 big = c.ui.px(76.0f) * k, margin = c.ui.px(18.0f) * k;
    const f32 cx = safe.right() - margin - big * 0.5f,
              cy = safe.bottom() - margin - big * 0.5f - c.ui.px(4.0f) * k;
    UiPanelSpace space;
    space.safe = safe;
    space.dp = c.ui.dp();
    space.unit = c.ui.px(ui::kUnitDp);
    space.wide = c.profile == ScreenProfile::Wide;
    space.framesBottom = safe.y + 100.0f * space.unit;
    const MinimapLayout map = minimapLayout(safe, space.unit, c.ui.dp());
    space.rightTop = map.top + 178.0f * map.s;
    space.cluster = {cx - c.ui.px(410.0f) * k, cy - c.ui.px(206.0f) * k, c.ui.px(410.0f) * k + big * 0.5f,
                     c.ui.px(206.0f) * k + big * 0.5f};
    const UiPanelPlaces places = uiPanelPlaces(space);
    return {space.cluster, places.stickPad};
}

} // namespace

void RaidPulloutGallery::draw(DialogContext& c, Page page, u64 nowMs) {
    VD_PROFILE_ZONE("RaidPulloutGallery::draw");
    ui::Gui g{c.ui, c.draw};
    const std::span<const RaidSampleMember> raid = raidSampleMembers();
    const f32 baseUnit = c.ui.px(ui::kUnitDp);
    if (page != m_page) { // the page's frames, as if dragged out of the raid tab along the top
        m_page = page;
        m_menuFrame = -1;
        m_frameCount = 0;
        const auto add = [&](bool byClass, u8 filter, bool buffs) {
            Frame& f = m_frames[m_frameCount++];
            f = Frame{};
            f.byClass = byClass;
            f.filter = filter;
            f.showBuffs = buffs;
        };
        if (page == Page::All) {
            for (u8 group = 0; group < 8; ++group) {
                add(false, group, false);
            }
            add(true, 5, true);  // priests
            add(true, 1, false); // warriors
            m_frames[3].background = false;
            m_menuFrame = 1;
        } else {
            add(false, 0, false);
            add(false, 2, false);
            add(false, 7, false);
            add(true, 5, true);
        }
    }
    const HudGuess hud = guessHud(c);
    g.draw.solid(hud.cluster, rgba(255, 0, 0, 50));
    g.draw.solid(hud.stickPad, rgba(0, 255, 0, 40));

    // The party frames: the others of the player's subgroup (Group 3), not the first four of the raid.
    std::array<PartyFrameView, 4> party{};
    u32 partyCount = 0;
    const RaidSampleMember& self = raid[kRaidSampleSelf];
    for (u32 i = 0; i < raid.size() && partyCount < party.size(); ++i) {
        const RaidSampleMember& m = raid[i];
        if (i == kRaidSampleSelf || m.subgroup != self.subgroup) {
            continue;
        }
        PartyFrameView& v = party[partyCount++];
        v.name = m.name;
        v.level = m.level;
        v.health = m.health;
        v.power = m.power;
        v.powerType = m.powerType;
        v.race = m.race;
        v.gender = m.gender;
        v.online = !m.offline;
        v.dead = m.dead;
        v.inSight = m.inSight;
        for (u32 d = 0; d < m.debuffCount && v.debuffCount < v.debuffs.size(); ++d) {
            v.debuffs[v.debuffCount++] = {m.debuffs[d].iconHash, m.debuffs[d].dispel};
        }
    }
    const Rect partyArea{c.safeArea.x + g.px(4.0f), c.safeArea.y + g.unit(100.0f), g.unit(128.0f) * 1.6f,
                         0.0f};
    const i32 partyTapped = partyFrames(c, partyArea, {party.data(), partyCount});
    char line[96];
    if (partyTapped >= 0) {
        std::snprintf(line, sizeof(line), "party frame %d (%.*s)", partyTapped,
                      static_cast<int>(party[static_cast<u32>(partyTapped)].name.size()),
                      party[static_cast<u32>(partyTapped)].name.data());
        logResult(line);
    }

    // The pullouts.
    std::array<RaidPulloutMemberView, kMaxRaidPulloutMembers> views{};
    std::array<u8, kMaxRaidPulloutMembers> members{};
    // Side by side right of the party frames, row after row; each placed off the HUD's controls and off the
    // frames placed before it while there is room (the game opens them where they are dropped).
    const f32 large = baseUnit * kRaidPulloutScale;
    const f32 cellW = (kRaidPulloutReach + 8.0f) * large;
    const f32 cellH =
        (kRaidPulloutTitle + 5.0f * kRaidPulloutButtonHeight + kRaidPulloutPadding + 8.0f) * large;
    const f32 left = c.safeArea.x + std::max(c.safeArea.w * 0.2f, 150.0f * large);
    const Rect space{left, c.safeArea.y + c.safeArea.h * 0.12f, std::max(c.safeArea.right() - left, cellW),
                     c.safeArea.h * 0.88f};
    std::array<Rect, 2 + kMaxRaidPullouts> avoid{};
    avoid[0] = hud.cluster;
    avoid[1] = hud.stickPad;
    u32 avoidCount = 2;
    u32 cell = 0;
    for (u32 fi = 0; fi < m_frameCount; ++fi) {
        Frame& f = m_frames[fi];
        if (!f.shown) {
            continue;
        }
        u32 count = 0;
        for (u32 i = 0; i < raid.size(); ++i) {
            if (f.byClass ? raid[i].cls == f.filter : raid[i].subgroup == f.filter) {
                members[count++] = static_cast<u8>(i);
            }
        }
        for (u32 j = 0; j < count; ++j) {
            views[j] = raidSampleView(raid[members[j]], f.showBuffs, static_cast<u32>(nowMs));
        }
        RaidPulloutGeometry at;
        at.unit = raidPulloutUnit(baseUnit, count, c.safeArea.h);
        at.members = count;
        f.unit = at.unit;
        if (!f.placed) {
            f.placed = true;
            const Rect where = raidPulloutDemoCell(cell++, space, cellW, cellH);
            f.x = where.x;
            f.y = where.y + kRaidPulloutTitle * at.unit;
            f.place = true;
        }
        at.x = f.x;
        at.y = f.y;
        const Rect hit = at.hit();
        const bool place = f.place;
        const Rect placed = raidPulloutPlace(hit, c.safeArea,
                                             place ? std::span<const Rect>(avoid.data(), avoidCount)
                                                   : std::span<const Rect>{});
        f.place = false;
        f.x += placed.x - hit.x;
        f.y += placed.y - hit.y;
        if (place && avoidCount < avoid.size()) {
            avoid[avoidCount++] = placed;
        }
        at.x = f.x;
        at.y = f.y;
        char title[48];
        RaidPulloutView v;
        if (f.byClass) {
            v.title = game::className(f.filter);
        } else {
            const std::string_view group = text::get("GROUP");
            std::snprintf(title, sizeof(title), "%.*s %u", static_cast<int>(group.size()), group.data(),
                          f.filter + 1u);
            v.title = title;
        }
        v.members = {views.data(), count};
        v.at = at;
        v.background = f.background;
        const RaidPulloutResult r = raidPullout(c, 100 + fi, v);
        if (r.moving) {
            if (!f.dragging) {
                f.dragging = true;
                f.dragX = f.x;
                f.dragY = f.y;
            }
            f.x = f.dragX + r.fingerDx;
            f.y = f.dragY + r.fingerDy;
            if (r.dropped) {
                f.dragging = false;
                f.place = true;
                std::snprintf(line, sizeof(line), "pullout %.*s dropped", static_cast<int>(v.title.size()),
                              v.title.data());
                logResult(line);
            }
        } else if (r.held) {
            m_menuFrame = static_cast<i32>(fi);
        } else if (r.aura >= 0) {
            std::snprintf(line, sizeof(line), "pullout aura %d of %s", r.aura,
                          raid[members[r.auraMember]].name);
            logResult(line);
        } else if (r.tapped >= 0) {
            std::snprintf(line, sizeof(line), "pullout target %s", raid[members[r.tapped]].name);
            logResult(line);
        }
    }

    // RaidPulloutDropDown at the frame's top right.
    if (m_menuFrame >= 0 && static_cast<u32>(m_menuFrame) < m_frameCount &&
        m_frames[static_cast<u32>(m_menuFrame)].shown) {
        Frame& f = m_frames[static_cast<u32>(m_menuFrame)];
        m_menuItems[0] = {text::get("SHOW_BUFFS"), 0, false, true, true, f.showBuffs};
        m_menuItems[1] = {text::get("SHOW_DEBUFFS"), 0, false, true, true, !f.showBuffs};
        m_menuItems[2] = {text::get("HIDE_PULLOUT_BG"), 0, false, true, true, !f.background};
        m_menuItems[3] = {text::get("CLOSE"), 0, false, true, true, false};
        DropDownMenuView v;
        v.x = f.x + kRaidPulloutWidth * f.unit;
        v.y = f.y;
        v.level1 = m_menuItems;
        const DropDownMenuResult r = dropDownMenu(c, "gallery.pullout.menu", v);
        if (r.level == 1 && r.picked >= 0) {
            if (r.picked == 0 || r.picked == 1) {
                f.showBuffs = r.picked == 0;
            } else if (r.picked == 2) {
                f.background = !f.background;
            } else {
                f.shown = false;
            }
            std::snprintf(line, sizeof(line), "pullout menu %d", r.picked);
            logResult(line);
            m_menuFrame = -1;
        } else if (r.closed) {
            m_menuFrame = -1;
        }
    }
}

} // namespace vd
