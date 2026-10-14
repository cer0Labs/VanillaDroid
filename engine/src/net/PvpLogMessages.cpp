#include "net/PvpLogMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

bool parsePvpLogData(std::span<const u8> payload, PvpLogData& out) {
    ByteReader r(payload);
    out.ended = r.u8v() != 0;
    out.winner = out.ended ? r.u8v() : 0;
    out.count = r.u32v();
    // Each line is at least its GUID and 6 u32: a count that cannot fit is malformed before reading on.
    if (!r.ok() || out.count > kPvpLogMaxScores || r.remaining() < usize{out.count} * 32) {
        out.count = 0;
        return false;
    }
    for (u32 i = 0; i < out.count; ++i) {
        PvpLogScore& s = out.scores[i];
        s.guid = r.u64v();
        s.rank = r.u32v();
        s.killingBlows = r.u32v();
        s.honorableKills = r.u32v();
        s.deaths = r.u32v();
        s.bonusHonor = r.u32v();
        s.extraCount = r.u32v();
        s.extra = {};
        if (!r.ok() || r.remaining() < usize{s.extraCount} * 4) {
            out.count = 0;
            return false;
        }
        for (u32 j = 0; j < s.extraCount; ++j) {
            const i32 value = r.i32v();
            if (j < kPvpLogExtraFields) {
                s.extra[j] = value;
            }
        }
    }
    if (!r.ok()) {
        out.count = 0;
        return false;
    }
    return true;
}

bool parseBattlegroundPositions(std::span<const u8> payload, BattlegroundPositions& out) {
    ByteReader r(payload);
    out.count = r.u32v();
    out.hasCarrier = false;
    out.carrier = {};
    if (!r.ok() || out.count > kBattlegroundMaxPositions || r.remaining() < usize{out.count} * 16 + 1) {
        out.count = 0;
        return false;
    }
    for (u32 i = 0; i < out.count; ++i) {
        BattlegroundPosition& p = out.players[i];
        p.guid = r.u64v();
        p.x = r.f32v();
        p.y = r.f32v();
    }
    out.hasCarrier = r.u8v() != 0;
    if (out.hasCarrier) {
        out.carrier.guid = r.u64v();
        out.carrier.x = r.f32v();
        out.carrier.y = r.f32v();
    }
    if (!r.ok()) {
        out.count = 0;
        out.hasCarrier = false;
        return false;
    }
    return true;
}

bool parseBattlegroundPlayer(std::span<const u8> payload, u64& player) {
    ByteReader r(payload);
    player = r.u64v();
    return r.ok();
}

} // namespace vd::net
