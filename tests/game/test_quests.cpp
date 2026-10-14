// Phase 13 (quests): the client side of the quest log (game/QuestJournal: zone headers, collapsing,
// selection, QuestWatchFrame's watches with AUTO_QUEST_WATCH, timers), the game objects' animations
// (game/GameObjectAnim) and the flows through game::Gameplay with a capturing packet sink: sharing a quest
// and its answers, declining a shared quest, QUEST_WATCH_UPDATE, the objects' animation packets, fishing and
// readable objects. Server packets are written as vmangos-core writes them; client packets are checked byte
// for byte.
#include "game/Entities.h"
#include "game/GameObjectAnim.h"
#include "game/Gameplay.h"
#include "game/QuestData.h"
#include "game/QuestJournal.h"
#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/QuestMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/ObjectCollision.h"
#include "pack/PackReader.h"
#include "physics/CollisionWorld.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using game::GameEventKind;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x11;
constexpr u64 kAeryn = 0x12; // a party member
constexpr u64 kBobber = 0xF110000000004321ull;
constexpr u64 kPlaque = 0xF110000000004322ull;
constexpr u64 kBook = 0xF110000000004323ull;

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    const u32 words = fields.back().first / 32u + 1u;
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

// The quest log fields of `slot` (Player::SetQuestSlot / Counter / State / Timer).
void questSlot(Fields& f, u32 slot, u32 questId, u32 counters = 0, u32 timer = 0) {
    const u16 base = static_cast<u16>(uf::PLAYER_QUEST_LOG_1_1 + slot * 3);
    f.push_back({base, questId});
    f.push_back({static_cast<u16>(base + 1), counters});
    f.push_back({static_cast<u16>(base + 2), timer});
}

Fields playerFields() {
    return {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_BYTES_0, 1}, {uf::UNIT_FIELD_LEVEL, 12}};
}

// A template as QueryCache::onQuest gets it (SMSG_QUEST_QUERY_RESPONSE's fields).
net::QuestQueryResponse quest(u32 id, i32 zone, const char* title, u32 flags = 0, u32 creature = 0,
                              u32 kills = 0, u32 item = 0, u32 items = 0) {
    net::QuestQueryResponse q;
    q.questId = id;
    q.zoneOrSort = zone;
    q.title = title;
    q.flags = flags;
    q.level = 10;
    q.objectiveList[0].creatureOrGo = creature;
    q.objectiveList[0].creatureOrGoCount = kills;
    q.objectiveList[1].itemId = item;
    q.objectiveList[1].itemCount = items;
    return q;
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

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kPlayer, 0, true, now);
    }
    void apply(const Bytes& update) {
        std::vector<u64> createdGuids;
        game::UpdateStats stats;
        stats.createdGuids = &createdGuids;
        REQUIRE(store.applyUpdateObject(update, now, &stats));
        play.afterUpdate(createdGuids, now);
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
    std::vector<game::GameEvent> events(u32& cursor) const {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            out.push_back(*e);
        }
        return out;
    }
};

const game::GameEvent* findEvent(const std::vector<game::GameEvent>& events, GameEventKind kind) {
    for (const game::GameEvent& e : events) {
        if (e.kind == kind) {
            return &e;
        }
    }
    return nullptr;
}

Bytes groupList(u64 leader, u64 member, const char* name) {
    Bytes out;
    net::ByteWriter w(out);
    w.u8v(0).u8v(0).u32v(1);
    w.cstring(name).u64v(member).u8v(1).u8v(0);
    w.u64v(leader);
    w.u8v(3).u64v(0).u8v(2).u8v(0);
    return out;
}

// What a model has, for the animator.
struct Model {
    std::vector<u16> animations;
    static bool has(const void* context, u16 animation) {
        const Model* m = static_cast<const Model*>(context);
        return std::find(m->animations.begin(), m->animations.end(), animation) != m->animations.end();
    }
};

} // namespace

TEST_CASE(
    "QuestJournal: zone headers by name, quests by level and title, folding, the All button, selection") {
    game::QueryCache queries;
    net::QuestQueryResponse bounty = quest(11, 12, "Riverpaw Gnoll Bounty");
    bounty.level = 10;
    net::QuestQueryResponse candles = quest(60, 12, "Kobold Candles");
    candles.level = 10;
    net::QuestQueryResponse mine = quest(62, 12, "The Fargodeep Mine");
    mine.level = 7;
    queries.onQuest(bounty);
    queries.onQuest(candles);
    queries.onQuest(mine);
    queries.onQuest(quest(38, 40, "Westfall Stew"));
    queries.onQuest(quest(1665, -81, "Bartleby's Mug"));
    queries.onQuest(quest(999, 12, "Hidden", game::kQuestFlagAutoRewarded));
    game::QuestData names;
    names.set({{81, "Warrior"}}, {}, nullptr, {{12, "Elwynn Forest"}, {40, "Westfall"}});
    // Log slots: Elwynn, Westfall, Elwynn, a quest whose template has not come, the warrior quest, an
    // auto-rewarded one, Elwynn again.
    game::EntityStore store;
    Fields f = playerFields();
    questSlot(f, 0, 11);
    questSlot(f, 1, 38);
    questSlot(f, 2, 60);
    questSlot(f, 3, 77);
    questSlot(f, 4, 1665);
    questSlot(f, 5, 999);
    questSlot(f, 6, 62);
    REQUIRE(store.applyUpdateObject(created(kPlayer, game::ObjectType::Player, f), 0));
    game::QuestLog log;
    log.refresh(*store.find(kPlayer), [](GameEventKind, u32) {});

    game::QuestJournal journal;
    journal.refresh(log, queries, &names);
    const auto rows = journal.rows();
    REQUIRE(rows.size() == 8);
    // WoW.exe's order: the zones by name (Elwynn Forest, Warrior, Westfall), a zone's quests by level then
    // title; the quest without its template and the auto-rewarded one are not listed.
    CHECK(rows[0].header);
    CHECK(rows[0].zoneOrSort == 12);
    CHECK(rows[1].questId == 62); // level 7
    CHECK(rows[2].questId == 60); // level 10, "Kobold Candles"
    CHECK(rows[3].questId == 11); // level 10, "Riverpaw Gnoll Bounty"
    CHECK(rows[4].header);
    CHECK(rows[4].zoneOrSort == -81);
    CHECK(rows[5].questId == 1665);
    CHECK(rows[6].header);
    CHECK(rows[6].zoneOrSort == 40);
    CHECK(rows[7].questId == 38);
    CHECK(journal.headerCount() == 3);
    // The first quest is selected (QuestLog_SetFirstValidSelection).
    CHECK(journal.selected() == 62);

    // Folding Elwynn hides its quests and moves the selection to the first quest still listed.
    journal.collapse(12);
    journal.refresh(log, queries, &names);
    REQUIRE(journal.rows().size() == 5);
    CHECK(journal.rows()[0].collapsed);
    CHECK(journal.rows()[1].zoneOrSort == -81);
    CHECK(journal.selected() == 1665);
    CHECK_FALSE(journal.allCollapsed());
    // "All": everything folds (only the headers), then everything unfolds.
    journal.collapse(0);
    journal.refresh(log, queries, &names);
    CHECK(journal.allCollapsed());
    CHECK(journal.rows().size() == 3);
    journal.expand(0);
    journal.refresh(log, queries, &names);
    CHECK(journal.rows().size() == 8);
    journal.expand(12);
    CHECK_FALSE(journal.collapsed(12));
    // Without names the zones go by number (the sorts' negative ids first).
    journal.refresh(log, queries);
    CHECK(journal.rows()[0].zoneOrSort == -81);
}

TEST_CASE("QuestJournal: watches, AUTO_QUEST_WATCH's five minutes and quests that leave the log") {
    game::QueryCache queries;
    game::EntityStore store;
    Fields f = playerFields();
    for (u32 i = 0; i < 7; ++i) {
        queries.onQuest(quest(100 + i, 12, "Quest"));
        questSlot(f, i, 100 + i);
    }
    REQUIRE(store.applyUpdateObject(created(kPlayer, game::ObjectType::Player, f), 0));
    game::QuestLog log;
    log.refresh(*store.find(kPlayer), [](GameEventKind, u32) {});
    game::QuestJournal journal;
    journal.refresh(log, queries);

    // Shift + click: a quest without objectives cannot be watched; five at most.
    CHECK(journal.toggleWatch(100, 0) == game::QuestWatchToggle::NoObjectives);
    for (u32 i = 0; i < 5; ++i) {
        CHECK(journal.toggleWatch(100 + i, 1) == game::QuestWatchToggle::Added);
    }
    CHECK(journal.toggleWatch(105, 1) == game::QuestWatchToggle::TooMany);
    CHECK(journal.watched(104));
    CHECK(journal.toggleWatch(104, 1) == game::QuestWatchToggle::Removed);
    CHECK_FALSE(journal.watched(104));

    // AUTO_QUEST_WATCH: a quest that progresses is watched five minutes; again, five more from then, at the
    // end.
    journal.autoWatch(105, 10000);
    REQUIRE(journal.watches().size() == 5);
    CHECK(journal.watches()[4].questId == 105);
    CHECK(journal.watches()[4].timed);
    journal.autoWatch(100, 20000); // watched for good: stays so
    CHECK_FALSE(journal.watches()[0].timed);
    // Five watched: the timed one closest to running out gives its place.
    journal.autoWatch(106, 30000);
    CHECK(journal.watched(106));
    CHECK_FALSE(journal.watched(105));
    journal.update(30000 + game::kAutoWatchMs - 1);
    CHECK(journal.watched(106));
    journal.update(30000 + game::kAutoWatchMs);
    CHECK_FALSE(journal.watched(106));
    REQUIRE(journal.watches().size() == 4);
    // With the option off nothing is watched by itself.
    journal.setAutoWatch(false);
    journal.autoWatch(106, 40000);
    CHECK_FALSE(journal.watched(106));

    // A watched quest turned in or abandoned stops being watched.
    REQUIRE(store.applyUpdateObject(values(kPlayer, {{uf::PLAYER_QUEST_LOG_1_1, 0}}), 0));
    log.refresh(*store.find(kPlayer), [](GameEventKind, u32) {});
    journal.refresh(log, queries);
    CHECK_FALSE(journal.watched(100));
    CHECK(journal.watches().size() == 3);
}

TEST_CASE("Quest timers: the server's unix time in the log and the packed date") {
    game::QuestLogEntry e;
    e.questId = 1;
    e.timer = 1000600;
    CHECK(game::questTimeLeft(e, 1000000) == 600);
    CHECK(game::questTimeLeft(e, 1000600) == 0);
    e.timer = 1; // Player::FailQuest: the timer of a failed timed quest
    CHECK(game::questTimeLeft(e, 1000000) == 0);
    e.timer = 0;
    CHECK(game::questTimeLeft(e, 1000000) == 0);

    // secsToTimeBitFields of the local time now, back to the same minute.
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const u32 packed =
        static_cast<u32>((local.tm_year - 100) << 24 | local.tm_mon << 20 | (local.tm_mday - 1) << 14 |
                         local.tm_wday << 11 | local.tm_hour << 6 | local.tm_min);
    const u64 unix = game::packedTimeToUnix(packed);
    CHECK(unix <= static_cast<u64>(now));
    CHECK(static_cast<u64>(now) - unix < 60);
    CHECK(game::packedTimeToUnix(0xFFFFFFFFu) == 0);
}

TEST_CASE("GameObjectAnimator: state loops, transitions, custom, despawn, what the model lacks") {
    game::GameObjectAnimator a;
    // A door: Closed, Open, Opened, Close, Destroy, Destroyed (Deadmines' DeadMineDoor01.m2).
    Model door{{game::goanim::kOpen, game::goanim::kOpened, game::goanim::kClose, game::goanim::kClosed,
                game::goanim::kDestroy, game::goanim::kDestroyed}};
    a.appear(game::kGoStateReady);
    CHECK(a.next(&Model::has, &door, false) == game::GameObjectAnimRequest{game::goanim::kClosed, true});
    a.changeState(game::kGoStateActive);
    CHECK(a.next(&Model::has, &door, false) == game::GameObjectAnimRequest{game::goanim::kOpen, false});
    CHECK(a.next(&Model::has, &door, false) ==
          game::GameObjectAnimRequest{game::goanim::kOpen, false}); // playing
    CHECK(a.next(&Model::has, &door, true) == game::GameObjectAnimRequest{game::goanim::kOpened, true});
    a.changeState(game::kGoStateReady);
    CHECK(a.next(&Model::has, &door, false).animation == game::goanim::kClose);
    CHECK(a.next(&Model::has, &door, true) == game::GameObjectAnimRequest{game::goanim::kClosed, true});
    a.changeState(game::kGoStateActiveAlternative);
    CHECK(a.next(&Model::has, &door, false).animation == game::goanim::kDestroy);
    CHECK(a.next(&Model::has, &door, true) == game::GameObjectAnimRequest{game::goanim::kDestroyed, true});
    // Rebuilt without a Rebuild of its own: it closes.
    a.changeState(game::kGoStateReady);
    CHECK(a.next(&Model::has, &door, false).animation == game::goanim::kClose);
    // Appearing open: no transition.
    a.appear(game::kGoStateActive);
    CHECK(a.next(&Model::has, &door, false) == game::GameObjectAnimRequest{game::goanim::kOpened, true});

    // The fishing bobber: Stand and Custom0 only. The bite: its state turns active (nothing to play), then
    // Custom0.
    Model bobber{{game::goanim::kStand, game::goanim::kCustom0}};
    a.appear(game::kGoStateReady);
    CHECK(a.next(&Model::has, &bobber, false) == game::GameObjectAnimRequest{game::goanim::kStand, true});
    a.changeState(game::kGoStateActive);
    a.custom(0);
    CHECK(a.next(&Model::has, &bobber, false) == game::GameObjectAnimRequest{game::goanim::kCustom0, false});
    CHECK(a.next(&Model::has, &bobber, true) == game::GameObjectAnimRequest{game::goanim::kStand, true});
    a.custom(1); // a model without Custom1: nothing
    CHECK(a.next(&Model::has, &bobber, false) == game::GameObjectAnimRequest{game::goanim::kStand, true});
    a.custom(7); // not a custom animation
    CHECK(a.oneShot() == game::goanim::kNone);

    // Reset stops a one-shot; a despawn holds its end.
    Model trap{{game::goanim::kStand, game::goanim::kCustom0, game::goanim::kDespawn}};
    a.appear(game::kGoStateReady);
    a.custom(0);
    CHECK(a.next(&Model::has, &trap, false).animation == game::goanim::kCustom0);
    a.resetState();
    CHECK(a.next(&Model::has, &trap, false) == game::GameObjectAnimRequest{game::goanim::kStand, true});
    a.despawn();
    CHECK(a.despawning());
    CHECK(a.next(&Model::has, &trap, false) == game::GameObjectAnimRequest{game::goanim::kDespawn, false});
    CHECK(a.next(&Model::has, &trap, true) == game::GameObjectAnimRequest{game::goanim::kDespawn, false});

    // The bobber is used from where it landed (GetInteractionDistance: 100 yards), the rest from the usual reach.
    game::Entity bobberEntity;
    bobberEntity.type = game::ObjectType::GameObject;
    bobberEntity.values.assign(uf::GAMEOBJECT_END, 0);
    bobberEntity.values[uf::GAMEOBJECT_TYPE_ID] = game::kGameObjectFishingNode;
    CHECK(game::gameObjectUseYards(bobberEntity, 5.5f) == 100.0f);
    bobberEntity.values[uf::GAMEOBJECT_TYPE_ID] = 3; // a chest
    CHECK(game::gameObjectUseYards(bobberEntity, 5.5f) == 5.5f);

    // GameObjectDisplayInfo's sound slots of the 1.12 rows.
    CHECK(game::gameObjectSoundSlot(game::goanim::kOpen) == 1);
    CHECK(game::gameObjectSoundSlot(game::goanim::kClose) == 3);
    CHECK(game::gameObjectSoundSlot(game::goanim::kDestroy) == 4);
    CHECK(game::gameObjectSoundSlot(game::goanim::kCustom0) == 6);
    CHECK(game::gameObjectSoundSlot(game::goanim::kCustom0 + 3) == 9);
}

TEST_CASE("Gameplay: sharing a quest with the party, its answers, declining a shared quest") {
    Harness h;
    Fields f = playerFields();
    questSlot(f, 0, 176);
    questSlot(f, 1, 62);
    h.apply(created(kPlayer, game::ObjectType::Player, f));
    h.play.queries().onQuest(quest(176, 12, "Wanted: \"Hogger\"", game::kQuestFlagSharable));
    h.play.queries().onQuest(quest(62, 12, "The Fargodeep Mine"));
    u32 cursor = h.play.events().end();

    // Alone: nothing to share with (QuestFramePushQuestButton disabled).
    CHECK_FALSE(h.play.canShareQuest(176));
    CHECK_FALSE(h.play.shareQuest(176));
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kPlayer, kAeryn, "Aeryn")) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.play.canShareQuest(176));
    CHECK_FALSE(h.play.canShareQuest(62)); // not QUEST_FLAGS_SHARABLE
    CHECK_FALSE(h.play.canShareQuest(5));  // not in the log
    REQUIRE(h.play.shareQuest(176));
    REQUIRE(h.sentWith(Opcode::CMSG_PUSHQUESTTOPARTY).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_PUSHQUESTTOPARTY)[0] == Bytes{0xB0, 0x00, 0x00, 0x00});

    // HandlePushQuestToParty answers "sharing", then each member's outcome.
    Bytes sharing;
    net::ByteWriter(sharing).u64v(kAeryn).u8v(net::kQuestPushSharing);
    Bytes accepted;
    net::ByteWriter(accepted).u64v(kAeryn).u8v(net::kQuestPushAccepted);
    REQUIRE(h.packet(Opcode::MSG_QUEST_PUSH_RESULT, sharing) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::MSG_QUEST_PUSH_RESULT, accepted) == game::Gameplay::Handled::Yes);
    const std::vector<game::GameEvent> ev = h.events(cursor);
    std::vector<i32> results;
    for (const game::GameEvent& e : ev) {
        if (e.kind == GameEventKind::QuestPushResult) {
            CHECK(e.guid == kAeryn);
            results.push_back(e.value[0]);
        }
    }
    CHECK(results == std::vector<i32>{net::kQuestPushSharing, net::kQuestPushAccepted});
    CHECK(h.packet(Opcode::MSG_QUEST_PUSH_RESULT, Bytes{0x12, 0x00}) == game::Gameplay::Handled::Malformed);

    // A quest Aeryn shares: its details come from her (SendQuestGiverQuestDetails with her GUID). Closing the
    // window declines it: MSG_QUEST_PUSH_RESULT with her GUID and QUEST_PARTY_MSG_DECLINE_QUEST.
    Bytes details;
    net::ByteWriter(details)
        .u64v(kAeryn)
        .u32v(60)
        .cstring("Kobold Candles")
        .cstring("The kobolds carry candles.")
        .cstring("Bring 8 Large Candles.")
        .u32v(1)  // activateAccept
        .u32v(0)  // choices
        .u32v(0)  // rewards
        .u32v(0)  // money
        .u32v(0)  // spell
        .u32v(0); // emotes
    REQUIRE(h.packet(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS, details) == game::Gameplay::Handled::Yes);
    REQUIRE(h.play.dialog().kind() == game::DialogKind::QuestDetails);
    h.play.closeDialog();
    REQUIRE(h.sentWith(Opcode::MSG_QUEST_PUSH_RESULT).size() == 1);
    CHECK(h.sentWith(Opcode::MSG_QUEST_PUSH_RESULT)[0] ==
          Bytes{0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03});
    // An NPC's quest closed: nothing to tell.
    Bytes npcDetails = details;
    npcDetails[7] = 0xF1;
    npcDetails[6] = 0x30;
    REQUIRE(h.packet(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS, npcDetails) == game::Gameplay::Handled::Yes);
    h.play.closeDialog();
    CHECK(h.sentWith(Opcode::MSG_QUEST_PUSH_RESULT).size() == 1);
}

TEST_CASE("Gameplay: QUEST_WATCH_UPDATE from kills and items watches the quest (AUTO_QUEST_WATCH)") {
    Harness h;
    Fields f = playerFields();
    questSlot(f, 0, 11);
    questSlot(f, 1, 60);
    h.apply(created(kPlayer, game::ObjectType::Player, f));
    h.play.queries().onQuest(quest(11, 12, "Riverpaw Gnoll Bounty", 0, 478, 10));
    h.play.queries().onQuest(quest(60, 12, "Kobold Candles", 0, 0, 0, 772, 8));
    u32 cursor = h.play.events().end();

    // SMSG_QUESTUPDATE_ADD_KILL: quest, entry, count, required, the creature.
    Bytes kill;
    net::ByteWriter(kill).u32v(11).u32v(478).u32v(3).u32v(10).u64v(0xF130000000000478ull);
    REQUIRE(h.packet(Opcode::SMSG_QUESTUPDATE_ADD_KILL, kill) == game::Gameplay::Handled::Yes);
    const std::vector<game::GameEvent> ev = h.events(cursor);
    REQUIRE(findEvent(ev, GameEventKind::QuestWatchUpdate));
    CHECK(findEvent(ev, GameEventKind::QuestWatchUpdate)->id == 11);
    REQUIRE(h.play.questJournal().watches().size() == 1);
    CHECK(h.play.questJournal().watches()[0].questId == 11);
    CHECK(h.play.questJournal().watches()[0].timed);

    // SMSG_QUESTUPDATE_ADD_ITEM: the item, the count added: every quest asking for it.
    Bytes item;
    net::ByteWriter(item).u32v(772).u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_QUESTUPDATE_ADD_ITEM, item) == game::Gameplay::Handled::Yes);
    CHECK(h.play.questJournal().watched(60));
    // Five minutes later they are gone.
    h.play.update(h.now + game::kAutoWatchMs + 1);
    CHECK(h.play.questJournal().watches().empty());
}

TEST_CASE("Gameplay: SMSG_QUESTUPDATE_ADD_ITEM counts the bags plus what was added (WoW.exe 0x5DD060)") {
    Harness h;
    Fields f = playerFields();
    questSlot(f, 0, 60);
    questSlot(f, 1, 61);
    h.apply(created(kPlayer, game::ObjectType::Player, f));
    h.play.queries().onQuest(quest(60, 12, "Kobold Candles", 0, 0, 0, 772, 8));
    h.play.queries().onQuest(quest(61, 12, "More Candles", 0, 0, 0, 772, 3)); // the first quest counts
    auto addItem = [&](u32 added) {
        u32 cursor = h.play.events().end();
        Bytes item;
        net::ByteWriter(item).u32v(772).u32v(added);
        REQUIRE(h.packet(Opcode::SMSG_QUESTUPDATE_ADD_ITEM, item) == game::Gameplay::Handled::Yes);
        const std::vector<game::GameEvent> ev = h.events(cursor);
        const game::GameEvent* e = findEvent(ev, GameEventKind::QuestItemProgress);
        REQUIRE(e);
        CHECK(e->id == 772);
        CHECK(e->value[0] == static_cast<i32>(added));
        return *e;
    };

    // vMaNGOS sends it before the item (ItemAddedQuestCheck, then StoreItem): the first candle is 1/8, not 0/8.
    game::GameEvent e = addItem(1);
    CHECK(e.value[1] == 1);
    CHECK(e.value[2] == 8);

    // Then the item arrives: 5 in the bags, 2 more looted: 7/8.
    constexpr u64 kCandles = 0x4000000000000772ull;
    h.apply(created(kCandles, game::ObjectType::Item, {{uf::OBJECT_FIELD_ENTRY, 772}, {uf::ITEM_FIELD_STACK_COUNT, 5}}));
    h.apply(values(kPlayer, {{uf::PLAYER_FIELD_PACK_SLOT_1, static_cast<u32>(kCandles)},
                             {uf::PLAYER_FIELD_PACK_SLOT_1 + 1, static_cast<u32>(kCandles >> 32)}}));
    e = addItem(2);
    CHECK(e.value[1] == 7);
    CHECK(e.value[2] == 8);
    // Never past the need.
    e = addItem(5);
    CHECK(e.value[1] == 8);
    CHECK(e.value[2] == 8);
    // The bags already hold enough: no line.
    h.apply(values(kCandles, {{uf::ITEM_FIELD_STACK_COUNT, 8}}));
    CHECK(addItem(1).value[2] == 0);
}

TEST_CASE("Gameplay: living objects' animations, fishing, plaques and books") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    u32 cursor = h.play.events().end();
    u32 fxCursor = h.play.fx().end();

    // SMSG_GAMEOBJECT_CUSTOM_ANIM / SPAWN_ANIM / DESPAWN_ANIM / RESET_STATE go to the fx ring.
    Bytes bite;
    net::ByteWriter(bite).u64v(kBobber).u32v(0);
    Bytes guid;
    net::writeGuid(guid, kBobber);
    REQUIRE(h.packet(Opcode::SMSG_GAMEOBJECT_CUSTOM_ANIM, bite) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_GAMEOBJECT_SPAWN_ANIM, guid) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_GAMEOBJECT_DESPAWN_ANIM, guid) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_GAMEOBJECT_RESET_STATE, guid) == game::Gameplay::Handled::Yes);
    CHECK(h.packet(Opcode::SMSG_GAMEOBJECT_CUSTOM_ANIM, Bytes{1, 2, 3}) ==
          game::Gameplay::Handled::Malformed);
    std::vector<game::FxEventKind> kinds;
    while (const game::FxEvent* e = h.play.fx().next(fxCursor)) {
        CHECK(e->guid == kBobber);
        kinds.push_back(e->kind);
    }
    CHECK(kinds == std::vector<game::FxEventKind>{
                       game::FxEventKind::ObjectCustomAnim, game::FxEventKind::ObjectSpawnAnim,
                       game::FxEventKind::ObjectDespawnAnim, game::FxEventKind::ObjectResetState});

    // Fishing: "No fish are hooked." / "Your fish got away!".
    REQUIRE(h.packet(Opcode::SMSG_FISH_NOT_HOOKED, {}) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_FISH_ESCAPED, {}) == game::Gameplay::Handled::Yes);
    std::vector<game::GameEvent> ev = h.events(cursor);
    CHECK(findEvent(ev, GameEventKind::FishNotHooked));
    CHECK(findEvent(ev, GameEventKind::FishEscaped));

    // A plaque (GAMEOBJECT_TYPE_TEXT, entry 1000): tapping it reads its page; no CMSG_GAMEOBJ_USE.
    h.apply(created(kPlaque, game::ObjectType::GameObject,
                    {{uf::OBJECT_FIELD_ENTRY, 1000}, {uf::GAMEOBJECT_TYPE_ID, game::kGameObjectText}}));
    CHECK(h.play.interact(kPlaque) == game::Interaction::None); // its template is asked for first
    net::GameObjectQueryInfo plaque;
    plaque.entry = 1000;
    plaque.found = true;
    plaque.type = game::kGameObjectText;
    plaque.name = "Plaque of the Fallen";
    plaque.data[0] = 2010; // page
    plaque.data[2] = 3;    // PageTextMaterial
    h.play.queries().onGameObject(plaque);
    CHECK(h.play.interact(kPlaque) == game::Interaction::ReadObject);
    CHECK(h.sentWith(Opcode::CMSG_GAMEOBJ_USE).empty());
    const game::ReadingWindow& r = h.play.inventory().reading();
    REQUIRE(r.open());
    CHECK(r.item == kPlaque);
    CHECK(r.title == "Plaque of the Fallen");
    CHECK(r.material == 3);
    CHECK(r.waiting);
    REQUIRE(h.sentWith(Opcode::CMSG_PAGE_TEXT_QUERY).size() == 1);
    Bytes query;
    net::ByteWriter(query).u32v(2010).u64v(kPlaque);
    CHECK(h.sentWith(Opcode::CMSG_PAGE_TEXT_QUERY)[0] == query);
    // The server answers the chain: two pages.
    Bytes page1, page2;
    net::ByteWriter(page1).u32v(2010).cstring("Here lie the heroes.").u32v(2011);
    net::ByteWriter(page2).u32v(2011).cstring("May they rest.").u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE, page1) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE, page2) == game::Gameplay::Handled::Yes);
    REQUIRE(h.play.inventory().reading().pageCount == 2);
    CHECK(h.play.inventory().reading().pages[1] == "May they rest.");
    CHECK_FALSE(h.play.inventory().reading().waiting);
    h.play.inventory().closeReading();

    // A book on a lectern (GAMEOBJECT_TYPE_GOOBER with a page): used with CMSG_GAMEOBJ_USE, the server
    // answers SMSG_GAMEOBJECT_PAGETEXT and the client reads data7's page (known now: no query).
    h.apply(created(kBook, game::ObjectType::GameObject,
                    {{uf::OBJECT_FIELD_ENTRY, 1001}, {uf::GAMEOBJECT_TYPE_ID, game::kGameObjectGoober}}));
    net::GameObjectQueryInfo book;
    book.entry = 1001;
    book.found = true;
    book.type = game::kGameObjectGoober;
    book.name = "Old Tome";
    book.data[7] = 2010;
    h.play.queries().onGameObject(book);
    CHECK(h.play.interact(kBook) == game::Interaction::UseObject);
    CHECK(h.sentWith(Opcode::CMSG_GAMEOBJ_USE).size() == 1);
    Bytes bookGuid;
    net::writeGuid(bookGuid, kBook);
    REQUIRE(h.packet(Opcode::SMSG_GAMEOBJECT_PAGETEXT, bookGuid) == game::Gameplay::Handled::Yes);
    REQUIRE(h.play.inventory().reading().open());
    CHECK(h.play.inventory().reading().title == "Old Tome");
    CHECK(h.play.inventory().reading().pageCount == 2);
    CHECK(h.sentWith(Opcode::CMSG_PAGE_TEXT_QUERY).size() == 1);
}

TEST_CASE("Doors: the Deadmines door's baked collision blocks where it stands, and lets pass once removed") {
    // app/ServerObjects adds it while the door is closed (GO_STATE_READY) and removes it when it opens.
    const char* packs = std::getenv("VD_PACKS_DIR");
    pack::PackReader reader;
    if (!packs || !reader.open(std::filesystem::path(packs) / pack::kObjectCollisionPack)) {
        MESSAGE("VD_PACKS_DIR has no collision_objects.vdpk: skipped");
        return;
    }
    // GameObjectDisplayInfo 394: WORLD\GENERIC\HUMAN\ACTIVEDOODADS\DOORS\DEADMINEDOOR01.MDX.
    const std::optional<pack::PackAsset> asset = reader.find(assetPathHash(
        pack::objectCollisionPath("WORLD\\GENERIC\\HUMAN\\ACTIVEDOODADS\\DOORS\\DEADMINEDOOR01.MDX")));
    REQUIRE(asset);
    std::vector<u8> blob(asset->rawSize);
    REQUIRE(pack::PackReader::read(*asset, blob.data(), blob.size()));
    physics::CollisionWorld world;
    const u32 door = world.addObject(std::move(blob));
    REQUIRE(door != physics::CollisionWorld::kNoObject);
    world.placeObject(door, glm::dvec3(-11000.0, 1500.0, 40.0), 1.0f);
    // Vertical rays over the door's place: some land on its top edge, above the floor it stands on.
    u32 hits = 0;
    for (f64 dx = -6.0; dx <= 6.0; dx += 0.25) {
        for (f64 dy = -6.0; dy <= 6.0; dy += 0.25) {
            const std::optional<f64> top = world.groundHeight(-11000.0 + dx, 1500.0 + dy, 80.0, 0.0);
            hits += top && *top > 41.0 ? 1 : 0;
        }
    }
    CHECK(hits > 0);
    world.removeObject(door);
    CHECK_FALSE(world.groundHeight(-11000.0, 1500.0, 80.0, 0.0));
}
