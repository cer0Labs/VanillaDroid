#pragma once

#include "app/Dialogs.h"
#include "app/RaidAlertWindows.h"
#include "app/UiSound.h"
#include "core/Types.h"
#include "ui/DrawList.h"
#include "ui/MessageFrame.h"

#include <array>
#include <string>

namespace vd {

class GameSession;
class Portraits;

// Phase 16 (alerts): the raid's alerts in the middle of the screen, filled from the session's game state:
//  - RaidWarningFrame (RaidWarning.lua: CHAT_MSG_RAID_WARNING's text in ChatTypeInfo RAID_WARNING's colour;
//  the
//    "RaidWarning" sound is GameAudio's, from the same chat line) and RaidBossEmoteFrame (ChatFrame_OnEvent's
//    CHAT_MSG_RAID_BOSS_EMOTE body), 10 s each then a 3 s fade (MessageFrame's defaults in WoW.exe, 0x81CC2C:
//    RaidWarning.xml sets neither), under the UIErrorsFrame (app/MessageHud raidWarningFrames). The chat
//    keeps its lines as before.
//  - ReadyCheckFrame (Blizzard_RaidUI's ShowReadyCheck) while game::ReadyCheck asks the player: the leader's
//    portrait (app/Portraits, its TemporaryPortrait out of sight), READY_CHECK_MESSAGE with its name, Yes /
//    No (ConfirmReadyCheck), PlaySound("ReadyCheck") when it shows.
// Development: VD_READYCHECK_DEMO=1 (the frame over the offline HUD, again after each answer or timeout),
// VD_RAIDWARNING_DEMO=1 (sample raid warnings and boss emotes every few seconds over the offline HUD) and
// VD_READYCHECK_SELF=1 (online: the leader's own check opens its frame too, game::ReadyCheck::setSelfPrompt).
// Owned by GameScreens. Main thread; nothing allocates per frame once warmed up.
class RaidAlertScreens {
public:
    // ChatTypeInfo RAID_WARNING / RAID_BOSS_EMOTE: WoW.exe's default chat colours (0x805E18, 0x805E9E: 255
    // 219 183).
    static constexpr u32 kRaidWarningColor = ui::rgba(255, 219, 183);
    static constexpr u32 kRaidBossEmoteColor = ui::rgba(255, 219, 183);

    RaidAlertScreens();
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }
    void setPortraits(Portraits* portraits) { m_portraits = portraits; }

    // In the world, every frame before drawing: the chat's new raid warnings and boss emotes into their
    // frames, the ready check's sound.
    void update(GameSession& session, u32 nowMs);
    // ReadyCheckFrame (MEDIUM strata: over the panels, under the HIGH message frames), clear of the touch
    // HUD's action `buttons`.
    void drawReadyCheck(GameSession& session, DialogContext& c, const ui::Rect& area, f32 below,
                        const ui::Rect& buttons, u32 nowMs);
    // After the UIErrorsFrame (`below` and `avoid` as it got them): RaidWarningFrame and RaidBossEmoteFrame
    // (frameStrata HIGH).
    void drawFrames(DialogContext& c, const ui::Rect& area, f32 below, const ui::Rect& avoid, u32 nowMs);
    // Out of the world: the development switches' samples over the offline HUD.
    void drawOffline(DialogContext& c, const ui::Rect& area, const ui::Rect& buttons, u32 nowMs);
    void leaveWorld();

    // The frames' lines (the gallery fills them with samples).
    ui::MessageFrame& warnings() { return m_warnings; }
    ui::MessageFrame& emotes() { return m_emotes; }

private:
    void readChat(GameSession& session, u32 nowMs);
    void readEvents(GameSession& session);
    // ShowReadyCheck: the leader's name, race and gender for the text and the stand-in portrait.
    void showReadyCheck(GameSession& session, u64 leader);
    void demoUpdate(u32 nowMs);

    UiSoundSink m_uiSound;
    Portraits* m_portraits = nullptr;
    ui::MessageFrame m_warnings;
    ui::MessageFrame m_emotes;
    u32 m_chatCursor = 0;
    u32 m_eventCursor = 0;
    u64 m_cursorsPlayer = 0; // the character whose rings the cursors follow
    std::string m_scratch;   // a boss emote's body (reserved)

    // ReadyCheckFrame.
    u64 m_leader = 0;
    std::array<char, 160> m_text{}; // READY_CHECK_MESSAGE with the leader's name
    u64 m_fallback = 0;             // the leader's TemporaryPortrait

    // Development.
    bool m_readyCheckDemo = false;
    bool m_readyCheckSelf =
        false; // VD_READYCHECK_SELF: the leader's own check opens its frame (online tests)
    bool m_readyCheckDemoShown = false;
    u32 m_readyCheckDemoAtMs = 0; // when it shows again (0: now)
    u32 m_readyCheckDemoEndMs = 0;
    bool m_raidWarningDemo = false;
    u32 m_raidWarningDemoAtMs = 0;
    u32 m_raidWarningDemoStep = 0;
};

} // namespace vd
