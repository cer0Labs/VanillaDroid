#pragma once

#include "core/Types.h"
#include "net/RealmListMessages.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd {

// Phase 18 (glue): the rules of the glue screens (GlueXML and the C side of their Lua API in WoW.exe 1.12.1)
// that do not draw: the version line, the realm list's columns, colors and sorting, what the GlueDialog says
// while the session connects, the character list's lines and selection, the delete confirmation, and the
// WTF\Config.wtf the glue keeps. No Vulkan, no UI: app/GlueScreens fills the views (app/GlueWindows) with it;
// tested on its own.

// --- AccountLogin ------------------------------------------------------------------------------------------

// GetBuildInfo (WoW.exe 0x46CD70): BETA_BUILD's text, RELEASE_BUILD's, "1.12.1", "5875", "Sep 19 2006";
// written with VERSION_TEMPLATE as AccountLogin_OnLoad does ("Version 1.12.1 (5875) (Release)\nSep 19 2006").
constexpr std::string_view kGlueVersion = "1.12.1";
constexpr std::string_view kGlueBuild = "5875";
constexpr std::string_view kGlueBuildDate = "Sep 19 2006";
std::string_view glueVersionText(std::span<char> out);

// GlueParent.lua's fades (seconds): AccountLoginUI appears in LOGIN_FADE_IN and leaves in LOGIN_FADE_OUT when
// the character list comes (GlueScreenExit), CharacterSelectUI appears in CHARACTER_SELECT_FADE_IN.
constexpr f32 kLoginFadeIn = 1.5f;
constexpr f32 kLoginFadeOut = 0.5f;
constexpr f32 kCharacterSelectFadeIn = 0.75f;
// GlueFrameFadeUpdate: 0..1 over `duration` (in), 1..0 (out).
f32 glueFade(f32 elapsed, f32 duration, bool in);

// --- GlueDialog --------------------------------------------------------------------------------------------

// GlueDialogTypes (GlueDialog.lua) the client opens (OPEN_STATUS_DIALOG), and ours.
enum class GlueDialogType : u8 {
    None,
    Cancel,        // "CANCEL": a step in progress, its button cancels it (StatusDialogClick)
    Okay,          // "OKAY": a failure
    Disconnected,  // "DISCONNECTED": DISCONNECTED_FROM_SERVER (GlueParent_OnEvent)
    RealmIsFull,   // "REALM_IS_FULL": REALM_IS_FULL_WARNING, Yes / No, the alert icon
    ServerAddress, // ours: the realm server's address (1.12's realmlist.wtf) in an edit box, Okay / Cancel
};

// What the session is doing, as the glue reads it from GameSession.
struct GlueSessionState {
    enum class Step : u8 {
        LoggedOut,
        Reconnecting,    // ours: the session comes back by itself after a disconnection
        Connecting,      // realmd: LOGIN_STATE_CONNECTING
        Authenticating,  // LOGIN_STATE_AUTHENTICATING
        Authenticated,   // LOGIN_STATE_AUTHENTICATED: logged in, the realm list on its way
        ChoosingRealm,   // RealmList shown: no dialog
        WorldConnecting, // GAME_SERVER_LOGIN
        Queued,          // the login queue
        CharacterList,   // the characters: the list asked for, or here
        EnteringWorld,
        InWorld,
    };
    Step step = Step::LoggedOut;
    std::string_view failure; // the last failure, worded (shown with OKAY until dismissed)
    u32 queuePosition = 0;
    std::string_view realm;            // the realm being connected to (the queue's text)
    bool characterListPending = false; // CHAR_LIST_RETRIEVING
    bool realmListPending = false;     // REALM_LIST_IN_PROGRESS (Change Realm)
    bool realmListShown = false;       // RealmList is up: no step dialog over it (the queue's waits under it)
    bool deleting = false;             // CHAR_DELETE_IN_PROGRESS
    bool renaming = false;             // CHAR_RENAME_IN_PROGRESS
};

// The status dialog of a state: its type, text and button (OPEN_STATUS_DIALOG / UPDATE_STATUS_DIALOG as
// CGlueMgr opens them: WoW.exe 0x46B1C9 the login states, 0x46B880 CHAR_LIST_RETRIEVING, 0x46B8D0
// REALM_LIST_IN_PROGRESS, 0x46BC52 the queue, whose button is CHANGE_REALM).
struct GlueStatus {
    GlueDialogType type = GlueDialogType::None;
    std::string_view text;
    std::string_view button; // empty: the type's own (CANCEL, OKAY)
    // What its button does (StatusDialogClick, WoW.exe 0x46B720): back to the login screen, the realm list
    // (the queue), or only closing (a failure).
    enum class Action : u8 { Close, Logout, ChangeRealm, CancelRealmList };
    Action action = Action::Close;
};
GlueStatus glueStatus(const GlueSessionState& s, std::span<char> scratch);

// --- RealmList ---------------------------------------------------------------------------------------------

// GetRealmInfo's load (WoW.exe 0x46EC60) as RealmList.lua words it: realmd's flag 0x20 is LOAD_RECOMMENDED
// (-3), 0x40 LOAD_NEW (-2), 0x80 LOAD_FULL (2); else the population against the list's mean +- 0.6 standard
// deviations (WoW.exe 0x46E67E; one realm: 1 +- 0): LOAD_LOW (-1) below, LOAD_HIGH (1) above, LOAD_MEDIUM (0)
// between.
enum class RealmLoad : i8 { Recommended = -3, New = -2, Low = -1, Medium = 0, High = 1, Full = 2 };

// One row of RealmList (RealmListUpdate): GetRealmInfo's values and how the list colors them.
struct RealmRow {
    u32 realm = 0; // index into the realms
    std::string_view name;
    bool pvp = false, rp = false; // Cfg_Configs.dbc by the realm's icon (its type): RPPVP / RP / PVP / Normal
    bool invalid = false;         // REALM_FLAG_INVALID: red
    bool down = false;            // REALM_FLAG_OFFLINE: REALM_DOWN, gray, not selectable
    bool current = false; // the realm of the session (GetServerName), highlighted when nothing is chosen
    u32 characters = 0;   // "(N)", green names
    RealmLoad load = RealmLoad::Medium;
};
// The realm type's PvP and RP marks (Cfg_Configs.dbc: id, the realm type realmd sends as icon, pvp, rp).
void realmType(u32 icon, bool& pvp, bool& rp);
// Texts and colors of a row's columns as RealmListUpdate sets them (RGBA8 of ui::rgba).
// RPPVP_PARENTHESES, RP_PARENTHESES, PVP_PARENTHESES, GAMETYPE_NORMAL.
std::string_view realmTypeText(const RealmRow& row);
u32 realmTypeColor(const RealmRow& row);             // NORMAL / GREEN / RED_FONT_COLOR
std::string_view realmLoadText(const RealmRow& row); // REALM_DOWN, LOAD_RECOMMENDED...
u32 realmLoadColor(const RealmRow& row);             // GRAY / BLUE / GREEN / RED / NORMAL_FONT_COLOR
u32 realmNameColor(const RealmRow& row);             // gray down, red invalid, green with characters, gold
u32 realmHighlightColor(const RealmRow& row);        // RealmListHighlightTexture's vertex color
bool realmIsFull(const RealmRow& row);               // LOAD_FULL: REALM_IS_FULL when it has no characters

// SortRealms ("name", "mode", "characters", "load": WoW.exe 0x46F330 / 0x46E9B0): the sort keys, the newest
// first; sorting by the key already first turns its direction around.
enum class RealmSortKey : u8 { Characters = 0, Load = 1, Name = 2, Mode = 3 };
struct RealmSort {
    struct Key {
        RealmSortKey key;
        bool descending;
    };
    std::array<Key, 4> keys{{{RealmSortKey::Name, false},
                             {RealmSortKey::Load, false},
                             {RealmSortKey::Characters, false},
                             {RealmSortKey::Mode, false}}};
    void by(RealmSortKey key);
};
// The rows of `realms` (GetRealmInfo for each, `current` being the session's realm), sorted.
void realmRows(std::span<const net::RealmInfo> realms, std::string_view current, const RealmSort& sort,
               std::vector<RealmRow>& out);

// --- CharacterSelect ---------------------------------------------------------------------------------------

constexpr u32 kMaxCharactersDisplayed = 10; // MAX_CHARACTERS_DISPLAYED, MAX_CHARACTERS_PER_REALM
// CharacterSelect.lua's rotation: CHARACTER_ROTATION_CONSTANT degrees per unit dragged (touch has no rotate
// buttons, so no CHARACTER_FACING_INCREMENT).
constexpr f32 kCharacterRotationPerUnit = 0.6f;

// The character the list selects when it arrives (WoW.exe 0x4724D0: the lastCharacterIndex CVar, first when
// beyond the list; SELECT_LAST_CHARACTER after a creation, SELECT_FIRST_CHARACTER after a deletion), -1
// without characters.
enum class CharacterListReason : u8 { Login, Created, Deleted };
i32 selectedCharacter(u32 count, i32 lastIndex, CharacterListReason reason);

// CharacterDeleteDialog's edit box: OKAY only with DELETE_CONFIRM_STRING typed (strupper on both sides).
bool deleteConfirmed(std::string_view typed);

// --- WTF\Config.wtf ----------------------------------------------------------------------------------------

// What 1.12 keeps between sessions for the glue (SET <cvar> "<value>" lines of Config.wtf, and
// realmlist.wtf's address): accountName (AccountLoginSaveAccountName checked, else ""), realmName (the last
// realm connected to), realmList (the realm server: host[:port]), lastCharacterIndex (EnterWorld). The
// password is never kept. As 1.12 reads them: "SET" and the CVar's name in any case, the value quoted or not
// (realmlist.wtf's "set realmlist logon.example.org"), other lines ignored.
struct GlueConfig {
    bool loaded = false; // the file was there: its accountName counts even when empty
    std::string accountName;
    std::string realmName;
    std::string realmList;
    i32 lastCharacterIndex = 0;
};
GlueConfig parseGlueConfig(std::string_view text);
std::string writeGlueConfig(const GlueConfig& config);

// --- The realm server (1.12's realmlist) -------------------------------------------------------------------

// The address AccountLogin starts with, the first one set of: a development launch's (--realmd, the Android
// intent's `realmd`; the published build has none), the player's own (the realmList CVar: the address dialog,
// Config.wtf, realmlist.wtf), client.cfg's realm.address. Empty: none, LOGIN asks for one first. The same
// order for the automatic login of a development launch (app/Game); "Change Realm" and reconnections reuse
// the session's.
std::string_view glueRealmList(std::string_view launch, std::string_view player, std::string_view configured);
// LOGIN (the button, Enter) without a server: the address dialog first, the login once it has one.
bool glueNeedsServer(std::string_view server);

// The address dialog's OKAY with `typed` (ours; 1.12 only has realmlist.wtf). Spaces around it and a pasted
// "set realmlist " are dropped. An empty address forgets the player's own: the form falls back on `fallback`
// (glueRealmList without the player's), unless the dialog was opened by LOGIN and there is none.
struct GlueServerEntry {
    bool close = false;         // the address is fine: the dialog closes
    std::string_view realmList; // the player's own to keep (empty: forgotten)
    std::string_view server;    // the form's server from now on
    std::string_view error;     // else the key of what is wrong: VD_LOGIN_ENTER_SERVER, VD_LOGIN_BAD_PORT
};
GlueServerEntry glueServerEntry(std::string_view typed, std::string_view fallback, bool forLogin);

} // namespace vd
