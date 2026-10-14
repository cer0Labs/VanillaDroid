// UI gallery pages of the raid tab (phase 16, raidframe): VD_UI_GALLERY=raid (the player leads a raid of 40),
// raid2 (the player is one of its members), raid3 (the player leads 37: empty slots to move members to) and
// raid0 (a party: Convert To Raid). Names are English sample data; the tab's words come from their keys as in
// the game (VD_LOCALE=esES shows them in Spanish).
#include "app/RaidGallery.h"

#include "app/RaidScreens.h"
#include "app/SocialWindows.h"
#include "core/Log.h"
#include "core/Text.h"

#include <cstdio>
#include <iterator>
#include <string>
#include <utility>

namespace vd {
namespace {

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: raid %s", what);
}

// The sample raid in GetRaidRosterInfo order (the server's join order, the player last).
constexpr std::string_view kNames[game::kMaxRaidMembers] = {
    "Aeryn",     "Brakk",  "Tolvir",  "Miriel",   "Galen",   "Thrallita", "Brunhilda", "Sombrio",
    "Hojaverde", "Ugha",   "Tinka",   "Kaelos",   "Morgrim", "Selenya",   "Draxx",     "Fenwick",
    "Ishara",    "Jorund", "Korran",  "Lysandra", "Maelis",  "Nadrel",    "Orvyn",     "Perrin",
    "Quillan",   "Rhovan", "Sylvara", "Taldren",  "Ulric",   "Vaelith",   "Wystan",    "Xandros",
    "Yrsa",      "Zeldra", "Aldwin",  "Bryndis",  "Cedric",  "Dagny",     "Evander",   "Arthel"};
constexpr u8 kClasses[] = {1, 2, 3, 4, 5, 7, 8, 9, 11}; // every class of 1.12, in turn

} // namespace

bool RaidGallery::select(std::string_view page) {
    if (page == "raid") {
        m_page = Page::Leader;
    } else if (page == "raid0") {
        m_page = Page::NotInRaid;
    } else if (page == "raid2") {
        m_page = Page::Member;
    } else if (page == "raid3") {
        m_page = Page::Partial;
    } else {
        return false;
    }
    m_built = false;
    return true;
}

void RaidGallery::build() {
    m_count = game::kMaxRaidMembers;
    for (u32 i = 0; i < m_count; ++i) {
        game::PartyMemberView& m = m_roster[i];
        m = game::PartyMemberView{};
        m.guid = 0x100 + i;
        m.name = kNames[i];
        m.cls = kClasses[i % std::size(kClasses)];
        m.level = 60 - (i % 7 == 3 ? 2u : 0u) - (i % 11 == 5 ? 1u : 0u);
        m.subgroup = static_cast<u8>((i * 3) % game::kRaidGroups); // five in each group, not in join order
        m.online = true;
        m.inSight = i % 4 != 0;
        m.health = m.maxHealth = 4000;
    }
    game::PartyMemberView& self = m_roster[m_count - 1];
    self.self = true;
    self.inSight = true;
    // The leader (the player on the "raid" page, Aeryn on "raid2"), two assistants, one dead, one offline and
    // one whose class has not come yet (its name query is out).
    (m_page == Page::Member ? m_roster[0] : self).leader = true;
    m_roster[1].assistant = true;
    m_roster[3].assistant = true;
    m_roster[4].health = 0; // Galen, in sight at 0 health
    m_roster[4].inSight = true;
    m_roster[2].online = false; // Tolvir
    m_roster[2].inSight = false;
    m_roster[38].cls = 0;          // Evander
    if (m_page == Page::Partial) { // 37: groups 3, 5 and 8 with an empty slot each (moves can be tried)
        m_roster[36] = self;
        m_count = 37;
    }
    m_built = true;
}

void RaidGallery::fill(RaidTabView& view) {
    view.inRaid = m_page != Page::NotInRaid;
    view.raidInfoEnabled = true;
    view.convertEnabled = !view.inRaid; // the party's leader
    if (!view.inRaid) {
        return;
    }
    const bool leader = m_page == Page::Leader || m_page == Page::Partial;
    view.readyCheck = leader;
    view.addMemberEnabled = leader; // a plain member: disabled
    view.canMove = leader;
    game::layoutRaidGroups({m_roster.data(), m_count}, m_slots);
    RaidScreens::buttonViews({m_roster.data(), m_count}, m_views, m_levels);
    view.members = {m_views.data(), m_count};
    view.slots = m_slots.memberIn;
}

void RaidGallery::draw(DialogContext& c, u64 nowMs) {
    if (!m_built) {
        build();
    }
    // The "server" answers a move or a swap with its next group list.
    if (m_pendingMember >= 0 && nowMs >= m_pendingAtMs) {
        game::PartyMemberView& m = m_roster[static_cast<u32>(m_pendingMember)];
        if (m_pendingOther >= 0) {
            std::swap(m.subgroup, m_roster[static_cast<u32>(m_pendingOther)].subgroup);
        } else if (m_slots.filled[m_pendingGroup] < game::kRaidGroupSize) {
            m.subgroup = m_pendingGroup;
        }
        m_pendingMember = -1;
    }
    // Retouches (friends frame): the raid tab on FriendsFrame's RaidFrame art.
    const FriendsFrameShell f = beginFriendsFrame(c, FriendsPage::Raid, text::get("RAID"));
    RaidTabView view;
    fill(view);
    const RaidTabResult r = raidTab(c, f.l, f.s, f.panel, view, m_drag);
    if (endFriendsFrame(c, f, SocialTab::Group, true) >= 0) {
        logResult("(another tab of the social window)");
    }
    char line[128];
    const auto name = [&](i32 i) { return std::string(kNames[static_cast<u32>(i)]); };
    if (r.tapped >= 0) {
        std::snprintf(line, sizeof(line), "target %s", name(r.tapped).c_str());
        logResult(line);
    }
    if (r.menu >= 0) {
        std::snprintf(line, sizeof(line), "menu RAID for %s at (%.0f, %.0f)", name(r.menu).c_str(),
                      r.menuRect.x, r.menuRect.bottom());
        logResult(line);
    }
    if (r.dropMember >= 0) {
        const game::RaidDrop drop =
            game::raidDrop(m_slots, {m_roster.data(), m_count}, static_cast<u32>(r.dropMember), r.dropSlot);
        if (drop.kind == game::RaidDropKind::None) {
            std::snprintf(line, sizeof(line), "%s back to its slot (dropped on slot %d)",
                          name(r.dropMember).c_str(), r.dropSlot);
        } else if (drop.kind == game::RaidDropKind::Move) {
            std::snprintf(line, sizeof(line), "SetRaidSubgroup %s to group %u", name(r.dropMember).c_str(),
                          drop.subgroup + 1u);
        } else {
            std::snprintf(line, sizeof(line), "SwapRaidSubgroup %s and %s", name(r.dropMember).c_str(),
                          name(drop.other).c_str());
        }
        logResult(line);
        if (drop.kind != game::RaidDropKind::None) {
            m_pendingMember = r.dropMember;
            m_pendingOther = drop.kind == game::RaidDropKind::Swap ? drop.other : -1;
            m_pendingGroup = drop.subgroup;
            m_pendingAtMs = nowMs + 600;
        }
    }
    if (r.pulloutGroup >= 0) {
        std::snprintf(line, sizeof(line), "pullout of group %d at (%.0f, %.0f)", r.pulloutGroup + 1,
                      r.pulloutX, r.pulloutY);
        logResult(line);
    }
    if (r.pulloutMember >= 0) {
        std::snprintf(line, sizeof(line), "pullout of class %u (%s's) at (%.0f, %.0f)",
                      m_roster[static_cast<u32>(r.pulloutMember)].cls, name(r.pulloutMember).c_str(),
                      r.pulloutX, r.pulloutY);
        logResult(line);
    }
    if (r.convert) {
        logResult("ConvertToRaid");
    }
    if (r.readyCheck) {
        logResult("DoReadyCheck");
    }
    if (r.raidInfo) {
        logResult("Raid Info (RaidInfoFrame: VD_UI_GALLERY=raidinfo)");
    }
    if (r.addMember) {
        logResult("Add Member (no target: ADD_RAIDMEMBER)");
        m_popup = true;
    }
    if (f.closed) {
        logResult("window closed");
    }
    // StaticPopup_Show("ADD_RAIDMEMBER").
    if (m_popup) {
        m_popupViews = {};
        PopupView& p = m_popupViews[0];
        p.text = text::get("ADD_RAIDMEMBER_LABEL");
        p.button1 = text::get("ACCEPT");
        p.button2 = text::get("CANCEL");
        p.editBox = true;
        p.editText = "Galen";
        const PopupTaps taps = popupStack(c, m_popupViews, m_popupHeights);
        if (taps.slot == 0) {
            logResult(taps.button == 1 ? "ADD_RAIDMEMBER accepted: InviteByName(Galen)"
                                       : "ADD_RAIDMEMBER cancelled");
            m_popup = false;
        }
    }
}

} // namespace vd
