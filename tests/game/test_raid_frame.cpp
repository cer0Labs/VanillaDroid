// Phase 16 (raidframe): the raid tab's rules (game/RaidRoster: RaidGroupFrame_Update's slots,
// RaidGroupButton_OnDragStop, RAID_CLASS_COLORS, GetRaidRosterInfo's isDead, RaidFrame_Update's Convert To
// Raid) and the raid roster's order as WoW.exe builds it from SMSG_GROUP_LIST (game::Social::raidMembers: the
// list's order, the player last).
#include "game/Entities.h"
#include "game/Queries.h"
#include "game/RaidRoster.h"
#include "game/Social.h"
#include "net/ByteBuffer.h"
#include "net/Messages.h"
#include "net/SocialMessages.h"

#include <doctest/doctest.h>

#include <array>
#include <ostream>
#include <tuple>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;

using Bytes = std::vector<u8>;

constexpr u64 kMe = 0x11;

struct Harness {
    game::EntityStore store;
    game::QueryCache queries;
    game::Social social{store, queries};
    std::vector<std::pair<Opcode, Bytes>> sent;
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
    void list(u8 type, u64 leader, u8 ownFlags,
              const std::vector<std::tuple<const char*, u64, u8, u8>>& members) {
        // SMSG_GROUP_LIST (Group::SendUpdate): type, the player's subgroup | 0x80 assistant, then each other
        // member's name, GUID, status (MEMBER_STATUS_*) and subgroup | 0x80; the leader and the loot rules.
        Bytes out;
        net::ByteWriter w(out);
        w.u8v(type).u8v(ownFlags).u32v(static_cast<u32>(members.size()));
        for (const auto& [name, guid, status, flags] : members) {
            w.cstring(name).u64v(guid).u8v(status).u8v(flags);
        }
        w.u64v(leader).u8v(3).u64v(0).u8v(2).u8v(0);
        REQUIRE(social.handle(Opcode::SMSG_GROUP_LIST, out, now) == game::Social::Handled::Yes);
    }
    std::vector<Bytes> sentWith(Opcode opcode) const {
        std::vector<Bytes> out;
        for (const auto& [op, body] : sent) {
            if (op == opcode) {
                out.push_back(body);
            }
        }
        return out;
    }
};

game::PartyMemberView member(u64 guid, u8 subgroup) {
    game::PartyMemberView m;
    m.guid = guid;
    m.subgroup = subgroup;
    m.online = true;
    return m;
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

constexpr u8 kOnline = net::kMemberOnline;

} // namespace

TEST_CASE("Raid roster order: the group list's order, the player last (WoW.exe 0x5E6A40 / 0x4BA5F0)") {
    Harness h;
    // The server lists the others in its join order, whatever their subgroups (Group::SendUpdate).
    h.list(net::kGroupTypeRaid, kMe, 2,
           {{"Tolvir", 0x23, kOnline, 4}, {"Aeryn", 0x21, kOnline, 0}, {"Brakk", 0x22, kOnline, 2 | 0x80}});
    std::array<game::PartyMemberView, 40> roster;
    REQUIRE(h.social.raidMembers(roster) == 4);
    CHECK(roster[0].name == "Tolvir"); // raid1
    CHECK(roster[1].name == "Aeryn");
    CHECK(roster[2].name == "Brakk");
    CHECK(roster[3].self); // raid4: the player
    CHECK(roster[3].rank() == 2);
    CHECK(roster[2].rank() == 1); // promoted: "(A)" as soon as the list says so
    // A new list in another order (a member left and joined again) is followed as it comes.
    h.list(net::kGroupTypeRaid, kMe, 2,
           {{"Aeryn", 0x21, kOnline, 0}, {"Brakk", 0x22, kOnline, 2}, {"Tolvir", 0x23, kOnline, 4}});
    REQUIRE(h.social.raidMembers(roster) == 4);
    CHECK(roster[0].name == "Aeryn");
    CHECK(roster[2].name == "Tolvir");
    CHECK(roster[1].rank() == 0); // demoted
    CHECK(roster[3].self);
}

TEST_CASE("Raid groups: RaidGroupFrame_Update's slots in roster order, five at most") {
    std::array<game::PartyMemberView, 9> roster{};
    const u8 groups[] = {1, 0, 1, 7, 1, 1, 1, 1, 9};
    for (u32 i = 0; i < roster.size(); ++i) {
        roster[i] = member(0x100 + i, groups[i]);
    }
    game::RaidSlots slots;
    game::layoutRaidGroups(roster, slots);
    CHECK(slots.slotOf[1] == 0); // group 1, first place
    CHECK(slots.slotOf[0] == 5); // group 2's places in roster order
    CHECK(slots.slotOf[2] == 6);
    CHECK(slots.slotOf[4] == 7);
    CHECK(slots.slotOf[5] == 8);
    CHECK(slots.slotOf[6] == 9);
    CHECK(slots.slotOf[7] == -1); // a sixth in group 2: no button ("when the server hiccups")
    CHECK(slots.slotOf[8] == -1); // no such group
    CHECK(slots.slotOf[3] == 35); // group 8
    CHECK(slots.memberIn[0] == 1);
    CHECK(slots.memberIn[1] == -1); // "Empty"
    CHECK(slots.memberIn[9] == 6);
    CHECK(slots.filled[1] == 5);
    CHECK(slots.filled[7] == 1);
    CHECK(slots.filled[3] == 0);
}

TEST_CASE("Raid groups: RaidGroupButton_OnDragStop's drops") {
    std::array<game::PartyMemberView, 3> roster = {member(0x21, 0), member(0x22, 0), member(0x23, 3)};
    game::RaidSlots slots;
    game::layoutRaidGroups(roster, slots);
    // In its own group (any of its slots): back to its slot.
    CHECK(game::raidDrop(slots, roster, 0, 1).kind == game::RaidDropKind::None);
    CHECK(game::raidDrop(slots, roster, 0, 4).kind == game::RaidDropKind::None);
    // An empty slot of another group: SetRaidSubgroup to that group (the place does not matter).
    const game::RaidDrop move = game::raidDrop(slots, roster, 0, 2 * 5 + 3);
    CHECK(move.kind == game::RaidDropKind::Move);
    CHECK(move.subgroup == 2);
    // An occupied slot: SwapRaidSubgroup with whoever is there.
    const game::RaidDrop swap = game::raidDrop(slots, roster, 1, 3 * 5);
    CHECK(swap.kind == game::RaidDropKind::Swap);
    CHECK(swap.other == 2);
    // Nowhere, or nonsense.
    CHECK(game::raidDrop(slots, roster, 0, -1).kind == game::RaidDropKind::None);
    CHECK(game::raidDrop(slots, roster, 0, 40).kind == game::RaidDropKind::None);
    CHECK(game::raidDrop(slots, roster, 7, 12).kind == game::RaidDropKind::None);
}

TEST_CASE("Raid groups: a drop sends what the leader's client sends") {
    Harness h;
    h.list(net::kGroupTypeRaid, kMe, 0, {{"Aeryn", 0x21, kOnline, 0}, {"Brakk", 0x22, kOnline, 1}});
    std::array<game::PartyMemberView, 40> roster;
    const u32 count = h.social.raidMembers(roster);
    REQUIRE(count == 3);
    game::RaidSlots slots;
    game::layoutRaidGroups({roster.data(), count}, slots);
    // Aeryn onto Brakk's slot (group 2, first place): CMSG_GROUP_SWAP_SUB_GROUP with both names.
    const game::RaidDrop swap = game::raidDrop(slots, {roster.data(), count}, 0, 5);
    REQUIRE(swap.kind == game::RaidDropKind::Swap);
    CHECK(h.social.swapRaidSubgroups(roster[0].guid, roster[static_cast<u32>(swap.other)].guid));
    CHECK(h.sentWith(Opcode::CMSG_GROUP_SWAP_SUB_GROUP).back() == cstrings({"Aeryn", "Brakk"}));
    // The player onto an empty slot of group 6: CMSG_GROUP_CHANGE_SUB_GROUP with its name and 5.
    const game::RaidDrop move = game::raidDrop(slots, {roster.data(), count}, 2, 5 * 5 + 2);
    REQUIRE(move.kind == game::RaidDropKind::Move);
    CHECK(h.social.setRaidSubgroup(roster[2].guid, move.subgroup));
    CHECK(h.sentWith(Opcode::CMSG_GROUP_CHANGE_SUB_GROUP).back() == cstrings({"Arthel"}, {5}));
}

TEST_CASE("Raid tab: RAID_CLASS_COLORS, GetRaidRosterInfo's isDead, Convert To Raid") {
    CHECK(game::raidClassColor(5) == 0xFFFFFFFFu);             // PRIEST: white
    CHECK(game::raidClassColor(7) == game::raidClassColor(2)); // 1.12: the shaman is pink like the paladin
    CHECK(game::raidClassColor(8) ==
          (105u | (204u << 8) | (240u << 16) | (255u << 24))); // MAGE 0.41 0.8 0.94
    CHECK(game::raidClassColor(0) == 0);                       // not known yet: the font's colour
    CHECK(game::raidClassColor(6) == 0);

    game::PartyMemberView m = member(0x21, 0);
    m.inSight = true;
    m.health = 0;
    CHECK(game::raidRosterDead(m)); // 0 health
    m.health = 1;
    m.ghost = true;
    m.dead = true;
    CHECK_FALSE(game::raidRosterDead(m)); // a ghost in sight has 1 health
    m.inSight = false;
    CHECK(game::raidRosterDead(m)); // out of sight: MEMBER_STATUS_DEAD
    m.online = false;
    CHECK_FALSE(game::raidRosterDead(m)); // offline: never dead

    Harness h;
    CHECK_FALSE(game::canConvertToRaid(h.social.group(), kMe)); // no group
    h.list(0, kMe, 0, {{"Aeryn", 0x21, kOnline, 0}});
    CHECK(game::canConvertToRaid(h.social.group(), kMe)); // a party's leader
    h.list(0, 0x21, 0, {{"Aeryn", 0x21, kOnline, 0}});
    CHECK_FALSE(game::canConvertToRaid(h.social.group(), kMe)); // not the leader
    h.list(net::kGroupTypeRaid, kMe, 0, {{"Aeryn", 0x21, kOnline, 0}});
    CHECK_FALSE(game::canConvertToRaid(h.social.group(), kMe)); // a raid already
}
