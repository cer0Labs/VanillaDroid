#include "app/GameSession.h"

#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Text.h"
#include "game/CharacterNames.h"
#include "game/QuestJournal.h"
#include "net/ByteBuffer.h"
#include "net/CharRenameMessages.h"
#include "net/ResponseCodes.gen.h"
#include "net/TravelMessages.h"
#include "net/UpdateFields.gen.h"
#include "world/Celestial.h"

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace vd {
namespace {

using net::Opcode;

// Movement broadcasts of other objects: packed GUID + MovementInfo (MovementHandler.cpp).
constexpr Opcode kMovementOpcodes[] = {
    Opcode::MSG_MOVE_START_FORWARD,      Opcode::MSG_MOVE_START_BACKWARD,     Opcode::MSG_MOVE_STOP,
    Opcode::MSG_MOVE_START_STRAFE_LEFT,  Opcode::MSG_MOVE_START_STRAFE_RIGHT, Opcode::MSG_MOVE_STOP_STRAFE,
    Opcode::MSG_MOVE_JUMP,               Opcode::MSG_MOVE_START_TURN_LEFT,    Opcode::MSG_MOVE_START_TURN_RIGHT,
    Opcode::MSG_MOVE_STOP_TURN,          Opcode::MSG_MOVE_START_PITCH_UP,     Opcode::MSG_MOVE_START_PITCH_DOWN,
    Opcode::MSG_MOVE_STOP_PITCH,         Opcode::MSG_MOVE_SET_RUN_MODE,       Opcode::MSG_MOVE_SET_WALK_MODE,
    Opcode::MSG_MOVE_FALL_LAND,          Opcode::MSG_MOVE_START_SWIM,         Opcode::MSG_MOVE_STOP_SWIM,
    Opcode::MSG_MOVE_SET_FACING,         Opcode::MSG_MOVE_SET_PITCH,          Opcode::MSG_MOVE_HEARTBEAT,
    // Broadcasts of changes to other players (the extra data after the MovementInfo is not needed).
    Opcode::MSG_MOVE_TELEPORT,           Opcode::MSG_MOVE_ROOT,               Opcode::MSG_MOVE_UNROOT,
    Opcode::MSG_MOVE_KNOCK_BACK,         Opcode::MSG_MOVE_WATER_WALK,         Opcode::MSG_MOVE_FEATHER_FALL,
    Opcode::MSG_MOVE_HOVER,
};

// MOVEFLAG_* granted by the server (MovementInfo.h).
constexpr u32 kMoveFlagWaterWalking = 0x10000000;
constexpr u32 kMoveFlagSafeFall = 0x20000000;
constexpr u32 kMoveFlagHover = 0x40000000;

constexpr f32 kEntityDrawDistance = 150.0f;

f32 asFloat(u32 bits) {
    f32 value = 0.0f;
    std::memcpy(&value, &bits, 4);
    return value;
}

} // namespace

GameSession::GameSession() {
    m_traceUnhandled = VD_DEV_ENV("VD_TRACE_UNHANDLED") != nullptr;
    for (Opcode opcode : kMovementOpcodes) {
        m_isMovement[static_cast<u16>(opcode)] = true;
    }
    game::PacketSink sink;
    sink.context = this;
    sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
        return static_cast<GameSession*>(context)->m_net.send(opcode, body);
    };
    m_play.setSink(sink);
    m_created.reserve(512);
}

void GameSession::configure(const net::LoginParams& params, std::string character) {
    m_params = params;
    m_haveCredentials = !params.account.empty() && params.credentials != net::Sha1Digest{};
    m_autoCharacter = std::move(character);
}

void GameSession::login() {
    if (!m_haveCredentials) {
        return;
    }
    if (m_params.realmdHost.empty()) { // no realm server set
        m_status.clear();
        text::append(m_status, "VD_LOGIN_ENTER_SERVER");
        return;
    }
    m_wantOnline = true;
    m_reconnectPending = false;
    m_hadSession = false; // phase 18 (glue): a login that fails says why (GlueDialog's OKAY) instead of retrying
    m_phase = Phase::Connecting;
    m_status.clear();
    m_net.login(m_params);
}

void GameSession::logout() {
    m_wantOnline = false;
    m_reconnectPending = false;
    m_autoCharacter.clear(); // back at the list, the player chooses
    m_net.logout();
}

bool GameSession::requestLogout(bool quit) {
    if (m_phase != Phase::InWorld || !m_play.logout().request(quit, m_lastUpdateMs)) {
        return false;
    }
    m_autoCharacter.clear(); // back at the list, the player chooses
    VD_LOG_INFO("Logout requested%s", quit ? " (exit game)" : "");
    return true;
}

void GameSession::forceQuit() {
    logout();
    m_quitRequested = true;
}

bool GameSession::loginWith(std::string_view account, std::string_view password, std::string_view server) {
    net::LoginParams params = m_params;
    const net::AddressError address = net::parseRealmdAddress(server, params.realmdHost, params.realmdPort);
    if (address == net::AddressError::BadPort) {
        m_status.clear();
        text::append(m_status, "VD_LOGIN_BAD_PORT");
        return false;
    }
    if (address != net::AddressError::None || account.empty() || password.empty()) {
        m_status.clear();
        text::append(m_status, account.empty()    ? text::Key("LOGIN_ENTER_NAME")
                                : password.empty() ? text::Key("LOGIN_ENTER_PASSWORD")
                                                   : text::Key("VD_LOGIN_ENTER_SERVER"));
        return false;
    }
    params.account.assign(account);
    params.credentials = net::srpCredentials(params.account, password);
    configure(params, m_autoCharacter);
    login();
    return true;
}

bool GameSession::createCharacter(const net::CharCreateRequest& request) {
    if (m_phase != Phase::CharacterSelect) {
        return false;
    }
    m_sendScratch.clear();
    net::writeCharCreate(m_sendScratch, request);
    return m_net.send(Opcode::CMSG_CHAR_CREATE, m_sendScratch);
}

bool GameSession::deleteCharacter(u64 guid) {
    if (m_phase != Phase::CharacterSelect || guid == 0) {
        return false;
    }
    m_sendScratch.clear();
    net::ByteWriter(m_sendScratch).u64v(guid);
    return m_net.send(Opcode::CMSG_CHAR_DELETE, m_sendScratch);
}

void GameSession::requestRealmList() {
    if (m_phase == Phase::CharacterSelect || m_net.state() == net::NetState::Queued) {
        m_realmListPending = true;
        m_net.requestRealmList();
    }
}

void GameSession::refreshRealmList() {
    if (m_realmListShown) {
        m_net.requestRealmList();
    }
}

void GameSession::chooseRealm(std::string_view name) {
    if (!m_realmListShown) {
        return;
    }
    m_realmListShown = false;
    VD_LOG_INFO("Realm chosen: %.*s", static_cast<int>(name.size()), name.data());
    m_net.chooseRealm(name);
}

void GameSession::cancelRealmList() {
    m_realmListShown = false;
    m_realmListPending = false;
    if (m_phase == Phase::Connecting && m_net.state() == net::NetState::ChoosingRealm) {
        logout(); // RealmListDialogCancelled at the login: no session
    } else {
        m_net.closeRealmList();
    }
}

void GameSession::onRealmList(std::span<const u8> body) {
    std::optional<std::vector<net::RealmInfo>> realms = body.empty() ? std::nullopt : net::parseRealmList(body);
    if (!realms) {
        if (m_realmListPending) {
            m_realmListPending = false;
            m_status.clear();
            text::append(m_status, "REALM_LIST_FAILED");
        }
        return;
    }
    m_realms = std::move(*realms);
    ++m_realmListVersion;
    if (m_realmListPending) {
        m_realmListPending = false;
        m_realmListShown = true;
    }
}

bool GameSession::renameCharacter(u64 guid, std::string_view name) {
    if (m_phase != Phase::CharacterSelect || guid == 0) {
        return false;
    }
    m_sendScratch.clear();
    net::writeCharRename(m_sendScratch, guid, name);
    return m_net.send(Opcode::CMSG_CHAR_RENAME, m_sendScratch);
}

void GameSession::enterCharacter(u32 index) {
    if (m_phase == Phase::CharacterSelect && index < m_characters.size()) {
        enter(m_characters[index]);
    }
}

std::string_view GameSession::statusText() const {
    if (m_reconnectPending) {
        return text::get("VD_LOGIN_RECONNECTING");
    }
    if (!m_status.empty()) {
        return m_status;
    }
    // The original login screen's states (GlueStrings).
    switch (m_phase) {
    case Phase::Connecting:
        switch (m_net.state()) {
        case net::NetState::LoggingIn: return text::get("LOGIN_STATE_AUTHENTICATING");
        case net::NetState::Queued: return text::format(m_statusScratch, "AUTH_WAIT_QUEUE", m_queuePosition);
        default: return text::get("GAME_SERVER_LOGIN");
        }
    case Phase::EnteringWorld: return text::get("CHAR_LOGIN_IN_PROGRESS");
    default: return {};
    }
}

void GameSession::onState(net::NetState state, std::string_view error) {
    switch (state) {
    case net::NetState::Online:
        m_phase = Phase::CharacterSelect;
        m_realmName = m_net.realmName();
        m_onlineSince = Clock::now();
        m_reconnectDelay = 5.0;
        m_realmListShown = false; // phase 18 (glue): a realm chosen
        m_hadSession = true;
        m_charListPending = m_net.send(Opcode::CMSG_CHAR_ENUM, {});
        break;
    case net::NetState::Offline: {
        const bool wasInWorld = m_phase == Phase::InWorld;
        m_phase = Phase::LoggedOut;
        m_realmListShown = m_realmListPending = m_charListPending = false; // phase 18 (glue)
        m_play.leaveWorld(); // also forgets the queries that were never answered
        m_entities.clear();
        m_characters.clear();
        if (!error.empty()) {
            m_status.assign(error);
        }
        if (m_wantOnline && m_hadSession) { // not asked for: try again, sooner if it was playing
            m_reconnectPending = true;
            m_reconnectAt = Clock::now() + std::chrono::milliseconds(static_cast<i64>(m_reconnectDelay * 1000.0));
            m_reconnectDelay = wasInWorld ? 5.0 : std::min(60.0, m_reconnectDelay * 2.0);
            VD_LOG_WARN("Session lost%s: reconnecting in %.0f s", wasInWorld ? " in the world" : "",
                        secondsBetween(Clock::now(), m_reconnectAt));
        }
        break;
    }
    case net::NetState::ChoosingRealm: // phase 18 (glue): RealmList over the login screen
        m_phase = Phase::Connecting;
        m_realmListShown = true;
        break;
    default:
        if (m_phase == Phase::CharacterSelect) { // phase 18 (glue): another realm (Change Realm): its own characters
            m_characters.clear();
        }
        m_phase = Phase::Connecting;
        break;
    }
}

void GameSession::enter(const net::CharacterInfo& character) {
    std::vector<u8> body;
    net::ByteWriter(body).u64v(character.guid);
    if (m_net.send(Opcode::CMSG_PLAYER_LOGIN, body)) {
        m_phase = Phase::EnteringWorld;
        m_playerGuid = character.guid;
        m_playerName = character.name;
        VD_LOG_INFO("Entering the world as %s", character.name.c_str());
    }
}

std::optional<WorldEntry> GameSession::onPacket(u16 opcode, std::span<const u8> payload, u32 nowMs) {
    ++m_packets;
    bool ok = true;
    std::optional<WorldEntry> entered;
    switch (static_cast<Opcode>(opcode)) {
    case Opcode::SMSG_AUTH_RESPONSE: { // forwarded by the network thread: the queue position while waiting
        const std::optional<net::AuthResponse> response = net::parseAuthResponse(payload);
        ok = response.has_value();
        m_queuePosition = response ? response->queuePosition : 0;
        break;
    }
    case Opcode::SMSG_WARDEN_DATA: {
        // Phase 18: the server runs Warden for this session (vMaNGOS Warden.WinEnabled: the Windows client's
        // anti-cheat, which inspects that client's memory). This client cannot answer it and the server would drop it
        // after a while: leave now and say why (the server has to disable Warden for it).
        VD_LOG_WARN("The server runs Warden (SMSG_WARDEN_DATA, %zu bytes): this client cannot answer it", payload.size());
        logout();
        m_status.assign(text::get("VD_LOGIN_WARDEN"));
        break;
    }
    case Opcode::SMSG_CHAR_ENUM: {
        std::optional<std::vector<net::CharacterInfo>> list = net::parseCharEnum(payload);
        if (!list) {
            ok = false;
            break;
        }
        m_characters = std::move(*list);
        m_charListPending = false; // phase 18 (glue)
        ++m_charListVersion;
        for (const net::CharacterInfo& c : m_characters) {
            if (!m_autoCharacter.empty() && c.name.size() == m_autoCharacter.size() &&
                std::equal(c.name.begin(), c.name.end(), m_autoCharacter.begin(),
                           [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                                       std::tolower(static_cast<unsigned char>(b)); })) {
                enter(c);
            }
        }
        break;
    }
    case Opcode::SMSG_CHAR_CREATE:
    case Opcode::SMSG_CHAR_DELETE: // phase 8: a ResponseCode; the list again after a success
        ok = net::parseU8(payload, m_charResult);
        if (ok) {
            ++m_charResultVersion;
            if (m_charResult == static_cast<u8>(net::ResponseCode::CHAR_CREATE_SUCCESS) ||
                m_charResult == static_cast<u8>(net::ResponseCode::CHAR_DELETE_SUCCESS)) {
                m_charListPending = m_net.send(Opcode::CMSG_CHAR_ENUM, {}); // phase 18 (glue): CHAR_LIST_RETRIEVING
            }
        }
        break;
    case Opcode::SMSG_CHAR_RENAME: { // phase 18 (glue): the character list renames it in place (WoW.exe 0x4725F0)
        const std::optional<net::CharRenameResult> renamed = net::parseCharRename(payload);
        ok = renamed.has_value();
        if (ok) {
            m_renameResult = renamed->result;
            ++m_renameVersion;
            for (net::CharacterInfo& c : m_characters) {
                if (renamed->result == 0 && c.guid == renamed->guid) {
                    m_renamedFrom.assign(c.name); // phase 18 (extras): its WTF folders follow
                    m_renamedTo.assign(renamed->name);
                    c.name = renamed->name;
                    c.flags &= ~net::kCharacterFlagRename;
                }
            }
        }
        break;
    }
    case Opcode::SMSG_CHARACTER_LOGIN_FAILED: {
        // A ResponseCode (CHAR_LOGIN_*): the client's GlueStrings keys are their names.
        net::ByteReader r(payload);
        const char* code = net::responseCodeName(r.u8v());
        VD_LOG_WARN("Entering the world refused: %s", code);
        m_status.clear();
        text::append(m_status, text::has(std::string_view(code)) ? text::Key(std::string_view(code))
                                                                 : text::Key("CHAR_LOGIN_FAILED"));
        m_phase = Phase::CharacterSelect;
        break;
    }
    case Opcode::SMSG_LOGIN_VERIFY_WORLD:
    case Opcode::SMSG_NEW_WORLD: {
        std::optional<net::LoginVerifyWorld> world = net::parseLoginVerifyWorld(payload); // same layout
        if (!world) {
            ok = false;
            break;
        }
        if (static_cast<Opcode>(opcode) == Opcode::SMSG_NEW_WORLD) {
            m_entities.clear(); // another map: the server sends everything again
            m_moveCount = 0;
            m_net.send(Opcode::MSG_MOVE_WORLDPORT_ACK, {});
        }
        // The client moves its own character: without this the server ignores its movement (GetConfirmedMover).
        m_sendScratch.clear();
        net::ByteWriter(m_sendScratch).u64v(m_playerGuid);
        m_net.send(Opcode::CMSG_SET_ACTIVE_MOVER, m_sendScratch);
        m_transferring = false;
        m_phase = Phase::InWorld;
        m_map = world->map;
        m_play.enterWorld(m_playerGuid, world->map, static_cast<Opcode>(opcode) == Opcode::SMSG_LOGIN_VERIFY_WORLD,
                          nowMs);
        m_reconnects += m_reconnectPending ? 1 : 0;
        const WorldEntry entry{world->map, glm::dvec3(world->position.x, world->position.y, world->position.z),
                               world->position.o};
        if (static_cast<Opcode>(opcode) == Opcode::SMSG_LOGIN_VERIFY_WORLD) {
            m_loginEntry = entry; // update() hands it over with the player's create block
            m_loginEntryAtMs = nowMs;
        } else {
            entered = entry;
        }
        VD_LOG_INFO("In the world: map %u at (%.2f, %.2f, %.2f)", world->map, world->position.x, world->position.y,
                    world->position.z);
        break;
    }
    case Opcode::SMSG_LOGIN_SETTIMESPEED: { // packed time: minute 6 bits, hour 5 bits, ... then speed
        net::ByteReader r(payload);
        const u32 packed = r.u32v();
        if (r.ok()) {
            m_timeOfDay = static_cast<f32>(((packed >> 6) & 31u) * 120u + (packed & 63u) * 2u);
            m_timeOfDayMs = nowMs; // phase 18 (p3): GetGameTime runs on from here
            const f32 speed = r.f32v();
            m_timeSpeed = r.ok() && speed > 0.0f ? speed : 1.0f / 60.0f;
            // Phase 11: day of the month from bit 14 (from 0), month from 20, year from 24 (since 2000):
            // vMaNGOS' secsToTimeBitFields.
            m_day = celestial::daysSince2000((packed >> 24) & 0xFFu, (packed >> 20) & 15u,
                                             (packed >> 14) & 63u);
            // Phase 13 (quests): the date has whole minutes; only a clock off by more than that is corrected.
            const i64 server = static_cast<i64>(game::packedTimeToUnix(packed));
            const i64 offset = server != 0 ? server - static_cast<i64>(std::time(nullptr)) : 0;
            m_serverClockOffset = offset > 120 || offset < -120 ? offset : 0;
        }
        break;
    }
    case Opcode::SMSG_LOGOUT_COMPLETE: {
        // Parity (popups): the character list again on the same connection (the server keeps the session
        // authenticated), as the client's camp; after Exit Game, the end.
        const bool quit = m_play.logout().pending() && m_play.logout().quitting();
        m_phase = Phase::CharacterSelect;
        m_transferring = false;
        m_moveCount = 0;
        m_play.leaveWorld();
        m_entities.clear();
        m_autoCharacter.clear();
        VD_LOG_INFO("Logged out%s", quit ? ": exiting" : "");
        if (quit) {
            forceQuit();
        } else {
            m_charListPending = m_net.send(Opcode::CMSG_CHAR_ENUM, {});
        }
        break;
    }
    case Opcode::SMSG_UPDATE_OBJECT: {
        m_created.clear();
        game::UpdateStats stats;
        stats.createdGuids = &m_created;
        ok = m_entities.applyUpdateObject(payload, nowMs, &stats);
        m_play.afterUpdate(m_created, nowMs); // quest givers to ask about, quest log, auras, death...
        break;
    }
    case Opcode::SMSG_DESTROY_OBJECT: ok = m_entities.applyDestroy(payload); break;
    case Opcode::SMSG_MONSTER_MOVE: ok = m_entities.applyMonsterMove(payload, nowMs); break;
    case Opcode::SMSG_MONSTER_MOVE_TRANSPORT: // phase 18 (motion): a unit walking on a ship, a zeppelin, an elevator
        ok = m_entities.applyMonsterMoveTransport(payload, nowMs);
        break;
    case Opcode::SMSG_TRANSFER_PENDING:
    case Opcode::SMSG_TRANSFER_ABORTED: // phase 8: Travel keeps the map (loading screen) and the abort reason
        m_transferring = static_cast<Opcode>(opcode) == Opcode::SMSG_TRANSFER_PENDING;
        ok = m_play.handle(static_cast<Opcode>(opcode), payload, nowMs) != game::Gameplay::Handled::Malformed;
        break;

    // Orders about the player's own movement (MovementPacketSender.cpp): applied and answered by the controller.
    case Opcode::SMSG_FORCE_WALK_SPEED_CHANGE:
    case Opcode::SMSG_FORCE_RUN_SPEED_CHANGE:
    case Opcode::SMSG_FORCE_RUN_BACK_SPEED_CHANGE:
    case Opcode::SMSG_FORCE_SWIM_SPEED_CHANGE:
    case Opcode::SMSG_FORCE_SWIM_BACK_SPEED_CHANGE:
    case Opcode::SMSG_FORCE_TURN_RATE_CHANGE: {
        struct Change {
            Opcode opcode;
            game::SpeedType speed;
            Opcode ack;
        };
        static constexpr Change kChanges[] = {
            {Opcode::SMSG_FORCE_WALK_SPEED_CHANGE, game::SpeedType::Walk, Opcode::CMSG_FORCE_WALK_SPEED_CHANGE_ACK},
            {Opcode::SMSG_FORCE_RUN_SPEED_CHANGE, game::SpeedType::Run, Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK},
            {Opcode::SMSG_FORCE_RUN_BACK_SPEED_CHANGE, game::SpeedType::RunBack,
             Opcode::CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK},
            {Opcode::SMSG_FORCE_SWIM_SPEED_CHANGE, game::SpeedType::Swim, Opcode::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK},
            {Opcode::SMSG_FORCE_SWIM_BACK_SPEED_CHANGE, game::SpeedType::SwimBack,
             Opcode::CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK},
            {Opcode::SMSG_FORCE_TURN_RATE_CHANGE, game::SpeedType::TurnRate, Opcode::CMSG_FORCE_TURN_RATE_CHANGE_ACK},
        };
        for (const Change& change : kChanges) {
            if (change.opcode == static_cast<Opcode>(opcode)) {
                MoveCommand command;
                command.kind = MoveCommand::Kind::Speed;
                command.speed = change.speed;
                command.ack = change.ack;
                if (readForcedChange(payload, command, 1, &command.value)) {
                    pushMoveCommand(command);
                }
            }
        }
        break;
    }
    case Opcode::SMSG_FORCE_MOVE_ROOT:
    case Opcode::SMSG_FORCE_MOVE_UNROOT: {
        MoveCommand command;
        const bool root = static_cast<Opcode>(opcode) == Opcode::SMSG_FORCE_MOVE_ROOT;
        command.kind = root ? MoveCommand::Kind::Root : MoveCommand::Kind::Unroot;
        command.ack = root ? Opcode::CMSG_FORCE_MOVE_ROOT_ACK : Opcode::CMSG_FORCE_MOVE_UNROOT_ACK;
        if (readForcedChange(payload, command, 0, nullptr)) {
            pushMoveCommand(command);
        }
        break;
    }
    case Opcode::SMSG_MOVE_WATER_WALK:
    case Opcode::SMSG_MOVE_LAND_WALK:
    case Opcode::SMSG_MOVE_FEATHER_FALL:
    case Opcode::SMSG_MOVE_NORMAL_FALL:
    case Opcode::SMSG_MOVE_SET_HOVER:
    case Opcode::SMSG_MOVE_UNSET_HOVER: {
        const Opcode op = static_cast<Opcode>(opcode);
        MoveCommand command;
        command.kind = MoveCommand::Kind::Ability;
        command.apply = op == Opcode::SMSG_MOVE_WATER_WALK || op == Opcode::SMSG_MOVE_FEATHER_FALL ||
                        op == Opcode::SMSG_MOVE_SET_HOVER;
        if (op == Opcode::SMSG_MOVE_WATER_WALK || op == Opcode::SMSG_MOVE_LAND_WALK) {
            command.flag = kMoveFlagWaterWalking;
            command.ack = Opcode::CMSG_MOVE_WATER_WALK_ACK;
        } else if (op == Opcode::SMSG_MOVE_FEATHER_FALL || op == Opcode::SMSG_MOVE_NORMAL_FALL) {
            command.flag = kMoveFlagSafeFall;
            command.ack = Opcode::CMSG_MOVE_FEATHER_FALL_ACK;
        } else {
            command.flag = kMoveFlagHover;
            command.ack = Opcode::CMSG_MOVE_HOVER_ACK;
        }
        if (readForcedChange(payload, command, 0, nullptr)) {
            pushMoveCommand(command);
        }
        break;
    }
    case Opcode::SMSG_MOVE_KNOCK_BACK: {
        MoveCommand command;
        command.kind = MoveCommand::Kind::Knockback;
        command.ack = Opcode::CMSG_MOVE_KNOCK_BACK_ACK;
        f32 values[4] = {};
        if (readForcedChange(payload, command, 4, values)) {
            command.knockCos = values[0];
            command.knockSin = values[1];
            command.knockSpeedXY = values[2];
            command.knockSpeedZ = values[3];
            pushMoveCommand(command);
        }
        break;
    }
    case Opcode::MSG_MOVE_TELEPORT_ACK: { // a near teleport: packed GUID, counter, where to
        net::ByteReader r(payload);
        MoveCommand command;
        command.kind = MoveCommand::Kind::Teleport;
        command.ack = Opcode::MSG_MOVE_TELEPORT_ACK;
        const u64 guid = r.packedGuid();
        command.counter = r.u32v();
        command.info = net::readMovementInfo(r);
        command.guid = guid;
        ok = r.ok();
        if (ok && guid == m_playerGuid) {
            pushMoveCommand(command);
        }
        break;
    }
    case Opcode::MSG_MOVE_TIME_SKIPPED: // phase 13 (combat): another client paused: its unit's dead reckoning waits
        ok = m_entities.applyTimeSkipped(payload);
        break;

    // Speeds of other units.
    case Opcode::MSG_MOVE_SET_WALK_SPEED: ok = m_entities.applyMoveSpeed(payload, game::SpeedType::Walk, nowMs); break;
    case Opcode::MSG_MOVE_SET_RUN_SPEED: ok = m_entities.applyMoveSpeed(payload, game::SpeedType::Run, nowMs); break;
    case Opcode::MSG_MOVE_SET_RUN_BACK_SPEED:
        ok = m_entities.applyMoveSpeed(payload, game::SpeedType::RunBack, nowMs);
        break;
    case Opcode::MSG_MOVE_SET_SWIM_SPEED: ok = m_entities.applyMoveSpeed(payload, game::SpeedType::Swim, nowMs); break;
    case Opcode::MSG_MOVE_SET_SWIM_BACK_SPEED:
        ok = m_entities.applyMoveSpeed(payload, game::SpeedType::SwimBack, nowMs);
        break;
    case Opcode::MSG_MOVE_SET_TURN_RATE:
        ok = m_entities.applyMoveSpeed(payload, game::SpeedType::TurnRate, nowMs);
        break;
    case Opcode::SMSG_SPLINE_SET_WALK_SPEED:
        ok = m_entities.applySplineSpeed(payload, game::SpeedType::Walk, nowMs);
        break;
    case Opcode::SMSG_SPLINE_SET_RUN_SPEED: ok = m_entities.applySplineSpeed(payload, game::SpeedType::Run, nowMs); break;
    case Opcode::SMSG_SPLINE_SET_RUN_BACK_SPEED:
        ok = m_entities.applySplineSpeed(payload, game::SpeedType::RunBack, nowMs);
        break;
    case Opcode::SMSG_SPLINE_SET_SWIM_SPEED:
        ok = m_entities.applySplineSpeed(payload, game::SpeedType::Swim, nowMs);
        break;
    case Opcode::SMSG_SPLINE_SET_SWIM_BACK_SPEED:
        ok = m_entities.applySplineSpeed(payload, game::SpeedType::SwimBack, nowMs);
        break;
    case Opcode::SMSG_SPLINE_SET_TURN_RATE:
        ok = m_entities.applySplineSpeed(payload, game::SpeedType::TurnRate, nowMs);
        break;
    // Phase 18 (motion): the movement states of the units the server moves (root, walk / run, swim, hover...).
    case Opcode::SMSG_SPLINE_MOVE_ROOT:
    case Opcode::SMSG_SPLINE_MOVE_UNROOT:
    case Opcode::SMSG_SPLINE_MOVE_WATER_WALK:
    case Opcode::SMSG_SPLINE_MOVE_LAND_WALK:
    case Opcode::SMSG_SPLINE_MOVE_FEATHER_FALL:
    case Opcode::SMSG_SPLINE_MOVE_NORMAL_FALL:
    case Opcode::SMSG_SPLINE_MOVE_SET_HOVER:
    case Opcode::SMSG_SPLINE_MOVE_UNSET_HOVER:
    case Opcode::SMSG_SPLINE_MOVE_START_SWIM:
    case Opcode::SMSG_SPLINE_MOVE_STOP_SWIM:
    case Opcode::SMSG_SPLINE_MOVE_SET_RUN_MODE:
    case Opcode::SMSG_SPLINE_MOVE_SET_WALK_MODE: {
        net::SplineMoveKind kind{};
        ok = net::splineMoveKind(static_cast<Opcode>(opcode), kind) && m_entities.applySplineMove(payload, kind, nowMs);
        break;
    }
    default:
        if (opcode < m_isMovement.size() && m_isMovement[opcode]) {
            net::ByteReader r(payload);
            const u64 mover = m_play.control().activeMover(); // phase 13: the player, a unit it moves, or none
            if (mover != 0 && r.packedGuid() == mover) {
                // The mover's own movement comes back only when the server overrides it (a rejected move). Without
                // control (fear, a Mind Control by another player) the player's moves are another unit's.
                MoveCommand command;
                command.kind = MoveCommand::Kind::Correction;
                command.guid = mover;
                command.info = net::readMovementInfo(r);
                ok = r.ok();
                if (ok) {
                    pushMoveCommand(command);
                    VD_LOG_WARN("The server moved the player (%s): (%.2f, %.2f, %.2f)", net::opcodeName(opcode),
                                command.info.position.x, command.info.position.y, command.info.position.z);
                }
            } else {
                ok = m_entities.applyMove(payload, nowMs);
            }
        } else if (m_phase == Phase::InWorld || m_phase == Phase::EnteringWorld) {
            // Combat, spells, quests, loot, chat... (phase 7).
            const Opcode op = static_cast<Opcode>(opcode);
            const game::Gameplay::Handled handled = m_play.handle(op, payload, nowMs);
            ok = handled != game::Gameplay::Handled::Malformed;
            if (handled == game::Gameplay::Handled::No && m_traceUnhandled) {
                VD_LOG_INFO("Unhandled %s (%zu bytes)", net::opcodeName(opcode), payload.size());
            }
            const game::ChatLine* line = m_play.chat().last();
            if (handled == game::Gameplay::Handled::Yes && line &&
                (op == Opcode::SMSG_MESSAGECHAT || op == Opcode::SMSG_NOTIFICATION) &&
                (line->type == net::ChatType::System || line->source == game::ChatSource::Notification)) {
                VD_LOG_INFO("Server: %s", line->text.c_str()); // answers to GM commands, errors
            }
        }
        break;
    }
    if (!ok) {
        ++m_parseErrors;
        VD_LOG_WARN("Could not parse %s (%zu bytes)", net::opcodeName(opcode), payload.size());
    }
    return entered;
}

std::optional<WorldEntry> GameSession::update(u32 nowMs) {
    m_lastUpdateMs = nowMs;
    m_entities.nextFrame(); // phase 18 (motion): what the packets of this frame change is this frame's
    std::optional<WorldEntry> entered;
    net::NetEventKind kind{};
    u16 opcode = 0;
    std::span<const u8> payload;
    // Everything that arrived; a city floods a few hundred packets at login, then it is a handful a frame.
    while (m_net.poll(kind, opcode, payload)) {
        if (kind == net::NetEventKind::State) {
            onState(static_cast<net::NetState>(opcode),
                    std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
        } else if (kind == net::NetEventKind::RealmList) {
            onRealmList(payload); // phase 18 (glue)
        } else if (std::optional<WorldEntry> e = onPacket(opcode, payload, nowMs)) {
            entered = e;
        }
    }
    // Phase 18: the login's place once the player's own create block came (or a few seconds without it): on board a
    // ship, the login packet's position is the offset on it (vMaNGOS HandlePlayerLogin: GetTransportPos), the block's
    // the world one.
    if (m_loginEntry && m_phase == Phase::InWorld) {
        const game::Entity* me = m_entities.find(m_playerGuid);
        if (me || static_cast<i32>(nowMs - m_loginEntryAtMs) > 3000) {
            WorldEntry entry = *m_loginEntry;
            m_loginEntry.reset();
            if (me && me->transportGuid != 0) {
                entry.transport = me->transportGuid;
                entry.transportOffset = me->transportOffset;
                entry.transportFacing = me->transportFacing;
                entry.position = glm::dvec3(me->position);
                VD_LOG_INFO("Logged in on transport %llx at (%.2f, %.2f, %.2f) on it",
                            static_cast<unsigned long long>(me->transportGuid), me->transportOffset.x,
                            me->transportOffset.y, me->transportOffset.z);
            }
            entered = entry;
        }
    } else if (m_phase != Phase::InWorld) {
        m_loginEntry.reset();
    }
    m_play.update(nowMs); // forgets a target that left, asks quest givers again after quest events
    if (m_reconnectPending && Clock::now() >= m_reconnectAt) {
        m_reconnectPending = false;
        ++m_reconnects;
        VD_LOG_INFO("Reconnecting (attempt %u)", m_reconnects);
        m_phase = Phase::Connecting;
        m_net.login(m_params);
    }
    return entered;
}

void GameSession::pushMoveCommand(const MoveCommand& command) {
    if (m_moveCount == m_moveCommands.size()) {
        VD_LOG_WARN("Movement orders overflow: the oldest is dropped");
        m_moveFirst = (m_moveFirst + 1) % m_moveCommands.size();
        --m_moveCount;
    }
    m_moveCommands[(m_moveFirst + m_moveCount) % m_moveCommands.size()] = command;
    ++m_moveCount;
}

bool GameSession::pollMoveCommand(MoveCommand& out) {
    if (m_moveCount == 0) {
        return false;
    }
    out = m_moveCommands[m_moveFirst];
    m_moveFirst = (m_moveFirst + 1) % m_moveCommands.size();
    --m_moveCount;
    return true;
}

bool GameSession::readForcedChange(std::span<const u8> payload, MoveCommand& command, u32 extraFloats, f32* extra) {
    net::ByteReader r(payload);
    const u64 guid = r.packedGuid();
    command.counter = r.u32v();
    for (u32 i = 0; i < extraFloats; ++i) {
        extra[i] = r.f32v();
    }
    command.guid = guid;
    // Orders for a unit this client does not move (a pet it does not control) need nothing. Phase 13: the unit it
    // moves (Mind Control, Eyes of the Beast) gets its own (MovementPacketSender::AddMovementFlagChangeToController).
    return r.ok() && guid != 0 && (guid == m_playerGuid || guid == m_play.control().activeMover());
}

void GameSession::sendMovement(net::Opcode opcode, const net::MovementInfo& info) {
    m_sendScratch.clear();
    net::ByteWriter w(m_sendScratch);
    net::writeMovementInfo(w, info);
    m_net.send(opcode, m_sendScratch);
    // Phase 18 (p3): the movement keys (WoW.exe's InputControl 0x514A83: forward, backward, a strafe) for the tutorials.
    if (opcode == Opcode::MSG_MOVE_START_FORWARD || opcode == Opcode::MSG_MOVE_START_BACKWARD ||
        opcode == Opcode::MSG_MOVE_START_STRAFE_LEFT || opcode == Opcode::MSG_MOVE_START_STRAFE_RIGHT) {
        m_play.tutorials().playerMoved(m_lastUpdateMs);
    }
}

void GameSession::acknowledge(const MoveCommand& command, const net::MovementInfo& info) {
    if (command.ack == Opcode::MSG_NULL_ACTION) {
        return;
    }
    m_sendScratch.clear();
    net::ByteWriter w(m_sendScratch);
    w.u64v(command.guid != 0 ? command.guid : m_playerGuid).u32v(command.counter);
    if (command.kind == MoveCommand::Kind::Speed) { // phase 13: the entity keeps it (a switch of mover reads it back)
        m_entities.setSpeed(command.guid != 0 ? command.guid : m_playerGuid, command.speed, command.value);
    }
    if (command.kind == MoveCommand::Kind::Teleport) {
        w.u32v(info.time); // MoveTeleportAck: guid, counter, client time
    } else {
        net::writeMovementInfo(w, info);
        if (command.kind == MoveCommand::Kind::Speed) {
            w.f32v(command.value);
        } else if (command.kind == MoveCommand::Kind::Ability) {
            w.u32v(command.apply ? 1u : 0u);
        }
    }
    m_net.send(command.ack, m_sendScratch);
}

void GameSession::say(std::string_view text) {
    if (m_phase != Phase::InWorld || text.empty()) {
        return;
    }
    if (m_play.sendChat(net::ChatType::Say, text)) {
        VD_LOG_INFO("Said: %.*s", static_cast<int>(text.size()), text.data());
    }
}

void GameSession::setPlayerMovement(const net::MovementInfo& info, u32 nowMs) {
    m_entities.setMovement(m_playerGuid, info, nowMs);
}

void GameSession::setMoverMovement(u64 guid, const net::MovementInfo& info, u32 nowMs) {
    m_entities.setMovement(guid, info, nowMs);
}

void GameSession::sendSplineDone(const net::MovementInfo& info, u32 splineId) {
    m_sendScratch.clear();
    net::writeMoveSplineDone(m_sendScratch, info, splineId);
    m_net.send(Opcode::CMSG_MOVE_SPLINE_DONE, m_sendScratch);
}

void GameSession::describe(char* out, usize size) const {
    std::snprintf(out, size, "%s %u ms | %u packets, %u errors, %u reconnects | %zu entities%s",
                  net::netStateName(m_net.state()), m_net.latencyMs(), m_packets, m_parseErrors, m_reconnects,
                  m_entities.size(), m_phase == Phase::InWorld ? " | in world" : "");
}

std::optional<f32> GameSession::gameTimeNow(u32 nowMs) const {
    if (!m_timeOfDay) {
        return std::nullopt;
    }
    const f32 minutes = static_cast<f32>(nowMs - m_timeOfDayMs) / 1000.0f * m_timeSpeed;
    return std::fmod(*m_timeOfDay + minutes * 2.0f, 24.0f * 60.0f * 2.0f);
}

u64 GameSession::serverUnixTime() const {
    return static_cast<u64>(static_cast<i64>(std::time(nullptr)) + m_serverClockOffset);
}

std::optional<glm::vec3> GameSession::playerPosition(u32 nowMs) const {
    const game::Entity* me = m_entities.find(m_playerGuid);
    if (!me || !me->hasPosition) {
        return std::nullopt;
    }
    return me->positionAt(nowMs);
}

void GameSession::drawPanel() {
    const f32 font = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(font, font * 17.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Session")) {
        ImGui::End();
        return;
    }
    const net::NetState state = m_net.state();
    ImGui::Text("State: %s", net::netStateName(state));
    if (state == net::NetState::Online) {
        ImGui::SameLine();
        ImGui::Text("| %s | latency %u ms", m_net.realmName().c_str(), m_net.latencyMs());
    }
    if (!m_status.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", m_status.c_str());
    }
    if (m_reconnectPending) {
        ImGui::Text("Reconnecting in %.0f s...", std::max(0.0, secondsBetween(Clock::now(), m_reconnectAt)));
    }
    if (!m_haveCredentials) {
        // Temporary login form. The password only lives until its SRP6 hash is computed.
        if (m_accountInput[0] == 0 && !m_params.account.empty()) {
            std::snprintf(m_accountInput.data(), m_accountInput.size(), "%s", m_params.account.c_str());
        }
        ImGui::InputText("Account", m_accountInput.data(), m_accountInput.size());
        ImGui::InputText("Password", m_passwordInput.data(), m_passwordInput.size(), ImGuiInputTextFlags_Password);
        if (ImGui::Button("Connect") && m_accountInput[0] != 0 && m_passwordInput[0] != 0) {
            net::LoginParams params = m_params;
            params.account = m_accountInput.data();
            params.credentials = net::srpCredentials(params.account, m_passwordInput.data());
            m_passwordInput.fill('\0');
            configure(params, m_autoCharacter);
            login();
        }
        ImGui::TextDisabled("(or --account/--password at startup; on Android, the intent's extras)");
    } else if (state == net::NetState::Offline && !m_reconnectPending) {
        if (ImGui::Button("Connect")) {
            login();
        }
    } else if (ImGui::Button("Disconnect")) {
        logout();
    }

    if (m_phase == Phase::CharacterSelect || m_phase == Phase::EnteringWorld) {
        ImGui::Separator();
        if (m_characters.empty()) {
            ImGui::TextUnformatted("No characters on this realm");
        }
        for (const net::CharacterInfo& c : m_characters) {
            ImGui::PushID(static_cast<int>(c.guid));
            const std::string race(game::raceName(c.race)), cls(game::className(c.cls));
            ImGui::Text("%s - level %u, %s %s (map %u)", c.name.c_str(), c.level, race.c_str(), cls.c_str(), c.map);
            ImGui::SameLine();
            if (m_phase == Phase::CharacterSelect && ImGui::Button("Enter")) {
                enter(c);
            }
            ImGui::PopID();
        }
    }
    if (m_phase == Phase::InWorld) {
        ImGui::Separator();
        ImGui::Text("%s on map %u", m_playerName.c_str(), m_map);
        const u32 now = static_cast<u32>(secondsBetween(m_onlineSince, Clock::now()));
        ImGui::Text("Online %u:%02u min | reconnects %u", now / 60, now % 60, m_reconnects);
        if (const game::Entity* me = m_entities.find(m_playerGuid)) {
            ImGui::Text("Position (%.1f, %.1f, %.1f) | health %u/%u", me->position.x, me->position.y, me->position.z,
                        me->value(net::uf::UNIT_FIELD_HEALTH), me->value(net::uf::UNIT_FIELD_MAXHEALTH));
        }
        ImGui::Text("Entities %zu: %u players, %u creatures, %u objects", m_entities.size(),
                    m_entities.countOf(game::ObjectType::Player), m_entities.countOf(game::ObjectType::Unit),
                    m_entities.countOf(game::ObjectType::GameObject));
        ImGui::Text("Packets %u | parse errors %u", m_packets, m_parseErrors);
        ImGui::Checkbox("Boxes", &m_showEntities);
        // Development: GM commands (".tele goldshire", ".mod speed 2") and the server's answers.
        const bool entered = ImGui::InputText("##command", m_commandInput.data(), m_commandInput.size(),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Say") || entered) && m_commandInput[0] != 0) {
            say(m_commandInput.data());
            m_commandInput.fill('\0');
        }
        // The last lines of the server itself (answers to commands, notifications).
        const game::ChatLog::Ring& chat = m_play.chat();
        std::array<const game::ChatLine*, 6> shown{};
        u32 count = 0;
        u32 cursor = chat.end() - chat.size();
        while (const game::ChatLine* line = chat.next(cursor)) {
            if (line->type == net::ChatType::System || line->source != game::ChatSource::Message) {
                if (count == shown.size()) {
                    std::copy(shown.begin() + 1, shown.end(), shown.begin());
                    --count;
                }
                shown[count++] = line;
            }
        }
        for (u32 i = 0; i < count; ++i) {
            ImGui::TextDisabled("%s", shown[i]->text.c_str());
        }
    }
    ImGui::End();
}

void GameSession::drawEntities(const glm::mat4& viewProj, const glm::dvec3& camera, f32 width, f32 height, u32 nowMs,
                               bool (*hasModel)(const void* context, u64 guid), const void* context) const {
    if (!m_showEntities || m_phase != Phase::InWorld) {
        return;
    }
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const glm::vec3 cam(camera);
    m_entities.forEach([&](const game::Entity& e) {
        if (!e.hasPosition || (hasModel && hasModel(context, e.guid))) {
            return;
        }
        const glm::vec3 base = e.positionAt(nowMs);
        if (glm::length(base - cam) > kEntityDrawDistance) {
            return;
        }
        // Size: units by their bounding radius and scale; objects a yard-wide cube.
        const bool unit = e.type == game::ObjectType::Unit || e.type == game::ObjectType::Player;
        const f32 scale = e.value(net::uf::OBJECT_FIELD_SCALE_X) ? asFloat(e.value(net::uf::OBJECT_FIELD_SCALE_X)) : 1.0f;
        f32 radius = unit ? asFloat(e.value(net::uf::UNIT_FIELD_BOUNDINGRADIUS)) : 0.5f;
        radius = std::clamp(radius > 0.0f ? radius : 0.4f, 0.2f, 5.0f) * std::clamp(scale, 0.1f, 10.0f);
        const f32 tall = unit ? radius * 5.0f : radius * 2.0f;
        ImU32 color = IM_COL32(170, 170, 200, 200); // game objects
        if (e.guid == m_playerGuid) {
            color = IM_COL32(60, 255, 90, 255);
        } else if (e.type == game::ObjectType::Player) {
            color = IM_COL32(80, 200, 255, 255);
        } else if (e.type == game::ObjectType::Unit) {
            color = IM_COL32(255, 220, 60, 230);
        } else if (e.type == game::ObjectType::Corpse) {
            color = IM_COL32(255, 80, 80, 200);
        }
        ImVec2 corners[8];
        for (u32 i = 0; i < 8; ++i) {
            const glm::vec3 p = base - cam + glm::vec3((i & 1) ? radius : -radius, (i & 2) ? radius : -radius,
                                                       (i & 4) ? tall : 0.0f);
            const glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
            if (clip.w < 0.1f) {
                return; // behind the camera
            }
            corners[i] = ImVec2((clip.x / clip.w * 0.5f + 0.5f) * width, (clip.y / clip.w * 0.5f + 0.5f) * height);
        }
        static constexpr u8 kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                             {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : kEdges) {
            draw->AddLine(corners[edge[0]], corners[edge[1]], color, e.guid == m_playerGuid ? 2.0f : 1.0f);
        }
    });
}

} // namespace vd
