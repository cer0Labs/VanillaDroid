// Parity pass (panels): GameScreens' panel windows and micro menu (the same class as GameScreens.cpp): what
// each module has open goes through the original's panel rules (app/UiPanels: UIPanelWindows, ShowUIPanel,
// HideUIPanel), each window is drawn in the place its slot has on this screen, and the micro menu
// (app/MicroMenu) opens them.

#include "app/GameScreens.h"

#include "app/GameSession.h"
#include "app/HudShared.h"
#include "app/MinimapView.h"
#include "app/PanelDemo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>

namespace vd {
namespace {

namespace uf = net::uf;
constexpr u32 kErrorColor = ui::rgba(255, 26, 26); // UIErrorsFrame.lua: UI_ERROR_MESSAGE (1, 0.1, 0.1)

bool deadNow(GameSession& session) {
    const game::DeathState& death = session.play().death();
    return session.inWorld() && death.dead && !death.ghost; // UnitIsDead: the corpse, not the ghost
}

} // namespace

GameScreens::GameScreens() = default;
GameScreens::~GameScreens() = default;

// --- The modules' windows ----------------------------------------------------------------------------------

bool GameScreens::panelWanted(GameSession& session, UiPanel panel) const {
    if (!session.inWorld()) {
        return m_panels.shown(panel); // VD_HUD_DEMO: the slots are the demo's state
    }
    game::Gameplay& play = session.play();
    using P = InventoryScreens::Panel;
    using X = ExchangeScreens::Window;
    switch (panel) {
    case UiPanel::GameMenu:
    case UiPanel::Options:
    case UiPanel::QuestLog:
    case UiPanel::Help: // phase 15 (help): HelpFrame, GMSurveyFrame and SuggestFrame are ours too
    case UiPanel::GmSurvey:
    case UiPanel::Suggest:
    case UiPanel::SoundOptions: // phase 15 (options)
    case UiPanel::UIOptions: return m_panels.shown(panel); // ours: the slot is the state
    case UiPanel::Macro: return m_macros.isOpen();         // phase 15 (options)
    case UiPanel::Character: return m_inventory.isOpen(InventoryScreens::Window::Character);
    case UiPanel::ItemText: return m_inventory.serverPanelOpen(session, P::Reading);
    case UiPanel::Merchant: return m_inventory.serverPanelOpen(session, P::Vendor);
    case UiPanel::Bank: return m_inventory.serverPanelOpen(session, P::Bank);
    case UiPanel::Spellbook: return m_combat.isOpen(CombatWindow::Spellbook);
    case UiPanel::Talents: return m_combat.isOpen(CombatWindow::Talents);
    case UiPanel::Trainer: return m_combat.trainerOpen(session);
    case UiPanel::Loot: return play.loot().open();
    case UiPanel::Quest: return play.dialog().open();
    case UiPanel::Taxi: return m_travel.taxiOpen(session);
    case UiPanel::WorldMap: return m_travel.worldMapOpen();
    case UiPanel::Friends: return m_social.windowWanted();
    case UiPanel::Trade: return m_exchange.window(session) == X::Trade;
    case UiPanel::Mail: return m_exchange.window(session) == X::Mail;
    case UiPanel::Auction: return m_exchange.window(session) == X::Auction;
    case UiPanel::TradeSkill: return TradeSkillScreens::tradeSkillOpen(session); // phase 13
    case UiPanel::Craft: return TradeSkillScreens::craftOpen(session);
    case UiPanel::Inspect: return m_unitMenus.inspectOpen(session); // phase 13 (social)
    case UiPanel::PetStable: return PetScreens::stableOpen(session); // phase 15 (pets)
    case UiPanel::DressUp: return m_dressUp.open(); // phase 15 (items)
    case UiPanel::GuildRegistrar: return GuildScreens::registrarOpen(session); // phase 15 (guild)
    case UiPanel::Petition: return GuildScreens::petitionOpen(session);
    case UiPanel::Tabard: return GuildScreens::tabardOpen(session);
    case UiPanel::Battlefield: return m_battlefield.frameOpen(); // phase 17 (queue)
    case UiPanel::WorldStateScore: return m_worldStateScreens.scoreShown(); // phase 17 (scores)
    default: return false;
    }
}

void GameScreens::closePanel(GameSession& session, UiPanel panel) {
    // What the original's frames do when ShowUIPanel hides them or refuses them (OnHide: CloseMerchant,
    // CloseGossip, CloseLoot, CloseTrainer, CloseTaxiMap, CloseMail, CloseAuctionHouse, CancelTrade...).
    game::Gameplay& play = session.play();
    switch (panel) {
    case UiPanel::Character: m_inventory.close(InventoryScreens::Window::Character); break;
    case UiPanel::ItemText: play.inventory().closeReading(); break;
    case UiPanel::Merchant: play.inventory().closeVendor(); break;
    case UiPanel::Bank: play.inventory().closeBank(); break;
    case UiPanel::Spellbook: m_combat.close(CombatWindow::Spellbook); break;
    case UiPanel::Talents: m_combat.close(CombatWindow::Talents); break;
    case UiPanel::Trainer: play.abilities().closeTrainer(); break;
    case UiPanel::Loot: play.closeLoot(); break;
    case UiPanel::Quest: play.closeDialog(); break;
    case UiPanel::Taxi: play.travel().closeTaxiMap(); break;
    case UiPanel::WorldMap: m_travel.closeWorldMap(); break;
    case UiPanel::Friends: m_social.closeWindow(); break;
    case UiPanel::Trade: m_exchange.closeWindow(session, ExchangeScreens::Window::Trade); break;
    case UiPanel::Mail: m_exchange.closeWindow(session, ExchangeScreens::Window::Mail); break;
    case UiPanel::Auction: m_exchange.closeWindow(session, ExchangeScreens::Window::Auction); break;
    case UiPanel::TradeSkill: m_tradeSkills.closeTradeSkill(session); break; // phase 13: CloseTradeSkill
    case UiPanel::Craft: m_tradeSkills.closeCraft(session); break;           // CloseCraft
    case UiPanel::Inspect: m_unitMenus.closeInspect(); break; // phase 13 (social)
    case UiPanel::PetStable: m_pets.closeStable(session); break; // phase 15 (pets): ClosePetStables
    case UiPanel::DressUp: m_dressUp.close(); break;          // phase 15 (items)
    case UiPanel::GuildRegistrar: m_guild.closeRegistrar(session); break; // phase 15 (guild): CloseGuildRegistrar
    case UiPanel::Petition: m_guild.closePetition(session); break;        // ClosePetition
    case UiPanel::Tabard: m_guild.closeTabard(session); break;            // CloseTabardCreation
    case UiPanel::Macro: m_macros.close(); break;             // phase 15 (options): MacroFrame_OnHide
    case UiPanel::Battlefield: m_battlefield.closeFrame(); break; // phase 17 (queue): CloseBattlefield
    case UiPanel::WorldStateScore: m_worldStateScreens.closeScore(); break; // phase 17 (scores)
    default: break; // the game menu, the options, the quest log: their slot was their state
    }
}

void GameScreens::closeHidden(GameSession& session) {
    for (const UiPanel panel : m_panels.takeHidden()) {
        if (session.inWorld() && panelWanted(session, panel)) {
            if (m_trace) {
                VD_LOG_INFO("Panels: %u closes (its slot went to another window)", static_cast<u32>(panel));
            }
            closePanel(session, panel);
        }
    }
    if (!m_panels.bagsOpen() && m_inventory.isOpen(InventoryScreens::Window::Bags)) {
        // A window took the bags' place, or has them inside.
        m_inventory.close(InventoryScreens::Window::Bags);
    }
}

void GameScreens::syncPanels(GameSession& session) {
    VD_PROFILE_ZONE("GameScreens::syncPanels");
    const bool dead = deadNow(session);
    if (dead && !m_wasDead) {
        m_panels.closeAll(true); // PLAYER_DEAD: CloseAllWindows(1)
    }
    m_wasDead = dead;
    closeHidden(session);
    for (u32 i = 1; i < static_cast<u32>(UiPanel::Count); ++i) {
        const UiPanel panel = static_cast<UiPanel>(i);
        const bool wanted = panelWanted(session, panel);
        const bool shown = m_panels.shown(panel);
        if (wanted && !shown) {
            // A window the server opened (a vendor, a quest giver, loot...): ShowUIPanel. Refused (a
            // full-screen or center window is up, or the player is dead), it closes again, as the original's
            // frames do.
            const UiPanelShow r = m_panels.show(panel, dead, false);
            if (m_trace) {
                VD_LOG_INFO("Panels: %u opens: %s (left %u, center %u)", i,
                            r == UiPanelShow::Shown ? "shown" : "refused", static_cast<u32>(m_panels.left()),
                            static_cast<u32>(m_panels.center()));
            }
            if (r != UiPanelShow::Shown) {
                closePanel(session, panel);
            }
            closeHidden(session);
        } else if (!wanted && shown) {
            m_panels.hide(panel); // its module closed it (its own button, the server, walking away)
            closeHidden(session);
        }
    }
    if (m_panels.bagsOpen() && !m_inventory.isOpen(InventoryScreens::Window::Bags)) {
        m_panels.closeBags(); // the bags' own close button
    }
}

void GameScreens::followTargeting(GameSession& session) {
    // Phase 13 (spells): choosing an item opens the bags (and the character window for a weapon or armor), closed
    // again afterwards when it opened them; choosing a place ends when another window opens.
    if (m_targeting.takeItemChoiceStarted()) {
        m_targetingOpenedBags = m_targeting.wantsBags() && !m_panels.bagsOpen();
        if (m_targetingOpenedBags) {
            m_inventory.openAllBags(session, true); // phase 15 (items): every bag's frame
            syncBags(session);
        }
        m_targetingOpenedCharacter = m_targeting.wantsCharacter() && !m_panels.shown(UiPanel::Character);
        if (m_targetingOpenedCharacter) {
            togglePanel(session, UiPanel::Character);
        }
    }
    if (m_targeting.takeItemChoiceEnded()) {
        if (m_targetingOpenedBags && m_panels.bagsOpen()) {
            m_inventory.close(InventoryScreens::Window::Bags);
            syncBags(session);
        }
        if (m_targetingOpenedCharacter && m_panels.shown(UiPanel::Character)) {
            togglePanel(session, UiPanel::Character);
        }
        m_targetingOpenedBags = m_targetingOpenedCharacter = false;
    }
    // A bit per panel, the bags' last (phase 15: more than 32 panels' room).
    static_assert(static_cast<u32>(UiPanel::Count) < 63, "one bit per panel and the bags'");
    u64 shown = m_panels.bagsOpen() ? 1ull << 63 : 0ull;
    for (u32 i = 1; i < static_cast<u32>(UiPanel::Count); ++i) {
        shown |= m_panels.shown(static_cast<UiPanel>(i)) ? 1ull << i : 0ull;
    }
    m_targeting.windowsShown(session, shown);
}

void GameScreens::togglePanel(GameSession& session, UiPanel panel) {
    const bool online = session.inWorld();
    if (m_panels.shown(panel)) {
        m_panels.hide(panel); // HideUIPanel
        if (online) {
            closePanel(session, panel);
        }
        closeHidden(session);
        return;
    }
    // A center window (the game menu, the options) in the way closes first: the micro menu stays in sight
    // here, where the original's micro buttons were refused (CanOpenPanels) until Escape closed it.
    const UiPanel center = m_panels.center();
    if (center != UiPanel::None && uiPanelInfo(center).area == UiPanelArea::Center &&
        uiPanelInfo(panel).area != UiPanelArea::Center) {
        m_panels.hide(center);
    }
    const UiPanelShow r = m_panels.show(panel, deadNow(session), true);
    if (r == UiPanelShow::Dead) {
        message(kErrorColor, false, "ERR_PLAYER_DEAD"); // NotWhileDeadError
    }
    if (r == UiPanelShow::Shown) { // phase 15 (options): the option windows and the macros start as 1.12's _Load
        switch (panel) {
        case UiPanel::UIOptions: m_options.open(OptionsScreens::Window::Interface, &session, m_settings); break;
        case UiPanel::SoundOptions: m_options.open(OptionsScreens::Window::Sound, &session, m_settings); break;
        case UiPanel::Macro: m_macros.open(); break;
        default: break;
        }
    }
    if (r == UiPanelShow::Shown && online) {
        switch (panel) {
        case UiPanel::Character: m_inventory.open(InventoryScreens::Window::Character); break;
        case UiPanel::Spellbook: m_combat.open(CombatWindow::Spellbook); break;
        case UiPanel::Talents: m_combat.open(CombatWindow::Talents); break;
        case UiPanel::Friends: m_social.openWindow(); break;
        case UiPanel::WorldMap: m_travel.openWorldMap(session); break;
        case UiPanel::Help: m_help.opened(session); break; // phase 15 (help): HelpFrame_OnShow
        default: break;
        }
    }
    closeHidden(session);
}

void GameScreens::toggleBags(GameSession& session) {
    // Phase 15 (items): OpenAllBags (the B key), as the minimap rim's backpack button: every bag's ContainerFrame
    // opens, or all close when they were all open.
    if (!session.inWorld()) {
        if (m_demo) {
            m_demo->bagButton(0, true); // VD_HUD_DEMO: its sample frames
        }
    } else {
        m_inventory.openAllBags(session);
    }
    syncBags(session);
}

void GameScreens::bagButton(GameSession& session, i32 button, bool shift) {
    // Phase 15 (items): the bag bar (MainMenuBarBagButtons): ToggleBackpack, ToggleBag, ToggleKeyRing, OpenAllBags.
    if (!session.inWorld()) {
        if (m_demo) {
            m_demo->bagButton(button, shift);
        }
    } else {
        m_inventory.bagButton(session, button, shift);
    }
    syncBags(session);
}

void GameScreens::syncBags(GameSession& session) {
    // Phase 15 (items): the bags' place follows the ContainerFrames: taken when one is open (not while a window has
    // the bags inside it: vendor, bank, mailbox...; then they close again), given back when the last one closes.
    const bool open = session.inWorld() ? m_inventory.isOpen(InventoryScreens::Window::Bags)
                                        : m_demo && m_demo->bagsOpen();
    if (open && !m_panels.bagsOpen()) {
        if (!m_panels.openBags()) {
            if (session.inWorld()) {
                m_inventory.close(InventoryScreens::Window::Bags);
            } else if (m_demo) {
                m_demo->closeBags();
            }
        }
        closeHidden(session);
    } else if (!open && m_panels.bagsOpen()) {
        m_panels.closeBags();
    }
}

// --- Places and drawing ------------------------------------------------------------------------------------

UiPanelPlaces GameScreens::panelPlaces(const Hud& hud, const DialogContext& c) const {
    UiPanelSpace space;
    space.safe = c.safeArea;
    space.dp = c.ui.dp();
    space.unit = c.ui.px(ui::kUnitDp);
    space.wide = c.profile == ScreenProfile::Wide;
    space.framesBottom = std::max(hud.playerFrameRect().bottom(), hud.targetFrameRect().bottom());
    space.leftTop = c.safeArea.y + 160.0f * space.unit; // under the target's auras (the micro menu's until phase 20)
    const MinimapLayout map = minimapLayout(c.safeArea, space.unit, space.dp);
    space.rightTop = map.top + 178.0f * map.s; // under MinimapZoomOut
    space.cluster = hud.actionBarRect();
    space.stance = hud.stanceBarRect();
    return uiPanelPlaces(space);
}

void GameScreens::drawPanels(GameSession& session, DialogContext& c, ui::TextInput& input,
                             const UiPanelPlaces& places, u32 nowMs) {
    VD_PROFILE_ZONE("GameScreens::panels");
    m_framesCovered = false;
    m_fullScreen = UiPanel::None;
    const auto draw = [&](UiPanel panel, const ui::Rect& place, PanelAlign align, bool opaque = false) {
        if (place.w <= 0.0f || place.h <= 0.0f) {
            return;
        }
        m_framesCovered = m_framesCovered || opaque;
        DialogContext pc = c;
        pc.panel = place;
        pc.panelAlign = align;
        pc.panelOpaque = opaque;
        pc.profile = uiPanelWide(place) ? ScreenProfile::Wide : ScreenProfile::Square;
        drawPanel(session, pc, input, panel, nowMs);
    };
    // What is shown is read first: a window's buttons change the slots for the next frame.
    const bool two = m_panels.capacity() == 2 && places.right.w > 0.0f;
    // Ours: on the wide screens the windows reach over the unit frames, opaque (UiPanelPlaces::overFrames).
    const bool opaque = places.overFrames;
    const UiPanel full = m_panels.full() != UiPanel::None ? m_panels.full() : m_panels.doublewide();
    const UiPanel left = m_panels.left();
    const UiPanel center = m_panels.center();
    const bool bags = m_panels.bagsOpen();
    bool right = false;
    if (full == UiPanel::WorldMap) {
        // Phase 20: WorldMapFrame as the 1.12's, over the whole screen (FULLSCREEN strata, setAllPoints on UIParent):
        // drawFullScreenPanel, after everything else.
        m_fullScreen = full;
        right = true;
    } else if (full != UiPanel::None) {
        ui::Rect place = places.both;
        if (full == UiPanel::Auction && m_dressUp.auctionOpen() && session.inWorld()) {
            // Phase 15 (items): AuctionDressUpFrame docked at the auction house's right edge (TOPLEFT to its
            // TOPRIGHT); the auction house gives it the room.
            const f32 dock = DressUpScreens::auctionDockWidth(c, place.w);
            place.w -= dock;
            m_dressUp.drawAuction(session, c, {place.right(), place.y, dock, place.h});
        }
        draw(full, place, PanelAlign::TopLeft, opaque); // both slots (the original's full screen)
        right = two;
    } else {
        if (left != UiPanel::None) {
            // The quest log's list and parchment side by side, over the stance bar rather than beside it.
            draw(left, left == UiPanel::QuestLog ? places.leftWide : places.left, PanelAlign::TopLeft, opaque);
        }
        if (center == UiPanel::WorldStateScore) { // phase 17 (scores): alone too, and as wide as both slots
            draw(center, WorldStateScreens::scorePlace(c, places), PanelAlign::Center);
            right = two;
        } else if (center == UiPanel::Options || center == UiPanel::SoundOptions || center == UiPanel::UIOptions) {
            m_fullScreen = center; // the whole screen, over everything: drawFullScreenPanel
        } else if (center == UiPanel::GameMenu) {
            draw(center, c.safeArea, PanelAlign::Center); // GameMenuFrame: the screen's center (over the buttons too)
        } else if (center != UiPanel::None && uiPanelInfo(center).area == UiPanelArea::Center) {
            draw(center, places.left, PanelAlign::Center); // the help windows: alone
        } else if (center != UiPanel::None) {
            draw(center, two ? places.right : places.left, PanelAlign::TopLeft, opaque && !two);
            right = two;
        }
        if (bags) { // ContainerFrame: from the bottom right of their place, above the action buttons
            ui::Rect place = two ? places.right : places.left;
            if (two && left == UiPanel::None && center == UiPanel::None) {
                // Phase 15 (items): with the left place free too, the frames' columns go on into it (leftwards, as
                // the original's), in the band both places share.
                const f32 top = std::max(places.left.y, places.right.y);
                const f32 bottom = std::min(places.left.bottom(), places.right.bottom());
                place = {places.left.x, top, places.right.right() - places.left.x, std::max(0.0f, bottom - top)};
            }
            draw(UiPanel::None, place, PanelAlign::BottomRight);
            right = right || two;
        }
    }
    m_rightPlaceUsed = right;
}

void GameScreens::drawFullScreenPanel(GameSession& session, DialogContext& c, ui::TextInput& input, u32 nowMs) {
    if (m_fullScreen == UiPanel::None || !m_panels.shown(m_fullScreen)) {
        return;
    }
    if (m_fullScreen == UiPanel::WorldMap) {
        // WorldMapFrame's BlackoutWorld (black, the whole screen) under the window, which takes the whole safe area;
        // no finger goes through to the world or the HUD.
        ui::Gui g{c.ui, c.draw};
        ui::modalBlocker(g, ui::widgetId("worldMap.blackout"), 0.0f);
        g.draw.solid({0.0f, 0.0f, c.ui.width(), c.ui.height()}, ui::rgba(0, 0, 0));
        DialogContext pc = c;
        pc.panel = c.safeArea;
        pc.panelAlign = PanelAlign::TopLeft;
        pc.panelOpaque = true;
        pc.profile = uiPanelWide(pc.panel) ? ScreenProfile::Wide : ScreenProfile::Square;
        drawPanel(session, pc, input, m_fullScreen, nowMs);
        return;
    }
    drawPanel(session, c, input, m_fullScreen, nowMs);
}

void GameScreens::drawPanel(GameSession& session, DialogContext& c, ui::TextInput& input, UiPanel panel,
                            u32 nowMs) {
    using P = InventoryScreens::Panel;
    using X = ExchangeScreens::Window;
    // The game menu and the options are the same without a server.
    if (panel == UiPanel::GameMenu) {
        switch (const MenuChoice choice = gameMenu(c, !m_popups.loggingOut())) {
        case MenuChoice::Resume: togglePanel(session, UiPanel::GameMenu); break;
        case MenuChoice::Settings:
            togglePanel(session, UiPanel::Options);
            break; // a center window replaces it
        case MenuChoice::SoundOptions: togglePanel(session, UiPanel::SoundOptions); break; // phase 15 (options)
        case MenuChoice::UiOptions: togglePanel(session, UiPanel::UIOptions); break;
        case MenuChoice::Macros: // HideUIPanel(GameMenuFrame); ShowMacroFrame()
            togglePanel(session, UiPanel::GameMenu);
            if (!m_panels.shown(UiPanel::Macro)) {
                togglePanel(session, UiPanel::Macro);
            }
            break;
        case MenuChoice::Logout: // parity (popups): Logout(), then CAMP or an error
        case MenuChoice::Quit:   // ... Quit(), then QUIT: the game ends once logged out
            togglePanel(session, UiPanel::GameMenu);
            if (session.inWorld()) {
                m_popups.askLogout(session, choice == MenuChoice::Quit);
            }
            break;
        default: break;
        }
        return;
    }
    if (panel == UiPanel::Options) {
        if (settingsPanel(c, m_settings)) {
            togglePanel(session, UiPanel::Options);
        }
        return;
    }
    // Phase 15 (options): UIOptionsFrame, SoundOptionsFrame (over the whole screen) and the MacroFrame (offline too).
    if (panel == UiPanel::UIOptions || panel == UiPanel::SoundOptions) {
        const OptionsScreens::Window window =
            panel == UiPanel::UIOptions ? OptionsScreens::Window::Interface : OptionsScreens::Window::Sound;
        if (!m_options.draw(window, &session, c, m_settings)) {
            togglePanel(session, panel);
        }
        return;
    }
    if (panel == UiPanel::Macro) {
        if (!m_macros.draw(&session, c, input)) {
            togglePanel(session, UiPanel::Macro);
        }
        return;
    }
    if (!session.inWorld()) {
        if (m_demo) {
            m_demo->draw(panel, c, m_travel); // VD_HUD_DEMO: sample data
        }
        return;
    }
    switch (panel) {
    case UiPanel::None: m_inventory.drawPanel(session, c, nowMs, P::Bags); break;
    case UiPanel::QuestLog: questLog(session, c); break;
    case UiPanel::Character: m_inventory.drawPanel(session, c, nowMs, P::Character); break;
    case UiPanel::ItemText: m_inventory.drawPanel(session, c, nowMs, P::Reading); break;
    case UiPanel::Merchant: m_inventory.drawPanel(session, c, nowMs, P::Vendor); break;
    case UiPanel::Bank: m_inventory.drawPanel(session, c, nowMs, P::Bank); break;
    case UiPanel::Spellbook: m_combat.drawPanel(session, c, nowMs, CombatWindow::Spellbook); break;
    case UiPanel::Talents: m_combat.drawPanel(session, c, nowMs, CombatWindow::Talents); break;
    case UiPanel::Trainer: m_combat.drawTrainer(session, c, nowMs); break;
    case UiPanel::Loot: lootWindow(session, c); break;
    case UiPanel::Quest: npcWindow(session, c); break;
    case UiPanel::Taxi: m_travel.drawTaxiPanel(session, c); break;
    case UiPanel::WorldMap: m_travel.drawWorldMapPanel(session, c); break;
    case UiPanel::Friends: m_social.drawWindow(session, c, input); break;
    case UiPanel::Trade: m_exchange.drawWindow(session, c, input, m_inventory, nowMs, X::Trade); break;
    case UiPanel::Mail: m_exchange.drawWindow(session, c, input, m_inventory, nowMs, X::Mail); break;
    case UiPanel::Auction: m_exchange.drawWindow(session, c, input, m_inventory, nowMs, X::Auction); break;
    case UiPanel::TradeSkill: m_tradeSkills.drawTradeSkill(session, c, input, nowMs); break; // phase 13
    case UiPanel::Craft: m_tradeSkills.drawCraft(session, c, nowMs); break;
    case UiPanel::Inspect: m_unitMenus.drawInspect(session, c, nowMs); break; // phase 13 (social)
    case UiPanel::PetStable: m_pets.drawStable(session, c, nowMs); break;    // phase 15 (pets)
    case UiPanel::DressUp: m_dressUp.draw(session, c); break;                  // phase 15 (items)
    case UiPanel::GuildRegistrar: m_guild.drawRegistrar(session, c, input); break; // phase 15 (guild)
    case UiPanel::Petition: m_guild.drawPetition(session, c); break;
    case UiPanel::Tabard: m_guild.drawTabard(session, c); break;
    case UiPanel::Help: m_help.drawHelp(session, c, input); break; // phase 15 (help)
    case UiPanel::GmSurvey: m_help.drawSurvey(session, c, input); break;
    case UiPanel::Suggest: m_help.drawSuggest(session, c, input); break;
    case UiPanel::Battlefield: m_battlefield.drawFrame(session, c, nowMs); break; // phase 17 (queue)
    case UiPanel::WorldStateScore: m_worldStateScreens.drawScore(session, c, nowMs); break; // phase 17 (scores)
    default: break;
    }
}

void GameScreens::questLog(GameSession& session, DialogContext& c) {
    // Phase 13 (quests): app/QuestScreens (headers, rewards, share, watch, timers).
    u32 abandon = 0;
    bool scrolled = false;
    const bool closed = m_quests.drawLog(questFrame(session, m_nowMs), c, abandon, scrolled);
    if (scrolled) {
        m_uiSound(UiSound::ChatScroll); // UIPanelScrollBarTemplate's buttons: PlaySound("UChatScrollButton")
    }
    if (abandon != 0) {
        m_popups.askAbandonQuest(session, abandon); // parity (popups): ABANDON_QUEST
    }
    if (closed) {
        togglePanel(session, UiPanel::QuestLog);
    }
}

// --- The micro menu ----------------------------------------------------------------------------------------

void GameScreens::microMenu(GameSession& session, DialogContext& c) {
    MicroMenuView v;
    v.newbieTips = m_settings.showNewbieTips; // phase 15 (options): SHOW_NEWBIE_TIPS
    if (session.inWorld()) {
        const game::Entity* me = session.entities().find(session.playerGuid());
        if (me) {
            v.level = me->value(uf::UNIT_FIELD_LEVEL);
            v.talentPoints = me->value(uf::PLAYER_CHARACTER_POINTS1);
            v.portraitArt = hud::racePortrait(me->byteValue(uf::UNIT_FIELD_BYTES_0, 0),
                                              me->byteValue(uf::UNIT_FIELD_BYTES_0, 2));
        }
        v.portrait = m_playerPortrait;
        v.spells = !session.play().spells().known().empty();
        const auto push = [&](MicroButton b, bool pushed) { v.pushed[static_cast<u32>(b)] = pushed; };
        push(MicroButton::Character, m_panels.shown(UiPanel::Character));
        push(MicroButton::Spellbook, m_panels.shown(UiPanel::Spellbook));
        push(MicroButton::Talents, m_panels.shown(UiPanel::Talents));
        push(MicroButton::QuestLog, m_panels.shown(UiPanel::QuestLog));
        push(MicroButton::Social, m_panels.shown(UiPanel::Friends));
        push(MicroButton::WorldMap, m_panels.shown(UiPanel::WorldMap));
        push(MicroButton::MainMenu, m_panels.shown(UiPanel::GameMenu) || m_panels.shown(UiPanel::Options) ||
                                        m_panels.shown(UiPanel::SoundOptions) || m_panels.shown(UiPanel::UIOptions));
        push(MicroButton::Help, m_panels.shown(UiPanel::Help)); // phase 15 (help)
        if (m_microState.bagsUnfolded) {
            m_inventory.bagBar(session, v.bags, v.keyring, v.freeSlots);
        }
        v.bagsOpen = m_panels.bagsOpen();
        // Phase 15 (items): each button checked while its bag's ContainerFrame is open (ContainerFrame_OnShow).
        for (u32 i = 0; i < 5; ++i) {
            v.bagOpen[i] = m_inventory.containerShown(static_cast<i32>(i));
        }
        v.bagOpen[5] = m_inventory.containerShown(kKeyringContainer);
    } else if (m_demo) {
        v = m_demo->microView(m_panels.bagsOpen(), m_panels);
    } else {
        return;
    }
    const MicroMenuResult r = vd::microMenu(c, m_microLayout, v, m_microState);
    m_bagRects = r.bagRects; // phase 15 (items): drops on the bag bar (after the windows, InventoryScreens)
    if (r.bag >= 0) {
        bagButton(session, r.bag, r.bagShift); // phase 15 (items): a ContainerFrame per bag
    }
    switch (r.button < 0 ? MicroButton::Count : static_cast<MicroButton>(r.button)) {
    case MicroButton::Character:
        // ToggleCharacter("PaperDollFrame"): another tab shown switches to the paper doll, the paper doll
        // closes.
        if (m_panels.shown(UiPanel::Character) && m_inventory.characterTab() != 0) {
            m_inventory.setCharacterTab(0);
        } else {
            togglePanel(session, UiPanel::Character);
            m_inventory.setCharacterTab(0);
        }
        break;
    case MicroButton::Spellbook: togglePanel(session, UiPanel::Spellbook); break;
    case MicroButton::Talents:
        if (v.level >= 10) { // ToggleTalentFrame
            togglePanel(session, UiPanel::Talents);
        }
        break;
    case MicroButton::QuestLog: togglePanel(session, UiPanel::QuestLog); break;
    case MicroButton::Social: togglePanel(session, UiPanel::Friends); break;
    case MicroButton::WorldMap: togglePanel(session, UiPanel::WorldMap); break;
    case MicroButton::MainMenu:
        // ToggleGameMenu(1): the options close; the game menu closes, or every window does and it opens.
        if (m_panels.shown(UiPanel::Options)) {
            togglePanel(session, UiPanel::Options);
        }
        for (const UiPanel options : {UiPanel::SoundOptions, UiPanel::UIOptions}) { // phase 15 (options)
            if (m_panels.shown(options)) {
                togglePanel(session, options);
            }
        }
        togglePanel(session, UiPanel::GameMenu);
        break;
    case MicroButton::Help: togglePanel(session, UiPanel::Help); break; // phase 15 (help): ToggleHelpFrame
    default: break;
    }
}

} // namespace vd
