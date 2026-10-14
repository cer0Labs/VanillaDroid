#pragma once

#include "app/Dialogs.h"
#include "app/MacroWindows.h"
#include "app/UiSound.h"
#include "core/Types.h"
#include "game/GameData.h"
#include "game/Macros.h"
#include "ui/TextInput.h"
#include "ui/UiAssets.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vd {

class GameSession;
class SocialScreens;
class UiResources;

// Phase 15 (options): the game's side of the macros (Blizzard_MacroUI and the C functions behind it, game/Macros):
// the macro window and its popup (app/MacroWindows) filled from the macros and applied back, the files (1.12's
// WTF\Account\<ACCOUNT>\macros-cache.txt and ...\<realm>\<character>\macros-local.txt under the app's private folder:
// read when a character enters the world, written in the background on every change), the action buttons that hold a
// macro (their icon, name and spell for the HUD: app/CombatScreens) and pressing one (each line through the chat:
// SocialScreens::runLine). GameScreens owns it. Main thread; changes allocate, frames do not once warmed up.
class MacroScreens {
public:
    void init(const game::GameData* data, UiResources* resources);
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }
    void setChat(SocialScreens* chat) { m_chat = chat; }
    // Where the files go (desktop: packs/.cache; Android: the app's files folder; empty: not saved) and how they are
    // written (the game's background writer: DeviceMonitor::writeFile).
    using FileWriter = void (*)(void* context, const std::filesystem::path& path, std::string text);
    void setStorage(std::filesystem::path folder, FileWriter writer, void* context);
    // Phase 18 (p3): the same storage for the account data's other files (app/AccountDataFiles): its folder (empty: none),
    // a background write, and the WTF folders of the session's account (and of its character).
    const std::filesystem::path& storageFolder() const { return m_folder; }
    void writeFile(const std::filesystem::path& path, std::string text) const {
        if (m_writer) {
            m_writer(m_writerContext, path, std::move(text));
        }
    }
    static std::filesystem::path wtfFolder(const std::filesystem::path& folder, const GameSession& session,
                                           bool character);
    // Phase 18 (p3): after each save of a tab's file (the account data's 30 s timer, game::AccountData::changed).
    using SaveHook = void (*)(void* context, game::MacroTab tab);
    void setSaveHook(SaveHook hook, void* context) {
        m_saveHook = hook;
        m_saveHookContext = context;
    }
    bool loaded() const { return m_haveCharacter; } // the files of the character in the world were read

    // A character entered the world (read its files) / left it (the window's text is saved first).
    void enterWorld(GameSession& session);
    void leaveWorld();

    // --- The window (UIPanelWindows "left") ---
    bool isOpen() const { return m_open; }
    void open();  // ShowMacroFrame: the General Macros tab, its first macro
    void close(); // HideUIPanel: the popup goes, the text is saved
    // Draws the frame in the place of `c` and, over it, the popup; returns false when it closed itself.
    bool draw(GameSession* session, DialogContext& c, ui::TextInput& input);
    // A macro the player picked up for an action button this frame (empty: none). Read once.
    game::ActionButton takePickup();

    // --- Action buttons holding a macro ---
    const game::Macro* macro(u32 id) const { return m_store.find(id); }
    // The spell of the macro's first /cast (0: none, or not known): the button shows its state (WoW.exe 0x4E5A50).
    u32 macroSpell(GameSession& session, u32 id);
    // UseAction on a macro button: the edit box's text saved first (ActionButtonUp: MacroFrame_SaveMacro), then each
    // line run as typed (RunMacro: EXECUTE_CHAT_LINE).
    void run(GameSession& session, u32 id);
    // MacroFrame_SaveMacro: the edited commands into the selected macro (and its file).
    void saveText();
    // MacroFrame_AddMacroLine (the spellbook's Shift-click while the window shows): `line` appended to the commands
    // being edited. Touch: the open keyboard is the modifier (as for chat links) and the line starts on a line of its
    // own. False when the commands are not being edited.
    bool addLine(std::string_view line);

    // Development and the gallery: the macros, as they are (no files); the character's tab's name; the popup up.
    game::MacroStore& store() { return m_store; }
    void setCharacterName(std::string_view name);
    void showPopup(bool newMacro);
    // The icon list (GetMacroIconInfo), from ui.vdpk.
    const ui::Assets::MacroIcons& icons() const { return m_icons; }

private:
    enum class Popup : u8 { None, New, Edit };

    u32 tabBase() const { return m_tab * game::kMacrosPerTab; }
    i32 selectedSlot() const { return m_selected >= 0 ? static_cast<i32>(tabBase()) + m_selected : -1; }
    void selectFirst();
    void select(i32 slot); // a slot of the store (-1 none): its tab and its commands
    void save(game::MacroTab tab);
    // DeleteMacro: the macro goes, and every action button holding it (0x4E5D10: CMSG_SET_ACTION_BUTTON 0).
    void remove(GameSession* session, u32 slot);
    void drawPopup(DialogContext& c, ui::TextInput& input);
    void stopEditing(ui::TextInput& input);
    std::filesystem::path accountFile() const;
    std::filesystem::path characterFile() const;
    std::filesystem::path legacyCharacterFile() const; // phase 18 (p3): phase 15's macros-local.txt

    const game::GameData* m_data = nullptr;
    UiResources* m_resources = nullptr;
    SocialScreens* m_chat = nullptr;
    UiSoundSink m_uiSound;
    ui::Assets::MacroIcons m_icons;
    game::MacroStore m_store;

    // Files.
    std::filesystem::path m_folder;
    FileWriter m_writer = nullptr;
    void* m_writerContext = nullptr;
    SaveHook m_saveHook = nullptr; // phase 18 (p3)
    void* m_saveHookContext = nullptr;
    std::string m_account, m_realm, m_character; // the session's, as the folders name them
    bool m_haveCharacter = false;

    // The window.
    bool m_open = false;
    bool m_wasOpen = false;
    u32 m_tab = 0;
    i32 m_selected = -1; // in the tab
    std::string m_text;  // the edit box's (MacroFrameText)
    bool m_textChanged = false;
    bool m_editing = false;
    Popup m_popup = Popup::None;
    std::string m_popupName;
    i32 m_popupIcon = -1;
    bool m_naming = false;
    bool m_focusBody = false; // a new macro: the edit box takes the keyboard (MacroPopupFrame_OnHide)
    std::vector<MacroSlotView> m_views;
    std::string m_characterTab;
    game::ActionButton m_pickup;
    std::string m_addedLine; // addLine(), into the edit box at the next draw

    // The macros' spells, looked up when the macros or the known spells change.
    struct SpellOf {
        u32 id = 0;
        u32 spell = 0;
    };
    std::array<SpellOf, game::kMacroSlots> m_spells{};
    u32 m_spellsMacros = 0, m_spellsKnown = 0;
};

} // namespace vd
