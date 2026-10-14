// vd_netcheck: runs the engine's network code against a vMaNGOS server without the game, to check the
// protocol and the server. PC only (development tool).
//
//   vd_netcheck --account NAME [--password PASS] [--realmd 127.0.0.1:3724] [--enter CHARACTER] [--seconds N]
//
// Logs in to realmd, lists the realms, opens the world session and lists the characters; with --enter it
// also logs that character in and listens for N seconds (default 20), then prints what arrived. Without
// --password it reads VD_PASSWORD from the environment. Nothing is written to disk.
// Phase 7: every gameplay packet also goes through game::Gameplay (read only: it sends nothing), which reports
// the ones it could not parse and what it understood (spells, quest log, chat, combat log).

#include "core/Clock.h"
#include "core/Log.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "net/ByteBuffer.h"
#include "net/Crypto.h"
#include "net/Messages.h"
#include "net/Opcodes.gen.h"
#include "net/RealmClient.h"
#include "net/ResponseCodes.gen.h"
#include "net/Socket.h"
#include "net/UpdateFields.gen.h"
#include "net/WorldConnection.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {

using vd::u16;
using vd::u32;
using vd::u8;

bool splitAddress(const std::string& address, std::string& host, u16& port) {
    const size_t colon = address.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }
    host = address.substr(0, colon);
    port = static_cast<u16>(std::atoi(address.c_str() + colon + 1));
    return port != 0;
}

// Waits for a given opcode, counting the others; false on timeout or error.
bool waitFor(vd::net::WorldConnection& world, vd::net::Opcode wanted, std::vector<u8>& payload, u32 timeoutMs,
             std::map<u16, u32>& seen) {
    const vd::Clock::time_point start = vd::Clock::now();
    while (vd::secondsBetween(start, vd::Clock::now()) * 1000.0 < timeoutMs) {
        u16 opcode = 0;
        vd::Result<bool> got = world.receive(opcode, payload, 100);
        if (!got) {
            VD_LOG_ERROR("world: %s", got.error().message.c_str());
            return false;
        }
        if (got.value()) {
            ++seen[opcode];
            if (opcode == static_cast<u16>(wanted)) {
                return true;
            }
        }
    }
    VD_LOG_ERROR("world: no %s after %u ms", vd::net::opcodeName(static_cast<u32>(wanted)), timeoutMs);
    return false;
}

} // namespace

int main(int argc, char** argv) {
    std::string account, password, enter;
    std::string realmd = "127.0.0.1:3724";
    std::string locale = "enUS";
    u32 seconds = 20;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--account") == 0) {
            account = argv[i + 1];
        } else if (std::strcmp(argv[i], "--password") == 0) {
            password = argv[i + 1];
        } else if (std::strcmp(argv[i], "--realmd") == 0) {
            realmd = argv[i + 1];
        } else if (std::strcmp(argv[i], "--enter") == 0) {
            enter = argv[i + 1];
        } else if (std::strcmp(argv[i], "--seconds") == 0) {
            seconds = static_cast<u32>(std::atoi(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--locale") == 0) { // phase 21: the language told to the server
            locale = argv[i + 1];
        }
    }
    if (password.empty()) {
        if (const char* env = std::getenv("VD_PASSWORD")) {
            password = env;
        }
    }
    std::string host;
    u16 port = 0;
    if (account.empty() || password.empty() || !splitAddress(realmd, host, port)) {
        std::fprintf(stderr, "usage: vd_netcheck --account NAME [--password PASS | env VD_PASSWORD] [--realmd host:port]"
                             " [--enter CHARACTER] [--seconds N] [--locale esES]\n");
        return 2;
    }

    const vd::net::Sha1Digest credentials = vd::net::srpCredentials(account, password);
    password.assign(password.size(), '\0');

    // 1. realmd
    vd::net::Socket socket;
    if (vd::Result<void> r = socket.connect(host, port, vd::net::kNetTimeoutMs); !r) {
        VD_LOG_ERROR("realmd %s: %s", realmd.c_str(), r.error().message.c_str());
        return 1;
    }
    vd::Result<vd::net::RealmSession> session = vd::net::loginToRealmd(socket, account, credentials, nullptr, locale);
    socket.close();
    if (!session) {
        VD_LOG_ERROR("realmd: %s", session.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("realmd: logged in as %s (session key %u bytes)", account.c_str(), session.value().sessionKeySize);
    for (const vd::net::RealmInfo& realm : session.value().realms) {
        VD_LOG_INFO("  realm \"%s\" at %s, %u characters, flags 0x%02X%s", realm.name.c_str(), realm.address.c_str(),
                    realm.characters, realm.flags, realm.offline() ? " (offline)" : "");
    }
    if (session.value().realms.empty()) {
        return 1;
    }

    // 2. world: the first realm. Through a loopback realmd (adb reverse), the world is on loopback too: the
    // server wants both connections from the same address.
    std::string worldHost;
    u16 worldPort = 0;
    splitAddress(session.value().realms[0].address, worldHost, worldPort);
    if (host == "127.0.0.1" || host == "localhost") {
        worldHost = host;
    }
    vd::net::WorldConnection world;
    const std::span<const u8> key(session.value().sessionKey.data(), 40);
    if (vd::Result<void> r = world.open(worldHost, worldPort, account, key); !r) {
        VD_LOG_ERROR("world %s:%u: %s", worldHost.c_str(), worldPort, r.error().message.c_str());
        return 1;
    }
    std::vector<u8> payload;
    std::map<u16, u32> seen;
    if (!waitFor(world, vd::net::Opcode::SMSG_AUTH_RESPONSE, payload, vd::net::kNetTimeoutMs, seen)) {
        return 1;
    }
    const std::optional<vd::net::AuthResponse> auth = vd::net::parseAuthResponse(payload);
    if (!auth || auth->result != static_cast<u8>(vd::net::ResponseCode::AUTH_OK)) {
        VD_LOG_ERROR("world: session refused: %s", auth ? vd::net::responseCodeName(auth->result) : "malformed answer");
        return 1;
    }
    VD_LOG_INFO("world %s:%u: session open", worldHost.c_str(), worldPort);

    // 3. characters
    if (vd::Result<void> r = world.send(vd::net::Opcode::CMSG_CHAR_ENUM, {}); !r) {
        VD_LOG_ERROR("world: %s", r.error().message.c_str());
        return 1;
    }
    if (!waitFor(world, vd::net::Opcode::SMSG_CHAR_ENUM, payload, vd::net::kNetTimeoutMs, seen)) {
        return 1;
    }
    const std::optional<std::vector<vd::net::CharacterInfo>> characters = vd::net::parseCharEnum(payload);
    if (!characters) {
        VD_LOG_ERROR("world: malformed SMSG_CHAR_ENUM (%zu bytes)", payload.size());
        return 1;
    }
    const vd::net::CharacterInfo* chosen = nullptr;
    for (const vd::net::CharacterInfo& c : *characters) {
        VD_LOG_INFO("  character \"%s\": level %u, race %u, class %u, map %u at (%.1f, %.1f, %.1f), zone %u", c.name.c_str(),
                    c.level, c.race, c.cls, c.map, c.x, c.y, c.z, c.zone);
        if (!enter.empty() && _stricmp(c.name.c_str(), enter.c_str()) == 0) {
            chosen = &c;
        }
    }
    if (enter.empty()) {
        return 0;
    }
    if (!chosen) {
        VD_LOG_ERROR("No character called %s", enter.c_str());
        return 1;
    }

    // 4. enter the world and listen
    std::vector<u8> body;
    vd::net::ByteWriter(body).u64v(chosen->guid);
    if (vd::Result<void> r = world.send(vd::net::Opcode::CMSG_PLAYER_LOGIN, body); !r) {
        VD_LOG_ERROR("world: %s", r.error().message.c_str());
        return 1;
    }
    if (!waitFor(world, vd::net::Opcode::SMSG_LOGIN_VERIFY_WORLD, payload, 30000, seen)) {
        return 1;
    }
    const std::optional<vd::net::LoginVerifyWorld> login = vd::net::parseLoginVerifyWorld(payload);
    if (login) {
        VD_LOG_INFO("in the world: map %u at (%.2f, %.2f, %.2f), facing %.2f", login->map, login->position.x,
                    login->position.y, login->position.z, login->position.o);
    }
    const vd::Clock::time_point start = vd::Clock::now();
    u32 bytes = 0;
    u32 failures = 0;
    vd::game::EntityStore entities;
    vd::game::UpdateStats stats;
    std::vector<vd::u64> created;
    stats.createdGuids = &created;
    std::vector<u8> inflated;
    auto nowMs = [&] { return static_cast<u32>(vd::secondsBetween(start, vd::Clock::now()) * 1000.0); };
    // No packet sink: it only listens.
    vd::game::Gameplay play(entities);
    play.enterWorld(chosen->guid, login ? login->map : 0, true, nowMs());
    std::map<u16, u32> gameplayFailures;
    u32 gameplayPackets = 0;
    while (vd::secondsBetween(start, vd::Clock::now()) < seconds) {
        u16 opcode = 0;
        vd::Result<bool> got = world.receive(opcode, payload, 200);
        if (!got) {
            VD_LOG_ERROR("world: %s", got.error().message.c_str());
            return 1;
        }
        if (!got.value()) {
            continue;
        }
        ++seen[opcode];
        bytes += static_cast<u32>(payload.size());
        bool ok = true;
        created.clear();
        switch (static_cast<vd::net::Opcode>(opcode)) {
        case vd::net::Opcode::SMSG_COMPRESSED_UPDATE_OBJECT:
            ok = vd::net::inflatePacket(payload, inflated).ok() && entities.applyUpdateObject(inflated, nowMs(), &stats);
            play.afterUpdate(created, nowMs());
            break;
        case vd::net::Opcode::SMSG_UPDATE_OBJECT:
            ok = entities.applyUpdateObject(payload, nowMs(), &stats);
            play.afterUpdate(created, nowMs());
            break;
        case vd::net::Opcode::SMSG_DESTROY_OBJECT: ok = entities.applyDestroy(payload); break;
        case vd::net::Opcode::SMSG_MONSTER_MOVE: ok = entities.applyMonsterMove(payload, nowMs()); break;
        default: {
            const vd::game::Gameplay::Handled handled =
                play.handle(static_cast<vd::net::Opcode>(opcode), payload, nowMs());
            gameplayPackets += handled != vd::game::Gameplay::Handled::No ? 1 : 0;
            if (handled == vd::game::Gameplay::Handled::Malformed) {
                ++gameplayFailures[opcode];
                ok = false;
            }
            break;
        }
        }
        play.update(nowMs());
        if (!ok) {
            ++failures;
            VD_LOG_ERROR("could not parse %s (%zu bytes)", vd::net::opcodeName(opcode), payload.size());
        }
    }
    VD_LOG_INFO("entities: %zu (%u players, %u units, %u game objects, %u items, %u corpses); created %u, updated %u, "
                "moved %u, removed %u; %u parse failures",
                entities.size(), entities.countOf(vd::game::ObjectType::Player),
                entities.countOf(vd::game::ObjectType::Unit), entities.countOf(vd::game::ObjectType::GameObject),
                entities.countOf(vd::game::ObjectType::Item) + entities.countOf(vd::game::ObjectType::Container),
                entities.countOf(vd::game::ObjectType::Corpse), stats.created, stats.updated, stats.moved,
                stats.removed, failures);
    if (const vd::game::Entity* me = entities.find(chosen->guid)) {
        VD_LOG_INFO("player entity: (%.2f, %.2f, %.2f), level %u, health %u/%u", me->position.x, me->position.y,
                    me->position.z, me->value(vd::net::uf::UNIT_FIELD_LEVEL), me->value(vd::net::uf::UNIT_FIELD_HEALTH),
                    me->value(vd::net::uf::UNIT_FIELD_MAXHEALTH));
    }
    const vd::game::SpellBook& spells = play.spells();
    u32 buttons = 0;
    for (u32 i = 0; i < vd::net::kActionButtonCount; ++i) {
        buttons += spells.button(i).empty() ? 0 : 1;
    }
    VD_LOG_INFO("gameplay: %u packets, %zu opcodes refused; %zu spells known, %u action buttons, %u quests in the "
                "log, %u chat lines, %u combat events, %u events",
                gameplayPackets, gameplayFailures.size(), spells.known().size(), buttons, play.questLog().count(),
                play.chat().end(), play.combatLog().end(), play.events().end());
    for (const auto& [opcode, count] : gameplayFailures) {
        VD_LOG_ERROR("  gameplay: %u x %s refused", count, vd::net::opcodeName(opcode));
    }
    for (const vd::game::QuestLogEntry& q : play.questLog().slots()) {
        if (q.questId != 0) {
            VD_LOG_INFO("  quest %u: counters %u %u %u %u%s%s", q.questId, q.counters[0], q.counters[1],
                        q.counters[2], q.counters[3], q.complete() ? ", complete" : "", q.failed() ? ", failed" : "");
        }
    }
    VD_LOG_INFO("%u s in the world, %u bytes of payload:", seconds, bytes);
    for (const auto& [opcode, count] : seen) {
        VD_LOG_INFO("  %5u x %s", count, vd::net::opcodeName(opcode));
    }
    world.close();
    return 0;
}
