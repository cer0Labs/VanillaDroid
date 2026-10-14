#include "net/ByteBuffer.h"
#include "net/Crypto.h"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <vector>

namespace {

using vd::u32;
using vd::u64;
using vd::u8;
using vd::usize;

std::string hex(std::span<const u8> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (u8 b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 15]);
    }
    return out;
}

template <usize N>
std::array<u8, N> pattern(u8 seed) {
    std::array<u8, N> out{};
    for (usize i = 0; i < N; ++i) {
        out[i] = static_cast<u8>(seed + i * 37);
    }
    return out;
}

// The server's AuthCrypt (src/shared/Auth/AuthCrypt.cpp), verbatim: what it does with our headers.
struct ServerCrypt {
    std::vector<u8> key;
    usize recvI = 0, recvJ = 0, sendI = 0, sendJ = 0;
    void decryptRecv(u8* data) { // 6 bytes from the client
        for (usize t = 0; t < 6; t++) {
            recvI %= key.size();
            u8 x = static_cast<u8>((data[t] - recvJ) ^ key[recvI]);
            ++recvI;
            recvJ = data[t];
            data[t] = x;
        }
    }
    void encryptSend(u8* data) { // 4 bytes to the client
        for (usize t = 0; t < 4; t++) {
            sendI %= key.size();
            u8 x = static_cast<u8>((data[t] ^ key[sendI]) + sendJ);
            ++sendI;
            data[t] = x;
            sendJ = x;
        }
    }
};

} // namespace

TEST_CASE("SHA-1 test vectors (FIPS 180)") {
    CHECK(hex(vd::net::sha1({vd::net::asBytes("abc")})) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(vd::net::sha1({})) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    // Parts are concatenated.
    CHECK(vd::net::sha1({vd::net::asBytes("a"), vd::net::asBytes("bc")}) == vd::net::sha1({vd::net::asBytes("abc")}));
}

TEST_CASE("Big numbers are hashed without their top zero bytes") {
    const u8 number[4] = {1, 2, 0, 0};
    CHECK(vd::net::minimalLe(number).size() == 2);
    const u8 zero[3] = {0, 0, 0};
    CHECK(vd::net::minimalLe(zero).empty());
}

TEST_CASE("SRP6: client and server (vMaNGOS algorithm) agree") {
    const vd::net::Sha1Digest credentials = vd::net::srpCredentials("Tester", "secret1");
    CHECK(credentials == vd::net::srpCredentials("TESTER", "SECRET1")); // case-insensitive, like the server

    for (u8 seed = 1; seed < 40; seed += 3) {
        CAPTURE(seed);
        const std::array<u8, 32> salt = pattern<32>(seed);
        const std::array<u8, 32> verifier = vd::net::srpVerifier(credentials, salt);
        const std::array<u8, 19> serverKey = pattern<19>(static_cast<u8>(seed * 7));
        vd::Result<vd::net::SrpServer> server = vd::net::srpServerChallenge(verifier, serverKey);
        REQUIRE(server.ok());

        vd::net::SrpChallenge challenge;
        challenge.B = server.value().B;
        challenge.g = 7;
        challenge.salt = salt;
        const u8 primeLe[32] = {0xB7, 0x9B, 0x3E, 0x2A, 0x87, 0x82, 0x3C, 0xAB, 0x8F, 0x5E, 0xBF,
                                0xBF, 0x8E, 0xB1, 0x01, 0x08, 0x53, 0x50, 0x06, 0x29, 0x8B, 0x5B,
                                0xAD, 0xBD, 0x5B, 0x53, 0xE1, 0x89, 0x5E, 0x64, 0x4B, 0x89};
        std::copy(std::begin(primeLe), std::end(primeLe), challenge.N.begin());
        const std::array<u8, 32> clientKey = pattern<32>(static_cast<u8>(seed * 11));

        vd::Result<vd::net::SrpProof> proof = vd::net::srpClientProof("TESTER", credentials, challenge, clientKey);
        REQUIRE(proof.ok());
        REQUIRE(vd::net::srpServerProof(server.value(), "TESTER", verifier, salt, serverKey, proof.value().A).ok());
        CHECK(proof.value().K == server.value().K);
        CHECK(proof.value().keySize == server.value().keySize);
        CHECK(proof.value().M1 == server.value().M1);
        CHECK(proof.value().M2 == server.value().M2);

        // A wrong password gives a different proof (the server would refuse it).
        vd::Result<vd::net::SrpProof> wrong =
            vd::net::srpClientProof("TESTER", vd::net::srpCredentials("TESTER", "SECRET2"), challenge, clientKey);
        REQUIRE(wrong.ok());
        CHECK(wrong.value().M1 != server.value().M1);
    }
}

TEST_CASE("SRP6: B = 0 mod N is refused") {
    vd::net::SrpChallenge challenge;
    challenge.g = 7;
    const u8 primeLe[32] = {0xB7, 0x9B, 0x3E, 0x2A, 0x87, 0x82, 0x3C, 0xAB, 0x8F, 0x5E, 0xBF,
                            0xBF, 0x8E, 0xB1, 0x01, 0x08, 0x53, 0x50, 0x06, 0x29, 0x8B, 0x5B,
                            0xAD, 0xBD, 0x5B, 0x53, 0xE1, 0x89, 0x5E, 0x64, 0x4B, 0x89};
    std::copy(std::begin(primeLe), std::end(primeLe), challenge.N.begin());
    challenge.B = challenge.N; // B = N
    const std::array<u8, 32> clientKey = pattern<32>(5);
    CHECK_FALSE(vd::net::srpClientProof("A", vd::net::srpCredentials("a", "b"), challenge, clientKey).ok());
}

TEST_CASE("World header cipher matches the server's AuthCrypt in both directions") {
    for (usize keySize : {usize{40}, usize{39}}) { // K without its top zero byte is 39 bytes long
        CAPTURE(keySize);
        std::array<u8, 40> key = pattern<40>(9);
        ServerCrypt server;
        server.key.assign(key.begin(), key.begin() + static_cast<std::ptrdiff_t>(keySize));
        vd::net::HeaderCipher client;
        client.init(std::span<const u8>(key.data(), keySize));

        for (u32 packet = 0; packet < 50; ++packet) {
            // Client -> server: size u16 big-endian + opcode u32.
            u8 out[6] = {0x00, static_cast<u8>(4 + packet), static_cast<u8>(packet), 0x01, 0x00, 0x00};
            u8 sent[6];
            std::copy(std::begin(out), std::end(out), sent);
            client.encryptOutgoing(sent, 6);
            server.decryptRecv(sent);
            CHECK(std::equal(std::begin(out), std::end(out), sent));

            // Server -> client: size u16 big-endian + opcode u16.
            u8 in[4] = {0x00, static_cast<u8>(2 + packet), static_cast<u8>(packet * 3), 0x02};
            u8 received[4];
            std::copy(std::begin(in), std::end(in), received);
            server.encryptSend(received);
            client.decryptIncoming(received, 4);
            CHECK(std::equal(std::begin(in), std::end(in), received));
        }
    }
}

TEST_CASE("ByteBuffer: little-endian values, strings, packed GUIDs and overruns") {
    std::vector<u8> bytes;
    vd::net::ByteWriter w(bytes);
    w.u8v(1).u16v(0x0203).u32v(0x04050607).f32v(1.5f).cstring("ADMIN").packedGuid(0x0000'0001'0000'0A0Bull);
    w.packedGuid(0);
    CHECK(bytes[1] == 0x03); // little-endian
    vd::net::ByteReader r(bytes);
    CHECK(r.u8v() == 1);
    CHECK(r.u16v() == 0x0203);
    CHECK(r.u32v() == 0x04050607);
    CHECK(r.f32v() == 1.5f);
    CHECK(std::string(r.cstring()) == "ADMIN");
    CHECK(r.packedGuid() == 0x0000'0001'0000'0A0Bull);
    CHECK(r.packedGuid() == 0);
    CHECK(r.ok());
    CHECK(r.atEnd());
    CHECK(r.u32v() == 0); // past the end: zero and failed
    CHECK(r.failed());

    const u8 noTerminator[3] = {'a', 'b', 'c'};
    vd::net::ByteReader s(noTerminator);
    CHECK(s.cstring().empty());
    CHECK(s.failed());
}
