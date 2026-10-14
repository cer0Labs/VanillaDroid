// Phase 18 (extras): the hardware keys of GameScreens (desktop Escape, Enter and the arrows; Android's Back
// and a physical keyboard), kept apart from GameScreens.cpp. Before the world they go to the glue screens
// (GlueXML's AccountLogin_OnKeyDown, RealmList_OnKeyDown, CharacterSelect_OnKeyDown,
// CharacterCreate_OnKeyDown, GlueDialog_OnKeyDown); in the world Escape is the binding TOGGLEGAMEMENU
// (UIParent.lua's ToggleGameMenu) and Enter OPENCHAT (ChatFrame_OpenChat).
#include "app/GameScreens.h"

#include "app/GameSession.h"
#include "core/Log.h"

namespace vd {

bool GameScreens::escapePressed(GameSession& session) {
    if (wantsLogin(session)) {
        if (m_social.creating()) {
            m_social.creationKey(GlueKey::Escape);
        } else {
            m_glueScreens.key(GlueKey::Escape);
        }
        return true;
    }
    if (!session.inWorld()) {
        return false; // entering the world; offline (development), the desktop's Escape closes the window
    }
    game::Gameplay& play = session.play();
    // The chat's edit box open (here: the chat opened to read and type): ChatEdit_OnEscapePressed closes it.
    if (m_social.collapseChat()) {
        return true;
    }
    // ToggleGameMenu(nil): the first of these that does something.
    if (m_popups.escape(session)) { // StaticPopup_EscapePressed
        return true;
    }
    if (m_touch.escape()) { // phase 20: the touch cluster's edit mode, template list or drawer
        return true;
    }
    for (const UiPanel options : {UiPanel::Options, UiPanel::SoundOptions, UiPanel::UIOptions}) {
        if (m_panels.shown(options)) { // OptionsFrameCancel:Click()
            togglePanel(session, options);
            return true;
        }
    }
    if (m_panels.shown(UiPanel::GameMenu)) { // HideUIPanel(GameMenuFrame), igMainMenuQuit
        togglePanel(session, UiPanel::GameMenu);
        return true;
    }
    if (closeMenus(session)) {
        return true;
    }
    if (play.spells().cast().active()) { // SpellStopCasting
        play.cancelCast();
        return true;
    }
    if (play.targeting().active()) { // SpellStopTargeting
        play.stopTargeting();
        return true;
    }
    if (closeAllWindows(session)) {
        return true;
    }
    if (session.target() != 0) { // ClearTarget
        session.setTarget(0);
        return true;
    }
    togglePanel(session, UiPanel::GameMenu); // ShowUIPanel(GameMenuFrame), igMainMenuOpen
    return true;
}

bool GameScreens::closeMenus(GameSession& session) {
    // CloseMenus hides the open UIMenus (the dropdowns): a unit's menu (UnitPopup), and what this client
    // shows as a step over the windows (a player's sheet and its questions, the emote and channel menus; the
    // action editor and its pickers).
    (void)session;
    if (m_unitMenus.wantsModal()) {
        m_unitMenus.closeMenu();
        return true;
    }
    return m_social.escapeModal() || m_combat.escapeWindow();
}

bool GameScreens::closeAllWindows(GameSession& session) {
    // CloseAllWindows: the bags (every ContainerFrame) and the windows (CloseWindows: the left, center,
    // doublewide and full-screen frames, whose OnHide end their NPC's dialog, loot, trade...). True when one
    // was shown.
    const bool any = m_panels.any() || m_inventory.isOpen(InventoryScreens::Window::Bags);
    if (!any) {
        return false;
    }
    m_panels.closeAll();
    m_inventory.closeAll();
    session.play().closeLoot();
    session.play().closeDialog();
    closeHidden(session);
    return true;
}

bool GameScreens::enterPressed(GameSession& session) {
    if (wantsLogin(session)) {
        if (m_social.creating()) {
            m_social.creationKey(GlueKey::Enter);
        } else {
            m_glueScreens.key(GlueKey::Enter);
        }
        return true;
    }
    if (!session.inWorld()) {
        return false;
    }
    m_social.openChat(); // OPENCHAT: ChatFrame_OpenChat("")
    return true;
}

bool GameScreens::arrowPressed(GameSession& session, bool next) {
    if (!wantsLogin(session) || m_social.creating()) {
        return false; // in the world the arrows move the character (the platform's movement keys)
    }
    m_glueScreens.key(next ? GlueKey::Next : GlueKey::Previous);
    return true;
}

} // namespace vd
