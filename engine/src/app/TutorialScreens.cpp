#include "app/TutorialScreens.h"

#include "app/GameSession.h"
#include "core/Profile.h"
#include "core/Text.h"

namespace vd {
namespace {

constexpr u32 kPulseMs = 10000;   // SetButtonPulse(button, 10, 0.5): ten seconds...
constexpr u32 kPulseStepMs = 500; // ...the highlight on and off every half second
constexpr u32 kWelcome = static_cast<u32>(game::Tutorial::Welcome);

// TUTORIAL_TITLE<n> and TUTORIAL<n>, n = id + 1.
constexpr const char* kTitles[game::kTutorialCount] = {
    "TUTORIAL_TITLE1",  "TUTORIAL_TITLE2",  "TUTORIAL_TITLE3",  "TUTORIAL_TITLE4",  "TUTORIAL_TITLE5",
    "TUTORIAL_TITLE6",  "TUTORIAL_TITLE7",  "TUTORIAL_TITLE8",  "TUTORIAL_TITLE9",  "TUTORIAL_TITLE10",
    "TUTORIAL_TITLE11", "TUTORIAL_TITLE12", "TUTORIAL_TITLE13", "TUTORIAL_TITLE14", "TUTORIAL_TITLE15",
    "TUTORIAL_TITLE16", "TUTORIAL_TITLE17", "TUTORIAL_TITLE18", "TUTORIAL_TITLE19", "TUTORIAL_TITLE20",
    "TUTORIAL_TITLE21", "TUTORIAL_TITLE22", "TUTORIAL_TITLE23", "TUTORIAL_TITLE24", "TUTORIAL_TITLE25",
    "TUTORIAL_TITLE26", "TUTORIAL_TITLE27", "TUTORIAL_TITLE28", "TUTORIAL_TITLE29", "TUTORIAL_TITLE30",
    "TUTORIAL_TITLE31", "TUTORIAL_TITLE32", "TUTORIAL_TITLE33", "TUTORIAL_TITLE34", "TUTORIAL_TITLE35",
    "TUTORIAL_TITLE36", "TUTORIAL_TITLE37", "TUTORIAL_TITLE38", "TUTORIAL_TITLE39", "TUTORIAL_TITLE40",
    "TUTORIAL_TITLE41", "TUTORIAL_TITLE42", "TUTORIAL_TITLE43", "TUTORIAL_TITLE44", "TUTORIAL_TITLE45",
    "TUTORIAL_TITLE46", "TUTORIAL_TITLE47", "TUTORIAL_TITLE48", "TUTORIAL_TITLE49", "TUTORIAL_TITLE50"};
// The client's text, or ours (VD_TUTORIAL_TOUCH<n>) where it teaches the mouse or the keyboard: the same advice for
// the stick, the taps, the long presses and the buttons of this client.
constexpr const char* kTexts[game::kTutorialCount] = {
    "VD_TUTORIAL_TOUCH1",  "VD_TUTORIAL_TOUCH2",  "VD_TUTORIAL_TOUCH3",  "VD_TUTORIAL_TOUCH4",
    "VD_TUTORIAL_TOUCH5",  "VD_TUTORIAL_TOUCH6",  "VD_TUTORIAL_TOUCH7",  "VD_TUTORIAL_TOUCH8",
    "VD_TUTORIAL_TOUCH9",  "VD_TUTORIAL_TOUCH10", "VD_TUTORIAL_TOUCH11", "VD_TUTORIAL_TOUCH12",
    "VD_TUTORIAL_TOUCH13", "TUTORIAL14",          "VD_TUTORIAL_TOUCH15", "TUTORIAL16",
    "VD_TUTORIAL_TOUCH17", "VD_TUTORIAL_TOUCH18", "TUTORIAL19",          "VD_TUTORIAL_TOUCH20",
    "VD_TUTORIAL_TOUCH21", "VD_TUTORIAL_TOUCH22", "VD_TUTORIAL_TOUCH23", "VD_TUTORIAL_TOUCH24",
    "TUTORIAL25",          "TUTORIAL26",          "TUTORIAL27",          "VD_TUTORIAL_TOUCH28",
    "TUTORIAL29",          "TUTORIAL30",          "TUTORIAL31",          "TUTORIAL32",
    "VD_TUTORIAL_TOUCH33", "TUTORIAL34",          "VD_TUTORIAL_TOUCH35", "TUTORIAL36",
    "TUTORIAL37",          "TUTORIAL38",          "TUTORIAL39",          "VD_TUTORIAL_TOUCH40",
    "TUTORIAL41",          "VD_TUTORIAL_TOUCH42", "TUTORIAL43",          "VD_TUTORIAL_TOUCH44",
    "VD_TUTORIAL_TOUCH45", "TUTORIAL46",          "VD_TUTORIAL_TOUCH47", "VD_TUTORIAL_TOUCH48",
    "VD_TUTORIAL_TOUCH49", "VD_TUTORIAL_TOUCH50"};

} // namespace

const char* TutorialScreens::titleKey(u32 tutorial) {
    return tutorial < game::kTutorialCount ? kTitles[tutorial] : "TUTORIAL_TITLE1";
}

const char* TutorialScreens::textKey(u32 tutorial) {
    return tutorial < game::kTutorialCount ? kTexts[tutorial] : "TUTORIAL1";
}

void TutorialScreens::reset() {
    hideAllAlerts();
    m_shown = false;
    m_centered = false;
    m_displayTips = true;
}

void TutorialScreens::hideAllAlerts() {
    // TutorialFrame_HideAllAlerts: every button and the queue.
    m_alerts.fill(Alert{});
    m_alertCount = 0;
}

void TutorialScreens::enqueue(u32 tutorial, u32 nowMs) {
    // TutorialFrame_NewTutorial: the first free button (none: dropped), at the end of the queue, pulsing.
    if (m_alertCount >= kMaxAlerts || tutorial >= game::kTutorialCount) {
        return;
    }
    m_alerts[m_alertCount++] = {tutorial, nowMs};
}

void TutorialScreens::demo(u32 first, u32 count, u32 nowMs) {
    hideAllAlerts();
    for (u32 i = 0; i < count && first + i < game::kTutorialCount; ++i) {
        enqueue(first + i, nowMs);
    }
}

void TutorialScreens::open(u32 tutorial, bool centered) {
    m_shown = true;
    m_shownTutorial = tutorial;
    m_centered = centered;
}

void TutorialScreens::update(GameSession& session, u32 nowMs) {
    VD_PROFILE_ZONE("TutorialScreens::update");
    game::Gameplay& play = session.play();
    if (m_cursorsPlayer != session.playerGuid()) {
        m_cursorsPlayer = session.playerGuid();
        // The events of this login from the oldest still held: PLAYER_ENTERING_WORLD came before this screen's first
        // frame in the world (the Welcome and Questgivers buttons); older logins' are skipped by their session.
        m_eventCursor = play.events().end() - play.events().size();
        m_lastCinematic = play.cinematic();
        reset();
    }
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        if (e->kind == game::GameEventKind::TutorialTriggered &&
            static_cast<u32>(e->value[0]) == play.tutorials().session()) {
            enqueue(e->id, nowMs);
            m_uiSound(UiSound::TutorialPopup); // TriggerTutorial's PlaySound("TutorialPopup")
        }
    }
    // CINEMATIC_STOP: TutorialFrame_CheckIntro clicks the welcome's button and centres the frame.
    const u32 cinematic = play.cinematic();
    if (m_lastCinematic != 0 && cinematic == 0) {
        for (u32 i = 0; i < m_alertCount; ++i) {
            if (m_alerts[i].tutorial == kWelcome) {
                show(&session, i);
                m_centered = true;
                break;
            }
        }
    }
    m_lastCinematic = cinematic;
}

void TutorialScreens::show(GameSession* session, u32 slot) {
    // The button's OnClick: TutorialFrame:Show(), the button goes, TutorialFrame_Update(id): FlagTutorial, the texts,
    // the queue closes up (the next buttons move left).
    if (slot >= m_alertCount) {
        return;
    }
    const u32 tutorial = m_alerts[slot].tutorial;
    for (u32 i = slot; i + 1 < m_alertCount; ++i) {
        m_alerts[i] = m_alerts[i + 1];
    }
    m_alerts[--m_alertCount] = Alert{};
    if (session && session->inWorld()) {
        session->play().tutorials().flag(static_cast<game::Tutorial>(tutorial));
    }
    m_shown = true;
    m_shownTutorial = tutorial;
    m_centered = false;
}

void TutorialScreens::hide(GameSession* session) {
    // TutorialFrame_OnHide: igMainMenuClose; unchecked: ClearTutorials and no more buttons; the intro's place goes.
    m_shown = false;
    m_centered = false;
    m_uiSound(UiSound::PanelClose);
    if (!m_displayTips) {
        if (session && session->inWorld()) {
            session->play().tutorials().clear();
        }
        hideAllAlerts();
    }
}

void TutorialScreens::drawAlerts(GameSession* session, DialogContext& c, f32 hudBottom, const ui::Rect& actionBar,
                                 u32 nowMs) {
    if (m_alertCount == 0) {
        return;
    }
    for (u32 i = 0; i < m_alertCount; ++i) {
        const u32 age = nowMs - m_alerts[i].sinceMs;
        m_views[i].title = text::get(text::Key(titleKey(m_alerts[i].tutorial)));
        // ButtonPulse_OnUpdate: LockHighlight after the first half second, then every half second, ten seconds.
        m_views[i].highlight = age < kPulseMs && (age / kPulseStepMs) % 2 == 1;
    }
    const i32 tapped =
        tutorialAlerts(c, {m_views.data(), m_alertCount}, c.safeArea.centerX(), tutorialAlertsBottom(c, hudBottom),
                       actionBar);
    if (tapped >= 0) {
        show(session, static_cast<u32>(tapped));
    }
}

void TutorialScreens::drawFrame(GameSession* session, DialogContext& c, f32 hudBottom, const ui::Rect& actionBar) {
    if (!m_shown) {
        return;
    }
    TutorialFrameView view;
    view.shown = true;
    view.title = text::get(text::Key(titleKey(m_shownTutorial)));
    view.text = text::get(text::Key(textKey(m_shownTutorial)));
    view.displayTips = m_displayTips;
    view.centered = m_centered;
    const TutorialResult r =
        tutorialFrame(c, view, c.safeArea.centerX(), tutorialAlertsBottom(c, hudBottom), actionBar);
    if (r.toggledTips) {
        m_displayTips = !m_displayTips;
        m_uiSound(m_displayTips ? UiSound::CheckBoxOn : UiSound::CheckBoxOff);
    }
    if (r.okay) {
        hide(session);
    }
}

} // namespace vd
