#pragma once

#include "HttpProtocol.h"
#include "core/Result.h"
#include "core/Types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Phase 22 (ADR-0005 §5): the local server's sockets (Winsock on Windows, BSD sockets elsewhere). Listens on
// every IPv4 interface but answers only addresses of a local network (loopback, 10/8, 172.16/12, 192.168/16,
// 169.254/16 and 100.64/10, the carrier-grade NAT range VPNs like Tailscale use): a PC with a public address
// or a forwarded port does not serve the packs to the internet. One thread per connection (at most
// kMaxConnections), persistent connections with a 30 s idle timeout.
namespace vd::setup {

// IPv4 address in host order -> "192.168.1.20".
std::string ipv4Text(u32 address);
// True for the address ranges above.
bool isLocalNetworkAddress(u32 address);

class HttpServer {
public:
    static constexpr u32 kMaxConnections = 16;

    // `client` is the peer's IPv4 address in host order.
    using Handler = std::function<HttpResponse(const HttpRequest& request, u32 client)>;

    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Listens on the first free port from `firstPort` (trying `attempts` ports). `loopbackOnly` binds
    // 127.0.0.1 only (tests). Returns the port.
    Result<u16> start(u16 firstPort, u32 attempts, Handler handler, bool loopbackOnly = false);
    void stop(); // closes the listener and every connection, joins their threads
    bool running() const { return m_running.load(); }
    u16 port() const { return m_port; }

private:
    struct Connection;
    void acceptLoop();
    void serve(Connection& connection);
    void reapFinished();

    Handler m_handler;
    std::atomic<bool> m_running{false};
    u16 m_port = 0;
    std::intptr_t m_listener = -1;
    std::thread m_acceptThread;
    std::mutex m_mutex;
    std::list<std::unique_ptr<Connection>> m_connections;
};

} // namespace vd::setup
