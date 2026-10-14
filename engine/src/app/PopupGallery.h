#pragma once

#include "app/Dialogs.h"
#include "app/PopupStack.h"
#include "app/PopupWindows.h"
#include "core/Types.h"

#include <array>

namespace vd {

// Development (parity, popups): the StaticPopup stack with sample questions, through the real PopupStack
// rules and PopupWindows view. VD_UI_GALLERY=popups / popups2 / popups3 draws it alone; VD_POPUP_DEMO=1 / 2 /
// 3 draws it over the VD_HUD_DEMO HUD (offline), to see that the HUD stays usable under it. Set 1: DEATH
// (with a self-resurrection and its countdown), PARTY_INVITE, DUEL_REQUESTED, CONFIRM_TALENT_WIPE (a money
// frame). Set 2: CAMP (its countdown), DELETE_GOOD_ITEM (an alert and its edit box), RESURRECT_NO_SICKNESS
// (Accept waits for the corpse delay), CONFIRM_SUMMON. Set 3: XP_LOSS (asked twice), GUILD_INVITE, TRADE.
// Taps go to the log ("Popup gallery: ..."); a set comes back a few seconds after its last popup closed.
class PopupGallery {
public:
    void draw(DialogContext& c, u32 set, u64 nowMs);

private:
    void fill(u32 set);

    PopupStack m_stack;
    std::array<f32, PopupStack::kSlots> m_heights{};
    std::array<PopupView, PopupStack::kSlots> m_views{};
    std::array<std::array<char, 400>, PopupStack::kSlots> m_texts{};
    u32 m_set = 0;
    u64 m_lastMs = 0;
    u64 m_emptySinceMs = 0;
    bool m_filled = false;
};

} // namespace vd
