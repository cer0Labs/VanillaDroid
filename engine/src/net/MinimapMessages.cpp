#include "net/MinimapMessages.h"

#include "net/ByteBuffer.h"

#include <cmath>

namespace vd::net {

bool parseMinimapPing(std::span<const u8> payload, MinimapPing& out) {
    ByteReader r(payload);
    out.sender = r.u64v();
    out.x = r.f32v();
    out.y = r.f32v();
    return r.ok() && std::isfinite(out.x) && std::isfinite(out.y);
}

void writeMinimapPing(std::vector<u8>& out, f32 x, f32 y) {
    ByteWriter(out).f32v(x).f32v(y);
}

} // namespace vd::net
