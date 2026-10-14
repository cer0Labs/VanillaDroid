// Social packets (phase 8): every parser against payloads written field by field as vmangos-core 4641790 writes
// them (build 5875), including every truncation (parsers must refuse, never read past the end), and every builder
// against the exact bytes the server reads.
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/SocialMessages.h"

#include <doctest/doctest.h>

#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace {

using namespace vd;
using namespace vd::net;

using Bytes = std::vector<u8>;

void checkTruncations(const Bytes& full, const std::function<bool(std::span<const u8>)>& parse) {
    for (usize length = 0; length < full.size(); ++length) {
        INFO("truncated to " << length << " of " << full.size() << " bytes");
        CHECK_FALSE(parse(std::span<const u8>(full.data(), length)));
    }
}

Bytes bytes(std::initializer_list<int> list) {
    Bytes out;
    for (int b : list) {
        out.push_back(static_cast<u8>(b));
    }
    return out;
}

constexpr u64 kAeryn = 0x0000000000000021ull;
constexpr u64 kBrakk = 0x0000000000000022ull;
constexpr u64 kFlag = 0xF11000000000ABCDull;

} // namespace

TEST_CASE("Characters: CMSG_CHAR_CREATE body, SMSG_CHAR_CREATE / CHAR_DELETE results") {
    // CharCreate::ReadFromWorldPacket: name, race, class, gender, skin, face, hair style, hair color, facial hair,
    // outfit.
    Bytes out;
    CharCreateRequest c;
    c.name = "Arthel";
    c.race = 1;
    c.cls = 1;
    c.gender = 0;
    c.skin = 3;
    c.face = 4;
    c.hairStyle = 5;
    c.hairColor = 6;
    c.facialHair = 7;
    writeCharCreate(out, c);
    CHECK(out == bytes({'A', 'r', 't', 'h', 'e', 'l', 0, 1, 1, 0, 3, 4, 5, 6, 7, 0}));
    u8 result = 0;
    REQUIRE(parseU8(bytes({46}), result)); // CHAR_CREATE_SUCCESS
    CHECK(result == 46);
    CHECK_FALSE(parseU8({}, result));
}

TEST_CASE("Groups: SMSG_GROUP_LIST in and out of a group") {
    // Group::SendUpdate: type, own subgroup | assistant, count, members (name, GUID, status, subgroup | assistant),
    // leader, then loot method, looter, threshold and difficulty.
    Bytes list;
    ByteWriter(list)
        .u8v(kGroupTypeRaid)
        .u8v(0x81) // own subgroup 1, assistant
        .u32v(2)
        .cstring("Aeryn")
        .u64v(kAeryn)
        .u8v(kMemberOnline | kMemberDead)
        .u8v(0x00)
        .cstring("Brakk")
        .u64v(kBrakk)
        .u8v(0)
        .u8v(0x82)
        .u64v(kAeryn)
        .u8v(static_cast<u8>(LootMethod::MasterLoot))
        .u64v(kBrakk)
        .u8v(2)
        .u8v(0);
    GroupList g;
    REQUIRE(parseGroupList(list, g));
    CHECK(g.groupType == kGroupTypeRaid);
    CHECK(g.ownSubgroup == 1);
    CHECK(g.ownAssistant);
    REQUIRE(g.members.size() == 2);
    CHECK(g.members[0].name == "Aeryn");
    CHECK(g.members[0].guid == kAeryn);
    CHECK(g.members[0].status == (kMemberOnline | kMemberDead));
    CHECK(g.members[0].subgroup == 0);
    CHECK_FALSE(g.members[0].assistant);
    CHECK(g.members[1].name == "Brakk");
    CHECK(g.members[1].subgroup == 2);
    CHECK(g.members[1].assistant);
    CHECK(g.leader == kAeryn);
    CHECK(g.lootMethod == static_cast<u8>(LootMethod::MasterLoot));
    CHECK(g.looter == kBrakk);
    CHECK(g.lootThreshold == 2);
    checkTruncations(list, [](auto p) {
        GroupList m;
        return parseGroupList(p, m);
    });

    // The default packet ("not in a group"): no members, no loot fields.
    Bytes empty;
    ByteWriter(empty).u8v(0).u8v(0).u32v(0).u64v(0);
    REQUIRE(parseGroupList(empty, g));
    CHECK(g.members.empty());
    CHECK(g.leader == 0);
    checkTruncations(empty, [](auto p) {
        GroupList m;
        return parseGroupList(p, m);
    });

    // A count the packet cannot hold.
    Bytes huge;
    ByteWriter(huge).u8v(0).u8v(0).u32v(0x7FFFFFFF).u64v(0);
    CHECK_FALSE(parseGroupList(huge, g));
}

TEST_CASE("Groups: SMSG_PARTY_MEMBER_STATS, partial and full") {
    // BuildPartyMemberStatsPacket: packed GUID, mask, then the fields of the mask in flag order.
    Bytes partial;
    ByteWriter(partial)
        .packedGuid(kAeryn)
        .u32v(kGroupUpdateStatus | kGroupUpdateCurHp | kGroupUpdateZone | kGroupUpdatePosition)
        .u8v(kMemberOnline)
        .u16v(250)
        .u16v(12)
        .u16v(static_cast<u16>(-8913))
        .u16v(554);
    PartyMemberStats s;
    REQUIRE(parsePartyMemberStats(partial, s));
    CHECK(s.guid == kAeryn);
    CHECK(s.status == kMemberOnline);
    CHECK(s.health == 250);
    CHECK(s.zone == 12);
    CHECK(s.x == -8913);
    CHECK(s.y == 554);
    CHECK(s.maxHealth == 0); // not in the mask
    checkTruncations(partial, [](auto p) {
        PartyMemberStats m;
        return parsePartyMemberStats(p, m);
    });

    // CMSG_REQUEST_PARTY_MEMBER_STATS answer: everything (GROUP_UPDATE_FULL), auras by mask, a pet.
    Bytes full;
    ByteWriter w(full);
    w.packedGuid(kBrakk).u32v(kGroupUpdateFull);
    w.u8v(kMemberOnline | kMemberAfk); // status
    w.u16v(100).u16v(120);              // health
    w.u8v(1);                           // rage
    w.u16v(300).u16v(1000);             // power
    w.u16v(14).u16v(1519);              // level, zone
    w.u16v(static_cast<u16>(-8800)).u16v(640);
    w.u32v(0x5);                        // positive auras in slots 0 and 2
    w.u16v(6673).u16v(1460);
    w.u16v(0x1);                        // negative aura in slot 32
    w.u16v(11);
    w.u64v(0xF140000000000055ull);      // pet
    w.cstring("Wolfie");
    w.u16v(903).u16v(80).u16v(90).u8v(2).u16v(50).u16v(100);
    w.u32v(0x1).u16v(24604); // pet auras
    w.u16v(0);
    REQUIRE(parsePartyMemberStats(full, s));
    CHECK(s.guid == kBrakk);
    CHECK(s.status == (kMemberOnline | kMemberAfk));
    CHECK(s.health == 100);
    CHECK(s.maxHealth == 120);
    CHECK(s.powerType == 1);
    CHECK(s.power == 300);
    CHECK(s.maxPower == 1000);
    CHECK(s.level == 14);
    CHECK(s.zone == 1519);
    CHECK(s.x == -8800);
    CHECK(s.y == 640);
    CHECK(s.auras[0] == 6673);
    CHECK(s.auras[2] == 1460);
    CHECK(s.negativeAuras[0] == 11);
    CHECK(s.petGuid == 0xF140000000000055ull);
    CHECK(s.petName == "Wolfie");
    CHECK(s.petDisplay == 903);
    CHECK(s.petMaxPower == 100);
    checkTruncations(full, [](auto p) {
        PartyMemberStats m;
        return parsePartyMemberStats(p, m);
    });

    // A player not in the raid: FULL with only the status, offline.
    Bytes offline;
    ByteWriter(offline).packedGuid(kAeryn).u32v(kGroupUpdateStatus).u8v(0);
    REQUIRE(parsePartyMemberStats(offline, s));
    CHECK(s.status == 0);
}

TEST_CASE("Groups: SMSG_PARTY_COMMAND_RESULT and the client bodies") {
    Bytes result;
    ByteWriter(result).u32v(kPartyOpInvite).cstring("Nadie").u32v(static_cast<u32>(PartyResult::BadPlayerName));
    PartyCommandResult r;
    REQUIRE(parsePartyCommandResult(result, r));
    CHECK(r.operation == kPartyOpInvite);
    CHECK(r.name == "Nadie");
    CHECK(r.result == static_cast<u32>(PartyResult::BadPlayerName));
    checkTruncations(result, [](auto p) {
        PartyCommandResult m;
        return parsePartyCommandResult(p, m);
    });

    Bytes out;
    writeCString(out, "Aeryn"); // CMSG_GROUP_INVITE
    CHECK(out == bytes({'A', 'e', 'r', 'y', 'n', 0}));
    out.clear();
    writeLootMethod(out, 2, kBrakk, 3); // CMSG_LOOT_METHOD: u32 method, GUID, u32 threshold
    CHECK(out == bytes({2, 0, 0, 0, 0x22, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0}));
}

TEST_CASE("Channels: every SMSG_CHANNEL_NOTIFY layout, SMSG_CHANNEL_LIST, join / emote bodies") {
    struct Case {
        const char* what;
        Bytes payload;
    };
    std::vector<Case> cases;
    {
        Bytes b;
        ByteWriter(b).u8v(0x02).cstring("General - Elwynn Forest").u32v(kChannelFlagGeneral | kChannelFlagNotLfg).u32v(0);
        cases.push_back({"you joined", b});
    }
    {
        Bytes b;
        ByteWriter(b).u8v(0x00).cstring("Prueba").u64v(kAeryn);
        cases.push_back({"joined", b});
    }
    {
        Bytes b;
        ByteWriter(b).u8v(0x0C).cstring("Prueba").u64v(kAeryn).u8v(0).u8v(kChannelMemberModerator);
        cases.push_back({"mode change", b});
    }
    {
        Bytes b;
        ByteWriter(b).u8v(0x12).cstring("Prueba").u64v(kAeryn).u64v(kBrakk);
        cases.push_back({"kicked", b});
    }
    {
        Bytes b;
        ByteWriter(b).u8v(0x09).cstring("Prueba").cstring("Nadie");
        cases.push_back({"player not found", b});
    }
    {
        Bytes b;
        ByteWriter(b).u8v(0x03).cstring("Trade - City");
        cases.push_back({"you left", b});
    }
    for (const Case& c : cases) {
        INFO(c.what);
        ChannelNotify n;
        REQUIRE(parseChannelNotify(c.payload, n));
        checkTruncations(c.payload, [](auto p) {
            ChannelNotify m;
            return parseChannelNotify(p, m);
        });
    }
    ChannelNotify n;
    REQUIRE(parseChannelNotify(cases[0].payload, n));
    CHECK(n.notice == ChannelNotice::YouJoined);
    CHECK(n.channel == "General - Elwynn Forest");
    CHECK(n.flags == (kChannelFlagGeneral | kChannelFlagNotLfg));
    REQUIRE(parseChannelNotify(cases[2].payload, n));
    CHECK(n.notice == ChannelNotice::ModeChange);
    CHECK(n.guid == kAeryn);
    CHECK(n.newFlags == kChannelMemberModerator);
    REQUIRE(parseChannelNotify(cases[3].payload, n));
    CHECK(n.guid == kAeryn);
    CHECK(n.source == kBrakk);
    REQUIRE(parseChannelNotify(cases[4].payload, n));
    CHECK(n.name == "Nadie");
    // An unknown notice has no known layout.
    Bytes unknown;
    ByteWriter(unknown).u8v(0x40).cstring("Prueba");
    CHECK_FALSE(parseChannelNotify(unknown, n));

    // Channel::List: name, flags, count, (GUID, member flags)...
    Bytes list;
    ByteWriter(list).cstring("Prueba").u8v(kChannelFlagCustom).u32v(2).u64v(kAeryn).u8v(kChannelMemberOwner).u64v(kBrakk).u8v(0);
    ChannelList cl;
    REQUIRE(parseChannelList(list, cl));
    CHECK(cl.channel == "Prueba");
    REQUIRE(cl.members.size() == 2);
    CHECK(cl.members[0].guid == kAeryn);
    CHECK(cl.members[0].flags == kChannelMemberOwner);
    checkTruncations(list, [](auto p) {
        ChannelList m;
        return parseChannelList(p, m);
    });

    Bytes out;
    writeJoinChannel(out, "Prueba", ""); // JoinChannel: name, password
    CHECK(out == bytes({'P', 'r', 'u', 'e', 'b', 'a', 0, 0}));
    out.clear();
    writeTextEmote(out, 101, 0, kAeryn); // TextEmote: text emote, emote number, GUID
    CHECK(out == bytes({101, 0, 0, 0, 0, 0, 0, 0, 0x21, 0, 0, 0, 0, 0, 0, 0}));
}

TEST_CASE("Friends: SMSG_FRIEND_LIST, SMSG_IGNORE_LIST, SMSG_FRIEND_STATUS") {
    // FriendList: count, then GUID + status (+ area, level, class only when online).
    Bytes list;
    ByteWriter(list).u8v(2).u64v(kAeryn).u8v(kFriendOnline).u32v(12).u32v(10).u32v(8).u64v(kBrakk).u8v(kFriendOffline);
    std::vector<FriendEntry> friends;
    REQUIRE(parseFriendList(list, friends));
    REQUIRE(friends.size() == 2);
    CHECK(friends[0].guid == kAeryn);
    CHECK(friends[0].status == kFriendOnline);
    CHECK(friends[0].area == 12);
    CHECK(friends[0].level == 10);
    CHECK(friends[0].cls == 8);
    CHECK(friends[1].status == kFriendOffline);
    CHECK(friends[1].level == 0);
    checkTruncations(list, [](auto p) {
        std::vector<FriendEntry> m;
        return parseFriendList(p, m);
    });

    Bytes ignore;
    ByteWriter(ignore).u8v(1).u64v(kBrakk);
    std::vector<u64> ignored;
    REQUIRE(parseIgnoreList(ignore, ignored));
    REQUIRE(ignored.size() == 1);
    CHECK(ignored[0] == kBrakk);
    checkTruncations(ignore, [](auto p) {
        std::vector<u64> m;
        return parseIgnoreList(p, m);
    });

    // FriendStatus: result, GUID; FRIEND_ONLINE / ADDED_ONLINE add status, area, level, class.
    Bytes online;
    ByteWriter(online).u8v(static_cast<u8>(FriendResult::Online)).u64v(kAeryn).u8v(kFriendAfk).u32v(1519).u32v(11).u32v(4);
    FriendStatus st;
    REQUIRE(parseFriendStatus(online, st));
    CHECK(st.guid == kAeryn);
    CHECK(st.hasInfo);
    CHECK(st.status == kFriendAfk);
    CHECK(st.area == 1519);
    CHECK(st.level == 11);
    CHECK(st.cls == 4);
    checkTruncations(online, [](auto p) {
        FriendStatus m;
        return parseFriendStatus(p, m);
    });
    Bytes offline;
    ByteWriter(offline).u8v(static_cast<u8>(FriendResult::Offline)).u64v(kAeryn);
    REQUIRE(parseFriendStatus(offline, st));
    CHECK_FALSE(st.hasInfo);
    checkTruncations(offline, [](auto p) {
        FriendStatus m;
        return parseFriendStatus(p, m);
    });
}

TEST_CASE("Who: CMSG_WHO body and SMSG_WHO") {
    // Who::ReadFromWorldPacket: levels, name, guild, race mask, class mask, zones, search strings.
    const u32 zones[] = {12};
    const std::string_view terms[] = {"elw"};
    WhoRequest request;
    request.minLevel = 5;
    request.maxLevel = 20;
    request.name = "Ae";
    request.zones = zones;
    request.terms = terms;
    Bytes out;
    writeWho(out, request);
    Bytes expected;
    ByteWriter(expected)
        .u32v(5)
        .u32v(20)
        .cstring("Ae")
        .cstring("")
        .u32v(0xFFFFFFFF)
        .u32v(0xFFFFFFFF)
        .u32v(1)
        .u32v(12)
        .u32v(1)
        .cstring("elw");
    CHECK(out == expected);

    // WhoListClientQueryTask: listed, online, then name, guild, level, class, race, zone.
    Bytes who;
    ByteWriter(who).u32v(2).u32v(2).cstring("Aeryn").cstring("Los Errantes").u32v(10).u32v(8).u32v(4).u32v(12)
        .cstring("Brakk").cstring("").u32v(12).u32v(1).u32v(2).u32v(14);
    WhoList list;
    REQUIRE(parseWhoList(who, list));
    CHECK(list.online == 2);
    REQUIRE(list.entries.size() == 2);
    CHECK(list.entries[0].name == "Aeryn");
    CHECK(list.entries[0].guild == "Los Errantes");
    CHECK(list.entries[0].level == 10);
    CHECK(list.entries[0].cls == 8);
    CHECK(list.entries[0].race == 4);
    CHECK(list.entries[0].zone == 12);
    CHECK(list.entries[1].guild.empty());
    checkTruncations(who, [](auto p) {
        WhoList m;
        return parseWhoList(p, m);
    });
}

TEST_CASE("Guild: query response, roster, events, invite, command result, info") {
    Bytes query;
    ByteWriter q(query);
    q.u32v(7).cstring("Los Errantes");
    const char* ranks[] = {"Guild Master", "Officer", "Veteran", "Member", "Initiate"};
    for (u32 i = 0; i < kGuildRankCount; ++i) {
        q.cstring(i < 5 ? ranks[i] : "");
    }
    q.u32v(1).u32v(2).u32v(3).u32v(4).u32v(5);
    GuildQueryResponse g;
    REQUIRE(parseGuildQueryResponse(query, g));
    CHECK(g.guildId == 7);
    CHECK(g.name == "Los Errantes");
    CHECK(g.ranks[0] == "Guild Master");
    CHECK(g.ranks[4] == "Initiate");
    CHECK(g.ranks[5].empty());
    CHECK(g.backgroundColor == 5);
    checkTruncations(query, [](auto p) {
        GuildQueryResponse m;
        return parseGuildQueryResponse(p, m);
    });

    // GuildRoster::AppendBodyTo: count, MOTD, info, rank count, rights, then members (offline ones carry the days
    // since their last login).
    Bytes roster;
    ByteWriter(roster)
        .u32v(2)
        .cstring("Raid el viernes")
        .cstring("Hermandad de pruebas")
        .u32v(2)
        .u32v(0x000FF1FF)
        .u32v(0x43)
        .u64v(kAeryn)
        .u8v(kGuildMemberOnline)
        .cstring("Aeryn")
        .u32v(0)
        .u8v(10)
        .u8v(8)
        .u32v(12)
        .cstring("jefa")
        .cstring("")
        .u64v(kBrakk)
        .u8v(0)
        .cstring("Brakk")
        .u32v(1)
        .u8v(12)
        .u8v(1)
        .u32v(14)
        .f32v(2.5f)
        .cstring("")
        .cstring("oficial");
    GuildRoster r;
    REQUIRE(parseGuildRoster(roster, r));
    CHECK(r.motd == "Raid el viernes");
    CHECK(r.info == "Hermandad de pruebas");
    REQUIRE(r.rankRights.size() == 2);
    CHECK(r.rankRights[1] == 0x43);
    REQUIRE(r.members.size() == 2);
    CHECK(r.members[0].name == "Aeryn");
    CHECK(r.members[0].presence == kGuildMemberOnline);
    CHECK(r.members[0].note == "jefa");
    CHECK(r.members[1].offlineDays == 2.5f);
    CHECK(r.members[1].officerNote == "oficial");
    checkTruncations(roster, [](auto p) {
        GuildRoster m;
        return parseGuildRoster(p, m);
    });

    // GuildEvent: event, count, strings, then the GUID of the player concerned (signed on / off, joined, left).
    Bytes signedOn;
    ByteWriter(signedOn).u8v(static_cast<u8>(GuildEventKind::SignedOn)).u8v(1).cstring("Aeryn").u64v(kAeryn);
    GuildEvent e;
    REQUIRE(parseGuildEvent(signedOn, e));
    CHECK(e.event == static_cast<u8>(GuildEventKind::SignedOn));
    CHECK(e.count == 1);
    CHECK(e.params[0] == "Aeryn");
    CHECK(e.guid == kAeryn);
    Bytes promotion;
    ByteWriter(promotion).u8v(0).u8v(3).cstring("Aeryn").cstring("Brakk").cstring("Officer");
    REQUIRE(parseGuildEvent(promotion, e));
    CHECK(e.params[2] == "Officer");
    CHECK(e.guid == 0);
    checkTruncations(promotion, [](auto p) {
        GuildEvent m;
        return parseGuildEvent(p, m);
    });
    // Cut inside the GUID: refused (a GUID-less event ends right after its strings).
    for (usize length = signedOn.size() - 7; length < signedOn.size(); ++length) {
        CHECK_FALSE(parseGuildEvent(std::span<const u8>(signedOn.data(), length), e));
    }

    Bytes invite;
    ByteWriter(invite).cstring("Aeryn").cstring("Los Errantes");
    GuildInvite gi;
    REQUIRE(parseGuildInvite(invite, gi));
    CHECK(gi.inviter == "Aeryn");
    CHECK(gi.guild == "Los Errantes");
    checkTruncations(invite, [](auto p) {
        GuildInvite m;
        return parseGuildInvite(p, m);
    });

    Bytes command;
    ByteWriter(command).u32v(kGuildCommandInvite).cstring("Brakk").u32v(static_cast<u32>(GuildError::AlreadyInGuildS));
    GuildCommandResult gc;
    REQUIRE(parseGuildCommandResult(command, gc));
    CHECK(gc.command == kGuildCommandInvite);
    CHECK(gc.name == "Brakk");
    CHECK(gc.result == static_cast<u32>(GuildError::AlreadyInGuildS));
    checkTruncations(command, [](auto p) {
        GuildCommandResult m;
        return parseGuildCommandResult(p, m);
    });

    Bytes info;
    ByteWriter(info).cstring("Los Errantes").u32v(1).u32v(9).u32v(2026).u32v(12).u32v(3);
    GuildInfo gf;
    REQUIRE(parseGuildInfo(info, gf));
    CHECK(gf.year == 2026);
    CHECK(gf.members == 12);
    checkTruncations(info, [](auto p) {
        GuildInfo m;
        return parseGuildInfo(p, m);
    });
}

TEST_CASE("Duels: SMSG_DUEL_REQUESTED, WINNER, COMPLETE, COUNTDOWN") {
    Bytes requested;
    ByteWriter(requested).u64v(kFlag).u64v(kAeryn);
    DuelRequested d;
    REQUIRE(parseDuelRequested(requested, d));
    CHECK(d.arbiter == kFlag);
    CHECK(d.initiator == kAeryn);
    checkTruncations(requested, [](auto p) {
        DuelRequested m;
        return parseDuelRequested(p, m);
    });

    Bytes winner;
    ByteWriter(winner).u8v(1).cstring("Aeryn").cstring("Brakk");
    DuelWinner w;
    REQUIRE(parseDuelWinner(winner, w));
    CHECK(w.fled);
    CHECK(w.winner == "Aeryn");
    CHECK(w.loser == "Brakk");
    checkTruncations(winner, [](auto p) {
        DuelWinner m;
        return parseDuelWinner(p, m);
    });

    u8 fought = 0;
    REQUIRE(parseU8(bytes({1}), fought));
    CHECK(fought == 1);
    u32 ms = 0;
    Bytes countdown;
    ByteWriter(countdown).u32v(3000);
    REQUIRE(parseU32(countdown, ms));
    CHECK(ms == 3000);
}

TEST_CASE("Rolls: MSG_RANDOM_ROLL both ways") {
    Bytes out;
    writeRandomRoll(out, 1, 100); // RandomRoll: minimum, maximum
    CHECK(out == bytes({1, 0, 0, 0, 100, 0, 0, 0}));
    // HandleRandomRollOpcode: minimum, maximum, roll, the roller's GUID.
    Bytes roll;
    ByteWriter(roll).u32v(1).u32v(100).u32v(57).u64v(kAeryn);
    RandomRoll r;
    REQUIRE(parseRandomRoll(roll, r));
    CHECK(r.roll == 57);
    CHECK(r.guid == kAeryn);
    checkTruncations(roll, [](auto p) {
        RandomRoll m;
        return parseRandomRoll(p, m);
    });
}
