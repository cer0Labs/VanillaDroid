#include "app/GuildGallery.h"

#include "app/ModelViews.h"
#include "app/UiResources.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/Travel.h"
#include "ui/Widgets.h"
#include "world/Actors.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace vd {
namespace {

constexpr const char* kPageNames[] = {"guild",     "guildstatus", "guildmember", "guildinfo", "guildcontrol",
                                      "registrar", "registrar2",  "petition",    "petition2", "tabard",
                                      "help",      "help2",       "help3",       "help4",     "help5",
                                      "gmsurvey",  "suggest"};
static_assert(std::size(kPageNames) == static_cast<usize>(GuildGallery::Page::Count));

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

// "The Wanderers": who is on line (1.12.1's roster lists no one else), as SMSG_GUILD_ROSTER and the client's
// tables name them.
struct Member {
    const char* name;
    const char* zone;
    u32 level;
    const char* cls;
    const char* rank;
    const char* note;
};
constexpr Member kMembers[] = {
    {"Arthel", "Stormwind City", 60, "Warrior", "Guild Master", "Main tank"},
    {"Aeryn", "Ironforge", 58, "Rogue", "Officer", "Lockpicking 300"},
    {"Brakk", "Stranglethorn Vale", 42, "Warrior", "Veteran", ""},
    {"Galen", "Blackrock Mountain", 51, "Paladin", "Veteran", "Alt: Galenna"},
    {"Miriel", "Duskwood", 37, "Priest", "Member", "Healer for Deadmines"},
    {"Seraphine", "Stormwind City", 60, "Druid", "Officer", "Herbalism 300"},
    {"Tolvir", "Westfall", 24, "Mage", "Initiate", ""},
};
constexpr const char* kMotd = "Deadmines run on Friday at 9 pm. Bring potions.";
constexpr const char* kInfo =
    "The Wanderers: a relaxed guild for levelling and dungeons.\n\nRaids: Molten Core on "
    "Wednesdays.\nBank: ask Seraphine.";
constexpr const char* kPlayerHeaders[kGuildColumns] = {"NAME", "ZONE", "LEVEL_ABBR", "CLASS"};
constexpr const char* kStatusHeaders[kGuildColumns] = {"NAME", "RANK", "LABEL_NOTE", "LASTONLINE"};
constexpr const char* kControlOptions[13] = {
    "GUILDCONTROL_OPTION1", "GUILDCONTROL_OPTION2",  "GUILDCONTROL_OPTION3",  "GUILDCONTROL_OPTION4",
    "GUILDCONTROL_OPTION5", "GUILDCONTROL_OPTION6",  "GUILDCONTROL_OPTION7",  "GUILDCONTROL_OPTION8",
    "GUILDCONTROL_OPTION9", "GUILDCONTROL_OPTION10", "GUILDCONTROL_OPTION11", "GUILDCONTROL_OPTION12",
    "GUILDCONTROL_OPTION13"};
// An officer's rights (vMaNGOS' default "Officer" rank: everything but the guild master's own).
constexpr std::array<bool, 13> kOfficerFlags = {true, true, true, true,  true, true, true,
                                                true, true, true, false, true, true};
constexpr const char* kEmblemLabels[5] = {"EMBLEM_SYMBOL", "EMBLEM_SYMBOL_COLOR", "EMBLEM_BORDER",
                                          "EMBLEM_BORDER_COLOR", "EMBLEM_BACKGROUND"};
// Placeholder ticket categories and survey questions (the game lists GMTicketCategory.dbc's, as HelpFrameGM does,
// and GMSurveyQuestions.dbc's: survey 6's are 28..32).
constexpr const char* kCategories[] = {"Stuck",     "Other players", "Guilds",    "Items",   "The world",
                                       "Creatures", "Quests",        "Technical", "Account", "Characters"};
constexpr const char* kQuestions[] = {
    "Did the answer you got from the GM actually solve the issue you reported to us?",
    "Were the steps or the explanation you received clear and easy to follow?",
    "Did the GM know the subject well, and treat you with courtesy and patience?",
    "How do you rate the time it took before someone handled your ticket?",
    "Taken as a whole, how would you rate your experience with the GM who helped you?"};
constexpr const char* kTicket =
    "My hunter's pet vanished after I took the boat to Menethil Harbor and the stable "
    "master does not list it.";

} // namespace

bool GuildGallery::select(const char* name) {
    for (u32 i = 0; i < std::size(kPageNames); ++i) {
        if (std::strcmp(name, kPageNames[i]) == 0) {
            m_page = static_cast<Page>(i);
            return true;
        }
    }
    return false;
}

bool GuildGallery::next() {
    m_page = static_cast<Page>(static_cast<u32>(m_page) + 1);
    if (m_page == Page::Count) {
        m_page = Page::Roster;
        return true;
    }
    VD_LOG_INFO("UI gallery: page %s", kPageNames[static_cast<u32>(m_page)]);
    return false;
}

void GuildGallery::drawPage(DialogContext& c, Page page) {
    VD_PROFILE_ZONE("GuildGallery::draw");
    if (page != m_lastPage) {
        // Each page starts as its name says.
        m_lastPage = page;
        m_status = page == Page::Status;
        m_flags = kOfficerFlags;
        m_helpPage = page == Page::HelpCategories   ? HelpPage::Categories
                     : page == Page::HelpGeneral    ? HelpPage::General
                     : page == Page::HelpHarassment ? HelpPage::Harassment
                     : page == Page::HelpTicket     ? HelpPage::OpenTicket
                                                    : HelpPage::Home;
        m_category = page == Page::HelpHarassment ? 2u : 4u;
    }
    switch (page) {
    case Page::Roster:
    case Page::Status:
    case Page::Member:
    case Page::Info:
    case Page::Control: roster(c, page); break;
    case Page::Registrar:
    case Page::Purchase: {
        GuildRegistrarView v;
        v.npcName = "Aldwin Laughlin";
        v.purchase = page == Page::Purchase;
        v.name = v.purchase ? std::string_view("The Wanderers") : std::string_view{};
        v.cost = 1000; // GetGuildCharterCost: 10 silver (vMaNGOS GUILD_CHARTER_COST)
        const GuildRegistrarResult r = guildRegistrarWindow(c, v);
        if (r.showPurchase) {
            logResult("registrar: purchase page");
        }
        if (r.turnIn) {
            logResult("registrar: turn in the charter (CMSG_TURN_IN_PETITION)");
        }
        if (r.nameTapped) {
            logResult("registrar: name box tapped (the keyboard opens)");
        }
        if (r.buy) {
            logResult("registrar: buy (CMSG_PETITION_BUY)");
        }
        if (r.closed || r.back) {
            logResult("registrar closed");
        }
        break;
    }
    case Page::Petition:
    case Page::PetitionSign: {
        const bool originator = page == Page::Petition;
        PetitionView v;
        v.title = text::format(m_title, "GUILD_CHARTER_TEMPLATE", "The Wanderers");
        v.charterName = "The Wanderers";
        v.master = originator ? "Arthel" : "Aeryn";
        const char* signers[] = {originator ? "Aeryn" : "Arthel", "Brakk", "Tolvir"};
        for (u32 i = 0; i < v.members.size(); ++i) {
            v.members[i] =
                i < std::size(signers) ? std::string_view(signers[i]) : text::get("NOT_YET_SIGNED");
        }
        v.originator = originator;
        v.instructions = text::get(originator ? "GUILD_PETITION_LEADER_INSTRUCTIONS"
                                              : "GUILD_PETITION_MEMBER_INSTRUCTIONS");
        v.requestEnabled = true;
        v.signEnabled = !originator;
        const PetitionResult r = petitionWindow(c, v);
        if (r.request) {
            logResult("petition: request signature (CMSG_OFFER_PETITION to the target)");
        }
        if (r.sign) {
            logResult("petition: sign (CMSG_PETITION_SIGN)");
        }
        if (r.rename) {
            logResult("petition: rename (the RENAME_GUILD popup)");
        }
        if (r.closed) {
            logResult("petition closed");
        }
        break;
    }
    case Page::Tabard: tabard(c); break;
    case Page::Help:
    case Page::HelpCategories:
    case Page::HelpGeneral:
    case Page::HelpHarassment:
    case Page::HelpTicket:
        ticketStatus(c); // under the windows, as the game draws it
        help(c, page);
        break;
    case Page::Survey:
        ticketStatus(c);
        survey(c);
        break;
    case Page::Suggest: suggest(c); break;
    case Page::Count: break;
    }
}

void GuildGallery::roster(DialogContext& c, Page page) {
    ui::Gui g{c.ui, c.draw};
    // Retouches (friends frame): the guild tab on FriendsFrame's GuildFrame art; FriendsFrameTitleText: the guild's name.
    const FriendsFrameShell f = beginFriendsFrame(c, FriendsPage::Guild, "The Wanderers");
    // The rows (rebuilt every frame: the view toggles; no allocation once reserved).
    m_rows.clear();
    m_rows.reserve(std::size(kMembers));
    for (u32 i = 0; i < std::size(kMembers); ++i) {
        const Member& m = kMembers[i];
        GuildRowView& row = m_rows.emplace_back();
        row.cells[0] = m.name;
        if (!m_status) {
            std::snprintf(m_levelCells[i].data(), m_levelCells[i].size(), "%u", m.level);
            row.cells[1] = m.zone;
            row.cells[2] = m_levelCells[i].data();
            row.cells[3] = m.cls;
        } else {
            row.cells[1] = m.rank;
            row.cells[2] = m.note;
            row.cells[3] = text::get("GUILD_ONLINE_LABEL");
        }
    }
    const Page side = page == Page::Status || page == Page::Roster ? Page::Count : page;
    GuildTabView v;
    char online[48];
    v.totals = text::format(m_totals, "GUILD_TOTAL_P1", static_cast<u32>(std::size(kMembers)));
    v.online = text::format(online, "GUILD_TOTALONLINE", static_cast<u32>(std::size(kMembers)));
    v.guildStatus = m_status;
    for (u32 i = 0; i < kGuildColumns; ++i) {
        v.headers[i] = text::get(m_status ? kStatusHeaders[i] : kPlayerHeaders[i]);
    }
    v.rows = m_rows;
    v.selected = side == Page::Member ? m_selected : -1;
    v.toggleLabel = text::get(m_status ? "PLAYER_STATUS" : "GUILD_STATUS");
    v.motd = kMotd;
    v.motdEditable = true;
    v.controlEnabled = page == Page::Control; // the guild master's page
    v.addEnabled = true;
    v.infoOpen = side == Page::Info;
    v.controlOpen = side == Page::Control;
    const GuildTabResult r = guildTab(c, f.l, f.s, v);
    if (endFriendsFrame(c, f, SocialTab::Guild, true) >= 0) {
        logResult("social tab");
    }
    if (f.closed) {
        logResult("social closed");
    }
    const ui::Rect sideArea = side != Page::Count ? guildSideFrame(g, c, f.l, side == Page::Member) : ui::Rect{};
    char line[96];
    if (r.header >= 0) {
        std::snprintf(line, sizeof(line), "guild: sort by column %d (SortGuildRoster)", r.header);
        logResult(line);
    }
    if (r.toggleView) {
        m_status = !m_status;
    }
    if (r.row >= 0) {
        m_selected = r.row;
        std::snprintf(line, sizeof(line), "guild: row %d (GuildMemberDetailFrame)", r.row);
        logResult(line);
    }
    if (r.rowHeld >= 0) {
        logResult("guild: row held (the FRIEND menu)");
    }
    if (r.motd) {
        logResult("guild: MOTD tapped (the SET_GUILDMOTD popup)");
    }
    if (r.addMember) {
        logResult("guild: add member (the ADD_GUILDMEMBER popup)");
    }
    if (r.information || r.control) {
        logResult(r.information ? "guild: information" : "guild: guild control");
    }
    switch (side) {
    case Page::Member: {
        const Member& m = kMembers[static_cast<u32>(std::clamp(m_selected, 0, 6))];
        GuildMemberView mv;
        mv.name = m.name;
        mv.levelClass = text::format(m_levelClass, "FRIENDS_LEVEL_TEMPLATE", m.level, m.cls);
        mv.zone = m.zone;
        mv.rank = m.rank;
        mv.online = text::get("GUILD_ONLINE_LABEL");
        mv.noteEditable = true;
        mv.note = *m.note ? std::string_view(m.note) : text::get("GUILD_NOTE_EDITLABEL");
        mv.officerNoteShown = true;
        mv.officerNoteEditable = true;
        mv.officerNote = "Reliable, knows the Deadmines.";
        mv.removeEnabled = true;
        mv.groupInviteEnabled = true;
        mv.promoteShown = true;
        mv.promoteEnabled = true;
        mv.demoteEnabled = m_selected >= 2;
        const GuildMemberResult mr = guildMemberDetail(c, g, sideArea, mv);
        if (mr.remove || mr.groupInvite || mr.promote || mr.demote || mr.note || mr.officerNote) {
            logResult(mr.remove        ? "member: remove (REMOVE_GUILDMEMBER)"
                      : mr.groupInvite ? "member: group invite"
                      : mr.promote     ? "member: promote (CMSG_GUILD_PROMOTE)"
                      : mr.demote      ? "member: demote (CMSG_GUILD_DEMOTE)"
                      : mr.note        ? "member: note (SET_GUILDPLAYERNOTE)"
                                       : "member: officer note (SET_GUILDOFFICERNOTE)");
        }
        if (mr.closed) {
            logResult("member detail closed");
        }
        break;
    }
    case Page::Info: {
        GuildInfoView iv;
        iv.text = kInfo;
        iv.editable = true;
        const GuildInfoResult ir = guildInfoFrame(c, g, sideArea, iv);
        if (ir.tapped) {
            logResult("guild info: text tapped (the keyboard opens)");
        }
        if (ir.save) {
            logResult("guild info: accept (CMSG_GUILD_INFO_TEXT)");
        }
        if (ir.closed) {
            logResult("guild info closed");
        }
        break;
    }
    case Page::Control: {
        GuildControlView cv;
        cv.rankName = "Officer";
        cv.label = "Officer";
        cv.flags = m_flags;
        for (u32 i = 0; i < 13; ++i) {
            cv.flagLabels[i] = text::get(kControlOptions[i]);
        }
        cv.addEnabled = true;
        cv.removeShown = false;
        cv.acceptEnabled = m_flags != kOfficerFlags;
        const GuildControlResult cr = guildControlFrame(c, g, sideArea, cv);
        if (cr.toggled >= 0) {
            m_flags[static_cast<u32>(cr.toggled)] = !m_flags[static_cast<u32>(cr.toggled)];
        }
        if (cr.accept) {
            logResult("guild control: accept (CMSG_GUILD_RANK)");
        }
        if (cr.addRank) {
            logResult("guild control: add rank (the ADD_GUILDRANK popup)");
        }
        if (cr.rankMenu) {
            logResult("guild control: the ranks' drop-down");
        }
        if (cr.labelTapped) {
            logResult("guild control: rank name tapped (the keyboard opens)");
        }
        if (cr.closed) {
            logResult("guild control closed");
        }
        break;
    }
    default: break;
    }
}

void GuildGallery::tabard(DialogContext& c) {
    TabardView v;
    v.npcName = "Rebecca Laughlin";
    v.greeting = text::get("TABARDVENDORGREETING");
    for (u32 i = 0; i < 5; ++i) {
        v.labels[i] = text::get(kEmblemLabels[i]);
    }
    v.cost = game::kTabardCreationCost;
    v.acceptEnabled = true;
    const game::EmblemTextures upper = game::emblemTextures(m_design, true);
    const game::EmblemTextures lower = game::emblemTextures(m_design, false);
    v.upperEmblem = upper.emblem ? c.resources.texture(upper.emblem) : TextureHandle{};
    v.lowerEmblem = lower.emblem ? c.resources.texture(lower.emblem) : TextureHandle{};
    // A human in a Guild Tabard with the design (rebuilt when it changes).
    if (m_actors && m_views && m_actors->isOpen() && m_actors->appearance().ready()) {
        if (!m_lookBuilt || !(m_lookDesign == m_design)) {
            game::CharacterLook look;
            look.race = 1;
            look.gender = 1;
            look.skin = 2;
            look.face = 1;
            look.hairStyle = 3;
            look.hairColor = 2;
            look.items[game::kSlotLegs] = 9960;       // black leather trousers
            look.inventoryTypes[game::kSlotLegs] = 7; // INVTYPE_LEGS
            look.items[game::kSlotTabard] = game::kGuildTabardDisplay;
            look.inventoryTypes[game::kSlotTabard] = 19; // INVTYPE_TABARD
            look.emblem = m_design;
            m_look = m_actors->appearance().character(look);
            m_lookDesign = m_design;
            m_lookBuilt = true;
        }
        v.model = m_views->show(ModelViewId::Tabard, m_look, m_model.facing);
    }
    const TabardResult r = tabardWindow(c, v, m_model);
    if (r.kind >= 0) {
        // CycleVariation: round the kind's count.
        const i32 count = game::kEmblemChoiceCounts[static_cast<u32>(r.kind)];
        const i32 value = (m_design.choice(static_cast<u32>(r.kind)) + r.step + count) % count;
        m_design.setChoice(static_cast<u32>(r.kind), value);
    }
    if (r.accept) {
        logResult("tabard: accept (MSG_SAVE_GUILD_EMBLEM)");
    }
    if (r.closed) {
        logResult("tabard closed");
    }
}

void GuildGallery::help(DialogContext& c, Page page) {
    (void)page;
    HelpView v;
    v.page = m_helpPage;
    if (m_helpPage == HelpPage::Categories) {
        m_categories.clear();
        for (const char* name : kCategories) {
            m_categories.push_back({name, {}});
        }
        m_categories.push_back(
            {text::get("HELPFRAME_BUG_BUTTON_TEXT"), text::get("HELPFRAME_BUG_BUTTON_DESCRIPTION")});
        m_categories.push_back({text::get("HELPFRAME_SUGGESTION_BUTTON_TEXT"),
                                text::get("HELPFRAME_SUGGESTION_BUTTON_DESCRIPTION")});
        v.categories = m_categories;
    }
    if (m_helpPage == HelpPage::General) {
        v.general = helpGeneralPage(m_category);
    }
    v.hasTicket = true;
    v.ticketText = kTicket;
    const HelpResult r = helpWindow(c, v);
    if (r.toCategories) {
        m_helpPage = HelpPage::Categories;
    }
    if (r.category >= 0) {
        if (static_cast<usize>(r.category) < std::size(kCategories)) {
            m_category = static_cast<u32>(r.category) + 1;
            m_helpPage = m_category == 2 ? HelpPage::Harassment : HelpPage::General;
        } else {
            logResult("help: bug / suggestion (SuggestFrame)");
        }
    }
    if (r.generalButton || r.generalButton2 || r.reportVerbal || r.reportPhysical) {
        if (r.generalButton && m_category == 1) {
            logResult("help: Auto-Unstuck (Stuck(): the hearthstone-less unstuck spell)");
        } else {
            m_helpPage = HelpPage::OpenTicket;
        }
    }
    if (r.ticketTapped) {
        logResult("help: ticket text tapped (the keyboard opens)");
    }
    if (r.submit) {
        logResult("help: save the ticket (CMSG_GMTICKET_UPDATETEXT)");
    }
    if (r.back) {
        m_helpPage = m_helpPage == HelpPage::Categories ? HelpPage::Home : HelpPage::Categories;
    }
    if (r.closed) {
        logResult("help closed");
    }
}

void GuildGallery::ticketStatus(DialogContext& c) {
    // TicketStatusFrame at the top right, left of where the minimap cluster is (its 180 units).
    ui::Gui g{c.ui, c.draw};
    const ui::Rect& safe = c.safeArea;
    const ui::Rect area{safe.x, safe.y, safe.w - g.unit(180.0f), safe.h};
    TicketStatusView v;
    char time[64];
    if (m_lastPage == Page::Survey) {
        v.title = text::get("CHOSEN_FOR_GMSURVEY");
        v.flash = true;
    } else {
        v.title = text::get("TICKET_STATUS1");
        v.time =
            text::format(m_wait, "GM_TICKET_WAIT_TIME", game::secondsToTime(time, 2 * 3600 + 25 * 60, true));
    }
    if (ticketStatusFrame(c, area, v, m_ticketStatus, c.ui.nowMs())) {
        logResult(v.flash ? "ticket status: the GM survey" : "ticket status: the HELP_TICKET popup");
    }
}

void GuildGallery::survey(DialogContext& c) {
    for (u32 i = 0; i < m_questions.size(); ++i) {
        m_questions[i].text = kQuestions[i];
    }
    if (m_questions[0].selected < 0 && m_questions[1].selected < 0) {
        m_questions[0].selected = 5;
        m_questions[1].selected = 4;
    }
    GmSurveyView v;
    v.questions = m_questions;
    v.comment = "Quick and friendly, thank you!";
    const GmSurveyResult r = gmSurveyWindow(c, v);
    if (r.question >= 0 && static_cast<u32>(r.question) < m_questions.size()) {
        m_questions[static_cast<u32>(r.question)].selected = r.answer;
    }
    if (r.commentTapped) {
        logResult("survey: comment tapped (the keyboard opens)");
    }
    if (r.submit) {
        logResult("survey: submit (CMSG_GMSURVEY_SUBMIT)");
    }
    if (r.closed) {
        logResult("survey closed");
    }
}

void GuildGallery::suggest(DialogContext& c) {
    // SuggestFramePlayerInfo as the game writes it (the race, class and place as the client's tables name
    // them).
    const std::string_view male = text::get("MALE"), level = text::get("LEVEL"),
                           at = text::get("LOCATION_COLON");
    std::snprintf(m_info.data(), m_info.size(),
                  "Arthel %.*s Human Warrior - %.*s 60\n%.*s Trade District (-8836.3, "
                  "625.4, 94.0)",
                  static_cast<int>(male.size()), male.data(), static_cast<int>(level.size()), level.data(),
                  static_cast<int>(at.size()), at.data());
    SuggestView v;
    v.playerInfo = m_info.data();
    v.category = text::get("BUG_CATEGORY5");
    v.text = "The mailbox next to the bank in Stormwind cannot be used from its back side.";
    const SuggestResult r = suggestWindow(c, v);
    if (r.categoryMenu) {
        logResult("suggest: category drop-down");
    }
    if (r.textTapped) {
        logResult("suggest: text tapped (the keyboard opens)");
    }
    if (r.sendBug || r.sendSuggestion) {
        logResult(r.sendBug ? "suggest: submit bug (CMSG_BUG)" : "suggest: submit suggestion (CMSG_BUG)");
    }
    if (r.closed) {
        logResult("suggest closed");
    }
}

} // namespace vd
