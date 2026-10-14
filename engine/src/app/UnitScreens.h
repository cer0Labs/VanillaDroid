#pragma once

#include "app/DialogKit.h"
#include "app/Hud.h"
#include "app/SocialWindows.h"
#include "app/UnitFrames.h"
#include "app/UnitTooltip.h"
#include "core/Types.h"
#include "game/UnitText.h"

#include <array>
#include <string_view>
#include <vector>

namespace vd {

class GameSession;

// Parity pass: the game's side of the unit frames, the unit tooltip and the experience bar (app/UnitFrames,
// app/UnitTooltip): fills their views from the session before the HUD draws, applies what the player did on
// them (target yourself, the player's menu, tooltips kept open) and draws the tooltips over the HUD. Main
// thread; the view storage is fixed (nothing allocates per frame).
class UnitScreens {
public:
    UnitScreens();
    // Before the HUD draws (GameScreens::prepareHud): the frames' and the bar's views into `hud` (left null
    // without a character in the world: the HUD's stand-ins), and the tooltip of a unit just selected (about
    // 4 s).
    void prepareHud(GameSession& session, HudFrame& hud, u32 nowMs);
    // What the HUD reported (GameScreens::actOnHud): a tap on the player frame targets the player
    // (PlayerFrame_OnClick: its helpful spells and items then land on it); a tap on the target frame or a long
    // press on a unit keeps the unit's tooltip (the mouse over them); taps on the PvP badge and the experience
    // bar show their tooltips. Phase 13: the frames' long presses are their menus (app/UnitMenuScreens).
    void actOnHud(GameSession& session, const HudOutput& out, u32 nowMs);
    // Over the HUD, not modal (GameScreens::drawGame, after the messages): the unit tooltip at the default
    // anchor clear of the action buttons, the PvP badge's and the rest state's tooltips.
    void drawOverlay(GameSession& session, DialogContext& c, const Hud& hud, u32 nowMs);
    // A new character in the world (or none): every tooltip closes.
    void reset();
    // Phase 15 (options): SHOW_NEWBIE_TIPS off: the PvP badge's tooltip shows only its name (GameTooltip_AddNewbieTip).
    void setNewbieTips(bool on) { m_newbieTips = on; }

private:
    void fillTooltip(GameSession& session, u64 guid);
    void showTooltip(u64 guid, u32 nowMs, bool kept);
    // Development (VD_HUD_DEMO without a server): a resting player leading a group, Hogger targeted with his
    // tooltip at the default anchor, the experience bar rested with its text.
    void demoViews(HudFrame& hud);
    void drawUnitTooltip(DialogContext& c, const Hud& hud, bool kept, bool clearButton, bool& clear,
                         bool& closed);

    PlayerFrameView m_player;
    TargetFrameView m_target;
    ExperienceBarView m_experience;
    bool m_combatGlow = false; // PlayerStatusTexture left on: the fight went on after the auto attack stopped

    // The unit tooltip: whose, shown until when (kept: until tapped, or the unit goes away).
    game::UnitTooltip m_tooltip;
    u64 m_tipGuid = 0;
    u32 m_tipUntilMs = 0;
    bool m_tipKept = false;
    u64 m_lastTarget = 0;
    u32 m_badgeUntilMs = 0; // the PvP badge's tooltip
    bool m_newbieTips = true; // phase 15 (options)
    u32 m_restUntilMs = 0;  // the experience bar's text and its rest tooltip

    u64 m_playerGuid = 0; // whose views these are (another character: everything closes)
    const game::Reactions* m_reactions = nullptr; // the HUD's (FactionTemplate.dbc), from prepareHud
    bool m_demo = false;                          // VD_HUD_DEMO
};

} // namespace vd
