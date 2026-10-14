#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "net/Crypto.h"
#include "net/Opcodes.gen.h"
#include "net/Socket.h"

#include <span>
#include <string_view>
#include <vector>

namespace vd::net {

// One world server connection (src/game/Server/WorldSocket.cpp): packet framing, the session handshake and
// the header cipher. Blocking; used by the network thread only.
//   server -> client header: size u16 big-endian (opcode included) + opcode u16, 4 bytes
//   client -> server header: size u16 big-endian (opcode included) + opcode u32, 6 bytes
class WorldConnection {
public:
    // Connects and answers SMSG_AUTH_CHALLENGE with CMSG_AUTH_SESSION; from then on headers are ciphered.
    // The server's verdict arrives as SMSG_AUTH_RESPONSE through receive().
    Result<void> open(std::string_view host, u16 port, std::string_view account, std::span<const u8> sessionKey);
    void shutdown() { m_socket.shutdown(); }
    void close();
    bool isOpen() const { return m_socket.isOpen(); }

    Result<void> send(Opcode opcode, std::span<const u8> payload);
    // The next complete packet into `payload` (reused), waiting up to `timeoutMs`. False when nothing came.
    Result<bool> receive(u16& opcode, std::vector<u8>& payload, u32 timeoutMs);

private:
    Result<bool> takePacket(u16& opcode, std::vector<u8>& payload);

    Socket m_socket;
    HeaderCipher m_cipher;
    std::vector<u8> m_in;  // received, not yet consumed
    usize m_inStart = 0;
    bool m_haveHeader = false; // the header at m_inStart is already deciphered
    u16 m_opcode = 0;
    u32 m_bodySize = 0;
    std::vector<u8> m_out;
};

// SMSG_COMPRESSED_UPDATE_OBJECT / SMSG_COMPRESSED_MOVES: u32 inflated size, then a zlib stream.
Result<void> inflatePacket(std::span<const u8> payload, std::vector<u8>& out);

} // namespace vd::net
