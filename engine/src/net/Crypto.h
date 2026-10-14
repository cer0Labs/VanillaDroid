#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <array>
#include <initializer_list>
#include <span>
#include <string_view>

namespace vd::net {

using Sha1Digest = std::array<u8, 20>;

// SHA-1 of the concatenation of `parts`.
Sha1Digest sha1(std::initializer_list<std::span<const u8>> parts);
inline std::span<const u8> asBytes(std::string_view text) {
    return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

// Big numbers travel little-endian. The server (vMaNGOS BigNumber) hashes them in their *minimal* length,
// without the zero bytes at the top: a 32-byte B whose last byte is 0 is hashed as 31 bytes. This trims
// a little-endian number the same way.
std::span<const u8> minimalLe(std::span<const u8> number);

// --- SRP6 (realmd login), as vMaNGOS src/shared/Crypto/Authentication/SRP6.cpp -------------------------
// N, g and k = 3 are fixed by the protocol; the server sends N and g anyway and the client uses those.

// SHA1(UPPER(account) ":" UPPER(password)): all the client needs (in memory) to log in.
Sha1Digest srpCredentials(std::string_view account, std::string_view password);

struct SrpChallenge {
    std::array<u8, 32> B{};    // server public ephemeral
    std::array<u8, 32> N{};    // prime
    u8 g = 0;                  // generator
    std::array<u8, 32> salt{};
};

struct SrpProof {
    std::array<u8, 32> A{};  // client public ephemeral, to send
    Sha1Digest M1{};         // client proof, to send
    std::array<u8, 40> K{};  // session key (little-endian, 40 bytes; see keySize)
    u32 keySize = 40;        // K's minimal length: what the server keys its header cipher and digests with
    Sha1Digest M2{};         // the server's proof must match this
};

// `privateKey`: the client secret a, 32 random bytes. `accountUpper` as sent in the challenge.
Result<SrpProof> srpClientProof(std::string_view accountUpper, const Sha1Digest& credentials,
                                const SrpChallenge& challenge, std::span<const u8, 32> privateKey);

// Server side, for tests and offline checks: the verifier v = g^x the server stores for an account, and the
// server's view of a login (B from b; then S, K and M1 from the client's A).
std::array<u8, 32> srpVerifier(const Sha1Digest& credentials, std::span<const u8, 32> salt);
struct SrpServer {
    std::array<u8, 32> B{};
    std::array<u8, 40> K{};
    u32 keySize = 40;
    Sha1Digest M1{}; // what the client must send
    Sha1Digest M2{}; // what the server answers
};
Result<SrpServer> srpServerChallenge(std::span<const u8, 32> verifier, std::span<const u8, 19> privateKey);
Result<void> srpServerProof(SrpServer& server, std::string_view accountUpper, std::span<const u8, 32> verifier,
                            std::span<const u8, 32> salt, std::span<const u8, 19> privateKey,
                            std::span<const u8, 32> clientA);

// --- World header cipher (src/shared/Auth/AuthCrypt.cpp), client side -------------------------------------
// Only packet headers are ciphered: the 6 bytes the client sends and the 4 it receives, keyed by K in its
// minimal length, with running indices for the whole connection.
class HeaderCipher {
public:
    void init(std::span<const u8> key);
    bool active() const { return m_keySize > 0; }
    void encryptOutgoing(u8* header, usize size);
    void decryptIncoming(u8* header, usize size);

private:
    std::array<u8, 40> m_key{};
    u32 m_keySize = 0;
    u32 m_sendIndex = 0;
    u8 m_sendLast = 0;
    u32 m_recvIndex = 0;
    u8 m_recvLast = 0;
};

} // namespace vd::net
