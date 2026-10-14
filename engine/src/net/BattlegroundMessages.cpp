#include "net/BattlegroundMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

bool parseBattlefieldStatus(std::span<const u8> payload, BattlefieldStatus& out) {
    ByteReader r(payload);
    out = BattlefieldStatus{};
    out.queueSlot = r.u32v();
    out.mapId = r.u32v();
    if (out.mapId == 0) { // BattlefieldStatusEmpty
        return r.ok();
    }
    out.bracketId = r.u8v();
    out.instanceId = r.u32v();
    out.status = r.u32v();
    out.time1 = r.u32v();
    if (out.status == kBattlefieldStatusWaitQueue || out.status == kBattlefieldStatusInProgress) {
        out.time2 = r.u32v();
    }
    return r.ok();
}

void writeBattlefieldPort(std::vector<u8>& out, u32 mapId, bool enter) {
    ByteWriter(out).u32v(mapId).u8v(enter ? 1 : 0);
}

void writeLeaveBattlefield(std::vector<u8>& out, u32 mapId) {
    ByteWriter(out).u32v(mapId);
}

bool parseInitWorldStates(std::span<const u8> payload, InitWorldStates& out) {
    ByteReader r(payload);
    out.mapId = r.u32v();
    out.zoneId = r.u32v();
    const u16 count = r.u16v();
    out.states.clear();
    if (!r.ok() || r.remaining() < usize{count} * 8) {
        return false;
    }
    for (u16 i = 0; i < count; ++i) {
        WorldStatePair s;
        s.id = r.u32v();
        s.value = r.u32v();
        if (s.id != 0) { // the terminator
            out.states.push_back(s);
        }
    }
    return r.ok();
}

bool parseUpdateWorldState(std::span<const u8> payload, WorldStatePair& out) {
    ByteReader r(payload);
    out.id = r.u32v();
    out.value = r.u32v();
    return r.ok();
}

} // namespace vd::net
