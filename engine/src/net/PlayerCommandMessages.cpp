#include "net/PlayerCommandMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

void writeRaidTargetSet(std::vector<u8>& out, u8 icon, u64 guid) {
    ByteWriter(out).u8v(icon).u64v(guid);
}

void writeRaidTargetRequest(std::vector<u8>& out) {
    ByteWriter(out).u8v(kRaidTargetRequestList);
}

bool parseRaidTargetUpdate(std::span<const u8> payload, RaidTargetUpdate& out) {
    ByteReader r(payload);
    const u8 mode = r.u8v();
    out.full = mode == 1;
    out.count = 0;
    if (!r.ok() || mode > 1) {
        return false;
    }
    if (!out.full) {
        RaidTargetUpdate::Entry& e = out.entries[0];
        e.icon = r.u8v();
        e.guid = r.u64v();
        out.count = 1;
        return r.ok() && e.icon < kRaidTargetIcons && r.atEnd();
    }
    // SendTargetIconList writes the marked icons only, at most one entry per icon.
    while (!r.atEnd()) {
        if (out.count == out.entries.size()) {
            return false;
        }
        RaidTargetUpdate::Entry& e = out.entries[out.count++];
        e.icon = r.u8v();
        e.guid = r.u64v();
        if (!r.ok() || e.icon >= kRaidTargetIcons) {
            return false;
        }
    }
    return r.ok();
}

void writeGroupAssistant(std::vector<u8>& out, u64 member, bool assistant) {
    ByteWriter(out).u64v(member).u8v(assistant ? 1 : 0);
}

void writeReadyCheckAnswer(std::vector<u8>& out, bool ready) {
    ByteWriter(out).u8v(ready ? 1 : 0);
}

bool parseReadyCheck(std::span<const u8> payload, ReadyCheckMessage& out) { // phase 16 (alerts)
    out = ReadyCheckMessage{};
    if (payload.empty()) {
        return true;
    }
    ByteReader r(payload);
    out.answer = true;
    out.member = r.u64v();
    out.state = r.u8v();
    return r.ok() && r.atEnd();
}

bool parsePlayedTime(std::span<const u8> payload, PlayedTime& out) {
    ByteReader r(payload);
    out.total = r.u32v();
    out.level = r.u32v();
    return r.ok();
}

void writePetRename(std::vector<u8>& out, u64 pet, std::string_view name) {
    ByteWriter(out).u64v(pet).cstring(name);
}

void writeChannelCommand(std::vector<u8>& out, std::string_view channel, std::string_view argument) {
    ByteWriter(out).cstring(channel).cstring(argument);
}

} // namespace vd::net
