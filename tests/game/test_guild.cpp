// Phase 15 (guild, help): the guild frame's rules (GuildControl), creating a guild (Petitions), the tabard
// designer (TabardDesigner), the guild emblem on tabards (GuildEmblem, AppearanceDb) and the GM help desk
// (HelpDesk), end to end through game::Gameplay with a capturing packet sink. Server packets are written as
// vmangos-core 4641790 writes them; what the client sends is checked byte for byte; the rules against
// FriendsFrame.lua, PetitionFrame.lua, TabardFrame.lua, HelpFrame.lua and the WoW.exe 1.12.1 functions behind
// them. The client tables (HelpData, the guild tabard's ItemDisplayInfo flag and its composed layers) come
// from $VD_PACKS_DIR, skipped without it.
#include "core/Hash.h"
#include "game/Appearance.h"
#include "game/Entities.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "game/GuildControl.h"
#include "game/GuildEmblem.h"
#include "game/HelpData.h"
#include "game/HelpDesk.h"
#include "game/Petitions.h"
#include "game/TabardDesigner.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/GuildMessages.h"
#include "net/HelpMessages.h"
#include "net/SocialMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kMe = 0x11;
constexpr u64 kAeryn = 0x21;
constexpr u64 kBrakk = 0x22;
constexpr u64 kTolvir = 0x23;
constexpr u64 kHorde = 0x24;
constexpr u64 kRegistrar = 0xF1300000000013B2ull;
constexpr u64 kCharter = 0x4000000000000A21ull;
constexpr u32 kGuild = 7;

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    const u32 words = maxIndex / 32 + 1;
    std::vector<u32> mask(words, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(words));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
}

Bytes createdPlayer(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(game::ObjectType::Player)).u8v(0x40);
    w.f32v(10.0f).f32v(20.0f).f32v(0.0f).f32v(0.0f);
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

Fields playerFields(Fields extra = {}) {
    Fields f = {{uf::UNIT_FIELD_HEALTH, 100},
                {uf::UNIT_FIELD_MAXHEALTH, 120},
                {uf::UNIT_FIELD_BYTES_0, 1 | (8u << 8) | (1u << 16)},
                {uf::UNIT_FIELD_LEVEL, 20}};
    f.insert(f.end(), extra.begin(), extra.end());
    return f;
}

Bytes guidBody(u64 guid) {
    Bytes out;
    net::writeGuid(out, guid);
    return out;
}

Bytes u32Body(u32 value) {
    Bytes out;
    net::writeU32(out, value);
    return out;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    std::vector<Sent> sent;
    u32 now = 1000;

    explicit Harness(Fields extra = {}) {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kMe, 0, true, now);
        apply(createdPlayer(kMe, playerFields(extra)));
        play.update(now);
    }
    void apply(const Bytes& update) {
        REQUIRE(store.applyUpdateObject(update, now));
        play.afterUpdate({}, now);
    }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
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
};

std::vector<std::string> keys(const game::GuildNotices& ring, u32& cursor) {
    std::vector<std::string> out;
    while (const game::GuildNotice* n = ring.next(cursor)) {
        out.emplace_back(n->key ? n->key : "");
    }
    return out;
}

// The guild's query answer: its name, two named ranks and a design (or none).
Bytes guildQuery(u32 id, const game::GuildEmblem& emblem, u32 ranks = 5) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(id).cstring("Los Errantes");
    static const char* kNames[] = {"Guild Master", "Officer", "Veteran", "Member", "Initiate",
                                   "Sixth",        "Seventh", "Eighth",  "Ninth",  "Tenth"};
    for (u32 i = 0; i < net::kGuildRankCount; ++i) {
        w.cstring(i < ranks ? kNames[i] : "");
    }
    w.u32v(static_cast<u32>(emblem.emblemStyle)).u32v(static_cast<u32>(emblem.emblemColor));
    w.u32v(static_cast<u32>(emblem.borderStyle)).u32v(static_cast<u32>(emblem.borderColor));
    w.u32v(static_cast<u32>(emblem.backgroundColor));
    return out;
}

struct Member {
    u64 guid;
    const char* name;
    u32 rank;
    u8 level;
    bool online;
    const char* note = "";
};

Bytes roster(const std::vector<u32>& rights, const std::vector<Member>& members) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(static_cast<u32>(members.size())).cstring("Hola").cstring("Bienvenidos");
    w.u32v(static_cast<u32>(rights.size()));
    for (const u32 r : rights) {
        w.u32v(r);
    }
    for (const Member& m : members) {
        w.u64v(m.guid).u8v(m.online ? 1 : 0).cstring(m.name).u32v(m.rank).u8v(m.level).u8v(1).u32v(12);
        if (!m.online) {
            w.f32v(2.5f);
        }
        w.cstring(m.note).cstring("");
    }
    return out;
}

constexpr u32 kAllRights = net::kGuildRightAll;
constexpr u32 kOfficer = 0x40 | 0x1 | 0x2 | 0x10 | 0x20 | 0x80 | 0x100 | 0x1000 | 0x2000 | 0x4000;
constexpr u32 kChat = 0x40 | 0x1 | 0x2;

std::vector<std::string> rosterNames(game::Gameplay& play) {
    std::vector<std::string> out;
    const game::GuildState& g = play.social().guild();
    for (const u32 i : play.guildControl().roster()) {
        out.push_back(g.members[i].name);
    }
    return out;
}

game::GuildEmblem design(i32 a, i32 b, i32 c, i32 d, i32 e) {
    game::GuildEmblem out;
    out.emblemStyle = a;
    out.emblemColor = b;
    out.borderStyle = c;
    out.borderColor = d;
    out.backgroundColor = e;
    return out;
}

} // namespace

TEST_CASE("Guild control: the roster as 1.12.1 lists it, sorted by SortGuildRoster's key list") {
    Harness h({{uf::PLAYER_GUILDID, kGuild}, {uf::PLAYER_GUILDRANK, 1}});
    REQUIRE(h.packet(Opcode::SMSG_GUILD_QUERY_RESPONSE, guildQuery(kGuild, {})) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_GUILD_ROSTER, roster({kAllRights, kOfficer, kChat, kChat, kChat},
                                                       {{kMe, "Arthel", 1, 20, true},
                                                        {kAeryn, "Aeryn", 3, 40, true, "Tanque"},
                                                        {kBrakk, "Brakk", 4, 12, true},
                                                        {kTolvir, "Tolvir", 3, 40, false},
                                                        {kHorde, "Zalda", 0, 60, true}})) ==
            game::Gameplay::Handled::Yes);
    game::GuildControl& gc = h.play.guildControl();
    // Online only (no "Show Offline Members" in 1.12.1); by rank first, the higher index first (WoW.exe
    // 0x4D0E4E), then the level up, then the name.
    CHECK(rosterNames(h.play) == std::vector<std::string>{"Brakk", "Aeryn", "Arthel", "Zalda"});
    gc.sort(game::GuildSort::Rank); // the first key again: turned round
    CHECK(rosterNames(h.play) == std::vector<std::string>{"Zalda", "Arthel", "Aeryn", "Brakk"});
    gc.sort(game::GuildSort::Name); // a new first key, in its own order
    CHECK(rosterNames(h.play) == std::vector<std::string>{"Aeryn", "Arthel", "Brakk", "Zalda"});
    CHECK(gc.sortKey(0) == game::GuildSort::Name);
    CHECK(gc.sortKey(1) == game::GuildSort::Rank);
    CHECK(gc.sortReversed(1)); // the rank kept its turned order behind it
    gc.sort(game::GuildSort::Name);
    CHECK(rosterNames(h.play) == std::vector<std::string>{"Zalda", "Brakk", "Arthel", "Aeryn"});
    gc.sort(game::GuildSort::Level);
    CHECK(rosterNames(h.play).front() == "Brakk"); // level 12 first

    // The selection survives a new roster; an unknown name selects nothing.
    gc.select("Aeryn");
    REQUIRE(gc.selected());
    CHECK(gc.selected()->note == "Tanque");
    gc.select("Tolvir");
    CHECK_FALSE(gc.selected()); // offline: not listed
    gc.select("");
    CHECK_FALSE(gc.selected());
}

TEST_CASE("Guild control: GuildStatus_Update's rights, notes, the information text and the ranks") {
    // The officer (rank 1) with promote, demote, remove, MOTD, public note and view officer note.
    Harness h({{uf::PLAYER_GUILDID, kGuild}, {uf::PLAYER_GUILDRANK, 1}});
    REQUIRE(h.packet(Opcode::SMSG_GUILD_QUERY_RESPONSE, guildQuery(kGuild, {})) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_GUILD_ROSTER, roster({kAllRights, kOfficer, kChat, kChat, kChat},
                                                       {{kMe, "Arthel", 1, 20, true},
                                                        {kAeryn, "Aeryn", 3, 40, true},
                                                        {kBrakk, "Brakk", 4, 12, true},
                                                        {kHorde, "Zalda", 0, 60, true},
                                                        {kTolvir, "Tolvir", 2, 30, true}})) ==
            game::Gameplay::Handled::Yes);
    game::GuildControl& gc = h.play.guildControl();
    CHECK_FALSE(gc.isGuildLeader());
    CHECK(gc.hasRight(game::guildbit::kSetMotd));
    CHECK(gc.hasRight(game::guildbit::kViewOfficerNote));
    CHECK_FALSE(gc.hasRight(game::guildbit::kEditOfficerNote));
    CHECK_FALSE(gc.hasRight(game::guildbit::kModifyInfo));
    const game::GuildState& g = h.play.social().guild();
    const auto rightsOf = [&](const char* name) {
        for (const game::GuildMember& m : g.members) {
            if (m.name == name) {
                return gc.memberRights(m);
            }
        }
        FAIL("no such member");
        return game::GuildMemberRights{};
    };
    // Member (rank 3): promote (3 > 1 and 3 > 1 + 1), demote (not the lowest), remove.
    game::GuildMemberRights r = rightsOf("Aeryn");
    CHECK(r.promote);
    CHECK(r.demote);
    CHECK(r.remove);
    CHECK(r.groupInvite);
    CHECK(r.editPublicNote);
    CHECK(r.viewOfficerNote);
    CHECK_FALSE(r.editOfficerNote);
    // Veteran (rank 2): no promotion to officer by an officer (2 > 1 + 1 is false).
    r = rightsOf("Tolvir");
    CHECK_FALSE(r.promote);
    CHECK(r.demote);
    // Initiate (the lowest): no demotion.
    r = rightsOf("Brakk");
    CHECK(r.promote);
    CHECK_FALSE(r.demote);
    // The guild master and the player itself: nothing.
    r = rightsOf("Zalda");
    CHECK_FALSE(r.promote);
    CHECK_FALSE(r.demote);
    CHECK_FALSE(r.remove);
    r = rightsOf("Arthel");
    CHECK_FALSE(r.groupInvite);
    CHECK_FALSE(r.remove);
    // Pure rules at the edges.
    CHECK(game::GuildControl::canPromote(0, 2, true));
    CHECK_FALSE(game::GuildControl::canPromote(0, 1, true)); // nobody is promoted to guild master
    CHECK_FALSE(game::GuildControl::canPromote(0, 4, false));
    CHECK(game::GuildControl::canDemote(0, 1, 4, true));
    CHECK_FALSE(game::GuildControl::canDemote(0, 4, 4, true));
    CHECK_FALSE(game::GuildControl::canRemove(1, 1, true));

    // Notes and the information text.
    REQUIRE(gc.setPublicNote("Aeryn", "Tanque principal"));
    Bytes expected;
    net::writeGuildSetNote(expected, "Aeryn", "Tanque principal");
    CHECK(h.sentWith(Opcode::CMSG_GUILD_SET_PUBLIC_NOTE).back() == expected);
    REQUIRE(gc.setOfficerNote("Aeryn", std::string(40, 'x')));
    expected.clear();
    net::writeGuildSetNote(expected, "Aeryn", std::string(31, 'x')); // GUILD_NOTE_MAX_LENGTH
    CHECK(h.sentWith(Opcode::CMSG_GUILD_SET_OFFICER_NOTE).back() == expected);
    const usize rosters = h.sentWith(Opcode::CMSG_GUILD_ROSTER).size();
    REQUIRE(gc.setInfoText("Raid los viernes"));
    expected.clear();
    net::writeCString(expected, "Raid los viernes");
    CHECK(h.sentWith(Opcode::CMSG_GUILD_INFO_TEXT).back() == expected);
    CHECK(h.sentWith(Opcode::CMSG_GUILD_ROSTER).size() == rosters + 1); // GuildInfoSaveButton's GuildRoster()

    // Ranks: the guild master's alone.
    CHECK(gc.rankCount() == 5);
    CHECK(gc.rankName(1) == "Officer");
    CHECK_FALSE(gc.saveRank(2, "Veterano", gc.rankFlags(2)));
    CHECK_FALSE(gc.addRank("Recluta"));
    CHECK_FALSE(gc.deleteLastRank());
}

TEST_CASE("Guild control: the guild master's GuildControlPopupFrame") {
    Harness h({{uf::PLAYER_GUILDID, kGuild}, {uf::PLAYER_GUILDRANK, 0}});
    REQUIRE(h.packet(Opcode::SMSG_GUILD_QUERY_RESPONSE, guildQuery(kGuild, {}, 6)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_GUILD_ROSTER, roster({kAllRights, kOfficer, kChat, kChat, kChat, kChat},
                                                       {{kMe, "Arthel", 0, 20, true},
                                                        {kAeryn, "Aeryn", 4, 40, true},
                                                        {kBrakk, "Brakk", 5, 12, false}})) ==
            game::Gameplay::Handled::Yes);
    game::GuildControl& gc = h.play.guildControl();
    REQUIRE(gc.isGuildLeader());
    CHECK(gc.rankCount() == 6);
    // GuildControlGetRankFlags: the check boxes from the rights (WoW.exe's table: promote is 0x80, invite
    // 0x10...).
    std::array<bool, game::kGuildControlOptions> flags = gc.rankFlags(1);
    CHECK(flags[0]);        // guild chat listen
    CHECK(flags[4]);        // promote (0x80)
    CHECK(flags[6]);        // invite (0x10)
    CHECK_FALSE(flags[11]); // edit officer note
    CHECK_FALSE(flags[12]); // modify guild info
    flags = gc.rankFlags(2);
    CHECK(flags[0]);
    CHECK(flags[1]);
    CHECK_FALSE(flags[2]);
    // GuildControlSaveRank: each box's bit applied to the rank's rights, the 0x40 kept.
    flags[1] = false; // no guild chat speak
    flags[9] = true;  // edit public note (0x2000)
    flags[12] = true; // modify guild info (0x10000)
    REQUIRE(gc.saveRank(2, "Veterano", flags));
    Bytes expected;
    net::writeGuildRank(expected, 2, 0x40 | 0x1 | 0x2000 | 0x10000, "Veterano");
    CHECK(h.sentWith(Opcode::CMSG_GUILD_RANK).back() == expected);
    CHECK_FALSE(gc.saveRank(2, "", flags));                    // a name is needed
    CHECK_FALSE(gc.saveRank(2, "Un nombre muy largo", flags)); // 15 letters at most
    CHECK_FALSE(gc.saveRank(6, "Fantasma", flags));            // a rank that exists

    // Add below 10, delete above 5; the remove button only for the lowest rank nobody listed holds.
    REQUIRE(gc.addRank("Recluta"));
    expected.clear();
    net::writeCString(expected, "Recluta");
    CHECK(h.sentWith(Opcode::CMSG_GUILD_ADD_RANK).back() == expected);
    CHECK(gc.canDeleteRank(5));       // Brakk holds it but is offline (not listed: playersInBotRank 0)
    CHECK_FALSE(gc.canDeleteRank(4)); // not the lowest
    REQUIRE(gc.deleteLastRank());
    CHECK(h.sentWith(Opcode::CMSG_GUILD_DEL_RANK).back().empty());
}

TEST_CASE("Petitions: the registrar, buying a charter with the client's checks, turning it in") {
    Harness h({{uf::PLAYER_FIELD_COINAGE, 500}});
    game::Petitions& pet = h.play.petitions();
    u32 cursor = 0;
    // An entry without flag 1: no window.
    Bytes hidden;
    net::ByteWriter(hidden)
        .u64v(kRegistrar)
        .u8v(1)
        .u32v(1)
        .u32v(net::kGuildCharterEntry)
        .u32v(16161)
        .u32v(1000)
        .u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_PETITION_SHOWLIST, hidden) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(pet.registrar().open);
    Bytes list;
    net::ByteWriter(list)
        .u64v(kRegistrar)
        .u8v(1)
        .u32v(1)
        .u32v(net::kGuildCharterEntry)
        .u32v(16161)
        .u32v(1000)
        .u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_PETITION_SHOWLIST, list) == game::Gameplay::Handled::Yes);
    REQUIRE(pet.registrar().open);
    CHECK(pet.registrar().cost == 1000);
    CHECK(pet.registrar().npc == kRegistrar);

    // Names first (WoW.exe 0x4F5160), then the money (50 silver short of 10).
    CHECK(game::Petitions::guildNameError("") == std::string("ERR_GUILD_ENTER_NAME"));
    CHECK(game::Petitions::guildNameError(" Los Errantes") == std::string("ERR_GUILD_NAME_INVALID_SPACE"));
    CHECK(game::Petitions::guildNameError("Los  Errantes") ==
          std::string("ERR_GUILD_NAME_NAME_CONSECUTIVE_SPACES"));
    CHECK(game::Petitions::guildNameError("L") == std::string("ERR_GUILD_NAME_TOO_SHORT"));
    CHECK(game::Petitions::guildNameError("Los Err4ntes") == std::string("ERR_GUILD_NAME_INVALID"));
    CHECK(game::Petitions::guildNameError("Los Errantes") == nullptr);
    CHECK(game::Petitions::guildNameError("Los Ñandúes") == nullptr); // any UTF-8 letter
    CHECK_FALSE(pet.buyCharter("Los Err4ntes", h.now));
    CHECK_FALSE(pet.buyCharter("Los Errantes", h.now));
    CHECK(keys(pet.notices(), cursor) ==
          std::vector<std::string>{"ERR_GUILD_NAME_INVALID", "ERR_NOT_ENOUGH_MONEY"});
    CHECK(h.sentWith(Opcode::CMSG_PETITION_BUY).empty());
    h.apply(values(kMe, {{uf::PLAYER_FIELD_COINAGE, 1500}}));
    REQUIRE(pet.buyCharter("Los Errantes", h.now));
    Bytes expected;
    net::writePetitionBuy(expected, kRegistrar, "Los Errantes", 1);
    CHECK(h.sentWith(Opcode::CMSG_PETITION_BUY).back() == expected);

    // Turning in: no charter carried is ERR_NO_GUILD_CHARTER; the answers.
    CHECK_FALSE(pet.turnInCharter());
    CHECK(keys(pet.notices(), cursor) == std::vector<std::string>{"ERR_NO_GUILD_CHARTER"});
    REQUIRE(h.packet(Opcode::SMSG_TURN_IN_PETITION_RESULTS, u32Body(net::kPetitionNeedMore)) ==
            game::Gameplay::Handled::Yes);
    CHECK(keys(pet.notices(), cursor) == std::vector<std::string>{"ERR_PETITION_NOT_ENOUGH_SIGNATURES"});
    CHECK(pet.registrar().open);
    REQUIRE(h.packet(Opcode::SMSG_TURN_IN_PETITION_RESULTS, u32Body(net::kPetitionSignOk)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(pet.registrar().open); // GUILD_REGISTRAR_CLOSED
    CHECK(keys(pet.notices(), cursor).empty());
    // In a guild: refused before anything is sent.
    REQUIRE(h.packet(Opcode::SMSG_PETITION_SHOWLIST, list) == game::Gameplay::Handled::Yes);
    h.apply(values(kMe, {{uf::PLAYER_GUILDID, kGuild}}));
    h.play.update(h.now);
    CHECK_FALSE(pet.buyCharter("Otro Nombre", h.now));
    CHECK(keys(pet.notices(), cursor) == std::vector<std::string>{"ERR_ALREADY_IN_GUILD"});
    CHECK(h.packet(Opcode::SMSG_PETITION_SHOWLIST, Bytes{1, 2, 3}) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Petitions: the owner's charter, offering it, signatures arriving, renaming") {
    Harness h;
    h.apply(createdPlayer(kAeryn, playerFields()));
    h.apply(createdPlayer(kBrakk, playerFields({{uf::PLAYER_GUILDID, 3}})));
    game::Petitions& pet = h.play.petitions();
    u32 cursor = 0;
    REQUIRE(pet.showCharter(kCharter));
    CHECK(h.sentWith(Opcode::CMSG_PETITION_SHOW_SIGNATURES).back() == guidBody(kCharter));
    Bytes shown;
    net::ByteWriter(shown).u64v(kCharter).u64v(kMe).u32v(77).u8v(1).u64v(kTolvir).u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_PETITION_SHOW_SIGNATURES, shown) == game::Gameplay::Handled::Yes);
    REQUIRE(pet.petition().open);
    CHECK(pet.isOriginator());
    CHECK(pet.petition().signatures == 1);
    CHECK_FALSE(pet.canSign()); // the originator does not sign
    Bytes query;
    net::writePetitionQuery(query, 77, kCharter);
    CHECK(h.sentWith(Opcode::CMSG_PETITION_QUERY).back() == query);
    Bytes response;
    net::ByteWriter q(response);
    q.u32v(77).u64v(kMe).cstring("Los Errantes").cstring("").u32v(1).u32v(9).u32v(9);
    q.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u16v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_PETITION_QUERY_RESPONSE, response) == game::Gameplay::Handled::Yes);
    CHECK(pet.petition().name == "Los Errantes");
    CHECK(pet.petition().maxSignatures == 9);

    // OfferPetition's checks: the player itself, a player in a guild, an enemy, then Aeryn.
    CHECK_FALSE(pet.offer(kMe, true, h.now));
    CHECK_FALSE(pet.offer(kBrakk, true, h.now));
    CHECK_FALSE(pet.offer(kAeryn, false, h.now));
    CHECK_FALSE(pet.offer(0xF130000000000001ull, true, h.now)); // nobody in sight
    REQUIRE(pet.offer(kAeryn, true, h.now));
    Bytes offered;
    net::writeOfferPetition(offered, kCharter, kAeryn);
    CHECK(h.sentWith(Opcode::CMSG_OFFER_PETITION).back() == offered);
    CHECK(keys(pet.notices(), cursor) ==
          std::vector<std::string>{"ERR_PETITION_CREATOR", "ERR_ALREADY_IN_GUILD_S", "ERR_GUILD_NOT_ALLIED",
                                   "ERR_OUT_OF_RANGE", "ERR_PETITION_OFFERED_S"});

    // Aeryn signs (the owner's copy of the result): added once, ERR_PETITION_SIGNED_S; a decline.
    Bytes signedIt;
    net::ByteWriter(signedIt).u64v(kCharter).u64v(kAeryn).u32v(net::kPetitionSignOk);
    REQUIRE(h.packet(Opcode::SMSG_PETITION_SIGN_RESULTS, signedIt) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_PETITION_SIGN_RESULTS, signedIt) == game::Gameplay::Handled::Yes);
    CHECK(pet.petition().signatures == 2);
    CHECK(pet.petition().signers[1] == kAeryn);
    REQUIRE(h.packet(Opcode::MSG_PETITION_DECLINE, guidBody(kBrakk)) == game::Gameplay::Handled::Yes);
    u32 before = cursor;
    std::vector<std::string> notices = keys(pet.notices(), cursor);
    CHECK(notices == std::vector<std::string>{"ERR_PETITION_SIGNED_S", "ERR_PETITION_DECLINED_S"});
    const game::GuildNotice* first = pet.notices().next(before);
    REQUIRE(first);
    CHECK(first->guid == kAeryn);

    // Renaming: the name checks, the request, the answer.
    CHECK_FALSE(pet.rename("X", h.now));
    REQUIRE(pet.rename("Los Otros", h.now));
    Bytes rename;
    net::writePetitionRename(rename, kCharter, "Los Otros");
    CHECK(h.sentWith(Opcode::MSG_PETITION_RENAME).back() == rename);
    REQUIRE(h.packet(Opcode::MSG_PETITION_RENAME, rename) == game::Gameplay::Handled::Yes);
    CHECK(pet.petition().name == "Los Otros");

    // Closing the owner's own charter declines nothing.
    pet.closePetition();
    CHECK_FALSE(pet.petition().open);
    CHECK(h.sentWith(Opcode::MSG_PETITION_DECLINE).empty());
}

TEST_CASE("Petitions: a charter someone offers: signing, the answers, declining by closing it") {
    Harness h;
    game::Petitions& pet = h.play.petitions();
    u32 cursor = 0;
    Bytes shown;
    net::ByteWriter(shown).u64v(kCharter).u64v(kAeryn).u32v(77).u8v(0);
    REQUIRE(h.packet(Opcode::SMSG_PETITION_SHOW_SIGNATURES, shown) == game::Gameplay::Handled::Yes);
    REQUIRE(pet.petition().open);
    CHECK_FALSE(pet.isOriginator());
    CHECK(pet.canSign());
    // Closed unsigned: MSG_PETITION_DECLINE with the charter (WoW.exe 0x4F3F60).
    pet.closePetition();
    CHECK(h.sentWith(Opcode::MSG_PETITION_DECLINE).back() == guidBody(kCharter));

    REQUIRE(h.packet(Opcode::SMSG_PETITION_SHOW_SIGNATURES, shown) == game::Gameplay::Handled::Yes);
    REQUIRE(pet.sign());
    Bytes sign;
    net::writePetitionSign(sign, kCharter);
    CHECK(h.sentWith(Opcode::CMSG_PETITION_SIGN).back() == sign);
    const auto result = [&](u32 code) {
        Bytes body;
        net::ByteWriter(body).u64v(kCharter).u64v(kMe).u32v(code);
        return body;
    };
    for (const u32 code : {net::kPetitionAlreadySigned, net::kPetitionAlreadyInGuild,
                           net::kPetitionCantSignOwn, net::kPetitionNotServer, net::kPetitionNeedMore}) {
        REQUIRE(h.packet(Opcode::SMSG_PETITION_SIGN_RESULTS, result(code)) == game::Gameplay::Handled::Yes);
    }
    CHECK(keys(pet.notices(), cursor) ==
          std::vector<std::string>{"ERR_PETITION_ALREADY_SIGNED", "ERR_PETITION_IN_GUILD",
                                   "ERR_PETITION_CREATOR", "ERR_PETITION_NOT_SAME_SERVER",
                                   "VD_GUILD_PETITION_ERROR"});
    REQUIRE(h.packet(Opcode::SMSG_PETITION_SIGN_RESULTS, result(net::kPetitionSignOk)) ==
            game::Gameplay::Handled::Yes);
    CHECK(keys(pet.notices(), cursor) == std::vector<std::string>{"ERR_PETITION_SIGNED"});
    CHECK_FALSE(pet.petition().open);                            // PETITION_CLOSED
    CHECK(h.sentWith(Opcode::MSG_PETITION_DECLINE).size() == 1); // signed: no decline
}

TEST_CASE("Tabard designer: the guild's design, the arrows, Accept's checks and the answers") {
    Harness h({{uf::PLAYER_GUILDID, kGuild}, {uf::PLAYER_GUILDRANK, 0}, {uf::PLAYER_FIELD_COINAGE, 200000}});
    const game::GuildEmblem saved = design(12, 3, 2, 7, 40);
    REQUIRE(h.packet(Opcode::SMSG_GUILD_QUERY_RESPONSE, guildQuery(kGuild, saved)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.play.social().guildEmblem(kGuild));
    CHECK(*h.play.social().guildEmblem(kGuild) == saved);
    game::TabardDesigner& tab = h.play.tabards();
    u32 cursor = 0;
    REQUIRE(h.packet(Opcode::MSG_TABARDVENDOR_ACTIVATE, guidBody(kRegistrar)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(tab.open());
    CHECK(tab.vendor() == kRegistrar);
    CHECK(tab.design() == saved); // InitializeTabardColors: the guild's own
    CHECK(tab.guildMaster());
    CHECK(tab.canSaveNow());
    // Accept with the same design: ERR_GUILDEMBLEM_SAME, nothing sent.
    CHECK_FALSE(tab.save(h.now));
    CHECK(keys(tab.notices(), cursor) == std::vector<std::string>{"ERR_GUILDEMBLEM_SAME"});
    // The arrows wrap round their counts.
    tab.cycle(0, -1);
    CHECK(tab.design().emblemStyle == 11);
    tab.cycle(2, +1);
    tab.cycle(2, +1);
    tab.cycle(2, +1);
    tab.cycle(2, +1);
    CHECK(tab.design().borderStyle == 0); // 2 + 4 round 6
    tab.cycle(4, +1);
    tab.cycle(4, +1);
    tab.cycle(4, +1);
    tab.cycle(4, +1);
    tab.cycle(4, +1);
    tab.cycle(4, +1);
    tab.cycle(4, +1);
    tab.cycle(4, +1);
    tab.cycle(4, +1);
    tab.cycle(4, +1);
    tab.cycle(4, +1);
    CHECK(tab.design().backgroundColor == 0); // 40 + 11 round 51
    tab.cycle(1, -1);
    tab.cycle(1, -1);
    tab.cycle(1, -1);
    tab.cycle(1, -1);
    CHECK(tab.design().emblemColor == 16); // 3 - 4 round 17
    REQUIRE(tab.save(h.now));
    Bytes expected;
    net::GuildEmblemChoice choice{11, 16, 0, 7, 0};
    net::writeSaveGuildEmblem(expected, kRegistrar, choice);
    CHECK(h.sentWith(Opcode::MSG_SAVE_GUILD_EMBLEM).back() == expected);
    CHECK_FALSE(tab.canSaveNow()); // waits for the answer
    for (u32 code = 0; code <= 5; ++code) {
        REQUIRE(h.packet(Opcode::MSG_SAVE_GUILD_EMBLEM, u32Body(code)) == game::Gameplay::Handled::Yes);
    }
    CHECK(keys(tab.notices(), cursor) ==
          std::vector<std::string>{"ERR_GUILDEMBLEM_SUCCESS", "ERR_GUILDEMBLEM_INVALID_TABARD_COLORS",
                                   "ERR_GUILDEMBLEM_NOGUILD", "ERR_GUILDEMBLEM_NOTGUILDMASTER",
                                   "ERR_GUILDEMBLEM_NOTENOUGHMONEY"});
    CHECK(tab.canSaveNow());
    // Not a guild master: TABARDVENDORNOGUILDGREETING, and Accept refuses.
    h.apply(values(kMe, {{uf::PLAYER_GUILDRANK, 2}}));
    h.play.update(h.now);
    CHECK_FALSE(tab.guildMaster());
    CHECK_FALSE(tab.save(h.now));
    CHECK(keys(tab.notices(), cursor) == std::vector<std::string>{"ERR_GUILDEMBLEM_NOTGUILDMASTER"});
    tab.close();
    CHECK_FALSE(tab.open());
}

TEST_CASE("Tabard designer: a guild without a design starts from a random one within the counts") {
    Harness h({{uf::PLAYER_GUILDID, kGuild}, {uf::PLAYER_GUILDRANK, 0}, {uf::PLAYER_FIELD_COINAGE, 50000}});
    REQUIRE(h.packet(Opcode::SMSG_GUILD_QUERY_RESPONSE, guildQuery(kGuild, {})) ==
            game::Gameplay::Handled::Yes);
    game::TabardDesigner& tab = h.play.tabards();
    tab.setSeed(1234);
    REQUIRE(h.packet(Opcode::MSG_TABARDVENDOR_ACTIVATE, guidBody(kRegistrar)) ==
            game::Gameplay::Handled::Yes);
    CHECK(tab.design().designed());
    u32 cursor = 0;
    CHECK_FALSE(tab.save(h.now)); // 5 gold of 10
    CHECK(keys(tab.notices(), cursor) == std::vector<std::string>{"ERR_NOT_ENOUGH_MONEY"});
    CHECK(h.packet(Opcode::MSG_TABARDVENDOR_ACTIVATE, Bytes{1, 2}) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Guild emblem: the textures of a design for each torso region") {
    const game::GuildEmblem e = design(105, 3, 2, 16, 50);
    CHECK(e.designed());
    CHECK_FALSE(game::GuildEmblem{}.designed());
    CHECK_FALSE(design(170, 0, 0, 0, 0).designed()); // 170 emblems: 0..169
    CHECK_FALSE(design(0, 0, 6, 0, 0).designed());   // borders 0..5
    const game::EmblemTextures up = game::emblemTextures(e, true);
    CHECK(up.background == assetPathHash("Textures\\GuildEmblems\\Background_50_TU_U.blp"));
    CHECK(up.border == assetPathHash("Textures\\GuildEmblems\\Border_02_16_TU_U.blp"));
    CHECK(up.emblem == assetPathHash("Textures\\GuildEmblems\\Emblem_105_03_TU_U.blp"));
    const game::EmblemTextures low = game::emblemTextures(e, false);
    CHECK(low.emblem == assetPathHash("textures\\guildemblems\\EMBLEM_105_03_TL_U.BLP")); // paths ignore case
    CHECK(game::emblemTextures(game::GuildEmblem{}, true).background == 0);
}

TEST_CASE("Guild emblem: a guild tabard composes the design over the torso (client data)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    if (!dir) {
        MESSAGE("VD_PACKS_DIR not set: skipping");
        return;
    }
    const std::filesystem::path root(dir);
    pack::PackReader db, textures, actors;
    if (!db.open(root / "db.vdpk").ok() || !textures.open(root / "textures.vdpk").ok() ||
        !actors.open(root / "actors.vdpk").ok()) {
        MESSAGE("packs missing: skipping");
        return;
    }
    game::AppearanceDb appearance;
    REQUIRE(appearance.init(db, textures, actors).ok());
    CHECK(appearance.guildTabard(game::kGuildTabardDisplay));
    CHECK(appearance.guildTabard(9589)); // the Crusader's tabard has the flag too (Stormwind's)
    CHECK_FALSE(appearance.guildTabard(9590 + 1));
    game::CharacterLook c;
    c.race = 1;
    c.gender = 0;
    c.items[game::kSlotTabard] = game::kGuildTabardDisplay;
    const game::ActorLook plain = appearance.character(c);
    c.emblem = design(10, 3, 2, 7, 5);
    const game::ActorLook guild = appearance.character(c);
    const game::EmblemTextures up = game::emblemTextures(c.emblem, true);
    const auto hasLayer = [](const game::ActorLook& look, u64 texture) {
        return std::any_of(look.skin.begin(), look.skin.end(),
                           [&](const game::SkinLayer& l) { return l.texture == texture; });
    };
    CHECK_FALSE(hasLayer(plain, up.emblem));
    CHECK(hasLayer(guild, up.background));
    CHECK(hasLayer(guild, up.border));
    CHECK(hasLayer(guild, up.emblem));
    // In that order over each other, after the chest's layers.
    auto at = [&](u64 texture) {
        return std::find_if(guild.skin.begin(), guild.skin.end(),
                            [&](const game::SkinLayer& l) { return l.texture == texture; }) -
               guild.skin.begin();
    };
    CHECK(at(up.background) < at(up.border));
    CHECK(at(up.border) < at(up.emblem));
    CHECK_FALSE(plain == guild);
}

TEST_CASE("Help desk: the ticket from login to deletion, WoW.exe's answers") {
    Harness h;
    game::HelpDesk& help = h.play.help();
    u32 cursor = 0;
    // PLAYER_ENTERING_WORLD: GetGMTicket.
    REQUIRE(h.sentWith(Opcode::CMSG_GMTICKET_GETTICKET).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_GMTICKET_GETTICKET)[0].empty());
    REQUIRE(h.packet(Opcode::SMSG_GMTICKET_GETTICKET, u32Body(net::kTicketStatusDefault)) ==
            game::Gameplay::Handled::Yes);
    CHECK(help.ticket().known);
    CHECK_FALSE(help.ticket().open);

    CHECK_FALSE(help.create(1, "", 0, 0, 0, 0));
    CHECK(keys(help.notices(), cursor) == std::vector<std::string>{"ERR_TICKET_NO_TEXT"});
    REQUIRE(help.create(1, "Atascado", 1, 1.5f, 2.5f, 3.5f));
    Bytes expected;
    net::GmTicketCreate t{1, 1, 1.5f, 2.5f, 3.5f, "Atascado"};
    net::writeGmTicketCreate(expected, t);
    CHECK(h.sentWith(Opcode::CMSG_GMTICKET_CREATE).back() == expected);
    // Created: the ticket is asked for again; it comes with its wait.
    REQUIRE(h.packet(Opcode::SMSG_GMTICKET_CREATE, u32Body(net::kTicketCreated)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_GMTICKET_GETTICKET).size() == 2);
    Bytes ticket;
    net::ByteWriter(ticket)
        .u32v(net::kTicketStatusHasText)
        .cstring("Atascado")
        .u8v(1)
        .f32v(0.0f)
        .f32v(0.01f)
        .f32v(0.001f)
        .u8v(0)
        .u8v(0);
    REQUIRE(h.packet(Opcode::SMSG_GMTICKET_GETTICKET, ticket) == game::Gameplay::Handled::Yes);
    REQUIRE(help.ticket().open);
    CHECK(help.ticket().text == "Atascado");
    CHECK(help.ticket().category == 1);
    // 0.01 days = 864 s: "Approximate time remaining", counting down from when it came.
    game::TicketWait w = game::HelpDesk::waitText(help.ticket(), h.now);
    CHECK(w.key == std::string("GM_TICKET_WAIT_TIME"));
    CHECK(w.seconds == 864);
    w = game::HelpDesk::waitText(help.ticket(), h.now + 64000);
    CHECK(w.seconds == 800);
    // Every 10 minutes while open, asked again.
    h.play.update(h.now + 599000);
    CHECK(h.sentWith(Opcode::CMSG_GMTICKET_GETTICKET).size() == 2);
    h.play.update(h.now + 600000);
    CHECK(h.sentWith(Opcode::CMSG_GMTICKET_GETTICKET).size() == 3);

    // The answers WoW.exe shows.
    for (const u32 code : {net::kTicketAlreadyExists, net::kTicketCreateError, net::kTicketUpdateError}) {
        REQUIRE(h.packet(Opcode::SMSG_GMTICKET_CREATE, u32Body(code)) == game::Gameplay::Handled::Yes);
    }
    REQUIRE(h.packet(Opcode::SMSG_GMTICKET_GETTICKET, u32Body(0)) == game::Gameplay::Handled::Yes);
    CHECK(keys(help.notices(), cursor) ==
          std::vector<std::string>{"ERR_TICKET_ALREADY_EXISTS", "ERR_TICKET_CREATE_ERROR",
                                   "ERR_TICKET_UPDATE_ERROR", "ERR_TICKET_DB_ERROR"});
    CHECK_FALSE(help.ticket().open);

    REQUIRE(help.updateText(4, "Nuevo texto"));
    expected.clear();
    net::writeGmTicketUpdateText(expected, 4, "Nuevo texto");
    CHECK(h.sentWith(Opcode::CMSG_GMTICKET_UPDATETEXT).back() == expected);
    REQUIRE(h.packet(Opcode::SMSG_GMTICKET_GETTICKET, ticket) == game::Gameplay::Handled::Yes);
    REQUIRE(help.remove());
    CHECK(h.sentWith(Opcode::CMSG_GMTICKET_DELETETICKET).back().empty());
    REQUIRE(h.packet(Opcode::SMSG_GMTICKET_DELETETICKET, u32Body(net::kTicketDeleted)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(help.ticket().open);
    REQUIRE(h.packet(Opcode::SMSG_GMTICKET_DELETETICKET, u32Body(1)) == game::Gameplay::Handled::Yes);
    CHECK(keys(help.notices(), cursor) == std::vector<std::string>{"ERR_TICKET_DB_ERROR"});
    CHECK(h.packet(Opcode::SMSG_GMTICKET_GETTICKET, Bytes{6, 0, 0, 0, 'x'}) ==
          game::Gameplay::Handled::Malformed);
}

TEST_CASE("Help desk: the queue's status, the status line's texts, the survey, bug reports, Stuck") {
    Harness h;
    game::HelpDesk& help = h.play.help();
    CHECK(help.queueActive()); // PETITION_QUEUE_ACTIVE starts as 1
    REQUIRE(help.requestStatus());
    CHECK(h.sentWith(Opcode::CMSG_GMTICKET_SYSTEMSTATUS).size() == 1);
    REQUIRE(h.packet(Opcode::SMSG_GMTICKET_SYSTEMSTATUS, u32Body(net::kTicketQueueDisabled)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(help.queueActive());
    CHECK_FALSE(help.takeQueueDisabledAlert()); // 0: no alert (only -1 shows it at once)
    REQUIRE(h.packet(Opcode::SMSG_GMTICKET_SYSTEMSTATUS, u32Body(0xFFFFFFFFu)) ==
            game::Gameplay::Handled::Yes);
    CHECK(help.takeQueueDisabledAlert());
    REQUIRE(h.packet(Opcode::SMSG_GMTICKET_SYSTEMSTATUS, u32Body(net::kTicketQueueEnabled)) ==
            game::Gameplay::Handled::Yes);
    CHECK(help.queueActive());

    // HelpFrameOpenTicket_OnEvent's status texts.
    game::GmTicketState t;
    t.open = true;
    t.openedByGm = 1;
    CHECK(game::HelpDesk::waitText(t, 0).key == std::string("GM_TICKET_SERVICE_SOON"));
    t.escalation = 2;
    CHECK(game::HelpDesk::waitText(t, 0).key == std::string("GM_TICKET_ESCALATED"));
    t.openedByGm = 0;
    t.oldestTicket = -1.0f;
    CHECK(game::HelpDesk::waitText(t, 0).key == std::string("GM_TICKET_UNAVAILABLE"));
    t.oldestTicket = 1.0f;
    t.updateAge = 0.05f; // data older than an hour
    CHECK(game::HelpDesk::waitText(t, 0).key == std::string("GM_TICKET_UNAVAILABLE"));
    t.updateAge = 0.0f;
    CHECK(game::HelpDesk::waitText(t, 0).key == std::string("GM_TICKET_HIGH_VOLUME")); // a day
    t.oldestTicket = 0.002f;                                                           // ~173 s
    CHECK(game::HelpDesk::waitText(t, 0).key == std::string("GM_TICKET_SERVICE_SOON"));
    t.open = false;
    CHECK(game::HelpDesk::waitText(t, 0).key == nullptr);

    // SMSG_GM_TICKET_STATUS_UPDATE (WoW.exe 0x5E78F0): 1 asks again, 2 closes, 3 offers the survey.
    const usize asked = h.sentWith(Opcode::CMSG_GMTICKET_GETTICKET).size();
    REQUIRE(h.packet(Opcode::SMSG_GM_TICKET_STATUS_UPDATE, u32Body(1)) == game::Gameplay::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_GMTICKET_GETTICKET).size() == asked + 1);
    REQUIRE(h.packet(Opcode::SMSG_GM_TICKET_STATUS_UPDATE, u32Body(3)) == game::Gameplay::Handled::Yes);
    CHECK(help.surveyOffered());
    const net::GmSurveyAnswer answers[] = {{28, 5, ""}, {29, 3, ""}};
    REQUIRE(help.submitSurvey(6, answers, "Bien"));
    Bytes expected;
    net::writeGmSurveySubmit(expected, 6, answers, "Bien");
    CHECK(h.sentWith(Opcode::CMSG_GMSURVEY_SUBMIT).back() == expected);
    CHECK_FALSE(help.surveyOffered());

    REQUIRE(help.report(false, "Arthel", "Category: Items\nUn error"));
    expected.clear();
    net::writeBug(expected, false, "Arthel", "Category: Items\nUn error");
    CHECK(h.sentWith(Opcode::CMSG_BUG).back() == expected);
    CHECK_FALSE(help.report(true, "Arthel", ""));

    // Stuck(): the known spell with SPELL_EFFECT_STUCK.
    struct Fake {
        static bool lookup(const void*, u32 id, game::SpellInfo& out) {
            out = game::SpellInfo{};
            out.effects[0] = id == 7355 ? 84u : 6u;
            return id == 7355 || id == 133;
        }
    };
    game::SpellSource source{&Fake::lookup, nullptr};
    const u32 known[] = {133, 7355, 999};
    CHECK(game::HelpDesk::stuckSpell(known, source) == 7355);
    const u32 none[] = {133};
    CHECK(game::HelpDesk::stuckSpell(none, source) == 0);
}

TEST_CASE("Help data: the ticket categories and the GM survey (client data)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or db.vdpk missing: skipping");
        return;
    }
    game::HelpData data;
    REQUIRE(data.init(db));
    REQUIRE(data.categories().size() == 10);
    CHECK(data.categories()[0].id == 1);
    CHECK(data.categories()[0].name == "Stuck");
    CHECK(data.categoryName(2) == "Behavior/Harassment");
    CHECK(data.categoryName(10) == "Character");
    CHECK(data.survey() == 6);
    REQUIRE(data.questions().size() == 5); // 28..32
    CHECK(data.questions()[0].id == 28);
    CHECK(data.questions()[4].id == 32);
    CHECK(data.questions()[0].text.starts_with("Did the GM provide"));
}
