// Phase 13 (13.9): the server messages the client used to drop, through game::Gameplay: the shutdown countdown
// worded as WoW.exe does (ServerMessages.dbc, "[%d]: %s" without it), mount failures as error events and never the
// successes, the creatures' alert / aggro reactions as visual-and-sound events, forced reactions and the at-war
// flag in how units look to the player (game::Reactions with the player's standing), the guards' gossip point on
// the minimap, an expiring item's time left. The real ServerMessages.dbc when VD_PACKS_DIR is set.
#include "game/Entities.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "game/Reactions.h"
#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/MiscMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <span>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
using Bytes = std::vector<u8>;
using Handled = game::Gameplay::Handled;
namespace uf = net::uf;

constexpr u64 kPlayer = 0x21;
constexpr u64 kCreature = 0xF130000F11000123ull;

Bytes serverMessage(u32 type, std::string_view text) {
    Bytes out;
    net::ByteWriter(out).u32v(type).cstring(text);
    return out;
}

struct Fixture {
    game::EntityStore store;
    game::Gameplay play{store};
    u32 now = 1000;
    Fixture() {
        play.setSink({[](void*, Opcode, std::span<const u8>) { return true; }, nullptr});
        play.enterWorld(kPlayer, 0, true, now);
    }
    const game::ChatLine* lastLine() const { return play.chat().last(); }
};

const game::GameData* gameData() {
    static pack::PackReader db;
    static game::GameData data;
    static int state = 0; // 0 not tried, 1 ready, 2 missing
    if (state == 0) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        state = dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok() && data.init(db) ? 1 : 2;
    }
    return state == 1 ? &data : nullptr;
}

// A unit as reactionTo() sees it: its GUID, type and faction template (not in any store).
game::Entity unit(u64 guid, u32 factionTemplate, game::ObjectType type) {
    game::Entity e;
    e.guid = guid;
    e.type = type;
    e.values.assign(uf::UNIT_END, 0u);
    e.values[uf::UNIT_FIELD_FACTIONTEMPLATE] = factionTemplate;
    return e;
}

} // namespace

TEST_CASE("Lost messages: the server's shutdown countdown in the chat") {
    Fixture f;
    // Without the client's table: WoW.exe's own "[%d]: %s".
    REQUIRE(f.play.handle(Opcode::SMSG_SERVER_MESSAGE, serverMessage(net::kServerMessageShutdownTime, "15 Second(s)"),
                          f.now) == Handled::Yes);
    REQUIRE(f.lastLine());
    CHECK(f.lastLine()->source == game::ChatSource::ServerMessage);
    CHECK(f.lastLine()->text == "[1]: 15 Second(s)");
    CHECK(f.play.handle(Opcode::SMSG_SERVER_MESSAGE, Bytes{1, 0, 0, 0}, f.now) == Handled::Malformed);

    const game::GameData* data = gameData();
    if (!data) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping ServerMessages.dbc");
        return;
    }
    f.play.setData(data, nullptr);
    CHECK(data->serverMessage(net::kServerMessageShutdownTime) == "[SERVER] Shutdown in %s");
    REQUIRE(f.play.handle(Opcode::SMSG_SERVER_MESSAGE,
                          serverMessage(net::kServerMessageRestartTime, "1 Minute(s) 5 Second(s)"),
                          f.now) == Handled::Yes);
    CHECK(f.lastLine()->text == "[SERVER] Restart in 1 Minute(s) 5 Second(s)");
    REQUIRE(f.play.handle(Opcode::SMSG_SERVER_MESSAGE, serverMessage(net::kServerMessageShutdownCancelled, ""),
                          f.now) == Handled::Yes);
    CHECK(f.lastLine()->text == "[SERVER] Shutdown cancelled");
    // The custom message: "%s" alone, the server's text never used as a format.
    REQUIRE(f.play.handle(Opcode::SMSG_SERVER_MESSAGE, serverMessage(net::kServerMessageCustom, "100%s done"),
                          f.now) == Handled::Yes);
    CHECK(f.lastLine()->text == "100%s done");
}

TEST_CASE("Lost messages: mount failures, never the successes") {
    Fixture f;
    u32 cursor = f.play.events().end();
    REQUIRE(f.play.handle(Opcode::SMSG_MOUNTRESULT, Bytes{10, 0, 0, 0}, f.now) == Handled::Yes); // OK
    REQUIRE(f.play.handle(Opcode::SMSG_DISMOUNTRESULT, Bytes{3, 0, 0, 0}, f.now) == Handled::Yes);
    CHECK(f.play.events().next(cursor) == nullptr);
    REQUIRE(f.play.handle(Opcode::SMSG_MOUNTRESULT, Bytes{6, 0, 0, 0}, f.now) == Handled::Yes); // looting
    const game::GameEvent* e = f.play.events().next(cursor);
    REQUIRE(e);
    CHECK(e->kind == game::GameEventKind::MountFailed);
    CHECK(e->value[0] == 6);
    CHECK(e->value[1] == 0);
    REQUIRE(f.play.handle(Opcode::SMSG_DISMOUNTRESULT, Bytes{1, 0, 0, 0}, f.now) == Handled::Yes); // not mounted
    e = f.play.events().next(cursor);
    REQUIRE(e);
    CHECK(e->kind == game::GameEventKind::MountFailed);
    CHECK(e->value[0] == 1);
    CHECK(e->value[1] == 1);
    CHECK(f.play.handle(Opcode::SMSG_MOUNTRESULT, Bytes{6, 0}, f.now) == Handled::Malformed);
}

TEST_CASE("Lost messages: a creature's alert and aggro") {
    Fixture f;
    u32 cursor = f.play.fx().end();
    const auto reaction = [](u32 kind) {
        Bytes out;
        net::ByteWriter(out).u64v(kCreature).u32v(kind);
        return out;
    };
    REQUIRE(f.play.handle(Opcode::SMSG_AI_REACTION, reaction(net::kAiReactionHostile), f.now) == Handled::Yes);
    const game::FxEvent* e = f.play.fx().next(cursor);
    REQUIRE(e);
    CHECK(e->kind == game::FxEventKind::AiReaction);
    CHECK(e->guid == kCreature);
    CHECK(e->id == net::kAiReactionHostile);
    // Friendly, afraid and destroy: the 1.12 client ignores them.
    REQUIRE(f.play.handle(Opcode::SMSG_AI_REACTION, reaction(3), f.now) == Handled::Yes);
    CHECK(f.play.fx().next(cursor) == nullptr);
}

TEST_CASE("Lost messages: forced reactions and at war change how units look to the player") {
    Fixture f;
    game::Reactions reactions;
    // A player (template 1, faction 1, Alliance) and a Booty Bay-like faction with a reputation (template 120,
    // faction 21, list id 21), friendly to everyone by its masks.
    reactions.setTemplate(1, 1, game::kFactionGroupAlliance | 1, 0, 0);
    reactions.setTemplate(120, 21, 0, game::kFactionGroupAlliance | game::kFactionGroupHorde | 1, 0);
    reactions.setTemplate(14, 7, 0, 0, game::kFactionGroupAlliance | game::kFactionGroupHorde | 1); // hostile
    reactions.setFactionList(21, 21);
    reactions.setPlayerStanding(&f.play.standing());
    const game::Entity me = unit(kPlayer, 1, game::ObjectType::Player);
    const game::Entity goon = unit(kCreature, 120, game::ObjectType::Unit);
    const game::Entity beast = unit(kCreature + 1, 14, game::ObjectType::Unit);

    // Reputation factions are friends until at war (the reputation list must exist).
    Bytes factions;
    net::ByteWriter w(factions);
    w.u32v(64);
    for (u32 i = 0; i < 64; ++i) {
        w.u8v(i == 21 ? 0x01 : 0x00).u32v(0); // visible, standing 0
    }
    REQUIRE(f.play.handle(Opcode::SMSG_INITIALIZE_FACTIONS, factions, f.now) == Handled::Yes);
    CHECK(game::reactionTo(me, goon, &reactions) == game::Reaction::Friendly);
    CHECK(game::reactionTo(me, beast, &reactions) == game::Reaction::Hostile);
    // A goon attacked us: SMSG_SET_FACTION_ATWAR (Creature::SetInCombatWith).
    REQUIRE(f.play.handle(Opcode::SMSG_SET_FACTION_ATWAR, Bytes{21, 0, 0, 0, 0x02}, f.now) == Handled::Yes);
    CHECK(game::reactionTo(me, goon, &reactions) == game::Reaction::Hostile);
    CHECK(game::reactionTo(goon, me, &reactions) == game::Reaction::Hostile);
    REQUIRE(f.play.handle(Opcode::SMSG_SET_FACTION_ATWAR, Bytes{21, 0, 0, 0, 0x00}, f.now) == Handled::Yes);
    CHECK(game::reactionTo(me, goon, &reactions) == game::Reaction::Friendly);

    // A forced reaction wins over everything: the beast's faction friendly, the goons hated.
    Bytes forced;
    net::ByteWriter(forced).u32v(2).u32v(7).u32v(4).u32v(21).u32v(0);
    REQUIRE(f.play.handle(Opcode::SMSG_SET_FORCED_REACTIONS, forced, f.now) == Handled::Yes);
    CHECK(game::reactionTo(me, beast, &reactions) == game::Reaction::Friendly);
    CHECK(game::reactionTo(beast, me, &reactions) == game::Reaction::Friendly);
    CHECK(game::reactionTo(me, goon, &reactions) == game::Reaction::Hostile);
    // Cleared by an empty list.
    REQUIRE(f.play.handle(Opcode::SMSG_SET_FORCED_REACTIONS, Bytes{0, 0, 0, 0}, f.now) == Handled::Yes);
    CHECK(game::reactionTo(me, beast, &reactions) == game::Reaction::Hostile);
    // Units that are not the player keep their templates' rule.
    CHECK(game::reactionTo(goon, beast, &reactions) == reactions.between(120, 14));
    // Out of the world: no standing.
    f.play.leaveWorld();
    CHECK(game::reactionTo(me, goon, &reactions) == game::Reaction::Friendly);
}

TEST_CASE("Lost messages: a guard's point on the minimap") {
    Fixture f;
    Bytes poi;
    net::ByteWriter(poi).u32v(99).f32v(-8933.5f).f32v(611.25f).u32v(6).u32v(0).cstring("Stormwind Bank");
    CHECK_FALSE(f.play.minimap().gossipPoint().active);
    REQUIRE(f.play.handle(Opcode::SMSG_GOSSIP_POI, poi, f.now) == Handled::Yes);
    const game::GossipPointState& point = f.play.minimap().gossipPoint();
    CHECK(point.active);
    CHECK(point.x == -8933.5f);
    CHECK(point.y == 611.25f);
    CHECK(point.icon == 6);
    CHECK(point.name == "Stormwind Bank");
    const u32 serial = point.serial;
    REQUIRE(f.play.handle(Opcode::SMSG_GOSSIP_POI, poi, f.now) == Handled::Yes);
    CHECK(point.serial == serial + 1);
    // The blip, and its name for a tap.
    std::vector<game::MinimapBlip> blips(16);
    game::BlipQuery q;
    q.x = -8900.0f;
    q.y = 600.0f;
    q.radius = 100.0f;
    const u32 count = game::collectMinimapBlips(f.store, f.play, nullptr, nullptr, q, blips);
    REQUIRE(count == 1);
    CHECK(blips[0].kind == game::BlipKind::GossipPoi);
    CHECK(game::minimapBlipName(f.store, f.play, blips[0]) == "Stormwind Bank");
    // Another map (a new world) drops it.
    f.play.enterWorld(kPlayer, 1, false, f.now);
    CHECK_FALSE(f.play.minimap().gossipPoint().active);
    CHECK(f.play.handle(Opcode::SMSG_GOSSIP_POI, Bytes{6, 0, 0}, f.now) == Handled::Malformed);
}

TEST_CASE("Lost messages: an expiring item's time left") {
    Fixture f;
    Bytes update;
    net::ByteWriter(update).u64v(0x4000000000001234ull).u32v(3540);
    CHECK(f.play.handle(Opcode::SMSG_ITEM_TIME_UPDATE, update, f.now) == Handled::Yes);
    CHECK(f.play.handle(Opcode::SMSG_ITEM_TIME_UPDATE, Bytes{1, 2, 3}, f.now) == Handled::Malformed);
}
