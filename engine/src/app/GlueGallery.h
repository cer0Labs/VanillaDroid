#pragma once

#include "app/Dialogs.h"
#include "app/GlueModel.h"
#include "app/GlueWindows.h"
#include "core/Types.h"
#include "net/RealmListMessages.h"
#include "ui/TextInput.h"

#include <array>
#include <string_view>
#include <vector>

namespace vd {

// Development (phase 18, glue): VD_UI_GALLERY pages of the glue screens (app/GlueWindows) with sample data
// worded by core/Text as app/GlueScreens words it (the 3D scene is not drawn under the gallery: VD_GLUE_DEMO
// shows the screens over it):
//  - login: AccountLogin (a tap on a field edits it, LOGIN shows the steps of a login, the server button asks
//    for the address);
//  - login2: AccountLogin under the OKAY of a wrong password (LOGIN_UNKNOWN_ACCOUNT);
//  - login3: AccountLogin under the CANCEL of a step (LOGIN_STATE_AUTHENTICATING);
//  - login4: AccountLogin without a realm server (a fresh install: VD_LOGIN_NO_SERVER under its button);
//  - loginkb: the password edited with the keyboard over the bottom 45% (the field and LOGIN in what is
//    left);
//  - serverdialog: the realm server's address (ours: realmlist.wtf in a GlueDialog with an edit box);
//  - queue: the login queue's dialog (QUEUE_NAME_TIME_LEFT_UNKNOWN, CHANGE_REALM);
//  - realms: RealmList over AccountLogin (five realms: types, populations, characters, one offline);
//  - realmfull: REALM_IS_FULL over it;
//  - charselect: CharacterSelect with eight characters (a ghost among them);
//  - charselect2: CharacterSelect without characters;
//  - chardelete: CharacterDeleteDialog over it ("DEL" typed: OKAY still disabled);
//  - rename: CharacterRenameDialog after a name too short.
// What the player taps goes to the log ("UI gallery: glue ..."); each page logs its frames' CPU time every
// 600 frames.
class GlueGallery {
public:
    GlueGallery();
    // One of the names above: true when it is one (the page to draw).
    bool select(std::string_view page);
    void draw(DialogContext& c, ui::TextInput& input);

private:
    enum class Page : u8 {
        Login,
        LoginError,
        LoginBusy,
        LoginNoServer,
        LoginKeyboard,
        ServerDialog,
        Queue,
        Realms,
        RealmFull,
        Characters,
        NoCharacters,
        Delete,
        Rename
    };
    void login(DialogContext& c, ui::TextInput& input);
    void realms(DialogContext& c);
    void characters(DialogContext& c);

    Page m_page = Page::Login;
    u64 m_startMs = 0;
    // AccountLogin.
    FormText<64> m_account;
    FormText<64> m_password;
    bool m_save = true;
    u64 m_loginAtMs = 0; // LOGIN tapped: the login's steps play
    std::array<char, 128> m_version{};
    std::string_view m_versionText;
    std::array<char, 256> m_scratch{};
    // RealmList.
    std::vector<net::RealmInfo> m_realms;
    std::vector<RealmRow> m_rows;
    RealmSort m_sort;
    i32 m_realmSelected = 0;
    // CharacterSelect.
    std::array<CharacterRowView, 8> m_characters{};
    std::array<std::array<char, 64>, 8> m_info{};
    i32 m_selected = 0;
    f32 m_facing = 0.0f;
    // The page's frames' CPU time.
    u32 m_costFrames = 0;
    f32 m_costSum = 0.0f;
    f32 m_costWorst = 0.0f;
};

} // namespace vd
