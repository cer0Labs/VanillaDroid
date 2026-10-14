// Phase 18 (glue): the network thread's realm choice against a fake realmd on loopback that speaks
// vmangos-core 4641790's protocol (src/realmd/AuthSocket.cpp: the logon challenge and proof of a 1.12 client
// with the SRP6 of net/Crypto's server side, then CMD_REALM_LIST as often as asked): with two realms and none
// kept, NetClient stops at ChoosingRealm and hands both lists to the main thread (the login's and a refresh),
// and chooseRealm connects to that realm's world server; with the realm kept, it goes there by itself.
// Windows only (the fake servers use Winsock).
#include "net/ByteBuffer.h"
#include "net/Crypto.h"
#include "net/NetClient.h"
#include "net/RealmListMessages.h"

#include <doctest/doctest.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace vd;
using namespace vd::net;
using Bytes = std::vector<u8>;

// A listening socket on 127.0.0.1 and an ephemeral port.
struct Listener {
    SOCKET socket = INVALID_SOCKET;
    u16 port = 0;

    Listener() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        ::listen(socket, 4);
        int size = sizeof(address);
        ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size);
        port = ntohs(address.sin_port);
    }
    ~Listener() {
        ::closesocket(socket);
        WSACleanup();
    }
    // The next connection within `timeoutMs`, INVALID_SOCKET without one.
    SOCKET accept(u32 timeoutMs) const {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket, &set);
        timeval wait{static_cast<long>(timeoutMs / 1000), static_cast<long>((timeoutMs % 1000) * 1000)};
        if (::select(0, &set, nullptr, nullptr, &wait) <= 0) {
            return INVALID_SOCKET;
        }
        const SOCKET client = ::accept(socket, nullptr, nullptr);
        const DWORD timeout = 5000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                     sizeof(timeout));
        return client;
    }
};

bool receive(SOCKET s, u8* out, int size) {
    while (size > 0) {
        const int got = ::recv(s, reinterpret_cast<char*>(out), size, 0);
        if (got <= 0) {
            return false;
        }
        out += got;
        size -= got;
    }
    return true;
}

void sendAll(SOCKET s, const Bytes& data) {
    ::send(s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0);
}

constexpr u8 kPrimeLe[32] = {0xB7, 0x9B, 0x3E, 0x2A, 0x87, 0x82, 0x3C, 0xAB, 0x8F, 0x5E, 0xBF,
                             0xBF, 0x8E, 0xB1, 0x01, 0x08, 0x53, 0x50, 0x06, 0x29, 0x8B, 0x5B,
                             0xAD, 0xBD, 0x5B, 0x53, 0xE1, 0x89, 0x5E, 0x64, 0x4B, 0x89};

// realmd for one connection: the logon challenge and proof of TESTER / SECRET
// (AuthSocket::_HandleLogonChallenge, _HandleLogonProof for build 5875), then the realm list each time it is
// asked for. Counts the lists it sent.
void serveRealmd(const Listener& listener, const Bytes& listBody, std::atomic<u32>& lists,
                 std::atomic<bool>& stop) {
    const SOCKET client = listener.accept(10000);
    if (client == INVALID_SOCKET) {
        return;
    }
    u8 head[4];
    Bytes body;
    if (receive(client, head, 4)) { // cmd, error, size
        body.resize(static_cast<usize>(head[2] | (head[3] << 8)));
        receive(client, body.data(), static_cast<int>(body.size()));
    }
    std::array<u8, 32> salt{};
    for (u32 i = 0; i < 32; ++i) {
        salt[i] = static_cast<u8>(i * 7 + 1);
    }
    std::array<u8, 19> serverKey{};
    for (u32 i = 0; i < 19; ++i) {
        serverKey[i] = static_cast<u8>(i * 13 + 5);
    }
    const std::array<u8, 32> verifier = srpVerifier(srpCredentials("TESTER", "SECRET"), salt);
    Result<SrpServer> server = srpServerChallenge(verifier, serverKey);
    Bytes out;
    ByteWriter w(out);
    w.u8v(0x00).u8v(0x00).u8v(0x00).bytes(server.value().B).u8v(1).u8v(7).u8v(32).bytes(kPrimeLe).bytes(salt);
    const u8 versionChallenge[16] = {};
    w.bytes(versionChallenge).u8v(0);
    sendAll(client, out);
    u8 proof[75]; // cmd, A, M1, crc hash, number of keys, security flags
    if (receive(client, proof, sizeof(proof))) {
        if (!srpServerProof(server.value(), "TESTER", verifier, salt, serverKey,
                            std::span<const u8, 32>(proof + 1, 32))
                 .ok()) {
            ::closesocket(client);
            return;
        }
        out.clear();
        w.u8v(0x01).u8v(0x00).bytes(server.value().M2).u32v(0);
        sendAll(client, out);
        u8 request[5];
        while (!stop.load() && receive(client, request, sizeof(request)) && request[0] == kCmdRealmList) {
            out.clear();
            w.u8v(kCmdRealmList).u16v(static_cast<u16>(listBody.size())).bytes(listBody);
            sendAll(client, out);
            ++lists;
        }
    }
    ::closesocket(client);
}

Bytes twoRealms(u16 alphaPort, u16 betaPort) {
    Bytes body;
    ByteWriter w(body);
    w.u32v(0).u8v(2);
    const std::string alpha = "127.0.0.1:" + std::to_string(alphaPort);
    const std::string beta = "127.0.0.1:" + std::to_string(betaPort);
    w.u32v(1).u8v(0).cstring("Alpha").cstring(alpha).f32v(0.5f).u8v(2).u8v(1).u8v(0);
    w.u32v(6).u8v(0).cstring("Beta").cstring(beta).f32v(1.5f).u8v(0).u8v(1).u8v(0);
    w.u16v(2);
    return body;
}

LoginParams params(u16 realmdPort, std::string realmName) {
    LoginParams p;
    p.realmdHost = "127.0.0.1";
    p.realmdPort = realmdPort;
    p.account = "TESTER";
    p.credentials = srpCredentials("TESTER", "SECRET");
    p.realmName = std::move(realmName);
    return p;
}

// Polls the client until `done` says so (or 8 s); counts the realm lists and the states it saw.
template <typename Done> bool pump(NetClient& client, u32& listsSeen, bool& choosing, Done done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < until) {
        NetEventKind kind{};
        u16 opcode = 0;
        std::span<const u8> payload;
        while (client.poll(kind, opcode, payload)) {
            if (kind == NetEventKind::RealmList) {
                const std::optional<std::vector<RealmInfo>> realms = parseRealmList(payload);
                CHECK(realms.has_value());
                CHECK((realms && realms->size() == 2));
                ++listsSeen;
            } else if (kind == NetEventKind::State &&
                       static_cast<NetState>(opcode) == NetState::ChoosingRealm) {
                choosing = true;
            }
        }
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST_CASE("Realm choice: two realms, none kept: the player chooses, then that world server") {
    const Listener realmd, alpha, beta;
    const Bytes body = twoRealms(alpha.port, beta.port);
    std::atomic<u32> lists{0};
    std::atomic<bool> stop{false};
    std::thread server([&] { serveRealmd(realmd, body, lists, stop); });
    NetClient client;
    client.login(params(realmd.port, ""));
    u32 listsSeen = 0;
    bool choosing = false;
    // The login's list, and the client waiting at ChoosingRealm with realmd still open.
    CHECK(pump(client, listsSeen, choosing, [&] { return choosing && listsSeen >= 1; }));
    CHECK(client.state() == NetState::ChoosingRealm);
    CHECK(client.loginStep() == LoginStep::Authenticated);
    // RealmList_OnUpdate's refresh: the same connection lists again (a second apart).
    client.requestRealmList();
    CHECK(pump(client, listsSeen, choosing, [&] { return listsSeen >= 2; }));
    CHECK(lists.load() == 2);
    // ChangeRealm: Beta's world server gets the connection.
    client.chooseRealm("Beta");
    const SOCKET world = beta.accept(8000);
    CHECK(world != INVALID_SOCKET);
    CHECK(client.realmName() == "Beta");
    ::closesocket(world);
    stop = true;
    client.logout();
    server.join();
}

TEST_CASE("Realm choice: the realm kept is taken by itself") {
    const Listener realmd, alpha, beta;
    const Bytes body = twoRealms(alpha.port, beta.port);
    std::atomic<u32> lists{0};
    std::atomic<bool> stop{false};
    std::thread server([&] { serveRealmd(realmd, body, lists, stop); });
    NetClient client;
    client.login(params(realmd.port, "Alpha"));
    const SOCKET world = alpha.accept(8000);
    CHECK(world != INVALID_SOCKET);
    CHECK(client.realmName() == "Alpha");
    ::closesocket(world);
    stop = true;
    client.logout();
    server.join();
}
#endif
