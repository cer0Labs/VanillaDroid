#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <atomic>
#include <span>
#include <string_view>

namespace vd::net {

// Blocking TCP socket (Winsock on Windows, BSD sockets on Android), used only from the network thread.
// shutdown() may be called from any thread to unblock a pending receive.
class Socket {
public:
    Socket() = default;
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // `host`: name or dotted address. Fails after `timeoutMs` without an answer.
    Result<void> connect(std::string_view host, u16 port, u32 timeoutMs);
    bool isOpen() const { return m_handle.load() != kInvalid; }

    Result<void> sendAll(std::span<const u8> data);
    // True when data (or the end of the stream) can be read without blocking.
    bool waitReadable(u32 timeoutMs);
    // Whatever arrived, up to `size` bytes; 0 means the peer closed the connection.
    Result<usize> receive(u8* out, usize size);
    // Exactly `size` bytes, or an error after `timeoutMs` without progress.
    Result<void> receiveExact(u8* out, usize size, u32 timeoutMs);

    void shutdown();
    void close();

private:
    static constexpr i64 kInvalid = -1;
    std::atomic<i64> m_handle{kInvalid};
};

} // namespace vd::net
