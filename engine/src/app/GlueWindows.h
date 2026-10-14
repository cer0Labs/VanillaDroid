#pragma once

#include "app/Dialogs.h"
#include "app/GlueModel.h"
#include "core/Types.h"
#include "ui/Widgets.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 18 (glue): the glue screens of the 1.12 client (Interface\GlueXML) by touch, over the 3D scene the
// game draws behind them (app/GlueStage): AccountLogin, GlueDialog, RealmList, CharacterSelect,
// CharacterDeleteDialog and CharacterRenameDialog. Pure views like app/Dialogs: a view-model in, what the
// player did out (app/GlueScreens fills them and acts).
//
// Layout. The XML's sizes and anchors are units of a 768-high screen (GlueParent covers the screen; the scene
// fills it, app/GlueStage). One unit is the screen's height / 768, but never less than kGlueMinUnitDp, so the
// buttons keep a finger's size: on the Fold5's inner screen (~690 dp high) a unit is 0.9 dp and the screens
// are the original's; on short screens (the 23:9 outer one, 20:9 phones) the units are bigger than 1/768 and
// the screens use a compact arrangement (the logo smaller, the login fields lower, the realm and character
// lists shorter and scrolled). Every touch target is at least ui::kTouchDp. While the platform's keyboard
// covers the bottom (c.safeArea shorter than the screen's), the field being edited and its label move into
// what is left.

constexpr f32 kGlueMinUnitDp = 0.85f;

// The glue's coordinate system on this screen.
struct GlueLayout {
    ui::Rect screen;  // GlueParent: the safe area without the keyboard
    ui::Rect visible; // what the keyboard leaves of it (== screen without the keyboard)
    f32 unit = 1.0f;  // logical pixels per unit
    f32 heightUnits = 768.0f;
    f32 widthUnits = 1024.0f;
    bool compact = false;  // fewer than kGlueCompactUnits units high
    bool keyboard = false; // the keyboard covers part of the screen

    f32 u(f32 units) const { return units * unit; }
    // A frame of w x h units whose `point` (as the XML's Anchor point: its own and the screen's) is offset by
    // (x, y) units, y up as in the XML.
    enum class Point : u8 { TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight };
    ui::Rect at(Point point, f32 x, f32 y, f32 w, f32 h) const;
};
constexpr f32 kGlueCompactUnits = 640.0f;
GlueLayout glueLayout(const DialogContext& c);

// --- AccountLogin ------------------------------------------------------------------------------------------

struct AccountLoginView {
    std::string_view account;
    u32 passwordLength = 0; // shown as stars
    i32 editing = -1; // the field the keyboard types into: 0 account, 1 password (caret, and the keyboard
                      // layout); -1 none
    bool saveAccountName = false; // AccountLoginSaveAccountName
    std::string_view realmName;   // AccountLoginRealmName (GetServerName: the last realm), may be empty
    std::string_view server;      // ours: the realm server's address (1.12's realmlist.wtf); empty: none set
    std::string_view version;     // AccountLoginVersion (glueVersionText)
    f32 alpha = 1.0f;             // AccountLoginUI's fade
    bool interactive = true;      // false under a dialog: nothing reacts
    bool offline = false;         // development (VD_GLUE_OFFLINE): VD_LOGIN_OFFLINE's button
};
struct AccountLoginResult {
    i32 edit = -1;           // a field tapped: edit it (0 account, 1 password)
    bool login = false;      // AccountLoginLoginButton
    bool quit = false;       // AccountLoginExitButton
    bool toggleSave = false; // AccountLoginSaveAccountName tapped
    bool server = false;     // the server button tapped: change the address
    bool offline = false;
};
AccountLoginResult accountLogin(DialogContext& c, const AccountLoginView& v);

// --- GlueDialog --------------------------------------------------------------------------------------------

struct GlueDialogView {
    std::string_view text;
    std::string_view button1; // GlueDialogButton1 (CANCEL, OKAY, YES...)
    std::string_view button2; // empty: one button, centered
    bool button1Enabled = true;
    bool alert = false;   // GlueDialogAlertIcon (showAlert: 600 units wide)
    bool editBox = false; // GlueDialogEditBox (hasEditBox)
    std::string_view editText;
    bool editing = false;                  // the keyboard types into it
    f32 editWidth = 130.0f;                // units
    std::string_view name = "glue.dialog"; // its widgets' ids
};
struct GlueDialogResult {
    bool button1 = false;
    bool button2 = false;
    bool edit = false; // the edit box tapped
};
// Modal: nothing under it takes a finger (GlueDialog enableMouse), the screen is not dimmed.
GlueDialogResult glueDialog(DialogContext& c, const GlueDialogView& v);

// CharacterDeleteDialog and CharacterRenameDialog: GlueDialog's look with two lines, the alert icon left and
// an edit box, OKAY (disabled until DELETE is typed) and CANCEL.
struct GlueEditDialogView {
    std::string_view text1, text2;
    std::string_view editText;
    bool editing = false;
    bool okEnabled = true;
    bool fixedHeight = false; // CharacterRenameDialog keeps its 256 units; the delete dialog fits its text
    std::string_view name;
};
GlueDialogResult glueEditDialog(DialogContext& c, const GlueEditDialogView& v);

// --- RealmList ---------------------------------------------------------------------------------------------

struct RealmListView {
    std::span<const RealmRow> rows;
    i32 selected = -1; // into rows (the locked highlight); -1 none
    bool okEnabled = false;
    f32 alpha = 1.0f;
    bool interactive = true;
};
struct RealmListResult {
    i32 select = -1; // a row tapped (RealmSelectButton_OnClick)
    i32 enter = -1;  // a second tap on the selected row (OnDoubleClick): choose it
    bool ok = false;
    bool cancel = false; // Cancel, the close button
    i32 sort = -1;       // a column header tapped: RealmSortKey
};
RealmListResult realmList(DialogContext& c, const RealmListView& v);

// --- CharacterSelect ---------------------------------------------------------------------------------------

struct CharacterRowView {
    std::string_view name;
    std::string_view info;     // CHARACTER_SELECT_INFO(_GHOST): "Level 60 Warrior"
    std::string_view location; // the zone (AreaTable)
};
struct CharacterSelectView {
    std::span<const CharacterRowView> characters;
    i32 selected = -1;
    std::string_view realm; // CharSelectRealmName: "<realm> (PVP)", with "\n(Server down)" when offline
    std::string_view selectedName; // CharSelectCharacterName
    bool canCreate = true;         // CharSelectCreateCharacterButton (fewer than 10, connected)
    bool connected = true;         // Change Realm, Delete and Enter World need the server
    f32 alpha = 1.0f;
    bool interactive = true;
};
struct CharacterSelectResult {
    i32 select = -1; // CharacterSelectButton_OnClick
    i32 enter = -1;  // a second tap on the selected one (OnDoubleClick): enter the world with it
    bool enterWorld = false;
    bool create = false;
    bool remove = false; // CharacterSelectDeleteButton
    bool changeRealm = false;
    bool back = false;      // CharacterSelectBackButton
    // A drag on the scene: the degrees it turned the character this frame (touch: no rotate buttons).
    f32 turnDegrees = 0.0f;
};
CharacterSelectResult characterSelect(DialogContext& c, const CharacterSelectView& v);

// The WoW logo (Glues-WoW-Logo) top left, as both screens place it (scaled down on short screens). Exposed
// for the creation screen.
void glueLogo(DialogContext& c, f32 alpha);

} // namespace vd
