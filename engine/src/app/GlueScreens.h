#pragma once

#include "app/Dialogs.h"
#include "app/GlueModel.h"
#include "app/GlueKeys.h" // phase 18 (extras)
#include "app/GlueScene.h"
#include "app/GlueWindows.h"
#include "app/UiSound.h"
#include "app/UpdateScreens.h" // phase 22 (updates)
#include "core/Types.h"
#include "net/Messages.h"

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd {

class GameSession;
class TravelScreens;

// Phase 18 (glue): the glue screens before the world, as the 1.12 client runs them (GlueParent, AccountLogin,
// GlueDialog, RealmList, CharacterSelect and its delete and rename dialogs; CharacterCreate is
// app/SocialScreens'): which screen shows and fades, the dialog the session's state opens, the realm list and
// the characters filled from GameSession and what the player does applied back (app/GlueWindows draws them),
// the scene they want behind them (GlueSceneRequest: app/GlueStage draws it), the sounds, and WTF\Config.wtf
// (the remembered account name, the last realm, the realm server's address and the last character).
// GameScreens::drawLogin calls it every frame before the world. Main thread; frames do not allocate once
// warmed up.
class GlueScreens {
public:
    GlueScreens();

    // Where WTF\Config.wtf is kept (desktop: packs/.cache; Android: the app's files folder; empty: nothing
    // kept) and how files are written (the game's background writer). Reads it now.
    using FileWriter = void (*)(void* context, const std::filesystem::path& path, std::string text);
    void setStorage(const std::filesystem::path& folder, FileWriter writer, void* context);
    // The realm server's other sources (1.12's realmlist): a development launch's address (--realmd, the
    // intent's `realmd`) and client.cfg's realm.address, either empty. serverAddress(): the address the
    // login starts with (glueRealmList: the launch's, the player's own, client.cfg's).
    void setServerSources(std::string_view launch, std::string_view configured);
    std::string_view serverAddress() const;
    void setUiSoundSink(UiSoundSink sink) { m_sound = sink; }
    // The loading screen shown once a character enters the world (TravelScreens::drawLoading).
    void setLoadingScreen(TravelScreens* travel) { m_travel = travel; }

    struct Result {
        bool create = false;  // CREATE_NEW_CHARACTER: the caller starts CharacterCreate
        bool offline = false; // development (VD_GLUE_OFFLINE): the world without a server
    };
    // One frame of the screens; `scene` gets what they want behind them.
    Result draw(GameSession& session, DialogContext& c, ui::TextInput& input, GlueSceneRequest& scene);
    // Phase 18 (extras): a hardware key (GlueXML's OnKeyDown), applied on the next frame to the screen or dialog on
    // top.
    void key(GlueKey key) { m_key = key; }

private:
    enum class Screen : u8 { Login, CharacterSelect };
    enum class Dialog : u8 { None, ServerAddress, RealmIsFull, Delete, Rename };

    void loadConfig();
    void saveConfig();
    void fillForm(const GameSession& session);
    // The address dialog, its edit box focused; `forLogin`: opened by LOGIN without a server (logs in after).
    void openServerDialog(ui::TextInput& input, bool forLogin);
    void play(UiSound sound) const { m_sound(sound); }
    void endInput(ui::TextInput& input) const;
    // Session events this frame: the character list, the server's answers to delete and rename, the realm.
    void follow(GameSession& session);
    GlueSessionState sessionState(const GameSession& session) const;

    void drawLogin(GameSession& session, DialogContext& c, ui::TextInput& input, f32 alpha, bool interactive,
                   Result& result);
    void drawCharacters(GameSession& session, DialogContext& c, f32 alpha, bool interactive,
                        Result& result);
    void drawRealms(GameSession& session, DialogContext& c, bool interactive);
    // The dialog over the screen: the session's status first, else one of ours. True when one is shown.
    bool drawDialogs(GameSession& session, DialogContext& c, ui::TextInput& input);
    void dialogKey(GlueDialogResult& r, bool twoButtons, bool okEnabled) const; // phase 18 (extras)
    void enterWorld(GameSession& session);
    void fillScene(const GameSession& session, GlueSceneRequest& scene) const;

    // Development (VD_GLUE_DEMO=login|charselect|realms): the screens with sample characters and realms, no
    // server.
    bool demo() const { return m_demo != 0; }
    std::span<const net::CharacterInfo> characters(const GameSession& session) const;

    UiSoundSink m_sound;
    TravelScreens* m_travel = nullptr;
    GlueKey m_key = GlueKey::None;      // phase 18 (extras): the key waiting for the next frame...
    GlueKey m_frameKey = GlueKey::None; // ...and this frame's
    std::filesystem::path m_configPath;
    std::filesystem::path m_realmlistPath; // realmlist.wtf: read, never written
    FileWriter m_writer = nullptr;
    void* m_writerContext = nullptr;
    GlueConfig m_config;
    std::string m_launchServer; // setServerSources
    std::string m_configuredServer;
    bool m_offlineButton = false; // VD_GLUE_OFFLINE

    Screen m_screen = Screen::Login;
    u64 m_screenSinceMs = 0;  // when the current screen showed (its fade in)
    u64 m_leavingSinceMs = 0; // AccountLoginUI fading out towards the character list (0: not)

    // AccountLogin.
    bool m_formFilled = false;
    FormText<64> m_account;
    FormText<64> m_password; // only until it is hashed (then wiped)
    FormText<96> m_server;   // realmd's host[:port]; empty: none set
    bool m_saveAccount = false;
    bool m_loginAfterServer = false; // the address dialog was opened by LOGIN...
    bool m_loginPending = false;     // ...and closed with an address: the login goes on next frame

    // GlueDialog and the other dialogs.
    Dialog m_dialog = Dialog::None;
    FormText<96> m_dialogText;        // the dialog's edit box
    std::string_view m_dialogMessage; // its text (a core/Text string), or empty for the default
    std::string m_failure;            // our own OKAY (a deletion refused, a locked character)
    bool m_deleting = false;
    u32 m_deleteVersion = 0;
    bool m_renaming = false;
    u32 m_renameVersion = 0;
    u64 m_actionGuid = 0; // the character being deleted or renamed
    std::array<char, 256> m_scratch{};
    std::array<char, 256> m_dialogScratch{};

    // RealmList.
    bool m_realmsShown = false;
    u32 m_realmsVersion = ~0u;
    RealmSort m_realmSort;
    bool m_realmsDirty = true;
    std::vector<RealmRow> m_realmRows;
    std::string m_realmSelected; // RealmList.selectedName
    u64 m_realmRefreshMs = 0;    // RealmList.refreshTime
    std::string m_realmFull;     // REALM_IS_FULL: the realm it asks about

    // CharacterSelect.
    i32 m_selected = -1;
    u32 m_listVersion = ~0u;
    CharacterListReason m_listReason = CharacterListReason::Login;
    bool m_creating = false;
    u32 m_createVersion = 0;
    f32 m_facing = 0.0f; // degrees (Get/SetCharacterSelectFacing)
    u32 m_enteringMap = 0;
    std::array<CharacterRowView, kMaxCharactersDisplayed> m_rows{};
    std::array<std::array<char, 64>, kMaxCharactersDisplayed> m_rowInfo{};
    std::array<char, 160> m_realmText{};
    std::array<char, 128> m_version{};
    std::string_view m_versionText;

    // Development samples.
    u32 m_demo = 0; // 0 none, 1 login, 2 charselect, 3 realms
    std::vector<net::CharacterInfo> m_demoCharacters;
    std::vector<net::RealmInfo> m_demoRealms;

    UpdateScreens m_updates; // phase 22 (updates): a new release, game data from another version
};

} // namespace vd
