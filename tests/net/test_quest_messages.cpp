// Phase 13 (quests) packets against the exact bytes vmangos-core 4641790 reads and writes them with
// (Server/Packets/Quest.cpp: PushQuestToParty::ReadFromWorldPacket "recv_data >> questId"; QuestPushResult
// "recv_data >> guid; recv_data >> msg" with a uint8 msg; QuestPushResultResponse::AppendBodyTo "buffer <<
// senderGuid; buffer << msg"; Objects/GameObject.cpp SendGameObjectCustomAnim "data << GetObjectGuid(); data
// << uint32(animId)"), including every truncation.
#include "net/GameMessages.h"
#include "net/QuestMessages.h"

#include <doctest/doctest.h>

#include <functional>
#include <ostream>
#include <vector>

namespace {

using namespace vd;
using namespace vd::net;

using Bytes = std::vector<u8>;

void checkTruncations(const Bytes& full, const std::function<bool(std::span<const u8>)>& parse) {
    for (usize length = 0; length < full.size(); ++length) {
        INFO("truncated to " << length << " of " << full.size() << " bytes");
        CHECK_FALSE(parse(std::span<const u8>(full.data(), length)));
    }
}

} // namespace

TEST_CASE("Quest messages: CMSG_PUSHQUESTTOPARTY") {
    // Quest 176 (Wanted: "Hogger"): a uint32.
    Bytes body;
    writePushQuestToParty(body, 176);
    CHECK(body == Bytes{0xB0, 0x00, 0x00, 0x00});
}

TEST_CASE("Quest messages: MSG_QUEST_PUSH_RESULT both ways") {
    // The server about party member 0x1234: "is already on that quest" (QUEST_PARTY_MSG_HAVE_QUEST = 7).
    const Bytes fromServer = {0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // senderGuid (the member)
                              0x07};                                          // msg
    QuestPushResult r;
    REQUIRE(parseQuestPushResult(fromServer, r));
    CHECK(r.guid == 0x1234);
    CHECK(r.result == kQuestPushOnQuest);
    checkTruncations(fromServer, [](std::span<const u8> p) {
        QuestPushResult out;
        return parseQuestPushResult(p, out);
    });
    // The values the client's lines are picked by (QuestShareMessages).
    CHECK(kQuestPushSharing == 0);
    CHECK(kQuestPushCantTake == 1);
    CHECK(kQuestPushAccepted == 2);
    CHECK(kQuestPushDeclined == 3);
    CHECK(kQuestPushTooFar == 4);
    CHECK(kQuestPushBusy == 5);
    CHECK(kQuestPushLogFull == 6);
    CHECK(kQuestPushAlreadyDone == 8);

    // The member declining the sharer's quest: the sharer's GUID and QUEST_PARTY_MSG_DECLINE_QUEST.
    Bytes decline;
    writeQuestPushResult(decline, 0x0000000000005678ull, kQuestPushDeclined);
    CHECK(decline == Bytes{0x78, 0x56, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03});
}

TEST_CASE("Quest messages: SMSG_GAMEOBJECT_CUSTOM_ANIM and the GUID-only object packets") {
    // The fishing bobber (HIGHGUID_GAMEOBJECT 0xF110) biting: animation 0.
    const Bytes bite = {0x21, 0x43, 0x00, 0x00, 0x00, 0x00, 0x10, 0xF1, // GetObjectGuid()
                        0x00, 0x00, 0x00, 0x00};                        // uint32(animId)
    GameObjectCustomAnim m;
    REQUIRE(parseGameObjectCustomAnim(bite, m));
    CHECK(m.guid == 0xF110000000004321ull);
    CHECK(m.anim == 0);
    const Bytes custom2 = {0x21, 0x43, 0x00, 0x00, 0x00, 0x00, 0x10, 0xF1, 0x02, 0x00, 0x00, 0x00};
    REQUIRE(parseGameObjectCustomAnim(custom2, m));
    CHECK(m.anim == 2);
    checkTruncations(bite, [](std::span<const u8> p) {
        GameObjectCustomAnim out;
        return parseGameObjectCustomAnim(p, out);
    });
    // SPAWN_ANIM / DESPAWN_ANIM / RESET_STATE / PAGETEXT: the full GUID alone.
    const Bytes guid = {0x21, 0x43, 0x00, 0x00, 0x00, 0x00, 0x10, 0xF1};
    u64 g = 0;
    REQUIRE(parseGuid(guid, g));
    CHECK(g == 0xF110000000004321ull);
    checkTruncations(guid, [](std::span<const u8> p) {
        u64 out = 0;
        return parseGuid(p, out);
    });
}
