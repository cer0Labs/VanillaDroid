#include "net/Socket.h"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace vd::net {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
int lastError() {
    return WSAGetLastError();
}
bool wouldBlock(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
void closeNative(NativeSocket s) {
    closesocket(s);
}
bool setBlocking(NativeSocket s, bool blocking) {
    u_long nonBlocking = blocking ? 0 : 1;
    return ioctlsocket(s, FIONBIO, &nonBlocking) == 0;
}
// Winsock is started once per process and never stopped (it lives as long as the app).
bool ensureWinsock() {
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}
// Waits for readability (write = false) or writability; > 0 ready, 0 timeout, < 0 error.
int waitFor(NativeSocket s, bool write, u32 timeoutMs) {
    WSAPOLLFD fd{};
    fd.fd = s;
    fd.events = write ? POLLWRNORM : POLLRDNORM;
    return WSAPoll(&fd, 1, static_cast<INT>(timeoutMs));
}
#else
using NativeSocket = int;
constexpr NativeSocket kNoSocket = -1;
int lastError() {
    return errno;
}
bool wouldBlock(int error) {
    return error == EINPROGRESS || error == EWOULDBLOCK || error == EAGAIN;
}
void closeNative(NativeSocket s) {
    ::close(s);
}
bool setBlocking(NativeSocket s, bool blocking) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
}
bool ensureWinsock() {
    return true;
}
int waitFor(NativeSocket s, bool write, u32 timeoutMs) {
    pollfd fd{};
    fd.fd = s;
    fd.events = write ? POLLOUT : POLLIN;
    int result = 0;
    do {
        result = poll(&fd, 1, static_cast<int>(timeoutMs));
    } while (result < 0 && errno == EINTR);
    return result;
}
#endif

Error socketError(const char* what, int code) {
    char text[96];
    std::snprintf(text, sizeof(text), "%s failed (error %d)", what, code);
    return Error{text};
}

} // namespace

Socket::~Socket() {
    close();
}

Result<void> Socket::connect(std::string_view host, u16 port, u32 timeoutMs) {
    close();
    if (!ensureWinsock()) {
        return Error{"Winsock could not start"};
    }
    const std::string hostName(host);
    const std::string portName = std::to_string(port);
    addrinfo hints{};
    hints.ai_family = AF_INET; // the server announces IPv4 addresses
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    if (const int r = getaddrinfo(hostName.c_str(), portName.c_str(), &hints, &found); r != 0 || !found) {
        return Error{"Cannot resolve " + hostName};
    }
    NativeSocket s = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (s == kNoSocket) {
        freeaddrinfo(found);
        return socketError("socket()", lastError());
    }
    // Non-blocking connect so it can time out, then back to blocking for the network thread.
    setBlocking(s, false);
    int result = ::connect(s, found->ai_addr, static_cast<int>(found->ai_addrlen));
    freeaddrinfo(found);
    if (result != 0) {
        const int error = lastError();
        if (!wouldBlock(error)) {
            closeNative(s);
            return socketError("connect()", error);
        }
        if (waitFor(s, true, timeoutMs) <= 0) {
            closeNative(s);
            return Error{"Connection to " + hostName + ":" + portName + " timed out"};
        }
        int socketErrorCode = 0;
        socklen_t length = sizeof(socketErrorCode);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketErrorCode), &length);
        if (socketErrorCode != 0) {
            closeNative(s);
            return Error{"Cannot connect to " + hostName + ":" + portName + " (error " +
                         std::to_string(socketErrorCode) + ")"};
        }
    }
    setBlocking(s, true);
    const int noDelay = 1; // small packets (movement, pings) must leave at once
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    m_handle.store(static_cast<i64>(s));
    return {};
}

Result<void> Socket::sendAll(std::span<const u8> data) {
    const i64 handle = m_handle.load();
    if (handle == kInvalid) {
        return Error{"Socket closed"};
    }
    const NativeSocket s = static_cast<NativeSocket>(handle);
    usize sent = 0;
    while (sent < data.size()) {
#if defined(_WIN32)
        const int n = send(s, reinterpret_cast<const char*>(data.data() + sent), static_cast<int>(data.size() - sent), 0);
#else
        const ssize_t n = ::send(s, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
#if !defined(_WIN32)
            if (n < 0 && errno == EINTR) {
                continue;
            }
#endif
            return socketError("send()", lastError());
        }
        sent += static_cast<usize>(n);
    }
    return {};
}

bool Socket::waitReadable(u32 timeoutMs) {
    const i64 handle = m_handle.load();
    return handle != kInvalid && waitFor(static_cast<NativeSocket>(handle), false, timeoutMs) != 0;
}

Result<usize> Socket::receive(u8* out, usize size) {
    const i64 handle = m_handle.load();
    if (handle == kInvalid) {
        return Error{"Socket closed"};
    }
    for (;;) {
#if defined(_WIN32)
        const int n = recv(static_cast<NativeSocket>(handle), reinterpret_cast<char*>(out), static_cast<int>(size), 0);
#else
        const ssize_t n = ::recv(static_cast<NativeSocket>(handle), out, size, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
#endif
        if (n < 0) {
            return socketError("recv()", lastError());
        }
        return static_cast<usize>(n);
    }
}

Result<void> Socket::receiveExact(u8* out, usize size, u32 timeoutMs) {
    usize got = 0;
    while (got < size) {
        if (!waitReadable(timeoutMs)) {
            return Error{"Timed out waiting for the server"};
        }
        Result<usize> n = receive(out + got, size - got);
        if (!n) {
            return n.error();
        }
        if (n.value() == 0) {
            return Error{"Connection closed by the server"};
        }
        got += n.value();
    }
    return {};
}

void Socket::shutdown() {
    const i64 handle = m_handle.load();
    if (handle != kInvalid) {
#if defined(_WIN32)
        ::shutdown(static_cast<NativeSocket>(handle), SD_BOTH);
#else
        ::shutdown(static_cast<NativeSocket>(handle), SHUT_RDWR);
#endif
    }
}

void Socket::close() {
    const i64 handle = m_handle.exchange(kInvalid);
    if (handle != kInvalid) {
        closeNative(static_cast<NativeSocket>(handle));
    }
}

} // namespace vd::net
