#pragma once

#include "core/Clock.h"
#include "core/Types.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "net/Messages.h"
#include "net/NetClient.h"
#include "net/RealmListMessages.h"
#include "net/SocialMessages.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vd {

// An order of the server about the player's own movement (phase 6). The player controller applies it and answers
// with GameSession::acknowledge() (a correction needs no answer).
struct MoveCommand {
    enum class Kind : u8 { Teleport, Speed, Root, Unroot, Ability, Knockback, Correction };
    Kind kind = Kind::Correction;
    u64 guid = 0;                                   // phase 13: the unit it is about (the player, or the unit it moves)
    u32 counter = 0;                                // movement counter echoed by the answer
    net::Opcode ack = net::Opcode::MSG_NULL_ACTION; // the answer
    game::SpeedType speed = game::SpeedType::Run;   // Speed: which one
    f32 value = 0.0f;                               // Speed: yards (or radians) per second
    u32 flag = 0;                                   // Ability: MOVEFLAG_* granted or taken away
    bool apply = false;
    f32 knockCos = 0.0f, knockSin = 0.0f, knockSpeedXY = 0.0f, knockSpeedZ = 0.0f; // SMSG_MOVE_KNOCK_BACK
    net::MovementInfo info; // Teleport, Correction: where the server puts the player
};

// Where the server put the player (SMSG_LOGIN_VERIFY_WORLD, SMSG_NEW_WORLD).
struct WorldEntry {
    u32 map = 0;
    glm::dvec3 position{0.0};
    f32 orientation = 0.0f;
    // Phase 18: a login on board a ship or zeppelin (MOVEFLAG_ONTRANSPORT in the player's own create block): which
    // transport, and the player's offset and facing on it; `position` is then the world position of that block (the
    // login packet's is the offset, as 1.12 sends it).
    u64 transport = 0;
    glm::vec3 transportOffset{0.0f};
    f32 transportFacing = 0.0f;
};

// The server session seen from the main thread (phase 4): login -> character list -> world, the entity store
// fed by the world packets, reconnection, and its debug panel. Network I/O itself runs on NetClient's thread.
// Phase 7: the gameplay packets go to game::Gameplay (play()), which the HUD reads and drives.
class GameSession {
public:
    GameSession();
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    // Client tables for spell targets, cooldowns and reactions (both may be null; they must outlive the session).
    void setGameData(const game::GameData* data, const game::Reactions* reactions) {
        m_play.setData(data, reactions);
    }
    // Combat, spells, auras, NPC dialogs, quests, loot, chat, experience, death: state and actions for the HUD.
    // Its actions use the clock of the last update(): call them after it in the frame.
    game::Gameplay& play() { return m_play; }
    const game::Gameplay& play() const { return m_play; }

    // `character`: logged in automatically when the list arrives (empty: wait for a click).
    void configure(const net::LoginParams& params, std::string character);
    bool hasCredentials() const { return m_haveCredentials; }
    void login();
    void logout(); // the player's choice: no reconnection, no automatic character next time
    // Parity (popups): the game menu's Logout / Exit Game. CMSG_LOGOUT_REQUEST (game::Logout): refused, at
    // once or after the server's 20 s; SMSG_LOGOUT_COMPLETE brings the character list back on the same
    // connection, or, after `quit`, ends the session and quitRequested() turns true. False when not in the
    // world or already asked.
    bool requestLogout(bool quit);
    // QUIT's "Exit now" (ForceQuit): the session ends at once (the server saves the character as the socket
    // closes).
    void forceQuit();
    bool quitRequested() const { return m_quitRequested; }

    // The login screen (phase 7). The password only lives until its SRP6 hash is computed here.
    enum class Phase : u8 { LoggedOut, Connecting, CharacterSelect, EnteringWorld, InWorld };
    Phase phase() const { return m_phase; }
    // `server`: "host" or "host:port" of the realm server (net::parseRealmdAddress). False (and a status) when it
    // cannot be read.
    bool loginWith(std::string_view account, std::string_view password, std::string_view server);
    const net::LoginParams& loginParams() const { return m_params; }
    std::span<const net::CharacterInfo> characters() const { return m_characters; }
    void enterCharacter(u32 index);
    // What the login screen says, in the player's language (core/Text): the last failure (statusIsError()), or what
    // is happening.
    std::string_view statusText() const;
    bool statusIsError() const { return !m_status.empty() && !m_reconnectPending; }
    bool reconnecting() const { return m_reconnectPending; }
    std::string_view realmName() const { return m_realmName; }
    std::string_view characterName() const { return m_playerName; } // phase 15 (options): the one entered
    // Character creation and deletion at the character list (phase 8). The server's answer is a ResponseCode
    // (net/ResponseCodes.gen.h) in charResult() with charResultVersion() bumped; after a success the list is asked
    // for again.
    bool createCharacter(const net::CharCreateRequest& request);
    bool deleteCharacter(u64 guid);
    u8 charResult() const { return m_charResult; }
    u32 charResultVersion() const { return m_charResultVersion; }

    // Phase 18 (glue): what the glue screens (app/GlueScreens) read and do besides the above.
    // The last failure was shown (GlueDialog's OKAY): forgotten.
    void clearStatus() { m_status.clear(); }
    // The last realm connected to (1.12's realmName): the next login connects to it by itself when it is listed.
    void setPreferredRealm(std::string_view name) { m_params.realmName.assign(name); }
    net::NetState netState() const { return m_net.state(); }
    net::LoginStep loginStep() const { return m_net.loginStep(); }
    u32 queuePosition() const { return m_queuePosition; }
    // CMSG_CHAR_ENUM asked, its answer not here yet (CHAR_LIST_RETRIEVING); characterListVersion() bumps with each.
    bool characterListPending() const { return m_charListPending; }
    u32 characterListVersion() const { return m_charListVersion; }
    // RealmList (RealmList.xml): the realms realmd listed last, shown while realmListShown(): at the login when the
    // player must choose (net::pickRealm), or asked for from the character list (Change Realm, the queue's button;
    // REALM_LIST_IN_PROGRESS while realmListPending()). A list that could not be had leaves REALM_LIST_FAILED as the
    // status.
    std::span<const net::RealmInfo> realms() const { return m_realms; }
    u32 realmListVersion() const { return m_realmListVersion; }
    bool realmListShown() const { return m_realmListShown; }
    bool realmListPending() const { return m_realmListPending; }
    void requestRealmList();
    void refreshRealmList(); // RealmList_OnUpdate: every REALM_LIST_REFRESH_TIME while shown
    // ChangeRealm: that realm's world server (from the character list: the session moves; its own realm: nothing).
    void chooseRealm(std::string_view name);
    // RealmList_OnCancel: at the login, back to the login screen; from the character list, the list closes.
    void cancelRealmList();
    // CharacterRenameDialog (a character with net::kCharacterFlagRename): CMSG_CHAR_RENAME. The answer's ResponseCode
    // in renameResult() with renameVersion() bumped; on success the list's character has its new name.
    bool renameCharacter(u64 guid, std::string_view name);
    u8 renameResult() const { return m_renameResult; }
    u32 renameVersion() const { return m_renameVersion; }
    // Phase 18 (extras): the last character renamed with success, its old and new names (its WTF folders follow:
    // WoW.exe 0x4725F0, app/AccountDataFiles).
    std::string_view renamedFrom() const { return m_renamedFrom; }
    std::string_view renamedTo() const { return m_renamedTo; }

    // Once per frame. Returns the new position when the player entered a world (or changed map) this frame.
    std::optional<WorldEntry> update(u32 nowMs);
    // Phase 8: the clock (ms) of the last update().
    u32 lastUpdateMs() const { return m_lastUpdateMs; }
    // Phase 18 (p3): GetNetStats' latency, the mean of the last pings' round trips (0 offline).
    u32 latencyMs() const { return m_net.latencyMs(); }
    // Game time from the server (SMSG_LOGIN_SETTIMESPEED), in half-minutes since midnight; nullopt until known.
    std::optional<f32> serverTimeOfDay() const { return m_timeOfDay; }
    // Phase 18 (p3): GetGameTime at `nowMs`: the login's time of day plus the time since, at the server's speed (the f32
    // of SMSG_LOGIN_SETTIMESPEED: game minutes per real second, 1/60 on vMaNGOS), in half-minutes since midnight.
    std::optional<f32> gameTimeNow(u32 nowMs) const;
    // Phase 11: the server's date as whole days since 2000-01-01 (the Blue Child's cycle runs on it); nullopt
    // until known.
    std::optional<u32> serverDay() const { return m_day; }
    // Phase 13 (quests): the server's unix time (the timed quests' countdowns): this device's clock, corrected by the
    // server's date of SMSG_LOGIN_SETTIMESPEED when the two disagree by more than a couple of minutes.
    u64 serverUnixTime() const;

    void drawPanel();
    // Entities as wire boxes over the scene (ImGui foreground list). `viewProj` is camera-relative. Entities for
    // which hasModel(context, guid) says true are drawn as models instead and skipped.
    void drawEntities(const glm::mat4& viewProj, const glm::dvec3& camera, f32 width, f32 height, u32 nowMs,
                      bool (*hasModel)(const void* context, u64 guid) = nullptr, const void* context = nullptr) const;

    bool inWorld() const { return m_phase == Phase::InWorld; }
    // The player's position now (server coordinates = world coordinates), if known.
    std::optional<glm::vec3> playerPosition(u32 nowMs) const;
    const game::EntityStore& entities() const { return m_entities; }
    // Phase 18 (motion, development): a world packet handled as if the server had sent it (app/MotionDemo: synthetic
    // units offline).
    void injectPacket(net::Opcode opcode, std::span<const u8> payload, u32 nowMs) {
        onPacket(static_cast<u16>(opcode), payload, nowMs);
    }
    u64 playerGuid() const { return m_playerGuid; }
    u32 map() const { return m_map; }

    // The player's own movement (phase 6). Orders of the server, oldest first.
    bool pollMoveCommand(MoveCommand& out);
    // MSG_MOVE_* of the player: its MovementInfo (client time, position, flags...).
    void sendMovement(net::Opcode opcode, const net::MovementInfo& info);
    // The answer to `command` with the player's state once applied.
    void acknowledge(const MoveCommand& command, const net::MovementInfo& info);
    // The player's entity follows the local simulation (the server does not echo it back).
    void setPlayerMovement(const net::MovementInfo& info, u32 nowMs);
    // Phase 13 (combat): the same for whichever unit the client moves (a Mind Controlled one, the pet with Eyes of the
    // Beast).
    void setMoverMovement(u64 guid, const net::MovementInfo& info, u32 nowMs);
    // Phase 8: CMSG_MOVE_SPLINE_DONE: where the player's copy of the server's spline `splineId` ended.
    void sendSplineDone(const net::MovementInfo& info, u32 splineId);
    // A far teleport is on its way (SMSG_TRANSFER_PENDING until SMSG_NEW_WORLD): the loading screen.
    bool transferring() const { return m_transferring; }
    // Development: says `text` (CMSG_MESSAGECHAT); for a GM account a leading '.' makes it a server command.
    void say(std::string_view text);
    // The player's target (phase 7): told to the server (CMSG_SET_SELECTION); forgotten when it leaves.
    void setTarget(u64 guid) { m_play.setTarget(guid); }
    u64 target() const { return m_play.target(); }

    // Static data the server describes on request (cached for the whole session; asked for once when missing).
    // nullptr until the answer arrives; the pointers stay valid for the session.
    const net::ItemQueryInfo* item(u32 entry) { return m_play.queries().item(entry); }
    const std::string* playerName(u64 guid) { return m_play.queries().playerName(guid); }
    const net::CreatureQueryInfo* creature(u32 entry) { return m_play.queries().creature(entry); }
    const game::GameObjectInfo* gameObject(u32 entry) { return m_play.queries().gameObject(entry); }
    // Bumped whenever an answer arrives: whoever got nullptr above can try again.
    u32 queryVersion() const { return m_play.queries().version(); }
    // For the periodic log: "online 2 ms | 1234 packets, 0 errors, 0 reconnects | 97 entities".
    void describe(char* out, usize size) const;

private:
    void onState(net::NetState state, std::string_view error);
    void onRealmList(std::span<const u8> body); // phase 18 (glue): a NetEventKind::RealmList
    std::optional<WorldEntry> onPacket(u16 opcode, std::span<const u8> payload, u32 nowMs);
    void enter(const net::CharacterInfo& character);
    void pushMoveCommand(const MoveCommand& command);
    // SMSG_FORCE_* / SMSG_MOVE_* for the player: packed GUID + counter (+ `extraFloats`). False if malformed.
    bool readForcedChange(std::span<const u8> payload, MoveCommand& command, u32 extraFloats, f32* extra);

    net::NetClient m_net;
    net::LoginParams m_params;
    bool m_haveCredentials = false;
    std::string m_autoCharacter;
    Phase m_phase = Phase::LoggedOut;
    std::vector<net::CharacterInfo> m_characters;
    u64 m_playerGuid = 0;
    std::string m_playerName;
    u32 m_map = 0;
    game::EntityStore m_entities;
    game::Gameplay m_play{m_entities};  // after m_entities: it keeps a reference
    std::vector<u64> m_created;         // GUIDs created by the last SMSG_UPDATE_OBJECT
    std::array<bool, net::kOpcodeCount> m_isMovement{}; // MSG_MOVE_* broadcasts of other objects
    std::array<MoveCommand, 32> m_moveCommands{};      // FIFO ring
    u32 m_moveFirst = 0;
    u32 m_moveCount = 0;
    std::vector<u8> m_sendScratch;
    bool m_transferring = false;
    // Phase 18: SMSG_LOGIN_VERIFY_WORLD waits for the player's own create block (right behind it), which says whether
    // the character logs in on a transport.
    std::optional<WorldEntry> m_loginEntry;
    u32 m_loginEntryAtMs = 0;
    u32 m_lastUpdateMs = 0;
    std::array<char, 128> m_commandInput{};
    std::optional<f32> m_timeOfDay;
    u32 m_timeOfDayMs = 0;              // phase 18 (p3): when it came
    f32 m_timeSpeed = 1.0f / 60.0f;     // phase 18 (p3): game minutes per real second
    std::optional<u32> m_day;
    i64 m_serverClockOffset = 0; // phase 13: seconds the server's clock is ahead of this device's
    std::string m_status; // the last failure, worded for the player
    mutable std::array<char, 96> m_statusScratch{}; // statusText() of the phases
    u32 m_queuePosition = 0;  // in the realm's login queue (SMSG_AUTH_RESPONSE AUTH_WAIT_QUEUE)
    std::string m_realmName; // of the session, once online
    u8 m_charResult = 0;
    u32 m_charResultVersion = 0;
    // Phase 18 (glue).
    bool m_charListPending = false;
    u32 m_charListVersion = 0;
    std::vector<net::RealmInfo> m_realms;
    u32 m_realmListVersion = 0;
    bool m_realmListShown = false;
    bool m_realmListPending = false;
    u8 m_renameResult = 0;
    u32 m_renameVersion = 0;
    std::string m_renamedFrom; // phase 18 (extras)
    std::string m_renamedTo;
    bool m_quitRequested = false; // parity (popups): Exit Game went through

    // Reconnection after an unexpected disconnection: 5 s, then doubling up to a minute. Phase 18 (glue): only once
    // the session was online (a login that fails is told, not retried).
    bool m_wantOnline = false;
    bool m_hadSession = false;
    bool m_reconnectPending = false;
    Clock::time_point m_reconnectAt{};
    f64 m_reconnectDelay = 5.0;
    u32 m_reconnects = 0;
    Clock::time_point m_onlineSince{};

    u32 m_packets = 0;
    u32 m_parseErrors = 0;
    bool m_traceUnhandled = false; // VD_TRACE_UNHANDLED=1 (development): world packets nothing consumed, to the log
    bool m_showEntities = true;
    std::array<char, 32> m_accountInput{}; // temporary login form (phase 4; the real one comes with the UI)
    std::array<char, 64> m_passwordInput{};
};

} // namespace vd
