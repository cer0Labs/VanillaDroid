#include "net/WorldConnection.h"

#include "net/ByteBuffer.h"
#include "net/RealmClient.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>
#include <string>

namespace vd::net {
namespace {

constexpr usize kServerHeader = 4;
constexpr usize kClientHeader = 6;
constexpr u32 kMaxInflated = 900000; // the server refuses to build anything larger (UpdateData.cpp)

} // namespace

Result<void> WorldConnection::open(std::string_view host, u16 port, std::string_view account,
                                   std::span<const u8> sessionKey) {
    close();
    VD_TRY(m_socket.connect(host, port, kNetTimeoutMs));

    // SMSG_AUTH_CHALLENGE (not ciphered): the server seed.
    u16 opcode = 0;
    std::vector<u8> payload;
    Result<bool> got = false;
    for (u32 waited = 0; waited < kNetTimeoutMs; waited += 100) { // it may arrive in pieces
        got = receive(opcode, payload, 100);
        if (!got || got.value()) {
            break;
        }
    }
    if (!got) {
        return got.error();
    }
    if (!got.value() || opcode != static_cast<u16>(Opcode::SMSG_AUTH_CHALLENGE) || payload.size() < 4) {
        return Error{"The world server did not send its challenge"};
    }
    u32 serverSeed = 0;
    std::memcpy(&serverSeed, payload.data(), 4);

    std::string accountUpper(account);
    std::transform(accountUpper.begin(), accountUpper.end(), accountUpper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::random_device device;
    const u32 clientSeed = device();
    const u32 zero = 0;
    // SHA1(account | 0 | clientSeed | serverSeed | K), K in its minimal length (_HandleAuthSession).
    const Sha1Digest digest =
        sha1({asBytes(accountUpper), {reinterpret_cast<const u8*>(&zero), 4},
              {reinterpret_cast<const u8*>(&clientSeed), 4}, {reinterpret_cast<const u8*>(&serverSeed), 4},
              minimalLe(sessionKey)});

    std::vector<u8> body;
    ByteWriter w(body);
    w.u32v(kClientBuild).u32v(0); // build, server id
    w.cstring(accountUpper);
    w.u32v(clientSeed).bytes(digest);
    w.u32v(0); // addon block of size 0: the server skips SMSG_ADDON_INFO (AddonHandler::BuildAddonPacket)
    VD_TRY(send(Opcode::CMSG_AUTH_SESSION, body));

    // The server keys its cipher as soon as the digest checks out: everything after this is ciphered.
    m_cipher.init(minimalLe(sessionKey));
    return {};
}

void WorldConnection::close() {
    m_socket.close();
    m_cipher = HeaderCipher{};
    m_in.clear();
    m_inStart = 0;
    m_haveHeader = false;
}

Result<void> WorldConnection::send(Opcode opcode, std::span<const u8> payload) {
    const usize size = payload.size() + 4;
    if (size > 0x2800) { // WorldSocket.cpp drops the connection above this
        return Error{"Packet too large for the world server"};
    }
    m_out.resize(kClientHeader + payload.size());
    m_out[0] = static_cast<u8>(size >> 8);
    m_out[1] = static_cast<u8>(size);
    const u32 code = static_cast<u32>(opcode);
    std::memcpy(m_out.data() + 2, &code, 4);
    if (m_cipher.active()) {
        m_cipher.encryptOutgoing(m_out.data(), kClientHeader);
    }
    if (!payload.empty()) {
        std::memcpy(m_out.data() + kClientHeader, payload.data(), payload.size());
    }
    return m_socket.sendAll(m_out);
}

Result<bool> WorldConnection::takePacket(u16& opcode, std::vector<u8>& payload) {
    const usize available = m_in.size() - m_inStart;
    if (!m_haveHeader) {
        if (available < kServerHeader) {
            return false;
        }
        u8* header = m_in.data() + m_inStart;
        if (m_cipher.active()) {
            m_cipher.decryptIncoming(header, kServerHeader); // exactly once per header: the cipher runs on
        }
        const u32 size = (static_cast<u32>(header[0]) << 8) | header[1];
        if (size < 2) {
            return Error{"Malformed packet header from the world server"};
        }
        m_opcode = static_cast<u16>(header[2] | (header[3] << 8));
        m_bodySize = size - 2;
        m_haveHeader = true;
    }
    if (available < kServerHeader + m_bodySize) {
        return false;
    }
    opcode = m_opcode;
    const u8* body = m_in.data() + m_inStart + kServerHeader;
    payload.assign(body, body + m_bodySize);
    m_inStart += kServerHeader + m_bodySize;
    m_haveHeader = false;
    if (m_inStart == m_in.size()) {
        m_in.clear();
        m_inStart = 0;
    }
    return true;
}

Result<bool> WorldConnection::receive(u16& opcode, std::vector<u8>& payload, u32 timeoutMs) {
    for (;;) {
        Result<bool> packet = takePacket(opcode, payload);
        if (!packet || packet.value()) {
            return packet;
        }
        if (!m_socket.waitReadable(timeoutMs)) {
            return false;
        }
        if (m_inStart > 0) { // keep the buffer compact before growing it
            m_in.erase(m_in.begin(), m_in.begin() + static_cast<std::ptrdiff_t>(m_inStart));
            m_inStart = 0;
        }
        const usize oldSize = m_in.size();
        m_in.resize(oldSize + 16384);
        Result<usize> n = m_socket.receive(m_in.data() + oldSize, 16384);
        if (!n) {
            m_in.resize(oldSize);
            return n.error();
        }
        m_in.resize(oldSize + n.value());
        if (n.value() == 0) {
            return Error{"The world server closed the connection"};
        }
        timeoutMs = 0; // got data: now only drain what is already there
    }
}

Result<void> inflatePacket(std::span<const u8> payload, std::vector<u8>& out) {
    if (payload.size() < 4) {
        return Error{"Compressed packet too small"};
    }
    u32 size = 0;
    std::memcpy(&size, payload.data(), 4);
    if (size > kMaxInflated) {
        return Error{"Compressed packet too large"};
    }
    out.resize(size);
    uLongf outSize = size;
    const int result = uncompress(out.data(), &outSize, payload.data() + 4, static_cast<uLong>(payload.size() - 4));
    if (result != Z_OK || outSize != size) {
        return Error{"Cannot inflate a compressed packet"};
    }
    return {};
}

} // namespace vd::net
