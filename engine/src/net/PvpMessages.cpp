#include "net/PvpMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

bool parseAreaSpiritHealerTime(std::span<const u8> payload, AreaSpiritHealerTime& out) {
    ByteReader r(payload);
    out.healer = r.u64v();
    out.timeMs = r.u32v();
    return r.ok();
}

void writeAreaSpiritHealerQuery(std::vector<u8>& out, u64 healer) {
    ByteWriter(out).u64v(healer);
}

void writeAreaSpiritHealerQueue(std::vector<u8>& out, u64 healer) {
    ByteWriter(out).u64v(healer);
}

bool parsePlayerSkinned(std::span<const u8> payload, PlayerSkinned& out) {
    ByteReader r(payload);
    out.freeRepop = r.u8v() != 0;
    return r.ok();
}

bool parsePvpCredit(std::span<const u8> payload, PvpCredit& out) {
    ByteReader r(payload);
    out.honor = r.i32v();
    out.victim = r.u64v();
    out.rank = r.i32v();
    return r.ok();
}

bool parseZoneUnderAttack(std::span<const u8> payload, ZoneUnderAttack& out) {
    ByteReader r(payload);
    out.areaId = r.u32v();
    return r.ok();
}

bool parseDefenseMessage(std::span<const u8> payload, DefenseMessage& out) {
    ByteReader r(payload);
    out.zoneId = r.u32v();
    const u32 length = r.u32v(); // with the NUL
    if (r.failed() || length == 0 || length > r.remaining()) {
        return false;
    }
    const std::span<const u8> bytes = r.view(length);
    if (bytes.back() != 0) {
        return false;
    }
    out.text = {reinterpret_cast<const char*>(bytes.data()), length - 1};
    return r.ok();
}

} // namespace vd::net
