#include "app/RaidScreens.h"

#include "app/GameSession.h"
#include "app/PopupScreens.h"
#include "app/RaidPullouts.h"
#include "app/UnitMenuScreens.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/CharacterNames.h"
#include "game/UnitInfo.h"
#include "ui/DrawList.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

constexpr u32 kRedFont = ui::rgba(255, 26, 26);    // RED_FONT_COLOR (1.0, 0.1, 0.1)
constexpr u32 kGrayFont = ui::rgba(128, 128, 128); // GRAY_FONT_COLOR (0.5)

} // namespace

void RaidScreens::fill(GameSession& session, RaidTabView& view) {
    game::Gameplay& play = session.play();
    const game::Social& social = play.social();
    view.inRaid = social.raidMemberCount() > 0;
    view.raidInfoEnabled = !play.travel().savedInstances().empty();
    if (!view.inRaid) {
        m_count = 0;
        view.convertEnabled = game::canConvertToRaid(social.group(), session.playerGuid());
        return;
    }
    view.readyCheck = social.isRaidLeader(); // RaidFrameReadyCheckButton_Update
    view.addMemberEnabled =
        social.isRaidLeader() || social.isRaidOfficer(); // RaidFrameAddMemberButton_Update
    view.canMove = social.isRaidLeader();                // RaidGroupButton_OnDragStart
    // RaidGroupFrame_Update over GetRaidRosterInfo(1..n).
    m_count = social.raidMembers(m_roster);
    game::layoutRaidGroups({m_roster.data(), m_count}, m_slots);
    buttonViews({m_roster.data(), m_count}, m_views, m_levels);
    view.members = {m_views.data(), m_count};
    view.slots = m_slots.memberIn;
}

void RaidScreens::buttonViews(std::span<const game::PartyMemberView> roster,
                              std::span<RaidMemberButtonView> out, std::span<std::array<char, 4>> levels) {
    const usize count = std::min({roster.size(), out.size(), levels.size()});
    for (usize i = 0; i < count; ++i) {
        const game::PartyMemberView& m = roster[i];
        RaidMemberButtonView& b = out[i];
        b.key = m.guid;
        b.subgroup = m.subgroup;
        b.name = m.name.empty() ? text::get("UNKNOWN") : m.name;
        // GetRaidRosterInfo's level: UnitLevel of a member in sight, its stats' otherwise, 0 offline (""
        // then).
        const u32 level = m.inSight || m.online ? m.level : 0;
        if (level > 0) {
            std::snprintf(levels[i].data(), levels[i].size(), "%u", level);
        } else {
            levels[i][0] = '\0';
        }
        b.level = levels[i].data();
        b.className = m.cls != 0 ? game::className(m.cls) : std::string_view{};
        b.rank = m.rank() == 2   ? text::get("RAID_LEADER_TOKEN")
                 : m.rank() == 1 ? text::get("RAID_ASSISTANT_TOKEN")
                                 : std::string_view{};
        b.color = !m.online ? kGrayFont : game::raidRosterDead(m) ? kRedFont : game::raidClassColor(m.cls);
    }
}

void RaidScreens::addMember(GameSession& session, PopupScreens* popups) {
    // RaidFrameAddMemberButton's OnClick: InviteByName(UnitName("target")) when UnitCanCooperate("player",
    // "target") (two players of the same side, or the player itself), else
    // StaticPopup_Show("ADD_RAIDMEMBER").
    game::Gameplay& play = session.play();
    const game::EntityStore& entities = session.entities();
    const game::Entity* me = entities.find(session.playerGuid());
    const game::Entity* target = play.target() != 0 ? entities.find(play.target()) : nullptr;
    if (me && target && me->type == game::ObjectType::Player && target->type == game::ObjectType::Player &&
        (target->guid == me->guid ||
         game::reactionTo(*me, *target, play.reactions()) == game::Reaction::Friendly)) {
        if (const std::string* name = session.playerName(target->guid)) {
            play.social().inviteToGroup(*name);
            return;
        }
    }
    if (popups) {
        popups->askAddRaidMember(session);
    }
}

RaidScreens::Action RaidScreens::drawTab(GameSession& session, DialogContext& c, const FriendsFrameShell& f,
                                         const Links& links) {
    VD_PROFILE_ZONE("RaidScreens::drawTab");
    const u64 nowMs = c.ui.nowMs();
    if (m_lastDrawMs == 0 || nowMs - m_lastDrawMs > 250) { // RaidFrame's OnShow
        m_drag.reset();
        session.play().travel().requestRaidInfo(); // RequestRaidInfo: Raid Info's button follows the answer
    }
    m_lastDrawMs = nowMs;
    RaidTabView view;
    fill(session, view);
    const RaidTabResult r = raidTab(c, f.l, f.s, f.panel, view, m_drag);
    game::Gameplay& play = session.play();
    game::Social& social = play.social();
    const auto member = [&](i32 i) -> const game::PartyMemberView* {
        return i >= 0 && static_cast<u32>(i) < m_count ? &m_roster[static_cast<u32>(i)] : nullptr;
    };
    if (const game::PartyMemberView* m = member(r.tapped)) {
        // RaidGroupButton_OnClick's left button: TargetUnit("raidN") (no spell waits for a unit here); a member
        // the client does not see cannot be targeted.
        if (m->inSight) {
            play.setTarget(m->guid);
        }
    }
    if (const game::PartyMemberView* m = member(r.menu); m && links.menus) {
        // The right button: FriendsDropDown with RaidFrameDropDown_Initialize, UnitPopup_ShowMenu(...,
        // "RAID").
        links.menus->openRaidMenu(session, m->guid, m->name, r.menuRect.x, r.menuRect.bottom());
    }
    if (const game::PartyMemberView* m = member(r.dropMember); m && social.isRaidLeader()) {
        // RaidGroupButton_OnDragStop. The button goes back to its slot: the server's next group list moves
        // it.
        const game::RaidDrop drop =
            game::raidDrop(m_slots, {m_roster.data(), m_count}, static_cast<u32>(r.dropMember), r.dropSlot);
        if (drop.kind == game::RaidDropKind::Move) {
            social.setRaidSubgroup(m->guid, drop.subgroup); // SetRaidSubgroup
        } else if (const game::PartyMemberView* other = member(drop.other);
                   drop.kind == game::RaidDropKind::Swap && other) {
            social.swapRaidSubgroups(m->guid, other->guid); // SwapRaidSubgroup
        }
    }
    if (r.pulloutGroup >= 0 && links.pullouts) {
        links.pullouts->openGroup(session, static_cast<u8>(r.pulloutGroup), r.pulloutX, r.pulloutY);
    }
    if (const game::PartyMemberView* m = member(r.pulloutMember); m && m->cls != 0 && links.pullouts) {
        links.pullouts->openClass(session, m->cls, r.pulloutX, r.pulloutY);
    }
    if (r.convert) {
        social.convertToRaid(); // ConvertToRaid
    }
    if (r.readyCheck && social.isRaidLeader()) {
        play.commands().readyCheck(); // DoReadyCheck (the answers: the ready check's own frames)
    }
    if (r.addMember) {
        addMember(session, links.popups);
    }
    return r.raidInfo ? Action::RaidInfo : Action::None;
}

} // namespace vd
