// Popup packets (parity pass): SMSG_LOGOUT_RESPONSE, SMSG_SUMMON_REQUEST and CMSG_SUMMON_RESPONSE against the
// exact bytes vmangos-core 4641790 writes and reads them with (Server/Packets/Misc.cpp:
// LogoutResponse::AppendBodyTo "buffer << reason << instant" with a uint32 reason and a uint8 instant;
// SummonRequest "summonerGuid, zoneId, autoDeclineDelay"; SummonResponse::ReadFromWorldPacket "recv_data >>
// summonerGuid"), including every truncation.
#include "net/PopupMessages.h"

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

TEST_CASE("Popup messages: SMSG_LOGOUT_RESPONSE") {
    LogoutResponse r;
    // In combat: reason 1, not instant (HandleLogoutRequestOpcode's refusal).
    const Bytes refused = {0x01, 0x00, 0x00, 0x00, 0x00};
    REQUIRE(parseLogoutResponse(refused, r));
    CHECK(r.reason == kLogoutRefusedCombat);
    CHECK_FALSE(r.instant);
    // Resting: reason 0, instant 1.
    REQUIRE(parseLogoutResponse(Bytes{0x00, 0x00, 0x00, 0x00, 0x01}, r));
    CHECK(r.reason == 0);
    CHECK(r.instant);
    // The countdown: reason 0, instant 0.
    REQUIRE(parseLogoutResponse(Bytes{0x00, 0x00, 0x00, 0x00, 0x00}, r));
    CHECK(r.reason == 0);
    CHECK_FALSE(r.instant);
    // Falling: reason 3.
    REQUIRE(parseLogoutResponse(Bytes{0x03, 0x00, 0x00, 0x00, 0x00}, r));
    CHECK(r.reason == kLogoutRefusedFalling);
    checkTruncations(refused, [](std::span<const u8> p) {
        LogoutResponse out;
        return parseLogoutResponse(p, out);
    });
}

TEST_CASE("Popup messages: SMSG_SUMMON_REQUEST and CMSG_SUMMON_RESPONSE") {
    // A player (GUID 0x1234) in Elwynn Forest (zone 12), two minutes (MAX_PLAYER_SUMMON_DELAY *
    // IN_MILLISECONDS).
    const Bytes request = {0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // summonerGuid
                           0x0C, 0x00, 0x00, 0x00,                         // zoneId
                           0xC0, 0xD4, 0x01, 0x00};                        // autoDeclineDelay: 120000
    SummonRequest r;
    REQUIRE(parseSummonRequest(request, r));
    CHECK(r.summoner == 0x1234);
    CHECK(r.zoneId == 12);
    CHECK(r.autoDeclineMs == 120000);
    checkTruncations(request, [](std::span<const u8> p) {
        SummonRequest out;
        return parseSummonRequest(p, out);
    });

    std::vector<u8> response;
    writeSummonResponse(response, 0xF130000000001234ull);
    CHECK(response == Bytes{0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x30, 0xF1});
}
