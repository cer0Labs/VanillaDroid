#include "net/QuestMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

void writePushQuestToParty(std::vector<u8>& out, u32 questId) {
    ByteWriter(out).u32v(questId);
}

bool parseQuestPushResult(std::span<const u8> payload, QuestPushResult& out) {
    ByteReader r(payload);
    out.guid = r.u64v();
    out.result = r.u8v();
    return r.ok();
}

void writeQuestPushResult(std::vector<u8>& out, u64 guid, u8 result) {
    ByteWriter(out).u64v(guid).u8v(result);
}

bool parseGameObjectCustomAnim(std::span<const u8> payload, GameObjectCustomAnim& out) {
    ByteReader r(payload);
    out.guid = r.u64v();
    out.anim = r.u32v();
    return r.ok();
}

} // namespace vd::net
