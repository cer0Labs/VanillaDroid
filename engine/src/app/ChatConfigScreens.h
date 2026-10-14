#pragma once

#include "app/Dialogs.h"
#include "app/SocialWindows.h"
#include "app/UiSound.h"
#include "core/Types.h"
#include "game/ChatWindows.h"

#include <array>
#include <string>
#include <vector>

namespace vd {

class GameSession;
class PopupScreens;

// Phase 18 (p3): a chat tab's menu, FCFOptionsDropDown (FloatingChatFrame.lua) over game::ChatWindows: Lock / Unlock
// Window, Rename Chat Window and Create New Window (NAME_CHAT), Close Chat Window (not ChatFrame1); Display: Font Size
// (CHAT_FONT_HEIGHTS) and Background; Filters: Channels (SAY, YELL, GUILD, WHISPER, PARTY and the channels joined),
// Combat, Spell, Spell (Other), Periodic and Other Messages, each group checked when the window listens to it.
// Touch: a long press on the tab opens it (the right click); each arrow's submenu replaces the list, its title goes
// back. The windows stay docked as tabs (no floating frames on a phone: Lock / Unlock only keeps the flag); the
// background's ColorPickerFrame is its opacity steps (OpacitySliderFrame) over the window's colour. The System
// Messages menu (colour swatches only, nothing to check) and Join New Channel (/join does it) are not offered.
// SocialScreens owns it. Main thread; the lists reuse their storage.
class ChatConfigScreens {
public:
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }
    void setPopups(PopupScreens* popups) { m_popups = popups; }
    void open(u32 window);
    void openFilter(u32 window, game::ChatMenu menu); // the gallery: a filter's submenu at once
    // The open menu (modal); false once it closed. Changes go into `windows` (marked changed).
    bool draw(GameSession* session, DialogContext& c, game::ChatWindows& windows);
    u32 window() const { return m_window; }

private:
    enum class Level : u8 { Main, FontSize, Background, Filter };
    enum class Action : u8 { None, Lock, Rename, Create, Close, FontSize, Background, Filter, Back, Set, Toggle,
                             Channel };
    struct Entry {
        Action action = Action::None;
        u32 value = 0;
    };
    void add(std::string_view text, Action action, u32 value = 0, bool checked = false, bool enabled = true,
             u32 color = 0xFFFFFFFF);
    std::string_view label(u32 slot, std::string_view text, bool arrow);

    UiSoundSink m_uiSound;
    PopupScreens* m_popups = nullptr;
    u32 m_window = 0;
    Level m_level = Level::Main;
    game::ChatMenu m_menu = game::ChatMenu::Channels;
    std::vector<ChoiceView> m_choices;
    std::vector<Entry> m_entries;
    std::vector<std::string> m_labels;   // texts built here (arrows, "12 pt", channels)
    std::vector<std::string> m_channels; // the channels joined, without their zones
};

} // namespace vd
