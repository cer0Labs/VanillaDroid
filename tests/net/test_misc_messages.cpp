// Phase 13 (13.9) packets against the exact bytes vmangos-core 4641790 writes them with: Server/Packets/Misc.cpp
// (ServerMessage "messageType << text", MountResult / DismountResult "result", AiReaction "unitGuid << reaction",
// SetForcedReactions "size, then factionId << reputationRank", FactionAtWarChange "reputationId << flags"),
// GossipDef.cpp PlayerMenu::SendPointOfInterest ("flags, x, y, icon, data, name") and Item.cpp
// Item::SendTimeUpdate ("ObjectGuid << duration"), including every truncation.
#include "net/MiscMessages.h"

#include <doctest/doctest.h>

#include <cstring>
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

void putU32(Bytes& b, u32 v) {
    for (u32 i = 0; i < 4; ++i) {
        b.push_back(static_cast<u8>(v >> (i * 8)));
    }
}
void putF32(Bytes& b, f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    putU32(b, bits);
}
void putU64(Bytes& b, u64 v) {
    putU32(b, static_cast<u32>(v));
    putU32(b, static_cast<u32>(v >> 32));
}
void putString(Bytes& b, std::string_view s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
}

} // namespace

TEST_CASE("Misc messages: SMSG_SERVER_MESSAGE") {
    // World::ShutdownMsg: SERVER_MSG_SHUTDOWN_TIME with secsToTimeString(65).
    Bytes p;
    putU32(p, kServerMessageShutdownTime);
    putString(p, "1 Minute(s) 5 Second(s)");
    ServerMessage m;
    REQUIRE(parseServerMessage(p, m));
    CHECK(m.type == kServerMessageShutdownTime);
    CHECK(m.text == "1 Minute(s) 5 Second(s)");
    // World::ShutdownCancel: an empty text.
    Bytes cancelled;
    putU32(cancelled, kServerMessageShutdownCancelled);
    putString(cancelled, "");
    REQUIRE(parseServerMessage(cancelled, m));
    CHECK(m.type == kServerMessageShutdownCancelled);
    CHECK(m.text.empty());
    checkTruncations(p, [](std::span<const u8> b) {
        ServerMessage out;
        return parseServerMessage(b, out);
    });
}

TEST_CASE("Misc messages: SMSG_MOUNTRESULT and SMSG_DISMOUNTRESULT") {
    u32 result = 0;
    REQUIRE(parseMountResult(Bytes{0x02, 0x00, 0x00, 0x00}, result)); // MOUNTRESULT_ALREADYMOUNTED
    CHECK(result == 2);
    REQUIRE(parseMountResult(Bytes{0x0A, 0x00, 0x00, 0x00}, result));
    CHECK(result == kMountResultOk);
    checkTruncations(Bytes{0x01, 0x00, 0x00, 0x00}, [](std::span<const u8> b) {
        u32 out = 0;
        return parseMountResult(b, out);
    });
}

TEST_CASE("Misc messages: SMSG_AI_REACTION") {
    Bytes p;
    putU64(p, 0xF130000F11000123ull); // a creature's full GUID
    putU32(p, kAiReactionHostile);
    AiReaction m;
    REQUIRE(parseAiReaction(p, m));
    CHECK(m.guid == 0xF130000F11000123ull);
    CHECK(m.reaction == kAiReactionHostile);
    checkTruncations(p, [](std::span<const u8> b) {
        AiReaction out;
        return parseAiReaction(b, out);
    });
}

TEST_CASE("Misc messages: SMSG_GOSSIP_POI") {
    // points_of_interest: the Stormwind bank (every row of vMaNGOS' world database has flags 99 and icon 6, the
    // red flag of POIIcons).
    Bytes p;
    putU32(p, 99);
    putF32(p, -8933.5f);
    putF32(p, 611.25f);
    putU32(p, 6);
    putU32(p, 0);
    putString(p, "Stormwind Bank");
    GossipPoi m;
    REQUIRE(parseGossipPoi(p, m));
    CHECK(m.flags == 99);
    CHECK(m.x == -8933.5f);
    CHECK(m.y == 611.25f);
    CHECK(m.icon == 6);
    CHECK(m.data == 0);
    CHECK(m.name == "Stormwind Bank");
    checkTruncations(p, [](std::span<const u8> b) {
        GossipPoi out;
        return parseGossipPoi(b, out);
    });
    // The client keeps 63 characters of the name.
    Bytes longName;
    for (u32 i = 0; i < 5; ++i) {
        putU32(longName, 0);
    }
    putString(longName, std::string(100, 'a'));
    REQUIRE(parseGossipPoi(longName, m));
    CHECK(m.name.size() == 63);
}

TEST_CASE("Misc messages: SMSG_SET_FORCED_REACTIONS") {
    Bytes p;
    putU32(p, 2);
    putU32(p, 59); // Thorium Brotherhood
    putU32(p, 4);  // REP_FRIENDLY
    putU32(p, 21); // Booty Bay
    putU32(p, 1);  // REP_HOSTILE
    std::vector<ForcedReaction> list;
    REQUIRE(parseForcedReactions(p, list));
    REQUIRE(list.size() == 2);
    CHECK(list[0].faction == 59);
    CHECK(list[0].rank == 4);
    CHECK(list[1].faction == 21);
    CHECK(list[1].rank == 1);
    // None left: an empty list clears them.
    REQUIRE(parseForcedReactions(Bytes{0x00, 0x00, 0x00, 0x00}, list));
    CHECK(list.empty());
    checkTruncations(p, [](std::span<const u8> b) {
        std::vector<ForcedReaction> out;
        return parseForcedReactions(b, out);
    });
    // A count the packet cannot hold.
    CHECK_FALSE(parseForcedReactions(Bytes{0xFF, 0xFF, 0xFF, 0xFF}, list));
}

TEST_CASE("Misc messages: SMSG_SET_FACTION_ATWAR") {
    FactionAtWar m;
    REQUIRE(parseFactionAtWar(Bytes{0x15, 0x00, 0x00, 0x00, 0x02}, m));
    CHECK(m.listId == 21);
    CHECK(m.atWar);
    REQUIRE(parseFactionAtWar(Bytes{0x15, 0x00, 0x00, 0x00, 0x00}, m));
    CHECK_FALSE(m.atWar);
    checkTruncations(Bytes{0x15, 0x00, 0x00, 0x00, 0x02}, [](std::span<const u8> b) {
        FactionAtWar out;
        return parseFactionAtWar(b, out);
    });
}

TEST_CASE("Misc messages: SMSG_ITEM_TIME_UPDATE") {
    Bytes p;
    putU64(p, 0x4000000000001234ull); // an item GUID
    putU32(p, 3540);
    ItemTimeUpdate m;
    REQUIRE(parseItemTimeUpdate(p, m));
    CHECK(m.item == 0x4000000000001234ull);
    CHECK(m.seconds == 3540);
    checkTruncations(p, [](std::span<const u8> b) {
        ItemTimeUpdate out;
        return parseItemTimeUpdate(b, out);
    });
}
