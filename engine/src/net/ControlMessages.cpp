#include "net/ControlMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

bool parseClientControlUpdate(std::span<const u8> payload, ClientControlUpdate& out) {
    ByteReader r(payload);
    out.guid = r.packedGuid();
    out.allowMove = r.u8v() != 0;
    return r.ok();
}

bool parseMoveTimeSkipped(std::span<const u8> payload, MoveTimeSkipped& out) {
    ByteReader r(payload);
    out.guid = r.packedGuid();
    out.lagMs = r.u32v();
    return r.ok();
}

void writeMoveTimeSkipped(std::vector<u8>& out, u64 mover, u32 lagMs) {
    ByteWriter(out).u64v(mover).u32v(lagMs);
}

void writeMoveNotActiveMover(std::vector<u8>& out, u64 oldMover, const MovementInfo& info) {
    ByteWriter w(out);
    w.u64v(oldMover);
    writeMovementInfo(w, info);
}

void writeSetActiveMover(std::vector<u8>& out, u64 mover) {
    ByteWriter(out).u64v(mover);
}

void writeFarSight(std::vector<u8>& out, bool viewing) {
    ByteWriter(out).u8v(viewing ? 1 : 0);
}

} // namespace vd::net
