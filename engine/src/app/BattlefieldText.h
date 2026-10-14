#pragma once

#include "app/UnitMenuWindows.h"
#include "core/Types.h"
#include "game/Battlegrounds.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 17 (queue): what MiniMapBattlefieldFrame says about the queues (BattlefieldFrame.lua), as text and
// menu buttons, without drawing (app/BattlefieldWindows draws them; unit tested on their own).

// One queue (GetBattlefieldStatus and its times) as the button's tooltip and menu read it.
struct BattlefieldQueueView {
    game::BattlefieldState state = game::BattlefieldState::None;
    std::string_view name;    // its mapName, " "..instanceID when it has one (the tooltip's)
    std::string_view mapName; // without it (the menu's title)
    u32 averageWaitMs = 0;    // GetBattlefieldEstimatedWaitTime
    u32 waitedMs = 0;         // GetBattlefieldTimeWaited
    u32 expiresMs = 0;        // GetBattlefieldPortExpiration
};
// BattlefieldFrame_UpdateStatus(1)'s tooltip: BATTLEFIELD_IN_QUEUE (QUEUE_TIME_UNAVAILABLE,
// LESS_THAN_ONE_MINUTE or the wait without seconds; the time in the queue), BATTLEFIELD_QUEUE_CONFIRM (the
// port's expiration), BATTLEFIELD_IN_BATTLEFIELD, "\n\n" between the queues and "\n"..RIGHT_CLICK_MESSAGE
// when one is queued or invited. Written into `out` (cut to fit); empty without queues.
std::string_view battlefieldStatusText(std::span<char> out, std::span<const BattlefieldQueueView> queues);
// MiniMapBattlefieldDropDown_Initialize's buttons: per queue queued or invited a spacer after the first, its
// map as a title, then CHANGE_INSTANCE and LEAVE_QUEUE, or ENTER_BATTLE and LEAVE_QUEUE. `actions` / `slots`
// say what each does (None for titles) and for which queue. Returns how many were written.
enum class BattlefieldMenuAction : u8 { None, ChangeInstance, LeaveQueue, EnterBattle };
u32 battlefieldMenu(std::span<const BattlefieldQueueView> queues, std::span<DropDownItemView> items,
                    std::span<BattlefieldMenuAction> actions, std::span<u8> slots);

} // namespace vd
