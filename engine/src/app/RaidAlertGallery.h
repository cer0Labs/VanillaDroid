#pragma once

#include "app/ChatFrame.h"
#include "app/Dialogs.h"
#include "app/MinimapView.h"
#include "core/Types.h"
#include "ui/MessageFrame.h"

#include <array>
#include <string>
#include <vector>

namespace vd {

// Development (phase 16, alerts): VD_UI_GALLERY=readycheck / raidwarning / meetingstone with sample data
// worded by core/Text as game/RaidAlertLines and the screens word them:
//  - readycheck: ReadyCheckFrame (the leader Aeryn's stand-in portrait, READY_CHECK_MESSAGE, Yes / No) at its
//  place,
//    the chat with a member's and the leader's lines (ERR_RAID_LEADER_READY_CHECK_START_S, READY_CHECK_START,
//    RAID_MEMBER_NOT_READY, RAID_MEMBERS_AFK of a 40-member raid, READY_CHECK_NO_AFK,
//    READY_CHECK_YOU_WERE_AFK) and the UIErrorsFrame's ERR_READY_CHECK_IN_PROGRESS.
//  - raidwarning: the UIErrorsFrame, RaidWarningFrame and RaidBossEmoteFrame under it (two lines each, never
//  fading
//    here), and the chat's lines of the same messages.
//  - meetingstone: the MinimapCluster with MiniMapMeetingStoneFrame under the letter, its tooltip,
//  CONFIRM_LEAVE_QUEUE,
//    the chat's ERR_MEETING_STONE_* lines and the UIErrorsFrame's red ones.
class RaidAlertGallery {
public:
    enum class Page : u8 { ReadyCheck, RaidWarning, MeetingStone };
    void draw(DialogContext& c, Page page, u64 nowMs);

private:
    void build(Page page);
    void chat(DialogContext& c);

    Page m_built = Page::ReadyCheck;
    bool m_ready = false;
    std::vector<std::string> m_texts;
    std::vector<ChatFrameLine> m_lines;
    std::array<ChatFrameTab, 1> m_tabs{};
    ChatFrameState m_chatState;
    ui::MessageFrame m_errors;
    ui::MessageFrame m_warnings{{4, 10000, 3000, true}};
    ui::MessageFrame m_emotes{{4, 10000, 3000, true}};
    MinimapGesture m_gesture;
    bool m_tooltip = true; // meetingstone: the button's tooltip (a long press on it toggles it again)
    std::array<f32, 4> m_popupHeights{};
    std::string m_readyText;
    // The page's frames' CPU time (logged every 600 frames).
    u32 m_costFrames = 0;
    f32 m_costSum = 0.0f;
    f32 m_costWorst = 0.0f;
};

} // namespace vd
