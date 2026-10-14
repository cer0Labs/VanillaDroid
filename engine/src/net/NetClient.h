#pragma once

#include "core/Text.h"
#include "core/Types.h"
#include "net/Crypto.h"
#include "net/Opcodes.gen.h"
#include "net/PacketRing.h"
#include "net/WorldConnection.h"

#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace vd::net {

enum class NetState : u8 {
    Offline,        // no session (never started, logged out, or failed: see lastError())
    LoggingIn,      // realmd: SRP6 login and realm list
    Connecting,     // world: connection, session handshake
    Queued,         // world: waiting in the login queue
    Online,         // world session open: packets flow both ways
    ChoosingRealm,  // phase 18 (glue): realmd logged in, the realm list shown (RealmList): waiting for the choice
};
const char* netStateName(NetState state);

// Phase 18 (glue): where realmd's login is (LoggingIn), for the status the glue shows (LOGIN_STATE_*).
enum class LoginStep : u8 {
    None,
    Connecting,     // LOGIN_STATE_CONNECTING
    Authenticating, // LOGIN_STATE_AUTHENTICATING
    Authenticated,  // LOGIN_STATE_AUTHENTICATED: logged in, the realm list on its way
};

// The realm server's port when its address has none (1.12's realmlist: "set realmlist logon.example.org").
constexpr u16 kRealmdPort = 3724;

// A realm server's address as a player writes it (1.12's realmlist): "host" or "host:port", the spaces around
// it ignored. NoHost: nothing before the port (or nothing at all); BadPort: a port that is not 1..65535.
enum class AddressError : u8 { None, NoHost, BadPort };
AddressError parseRealmdAddress(std::string_view address, std::string& host, u16& port);

struct LoginParams {
    std::string realmdHost; // empty: no server set yet (the login screen asks for one)
    u16 realmdPort = kRealmdPort;
    std::string account;
    Sha1Digest credentials{}; // srpCredentials(account, password): the password itself is never kept
    // The last realm connected to (1.12's realmName CVar): connected to by itself when listed and up; empty, or not
    // listed: the only realm when there is one, else the player chooses (net::pickRealm).
    std::string realmName;
    // Phase 21 (languages): the client's language as AUTH_LOGON_CHALLENGE tells it ("esES"): vMaNGOS answers item,
    // creature, quest, gossip and its own messages in it (its locales_* tables) where it has them.
    std::string locale = "enUS";
};

// What the network thread hands to the main thread.
enum class NetEventKind : u16 {
    Packet = 1, // a world packet (compressed ones arrive inflated, as their plain opcode)
    State = 2,  // `opcode` holds the new NetState; payload: what the player is told, when it went Offline on a failure
    // Phase 18 (glue): realmd's realm list (payload: CMD_REALM_LIST's body, net::parseRealmList); an empty payload when
    // it could not be had (REALM_LIST_FAILED).
    RealmList = 3,
};

// The network thread ("vd-net"): realmd login, world session, keep-alive pings, inflating compressed
// packets. It talks to the main thread only through two lock-free packet rings, so neither side ever
// waits on the other.
class NetClient {
public:
    NetClient();
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // Starts a new session (dropping the current one, if any).
    void login(const LoginParams& params);
    void logout();

    // Queues a world packet. False when the outgoing ring is full.
    bool send(Opcode opcode, std::span<const u8> payload);
    // Next event; its payload stays valid until the next call.
    bool poll(NetEventKind& kind, u16& opcode, std::span<const u8>& payload);

    NetState state() const { return m_state.load(std::memory_order_acquire); }
    u32 latencyMs() const { return m_latencyMs.load(std::memory_order_relaxed); }
    std::string realmName() const;
    LoginStep loginStep() const { return m_loginStep.load(std::memory_order_acquire); } // phase 18 (glue)

    // Phase 18 (glue): RealmList. While choosing at login (ChoosingRealm) and once online (CharacterSelect's Change
    // Realm), the network thread keeps realmd's connection open (logging in again when it is not) and sends each list
    // asked for (RealmList events). chooseRealm connects to that realm's world server (online: the current session
    // ends first; the current realm itself changes nothing); closeRealmList closes realmd (at login: the session ends,
    // back to the login screen).
    void requestRealmList();
    void chooseRealm(std::string_view name);
    void closeRealmList();

private:
    void run();
    void session(const LoginParams& params);
    // Phase 18 (glue): realmd's connection of the network thread, and the main thread's realm list requests it has
    // seen.
    struct RealmdLink;
    struct RealmRequests {
        u32 asked = 0;
        u32 closed = 0;
        u32 chosen = 0;
    };
    // Logs in (at the login: its steps and failures are the session's).
    bool openRealmd(const LoginParams& params, RealmdLink& link, bool atLogin);
    // The list (logging in again when the connection is gone) to the main thread; false when there is none (outside
    // the login: an empty RealmList event).
    bool listRealms(const LoginParams& params, RealmdLink& link, bool atLogin);
    std::string chosenRealm();
    // The world session on m_world until it ends (-1) or the player chooses another realm of `link` (its index).
    i32 worldSession(const LoginParams& params, RealmdLink& link, std::string_view realm, RealmRequests& seen,
                     u32 request);
    bool pushEvent(NetEventKind kind, u16 opcode, std::span<const u8> payload);
    void setState(NetState state, std::string_view error = {});
    // Offline after a failure: the player is told `key`'s text (core/Text, worded on this thread: the table is
    // read-only), the log gets the technical `detail`.
    template <typename... Args> void fail(std::string_view detail, text::Key key, const Args&... args) {
        char shown[512];
        failed(detail, text::format(shown, key, args...));
    }
    void failed(std::string_view detail, std::string_view shown);
    bool forward(u16 opcode, std::span<const u8> payload);

    std::thread m_thread;
    std::atomic<bool> m_quit{false};
    std::atomic<u32> m_request{0}; // bumped by login()/logout(); the thread wakes on it
    std::atomic<bool> m_wantSession{false};
    std::mutex m_paramsMutex; // login parameters: set once per login, read once by the thread
    LoginParams m_params;
    std::string m_realmName;

    PacketRing m_incoming; // network thread -> main thread
    PacketRing m_outgoing; // main thread -> network thread
    bool m_frontPending = false; // main thread: an incoming record to pop on the next poll()
    WorldConnection m_world;
    std::atomic<NetState> m_state{NetState::Offline};
    std::atomic<u32> m_latencyMs{0};
    // Phase 18 (glue): the realm list's requests (bumped by the main thread, compared by the network thread).
    std::atomic<LoginStep> m_loginStep{LoginStep::None};
    std::atomic<u32> m_realmListAsked{0};
    std::atomic<u32> m_realmListClosed{0};
    std::atomic<u32> m_realmChosen{0};
    std::string m_chosenRealm; // under m_paramsMutex
    std::vector<u8> m_inflated; // network thread scratch
};

} // namespace vd::net
