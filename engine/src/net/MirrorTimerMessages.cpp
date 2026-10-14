#include "net/MirrorTimerMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

bool parseMirrorTimerStart(std::span<const u8> payload, MirrorTimerStart& out) {
    ByteReader r(payload);
    out.type = r.u32v();
    out.remainingMs = r.u32v();
    out.durationMs = r.u32v();
    out.scale = r.i32v();
    out.paused = r.u8v() != 0; // ByteBuffer << bool: one byte
    out.spellId = r.u32v();
    return r.ok();
}

bool parseMirrorTimerPause(std::span<const u8> payload, MirrorTimerPause& out) {
    ByteReader r(payload);
    out.type = r.u32v();
    out.paused = r.u8v() != 0;
    return r.ok();
}

bool parseMirrorTimerStop(std::span<const u8> payload, u32& type) {
    ByteReader r(payload);
    type = r.u32v();
    return r.ok();
}

} // namespace vd::net
