#include "net/MeetingStoneMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

void writeMeetingStoneJoin(std::vector<u8>& out, u64 stone) {
    ByteWriter(out).u64v(stone);
}

bool parseMeetingStoneSetQueue(std::span<const u8> payload, MeetingStoneQueue& out) {
    ByteReader r(payload);
    out.areaId = r.u32v();
    out.status = r.u8v();
    return r.ok();
}

bool parseMeetingStoneJoinFailed(std::span<const u8> payload, u8& reason) {
    ByteReader r(payload);
    reason = r.u8v();
    return r.ok();
}

bool parseMeetingStoneMemberAdded(std::span<const u8> payload, u64& player) {
    ByteReader r(payload);
    player = r.u64v();
    return r.ok();
}

} // namespace vd::net
