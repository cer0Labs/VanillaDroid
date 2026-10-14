// Social state (phase 8): groups, channels, friends, who, guild and duels driven end to end through game::Social
// (and through game::Gameplay, which hands it its packets) with a capturing packet sink. Server packets are
// written as vmangos-core writes them; client packets are checked byte for byte. Then the texts the social side
// shows for them (game/SocialText) against the baked table (VD_PACKS_DIR; skipped without it).
#include "TestData.h"
#include "core/Text.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Queries.h"
#include "game/Social.h"
#include "game/SocialText.h"
#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/ResponseCodes.gen.h"
#include "net/SocialMessages.h"
#include "net/UpdateFields.gen.h"

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
using game::SocialEventKind;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kMe = 0x11;
constexpr u64 kAeryn = 0x21;
constexpr u64 kBrakk = 0x22;
constexpr u64 kTolvir = 0x23;
constexpr u64 kFlag = 0xF11000000000ABCDull;

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
                {uf::UNIT_FIELD_BYTES_0, 1 | (8u << 8) | (1u << 16)}, // human mage, female, mana
                {uf::UNIT_FIELD_LEVEL, 10}};
    f.insert(f.end(), extra.begin(), extra.end());
    return f;
}

Bytes cstring(const char* text) {
    Bytes out;
    net::ByteWriter(out).cstring(text);
    return out;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

// Social over its own entity store and query cache; what it sends is captured.
struct Harness {
    game::EntityStore store;
    game::QueryCache queries;
    game::Social social{store, queries};
    std::vector<Sent> sent;
    u32 now = 1000;
    u32 cursor = 0;

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
        queries.onName(std::move(me));
    }
    void apply(const Bytes& update) { REQUIRE(store.applyUpdateObject(update, now)); }
    game::Social::Handled packet(Opcode opcode, const Bytes& body) { return social.handle(opcode, body, now); }
    std::vector<Bytes> sentWith(Opcode opcode) const {
        std::vector<Bytes> out;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out.push_back(s.body);
            }
        }
        return out;
    }
    std::vector<game::SocialEvent> events() {
        std::vector<game::SocialEvent> out;
        while (const game::SocialEvent* e = social.events().next(cursor)) {
            out.push_back(*e);
        }
        return out;
    }
};

const game::SocialEvent* find(const std::vector<game::SocialEvent>& events, SocialEventKind kind) {
    for (const game::SocialEvent& e : events) {
        if (e.kind == kind) {
            return &e;
        }
    }
    return nullptr;
}

Bytes groupList(u64 leader, std::vector<std::pair<const char*, u64>> members, u8 lootMethod = 3, bool raid = false) {
    Bytes out;
    net::ByteWriter w(out);
    w.u8v(raid ? 1 : 0).u8v(0).u32v(static_cast<u32>(members.size()));
    for (const auto& [name, guid] : members) {
        w.cstring(name).u64v(guid).u8v(net::kMemberOnline).u8v(0);
    }
    w.u64v(leader);
    if (!members.empty()) {
        w.u8v(lootMethod).u64v(0).u8v(2).u8v(0);
    }
    return out;
}

Bytes guidBody(u64 guid) {
    Bytes out;
    net::writeGuid(out, guid);
    return out;
}

} // namespace

TEST_CASE("Social: invitation, group list changes, stats out of sight, leaving") {
    Harness h;
    h.apply(createdPlayer(kMe, playerFields()));
    h.social.update(h.now);
    h.events();

    // Inviting by name: the body is the name.
    REQUIRE(h.social.inviteToGroup("Aeryn"));
    CHECK(h.sentWith(Opcode::CMSG_GROUP_INVITE).back() == cstring("Aeryn"));
    Bytes result;
    net::ByteWriter(result).u32v(net::kPartyOpInvite).cstring("Aeryn").u32v(0);
    CHECK(h.packet(Opcode::SMSG_PARTY_COMMAND_RESULT, result) == game::Social::Handled::Yes);
    const auto invited = h.events();
    REQUIRE(find(invited, SocialEventKind::PartyResult));
    CHECK(find(invited, SocialEventKind::PartyResult)->name == "Aeryn");

    // Aeryn joins: the first list puts the player in a group.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kMe, {{"Aeryn", kAeryn}})) == game::Social::Handled::Yes);
    auto ev = h.events();
    CHECK(find(ev, SocialEventKind::GroupJoined));
    CHECK(h.social.group().active);
    CHECK(h.social.isLeader());
    CHECK(h.social.inGroupWith(kAeryn));
    REQUIRE(h.queries.findPlayerName(kAeryn));
    CHECK(*h.queries.findPlayerName(kAeryn) == "Aeryn"); // names come with the list

    // Aeryn is out of sight: her stats are asked for once, then they come as SMSG_PARTY_MEMBER_STATS.
    h.social.update(h.now);
    h.social.update(h.now + 100);
    REQUIRE(h.sentWith(Opcode::CMSG_REQUEST_PARTY_MEMBER_STATS).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_REQUEST_PARTY_MEMBER_STATS)[0] == guidBody(kAeryn));
    Bytes stats;
    net::ByteWriter(stats)
        .packedGuid(kAeryn)
        .u32v(net::kGroupUpdateStatus | net::kGroupUpdateCurHp | net::kGroupUpdateMaxHp | net::kGroupUpdateLevel)
        .u8v(net::kMemberOnline | net::kMemberDead)
        .u16v(0)
        .u16v(200)
        .u16v(9);
    REQUIRE(h.packet(Opcode::SMSG_PARTY_MEMBER_STATS, stats) == game::Social::Handled::Yes);
    std::array<game::PartyMemberView, 4> views;
    REQUIRE(h.social.partyMembers(views) == 1);
    CHECK(views[0].name == "Aeryn");
    CHECK(views[0].level == 9);
    CHECK(views[0].maxHealth == 200);
    CHECK(views[0].dead);
    CHECK_FALSE(views[0].inSight);
    CHECK_FALSE(views[0].leader);

    // In sight, the update fields win.
    h.apply(createdPlayer(kAeryn, playerFields({{uf::PLAYER_FLAGS, game::kPlayerFlagAfk}})));
    REQUIRE(h.social.partyMembers(views) == 1);
    CHECK(views[0].inSight);
    CHECK(views[0].health == 100);
    CHECK(views[0].level == 10);
    CHECK(views[0].cls == 8);
    CHECK_FALSE(views[0].dead);
    CHECK(views[0].afk);

    // Brakk joins, the loot method changes, Aeryn leaves.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kMe, {{"Aeryn", kAeryn}, {"Brakk", kBrakk}})) ==
            game::Social::Handled::Yes);
    ev = h.events();
    REQUIRE(find(ev, SocialEventKind::MemberJoined));
    CHECK(find(ev, SocialEventKind::MemberJoined)->name == "Brakk");
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kMe, {{"Brakk", kBrakk}}, 0)) == game::Social::Handled::Yes);
    ev = h.events();
    REQUIRE(find(ev, SocialEventKind::MemberLeft));
    CHECK(find(ev, SocialEventKind::MemberLeft)->name == "Aeryn");
    REQUIRE(find(ev, SocialEventKind::LootMethodChanged));
    CHECK(find(ev, SocialEventKind::LootMethodChanged)->value[0] == 0);
    REQUIRE(h.social.partyMembers(views) == 1);
    CHECK(views[0].name == "Brakk");

    // The leader's commands.
    h.social.promoteToLeader(kBrakk);
    CHECK(h.sentWith(Opcode::CMSG_GROUP_SET_LEADER).back() == guidBody(kBrakk));
    h.social.setLootMethod(net::LootMethod::MasterLoot, kBrakk, 2);
    Bytes loot;
    net::writeLootMethod(loot, 2, kBrakk, 2);
    CHECK(h.sentWith(Opcode::CMSG_LOOT_METHOD).back() == loot);
    h.social.setLootMethod(net::LootMethod::FreeForAll, kBrakk, 2); // no master looter outside master loot
    loot.clear();
    net::writeLootMethod(loot, 0, 0, 2);
    CHECK(h.sentWith(Opcode::CMSG_LOOT_METHOD).back() == loot);
    h.social.uninvite(kBrakk);
    CHECK(h.sentWith(Opcode::CMSG_GROUP_UNINVITE_GUID).back() == guidBody(kBrakk));
    h.social.convertToRaid();
    CHECK(h.sentWith(Opcode::CMSG_GROUP_RAID_CONVERT).size() == 1);

    // The new leader, by name.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_SET_LEADER, cstring("Brakk")) == game::Social::Handled::Yes);
    CHECK(h.social.group().leader == kBrakk);
    CHECK_FALSE(h.social.isLeader());
    ev = h.events();
    REQUIRE(find(ev, SocialEventKind::LeaderChanged));
    CHECK(find(ev, SocialEventKind::LeaderChanged)->name == "Brakk");
    h.social.promoteToLeader(kBrakk); // not the leader any more: nothing sent
    CHECK(h.sentWith(Opcode::CMSG_GROUP_SET_LEADER).size() == 1);

    // Leaving: the body is empty; the server answers with the empty list.
    h.social.leaveGroup();
    REQUIRE(h.sentWith(Opcode::CMSG_GROUP_DISBAND).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_GROUP_DISBAND)[0].empty());
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(0, {})) == game::Social::Handled::Yes);
    CHECK_FALSE(h.social.group().active);
    CHECK(find(h.events(), SocialEventKind::GroupLeft));
    CHECK(h.social.partyMembers(views) == 0);
}

TEST_CASE("Social: an invitation to accept or decline, kicked, disbanded, malformed lists") {
    Harness h;
    REQUIRE(h.packet(Opcode::SMSG_GROUP_INVITE, cstring("Aeryn")) == game::Social::Handled::Yes);
    auto ev = h.events();
    REQUIRE(find(ev, SocialEventKind::GroupInvite));
    CHECK(find(ev, SocialEventKind::GroupInvite)->name == "Aeryn");
    CHECK(h.social.group().pendingInviter == "Aeryn");
    h.social.declineGroupInvite();
    REQUIRE(h.sentWith(Opcode::CMSG_GROUP_DECLINE).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_GROUP_DECLINE)[0].empty());
    CHECK(h.social.group().pendingInviter.empty());
    h.social.acceptGroupInvite(); // nothing pending any more
    CHECK(h.sentWith(Opcode::CMSG_GROUP_ACCEPT).empty());

    REQUIRE(h.packet(Opcode::SMSG_GROUP_INVITE, cstring("Brakk")) == game::Social::Handled::Yes);
    h.social.acceptGroupInvite();
    REQUIRE(h.sentWith(Opcode::CMSG_GROUP_ACCEPT).size() == 1);
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kBrakk, {{"Brakk", kBrakk}})) == game::Social::Handled::Yes);
    CHECK(h.social.group().leader == kBrakk);

    // Kicked: SMSG_GROUP_UNINVITE, then the empty list.
    CHECK(h.packet(Opcode::SMSG_GROUP_UNINVITE, {}) == game::Social::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(0, {})) == game::Social::Handled::Yes);
    ev = h.events();
    CHECK(find(ev, SocialEventKind::GroupUninvited));
    CHECK(find(ev, SocialEventKind::GroupLeft));

    // Disbanded.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kBrakk, {{"Brakk", kBrakk}})) == game::Social::Handled::Yes);
    CHECK(h.packet(Opcode::SMSG_GROUP_DESTROYED, {}) == game::Social::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(0, {})) == game::Social::Handled::Yes);
    ev = h.events();
    CHECK(find(ev, SocialEventKind::GroupDestroyed));
    CHECK_FALSE(h.social.group().active);

    // A cut list changes nothing.
    Bytes cut = groupList(kBrakk, {{"Brakk", kBrakk}});
    cut.resize(cut.size() - 3);
    CHECK(h.packet(Opcode::SMSG_GROUP_LIST, cut) == game::Social::Handled::Malformed);
    CHECK_FALSE(h.social.group().active);
    // Somebody else's packet.
    CHECK(h.packet(Opcode::SMSG_ATTACKSTART, {}) == game::Social::Handled::No);
}

TEST_CASE("Social: channels keep their numbers across zones, custom channels, notices") {
    Harness h;
    const game::ZoneChannel elwynn[] = {{1, "General - Elwynn Forest"}, {22, "LocalDefense - Elwynn Forest"}};
    h.social.setZone(12, elwynn);
    CHECK(h.social.zone() == 12);
    auto joins = h.sentWith(Opcode::CMSG_JOIN_CHANNEL);
    REQUIRE(joins.size() == 2);
    Bytes general;
    net::writeJoinChannel(general, "General - Elwynn Forest", "");
    CHECK(joins[0] == general);
    CHECK(h.social.channelNumber("general - elwynn forest") == 1); // case insensitive
    CHECK(h.social.channelNumber("LocalDefense - Elwynn Forest") == 2);

    // Confirmed by the server.
    Bytes joined;
    net::ByteWriter(joined).u8v(0x02).cstring("General - Elwynn Forest").u32v(0x18).u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_CHANNEL_NOTIFY, joined) == game::Social::Handled::Yes);
    REQUIRE(h.social.channelByNumber(1));
    CHECK(h.social.channelByNumber(1)->joined);
    CHECK(h.social.channelByNumber(1)->flags == 0x18);
    auto ev = h.events();
    REQUIRE(find(ev, SocialEventKind::ChannelNotice));
    CHECK(find(ev, SocialEventKind::ChannelNotice)->value[1] == 1);

    // A custom channel takes the next free number.
    REQUIRE(h.social.joinChannel("Prueba"));
    CHECK(h.social.channelNumber("Prueba") == 3);

    // Stormwind: General changes name on the same number, Trade arrives, LocalDefense follows the zone.
    const game::ZoneChannel city[] = {
        {1, "General - Stormwind City"}, {2, "Trade - City"}, {22, "LocalDefense - Stormwind City"}};
    h.sent.clear();
    h.social.setZone(1519, city);
    CHECK(h.social.channelNumber("General - Stormwind City") == 1);
    CHECK(h.social.channelNumber("LocalDefense - Stormwind City") == 2);
    CHECK(h.social.channelNumber("Trade - City") == 4);
    CHECK(h.social.channelNumber("General - Elwynn Forest") == 0);
    CHECK(h.sentWith(Opcode::CMSG_LEAVE_CHANNEL).size() == 2);
    CHECK(h.sentWith(Opcode::CMSG_JOIN_CHANNEL).size() == 3);
    // The late YouLeft of the old General does not free the renamed number.
    Bytes left;
    net::ByteWriter(left).u8v(0x03).cstring("General - Elwynn Forest");
    REQUIRE(h.packet(Opcode::SMSG_CHANNEL_NOTIFY, left) == game::Social::Handled::Yes);
    CHECK(h.social.channelNumber("General - Stormwind City") == 1);

    // Out of the city: Trade is left; when it comes back it keeps its number.
    const game::ZoneChannel goldshire[] = {{1, "General - Elwynn Forest"}, {22, "LocalDefense - Elwynn Forest"}};
    h.social.setZone(12, goldshire);
    CHECK(h.social.channelNumber("Trade - City") == 0);
    REQUIRE(h.social.joinChannel("Otro")); // a custom channel does not take Trade's number
    CHECK(h.social.channelNumber("Otro") == 5);
    h.social.setZone(1519, city);
    CHECK(h.social.channelNumber("Trade - City") == 4);

    // Leaving by hand, a refused join (wrong password) frees its number.
    REQUIRE(h.social.leaveChannel("prueba"));
    CHECK(h.sentWith(Opcode::CMSG_LEAVE_CHANNEL).back() == cstring("Prueba"));
    CHECK(h.social.channelNumber("Prueba") == 0);
    REQUIRE(h.social.joinChannel("Secreto", "clave"));
    Bytes withPassword;
    net::writeJoinChannel(withPassword, "Secreto", "clave");
    CHECK(h.sentWith(Opcode::CMSG_JOIN_CHANNEL).back() == withPassword);
    Bytes wrong;
    net::ByteWriter(wrong).u8v(0x04).cstring("Secreto");
    REQUIRE(h.packet(Opcode::SMSG_CHANNEL_NOTIFY, wrong) == game::Social::Handled::Yes);
    CHECK(h.social.channelNumber("Secreto") == 0);

    // Someone joins a custom channel: the name is asked for.
    h.events();
    Bytes other;
    net::ByteWriter(other).u8v(0x00).cstring("Otro").u64v(kTolvir);
    h.sent.clear();
    REQUIRE(h.packet(Opcode::SMSG_CHANNEL_NOTIFY, other) == game::Social::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_NAME_QUERY).size() == 1);
    ev = h.events();
    const game::SocialEvent* notice = find(ev, SocialEventKind::ChannelNotice);
    REQUIRE(notice);
    CHECK(notice->guid == kTolvir);
    CHECK(notice->value[1] == 5);

    // The member list.
    Bytes list;
    net::ByteWriter(list).cstring("Otro").u8v(1).u32v(1).u64v(kTolvir).u8v(1);
    REQUIRE(h.packet(Opcode::SMSG_CHANNEL_LIST, list) == game::Social::Handled::Yes);
    CHECK(h.social.channelMembers().members.size() == 1);
    CHECK(find(h.events(), SocialEventKind::ChannelMembers));

    // A new character starts with no channels and asks for the zone again; kinds without a channel in the zone
    // keep their numbers (Trade is /2 even outside the cities).
    h.social.enterWorld(kMe, true, h.now);
    const game::ZoneChannel all[] = {{1, "General - Elwynn Forest"},
                                     {2, ""},
                                     {22, "LocalDefense - Elwynn Forest"},
                                     {24, "LookingForGroup"}};
    h.social.setZone(12, all);
    CHECK(h.social.channelNumber("General - Elwynn Forest") == 1);
    CHECK(h.social.channelNumber("LocalDefense - Elwynn Forest") == 3);
    CHECK(h.social.channelNumber("LookingForGroup") == 4);
    REQUIRE(h.social.joinChannel("Prueba"));
    CHECK(h.social.channelNumber("Prueba") == 5);
    const game::ZoneChannel allCity[] = {{1, "General - Stormwind City"},
                                         {2, "Trade - City"},
                                         {22, "LocalDefense - Stormwind City"},
                                         {24, "LookingForGroup"}};
    h.social.setZone(1519, allCity);
    CHECK(h.social.channelNumber("Trade - City") == 2);
    CHECK(h.social.channelNumber("LookingForGroup") == 4);
    h.social.enterWorld(kMe, true, h.now);
    CHECK(h.social.zone() == 0);
    CHECK(h.social.channelNumber("Trade - City") == 0);
}

TEST_CASE("Social: AFK and DND toggles, text emotes") {
    Harness h;
    h.apply(createdPlayer(kMe, playerFields()));
    h.social.update(h.now);
    h.social.setAfk("comiendo");
    Bytes afk;
    net::writeMessageChat(afk, net::ChatType::Afk, net::Language::Universal, {}, "comiendo");
    CHECK(h.sentWith(Opcode::CMSG_MESSAGECHAT).back() == afk);
    h.social.setDnd("");
    Bytes dnd;
    net::writeMessageChat(dnd, net::ChatType::Dnd, net::Language::Universal, {}, "");
    CHECK(h.sentWith(Opcode::CMSG_MESSAGECHAT).back() == dnd);
    // The server sets the flag: the change is reported once.
    h.apply(values(kMe, {{uf::PLAYER_FLAGS, game::kPlayerFlagAfk}}));
    h.social.update(h.now);
    h.social.update(h.now + 10);
    const auto ev = h.events();
    REQUIRE(std::count_if(ev.begin(), ev.end(), [](const auto& e) { return e.kind == SocialEventKind::AwayChanged; }) ==
            1);
    CHECK(find(ev, SocialEventKind::AwayChanged)->value[0] == 1);
    CHECK(h.social.afk());
    CHECK_FALSE(h.social.dnd());

    h.social.textEmote(101, kAeryn); // WAVE at Aeryn
    Bytes emote;
    net::writeTextEmote(emote, 101, 0, kAeryn);
    CHECK(h.sentWith(Opcode::CMSG_TEXT_EMOTE).back() == emote);
}

TEST_CASE("Social: friend list, status changes as events, ignore list") {
    Harness h;
    Bytes list;
    net::ByteWriter(list).u8v(2).u64v(kAeryn).u8v(net::kFriendOnline).u32v(12).u32v(10).u32v(8).u64v(kBrakk).u8v(0);
    REQUIRE(h.packet(Opcode::SMSG_FRIEND_LIST, list) == game::Social::Handled::Yes);
    REQUIRE(h.social.friends().size() == 2);
    CHECK(h.social.isFriend(kAeryn));
    CHECK(h.sentWith(Opcode::CMSG_NAME_QUERY).size() == 2); // names of both

    // Brakk comes online, Aeryn goes offline.
    Bytes online;
    net::ByteWriter(online).u8v(0x02).u64v(kBrakk).u8v(net::kFriendDnd).u32v(1519).u32v(12).u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_FRIEND_STATUS, online) == game::Social::Handled::Yes);
    Bytes offline;
    net::ByteWriter(offline).u8v(0x03).u64v(kAeryn);
    REQUIRE(h.packet(Opcode::SMSG_FRIEND_STATUS, offline) == game::Social::Handled::Yes);
    const auto& friends = h.social.friends();
    CHECK(friends[1].status == net::kFriendDnd);
    CHECK(friends[1].area == 1519);
    CHECK(friends[0].status == net::kFriendOffline);
    auto ev = h.events();
    CHECK(std::count_if(ev.begin(), ev.end(), [](const auto& e) { return e.kind == SocialEventKind::FriendStatus; }) ==
          2);

    // Adding by name: the body is the name; the answer has only the GUID, the typed name names it.
    h.social.addFriend("Tolvir");
    CHECK(h.sentWith(Opcode::CMSG_ADD_FRIEND).back() == cstring("Tolvir"));
    Bytes added;
    net::ByteWriter(added).u8v(0x07).u64v(kTolvir);
    REQUIRE(h.packet(Opcode::SMSG_FRIEND_STATUS, added) == game::Social::Handled::Yes);
    ev = h.events();
    REQUIRE(find(ev, SocialEventKind::FriendStatus));
    CHECK(find(ev, SocialEventKind::FriendStatus)->name == "Tolvir");
    CHECK(h.social.friends().size() == 3);
    REQUIRE(h.queries.findPlayerName(kTolvir));

    // Errors are only events.
    Bytes notFound;
    net::ByteWriter(notFound).u8v(0x04).u64v(0);
    REQUIRE(h.packet(Opcode::SMSG_FRIEND_STATUS, notFound) == game::Social::Handled::Yes);
    ev = h.events();
    CHECK(find(ev, SocialEventKind::FriendStatus)->value[0] == static_cast<i32>(net::FriendResult::NotFound));
    CHECK(h.social.friends().size() == 3);

    h.social.removeFriend(kTolvir);
    CHECK(h.sentWith(Opcode::CMSG_DEL_FRIEND).back() == guidBody(kTolvir));
    Bytes removed;
    net::ByteWriter(removed).u8v(0x05).u64v(kTolvir);
    REQUIRE(h.packet(Opcode::SMSG_FRIEND_STATUS, removed) == game::Social::Handled::Yes);
    CHECK_FALSE(h.social.isFriend(kTolvir));

    // Ignore list.
    Bytes ignore;
    net::ByteWriter(ignore).u8v(1).u64v(kBrakk);
    REQUIRE(h.packet(Opcode::SMSG_IGNORE_LIST, ignore) == game::Social::Handled::Yes);
    CHECK(h.social.isIgnored(kBrakk));
    h.social.addIgnore("Tolvir");
    Bytes ignored;
    net::ByteWriter(ignored).u8v(0x0F).u64v(kTolvir);
    REQUIRE(h.packet(Opcode::SMSG_FRIEND_STATUS, ignored) == game::Social::Handled::Yes);
    CHECK(h.social.isIgnored(kTolvir));
    h.social.removeIgnore(kBrakk);
    CHECK(h.sentWith(Opcode::CMSG_DEL_IGNORE).back() == guidBody(kBrakk));
    Bytes unignored;
    net::ByteWriter(unignored).u8v(0x10).u64v(kBrakk);
    REQUIRE(h.packet(Opcode::SMSG_FRIEND_STATUS, unignored) == game::Social::Handled::Yes);
    CHECK_FALSE(h.social.isIgnored(kBrakk));
    CHECK_FALSE(h.social.isIgnored(0));
}

TEST_CASE("Social: who request and results") {
    Harness h;
    net::WhoRequest request;
    request.name = "ae";
    h.social.who(request);
    CHECK(h.social.who().pending);
    Bytes body;
    net::writeWho(body, request);
    CHECK(h.sentWith(Opcode::CMSG_WHO).back() == body);
    Bytes who;
    net::ByteWriter(who).u32v(1).u32v(1).cstring("Aeryn").cstring("").u32v(10).u32v(8).u32v(4).u32v(12);
    REQUIRE(h.packet(Opcode::SMSG_WHO, who) == game::Social::Handled::Yes);
    CHECK_FALSE(h.social.who().pending);
    REQUIRE(h.social.who().entries.size() == 1);
    CHECK(h.social.who().entries[0].name == "Aeryn");
    CHECK(h.social.who().version == 1);
    CHECK(find(h.events(), SocialEventKind::WhoResults)->value[0] == 1);
}

TEST_CASE("Social: guild from the player's fields, roster, events, invitations, officers' commands") {
    Harness h;
    h.apply(createdPlayer(kMe, playerFields({{uf::PLAYER_GUILDID, 7}, {uf::PLAYER_GUILDRANK, 1}})));
    h.social.update(h.now);
    // The guild is asked for: its name and ranks, the roster and the info.
    Bytes query;
    net::writeU32(query, 7);
    REQUIRE(h.sentWith(Opcode::CMSG_GUILD_QUERY).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_GUILD_QUERY)[0] == query);
    CHECK(h.sentWith(Opcode::CMSG_GUILD_ROSTER).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_GUILD_INFO).size() == 1);
    h.social.update(h.now + 10); // not again
    CHECK(h.sentWith(Opcode::CMSG_GUILD_QUERY).size() == 1);

    Bytes response;
    net::ByteWriter q(response);
    q.u32v(7).cstring("Los Errantes");
    for (u32 i = 0; i < net::kGuildRankCount; ++i) {
        q.cstring(i == 0 ? "Guild Master" : i == 1 ? "Officer" : "");
    }
    q.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_GUILD_QUERY_RESPONSE, response) == game::Social::Handled::Yes);
    CHECK(h.social.guild().known);
    CHECK(h.social.guild().name == "Los Errantes");
    CHECK(h.social.guild().rankName(1) == "Officer");
    CHECK(h.social.guildName(7) == "Los Errantes");

    Bytes roster;
    net::ByteWriter(roster)
        .u32v(1)
        .cstring("Hola")
        .cstring("")
        .u32v(2)
        .u32v(0x000FF1FF)
        .u32v(0x40 | 0x50 | 0x1040)
        .u64v(kAeryn)
        .u8v(1)
        .cstring("Aeryn")
        .u32v(0)
        .u8v(10)
        .u8v(8)
        .u32v(12)
        .cstring("")
        .cstring("");
    REQUIRE(h.packet(Opcode::SMSG_GUILD_ROSTER, roster) == game::Social::Handled::Yes);
    const game::GuildState& g = h.social.guild();
    CHECK(g.rosterKnown);
    CHECK(g.motd == "Hola");
    REQUIRE(g.members.size() == 1);
    CHECK(g.members[0].name == "Aeryn");
    CHECK(g.can(net::kGuildRightInvite));
    CHECK(g.can(net::kGuildRightSetMotd));
    CHECK_FALSE(g.can(net::kGuildRightRemove));

    // Events: the MOTD changes it; membership changes refresh the open roster.
    Bytes motd;
    net::ByteWriter(motd).u8v(static_cast<u8>(net::GuildEventKind::Motd)).u8v(1).cstring("Raid el viernes");
    REQUIRE(h.packet(Opcode::SMSG_GUILD_EVENT, motd) == game::Social::Handled::Yes);
    CHECK(h.social.guild().motd == "Raid el viernes");
    Bytes signedOn;
    net::ByteWriter(signedOn).u8v(static_cast<u8>(net::GuildEventKind::SignedOn)).u8v(1).cstring("Aeryn").u64v(kAeryn);
    REQUIRE(h.packet(Opcode::SMSG_GUILD_EVENT, signedOn) == game::Social::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_GUILD_ROSTER).size() == 2);
    auto ev = h.events();
    const game::SocialEvent* e = nullptr;
    for (const auto& x : ev) {
        if (x.kind == SocialEventKind::GuildEvent && x.value[0] == static_cast<i32>(net::GuildEventKind::SignedOn)) {
            e = &x;
        }
    }
    REQUIRE(e);
    CHECK(e->name == "Aeryn");
    CHECK(e->guid == kAeryn);

    // Commands: names as bodies.
    h.social.guildInvite("Brakk");
    CHECK(h.sentWith(Opcode::CMSG_GUILD_INVITE).back() == cstring("Brakk"));
    h.social.guildPromote("Aeryn");
    CHECK(h.sentWith(Opcode::CMSG_GUILD_PROMOTE).back() == cstring("Aeryn"));
    h.social.guildDemote("Aeryn");
    CHECK(h.sentWith(Opcode::CMSG_GUILD_DEMOTE).back() == cstring("Aeryn"));
    h.social.guildRemove("Aeryn");
    CHECK(h.sentWith(Opcode::CMSG_GUILD_REMOVE).back() == cstring("Aeryn"));
    h.social.setGuildMotd("Nuevo");
    CHECK(h.sentWith(Opcode::CMSG_GUILD_MOTD).back() == cstring("Nuevo"));
    Bytes failed;
    net::ByteWriter(failed).u32v(net::kGuildCommandInvite).cstring("Brakk").u32v(0x03);
    REQUIRE(h.packet(Opcode::SMSG_GUILD_COMMAND_RESULT, failed) == game::Social::Handled::Yes);
    CHECK(find(h.events(), SocialEventKind::GuildCommandResult)->value[1] == 3);

    // Leaving: the guild field goes to 0 and everything is forgotten.
    h.social.leaveGuild();
    CHECK(h.sentWith(Opcode::CMSG_GUILD_LEAVE).size() == 1);
    h.apply(values(kMe, {{uf::PLAYER_GUILDID, 0}, {uf::PLAYER_GUILDRANK, 0}}));
    h.social.update(h.now);
    CHECK(h.social.guild().id == 0);
    CHECK_FALSE(h.social.guild().rosterKnown);

    // An invitation the other way.
    Bytes invite;
    net::ByteWriter(invite).cstring("Aeryn").cstring("Los Errantes");
    REQUIRE(h.packet(Opcode::SMSG_GUILD_INVITE, invite) == game::Social::Handled::Yes);
    ev = h.events();
    REQUIRE(find(ev, SocialEventKind::GuildInvite));
    CHECK(find(ev, SocialEventKind::GuildInvite)->text == "Los Errantes");
    h.social.acceptGuildInvite();
    REQUIRE(h.sentWith(Opcode::CMSG_GUILD_ACCEPT).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_GUILD_ACCEPT)[0].empty());
    CHECK(h.social.guild().pendingInviter.empty());
}

TEST_CASE("Social: a duel from the challenge to the winner, attackable only while it lasts") {
    Harness h;
    h.apply(createdPlayer(kMe, playerFields()));
    h.apply(createdPlayer(kAeryn, playerFields()));
    const game::Entity* me = h.store.find(kMe);
    const game::Entity* aeryn = h.store.find(kAeryn);
    REQUIRE(me);
    REQUIRE(aeryn);
    CHECK_FALSE(game::canAttack(*me, *aeryn, nullptr)); // a player not flagged for PvP

    // Aeryn challenges the player.
    Bytes request;
    net::ByteWriter(request).u64v(kFlag).u64v(kAeryn);
    REQUIRE(h.packet(Opcode::SMSG_DUEL_REQUESTED, request) == game::Social::Handled::Yes);
    CHECK(h.social.duel().phase == game::DuelPhase::Challenged);
    CHECK(h.social.duel().opponent == kAeryn);
    auto ev = h.events();
    REQUIRE(find(ev, SocialEventKind::DuelRequested));
    CHECK(find(ev, SocialEventKind::DuelRequested)->value[0] == 0);
    h.social.acceptDuel();
    CHECK(h.sentWith(Opcode::CMSG_DUEL_ACCEPTED).back() == guidBody(kFlag));

    // The countdown, then the fight: the server sets both arbiters and the teams.
    Bytes countdown;
    net::writeU32(countdown, 3000);
    REQUIRE(h.packet(Opcode::SMSG_DUEL_COUNTDOWN, countdown) == game::Social::Handled::Yes);
    CHECK(h.social.duel().phase == game::DuelPhase::Countdown);
    h.social.update(h.now + 1000);
    CHECK(h.social.duel().phase == game::DuelPhase::Countdown);
    const Fields arbiter = {{uf::PLAYER_DUEL_ARBITER, static_cast<u32>(kFlag)},
                            {static_cast<u16>(uf::PLAYER_DUEL_ARBITER + 1), static_cast<u32>(kFlag >> 32)}};
    h.apply(values(kMe, arbiter));
    h.apply(values(kAeryn, arbiter));
    CHECK_FALSE(game::canAttack(*h.store.find(kMe), *h.store.find(kAeryn), nullptr)); // no teams yet
    h.apply(values(kMe, {{uf::PLAYER_DUEL_TEAM, 1}}));
    h.apply(values(kAeryn, {{uf::PLAYER_DUEL_TEAM, 2}}));
    h.social.update(h.now + 3000);
    CHECK(h.social.duel().phase == game::DuelPhase::Fighting);
    CHECK(game::inDuelWith(*h.store.find(kMe), *h.store.find(kAeryn)));
    CHECK(game::canAttack(*h.store.find(kMe), *h.store.find(kAeryn), nullptr));

    // Out of bounds and back.
    REQUIRE(h.packet(Opcode::SMSG_DUEL_OUTOFBOUNDS, {}) == game::Social::Handled::Yes);
    CHECK(h.social.duel().outOfBounds);
    REQUIRE(h.packet(Opcode::SMSG_DUEL_INBOUNDS, {}) == game::Social::Handled::Yes);
    CHECK_FALSE(h.social.duel().outOfBounds);

    // Over: complete, the winner for everyone around, the fields cleared.
    REQUIRE(h.packet(Opcode::SMSG_DUEL_COMPLETE, Bytes{1}) == game::Social::Handled::Yes);
    Bytes winner;
    net::ByteWriter(winner).u8v(0).cstring("Aeryn").cstring("Arthel");
    REQUIRE(h.packet(Opcode::SMSG_DUEL_WINNER, winner) == game::Social::Handled::Yes);
    CHECK(h.social.duel().phase == game::DuelPhase::None);
    ev = h.events();
    REQUIRE(find(ev, SocialEventKind::DuelComplete));
    CHECK(find(ev, SocialEventKind::DuelComplete)->value[0] == 1);
    REQUIRE(find(ev, SocialEventKind::DuelWinner));
    CHECK(find(ev, SocialEventKind::DuelWinner)->value[1] == 1); // the player fought it
    h.apply(values(kMe, {{uf::PLAYER_DUEL_ARBITER, 0}, {static_cast<u16>(uf::PLAYER_DUEL_ARBITER + 1), 0},
                         {uf::PLAYER_DUEL_TEAM, 0}}));
    CHECK_FALSE(game::canAttack(*h.store.find(kMe), *h.store.find(kAeryn), nullptr));

    // The player challenges: the target it cast at is the opponent; withdrawing sends CMSG_DUEL_CANCELLED.
    h.social.challengeSent(kAeryn);
    Bytes mine;
    net::ByteWriter(mine).u64v(kFlag).u64v(kMe);
    REQUIRE(h.packet(Opcode::SMSG_DUEL_REQUESTED, mine) == game::Social::Handled::Yes);
    CHECK(h.social.duel().phase == game::DuelPhase::Challenging);
    CHECK(h.social.duel().opponent == kAeryn);
    CHECK(find(h.events(), SocialEventKind::DuelRequested)->value[0] == 1);
    h.social.declineDuel();
    CHECK(h.sentWith(Opcode::CMSG_DUEL_CANCELLED).back() == guidBody(kFlag));
    REQUIRE(h.packet(Opcode::SMSG_DUEL_COMPLETE, Bytes{0}) == game::Social::Handled::Yes);
    CHECK(find(h.events(), SocialEventKind::DuelComplete)->value[0] == 0);
}

TEST_CASE("Social: Gameplay hands it its packets and its session") {
    game::EntityStore store;
    game::Gameplay play{store};
    std::vector<Sent> sent;
    game::PacketSink sink;
    sink.context = &sent;
    sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
        static_cast<std::vector<Sent>*>(context)->push_back({opcode, Bytes(body.begin(), body.end())});
        return true;
    };
    play.setSink(sink);
    play.enterWorld(kMe, 0, true, 1000);
    CHECK(play.handle(Opcode::SMSG_GROUP_INVITE, cstring("Aeryn"), 1000) == game::Gameplay::Handled::Yes);
    CHECK(play.social().group().pendingInviter == "Aeryn");
    Bytes cut = {0x01};
    CHECK(play.handle(Opcode::SMSG_GROUP_LIST, cut, 1000) == game::Gameplay::Handled::Malformed);
    play.social().acceptGroupInvite();
    REQUIRE_FALSE(sent.empty());
    CHECK(sent.back().opcode == Opcode::CMSG_GROUP_ACCEPT);
    // A new character: the invitation is gone; a new map keeps the group.
    play.enterWorld(kMe, 0, false, 2000);
    CHECK(play.handle(Opcode::SMSG_GROUP_LIST, groupList(kAeryn, {{"Aeryn", kAeryn}}), 2000) ==
          game::Gameplay::Handled::Yes);
    play.enterWorld(kMe, 1, false, 3000);
    CHECK(play.social().group().active);
    play.enterWorld(kMe, 0, true, 4000);
    CHECK_FALSE(play.social().group().active);
}

TEST_CASE("Social texts: the client's strings for each answer, the chat's lines, channel notices") {
    using namespace game;
    // Which string answers what (pure mappings).
    CHECK(std::string_view(partyResultKey(net::PartyResult::GroupFull)) == "ERR_GROUP_FULL");
    CHECK(partyResultKey(net::PartyResult::Ok) == nullptr);
    CHECK(std::string_view(guildErrorKey(net::GuildError::Permissions, net::kGuildCommandQuit)) ==
          "ERR_GUILD_LEADER_LEAVE");
    CHECK(std::string_view(guildErrorKey(net::GuildError::Permissions, net::kGuildCommandInvite)) ==
          "ERR_GUILD_PERMISSIONS");
    CHECK(guildErrorKey(net::GuildError::Ok, 0) == nullptr);
    CHECK(guildEventKey(net::GuildEventKind::RosterUpdate) == nullptr);
    using Option = AppearanceOption;
    CHECK(std::string_view(customizationKey(Option::HairStyle, FeatureKind::Horns, FeatureKind::FacialHair)) ==
          "HAIR_HORNS_STYLE");
    CHECK(std::string_view(customizationKey(Option::FacialHair, FeatureKind::HairStyle, FeatureKind::Tusks)) ==
          "FACIAL_HAIR_TUSKS");
    CHECK(itemQualityColor(2) == "|cff1eff00");
    char buffer[256];
    CHECK(formatChannelNotice(buffer, net::ChannelNotice::ModeChange, "1. General", "Aeryn", {}).empty());

    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping the texts");
        return;
    }
    // Every key the social side can ask for is in the baked table (a missing one would show as itself).
    for (u32 v = 0; v <= 0xFF; ++v) {
        for (const char* key : {chatTypeKey(static_cast<net::ChatType>(v)), lootMethodKey(static_cast<u8>(v)),
                                lootMethodChangedKey(static_cast<u8>(v)), itemQualityKey(static_cast<u8>(v)),
                                partyResultKey(static_cast<net::PartyResult>(v)),
                                friendResultKey(static_cast<net::FriendResult>(v)),
                                guildErrorKey(static_cast<net::GuildError>(v), 0),
                                guildErrorKey(static_cast<net::GuildError>(v), net::kGuildCommandQuit),
                                guildEventKey(static_cast<net::GuildEventKind>(v))}) {
            if (key) {
                INFO(key);
                CHECK(text::has(key));
            }
        }
        const std::string_view notice =
            formatChannelNotice(buffer, static_cast<net::ChannelNotice>(v), "2. Trade - City", "Aeryn", "Brakk");
        INFO(notice);
        CHECK(notice.find("_NOTICE") == std::string_view::npos);
        CHECK(notice.find("_GET") == std::string_view::npos);
    }
    for (u32 option = 0; option < static_cast<u32>(AppearanceOption::Count); ++option) {
        for (u32 facial = 0; facial <= static_cast<u32>(FeatureKind::Horns); ++facial) {
            for (const FeatureKind hair : {FeatureKind::HairStyle, FeatureKind::Horns}) {
                const char* key = customizationKey(static_cast<AppearanceOption>(option), hair,
                                                   static_cast<FeatureKind>(facial));
                REQUIRE(key);
                INFO(key);
                CHECK(text::has(key));
            }
        }
    }
    using RC = net::ResponseCode;
    for (u32 code = static_cast<u32>(RC::CHAR_CREATE_IN_PROGRESS); code <= static_cast<u32>(RC::CHAR_NAME_FAILURE);
         ++code) {
        if (code >= static_cast<u32>(RC::CHAR_LOGIN_IN_PROGRESS) && code < static_cast<u32>(RC::CHAR_NAME_NO_NAME)) {
            continue; // the login's, not creation's
        }
        INFO(code);
        CHECK(charResultText(static_cast<u8>(code)) != text::get("CHAR_CREATE_UNKNOWN"));
    }
    CHECK(charResultText(static_cast<u8>(RC::CHAR_CREATE_NAME_IN_USE)) == "That name is unavailable");
    CHECK(charResultText(static_cast<u8>(RC::CHAR_NAME_CONSECUTIVE_SPACES)) ==
          text::get("CHAR_NAME_INVALID_SPACE"));
    CHECK(charResultText(static_cast<u8>(RC::CHAR_NAME_SUCCESS)).empty());
    CHECK(charResultText(250) == "Unknown error");

    // The chat's lines as ChatFrame prints them: players' names linked (a tap opens them), the channel without its
    // zone, the text's own links kept.
    const auto line = [](net::ChatType type, std::string_view sender, std::string_view said, u8 tag = 0,
                         u32 number = 0, std::string_view channel = {}) {
        ChatLine l;
        l.type = type;
        l.tag = tag;
        l.text.assign(said);
        l.channel.assign(channel);
        std::string out;
        appendChatLine(out, l, sender, number);
        return out;
    };
    CHECK(line(net::ChatType::Say, "Aeryn", "hi") == "|Hplayer:Aeryn|h[Aeryn]|h says: hi");
    CHECK(line(net::ChatType::Whisper, "Aeryn", "psst", 1) == "<AFK>|Hplayer:Aeryn|h[Aeryn]|h whispers: psst");
    CHECK(line(net::ChatType::WhisperInform, "Aeryn", "on my way") == "To |Hplayer:Aeryn|h[Aeryn]|h: on my way");
    CHECK(line(net::ChatType::Party, "Brakk", "pull") == "[Party] |Hplayer:Brakk|h[Brakk]|h: pull");
    CHECK(line(net::ChatType::Guild, "Brakk", "gz", 3) == "[Guild] <GM>|Hplayer:Brakk|h[Brakk]|h: gz");
    CHECK(line(net::ChatType::Emote, "Aeryn", "dances.") == "Aeryn dances.");
    CHECK(line(net::ChatType::MonsterYell, "Hogger", "Grr!") == "Hogger yells: Grr!");
    CHECK(line(net::ChatType::MonsterEmote, "Hogger", "%s goes into a frenzy!") == "Hogger goes into a frenzy!");
    CHECK(line(net::ChatType::Ignored, "Aeryn", {}) == "Aeryn is ignoring you.");
    CHECK(line(net::ChatType::Channel, "Galen", "WTS |cff1eff00|Hitem:2132:0:0:0|h[Militia Shortsword]|h|r", 0, 2,
               "Trade - City") ==
          "[2. Trade] |Hplayer:Galen|h[Galen]|h: WTS |cff1eff00|Hitem:2132:0:0:0|h[Militia Shortsword]|h|r");
    CHECK(line(net::ChatType::Channel, "Galen", "hey", 0, 0, "Secret") == "[Secret] |Hplayer:Galen|h[Galen]|h: hey");
    CHECK(line(net::ChatType::System, {}, "Welcome!") == "Welcome!");
    // A broken escape never hides the rest of the line: it shows as typed.
    CHECK(line(net::ChatType::Say, "Aeryn", "look |Hitem:12 here") ==
          "|Hplayer:Aeryn|h[Aeryn]|h says: look ||Hitem:12 here");
    // The client's longer sentences, by their keys.
    const auto keyed = [](text::Key key, const auto&... args) {
        char out[256];
        return std::string(text::format(out, key, args...));
    };
    {
        ChatLine notFound;
        notFound.source = ChatSource::PlayerNotFound;
        notFound.text = "Aeryn";
        std::string out;
        appendChatLine(out, notFound, {}, 0);
        CHECK(out == keyed("ERR_CHAT_PLAYER_NOT_FOUND_S", "Aeryn"));
    }
    std::string emote;
    appendTextEmote(emote, "%s waves at %s.", "Aeryn", "Brakk");
    CHECK(emote == "Aeryn waves at Brakk.");
    emote.clear();
    appendTextEmote(emote, {}, "Aeryn", {});
    CHECK(emote == "Aeryn " + std::string(text::get("CHAT_EMOTE_UNKNOWN")));

    // Channel notices: the channel, whom it is about, who did it.
    CHECK(formatChannelNotice(buffer, net::ChannelNotice::YouJoined, "1. General - Elwynn Forest", {}, {}) ==
          "Joined Channel: [1. General - Elwynn Forest]");
    CHECK(formatChannelNotice(buffer, net::ChannelNotice::Joined, "2. Trade - City", "Aeryn", {}) ==
          "[2. Trade] Aeryn joined channel.");
    CHECK(formatChannelNotice(buffer, net::ChannelNotice::PlayerKicked, "2. Trade - City", "Aeryn", "Brakk") ==
          "[2. Trade - City] Player Aeryn kicked by Brakk.");
    CHECK(formatChannelNotice(buffer, net::ChannelNotice::Invite, "Secret", "Brakk", {}) ==
          keyed("CHAT_INVITE_NOTICE", "Secret", "Brakk"));
    CHECK(std::string_view(buffer).starts_with("Brakk ")); // the inviter first: positional arguments
    CHECK(formatChannelNotice(buffer, net::ChannelNotice::WrongPassword, "Secret", {}, {}) ==
          keyed("CHAT_WRONG_PASSWORD_NOTICE", "Secret"));

    // The Spanish table: our keys and the client's, with the arguments reordered where Spanish needs it.
    REQUIRE(text::load(std::filesystem::path(std::getenv("VD_PACKS_DIR")) / "ui.vdpk", "esES"));
    CHECK(text::get("VD_SOCIAL_GHOST") == "Fantasma");
    CHECK(line(net::ChatType::Say, "Aeryn", "hola") == "|Hplayer:Aeryn|h[Aeryn]|h dice: hola");
    CHECK(formatChannelNotice(buffer, net::ChannelNotice::PlayerKicked, "2. Trade - City", "Aeryn", "Brakk") ==
          (test::esESClientTexts() ? keyed("CHAT_PLAYER_KICKED_NOTICE", "2. Trade - City", "Aeryn", "Brakk")
                                   : "[2. Trade - City] Brakk ha expulsado a Aeryn.")); // phase 21: Blizzard's, or ours
    text::unload();
}
