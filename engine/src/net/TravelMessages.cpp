#include "net/TravelMessages.h"

namespace vd::net {

const char* taxiReplyName(u32 reply) {
    static constexpr const char* kNames[] = {"OK",
                                             "UNSPECIFIED_SERVER_ERROR",
                                             "NO_SUCH_PATH",
                                             "NOT_ENOUGH_MONEY",
                                             "TOO_FAR_AWAY",
                                             "NO_VENDOR_NEARBY",
                                             "NOT_VISITED",
                                             "PLAYER_BUSY",
                                             "PLAYER_ALREADY_MOUNTED",
                                             "PLAYER_SHAPESHIFTED",
                                             "PLAYER_MOVING",
                                             "SAME_NODE",
                                             "NOT_STANDING"};
    return reply < std::size(kNames) ? kNames[reply] : "UNKNOWN";
}

bool parseExplorationExperience(std::span<const u8> payload, ExplorationExperience& out) {
    ByteReader r(payload);
    out.areaId = r.u32v();
    out.experience = r.u32v();
    return r.ok();
}

bool parseShowTaxiNodes(std::span<const u8> payload, ShowTaxiNodes& out) {
    ByteReader r(payload);
    out.showUi = r.u32v();
    out.flightMaster = r.u64v();
    out.currentNode = r.u32v();
    for (u32& mask : out.knownNodes) {
        mask = r.u32v();
    }
    return r.ok();
}

bool parseTaxiNodeStatus(std::span<const u8> payload, u64& guid, bool& known) {
    ByteReader r(payload);
    guid = r.u64v();
    known = r.u8v() != 0;
    return r.ok();
}

bool parseActivateTaxiReply(std::span<const u8> payload, u32& reply) {
    ByteReader r(payload);
    reply = r.u32v();
    return r.ok();
}

bool parseBindPointUpdate(std::span<const u8> payload, BindPointUpdate& out) {
    ByteReader r(payload);
    out.x = r.f32v();
    out.y = r.f32v();
    out.z = r.f32v();
    out.map = r.u32v();
    out.areaId = r.u32v();
    return r.ok();
}

bool parsePlayerBound(std::span<const u8> payload, u64& binder, u32& areaId) {
    ByteReader r(payload);
    binder = r.u64v();
    areaId = r.u32v();
    return r.ok();
}

bool parseTransferPending(std::span<const u8> payload, TransferPending& out) {
    ByteReader r(payload);
    out.map = r.u32v();
    out.onTransport = r.remaining() >= 8;
    if (out.onTransport) {
        out.transportEntry = r.u32v();
        out.oldMap = r.u32v();
    } else {
        out.transportEntry = out.oldMap = 0;
    }
    return r.ok();
}

bool parseTransferAborted(std::span<const u8> payload, u8& reason) {
    ByteReader r(payload);
    reason = r.u8v();
    return r.ok();
}

bool parseRaidInstanceMessage(std::span<const u8> payload, RaidInstanceMessage& out) {
    ByteReader r(payload);
    out.type = r.u32v();
    out.map = r.u32v();
    out.secondsLeft = r.u32v();
    return r.ok();
}

bool parseInstanceResetFailed(std::span<const u8> payload, u32& reason, u32& map) {
    ByteReader r(payload);
    reason = r.u32v();
    map = r.u32v();
    return r.ok();
}

bool parseRaidInstanceInfo(std::span<const u8> payload, std::vector<SavedInstanceInfo>& out) {
    ByteReader r(payload);
    const u32 count = r.u32v();
    out.clear();
    if (!r.ok() || count > r.remaining() / 12) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        SavedInstanceInfo& s = out.emplace_back();
        s.map = r.u32v();
        s.secondsLeft = r.u32v();
        s.instanceId = r.u32v();
    }
    return r.ok();
}

bool parseRaidGroupOnly(std::span<const u8> payload, u32& timerMs, u32& error) {
    ByteReader r(payload);
    timerMs = r.u32v();
    error = r.u32v();
    return r.ok();
}

void writeAreaTrigger(std::vector<u8>& out, u32 triggerId) {
    ByteWriter(out).u32v(triggerId);
}

void writeActivateTaxi(std::vector<u8>& out, u64 flightMaster, u32 from, u32 to) {
    ByteWriter(out).u64v(flightMaster).u32v(from).u32v(to);
}

void writeActivateTaxiExpress(std::vector<u8>& out, u64 flightMaster, u32 totalCost,
                              std::span<const u32> nodes) {
    ByteWriter w(out);
    w.u64v(flightMaster).u32v(totalCost).u32v(static_cast<u32>(nodes.size()));
    for (const u32 node : nodes) {
        w.u32v(node);
    }
}

void writeMoveSplineDone(std::vector<u8>& out, const MovementInfo& info, u32 splineId) {
    ByteWriter w(out);
    writeMovementInfo(w, info);
    w.u32v(splineId).f32v(0.0f);
}

} // namespace vd::net
