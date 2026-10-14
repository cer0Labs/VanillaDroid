// Phase 17 (queue): the client's own lines about the battleground queue (game::NoticeFeed's part), worded as
// WoW.exe and BattlefieldFrame.lua word them (game/BattlefieldQueue.h).
#include "core/Text.h"
#include "game/AreaData.h"
#include "game/BattlefieldQueue.h"
#include "game/Gameplay.h"
#include "game/Notices.h"
#include "game/Travel.h"

namespace vd::game {

void NoticeFeed::battlefieldLine(Gameplay& play, const GameEvent& e) {
    const auto line = static_cast<BattlefieldLine>(e.value[0]);
    const u32 place = battlefieldLinePlace(line);
    const NoticeKind kind = place == 1   ? NoticeKind::Info
                            : place == 2 ? NoticeKind::Error
                                         : NoticeKind::System;
    const text::Key key = battlefieldLineKey(line);
    switch (line) {
    case BattlefieldLine::GroupJoined: {
        // 0x4AACC1: Map.dbc's name of the queued map.
        const AreaData* areas = play.travel().areas();
        const MapInfo* map = areas ? areas->map(e.id) : nullptr;
        text::format(push(kind), key, map ? map->name : std::string_view{});
        break;
    }
    case BattlefieldLine::InstanceShutdown:
    case BattlefieldLine::InstanceComplete: {
        // format(..., SecondsToTime(ceil(timer / threshold) * threshold)).
        char time[64];
        const std::string_view when = secondsToTime(time, static_cast<u32>(e.value[1] > 0 ? e.value[1] : 0));
        text::format(push(kind), key, when);
        break;
    }
    default: text::format(push(kind), key); break;
    }
}

} // namespace vd::game
