// Gameplay state (phase 7): the flows the HUD drives, end to end through game::Gameplay with a capturing
// packet sink: quest giver dialogs, the quest log, loot, cooldowns and the global cooldown, cast bars, auras,
// combat events, chat and death. Server packets are written as vmangos-core writes them; client packets are
// checked byte for byte.
#include "core/Hash.h"
#include "game/CharacterNames.h"
#include "game/Chat.h"
#include "game/Entities.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
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
using game::GameEventKind;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x11;
constexpr u64 kOtherPlayer = 0x12;
constexpr u64 kNpc = 0xF130000000000123ull;
constexpr u64 kWolf = 0xF130000000000456ull;
constexpr u64 kChest = 0xF110000000000777ull;
constexpr u64 kItem = 0x4000000000000888ull;
constexpr u64 kCorpse = 0xF101000000000999ull;

// Values block (Object::BuildValuesUpdate): mask words, mask, then the values in index order.
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

// SMSG_UPDATE_OBJECT with one create block (UPDATEFLAG_HAS_POSITION).
Bytes created(u64 guid, game::ObjectType type, const Fields& fields, f32 x = 0.0f, f32 y = 0.0f,
              f32 z = 0.0f) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(x).f32v(y).f32v(z).f32v(0.0f);
    writeValues(w, fields);
    return out;
}

// SMSG_UPDATE_OBJECT with one values block.
Bytes values(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

Bytes guidBody(u64 guid) {
    Bytes out;
    net::writeGuid(out, guid);
    return out;
}

Bytes guidQuest(u64 guid, u32 questId) {
    Bytes out;
    net::writeQuestGiverQuest(out, guid, questId);
    return out;
}

Bytes u32Body(u32 value) {
    Bytes out;
    net::writeU32(out, value);
    return out;
}

u32 low(u64 guid) {
    return static_cast<u32>(guid);
}
u32 high(u64 guid) {
    return static_cast<u32>(guid >> 32);
}

Fields playerFields(Fields extra = {}) {
    Fields f = {{uf::UNIT_FIELD_HEALTH, 100},
                {uf::UNIT_FIELD_BYTES_0, 1 /* human */},
                {uf::UNIT_FIELD_FACTIONTEMPLATE, 1},
                {uf::UNIT_FIELD_LEVEL, 3}};
    f.insert(f.end(), extra.begin(), extra.end());
    return f;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

// Gameplay over its own entity store; what it sends is captured.
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    std::vector<Sent> sent;
    std::vector<u64> createdGuids;
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
        createdGuids.clear();
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
    // Events since `cursor` (advanced).
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

// A table of spells for the timing rules.
struct SpellTable {
    std::vector<game::SpellInfo> spells;
    game::SpellSource source() const {
        game::SpellSource s;
        s.context = this;
        s.lookup = [](const void* context, u32 id, game::SpellInfo& out) {
            for (const game::SpellInfo& info : static_cast<const SpellTable*>(context)->spells) {
                if (info.id == id) {
                    out = info;
                    return true;
                }
            }
            return false;
        };
        return s;
    }
};

game::SpellInfo spell(u32 id, u32 recoveryMs, u32 gcdMs, u32 category = 0, u32 categoryMs = 0,
                      u32 attributes = 0) {
    game::SpellInfo info;
    info.id = id;
    info.recoveryMs = recoveryMs;
    info.gcdCategory = gcdMs ? 133 : 0;
    info.gcdMs = gcdMs;
    info.category = category;
    info.categoryRecoveryMs = categoryMs;
    info.attributes = attributes;
    return info;
}

// Server packets used by several tests.
Bytes gossipWithQuest(u64 npc, u32 questId, u32 icon, const char* title) {
    Bytes out;
    net::ByteWriter(out)
        .u64v(npc)
        .u32v(2)
        .u32v(1)
        .u32v(0)
        .u8v(0)
        .u8v(0)
        .cstring("Tell me more.")
        .u32v(1)
        .u32v(questId)
        .u32v(icon)
        .u32v(2)
        .cstring(title);
    return out;
}

Bytes questQueryResponse(u32 questId, const char* title, u32 creature, u32 kills, u32 item, u32 items) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(questId)
        .u32v(2)
        .u32v(2)
        .u32v(12)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(40)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0);
    for (u32 i = 0; i < 4 + 6; ++i) {
        w.u32v(0).u32v(0);
    }
    w.u32v(0).f32v(0.0f).f32v(0.0f).u32v(0);
    w.cstring(title).cstring("Objectives").cstring("Details").cstring("");
    w.u32v(creature).u32v(kills).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(item).u32v(items);
    for (u32 i = 0; i < 2; ++i) {
        w.u32v(0).u32v(0).u32v(0).u32v(0);
    }
    w.cstring("").cstring("").cstring("").cstring("");
    return out;
}

} // namespace

TEST_CASE("Gameplay: a quest from the gossip menu to the reward") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    h.apply(created(kNpc, game::ObjectType::Unit,
                    {{uf::OBJECT_FIELD_ENTRY, 197},
                     {uf::UNIT_FIELD_HEALTH, 50},
                     {uf::UNIT_NPC_FLAGS, game::kNpcFlagGossip | game::kNpcFlagQuestGiver}},
                    2.0f));
    // A quest giver in sight: its "!" is asked for at once.
    REQUIRE(h.sentWith(Opcode::CMSG_QUESTGIVER_STATUS_QUERY) == std::vector<Bytes>{guidBody(kNpc)});
    Bytes status;
    net::ByteWriter(status).u64v(kNpc).u32v(5);
    CHECK(h.packet(Opcode::SMSG_QUESTGIVER_STATUS, status) == game::Gameplay::Handled::Yes);
    CHECK(h.play.questGiverStatus(kNpc) == net::QuestGiverStatus::Available);
    CHECK(h.play.questGiverStatus(kWolf) == net::QuestGiverStatus::None);

    // Talking: it becomes the target, and a gossip NPC gets CMSG_GOSSIP_HELLO.
    h.sent.clear();
    CHECK(h.play.interact(kNpc) == game::Interaction::Gossip);
    CHECK(h.sentWith(Opcode::CMSG_SET_SELECTION) == std::vector<Bytes>{guidBody(kNpc)});
    CHECK(h.sentWith(Opcode::CMSG_GOSSIP_HELLO) == std::vector<Bytes>{guidBody(kNpc)});
    CHECK(h.play.target() == kNpc);

    REQUIRE(h.packet(Opcode::SMSG_GOSSIP_MESSAGE, gossipWithQuest(kNpc, 783, 5, "A Threat Within")) ==
            game::Gameplay::Handled::Yes);
    const game::NpcDialog& dialog = h.play.dialog();
    CHECK(dialog.kind() == game::DialogKind::Gossip);
    CHECK(dialog.npc() == kNpc);
    REQUIRE(dialog.options().size() == 1);
    CHECK(dialog.options()[0].text == "Tell me more.");
    REQUIRE(dialog.quests().size() == 1);
    CHECK(dialog.quests()[0].title == "A Threat Within");
    CHECK(dialog.quests()[0].icon == net::QuestGiverStatus::Available);
    // Its text is asked for (text id + the NPC), then cached.
    Bytes textQuery;
    net::writeNpcTextQuery(textQuery, 2, kNpc);
    CHECK(h.sentWith(Opcode::CMSG_NPC_TEXT_QUERY) == std::vector<Bytes>{textQuery});
    Bytes text;
    net::ByteWriter t(text);
    t.u32v(2);
    for (u32 i = 0; i < 8; ++i) {
        t.f32v(i == 0 ? 1.0f : 0.0f)
            .cstring(i == 0 ? "Hello, $N." : "")
            .cstring(i == 0 ? "Hello, lady $N." : "");
        t.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    }
    REQUIRE(h.packet(Opcode::SMSG_NPC_TEXT_UPDATE, text) == game::Gameplay::Handled::Yes);
    const game::NpcText* greeting = h.play.npcText(2);
    REQUIRE(greeting != nullptr);
    CHECK(greeting->text(0) == "Hello, $N.");
    CHECK(greeting->text(1) == "Hello, lady $N.");

    // A new quest: its details, accepted.
    h.sent.clear();
    h.play.selectQuest(783);
    CHECK(h.sentWith(Opcode::CMSG_QUESTGIVER_QUERY_QUEST) == std::vector<Bytes>{guidQuest(kNpc, 783)});
    Bytes details;
    net::ByteWriter d(details);
    d.u64v(kNpc)
        .u32v(783)
        .cstring("A Threat Within")
        .cstring("We need you, $N.")
        .cstring("Speak with Marshal")
        .u32v(1);
    d.u32v(0).u32v(1).u32v(2080).u32v(1).u32v(9000).u32v(35).u32v(0).u32v(4);
    for (u32 i = 0; i < 4; ++i) {
        d.u32v(0).u32v(0);
    }
    REQUIRE(h.packet(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS, details) == game::Gameplay::Handled::Yes);
    CHECK(dialog.kind() == game::DialogKind::QuestDetails);
    CHECK(dialog.quest().questId == 783);
    CHECK(dialog.quest().text == "We need you, $N.");
    CHECK(dialog.quest().rewardCount == 1);
    CHECK(dialog.quest().money == 35);
    Bytes itemQuery;
    net::writeEntryQuery(itemQuery, 2080, 0);
    CHECK(h.sentWith(Opcode::CMSG_ITEM_QUERY_SINGLE) == std::vector<Bytes>{itemQuery}); // the reward's name
    h.play.acceptQuest();
    CHECK(h.sentWith(Opcode::CMSG_QUESTGIVER_ACCEPT_QUEST) == std::vector<Bytes>{guidQuest(kNpc, 783)});
    REQUIRE(h.packet(Opcode::SMSG_GOSSIP_COMPLETE, {}) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(dialog.open());

    // The server puts it in the log: an event, its template asked for, the quest givers asked again.
    u32 cursor = h.play.events().end();
    h.sent.clear();
    h.now += 50;
    h.apply(values(kPlayer, {{uf::PLAYER_QUEST_LOG_1_1, 783}}));
    std::vector<game::GameEvent> events = h.events(cursor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GameEventKind::QuestAccepted);
    CHECK(events[0].id == 783);
    CHECK(h.play.questLog().count() == 1);
    CHECK(h.play.questLog().slotOf(783) == 0);
    CHECK(h.sentWith(Opcode::CMSG_QUEST_QUERY) == std::vector<Bytes>{u32Body(783)});
    h.sent.clear();
    h.now += 400;
    h.play.update(h.now);
    CHECK(h.sentWith(Opcode::CMSG_QUESTGIVER_STATUS_QUERY) == std::vector<Bytes>{guidBody(kNpc)});

    // Its template: kill 10 of creature 6, bring 8 of item 750.
    REQUIRE(h.packet(Opcode::SMSG_QUEST_QUERY_RESPONSE,
                     questQueryResponse(783, "A Threat Within", 6, 10, 750, 8)) ==
            game::Gameplay::Handled::Yes);
    const game::QuestInfo* quest = h.play.quest(783);
    REQUIRE(quest != nullptr);
    CHECK(quest->title == "A Threat Within");

    // Progress: SMSG_QUESTUPDATE_ADD_KILL is the event, the log field the count.
    Bytes addKill;
    net::ByteWriter(addKill).u32v(783).u32v(6).u32v(3).u32v(10).u64v(kWolf);
    REQUIRE(h.packet(Opcode::SMSG_QUESTUPDATE_ADD_KILL, addKill) == game::Gameplay::Handled::Yes);
    events = h.events(cursor);
    REQUIRE(events.size() == 2); // phase 13: and QUEST_WATCH_UPDATE (AUTO_QUEST_WATCH)
    CHECK(events[0].kind == GameEventKind::QuestObjectiveProgress);
    CHECK(events[0].id == 783);
    CHECK(events[0].value == std::array<i32, 3>{6, 3, 10});
    CHECK(events[1].kind == GameEventKind::QuestWatchUpdate);
    h.apply(values(kPlayer, {{uf::PLAYER_QUEST_LOG_1_1 + 1, 3}})); // counter 0 = 3
    std::array<game::QuestObjectiveView, 8> objectives;
    REQUIRE(h.play.questObjectives(783, objectives) == 2);
    CHECK(objectives[0].kind == game::QuestObjectiveView::Kind::Creature);
    CHECK(objectives[0].entry == 6);
    CHECK(objectives[0].have == 3);
    CHECK(objectives[0].need == 10);
    CHECK(objectives[0].name.empty()); // asked for
    CHECK(objectives[1].kind == game::QuestObjectiveView::Kind::Item);
    CHECK(objectives[1].have == 0);
    Bytes creatureName;
    net::ByteWriter(creatureName)
        .u32v(6)
        .cstring("Kobold Vermin")
        .cstring("")
        .cstring("")
        .cstring("")
        .cstring("")
        .u32v(0)
        .u32v(7)
        .u32v(0)
        .u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_CREATURE_QUERY_RESPONSE, creatureName) == game::Gameplay::Handled::Yes);
    // Items are counted in the bags: 8 of item 750 in the first backpack slot.
    h.apply(created(kItem, game::ObjectType::Item,
                    {{uf::OBJECT_FIELD_ENTRY, 750}, {uf::ITEM_FIELD_STACK_COUNT, 8}}));
    h.apply(values(kPlayer, {{uf::PLAYER_FIELD_PACK_SLOT_1, low(kItem)},
                             {uf::PLAYER_FIELD_PACK_SLOT_1 + 1, high(kItem)}}));
    REQUIRE(h.play.questObjectives(783, objectives) == 2);
    CHECK(objectives[0].name == "Kobold Vermin");
    CHECK(objectives[1].have == 8);
    CHECK(objectives[1].done());
    CHECK(game::inventoryCount(h.store, *h.store.find(kPlayer), 750) == 8);

    // Every objective met: the complete bit.
    h.apply(values(kPlayer, {{uf::PLAYER_QUEST_LOG_1_1 + 1, 10u | (u32{game::kQuestStateComplete} << 24)}}));
    events = h.events(cursor);
    REQUIRE(findEvent(events, GameEventKind::QuestObjectivesDone) != nullptr);
    CHECK(h.play.questLog().find(783)->complete());
    CHECK(h.play.questLog().find(783)->counters[0] == 10);

    // Turning it in: "?" in the menu -> request items -> offer reward -> choose -> complete.
    REQUIRE(h.packet(Opcode::SMSG_GOSSIP_MESSAGE, gossipWithQuest(kNpc, 783, 4, "A Threat Within")) ==
            game::Gameplay::Handled::Yes);
    h.sent.clear();
    h.play.selectQuest(783);
    CHECK(h.sentWith(Opcode::CMSG_QUESTGIVER_COMPLETE_QUEST) == std::vector<Bytes>{guidQuest(kNpc, 783)});
    Bytes request;
    net::ByteWriter(request)
        .u64v(kNpc)
        .u32v(783)
        .cstring("A Threat Within")
        .cstring("Well?")
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(1)
        .u32v(750)
        .u32v(8)
        .u32v(7300)
        .u32v(2)
        .u32v(3)
        .u32v(4)
        .u32v(8);
    REQUIRE(h.packet(Opcode::SMSG_QUESTGIVER_REQUEST_ITEMS, request) == game::Gameplay::Handled::Yes);
    CHECK(dialog.kind() == game::DialogKind::QuestRequestItems);
    CHECK(dialog.quest().completable);
    REQUIRE(dialog.quest().requiredCount == 1);
    CHECK(dialog.quest().required[0].itemId == 750);
    h.play.completeQuest();
    CHECK(h.sentWith(Opcode::CMSG_QUESTGIVER_REQUEST_REWARD) == std::vector<Bytes>{guidQuest(kNpc, 783)});
    Bytes offer;
    net::ByteWriter(offer)
        .u64v(kNpc)
        .u32v(783)
        .cstring("A Threat Within")
        .cstring("Thank you.")
        .u32v(1)
        .u32v(0)
        .u32v(2)
        .u32v(2080)
        .u32v(1)
        .u32v(9000)
        .u32v(2081)
        .u32v(1)
        .u32v(9001)
        .u32v(0)
        .u32v(40)
        .u32v(0)
        .u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_QUESTGIVER_OFFER_REWARD, offer) == game::Gameplay::Handled::Yes);
    CHECK(dialog.kind() == game::DialogKind::QuestOfferReward);
    CHECK(dialog.quest().choiceCount == 2);
    h.play.chooseQuestReward(1);
    Bytes choose;
    net::writeQuestGiverChooseReward(choose, kNpc, 783, 1);
    CHECK(h.sentWith(Opcode::CMSG_QUESTGIVER_CHOOSE_REWARD) == std::vector<Bytes>{choose});
    Bytes complete;
    net::ByteWriter(complete).u32v(783).u32v(3).u32v(250).u32v(40).u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_QUESTGIVER_QUEST_COMPLETE, complete) == game::Gameplay::Handled::Yes);
    events = h.events(cursor);
    const game::GameEvent* rewarded = findEvent(events, GameEventKind::QuestRewarded);
    REQUIRE(rewarded != nullptr);
    CHECK(rewarded->id == 783);
    CHECK(rewarded->value[0] == 250);
    CHECK(rewarded->value[1] == 40);
    CHECK_FALSE(dialog.open());
    h.apply(values(kPlayer, {{uf::PLAYER_QUEST_LOG_1_1, 0}, {uf::PLAYER_QUEST_LOG_1_1 + 1, 0}}));
    events = h.events(cursor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GameEventKind::QuestRemoved);
    CHECK(h.play.questLog().count() == 0);
}

TEST_CASE("Gameplay: quest log decoding, abandon, packet and field events not doubled") {
    Harness h;
    // Slot 0: quest 33 with counters 1, 2, 3, 63 and no state; slot 19: quest 7 failed, timed.
    const u32 counters = 1u | (2u << 6) | (3u << 12) | (63u << 18);
    h.apply(created(kPlayer, game::ObjectType::Player,
                    playerFields({{uf::PLAYER_QUEST_LOG_1_1, 33},
                                  {uf::PLAYER_QUEST_LOG_1_1 + 1, counters},
                                  {uf::PLAYER_QUEST_LOG_1_1 + 19 * 3, 7},
                                  {uf::PLAYER_QUEST_LOG_1_1 + 19 * 3 + 1, u32{game::kQuestStateFailed} << 24},
                                  {uf::PLAYER_QUEST_LOG_1_1 + 19 * 3 + 2, 1234567}})));
    const game::QuestLog& log = h.play.questLog();
    CHECK(log.count() == 2);
    CHECK(log.slots()[0].counters == std::array<u8, 4>{1, 2, 3, 63});
    CHECK_FALSE(log.slots()[0].complete());
    CHECK(log.slots()[19].questId == 7);
    CHECK(log.slots()[19].failed());
    CHECK(log.slots()[19].timer == 1234567);
    CHECK(log.slotOf(7) == 19);
    CHECK(log.slotOf(99) == -1);

    // Abandon: the slot index.
    h.sent.clear();
    h.play.abandonQuest(7);
    CHECK(h.sentWith(Opcode::CMSG_QUESTLOG_REMOVE_QUEST) == std::vector<Bytes>{Bytes{19}});
    h.play.abandonQuest(99); // not in the log: nothing
    CHECK(h.sentWith(Opcode::CMSG_QUESTLOG_REMOVE_QUEST).size() == 1);

    // Exploration: SMSG_QUESTUPDATE_COMPLETE, then the state bit: one event.
    u32 cursor = h.play.events().end();
    REQUIRE(h.packet(Opcode::SMSG_QUESTUPDATE_COMPLETE, u32Body(33)) == game::Gameplay::Handled::Yes);
    h.apply(
        values(kPlayer, {{uf::PLAYER_QUEST_LOG_1_1 + 1, counters | (u32{game::kQuestStateComplete} << 24)}}));
    const std::vector<game::GameEvent> events = h.events(cursor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GameEventKind::QuestObjectivesDone);
    CHECK(events[0].id == 33);

    // Swapping slots is not an accept or a removal.
    h.apply(values(kPlayer, {{uf::PLAYER_QUEST_LOG_1_1, 7},
                             {uf::PLAYER_QUEST_LOG_1_1 + 1, 0},
                             {uf::PLAYER_QUEST_LOG_1_1 + 19 * 3, 33},
                             {uf::PLAYER_QUEST_LOG_1_1 + 19 * 3 + 1, u32{game::kQuestStateComplete} << 24}}));
    CHECK(h.events(cursor).empty());
    CHECK(log.slotOf(33) == 19);
}

TEST_CASE("Gameplay: loot a corpse, take everything, money and items as events") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields({{uf::PLAYER_FIELD_COINAGE, 100}})));
    h.apply(
        created(kWolf, game::ObjectType::Unit,
                {{uf::OBJECT_FIELD_ENTRY, 299}, {uf::UNIT_FIELD_HEALTH, 0}, {uf::UNIT_DYNAMIC_FLAGS, 0x01}}));
    const game::Entity* wolf = h.store.find(kWolf);
    REQUIRE(wolf != nullptr);
    CHECK(game::isDead(*wolf));
    CHECK(game::isLootable(*wolf));
    h.sent.clear();
    CHECK(h.play.interact(kWolf) == game::Interaction::Loot);
    CHECK(h.sentWith(Opcode::CMSG_LOOT) == std::vector<Bytes>{guidBody(kWolf)});

    Bytes response;
    net::ByteWriter(response)
        .u64v(kWolf)
        .u8v(1)
        .u32v(23)
        .u8v(2)
        .u8v(0)
        .u32v(750)
        .u32v(1)
        .u32v(7300)
        .u32v(0)
        .u32v(0)
        .u8v(0)
        .u8v(2)
        .u32v(2589)
        .u32v(2)
        .u32v(1234)
        .u32v(0)
        .u32v(0)
        .u8v(0);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_RESPONSE, response) == game::Gameplay::Handled::Yes);
    const game::LootWindow& loot = h.play.loot();
    REQUIRE(loot.open());
    CHECK(loot.guid() == kWolf);
    CHECK(loot.gold() == 23);
    REQUIRE(loot.items().size() == 2);
    CHECK(loot.items()[1].slot == 2);
    CHECK(h.sentWith(Opcode::CMSG_ITEM_QUERY_SINGLE).size() == 2); // names and icons

    h.sent.clear();
    h.play.lootAll();
    REQUIRE(h.sent.size() == 3);
    CHECK(h.sent[0].opcode == Opcode::CMSG_LOOT_MONEY);
    CHECK(h.sent[0].body.empty());
    CHECK(h.sent[1].opcode == Opcode::CMSG_AUTOSTORE_LOOT_ITEM);
    CHECK(h.sent[1].body == Bytes{0});
    CHECK(h.sent[2].body == Bytes{2});

    u32 cursor = h.play.events().end();
    REQUIRE(h.packet(Opcode::SMSG_LOOT_CLEAR_MONEY, {}) == game::Gameplay::Handled::Yes);
    CHECK(loot.gold() == 0);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_REMOVED, Bytes{0}) == game::Gameplay::Handled::Yes);
    CHECK(loot.find(0)->taken);
    CHECK_FALSE(loot.empty());
    Bytes push;
    net::ByteWriter(push)
        .u64v(kPlayer)
        .u32v(0)
        .u32v(0)
        .u32v(1)
        .u8v(255)
        .u32v(23)
        .u32v(750)
        .u32v(0)
        .u32v(0)
        .u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, push) == game::Gameplay::Handled::Yes);
    Bytes otherPush(push);
    otherPush[0] = static_cast<u8>(kOtherPlayer); // group loot is broadcast: not ours (a member's chat line)
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, otherPush) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_REMOVED, Bytes{2}) == game::Gameplay::Handled::Yes);
    CHECK(loot.empty());
    // Solo money is only a coinage change.
    h.apply(values(kPlayer, {{uf::PLAYER_FIELD_COINAGE, 123}}));
    std::vector<game::GameEvent> events = h.events(cursor);
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == GameEventKind::ItemReceived);
    CHECK(events[0].id == 750);
    CHECK(events[0].value[0] == 1);
    CHECK(events[1].kind == GameEventKind::MemberItemReceived);
    CHECK(events[1].guid == kOtherPlayer);
    CHECK(events[2].kind == GameEventKind::MoneyLooted);
    CHECK(events[2].value[0] == 23);
    CHECK(events[2].value[1] == 0); // alone

    // Closing releases it.
    h.sent.clear();
    h.play.closeLoot();
    CHECK(h.sentWith(Opcode::CMSG_LOOT_RELEASE) == std::vector<Bytes>{guidBody(kWolf)});
    CHECK_FALSE(loot.open());
    Bytes released;
    net::ByteWriter(released).u64v(kWolf).u8v(1);
    CHECK(h.packet(Opcode::SMSG_LOOT_RELEASE_RESPONSE, released) == game::Gameplay::Handled::Yes);

    // A refusal (too far): an event, no window.
    Bytes refused;
    net::ByteWriter(refused).u64v(kWolf).u8v(0).u8v(4);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_RESPONSE, refused) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(loot.open());
    events = h.events(cursor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GameEventKind::LootError);
    CHECK(events[0].value[0] == 4);

    // In a group the share comes as SMSG_LOOT_MONEY_NOTIFY (and the coinage change is not counted twice).
    REQUIRE(h.packet(Opcode::SMSG_LOOT_RESPONSE, response) == game::Gameplay::Handled::Yes);
    h.play.lootMoney();
    REQUIRE(h.packet(Opcode::SMSG_LOOT_CLEAR_MONEY, {}) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_MONEY_NOTIFY, u32Body(11)) == game::Gameplay::Handled::Yes);
    h.apply(values(kPlayer, {{uf::PLAYER_FIELD_COINAGE, 134}}));
    events = h.events(cursor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GameEventKind::MoneyLooted);
    CHECK(events[0].value[0] == 11);

    // A chest: used, and its loot comes the same way.
    h.apply(created(kChest, game::ObjectType::GameObject,
                    {{uf::OBJECT_FIELD_ENTRY, 1561}, {uf::GAMEOBJECT_TYPE_ID, game::kGameObjectChest}}));
    h.sent.clear();
    CHECK(h.play.interact(kChest) == game::Interaction::UseObject);
    CHECK(h.sentWith(Opcode::CMSG_GAMEOBJ_USE) == std::vector<Bytes>{guidBody(kChest)});
}

TEST_CASE("Gameplay: global cooldown, spell and category cooldowns, cooldowns on event") {
    Harness h;
    SpellTable table;
    constexpr u32 kFireball = 133, kFireBlast = 2136, kPotionA = 118, kPotionB = 858, kStealth = 1784;
    table.spells = {spell(kFireball, 0, 1500), spell(kFireBlast, 8000, 1500),
                    spell(kPotionA, 0, 0, 4, 120000), spell(kPotionB, 0, 0, 4, 120000),
                    spell(kStealth, 10000, 1000, 0, 0, game::kSpellAttrCooldownOnEvent)};
    h.play.setSpellSource(table.source());
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    h.apply(created(kWolf, game::ObjectType::Unit, {{uf::UNIT_FIELD_HEALTH, 10}}));
    const game::SpellBook& spells = h.play.spells();

    // Casting starts the global cooldown at once (the client's own prediction), for every spell sharing it.
    h.sent.clear();
    REQUIRE(h.play.castSpell(kFireBlast, kWolf));
    Bytes cast;
    net::SpellCastTargets unit;
    unit.mask = net::kTargetUnit;
    unit.unit = kWolf;
    net::writeCastSpell(cast, kFireBlast, unit);
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL) == std::vector<Bytes>{cast});
    CHECK(spells.remaining(kFireball, h.now + 500) == 1000);
    CHECK(spells.globalCooldown(h.now + 500).active(h.now + 500));
    CHECK(spells.remaining(kPotionA, h.now + 500) == 0); // not on the global cooldown
    // The server refuses it: the prediction is undone.
    Bytes refused;
    net::ByteWriter(refused).u32v(kFireBlast).u8v(2).u8v(net::kCastFailedOutOfRange);
    u32 cursor = h.play.events().end();
    REQUIRE(h.packet(Opcode::SMSG_CAST_RESULT, refused) == game::Gameplay::Handled::Yes);
    CHECK(spells.remaining(kFireball, h.now + 500) == 0);
    std::vector<game::GameEvent> events = h.events(cursor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GameEventKind::CastFailed);
    CHECK(events[0].id == kFireBlast);
    CHECK(events[0].value[0] == net::kCastFailedOutOfRange);

    // Cast again: accepted (SPELL_START, instant) and gone off (SPELL_GO): its own 8 s cooldown starts.
    REQUIRE(h.play.castSpell(kFireBlast, kWolf));
    Bytes start;
    net::ByteWriter(start)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(kFireBlast)
        .u16v(2)
        .u32v(0)
        .u16v(net::kTargetUnit)
        .packedGuid(kWolf);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_START, start) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(spells.cast().active()); // instant: no bar
    Bytes go;
    net::ByteWriter(go)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(kFireBlast)
        .u16v(0x100)
        .u8v(1)
        .u64v(kWolf)
        .u8v(0)
        .u16v(net::kTargetUnit)
        .packedGuid(kWolf);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, go) == game::Gameplay::Handled::Yes);
    CHECK(spells.remaining(kFireBlast, h.now + 2000) == 6000);
    CHECK(spells.remaining(kFireball, h.now + 2000) == 0); // the global cooldown is over
    CHECK(spells.cooldown(kFireBlast, h.now + 100).durationMs == 8000);
    CHECK(spells.remaining(kFireBlast, h.now + 9000) == 0);

    // A category cooldown locks every spell of the category.
    h.now += 20000;
    Bytes potion;
    net::ByteWriter(potion)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(kPotionA)
        .u16v(0x100)
        .u8v(0)
        .u8v(0)
        .u16v(0);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, potion) == game::Gameplay::Handled::Yes);
    CHECK(spells.remaining(kPotionB, h.now + 1000) == 119000);
    CHECK(spells.remaining(kPotionA, h.now + 1000) == 119000);

    // "Cooldown on event" spells wait for SMSG_COOLDOWN_EVENT (Stealth: when it ends).
    Bytes stealth;
    net::ByteWriter(stealth)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(kStealth)
        .u16v(0x100)
        .u8v(0)
        .u8v(0)
        .u16v(0);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, stealth) == game::Gameplay::Handled::Yes);
    CHECK(spells.cooldown(kStealth, h.now + 60000).untilEvent);
    CHECK(spells.cooldown(kStealth, h.now).active(h.now + 60000));
    h.now += 30000;
    Bytes event;
    net::ByteWriter(event).u32v(kStealth).u64v(kPlayer);
    REQUIRE(h.packet(Opcode::SMSG_COOLDOWN_EVENT, event) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(spells.cooldown(kStealth, h.now).untilEvent);
    CHECK(spells.remaining(kStealth, h.now + 4000) == 6000);
    REQUIRE(h.packet(Opcode::SMSG_CLEAR_COOLDOWN, event) == game::Gameplay::Handled::Yes);
    CHECK(spells.remaining(kStealth, h.now + 4000) == 0);

    // SMSG_SPELL_COOLDOWN: 0 ms = the spell's global cooldown; else a lockout of that length.
    h.now += 1000;
    Bytes lockout;
    net::ByteWriter(lockout).u64v(kPlayer).u32v(kFireball).u32v(0).u32v(kFireBlast).u32v(4000);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_COOLDOWN, lockout) == game::Gameplay::Handled::Yes);
    CHECK(spells.globalCooldown(h.now).active(h.now));
    CHECK(spells.remaining(kFireBlast, h.now + 3000) == 1000);

    // Cooldowns at login: remaining times; the permanent ones wait for an event.
    Bytes initial;
    net::ByteWriter(initial)
        .u8v(0)
        .u16v(2)
        .u16v(kFireball)
        .u16v(0)
        .u16v(kFireBlast)
        .u16v(0)
        .u16v(2)
        .u16v(kFireBlast)
        .u16v(0)
        .u16v(0)
        .u32v(4000)
        .u32v(0)
        .u16v(kStealth)
        .u16v(0)
        .u16v(0)
        .u32v(1)
        .u32v(0x80000000u);
    REQUIRE(h.packet(Opcode::SMSG_INITIAL_SPELLS, initial) == game::Gameplay::Handled::Yes);
    CHECK(spells.knows(kFireball));
    CHECK_FALSE(spells.knows(kPotionA));
    CHECK(spells.remaining(kFireBlast, h.now + 1000) == 3000);
    CHECK(spells.cooldown(kStealth, h.now).untilEvent);

    // Tapping again during the global cooldown does not restart it. (Actions use the clock of the last update().)
    h.now += 100000;
    h.play.update(h.now);
    REQUIRE(h.play.castSpell(kFireball, kWolf));
    h.now += 500;
    h.play.update(h.now);
    REQUIRE(h.play.castSpell(kFireball, kWolf));
    CHECK(spells.remaining(kFireball, h.now) == 1000);
}

TEST_CASE("Gameplay: known spells, action buttons, cast bars and channels") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    h.apply(created(kWolf, game::ObjectType::Unit, {{uf::UNIT_FIELD_HEALTH, 10}}));
    const game::SpellBook& spells = h.play.spells();
    u32 cursor = h.play.events().end();

    Bytes learned;
    net::ByteWriter(learned).u16v(133).u16v(0);
    REQUIRE(h.packet(Opcode::SMSG_LEARNED_SPELL, learned) == game::Gameplay::Handled::Yes);
    CHECK(spells.knows(133));
    std::vector<game::GameEvent> events = h.events(cursor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GameEventKind::SpellLearned);

    // Buttons: button 0 casts spell 133 at the target; a new rank replaces the old one on the bar.
    Bytes buttons;
    net::ByteWriter b(buttons);
    for (u32 i = 0; i < net::kActionButtonCount; ++i) {
        b.u32v(i == 0 ? 133u : 0u);
    }
    REQUIRE(h.packet(Opcode::SMSG_ACTION_BUTTONS, buttons) == game::Gameplay::Handled::Yes);
    CHECK(spells.button(0).type == net::kActionSpell);
    CHECK(spells.button(0).action == 133);
    CHECK(spells.button(1).empty());
    h.play.setTarget(kWolf);
    h.sent.clear();
    REQUIRE(h.play.useAction(0));
    REQUIRE(h.sentWith(Opcode::CMSG_CAST_SPELL).size() == 1);
    Bytes supercede;
    net::ByteWriter(supercede).u16v(133).u16v(143);
    REQUIRE(h.packet(Opcode::SMSG_SUPERCEDED_SPELL, supercede) == game::Gameplay::Handled::Yes);
    CHECK(spells.button(0).action == 143);
    CHECK(spells.knows(143));
    CHECK_FALSE(spells.knows(133));
    h.sent.clear();
    h.play.setActionButton(1, game::ActionButton{net::kActionItem, 159});
    CHECK(h.sentWith(Opcode::CMSG_SET_ACTION_BUTTON) == std::vector<Bytes>{Bytes{1, 159, 0, 0, 0x80}});
    CHECK(spells.button(1).type == net::kActionItem);

    // A 3 s cast with a 0.5 s pushback, then gone off.
    Bytes start;
    net::ByteWriter(start)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(143)
        .u16v(2)
        .u32v(3000)
        .u16v(net::kTargetUnit)
        .packedGuid(kWolf);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_START, start) == game::Gameplay::Handled::Yes);
    REQUIRE(spells.cast().active());
    CHECK(spells.cast().target == kWolf);
    CHECK(spells.cast().progress(h.now + 1500) == doctest::Approx(0.5f));
    Bytes delayed;
    net::ByteWriter(delayed).u64v(kPlayer).u32v(500);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_DELAYED, delayed) == game::Gameplay::Handled::Yes);
    CHECK(spells.cast().remaining(h.now + 1500) == 2000);
    Bytes go;
    net::ByteWriter(go)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(143)
        .u16v(0x100)
        .u8v(1)
        .u64v(kWolf)
        .u8v(0)
        .u16v(net::kTargetUnit)
        .packedGuid(kWolf);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, go) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(spells.cast().active());

    // Interrupted: SPELL_FAILED_OTHER ends the bar.
    REQUIRE(h.packet(Opcode::SMSG_SPELL_START, start) == game::Gameplay::Handled::Yes);
    Bytes interrupted;
    net::ByteWriter(interrupted).u64v(kPlayer).u32v(143);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_FAILED_OTHER, interrupted) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(spells.cast().active());

    // A channel empties; the player cancels it.
    Bytes channel;
    net::ByteWriter(channel).u32v(5143).u32v(5000);
    REQUIRE(h.packet(Opcode::MSG_CHANNEL_START, channel) == game::Gameplay::Handled::Yes);
    CHECK(spells.cast().channel);
    CHECK(spells.cast().progress(h.now + 1000) == doctest::Approx(0.8f));
    REQUIRE(h.packet(Opcode::MSG_CHANNEL_UPDATE, u32Body(2000)) == game::Gameplay::Handled::Yes);
    CHECK(spells.cast().remaining(h.now) == 2000);
    h.sent.clear();
    h.play.cancelCast();
    CHECK(h.sentWith(Opcode::CMSG_CANCEL_CHANNELLING) == std::vector<Bytes>{u32Body(5143)});
    CHECK_FALSE(spells.cast().active());

    // Another unit's cast, for the target frame.
    Bytes wolfCast;
    net::ByteWriter(wolfCast).packedGuid(kWolf).packedGuid(kWolf).u32v(3261).u16v(2).u32v(2000).u16v(0);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_START, wolfCast) == game::Gameplay::Handled::Yes);
    const game::CastBar* bar = spells.unitCast(kWolf, h.now + 500);
    REQUIRE(bar != nullptr);
    CHECK(bar->spellId == 3261);
    CHECK(spells.unitCast(kWolf, h.now + 2500) == nullptr);
}

TEST_CASE("Gameplay: visual and sound events of every unit") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    u32 cursor = h.play.fx().end();
    auto next = [&]() {
        const game::FxEvent* e = h.play.fx().next(cursor);
        REQUIRE(e != nullptr);
        return *e;
    };
    using game::FxEventKind;

    // A wolf casts at the player; another unit's Blizzard lands on a location.
    Bytes start;
    net::ByteWriter(start).packedGuid(kWolf).packedGuid(kWolf).u32v(3261).u16v(2).u32v(2000).u16v(
        net::kTargetUnit).packedGuid(kPlayer);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_START, start) == game::Gameplay::Handled::Yes);
    game::FxEvent e = next();
    CHECK(e.kind == FxEventKind::SpellStart);
    CHECK(e.guid == kWolf);
    CHECK(e.other == kPlayer);
    CHECK(e.id == 3261);
    CHECK(e.value[0] == 2000);
    CHECK_FALSE(e.hasPosition);
    Bytes blizzard;
    net::ByteWriter(blizzard)
        .packedGuid(kOtherPlayer)
        .packedGuid(kOtherPlayer)
        .u32v(10)
        .u16v(0x100)
        .u8v(2)
        .u64v(kWolf)
        .u64v(kNpc)
        .u8v(1)
        .u64v(kChest)
        .u8v(2)
        .u16v(net::kTargetDestLocation)
        .f32v(1.0f)
        .f32v(2.0f)
        .f32v(3.0f);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, blizzard) == game::Gameplay::Handled::Yes);
    e = next();
    CHECK(e.kind == FxEventKind::SpellGo);
    CHECK(e.guid == kOtherPlayer);
    CHECK(e.other == kWolf); // no unit target: the first hit
    CHECK(e.value[0] == 2);
    CHECK(e.value[1] == 1);
    CHECK(e.hasPosition);
    CHECK(e.position[2] == 3.0f);
    CHECK(next().other == kWolf);
    e = next();
    CHECK(e.kind == FxEventKind::SpellHit);
    CHECK(e.other == kNpc);

    // Interrupted, a kit on a unit, sounds, emotes, melee.
    Bytes interrupted;
    net::ByteWriter(interrupted).u64v(kWolf).u32v(3261);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_FAILED_OTHER, interrupted) == game::Gameplay::Handled::Yes);
    CHECK(next().kind == FxEventKind::SpellInterrupted);
    Bytes kit;
    net::ByteWriter(kit).u64v(kNpc).u32v(191);
    REQUIRE(h.packet(Opcode::SMSG_PLAY_SPELL_IMPACT, kit) == game::Gameplay::Handled::Yes);
    e = next();
    CHECK(e.kind == FxEventKind::SpellImpact);
    CHECK(e.guid == kNpc);
    CHECK(e.id == 191);
    REQUIRE(h.packet(Opcode::SMSG_PLAY_MUSIC, u32Body(2398)) == game::Gameplay::Handled::Yes);
    e = next();
    CHECK(e.kind == FxEventKind::Music);
    CHECK(e.id == 2398);
    Bytes emote;
    net::ByteWriter(emote).u32v(10).u64v(kNpc);
    REQUIRE(h.packet(Opcode::SMSG_EMOTE, emote) == game::Gameplay::Handled::Yes);
    e = next();
    CHECK(e.kind == FxEventKind::Emote);
    CHECK(e.guid == kNpc);
    CHECK(e.id == 10);
    Bytes attack;
    net::ByteWriter(attack).u64v(kWolf).u64v(kOtherPlayer);
    REQUIRE(h.packet(Opcode::SMSG_ATTACKSTART, attack) == game::Gameplay::Handled::Yes);
    e = next();
    CHECK(e.kind == FxEventKind::AttackStart);
    CHECK(e.other == kOtherPlayer);
    CHECK(h.play.fx().next(cursor) == nullptr);
    CHECK(h.packet(Opcode::SMSG_PLAY_SPELL_VISUAL, Bytes{1, 2, 3}) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Gameplay: a new character's intro cinematic plays until the client ends it") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    h.sent.clear();
    REQUIRE(h.packet(Opcode::SMSG_TRIGGER_CINEMATIC, u32Body(81)) == game::Gameplay::Handled::Yes);
    CHECK(h.play.cinematic() == 81);
    CHECK(h.sentWith(Opcode::CMSG_COMPLETE_CINEMATIC).empty());
    h.play.nextCinematicCamera();
    CHECK(h.sentWith(Opcode::CMSG_NEXT_CINEMATIC_CAMERA) == std::vector<Bytes>{Bytes{}});
    h.play.completeCinematic();
    CHECK(h.play.cinematic() == 0);
    CHECK(h.sentWith(Opcode::CMSG_COMPLETE_CINEMATIC) == std::vector<Bytes>{Bytes{}});
    h.play.nextCinematicCamera(); // none plays: nothing sent
    CHECK(h.sentWith(Opcode::CMSG_NEXT_CINEMATIC_CAMERA).size() == 1);
    // Skipped at once when asked (tests, development), as before phase 14.
    h.play.skipCinematics = true;
    REQUIRE(h.packet(Opcode::SMSG_TRIGGER_CINEMATIC, u32Body(81)) == game::Gameplay::Handled::Yes);
    CHECK(h.play.cinematic() == 0);
    CHECK(h.sentWith(Opcode::CMSG_COMPLETE_CINEMATIC).size() == 2);
    CHECK(h.packet(Opcode::SMSG_TRIGGER_CINEMATIC, Bytes{1}) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Gameplay: auras from the update fields, durations of the player's own") {
    Harness h;
    // Slots 0 and 5 (buffs), 32 (a debuff). Flags: 4 bits a slot; levels / applications: a byte a slot.
    const u32 flags0 = 0x9u | (0x8u << (5 * 4)); // slot 0: cancelable + effect 0; slot 5: effect 0
    h.apply(created(kPlayer, game::ObjectType::Player,
                    playerFields({{uf::UNIT_FIELD_AURA + 0, 1243},
                                  {uf::UNIT_FIELD_AURA + 5, 774},
                                  {uf::UNIT_FIELD_AURA + 32, 172},
                                  {uf::UNIT_FIELD_AURAFLAGS + 0, flags0},
                                  {uf::UNIT_FIELD_AURAFLAGS + 4, 0x8u},
                                  {uf::UNIT_FIELD_AURALEVELS + 1, 3u << 8},      // slot 5
                                  {uf::UNIT_FIELD_AURAAPPLICATIONS + 8, 4u}}))); // slot 32: 5 stacks
    std::array<game::AuraInfo, game::kMaxAuras> auras;
    REQUIRE(h.play.auras(kPlayer, auras) == 3);
    CHECK(auras[0].spellId == 1243);
    CHECK(auras[0].positive());
    CHECK(auras[0].cancelable());
    CHECK(auras[0].stacks == 1);
    CHECK(auras[1].spellId == 774);
    CHECK(auras[1].slot == 5);
    CHECK(auras[1].casterLevel == 3);
    CHECK_FALSE(auras[1].cancelable());
    CHECK(auras[2].slot == 32);
    CHECK_FALSE(auras[2].positive());
    CHECK(auras[2].stacks == 5);
    CHECK_FALSE(auras[0].timed());

    // A duration for slot 5 arrives just before the field update that shows a new aura there: it belongs to
    // it.
    Bytes duration;
    net::ByteWriter(duration).u8v(5).u32v(12000);
    REQUIRE(h.packet(Opcode::SMSG_UPDATE_AURA_DURATION, duration) == game::Gameplay::Handled::Yes);
    h.now += 100;
    h.apply(values(kPlayer, {{uf::UNIT_FIELD_AURA + 5, 8936}}));
    REQUIRE(h.play.auras(kPlayer, auras) == 3);
    CHECK(auras[1].spellId == 8936);
    CHECK(auras[1].durationMs == 12000);
    h.now += 2000;
    h.play.update(h.now);
    REQUIRE(h.play.auras(kPlayer, auras) == 3);
    CHECK(auras[1].remainingMs == 12000 - 2100);
    // Much later the slot changes aura without a new duration: the old one is dropped.
    h.now += 20000;
    h.apply(values(kPlayer, {{uf::UNIT_FIELD_AURA + 5, 1126}}));
    REQUIRE(h.play.auras(kPlayer, auras) == 3);
    CHECK_FALSE(auras[1].timed());
    // Output shorter than the auras: only what fits.
    CHECK(h.play.auras(kPlayer, std::span<game::AuraInfo>(auras.data(), 2)) == 2);
}

TEST_CASE("Gameplay: an attack draws the weapons; the swing ends when the victim dies") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    h.apply(
        created(kWolf, game::ObjectType::Unit, {{uf::OBJECT_FIELD_ENTRY, 299}, {uf::UNIT_FIELD_HEALTH, 30}}));
    u32 cursor = h.play.events().end();

    // The server never draws a player's weapons: the client asks (sheath state 1, melee).
    h.sent.clear();
    REQUIRE(h.play.startAttack(kWolf));
    CHECK(h.sentWith(Opcode::CMSG_SETSHEATHED) == std::vector<Bytes>{u32Body(game::kSheathMelee)});
    h.apply(values(kPlayer, {{uf::UNIT_FIELD_BYTES_2, game::kSheathMelee}}));
    h.sent.clear();
    REQUIRE(h.play.startAttack(kWolf));
    CHECK(h.sentWith(Opcode::CMSG_SETSHEATHED).empty()); // already drawn
    h.play.setSheath(game::kSheathRanged);
    CHECK(h.sentWith(Opcode::CMSG_SETSHEATHED) == std::vector<Bytes>{u32Body(game::kSheathRanged)});

    // The victim dies: the swing stops at once (the server's SMSG_ATTACKSTOP may still be on its way), one event.
    h.apply(values(kWolf, {{uf::UNIT_FIELD_HEALTH, 0}}));
    h.now += 50;
    h.play.update(h.now);
    CHECK_FALSE(h.play.melee().attacking());
    Bytes stopped;
    net::ByteWriter(stopped).packedGuid(kPlayer).packedGuid(kWolf).u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_ATTACKSTOP, stopped) == game::Gameplay::Handled::Yes);
    std::vector<game::GameEvent> events = h.events(cursor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GameEventKind::AttackStopped);
    CHECK(events[0].guid == kWolf);
}

TEST_CASE("Gameplay: melee state, swing errors and the combat log") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    h.apply(
        created(kWolf, game::ObjectType::Unit, {{uf::OBJECT_FIELD_ENTRY, 299}, {uf::UNIT_FIELD_HEALTH, 30}}));
    u32 cursor = h.play.events().end();
    u32 combat = h.play.combatLog().end();

    h.sent.clear();
    REQUIRE(h.play.startAttack(kWolf));
    CHECK(h.sentWith(Opcode::CMSG_SET_SELECTION) == std::vector<Bytes>{guidBody(kWolf)});
    CHECK(h.sentWith(Opcode::CMSG_ATTACKSWING) == std::vector<Bytes>{guidBody(kWolf)});
    CHECK(h.play.melee().attacking());
    Bytes started;
    net::ByteWriter(started).u64v(kPlayer).u64v(kWolf);
    REQUIRE(h.packet(Opcode::SMSG_ATTACKSTART, started) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ATTACKSWING_NOTINRANGE, {}) == game::Gameplay::Handled::Yes);
    std::vector<game::GameEvent> events = h.events(cursor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GameEventKind::SwingNotInRange);
    CHECK(events[0].guid == kWolf);

    // A crit; a dodge; an absorbed hit; a partial block.
    auto swing = [&](u32 hitInfo, i32 total, i32 absorbed, u32 victimState, i32 blocked) {
        Bytes out;
        net::ByteWriter(out)
            .u32v(hitInfo)
            .packedGuid(kPlayer)
            .packedGuid(kWolf)
            .u32v(static_cast<u32>(total))
            .u8v(1)
            .u32v(0)
            .f32v(static_cast<f32>(total))
            .u32v(static_cast<u32>(total))
            .u32v(static_cast<u32>(absorbed))
            .u32v(0)
            .u32v(victimState)
            .u32v(0)
            .u32v(0)
            .u32v(static_cast<u32>(blocked));
        REQUIRE(h.packet(Opcode::SMSG_ATTACKERSTATEUPDATE, out) == game::Gameplay::Handled::Yes);
    };
    swing(net::kHitInfoAffectsVictim | net::kHitInfoCritical, 24, 0, 1, 0);
    swing(0, 0, 0, 2, 0);
    swing(net::kHitInfoAbsorb, 0, 12, 1, 0);
    swing(net::kHitInfoAffectsVictim | net::kHitInfoBlock, 5, 0, 1, 7);
    swing(net::kHitInfoMiss, 0, 0, 0, 0);
    using K = game::CombatEvent::Kind;
    std::vector<game::CombatEvent> log;
    while (const game::CombatEvent* e = h.play.combatLog().next(combat)) {
        log.push_back(*e);
    }
    REQUIRE(log.size() == 5);
    CHECK(log[0].kind == K::Damage);
    CHECK(log[0].amount == 24);
    CHECK((log[0].flags & game::CombatEvent::kCrit) != 0);
    CHECK(log[0].attacker == kPlayer);
    CHECK(log[0].victim == kWolf);
    CHECK(log[1].kind == K::Dodge);
    CHECK(log[2].kind == K::Absorb);
    CHECK(log[2].absorbed == 12);
    CHECK(log[3].kind == K::Damage);
    CHECK(log[3].blocked == 7);
    CHECK(log[4].kind == K::Miss);

    // Spells: a resisted cast (only in SPELL_GO's misses), damage over time, a heal.
    Bytes resisted;
    net::ByteWriter(resisted)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(116)
        .u16v(0x100)
        .u8v(0)
        .u8v(1)
        .u64v(kWolf)
        .u8v(2)
        .u16v(net::kTargetUnit)
        .packedGuid(kWolf);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, resisted) == game::Gameplay::Handled::Yes);
    Bytes dot;
    net::ByteWriter(dot)
        .packedGuid(kWolf)
        .packedGuid(kPlayer)
        .u32v(172)
        .u32v(1)
        .u32v(3)
        .u32v(9)
        .u32v(5)
        .u32v(0)
        .u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_PERIODICAURALOG, dot) == game::Gameplay::Handled::Yes);
    Bytes heal;
    net::ByteWriter(heal).packedGuid(kPlayer).packedGuid(kPlayer).u32v(2050).u32v(120).u8v(1);
    REQUIRE(h.packet(Opcode::SMSG_SPELLHEALLOG, heal) == game::Gameplay::Handled::Yes);
    log.clear();
    while (const game::CombatEvent* e = h.play.combatLog().next(combat)) {
        log.push_back(*e);
    }
    REQUIRE(log.size() == 3);
    CHECK(log[0].kind == K::Resist);
    CHECK(log[0].spellId == 116);
    CHECK(log[1].kind == K::Damage);
    CHECK((log[1].flags & game::CombatEvent::kPeriodic) != 0);
    CHECK(log[1].school == 5);
    CHECK(log[2].kind == K::Heal);
    CHECK(log[2].amount == 120);

    // The server stops the swing (the wolf died): the state follows, one event.
    Bytes stopped;
    net::ByteWriter(stopped).packedGuid(kPlayer).packedGuid(kWolf).u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_ATTACKSTOP, stopped) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(h.play.melee().attacking());
    events = h.events(cursor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GameEventKind::AttackStopped);

    // Unit helpers from the fields.
    h.apply(values(kWolf, {{uf::UNIT_FIELD_FLAGS, game::kUnitFlagInCombat}, {uf::UNIT_DYNAMIC_FLAGS, 0x04}}));
    const game::Entity* wolf = h.store.find(kWolf);
    CHECK(game::inCombat(*wolf));
    CHECK(game::isTappedByOther(*wolf));
    CHECK_FALSE(game::isTappedByMe(*wolf));
    h.apply(values(kWolf, {{uf::UNIT_DYNAMIC_FLAGS, 0x04 | 0x08}}));
    CHECK_FALSE(game::isTappedByOther(*wolf));
    CHECK(game::isTappedByMe(*wolf));
}

TEST_CASE("Gameplay: what a tap does, by reaction and NPC flags") {
    Harness h;
    game::Reactions reactions;
    reactions.setTemplate(1, 1, 0x1, 0x1, 0x8);   // the player's side: friends with group 1, enemies of 8
    reactions.setTemplate(14, 14, 0x8, 0x0, 0x1); // monsters: hostile to group 1
    reactions.setTemplate(7, 7, 0x0, 0x0, 0x0);   // neutral creatures
    h.play.setData(nullptr, &reactions);
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    h.apply(created(kWolf, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 30}, {uf::UNIT_FIELD_FACTIONTEMPLATE, 14}}));
    h.apply(created(kNpc, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 30},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, 1},
                     {uf::UNIT_NPC_FLAGS, game::kNpcFlagQuestGiver}}));
    const u64 kSpiritHealer = 0xF130000000006666ull;
    h.apply(created(kSpiritHealer, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 30},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, 1},
                     {uf::UNIT_NPC_FLAGS, game::kNpcFlagSpiritHealer | game::kNpcFlagGossip}}));
    const u64 kDeer = 0xF130000000007777ull;
    h.apply(created(kDeer, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 30}, {uf::UNIT_FIELD_FACTIONTEMPLATE, 7}}));
    const u64 kGuard = 0xF130000000008888ull;
    h.apply(created(kGuard, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 30},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, 14},
                     {uf::UNIT_FIELD_FLAGS, game::kUnitFlagNotSelectable}}));

    const game::Entity& me = *h.store.find(kPlayer);
    CHECK(game::reactionTo(me, *h.store.find(kWolf), &reactions) == game::Reaction::Hostile);
    CHECK(game::canAttack(me, *h.store.find(kWolf), &reactions));
    CHECK_FALSE(game::canAttack(me, *h.store.find(kNpc), &reactions));
    CHECK_FALSE(game::canAttack(me, *h.store.find(kGuard), &reactions));
    CHECK(game::isQuestGiver(*h.store.find(kNpc)));

    h.sent.clear();
    CHECK(h.play.interact(kWolf) == game::Interaction::Attack);
    CHECK(h.sentWith(Opcode::CMSG_ATTACKSWING) == std::vector<Bytes>{guidBody(kWolf)});
    CHECK(h.play.interact(kNpc) == game::Interaction::QuestGiver);
    CHECK(h.sentWith(Opcode::CMSG_QUESTGIVER_HELLO) == std::vector<Bytes>{guidBody(kNpc)});
    CHECK(h.play.interact(kSpiritHealer) == game::Interaction::SpiritHealer);
    CHECK(h.sentWith(Opcode::CMSG_GOSSIP_HELLO) == std::vector<Bytes>{guidBody(kSpiritHealer)});
    CHECK(h.play.interact(kDeer) == game::Interaction::Attack); // neutral, nothing to talk about
    CHECK(h.play.interact(kGuard) == game::Interaction::None);
    CHECK(h.play.interact(0x999) == game::Interaction::None); // unknown

    // Not in the world: nothing goes out.
    h.play.leaveWorld();
    h.sent.clear();
    CHECK(h.play.interact(kWolf) == game::Interaction::None);
    CHECK_FALSE(h.play.castSpell(133, 0));
    CHECK_FALSE(h.play.sendChat(net::ChatType::Say, "hello"));
    CHECK(h.sent.empty());
}

TEST_CASE("Gameplay: chat lines, names, sending in the player's language") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    u32 cursor = h.play.chat().end();
    Bytes say;
    net::ByteWriter(say)
        .u8v(0x00)
        .u32v(7)
        .u64v(kOtherPlayer)
        .u64v(kOtherPlayer)
        .u32v(6)
        .cstring("hello")
        .u8v(0);
    h.sent.clear();
    REQUIRE(h.packet(Opcode::SMSG_MESSAGECHAT, say) == game::Gameplay::Handled::Yes);
    // A player's name never comes with the line: asked for at once.
    CHECK(h.sentWith(Opcode::CMSG_NAME_QUERY) == std::vector<Bytes>{guidBody(kOtherPlayer)});
    const game::ChatLine* line = h.play.chat().next(cursor);
    REQUIRE(line != nullptr);
    CHECK(line->type == net::ChatType::Say);
    CHECK(line->text == "hello");
    CHECK(h.play.senderName(*line).empty());
    Bytes name;
    net::ByteWriter(name).u64v(kOtherPlayer).cstring("Bob").cstring("").u32v(1).u32v(0).u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_NAME_QUERY_RESPONSE, name) == game::Gameplay::Handled::Yes);
    CHECK(h.play.senderName(*line) == "Bob");
    CHECK(h.sentWith(Opcode::CMSG_NAME_QUERY).size() == 1); // no second query

    Bytes monster;
    net::ByteWriter(monster)
        .u8v(0x0C)
        .u32v(0)
        .u64v(kNpc)
        .u32v(7)
        .cstring("Hogger")
        .u64v(0)
        .u32v(5)
        .cstring("Grr!")
        .u8v(0);
    REQUIRE(h.packet(Opcode::SMSG_MESSAGECHAT, monster) == game::Gameplay::Handled::Yes);
    Bytes notice;
    net::ByteWriter(notice).cstring("You are too far away.");
    REQUIRE(h.packet(Opcode::SMSG_NOTIFICATION, notice) == game::Gameplay::Handled::Yes);
    Bytes notFound;
    net::ByteWriter(notFound).cstring("Nobody");
    REQUIRE(h.packet(Opcode::SMSG_CHAT_PLAYER_NOT_FOUND, notFound) == game::Gameplay::Handled::Yes);
    line = h.play.chat().next(cursor);
    REQUIRE(line != nullptr);
    CHECK(line->type == net::ChatType::MonsterYell);
    CHECK(h.play.senderName(*line) == "Hogger");
    line = h.play.chat().next(cursor);
    REQUIRE(line != nullptr);
    CHECK(line->source == game::ChatSource::Notification);
    CHECK(line->text == "You are too far away.");
    line = h.play.chat().next(cursor);
    REQUIRE(line != nullptr);
    CHECK(line->source == game::ChatSource::PlayerNotFound);
    CHECK(line->text == "Nobody");
    CHECK(h.play.chat().next(cursor) == nullptr);

    // Sending: a human speaks Common; whispers carry the name; emotes too go in the race's language.
    h.sent.clear();
    REQUIRE(h.play.sendChat(net::ChatType::Say, "hi"));
    REQUIRE(h.play.sendChat(net::ChatType::Whisper, "psst", "Bob"));
    REQUIRE(h.play.sendChat(net::ChatType::Emote, "waves"));
    CHECK_FALSE(h.play.sendChat(net::ChatType::Whisper, "psst")); // whom to?
    const std::vector<Bytes> chat = h.sentWith(Opcode::CMSG_MESSAGECHAT);
    REQUIRE(chat.size() == 3);
    CHECK(chat[0] == Bytes{0, 0, 0, 0, 7, 0, 0, 0, 'h', 'i', 0});
    CHECK(chat[1] == Bytes{6, 0, 0, 0, 7, 0, 0, 0, 'B', 'o', 'b', 0, 'p', 's', 's', 't', 0});
    CHECK(chat[2] == Bytes{8, 0, 0, 0, 7, 0, 0, 0, 'w', 'a', 'v', 'e', 's', 0});
    // An orc speaks Orcish.
    h.apply(values(kPlayer, {{uf::UNIT_FIELD_BYTES_0, 2}}));
    h.sent.clear();
    REQUIRE(h.play.sendChat(net::ChatType::Yell, "Lok'tar!"));
    CHECK(h.sentWith(Opcode::CMSG_MESSAGECHAT)[0][4] == 1);

    // The ring keeps the last lines (and reuses their strings).
    u32 fromStart = 0;
    for (u32 i = 0; i < game::ChatLog::kCapacity + 10; ++i) {
        REQUIRE(h.packet(Opcode::SMSG_NOTIFICATION, notice) == game::Gameplay::Handled::Yes);
    }
    u32 lines = 0;
    while (h.play.chat().next(fromStart)) {
        ++lines;
    }
    CHECK(lines == game::ChatLog::kCapacity);
}

TEST_CASE("Gameplay: death, release, corpse, reclaim, resurrection and spirit healer") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields(), 100.0f, 100.0f, 0.0f));
    u32 cursor = h.play.events().end();
    CHECK_FALSE(h.play.death().dead);

    // Death: health 0 and the release timer.
    h.apply(values(kPlayer,
                   {{uf::UNIT_FIELD_HEALTH, 0}, {uf::PLAYER_FIELD_BYTES, game::kPlayerBytesReleaseTimer}}));
    std::vector<game::GameEvent> events = h.events(cursor);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == GameEventKind::PlayerDied);
    CHECK(h.play.death().dead);
    CHECK(h.play.death().releaseTimer);
    h.sent.clear();
    h.play.releaseSpirit();
    CHECK(h.sentWith(Opcode::CMSG_REPOP_REQUEST) == std::vector<Bytes>{Bytes{}});

    // A ghost now: the corpse is asked for; the reclaim delay counts down.
    REQUIRE(h.packet(Opcode::SMSG_CORPSE_RECLAIM_DELAY, u32Body(30000)) == game::Gameplay::Handled::Yes);
    h.sent.clear();
    h.apply(values(kPlayer, {{uf::UNIT_FIELD_HEALTH, 1}, {uf::PLAYER_FLAGS, game::kPlayerFlagGhost}}));
    events = h.events(cursor);
    REQUIRE(findEvent(events, GameEventKind::PlayerReleased) != nullptr);
    CHECK(h.play.death().ghost);
    CHECK(h.sentWith(Opcode::MSG_CORPSE_QUERY) == std::vector<Bytes>{Bytes{}});
    Bytes corpse;
    net::ByteWriter(corpse).u8v(1).u32v(0).f32v(110.0f).f32v(100.0f).f32v(0.0f).u32v(0);
    REQUIRE(h.packet(Opcode::MSG_CORPSE_QUERY, corpse) == game::Gameplay::Handled::Yes);
    CHECK(h.play.death().corpseKnown);
    CHECK(h.play.death().corpseX == 110.0f);
    CHECK(h.play.reclaimDelayRemaining(h.now) == 30000);
    CHECK_FALSE(h.play.canReclaimCorpse(h.now)); // too early
    h.now += 30000;
    CHECK(h.play.canReclaimCorpse(h.now)); // 10 yards away, delay over
    h.apply(created(kCorpse, game::ObjectType::Corpse,
                    {{uf::CORPSE_FIELD_OWNER, low(kPlayer)}, {uf::CORPSE_FIELD_OWNER + 1, high(kPlayer)}},
                    110.0f, 100.0f));
    h.sent.clear();
    h.play.reclaimCorpse();
    CHECK(h.sentWith(Opcode::CMSG_RECLAIM_CORPSE) == std::vector<Bytes>{guidBody(kCorpse)});

    // A resurrection offer, accepted.
    Bytes rez;
    net::ByteWriter(rez).u64v(kOtherPlayer).u32v(1).u8v(0).u8v(0).u8v(1);
    REQUIRE(h.packet(Opcode::SMSG_RESURRECT_REQUEST, rez) == game::Gameplay::Handled::Yes);
    CHECK(h.play.death().resurrector == kOtherPlayer);
    h.sent.clear();
    h.play.answerResurrect(true);
    Bytes answer;
    net::writeResurrectResponse(answer, kOtherPlayer, true);
    CHECK(h.sentWith(Opcode::CMSG_RESURRECT_RESPONSE) == std::vector<Bytes>{answer});
    CHECK(h.play.death().resurrector == 0);

    // The spirit healer's confirmation.
    const u64 kSpiritHealer = 0xF130000000006666ull;
    REQUIRE(h.packet(Opcode::SMSG_SPIRIT_HEALER_CONFIRM, guidBody(kSpiritHealer)) ==
            game::Gameplay::Handled::Yes);
    events = h.events(cursor);
    REQUIRE(findEvent(events, GameEventKind::SpiritHealerConfirm) != nullptr);
    h.sent.clear();
    h.play.acceptSpiritHealer();
    CHECK(h.sentWith(Opcode::CMSG_SPIRIT_HEALER_ACTIVATE) == std::vector<Bytes>{guidBody(kSpiritHealer)});

    // Alive again.
    h.apply(values(kPlayer, {{uf::UNIT_FIELD_HEALTH, 50}, {uf::PLAYER_FLAGS, 0}}));
    events = h.events(cursor);
    REQUIRE(findEvent(events, GameEventKind::PlayerAlive) != nullptr);
    CHECK_FALSE(h.play.death().dead);
    CHECK_FALSE(h.play.death().corpseKnown);
}

TEST_CASE("Gameplay: experience, level up, malformed and foreign packets") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    h.apply(created(kWolf, game::ObjectType::Unit, {{uf::UNIT_FIELD_HEALTH, 0}})); // a known victim: named
    u32 cursor = h.play.events().end();
    Bytes xp;
    net::ByteWriter(xp).u64v(kWolf).u32v(90).u8v(0).u32v(45).f32v(1.0f);
    REQUIRE(h.packet(Opcode::SMSG_LOG_XPGAIN, xp) == game::Gameplay::Handled::Yes);
    Bytes level;
    net::ByteWriter l(level);
    l.u32v(4).u32v(17).u32v(22);
    for (u32 i = 0; i < 9; ++i) {
        l.u32v(1);
    }
    REQUIRE(h.packet(Opcode::SMSG_LEVELUP_INFO, level) == game::Gameplay::Handled::Yes);
    const std::vector<game::GameEvent> events = h.events(cursor);
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == GameEventKind::XpGained);
    CHECK(events[0].guid == kWolf);
    CHECK(events[0].id == 0);
    CHECK(events[0].value[0] == 90);
    CHECK(events[0].value[1] == 45); // without the rested bonus
    CHECK(std::bit_cast<f32>(events[0].value[2]) == 1.0f);
    CHECK(events[1].kind == GameEventKind::LevelUp);
    CHECK(events[1].value[0] == 4);
    CHECK(h.play.lastLevelUp().healthGain == 17);

    // Truncated: refused, nothing changes. Not a gameplay packet: not consumed.
    CHECK(h.packet(Opcode::SMSG_LOOT_RESPONSE, Bytes{1, 2, 3}) == game::Gameplay::Handled::Malformed);
    CHECK_FALSE(h.play.loot().open());
    CHECK(h.packet(Opcode::SMSG_GOSSIP_MESSAGE, Bytes{1}) == game::Gameplay::Handled::Malformed);
    CHECK_FALSE(h.play.dialog().open());
    CHECK(h.packet(Opcode::SMSG_MONSTER_MOVE, Bytes{}) == game::Gameplay::Handled::No);
}

TEST_CASE("Quest and gossip text placeholders") {
    game::TextSubject subject{"Arthel", 1, 8, 0};
    std::string out;
    game::expandText("Greetings, $N.$BGo, $G lad:lass;!", subject, out);
    CHECK(out == "Greetings, Arthel.\nGo, lad!");
    subject.gender = 1;
    game::expandText("$Gsir:madam;", subject, out);
    CHECK(out == "madam");
    game::expandText("100$ and $", subject, out); // not a placeholder
    CHECK(out == "100$ and $");

    // $R / $C: the names of the client's tables (game/CharacterNames: ChrRaces, ChrClasses of the baked db.vdpk).
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok() || !game::loadCharacterNames(db)) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping the race and class names");
        return;
    }
    subject.gender = 0;
    game::expandText("Greetings, $N the $c. You are a $R $G lad:lass;.", subject, out);
    CHECK(out == "Greetings, Arthel the mage. You are a Human lad.");
    CHECK(game::raceName(4) == "Night Elf");
    CHECK(game::raceName(5) == "Undead");
    CHECK(game::className(11) == "Druid");
    CHECK(game::className(6).empty()); // no class 6 in 1.12
    CHECK(game::raceName(200).empty());
    game::unloadCharacterNames();
    CHECK(game::raceName(1).empty());
}

TEST_CASE("GameData: spell timing and target kinds from Spell.dbc") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    game::GameData data;
    REQUIRE(data.init(db));
    const std::optional<game::SpellInfo> fireball = data.spell(133); // rank 1
    REQUIRE(fireball.has_value());
    CHECK(fireball->name == "Fireball");
    CHECK(fireball->gcdCategory == 133);
    CHECK(fireball->gcdMs == 1500);
    CHECK(fireball->castTimeMs == 1500);
    CHECK(fireball->school == 2);
    CHECK(fireball->targetKind() == game::SpellTargetKind::Enemy);
    CHECK(fireball->iconHash == assetPathHash("Interface/Icons/Spell_Fire_FlameBolt.blp"));
    CHECK(fireball->rank == "Rank 1");
    // 1.12 keeps these cooldowns on the spell's category (Fire Blast: category 19, 8 s).
    const std::optional<game::SpellInfo> fireBlast = data.spell(2136);
    REQUIRE(fireBlast.has_value());
    CHECK(fireBlast->recoveryMs == 0);
    CHECK(fireBlast->category == 19);
    CHECK(fireBlast->categoryRecoveryMs == 8000);
    CHECK(fireBlast->targetKind() == game::SpellTargetKind::Enemy);
    CHECK(data.spell(2050)->targetKind() == game::SpellTargetKind::Friend); // Lesser Heal
    CHECK(data.spell(168)->targetKind() == game::SpellTargetKind::None);    // Frost Armor
    CHECK(data.spell(10)->targetKind() == game::SpellTargetKind::Location); // Blizzard
    CHECK(data.spell(100)->targetKind() == game::SpellTargetKind::Enemy);   // Charge
    CHECK(data.spell(100)->categoryRecoveryMs == 15000);
    CHECK(data.spell(100)->gcdCategory == 0); // warrior abilities of 1.12 are off the global cooldown

    // Gameplay picks targets and starts cooldowns with it.
    Harness h;
    h.play.setData(&data, nullptr);
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    h.apply(created(kWolf, game::ObjectType::Unit, {{uf::UNIT_FIELD_HEALTH, 10}}));
    h.play.setTarget(kWolf);
    CHECK(h.play.spellTarget(133) == kWolf);
    CHECK(h.play.spellTarget(168) == 0);
    CHECK(h.play.spellTarget(2050) == kPlayer); // a heal with a non-friendly target: on the player
    REQUIRE(h.play.castSpell(2136, kWolf));
    CHECK(h.play.spells().remaining(133, h.now + 500) == 1000); // the global cooldown
    Bytes go;
    net::ByteWriter(go).packedGuid(kPlayer).packedGuid(kPlayer).u32v(2136).u16v(0x100).u8v(1).u64v(kWolf).u8v(0)
        .u16v(net::kTargetUnit).packedGuid(kWolf);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, go) == game::Gameplay::Handled::Yes);
    CHECK(h.play.spells().remaining(2136, h.now + 1000) == 7000);
}
