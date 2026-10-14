// Phase 15 (character): the character window's packets against the exact bytes vmangos-core 4641790 writes
// and reads (Server/Packets/Misc.cpp InspectHonorStats::ReadFromWorldPacket "guid",
// InspectHonorStatsResponse::AppendBodyTo "playerGuid, highestRank, sessionKills, yesterdayHK, unknownOld1,
// lastWeekHK, unknownOld2, thisWeekHK, unknownOld3, lifetimeHK, lifetimeDHK, yesterdayHonor, lastWeekHonor,
// thisWeekHonor, lastWeekRank, rankBar" (builds >= 1.6.1); SetWatchedFaction "int32 repId";
// SetFactionInactive "replistid, uint8 inactive"; Skill.cpp UnlearnSkill "skillId"), including every
// truncation.
#include "net/ByteBuffer.h"
#include "net/CharacterMessages.h"

#include <doctest/doctest.h>

#include <vector>

namespace {

using namespace vd;
using namespace vd::net;

using Bytes = std::vector<u8>;

constexpr u64 kAeryn = 0x21;

// InspectHonorStatsResponse as vMaNGOS writes it: the session kills as one uint32 (honorable in its low
// half).
Bytes honorResponse() {
    Bytes out;
    ByteWriter w(out);
    w.u64v(kAeryn).u8v(9);             // First Sergeant / Sergeant Major at best
    w.u32v(3u | (1u << 16));           // today: 3 honorable, 1 dishonorable
    w.u16v(12).u16v(0);                // yesterday's honorable kills, unknownOld1
    w.u16v(140).u16v(0);               // last week's, unknownOld2
    w.u16v(41).u16v(0);                // this week's, unknownOld3
    w.u32v(2000).u32v(7);              // lifetime honorable and dishonorable kills
    w.u32v(310).u32v(4520).u32v(1200); // honor: yesterday, last week, this week
    w.u32v(87);                        // last week's standing
    w.u8v(128);                        // the rank bar, half way
    return out;
}

} // namespace

TEST_CASE("Character: MSG_INSPECT_HONOR_STATS request") {
    Bytes out;
    writeInspectHonorRequest(out, kAeryn);
    Bytes expected;
    ByteWriter(expected).u64v(kAeryn);
    CHECK(out == expected);
}

TEST_CASE("Character: MSG_INSPECT_HONOR_STATS answer") {
    const Bytes body = honorResponse();
    CHECK(body.size() == 50);
    InspectHonorStats m;
    REQUIRE(parseInspectHonorStats(body, m));
    CHECK(m.player == kAeryn);
    CHECK(m.highestRank == 9);
    CHECK(m.todayHonorable == 3);
    CHECK(m.todayDishonorable == 1);
    CHECK(m.yesterdayHonorable == 12);
    CHECK(m.lastWeekHonorable == 140);
    CHECK(m.thisWeekHonorable == 41);
    CHECK(m.lifetimeHonorable == 2000);
    CHECK(m.lifetimeDishonorable == 7);
    CHECK(m.yesterdayHonor == 310);
    CHECK(m.lastWeekHonor == 4520);
    CHECK(m.thisWeekHonor == 1200);
    CHECK(m.lastWeekStanding == 87);
    CHECK(m.rankProgress == 128);
    // Truncated anywhere: refused.
    for (usize size = 0; size < body.size(); ++size) {
        CHECK_FALSE(parseInspectHonorStats(std::span<const u8>(body.data(), size), m));
    }
}

TEST_CASE("Character: reputation and skill requests") {
    Bytes out;
    writeSetWatchedFaction(out, 21); // Darnassus' list id
    CHECK(out == Bytes{21, 0, 0, 0});
    out.clear();
    writeSetWatchedFaction(out, -1); // none: the int32 -1
    CHECK(out == Bytes{0xFF, 0xFF, 0xFF, 0xFF});
    out.clear();
    writeSetFactionInactive(out, 13, true); // Argent Dawn to the Inactive header
    CHECK(out == Bytes{13, 0, 0, 0, 1});
    out.clear();
    writeSetFactionInactive(out, 13, false);
    CHECK(out == Bytes{13, 0, 0, 0, 0});
    out.clear();
    writeUnlearnSkill(out, 186); // Mining
    CHECK(out == Bytes{186, 0, 0, 0});
}
