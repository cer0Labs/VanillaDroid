#include "net/CharacterMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

void writeInspectHonorRequest(std::vector<u8>& out, u64 player) {
    ByteWriter(out).u64v(player);
}

bool parseInspectHonorStats(std::span<const u8> payload, InspectHonorStats& out) {
    ByteReader r(payload);
    out.player = r.u64v();
    out.highestRank = r.u8v();
    out.todayHonorable = r.u16v();
    out.todayDishonorable = r.u16v();
    out.yesterdayHonorable = r.u16v();
    out.yesterdayUnused = r.u16v();
    out.lastWeekHonorable = r.u16v();
    out.lastWeekUnused = r.u16v();
    out.thisWeekHonorable = r.u16v();
    out.thisWeekUnused = r.u16v();
    out.lifetimeHonorable = r.u32v();
    out.lifetimeDishonorable = r.u32v();
    out.yesterdayHonor = r.u32v();
    out.lastWeekHonor = r.u32v();
    out.thisWeekHonor = r.u32v();
    out.lastWeekStanding = r.u32v();
    out.rankProgress = r.u8v();
    return r.ok();
}

void writeSetWatchedFaction(std::vector<u8>& out, i32 listId) {
    ByteWriter(out).u32v(static_cast<u32>(listId));
}

void writeSetFactionInactive(std::vector<u8>& out, u32 listId, bool inactive) {
    ByteWriter(out).u32v(listId).u8v(inactive ? 1 : 0);
}

void writeUnlearnSkill(std::vector<u8>& out, u32 skillId) {
    ByteWriter(out).u32v(skillId);
}

} // namespace vd::net
