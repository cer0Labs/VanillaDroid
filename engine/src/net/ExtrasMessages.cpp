#include "net/ExtrasMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

bool parseMountSpecialAnim(std::span<const u8> payload, u64& unit) {
    ByteReader r(payload);
    unit = r.u64v();
    return r.ok();
}

bool parsePetActionSound(std::span<const u8> payload, PetActionSound& out) {
    ByteReader r(payload);
    out.pet = r.u64v();
    out.talk = r.u32v();
    return r.ok();
}

bool parseInvalidatePlayer(std::span<const u8> payload, u64& player) {
    ByteReader r(payload);
    player = r.u64v();
    return r.ok();
}

bool parseSetRestStart(std::span<const u8> payload, u32& time) {
    ByteReader r(payload);
    time = r.u32v();
    return r.ok();
}

void writeItemNameQuery(std::vector<u8>& out, u32 entry) {
    ByteWriter(out).u32v(entry).u64v(0);
}

bool parseItemNameQueryResponse(std::span<const u8> payload, ItemNameQueryResponse& out) {
    ByteReader r(payload);
    out.entry = r.u32v();
    out.name = r.cstring();
    return r.ok();
}

} // namespace vd::net
