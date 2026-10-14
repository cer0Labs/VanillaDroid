#include "app/GlueGallery.h"

#include "core/Log.h"
#include "core/Text.h"
#include "game/CharacterNames.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace vd {
namespace {

constexpr ui::WidgetId kAccountField = ui::widgetId("gallery.glue.account");
constexpr ui::WidgetId kPasswordField = ui::widgetId("gallery.glue.password");

net::RealmInfo realm(const char* name, u32 icon, u8 flags, f32 population, u8 characters) {
    net::RealmInfo r;
    r.name = name;
    r.icon = icon;
    r.flags = flags;
    r.population = population;
    r.characters = characters;
    return r;
}

struct SampleCharacter {
    const char* name;
    u8 cls;
    u8 level;
    bool ghost;
    const char* zone; // AreaTable's name (enUS)
};
constexpr SampleCharacter kCharacters[] = {
    {"Arthel", 2, 60, false, "Stormwind City"},    {"Thrallita", 3, 12, false, "Durotar"},
    {"Brunhilda", 1, 5, false, "Dun Morogh"},      {"Hojaverde", 11, 20, true, "Darkshore"},
    {"Sombrio", 9, 33, false, "Tirisfall Glades"}, {"Pezuna", 3, 8, false, "Mulgore"},
    {"Chispa", 8, 14, false, "Dun Morogh"},        {"Zuljin", 4, 27, false, "Durotar"},
};

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: glue %s", what);
}

} // namespace

GlueGallery::GlueGallery() {
    m_rows.reserve(16);
    m_realms = {realm("Dev Realm", 1, 0, 0.3f, 8), realm("Kirin Tor", 6, net::kRealmFlagNewPlayers, 1.0f, 0),
                realm("Archimonde", 1, 0, 1.9f, 2), realm("Frostmane", 8, net::kRealmFlagFull, 2.0f, 0),
                realm("Gurubashi", 1, net::kRealmFlagOffline, 0.0f, 1)};
    m_account.set("admin");
}

bool GlueGallery::select(std::string_view page) {
    static constexpr std::pair<std::string_view, Page> kPages[] = {{"login", Page::Login},
                                                                   {"login2", Page::LoginError},
                                                                   {"login3", Page::LoginBusy},
                                                                   {"login4", Page::LoginNoServer},
                                                                   {"loginkb", Page::LoginKeyboard},
                                                                   {"serverdialog", Page::ServerDialog},
                                                                   {"queue", Page::Queue},
                                                                   {"realms", Page::Realms},
                                                                   {"realmfull", Page::RealmFull},
                                                                   {"charselect", Page::Characters},
                                                                   {"charselect2", Page::NoCharacters},
                                                                   {"chardelete", Page::Delete},
                                                                   {"rename", Page::Rename}};
    for (const auto& [name, kind] : kPages) {
        if (page == name) {
            m_page = kind;
            return true;
        }
    }
    return false;
}

void GlueGallery::draw(DialogContext& c, ui::TextInput& input) {
    const auto start = std::chrono::steady_clock::now();
    if (m_startMs == 0) {
        m_startMs = c.ui.nowMs();
        m_versionText = glueVersionText(m_version);
        realmRows(m_realms, "Dev Realm", m_sort, m_rows);
    }
    switch (m_page) {
    case Page::Realms:
    case Page::RealmFull: realms(c); break;
    case Page::Characters:
    case Page::NoCharacters:
    case Page::Delete:
    case Page::Rename: characters(c); break;
    default: login(c, input); break;
    }
    const f32 ms = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - start).count();
    m_costSum += ms;
    m_costWorst = std::max(m_costWorst, ms);
    if (++m_costFrames == 600) {
        VD_LOG_INFO("UI gallery: glue page %u %.3f ms avg, %.3f ms worst (600 frames)",
                    static_cast<u32>(m_page), m_costSum / 600.0f, m_costWorst);
        m_costFrames = 0;
        m_costSum = m_costWorst = 0.0f;
    }
}

void GlueGallery::login(DialogContext& c, ui::TextInput& input) {
    // The keyboard page: the password field edited, the bottom 45% covered.
    DialogContext page = c;
    i32 editing = input.active() && input.field() == kAccountField    ? 0
                  : input.active() && input.field() == kPasswordField ? 1
                                                                      : -1;
    if (editing == 0) {
        m_account.set(input.text());
    } else if (editing == 1) {
        m_password.set(input.text());
    }
    if (m_page == Page::LoginKeyboard) {
        page.safeArea.h *= 0.55f;
        editing = 1;
        if (m_password.empty()) {
            m_password.set("secret");
        }
    }
    const bool dialog = m_page == Page::LoginError || m_page == Page::LoginBusy ||
                        m_page == Page::ServerDialog || m_page == Page::Queue || m_loginAtMs != 0;
    AccountLoginView v;
    v.account = m_account.view();
    v.passwordLength = static_cast<u32>(m_password.view().size());
    v.editing = editing;
    v.saveAccountName = m_save;
    const bool server = m_page != Page::LoginNoServer;
    v.realmName = server ? std::string_view("Dev Realm") : std::string_view{};
    v.server = server ? std::string_view("192.168.1.20:3724") : std::string_view{};
    v.version = m_versionText;
    v.alpha = std::min(1.0f, static_cast<f32>(c.ui.nowMs() - m_startMs) / 1000.0f / kLoginFadeIn);
    v.interactive = !dialog;
    const AccountLoginResult r = accountLogin(page, v);
    if (r.edit == 0) {
        input.begin(kAccountField, m_account.view(), 16, ui::TextKind::Plain);
    } else if (r.edit == 1) {
        input.begin(kPasswordField, m_password.view(), 16, ui::TextKind::Secret);
    }
    if (r.toggleSave) {
        m_save = !m_save;
        logResult(m_save ? "SaveAccountName checked" : "SaveAccountName unchecked");
    }
    if (r.login) {
        logResult("AccountLogin_Login");
        input.end();
        m_password.wipe();
        m_loginAtMs = c.ui.nowMs();
    }
    if (r.quit) {
        logResult("AccountLogin_Exit");
    }
    if (r.server) {
        logResult("the server's address");
    }
    // The dialogs: the steps of a login (two seconds each), a failure, the address, the queue.
    GlueSessionState state;
    using Step = GlueSessionState::Step;
    if (m_loginAtMs != 0) {
        static constexpr Step kSteps[] = {Step::Connecting, Step::Authenticating, Step::Authenticated,
                                          Step::WorldConnecting, Step::CharacterList};
        const u64 step = (c.ui.nowMs() - m_loginAtMs) / 1500;
        if (step >= std::size(kSteps)) {
            m_loginAtMs = 0;
        } else {
            state.step = kSteps[step];
            state.characterListPending = true;
        }
    } else if (m_page == Page::LoginError) {
        state.failure = text::get("LOGIN_UNKNOWN_ACCOUNT");
    } else if (m_page == Page::LoginBusy) {
        state.step = Step::Authenticating;
    } else if (m_page == Page::Queue) {
        state.step = Step::Queued;
        state.queuePosition = 12;
        state.realm = "Dev Realm";
    }
    const GlueStatus status = glueStatus(state, m_scratch);
    if (status.type != GlueDialogType::None) {
        GlueDialogView d;
        d.text = status.text;
        d.button1 = !status.button.empty()
                        ? status.button
                        : text::get(status.type == GlueDialogType::Okay ? "OKAY" : "CANCEL");
        if (glueDialog(c, d).button1) {
            logResult("StatusDialogClick");
            m_loginAtMs = 0;
        }
    } else if (m_page == Page::ServerDialog) {
        GlueDialogView d;
        d.text = text::get("VD_GLUE_SERVER_ADDRESS");
        d.button1 = text::get("OKAY");
        d.button2 = text::get("CANCEL");
        d.editBox = true;
        d.editText = "192.168.1.20:3724";
        d.editWidth = 260.0f;
        const GlueDialogResult r2 = glueDialog(c, d);
        if (r2.button1 || r2.button2 || r2.edit) {
            logResult(r2.edit      ? "server address edit box"
                      : r2.button1 ? "server address OKAY"
                                   : "server address CANCEL");
        }
    }
}

void GlueGallery::realms(DialogContext& c) {
    AccountLoginView v;
    v.account = m_account.view();
    v.version = m_versionText;
    v.realmName = "Dev Realm";
    v.server = "192.168.1.20:3724";
    v.interactive = false;
    accountLogin(c, v);
    RealmListView list;
    list.rows = m_rows;
    list.selected = m_page == Page::RealmFull ? 3 : m_realmSelected;
    for (usize i = 0; i < m_rows.size(); ++i) {
        if (m_page == Page::RealmFull && m_rows[i].name == std::string_view("Frostmane")) {
            list.selected = static_cast<i32>(i);
        }
    }
    list.okEnabled = list.selected >= 0 && !m_rows[static_cast<usize>(list.selected)].down;
    list.interactive = m_page == Page::Realms;
    const RealmListResult r = realmList(c, list);
    if (r.select >= 0) {
        m_realmSelected = r.select;
        logResult("RealmSelectButton_OnClick");
    }
    if (r.sort >= 0) {
        m_sort.by(static_cast<RealmSortKey>(r.sort));
        realmRows(m_realms, "Dev Realm", m_sort, m_rows);
        logResult("SortRealms");
    }
    if (r.ok || r.enter >= 0 || r.cancel) {
        logResult(r.cancel ? "RealmList_OnCancel" : "RealmList_OnOk");
    }
    if (m_page == Page::RealmFull) {
        GlueDialogView d;
        d.text = text::get("REALM_IS_FULL_WARNING");
        d.button1 = text::get("YES");
        d.button2 = text::get("NO");
        d.alert = true;
        if (const GlueDialogResult r2 = glueDialog(c, d); r2.button1 || r2.button2) {
            logResult(r2.button1 ? "REALM_IS_FULL Yes" : "REALM_IS_FULL No");
        }
    }
}

void GlueGallery::characters(DialogContext& c) {
    const u32 count = m_page == Page::NoCharacters ? 0u : static_cast<u32>(std::size(kCharacters));
    for (u32 i = 0; i < count; ++i) {
        const SampleCharacter& s = kCharacters[i];
        m_characters[i].name = s.name;
        m_characters[i].info =
            text::format(m_info[i], s.ghost ? "CHARACTER_SELECT_INFO_GHOST" : "CHARACTER_SELECT_INFO",
                         static_cast<u32>(s.level), game::className(s.cls));
        m_characters[i].location = s.zone;
    }
    const bool dialog = m_page == Page::Delete || m_page == Page::Rename;
    CharacterSelectView v;
    v.characters = {m_characters.data(), count};
    v.selected = count > 0 ? m_selected : -1;
    v.realm = "Dev Realm (PVP)";
    v.selectedName = count > 0 ? m_characters[static_cast<usize>(m_selected)].name : std::string_view{};
    v.canCreate = count < kMaxCharactersDisplayed;
    v.alpha = std::min(1.0f, static_cast<f32>(c.ui.nowMs() - m_startMs) / 1000.0f / kCharacterSelectFadeIn);
    v.interactive = !dialog;
    const CharacterSelectResult r = characterSelect(c, v);
    if (r.select >= 0) {
        m_selected = r.select;
        logResult("CharacterSelectButton_OnClick");
    }
    m_facing += r.turnDegrees;
    if (r.turnDegrees != 0.0f) {
        char facing[64];
        std::snprintf(facing, sizeof(facing), "facing %.0f degrees", static_cast<f64>(m_facing));
        logResult(facing);
    }
    if (r.enter >= 0 || r.enterWorld) {
        logResult("CharacterSelect_EnterWorld");
    }
    if (r.create || r.remove || r.changeRealm || r.back) {
        logResult(r.create        ? "CharacterSelect_SelectCharacter(create)"
                  : r.remove      ? "CharacterSelect_Delete"
                  : r.changeRealm ? "CharacterSelect_ChangeRealm"
                                  : "CharacterSelect_Exit");
    }
    if (m_page == Page::Delete) {
        GlueEditDialogView d;
        d.text1 = text::format(m_scratch, "CONFIRM_CHAR_DELETE", kCharacters[m_selected].name,
                               static_cast<u32>(kCharacters[m_selected].level),
                               game::className(kCharacters[m_selected].cls));
        d.text2 = text::get("CONFIRM_CHAR_DELETE_INSTRUCTIONS");
        d.editText = "DEL";
        d.okEnabled = deleteConfirmed(d.editText);
        d.name = "gallery.glue.delete";
        if (const GlueDialogResult r2 = glueEditDialog(c, d); r2.button1 || r2.button2) {
            logResult(r2.button1 ? "DeleteCharacter" : "CharacterDeleteDialog Cancel");
        }
    } else if (m_page == Page::Rename) {
        GlueEditDialogView d;
        d.text1 = text::get("CHAR_NAME_TOO_SHORT");
        d.text2 = text::get("CHAR_RENAME_INSTRUCTIONS");
        d.editText = "C";
        d.fixedHeight = true;
        d.name = "gallery.glue.rename";
        if (const GlueDialogResult r2 = glueEditDialog(c, d); r2.button1 || r2.button2) {
            logResult(r2.button1 ? "RenameCharacter" : "CharacterRenameDialog Cancel");
        }
    }
}

} // namespace vd
