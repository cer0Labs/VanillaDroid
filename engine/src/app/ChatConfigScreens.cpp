#include "app/ChatConfigScreens.h"

#include "app/GameSession.h"
#include "app/PopupScreens.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/SocialText.h"
#include "ui/DrawList.h"

#include <cmath>

namespace vd {
namespace {

constexpr u32 kFontHeights[] = {12, 14, 16, 18}; // CHAT_FONT_HEIGHTS (Fonts.xml)
constexpr u32 kDefaultFontHeight = 14;           // ChatFontNormal
constexpr u32 kOpacitySteps[] = {0, 25, 50, 75, 100};
constexpr u32 kLabels = 64;

// The window's name as its tab shows it (FCF_SetWindowName's defaults).
std::string_view windowName(const game::ChatWindowConfig& w, u32 index, std::span<char> buffer) {
    if (!w.name.empty()) {
        return w.name;
    }
    if (index == 0) {
        return text::get("GENERAL");
    }
    if (index == 1) {
        return text::get("COMBAT_LOG");
    }
    return text::format(buffer, "CHAT_NAME_TEMPLATE", index + 1);
}

std::string_view baseChannelName(std::string_view name) {
    const usize dash = name.find(" - ");
    return dash == std::string_view::npos ? name : name.substr(0, dash);
}

} // namespace

void ChatConfigScreens::open(u32 window) {
    m_window = window;
    m_level = Level::Main;
}

void ChatConfigScreens::openFilter(u32 window, game::ChatMenu menu) {
    m_window = window;
    m_level = Level::Filter;
    m_menu = menu;
}

std::string_view ChatConfigScreens::label(u32 slot, std::string_view text, bool arrow) {
    if (slot >= m_labels.size()) {
        return text; // more than kLabels: plain (never: the longest list is the channels')
    }
    std::string& s = m_labels[slot];
    s.assign(text);
    if (arrow) {
        s.append("  >"); // the entry's arrow (hasArrow): its submenu
    }
    return s;
}

void ChatConfigScreens::add(std::string_view text, Action action, u32 value, bool checked, bool enabled, u32 color) {
    m_choices.push_back({text, color, checked, enabled});
    m_entries.push_back({action, value});
}

bool ChatConfigScreens::draw(GameSession* session, DialogContext& c, game::ChatWindows& windows) {
    VD_PROFILE_ZONE("ChatConfigScreens::draw");
    if (m_labels.empty()) {
        m_labels.resize(kLabels); // their texts' views stay valid while the list is drawn
        m_choices.reserve(kLabels);
        m_entries.reserve(kLabels);
    }
    m_choices.clear();
    m_entries.clear();
    const game::ChatWindowConfig& w = windows.window(m_window);
    char nameBuffer[48];
    const std::string_view title = windowName(w, m_window, nameBuffer);
    u32 slot = 0;
    const u32 gold = ui::kGold;
    switch (m_level) {
    case Level::Main:
        // FCFOptionsDropDown_Initialize, level 1.
        add(text::get(w.locked ? "UNLOCK_WINDOW" : "LOCK_WINDOW"), Action::Lock);
        add(text::get("RENAME_CHAT_WINDOW"), Action::Rename);
        add(text::get("NEW_CHAT_WINDOW"), Action::Create, 0, false, windows.active() < game::kChatWindows);
        if (m_window != 0) { // FCF_GetCurrentChatFrame() ~= DEFAULT_CHAT_FRAME
            add(text::get("CLOSE_CHAT_WINDOW"), Action::Close);
        }
        add(text::get("DISPLAY"), Action::None, 0, false, true, gold); // isTitle
        add(label(slot++, text::get("FONT_SIZE"), true), Action::FontSize);
        add(label(slot++, text::get("BACKGROUND"), true), Action::Background);
        add(text::get("FILTERS"), Action::None, 0, false, true, gold);
        for (u32 m = 0; m < static_cast<u32>(game::ChatMenu::Count); ++m) {
            add(label(slot++, text::get(text::Key(game::chatMenuLabelKey(static_cast<game::ChatMenu>(m)))), true),
                Action::Filter, m);
        }
        break;
    case Level::FontSize: {
        add(label(slot++, text::get("FONT_SIZE"), false), Action::Back, 0, false, true, gold);
        const u32 current = w.fontSize != 0 ? w.fontSize : kDefaultFontHeight;
        for (const u32 h : kFontHeights) {
            char line[32];
            add(label(slot++, text::format(line, "FONT_SIZE_TEMPLATE", h), false), Action::Set, h, h == current);
        }
        break;
    }
    case Level::Background: {
        // The colour picker's opacity (OpacitySliderFrame: 1 - alpha), here its steps.
        add(label(slot++, text::get("BACKGROUND"), false), Action::Back, 0, false, true, gold);
        const u32 percent = static_cast<u32>(std::lround(static_cast<f32>(w.a) * 100.0f / 255.0f));
        for (const u32 step : kOpacitySteps) {
            char line[48];
            add(label(slot++, text::format(line, "VD_P3_CHAT_OPACITY", step), false), Action::Set, step,
                percent >= step && percent < step + 13 && (step != 0 || percent < 13));
        }
        break;
    }
    case Level::Filter: {
        add(label(slot++, text::get(text::Key(game::chatMenuLabelKey(m_menu))), false), Action::Back, 0, false,
            true, gold);
        for (const u8 group : game::chatMenuGroups(m_menu)) {
            add(text::get(text::Key(game::chatGroupLabelKey(group))), Action::Toggle, group, w.hasGroup(group));
        }
        if (m_menu == game::ChatMenu::Channels && session && session->inWorld()) {
            // FCFDropDown_LoadChannels(GetChannelList()): the channels joined, without their zone.
            m_channels.clear();
            for (const game::ChatChannel& channel : session->play().social().channels()) {
                if (channel.joined && !channel.name.empty()) {
                    m_channels.emplace_back(baseChannelName(channel.name));
                }
            }
            for (u32 i = 0; i < m_channels.size(); ++i) {
                // Kept by the server's name (chat-cache.txt), shown in the player's language (phase 21).
                char shown[128];
                add(label(slot++, game::channelDisplayBase(m_channels[i], shown), false), Action::Channel, i,
                    w.hasChannel(m_channels[i]));
            }
        }
        break;
    }
    }

    const i32 picked = choiceList(c, "chat.config", title, m_choices);
    if (picked == -2) {
        return false; // closed, or a tap outside: the menu goes
    }
    if (picked < 0 || static_cast<u32>(picked) >= m_entries.size()) {
        return true;
    }
    const Entry e = m_entries[static_cast<u32>(picked)];
    game::ChatWindowConfig& edit = windows.window(m_window);
    switch (e.action) {
    case Action::None: return true;
    case Action::Lock: // FCF_ToggleLock (the windows stay docked: only the flag)
        edit.locked = !edit.locked;
        windows.markChanged();
        return false;
    case Action::Rename:
        if (m_popups && session) {
            m_popups->askNameChat(*session, m_window + 1);
        }
        return false;
    case Action::Create:
        if (m_popups && session) {
            m_popups->askNameChat(*session, 0);
        }
        return false;
    case Action::Close:
        windows.close(m_window);
        return false;
    case Action::FontSize: m_level = Level::FontSize; return true;
    case Action::Background: m_level = Level::Background; return true;
    case Action::Filter:
        m_level = Level::Filter;
        m_menu = static_cast<game::ChatMenu>(e.value);
        return true;
    case Action::Back: m_level = Level::Main; return true;
    case Action::Set:
        if (m_level == Level::FontSize) { // FCF_SetChatWindowFontSize
            edit.fontSize = e.value == kDefaultFontHeight ? 0 : e.value;
        } else { // FCF_SetChatWindowOpacity
            edit.a = static_cast<u8>(std::lround(static_cast<f32>(e.value) * 255.0f / 100.0f));
        }
        windows.markChanged();
        m_level = Level::Main;
        return false;
    case Action::Toggle: // FCFMessageTypeDropDown_OnClick (keepShownOnClick)
        windows.toggleGroup(m_window, e.value);
        m_uiSound(edit.hasGroup(e.value) ? UiSound::CheckBoxOn : UiSound::CheckBoxOff);
        return true;
    case Action::Channel: // FCFChannelDropDown_OnClick
        if (e.value < m_channels.size()) {
            windows.toggleChannel(m_window, m_channels[e.value]);
            m_uiSound(edit.hasChannel(m_channels[e.value]) ? UiSound::CheckBoxOn : UiSound::CheckBoxOff);
        }
        return true;
    }
    return true;
}

} // namespace vd
