// Phase 17 (honor): the battleground graveyards' area spirit healer (SMSG_AREA_SPIRIT_HEALER_TIME,
// CMSG_AREA_SPIRIT_HEALER_QUERY / _QUEUE), the insignia (SMSG_PLAYER_SKINNED, Remove Insignia), honor
// (SMSG_PVP_CREDIT) and the defense channels (SMSG_ZONE_UNDER_ATTACK, SMSG_DEFENSE_MESSAGE) byte for byte as
// vmangos-core writes and reads them (Server/Packets/Npc.cpp, Battleground.cpp, Misc.cpp, Maps/Map.cpp
// SendDefenseMessage), truncated packets rejected; the rules WoW.exe 1.12.1 follows (game/AreaSpiritHealer.h,
// game/Honor.h) through game::Gameplay with synthetic packets; and the world states vMaNGOS' outdoor PvP
// sends (OutdoorPvPEP, OutdoorPvPSI).
#include "TestData.h"
#include "TextTable.h"
#include "core/Text.h"
#include "db/DbcView.h"
#include "game/AreaData.h"
#include "game/Chat.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Honor.h"
#include "game/Notices.h"
#include "game/Reactions.h"
#include "game/SocialText.h"
#include "game/UnitInfo.h"
#include "game/WorldStates.h"
#include "net/BattlegroundMessages.h"
#include "net/ByteBuffer.h"
#include "net/Messages.h"
#include "net/PvpMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using game::GameEventKind;
using game::HonorCredit;
using game::NoticeKind;
using net::Opcode;
namespace uf = net::uf;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kMe = 0x11;
constexpr u64 kGrommash = 0x21;                    // a Horde player
constexpr u64 kGuide = 0xF130000032CC0101ull;      // Alliance Spirit Guide (13116)
constexpr u64 kHordeGuide = 0xF130000032CD0102ull; // Horde Spirit Guide (13117)
constexpr u64 kInnkeeper = 0xF130000018A00103ull;  // a Horde civilian
constexpr u64 kCorpse = 0xF101000000000201ull;     // HIGHGUID_CORPSE
constexpr u32 kAllianceTemplate = 1;               // FactionTemplate: Human (group Alliance)
constexpr u32 kHordeTemplate = 2;                  // Orc (group Horde)
constexpr u32 kNoSideTemplate = 7;                 // a creature of neither side
constexpr u32 kRemoveInsignia = 22027;
constexpr u32 kWarsongGulch = 489;
constexpr u32 kAzeroth = 0;
constexpr u32 kKalimdor = 1;

template <typename F> void checkTruncations(const Bytes& payload, F&& parse) {
    for (usize n = 0; n < payload.size(); ++n) {
        CHECK_FALSE(parse(std::span<const u8>(payload.data(), n)));
    }
}

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    std::vector<u32> mask(maxIndex / 32 + 1, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(mask.size()));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
}

Bytes created(u64 guid, game::ObjectType type, f32 x, f32 y, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(x).f32v(y).f32v(0.0f).f32v(0.0f);
    writeValues(w, fields);
    return out;
}

Bytes values(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

Bytes healerTime(u64 guid, u32 ms) {
    Bytes out;
    net::ByteWriter(out).u64v(guid).u32v(ms);
    return out;
}

Bytes pvpCredit(i32 honor, u64 victim, i32 rank) {
    Bytes out;
    net::ByteWriter(out).u32v(static_cast<u32>(honor)).u64v(victim).u32v(static_cast<u32>(rank));
    return out;
}

Bytes u32Body(u32 v) {
    Bytes out;
    net::ByteWriter(out).u32v(v);
    return out;
}

// Map::SendDefenseMessage: u32 zone, u32 strlen + 1, the text and its NUL.
Bytes defenseMessage(u32 zone, std::string_view text) {
    Bytes out;
    net::ByteWriter(out).u32v(zone).u32v(static_cast<u32>(text.size() + 1)).cstring(text);
    return out;
}

Bytes initStates(u32 map, u32 zone, const std::vector<std::pair<u32, u32>>& states) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(map).u32v(zone).u16v(static_cast<u16>(states.size() + 1));
    for (const auto& [id, value] : states) {
        w.u32v(id).u32v(value);
    }
    w.u32v(0).u32v(0);
    return out;
}

Bytes updateState(u32 id, u32 value) {
    Bytes out;
    net::ByteWriter(out).u32v(id).u32v(value);
    return out;
}

Bytes channelJoined(std::string_view name) {
    Bytes out;
    net::ByteWriter(out).u8v(0x02).cstring(name).u32v(0x18).u32v(0); // CHAT_YOU_JOINED_NOTICE
    return out;
}

Bytes castSpell(u32 spell, u16 mask, u64 guid) {
    Bytes out;
    net::ByteWriter(out).u32v(spell).u16v(mask).packedGuid(guid);
    return out;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

// Stub texts with the formats of GlobalStrings.lua 1.12.1 (the game shows the client's own).
const std::vector<std::pair<std::string, std::string>> kEnglish = {
    {"COMBATLOG_HONORGAIN", "%s slain, honorable (%s, ~%d honor)"},
    {"COMBATLOG_DISHONORGAIN", "%s slain, dishonorable."},
    {"COMBATLOG_HONORAWARD", "Honor awarded: %d."},
    {"PVP_RANK_6_0", "Grunt"},
    {"PVP_RANK_6_1", "Corporal"},
    {"PVP_RANK_18_1_FEMALE", "Grand Marshal"},
    {"PVP_RANK_18_1", "Grand Marshal"},
    {"PVP_RANK_CIVILIAN", "Civilian"},
    {"PVP_RANK_CIVILIAN_FEMALE", "Civilian"},
    {"HK", "HK"},
    {"DK", "DK"},
    {"DEATH_CORPSE_SKINNED", "Insignia taken: graveyard only"},
    {"ZONE_UNDER_ATTACK", "|cffffff00%s is under attack!|r"},
    {"CHAT_CHANNEL_GET", "%s: "},
    {"UNKNOWNOBJECT", "Unknown"},
};

// Spell.dbc as the client has it for the spells these tests use.
bool spellLookup(const void*, u32 spellId, game::SpellInfo& out) {
    out = game::SpellInfo{};
    out.id = spellId;
    // Remove Insignia: effect 116 (SPELL_EFFECT_SKIN_PLAYER_CORPSE), corpse targets.
    if (spellId == kRemoveInsignia) {
        out.name = "Remove Insignia";
        out.effects[0] = 116;
        out.targets = net::kTargetCorpseEnemy;
        return true;
    }
    if (spellId == 133) {
        out.name = "Fireball";
        out.effects[0] = 2;
        return true;
    }
    return false;
}

// A Gameplay in Warsong Gulch (or `map`) with the player (an Alliance human), the factions' templates, the
// areas the defense channels need, the client's English texts and its sent packets.
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    game::Reactions reactions;
    game::AreaData areas;
    game::NoticeFeed feed;
    std::vector<Sent> sent;
    std::vector<u8> texts = test::textTable(kEnglish);
    u32 now = 100000;
    u32 cursor = 0;
    u32 chatCursor = 0;
    u32 lineCursor = 0;

    explicit Harness(u32 map = kWarsongGulch) {
        text::setTable(texts);
        reactions.setTemplate(kAllianceTemplate, 1, game::kFactionGroupAlliance | 0x1,
                              game::kFactionGroupAlliance, game::kFactionGroupHorde);
        reactions.setTemplate(kHordeTemplate, 2, game::kFactionGroupHorde | 0x1, game::kFactionGroupHorde,
                              game::kFactionGroupAlliance);
        reactions.setTemplate(kNoSideTemplate, 7, 0x8, 0, 0);
        play.setData(nullptr, &reactions);
        play.setSpellSource({spellLookup, nullptr});
        std::vector<game::AreaInfo> list(6);
        list[0] = {12, 0, 0, 0, 0, 0, game::kAreaTeamAlliance, "Elwynn Forest"};
        list[1] = {87, 0, 12, 0, 0, 0, game::kAreaTeamAlliance, "Goldshire"};
        list[2] = {1519, 0, 0, 0, 0, 0, game::kAreaTeamAlliance, "Stormwind City"};
        list[3] = {139, 0, 0, 0, 0, 0, 0, "Eastern Plaguelands"};
        list[4] = {2258, 0, 139, 0, 0, 0, 0, "The Fungal Vale"};
        list[5] = {1377, 1, 0, 0, 0, 0, 0, "Silithus"};
        std::vector<game::MapInfo> maps(3);
        maps[0] = {kAzeroth, "Azeroth", "Eastern Kingdoms"};
        maps[1] = {kKalimdor, "Kalimdor", "Kalimdor"};
        maps[2] = {kWarsongGulch, "PVPZone03", "Warsong Gulch", 3};
        areas.set(std::move(list), {}, std::move(maps));
        play.travel().setData(&areas, nullptr);
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kMe, map, true, now);
        REQUIRE(store.applyUpdateObject(created(kMe, game::ObjectType::Player, 0.0f, 0.0f,
                                                {{uf::UNIT_FIELD_HEALTH, 100},
                                                 {uf::UNIT_FIELD_FACTIONTEMPLATE, kAllianceTemplate},
                                                 {uf::UNIT_FIELD_BYTES_0, 1}}), // human male
                                        now));
        play.afterUpdate({}, now);
        net::NameQueryInfo me;
        me.guid = kMe;
        me.name = "Pruebaguerra";
        play.queries().onName(std::move(me));
        feed.init(nullptr, nullptr);
        feed.restart(play);
        cursor = play.events().end();
        chatCursor = play.chat().end();
        sent.clear();
    }
    ~Harness() { text::unload(); }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
    }
    void apply(const Bytes& update) {
        REQUIRE(store.applyUpdateObject(update, now));
        play.afterUpdate({}, now);
    }
    void moveMe(f32 x, f32 y) {
        net::MovementInfo m;
        m.position = {x, y, 0.0f, 0.0f};
        store.setMovement(kMe, m, now);
    }
    // The player released: a ghost (PLAYER_FLAGS_GHOST, 1 health).
    void ghost(bool on) {
        apply(values(kMe, {{uf::PLAYER_FLAGS, on ? 0x10u : 0u}, {uf::UNIT_FIELD_HEALTH, on ? 1u : 100u}}));
    }
    std::vector<Bytes> sentWith(Opcode opcode) const {
        std::vector<Bytes> out;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out.push_back(s.body);
            }
        }
        return out;
    }
    std::vector<game::GameEvent> events(GameEventKind kind) {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            if (e->kind == kind) {
                out.push_back(*e);
            }
        }
        return out;
    }
    std::vector<std::pair<NoticeKind, std::string>> lines() {
        // Names as the combat log has them: players from the name cache (creatures unnamed here: "Unknown").
        game::CombatNames names;
        names.player = kMe;
        names.context = &play;
        names.unitName = [](const void* context, u64 guid) -> std::string_view {
            const std::string* name =
                static_cast<const game::Gameplay*>(context)->queries().findPlayerName(guid);
            return name ? std::string_view(*name) : std::string_view{};
        };
        feed.update(play, store.find(kMe), names, now);
        std::vector<std::pair<NoticeKind, std::string>> out;
        while (const game::Notice* n = feed.lines().next(lineCursor)) {
            out.emplace_back(n->kind, n->text.data());
        }
        return out;
    }
    // The chat's new lines as ChatFrame prints them (game::appendChatLine with the line's channel number).
    std::vector<std::string> chat() {
        std::vector<std::string> out;
        while (const game::ChatLine* l = play.chat().next(chatCursor)) {
            std::string text;
            game::appendChatLine(text, *l, play.senderName(*l), play.social().channelNumber(l->channel));
            out.push_back(text);
        }
        return out;
    }
};

Bytes spiritGuide(u64 guid, f32 x, u32 factionTemplate, u32 npcFlags = 0x40) {
    return created(guid, game::ObjectType::Unit, x, 0.0f,
                   {{uf::OBJECT_FIELD_ENTRY, 13116},
                    {uf::UNIT_FIELD_HEALTH, 1000},
                    {uf::UNIT_FIELD_FACTIONTEMPLATE, factionTemplate},
                    {uf::UNIT_NPC_FLAGS, npcFlags}});
}

} // namespace

TEST_CASE("PvP: the area spirit healer's, the insignia's, honor's and the defense packets byte for byte") {
    // SMSG_AREA_SPIRIT_HEALER_TIME: u64 + u32 (AreaSpiritHealerTime::AppendBodyTo).
    const Bytes time = healerTime(kGuide, 23500);
    CHECK(time.size() == 12);
    net::AreaSpiritHealerTime t;
    REQUIRE(net::parseAreaSpiritHealerTime(time, t));
    CHECK(t.healer == kGuide);
    CHECK(t.timeMs == 23500);
    checkTruncations(time, [&](std::span<const u8> p) { return net::parseAreaSpiritHealerTime(p, t); });

    // CMSG_AREA_SPIRIT_HEALER_QUERY / _QUEUE: the full GUID (AreaSpiritHealerQuery::ReadFromWorldPacket).
    Bytes body;
    net::writeAreaSpiritHealerQuery(body, kGuide);
    CHECK(body == Bytes{0x01, 0x01, 0xCC, 0x32, 0x00, 0x00, 0x30, 0xF1});
    body.clear();
    net::writeAreaSpiritHealerQueue(body, kGuide);
    CHECK(body == Bytes{0x01, 0x01, 0xCC, 0x32, 0x00, 0x00, 0x30, 0xF1});

    // SMSG_PLAYER_SKINNED: u8 free repop.
    net::PlayerSkinned skinned;
    REQUIRE(net::parsePlayerSkinned(Bytes{0}, skinned));
    CHECK_FALSE(skinned.freeRepop);
    REQUIRE(net::parsePlayerSkinned(Bytes{1}, skinned));
    CHECK(skinned.freeRepop);
    CHECK_FALSE(net::parsePlayerSkinned(Bytes{}, skinned));

    // SMSG_PVP_CREDIT: i32 honor, u64 victim, i32 rank (PvpCredit::AppendBodyTo).
    const Bytes credit = pvpCredit(-17, kInnkeeper, 0);
    CHECK(credit.size() == 16);
    net::PvpCredit c;
    REQUIRE(net::parsePvpCredit(credit, c));
    CHECK(c.honor == -17);
    CHECK(c.victim == kInnkeeper);
    CHECK(c.rank == 0);
    REQUIRE(net::parsePvpCredit(pvpCredit(37, kGrommash, 6), c));
    CHECK(c.honor == 37);
    CHECK(c.rank == 6);
    checkTruncations(credit, [&](std::span<const u8> p) { return net::parsePvpCredit(p, c); });

    // SMSG_ZONE_UNDER_ATTACK: u32 area.
    net::ZoneUnderAttack z;
    REQUIRE(net::parseZoneUnderAttack(u32Body(87), z));
    CHECK(z.areaId == 87);
    checkTruncations(u32Body(87), [&](std::span<const u8> p) { return net::parseZoneUnderAttack(p, z); });

    // SMSG_DEFENSE_MESSAGE: u32 zone, u32 length with the NUL, the text.
    const Bytes defense = defenseMessage(139, "|cffffff00The Alliance now holds Eastwall Tower!|r");
    net::DefenseMessage d;
    REQUIRE(net::parseDefenseMessage(defense, d));
    CHECK(d.zoneId == 139);
    CHECK(d.text == "|cffffff00The Alliance now holds Eastwall Tower!|r");
    checkTruncations(defense, [&](std::span<const u8> p) { return net::parseDefenseMessage(p, d); });
    Bytes noNul;
    net::ByteWriter(noNul).u32v(139).u32v(3).u8v('a').u8v('b').u8v('c');
    CHECK_FALSE(net::parseDefenseMessage(noNul, d)); // the length must end on the NUL
    Bytes empty;
    net::ByteWriter(empty).u32v(139).u32v(0);
    CHECK_FALSE(net::parseDefenseMessage(empty, d));
}

TEST_CASE("PvP: a ghost finds the spirit guide, its wave counts down, the queue and its cancel as WoW.exe") {
    Harness h;
    game::AreaSpiritHealer& healer = h.play.areaSpiritHealer();
    // Alive: no healer, nothing asked (0x4923A6).
    h.apply(spiritGuide(kGuide, 10.0f, kAllianceTemplate));
    h.apply(spiritGuide(kHordeGuide, 5.0f, kHordeTemplate)); // closer, but the player cannot assist it
    h.play.update(h.now);
    CHECK(healer.healer() == 0);
    CHECK(h.sentWith(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY).empty());

    // Killed in the battleground: the body lies there (a battleground is instanceable: no release timer flag, the
    // DEATH popup without a countdown); its Release Spirit is CMSG_REPOP_REQUEST.
    h.apply(values(kMe, {{uf::UNIT_FIELD_HEALTH, 0}}));
    CHECK(h.play.death().dead);
    CHECK_FALSE(h.play.death().releaseTimer);
    h.play.update(h.now);
    CHECK(healer.healer() == 0); // dead, not yet a ghost: no healer
    h.play.releaseSpirit();
    REQUIRE(h.sentWith(Opcode::CMSG_REPOP_REQUEST).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_REPOP_REQUEST)[0].empty());
    // Released (BuildPlayerRepop with Waiting to Resurrect, RepopAtGraveyard to the battleground's graveyard): the
    // friendly guide within 20 yd, asked once.
    h.ghost(true);
    h.play.update(h.now);
    CHECK(healer.healer() == kGuide);
    auto queries = h.sentWith(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY);
    REQUIRE(queries.size() == 1);
    CHECK(queries[0] == Bytes{0x01, 0x01, 0xCC, 0x32, 0x00, 0x00, 0x30, 0xF1});
    h.play.update(h.now + 16);
    CHECK(h.sentWith(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY).size() == 1);

    // The answer: 23.5 s to the wave. AREA_SPIRIT_HEALER_IN_RANGE; GetAreaSpiritHealerTime truncates to 23.
    REQUIRE(h.packet(Opcode::SMSG_AREA_SPIRIT_HEALER_TIME, healerTime(kGuide, 23500)) ==
            game::Gameplay::Handled::Yes);
    auto ev = h.events(GameEventKind::AreaSpiritHealerInRange);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].guid == kGuide);
    CHECK(ev[0].value[0] == 23500);
    CHECK(healer.timeLeftMs(h.now) == 23500);
    CHECK(healer.timeLeftSeconds(h.now) == 23);
    CHECK(healer.timeLeftSeconds(h.now + 600) == 22);
    CHECK(healer.timeLeftMs(h.now + 30000) == 0);
    // The popup's OnShow: AcceptAreaSpiritHeal.
    REQUIRE(healer.accept());
    REQUIRE(h.sentWith(Opcode::CMSG_AREA_SPIRIT_HEALER_QUEUE).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_AREA_SPIRIT_HEALER_QUEUE)[0] ==
          Bytes{0x01, 0x01, 0xCC, 0x32, 0x00, 0x00, 0x30, 0xF1});

    // Another guide's time, or a guide not channeling (0): nothing.
    REQUIRE(h.packet(Opcode::SMSG_AREA_SPIRIT_HEALER_TIME, healerTime(kHordeGuide, 9000)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_AREA_SPIRIT_HEALER_TIME, healerTime(kGuide, 0)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.events(GameEventKind::AreaSpiritHealerInRange).empty());
    CHECK(healer.timeLeftMs(h.now) == 23500);

    // CancelAreaSpiritHeal (the popup's Cancel): CMSG_CANCEL_AURA 2584 and _OUT_OF_RANGE; the healer stays.
    healer.cancel();
    REQUIRE(h.sentWith(Opcode::CMSG_CANCEL_AURA).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_CANCEL_AURA)[0] == Bytes{0x18, 0x0A, 0x00, 0x00});
    CHECK(h.events(GameEventKind::AreaSpiritHealerOutOfRange).size() == 1);
    CHECK(healer.healer() == kGuide);

    // The buff frame's cancel of Waiting to Resurrect does the same (0x6E7040 fires the event for 2584).
    h.play.cancelAura(game::AreaSpiritHealer::kWaitingToResurrect);
    CHECK(h.events(GameEventKind::AreaSpiritHealerOutOfRange).size() == 1);
    h.play.cancelAura(133);
    CHECK(h.events(GameEventKind::AreaSpiritHealerOutOfRange).empty());

    // 21 yd away: kept (22 yd, 0x4923B0); 23 yd: dropped, and with a time coming, out of the queue.
    h.sent.clear();
    h.moveMe(-11.0f, 0.0f);
    h.play.update(h.now);
    CHECK(healer.healer() == kGuide);
    h.moveMe(-13.0f, 0.0f);
    h.play.update(h.now);
    CHECK(healer.healer() == 0);
    CHECK(h.sentWith(Opcode::CMSG_CANCEL_AURA).size() == 1);
    CHECK(h.events(GameEventKind::AreaSpiritHealerOutOfRange).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY).empty()); // 23 yd: none in 20 yd to take

    // Back in range: found and asked again; the wave resurrects the player: PLAYER_UNGHOST drops the healer,
    // and the time it had cancels the aura (the server already removed it: harmless).
    h.moveMe(0.0f, 0.0f);
    h.play.update(h.now);
    CHECK(h.sentWith(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY).size() == 1);
    REQUIRE(h.packet(Opcode::SMSG_AREA_SPIRIT_HEALER_TIME, healerTime(kGuide, 4000)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.events(GameEventKind::AreaSpiritHealerInRange).size() == 1);
    h.sent.clear();
    h.ghost(false);
    h.play.update(h.now + 4000);
    CHECK(healer.healer() == 0);
    CHECK(h.sentWith(Opcode::CMSG_CANCEL_AURA).size() == 1);
    CHECK_FALSE(healer.accept()); // no healer: nothing to queue with
}

TEST_CASE(
    "PvP: tapping a spirit guide: without a gossip menu a ghost asks again, with one CMSG_GOSSIP_HELLO") {
    Harness h;
    h.apply(spiritGuide(kGuide, 10.0f, kAllianceTemplate));
    // Alive: nothing (0x5DF950 only acts for a ghost).
    CHECK(h.play.interact(kGuide) == game::Interaction::None);
    CHECK(h.sentWith(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY).empty());
    CHECK(h.sentWith(Opcode::CMSG_GOSSIP_HELLO).empty());
    // A ghost: the healer dropped and chosen again (a new query each tap).
    h.ghost(true);
    h.play.update(h.now);
    REQUIRE(h.sentWith(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY).size() == 1);
    CHECK(h.play.interact(kGuide) == game::Interaction::SpiritHealer);
    CHECK(h.sentWith(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY).size() == 2);
    CHECK(h.play.areaSpiritHealer().healer() == kGuide);
    // A guide with the gossip flag (0x1 first in the NPC chain): CMSG_GOSSIP_HELLO (vMaNGOS answers the time
    // and its script queues the player).
    Harness g;
    g.apply(spiritGuide(kGuide, 10.0f, kAllianceTemplate, 0x41));
    g.ghost(true);
    CHECK(g.play.interact(kGuide) == game::Interaction::SpiritHealer);
    CHECK(g.sentWith(Opcode::CMSG_GOSSIP_HELLO).size() == 1);
}

TEST_CASE(
    "PvP: honor credits as WoW.exe words them: the combat log's lines, their rank titles, the HK text") {
    Harness h;
    // Grommash, an orc (a Horde FactionTemplate), killed by the player: rank 6 on the Horde's side (Grunt).
    h.apply(created(kGrommash, game::ObjectType::Player, 3.0f, 0.0f,
                    {{uf::UNIT_FIELD_HEALTH, 0}, {uf::UNIT_FIELD_FACTIONTEMPLATE, kHordeTemplate}}));
    net::NameQueryInfo name;
    name.guid = kGrommash;
    name.name = "Grommash";
    h.play.queries().onName(std::move(name));
    REQUIRE(h.packet(Opcode::SMSG_PVP_CREDIT, pvpCredit(37, kGrommash, 6)) == game::Gameplay::Handled::Yes);
    auto ev = h.events(GameEventKind::HonorGained);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].guid == kGrommash);
    CHECK(ev[0].id == 6);
    CHECK(ev[0].value[0] == 37);
    CHECK(ev[0].value[1] == static_cast<i32>(HonorCredit::Honorable));
    CHECK(ev[0].value[2] == 0);
    auto lines = h.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].first == NoticeKind::HonorGain);
    CHECK(lines[0].second == "Grommash slain, honorable (Grunt, ~37 honor)");
    CHECK(game::noticePlace(NoticeKind::HonorGain) == game::NoticePlace::CombatLog);
    CHECK(game::noticeColor(NoticeKind::HonorGain).r == 224);
    CHECK(game::noticeColor(NoticeKind::HonorGain).g == 202);
    CHECK(game::noticeColor(NoticeKind::HonorGain).b == 10);
    char text[96];
    CHECK(game::formatHonorWorldText(text, ev[0], false) == "HK: Grunt");

    // A dishonorable kill (honor <= 0): a civilian of no side; its name from the creature cache.
    h.apply(created(kInnkeeper, game::ObjectType::Unit, 4.0f, 0.0f,
                    {{uf::OBJECT_FIELD_ENTRY, 6929},
                     {uf::UNIT_FIELD_HEALTH, 0},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, kNoSideTemplate}}));
    REQUIRE(h.packet(Opcode::SMSG_PVP_CREDIT, pvpCredit(-17, kInnkeeper, 0)) == game::Gameplay::Handled::Yes);
    ev = h.events(GameEventKind::HonorGained);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].value[1] == static_cast<i32>(HonorCredit::Dishonorable));
    CHECK(ev[0].id == 0);
    lines = h.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].second == "Unknown slain, dishonorable."); // no names in this test's notice feed
    CHECK(game::formatHonorWorldText(text, ev[0], false) == "DK: Civilian");
    CHECK(game::formatHonorLine(text, ev[0], "Innkeeper Gryshka", false) ==
          "Innkeeper Gryshka slain, dishonorable.");

    // Honor awarded (a battleground's bonus: no victim).
    REQUIRE(h.packet(Opcode::SMSG_PVP_CREDIT, pvpCredit(82, 0, 0)) == game::Gameplay::Handled::Yes);
    ev = h.events(GameEventKind::HonorGained);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].value[1] == static_cast<i32>(HonorCredit::Awarded));
    lines = h.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].second == "Honor awarded: 82.");
    CHECK(game::formatHonorWorldText(text, ev[0], false).empty()); // no "HK:" for an award

    // The victim the client no longer has, or an honorable kill of neither side: nothing at all (0x6252AB,
    // 0x625321).
    REQUIRE(h.packet(Opcode::SMSG_PVP_CREDIT, pvpCredit(37, 0x99, 6)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_PVP_CREDIT, pvpCredit(5, kInnkeeper, 19)) == game::Gameplay::Handled::Yes);
    CHECK(h.events(GameEventKind::HonorGained).empty());
    CHECK(h.lines().empty());
    CHECK(h.packet(Opcode::SMSG_PVP_CREDIT, Bytes{1, 2, 3}) == game::Gameplay::Handled::Malformed);

    // The rank title takes the player's gender (0x612BF0 asks the player object), the victim's side from its
    // template.
    game::GameEvent marshal = ev.empty() ? game::GameEvent{} : ev[0];
    marshal.kind = GameEventKind::HonorGained;
    marshal.id = 18;
    marshal.value = {200, static_cast<i32>(HonorCredit::Honorable), 1};
    CHECK(game::formatHonorLine(text, marshal, "Aeryn", true) ==
          "Aeryn slain, honorable (Grand Marshal, ~200 honor)");
    char key[48];
    CHECK(game::honorRankTitle(key, marshal, false) == "Grand Marshal");
    marshal.id = 0; // a rank without a title: empty
    CHECK(game::honorRankTitle(key, marshal, false).empty());
}

TEST_CASE("PvP: the insignia: Remove Insignia on an enemy's body or corpse, PLAYER_SKINNED's line") {
    Harness h;
    REQUIRE(h.packet(Opcode::SMSG_LEARNED_SPELL, u32Body(kRemoveInsignia)) == game::Gameplay::Handled::Yes);
    CHECK(h.play.insigniaSpell() == kRemoveInsignia);

    // An enemy player dead in its body, skinnable (BattleGround::HandleKillPlayer): cast at the unit.
    h.apply(created(kGrommash, game::ObjectType::Player, 3.0f, 0.0f,
                    {{uf::UNIT_FIELD_HEALTH, 0},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, kHordeTemplate},
                     {uf::UNIT_FIELD_FLAGS, game::kUnitFlagSkinnable}}));
    CHECK(h.play.interact(kGrommash) == game::Interaction::Loot);
    auto casts = h.sentWith(Opcode::CMSG_CAST_SPELL);
    REQUIRE(casts.size() == 1);
    CHECK(casts[0] == castSpell(kRemoveInsignia, net::kTargetUnit, kGrommash));

    // Released: its corpse (CORPSE_FLAG_LOOTABLE, an orc's): cast at the corpse (TARGET_FLAG_CORPSE_ENEMY).
    h.apply(created(kCorpse, game::ObjectType::Corpse, 3.0f, 0.0f,
                    {{uf::CORPSE_FIELD_OWNER, static_cast<u32>(kGrommash)},
                     {uf::CORPSE_FIELD_BYTES_1, 2u << 8}, // race 2 (orc) in byte 1
                     {uf::CORPSE_FIELD_FLAGS, 0x24}}));
    CHECK(h.play.interact(kCorpse) == game::Interaction::Loot);
    casts = h.sentWith(Opcode::CMSG_CAST_SPELL);
    REQUIRE(casts.size() == 2);
    CHECK(casts[1] == castSpell(kRemoveInsignia, net::kTargetCorpseEnemy, kCorpse));

    // A corpse of the player's side, or the bones the insignia left (no CORPSE_FLAG_LOOTABLE): not for the
    // spell.
    h.apply(values(kCorpse, {{uf::CORPSE_FIELD_BYTES_1, 1u << 8}})); // a human's
    CHECK(h.play.interact(kCorpse) == game::Interaction::None);
    h.apply(values(kCorpse, {{uf::CORPSE_FIELD_BYTES_1, 2u << 8},
                             {uf::CORPSE_FIELD_FLAGS, 0x05},
                             {uf::CORPSE_FIELD_DYNAMIC_FLAGS, 1}})); // bones, lootable
    CHECK(h.play.interact(kCorpse) == game::Interaction::Loot);
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL).size() == 2);
    CHECK(h.sentWith(Opcode::CMSG_LOOT).size() == 1);

    // Without the spell (never learned): nothing.
    Harness n;
    n.apply(created(kGrommash, game::ObjectType::Player, 3.0f, 0.0f,
                    {{uf::UNIT_FIELD_HEALTH, 0},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, kHordeTemplate},
                     {uf::UNIT_FIELD_FLAGS, game::kUnitFlagSkinnable}}));
    CHECK(n.play.interact(kGrommash) == game::Interaction::None);
    CHECK(n.sentWith(Opcode::CMSG_CAST_SPELL).empty());

    // The player's own insignia taken: PLAYER_SKINNED, DEATH_CORPSE_SKINNED in the UIErrorsFrame (red).
    REQUIRE(h.packet(Opcode::SMSG_PLAYER_SKINNED, Bytes{0}) == game::Gameplay::Handled::Yes);
    const auto ev = h.events(GameEventKind::PlayerSkinned);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].value[0] == 0);
    const auto lines = h.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].first == NoticeKind::Error);
    CHECK(lines[0].second == "Insignia taken: graveyard only");
    CHECK(h.packet(Opcode::SMSG_PLAYER_SKINNED, Bytes{}) == game::Gameplay::Handled::Malformed);
}

TEST_CASE(
    "PvP: under attack and the defense messages go to the joined defense channels, LocalDefense by zone") {
    Harness h(kAzeroth);
    // In Goldshire (Elwynn Forest): the client joins General and LocalDefense; the server confirms them.
    h.play.travel().updatePosition(kAzeroth, {0.0f, 0.0f, 0.0f}, 87, false, h.now);
    REQUIRE(h.play.travel().zone().zone == 12);
    const game::ZoneChannel elwynn[] = {{1, "General - Elwynn Forest"}, {22, "LocalDefense - Elwynn Forest"}};
    h.play.social().setZone(12, elwynn);
    REQUIRE(h.packet(Opcode::SMSG_CHANNEL_NOTIFY, channelJoined("General - Elwynn Forest")) ==
            game::Gameplay::Handled::Yes);
    // Not confirmed yet: LocalDefense takes nothing.
    REQUIRE(h.packet(Opcode::SMSG_ZONE_UNDER_ATTACK, u32Body(87)) == game::Gameplay::Handled::Yes);
    CHECK(h.chat().empty());
    REQUIRE(h.packet(Opcode::SMSG_CHANNEL_NOTIFY, channelJoined("LocalDefense - Elwynn Forest")) ==
            game::Gameplay::Handled::Yes);
    h.chat();

    // A guard killed in Goldshire: ZONE_UNDER_ATTACK with the area's name, as a sender-less channel line.
    REQUIRE(h.packet(Opcode::SMSG_ZONE_UNDER_ATTACK, u32Body(87)) == game::Gameplay::Handled::Yes);
    auto chat = h.chat();
    REQUIRE(chat.size() == 1);
    CHECK(chat[0] == "[2. LocalDefense] : |cffffff00Goldshire is under attack!|r");
    CHECK(h.play.honor().defenseLines() == 1);
    // Another zone's (Stormwind): not in LocalDefense; an area AreaTable lacks: nothing.
    REQUIRE(h.packet(Opcode::SMSG_ZONE_UNDER_ATTACK, u32Body(1519)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ZONE_UNDER_ATTACK, u32Body(4242)) == game::Gameplay::Handled::Yes);
    CHECK(h.chat().empty());

    // WorldDefense joined by hand: every zone's.
    REQUIRE(h.play.social().joinChannel("WorldDefense"));
    REQUIRE(h.packet(Opcode::SMSG_CHANNEL_NOTIFY, channelJoined("WorldDefense")) ==
            game::Gameplay::Handled::Yes);
    h.chat();
    REQUIRE(h.packet(Opcode::SMSG_ZONE_UNDER_ATTACK, u32Body(1519)) == game::Gameplay::Handled::Yes);
    chat = h.chat();
    REQUIRE(chat.size() == 1);
    CHECK(chat[0] == "[3. WorldDefense] : |cffffff00Stormwind City is under attack!|r");
    REQUIRE(h.packet(Opcode::SMSG_ZONE_UNDER_ATTACK, u32Body(87)) == game::Gameplay::Handled::Yes);
    CHECK(h.chat().size() == 2); // LocalDefense and WorldDefense

    // The Eastern Plaguelands' tower (Map::SendDefenseMessage to the whole map): only WorldDefense here; in
    // the zone, LocalDefense too (the subzone's parent counts: 2258 The Fungal Vale -> 139).
    const std::string tower = "|cffffff00The Alliance now holds Eastwall Tower!|r";
    REQUIRE(h.packet(Opcode::SMSG_DEFENSE_MESSAGE, defenseMessage(139, tower)) ==
            game::Gameplay::Handled::Yes);
    chat = h.chat();
    REQUIRE(chat.size() == 1);
    CHECK(chat[0] == "[3. WorldDefense] : " + tower);
    h.play.travel().updatePosition(kAzeroth, {0.0f, 0.0f, 0.0f}, 2258, false, h.now);
    const game::ZoneChannel plaguelands[] = {{1, "General - Eastern Plaguelands"},
                                             {22, "LocalDefense - Eastern Plaguelands"}};
    h.play.social().setZone(139, plaguelands);
    REQUIRE(h.packet(Opcode::SMSG_CHANNEL_NOTIFY, channelJoined("LocalDefense - Eastern Plaguelands")) ==
            game::Gameplay::Handled::Yes);
    h.chat();
    REQUIRE(h.packet(Opcode::SMSG_DEFENSE_MESSAGE, defenseMessage(2258, tower)) ==
            game::Gameplay::Handled::Yes);
    chat = h.chat();
    REQUIRE(chat.size() == 2);
    CHECK(chat[0] == "[2. LocalDefense] : " + tower);
    CHECK(h.packet(Opcode::SMSG_DEFENSE_MESSAGE, Bytes{0x8B, 0, 0, 0, 9, 0, 0, 0, 'x'}) ==
          game::Gameplay::Handled::Malformed);

    // The kinds by name and by ChatChannels id.
    CHECK(game::defenseChannelKind(std::string_view("LocalDefense - Durotar")) == game::kChannelLocalDefense);
    CHECK(game::defenseChannelKind(std::string_view("worlddefense")) == game::kChannelWorldDefense);
    CHECK(game::defenseChannelKind(std::string_view("WorldDefenseX")) == 0);
    CHECK(game::defenseChannelKind(std::string_view("General - Durotar")) == 0);
}

TEST_CASE(
    "PvP: the Eastern Plaguelands' towers and Silithus' silithyst as vMaNGOS sends their world states") {
    Harness h(kAzeroth);
    const game::WorldStates& ws = h.play.worldStates();
    // Entering zone 139 (Player::SendInitWorldStates -> OutdoorPvPEP::FillInitialWorldStates): the counts,
    // the slider hidden at 50 of 100, then each tower's seven states (Eastwall neutral here).
    REQUIRE(h.packet(Opcode::SMSG_INIT_WORLD_STATES, initStates(kAzeroth, 139,
                                                                {{2327, 0},
                                                                 {2328, 0},
                                                                 {2426, 0},
                                                                 {2427, 50},
                                                                 {2428, 100},
                                                                 {2354, 0},
                                                                 {2356, 0},
                                                                 {2357, 0},
                                                                 {2358, 0},
                                                                 {2359, 0},
                                                                 {2360, 0},
                                                                 {2361, 1}})) ==
            game::Gameplay::Handled::Yes);
    CHECK(ws.zoneId() == 139);
    CHECK(ws.value(2361) == 1);
    CHECK(ws.value(2426) == 0);
    // In Eastwall's radius (OPvPCapturePoint::HandlePlayerEnter): the bar shown, its neutral width, then its
    // place.
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2426, 1)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2428, 20)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2427, 50)) == game::Gameplay::Handled::Yes);
    // The Alliance pushes it (SendChangePhase) past the neutral band: the tower's states (UpdateTowerState),
    // the defense message, the capture sound from the banner (PlayDirectSound: SMSG_PLAY_SOUND), the count
    // (Update).
    for (u32 v = 51; v <= 61; ++v) {
        REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2427, v)) ==
                game::Gameplay::Handled::Yes);
    }
    const std::vector<std::pair<u32, u32>> progressing = {{2354, 0}, {2356, 0}, {2357, 1}, {2358, 0},
                                                          {2359, 0}, {2360, 0}, {2361, 0}};
    for (const auto& [id, v] : progressing) {
        REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(id, v)) ==
                game::Gameplay::Handled::Yes);
    }
    REQUIRE(h.packet(Opcode::SMSG_PLAY_SOUND, u32Body(8173)) ==
            game::Gameplay::Handled::Yes); // PVPFLAGCAPTUREDALLIANCE
    REQUIRE(h.packet(Opcode::SMSG_DEFENSE_MESSAGE,
                     defenseMessage(139, "|cffffff00The Alliance now holds Eastwall Tower!|r")) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2327, 1)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2328, 0)) == game::Gameplay::Handled::Yes);
    CHECK(ws.value(2427) == 61);
    CHECK(ws.value(2357) == 1);
    CHECK(ws.value(2361) == 0);
    CHECK(ws.value(2327) == 1);
    u32 fxCursor = 0;
    bool sound = false;
    while (const game::FxEvent* e = h.play.fx().next(fxCursor)) {
        sound = sound || (e->kind == game::FxEventKind::Sound && e->id == 8173);
    }
    CHECK(sound);
    // Out of the radius: the bar hidden (HandlePlayerLeave); out of the zone: everything zeroed
    // (SendRemoveWorldStates), then the next zone's set replaces it.
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2426, 0)) == game::Gameplay::Handled::Yes);
    CHECK(ws.value(2426) == 0);
    REQUIRE(h.packet(Opcode::SMSG_INIT_WORLD_STATES, initStates(kAzeroth, 28, {})) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(ws.has(2327));

    // Silithus (OutdoorPvPSI::FillInitialWorldStates): gathered by each side and the goal; a turn-in raises
    // one.
    REQUIRE(h.packet(Opcode::SMSG_INIT_WORLD_STATES,
                     initStates(kKalimdor, 1377, {{2313, 0}, {2314, 3}, {2317, 200}})) ==
            game::Gameplay::Handled::Yes);
    CHECK(ws.value(2317) == 200);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2313, 1)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2314, 3)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_WORLD_STATE, updateState(2317, 200)) ==
            game::Gameplay::Handled::Yes);
    CHECK(ws.value(2313) == 1);
    CHECK(ws.value(2314) == 3);
}

TEST_CASE("PvP: the client data the outdoor PvP and the defense channels rely on (db.vdpk)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    const auto table = [&](const char* path) {
        const std::optional<pack::PackAsset> asset = db.find(path);
        REQUIRE(asset);
        Result<db::DbcView> view = db::DbcView::parse(asset->data, asset->size);
        REQUIRE(view.ok());
        return view.value();
    };
    // ChatChannels: the two defense channels (flag 0x10000), LocalDefense zone dependent (0x2) and joined
    // (0x1).
    const db::DbcView channels = table("DBFilesClient\\ChatChannels.dbc");
    const u32 local = channels.findById(game::kChannelLocalDefense);
    const u32 world = channels.findById(game::kChannelWorldDefense);
    REQUIRE(local < channels.recordCount());
    REQUIRE(world < channels.recordCount());
    CHECK(channels.getU32(local, 1) == 0x10003);
    CHECK(channels.getU32(world, 1) == 0x10004);
    CHECK(channels.getString(local, 3) == "LocalDefense - %s");
    CHECK(channels.getString(world, 3) == "WorldDefense");
    // WorldStateUI: the always-up lines and the capture bar of zone 139 and Silithus read the states vMaNGOS
    // sends.
    const db::DbcView ui = table("DBFilesClient\\WorldStateUI.dbc");
    const auto row = [&](u32 id) {
        const u32 r = ui.findById(id);
        REQUIRE(r < ui.recordCount());
        return r;
    };
    CHECK(ui.getU32(row(136), 2) == 139);
    CHECK(ui.getString(row(136), 4) == "Towers Controlled: %2327w");
    CHECK(ui.getString(row(137), 4) == "Towers Controlled: %2328w");
    CHECK(ui.getU32(row(138), 23) == 2426); // the capture bar's state: shown while 1
    CHECK(ui.getString(row(138), 35) == "CAPTUREPOINT");
    CHECK(ui.getU32(row(138), 36) == 2427); // its place
    CHECK(ui.getU32(row(138), 37) == 2428); // its neutral band
    CHECK(ui.getU32(row(126), 2) == 1377);
    CHECK(ui.getString(row(126), 4) == "%2313w/%2317w");
    CHECK(ui.getString(row(127), 4) == "%2314w/%2317w");
}

TEST_CASE("PvP: cost of a ghost's look for a spirit guide (the per-frame work, Alterac Valley's crowd)") {
    Harness h;
    // 80 players and 120 creatures around (Alterac Valley at its fullest), none of them a spirit guide: the
    // worst case, the search runs every frame.
    for (u64 i = 0; i < 200; ++i) {
        const bool player = i < 80;
        h.apply(created(player ? 0x1000 + i : 0xF130000000100000ull + i,
                        player ? game::ObjectType::Player : game::ObjectType::Unit,
                        5.0f + static_cast<f32>(i % 20), static_cast<f32>(i / 20),
                        {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_FACTIONTEMPLATE, kHordeTemplate}}));
    }
    h.ghost(true);
    constexpr u32 kFrames = 2000;
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kFrames; ++i) {
        h.play.areaSpiritHealer().update(h.now + i * 16, &h.reactions);
    }
    const f64 us = std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("AreaSpiritHealer::update, 200 units, no guide: " << us / kFrames << " us per frame");
    CHECK(h.play.areaSpiritHealer().healer() == 0);
    CHECK(h.sentWith(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY).empty());
}

TEST_CASE("PvP: the client's texts honor and the graveyards use are in ui.vdpk, in English and Spanish") {
    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    static constexpr const char* kKeys[] = {"AREA_SPIRIT_HEAL",
                                            "DEATH_CORPSE_SKINNED",
                                            "COMBATLOG_HONORGAIN",
                                            "COMBATLOG_DISHONORGAIN",
                                            "COMBATLOG_HONORAWARD",
                                            "COMBAT_TEXT_HONOR_GAINED",
                                            "PVP_RANK_CIVILIAN",
                                            "PVP_RANK_CIVILIAN_FEMALE",
                                            "PVP_RANK_19_0",
                                            "PVP_RANK_19_1",
                                            "PVP_RANK_5_0",
                                            "PVP_RANK_18_1_FEMALE",
                                            "HK",
                                            "DK",
                                            "ZONE_UNDER_ATTACK",
                                            "CHAT_CHANNEL_GET",
                                            "UNKNOWNOBJECT",
                                            "SECONDS",
                                            "SECONDS_P1",
                                            "CANCEL"};
    for (const char* key : kKeys) {
        INFO(key);
        CHECK(text::has(key));
    }
    char buffer[256];
    CHECK(text::format(buffer, "AREA_SPIRIT_HEAL", 23, text::get("SECONDS_P1")) ==
          "Resurrection in 23 Seconds");
    const std::string_view attack = text::format(buffer, "ZONE_UNDER_ATTACK", "Goldshire");
    CHECK(attack.starts_with("|cffffff00Goldshire "));
    CHECK(attack.ends_with("|r"));
    // Spanish (locale/esES/pvp.lang; the ranks 1..18 in character.lang).
    REQUIRE(text::load(std::filesystem::path(std::getenv("VD_PACKS_DIR")) / "ui.vdpk", "esES"));
    // Phase 21: the esES client's own wording when ui.vdpk was baked with it, ours otherwise.
    const bool official = test::esESClientTexts();
    CHECK(text::format(buffer, "AREA_SPIRIT_HEAL", 23, text::get("SECONDS_P1")) ==
          (official ? "Resurrección en 23 Segundos" : "Resurrección en 23 segundos"));
    const std::string_view award = text::format(buffer, "COMBATLOG_HONORAWARD", 82);
    if (official) {
        CHECK(award.starts_with("Has recibido 82 "));
    } else {
        CHECK(award == "Se te han concedido 82 puntos de honor.");
    }
    CHECK(text::get("PVP_RANK_CIVILIAN") == "Civil");
    for (const char* key : kKeys) {
        INFO(key);
        CHECK(text::has(key));
    }
    text::unload();
}
