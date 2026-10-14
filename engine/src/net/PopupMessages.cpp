#include "net/PopupMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

bool parseLogoutResponse(std::span<const u8> payload, LogoutResponse& out) {
    ByteReader r(payload);
    out.reason = r.u32v();
    out.instant = r.u8v() != 0;
    return r.ok();
}

bool parseSummonRequest(std::span<const u8> payload, SummonRequest& out) {
    ByteReader r(payload);
    out.summoner = r.u64v();
    out.zoneId = r.u32v();
    out.autoDeclineMs = r.u32v();
    return r.ok();
}

void writeSummonResponse(std::vector<u8>& out, u64 summoner) {
    ByteWriter(out).u64v(summoner);
}

} // namespace vd::net
