#pragma once

#include "app/Dialogs.h"
#include "app/TutorialWindows.h"
#include "app/UiSound.h"
#include "core/Types.h"
#include "game/Tutorials.h"

#include <array>

namespace vd {

class GameSession;

// Phase 18 (p3): the game's side of TutorialFrame (TutorialFrame.lua over game::Tutorials): TUTORIAL_TRIGGER queues an
// alert button (at most MAX_TUTORIAL_ALERTS, the TutorialPopup sound; TutorialFrame_NewTutorial), a tap shows its
// tutorial (TutorialFrame_Update: FlagTutorial, the title and text, the queue closes up), Okay hides the frame
// (igMainMenuClose; with "Display Tips" unchecked: ClearTutorials and every button gone), CINEMATIC_STOP opens the
// welcome by itself at the screen's centre (TutorialFrame_CheckIntro). The texts are the client's (TUTORIAL_TITLE<n>,
// TUTORIAL<n>) except where they teach a mouse or keys: those say the touch controls (VD_TUTORIAL_TOUCH<n>,
// locale/*/p3.lang). GameScreens owns it. Main thread; nothing allocates.
class TutorialScreens {
public:
    static constexpr u32 kMaxAlerts = 10; // MAX_TUTORIAL_ALERTS

    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }
    // Once a frame in the world: TUTORIAL_TRIGGER and CINEMATIC_STOP.
    void update(GameSession& session, u32 nowMs);
    // The buttons (under the panels) and the frame (over them).
    // `hudBottom`: where the buttons' bottom goes (over the HUD's cast bar: GameScreens::hudBottom); `actionBar`: the
    // HUD's action buttons (kept clear of).
    void drawAlerts(GameSession* session, DialogContext& c, f32 hudBottom, const ui::Rect& actionBar, u32 nowMs);
    void drawFrame(GameSession* session, DialogContext& c, f32 hudBottom, const ui::Rect& actionBar);
    // A new character (or none): the queue and the frame go (a session's Lua state starts over).
    void reset();

    // UIOptionsFrame: TutorialFrame_HideAllAlerts, and TutorialFrameCheckButton:SetChecked(1) after a reset.
    void hideAllAlerts();
    void setDisplayTips(bool on) { m_displayTips = on; }

    // The tutorial's keys (TUTORIAL_TITLE<n> / TUTORIAL<n> or ours), `tutorial` 0-based.
    static const char* titleKey(u32 tutorial);
    static const char* textKey(u32 tutorial);

    // Development and the gallery (VD_P3_TUTORIAL=<n>|all): alerts queued without a server; `open` shows one.
    void demo(u32 first, u32 count, u32 nowMs);
    void open(u32 tutorial, bool centered);

private:
    struct Alert {
        u32 tutorial = game::kTutorialCount; // kTutorialCount: free
        u32 sinceMs = 0;                      // SetButtonPulse's start
    };
    void enqueue(u32 tutorial, u32 nowMs);
    void show(GameSession* session, u32 slot);
    void hide(GameSession* session);

    UiSoundSink m_uiSound;
    std::array<Alert, kMaxAlerts> m_alerts{}; // in queue order (TUTORIALFRAME_QUEUE)
    u32 m_alertCount = 0;
    std::array<TutorialAlertView, kMaxAlerts> m_views{};
    u32 m_eventCursor = 0;
    u64 m_cursorsPlayer = 0;
    u32 m_lastCinematic = 0;
    // TutorialFrame.
    bool m_shown = false;
    u32 m_shownTutorial = 0;
    bool m_centered = false;
    bool m_displayTips = true; // TutorialFrameCheckButton (checked="true")
};

} // namespace vd
