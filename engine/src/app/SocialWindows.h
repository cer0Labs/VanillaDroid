#pragma once

#include "app/DialogKit.h"
#include "app/Dialogs.h"
#include "app/FriendsFrameLayout.h"
#include "app/SortHeaders.h" // phase 18 (p3)
#include "core/Types.h"
#include "ui/Context.h"
#include "ui/DrawList.h"
#include "ui/TextInput.h"
#include "ui/Widgets.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

// The social side's windows (phase 8): party frames, the chat's channel picker (the chat
// itself: app/ChatFrame), the emote menu, a player's action sheet and the social window (friends, ignore list,
// who, guild, group). Same rules as app/Dialogs: pure views over the client's art, a view struct in and what the
// player did this frame out; no network, no game state (app/SocialScreens fills them from game::Social and applies
// the results). Interface words by key (core/Text: the client's strings, VD_SOCIAL_* of locale/*/social.lang);
// names and the server's texts as they come in the views.

// --- Party frames ------------------------------------------------------------------------------------------

// Parity (auras): a debuff on a party frame (PartyBuffButtonTemplate): its icon and its border's dispel type.
struct PartyDebuffView {
    u64 iconHash = 0;
    u8 dispel = 0; // Spell.dbc Dispel (DebuffTypeColor, app/AuraLayout)
};

struct PartyFrameView {
    std::string_view name;
    u32 level = 0;
    f32 health = 0.0f; // 0..1
    f32 power = 0.0f;  // 0..1
    u8 powerType = 0;  // mana, rage, focus, energy, happiness
    u8 race = 0, gender = 0;
    bool online = true, dead = false, ghost = false, afk = false, dnd = false;
    bool leader = false;
    bool inSight = true; // dimmed when out of sight (range)
    bool targeted = false;
    TextureHandle portrait; // parity: the member's portrait (app/Portraits); invalid: the race's picture
    // Parity (auras, app/AuraScreens): its first four debuffs under the bars (RefreshBuffs), and the portrait's glow
    // (the frame's "Status" overlay) in a typed debuff's colour, flashing, for 30 s after its auras changed.
    std::array<PartyDebuffView, 4> debuffs{};
    u32 debuffCount = 0;
    u32 statusColor = 0; // 0: no glow
    f32 statusAlpha = 0.0f;
    // Phase 15 (pets): PartyMemberPetFrame under it (PartyMemberFrame_UpdatePet: the member connected, its pet known,
    // SHOW_PARTY_PETS on): the pet's portrait and health, its first four debuffs.
    bool pet = false;
    f32 petHealth = 0.0f; // 0..1
    bool petTargeted = false;
    TextureHandle petPortrait; // invalid: TemporaryPortrait-Monster
    std::array<PartyDebuffView, 4> petDebuffs{};
    u32 petDebuffCount = 0;
};

// PartyMemberFrame: up to four compact frames stacked down from area's top left (not modal). Returns the frame
// tapped (-1 none); `held` / `heldFrame` (parity: auras) get a frame held down long and where it is.
// Phase 20 (assists): `frames` (8 rectangles, may be null) gets each member's frame, then each one's pet frame (empty:
// none), where a helpful spell may be dragged onto.
i32 partyFrames(DialogContext& c, const ui::Rect& area, std::span<const PartyFrameView> members, i32* held = nullptr,
                ui::Rect* heldFrame = nullptr, ui::Rect* tappedFrame = nullptr, i32* tappedPet = nullptr,
                ui::Rect* frames = nullptr);
// The height partyFrames() takes for `count` members.
f32 partyFramesHeight(const DialogContext& c, u32 count);
// Phase 15 (pets): ... with their pets' frames (each one shown moves the next member down, as PartyMemberFrameN+1 hangs
// under PartyMemberFrameNPetFrame).
f32 partyFramesHeight(const DialogContext& c, std::span<const PartyFrameView> members);

// --- Choices -----------------------------------------------------------------------------------------------

// A short list of choices in a small modal panel (the chat's channel picker, the loot methods...). Returns the
// index tapped, -2 when closed, -1 otherwise.
struct ChoiceView {
    std::string_view text;
    u32 color = 0xFFFFFFFF;
    bool selected = false;
    bool enabled = true;
};
i32 choiceList(DialogContext& c, std::string_view name, std::string_view title, std::span<const ChoiceView> choices);

// --- Emotes ------------------------------------------------------------------------------------------------

struct EmoteButtonView {
    std::string_view label;
    std::string_view token; // the slash command
};
// A grid of emote buttons. Returns the index tapped, -2 when closed, -1 otherwise.
i32 emoteMenu(DialogContext& c, std::span<const EmoteButtonView> emotes, std::string_view target);

// --- A player's sheet --------------------------------------------------------------------------------------

struct SheetActionView {
    std::string_view text;
    bool enabled = true;
    bool danger = false; // red text (remove, ignore...)
};
struct PlayerSheetView {
    std::string_view name;
    std::string_view detail; // "Level 12 Human Mage", the guild
    std::span<const SheetActionView> actions;
};
// The actions on a player (a name tapped in the chat, a party frame, a row of the social window): returns the
// action tapped, -2 when closed, -1 otherwise.
i32 playerSheet(DialogContext& c, const PlayerSheetView& v);

// --- The social window: FriendsFrame -----------------------------------------------------------------------

// Phase 16 (raidframe): Group is FriendsFrame's fourth tab, the raid tab (RAID; its body: app/RaidWindows).
// Retouches (friends frame): FriendsFrameTab1..4 (Friends, Who, Guild, Raid) turned along the window's right edge, from
// the top; Friends and Ignored are its first one's two lists (FriendsFrameToggleTab1 / 2).
enum class SocialTab : u8 { Friends, Ignored, Who, Guild, Group, Count };

// Friends frame (every tab as 1.12's, with the tabs at the right): the window of every tab is FriendsFrame with its
// 1.12 art (app/FriendsFrameLayout). The pages' art (FriendsFrame_Update's four textures: the top and the bottom halves
// of the picture).
enum class FriendsPage : u8 { Friends, Ignore, Who, Guild, Raid };

struct FriendsFrameShell {
    FriendsFrameLayout l;
    kit::QuestArtSizes s; // the art's sizes (fonts, a finger's row) at its scale
    ui::Rect panel;       // the whole window with its tabs
    bool closed = false;  // its X (or, over the world, a tap outside it)
};
// The frame of a page at the window buttons' scale (the quest frames'), in c's place (UIPanelWindows: left) or, without
// one, centered over the world: FriendsFrameScrollIcon under its ring, the page's art (a dark plate while it streams
// in), FriendsFrameTitleText (GameFontNormal) and the close button. The fingers that miss the page's controls are the
// frame's own.
FriendsFrameShell beginFriendsFrame(DialogContext& c, FriendsPage page, std::string_view title);
// Its turned tabs, last (over its border: the selected one joins it), the guild's disabled out of a guild. Returns the
// tab tapped other than `current` (a SocialTab), -1 none.
i32 endFriendsFrame(DialogContext& c, const FriendsFrameShell& f, SocialTab current, bool guildEnabled);

// A page's list (FauxScrollFrameTemplate): its track and scroll bar shown only when the rows overflow
// (FauxScrollFrame_Update), the rows drawn from `top` between begin and end, each a touch target inside the list with
// its HighlightTexture (`highlight`, ADD) locked on the selected one and under a finger. endFriendsList returns true
// when a bar's button scrolled (UChatScrollButton).
struct FriendsListScroll {
    ui::ScrollView view;
    f32 top = 0.0f; // the first row's top this frame
    f32 content = 0.0f;
    bool overflow = false;
};
FriendsListScroll beginFriendsList(DialogContext& c, ui::WidgetId id, const FriendsListLayout& list, u32 rows,
                                   f32 rowH);
bool endFriendsList(DialogContext& c, const FriendsListScroll& s, const FriendsListLayout& list,
                    f32 barRightLimit = 1e30f); // retouches (character frame): panelScrollBar's rightLimit
ui::WidgetState friendsListRow(DialogContext& c, ui::WidgetId id, const FriendsListScroll& s, const ui::Rect& r,
                               bool selected, u64 highlight);

// --- The friends frame -------------------------------------------------------------------------------------

// Friends frame: FriendsListFrame and IgnoreListFrame with their 1.12 art (app/FriendsFrameLayout) and
// FriendsFrameTab1..4 turned to stand along the frame's right edge. A friend's row (FriendsFrameButtonTemplate):
// FRIENDS_LIST_TEMPLATE / FRIENDS_LIST_OFFLINE_TEMPLATE with their color codes (GameFontNormal) over
// FRIENDS_LEVEL_TEMPLATE / UNKNOWN (GameFontHighlightSmall); an ignored player's, the name alone.
struct FriendRowView {
    std::string_view nameLocation;
    std::string_view info;
};

struct FriendsFrameView {
    bool ignore = false; // IgnoreListFrame (FriendsFrame.showFriendsList nil)
    std::span<const FriendRowView> rows;
    i32 selected = -1;        // FriendsFrame.selectedFriend / selectedIgnore (LockHighlight)
    bool connected = false;   // the selected friend is online: Send Message and Group Invite (FriendsList_Update)
    bool guildEnabled = true; // InGuildCheck: the guild's tab disabled out of a guild
};

struct FriendsFrameResult {
    i32 tab = -1;             // a turned tab tapped (a SocialTab: Who, Guild, Group)
    bool showFriends = false; // IgnoreFrameToggleTab1
    bool showIgnore = false;  // FriendsFrameToggleTab2
    i32 row = -1;             // a row tapped (selects it)
    i32 rowHeld = -1;         // a row held (the right click: FriendsDropDown)...
    ui::Rect rowRect;         // ...and where it is
    bool addFriend = false, removeFriend = false, sendMessage = false, groupInvite = false;
    bool ignorePlayer = false, stopIgnore = false;
    bool scrolled = false; // a scroll bar's button (UChatScrollButton)
    bool closed = false;
    ui::Rect panel; // the whole window with its tabs
};

FriendsFrameResult friendsFrame(DialogContext& c, const FriendsFrameView& v);

// --- WhoFrame ----------------------------------------------------------------------------------------------

// Retouches (friends frame): the Who tab as 1.12's WhoFrame: the column headers that sort the list (the second one's
// dropdown chooses what it shows: zone, guild or race), FriendsFrameWhoButtonTemplate's rows (the name in
// GameFontNormalSmall, the rest in GameFontHighlightSmall), WhoFrameTotals, WhoFrameEditBox (the query, typed as
// /who's) and Refresh / Add Friend / Group Invite (the last two with a player selected: WhoList_Update).
struct WhoRowView {
    std::string_view name, variable, level, cls;
};

struct WhoFrameView {
    std::span<const WhoRowView> rows;
    i32 selected = -1; // WhoFrame.selectedWho (LockHighlight)
    std::span<const SortHeaderView> headers; // NAME, the dropdown's column, LEVEL_ABBR, CLASS
    std::string_view totals;                 // "3 People Found  (49 displayed)"
    std::string_view input;                  // WhoFrameEditBox
    bool editing = false;
    bool guildEnabled = true;
};

struct WhoFrameResult {
    i32 tab = -1;     // a turned tab tapped
    i32 row = -1;     // a row tapped (selects it)
    i32 rowHeld = -1; // a row held (the right click: FriendsFrame_ShowDropdown)...
    ui::Rect rowRect; // ...and where it is
    i32 header = -1;  // a column header tapped: sort by it
    i32 headerMenu = -1; // the second header's dropdown asked for
    bool tappedEdit = false; // the edit box (the keyboard)
    bool refresh = false, addFriend = false, groupInvite = false;
    bool scrolled = false;
    bool closed = false;
};

WhoFrameResult whoFrame(DialogContext& c, const WhoFrameView& v);

// Phase 12: RaidInfoFrame (RaidFrame.xml): "Raid Information", the saved raids with their instance id and the time
// to their reset (RaidInfoInstanceTemplate: the name, the ID right, "Resets in ..." under the name). Returns true
// when closed.
struct RaidInfoRowView {
    std::string_view name;
    std::string_view id;
    std::string_view reset;
};
bool raidInfoPanel(DialogContext& c, std::span<const RaidInfoRowView> rows);


} // namespace vd
