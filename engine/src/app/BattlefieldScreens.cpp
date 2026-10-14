#include "app/BattlefieldScreens.h"

#include "app/GameSession.h"
#include "app/MinimapView.h"
#include "app/PopupScreens.h"
#include "app/WorldStateScreens.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/AreaData.h"
#include "game/BattlefieldQueue.h"
#include "game/Gameplay.h"
#include "game/Travel.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

using game::BattlefieldState;
using game::GameEventKind;

constexpr u32 kWarsongGulch = 489; // the demo's queues
constexpr u32 kArathiBasin = 529;
constexpr u32 kAlteracValley = 30;

const char* actionName(u32 action) {
    switch (action) {
    case 1: return "CHANGE_INSTANCE (ShowBattlefieldList)";
    case 2: return "LEAVE_QUEUE (AcceptBattlefieldPort(i, nil))";
    case 3: return "ENTER_BATTLE (AcceptBattlefieldPort(i, 1))";
    default: return "?";
    }
}

} // namespace

void BattlefieldScreens::init(PopupScreens* popups, WorldStateScreens* worldStates) {
    m_popups = popups;
    m_worldStates = worldStates;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    if (const char* demo = VD_DEV_ENV("VD_BATTLEFIELD_DEMO")) {
        const std::string_view d(demo);
        m_demo = d == "queued" ? 1u : d == "confirm" ? 2u : d == "active" ? 3u : 0u;
        if (m_demo == 0) {
            VD_LOG_WARN("VD_BATTLEFIELD_DEMO=%s: queued, confirm or active", demo);
        }
    }
}

// --- Every frame in the world ------------------------------------------------------------------------------

void BattlefieldScreens::update(GameSession& session, u32 nowMs) {
    VD_PROFILE_ZONE("BattlefieldScreens::update");
    game::Gameplay& play = session.play();
    if (m_cursorsPlayer != session.playerGuid()) {
        // Another character: from now on, its own interface (the shine starts lit again, as the XML leaves
        // it).
        m_cursorsPlayer = session.playerGuid();
        m_cursor = play.events().end();
        m_frameOpen = false;
        m_buttonShown = false;
        m_buttonFadeAtMs = 0;
        m_shineFaded = false;
        m_menuOpen = false;
        m_tip = Tip::None;
        m_pendingTap = false;
    }
    readEvents(session, nowMs);
    if (play.battlefieldQueueing().mapId() == 0) {
        m_frameOpen = false; // a new world forgot the list (the loading screen closed every window)
    }
}

void BattlefieldScreens::readEvents(GameSession& session, u32 nowMs) {
    game::Gameplay& play = session.play();
    const game::Battlegrounds& bg = play.battlegrounds();
    while (const game::GameEvent* e = play.events().next(m_cursor)) {
        switch (e->kind) {
        case GameEventKind::BattlefieldsShow: // ShowUIPanel(BattlefieldFrame); SetSelectedBattlefield(0)
            m_frameOpen = true;
            play.battlefieldQueueing().select(0);
            break;
        case GameEventKind::BattlefieldsClosed: m_frameOpen = false; break; // HideUIPanel
        case GameEventKind::BattlefieldQueueJoined: queueJoined(nowMs); break;
        case GameEventKind::BattlefieldConfirm:
            // StaticPopup_Show("CONFIRM_BATTLEFIELD_ENTRY", mapName, nil, i); PlaySound("PVPTHROUGHQUEUE").
            if (m_popups) {
                m_popups->askBattlefieldEntry(session, e->id, queueName(session, e->id));
            }
            m_uiSound(UiSound::PvpThroughQueue);
            break;
        case GameEventKind::BattlefieldStatus:
            // Not "confirm": StaticPopup_Hide("CONFIRM_BATTLEFIELD_ENTRY", i).
            for (u32 slot = 0; slot < game::Battlegrounds::kQueues; ++slot) {
                if (bg.queue(slot).state != BattlefieldState::Confirm && m_popups) {
                    m_popups->hideBattlefieldEntry(slot);
                }
            }
            break;
        default: break;
        }
    }
}

void BattlefieldScreens::queueJoined(u32 nowMs) {
    m_uiSound(UiSound::PvpEnterQueue);
    m_buttonFadeAtMs = std::max(nowMs, 1u); // UIFrameFadeIn(MiniMapBattlefieldFrame, CHAT_FRAME_FADE_TIME)
    m_shineFaded = true;                    // BattlegroundShineFadeIn: 0 to 1 in 0.5 s, then out in 0.5 s
    m_shineAtMs = nowMs;
}

void BattlefieldScreens::panelShown(bool shown) {
    if (shown != m_frameShown) {
        m_uiSound(shown ? UiSound::CharacterOpen : UiSound::CharacterClose); // OnShow / OnHide
        m_frameShown = shown;
    }
}

std::string_view BattlefieldScreens::queueName(GameSession& session, u32 slot) {
    // GetBattlefieldStatus' mapName, " "..instanceID when it has one.
    const game::Battlegrounds& bg = session.play().battlegrounds();
    std::array<char, 96>& out = m_names[std::min(slot, 2u)];
    const std::string_view map = bg.mapName(slot);
    const u32 instance = bg.queue(slot).instanceId;
    const int n = instance != 0 ? std::snprintf(out.data(), out.size(), "%.*s %u",
                                                static_cast<int>(map.size()), map.data(), instance)
                                : std::snprintf(out.data(), out.size(), "%.*s", static_cast<int>(map.size()),
                                                map.data());
    return {out.data(), static_cast<usize>(std::clamp(n, 0, static_cast<int>(out.size()) - 1))};
}

// --- BattlefieldFrame --------------------------------------------------------------------------------------

void BattlefieldScreens::drawFrame(GameSession& session, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("BattlefieldScreens::frame");
    game::Gameplay& play = session.play();
    game::BattlefieldQueueing& queue = play.battlefieldQueueing();
    const game::Battlegrounds& bg = play.battlegrounds();
    game::BattlefieldInfo info;
    queue.info(info);
    // BattlefieldFrame_Update: "First Available", then "<map> <instance>" per battle, each with its queue's
    // status.
    const u32 rows = std::min(queue.count() + 1, kMaxRows);
    for (u32 i = 0; i < rows; ++i) {
        BattlefieldRowView& row = m_rows[i];
        if (i == 0) {
            row.text = text::get("FIRST_AVAILABLE");
        } else {
            std::array<char, 64>& out = m_rowTexts[i];
            const int n = std::snprintf(out.data(), out.size(), "%.*s %u", static_cast<int>(info.name.size()),
                                        info.name.data(), queue.instanceId(i - 1));
            row.text = {out.data(), static_cast<usize>(std::clamp(n, 0, static_cast<int>(out.size()) - 1))};
        }
        row.status = {};
        for (u32 slot = 0; slot < game::Battlegrounds::kQueues; ++slot) {
            const game::BattlefieldQueue& q = bg.queue(slot);
            const bool sameMap = q.mapId == queue.mapId();
            if (q.state != BattlefieldState::None && i > 0 && sameMap &&
                q.instanceId == queue.instanceId(i - 1)) {
                if (q.state == BattlefieldState::Queued) {
                    row.status = text::get("BATTLEFIELD_QUEUE_STATUS");
                } else if (q.state == BattlefieldState::Confirm) {
                    row.status = text::get("BATTLEFIELD_CONFIRM_STATUS");
                }
            } else if (i == 0 && sameMap && queue.count() == 0 && q.state == BattlefieldState::Queued) {
                row.status = text::get("BATTLEFIELD_QUEUE_STATUS");
            }
        }
    }
    BattlefieldFrameView v;
    v.title = info.name;
    v.rows = {m_rows.data(), rows};
    v.selected = static_cast<i32>(queue.selected());
    v.description = info.description;
    v.groupJoinShown = queue.canJoinAsGroup();
    v.groupJoinEnabled = queue.groupLeader();
    const BattlefieldFrameResult r = battlefieldFrame(c, v);
    if (r.select >= 0) {
        queue.select(static_cast<u32>(r.select)); // BattlefieldButton_OnClick
    }
    if (r.join || r.joinAsGroup) {
        // BattlefieldFrameJoinButton_OnClick: JoinBattlefield(GetSelectedBattlefield()[, 1]); HideUIPanel.
        const bool sent = queue.join(queue.selected(), r.joinAsGroup, nowMs);
        if (m_trace) {
            VD_LOG_INFO("Battlefield: join %u%s (%s)", queue.selected(), r.joinAsGroup ? " as a group" : "",
                        sent ? "sent" : "refused");
        }
        m_frameOpen = false;
    }
    if (r.closed) {
        m_frameOpen = false; // HideUIPanel: CloseBattlefield (nothing sent)
    }
    if (r.rowTip >= 0 || r.groupTip) {
        m_tip = r.groupTip ? Tip::GroupJoin : Tip::Row;
        m_tipRow = r.rowTip;
        m_tipAnchor = r.tipAnchor;
        m_tipUntilMs = nowMs + kTooltipMs;
    }
}

// --- MiniMapBattlefieldFrame -------------------------------------------------------------------------------

f32 BattlefieldScreens::buttonAlpha(u32 nowMs) const {
    if (m_buttonFadeAtMs == 0) {
        return 1.0f;
    }
    return std::clamp(static_cast<f32>(nowMs - m_buttonFadeAtMs) / static_cast<f32>(kButtonFadeMs), 0.0f,
                      1.0f);
}

f32 BattlefieldScreens::shineAlpha(u32 nowMs) const {
    if (!m_shineFaded) {
        return 1.0f; // BattlegroundShine has no alpha="0" in Minimap.xml: lit until its first fade
    }
    const u32 t = nowMs - m_shineAtMs;
    if (t < kShineFadeMs) {
        return static_cast<f32>(t) / static_cast<f32>(kShineFadeMs);
    }
    if (t < 2 * kShineFadeMs) {
        return 1.0f - static_cast<f32>(t - kShineFadeMs) / static_cast<f32>(kShineFadeMs);
    }
    return 0.0f;
}

void BattlefieldScreens::queueViews(GameSession& session, u32 nowMs) {
    const game::Battlegrounds& bg = session.play().battlegrounds();
    for (u32 slot = 0; slot < game::Battlegrounds::kQueues; ++slot) {
        const game::BattlefieldQueue& q = bg.queue(slot);
        BattlefieldQueueView& v = m_queueViews[slot];
        v.state = q.state;
        v.name = queueName(session, slot);
        v.mapName = bg.mapName(slot);
        v.averageWaitMs = q.averageWaitMs;
        v.waitedMs = bg.timeWaitedMs(slot, nowMs);
        v.expiresMs = bg.portExpirationMs(slot, nowMs);
    }
}

std::string_view BattlefieldScreens::tooltipText(GameSession& session, u32 nowMs) {
    queueViews(session, nowMs);
    return battlefieldStatusText(m_tipText, m_queueViews);
}

u32 BattlefieldScreens::buildMenu(GameSession& session) {
    queueViews(session, session.lastUpdateMs());
    m_menuCount = battlefieldMenu(m_queueViews, m_menuItems, m_menuActions, m_menuSlots);
    return m_menuCount;
}

void BattlefieldScreens::click(GameSession& session, bool shift, bool online) {
    // OnClick while MiniMapBattlefieldFrame.status == "active".
    if (m_worldStates) {
        if (shift) {
            m_worldStates->toggleBattlefieldMinimap(session);
        } else {
            m_worldStates->toggleScore(session);
        }
    }
    if (m_trace || !online) {
        VD_LOG_INFO("Battlefield%s: %s", online ? "" : " demo",
                    shift ? "ToggleBattlefieldMinimap (the second tap: Shift + click)"
                          : "ToggleWorldStateScoreFrame");
    }
}

void BattlefieldScreens::pick(GameSession& session, BattlefieldMenuAction action, u32 slot, bool online,
                              u32 nowMs) {
    game::Gameplay& play = session.play();
    if (m_trace || !online) {
        VD_LOG_INFO("Battlefield%s: queue %u: %s", online ? "" : " demo", slot + 1,
                    actionName(static_cast<u32>(action)));
    }
    if (!online) {
        // The demo: as if the server had answered.
        net::BattlefieldStatus s;
        s.queueSlot = slot;
        const game::BattlefieldQueue& q = play.battlegrounds().queue(slot);
        if (action == BattlefieldMenuAction::LeaveQueue) {
            play.battlegrounds().demoStatus(s, nowMs); // map 0: the slot empties
        } else if (action == BattlefieldMenuAction::EnterBattle) {
            s.mapId = q.mapId;
            s.bracketId = q.bracketId;
            s.instanceId = q.instanceId;
            s.status = net::kBattlefieldStatusInProgress;
            s.time2 = 1000;
            play.battlegrounds().demoStatus(s, nowMs);
        }
        return;
    }
    switch (action) {
    case BattlefieldMenuAction::ChangeInstance: play.battlefieldQueueing().showList(slot); break;
    case BattlefieldMenuAction::LeaveQueue: play.battlegrounds().acceptPort(slot, false); break;
    case BattlefieldMenuAction::EnterBattle: play.battlegrounds().acceptPort(slot, true); break;
    case BattlefieldMenuAction::None: break;
    }
}

void BattlefieldScreens::button(GameSession& session, DialogContext& c, bool minimapShown, u32 nowMs,
                                bool online) {
    const game::Gameplay& play = session.play();
    const game::Battlegrounds& bg = play.battlegrounds();
    const bool shown = minimapShown && bg.queueCount() > 0;
    if (!shown) {
        m_buttonShown = false;
        m_menuOpen = false;
        m_pendingTap = false;
        if (m_tip == Tip::Button) {
            m_tip = Tip::None;
        }
        return;
    }
    m_buttonShown = true;
    BattlefieldButtonView v;
    v.icon =
        play.travel().playerTeam() == game::kAreaTeamHorde ? kBattlefieldIconHorde : kBattlefieldIconAlliance;
    v.alpha = buttonAlpha(nowMs);
    v.shine = shineAlpha(nowMs);
    m_place = v.place;
    const MinimapLayout layout = minimapLayout(c.safeArea, c.ui.px(ui::kUnitDp), c.ui.dp());
    const BattlefieldButtonResult r = battlefieldMinimapButton(c, layout, v);
    m_buttonRect = r.rect;
    bool active = false; // MiniMapBattlefieldFrame.status
    for (u32 slot = 0; slot < game::Battlegrounds::kQueues; ++slot) {
        active = active || bg.queue(slot).state == BattlefieldState::Active;
    }
    if (r.longPress) {
        // The right button: the menu; nothing to put in it (only the battle) shows the tooltip instead.
        if (buildMenu(session) > 0) {
            m_menuOpen = true;
            m_tip = Tip::None;
        } else {
            m_tip = Tip::Button;
            m_tipUntilMs = nowMs + kTooltipMs;
        }
    } else if (r.tapped) {
        if (active) {
            m_tip = Tip::None; // OnClick: GameTooltip:Hide()
            if (m_pendingTap && nowMs - m_pendingTapMs <= kDoubleTapMs) {
                m_pendingTap = false;
                click(session, true, online);
            } else {
                m_pendingTap = true;
                m_pendingTapMs = nowMs;
            }
        } else if (m_tip == Tip::Button) {
            m_tip = Tip::None; // a second tap closes it
        } else {
            m_tip = Tip::Button;
            m_tipUntilMs = nowMs + kTooltipMs;
        }
    }
    if (m_pendingTap && nowMs - m_pendingTapMs > kDoubleTapMs) {
        m_pendingTap = false;
        if (active) {
            click(session, false, online);
        }
    }
    if (m_tip == Tip::Button) {
        if (static_cast<i32>(nowMs - m_tipUntilMs) >= 0) {
            m_tip = Tip::None;
        } else {
            battlefieldButtonTooltip(c, m_buttonRect, tooltipText(session, nowMs)); // worded again each frame
        }
    }
}

void BattlefieldScreens::drawMinimapButton(GameSession& session, DialogContext& c, bool minimapShown,
                                           u32 nowMs) {
    VD_PROFILE_ZONE("BattlefieldScreens::minimapButton");
    button(session, c, minimapShown, nowMs, true);
}

void BattlefieldScreens::menu(GameSession& session, DialogContext& c, bool online, u32 nowMs) {
    if (!m_menuOpen) {
        return;
    }
    if (buildMenu(session) == 0) {
        m_menuOpen = false;
        return;
    }
    // ToggleDropDownMenu(1, nil, MiniMapBattlefieldDropDown, "MiniMapBattlefieldFrame", 10, -5).
    const ui::Rect anchor =
        battlefieldMenuAnchor(minimapLayout(c.safeArea, c.ui.px(ui::kUnitDp), c.ui.dp()), m_place);
    DropDownMenuView v;
    v.x = anchor.x;
    v.y = anchor.y;
    v.level1 = {m_menuItems.data(), m_menuCount};
    const DropDownMenuResult r = dropDownMenu(c, "battlefield.menu", v);
    if (r.level == 1 && r.picked >= 0 && static_cast<u32>(r.picked) < m_menuCount) {
        m_menuOpen = false;
        pick(session, m_menuActions[static_cast<u32>(r.picked)], m_menuSlots[static_cast<u32>(r.picked)],
             online, nowMs);
    } else if (r.closed) {
        m_menuOpen = false;
    }
}

void BattlefieldScreens::drawOverlay(GameSession& session, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("BattlefieldScreens::overlay");
    if (m_tip == Tip::Row || m_tip == Tip::GroupJoin) {
        if (!m_frameOpen || static_cast<i32>(nowMs - m_tipUntilMs) >= 0) {
            m_tip = Tip::None;
        } else if (m_tip == Tip::GroupJoin) {
            battlefieldTooltip(c, m_tipAnchor, text::get("BATTLEFIELD_GROUP_JOIN"),
                               text::get("NEWBIE_TOOLTIP_BATTLEFIELD_GROUP_JOIN"));
        } else if (m_tipRow >= 0 && static_cast<u32>(m_tipRow) < kMaxRows) {
            battlefieldTooltip(c, m_tipAnchor, m_rows[static_cast<u32>(m_tipRow)].text,
                               text::get(m_tipRow == 0 ? "NEWBIE_TOOLTIP_FIRST_AVAILABLE"
                                                       : "NEWBIE_TOOLTIP_ENTER_BATTLEGROUND"));
        }
    }
    menu(session, c, true, nowMs);
}

// --- Development: VD_BATTLEFIELD_DEMO ----------------------------------------------------------------------

void BattlefieldScreens::demoStart(GameSession& session, u32 nowMs) {
    game::Battlegrounds& bg = session.play().battlegrounds();
    const auto status = [&](u32 slot, u32 map, u8 bracket, u32 instance, u32 state, u32 time1, u32 time2) {
        net::BattlefieldStatus s;
        s.queueSlot = slot;
        s.mapId = map;
        s.bracketId = bracket;
        s.instanceId = instance;
        s.status = state;
        s.time1 = time1;
        s.time2 = time2;
        bg.demoStatus(s, nowMs);
    };
    if (m_demo == 1) {
        status(0, kWarsongGulch, 4, 0, net::kBattlefieldStatusWaitQueue, 135000, 47000);
    } else if (m_demo == 2) {
        status(0, kWarsongGulch, 4, 3, net::kBattlefieldStatusWaitJoin, 80000, 0);
        status(1, kArathiBasin, 3, 0, net::kBattlefieldStatusWaitQueue, 0, 12000);
    } else {
        status(0, kWarsongGulch, 4, 3, net::kBattlefieldStatusInProgress, 0, 300000);
        status(1, kArathiBasin, 3, 0, net::kBattlefieldStatusWaitQueue, 0, 12000);
        status(2, kAlteracValley, 0, 2, net::kBattlefieldStatusWaitJoin, 60000, 0);
    }
    // What BattlefieldFrame_UpdateStatus does with them: the sound and the shine, the invitations' popups.
    for (u32 slot = 0; slot < game::Battlegrounds::kQueues; ++slot) {
        const BattlefieldState state = bg.queue(slot).state;
        if (state == BattlefieldState::Queued) {
            queueJoined(nowMs);
        } else if (state == BattlefieldState::Confirm) {
            if (Popup* p = m_demoStack.show(PopupKind::ConfirmBattlefieldEntry, false, slot)) {
                p->setName(queueName(session, slot));
            }
            m_uiSound(UiSound::PvpThroughQueue);
        }
    }
    VD_LOG_INFO("Battlefield demo: %u queues (long press the minimap's button for its menu)",
                bg.queueCount());
}

void BattlefieldScreens::demoPopups(GameSession& session, DialogContext& c, u32 nowMs) {
    const game::Battlegrounds& bg = session.play().battlegrounds();
    for (u32 slot = 0; slot < game::Battlegrounds::kQueues; ++slot) {
        if (bg.queue(slot).state != BattlefieldState::Confirm) {
            m_demoStack.hide(PopupKind::ConfirmBattlefieldEntry, slot);
        }
    }
    for (u32 i = 0; i < PopupStack::kSlots; ++i) {
        const Popup& p = m_demoStack.slots()[i];
        PopupView& v = m_demoViews[i];
        v = PopupView{};
        if (!p.shown()) {
            continue;
        }
        const PopupInfo& info = popupInfo(p.kind);
        v.text = text::format(m_demoTexts[i], info.text, p.nameView());
        v.button1 = text::get(info.button1);
        v.button2 = text::get(info.button2);
    }
    const PopupTaps taps = popupStack(c, m_demoViews, m_demoHeights);
    if (taps.slot >= 0) {
        m_demoStack.click(static_cast<u32>(taps.slot), taps.button);
    }
    PopupOutcome o;
    while (m_demoStack.takeOutcome(o)) {
        if (o.end == PopupEnd::Accept) {
            pick(session, BattlefieldMenuAction::EnterBattle, static_cast<u32>(o.data), false, nowMs);
        } else if (o.end == PopupEnd::Cancel) {
            VD_LOG_INFO("Battlefield demo: queue %u: HIDE (the popup goes, the queue stays)",
                        static_cast<u32>(o.data) + 1);
        }
    }
}

void BattlefieldScreens::drawOffline(GameSession& session, DialogContext& c, bool minimapShown, u32 nowMs) {
    if (m_demo == 0) {
        return;
    }
    VD_PROFILE_ZONE("BattlefieldScreens::offline");
    if (!m_demoStarted) {
        m_demoStarted = true;
        demoStart(session, nowMs);
    }
    button(session, c, minimapShown, nowMs, false);
    demoPopups(session, c, nowMs);
    menu(session, c, false, nowMs);
}

} // namespace vd
