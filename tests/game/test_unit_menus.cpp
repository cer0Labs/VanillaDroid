// Phase 13 (social): the players' menus (game/UnitPopup: UnitPopup.lua's UnitPopupMenus, UnitPopup_HideButtons and
// UnitPopup_OnUpdate, menu by menu) and the game side of their buttons and of the slash commands (game/
// PlayerCommands through game::Gameplay with a capturing packet sink: raid target icons, inspect, /played, /pvp,
// guild leader and disband, ready check, assistants, pet names, the channels' moderation). Server packets are
// written as vmangos-core writes them; client packets are checked byte for byte.
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/PlayerCommands.h"
#include "game/UnitPopup.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/PlayerCommandMessages.h"
#include "net/SocialMessages.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using game::UnitPopupButton;
using game::UnitPopupMenu;
using game::UnitPopupState;
using net::Opcode;
namespace uf = net::uf;
using B = UnitPopupButton;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kMe = 0x11;
constexpr u64 kAeryn = 0x21;
constexpr u64 kBrakk = 0x22;
constexpr u64 kHogger = 0xF130000001C5A1B2ull;

std::vector<B> buttons(UnitPopupMenu menu, const UnitPopupState& s) {
    std::array<game::UnitPopupItem, game::kUnitPopupMaxItems> items{};
    const u32 n = game::unitPopupItems(menu, s, items);
    std::vector<B> out;
    for (u32 i = 0; i < n; ++i) {
        out.push_back(items[i].button);
    }
    return out;
}

game::UnitPopupItem item(UnitPopupMenu menu, const UnitPopupState& s, B button) {
    std::array<game::UnitPopupItem, game::kUnitPopupMaxItems> items{};
    const u32 n = game::unitPopupItems(menu, s, items);
    for (u32 i = 0; i < n; ++i) {
        if (items[i].button == button) {
            return items[i];
        }
    }
    FAIL("button not shown");
    return {};
}

// A party leader looking at a party member beside it (every distance in reach).
UnitPopupState leaderOf() {
    UnitPopupState s;
    s.inParty = true;
    s.leader = true;
    s.canCooperate = true;
    s.lootMethod = 3; // group loot
    s.targetExists = true;
    s.targetIsPlayer = true;
    s.targetCanCooperate = true;
    s.targetInGroup = true;
    return s;
}

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
    w.f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f);
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

Bytes groupList(u64 leader, std::vector<std::pair<const char*, u64>> members, bool raid = false) {
    Bytes out;
    net::ByteWriter w(out);
    w.u8v(raid ? 1 : 0).u8v(0).u32v(static_cast<u32>(members.size()));
    for (const auto& [name, guid] : members) {
        w.cstring(name).u64v(guid).u8v(net::kMemberOnline).u8v(0);
    }
    w.u64v(leader);
    if (!members.empty()) {
        w.u8v(3).u64v(0).u8v(2).u8v(0);
    }
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
    u32 cursor = 0;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kMe, 0, true, now);
        REQUIRE(store.applyUpdateObject(createdPlayer(kMe, {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_LEVEL, 10}}),
                                        now));
    }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) { return play.handle(opcode, body, now); }
    std::vector<Bytes> sentWith(Opcode opcode) const {
        std::vector<Bytes> out;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out.push_back(s.body);
            }
        }
        return out;
    }
    std::vector<game::GameEvent> events() {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            out.push_back(*e);
        }
        return out;
    }
    void tick(u32 ms = 50) {
        now += ms;
        play.update(now);
    }
};

const game::GameEvent* find(const std::vector<game::GameEvent>& events, game::GameEventKind kind) {
    for (const game::GameEvent& e : events) {
        if (e.kind == kind) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Unit menus: UnitPopupMenus as UnitPopup_HideButtons shows them") {
    // A party member's menu, its leader looking: every button of "PARTY" (LOOT_PROMOTE only with master looting).
    UnitPopupState s = leaderOf();
    CHECK(buttons(UnitPopupMenu::Party, s) == std::vector<B>{B::Whisper, B::Promote, B::Uninvite, B::Inspect, B::Trade,
                                                              B::Follow, B::Duel, B::RaidTargetIcon, B::Cancel});
    s.lootMethod = 2;
    CHECK(buttons(UnitPopupMenu::Party, s)[2] == B::LootPromote);
    s.unitIsMasterLooter = true; // already the master looter: not offered
    const std::vector<B> promoted = buttons(UnitPopupMenu::Party, s);
    CHECK(std::count(promoted.begin(), promoted.end(), B::LootPromote) == 0);

    // A member looking: no promote, no uninvite, no raid target icons (leaders and assistants only).
    s = leaderOf();
    s.leader = false;
    CHECK(buttons(UnitPopupMenu::Party, s) ==
          std::vector<B>{B::Whisper, B::Inspect, B::Trade, B::Follow, B::Duel, B::Cancel});

    // Another player of the same side, out of a group: "PLAYER".
    s = UnitPopupState{};
    s.canCooperate = true;
    s.targetExists = s.targetIsPlayer = s.targetCanCooperate = true;
    CHECK(buttons(UnitPopupMenu::Player, s) ==
          std::vector<B>{B::Whisper, B::Inspect, B::Invite, B::Trade, B::Follow, B::Duel, B::Cancel});
    // An enemy player: only what needs no cooperation (nothing but CANCEL): the menu does not open.
    s.canCooperate = s.targetCanCooperate = false;
    CHECK(buttons(UnitPopupMenu::Player, s).empty());
    // ... and in a group led by the player, still no icon on an enemy player (UnitPopup_HideButtons' target rule).
    s.inParty = s.leader = true;
    CHECK(buttons(UnitPopupMenu::Player, s).empty());

    // The player's own menu: alone it is only CANCEL (no menu), unless instances can be reset.
    s = UnitPopupState{};
    s.canCooperate = true;
    CHECK(buttons(UnitPopupMenu::Self, s).empty());
    s.canShowResetInstances = true;
    CHECK(buttons(UnitPopupMenu::Self, s) == std::vector<B>{B::ResetInstances, B::Cancel});
    // In a group led by someone else: the loot rules to read, leave; no reset, no icons.
    s.inParty = true;
    CHECK(buttons(UnitPopupMenu::Self, s) ==
          std::vector<B>{B::LootMethod, B::LootThreshold, B::Leave, B::Cancel});
    CHECK_FALSE(item(UnitPopupMenu::Self, s, B::LootMethod).nested); // only the leader opens them
    s.leader = true;
    CHECK(buttons(UnitPopupMenu::Self, s) == std::vector<B>{B::LootMethod, B::LootThreshold, B::Leave,
                                                             B::ResetInstances, B::RaidTargetIcon, B::Cancel});
    CHECK(item(UnitPopupMenu::Self, s, B::LootMethod).nested);
    CHECK(item(UnitPopupMenu::Self, s, B::RaidTargetIcon).nested);

    // A creature targeted: the raid target icons alone, for a group's leader or assistant only.
    s = UnitPopupState{};
    s.targetExists = true;
    CHECK(buttons(UnitPopupMenu::RaidTargetIcon, s).empty());
    s.inParty = true;
    CHECK(buttons(UnitPopupMenu::RaidTargetIcon, s).empty());
    s.assistant = true;
    s.targetRaidTarget = 8;
    const std::vector<B> icons = buttons(UnitPopupMenu::RaidTargetIcon, s);
    CHECK(icons.size() == 9);
    CHECK(icons.back() == B::RaidTargetNone);
    CHECK(item(UnitPopupMenu::RaidTargetIcon, s, B::RaidTarget8).checked);
    CHECK_FALSE(item(UnitPopupMenu::RaidTargetIcon, s, B::RaidTarget1).checked);

    // The pets: a hunter's (abandonable, renamable once) and a warlock's.
    s = UnitPopupState{};
    s.petCanBeAbandoned = true;
    s.petCanBeRenamed = true;
    CHECK(buttons(UnitPopupMenu::Pet, s) == std::vector<B>{B::PetPaperdoll, B::PetRename, B::PetAbandon, B::Cancel});
    s.petCanBeRenamed = false;
    CHECK(buttons(UnitPopupMenu::Pet, s) == std::vector<B>{B::PetPaperdoll, B::PetAbandon, B::Cancel});
    s.petCanBeAbandoned = false;
    CHECK(buttons(UnitPopupMenu::Pet, s) == std::vector<B>{B::PetDismiss, B::Cancel});

    // A friend's name (no unit): invite unless already in the party; the guild's buttons only in its tab.
    s = UnitPopupState{};
    s.hasUnit = false;
    CHECK(buttons(UnitPopupMenu::Friend, s) == std::vector<B>{B::Whisper, B::Invite, B::Target, B::Cancel});
    s.nameInParty = true;
    CHECK(buttons(UnitPopupMenu::Friend, s) == std::vector<B>{B::Whisper, B::Target, B::Cancel});
    s.nameInParty = false;
    s.guildFrameVisible = true;
    s.guildLeader = true;
    CHECK(buttons(UnitPopupMenu::Friend, s) ==
          std::vector<B>{B::Whisper, B::Invite, B::Target, B::GuildPromote, B::Cancel});
    s.nameIsSelf = true; // the player's own name in the roster: leave, not promote
    CHECK(buttons(UnitPopupMenu::Friend, s) == std::vector<B>{B::Whisper, B::Target, B::GuildLeave, B::Cancel});

    // The raid roster, by the members' ranks.
    s = leaderOf();
    s.raidRank = 0;
    CHECK(buttons(UnitPopupMenu::Raid, s) ==
          std::vector<B>{B::RaidLeader, B::RaidPromote, B::RaidRemove, B::Cancel});
    s.raidRank = 1;
    CHECK(buttons(UnitPopupMenu::Raid, s) == std::vector<B>{B::RaidLeader, B::RaidDemote, B::RaidRemove, B::Cancel});
    s.raidRank = 2; // the leader himself
    CHECK(buttons(UnitPopupMenu::Raid, s).empty());
    s.raidRank = 0;
    s.leader = false;
    s.assistant = true; // an assistant may only remove
    CHECK(buttons(UnitPopupMenu::Raid, s) == std::vector<B>{B::RaidRemove, B::Cancel});
}

TEST_CASE("Unit menus: UnitPopup_OnUpdate's enabled buttons and the second levels") {
    UnitPopupState s = leaderOf();
    // CheckInteractDistance: inspect 10 yards, trade 11.11, duel 10, follow 30 (WoW.exe 1.12.1).
    CHECK(game::kInteractDistances[1] == 10.0f);
    CHECK(game::kInteractDistances[2] == doctest::Approx(11.11f));
    CHECK(game::kInteractDistances[4] == 30.0f);
    s.inDistance = {true, false, false, false, true}; // 20 yards away
    CHECK_FALSE(item(UnitPopupMenu::Party, s, B::Inspect).enabled);
    CHECK_FALSE(item(UnitPopupMenu::Party, s, B::Trade).enabled);
    CHECK_FALSE(item(UnitPopupMenu::Party, s, B::Duel).enabled);
    CHECK(item(UnitPopupMenu::Party, s, B::Follow).enabled);
    CHECK(item(UnitPopupMenu::Party, s, B::Whisper).enabled);
    s.inDistance = {true, true, true, true, true};
    s.unitDeadOrGhost = true; // no trading nor duelling a corpse
    CHECK_FALSE(item(UnitPopupMenu::Party, s, B::Trade).enabled);
    CHECK_FALSE(item(UnitPopupMenu::Party, s, B::Duel).enabled);
    CHECK(item(UnitPopupMenu::Party, s, B::Inspect).enabled);
    s.unitDeadOrGhost = false;
    s.playerDead = s.playerDeadOrGhost = true;
    CHECK_FALSE(item(UnitPopupMenu::Party, s, B::Follow).enabled);
    CHECK_FALSE(item(UnitPopupMenu::Party, s, B::Inspect).enabled);
    s = leaderOf();
    s.unitConnected = false; // offline: no whisper
    CHECK_FALSE(item(UnitPopupMenu::Party, s, B::Whisper).enabled);

    // In a group led by someone else, inviting a stranger is greyed (leaders and assistants invite).
    s = UnitPopupState{};
    s.canCooperate = true;
    s.inParty = true;
    CHECK_FALSE(item(UnitPopupMenu::Player, s, B::Invite).enabled);

    // The second levels: every button, checked where the group or the unit is.
    s = leaderOf();
    s.lootMethod = 1;
    s.lootThreshold = 3;
    s.unitRaidTarget = 2;
    std::array<game::UnitPopupItem, game::kUnitPopupMaxItems> sub{};
    REQUIRE(game::unitPopupSubItems(B::LootMethod, s, sub) == 6);
    CHECK(sub[1].button == B::RoundRobin);
    CHECK(sub[1].checked);
    CHECK_FALSE(sub[0].checked);
    CHECK(sub[5].button == B::Cancel);
    REQUIRE(game::unitPopupSubItems(B::LootThreshold, s, sub) == 4);
    CHECK(sub[1].button == B::Quality3);
    CHECK(sub[1].checked);
    REQUIRE(game::unitPopupSubItems(B::RaidTargetIcon, s, sub) == 9);
    CHECK(sub[1].checked); // the circle
    CHECK(game::unitPopupSubItems(B::Trade, s, sub) == 0);

    // The buttons' table: the icons' colours, textures and texts.
    CHECK(game::unitPopupButtonInfo(B::RaidTarget8).icon == 8);
    CHECK(std::string(game::unitPopupButtonInfo(B::RaidTarget8).text) == "RAID_TARGET_8");
    CHECK(std::string(game::unitPopupButtonInfo(B::Invite).text) == "PARTY_INVITE");
    CHECK(game::raidTargetOfButton(B::RaidTarget3) == 3);
    CHECK(game::raidTargetOfButton(B::RaidTargetNone) == 0);
    // SetRaidTargetIcon: the icon the unit wears again takes it off.
    CHECK(game::toggledRaidTarget(8, 8) == 0);
    CHECK(game::toggledRaidTarget(8, 3) == 3);
    CHECK(game::toggledRaidTarget(0, 0) == 0);

    // TargetFrameDropDown_Initialize.
    CHECK(game::targetFrameMenu(true, false, true, false) == UnitPopupMenu::Self);
    CHECK(game::targetFrameMenu(false, true, false, false) == UnitPopupMenu::Pet);
    CHECK(game::targetFrameMenu(false, false, true, true) == UnitPopupMenu::Party);
    CHECK(game::targetFrameMenu(false, false, true, false) == UnitPopupMenu::Player);
    CHECK(game::targetFrameMenu(false, false, false, false) == UnitPopupMenu::RaidTargetIcon);
}

TEST_CASE("Player commands: raid target icons of the group") {
    Harness h;
    game::PlayerCommands& c = h.play.commands();
    // Alone: nothing is sent, nothing is marked.
    CHECK_FALSE(c.setRaidTarget(kHogger, 8));
    h.tick();
    CHECK(h.sentWith(Opcode::MSG_RAID_TARGET_UPDATE).empty());

    // In a party: the list is asked for once.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kMe, {{"Aeryn", kAeryn}})) == game::Gameplay::Handled::Yes);
    h.tick();
    h.tick();
    REQUIRE(h.sentWith(Opcode::MSG_RAID_TARGET_UPDATE).size() == 1);
    CHECK(h.sentWith(Opcode::MSG_RAID_TARGET_UPDATE)[0] == Bytes{0xFF});
    h.events();

    // The skull on Hogger: the server echoes it to the whole group (a delta), the player's included.
    REQUIRE(c.setRaidTarget(kHogger, 8));
    Bytes set;
    net::ByteWriter(set).u8v(7).u64v(kHogger);
    CHECK(h.sentWith(Opcode::MSG_RAID_TARGET_UPDATE).back() == set);
    const u32 version = c.raidTargetVersion();
    Bytes delta;
    net::ByteWriter(delta).u8v(0).u8v(7).u64v(kHogger);
    REQUIRE(h.packet(Opcode::MSG_RAID_TARGET_UPDATE, delta) == game::Gameplay::Handled::Yes);
    CHECK(c.raidTarget(kHogger) == 8);
    CHECK(c.raidTargetUnit(8) == kHogger);
    CHECK(c.raidTargetVersion() != version);
    auto ev = h.events();
    REQUIRE(find(ev, game::GameEventKind::RaidTargetChanged));
    CHECK(find(ev, game::GameEventKind::RaidTargetChanged)->id == 8);
    CHECK(find(ev, game::GameEventKind::RaidTargetChanged)->guid == kHogger);

    // Another member marks Hogger with the star: one icon per unit, the skull goes.
    Bytes star;
    net::ByteWriter(star).u8v(0).u8v(0).u64v(kHogger);
    REQUIRE(h.packet(Opcode::MSG_RAID_TARGET_UPDATE, star) == game::Gameplay::Handled::Yes);
    CHECK(c.raidTarget(kHogger) == 1);
    CHECK(c.raidTargetUnit(8) == 0);

    // Taking it off sends that icon's slot with no unit.
    REQUIRE(c.setRaidTarget(kHogger, 0));
    Bytes off;
    net::ByteWriter(off).u8v(0).u64v(0);
    CHECK(h.sentWith(Opcode::MSG_RAID_TARGET_UPDATE).back() == off);
    CHECK_FALSE(c.setRaidTarget(kAeryn, 0)); // nothing on Aeryn to take off

    // The whole list replaces everything.
    Bytes all;
    net::ByteWriter(all).u8v(1).u8v(5).u64v(kAeryn);
    REQUIRE(h.packet(Opcode::MSG_RAID_TARGET_UPDATE, all) == game::Gameplay::Handled::Yes);
    CHECK(c.raidTarget(kHogger) == 0);
    CHECK(c.raidTarget(kAeryn) == 6);
    CHECK(h.packet(Opcode::MSG_RAID_TARGET_UPDATE, Bytes{0, 9}) == game::Gameplay::Handled::Malformed);

    // Leaving the group: the icons go with it.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(0, {})) == game::Gameplay::Handled::Yes);
    h.tick();
    CHECK(c.raidTarget(kAeryn) == 0);
}

TEST_CASE("Player commands: inspect, /played, /pvp, guild, ready check, assistants, pet names, channels") {
    Harness h;
    game::PlayerCommands& c = h.play.commands();
    h.events();

    REQUIRE(c.inspect(kAeryn));
    Bytes guid;
    net::writeGuid(guid, kAeryn);
    CHECK(h.sentWith(Opcode::CMSG_INSPECT).back() == guid);
    CHECK(h.packet(Opcode::SMSG_INSPECT, guid) == game::Gameplay::Handled::Yes); // read, nothing else
    CHECK(h.packet(Opcode::SMSG_INSPECT, Bytes{1, 2}) == game::Gameplay::Handled::Malformed);

    REQUIRE(c.requestTimePlayed());
    CHECK(h.sentWith(Opcode::CMSG_PLAYED_TIME).back().empty());
    Bytes played;
    net::ByteWriter(played).u32v(90061).u32v(3600);
    REQUIRE(h.packet(Opcode::SMSG_PLAYED_TIME, played) == game::Gameplay::Handled::Yes);
    auto ev = h.events();
    REQUIRE(find(ev, game::GameEventKind::TimePlayed));
    CHECK(find(ev, game::GameEventKind::TimePlayed)->value[0] == 90061);
    CHECK(find(ev, game::GameEventKind::TimePlayed)->value[1] == 3600);

    // /pvp: the wish flips on the server; the player's flags tell, and the line says which.
    REQUIRE(c.togglePvp(h.now));
    CHECK(h.sentWith(Opcode::CMSG_TOGGLE_PVP).back().empty()); // TogglePVP sends no state
    h.tick();
    CHECK_FALSE(find(h.events(), game::GameEventKind::PvpToggled));
    REQUIRE(h.store.applyUpdateObject(values(kMe, {{uf::PLAYER_FLAGS, game::kPlayerFlagPvpDesired}}), h.now));
    h.tick();
    ev = h.events();
    REQUIRE(find(ev, game::GameEventKind::PvpToggled));
    CHECK(find(ev, game::GameEventKind::PvpToggled)->value[0] == 1);

    REQUIRE(c.guildSetLeader("Aeryn"));
    CHECK(h.sentWith(Opcode::CMSG_GUILD_LEADER).back() == Bytes{'A', 'e', 'r', 'y', 'n', 0});
    REQUIRE(c.guildDisband());
    CHECK(h.sentWith(Opcode::CMSG_GUILD_DISBAND).back().empty());
    // Phase 16 (alerts): DoReadyCheck is the group leader's alone (game::ReadyCheck; test_raid_alerts.cpp).
    CHECK_FALSE(c.readyCheck());
    CHECK(h.sentWith(Opcode::MSG_RAID_READY_CHECK).empty());
    REQUIRE(c.setAssistant(kBrakk, true));
    Bytes assistant;
    net::ByteWriter(assistant).u64v(kBrakk).u8v(1);
    CHECK(h.sentWith(Opcode::CMSG_GROUP_ASSISTANT_LEADER).back() == assistant);

    REQUIRE(c.renamePet(0xF14000000000BEEFull, "Fang"));
    Bytes rename;
    net::ByteWriter(rename).u64v(0xF14000000000BEEFull).cstring("Fang");
    CHECK(h.sentWith(Opcode::CMSG_PET_RENAME).back() == rename);
    REQUIRE(h.packet(Opcode::SMSG_PET_NAME_INVALID, {}) == game::Gameplay::Handled::Yes);
    CHECK(find(h.events(), game::GameEventKind::PetNameInvalid));

    // The channels' moderation: the channel and a player, or the channel alone.
    REQUIRE(c.channelCommand(Opcode::CMSG_CHANNEL_MODERATOR, "Trade - City", "Galen"));
    Bytes two;
    net::ByteWriter(two).cstring("Trade - City").cstring("Galen");
    CHECK(h.sentWith(Opcode::CMSG_CHANNEL_MODERATOR).back() == two);
    REQUIRE(c.channelCommand(Opcode::CMSG_CHANNEL_ANNOUNCEMENTS, "secretos"));
    CHECK(h.sentWith(Opcode::CMSG_CHANNEL_ANNOUNCEMENTS).back() == Bytes{'s', 'e', 'c', 'r', 'e', 't', 'o', 's', 0});
    REQUIRE(c.channelCommand(Opcode::CMSG_CHANNEL_OWNER, "secretos"));
    CHECK(h.sentWith(Opcode::CMSG_CHANNEL_OWNER).size() == 1);
    for (const Opcode op : {Opcode::CMSG_CHANNEL_PASSWORD, Opcode::CMSG_CHANNEL_SET_OWNER, Opcode::CMSG_CHANNEL_UNMODERATOR,
                            Opcode::CMSG_CHANNEL_MUTE, Opcode::CMSG_CHANNEL_UNMUTE, Opcode::CMSG_CHANNEL_INVITE,
                            Opcode::CMSG_CHANNEL_KICK, Opcode::CMSG_CHANNEL_BAN, Opcode::CMSG_CHANNEL_UNBAN}) {
        CHECK(c.channelCommand(op, "secretos", "Galen"));
        CHECK(h.sentWith(op).size() == 1);
    }
    CHECK(c.channelCommand(Opcode::CMSG_CHANNEL_MODERATE, "secretos"));
    CHECK_FALSE(c.channelCommand(Opcode::CMSG_CHANNEL_KICK, "", "Galen")); // no channel
    CHECK_FALSE(c.channelCommand(Opcode::CMSG_GUILD_LEADER, "secretos")); // not a channel command

    // Out of the world nothing is sent.
    h.play.leaveWorld();
    const usize before = h.sent.size();
    CHECK_FALSE(c.requestTimePlayed());
    CHECK(h.sent.size() == before);
}
