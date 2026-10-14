// game::Exchange (phase 8): group loot rolls and master loot, the mailbox (list, read, take, delete, return,
// copy, send, the new-mail letter), the auction house (open, lists one at a time, bids, buyouts, sales with
// their deposit, cancels, notifications) and trade (requests both ways, the window, accept, cancel, errors),
// end to end through game::Gameplay with a capturing packet sink; and the client tables (AuctionHouse.dbc,
// MailTemplate.dbc) against the real db.vdpk (VD_PACKS_DIR).
#include "TestData.h"
#include "core/Text.h"
#include "game/Entities.h"
#include "game/Exchange.h"
#include "game/ExchangeData.h"
#include "game/Gameplay.h"
#include "game/ItemActions.h"
#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/ExchangeMessages.h"
#include "net/GameMessages.h"
#include "net/ItemMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
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
using game::ItemPos;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x11;
constexpr u64 kOther = 0x22;
constexpr u64 kFar = 0x33;
constexpr u64 kCorpse = 0xF130000000000321ull;
constexpr u64 kMailbox = 0xF11000000000A001ull;
constexpr u64 kAuctioneer = 0xF130000000000456ull;
constexpr u64 kLinen = 0x4000000000000901ull;
constexpr u64 kRing = 0x4000000000000902ull;
constexpr u64 kBound = 0x4000000000000903ull;
constexpr u32 kLinenEntry = 2589;
constexpr u32 kRingEntry = 11993;
constexpr u32 kBoundEntry = 6948; // Hearthstone

u32 low(u64 guid) {
    return static_cast<u32>(guid);
}
u32 high(u64 guid) {
    return static_cast<u32>(guid >> 32);
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

// SMSG_UPDATE_OBJECT with one create block (UPDATEFLAG_HAS_POSITION).
Bytes created(u64 guid, game::ObjectType type, const Fields& fields, f32 x = 0.0f, f32 y = 0.0f) {
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

Bytes itemObject(u64 guid, u32 entry, u32 count, u32 flags = 0) {
    return created(guid, game::ObjectType::Item,
                   {{uf::OBJECT_FIELD_ENTRY, entry},
                    {uf::ITEM_FIELD_OWNER, low(kPlayer)},
                    {uf::ITEM_FIELD_STACK_COUNT, count},
                    {uf::ITEM_FIELD_FLAGS, flags}});
}

void slot(Fields& f, u8 slotIndex, u64 guid) {
    f.push_back({static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slotIndex * 2), low(guid)});
    f.push_back({static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slotIndex * 2 + 1), high(guid)});
}

// HandleItemQuerySingleOpcode's layout, the fields these tests need.
Bytes itemQuery(u32 entry, const char* name, u32 quality, u32 sellPrice, u32 stackable) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(entry).u32v(7).u32v(5).cstring(name).cstring("").cstring("").cstring("");
    w.u32v(1000).u32v(quality).u32v(0).u32v(sellPrice * 4).u32v(sellPrice).u32v(0);
    w.u32v(0xFFFFFFFFu).u32v(0xFFFFFFFFu).u32v(5).u32v(0);
    for (u32 i = 0; i < 8; ++i) {
        w.u32v(0);
    }
    w.u32v(stackable).u32v(0);
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
        w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    }
    w.u32v(0).cstring("").u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
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
    game::Exchange& ex() { return play.exchange(); }
    void apply(const Bytes& update) {
        createdGuids.clear();
        game::UpdateStats stats;
        stats.createdGuids = &createdGuids;
        REQUIRE(store.applyUpdateObject(update, now, &stats));
        play.afterUpdate(createdGuids, now);
        play.update(now);
        play.inventory().refresh(now, true);
    }
    void tick(u32 ms) {
        now += ms;
        play.update(now);
    }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
    }
    // Bodies sent with `opcode` (in order); everything sent is forgotten.
    std::vector<Bytes> take(Opcode opcode) {
        std::vector<Bytes> out;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out.push_back(s.body);
            }
        }
        sent.clear();
        return out;
    }
    u32 count(Opcode opcode) const {
        return static_cast<u32>(
            std::count_if(sent.begin(), sent.end(), [&](const Sent& s) { return s.opcode == opcode; }));
    }
    std::vector<game::GameEvent> events() {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            out.push_back(*e);
        }
        return out;
    }
    const game::GameEvent* find(const std::vector<game::GameEvent>& list, GameEventKind kind) {
        for (const game::GameEvent& e : list) {
            if (e.kind == kind) {
                m_found = e;
                return &m_found;
            }
        }
        return nullptr;
    }
    // A level-20 character with 20 Linen Cloth in the backpack's first slot, a ring in the second, a bound
    // hearthstone in the third and `money` copper; the templates known.
    void character(u32 money = 10000) {
        apply(itemObject(kLinen, kLinenEntry, 20));
        apply(itemObject(kRing, kRingEntry, 1));
        apply(itemObject(kBound, kBoundEntry, 1, game::kItemDynFlagBound));
        Fields f = {
            {uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_LEVEL, 20}, {uf::PLAYER_FIELD_COINAGE, money}};
        slot(f, 23, kLinen);
        slot(f, 24, kRing);
        slot(f, 25, kBound);
        apply(created(kPlayer, game::ObjectType::Player, f));
        REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                       itemQuery(kLinenEntry, "Linen Cloth", 1, 13, 20)) == game::Gameplay::Handled::Yes);
        REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(kRingEntry, "Ring", 2, 500, 1)) ==
                game::Gameplay::Handled::Yes);
        REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                       itemQuery(kBoundEntry, "Hearthstone", 1, 0, 1)) == game::Gameplay::Handled::Yes);
        sent.clear();
        events();
    }

private:
    game::GameEvent m_found;
};

Bytes guidBody(u64 guid) {
    Bytes out;
    net::writeGuid(out, guid);
    return out;
}

Bytes bytes(std::initializer_list<int> list) {
    Bytes out;
    for (int b : list) {
        out.push_back(static_cast<u8>(b));
    }
    return out;
}

} // namespace

// --- Group loot
// ------------------------------------------------------------------------------------------------

TEST_CASE("Exchange: a group loot roll from start to win, and the other results") {
    Harness h;
    h.character();
    game::LootRolls& rolls = h.ex().rolls();
    Bytes start;
    net::ByteWriter(start).u64v(kCorpse).u32v(2).u32v(kRingEntry).u32v(0).u32v(0).u32v(60000);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_START_ROLL, start) == game::Gameplay::Handled::Yes);
    REQUIRE(rolls.rolls().size() == 1);
    CHECK(rolls.waiting() == 1);
    CHECK(rolls.rolls()[0].itemId == kRingEntry);
    CHECK(rolls.rolls()[0].remainingMs(h.now + 1000) == 59000);
    auto events = h.events();
    const game::GameEvent* e = h.find(events, GameEventKind::LootRollStarted);
    REQUIRE(e);
    CHECK(e->guid == kCorpse);
    CHECK(e->value[0] == 2);
    CHECK(e->value[1] == 60000);

    // The same roll again (a reconnect) does not add a second window.
    REQUIRE(h.packet(Opcode::SMSG_LOOT_START_ROLL, start) == game::Gameplay::Handled::Yes);
    CHECK(rolls.rolls().size() == 1);

    // Need: CMSG_LOOT_ROLL, and the window goes.
    CHECK_FALSE(rolls.roll(kCorpse, 7, net::kRollNeed)); // no such roll
    REQUIRE(rolls.roll(kCorpse, 2, net::kRollNeed));
    CHECK(h.take(Opcode::CMSG_LOOT_ROLL) ==
          std::vector<Bytes>{bytes({0x21, 0x03, 0, 0, 0, 0, 0x30, 0xF1, 2, 0, 0, 0, net::kRollNeed})});
    CHECK(rolls.waiting() == 0);
    CHECK(rolls.rolls().size() == 1); // until its result

    // The server tells everybody's choices, then the dice, then the winner.
    Bytes chose;
    net::ByteWriter(chose)
        .u64v(kCorpse)
        .u32v(2)
        .u64v(kOther)
        .u32v(kRingEntry)
        .u32v(0)
        .u32v(0)
        .u8v(128)
        .u8v(2);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_ROLL, chose) == game::Gameplay::Handled::Yes);
    Bytes dice;
    net::ByteWriter(dice).u64v(kCorpse).u32v(2).u64v(kPlayer).u32v(kRingEntry).u32v(0).u32v(0).u8v(77).u8v(1);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_ROLL, dice) == game::Gameplay::Handled::Yes);
    Bytes won;
    net::ByteWriter(won).u64v(kCorpse).u32v(2).u32v(kRingEntry).u32v(0).u32v(0).u64v(kPlayer).u8v(77).u8v(1);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_ROLL_WON, won) == game::Gameplay::Handled::Yes);
    CHECK(rolls.rolls().empty());
    events = h.events();
    std::vector<game::GameEvent> rolled;
    for (const game::GameEvent& ev : events) {
        if (ev.kind == GameEventKind::LootRolled) {
            rolled.push_back(ev);
        }
    }
    REQUIRE(rolled.size() == 2);
    CHECK(rolled[0].other == kOther);
    CHECK(rolled[0].value[0] == static_cast<i32>(net::LootRollKind::ChoseGreed));
    CHECK(rolled[1].other == kPlayer);
    CHECK(rolled[1].value[0] == static_cast<i32>(net::LootRollKind::NeedRoll));
    CHECK(rolled[1].value[1] == 77);
    e = h.find(events, GameEventKind::LootRollWon);
    REQUIRE(e);
    CHECK(e->other == kPlayer);
    CHECK(e->value[0] == 77);
    CHECK(e->value[1] == net::kRollNeed);
    // The other roller's name is asked for (the chat line shows it).
    CHECK(h.count(Opcode::CMSG_NAME_QUERY) == 1);

    // Everybody passed on another item.
    Bytes start2;
    net::ByteWriter(start2).u64v(kCorpse).u32v(3).u32v(kLinenEntry).u32v(0).u32v(0).u32v(60000);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_START_ROLL, start2) == game::Gameplay::Handled::Yes);
    REQUIRE(rolls.roll(kCorpse, 3, net::kRollPass));
    Bytes passed;
    net::ByteWriter(passed).u64v(kCorpse).u32v(3).u32v(kLinenEntry).u32v(0).u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_ALL_PASSED, passed) == game::Gameplay::Handled::Yes);
    CHECK(rolls.rolls().empty());
    events = h.events();
    REQUIRE(h.find(events, GameEventKind::LootAllPassed));

    // A roll whose result never comes goes a little after its countdown.
    REQUIRE(h.packet(Opcode::SMSG_LOOT_START_ROLL, start2) == game::Gameplay::Handled::Yes);
    h.tick(60000);
    CHECK(rolls.rolls().size() == 1);
    h.tick(game::LootRolls::kGraceMs + 1);
    CHECK(rolls.rolls().empty());

    // Malformed packets are refused.
    CHECK(h.packet(Opcode::SMSG_LOOT_ROLL, bytes({1, 2, 3})) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Exchange: master loot candidates and CMSG_LOOT_MASTER_GIVE") {
    Harness h;
    h.character();
    game::LootRolls& rolls = h.ex().rolls();
    Bytes list;
    net::ByteWriter(list).u8v(2).u64v(kPlayer).u64v(kOther);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_MASTER_LIST, list) == game::Gameplay::Handled::Yes);
    REQUIRE(rolls.masterCandidates().size() == 2);
    CHECK(rolls.masterCandidates()[1] == kOther);
    CHECK(h.count(Opcode::CMSG_NAME_QUERY) == 1); // the other's name for the picker (not the player's own)
    h.sent.clear();
    REQUIRE(rolls.giveMasterLoot(kCorpse, 1, kOther));
    CHECK(h.take(Opcode::CMSG_LOOT_MASTER_GIVE) ==
          std::vector<Bytes>{bytes({0x21, 0x03, 0, 0, 0, 0, 0x30, 0xF1, 1, 0x22, 0, 0, 0, 0, 0, 0, 0})});
    // Closing the loot window forgets them.
    Bytes response;
    net::ByteWriter(response).u64v(kCorpse).u8v(1).u32v(0).u8v(0);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_RESPONSE, response) == game::Gameplay::Handled::Yes);
    h.play.closeLoot();
    CHECK(rolls.masterCandidates().empty());
}

// --- Mail
// ------------------------------------------------------------------------------------------------------

namespace {

// A mailbox with a player's letter (20 Linen Cloth on 1500 copper COD, a body), an auction house's payment
// (read) and an NPC's template letter.
Bytes mailList() {
    Bytes out;
    net::ByteWriter w(out);
    w.u8v(3);
    w.u32v(17).u8v(net::kMailNormal).u64v(kOther).cstring("Linen for you").u32v(905).u32v(0).u32v(41);
    w.u32v(kLinenEntry).u32v(0).u32v(0).u32v(0).u8v(20).u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(1500).u32v(net::kMailFlagHasBody).f32v(29.5f).u32v(0);
    w.u32v(18).u8v(net::kMailAuction).u32v(2).cstring("2589:0:2").u32v(0).u32v(0).u32v(62);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u8v(0).u32v(0).u32v(0).u32v(0);
    w.u32v(4750).u32v(0).u32v(net::kMailFlagRead | net::kMailFlagCopied).f32v(2.25f).u32v(0);
    w.u32v(19).u8v(net::kMailCreature).u32v(466).cstring("").u32v(0).u32v(0).u32v(41);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u8v(0).u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(net::kMailFlagRead).f32v(0.5f).u32v(12);
    return out;
}

Bytes mailResult(u32 id, u32 action, u32 error, u32 extra = 0) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(id).u32v(action).u32v(error);
    if (error == net::kMailErrEquipError) {
        w.u32v(extra);
    } else if (action == net::kMailActionItemTaken) {
        w.u32v(kLinenEntry).u32v(20);
    }
    return out;
}

} // namespace

TEST_CASE("Exchange: the mailbox opens from its object, lists, reads and takes") {
    Harness h;
    // At login the client asks whether mail waits.
    CHECK(h.count(Opcode::MSG_QUERY_NEXT_MAIL_TIME) == 1);
    h.character();
    game::Mail& mail = h.ex().mail();
    Bytes next;
    net::ByteWriter(next).f32v(0.0f);
    REQUIRE(h.packet(Opcode::MSG_QUERY_NEXT_MAIL_TIME, next) == game::Gameplay::Handled::Yes);
    CHECK(mail.hasNewMail());

    h.apply(created(kMailbox, game::ObjectType::GameObject,
                    {{uf::OBJECT_FIELD_ENTRY, 32349}, {uf::GAMEOBJECT_TYPE_ID, game::kGameObjectMailbox}},
                    2.0f));
    h.sent.clear();
    CHECK(h.play.interact(kMailbox) == game::Interaction::Mailbox);
    CHECK(h.count(Opcode::CMSG_GAMEOBJ_USE) == 0); // the client opens it itself
    CHECK(h.take(Opcode::CMSG_GET_MAIL_LIST) == std::vector<Bytes>{guidBody(kMailbox)});
    CHECK(mail.open());
    CHECK_FALSE(mail.loaded());

    REQUIRE(h.packet(Opcode::SMSG_MAIL_LIST_RESULT, mailList()) == game::Gameplay::Handled::Yes);
    CHECK(mail.loaded());
    REQUIRE(mail.messages().size() == 3);
    const game::MailMessage* letter = mail.find(17);
    REQUIRE(letter);
    CHECK(letter->subject == "Linen for you");
    CHECK(letter->hasItem());
    CHECK(letter->cod == 1500);
    CHECK_FALSE(letter->read());
    CHECK(letter->canReturn());
    CHECK_FALSE(letter->canDelete());
    CHECK(letter->canCopyText());
    CHECK(mail.hasNewMail()); // one unread
    CHECK(h.find(h.events(), GameEventKind::MailListed));
    // Names, the item and the NPC are asked for.
    CHECK(h.count(Opcode::CMSG_NAME_QUERY) == 1);
    CHECK(h.count(Opcode::CMSG_CREATURE_QUERY) == 1);
    h.sent.clear();

    // Reading it: marked read (no answer comes), its body asked for once.
    REQUIRE(mail.markRead(17));
    Bytes read;
    net::writeMailAction(read, kMailbox, 17);
    CHECK(h.take(Opcode::CMSG_MAIL_MARK_AS_READ) == std::vector<Bytes>{read});
    CHECK(mail.find(17)->read());
    CHECK_FALSE(mail.hasNewMail());
    CHECK_FALSE(mail.markRead(17)); // already
    CHECK(mail.body(17).empty());
    CHECK(mail.body(17).empty());
    CHECK(h.count(Opcode::CMSG_ITEM_TEXT_QUERY) == 1);
    Bytes text;
    net::ByteWriter(text).u32v(905).cstring("Twenty bolts, as promised.");
    REQUIRE(h.packet(Opcode::SMSG_ITEM_TEXT_QUERY_RESPONSE, text) == game::Gameplay::Handled::Yes);
    CHECK(mail.body(17) == "Twenty bolts, as promised.");
    h.sent.clear();

    // Taking the money of the auction mail, then deleting it.
    // Phase 18 (p3): an auction house's letter can be deleted with its money (InboxItemCanDelete; DELETE_MONEY asks).
    CHECK(mail.find(18)->canDelete());
    REQUIRE(mail.takeMoney(18));
    Bytes money;
    net::writeMailAction(money, kMailbox, 18);
    CHECK(h.take(Opcode::CMSG_MAIL_TAKE_MONEY) == std::vector<Bytes>{money});
    REQUIRE(
        h.packet(Opcode::SMSG_SEND_MAIL_RESULT, mailResult(18, net::kMailActionMoneyTaken, net::kMailOk)) ==
        game::Gameplay::Handled::Yes);
    CHECK(mail.find(18)->money == 0);
    CHECK_FALSE(mail.returnToSender(18)); // not from a player
    REQUIRE(mail.remove(18));
    CHECK(h.count(Opcode::CMSG_MAIL_DELETE) == 1);
    REQUIRE(h.packet(Opcode::SMSG_SEND_MAIL_RESULT, mailResult(18, net::kMailActionDeleted, net::kMailOk)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(mail.find(18));
    CHECK(mail.messages().size() == 2);

    // Taking the COD item: bags full first, then paid.
    h.events();
    REQUIRE(mail.takeItem(17));
    REQUIRE(h.packet(Opcode::SMSG_SEND_MAIL_RESULT,
                     mailResult(17, net::kMailActionItemTaken, net::kMailErrEquipError, 50)) ==
            game::Gameplay::Handled::Yes);
    auto events = h.events();
    const game::GameEvent* e = h.find(events, GameEventKind::MailResult);
    REQUIRE(e);
    CHECK(e->id == 17);
    CHECK(e->value[1] == static_cast<i32>(net::kMailErrEquipError));
    CHECK(e->value[2] == 50);
    CHECK(mail.find(17)->hasItem());
    REQUIRE(mail.takeItem(17));
    REQUIRE(
        h.packet(Opcode::SMSG_SEND_MAIL_RESULT, mailResult(17, net::kMailActionItemTaken, net::kMailOk)) ==
        game::Gameplay::Handled::Yes);
    CHECK_FALSE(mail.find(17)->hasItem());
    CHECK(mail.find(17)->cod == 0);
    CHECK(mail.find(17)->canDelete());

    // Copying the text, returning it.
    h.sent.clear();
    REQUIRE(mail.copyText(17));
    Bytes copy;
    net::writeMailCreateTextItem(copy, kMailbox, 17, 0);
    CHECK(h.take(Opcode::CMSG_MAIL_CREATE_TEXT_ITEM) == std::vector<Bytes>{copy});
    REQUIRE(h.packet(Opcode::SMSG_SEND_MAIL_RESULT,
                     mailResult(17, net::kMailActionMadePermanent, net::kMailOk)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(mail.find(17)->canCopyText());
    REQUIRE(mail.returnToSender(17));
    REQUIRE(h.packet(Opcode::SMSG_SEND_MAIL_RESULT, mailResult(17, net::kMailActionReturned, net::kMailOk)) ==
            game::Gameplay::Handled::Yes);
    CHECK(mail.messages().size() == 1);
    // The NPC letter copies with its template.
    h.sent.clear();
    REQUIRE(mail.copyText(19));
    Bytes copyTemplate;
    net::writeMailCreateTextItem(copyTemplate, kMailbox, 19, 12);
    CHECK(h.take(Opcode::CMSG_MAIL_CREATE_TEXT_ITEM) == std::vector<Bytes>{copyTemplate});

    // New mail while the mailbox is open: the letter lights and the list comes again.
    h.sent.clear();
    REQUIRE(h.packet(Opcode::SMSG_RECEIVED_MAIL, bytes({0, 0, 0, 0})) == game::Gameplay::Handled::Yes);
    CHECK(mail.hasNewMail());
    CHECK(h.find(h.events(), GameEventKind::NewMail));
    CHECK(h.count(Opcode::CMSG_GET_MAIL_LIST) == 1);
    // Closing asks whether unread mail is left.
    h.sent.clear();
    mail.close();
    CHECK_FALSE(mail.open());
    CHECK(h.count(Opcode::MSG_QUERY_NEXT_MAIL_TIME) == 1);
    Bytes none;
    net::ByteWriter(none).f32v(-86400.0f);
    REQUIRE(h.packet(Opcode::MSG_QUERY_NEXT_MAIL_TIME, none) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(mail.hasNewMail());
    CHECK_FALSE(mail.takeMoney(19)); // closed
}

TEST_CASE("Exchange: sending mail with an item, money or COD") {
    Harness h;
    h.character(500);
    game::Mail& mail = h.ex().mail();
    game::MailDraftView d;
    d.to = "Aeryn";
    d.subject = "Linen";
    d.body = "For the bandages.";
    // Why not, as a text of the table (without one loaded each key shows as itself).
    CHECK(mail.sendProblem(d) == text::get("VD_EXCHANGE_NO_MAILBOX"));
    mail.open(kMailbox);
    CHECK(mail.sendProblem(d).empty());
    d.subject = {};
    CHECK(mail.sendProblem(d) == text::get("VD_EXCHANGE_MAIL_NO_SUBJECT"));
    d.subject = "Linen";
    d.money = 471; // + 30 postage > 500
    CHECK(mail.sendProblem(d) == text::get("ERR_NOT_ENOUGH_MONEY"));
    d.money = 470;
    CHECK(mail.sendProblem(d).empty());
    CHECK(game::Mail::cost(d) == 500);
    d.money = 0;
    d.cod = 100;
    CHECK(mail.sendProblem(d) == text::get("VD_EXCHANGE_MAIL_COD_NEEDS_ITEM"));
    d.item = kBound;
    CHECK(mail.sendProblem(d) == text::get("ERR_MAIL_BOUND_ITEM"));
    d.item = kLinen;
    CHECK(mail.sendProblem(d).empty());
    d.cod = net::kMailMaxCod + 1;
    CHECK_FALSE(mail.sendProblem(d).empty());
    if (test::loadTexts()) { // the client's MAIL_COD_ERROR with MailFrame's MAX_COD_AMOUNT
        CHECK(mail.sendProblem(d) == "C.O.D. must be <= 10000");
        text::unload();
    }
    d.cod = 100;
    d.money = 1000; // ignored with COD: only the postage is paid
    CHECK(mail.sendProblem(d).empty());
    h.sent.clear();
    REQUIRE(mail.send(d));
    CHECK(mail.sending());
    CHECK_FALSE(mail.send(d)); // one at a time
    Bytes expected;
    net::ByteWriter(expected).u64v(kMailbox).cstring("Aeryn").cstring("Linen").cstring("For the bandages.");
    net::ByteWriter(expected).u32v(41).u32v(0).u64v(kLinen).u32v(0).u32v(100).u64v(0).u8v(0);
    CHECK(h.take(Opcode::CMSG_SEND_MAIL) == std::vector<Bytes>{expected});
    REQUIRE(h.packet(Opcode::SMSG_SEND_MAIL_RESULT,
                     mailResult(0, net::kMailActionSend, net::kMailErrRecipientNotFound)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(mail.sending());
    const auto events = h.events();
    const game::GameEvent* e = h.find(events, GameEventKind::MailResult);
    REQUIRE(e);
    CHECK(e->value[0] == static_cast<i32>(net::kMailActionSend));
    CHECK(e->value[1] == static_cast<i32>(net::kMailErrRecipientNotFound));
}

// --- Auction house
// ---------------------------------------------------------------------------------------------

namespace {

Bytes auctionPage(std::initializer_list<net::AuctionItem> items, u32 total) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(static_cast<u32>(items.size()));
    for (const net::AuctionItem& a : items) {
        w.u32v(a.id).u32v(a.itemId).u32v(0).u32v(0).u32v(0).u32v(a.count).u32v(0).u64v(a.owner);
        w.u32v(a.startBid).u32v(a.minOutbid).u32v(a.buyout).u32v(a.timeLeftMs).u64v(a.bidder).u32v(a.bid);
    }
    w.u32v(total);
    return out;
}

net::AuctionItem auction(u32 id, u32 itemId, u64 owner, u32 startBid, u32 buyout, u32 bid = 0,
                         u64 bidder = 0) {
    net::AuctionItem a;
    a.id = id;
    a.itemId = itemId;
    a.count = 1;
    a.owner = owner;
    a.startBid = startBid;
    a.buyout = buyout;
    a.bid = bid;
    a.bidder = bidder;
    a.minOutbid = bid ? net::auctionOutbid(bid) : 0;
    a.timeLeftMs = 7200000;
    return a;
}

Bytes commandResult(u32 id, u32 action, u32 error) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(id).u32v(action).u32v(error);
    if (error == net::kAuctionOk && action == net::kAuctionActionBidPlaced) {
        w.u32v(5);
    }
    return out;
}

} // namespace

TEST_CASE("Exchange: the auction house opens, browses one request at a time, bids and buys out") {
    Harness h;
    h.character(10000);
    game::AuctionHouse& ah = h.ex().auction();
    h.apply(created(kAuctioneer, game::ObjectType::Unit,
                    {{uf::OBJECT_FIELD_ENTRY, 8719},
                     {uf::UNIT_FIELD_HEALTH, 100},
                     {uf::UNIT_NPC_FLAGS, game::kNpcFlagAuctioneer}},
                    3.0f));
    h.sent.clear();
    CHECK(h.play.interact(kAuctioneer) == game::Interaction::Auctioneer);
    CHECK(h.take(Opcode::MSG_AUCTION_HELLO) == std::vector<Bytes>{guidBody(kAuctioneer)});
    CHECK_FALSE(ah.open());
    Bytes hello;
    net::ByteWriter(hello).u64v(kAuctioneer).u32v(1);
    REQUIRE(h.packet(Opcode::MSG_AUCTION_HELLO, hello) == game::Gameplay::Handled::Yes);
    REQUIRE(ah.open());
    CHECK(ah.houseId() == 1);
    CHECK(h.find(h.events(), GameEventKind::AuctionOpened));

    // A search, then "my auctions" while it is pending: the second waits for the first answer.
    game::AuctionSearch s;
    s.name = "linen";
    s.itemClass = 7;
    s.usable = true;
    REQUIRE(ah.search(s));
    REQUIRE(ah.listOwned());
    REQUIRE(h.sent.size() == 1);
    net::AuctionQuery q;
    q.name = "linen";
    q.itemClass = 7;
    q.usable = true;
    Bytes query;
    net::writeAuctionListItems(query, kAuctioneer, q);
    CHECK(h.take(Opcode::CMSG_AUCTION_LIST_ITEMS) == std::vector<Bytes>{query});
    CHECK(ah.page(game::AuctionList::Browse).pending);
    CHECK(ah.page(game::AuctionList::Owned).pending);
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_LIST_RESULT,
                     auctionPage({auction(501, kLinenEntry, kOther, 400, 1000),
                                  auction(502, kRingEntry, kOther, 5000, 0, 6000, kFar)},
                                 137)) == game::Gameplay::Handled::Yes);
    const game::AuctionPage& browse = ah.page(game::AuctionList::Browse);
    CHECK(browse.loaded);
    CHECK_FALSE(browse.pending);
    REQUIRE(browse.items.size() == 2);
    CHECK(browse.total == 137);
    CHECK(browse.pages() == 3);
    // ... and now the queued one goes.
    Bytes owner;
    net::writeAuctionListOwnerItems(owner, kAuctioneer, 0);
    CHECK(h.take(Opcode::CMSG_AUCTION_LIST_OWNER_ITEMS) == std::vector<Bytes>{owner});
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_OWNER_LIST_RESULT, auctionPage({}, 0)) ==
            game::Gameplay::Handled::Yes);
    CHECK(ah.page(game::AuctionList::Owned).loaded);
    h.tick(1000);
    CHECK(ah.timeLeftMs(browse, browse.items[0]) == 7200000 - 1000);

    // Bids: under the minimum refused here, a fair one sent; one command at a time.
    CHECK_FALSE(ah.bid(502, 6100)); // 6000 + 300 at least
    CHECK_FALSE(ah.bid(501, 399));
    REQUIRE(ah.bid(501, 450));
    CHECK(ah.busy());
    CHECK_FALSE(ah.bid(501, 500));
    Bytes bid;
    net::writeAuctionPlaceBid(bid, kAuctioneer, 501, 450);
    CHECK(h.take(Opcode::CMSG_AUCTION_PLACE_BID) == std::vector<Bytes>{bid});
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_COMMAND_RESULT,
                     commandResult(501, net::kAuctionActionBidPlaced, net::kAuctionOk)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(ah.busy());
    auto events = h.events();
    const game::GameEvent* e = h.find(events, GameEventKind::AuctionResult);
    REQUIRE(e);
    CHECK(e->id == 501);
    // The browse page and the bids come again, one after the other.
    CHECK(h.take(Opcode::CMSG_AUCTION_LIST_ITEMS).size() == 1);
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_LIST_RESULT,
                     auctionPage({auction(501, kLinenEntry, kOther, 400, 1000, 450, kPlayer)}, 1)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.take(Opcode::CMSG_AUCTION_LIST_BIDDER_ITEMS).size() == 1);
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_BIDDER_LIST_RESULT,
                     auctionPage({auction(501, kLinenEntry, kOther, 400, 1000, 450, kPlayer)}, 1)) ==
            game::Gameplay::Handled::Yes);
    CHECK(ah.page(game::AuctionList::Bids).items.size() == 1);

    // Someone bid higher first: the auction shows it.
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_LIST_RESULT,
                     auctionPage({auction(502, kRingEntry, kOther, 5000, 0, 6000, kFar)}, 1)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(ah.bid(502, 6300));
    Bytes higher;
    net::ByteWriter(higher)
        .u32v(502)
        .u32v(net::kAuctionActionBidPlaced)
        .u32v(net::kAuctionErrHigherBid)
        .u64v(kFar)
        .u32v(7000)
        .u32v(350);
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_COMMAND_RESULT, higher) == game::Gameplay::Handled::Yes);
    REQUIRE(ah.find(502));
    CHECK(ah.find(502)->bid == 7000);
    CHECK(net::auctionMinimumBid(*ah.find(502)) == 7350);

    // A buyout pays the buyout price; too dear is not sent (the server would not answer).
    REQUIRE(
        h.packet(Opcode::SMSG_AUCTION_LIST_RESULT, auctionPage({auction(503, kRingEntry, kOther, 100, 900),
                                                                auction(504, kRingEntry, kOther, 100, 20000)},
                                                               2)) == game::Gameplay::Handled::Yes);
    h.sent.clear();
    CHECK_FALSE(ah.buyout(504));
    REQUIRE(ah.buyout(503));
    Bytes buy;
    net::writeAuctionPlaceBid(buy, kAuctioneer, 503, 900);
    CHECK(h.take(Opcode::CMSG_AUCTION_PLACE_BID) == std::vector<Bytes>{buy});
    // An answer that never comes frees the window after a while.
    h.tick(game::AuctionHouse::kListTimeoutMs + 1);
    CHECK_FALSE(ah.busy());

    // Notifications, wherever the player is.
    Bytes outbid;
    net::ByteWriter(outbid).u32v(1).u32v(501).u64v(kFar).u32v(600).u32v(30).u32v(kLinenEntry).u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_BIDDER_NOTIFICATION, outbid) == game::Gameplay::Handled::Yes);
    Bytes sold;
    net::ByteWriter(sold).u32v(600).u32v(1200).u32v(60).u64v(0).u32v(kRingEntry).u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_OWNER_NOTIFICATION, sold) == game::Gameplay::Handled::Yes);
    events = h.events();
    e = h.find(events, GameEventKind::AuctionOutbid);
    REQUIRE(e);
    CHECK(e->id == kLinenEntry);
    CHECK(e->value[0] == 600);
    e = h.find(events, GameEventKind::AuctionSold);
    REQUIRE(e);
    CHECK(e->value[0] == 1200);

    ah.close();
    CHECK_FALSE(ah.open());
    CHECK_FALSE(ah.search(s));
}

TEST_CASE("Exchange: selling at the auction house with the deposit, and cancelling") {
    Harness h;
    h.character(1000);
    game::AuctionHouse& ah = h.ex().auction();
    Bytes hello;
    net::ByteWriter(hello).u64v(kAuctioneer).u32v(7); // a neutral house: its rate comes from the tables
    REQUIRE(h.packet(Opcode::MSG_AUCTION_HELLO, hello) == game::Gameplay::Handled::Yes);
    // Without the tables the faction houses' 5 %: 13 c x 20 x 4 (8 hours) x 5 % = 52 c.
    CHECK(ah.deposit(kLinen, 1) == 52);
    CHECK(ah.deposit(kLinen, 0) == 13);
    CHECK(ah.sellProblem(kBound, 100, 0) == text::get("ERR_AUCTION_BOUND_ITEM"));
    CHECK(ah.sellProblem(kLinen, 0, 0) == text::get("VD_EXCHANGE_AUCTION_NO_START_PRICE"));
    CHECK(ah.sellProblem(kLinen, 500, 400) == text::get("AUCTION_BUYOUT_ERROR"));
    CHECK(ah.sellProblem(kLinen, 400, 500).empty());
    h.sent.clear();
    REQUIRE(ah.sell(kLinen, 400, 500, 1));
    Bytes sell;
    net::writeAuctionSellItem(sell, kAuctioneer, kLinen, 400, 500, 480);
    CHECK(h.take(Opcode::CMSG_AUCTION_SELL_ITEM) == std::vector<Bytes>{sell});
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_COMMAND_RESULT,
                     commandResult(601, net::kAuctionActionStarted, net::kAuctionOk)) ==
            game::Gameplay::Handled::Yes);
    // "My auctions" comes again, and the player may cancel its own only.
    CHECK(h.take(Opcode::CMSG_AUCTION_LIST_OWNER_ITEMS).size() == 1);
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_OWNER_LIST_RESULT,
                     auctionPage({auction(601, kLinenEntry, kPlayer, 400, 500)}, 1)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(ah.cancel(999));
    REQUIRE(ah.cancel(601));
    Bytes remove;
    net::writeAuctionRemoveItem(remove, kAuctioneer, 601);
    CHECK(h.take(Opcode::CMSG_AUCTION_REMOVE_ITEM) == std::vector<Bytes>{remove});
    // The player cannot bid on its own auction.
    REQUIRE(h.packet(Opcode::SMSG_AUCTION_COMMAND_RESULT,
                     commandResult(601, net::kAuctionActionRemoved, net::kAuctionOk)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(ah.bid(601, 450));
    // A whole day of the ring: 500 c x 12 x 5 %.
    CHECK(ah.deposit(kRing, 2) == 300);
}

// --- Trade
// -----------------------------------------------------------------------------------------------------

namespace {

Bytes tradeStatus(u32 status, u64 guid = 0) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(status);
    if (status == net::kTradeBegin) {
        w.u64v(guid);
    }
    return out;
}

} // namespace

TEST_CASE("Exchange: trading from the request to the completed trade") {
    Harness h;
    h.character(5000);
    game::Trade& trade = h.ex().trade();
    h.apply(created(kOther, game::ObjectType::Player, {{uf::UNIT_FIELD_HEALTH, 100}}, 5.0f));
    h.apply(created(kFar, game::ObjectType::Player, {{uf::UNIT_FIELD_HEALTH, 100}}, 30.0f));
    CHECK(trade.initiateProblem(kFar) == text::get("ERR_TRADE_TOO_FAR"));
    CHECK(trade.initiateProblem(kCorpse) == text::get("PLAYER_NOT_FOUND")); // not a player
    CHECK(trade.initiateProblem(kOther).empty());
    h.sent.clear();
    REQUIRE(trade.initiate(kOther));
    CHECK(h.take(Opcode::CMSG_INITIATE_TRADE) == std::vector<Bytes>{guidBody(kOther)});
    CHECK(trade.phase() == game::TradePhase::Proposed);
    CHECK(trade.initiateProblem(kOther) == text::get("ERR_ALREADY_TRADING"));

    // The other accepts: both windows open.
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS, tradeStatus(net::kTradeOpenWindow)) ==
            game::Gameplay::Handled::Yes);
    CHECK(trade.open());
    CHECK(trade.partner() == kOther);
    auto events = h.events();
    REQUIRE(h.find(events, GameEventKind::TradeOpened));

    // Items and money.
    REQUIRE(trade.setItem(0, {255, 23}));
    CHECK(h.take(Opcode::CMSG_SET_TRADE_ITEM) == std::vector<Bytes>{bytes({0, 255, 23})});
    CHECK(trade.slotOf(kLinen) == 0);
    CHECK(trade.freeSlot() == 1);
    CHECK_FALSE(trade.setItem(1, {255, 25})); // bound
    CHECK_FALSE(trade.setItem(0, {255, 23})); // already there
    // The same item into another slot leaves the first (twice would cancel the trade on the server).
    REQUIRE(trade.setItem(2, {255, 23}));
    REQUIRE(h.sent.size() == 2);
    CHECK(h.sent[0].opcode == Opcode::CMSG_CLEAR_TRADE_ITEM);
    CHECK(h.sent[0].body == bytes({0}));
    CHECK(h.sent[1].opcode == Opcode::CMSG_SET_TRADE_ITEM);
    CHECK(h.sent[1].body == bytes({2, 255, 23}));
    h.sent.clear();
    CHECK(trade.slotOf(kLinen) == 2);
    CHECK_FALSE(trade.setMoney(6000)); // more than the player has
    REQUIRE(trade.setMoney(2500));
    CHECK(h.take(Opcode::CMSG_SET_TRADE_GOLD) == std::vector<Bytes>{bytes({0xC4, 0x09, 0, 0})});

    // The other side's offer.
    Bytes theirs;
    net::ByteWriter w(theirs);
    w.u8v(1).u32v(7).u32v(7).u32v(100).u32v(0);
    for (u8 i = 0; i < 7; ++i) {
        w.u8v(i);
        if (i == 0) {
            w.u32v(kRingEntry)
                .u32v(9000)
                .u32v(1)
                .u32v(0)
                .u64v(0)
                .u32v(0)
                .u64v(0)
                .u32v(0)
                .u32v(0)
                .u32v(0)
                .u32v(0);
            w.u32v(0).u32v(0);
        } else {
            for (u32 j = 0; j < 15; ++j) {
                w.u32v(0);
            }
        }
    }
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS_EXTENDED, theirs) == game::Gameplay::Handled::Yes);
    CHECK(trade.theirs().money == 100);
    CHECK(trade.theirs().slots[0].itemId == kRingEntry);

    // Accepting: the other accepted first, the player then; a change withdraws both.
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS, tradeStatus(net::kTradeAccept)) ==
            game::Gameplay::Handled::Yes);
    CHECK(trade.theirs().accepted);
    REQUIRE(trade.accept());
    CHECK(h.take(Opcode::CMSG_ACCEPT_TRADE) == std::vector<Bytes>{bytes({1, 0, 0, 0})});
    CHECK(trade.mine().accepted);
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS, tradeStatus(net::kTradeBackToTrade)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(trade.mine().accepted);
    CHECK_FALSE(trade.theirs().accepted);
    REQUIRE(trade.accept());
    REQUIRE(trade.unaccept());
    CHECK(h.count(Opcode::CMSG_UNACCEPT_TRADE) == 1);
    REQUIRE(trade.accept());
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS, tradeStatus(net::kTradeComplete)) ==
            game::Gameplay::Handled::Yes);
    CHECK(trade.phase() == game::TradePhase::None);
    events = h.events();
    const game::GameEvent* e = h.find(events, GameEventKind::TradeClosed);
    REQUIRE(e);
    CHECK(e->guid == kOther);
    CHECK(e->value[0] == static_cast<i32>(net::kTradeComplete));
}

TEST_CASE("Exchange: a trade request declined, an error while open, and an item that left the bags") {
    Harness h;
    h.character();
    game::Trade& trade = h.ex().trade();
    // Someone asks.
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS, tradeStatus(net::kTradeBegin, kOther)) ==
            game::Gameplay::Handled::Yes);
    CHECK(trade.phase() == game::TradePhase::Requested);
    CHECK(trade.partner() == kOther);
    auto events = h.events();
    const game::GameEvent* e = h.find(events, GameEventKind::TradeRequested);
    REQUIRE(e);
    CHECK(e->guid == kOther);
    h.sent.clear();
    trade.declineRequest();
    CHECK(h.count(Opcode::CMSG_CANCEL_TRADE) == 1);
    CHECK(trade.phase() == game::TradePhase::None);
    // The server's TRADE_CANCELED that follows changes nothing.
    events = h.events();
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS, tradeStatus(net::kTradeCanceled)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(h.find(h.events(), GameEventKind::TradeClosed));

    // Accepted this time; then the partner walks away while accepting: the window closes and tells the
    // server.
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS, tradeStatus(net::kTradeBegin, kOther)) ==
            game::Gameplay::Handled::Yes);
    h.sent.clear();
    trade.acceptRequest();
    CHECK(h.count(Opcode::CMSG_BEGIN_TRADE) == 1);
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS, tradeStatus(net::kTradeOpenWindow)) ==
            game::Gameplay::Handled::Yes);
    CHECK(trade.partner() == kOther);
    REQUIRE(trade.setItem(0, {255, 24}));
    // The ring leaves the bags (sold, say): no longer offered.
    Fields moved;
    slot(moved, 24, 0);
    h.apply(values(kPlayer, moved));
    h.tick(16); // the next frame sees the rebuilt inventory
    CHECK(trade.slotOf(kRing) == -1);
    h.sent.clear();
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS, tradeStatus(net::kTradeTargetTooFar)) ==
            game::Gameplay::Handled::Yes);
    CHECK(trade.phase() == game::TradePhase::None);
    CHECK(h.count(Opcode::CMSG_CANCEL_TRADE) == 1);
    e = h.find(h.events(), GameEventKind::TradeClosed);
    REQUIRE(e);
    CHECK(e->value[0] == static_cast<i32>(net::kTradeTargetTooFar));
    // Errors to a request the player made are only shown.
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS, tradeStatus(net::kTradeBusy)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.count(Opcode::CMSG_CANCEL_TRADE) == 1);
}

// --- Client tables
// ---------------------------------------------------------------------------------------------

TEST_CASE("ExchangeData: AuctionHouse.dbc rates and MailTemplate.dbc subjects of the real client") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    const std::filesystem::path path = dir ? std::filesystem::path(dir) / "db.vdpk" : std::filesystem::path{};
    if (!dir || !std::filesystem::exists(path)) {
        MESSAGE("VD_PACKS_DIR without db.vdpk: skipped");
        return;
    }
    pack::PackReader db;
    REQUIRE(db.open(path));
    game::ExchangeData data;
    REQUIRE(data.init(db));
    REQUIRE(data.ready());
    // The faction houses (1 to 6) take 5 % (per 2 hours) and cut 5 %; the goblins' (7) 25 % and 15 %.
    const game::ExchangeData::House alliance = data.house(1);
    CHECK(alliance.known);
    CHECK(alliance.depositPercent == 5);
    CHECK(alliance.cutPercent == 5);
    const game::ExchangeData::House neutral = data.house(7);
    CHECK(neutral.known);
    CHECK(neutral.depositPercent == 25);
    CHECK(neutral.cutPercent == 15);
    CHECK_FALSE(neutral.name.empty());
    CHECK_FALSE(data.house(999).known);
    // The auction house's categories: every class and subclass has its name.
    CHECK(data.itemClassName(game::kItemClassWeapon) == "Weapon");
    CHECK(data.itemClassName(game::kItemClassArmor) == "Armor");
    CHECK(data.itemClassName(game::kItemClassJunk) == "Miscellaneous");
    CHECK(data.itemSubclassName(game::kItemClassWeapon, 7) == "One-Handed Swords");
    CHECK(data.itemSubclassName(game::kItemClassArmor, 1) == "Cloth");
    CHECK(data.itemSubclassName(game::kItemClassTradeGoods, 1) == "Parts");
    CHECK(data.itemSubclassName(game::kItemClassQuiver, 3) == "Ammo Pouch");
    CHECK(data.itemSubclassName(99, 0).empty());
    for (const game::AuctionCategory& category : game::auctionCategories()) {
        INFO("class " << category.itemClass << " subclass " << category.subclass);
        CHECK_FALSE(data.itemClassName(category.itemClass).empty());
        if (category.subclass != net::kAuctionAny) {
            CHECK_FALSE(data.itemSubclassName(category.itemClass, category.subclass).empty());
        }
    }
    // Some template letter has a subject.
    bool anySubject = false;
    for (u32 id = 1; id < 200 && !anySubject; ++id) {
        anySubject = !data.mailTemplateSubject(id).empty();
    }
    CHECK(anySubject);
}

// --- The item actions and drops (game/ItemActions)
// ------------------------------------------------------------

TEST_CASE("Exchange: the item tooltip offers Attach, Auction and Trade, and drops reach the windows") {
    Harness h;
    h.character();
    game::Exchange& ex = h.ex();
    const game::ItemPlace linen = game::ItemPlace::slot({255, 23});
    const game::ItemPlace bound = game::ItemPlace::slot({255, 25});
    std::array<game::ItemAction, 16> actions{};
    const auto offered = [&](const game::ItemPlace& place, game::ItemAction action) {
        const u32 n = game::itemActions(h.play, place, actions);
        return std::find(actions.begin(), actions.begin() + n, action) != actions.begin() + n;
    };
    CHECK_FALSE(offered(linen, game::ItemAction::Attach));

    // The mailbox: "Adjuntar" first, not for bound items, not twice; a drop on the attachment does the same.
    ex.mail().open(kMailbox);
    REQUIRE(game::itemActions(h.play, linen, actions) > 0);
    CHECK(actions[0] == game::ItemAction::Attach);
    CHECK(game::actionLabel(game::ItemAction::Attach) == text::get("VD_INV_ATTACH"));
    if (test::loadTexts()) {
        CHECK(game::actionLabel(game::ItemAction::Attach) == "Attach");
        CHECK(game::actionLabel(game::ItemAction::Trade) == "Trade");
        text::unload();
    }
    CHECK_FALSE(offered(bound, game::ItemAction::Attach));
    REQUIRE(game::doItemAction(h.play, linen, game::ItemAction::Attach));
    CHECK(ex.mail().attachment() == kLinen);
    CHECK_FALSE(offered(linen, game::ItemAction::Attach));
    REQUIRE(game::dropItem(h.play, game::ItemPlace::of(game::ItemPlace::Kind::MailAttachment),
                           game::ItemPlace::of(game::ItemPlace::Kind::Bags)));
    CHECK(ex.mail().attachment() == 0);
    CHECK_FALSE(game::dropItem(h.play, bound, game::ItemPlace::of(game::ItemPlace::Kind::MailAttachment)));
    REQUIRE(game::dropItem(h.play, linen, game::ItemPlace::of(game::ItemPlace::Kind::MailAttachment)));
    CHECK(ex.mail().attachment() == kLinen);
    ex.mail().close();
    CHECK(ex.mail().attachment() == 0);

    // The auction house's sale slot.
    Bytes hello;
    net::ByteWriter(hello).u64v(kAuctioneer).u32v(1);
    REQUIRE(h.packet(Opcode::MSG_AUCTION_HELLO, hello) == game::Gameplay::Handled::Yes);
    CHECK(offered(linen, game::ItemAction::Auction));
    REQUIRE(game::doItemAction(h.play, game::ItemPlace::slot({255, 24}), game::ItemAction::Auction));
    CHECK(ex.auction().staged() == kRing);
    REQUIRE(game::dropItem(h.play, linen, game::ItemPlace::of(game::ItemPlace::Kind::AuctionSale)));
    CHECK(ex.auction().staged() == kLinen);
    ex.auction().close();
    CHECK(ex.auction().staged() == 0);

    // The trade window: into the first free slot, onto a chosen slot, between slots and back out.
    REQUIRE(h.packet(Opcode::SMSG_TRADE_STATUS, bytes({net::kTradeOpenWindow, 0, 0, 0})) ==
            game::Gameplay::Handled::Yes);
    CHECK(offered(linen, game::ItemAction::Trade));
    CHECK_FALSE(offered(bound, game::ItemAction::Trade));
    h.sent.clear();
    REQUIRE(game::doItemAction(h.play, linen, game::ItemAction::Trade));
    CHECK(h.take(Opcode::CMSG_SET_TRADE_ITEM) == std::vector<Bytes>{bytes({0, 255, 23})});
    CHECK_FALSE(offered(linen, game::ItemAction::Trade)); // already offered
    REQUIRE(game::dropItem(h.play, game::ItemPlace::slot({255, 24}),
                           game::ItemPlace::of(game::ItemPlace::Kind::Trade, 6)));
    CHECK(h.take(Opcode::CMSG_SET_TRADE_ITEM) == std::vector<Bytes>{bytes({6, 255, 24})});
    REQUIRE(game::dropItem(h.play, game::ItemPlace::of(game::ItemPlace::Kind::Trade, 0),
                           game::ItemPlace::of(game::ItemPlace::Kind::Trade, 3)));
    CHECK(ex.trade().slotOf(kLinen) == 3);
    h.sent.clear();
    REQUIRE(game::dropItem(h.play, game::ItemPlace::of(game::ItemPlace::Kind::Trade, 3),
                           game::ItemPlace::of(game::ItemPlace::Kind::Bags)));
    CHECK(h.take(Opcode::CMSG_CLEAR_TRADE_ITEM) == std::vector<Bytes>{bytes({3})});
    CHECK(ex.trade().slotOf(kLinen) == -1);
}

TEST_CASE("Exchange: the loot window's group behaviour (rolls, master looter, locked items)") {
    Harness h;
    h.character();
    // Player::SendLoot under group loot: an item free to take, one being rolled for, one for the master
    // looter and one locked (LOOT_SLOT_TYPE_*).
    Bytes response;
    net::ByteWriter w(response);
    w.u64v(kCorpse).u8v(1).u32v(0).u8v(4);
    const u8 types[4] = {0, 1, 2, 3};
    for (u8 i = 0; i < 4; ++i) {
        w.u8v(i).u32v(kLinenEntry + i).u32v(1).u32v(1000).u32v(0).u32v(0).u8v(types[i]);
    }
    REQUIRE(h.packet(Opcode::SMSG_LOOT_RESPONSE, response) == game::Gameplay::Handled::Yes);
    REQUIRE(h.play.loot().open());
    CHECK(h.play.loot().find(0)->lootable());
    CHECK_FALSE(h.play.loot().find(1)->lootable());
    CHECK_FALSE(h.play.loot().find(2)->lootable());
    CHECK_FALSE(h.play.loot().find(3)->lootable());
    // "Take all" takes only what the player may.
    h.sent.clear();
    h.play.lootAll();
    CHECK(h.take(Opcode::CMSG_AUTOSTORE_LOOT_ITEM) == std::vector<Bytes>{bytes({0})});
    // Everybody passed on the rolled item: it may be taken now (from this loot window only).
    Bytes otherCorpse;
    net::ByteWriter(otherCorpse).u64v(kCorpse + 1).u32v(1).u32v(kLinenEntry + 1).u32v(0).u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_ALL_PASSED, otherCorpse) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(h.play.loot().find(1)->lootable());
    Bytes passed;
    net::ByteWriter(passed).u64v(kCorpse).u32v(1).u32v(kLinenEntry + 1).u32v(0).u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_ALL_PASSED, passed) == game::Gameplay::Handled::Yes);
    CHECK(h.play.loot().find(1)->lootable());
    CHECK_FALSE(h.play.loot().find(2)->lootable()); // still the master looter's
    // The winner's item leaves the window.
    REQUIRE(h.packet(Opcode::SMSG_LOOT_REMOVED, bytes({2})) == game::Gameplay::Handled::Yes);
    CHECK(h.play.loot().find(2)->taken);
}
