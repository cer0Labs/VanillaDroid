#pragma once

#include "app/Dialogs.h"
#include "app/PvpGallery.h"
#include "core/Types.h"

namespace vd {

class CombatScreens;
class GameSession;
class Hud;
class SocialScreens;

// Phase 17 (honor): what the game shows of honor besides the lines game::NoticeFeed words and the popups
// app/PopupScreens opens (AREA_SPIRIT_HEAL): the text that rises over the player's head for a kill (WoW.exe
// 0x6072E0: "HK: <rank>" / "DK: <rank>", in COMBAT_HONOR_GAIN's colour, on the player's name plate), and the
// development switch VD_PVP_DEMO over the offline HUD (--walk, VD_HUD_DEMO): spirit (AREA_SPIRIT_HEAL
// counting down), skinned (DEATH_CORPSE_SKINNED in the UIErrorsFrame), credit (the honor lines in the chat's
// Combat Log, "Honor +37" and "HK: Grunt"), attack (the defense channels' lines in the chat); several at once
// separated by commas. Main thread; nothing allocates per frame.
class PvpScreens {
public:
    PvpScreens();
    // In the world, after the session's events were read by the others (any order: its own cursor).
    void update(GameSession& session, Hud& hud, u32 nowMs);
    // Offline: VD_PVP_DEMO's samples (nothing without it).
    void drawOffline(DialogContext& c, CombatScreens& combat, SocialScreens& social, u32 nowMs);
    bool demo() const { return m_demo != 0; }

private:
    static constexpr u8 kDemoSpirit = 1, kDemoSkinned = 2, kDemoCredit = 4, kDemoAttack = 8;

    void readDemo(); // VD_PVP_DEMO, on the first offline frame

    u32 m_eventCursor = 0;
    u64 m_player = 0;
    u8 m_demo = 0;
    bool m_demoRead = false;
    bool m_demoLines = false; // the sample lines went into the chat and the combat log
    PvpGallery m_gallery;     // VD_PVP_DEMO's pieces
};

} // namespace vd
