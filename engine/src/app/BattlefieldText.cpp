#include "app/BattlefieldText.h"

#include "core/Text.h"
#include "game/Travel.h"

#include <algorithm>
#include <cstring>

namespace vd {
namespace {

// Appends `s` to the NUL-terminated text of `out` (`used` so far), cut to fit.
void put(std::span<char> out, usize& used, std::string_view s) {
    if (out.empty() || used + 1 >= out.size()) {
        return;
    }
    const usize n = std::min(s.size(), out.size() - 1 - used);
    std::memcpy(out.data() + used, s.data(), n);
    used += n;
    out[used] = '\0';
}

} // namespace

std::string_view battlefieldStatusText(std::span<char> out, std::span<const BattlefieldQueueView> queues) {
    using game::BattlefieldState;
    usize used = 0;
    if (!out.empty()) {
        out[0] = '\0';
    }
    bool rightClick = false;
    for (const BattlefieldQueueView& q : queues) {
        char line[512];
        std::string_view part;
        switch (q.state) {
        case BattlefieldState::Queued: {
            char wait[64], waited[64];
            const std::string_view waitText = q.averageWaitMs == 0 ? text::get("QUEUE_TIME_UNAVAILABLE")
                                              : q.averageWaitMs < 60000
                                                  ? text::get("LESS_THAN_ONE_MINUTE")
                                                  : game::secondsToTime(wait, q.averageWaitMs / 1000, true);
            part = text::format(line, "BATTLEFIELD_IN_QUEUE", q.name, waitText,
                                game::secondsToTime(waited, q.waitedMs / 1000));
            rightClick = true;
            break;
        }
        case BattlefieldState::Confirm: {
            char left[64];
            part = text::format(line, "BATTLEFIELD_QUEUE_CONFIRM", q.name,
                                game::secondsToTime(left, q.expiresMs / 1000));
            rightClick = true;
            break;
        }
        case BattlefieldState::Active: part = text::format(line, "BATTLEFIELD_IN_BATTLEFIELD", q.name); break;
        default: break; // "none", and "error" ("Should never happen haha")
        }
        if (!part.empty()) {
            put(out, used, used > 0 ? "\n\n" : "");
            put(out, used, part);
        }
    }
    if (used > 0 && rightClick) {
        put(out, used, "\n");
        put(out, used, text::get("RIGHT_CLICK_MESSAGE"));
    }
    return {out.data(), used};
}

u32 battlefieldMenu(std::span<const BattlefieldQueueView> queues, std::span<DropDownItemView> items,
                    std::span<BattlefieldMenuAction> actions, std::span<u8> slots) {
    using game::BattlefieldState;
    u32 count = 0;
    const auto add = [&](std::string_view text, bool title, BattlefieldMenuAction action, u32 slot) {
        if (count < items.size() && count < actions.size() && count < slots.size()) {
            items[count] = DropDownItemView{};
            items[count].text = text;
            items[count].title = title;
            actions[count] = action;
            slots[count] = static_cast<u8>(slot);
            ++count;
        }
    };
    u32 queued = 0;
    for (u32 slot = 0; slot < queues.size(); ++slot) {
        const BattlefieldState state = queues[slot].state;
        if (state != BattlefieldState::Queued && state != BattlefieldState::Confirm) {
            continue;
        }
        if (++queued > 1) {
            add({}, true, BattlefieldMenuAction::None, slot); // the spacer
        }
        add(queues[slot].mapName, true, BattlefieldMenuAction::None, slot);
        if (state == BattlefieldState::Queued) {
            add(text::get("CHANGE_INSTANCE"), false, BattlefieldMenuAction::ChangeInstance, slot);
        } else {
            add(text::get("ENTER_BATTLE"), false, BattlefieldMenuAction::EnterBattle, slot);
        }
        add(text::get("LEAVE_QUEUE"), false, BattlefieldMenuAction::LeaveQueue, slot);
    }
    return count;
}

} // namespace vd
