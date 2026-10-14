// Phase 18 (p3): the tutorial flags and the account data byte for byte as vmangos-core 4641790 writes and reads them
// (WorldSession::SendTutorialsData, Packets/Misc TutorialFlag, SendAccountDataTimes, UpdateAccountData,
// RequestAccountData, UpdateAccountDataResponse), truncated packets rejected; MD5 against RFC 1321's vectors.
#include "net/AccountDataMessages.h"
#include "net/ByteBuffer.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

using namespace vd;
using Bytes = std::vector<u8>;

template <typename F> void checkTruncations(const Bytes& payload, F&& parse) {
    for (usize n = 0; n < payload.size(); ++n) {
        CHECK_FALSE(parse(std::span<const u8>(payload.data(), n)));
    }
}

std::string hex(const net::Md5& digest) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    for (const u8 b : digest) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 15]);
    }
    return out;
}

u32 adler32(std::string_view text) {
    u32 a = 1, b = 0;
    for (const char c : text) {
        a = (a + static_cast<u8>(c)) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

// A zlib stream with one stored deflate block (RFC 1950 / 1951), as any zlib may write it: what the server's
// Compression::ZLib::Compress output looks like to an inflater.
Bytes storedZlib(std::string_view text) {
    Bytes out = {0x78, 0x01, 0x01};
    const u16 len = static_cast<u16>(text.size());
    out.push_back(static_cast<u8>(len));
    out.push_back(static_cast<u8>(len >> 8));
    out.push_back(static_cast<u8>(~len));
    out.push_back(static_cast<u8>(~len >> 8));
    out.insert(out.end(), text.begin(), text.end());
    const u32 adler = adler32(text);
    for (u32 i = 0; i < 4; ++i) {
        out.push_back(static_cast<u8>(adler >> (24 - 8 * i))); // big-endian
    }
    return out;
}

} // namespace

TEST_CASE("Account data: MD5 as RFC 1321") {
    CHECK(hex(net::md5(std::string_view(""))) == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(hex(net::md5(std::string_view("a"))) == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(hex(net::md5(std::string_view("abc"))) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(hex(net::md5(std::string_view("message digest"))) == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(hex(net::md5(std::string_view("abcdefghijklmnopqrstuvwxyz"))) == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(hex(net::md5(std::string_view(
              "12345678901234567890123456789012345678901234567890123456789012345678901234567890"))) ==
          "57edf4a22be3c955ac49da2e2107b67a");
    // Around the block size: from 56 bytes the padding spills into a second block.
    CHECK(hex(net::md5(std::string_view(std::string(55, 'x')))) == "04364420e25c512fd958a70738aa8f72");
    CHECK(hex(net::md5(std::string_view(std::string(56, 'x')))) == "668a72d5ba17f08e62dabcafad6db14b");
    CHECK(hex(net::md5(std::string_view(std::string(63, 'x')))) == "7dc2ca208106a2f703567bdff99d8981");
    CHECK(hex(net::md5(std::string_view(std::string(64, 'x')))) == "c1bb4f81d892b2d57947682aeb252456");
    CHECK(hex(net::md5(std::string_view(std::string(65, 'x')))) == "1bc932052302d074bdec39795fe00cf6");
    CHECK(hex(net::md5(std::string_view(std::string(128, 'x')))) == "d69cb61a6ee87200676eb0d4b90edbcb");
    CHECK(net::md5IsZero(net::Md5{}));
    CHECK_FALSE(net::md5IsZero(net::md5(std::string_view(""))));
}

TEST_CASE("Account data: SMSG_TUTORIAL_FLAGS and CMSG_TUTORIAL_FLAG byte for byte") {
    // SendTutorialsData: the eight u32 words, 32 bytes.
    Bytes flags;
    net::ByteWriter w(flags);
    for (u32 i = 0; i < 8; ++i) {
        w.u32v(i == 0 ? 0x00040003u : i == 1 ? 0x80000000u : 0u);
    }
    CHECK(flags.size() == 32);
    net::TutorialWords words{};
    REQUIRE(net::parseTutorialFlags(flags, words));
    CHECK(words[0] == 0x00040003u);
    CHECK(words[1] == 0x80000000u);
    CHECK(words[7] == 0u);
    checkTruncations(flags, [&](std::span<const u8> p) { return net::parseTutorialFlags(p, words); });

    // TutorialFlag::ReadFromWorldPacket: one u32, the 0-based id (wInt = id / 32, bit id % 32).
    Bytes body;
    net::writeTutorialFlag(body, 41);
    CHECK(body == Bytes{41, 0, 0, 0});
}

TEST_CASE("Account data: SMSG_ACCOUNT_DATA_MD5, CMSG_REQUEST_ACCOUNT_DATA and the update packets byte for byte") {
    // SendAccountDataTimes for a 1.12 client: eight 16-byte digests, empty texts as zeros.
    Bytes md5s;
    std::array<net::Md5, net::kAccountDataTypes> sent{};
    sent[4] = net::md5(std::string_view("MACRO 1 \"Hi\" Ability_Warrior_Charge\r\n/wave\r\nEND\r\n"));
    sent[7] = net::md5(std::string_view("VERSION 2\n\n"));
    for (const net::Md5& d : sent) {
        md5s.insert(md5s.end(), d.begin(), d.end());
    }
    CHECK(md5s.size() == 128);
    std::array<net::Md5, net::kAccountDataTypes> got{};
    REQUIRE(net::parseAccountDataMd5(md5s, got));
    CHECK(got == sent);
    CHECK(net::md5IsZero(got[0]));
    checkTruncations(md5s, [&](std::span<const u8> p) { return net::parseAccountDataMd5(p, got); });

    // RequestAccountData: u32 type.
    Bytes request;
    net::writeRequestAccountData(request, 7);
    CHECK(request == Bytes{7, 0, 0, 0});

    // UpdateAccountData: u32 type, u32 decompressedSize, then the rest of the packet is the zlib stream.
    const std::string text = "VERSION 2\n\nADDEDVERSION 2\n\nWINDOW 1\nNAME General\nEND\n\n";
    Bytes update;
    REQUIRE(net::writeUpdateAccountData(update, 7, text));
    net::ByteReader r(update);
    CHECK(r.u32v() == 7);
    CHECK(r.u32v() == text.size());
    CHECK(update[8] == 0x78); // a zlib header
    // The same layout comes back (UpdateAccountDataResponse): it inflates to the text.
    net::AccountDataUpdate parsed;
    REQUIRE(net::parseUpdateAccountData(update, parsed));
    CHECK(parsed.type == 7);
    CHECK(parsed.text == text);
    // Erasing: size 0 and nothing else (HandleUpdateAccountData: decompressedSize 0 = SetAccountData(type, "")).
    Bytes erase;
    REQUIRE(net::writeUpdateAccountData(erase, 5, {}));
    CHECK(erase == Bytes{5, 0, 0, 0, 0, 0, 0, 0});
    // Too big for the server (> 0xFFFF): nothing.
    Bytes big;
    CHECK_FALSE(net::writeUpdateAccountData(big, 4, std::string(0x10000, 'a')));
    CHECK(big.empty());
}

TEST_CASE("Account data: SMSG_UPDATE_ACCOUNT_DATA as the server's zlib writes it, and malformed ones") {
    const std::string text = "MACRO 16777217 \"Shout\" Ability_Warrior_BattleShout\r\n/cast Battle Shout\r\nEND\r\n";
    Bytes packet;
    net::ByteWriter(packet).u32v(5).u32v(static_cast<u32>(text.size()));
    const Bytes stream = storedZlib(text);
    packet.insert(packet.end(), stream.begin(), stream.end());
    net::AccountDataUpdate parsed;
    REQUIRE(net::parseUpdateAccountData(packet, parsed));
    CHECK(parsed.type == 5);
    CHECK(parsed.text == text);
    checkTruncations(packet, [&](std::span<const u8> p) { return net::parseUpdateAccountData(p, parsed); });

    // No data kept: type and a 0 size only.
    Bytes none;
    net::ByteWriter(none).u32v(7).u32v(0);
    REQUIRE(net::parseUpdateAccountData(none, parsed));
    CHECK(parsed.type == 7);
    CHECK(parsed.text.empty());

    // A size that does not match the stream, or garbage instead of zlib.
    Bytes wrongSize;
    net::ByteWriter(wrongSize).u32v(5).u32v(static_cast<u32>(text.size() + 1));
    wrongSize.insert(wrongSize.end(), stream.begin(), stream.end());
    CHECK_FALSE(net::parseUpdateAccountData(wrongSize, parsed));
    Bytes garbage;
    net::ByteWriter(garbage).u32v(5).u32v(4).u32v(0xDEADBEEF);
    CHECK_FALSE(net::parseUpdateAccountData(garbage, parsed));
}
