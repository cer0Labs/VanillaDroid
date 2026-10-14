#include "app/MacroScreens.h"

#include "app/GameSession.h"
#include "app/SocialScreens.h"
#include "app/UiResources.h"
#include "app/WtfFolders.h" // phase 18 (extras)
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "net/GameMessages.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace vd {
namespace {

constexpr ui::WidgetId kBodyField = ui::widgetId("macro.body.input");
constexpr ui::WidgetId kNameField = ui::widgetId("macropopup.name.input");

// A folder name of WTF (phase 18, extras: the rule moved to app/WtfFolders, which the renames share).
std::string folderName(std::string_view name, bool upper) {
    return wtfFolderName(name, upper);
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace

void MacroScreens::init(const game::GameData* data, UiResources* resources) {
    m_data = data;
    m_resources = resources;
    m_views.reserve(game::kMacrosPerTab);
    if (resources) {
        m_icons = resources->assets().macroIcons();
        if (m_icons.icons.empty()) {
            VD_LOG_WARN("Macros: ui.vdpk has no macro icons (bake it again: vd_baker bake --out packs --only ui)");
        }
    }
}

void MacroScreens::setStorage(std::filesystem::path folder, FileWriter writer, void* context) {
    m_folder = std::move(folder);
    m_writer = writer;
    m_writerContext = context;
}

std::filesystem::path MacroScreens::accountFile() const {
    return m_folder / "WTF" / "Account" / m_account / "macros-cache.txt";
}

std::filesystem::path MacroScreens::characterFile() const {
    // Phase 18 (p3): the character's macros-cache.txt, the file WoW.exe writes them to (0x4F0AA0: every macro of the
    // UI goes to the second file of its table entry, 0x85DDD4) and keeps in step with the server (account data type 5).
    return m_folder / "WTF" / "Account" / m_account / m_realm / m_character / "macros-cache.txt";
}

std::filesystem::path MacroScreens::legacyCharacterFile() const {
    return m_folder / "WTF" / "Account" / m_account / m_realm / m_character / "macros-local.txt";
}

std::filesystem::path MacroScreens::wtfFolder(const std::filesystem::path& folder, const GameSession& session,
                                              bool character) {
    std::filesystem::path path = folder / "WTF" / "Account" / folderName(session.loginParams().account, true);
    if (character) {
        path /= folderName(session.realmName(), false);
        path /= folderName(session.characterName(), false);
    }
    return path;
}

void MacroScreens::enterWorld(GameSession& session) {
    leaveWorld();
    m_store.clear();
    m_account = folderName(session.loginParams().account, true);
    m_realm = folderName(session.realmName(), false);
    m_character = folderName(session.characterName(), false);
    m_haveCharacter = !m_folder.empty() && !m_account.empty() && !m_character.empty();
    if (m_haveCharacter) {
        // Read once at login (the original reads both when the character enters the world).
        const u32 account = m_store.parse(game::MacroTab::Account, readText(accountFile()));
        std::string characterText = readText(characterFile());
        if (characterText.empty()) {
            characterText = readText(legacyCharacterFile()); // phase 18 (p3): written by phase 15's client
        }
        const u32 character = m_store.parse(game::MacroTab::Character, characterText);
        VD_LOG_INFO("Macros: %u of the account, %u of %s", account, character, m_character.c_str());
    }
    setCharacterName(session.characterName());
    m_tab = 0;
    selectFirst();
}

void MacroScreens::setCharacterName(std::string_view name) {
    char tab[96];
    m_characterTab.assign(text::format(tab, "CHARACTER_SPECIFIC_MACROS", name));
}

void MacroScreens::showPopup(bool newMacro) {
    m_popup = newMacro ? Popup::New : Popup::Edit;
    m_popupName = newMacro ? std::string{} : m_store.slot(static_cast<u32>(std::max(selectedSlot(), 0))) != nullptr
                                                 ? m_store.slot(static_cast<u32>(std::max(selectedSlot(), 0)))->name
                                                 : std::string{};
    m_popupIcon = -1;
}

void MacroScreens::leaveWorld() {
    saveText();
    m_open = false;
    m_popup = Popup::None;
    m_pickup = {};
}

void MacroScreens::save(game::MacroTab tab) {
    if (!m_haveCharacter || !m_writer) {
        return;
    }
    m_writer(m_writerContext, tab == game::MacroTab::Account ? accountFile() : characterFile(), m_store.format(tab));
    if (m_saveHook) {
        m_saveHook(m_saveHookContext, tab); // phase 18 (p3): the account data's timer
    }
}

void MacroScreens::open() {
    if (m_open) {
        return;
    }
    // MacroFrame_OnLoad / OnShow: the General Macros tab and its first macro.
    m_open = true;
    m_tab = 0;
    selectFirst();
}

void MacroScreens::close() {
    // MacroFrame_OnHide: the popup goes, the text is saved.
    m_popup = Popup::None;
    saveText();
    m_open = false;
}

void MacroScreens::selectFirst() {
    select(m_store.count(static_cast<game::MacroTab>(m_tab)) > 0 ? static_cast<i32>(tabBase()) : -1);
}

void MacroScreens::select(i32 slot) {
    if (slot < 0 || !m_store.slot(static_cast<u32>(slot))) {
        m_selected = -1;
        m_text.clear();
    } else {
        m_tab = static_cast<u32>(slot) / game::kMacrosPerTab;
        m_selected = slot - static_cast<i32>(tabBase());
        m_text = m_store.slot(static_cast<u32>(slot))->body;
    }
    m_textChanged = false;
}

void MacroScreens::saveText() {
    // MacroFrame_SaveMacro: EditMacro(selected, nil, nil, text).
    const i32 slot = selectedSlot();
    if (!m_textChanged || slot < 0) {
        return;
    }
    m_textChanged = false;
    const i32 moved = m_store.edit(static_cast<u32>(slot), {}, {}, true, m_text);
    if (moved >= 0) {
        save(game::MacroStore::tabOf(static_cast<u32>(moved)));
    }
}

void MacroScreens::remove(GameSession* session, u32 slot) {
    const game::Macro* macro = m_store.slot(slot);
    if (!macro) {
        return;
    }
    const u32 id = macro->id;
    const game::MacroTab tab = game::MacroStore::tabOf(slot);
    m_store.remove(slot);
    save(tab);
    if (session && session->inWorld()) {
        game::Gameplay& play = session->play();
        for (u32 button = 0; button < net::kActionButtonCount; ++button) {
            if (game::macroIdOf(play.spells().button(button)) == id) {
                play.setActionButton(button, {}); // 0x4E5D10: the buttons holding it are emptied
            }
        }
    }
}

game::ActionButton MacroScreens::takePickup() {
    const game::ActionButton b = m_pickup;
    m_pickup = {};
    return b;
}

u32 MacroScreens::macroSpell(GameSession& session, u32 id) {
    if (!m_data || id == 0) {
        return 0;
    }
    const game::SpellBook& spells = session.play().spells();
    if (m_spellsMacros != m_store.version() || m_spellsKnown != spells.knownVersion()) {
        // WoW.exe 0x4EFE00 / 0x4F0CE0: looked up again when a macro changes or the spell book does.
        m_spellsMacros = m_store.version();
        m_spellsKnown = spells.knownVersion();
        for (u32 slot = 0; slot < game::kMacroSlots; ++slot) {
            const game::Macro* m = m_store.slot(slot);
            m_spells[slot] = m ? SpellOf{m->id, game::knownSpellByName(spells.known(), *m_data,
                                                                       game::macroCastText(m->body))}
                               : SpellOf{};
        }
    }
    for (const SpellOf& s : m_spells) {
        if (s.id == id) {
            return s.spell;
        }
    }
    return 0;
}

void MacroScreens::run(GameSession& session, u32 id) {
    saveText();
    const game::Macro* macro = m_store.find(id);
    if (!macro || !m_chat) {
        return;
    }
    // A copy: a line may change the macros (a /macro line opens the window, nothing edits them), and the body's
    // lines are only views into it.
    const std::string body = macro->body;
    game::forEachMacroLine(body, [&](std::string_view line) { m_chat->runLine(session, line); });
}

void MacroScreens::stopEditing(ui::TextInput& input) {
    if (input.active() && (input.field() == kBodyField || input.field() == kNameField)) {
        input.end();
    }
    m_editing = false;
    m_naming = false;
}

bool MacroScreens::addLine(std::string_view line) {
    if (!m_open || !m_editing || m_selected < 0 || line.empty()) {
        return false;
    }
    m_addedLine.assign(line);
    return true;
}

bool MacroScreens::draw(GameSession* session, DialogContext& c, ui::TextInput& input) {
    VD_PROFILE_ZONE("MacroScreens::draw");
    if (!m_open) {
        if (m_wasOpen) {
            stopEditing(input);
            m_uiSound(UiSound::CharacterClose); // igCharacterInfoClose
            m_wasOpen = false;
        }
        return false;
    }
    if (!m_wasOpen) {
        m_uiSound(UiSound::CharacterOpen); // MacroFrame_OnShow: igCharacterInfoOpen
        m_wasOpen = true;
    }
    // The edit box's text follows the keyboard while it has it.
    m_editing = input.active() && input.field() == kBodyField;
    if (!m_addedLine.empty()) {
        if (m_editing) {
            std::string text(input.text());
            if (!text.empty() && text.back() != '\n') {
                text.push_back('\n');
            }
            text.append(m_addedLine);
            if (text.size() <= game::kMacroBodyBytes) {
                input.setText(text);
            }
        }
        m_addedLine.clear();
    }
    if (m_editing && input.text() != m_text) {
        m_text.assign(input.text());
        m_textChanged = true;
        if (m_popup == Popup::New) {
            m_popup = Popup::None; // OnTextChanged: typing hides a new macro's popup
        }
    }
    if (m_characterTab.empty() && session) {
        setCharacterName(session->characterName());
    }

    MacroFrameView view;
    view.tab = m_tab;
    view.characterTab = m_characterTab;
    m_views.clear();
    const game::MacroTab tab = static_cast<game::MacroTab>(m_tab);
    for (u32 i = 0; i < m_store.count(tab); ++i) {
        const game::Macro* m = m_store.slot(tabBase() + i);
        m_views.push_back({m->iconHash, m->name});
    }
    view.macros = m_views;
    view.selected = m_selected;
    view.body = m_text;
    view.editing = m_editing;
    view.popup = m_popup != Popup::None;
    const MacroFrameResult r = macroFrame(c, view);

    if (r.tab >= 0 && static_cast<u32>(r.tab) != m_tab) {
        // PanelTemplates_SetTab, MacroFrame_SaveMacro, MacroFrame_SetAccountMacros / SetCharacterMacros.
        stopEditing(input);
        saveText();
        m_tab = static_cast<u32>(r.tab);
        selectFirst();
        m_uiSound(UiSound::CharacterTab);
    }
    if (r.select >= 0) { // MacroButton_OnClick: save, select, the popup goes, the edit box lets go
        stopEditing(input);
        saveText();
        select(static_cast<i32>(tabBase()) + r.select);
        m_popup = Popup::None;
    }
    if (r.pickup >= 0) { // PickupMacro: the action buttons take it
        saveText();
        if (const game::Macro* m = m_store.slot(tabBase() + static_cast<u32>(r.pickup))) {
            m_pickup = game::macroButton(m->id);
        }
    }
    if (r.bodyTapped && m_selected >= 0 && !m_editing) {
        input.begin(kBodyField, m_text, game::kMacroBodyBytes, ui::TextKind::MultiLine);
        m_editing = true;
    }
    if (m_focusBody) {
        m_focusBody = false;
        if (m_selected >= 0) {
            input.begin(kBodyField, m_text, game::kMacroBodyBytes, ui::TextKind::MultiLine);
            m_editing = true;
        }
    }
    if (r.create || r.change) { // MacroNewButton / MacroEditButton: save, then the popup
        stopEditing(input);
        saveText();
        m_popup = r.create ? Popup::New : Popup::Edit;
        if (r.create) {
            m_popupName.clear();
            m_popupIcon = -1; // MacroPopupFrame_OnShow: a new one starts without an icon
        } else if (const game::Macro* m = m_store.slot(static_cast<u32>(selectedSlot()))) {
            m_popupName = m->name;
        }
        input.begin(kNameField, m_popupName, game::kMacroNameBytes, ui::TextKind::Plain); // MacroPopupEditBox:SetFocus
        m_naming = true;
        m_uiSound(UiSound::CharacterOpen);
    }
    if (r.remove && selectedSlot() >= 0) { // DeleteMacro, MacroFrame_OnLoad: back to the first General macro
        stopEditing(input);
        remove(session, static_cast<u32>(selectedSlot()));
        m_tab = 0;
        selectFirst();
    }
    if (m_popup != Popup::None) {
        drawPopup(c, input);
    }
    if (r.closed) {
        stopEditing(input);
        close();
        return false;
    }
    return true;
}

void MacroScreens::drawPopup(DialogContext& c, ui::TextInput& input) {
    m_naming = input.active() && input.field() == kNameField;
    if (m_naming) {
        m_popupName.assign(input.text());
    }
    MacroPopupView view;
    view.name = m_popupName;
    view.nameEditing = m_naming;
    view.icons = m_icons.icons;
    view.selectedIcon = m_popupIcon;
    // MacroPopupOkayButton_Update: a name and an icon; editing, a name is enough (the icon stays).
    const bool named = !m_popupName.empty();
    view.okay = named && (m_popupIcon >= 0 || m_popup == Popup::Edit);
    const MacroPopupResult r = macroPopup(c, view);
    if (r.nameTapped && !m_naming) {
        input.begin(kNameField, m_popupName, game::kMacroNameBytes, ui::TextKind::Plain);
        m_naming = true;
    }
    if (r.icon >= 0) {
        m_popupIcon = r.icon; // MacroPopupButton_OnClick
    }
    // Enter in the name box: the Okay button when it is enabled (OnEnterPressed).
    const bool submitted = m_naming && input.submitted();
    if ((r.okay || (submitted && view.okay)) && view.okay) {
        stopEditing(input);
        const std::string_view icon = m_popupIcon >= 0 ? m_icons.name(static_cast<u32>(m_popupIcon)) : std::string_view{};
        i32 slot = -1;
        if (m_popup == Popup::New) {
            // CreateMacro(name, icon, nil, nil, perCharacter): an empty body, in the tab shown.
            const game::MacroTab tab = static_cast<game::MacroTab>(m_tab);
            slot = m_store.create(tab, m_popupName, icon, {});
            if (slot >= 0) {
                save(tab);
                m_focusBody = true; // MacroPopupFrame_OnHide: MacroFrameText:SetFocus
            }
        } else if (selectedSlot() >= 0) {
            slot = m_store.edit(static_cast<u32>(selectedSlot()), m_popupName, icon, false, {});
            if (slot >= 0) {
                save(game::MacroStore::tabOf(static_cast<u32>(slot)));
            }
        }
        m_popup = Popup::None;
        if (slot >= 0) {
            select(slot);
        }
        return;
    }
    if (r.cancel || (m_naming && input.cancelled())) {
        stopEditing(input);
        m_popup = Popup::None;
    }
}

} // namespace vd
