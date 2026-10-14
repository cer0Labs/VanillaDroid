#pragma once

#include "app/DialogKit.h"
#include "app/Dialogs.h"
#include "app/FriendsFrameLayout.h"
#include "app/ModelFrame.h"
#include "core/Types.h"
#include "ui/Context.h"
#include "ui/DrawList.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

namespace ui {
struct Gui;
}

// Phase 15 (guild): the guild's windows as pure views like app/Dialogs (a view-model in, what the player did
// out; no game state, no network): GuildFrame (the social window's guild tab: FriendsFrame.xml's roster in
// its two views, the column headers that sort it, the totals, the message of the day, Guild Information / Add
// Member / Guild Control), GuildMemberDetailFrame, GuildInfoFrame and GuildControlPopupFrame (hung to the
// right of the FriendsFrame as 1.12 does: retouches, friends frame: right of its turned tabs), GuildRegistrarFrame, PetitionFrame and TabardFrame (panel windows: UIPanelWindows has them
// "left"). Interface words by key (core/Text: the client's GlobalStrings, VD_GUILD_* of locale/*/guild.lang);
// names and the server's texts as they come. Touch: a tap is the click; a long press on a roster row is its
// right click (FriendsFrame_ShowDropdown); the drop-downs open their lists in a small modal panel
// (app/SocialWindows choiceList, the caller draws it); text boxes open the platform's keyboard (the caller
// starts it when a field reports a tap).

// --- GuildFrame
// ------------------------------------------------------------------------------------------------

constexpr u32 kGuildColumns = 4;

struct GuildRowView {
    std::array<std::string_view, kGuildColumns>
        cells{};        // name, zone, level, class / name, rank, note, online
    bool online = true; // offline rows in gray (1.12.1 lists none: kept for the views' rules)
};

struct GuildTabView {
    std::string_view totals;  // GuildFrameTotals: GUILD_TOTAL
    std::string_view online;  // GuildFrameOnlineTotals: GUILD_TOTALONLINE
    bool guildStatus = false; // GuildStatusFrame (rank, note, last online) instead of GuildPlayerStatusFrame
    std::array<std::string_view, kGuildColumns> headers{}; // NAME, ZONE, LEVEL_ABBR, CLASS / NAME, RANK, ...
    std::span<const GuildRowView> rows;
    i32 selected = -1;
    std::string_view toggleLabel; // GuildFrameGuildListToggleButton: GUILD_STATUS / PLAYER_STATUS
    std::string_view motd;        // CURRENT_GUILD_MOTD (GUILD_MOTD_EDITLABEL when empty and editable)
    bool motdEditable = false;    // CanEditMOTD: white and tappable, gray otherwise
    bool controlEnabled = false;  // IsGuildLeader
    bool addEnabled = false;      // CanGuildInvite
    bool infoOpen = false, controlOpen = false; // the buttons' locked highlight
};

struct GuildTabResult {
    i32 row = -1;     // a row tapped (FriendsFrameGuildStatusButton_OnClick)
    i32 rowHeld = -1; // ... held (its right click)
    ui::Rect rowRect;
    i32 header = -1; // a column header tapped (SortGuildRoster)
    bool toggleView = false;
    bool motd = false; // GuildMOTDEditButton
    bool information = false, addMember = false, control = false;
    bool scrolled = false; // the list's scroll bar (UChatScrollButton)
};

// Retouches (friends frame): the guild tab on FriendsFrame's GuildFrame art (`l`, at its sizes `s`: GuildFrame's
// places in FriendsFrame.xml), its rows a finger's height at least.
GuildTabResult guildTab(DialogContext& c, const FriendsFrameLayout& l, const kit::QuestArtSizes& s,
                        const GuildTabView& v);
// Where a side frame goes (GuildMemberDetailFrame at TOPRIGHT (-33, -28), GuildInfoFrame and GuildControlPopupFrame
// at (-35, -65)): right of the frame's turned tabs, as tall as the frame under that, wide enough for its controls.
ui::Rect guildSideFrame(const ui::Gui& g, const DialogContext& c, const FriendsFrameLayout& l, bool member);

// --- GuildMemberDetailFrame
// ------------------------------------------------------------------------------------

struct GuildMemberView {
    std::string_view name;
    std::string_view levelClass; // FRIENDS_LEVEL_TEMPLATE
    std::string_view zone, rank, online;
    std::string_view note; // the note, or GUILD_NOTE_EDITLABEL when empty and editable
    bool noteEditable = false;
    bool officerNoteShown = false; // CanViewOfficerNote (GUILD_DETAIL_OFFICER_HEIGHT)
    std::string_view officerNote;
    bool officerNoteEditable = false;
    bool removeEnabled = false, groupInviteEnabled = false;
    bool promoteShown = false, promoteEnabled = false, demoteEnabled = false; // hidden when both are disabled
};

struct GuildMemberResult {
    bool closed = false;
    bool remove = false, groupInvite = false, promote = false, demote = false;
    bool note = false, officerNote = false; // a note box tapped (SET_GUILDPLAYERNOTE / SET_GUILDOFFICERNOTE)
};

GuildMemberResult guildMemberDetail(DialogContext& c, ui::Gui& g, const ui::Rect& area,
                                    const GuildMemberView& v);

// --- GuildInfoFrame
// --------------------------------------------------------------------------------------------

struct GuildInfoView {
    std::string_view text; // the info text (GUILD_INFO_EDITLABEL when empty and editable)
    bool editable = false; // CanEditGuildInfo: white, the box takes the keyboard, Accept enabled
    bool editing = false;
};

struct GuildInfoResult {
    bool closed = false; // the X or Close
    bool save = false;   // Accept
    bool tapped = false; // the text box (editable: the keyboard)
};

GuildInfoResult guildInfoFrame(DialogContext& c, ui::Gui& g, const ui::Rect& area, const GuildInfoView& v);

// --- GuildControlPopupFrame
// ------------------------------------------------------------------------------------

struct GuildControlView {
    std::string_view rankName; // the drop-down's choice
    std::string_view label;    // GuildControlPopupFrameEditBox
    bool editing = false;
    std::array<bool, 13> flags{};                    // GuildControlPopupFrameCheckbox1..13
    std::array<std::string_view, 13> flagLabels{};   // GUILDCONTROL_OPTION1..13
    bool addEnabled = false;                         // fewer than 10 ranks
    bool removeShown = false, removeEnabled = false; // the lowest of more than five, held by nobody listed
    bool acceptEnabled = false;                      // something changed
};

struct GuildControlResult {
    bool closed = false; // the X or Cancel
    bool accept = false;
    bool rankMenu = false; // the drop-down
    bool addRank = false, removeRank = false;
    bool labelTapped = false;
    i32 toggled = -1; // a check box changed (0..12)
};

GuildControlResult guildControlFrame(DialogContext& c, ui::Gui& g, const ui::Rect& area,
                                     const GuildControlView& v);

// --- GuildRegistrarFrame
// ---------------------------------------------------------------------------------------

struct GuildRegistrarView {
    std::string_view npcName;
    bool purchase = false; // GuildRegistrarPurchaseFrame (else the greeting with its services)
    std::string_view name; // GuildRegistrarFrameEditBox
    bool editing = false;
    u32 cost = 0; // GetGuildCharterCost
};

struct GuildRegistrarResult {
    bool closed = false;
    bool showPurchase = false; // GUILD_CHARTER_PURCHASE
    bool turnIn = false;       // GUILD_CHARTER_REGISTER: TurnInGuildCharter
    bool nameTapped = false;
    bool buy = false;  // Purchase
    bool back = false; // the purchase page's Cancel
};

GuildRegistrarResult guildRegistrarWindow(DialogContext& c, const GuildRegistrarView& v);

// --- PetitionFrame
// ---------------------------------------------------------------------------------------------

struct PetitionView {
    std::string_view title;       // GUILD_CHARTER_TEMPLATE with the name ("Petition" for other kinds)
    std::string_view charterName; // the guild's name
    std::string_view master;      // the originator's name
    std::array<std::string_view, 9> members{}; // the signers' names, NOT_YET_SIGNED for the rest
    std::string_view instructions;             // GUILD_PETITION_LEADER_INSTRUCTIONS / _MEMBER_INSTRUCTIONS
    bool originator = false;                   // Request Signature and Rename instead of Sign Charter
    bool requestEnabled = false;               // fewer signatures than the maximum
    bool signEnabled = false;                  // CanSignPetition
};

struct PetitionResult {
    bool closed = false;
    bool request = false, sign = false, rename = false;
};

PetitionResult petitionWindow(DialogContext& c, const PetitionView& v);

// --- TabardFrame
// -----------------------------------------------------------------------------------------------

struct TabardView {
    std::string_view npcName;
    std::string_view greeting; // TABARDVENDORGREETING / TABARDVENDORNOGUILDGREETING
    TextureHandle model;       // the player wearing the design (app/ModelViews; invalid while loading)
    TextureHandle
        upperEmblem; // TabardFrameEmblemTop*: the emblem's upper texture, at 40% alpha (invalid: none)
    TextureHandle lowerEmblem;                // TabardFrameEmblemBottom*
    std::array<std::string_view, 5> labels{}; // EMBLEM_SYMBOL, EMBLEM_SYMBOL_COLOR, EMBLEM_BORDER, ...
    u32 cost = 0;                             // GetTabardCreationCost
    bool acceptEnabled = false;
};

struct TabardResult {
    bool closed = false; // the X or Cancel
    bool accept = false;
    i32 kind = -1;        // an arrow tapped: the customization 0..4 ...
    i32 step = 0;         // ... -1 or +1
    bool rotated = false; // a rotate button went down (igInventoryRotateCharacter)
};

TabardResult tabardWindow(DialogContext& c, const TabardView& v, ModelFrameState& model);

} // namespace vd
