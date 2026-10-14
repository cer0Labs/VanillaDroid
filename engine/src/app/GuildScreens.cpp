#include "app/GuildScreens.h"

#include "app/DialogKit.h"
#include "app/GameSession.h"
#include "app/ModelViews.h"
#include "app/PlayerLook.h"
#include "app/PopupScreens.h"
#include "app/SocialScreens.h"
#include "app/UiResources.h"
#include "app/UnitMenuScreens.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/GameData.h"
#include "game/SocialData.h"
#include "net/UpdateFields.gen.h"
#include "ui/TextInput.h"
#include "ui/Widgets.h"
#include "world/Actors.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

namespace uf = net::uf;
using ui::rgba;

constexpr u32 kErrorColor = rgba(255, 26, 26); // UIErrorsFrame: UI_ERROR_MESSAGE (1, 0.1, 0.1)
constexpr u32 kInfoColor = rgba(255, 255, 0);  // UI_INFO_MESSAGE (1, 1, 0)
constexpr u64 kTabResetMs = 500;               // a tab not drawn that long was hidden (FriendsFrame_OnHide)

constexpr ui::WidgetId kInfoField = ui::widgetId("guildInfo.edit");
constexpr ui::WidgetId kRankField = ui::widgetId("guildControl.edit");
constexpr ui::WidgetId kCharterField = ui::widgetId("guildRegistrar.edit");

// GuildFrameColumnHeader1..4 and GuildFrameGuildStatusColumnHeader1..4: their labels and sort keys.
constexpr const char* kPlayerHeaders[kGuildColumns] = {"NAME", "ZONE", "LEVEL_ABBR", "CLASS"};
constexpr const char* kStatusHeaders[kGuildColumns] = {"NAME", "RANK", "LABEL_NOTE", "LASTONLINE"};
constexpr game::GuildSort kPlayerSorts[kGuildColumns] = {game::GuildSort::Name, game::GuildSort::Zone,
                                                         game::GuildSort::Level, game::GuildSort::Class};
constexpr game::GuildSort kStatusSorts[kGuildColumns] = {game::GuildSort::Name, game::GuildSort::Rank,
                                                         game::GuildSort::Note, game::GuildSort::Online};
constexpr const char* kControlOptions[game::kGuildControlOptions] = {
    "GUILDCONTROL_OPTION1", "GUILDCONTROL_OPTION2",  "GUILDCONTROL_OPTION3",  "GUILDCONTROL_OPTION4",
    "GUILDCONTROL_OPTION5", "GUILDCONTROL_OPTION6",  "GUILDCONTROL_OPTION7",  "GUILDCONTROL_OPTION8",
    "GUILDCONTROL_OPTION9", "GUILDCONTROL_OPTION10", "GUILDCONTROL_OPTION11", "GUILDCONTROL_OPTION12",
    "GUILDCONTROL_OPTION13"};
constexpr const char* kEmblemLabels[5] = {"EMBLEM_SYMBOL", "EMBLEM_SYMBOL_COLOR", "EMBLEM_BORDER",
                                          "EMBLEM_BORDER_COLOR", "EMBLEM_BACKGROUND"};

// The UIErrorsFrame lines that report what went right (UI_INFO_MESSAGE) rather than an error.
bool infoLine(const char* key) {
    const std::string_view k = key;
    return k == "ERR_GUILDEMBLEM_SUCCESS" || k == "ERR_PETITION_SIGNED" || k == "ERR_PETITION_SIGNED_S" ||
           k == "ERR_PETITION_OFFERED_S" || k == "ERR_PETITION_DECLINED_S";
}

// GuildFrame_GetLastOnline: the offline time in the roster's LASTONLINE_* texts.
std::string_view lastOnline(std::span<char> out, f32 days) {
    const f32 hours = days * 24.0f;
    if (hours < 1.0f) {
        return text::format(out, "LASTONLINE_MINS");
    }
    const auto count = [&](f32 value, const char* one, const char* many) {
        const i32 n = std::max(1, static_cast<i32>(value));
        return text::format(out, n == 1 ? one : many, n);
    };
    if (hours < 24.0f) {
        return count(hours, "LASTONLINE_HOURS", "LASTONLINE_HOURS_P1");
    }
    if (days < 30.0f) {
        return count(days, "LASTONLINE_DAYS", "LASTONLINE_DAYS_P1");
    }
    if (days < 365.0f) {
        return count(days / 30.0f, "LASTONLINE_MONTHS", "LASTONLINE_MONTHS_P1");
    }
    return count(days / 365.0f, "LASTONLINE_YEARS", "LASTONLINE_YEARS_P1");
}

std::string_view creatureName(GameSession& session, u64 guid) {
    const game::Entity* e = session.entities().find(guid);
    const net::CreatureQueryInfo* info = e ? session.creature(e->value(uf::OBJECT_FIELD_ENTRY)) : nullptr;
    return info ? std::string_view(info->name) : std::string_view{};
}

} // namespace

void GuildScreens::init(const game::GameData* data, UiResources* resources) {
    m_data = data;
    m_resources = resources;
    m_rows.reserve(64);
    m_rowMembers.reserve(64);
    m_cells.reserve(64);
    m_choices.reserve(game::kGuildControlOptions);
    m_info.reserve(512);
    m_motd.reserve(160);
    m_totals.reserve(96);
    m_onlineTotals.reserve(48);
}

void GuildScreens::sound(UiSound s) const {
    m_uiSound(s);
}

std::string_view GuildScreens::zoneName(u32 zone) const {
    const game::SocialData* social = m_socialScreens ? m_socialScreens->socialData() : nullptr;
    if (social) {
        return social->areaName(zone);
    }
    return m_data ? m_data->areaName(zone) : std::string_view{};
}

std::string_view GuildScreens::className(u32 cls) const {
    const game::SocialData* social = m_socialScreens ? m_socialScreens->socialData() : nullptr;
    return social ? social->className(cls) : std::string_view{};
}

std::string_view GuildScreens::zoneOf(const void* self, u32 zone) {
    return static_cast<const GuildScreens*>(self)->zoneName(zone);
}

std::string_view GuildScreens::classOf(const void* self, u32 cls) {
    return static_cast<const GuildScreens*>(self)->className(cls);
}

void GuildScreens::leaveWorld() {
    m_side = Side::None;
    m_menu = Menu::None;
    m_registrarPurchase = false;
    m_tabardLookBuilt = false;
    m_cursorsPlayer = 0;
}

// --- Notices
// ---------------------------------------------------------------------------------------------------

void GuildScreens::notice(GameSession& session, const game::GuildNotice& n) {
    if (!m_message || !n.key) {
        return;
    }
    std::string_view name = n.textView();
    if (n.guid != 0) {
        const std::string* known = session.playerName(n.guid);
        name = known && !known->empty() ? std::string_view(*known) : std::string_view("...");
    }
    char line[256];
    text::format(line, n.key, name);
    m_message(m_messageContext, infoLine(n.key) ? kInfoColor : kErrorColor, false, line);
}

void GuildScreens::update(GameSession& session, u32 nowMs) {
    (void)nowMs;
    game::Gameplay& play = session.play();
    if (m_cursorsPlayer != session.playerGuid()) {
        m_cursorsPlayer = session.playerGuid();
        m_petitionCursor = play.petitions().notices().end();
        m_tabardCursor = play.tabards().notices().end();
    }
    while (const game::GuildNotice* n = play.petitions().notices().next(m_petitionCursor)) {
        notice(session, *n);
    }
    while (const game::GuildNotice* n = play.tabards().notices().next(m_tabardCursor)) {
        notice(session, *n);
    }
}

// --- The guild tab
// ---------------------------------------------------------------------------------------------

void GuildScreens::buildTab(GameSession& session) {
    VD_PROFILE_ZONE("GuildScreens::buildTab");
    game::Gameplay& play = session.play();
    game::GuildControl& gc = play.guildControl();
    const game::GuildState& g = play.social().guild();
    const std::span<const u32> roster = gc.roster();
    m_rows.clear();
    m_rowMembers.clear();
    if (m_cells.size() < roster.size()) {
        m_cells.resize(roster.size());
    }
    char buffer[64];
    for (u32 i = 0; i < roster.size(); ++i) {
        const game::GuildMember& m = g.members[roster[i]];
        std::array<std::string, kGuildColumns>& cells = m_cells[i];
        cells[0].assign(m.name);
        if (!m_guildStatus) {
            cells[1].assign(zoneName(m.zone));
            std::snprintf(buffer, sizeof(buffer), "%u", static_cast<u32>(m.level));
            cells[2].assign(buffer);
            cells[3].assign(className(m.cls));
        } else {
            cells[1].assign(g.rankName(m.rank));
            cells[2].assign(m.note);
            // Online: the status ("" for none) or GUILD_ONLINE_LABEL; offline: GuildFrame_GetLastOnline.
            cells[3].assign(m.presence != 0 ? text::get("GUILD_ONLINE_LABEL")
                                            : lastOnline(buffer, m.offlineDays));
        }
        GuildRowView& row = m_rows.emplace_back();
        for (u32 c = 0; c < kGuildColumns; ++c) {
            row.cells[c] = cells[c];
        }
        row.online = m.presence != 0;
        m_rowMembers.push_back(roster[i]);
    }
    // GuildFrameTotals (GUILD_TOTAL: GetNumGuildMembers, the listed ones) and GuildFrameOnlineTotals.
    u32 online = 0;
    for (const u32 i : roster) {
        online += g.members[i].presence != 0 ? 1u : 0u;
    }
    const u32 total = static_cast<u32>(roster.size());
    m_totals.clear();
    text::append(m_totals, total == 1 ? "GUILD_TOTAL" : "GUILD_TOTAL_P1", total);
    m_onlineTotals.clear();
    text::append(m_onlineTotals, "GUILD_TOTALONLINE", online);
    m_rosterVersion = g.rosterVersion;
}

void GuildScreens::drawTab(GameSession& session, DialogContext& c, ui::TextInput& text,
                           const FriendsFrameShell& f) {
    VD_PROFILE_ZONE("GuildScreens::tab");
    game::Gameplay& play = session.play();
    game::GuildControl& gc = play.guildControl();
    game::Social& social = play.social();
    const game::GuildState& g = social.guild();
    ui::Gui gui{c.ui, c.draw};
    const u64 now = c.ui.nowMs();
    if (now - m_lastTabMs > kTabResetMs) {
        // FriendsFrame_OnShow / OnHide: nothing selected, the side frames hidden.
        m_side = Side::None;
        m_menu = Menu::None;
        gc.select({});
        m_rosterVersion = ~0u;
    }
    m_lastTabMs = now;
    if (!m_namesSet) {
        gc.setNames(&GuildScreens::zoneOf, &GuildScreens::classOf, this);
        m_namesSet = true;
    }
    if (g.id == 0) { // InGuildCheck: the tab is disabled outside a guild (SocialScreens leaves it)
        m_side = Side::None;
        return;
    }
    if (m_rosterVersion != g.rosterVersion || m_rows.size() != gc.roster().size()) {
        buildTab(session);
    }
    GuildTabView v;
    v.totals = m_totals;
    v.online = m_onlineTotals;
    v.guildStatus = m_guildStatus;
    for (u32 i = 0; i < kGuildColumns; ++i) {
        v.headers[i] = text::get(m_guildStatus ? kStatusHeaders[i] : kPlayerHeaders[i]);
    }
    v.rows = m_rows;
    const game::GuildMember* selected = gc.selected();
    for (u32 i = 0; i < m_rowMembers.size() && selected; ++i) {
        if (&g.members[m_rowMembers[i]] == selected) {
            v.selected = static_cast<i32>(i);
        }
    }
    v.toggleLabel = text::get(m_guildStatus ? "PLAYER_STATUS" : "GUILD_STATUS");
    v.motdEditable = gc.hasRight(game::guildbit::kSetMotd);
    v.motd = g.motd.empty() && v.motdEditable ? text::get("GUILD_MOTD_EDITLABEL") : std::string_view(g.motd);
    v.controlEnabled = gc.isGuildLeader();
    v.addEnabled = gc.hasRight(game::guildbit::kInvite);
    v.infoOpen = m_side == Side::Info;
    v.controlOpen = m_side == Side::Control;
    const GuildTabResult r = guildTab(c, f.l, f.s, v);
    if (r.scrolled) {
        sound(UiSound::ChatScroll);
    }
    if (r.header >= 0) {
        gc.sort(m_guildStatus ? kStatusSorts[r.header] : kPlayerSorts[r.header]); // SortGuildRoster
        m_rosterVersion = ~0u;
        sound(UiSound::CheckBoxOn);
    }
    if (r.toggleView) { // GuildFrameGuildListToggleButton_OnClick
        m_guildStatus = !m_guildStatus;
        m_rosterVersion = ~0u;
        sound(UiSound::CheckBoxOn);
    }
    if (r.rowHeld >= 0 && static_cast<u32>(r.rowHeld) < m_rowMembers.size() && m_unitMenus) {
        // The right click: FriendsFrame_ShowDropdown(name, online) — the FRIEND menu with the guild's own
        // buttons.
        const game::GuildMember& m = g.members[m_rowMembers[static_cast<u32>(r.rowHeld)]];
        if (m.presence != 0) {
            m_unitMenus->openFriendMenu(session, m.name, r.rowRect.x, r.rowRect.bottom(), true);
        }
    } else if (r.row >= 0 && static_cast<u32>(r.row) < m_rowMembers.size()) {
        // FriendsFrameGuildStatusButton_OnClick: the same member again hides the detail, another shows it
        // (hiding the control and information frames).
        const game::GuildMember& m = g.members[m_rowMembers[static_cast<u32>(r.row)]];
        if (m_side == Side::Member && selected == &m) {
            m_side = Side::None;
            gc.select({});
        } else {
            gc.select(m.name);
            m_side = Side::Member;
        }
    }
    if (r.motd && v.motdEditable && m_popups) {
        m_popups->askGuildMotd(session, g.motd); // SET_GUILDMOTD
    }
    if (r.addMember && m_popups) {
        m_popups->askAddGuildMember(session); // ADD_GUILDMEMBER
    }
    if (r.information) { // ToggleGuildInfoFrame
        m_side = m_side == Side::Info ? Side::None : Side::Info;
        if (m_side == Side::Info) {
            m_info.assign(g.info);
            m_infoDirty = false;
        }
    }
    if (r.control && v.controlEnabled) { // GuildControlPopupFrame:Show() / Hide()
        m_side = m_side == Side::Control ? Side::None : Side::Control;
        if (m_side == Side::Control) {
            selectRank(session, 0); // GuildControlPopupFrame_OnShow: GuildControlSetRank(1), the guild master
        }
    }
    if (text.active() && ((text.field() == kInfoField && m_side != Side::Info) ||
                          (text.field() == kRankField && m_side != Side::Control))) {
        text.end();
    }
}

void GuildScreens::drawSideFrame(GameSession& session, DialogContext& c, ui::TextInput& text,
                                 const FriendsFrameShell& f) {
    if (m_side == Side::None || session.play().social().guild().id == 0) {
        return;
    }
    // Retouches (friends frame): 1.12 hangs them at FriendsFrame's right; here right of its turned tabs.
    const ui::Gui gui{c.ui, c.draw};
    drawSide(session, c, text, guildSideFrame(gui, c, f.l, m_side == Side::Member));
}

void GuildScreens::drawSide(GameSession& session, DialogContext& c, ui::TextInput& text,
                            const ui::Rect& area) {
    switch (m_side) {
    case Side::Member: memberFrame(session, c, area); break;
    case Side::Info: infoFrame(session, c, text, area); break;
    case Side::Control: controlFrame(session, c, text, area); break;
    case Side::None: break;
    }
}

void GuildScreens::memberFrame(GameSession& session, DialogContext& c, const ui::Rect& area) {
    game::Gameplay& play = session.play();
    game::GuildControl& gc = play.guildControl();
    const game::GuildState& g = play.social().guild();
    const game::GuildMember* m = gc.selected();
    if (!m) { // gone from the roster (logged off, removed)
        m_side = Side::None;
        return;
    }
    const game::GuildMemberRights rights = gc.memberRights(*m);
    char buffer[96];
    GuildMemberView v;
    v.name = m->name;
    m_levelClass.assign(
        text::format(buffer, "FRIENDS_LEVEL_TEMPLATE", static_cast<u32>(m->level), className(m->cls)));
    v.levelClass = m_levelClass;
    m_zone.assign(zoneName(m->zone));
    v.zone = m_zone;
    v.rank = g.rankName(m->rank);
    m_online.assign(m->presence != 0 ? text::get("GUILD_ONLINE_LABEL") : lastOnline(buffer, m->offlineDays));
    v.online = m_online;
    v.noteEditable = rights.editPublicNote;
    v.note =
        m->note.empty() && v.noteEditable ? text::get("GUILD_NOTE_EDITLABEL") : std::string_view(m->note);
    v.officerNoteShown = rights.viewOfficerNote;
    v.officerNoteEditable = rights.editOfficerNote;
    v.officerNote = m->officerNote.empty() && v.officerNoteEditable ? text::get("GUILD_OFFICERNOTE_EDITLABEL")
                                                                    : std::string_view(m->officerNote);
    v.removeEnabled = rights.remove;
    v.groupInviteEnabled = rights.groupInvite;
    v.promoteEnabled = rights.promote;
    v.demoteEnabled = rights.demote;
    v.promoteShown = rights.promote || rights.demote; // both disabled: both hidden
    ui::Gui gui{c.ui, c.draw};
    const GuildMemberResult r = guildMemberDetail(c, gui, area, v);
    const std::string name = m->name; // the member may go with the roster the answers bring
    if (r.closed) {
        m_side = Side::None;
    }
    if (r.remove && m_popups) {
        m_popups->askRemoveGuildMember(session, name); // REMOVE_GUILDMEMBER
    }
    if (r.groupInvite) {
        play.social().inviteToGroup(name); // InviteByName(GuildFrame.selectedName)
    }
    if (r.promote) {
        play.social().guildPromote(name); // GuildPromoteByName
        sound(UiSound::ChatScroll);
    }
    if (r.demote) {
        play.social().guildDemote(name);
        sound(UiSound::ChatScroll);
    }
    if (r.note && m_popups) {
        m_popups->askGuildNote(session, name, m->note, false); // SET_GUILDPLAYERNOTE
    }
    if (r.officerNote && m_popups) {
        m_popups->askGuildNote(session, name, m->officerNote, true); // SET_GUILDOFFICERNOTE
    }
}

void GuildScreens::infoFrame(GameSession& session, DialogContext& c, ui::TextInput& text,
                             const ui::Rect& area) {
    game::GuildControl& gc = session.play().guildControl();
    const game::GuildState& g = session.play().social().guild();
    const bool editable = gc.hasRight(game::guildbit::kModifyInfo); // CanEditGuildInfo
    const bool editing = text.active() && text.field() == kInfoField;
    if (editing) {
        m_info.assign(text.text());
        m_infoDirty = true;
    } else if (!m_infoDirty) {
        m_info.assign(g.info); // GuildInfoFrame_OnShow: GetGuildInfoText
    }
    GuildInfoView v;
    v.editable = editable;
    v.editing = editing;
    v.text =
        m_info.empty() && editable && !editing ? text::get("GUILD_INFO_EDITLABEL") : std::string_view(m_info);
    ui::Gui gui{c.ui, c.draw};
    const GuildInfoResult r = guildInfoFrame(c, gui, area, v);
    if (r.tapped && editable && !editing) {
        text.begin(kInfoField, m_info, net::kGuildInfoMax, ui::TextKind::Message);
    }
    if (r.save && editable) {
        gc.setInfoText(m_info); // SetGuildInfoText, GuildRoster(), hidden
        m_side = Side::None;
        m_infoDirty = false;
        if (editing) {
            text.end();
        }
    }
    if (r.closed) {
        m_side = Side::None;
        m_infoDirty = false;
        if (editing) {
            text.end();
        }
    }
}

void GuildScreens::selectRank(GameSession& session, u32 rank) {
    game::GuildControl& gc = session.play().guildControl();
    m_controlRank = rank;
    m_flags = gc.rankFlags(rank); // GuildControlCheckboxUpdate(GuildControlGetRankFlags())
    m_rankLabel.assign(
        gc.rankName(rank));   // GuildControlPopupFrameEditBox:SetText(GuildControlGetRankName(id))
    m_controlChanged = false; // GuildControlPopupAcceptButton:Disable()
}

void GuildScreens::controlFrame(GameSession& session, DialogContext& c, ui::TextInput& text,
                                const ui::Rect& area) {
    game::GuildControl& gc = session.play().guildControl();
    if (!gc.isGuildLeader()) {
        m_side = Side::None;
        return;
    }
    const u32 ranks = gc.rankCount();
    if (m_controlRank >= ranks) {
        selectRank(session, 0);
    }
    const bool editing = text.active() && text.field() == kRankField;
    if (editing) {
        if (m_rankLabel != text.text()) {
            m_rankLabel.assign(text.text());
            m_controlChanged = true; // the edit box's OnTextChanged enables Accept
        }
        if (text.submitted()) {
            text.end();
        }
    }
    GuildControlView v;
    v.rankName = gc.rankName(m_controlRank);
    v.label = m_rankLabel;
    v.editing = editing;
    v.flags = m_flags;
    for (u32 i = 0; i < game::kGuildControlOptions; ++i) {
        v.flagLabels[i] = text::get(kControlOptions[i]);
    }
    v.addEnabled = ranks < net::kGuildRanksMax; // GuildControlPopupFrameAddRankButton_OnUpdate
    v.removeShown = m_controlRank + 1 == ranks && ranks > net::kGuildRanksMin;
    v.removeEnabled = gc.canDeleteRank(m_controlRank);
    v.acceptEnabled = m_controlChanged;
    ui::Gui gui{c.ui, c.draw};
    const GuildControlResult r = guildControlFrame(c, gui, area, v);
    if (r.toggled >= 0) {
        // GuildControlSetRankFlag(id, checked) and the Accept button.
        m_flags[static_cast<u32>(r.toggled)] = !m_flags[static_cast<u32>(r.toggled)];
        m_controlChanged = true;
        sound(m_flags[static_cast<u32>(r.toggled)] ? UiSound::CheckBoxOff : UiSound::CheckBoxOn);
    }
    if (r.labelTapped && !editing) {
        text.begin(kRankField, m_rankLabel, net::kGuildRankNameMax, ui::TextKind::Plain);
    }
    if (r.addRank && m_popups) {
        m_popups->askAddGuildRank(session); // ADD_GUILDRANK
    }
    if (r.removeRank && gc.deleteLastRank()) { // GuildControlPopupFrameRemoveRankButton_OnClick
        selectRank(session, 0);
    }
    if (r.rankMenu) {
        m_menu = Menu::Rank;
    }
    if (r.accept &&
        gc.saveRank(m_controlRank, m_rankLabel, m_flags)) { // GuildControlPopupAcceptButton_OnClick
        m_side = Side::None;
        if (editing) {
            text.end();
        }
    }
    if (r.closed) {
        m_side = Side::None;
        if (editing) {
            text.end();
        }
    }
    if (m_menu == Menu::Rank) {
        // GuildControlPopupFrameDropDown_Initialize: one entry per rank.
        m_choices.clear();
        for (u32 i = 0; i < ranks; ++i) {
            ChoiceView& ch = m_choices.emplace_back();
            ch.text = gc.rankName(i);
            ch.selected = i == m_controlRank;
        }
        const i32 picked =
            choiceList(c, "guildControl.ranks", text::get("GUILDCONTROL_SELECTRANK"), m_choices);
        if (picked >= 0) {
            selectRank(session, static_cast<u32>(picked)); // GuildControlPopupFrameDropDownButton_OnClick
            if (editing) {
                text.end();
            }
        }
        if (picked != -1) {
            m_menu = Menu::None;
        }
    }
}

// --- The panels
// ------------------------------------------------------------------------------------------------

bool GuildScreens::registrarOpen(const GameSession& session) {
    return session.play().petitions().registrar().open;
}

bool GuildScreens::petitionOpen(const GameSession& session) {
    return session.play().petitions().petition().open;
}

bool GuildScreens::tabardOpen(const GameSession& session) {
    return session.play().tabards().open();
}

void GuildScreens::closeRegistrar(GameSession& session) {
    session.play().petitions().closeRegistrar(); // CloseGuildRegistrar
    m_registrarPurchase = false;
}

void GuildScreens::closePetition(GameSession& session) {
    session.play().petitions().closePetition(); // ClosePetition
}

void GuildScreens::closeTabard(GameSession& session) {
    session.play().tabards().close(); // CloseTabardCreation
    m_tabardLookBuilt = false;
}

void GuildScreens::drawRegistrar(GameSession& session, DialogContext& c, ui::TextInput& text) {
    game::Petitions& pet = session.play().petitions();
    const game::GuildRegistrar& reg = pet.registrar();
    if (reg.npc != m_registrarNpc) { // GuildRegistrar_OnShow: the greeting first
        m_registrarNpc = reg.npc;
        m_registrarPurchase = false;
        m_charterName.clear();
    }
    const bool editing = text.active() && text.field() == kCharterField;
    if (editing) {
        m_charterName.assign(text.text());
    }
    GuildRegistrarView v;
    v.npcName = creatureName(session, reg.npc);
    v.purchase = m_registrarPurchase;
    v.name = m_charterName;
    v.editing = editing;
    v.cost = reg.cost;
    const GuildRegistrarResult r = guildRegistrarWindow(c, v);
    if (r.showPurchase) {
        m_registrarPurchase = true; // GuildRegistrar_ShowPurchaseFrame
    }
    if (r.turnIn) {
        pet.turnInCharter(); // TurnInGuildCharter
    }
    if (r.nameTapped && !editing) {
        text.begin(kCharterField, m_charterName, net::kGuildNameMax * 2, ui::TextKind::Plain);
    }
    if (r.buy || (editing && text.submitted())) { // GuildRegistrarFramePurchaseButton / the edit box's Enter
        if (pet.buyCharter(m_charterName, static_cast<u32>(c.ui.nowMs()))) {
            if (text.active()) {
                text.end();
            }
            closeRegistrar(session); // the purchase button: HideUIPanel(GuildRegistrarFrame)
            return;
        }
    }
    if (r.back) {
        m_registrarPurchase =
            false; // the purchase page's Cancel: HideUIPanel too in 1.12 (the greeting next time)
        if (editing) {
            text.end();
        }
        closeRegistrar(session);
        return;
    }
    if (r.closed) {
        if (editing) {
            text.end();
        }
        closeRegistrar(session);
    }
}

void GuildScreens::drawPetition(GameSession& session, DialogContext& c) {
    game::Gameplay& play = session.play();
    game::Petitions& pet = play.petitions();
    const game::GuildPetition& p = pet.petition();
    char buffer[128];
    PetitionView v;
    // PetitionFrame_Update.
    if (p.charter) {
        m_petitionTitle.assign(text::format(buffer, "GUILD_CHARTER_TEMPLATE", p.name));
    } else {
        m_petitionTitle.assign(text::get("VD_GUILD_PETITION"));
    }
    v.title = m_petitionTitle;
    v.charterName = p.name;
    const std::string* master = session.playerName(p.owner);
    m_petitionMaster.assign(master ? std::string_view(*master) : std::string_view("..."));
    v.master = m_petitionMaster;
    for (u32 i = 0; i < m_signers.size(); ++i) {
        if (i < p.signatures) {
            const std::string* name = session.playerName(p.signers[i]);
            m_signers[i].assign(name ? std::string_view(*name) : std::string_view("..."));
        } else {
            m_signers[i].assign(text::get("NOT_YET_SIGNED"));
        }
        v.members[i] = m_signers[i];
    }
    v.originator = pet.isOriginator();
    v.instructions =
        text::get(v.originator ? "GUILD_PETITION_LEADER_INSTRUCTIONS" : "GUILD_PETITION_MEMBER_INSTRUCTIONS");
    v.requestEnabled = p.signatures < p.maxSignatures;
    v.signEnabled = pet.canSign();
    const PetitionResult r = petitionWindow(c, v);
    if (r.request) {
        // OfferPetition: to the target (WoW.exe 0x606BA0, a player of the player's side: the races' teams).
        const u64 target = play.target();
        const game::Entity* me = session.entities().find(session.playerGuid());
        const game::Entity* other = target ? session.entities().find(target) : nullptr;
        const auto alliance = [](u32 race) { return race == 1 || race == 3 || race == 4 || race == 7; };
        const bool friendly = me && other &&
                              alliance(me->byteValue(uf::UNIT_FIELD_BYTES_0, 0)) ==
                                  alliance(other->byteValue(uf::UNIT_FIELD_BYTES_0, 0));
        pet.offer(target, friendly, static_cast<u32>(c.ui.nowMs()));
    }
    if (r.sign) {
        pet.sign(); // SignPetition
    }
    if (r.rename && m_popups) {
        m_popups->askRenameGuild(session); // RENAME_GUILD
    }
    if (r.closed) {
        closePetition(session);
    }
}

void GuildScreens::drawTabard(GameSession& session, DialogContext& c) {
    VD_PROFILE_ZONE("GuildScreens::tabard");
    game::Gameplay& play = session.play();
    game::TabardDesigner& tab = play.tabards();
    if (tab.vendor() != m_tabardVendor) {
        m_tabardVendor = tab.vendor();
        m_tabardModel = ModelFrameState{}; // TabardCharacterModelFrame_OnLoad: rotation 0
        m_tabardModel.facing = 0.0f;
        m_tabardLookBuilt = false;
    }
    TabardView v;
    v.npcName = creatureName(session, tab.vendor());
    v.greeting = text::get(tab.guildMaster() ? "TABARDVENDORGREETING" : "TABARDVENDORNOGUILDGREETING");
    for (u32 i = 0; i < 5; ++i) {
        v.labels[i] = text::get(kEmblemLabels[i]);
    }
    v.cost = game::kTabardCreationCost; // GetTabardCreationCost
    v.acceptEnabled = tab.guildMaster() && tab.canSaveNow();
    // TabardFrame_UpdateTextures: the emblem's upper and lower textures behind the model.
    const game::EmblemTextures upper = game::emblemTextures(tab.design(), true);
    const game::EmblemTextures lower = game::emblemTextures(tab.design(), false);
    if (m_resources) {
        v.upperEmblem = upper.emblem ? m_resources->texture(upper.emblem) : TextureHandle{};
        v.lowerEmblem = lower.emblem ? m_resources->texture(lower.emblem) : TextureHandle{};
    }
    // TabardModel:SetUnit("player") wearing a guild tabard with the design (rebuilt only when it changes).
    const game::Entity* me = session.entities().find(session.playerGuid());
    const Actors* actors = m_modelViews ? m_modelViews->actors() : nullptr;
    if (me && actors) {
        game::CharacterLook look;
        playerCharacterLook(session, *me, look);
        const game::AppearanceDb& db = actors->appearance();
        if (!db.guildTabard(look.items[game::kSlotTabard])) {
            look.items[game::kSlotTabard] = game::kGuildTabardDisplay;
            look.inventoryTypes[game::kSlotTabard] = 19; // INVTYPE_TABARD
        }
        look.emblem = tab.design();
        if (!m_tabardLookBuilt || !(look == m_tabardCharacter)) {
            m_tabardCharacter = look;
            m_tabardLook = db.character(look);
            m_tabardLookBuilt = true;
        }
        v.model = m_modelViews->show(ModelViewId::Tabard, m_tabardLook, m_tabardModel.facing);
    }
    const TabardResult r = tabardWindow(c, v, m_tabardModel);
    if (r.rotated) {
        sound(UiSound::InventoryRotate);
    }
    if (r.kind >= 0) {
        tab.cycle(static_cast<u32>(r.kind), r.step); // TabardCustomization_Left / _Right
    }
    if (r.accept) {
        tab.save(static_cast<u32>(c.ui.nowMs())); // TabardModel:Save()
    }
    if (r.closed) {
        closeTabard(session); // TabardFrameCancelButton: HideUIPanel -> CloseTabardCreation
    }
}

} // namespace vd
