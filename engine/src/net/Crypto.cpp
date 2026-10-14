#include "net/Crypto.h"

#include <mbedtls/bignum.h>
#include <mbedtls/sha1.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace vd::net {
namespace {

// N = 894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7 (SRP6::SRP6), little-endian; g = 7.
constexpr u8 kPrimeLe[32] = {0xB7, 0x9B, 0x3E, 0x2A, 0x87, 0x82, 0x3C, 0xAB, 0x8F, 0x5E, 0xBF,
                             0xBF, 0x8E, 0xB1, 0x01, 0x08, 0x53, 0x50, 0x06, 0x29, 0x8B, 0x5B,
                             0xAD, 0xBD, 0x5B, 0x53, 0xE1, 0x89, 0x5E, 0x64, 0x4B, 0x89};

// RAII over mbedtls_mpi (the engine has no exceptions: every call is checked through `ok`).
struct Mpi {
    mbedtls_mpi v;
    Mpi() { mbedtls_mpi_init(&v); }
    ~Mpi() { mbedtls_mpi_free(&v); }
    Mpi(const Mpi&) = delete;
    Mpi& operator=(const Mpi&) = delete;
};

class Calc {
public:
    bool ok = true;

    void readLe(Mpi& x, std::span<const u8> bytes) { check(mbedtls_mpi_read_binary_le(&x.v, bytes.data(), bytes.size())); }
    template <usize Size>
    std::array<u8, Size> writeLe(const Mpi& x) {
        std::array<u8, Size> out{};
        check(mbedtls_mpi_write_binary_le(&x.v, out.data(), out.size()));
        return out;
    }
    void set(Mpi& x, i32 value) { check(mbedtls_mpi_lset(&x.v, value)); }
    void mul(Mpi& x, const Mpi& a, const Mpi& b) { check(mbedtls_mpi_mul_mpi(&x.v, &a.v, &b.v)); }
    void add(Mpi& x, const Mpi& a, const Mpi& b) { check(mbedtls_mpi_add_mpi(&x.v, &a.v, &b.v)); }
    void sub(Mpi& x, const Mpi& a, const Mpi& b) { check(mbedtls_mpi_sub_mpi(&x.v, &a.v, &b.v)); }
    void mod(Mpi& x, const Mpi& a, const Mpi& n) { check(mbedtls_mpi_mod_mpi(&x.v, &a.v, &n.v)); } // 0 <= x < n
    void expMod(Mpi& x, const Mpi& base, const Mpi& exponent, const Mpi& n) {
        Mpi reduced;
        mod(reduced, base, n);
        check(mbedtls_mpi_exp_mod(&x.v, &reduced.v, &exponent.v, &n.v, nullptr));
    }
    bool isZero(const Mpi& x) const { return mbedtls_mpi_cmp_int(&x.v, 0) == 0; }

private:
    void check(int result) { ok = ok && result == 0; }
};

std::array<u8, 20> digestNumber(std::span<const u8> bytes) {
    std::array<u8, 20> out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

// K = SHA1 of the even bytes of S interleaved with SHA1 of the odd ones (SRP6::HashSessionKey).
std::array<u8, 40> interleaveKey(const std::array<u8, 32>& s) {
    std::array<u8, 16> half{};
    std::array<u8, 40> k{};
    for (u32 parity = 0; parity < 2; ++parity) {
        for (u32 i = 0; i < 16; ++i) {
            half[i] = s[i * 2 + parity];
        }
        const Sha1Digest digest = sha1({half});
        for (u32 i = 0; i < 20; ++i) {
            k[i * 2 + parity] = digest[i];
        }
    }
    return k;
}

// M1 = H( (H(N) xor H(g)) | H(account) | s | A | B | K ), every number in its minimal length (CalculateProof).
Sha1Digest proofM1(std::string_view accountUpper, std::span<const u8> n, u8 g, std::span<const u8> salt,
                   std::span<const u8> a, std::span<const u8> b, std::span<const u8> k) {
    Sha1Digest hashN = sha1({minimalLe(n)});
    const u8 gByte[1] = {g};
    const Sha1Digest hashG = sha1({minimalLe(gByte)});
    for (u32 i = 0; i < 20; ++i) {
        hashN[i] ^= hashG[i];
    }
    const Sha1Digest hashAccount = sha1({asBytes(accountUpper)});
    return sha1({minimalLe(hashN), hashAccount, minimalLe(salt), minimalLe(a), minimalLe(b), minimalLe(k)});
}

// M2 = H(A | M1 | K) (SRP6::Finalize).
Sha1Digest proofM2(std::span<const u8> a, const Sha1Digest& m1, std::span<const u8> k) {
    return sha1({minimalLe(a), minimalLe(m1), minimalLe(k)});
}

} // namespace

Sha1Digest sha1(std::initializer_list<std::span<const u8>> parts) {
    mbedtls_sha1_context context;
    mbedtls_sha1_init(&context);
    mbedtls_sha1_starts(&context);
    for (std::span<const u8> part : parts) {
        mbedtls_sha1_update(&context, part.data(), part.size());
    }
    Sha1Digest digest{};
    mbedtls_sha1_finish(&context, digest.data());
    mbedtls_sha1_free(&context);
    return digest;
}

std::span<const u8> minimalLe(std::span<const u8> number) {
    usize size = number.size();
    while (size > 0 && number[size - 1] == 0) {
        --size;
    }
    return number.first(size);
}

Sha1Digest srpCredentials(std::string_view account, std::string_view password) {
    std::string text;
    text.reserve(account.size() + password.size() + 1);
    for (char c : account) {
        text.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    text.push_back(':');
    for (char c : password) {
        text.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    const Sha1Digest digest = sha1({asBytes(text)});
    std::fill(text.begin(), text.end(), '\0'); // don't leave the password around in freed memory
    return digest;
}

Result<SrpProof> srpClientProof(std::string_view accountUpper, const Sha1Digest& credentials,
                                const SrpChallenge& challenge, std::span<const u8, 32> privateKey) {
    Calc c;
    Mpi n, g, b, a, x, bigA, u, k, gx, base, exponent, s, tmp;
    c.readLe(n, challenge.N);
    c.set(g, challenge.g);
    c.readLe(b, challenge.B);
    c.readLe(a, privateKey);
    c.set(k, 3);
    if (!c.ok || c.isZero(n) || challenge.g == 0) {
        return Error{"SRP6: bad challenge"};
    }
    {
        Mpi bModN;
        c.mod(bModN, b, n);
        if (c.isZero(bModN)) {
            return Error{"SRP6: B = 0 mod N"}; // would let anyone pass: refuse
        }
    }

    SrpProof proof;
    // x = H(s | H(USER:PASS))
    c.readLe(x, sha1({minimalLe(challenge.salt), credentials}));
    // A = g^a mod N
    c.expMod(bigA, g, a, n);
    proof.A = c.writeLe<32>(bigA);
    // u = H(A | B)
    c.readLe(u, sha1({minimalLe(proof.A), minimalLe(challenge.B)}));
    // S = (B - k * g^x)^(a + u * x) mod N
    c.expMod(gx, g, x, n);
    c.mul(tmp, k, gx);
    c.sub(base, b, tmp);
    c.mod(base, base, n);
    c.mul(tmp, u, x);
    c.add(exponent, a, tmp);
    c.expMod(s, base, exponent, n);
    const std::array<u8, 32> sBytes = c.writeLe<32>(s);
    if (!c.ok) {
        return Error{"SRP6: big number arithmetic failed"};
    }

    proof.K = interleaveKey(sBytes);
    proof.keySize = static_cast<u32>(minimalLe(proof.K).size());
    proof.M1 = proofM1(accountUpper, challenge.N, challenge.g, challenge.salt, proof.A, challenge.B, proof.K);
    proof.M2 = proofM2(proof.A, proof.M1, proof.K);
    return proof;
}

std::array<u8, 32> srpVerifier(const Sha1Digest& credentials, std::span<const u8, 32> salt) {
    Calc c;
    Mpi n, g, x, v;
    c.readLe(n, kPrimeLe);
    c.set(g, 7);
    c.readLe(x, sha1({minimalLe(salt), credentials}));
    c.expMod(v, g, x, n);
    return c.writeLe<32>(v);
}

Result<SrpServer> srpServerChallenge(std::span<const u8, 32> verifier, std::span<const u8, 19> privateKey) {
    Calc c;
    Mpi n, g, v, b, gb, tmp, bigB, three;
    c.readLe(n, kPrimeLe);
    c.set(g, 7);
    c.set(three, 3);
    c.readLe(v, verifier);
    c.readLe(b, privateKey);
    // B = (3v + g^b) mod N (CalculateHostPublicEphemeral)
    c.expMod(gb, g, b, n);
    c.mul(tmp, v, three);
    c.add(tmp, tmp, gb);
    c.mod(bigB, tmp, n);
    SrpServer server;
    server.B = c.writeLe<32>(bigB);
    if (!c.ok) {
        return Error{"SRP6 server: arithmetic failed"};
    }
    return server;
}

Result<void> srpServerProof(SrpServer& server, std::string_view accountUpper, std::span<const u8, 32> verifier,
                            std::span<const u8, 32> salt, std::span<const u8, 19> privateKey,
                            std::span<const u8, 32> clientA) {
    Calc c;
    Mpi n, v, b, a, u, vu, tmp, s;
    c.readLe(n, kPrimeLe);
    c.readLe(v, verifier);
    c.readLe(b, privateKey);
    c.readLe(a, clientA);
    // S = (A * v^u)^b mod N (CalculateSessionKey)
    c.readLe(u, sha1({minimalLe(clientA), minimalLe(server.B)}));
    c.expMod(vu, v, u, n);
    c.mul(tmp, a, vu);
    c.expMod(s, tmp, b, n);
    const std::array<u8, 32> sBytes = c.writeLe<32>(s);
    if (!c.ok) {
        return Error{"SRP6 server: arithmetic failed"};
    }
    server.K = interleaveKey(sBytes);
    server.keySize = static_cast<u32>(minimalLe(server.K).size());
    server.M1 = proofM1(accountUpper, kPrimeLe, 7, salt, clientA, server.B, server.K);
    server.M2 = proofM2(clientA, server.M1, server.K);
    return {};
}

void HeaderCipher::init(std::span<const u8> key) {
    m_keySize = static_cast<u32>(std::min<usize>(key.size(), m_key.size()));
    std::copy_n(key.begin(), m_keySize, m_key.begin());
    if (m_keySize == 0) { // as the server: an empty key becomes one zero byte
        m_key[0] = 0;
        m_keySize = 1;
    }
    m_sendIndex = m_recvIndex = 0;
    m_sendLast = m_recvLast = 0;
}

void HeaderCipher::encryptOutgoing(u8* header, usize size) {
    for (usize t = 0; t < size; ++t) {
        m_sendIndex %= m_keySize;
        const u8 x = static_cast<u8>((header[t] ^ m_key[m_sendIndex]) + m_sendLast);
        ++m_sendIndex;
        header[t] = m_sendLast = x;
    }
}

void HeaderCipher::decryptIncoming(u8* header, usize size) {
    for (usize t = 0; t < size; ++t) {
        m_recvIndex %= m_keySize;
        const u8 x = static_cast<u8>((header[t] - m_recvLast) ^ m_key[m_recvIndex]);
        ++m_recvIndex;
        m_recvLast = header[t];
        header[t] = x;
    }
}

} // namespace vd::net
