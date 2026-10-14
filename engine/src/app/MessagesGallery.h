#pragma once

#include "app/ChatFrame.h"
#include "app/Dialogs.h"
#include "app/MessageHud.h"
#include "core/Types.h"
#include "ui/MessageFrame.h"

#include <array>
#include <string>
#include <vector>

namespace vd {

// Development (parity, VD_UI_GALLERY=messages / messagelog): where the client's own lines go, with Northshire
// sample data worded by core/Text as game/Notices words them: the chat's General tab (loot, skill-ups,
// learned spells, a level-up, quests; SYSTEM / LOOT / SKILL colours), its Combat Log tab (experience,
// reputation, money), the UIErrorsFrame (info and error lines) and the three mirror timer bars (breath,
// fatigue, feign death).
class MessagesGallery {
public:
    // `combatLog`: the chat's Combat Log tab first (at rest) instead of the open General tab.
    void draw(DialogContext& c, bool combatLog, u64 nowMs);

private:
    void build();

    bool m_built = false;
    std::vector<std::string> m_chatTexts;
    std::vector<ChatFrameLine> m_chatLines;
    std::vector<std::array<ChatFrameLink, 4>> m_chatLinks;
    std::vector<std::string> m_logTexts;
    std::vector<ChatFrameLine> m_logLines;
    std::array<ChatFrameTab, 2> m_tabs{};
    ui::MessageFrame m_errors;
    std::array<MirrorTimerBarView, 3> m_bars{};
    ChatFrameState m_chatState;
    i32 m_selected = -1; // -1: the page's own tab
    i32 m_expanded = -1; // -1: the page's own state
};

} // namespace vd
