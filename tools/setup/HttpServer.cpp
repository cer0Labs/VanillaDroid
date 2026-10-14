#include "HttpServer.h"

#include "core/Log.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
#endif

namespace vd::setup {
namespace {

constexpr usize kMaxHeadBytes = 16 * 1024;
constexpr u32 kIdleSeconds = 30;
constexpr usize kFileBlock = 256 * 1024;

SocketHandle handleOf(std::intptr_t value) {
    return static_cast<SocketHandle>(value);
}

void closeSocket(SocketHandle socket) {
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

void shutdownSocket(SocketHandle socket) {
#if defined(_WIN32)
    shutdown(socket, SD_BOTH);
    // Unlike elsewhere, shutdown() does not end a recv() another thread waits in: cancel it (overlapped sockets).
    CancelIoEx(reinterpret_cast<HANDLE>(socket), nullptr);
#else
    shutdown(socket, SHUT_RDWR);
#endif
}

// Phase 22 (updates): a connection waits on the listener, for at most `milliseconds`.
bool acceptReady(SocketHandle listener, int milliseconds) {
#if defined(_WIN32)
    fd_set set;
    FD_ZERO(&set);
    FD_SET(listener, &set);
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    return select(0, &set, nullptr, nullptr, &timeout) > 0;
#else
    pollfd descriptor{listener, POLLIN, 0};
    return poll(&descriptor, 1, milliseconds) > 0;
#endif
}

SocketHandle newSocket() {
#if defined(_WIN32)
    // Not inherited: a child process (adb starting its server) must not keep our port open. Overlapped, as socket()
    // makes them (calls still block): closing a socket that another thread waits on (accept, recv) then ends that
    // wait, where a synchronous one makes the close itself wait (stop() hung until the next connection).
    return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                      WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#else
    const SocketHandle s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s >= 0) {
        fcntl(s, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
        const int on = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    }
    return s;
#endif
}

void setTimeouts(SocketHandle socket, u32 seconds) {
#if defined(_WIN32)
    const DWORD ms = seconds * 1000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(seconds);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool sendAll(SocketHandle socket, const char* data, usize size) {
    while (size > 0) {
        const int chunk = static_cast<int>(std::min<usize>(size, 1 << 20));
#if defined(_WIN32)
        const int sent = send(socket, data, chunk, 0);
#elif defined(MSG_NOSIGNAL)
        const int sent = static_cast<int>(::send(socket, data, static_cast<size_t>(chunk), MSG_NOSIGNAL));
#else
        const int sent = static_cast<int>(::send(socket, data, static_cast<size_t>(chunk), 0));
#endif
        if (sent <= 0) {
            return false;
        }
        data += sent;
        size -= static_cast<usize>(sent);
    }
    return true;
}

} // namespace

std::string ipv4Text(u32 address) {
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", (address >> 24) & 255, (address >> 16) & 255,
                  (address >> 8) & 255, address & 255);
    return text;
}

bool isLocalNetworkAddress(u32 address) {
    const u32 a = address >> 24;
    const u32 b = (address >> 16) & 255;
    return a == 127 || a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127);
}

struct HttpServer::Connection {
    std::intptr_t socket = -1;
    u32 client = 0;
    std::thread thread;
    std::atomic<bool> finished{false};
};

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() {
    stop();
}

Result<u16> HttpServer::start(u16 firstPort, u32 attempts, Handler handler, bool loopbackOnly) {
    stop();
#if defined(_WIN32)
    static const bool winsockReady = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsockReady) {
        return Error{"WSAStartup failed"};
    }
#endif
    m_handler = std::move(handler);
    for (u32 i = 0; i < attempts && firstPort + i <= 65535; ++i) {
        const u16 port = static_cast<u16>(firstPort + i);
        const SocketHandle listener = newSocket();
        if (listener == kNoSocket) {
            return Error{"cannot create a socket"};
        }
#if defined(_WIN32)
        // Nobody else may bind the same port while we listen (the default on Windows lets them).
        const BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                   sizeof(exclusive));
#else
        const int reuse = 1; // a restart can listen again at once (TIME_WAIT)
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener, 16) != 0) {
            closeSocket(listener);
            continue; // in use: the next one
        }
        m_listener = static_cast<std::intptr_t>(listener);
        m_port = port;
        m_running = true;
        m_acceptThread = std::thread([this] { acceptLoop(); });
        return port;
    }
    return Error{"no free port from " + std::to_string(firstPort)};
}

void HttpServer::stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    // Phase 22 (updates): the accept loop sees m_running within its wait; only then the listener closes. Shutting down
    // or closing a socket another thread is blocked in accept() on can block the caller too (seen on Windows: stop()
    // never returned).
    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }
    closeSocket(handleOf(m_listener));
    m_listener = -1;
    std::list<std::unique_ptr<Connection>> connections;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const std::unique_ptr<Connection>& connection : m_connections) {
            shutdownSocket(handleOf(connection->socket)); // a blocked recv or send returns
        }
        connections.swap(m_connections);
    }
    for (const std::unique_ptr<Connection>& connection : connections) {
        if (connection->thread.joinable()) {
            connection->thread.join();
        }
    }
}

void HttpServer::reapFinished() {
    std::list<std::unique_ptr<Connection>> done;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_connections.begin(); it != m_connections.end();) {
            if ((*it)->finished.load()) {
                done.push_back(std::move(*it));
                it = m_connections.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const std::unique_ptr<Connection>& connection : done) {
        connection->thread.join();
    }
}

void HttpServer::acceptLoop() {
    while (m_running.load()) {
        sockaddr_in peer{};
#if defined(_WIN32)
        int peerSize = sizeof(peer);
#else
        socklen_t peerSize = sizeof(peer);
#endif
        if (!acceptReady(handleOf(m_listener), 200)) {
            continue; // phase 22 (updates): stop() is noticed within the wait
        }
        const SocketHandle client =
            accept(handleOf(m_listener), reinterpret_cast<sockaddr*>(&peer), &peerSize);
        if (client == kNoSocket) {
            if (!m_running.load()) {
                return;
            }
            continue;
        }
#if !defined(_WIN32)
        fcntl(client, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
        const int on = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
#endif
        const u32 address = ntohl(peer.sin_addr.s_addr);
        reapFinished();
        usize open = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            open = m_connections.size();
        }
        if (!isLocalNetworkAddress(address) || open >= kMaxConnections) {
            if (!isLocalNetworkAddress(address)) {
                VD_LOG_WARN("serve: refused %s (not a local network address)", ipv4Text(address).c_str());
            }
            closeSocket(client);
            continue;
        }
        const int noDelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
                   sizeof(noDelay));
        setTimeouts(client, kIdleSeconds);
        auto connection = std::make_unique<Connection>();
        connection->socket = static_cast<std::intptr_t>(client);
        connection->client = address;
        Connection* raw = connection.get();
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) {
            closeSocket(client);
            return;
        }
        m_connections.push_back(std::move(connection));
        raw->thread = std::thread([this, raw] {
            serve(*raw);
            closeSocket(handleOf(raw->socket));
            raw->finished = true;
        });
    }
}

void HttpServer::serve(Connection& connection) {
    const SocketHandle socket = handleOf(connection.socket);
    std::string buffer;
    std::vector<char> block;
    char chunk[4096];
    while (m_running.load()) {
        // The head: up to the blank line.
        usize headEnd = buffer.find("\r\n\r\n");
        while (headEnd == std::string::npos) {
            if (buffer.size() > kMaxHeadBytes) {
                HttpResponse tooLarge;
                tooLarge.status = 431;
                tooLarge.close = true;
                const std::string head = responseHead(tooLarge, 0, false);
                sendAll(socket, head.data(), head.size());
                return;
            }
            const int got = static_cast<int>(recv(socket, chunk, sizeof(chunk), 0));
            if (got <= 0) {
                return; // closed, idle timeout or stop()
            }
            buffer.append(chunk, static_cast<usize>(got));
            headEnd = buffer.find("\r\n\r\n");
        }
        const std::string head = buffer.substr(0, headEnd);
        buffer.erase(0, headEnd + 4);

        Result<HttpRequest> parsed = parseRequestHead(head);
        HttpResponse response;
        bool keepAlive = false;
        bool isHead = false;
        if (!parsed) {
            response.status = 400;
            response.close = true;
        } else {
            const HttpRequest& request = parsed.value();
            keepAlive = request.keepAlive;
            isHead = request.method == "HEAD";
            if (!request.header("content-length").empty() || !request.header("transfer-encoding").empty()) {
                // No request bodies here: answer and close rather than parse one.
                response.status = 405;
                response.close = true;
            } else {
                response = m_handler(request, connection.client);
            }
        }
        keepAlive = keepAlive && !response.close;
        const u64 bodyLength = response.file.empty() ? response.body.size() : response.length;
        const std::string responseHeadText = responseHead(response, bodyLength, keepAlive);
        if (!sendAll(socket, responseHeadText.data(), responseHeadText.size())) {
            return;
        }
        if (!isHead) {
            if (response.file.empty()) {
                if (!sendAll(socket, response.body.data(), response.body.size())) {
                    return;
                }
            } else {
                // The file in blocks; a closed connection (the phone gave up, stop()) ends it.
                std::ifstream in(response.file, std::ios::binary);
                in.seekg(static_cast<std::streamoff>(response.offset));
                block.resize(kFileBlock);
                u64 left = response.length;
                u64 sent = 0;
                bool ok = static_cast<bool>(in);
                while (ok && left > 0 && m_running.load()) {
                    const usize want = static_cast<usize>(std::min<u64>(left, block.size()));
                    in.read(block.data(), static_cast<std::streamsize>(want));
                    if (static_cast<usize>(in.gcount()) != want) {
                        ok = false; // shorter than announced: it changed under us
                        break;
                    }
                    if (!sendAll(socket, block.data(), want)) {
                        ok = false;
                        break;
                    }
                    left -= want;
                    sent += want;
                    if (response.onProgress) {
                        response.onProgress(sent);
                    }
                }
                if (response.onSent) {
                    response.onSent(sent, left == 0);
                }
                if (left != 0) {
                    return;
                }
            }
        }
        if (!keepAlive) {
            return;
        }
    }
}

} // namespace vd::setup
