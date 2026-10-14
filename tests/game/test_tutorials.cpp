// Phase 18 (p3): the tutorials through game::Gameplay with synthetic packets, as WoW.exe 1.12.1 keeps them (Tutorial.cpp
// 0x4B5280..0x4B5A60: SMSG_TUTORIAL_FLAGS, TriggerTutorial, FlagTutorial, ClearTutorials, ResetTutorials,
// TutorialsEnabled) and as its call sites trigger them; and the account data kept in step with the server
// (AccountData.cpp 0x5AF8E0 / 0x5AFA10 / 0x5AFC60 / 0x5AFD70) against a fake store.
#include "game/AccountData.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Tutorials.h"
#include "net/AccountDataMessages.h"
#include "net/ByteBuffer.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using game::GameEventKind;
using game::Tutorial;
using net::Opcode;
namespace uf = net::uf;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x11;
constexpr u64 kOtherPlayer = 0x12;
constexpr u64 kWolf = 0xF130000000000456ull;
constexpr u64 kFlightMaster = 0xF130000000000457ull;

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

Bytes created(u64 guid, game::ObjectType type, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
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

Bytes tutorialFlags(net::TutorialWords words) {
    Bytes out;
    net::ByteWriter w(out);
    for (const u32 word : words) {
        w.u32v(word);
    }
    return out;
}

// HandleItemQuerySingleOpcode's layout with the fields the tutorials look at.
Bytes itemQuery(u32 entry, u32 itemClass, u32 inventoryType, u32 useSpell) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(entry).u32v(itemClass).u32v(0).cstring("Item").cstring("").cstring("").cstring("");
    w.u32v(1000).u32v(1).u32v(0).u32v(4).u32v(1).u32v(inventoryType);
    w.u32v(0xFFFFFFFFu).u32v(0xFFFFFFFFu).u32v(5).u32v(0);
    for (u32 i = 0; i < 8; ++i) {
        w.u32v(0);
    }
    w.u32v(1).u32v(0);
    for (u32 i = 0; i < 10; ++i) {
        w.u32v(0).u32v(0);
    }
    for (u32 i = 0; i < 5; ++i) {
        w.f32v(0.0f).f32v(0.0f).u32v(0);
    }
    for (u32 i = 0; i < 7; ++i) {
        w.u32v(0);
    }
    w.u32v(0).u32v(0).f32v(0.0f);
    for (u32 i = 0; i < 5; ++i) {
        w.u32v(i == 0 ? useSpell : 0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    }
    w.u32v(0).cstring("").u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    return out;
}

Bytes itemPush(u64 player, u32 entry) {
    Bytes out;
    net::ByteWriter(out).u64v(player).u32v(0).u32v(0).u32v(1).u8v(255).u32v(23).u32v(entry).u32v(0).u32v(0).u32v(1);
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
    std::vector<u64> createdGuids;
    u32 now = 1000;
    u32 cursor = 0;

    explicit Harness(bool flags = true) {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        cursor = play.events().end();
        if (flags) {
            // SMSG_TUTORIAL_FLAGS comes before SMSG_LOGIN_VERIFY_WORLD (SendInitialPacketsBeforeAddToMap).
            REQUIRE(packet(Opcode::SMSG_TUTORIAL_FLAGS, tutorialFlags({})) == game::Gameplay::Handled::Yes);
        }
        play.enterWorld(kPlayer, 0, true, now);
        apply(created(kPlayer, game::ObjectType::Player,
                      {{uf::UNIT_FIELD_HEALTH, 100},
                       {uf::UNIT_FIELD_MAXHEALTH, 100},
                       {uf::UNIT_FIELD_BYTES_0, 1 | (1u << 8)}, // a human warrior
                       {uf::UNIT_FIELD_FACTIONTEMPLATE, 1},
                       {uf::UNIT_FIELD_LEVEL, 3}}));
    }
    void apply(const Bytes& update) {
        createdGuids.clear();
        game::UpdateStats stats;
        stats.createdGuids = &createdGuids;
        REQUIRE(store.applyUpdateObject(update, now, &stats));
        play.afterUpdate(createdGuids, now);
    }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) { return play.handle(opcode, body, now); }
    void step(u32 ms) {
        now += ms;
        play.update(now);
    }
    // The tutorials triggered since the last call, in order.
    std::vector<Tutorial> triggered() {
        std::vector<Tutorial> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            if (e->kind == GameEventKind::TutorialTriggered) {
                out.push_back(static_cast<Tutorial>(e->id));
            }
        }
        return out;
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

bool contains(const std::vector<Tutorial>& list, Tutorial t) {
    return std::find(list.begin(), list.end(), t) != list.end();
}

} // namespace

TEST_CASE("Tutorials: the flags, TriggerTutorial once per session, FlagTutorial, Clear and Reset") {
    Harness h;
    // PLAYER_ENTERING_WORLD: Welcome, then Questgivers.
    const std::vector<Tutorial> entered = h.triggered();
    REQUIRE(entered.size() == 2);
    CHECK(entered[0] == Tutorial::Welcome);
    CHECK(entered[1] == Tutorial::Questgivers);
    game::Tutorials& t = h.play.tutorials();
    CHECK(t.known());
    CHECK(t.enabled());
    // Once per session: another map's PLAYER_ENTERING_WORLD triggers nothing.
    h.play.enterWorld(kPlayer, 1, false, h.now);
    CHECK(h.triggered().empty());
    // FlagTutorial: CMSG_TUTORIAL_FLAG with the 0-based id, once.
    t.flag(Tutorial::Welcome);
    t.flag(Tutorial::Welcome);
    REQUIRE(h.sentWith(Opcode::CMSG_TUTORIAL_FLAG).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_TUTORIAL_FLAG)[0] == Bytes{41, 0, 0, 0});
    CHECK(t.flagged(Tutorial::Welcome));
    // ClearTutorials: every bit, CMSG_TUTORIAL_CLEAR; TutorialsEnabled turns false.
    t.clear();
    CHECK(h.sentWith(Opcode::CMSG_TUTORIAL_CLEAR).size() == 1);
    CHECK_FALSE(t.enabled());
    t.trigger(Tutorial::Bags, h.now);
    CHECK(h.triggered().empty());
    // ResetTutorials: every bit clear, CMSG_TUTORIAL_RESET; they show again.
    t.reset();
    CHECK(h.sentWith(Opcode::CMSG_TUTORIAL_RESET).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_TUTORIAL_RESET)[0].empty());
    CHECK(t.enabled());
    t.trigger(Tutorial::Bags, h.now);
    CHECK(h.triggered() == std::vector<Tutorial>{Tutorial::Bags});

    // A new login: the server's flags again (seen ones stay quiet).
    Harness seen(false);
    net::TutorialWords words{};
    words[1] = 1u << (41 - 32); // Welcome flagged
    REQUIRE(seen.packet(Opcode::SMSG_TUTORIAL_FLAGS, tutorialFlags(words)) == game::Gameplay::Handled::Yes);
    seen.play.enterWorld(kPlayer, 0, true, seen.now);
    CHECK(seen.triggered() == std::vector<Tutorial>{Tutorial::Questgivers});
    // Truncated flags: malformed.
    CHECK(seen.packet(Opcode::SMSG_TUTORIAL_FLAGS, Bytes(31, 0)) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Tutorials: nothing before the flags came") {
    Harness h(false);
    CHECK_FALSE(h.play.tutorials().known());
    CHECK(h.triggered().empty());
    h.play.tutorials().trigger(Tutorial::Bags, h.now);
    h.step(100000);
    CHECK(h.triggered().empty());
}

TEST_CASE("Tutorials: Movement after 90 s still, Targeting 10 s after the first move") {
    Harness h;
    h.triggered();
    h.step(89000);
    CHECK(h.triggered().empty());
    h.step(1500);
    CHECK(h.triggered() == std::vector<Tutorial>{Tutorial::Movement});

    Harness moving;
    moving.triggered();
    moving.step(1000);
    moving.play.tutorials().playerMoved(moving.now);
    CHECK(moving.play.tutorials().flagged(Tutorial::Movement)); // the move flags it: it never shows
    REQUIRE(moving.sentWith(Opcode::CMSG_TUTORIAL_FLAG).size() == 1);
    CHECK(moving.sentWith(Opcode::CMSG_TUTORIAL_FLAG)[0] == Bytes{1, 0, 0, 0});
    moving.step(9000);
    CHECK(moving.triggered().empty());
    moving.step(1500);
    CHECK(moving.triggered() == std::vector<Tutorial>{Tutorial::Targeting});
    moving.step(100000);
    CHECK(moving.triggered().empty());
}

TEST_CASE("Tutorials: level ups, items, whispers, targets and the player's state") {
    Harness h;
    h.triggered();
    // SMSG_LEVELUP_INFO to level 4: Chatting (3), Trainers and Cameras (4).
    Bytes level;
    net::ByteWriter lw(level);
    lw.u32v(4).u32v(20);
    for (u32 i = 0; i < 10; ++i) {
        lw.u32v(0);
    }
    REQUIRE(h.packet(Opcode::SMSG_LEVELUP_INFO, level) == game::Gameplay::Handled::Yes);
    h.step(16);
    const std::vector<Tutorial> levels = h.triggered();
    CHECK(levels == std::vector<Tutorial>{Tutorial::Chatting, Tutorial::Trainers, Tutorial::Cameras});

    // An item pushed to the player: Backpack at once; the rest once its template is known (a container here).
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kPlayer, 4496)) == game::Gameplay::Handled::Yes);
    h.step(16);
    CHECK(h.triggered() == std::vector<Tutorial>{Tutorial::Backpack});
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(4496, 1, 18, 0)) ==
            game::Gameplay::Handled::Yes);
    h.step(16);
    const std::vector<Tutorial> bag = h.triggered();
    CHECK(contains(bag, Tutorial::Bags));
    CHECK(contains(bag, Tutorial::EquippableItems)); // a bag has bag slots (0x809200)
    CHECK_FALSE(contains(bag, Tutorial::UsingItems));
    // The Hearthstone: on-use spell, its own tutorial; a bow for a warrior: RangedWeapons.
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(6948, 15, 0, 8690)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kPlayer, 6948)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(2504, 2, 15, 0)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kPlayer, 2504)) == game::Gameplay::Handled::Yes);
    h.step(16);
    const std::vector<Tutorial> more = h.triggered();
    CHECK(contains(more, Tutorial::UsingItems));
    CHECK(contains(more, Tutorial::Hearthstones));
    CHECK(contains(more, Tutorial::RangedWeapons));

    // A whisper: ReplyingToTells.
    Bytes whisper;
    net::ByteWriter ww(whisper);
    ww.u8v(static_cast<u8>(net::ChatType::Whisper)).u32v(7).u64v(kOtherPlayer).u32v(4).cstring("hey").u8v(0);
    REQUIRE(h.packet(Opcode::SMSG_MESSAGECHAT, whisper) == game::Gameplay::Handled::Yes);
    h.step(16);
    CHECK(h.triggered() == std::vector<Tutorial>{Tutorial::ReplyingToTells});

    // Targets: another player (Players, the first time only), a hostile creature (CombatMode).
    h.apply(created(kOtherPlayer, game::ObjectType::Player,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_BYTES_0, 1}, {uf::UNIT_FIELD_LEVEL, 3}}));
    h.play.setTarget(kOtherPlayer);
    h.step(16);
    CHECK(h.triggered() == std::vector<Tutorial>{Tutorial::Players});
    h.apply(created(kFlightMaster, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_LEVEL, 55}, {uf::UNIT_NPC_FLAGS, 0x08},
                     {uf::UNIT_FIELD_FLAGS, 0x2 /* not attackable */}}));
    h.play.setTarget(kFlightMaster);
    h.step(16);
    CHECK(h.triggered() == std::vector<Tutorial>{Tutorial::Travel});

    // The player's flags: resting, PvP; swimming; a ghost (Death).
    h.apply(values(kPlayer, {{uf::PLAYER_FLAGS, 0x20}, {uf::UNIT_FIELD_FLAGS, 0x1000}}));
    h.step(300);
    const std::vector<Tutorial> flags = h.triggered();
    CHECK(contains(flags, Tutorial::Resting));
    CHECK(contains(flags, Tutorial::Pvp));
    h.apply(values(kPlayer, {{uf::PLAYER_BYTES_2, 1u << 24}}));
    h.step(300);
    CHECK(h.triggered() == std::vector<Tutorial>{Tutorial::Rested});
    // In combat under half health: Food (a warrior: no Drink).
    h.apply(values(kPlayer, {{uf::UNIT_FIELD_FLAGS, 0x1000 | 0x80000}, {uf::UNIT_FIELD_HEALTH, 40}}));
    h.step(300);
    CHECK(h.triggered() == std::vector<Tutorial>{Tutorial::Food});
    // The cast refused for want of ammunition (SMSG_CAST_RESULT failed, SPELL_FAILED_NEED_AMMO).
    Bytes cast;
    net::ByteWriter(cast).u32v(75).u8v(2).u8v(47);
    REQUIRE(h.packet(Opcode::SMSG_CAST_RESULT, cast) == game::Gameplay::Handled::Yes);
    h.step(16);
    CHECK(h.triggered() == std::vector<Tutorial>{Tutorial::Ammunition});
}

namespace {

// The files of a fake device: type -> text (absent: not kept by this client).
struct FakeStore {
    std::map<u32, std::string> files;
    std::vector<std::pair<u32, std::string>> written;
    game::AccountDataStore store() {
        game::AccountDataStore s;
        s.context = this;
        s.read = [](void* c, u32 type, std::string& out) {
            FakeStore& self = *static_cast<FakeStore*>(c);
            const auto it = self.files.find(type);
            if (it == self.files.end()) {
                return false;
            }
            out = it->second;
            return true;
        };
        s.write = [](void* c, u32 type, std::string_view text) {
            FakeStore& self = *static_cast<FakeStore*>(c);
            self.files[type] = std::string(text);
            self.written.emplace_back(type, std::string(text));
        };
        return s;
    }
};

Bytes md5Packet(const std::array<net::Md5, net::kAccountDataTypes>& digests) {
    Bytes out;
    for (const net::Md5& d : digests) {
        out.insert(out.end(), d.begin(), d.end());
    }
    return out;
}

} // namespace

TEST_CASE("Account data: WoW.exe's rules at login, the downloads and the 30 s saves") {
    Harness h;
    FakeStore device;
    const std::string accountMacros = "MACRO 1 \"Hi\" Ability_Warrior_Charge\r\n/wave\r\nEND\r\n";
    const std::string charMacrosOld = "MACRO 16777217 \"Old\" Spell_Holy_Heal\r\n/sit\r\nEND\r\n";
    const std::string charMacrosNew = "MACRO 16777217 \"New\" Spell_Holy_Heal\r\n/dance\r\nEND\r\n";
    const std::string chat = "VERSION 2\n\nADDEDVERSION 2\n\n";
    device.files[4] = accountMacros; // changed here since the last sync (synced: nothing)
    device.files[5] = charMacrosOld; // untouched since the last sync
    device.files[7] = chat;          // the server has nothing
    game::AccountData& data = h.play.accountData();
    data.setStore(device.store());
    game::AccountData::Digests synced{};
    synced[5] = net::md5(std::string_view(charMacrosOld));
    data.setSynced(synced);

    std::array<net::Md5, net::kAccountDataTypes> server{};
    server[4] = net::md5(std::string_view("MACRO 1 \"Other\" Ability_Ambush\r\n/cheer\r\nEND\r\n"));
    server[5] = net::md5(std::string_view(charMacrosNew)); // another device changed them
    server[2] = net::md5(std::string_view("bindings")); // not kept here: left alone
    const u32 version = data.syncedVersion();
    REQUIRE(h.packet(Opcode::SMSG_ACCOUNT_DATA_MD5, md5Packet(server)) == game::Gameplay::Handled::Yes);
    // 4: changed here -> up (local wins); 5: untouched here -> asked for; 7: the server has none -> up.
    const std::vector<Bytes> requests = h.sentWith(Opcode::CMSG_REQUEST_ACCOUNT_DATA);
    REQUIRE(requests.size() == 1);
    CHECK(requests[0] == Bytes{5, 0, 0, 0});
    const std::vector<Bytes> uploads = h.sentWith(Opcode::CMSG_UPDATE_ACCOUNT_DATA);
    REQUIRE(uploads.size() == 2);
    net::AccountDataUpdate up;
    REQUIRE(net::parseUpdateAccountData(uploads[0], up));
    CHECK(up.type == 4);
    CHECK(up.text == accountMacros);
    REQUIRE(net::parseUpdateAccountData(uploads[1], up));
    CHECK(up.type == 7);
    CHECK(up.text == chat);
    CHECK(data.waiting(5));
    CHECK_FALSE(data.waiting(4));
    CHECK(data.syncedVersion() != version);
    CHECK(data.synced()[4] == net::md5(std::string_view(accountMacros)));
    CHECK(data.synced()[5] == server[5]);

    // While type 5 waits, a save of it does nothing (0x5AFED0).
    data.changed(5, h.now);
    h.step(31000);
    CHECK(h.sentWith(Opcode::CMSG_UPDATE_ACCOUNT_DATA).size() == 2);

    // The server's text comes: written over the device's file; the type no longer waits.
    Bytes answer;
    REQUIRE(net::writeUpdateAccountData(answer, 5, charMacrosNew));
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_ACCOUNT_DATA, answer) == game::Gameplay::Handled::Yes);
    REQUIRE(device.written.size() == 1);
    CHECK(device.written[0].first == 5);
    CHECK(device.files[5] == charMacrosNew);
    CHECK_FALSE(data.waiting(5));
    CHECK(data.downloads() == 1);

    // A save: 30 s later, only if the text differs from the synced one.
    device.files[7] = chat + "WINDOW 1\nNAME General\n";
    data.changed(7, h.now);
    h.step(29000);
    CHECK(h.sentWith(Opcode::CMSG_UPDATE_ACCOUNT_DATA).size() == 2);
    h.step(1500);
    REQUIRE(h.sentWith(Opcode::CMSG_UPDATE_ACCOUNT_DATA).size() == 3);
    data.changed(7, h.now); // nothing new: nothing goes up
    h.step(31000);
    CHECK(h.sentWith(Opcode::CMSG_UPDATE_ACCOUNT_DATA).size() == 3);
    // Leaving: a waiting save goes at once.
    device.files[4] = accountMacros + "MACRO 2 \"B\" Ability_Ambush\r\n/bow\r\nEND\r\n";
    data.changed(4, h.now);
    data.flush();
    CHECK(h.sentWith(Opcode::CMSG_UPDATE_ACCOUNT_DATA).size() == 4);
    // A truncated MD5 packet is malformed.
    CHECK(h.packet(Opcode::SMSG_ACCOUNT_DATA_MD5, Bytes(127, 0)) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Tutorials: the cost of a frame with a crowd around (desktop)") {
    Harness h;
    // 120 creatures and 39 players around: some lootable, some quest givers, as a capital in a busy hour.
    for (u32 i = 0; i < 160; ++i) {
        const bool player = i >= 120;
        const u64 guid = player ? 0x100 + i : 0xF130000000010000ull + i;
        Fields fields = {{uf::UNIT_FIELD_HEALTH, i % 7 == 0 ? 0u : 100u},
                         {uf::UNIT_FIELD_MAXHEALTH, 100},
                         {uf::UNIT_FIELD_LEVEL, 1 + i % 60},
                         {uf::UNIT_FIELD_FACTIONTEMPLATE, i % 3 == 0 ? 14u : 12u}};
        if (!player) {
            fields.push_back({uf::UNIT_DYNAMIC_FLAGS, i % 7 == 0 ? 1u : 0u});
            fields.push_back({uf::UNIT_NPC_FLAGS, i % 11 == 0 ? 2u : 0u});
        }
        h.apply(created(guid, player ? game::ObjectType::Player : game::ObjectType::Unit, fields));
    }
    game::Tutorials& t = h.play.tutorials();
    constexpr u32 kFrames = 6000; // 100 s at 60 fps: the 250 ms and 500 ms scans run hundreds of times
    const auto start = std::chrono::steady_clock::now();
    f64 worst = 0.0;
    for (u32 i = 0; i < kFrames; ++i) {
        h.now += 16;
        const auto a = std::chrono::steady_clock::now();
        t.update(h.play, h.now);
        worst = std::max(worst, std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - a).count());
    }
    const f64 total = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("Tutorials::update with 160 units: " << total * 1000.0 / kFrames << " us per frame, worst " << worst
                                                 << " ms");
    CHECK(total / kFrames < 0.5); // far below: a sanity bound, not the budget
}
