#include "net/MiscMessages.h"

#include "net/ByteBuffer.h"

#include <cmath>

namespace vd::net {

namespace {
constexpr usize kGossipPoiNameMax = 64; // the client reads at most 0x40 bytes of it
constexpr u32 kFactionFlagAtWar = 0x02; // FACTION_FLAG_AT_WAR
} // namespace

bool parseServerMessage(std::span<const u8> payload, ServerMessage& out) {
    ByteReader r(payload);
    out.type = r.u32v();
    out.text.assign(r.cstring());
    return r.ok();
}

bool parseMountResult(std::span<const u8> payload, u32& result) {
    ByteReader r(payload);
    result = r.u32v();
    return r.ok();
}

bool parseAiReaction(std::span<const u8> payload, AiReaction& out) {
    ByteReader r(payload);
    out.guid = r.u64v();
    out.reaction = r.u32v();
    return r.ok();
}

bool parseGossipPoi(std::span<const u8> payload, GossipPoi& out) {
    ByteReader r(payload);
    out.flags = r.u32v();
    out.x = r.f32v();
    out.y = r.f32v();
    out.icon = r.u32v();
    out.data = r.u32v();
    const std::string_view name = r.cstring();
    out.name.assign(name.substr(0, kGossipPoiNameMax - 1));
    if (r.ok() && (!std::isfinite(out.x) || !std::isfinite(out.y))) {
        r.fail();
    }
    return r.ok();
}

bool parseForcedReactions(std::span<const u8> payload, std::vector<ForcedReaction>& out) {
    ByteReader r(payload);
    const u32 count = r.u32v();
    out.clear();
    if (!r.ok() || r.remaining() < static_cast<usize>(count) * 8) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        ForcedReaction& f = out.emplace_back();
        f.faction = r.u32v();
        f.rank = r.u32v();
    }
    return r.ok();
}

bool parseFactionAtWar(std::span<const u8> payload, FactionAtWar& out) {
    ByteReader r(payload);
    out.listId = r.u32v();
    out.atWar = (r.u8v() & kFactionFlagAtWar) != 0;
    return r.ok();
}

bool parseItemTimeUpdate(std::span<const u8> payload, ItemTimeUpdate& out) {
    ByteReader r(payload);
    out.item = r.u64v();
    out.seconds = r.u32v();
    return r.ok();
}

} // namespace vd::net
