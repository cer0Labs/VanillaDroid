#include "app/GlueScreens.h"

#include "app/GameSession.h"
#include "app/TravelScreens.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/CharCreate.h"
#include "game/CharacterNames.h"
#include "game/SocialText.h"
#include "net/CharRenameMessages.h"
#include "net/ResponseCodes.gen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>

namespace vd {
namespace {

constexpr ui::WidgetId kAccountField = ui::widgetId("glue.field.account");
constexpr ui::WidgetId kPasswordField = ui::widgetId("glue.field.password");
constexpr ui::WidgetId kDialogField = ui::widgetId("glue.field.dialog");
constexpr u64 kRealmListRefreshMs = 5000;               // REALM_LIST_REFRESH_TIME
constexpr u32 kCharacterFlagLockedForTransfer = 0x0004; // CHARACTER_FLAG_LOCKED_FOR_TRANSFER
constexpr u32 kCharacterFlagGhost = 0x2000;             // CHARACTER_FLAG_GHOST
constexpr f32 kDegrees = 3.14159265358979f / 180.0f;

using RC = net::ResponseCode;

std::string readText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return file ? std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>())
                : std::string{};
}

bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

// Appends `s` to `out` at `at` (cut to fit, NUL-terminated); returns the new length.
usize append(std::span<char> out, usize at, std::string_view s) {
    at = std::min(at, out.size() - 1);
    const usize n = std::min(s.size(), out.size() - 1 - at);
    std::memcpy(out.data() + at, s.data(), n);
    out[at + n] = '\0';
    return at + n;
}

// Development samples (VD_GLUE_DEMO): characters of every scene, as SMSG_CHAR_ENUM would list them.
net::CharacterInfo sampleCharacter(const char* name, u8 race, u8 cls, u8 gender, u8 level, u32 zone, u32 map,
                                   u32 flags) {
    net::CharacterInfo c;
    c.guid = 0x100u + race * 16u + cls;
    c.name = name;
    c.race = race;
    c.cls = cls;
    c.gender = gender;
    c.level = level;
    c.zone = zone;
    c.map = map;
    c.flags = flags;
    c.skin = 1;
    c.face = 2;
    c.hairStyle = 3;
    c.hairColor = 2;
    c.facialHair = 1;
    return c;
}

net::RealmInfo sampleRealm(const char* name, u32 icon, u8 flags, f32 population, u8 characters) {
    net::RealmInfo r;
    r.name = name;
    r.icon = icon;
    r.flags = flags;
    r.address = "127.0.0.1:8085";
    r.population = population;
    r.characters = characters;
    r.category = 1;
    return r;
}

} // namespace

GlueScreens::GlueScreens() {
    m_realmRows.reserve(64);
    m_realmSelected.reserve(64);
    m_realmFull.reserve(64);
    m_failure.reserve(256);
    const char* offline = VD_DEV_ENV("VD_GLUE_OFFLINE");
    m_offlineButton = offline && std::strcmp(offline, "0") != 0;
    if (const char* demo = VD_DEV_ENV("VD_GLUE_DEMO")) {
        const std::string_view page(demo);
        m_demo = page == "login" ? 1u : page == "charselect" ? 2u : page == "realms" ? 3u : 0u;
        if (m_demo == 0) {
            VD_LOG_WARN("VD_GLUE_DEMO=%s: no such page (login, charselect, realms)", demo);
        }
    }
    if (m_demo != 0) {
        // One of each scene: Elwynn, Durotar, Dun Morogh, Teldrassil, Tirisfall, Mulgore; a gnome in the
        // dwarves' and a troll in the orcs'; a ghost; one marked for a new name.
        m_demoCharacters = {sampleCharacter("Arthel", 1, 2, 0, 60, 1519, 0, 0),
                            sampleCharacter("Thrallita", 2, 3, 1, 12, 14, 1, 0),
                            sampleCharacter("Brunhilda", 3, 1, 1, 5, 1, 0, 0),
                            sampleCharacter("Hojaverde", 4, 11, 1, 20, 141, 1, kCharacterFlagGhost),
                            sampleCharacter("Sombrio", 5, 9, 0, 33, 85, 0, 0),
                            sampleCharacter("Pezuna", 6, 3, 0, 8, 215, 1, 0),
                            sampleCharacter("Chispa", 7, 8, 1, 14, 1, 0, net::kCharacterFlagRename),
                            sampleCharacter("Zuljin", 8, 4, 0, 27, 14, 1, 0)};
        m_demoRealms = {sampleRealm("Dev Realm", 1, 0, 0.3f, 8),
                        sampleRealm("Kirin Tor", 6, net::kRealmFlagNewPlayers, 1.0f, 0),
                        sampleRealm("Archimonde", 1, 0, 1.9f, 2),
                        sampleRealm("Frostmane", 8, net::kRealmFlagFull, 2.0f, 0),
                        sampleRealm("Gurubashi", 1, net::kRealmFlagOffline, 0.0f, 1)};
        m_screen = m_demo == 2 ? Screen::CharacterSelect : Screen::Login;
    }
}

// --- WTF\Config.wtf ----------------------------------------------------------------------------------------

void GlueScreens::setStorage(const std::filesystem::path& folder, FileWriter writer, void* context) {
    m_configPath = folder.empty() ? std::filesystem::path{} : folder / "WTF" / "Config.wtf";
    m_realmlistPath = folder.empty() ? std::filesystem::path{} : folder / "realmlist.wtf";
    m_writer = writer;
    m_writerContext = context;
    loadConfig();
}

void GlueScreens::setServerSources(std::string_view launch, std::string_view configured) {
    m_launchServer.assign(launch);
    m_configuredServer.assign(configured);
}

std::string_view GlueScreens::serverAddress() const {
    return glueRealmList(m_launchServer, m_config.realmList, m_configuredServer);
}

void GlueScreens::loadConfig() {
    if (m_configPath.empty()) {
        return;
    }
    std::error_code error;
    if (std::filesystem::exists(m_configPath, error)) {
        m_config = parseGlueConfig(readText(m_configPath));
        VD_LOG_INFO("Glue: %s (account name %s, realm \"%s\", server \"%s\")", m_configPath.string().c_str(),
                    m_config.accountName.empty() ? "not kept" : "kept", m_config.realmName.c_str(),
                    m_config.realmList.c_str());
    }
    // realmlist.wtf next to WTF (1.12 reads it after Config.wtf: its address wins, and the next save of
    // Config.wtf keeps it).
    if (std::filesystem::exists(m_realmlistPath, error)) {
        if (const GlueConfig file = parseGlueConfig(readText(m_realmlistPath)); !file.realmList.empty()) {
            m_config.realmList = file.realmList;
            VD_LOG_INFO("Glue: %s (server \"%s\")", m_realmlistPath.string().c_str(),
                        m_config.realmList.c_str());
        }
    }
}

void GlueScreens::saveConfig() {
    m_config.loaded = true;
    if (m_writer && !m_configPath.empty() && !demo()) {
        m_writer(m_writerContext, m_configPath, writeGlueConfig(m_config));
    }
}

void GlueScreens::fillForm(const GameSession& session) {
    // What Config.wtf keeps (once kept, its account name counts even when empty: Remember Account Name was
    // unchecked), else what the configuration or the launch said. The password is never kept. The server:
    // serverAddress()'s order.
    const net::LoginParams& params = session.loginParams();
    m_account.set(m_config.loaded && !session.hasCredentials() ? std::string_view(m_config.accountName)
                                                               : std::string_view(params.account));
    m_saveAccount = !m_config.accountName.empty();
    m_server.set(serverAddress());
    m_formFilled = true;
}

void GlueScreens::openServerDialog(ui::TextInput& input, bool forLogin) {
    m_dialog = Dialog::ServerAddress;
    m_dialogText.set(m_server.view());
    m_dialogMessage = {};
    m_loginAfterServer = forLogin;
    input.begin(kDialogField, m_dialogText.view(), 95, ui::TextKind::Plain);
}

void GlueScreens::endInput(ui::TextInput& input) const {
    const u64 field = input.field();
    if (field == kAccountField || field == kPasswordField || field == kDialogField) {
        input.end();
    }
}

std::span<const net::CharacterInfo> GlueScreens::characters(const GameSession& session) const {
    return demo() ? std::span<const net::CharacterInfo>(m_demoCharacters) : session.characters();
}

// --- The session -------------------------------------------------------------------------------------------

GlueSessionState GlueScreens::sessionState(const GameSession& session) const {
    using Step = GlueSessionState::Step;
    GlueSessionState s;
    s.failure = m_failure;
    if (demo()) {
        s.step = m_demo == 2 ? Step::CharacterList : Step::LoggedOut;
        return s;
    }
    switch (session.phase()) {
    case GameSession::Phase::LoggedOut:
        s.step = session.reconnecting() ? Step::Reconnecting : Step::LoggedOut;
        break;
    case GameSession::Phase::Connecting:
        switch (session.netState()) {
        case net::NetState::LoggingIn:
            s.step = session.loginStep() == net::LoginStep::Authenticating  ? Step::Authenticating
                     : session.loginStep() == net::LoginStep::Authenticated ? Step::Authenticated
                                                                            : Step::Connecting;
            break;
        case net::NetState::ChoosingRealm: s.step = Step::ChoosingRealm; break;
        case net::NetState::Queued: s.step = Step::Queued; break;
        case net::NetState::Online: s.step = Step::CharacterList; break;
        default: s.step = Step::WorldConnecting; break;
        }
        break;
    case GameSession::Phase::CharacterSelect: s.step = Step::CharacterList; break;
    case GameSession::Phase::EnteringWorld: s.step = Step::EnteringWorld; break;
    case GameSession::Phase::InWorld: s.step = Step::InWorld; break;
    }
    if (session.statusIsError()) {
        s.failure = session.statusText();
    }
    s.queuePosition = session.queuePosition();
    s.realm = session.realmName();
    s.characterListPending = session.characterListPending();
    s.realmListPending = session.realmListPending();
    s.realmListShown = session.realmListShown();
    s.deleting = m_deleting;
    s.renaming = m_renaming;
    return s;
}

void GlueScreens::follow(GameSession& session) {
    if (demo()) {
        if (m_listVersion == ~0u) {
            m_listVersion = 0;
            m_selected = selectedCharacter(static_cast<u32>(m_demoCharacters.size()),
                                           m_config.lastCharacterIndex, CharacterListReason::Login);
        }
        return;
    }
    // CharacterCreate is over (the glue screens draw only once it is: app/GameScreens) with a new character
    // as its last answer: the list selects the last one (SELECT_LAST_CHARACTER).
    if (m_creating) {
        m_creating = false;
        if (session.charResultVersion() != m_createVersion &&
            session.charResult() == static_cast<u8>(RC::CHAR_CREATE_SUCCESS)) {
            m_listReason = CharacterListReason::Created;
        }
    }
    // The server's answer to a deletion: SELECT_FIRST_CHARACTER, or CHAR_DELETE_FAILED in an OKAY.
    if (m_deleting && session.charResultVersion() != m_deleteVersion) {
        m_deleting = false;
        const u8 code = session.charResult();
        if (code == static_cast<u8>(RC::CHAR_DELETE_SUCCESS)) {
            m_listReason = CharacterListReason::Deleted;
        } else {
            m_failure.assign(game::charResultText(code));
        }
    }
    // The answer to a rename (WoW.exe 0x46B440): renamed in the list, or CharacterRenameDialog again with
    // CHAR_CREATE_NAME_IN_USE (its own text) or CHAR_RENAME_FAILED.
    if (m_renaming && session.renameVersion() != m_renameVersion) {
        m_renaming = false;
        const u8 code = session.renameResult();
        if (code != static_cast<u8>(RC::RESPONSE_SUCCESS)) {
            m_dialog = Dialog::Rename;
            m_dialogText.wipe();
            m_dialogMessage =
                text::get(code == static_cast<u8>(RC::CHAR_CREATE_NAME_IN_USE) ? "CHAR_CREATE_NAME_IN_USE"
                                                                               : "CHAR_RENAME_FAILED");
        }
    }
    // A new list (CHARACTER_LIST_UPDATE): the character it selects (WoW.exe 0x4724D0).
    if (session.characterListVersion() != m_listVersion) {
        m_listVersion = session.characterListVersion();
        m_selected = selectedCharacter(static_cast<u32>(session.characters().size()),
                                       m_config.lastCharacterIndex, m_listReason);
        m_listReason = CharacterListReason::Login;
        m_facing = 0.0f;
    }
    // Connected to a realm: the last realm connected to (the realmName CVar).
    if (session.phase() == GameSession::Phase::CharacterSelect && !session.realmName().empty() &&
        session.realmName() != m_config.realmName) {
        m_config.realmName.assign(session.realmName());
        saveConfig();
    }
}

// --- One frame ---------------------------------------------------------------------------------------------

GlueScreens::Result GlueScreens::draw(GameSession& session, DialogContext& c, ui::TextInput& input,
                                      GlueSceneRequest& scene) {
    VD_PROFILE_ZONE("GlueScreens::draw");
    Result result;
    m_frameKey = std::exchange(m_key, GlueKey::None); // phase 18 (extras)
    const u64 now = c.ui.nowMs();
    if (m_screenSinceMs == 0) {
        m_screenSinceMs = now;
    }
    if (!m_formFilled) {
        fillForm(session);
        m_versionText = glueVersionText(m_version);
        if (!m_config.realmName.empty()) {
            session.setPreferredRealm(m_config.realmName);
        }
    }
    follow(session);

    // Which screen (GlueParent's SetGlueScreen): the character list once the session has one (also while it
    // moves to another realm), else AccountLogin. AccountLoginUI fades out first (GlueScreenExit:
    // LOGIN_FADE_OUT).
    const GameSession::Phase phase =
        demo() ? (m_demo == 2 ? GameSession::Phase::CharacterSelect : GameSession::Phase::LoggedOut)
               : session.phase();
    const bool onList = phase == GameSession::Phase::CharacterSelect ||
                        phase == GameSession::Phase::EnteringWorld ||
                        (phase == GameSession::Phase::Connecting && m_screen == Screen::CharacterSelect &&
                         session.netState() != net::NetState::ChoosingRealm &&
                         session.netState() != net::NetState::LoggingIn);
    if (onList && m_screen == Screen::Login) {
        if (m_leavingSinceMs == 0) {
            m_leavingSinceMs = now;
            endInput(input);
        } else if (static_cast<f32>(now - m_leavingSinceMs) / 1000.0f >= kLoginFadeOut) {
            m_screen = Screen::CharacterSelect;
            m_screenSinceMs = now;
            m_leavingSinceMs = 0;
        }
    } else if (!onList && m_screen == Screen::CharacterSelect && !demo()) {
        m_screen =
            Screen::Login; // logged out or disconnected (AccountLogin_OnShow: the password field empty)
        m_screenSinceMs = now;
        m_leavingSinceMs = 0;
        m_password.wipe();
        m_dialog = Dialog::None;
        m_deleting = m_renaming = false;
        endInput(input);
    } else if (!onList) {
        m_leavingSinceMs = 0;
    }

    // EnterWorld: the loading screen of the character's map until the world is there.
    if (phase == GameSession::Phase::EnteringWorld && m_travel) {
        scene = GlueSceneRequest{};
        // The one entered here, or the one the session entered by itself (--character).
        u32 map = m_enteringMap;
        for (const net::CharacterInfo& ch : session.characters()) {
            map = ch.name == session.characterName() ? ch.map : map;
        }
        m_travel->drawLoading(c, map, 0.05f);
        return result;
    }

    fillScene(session, scene);
    const bool realms = m_demo == 3 || (!demo() && session.realmListShown());
    // A dialog takes the fingers from what is under it.
    const bool dialog =
        glueStatus(sessionState(session), m_scratch).type != GlueDialogType::None || m_dialog != Dialog::None;
    // Phase 22 (updates): a release's notice when no dialog of the session's or ours is up (app/UpdateScreens).
    const bool notice = !dialog && !realms && m_leavingSinceMs == 0 && m_updates.wanted();
    const bool covered = dialog || notice;
    const f32 seconds = static_cast<f32>(now - m_screenSinceMs) / 1000.0f;
    if (m_screen == Screen::Login) {
        const f32 alpha = m_leavingSinceMs != 0 ? glueFade(static_cast<f32>(now - m_leavingSinceMs) / 1000.0f,
                                                           kLoginFadeOut, false)
                                                : glueFade(seconds, kLoginFadeIn, true);
        drawLogin(session, c, input, alpha, !covered && !realms && m_leavingSinceMs == 0, result);
    } else {
        drawCharacters(session, c, glueFade(seconds, kCharacterSelectFadeIn, true), !covered && !realms, result);
    }
    if (realms && m_dialog != Dialog::RealmIsFull) { // RealmList_OnOk hides the list before REALM_IS_FULL
        drawRealms(session, c, !covered);
    } else if (!realms) {
        m_realmsShown = false;
    }
    drawDialogs(session, c, input);
    if (notice && m_updates.draw(c, m_frameKey)) { // phase 22 (updates)
        play(UiSound::TitleOptionOk);
    }
    return result;
}

void GlueScreens::fillScene(const GameSession& session, GlueSceneRequest& scene) const {
    scene = GlueSceneRequest{};
    if (m_screen == Screen::Login) {
        scene.background = GlueBackground::MainMenu; // AccountLogin: UI_MainMenu
        return;
    }
    // CharacterSelect: the selected character's race scene (SetBackgroundModel), UI_Orc before any (OnLoad).
    scene.background = GlueBackground::Orc;
    const std::span<const net::CharacterInfo> list = characters(session);
    if (m_selected >= 0 && static_cast<usize>(m_selected) < list.size()) {
        const net::CharacterInfo& chosen = list[static_cast<usize>(m_selected)];
        scene.background = glueBackgroundOfRace(chosen.race);
        scene.hasCharacter = true;
        scene.character = lookOfCharacter(chosen);
        scene.characterFacing = m_facing * kDegrees;
    }
}

// --- AccountLogin ------------------------------------------------------------------------------------------

void GlueScreens::drawLogin(GameSession& session, DialogContext& c, ui::TextInput& input, f32 alpha,
                            bool interactive, Result& result) {
    // The field the keyboard types into follows it; the form keeps its text.
    i32 editing = -1;
    if (input.active() && input.field() == kAccountField) {
        m_account.set(input.text());
        editing = 0;
    } else if (input.active() && input.field() == kPasswordField) {
        m_password.set(input.text());
        editing = 1;
    }
    const auto edit = [&](i32 field) {
        if (field == 0) {
            input.begin(kAccountField, m_account.view(), 16, ui::TextKind::Plain); // letters="16"
        } else {
            input.begin(kPasswordField, m_password.view(), 16, ui::TextKind::Secret);
        }
    };
    const auto login = [&] {
        // AccountLogin_Login: gsLogin, DefaultServerLogin, the password field emptied, the account name kept
        // or not. Ours first: without a realm server, the address dialog (the login once it has one).
        play(UiSound::GlueLogin);
        if (glueNeedsServer(m_server.view())) {
            openServerDialog(input, true);
            return;
        }
        endInput(input);
        m_config.accountName.assign(m_saveAccount ? m_account.view() : std::string_view{});
        saveConfig();
        if (demo()) {
            VD_LOG_INFO("Glue demo: DefaultServerLogin(%.*s)", static_cast<int>(m_account.view().size()),
                        m_account.view().data());
        } else {
            session.clearStatus();
            m_failure.clear();
            session.loginWith(m_account.view(), m_password.view(), m_server.view());
        }
        m_password.wipe();
    };
    if (m_loginPending && interactive) { // the address dialog opened by LOGIN closed with an address
        m_loginPending = false;
        login();
        editing = -1;
    }
    // Enter: on the account name the password comes next while it is empty (the keyboard's "next" key), else
    // LOGIN.
    if (editing >= 0 && input.submitted()) {
        if (editing == 0 && m_password.empty()) {
            edit(1);
            editing = 1;
        } else {
            login();
            editing = -1;
        }
    }
    if (editing >= 0 && input.cancelled()) {
        editing = -1;
    }

    AccountLoginView v;
    v.account = m_account.view();
    v.passwordLength = static_cast<u32>(m_password.view().size());
    v.editing = editing;
    v.saveAccountName = m_saveAccount;
    v.realmName = m_config.realmName;
    v.server = m_server.view();
    v.version = m_versionText;
    v.alpha = alpha;
    v.interactive = interactive;
    v.offline = m_offlineButton;
    AccountLoginResult r = accountLogin(c, v);
    if (interactive) { // phase 18 (extras): AccountLogin_OnKeyDown
        r.quit = r.quit || m_frameKey == GlueKey::Escape;
        r.login = r.login || m_frameKey == GlueKey::Enter;
    }
    if (r.edit >= 0 && r.edit != editing) {
        edit(r.edit);
    }
    if (r.login) {
        login();
    }
    if (r.quit) {
        play(UiSound::GlueQuit); // AccountLogin_Exit: QuitGame
        endInput(input);
        if (!demo()) {
            session.forceQuit();
        }
    }
    if (r.toggleSave) {
        m_saveAccount = !m_saveAccount;
        // AccountLoginSaveAccountName's OnClick: "Off" when it ends up checked (sic), "On" otherwise.
        play(m_saveAccount ? UiSound::CheckBoxOff : UiSound::CheckBoxOn);
    }
    if (r.server) {
        openServerDialog(input, false);
    }
    if (r.offline) {
        endInput(input);
        result.offline = true;
    }
}

// --- CharacterSelect ---------------------------------------------------------------------------------------

void GlueScreens::drawCharacters(GameSession& session, DialogContext& c, f32 alpha, bool interactive,
                                 Result& result) {
    const std::span<const net::CharacterInfo> list = characters(session);
    const u32 count = static_cast<u32>(std::min<usize>(list.size(), kMaxCharactersDisplayed));
    if (m_selected >= static_cast<i32>(count)) {
        m_selected = count > 0 ? 0 : -1;
    }
    const game::AreaData* areas = session.play().travel().areas();
    for (u32 i = 0; i < count; ++i) {
        const net::CharacterInfo& ch = list[i];
        CharacterRowView& row = m_rows[i];
        row.name = ch.name;
        // UpdateCharacterList: CHARACTER_SELECT_INFO(_GHOST) with the class; the zone's name.
        row.info = text::format(m_rowInfo[i],
                                (ch.flags & kCharacterFlagGhost) ? "CHARACTER_SELECT_INFO_GHOST"
                                                                 : "CHARACTER_SELECT_INFO",
                                static_cast<u32>(ch.level), game::className(ch.cls));
        const game::AreaInfo* zone = areas ? areas->area(ch.zone) : nullptr;
        row.location = zone ? zone->name : std::string_view{};
    }
    // CharSelectRealmName: the realm, "(Server down)" under it when not connected, then its type.
    const bool connected = demo() || session.phase() == GameSession::Phase::CharacterSelect;
    usize at = 0;
    const std::string_view realm = demo() ? std::string_view(m_demoRealms.front().name) : session.realmName();
    m_realmText[0] = '\0';
    if (!realm.empty()) {
        at = append(m_realmText, at, realm);
        if (!connected) {
            at = append(m_realmText, at, "\n(");
            at = append(m_realmText, at, text::get("SERVER_DOWN"));
            at = append(m_realmText, at, ")");
        }
        const std::span<const net::RealmInfo> realms =
            demo() ? std::span<const net::RealmInfo>(m_demoRealms) : session.realms();
        for (const net::RealmInfo& r : realms) {
            bool pvp = false, rp = false;
            realmType(r.icon, pvp, rp);
            if (r.name == realm && (pvp || rp)) {
                at = append(m_realmText, at, " ");
                at = append(m_realmText, at,
                            text::get(pvp && rp ? "RPPVP_PARENTHESES"
                                      : pvp     ? "PVP_PARENTHESES"
                                                : "RP_PARENTHESES"));
            }
        }
    }
    CharacterSelectView v;
    v.characters = {m_rows.data(), count};
    v.selected = m_selected;
    v.realm = {m_realmText.data(), at};
    v.selectedName = m_selected >= 0 ? m_rows[static_cast<usize>(m_selected)].name : std::string_view{};
    v.canCreate = connected && count < kMaxCharactersDisplayed;
    v.connected = connected;
    v.alpha = alpha;
    v.interactive = interactive;
    CharacterSelectResult r = characterSelect(c, v);
    if (interactive) { // phase 18 (extras): CharacterSelect_OnKeyDown
        r.back = r.back || m_frameKey == GlueKey::Escape;
        r.enterWorld = r.enterWorld || m_frameKey == GlueKey::Enter;
        if ((m_frameKey == GlueKey::Previous || m_frameKey == GlueKey::Next) && count > 1 && m_selected >= 0) {
            const i32 n = static_cast<i32>(count);
            r.select = (m_selected + (m_frameKey == GlueKey::Next ? 1 : n - 1)) % n; // wrapping round
        }
    }

    // The drag on the scene (CHARACTER_ROTATION_CONSTANT; touch has no CharacterSelectRotateLeft / Right).
    m_facing += r.turnDegrees;
    if (r.select >= 0 &&
        r.select != m_selected) { // CharacterSelectButton_OnClick: SelectCharacter (no sound)
        m_selected = r.select;
        m_facing = 0.0f;
    }
    if (r.enter >= 0 || r.enterWorld) { // a second tap on the selected one (OnDoubleClick), or ENTER_WORLD
        if (r.enter >= 0) {
            m_selected = r.enter;
        }
        enterWorld(session);
    }
    if (r.create) {
        play(UiSound::GlueCreateCharacter);
        m_creating = true;
        m_createVersion = session.charResultVersion();
        result.create = true;
    }
    if (r.remove && m_selected >= 0) { // CharacterSelect_Delete: CharacterDeleteDialog
        play(UiSound::GlueDeleteCharacter);
        m_dialog = Dialog::Delete;
        m_dialogText.wipe();
        m_actionGuid = list[static_cast<usize>(m_selected)].guid;
    }
    if (r.changeRealm) { // CharacterSelect_ChangeRealm: RequestRealmList(1)
        play(UiSound::GlueRealmSelect);
        if (demo()) {
            m_demo = 3; // the realm list over the character list
            m_screen = Screen::CharacterSelect;
        } else {
            session.requestRealmList();
        }
    }
    if (r.back) { // CharacterSelect_Exit: DisconnectFromServer, the login screen
        play(UiSound::GlueCharacterExit);
        if (demo()) {
            VD_LOG_INFO("Glue demo: CharacterSelect_Exit");
        } else {
            session.logout();
        }
    }
}

void GlueScreens::enterWorld(GameSession& session) {
    const std::span<const net::CharacterInfo> list = characters(session);
    if (m_selected < 0 || static_cast<usize>(m_selected) >= list.size()) {
        return;
    }
    const net::CharacterInfo& ch = list[static_cast<usize>(m_selected)];
    // EnterWorld (WoW.exe 0x46B500): a character locked for a transfer is refused; one marked for a new name
    // asks for it (FORCE_RENAME_CHARACTER CHAR_RENAME_DESCRIPTION) instead of entering.
    if (ch.flags & kCharacterFlagLockedForTransfer) {
        m_failure.assign(game::charResultText(static_cast<u8>(RC::CHAR_LOGIN_LOCKED_FOR_TRANSFER)));
        return;
    }
    if (ch.flags & net::kCharacterFlagRename) {
        m_dialog = Dialog::Rename;
        m_dialogText.wipe();
        m_dialogMessage = text::get("CHAR_RENAME_DESCRIPTION");
        m_actionGuid = ch.guid;
        return;
    }
    play(UiSound::GlueEnterWorld);
    m_config.lastCharacterIndex = m_selected; // the lastCharacterIndex CVar
    saveConfig();
    m_enteringMap = ch.map;
    if (demo()) {
        VD_LOG_INFO("Glue demo: EnterWorld(%s)", ch.name.c_str());
        return;
    }
    session.enterCharacter(static_cast<u32>(m_selected));
}

// --- RealmList ---------------------------------------------------------------------------------------------

void GlueScreens::drawRealms(GameSession& session, DialogContext& c, bool interactive) {
    const std::span<const net::RealmInfo> realms =
        demo() ? std::span<const net::RealmInfo>(m_demoRealms) : session.realms();
    const std::string_view current = demo() ? std::string_view(m_demoRealms.front().name)
                                     : !session.realmName().empty() ? session.realmName()
                                                                    : std::string_view(m_config.realmName);
    const u64 now = c.ui.nowMs();
    if (!m_realmsShown) { // RealmList_OnShow: the current realm selected, the refresh timer started
        m_realmsShown = true;
        m_realmSelected.assign(current);
        m_realmRefreshMs = now + kRealmListRefreshMs;
        m_realmsDirty = true;
    }
    const u32 version = demo() ? 0u : session.realmListVersion();
    if (m_realmsDirty || version != m_realmsVersion) {
        m_realmsVersion = version;
        m_realmsDirty = false;
        realmRows(realms, current, m_realmSort, m_realmRows);
    }
    i32 selected = -1;
    for (usize i = 0; i < m_realmRows.size(); ++i) {
        if (m_realmRows[i].name == m_realmSelected) {
            selected = static_cast<i32>(i);
        }
    }
    // RealmList_OnUpdate: asked again REALM_LIST_REFRESH_TIME after it showed or the player last touched it.
    if (now >= m_realmRefreshMs) {
        m_realmRefreshMs = now + kRealmListRefreshMs;
        if (!demo()) {
            session.refreshRealmList();
        }
    }
    RealmListView v;
    v.rows = m_realmRows;
    v.selected = selected;
    v.okEnabled = selected >= 0 && !m_realmRows[static_cast<usize>(selected)].down;
    v.interactive = interactive;
    RealmListResult r = realmList(c, v);
    if (interactive) { // phase 18 (extras): RealmList_OnKeyDown
        r.cancel = r.cancel || m_frameKey == GlueKey::Escape;
        r.ok = r.ok || m_frameKey == GlueKey::Enter;
    }
    if (r.select >= 0 || r.enter >= 0 || r.sort >= 0) {
        m_realmRefreshMs = now + kRealmListRefreshMs;
    }
    if (r.select >= 0) { // RealmSelectButton_OnClick
        selected = r.select;
        m_realmSelected.assign(m_realmRows[static_cast<usize>(r.select)].name);
    }
    if (r.sort >= 0) { // SortRealms
        m_realmSort.by(static_cast<RealmSortKey>(r.sort));
        m_realmsDirty = true;
    }
    if ((r.ok || r.enter >= 0) && selected >= 0 && !m_realmRows[static_cast<usize>(selected)].down) {
        // RealmList_OnOk: a full realm without characters asks first (REALM_IS_FULL).
        play(UiSound::GlueRealmOk);
        const RealmRow& row = m_realmRows[static_cast<usize>(selected)];
        if (realmIsFull(row) && row.characters == 0) {
            m_dialog = Dialog::RealmIsFull;
            m_realmFull.assign(row.name);
        } else if (demo()) {
            VD_LOG_INFO("Glue demo: ChangeRealm(%.*s)", static_cast<int>(row.name.size()), row.name.data());
        } else {
            session.chooseRealm(row.name);
        }
    }
    if (r.cancel) { // RealmList_OnCancel
        play(UiSound::GlueRealmCancel);
        if (demo()) {
            VD_LOG_INFO("Glue demo: RealmListDialogCancelled");
            m_demo = m_screen == Screen::CharacterSelect ? 2u : 1u;
        } else {
            session.cancelRealmList();
        }
    }
}

// --- The dialogs -------------------------------------------------------------------------------------------

void GlueScreens::dialogKey(GlueDialogResult& r, bool twoButtons, bool okEnabled) const {
    // Phase 18 (extras): GlueDialog_OnKeyDown: Escape clicks GlueDialogButton2 (Button1 when alone), Enter Button1.
    if (m_frameKey == GlueKey::Escape) {
        r.button2 = r.button2 || twoButtons;
        r.button1 = r.button1 || !twoButtons;
    } else if (m_frameKey == GlueKey::Enter && okEnabled) {
        r.button1 = true;
    }
}

bool GlueScreens::drawDialogs(GameSession& session, DialogContext& c, ui::TextInput& input) {
    // The session's status (OPEN_STATUS_DIALOG) above all.
    const GlueStatus status = glueStatus(sessionState(session), m_scratch);
    if (status.type != GlueDialogType::None) {
        if (input.field() == kDialogField) {
            input.end();
        }
        GlueDialogView v;
        v.text = status.text;
        v.button1 = !status.button.empty()
                        ? status.button
                        : text::get(status.type == GlueDialogType::Okay ? "OKAY" : "CANCEL");
        GlueDialogResult r = glueDialog(c, v);
        dialogKey(r, !v.button2.empty(), true); // phase 18 (extras)
        if (r.button1) { // GlueDialog_OnClick: StatusDialogClick (WoW.exe 0x46B720), gsTitleOptionOK
            play(UiSound::TitleOptionOk);
            if (status.type == GlueDialogType::Okay) {
                session.clearStatus();
                m_failure.clear();
            } else {
                switch (status.action) {
                case GlueStatus::Action::Logout: session.logout(); break;
                case GlueStatus::Action::ChangeRealm: session.requestRealmList(); break;
                case GlueStatus::Action::CancelRealmList: session.cancelRealmList(); break;
                case GlueStatus::Action::Close: m_deleting = m_renaming = false; break;
                }
            }
        }
        return true;
    }
    if (m_dialog == Dialog::None) {
        return false;
    }
    // Our dialogs' edit box: the keyboard types into it.
    const bool editing = input.active() && input.field() == kDialogField;
    if (editing) {
        m_dialogText.set(input.text());
    }
    const bool submitted = editing && input.submitted();
    const bool cancelled = editing && input.cancelled();
    const auto close = [&] {
        m_dialog = Dialog::None;
        m_dialogText.wipe();
        if (input.field() == kDialogField) {
            input.end();
        }
    };
    const auto character = [&]() -> const net::CharacterInfo* {
        for (const net::CharacterInfo& candidate : characters(session)) {
            if (candidate.guid == m_actionGuid) {
                return &candidate;
            }
        }
        return nullptr;
    };
    switch (m_dialog) {
    case Dialog::ServerAddress: {
        // Ours: realmlist.wtf's address in a GlueDialog with an edit box.
        GlueDialogView v;
        v.text = !m_dialogMessage.empty() ? m_dialogMessage : text::get("VD_GLUE_SERVER_ADDRESS");
        v.button1 = text::get("OKAY");
        v.button2 = text::get("CANCEL");
        v.editBox = true;
        v.editText = m_dialogText.view();
        v.editing = editing;
        v.editWidth = 260.0f;
        v.name = "glue.server";
        GlueDialogResult r = glueDialog(c, v);
        dialogKey(r, !v.button2.empty(), true); // phase 18 (extras)
        if (r.edit && !editing) {
            input.begin(kDialogField, m_dialogText.view(), 95, ui::TextKind::Plain);
        }
        if (r.button1 || submitted) {
            play(UiSound::TitleOptionOk);
            // host or host:port (as GameSession::loginWith reads it); empty forgets the player's own address.
            const GlueServerEntry entry =
                glueServerEntry(m_dialogText.view(), glueRealmList(m_launchServer, {}, m_configuredServer),
                                m_loginAfterServer);
            if (!entry.close) {
                m_dialogMessage = text::get(entry.error);
            } else {
                m_server.set(entry.server);
                m_config.realmList.assign(entry.realmList);
                saveConfig();
                m_loginPending = m_loginAfterServer;
                m_loginAfterServer = false;
                m_dialogMessage = {};
                close();
            }
        } else if (r.button2 || cancelled) {
            play(UiSound::TitleOptionOk);
            m_loginAfterServer = false;
            m_dialogMessage = {};
            close();
        }
        break;
    }
    case Dialog::RealmIsFull: {
        GlueDialogView v;
        v.text = text::get("REALM_IS_FULL_WARNING");
        v.button1 = text::get("YES");
        v.button2 = text::get("NO");
        v.alert = true;
        v.name = "glue.realmfull";
        GlueDialogResult r = glueDialog(c, v);
        dialogKey(r, !v.button2.empty(), true); // phase 18 (extras)
        if (r.button1 || r.button2) {
            play(UiSound::TitleOptionOk);
            if (r.button1) { // OnAccept: ChangeRealm; OnCancel: the realm list again
                if (demo()) {
                    VD_LOG_INFO("Glue demo: ChangeRealm(%s)", m_realmFull.c_str());
                } else {
                    session.chooseRealm(m_realmFull);
                }
            }
            close();
        }
        break;
    }
    case Dialog::Delete: {
        // CharacterDeleteDialog: "Do you want to delete <name> Level <n> <class>?"; DELETE typed enables
        // OKAY.
        const net::CharacterInfo* ch = character();
        if (!ch) {
            close();
            break;
        }
        GlueEditDialogView v;
        v.text1 = text::format(m_dialogScratch, "CONFIRM_CHAR_DELETE", ch->name, static_cast<u32>(ch->level),
                               game::className(ch->cls));
        v.text2 = text::get("CONFIRM_CHAR_DELETE_INSTRUCTIONS");
        v.editText = m_dialogText.view();
        v.editing = editing;
        v.okEnabled = deleteConfirmed(m_dialogText.view());
        v.name = "glue.delete";
        GlueDialogResult r = glueEditDialog(c, v);
        dialogKey(r, true, v.okEnabled); // phase 18 (extras)
        if (r.edit && !editing) {
            input.begin(kDialogField, m_dialogText.view(), 32, ui::TextKind::Plain); // letters="32"
        }
        if ((r.button1 || submitted) && v.okEnabled) { // DeleteCharacter
            play(UiSound::TitleOptionOk);
            if (demo()) {
                VD_LOG_INFO("Glue demo: DeleteCharacter(%s)", ch->name.c_str());
            } else {
                m_deleteVersion = session.charResultVersion();
                m_deleting = session.deleteCharacter(m_actionGuid);
            }
            close();
        } else if (r.button2 || cancelled) {
            play(UiSound::GlueOptionExit);
            close();
        }
        break;
    }
    case Dialog::Rename: {
        // CharacterRenameDialog: CHAR_RENAME_DESCRIPTION (or why the last name failed),
        // CHAR_RENAME_INSTRUCTIONS.
        const net::CharacterInfo* ch = character();
        if (!ch) {
            close();
            break;
        }
        GlueEditDialogView v;
        v.text1 = !m_dialogMessage.empty() ? m_dialogMessage : text::get("CHAR_RENAME_DESCRIPTION");
        v.text2 = text::get("CHAR_RENAME_INSTRUCTIONS");
        v.editText = m_dialogText.view();
        v.editing = editing;
        v.fixedHeight = true;
        v.name = "glue.rename";
        GlueDialogResult r = glueEditDialog(c, v);
        dialogKey(r, true, v.okEnabled); // phase 18 (extras)
        if (r.edit && !editing) {
            input.begin(kDialogField, m_dialogText.view(), 24, ui::TextKind::Plain); // letters="12"
        }
        if (r.button1 || submitted) {
            // RenameCharacter (WoW.exe 0x473520): the same name is CHAR_CREATE_NAME_IN_USE; a name the rules
            // refuse says why (0x46B340); else CMSG_CHAR_RENAME under CHAR_RENAME_IN_PROGRESS.
            std::string normalized;
            const u8 check = game::checkCharacterName(m_dialogText.view(), normalized);
            if (check != static_cast<u8>(RC::CHAR_NAME_SUCCESS)) {
                m_dialogMessage = game::charResultText(check);
            } else if (sameName(normalized, ch->name)) {
                m_dialogMessage = text::get("CHAR_CREATE_NAME_IN_USE");
            } else if (demo()) {
                VD_LOG_INFO("Glue demo: RenameCharacter(%s, %s)", ch->name.c_str(), normalized.c_str());
                m_dialogMessage = {};
                close();
            } else {
                m_renameVersion = session.renameVersion();
                m_renaming = session.renameCharacter(m_actionGuid, normalized);
                m_dialogMessage = {};
                close();
            }
        } else if (r.button2 || cancelled) {
            m_dialogMessage = {};
            close();
        }
        break;
    }
    case Dialog::None: break;
    }
    return true;
}

} // namespace vd
