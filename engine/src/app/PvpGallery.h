#pragma once

#include "app/ChatFrame.h"
#include "app/CombatTextScreens.h"
#include "app/Dialogs.h"
#include "app/PopupStack.h"
#include "app/PopupWindows.h"
#include "core/Types.h"
#include "ui/MessageFrame.h"

#include <array>
#include <string>
#include <vector>

namespace vd {

// Development (phase 17, honor): the battleground graveyards' and honor's windows and lines with sample data,
// worded by the real code (game/Honor, game/SocialText, app/PopupStack), as VD_UI_GALLERY pages:
//  - "spirithealer": AREA_SPIRIT_HEAL counting down from a spirit guide's 30 s wave (1.12.1's: its only
//    button, Cancel; back a moment after it ran out or was cancelled).
//  - "skinned": the insignia taken: DEATH_CORPSE_SKINNED in the UIErrorsFrame (1.12.1 has its SKINNED popups
//    commented out in UIParent.lua).
//  - "pvp": the chat with the defense channels' lines ("[2. LocalDefense] : Goldshire is under attack!", the
//    Eastern Plaguelands' tower, WorldDefense) and its Combat Log tab with the honor lines (honorable,
//    dishonorable, awarded), the floating combat text's "Honor +37" and the "HK: Grunt" that rises over the
//    player.
// The same pieces draw VD_PVP_DEMO's samples over the offline HUD (app/PvpScreens). Taps go to the log.
class PvpGallery {
public:
    enum class Page : u8 { SpiritHealer, Skinned, Pvp };
    // A page by its VD_UI_GALLERY name; false for another name.
    bool select(const char* name);
    void draw(DialogContext& c, u64 nowMs);

    // The pieces (VD_PVP_DEMO).
    void drawSpiritHealer(DialogContext& c, u64 nowMs);
    void drawSkinned(DialogContext& c, const ui::Rect& avoid, u64 nowMs);
    // The floating combat text and "HK: <rank>" over the player.
    void drawHonorTexts(DialogContext& c, u64 nowMs);
    // The sample lines as the client words them: the combat log's (`honor`) or the chat's defense channels'.
    struct Line {
        std::string text;
        u32 color = 0;
    };
    static std::vector<Line> sampleLines(bool honor);

private:
    void drawChat(DialogContext& c);

    Page m_page = Page::Pvp;
    // spirithealer
    PopupStack m_stack;
    std::array<f32, PopupStack::kSlots> m_heights{};
    std::array<PopupView, PopupStack::kSlots> m_views{};
    std::array<char, 128> m_popupText{};
    u64 m_lastMs = 0;
    u64 m_emptySinceMs = 0;
    // skinned
    ui::MessageFrame m_errors;
    bool m_errorsFilled = false;
    // pvp
    bool m_chatBuilt = false;
    std::vector<Line> m_chatTexts, m_logTexts;
    std::vector<ChatFrameLine> m_chatLines, m_logLines;
    std::array<ChatFrameTab, 2> m_tabs{};
    ChatFrameState m_chatState;
    i32 m_selected = 0;
    bool m_expanded = true;
    CombatTextScreens m_combatText;
    SettingsView m_settings;
    u64 m_nextHonorMs = 0;
    u32 m_honorIndex = 0;
};

} // namespace vd
