// Phase 13 (social): the packets of the players' menus and the slash commands against the exact bytes vmangos-core
// 4641790 writes and reads (Server/Packets/Group.cpp RaidTargetUpdate::ReadFromWorldPacket "iconId, then the GUID
// unless 0xFF", RaidTargetUpdateDelta "u8 0, icon, guid", RaidTargetUpdateAll "u8 1, then icon + guid per marked
// icon"; GroupAssistantLeader "guid, flag" (builds after 1.11.2); RaidReadyCheckFromClient "an optional u8";
// Misc.cpp PlayedTime "total, level"; Pet.cpp PetRename "petGuid, name"; Channel.cpp ChannelPassword /
// ChannelModerator... "channelName, playerName"), including every truncation.
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/PlayerCommandMessages.h"
#include "net/SocialMessages.h"

#include <doctest/doctest.h>

#include <functional>
#include <ostream>
#include <vector>

namespace {

using namespace vd;
using namespace vd::net;

using Bytes = std::vector<u8>;

constexpr u64 kHogger = 0xF130000001C5A1B2ull;
constexpr u64 kAeryn = 0x21;

Bytes delta(u8 icon, u64 guid) {
    Bytes out;
    ByteWriter(out).u8v(0).u8v(icon).u64v(guid);
    return out;
}

} // namespace

TEST_CASE("Player commands: MSG_RAID_TARGET_UPDATE from the client") {
    Bytes out;
    writeRaidTargetSet(out, 7, kHogger); // the skull on Hogger
    Bytes expected;
    ByteWriter(expected).u8v(7).u64v(kHogger);
    CHECK(out == expected);
    out.clear();
    writeRaidTargetSet(out, 2, 0); // the diamond taken off: its slot emptied
    expected.clear();
    ByteWriter(expected).u8v(2).u64v(0);
    CHECK(out == expected);
    out.clear();
    writeRaidTargetRequest(out); // the whole list: 0xFF alone
    CHECK(out == Bytes{0xFF});
}

TEST_CASE("Player commands: MSG_RAID_TARGET_UPDATE from the server") {
    RaidTargetUpdate u;
    // RaidTargetUpdateDelta: one icon moved.
    const Bytes one = delta(7, kHogger);
    REQUIRE(parseRaidTargetUpdate(one, u));
    CHECK_FALSE(u.full);
    CHECK(u.count == 1);
    CHECK(u.entries[0].icon == 7);
    CHECK(u.entries[0].guid == kHogger);
    for (usize length = 0; length < one.size(); ++length) {
        CHECK_FALSE(parseRaidTargetUpdate(std::span<const u8>(one.data(), length), u));
    }
    Bytes longer = one;
    longer.push_back(0);
    CHECK_FALSE(parseRaidTargetUpdate(longer, u)); // a delta is exactly one entry
    CHECK_FALSE(parseRaidTargetUpdate(delta(8, kHogger), u)); // icons are 0..7

    // RaidTargetUpdateAll: the marked icons only.
    Bytes all;
    ByteWriter(all).u8v(1).u8v(0).u64v(kAeryn).u8v(7).u64v(kHogger);
    REQUIRE(parseRaidTargetUpdate(all, u));
    CHECK(u.full);
    REQUIRE(u.count == 2);
    CHECK(u.entries[0].icon == 0);
    CHECK(u.entries[0].guid == kAeryn);
    CHECK(u.entries[1].icon == 7);
    CHECK(u.entries[1].guid == kHogger);
    // Nothing marked: the mode byte alone.
    REQUIRE(parseRaidTargetUpdate(Bytes{1}, u));
    CHECK(u.full);
    CHECK(u.count == 0);
    // A truncated entry is refused (cut at an entry's end it is a shorter list, which the format allows).
    for (usize length = 0; length < all.size(); ++length) {
        const bool boundary = length >= 1 && (length - 1) % 9 == 0;
        CHECK(parseRaidTargetUpdate(std::span<const u8>(all.data(), length), u) == boundary);
    }
    CHECK_FALSE(parseRaidTargetUpdate(Bytes{2}, u)); // no third mode
    Bytes nine;
    ByteWriter w(nine);
    w.u8v(1);
    for (u8 i = 0; i < 9; ++i) {
        w.u8v(static_cast<u8>(i % 8)).u64v(kAeryn + i);
    }
    CHECK_FALSE(parseRaidTargetUpdate(nine, u)); // at most one entry per icon
}

TEST_CASE("Player commands: assistants, ready check, time played, pets, channels") {
    Bytes out;
    writeGroupAssistant(out, kAeryn, true);
    Bytes expected;
    ByteWriter(expected).u64v(kAeryn).u8v(1);
    CHECK(out == expected);
    out.clear();
    writeGroupAssistant(out, kAeryn, false);
    CHECK(out.back() == 0);

    out.clear();
    writeReadyCheckAnswer(out, true);
    CHECK(out == Bytes{1});

    Bytes played;
    ByteWriter(played).u32v(3 * 86400 + 4 * 3600 + 5 * 60 + 6).u32v(7200);
    PlayedTime t;
    REQUIRE(parsePlayedTime(played, t));
    CHECK(t.total == 3 * 86400 + 4 * 3600 + 5 * 60 + 6);
    CHECK(t.level == 7200);
    for (usize length = 0; length < played.size(); ++length) {
        CHECK_FALSE(parsePlayedTime(std::span<const u8>(played.data(), length), t));
    }

    out.clear();
    writePetRename(out, 0xF14000000000BEEFull, "Fang");
    expected.clear();
    ByteWriter(expected).u64v(0xF14000000000BEEFull).cstring("Fang");
    CHECK(out == expected);

    out.clear();
    writeChannelCommand(out, "Trade - City", "Galen");
    expected.clear();
    ByteWriter(expected).cstring("Trade - City").cstring("Galen");
    CHECK(out == expected);
    out.clear();
    writeChannelCommand(out, "secretos", ""); // /password secretos: the password taken off
    CHECK(out == Bytes{'s', 'e', 'c', 'r', 'e', 't', 'o', 's', 0, 0});

    // SMSG_INSPECT: the GUID the client reads and ignores.
    Bytes inspect;
    ByteWriter(inspect).u64v(kAeryn);
    u64 guid = 0;
    CHECK(parseGuid(inspect, guid));
    CHECK(guid == kAeryn);
}
