#include "net/NetClient.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "net/ByteBuffer.h"
#include "net/Messages.h"
#include "net/RealmClient.h"
#include "net/ResponseCodes.gen.h"
#include "platform/Thread.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>

namespace vd::net {
namespace {

constexpr u32 kIncomingBytes = 4u << 20; // a whole city's worth of updates while the main thread loads
constexpr u32 kOutgoingBytes = 256u << 10;
constexpr u32 kPollMs = 5;             // longest wait before queued outgoing packets leave
constexpr f64 kPingSeconds = 30.0;     // the server counts pings closer than 27 s as "overspeed"

bool splitAddress(const std::string& address, std::string& host, u16& port) {
    const usize colon = address.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }
    host = address.substr(0, colon);
    port = static_cast<u16>(std::atoi(address.c_str() + colon + 1));
    return port != 0;
}

bool isLoopback(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost";
}

} // namespace

AddressError parseRealmdAddress(std::string_view address, std::string& host, u16& port) {
    while (!address.empty() && (address.front() == ' ' || address.front() == '\t')) {
        address.remove_prefix(1);
    }
    while (!address.empty() && (address.back() == ' ' || address.back() == '\t')) {
        address.remove_suffix(1);
    }
    const usize colon = address.rfind(':');
    const std::string_view name = address.substr(0, colon);
    u32 number = kRealmdPort;
    if (colon != std::string_view::npos) {
        number = 0;
        for (const char c : address.substr(colon + 1)) {
            if (c < '0' || c > '9' || number > 65535) {
                number = 0;
                break;
            }
            number = number * 10 + static_cast<u32>(c - '0');
        }
        if (number == 0 || number > 65535) {
            return name.empty() ? AddressError::NoHost : AddressError::BadPort;
        }
    }
    if (name.empty()) {
        return AddressError::NoHost;
    }
    host.assign(name);
    port = static_cast<u16>(number);
    return AddressError::None;
}

const char* netStateName(NetState state) {
    switch (state) {
    case NetState::Offline: return "offline";
    case NetState::LoggingIn: return "logging in";
    case NetState::Connecting: return "connecting to the world";
    case NetState::Queued: return "queued";
    case NetState::Online: return "online";
    case NetState::ChoosingRealm: return "choosing a realm";
    }
    return "?";
}

NetClient::NetClient() : m_incoming(kIncomingBytes), m_outgoing(kOutgoingBytes) {
    m_thread = std::thread([this] { run(); });
}

NetClient::~NetClient() {
    m_quit.store(true);
    m_wantSession.store(false);
    m_request.fetch_add(1);
    m_request.notify_one();
    m_world.shutdown();
    m_thread.join();
}

void NetClient::login(const LoginParams& params) {
    {
        std::lock_guard<std::mutex> lock(m_paramsMutex);
        m_params = params;
    }
    m_wantSession.store(true);
    m_request.fetch_add(1);
    m_request.notify_one();
    m_world.shutdown(); // a current session ends at once; the thread then starts the new one
}

void NetClient::logout() {
    m_wantSession.store(false);
    m_request.fetch_add(1);
    m_request.notify_one();
    m_world.shutdown();
}

void NetClient::requestRealmList() {
    m_realmListAsked.fetch_add(1);
}

void NetClient::chooseRealm(std::string_view name) {
    {
        std::lock_guard<std::mutex> lock(m_paramsMutex);
        m_chosenRealm.assign(name);
    }
    m_realmChosen.fetch_add(1);
}

void NetClient::closeRealmList() {
    m_realmListClosed.fetch_add(1);
}

std::string NetClient::realmName() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_paramsMutex));
    return m_realmName;
}

bool NetClient::send(Opcode opcode, std::span<const u8> payload) {
    return m_outgoing.push(0, static_cast<u16>(opcode), payload);
}

bool NetClient::poll(NetEventKind& kind, u16& opcode, std::span<const u8>& payload) {
    if (m_frontPending) {
        m_incoming.pop();
        m_frontPending = false;
    }
    u16 rawKind = 0;
    if (!m_incoming.front(rawKind, opcode, payload)) {
        return false;
    }
    kind = static_cast<NetEventKind>(rawKind);
    m_frontPending = true;
    return true;
}

bool NetClient::pushEvent(NetEventKind kind, u16 opcode, std::span<const u8> payload) {
    // Full: the main thread is behind (loading a map). Wait for it; TCP holds the server back meanwhile.
    while (!m_incoming.push(static_cast<u16>(kind), opcode, payload)) {
        if (m_quit.load()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void NetClient::setState(NetState state, std::string_view error) {
    m_state.store(state, std::memory_order_release);
    pushEvent(NetEventKind::State, static_cast<u16>(state), asBytes(error));
}

void NetClient::failed(std::string_view detail, std::string_view shown) {
    VD_LOG_WARN("Network: %.*s", static_cast<int>(detail.size()), detail.data());
    setState(NetState::Offline, shown);
}

bool NetClient::forward(u16 opcode, std::span<const u8> payload) {
    return pushEvent(NetEventKind::Packet, opcode, payload);
}

void NetClient::run() {
    setCurrentThreadName("vd-net");
    for (;;) {
        const u32 request = m_request.load();
        if (m_quit.load()) {
            break;
        }
        if (m_wantSession.load()) {
            LoginParams params;
            {
                std::lock_guard<std::mutex> lock(m_paramsMutex);
                params = m_params;
            }
            session(params);
            m_world.close();
            if (m_state.load() != NetState::Offline) {
                setState(NetState::Offline);
            }
            if (m_request.load() == request) {
                m_wantSession.store(false); // ended on its own (error, kick): wait for a new login()
            }
        }
        if (m_request.load() == request) {
            m_request.wait(request); // sleeps until login(), logout() or shutdown
        }
    }
}

// Phase 18 (glue): realmd's connection of the network thread: logged in (its session key is the world server's), and
// the last realm list it sent.
struct NetClient::RealmdLink {
    Socket socket;
    bool loggedIn = false;
    RealmSession session;
    std::vector<u8> body;
    std::vector<RealmInfo> realms;
    Clock::time_point listedAt{};
    bool listed = false;

    void close() {
        socket.close();
        loggedIn = false;
        listed = false;
    }
    i32 find(std::string_view name) const {
        for (usize i = 0; i < realms.size(); ++i) {
            if (realms[i].name == name) {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }
};

bool NetClient::openRealmd(const LoginParams& params, RealmdLink& link, bool atLogin) {
    link.close();
    if (atLogin) {
        m_loginStep.store(LoginStep::Connecting, std::memory_order_release);
    }
    if (Result<void> r = link.socket.connect(params.realmdHost, params.realmdPort, kNetTimeoutMs); !r) {
        if (atLogin) {
            fail("realmd: " + r.error().message, "VD_LOGIN_CANT_CONNECT", params.realmdHost, params.realmdPort);
        } else {
            VD_LOG_WARN("Network: realmd: %s", r.error().message.c_str());
        }
        return false;
    }
    if (atLogin) {
        m_loginStep.store(LoginStep::Authenticating, std::memory_order_release);
    }
    const char* refusal = nullptr;
    Result<RealmSession> login =
        authenticateRealmd(link.socket, params.account, params.credentials, &refusal, params.locale);
    if (!login) {
        link.close();
        if (atLogin) {
            fail(login.error().message, refusal);
        } else {
            VD_LOG_WARN("Network: realmd: %s", login.error().message.c_str());
        }
        return false;
    }
    link.session = std::move(login.value());
    link.loggedIn = true;
    if (atLogin) {
        m_loginStep.store(LoginStep::Authenticated, std::memory_order_release);
    }
    return true;
}

bool NetClient::listRealms(const LoginParams& params, RealmdLink& link, bool atLogin) {
    // realmd drops a connection that asks again within a second (MinRealmListDelay) and every connection after
    // MaxSessionDuration (300 s): wait out the second, and log in again once when the connection is gone.
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        if (!link.loggedIn && !openRealmd(params, link, false)) {
            break;
        }
        if (link.listed) {
            const f64 since = secondsBetween(link.listedAt, Clock::now());
            if (since < 1.05) {
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<i64>((1.05 - since) * 1000.0)));
            }
        }
        const Result<void> got = fetchRealmList(link.socket, link.body);
        std::optional<std::vector<RealmInfo>> realms = got ? parseRealmList(link.body) : std::nullopt;
        link.listedAt = Clock::now();
        link.listed = true;
        if (realms) {
            link.realms = std::move(*realms);
            pushEvent(NetEventKind::RealmList, 0, link.body);
            return true;
        }
        VD_LOG_WARN("Network: realmd: realm list %s", got ? "malformed" : got.error().message.c_str());
        link.close();
    }
    if (!atLogin) {
        pushEvent(NetEventKind::RealmList, 0, {}); // REALM_LIST_FAILED
    }
    return false;
}

void NetClient::session(const LoginParams& params) {
    const u32 request = m_request.load();
    auto cancelled = [&] { return m_quit.load() || m_request.load() != request; };
    {
        u16 kind = 0, opcode = 0;
        std::span<const u8> payload;
        while (m_outgoing.front(kind, opcode, payload)) { // leftovers of an older session
            m_outgoing.pop();
        }
    }
    RealmRequests seen{m_realmListAsked.load(), m_realmListClosed.load(), m_realmChosen.load()};

    // 1. realmd: the login and the realm list; the realm of the last session, the only one, or the player's choice.
    setState(NetState::LoggingIn);
    RealmdLink link;
    if (!openRealmd(params, link, true)) {
        return;
    }
    if (cancelled()) {
        return;
    }
    if (!listRealms(params, link, true)) {
        fail("realmd sent no realm list", "REALM_LIST_FAILED");
        return;
    }
    if (link.realms.empty()) {
        fail("realmd lists no realms", "REALM_LIST_INVALID");
        return;
    }
    i32 pick = pickRealm(link.realms, params.realmName);
    if (pick < 0) {
        setState(NetState::ChoosingRealm);
        while (pick < 0) {
            if (cancelled()) {
                return;
            }
            if (const u32 closed = m_realmListClosed.load(); closed != seen.closed) {
                VD_LOG_INFO("Network: no realm chosen");
                return;
            }
            if (const u32 chosen = m_realmChosen.load(); chosen != seen.chosen) {
                seen.chosen = chosen;
                pick = link.find(chosenRealm());
                pick = pick >= 0 && link.realms[static_cast<usize>(pick)].offline() ? -1 : pick;
            }
            if (const u32 asked = m_realmListAsked.load(); asked != seen.asked && pick < 0) {
                seen.asked = asked;
                listRealms(params, link, false);
            }
            if (pick < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    RealmInfo chosen = link.realms[static_cast<usize>(pick)];
    RealmSession keys = link.session;
    link.close();

    // 2. world; again with another realm when the player changes realms (CharacterSelect's Change Realm).
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(m_paramsMutex);
            m_realmName = chosen.name;
        }
        std::string worldHost;
        u16 worldPort = 0;
        if (!splitAddress(chosen.address, worldHost, worldPort)) {
            fail("Bad realm address: " + chosen.address, "REALM_LIST_INVALID");
            return;
        }
        // The server wants realmd and world connections from the same address: through adb reverse (realmd on
        // loopback) the world is reached on loopback too, whatever LAN address the realm announces.
        if (isLoopback(params.realmdHost)) {
            worldHost = params.realmdHost;
        }
        if (cancelled()) {
            return;
        }
        setState(NetState::Connecting);
        if (Result<void> r = m_world.open(worldHost, worldPort, params.account,
                                          std::span<const u8>(keys.sessionKey.data(), keys.sessionKeySize));
            !r) {
            fail("world: " + r.error().message, "VD_LOGIN_CANT_CONNECT", worldHost, worldPort);
            return;
        }
        VD_LOG_INFO("Network: world session with %s:%u (%s)", worldHost.c_str(), worldPort, chosen.name.c_str());
        const i32 next = worldSession(params, link, chosen.name, seen, request);
        if (next < 0) {
            return;
        }
        chosen = link.realms[static_cast<usize>(next)];
        keys = link.session;
        link.close();
        m_world.close();
        VD_LOG_INFO("Network: changing realm to %s", chosen.name.c_str());
    }
}

std::string NetClient::chosenRealm() {
    std::lock_guard<std::mutex> lock(m_paramsMutex);
    return m_chosenRealm;
}

i32 NetClient::worldSession(const LoginParams& params, RealmdLink& link, std::string_view realm, RealmRequests& seen,
                            u32 request) {
    auto cancelled = [&] { return m_quit.load() || m_request.load() != request; };
    u16 opcode = 0;
    std::vector<u8> payload;
    payload.reserve(65536);
    u32 pingSequence = 0;
    Clock::time_point lastPing{};
    Clock::time_point pingSent{};
    // Phase 18 (p3): the last 16 round trips; the latency is their mean, as GetNetStats' (WoW.exe 0x537F20 averages its
    // ring of 16 at 0x1A6C), and CMSG_PING tells the server that mean.
    std::array<u32, 16> roundTrips{};
    u32 roundTripCount = 0, roundTripNext = 0;
    bool online = false;
    while (!cancelled()) {
        // Phase 18 (glue): RealmList over the session (Change Realm, or the queue's button): realmd again.
        if (const u32 asked = m_realmListAsked.load(); asked != seen.asked) {
            seen.asked = asked;
            listRealms(params, link, false);
        }
        if (const u32 closed = m_realmListClosed.load(); closed != seen.closed) {
            seen.closed = closed;
            link.close();
        }
        if (const u32 chosen = m_realmChosen.load(); chosen != seen.chosen) {
            seen.chosen = chosen;
            const std::string name = chosenRealm();
            const i32 found = link.find(name);
            // Its world server takes the session key of a login to realmd (closed by realmd after 300 s: again).
            if (found >= 0 && name != realm && !link.realms[static_cast<usize>(found)].offline() &&
                (link.loggedIn || openRealmd(params, link, false))) {
                return found;
            }
            link.close(); // the realm of this session (or one gone): nothing changes
        }
        // Outgoing: only once the session is open (the server drops anything else before AUTH_OK).
        u16 kind = 0, outOpcode = 0;
        std::span<const u8> outPayload;
        while (online && m_outgoing.front(kind, outOpcode, outPayload)) {
            if (Result<void> r = m_world.send(static_cast<Opcode>(outOpcode), outPayload); !r) {
                fail("Connection lost: " + r.error().message, "DISCONNECTED");
                return -1;
            }
            m_outgoing.pop();
        }
        const Clock::time_point now = Clock::now();
        if (online && secondsBetween(lastPing, now) >= kPingSeconds) {
            std::vector<u8> ping;
            ByteWriter(ping).u32v(++pingSequence).u32v(m_latencyMs.load());
            if (Result<void> r = m_world.send(Opcode::CMSG_PING, ping); !r) {
                fail("Connection lost: " + r.error().message, "DISCONNECTED");
                return -1;
            }
            lastPing = pingSent = now;
        }

        Result<bool> got = m_world.receive(opcode, payload, kPollMs);
        if (!got) {
            if (!cancelled()) {
                fail("Connection lost: " + got.error().message, "DISCONNECTED");
            }
            return -1;
        }
        if (!got.value()) {
            continue;
        }
        switch (static_cast<Opcode>(opcode)) {
        case Opcode::SMSG_AUTH_RESPONSE: {
            const std::optional<AuthResponse> response = parseAuthResponse(payload);
            if (response && response->result == static_cast<u8>(ResponseCode::AUTH_OK)) {
                online = true;
                lastPing = Clock::now() - std::chrono::seconds(60); // first ping right away
                setState(NetState::Online);
            } else if (response && response->result == static_cast<u8>(ResponseCode::AUTH_WAIT_QUEUE)) {
                setState(NetState::Queued);
            } else {
                // The client's text for the code: its GlueStrings keys are the ResponseCodes' names (AUTH_*).
                const char* code = response ? responseCodeName(response->result) : "malformed answer";
                const std::string detail = std::string("World session refused: ") + code;
                fail(detail, response && text::has(std::string_view(code)) ? text::Key(std::string_view(code))
                                                                           : text::Key("AUTH_FAILED"));
                return -1;
            }
            forward(opcode, payload);
            break;
        }
        case Opcode::SMSG_PONG: {
            ByteReader r(payload);
            if (r.u32v() == pingSequence) {
                roundTrips[roundTripNext++ % roundTrips.size()] =
                    static_cast<u32>(secondsBetween(pingSent, Clock::now()) * 1000.0);
                roundTripCount = std::min<u32>(roundTripCount + 1, static_cast<u32>(roundTrips.size()));
                u64 sum = 0;
                for (u32 i = 0; i < roundTripCount; ++i) {
                    sum += roundTrips[i];
                }
                m_latencyMs.store(static_cast<u32>(sum / roundTripCount));
            }
            break;
        }
        case Opcode::SMSG_COMPRESSED_UPDATE_OBJECT:
            if (inflatePacket(payload, m_inflated).ok()) {
                forward(static_cast<u16>(Opcode::SMSG_UPDATE_OBJECT), m_inflated);
            } else {
                VD_LOG_WARN("Network: could not inflate SMSG_COMPRESSED_UPDATE_OBJECT (%zu bytes)", payload.size());
            }
            break;
        case Opcode::SMSG_COMPRESSED_MOVES: // [u8 size][u16 opcode][size - 2 bytes]... (MovementData)
            if (inflatePacket(payload, m_inflated).ok()) {
                ByteReader r(m_inflated);
                while (r.remaining() >= 3) {
                    const u8 size = r.u8v();
                    const u16 innerOpcode = r.u16v();
                    const std::span<const u8> inner = r.view(size >= 2 ? size - 2u : 0u);
                    if (r.failed()) {
                        break;
                    }
                    forward(innerOpcode, inner);
                }
            }
            break;
        default: forward(opcode, payload); break;
        }
    }
    return -1;
}

} // namespace vd::net
