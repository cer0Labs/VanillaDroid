// Phase 16 foundation: the raid as GetRaidRosterInfo describes it (game::Social::raidMembers, the player included),
// the classes of members never seen from their name queries (game::QueryCache::playerInfo), and moving members
// between subgroups (CMSG_GROUP_CHANGE_SUB_GROUP / _SWAP_SUB_GROUP, as GroupHandler.cpp reads them).
#include "game/Entities.h"
#include "game/Queries.h"
#include "game/Social.h"
#include "net/ByteBuffer.h"
#include "net/Messages.h"
#include "net/SocialMessages.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <ostream>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;

using Bytes = std::vector<u8>;

constexpr u64 kMe = 0x11;
constexpr u64 kAeryn = 0x21;
constexpr u64 kBrakk = 0x22;
constexpr u64 kTolvir = 0x23;

struct Sent {
    Opcode opcode;
    Bytes body;
};

struct Harness {
    game::EntityStore store;
    game::QueryCache queries;
    game::Social social{store, queries};
    std::vector<Sent> sent;
    u32 now = 1000;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        social.setSink(sink);
        queries.setSink(sink);
        queries.setOnline(true);
        social.enterWorld(kMe, true, now);
        net::NameQueryInfo me;
        me.guid = kMe;
        me.name = "Arthel";
        me.race = 1;
        me.cls = 1;
        queries.onName(std::move(me));
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

// SMSG_GROUP_LIST of a raid: the player's subgroup (| 0x80 assistant), then each member's name, GUID, status and
// subgroup (| 0x80 assistant).
Bytes raidList(u64 leader, u8 ownFlags, std::vector<std::tuple<const char*, u64, u8>> members) {
    Bytes out;
    net::ByteWriter w(out);
    w.u8v(net::kGroupTypeRaid).u8v(ownFlags).u32v(static_cast<u32>(members.size()));
    for (const auto& [name, guid, flags] : members) {
        w.cstring(name).u64v(guid).u8v(net::kMemberOnline).u8v(flags);
    }
    w.u64v(leader).u8v(3).u64v(0).u8v(2).u8v(0);
    return out;
}

Bytes cstrings(std::initializer_list<const char*> texts, std::initializer_list<u8> tail = {}) {
    Bytes out;
    net::ByteWriter w(out);
    for (const char* t : texts) {
        w.cstring(t);
    }
    for (const u8 b : tail) {
        w.u8v(b);
    }
    return out;
}

} // namespace

TEST_CASE("Raid roster: every member with the player, subgroups, ranks, classes from name queries") {
    Harness h;
    std::array<game::PartyMemberView, 40> roster;
    CHECK(h.social.raidMemberCount() == 0);
    CHECK(h.social.raidMembers(roster) == 0);

    // A raid led by Aeryn: the player in subgroup 2 (index 1) as an assistant, Brakk in 1, Tolvir in 3.
    REQUIRE(h.social.handle(Opcode::SMSG_GROUP_LIST,
                            raidList(kAeryn, 1 | 0x80, {{"Aeryn", kAeryn, 0}, {"Brakk", kBrakk, 0}, {"Tolvir", kTolvir, 2}}),
                            h.now) == game::Social::Handled::Yes);
    CHECK(h.social.raidMemberCount() == 4);
    CHECK_FALSE(h.social.isRaidLeader());
    CHECK(h.social.isRaidOfficer());
    REQUIRE(h.social.raidMembers(roster) == 4);
    CHECK(roster[0].name == "Aeryn");
    CHECK(roster[0].rank() == 2);
    CHECK(roster[2].name == "Tolvir");
    CHECK(roster[2].subgroup == 2);
    CHECK(roster[3].self);
    CHECK(roster[3].name == "Arthel");
    CHECK(roster[3].subgroup == 1);
    CHECK(roster[3].rank() == 1);
    CHECK(h.social.subgroupOf(kMe) == 1);
    CHECK(h.social.subgroupOf(kTolvir) == 2);
    CHECK(h.social.subgroupOf(0x99) == -1);

    // Nobody is in sight: their classes are asked for once with name queries (the list gives names only).
    CHECK(roster[1].cls == 0);
    h.social.update(h.now);
    h.social.update(h.now + 16);
    CHECK(h.sentWith(Opcode::CMSG_NAME_QUERY).size() == 3);
    net::NameQueryInfo brakk;
    brakk.guid = kBrakk;
    brakk.name = "Brakk";
    brakk.race = 2;
    brakk.cls = 9;
    h.queries.onName(std::move(brakk));
    REQUIRE(h.social.raidMembers(roster) == 4);
    CHECK(roster[1].cls == 9);
    CHECK(roster[1].race == 2);
    REQUIRE(h.queries.findPlayerInfo(kBrakk));
    CHECK(h.queries.findPlayerInfo(kBrakk)->cls == 9);
    // A name without a class (the group list's) leaves the player info unknown.
    CHECK_FALSE(h.queries.findPlayerInfo(kTolvir));
    // Out of a raid, the roster is empty (a party has no raid roster).
    REQUIRE(h.social.handle(Opcode::SMSG_GROUP_LIST, raidList(0, 0, {}), h.now) == game::Social::Handled::Yes);
    CHECK(h.social.raidMembers(roster) == 0);
}

TEST_CASE("Raid roster: moving and swapping members between subgroups") {
    Harness h;
    REQUIRE(h.social.handle(Opcode::SMSG_GROUP_LIST,
                            raidList(kMe, 0, {{"Aeryn", kAeryn, 0}, {"Brakk", kBrakk, 1}, {"Tolvir", kTolvir, 1 | 0x80}}),
                            h.now) == game::Social::Handled::Yes);
    REQUIRE(h.social.isRaidLeader());

    // SetRaidSubgroup: the name and the subgroup 0..7.
    CHECK(h.social.setRaidSubgroup(kAeryn, 4));
    CHECK(h.sentWith(Opcode::CMSG_GROUP_CHANGE_SUB_GROUP).back() == cstrings({"Aeryn"}, {4}));
    CHECK(h.social.setRaidSubgroup(kMe, 7)); // the player too
    CHECK(h.sentWith(Opcode::CMSG_GROUP_CHANGE_SUB_GROUP).back() == cstrings({"Arthel"}, {7}));
    CHECK_FALSE(h.social.setRaidSubgroup(kAeryn, 0)); // already there
    CHECK_FALSE(h.social.setRaidSubgroup(kAeryn, 8)); // no such subgroup
    CHECK_FALSE(h.social.setRaidSubgroup(0x99, 2));   // not a member

    // SwapRaidSubgroup: two names, in different subgroups.
    CHECK(h.social.swapRaidSubgroups(kAeryn, kBrakk));
    CHECK(h.sentWith(Opcode::CMSG_GROUP_SWAP_SUB_GROUP).back() == cstrings({"Aeryn", "Brakk"}));
    CHECK_FALSE(h.social.swapRaidSubgroups(kBrakk, kTolvir)); // same subgroup
    CHECK(h.sentWith(Opcode::CMSG_GROUP_SWAP_SUB_GROUP).size() == 1);

    // A plain member is not obeyed: nothing goes out.
    REQUIRE(h.social.handle(Opcode::SMSG_GROUP_LIST,
                            raidList(kAeryn, 0, {{"Aeryn", kAeryn, 0}, {"Brakk", kBrakk, 1}, {"Tolvir", kTolvir, 1}}),
                            h.now) == game::Social::Handled::Yes);
    CHECK_FALSE(h.social.isRaidOfficer());
    CHECK_FALSE(h.social.setRaidSubgroup(kBrakk, 3));
    CHECK_FALSE(h.social.swapRaidSubgroups(kAeryn, kBrakk));
    CHECK(h.sentWith(Opcode::CMSG_GROUP_CHANGE_SUB_GROUP).size() == 2);
}
