#include "net/BattlefieldListMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

void writeBattlemasterHello(std::vector<u8>& out, u64 battlemaster) {
    ByteWriter(out).u64v(battlemaster);
}

bool parseBattlefieldList(std::span<const u8> payload, BattlefieldList& out) {
    ByteReader r(payload);
    out.battlemaster = r.u64v();
    out.mapId = r.u32v();
    out.bracketId = r.u8v();
    const u32 count = r.u32v();
    out.instanceIds.clear();
    if (!r.ok() || r.remaining() / 4 < count) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        out.instanceIds.push_back(r.u32v());
    }
    return r.ok();
}

void writeBattlemasterJoin(std::vector<u8>& out, u64 battlemaster, u32 mapId, u32 instanceId, bool asGroup) {
    ByteWriter(out).u64v(battlemaster).u32v(mapId).u32v(instanceId).u8v(asGroup ? 1 : 0);
}

void writeBattlefieldJoin(std::vector<u8>& out, u32 mapId, u32 instanceId, bool asGroup) {
    ByteWriter(out).u32v(mapId).u32v(instanceId).u8v(asGroup ? 1 : 0);
}

void writeBattlefieldListRequest(std::vector<u8>& out, u32 mapId) {
    ByteWriter(out).u32v(mapId);
}

bool parseGroupJoinedBattleground(std::span<const u8> payload, i32& result) {
    ByteReader r(payload);
    result = r.i32v();
    return r.ok();
}

} // namespace vd::net
