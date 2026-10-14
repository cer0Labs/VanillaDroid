// Group loot rolls, master loot, mail, auction house and trade packets (phase 8): every parser against
// payloads written field by field as vmangos-core 4641790 writes them (build 5875), every strict prefix
// refused, and every builder against the exact bytes the server reads (Server/Packets/Loot.cpp, Group.cpp,
// Mail.cpp, AuctionHouse.cpp, Trade.cpp; Handlers/MailHandler.cpp, TradeHandler.cpp;
// AuctionHouse/AuctionHouseMgr.cpp).
#include "net/ByteBuffer.h"
#include "net/ExchangeMessages.h"

#include <doctest/doctest.h>

#include <functional>
#include <ostream>
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

constexpr u64 kCorpse = 0xF130000000000321ull;
constexpr u64 kPlayer = 0x11;
constexpr u64 kOther = 0x22;
constexpr u64 kMailbox = 0xF11000000000A001ull;
constexpr u64 kAuctioneer = 0xF130000000000456ull;
constexpr u64 kItem = 0x4000000000000888ull;

} // namespace

// --- Group loot
// ------------------------------------------------------------------------------------------------

TEST_CASE("Loot rolls: SMSG_LOOT_START_ROLL, SMSG_LOOT_ROLL, SMSG_LOOT_ROLL_WON, SMSG_LOOT_ALL_PASSED") {
    {
        Bytes p;
        ByteWriter(p).u64v(kCorpse).u32v(2).u32v(2140).u32v(0).u32v(1015).u32v(60000);
        LootStartRoll m;
        REQUIRE(parseLootStartRoll(p, m));
        CHECK(m.object == kCorpse);
        CHECK(m.slot == 2);
        CHECK(m.itemId == 2140);
        CHECK(m.randomPropertyId == 1015);
        CHECK(m.countdownMs == 60000);
        checkTruncations(p, [](std::span<const u8> s) {
            LootStartRoll x;
            return parseLootStartRoll(s, x);
        });
    }
    {
        // Group::SendLootRoll: object, slot, roller, item, suffix 0, property, number, type.
        Bytes p;
        ByteWriter(p).u64v(kCorpse).u32v(1).u64v(kOther).u32v(2140).u32v(0).u32v(0).u8v(87).u8v(kRollGreed);
        LootRoll m;
        REQUIRE(parseLootRoll(p, m));
        CHECK(m.object == kCorpse);
        CHECK(m.slot == 1);
        CHECK(m.roller == kOther);
        CHECK(m.itemId == 2140);
        CHECK(m.number == 87);
        CHECK(m.kind() == LootRollKind::GreedRoll);
        checkTruncations(p, [](std::span<const u8> s) {
            LootRoll x;
            return parseLootRoll(s, x);
        });
    }
    // The numbers and types Group::CountRollVote and CountTheRoll send.
    const auto kindOf = [](u8 number, u8 type) {
        LootRoll r;
        r.number = number;
        r.type = type;
        return r.kind();
    };
    CHECK(kindOf(128, 128) == LootRollKind::Passed);
    CHECK(kindOf(0, 0) == LootRollKind::ChoseNeed);
    CHECK(kindOf(128, kRollGreed) == LootRollKind::ChoseGreed);
    CHECK(kindOf(1, kRollNeed) == LootRollKind::NeedRoll);
    CHECK(kindOf(100, kRollNeed) == LootRollKind::NeedRoll);
    CHECK(kindOf(42, kRollGreed) == LootRollKind::GreedRoll);
    CHECK(kindOf(0, kRollNeed) == LootRollKind::Unknown);
    {
        // Group::SendLootRollWon: object, slot, item, suffix, property, winner, number, type.
        Bytes p;
        ByteWriter(p).u64v(kCorpse).u32v(1).u32v(2140).u32v(0).u32v(0).u64v(kPlayer).u8v(93).u8v(kRollNeed);
        LootRollWon m;
        REQUIRE(parseLootRollWon(p, m));
        CHECK(m.slot == 1);
        CHECK(m.itemId == 2140);
        CHECK(m.winner == kPlayer);
        CHECK(m.number == 93);
        CHECK(m.type == kRollNeed);
        checkTruncations(p, [](std::span<const u8> s) {
            LootRollWon x;
            return parseLootRollWon(s, x);
        });
    }
    {
        // Group::SendLootAllPassed: the property before the suffix.
        Bytes p;
        ByteWriter(p).u64v(kCorpse).u32v(3).u32v(2140).u32v(1015).u32v(0);
        LootAllPassed m;
        REQUIRE(parseLootAllPassed(p, m));
        CHECK(m.slot == 3);
        CHECK(m.itemId == 2140);
        CHECK(m.randomPropertyId == 1015);
        checkTruncations(p, [](std::span<const u8> s) {
            LootAllPassed x;
            return parseLootAllPassed(s, x);
        });
    }
}

TEST_CASE("Master loot: SMSG_LOOT_MASTER_LIST, CMSG_LOOT_MASTER_GIVE and CMSG_LOOT_ROLL") {
    Bytes p;
    ByteWriter(p).u8v(2).u64v(kPlayer).u64v(kOther);
    LootMasterList m;
    REQUIRE(parseLootMasterList(p, m));
    REQUIRE(m.count == 2);
    CHECK(m.list()[0] == kPlayer);
    CHECK(m.list()[1] == kOther);
    checkTruncations(p, [](std::span<const u8> s) {
        LootMasterList x;
        return parseLootMasterList(s, x);
    });
    Bytes tooMany;
    ByteWriter(tooMany).u8v(41);
    for (u32 i = 0; i < 41; ++i) {
        ByteWriter(tooMany).u64v(i + 1);
    }
    CHECK_FALSE(parseLootMasterList(tooMany, m));

    // LootRoll::ReadFromWorldPacket: object, slot (u32), vote (u8).
    Bytes roll;
    writeLootRoll(roll, kCorpse, 2, kRollNeed);
    CHECK(roll == bytes({0x21, 0x03, 0, 0, 0, 0, 0x30, 0xF1, 2, 0, 0, 0, 1}));
    // LootMasterGive: object, slot (u8), player.
    Bytes give;
    writeLootMasterGive(give, kCorpse, 3, kOther);
    CHECK(give == bytes({0x21, 0x03, 0, 0, 0, 0, 0x30, 0xF1, 3, 0x22, 0, 0, 0, 0, 0, 0, 0}));
}

// --- Mail
// ------------------------------------------------------------------------------------------------------

namespace {

// HandleGetMailList: three messages, one of each sender form, with an item, money and COD where the fields
// show.
Bytes mailList() {
    Bytes out;
    ByteWriter w(out);
    w.u8v(3);
    // A player's letter with an item on COD.
    w.u32v(17).u8v(kMailNormal).u64v(kOther).cstring("Linen for you").u32v(905).u32v(0).u32v(41);
    w.u32v(2589).u32v(0).u32v(0).u32v(0).u8v(20).u32v(0).u32v(0).u32v(0); // 20 Linen Cloth
    w.u32v(0).u32v(1500).u32v(kMailFlagHasBody).f32v(29.5f).u32v(0);
    // An auction house's payment (the subject is "item:0:result").
    w.u32v(18).u8v(kMailAuction).u32v(2).cstring("2589:0:2").u32v(0).u32v(0).u32v(62);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u8v(0).u32v(0).u32v(0).u32v(0);
    w.u32v(4750).u32v(0).u32v(kMailFlagRead | kMailFlagCopied).f32v(2.25f).u32v(0);
    // An NPC's letter with a template.
    w.u32v(19).u8v(kMailCreature).u32v(466).cstring("").u32v(0).u32v(0).u32v(41);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u8v(0).u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).f32v(0.5f).u32v(12);
    return out;
}

} // namespace

TEST_CASE("Mail: SMSG_MAIL_LIST_RESULT") {
    const Bytes p = mailList();
    MailList list;
    list.messages.reserve(kMaxMailMessages);
    REQUIRE(parseMailList(p, list));
    REQUIRE(list.messages.size() == 3);
    const MailEntry& a = list.messages[0];
    CHECK(a.id == 17);
    CHECK(a.type == kMailNormal);
    CHECK(a.sender == kOther);
    CHECK(a.subject == "Linen for you");
    CHECK(a.itemTextId == 905);
    CHECK(a.stationery == 41);
    CHECK(a.item.entry == 2589);
    CHECK(a.item.count == 20);
    CHECK(a.cod == 1500);
    CHECK(a.flags == kMailFlagHasBody);
    CHECK(a.daysLeft == doctest::Approx(29.5f));
    const MailEntry& b = list.messages[1];
    CHECK(b.type == kMailAuction);
    CHECK(b.senderEntry == 2);
    CHECK(b.sender == 0);
    CHECK(b.subject == "2589:0:2");
    CHECK(b.money == 4750);
    CHECK(b.flags == (kMailFlagRead | kMailFlagCopied));
    const MailEntry& c = list.messages[2];
    CHECK(c.type == kMailCreature);
    CHECK(c.senderEntry == 466);
    CHECK(c.templateId == 12);
    CHECK(list.messages.capacity() >= kMaxMailMessages); // parsing reused the storage
    checkTruncations(p, [](std::span<const u8> s) {
        MailList x;
        return parseMailList(s, x);
    });
    // An empty mailbox, and a sender type the server never writes.
    CHECK(parseMailList(bytes({0}), list));
    CHECK(list.messages.empty());
    Bytes bad;
    ByteWriter(bad).u8v(1).u32v(1).u8v(1);
    CHECK_FALSE(parseMailList(bad, list));
}

TEST_CASE("Mail: SMSG_SEND_MAIL_RESULT, MSG_QUERY_NEXT_MAIL_TIME and the client's bodies") {
    SendMailResult m;
    {
        Bytes p;
        ByteWriter(p).u32v(0).u32v(kMailActionSend).u32v(kMailErrRecipientNotFound);
        REQUIRE(parseSendMailResult(p, m));
        CHECK(m.action == kMailActionSend);
        CHECK(m.error == kMailErrRecipientNotFound);
        checkTruncations(p, [](std::span<const u8> s) {
            SendMailResult x;
            return parseSendMailResult(s, x);
        });
    }
    {
        Bytes p; // an item taken: its entry and count follow
        ByteWriter(p).u32v(17).u32v(kMailActionItemTaken).u32v(kMailOk).u32v(2589).u32v(20);
        REQUIRE(parseSendMailResult(p, m));
        CHECK(m.mailId == 17);
        CHECK(m.itemId == 2589);
        CHECK(m.count == 20);
        checkTruncations(p, [](std::span<const u8> s) {
            SendMailResult x;
            return parseSendMailResult(s, x);
        });
    }
    {
        Bytes p; // bags full: the InventoryResult follows instead
        ByteWriter(p).u32v(17).u32v(kMailActionItemTaken).u32v(kMailErrEquipError).u32v(50);
        REQUIRE(parseSendMailResult(p, m));
        CHECK(m.error == kMailErrEquipError);
        CHECK(m.equipError == 50);
        CHECK(m.itemId == 0);
        checkTruncations(p, [](std::span<const u8> s) {
            SendMailResult x;
            return parseSendMailResult(s, x);
        });
    }
    f32 next = 1.0f;
    Bytes time;
    ByteWriter(time).f32v(-86400.0f);
    REQUIRE(parseNextMailTime(time, next));
    CHECK(next == -86400.0f);
    CHECK_FALSE(parseNextMailTime(bytes({0, 0}), next));

    Bytes action;
    writeMailAction(action, kMailbox, 17);
    CHECK(action == bytes({0x01, 0xA0, 0, 0, 0, 0, 0x10, 0xF1, 17, 0, 0, 0}));
    Bytes copy;
    writeMailCreateTextItem(copy, kMailbox, 17, 12);
    CHECK(copy == bytes({0x01, 0xA0, 0, 0, 0, 0, 0x10, 0xF1, 17, 0, 0, 0, 12, 0, 0, 0}));

    // SendMail::ReadFromWorldPacket: mailbox, receiver, subject, body, stationery, package, item, money, COD,
    // u64, u8.
    MailDraft draft;
    draft.to = "Aeryn";
    draft.subject = "Hi";
    draft.body = "x";
    draft.item = kItem;
    draft.money = 100;
    draft.cod = 0;
    Bytes send;
    writeSendMail(send, kMailbox, draft);
    Bytes expected;
    ByteWriter(expected).u64v(kMailbox).cstring("Aeryn").cstring("Hi").cstring("x").u32v(41).u32v(0).u64v(
        kItem);
    ByteWriter(expected).u32v(100).u32v(0).u64v(0).u8v(0);
    CHECK(send == expected);
    CHECK(send.size() == 8 + 6 + 3 + 2 + 4 + 4 + 8 + 4 + 4 + 8 + 1);
}

// --- Auction house
// ---------------------------------------------------------------------------------------------

namespace {

// AuctionEntry::BuildAuctionInfo, then the total.
Bytes auctionList() {
    Bytes out;
    ByteWriter w(out);
    w.u32v(2);
    w.u32v(501).u32v(2589).u32v(0).u32v(0).u32v(0).u32v(20).u32v(0).u64v(kOther);
    w.u32v(400).u32v(0).u32v(1000).u32v(7200000).u64v(0).u32v(0); // no bid yet
    w.u32v(502).u32v(14047).u32v(0).u32v(static_cast<u32>(-15)).u32v(0).u32v(1).u32v(0).u64v(kPlayer);
    w.u32v(5000).u32v(300).u32v(0).u32v(1800000).u64v(kOther).u32v(6000);
    w.u32v(137);
    return out;
}

} // namespace

TEST_CASE("Auction: MSG_AUCTION_HELLO and the three list results") {
    Bytes hello;
    ByteWriter(hello).u64v(kAuctioneer).u32v(2);
    AuctionHello h;
    REQUIRE(parseAuctionHello(hello, h));
    CHECK(h.auctioneer == kAuctioneer);
    CHECK(h.houseId == 2);
    checkTruncations(hello, [](std::span<const u8> s) {
        AuctionHello x;
        return parseAuctionHello(s, x);
    });

    const Bytes p = auctionList();
    AuctionListResult list;
    list.items.reserve(kAuctionPageSize);
    REQUIRE(parseAuctionList(p, list));
    REQUIRE(list.items.size() == 2);
    CHECK(list.total == 137);
    const AuctionItem& a = list.items[0];
    CHECK(a.id == 501);
    CHECK(a.itemId == 2589);
    CHECK(a.count == 20);
    CHECK(a.owner == kOther);
    CHECK(a.startBid == 400);
    CHECK(a.buyout == 1000);
    CHECK(a.timeLeftMs == 7200000);
    CHECK(a.bid == 0);
    CHECK(auctionMinimumBid(a) == 400);
    const AuctionItem& b = list.items[1];
    CHECK(b.randomPropertyId == static_cast<u32>(-15));
    CHECK(b.bidder == kOther);
    CHECK(b.bid == 6000);
    CHECK(b.minOutbid == 300);
    CHECK(auctionMinimumBid(b) == 6300);
    checkTruncations(p, [](std::span<const u8> s) {
        AuctionListResult x;
        return parseAuctionList(s, x);
    });
    // A count the payload cannot hold.
    Bytes lying;
    ByteWriter(lying).u32v(1000000).u32v(0);
    CHECK_FALSE(parseAuctionList(lying, list));
    CHECK(list.items.empty());
}

TEST_CASE("Auction: SMSG_AUCTION_COMMAND_RESULT forms and the notifications") {
    AuctionCommandResult m;
    {
        Bytes p; // a bid placed: the next outbid follows
        ByteWriter(p).u32v(501).u32v(kAuctionActionBidPlaced).u32v(kAuctionOk).u32v(25);
        REQUIRE(parseAuctionCommandResult(p, m));
        CHECK(m.auctionId == 501);
        CHECK(m.outbid == 25);
        checkTruncations(p, [](std::span<const u8> s) {
            AuctionCommandResult x;
            return parseAuctionCommandResult(s, x);
        });
    }
    {
        Bytes p; // started: nothing follows
        ByteWriter(p).u32v(503).u32v(kAuctionActionStarted).u32v(kAuctionOk);
        REQUIRE(parseAuctionCommandResult(p, m));
        CHECK(m.action == kAuctionActionStarted);
        CHECK(m.outbid == 0);
    }
    {
        Bytes p;
        ByteWriter(p).u32v(0).u32v(kAuctionActionStarted).u32v(kAuctionErrInventory).u32v(23);
        REQUIRE(parseAuctionCommandResult(p, m));
        CHECK(m.inventoryError == 23);
        checkTruncations(p, [](std::span<const u8> s) {
            AuctionCommandResult x;
            return parseAuctionCommandResult(s, x);
        });
    }
    {
        Bytes p;
        ByteWriter(p)
            .u32v(502)
            .u32v(kAuctionActionBidPlaced)
            .u32v(kAuctionErrHigherBid)
            .u64v(kOther)
            .u32v(7000)
            .u32v(350);
        REQUIRE(parseAuctionCommandResult(p, m));
        CHECK(m.newBidder == kOther);
        CHECK(m.newBid == 7000);
        CHECK(m.outbid == 350);
        checkTruncations(p, [](std::span<const u8> s) {
            AuctionCommandResult x;
            return parseAuctionCommandResult(s, x);
        });
    }
    {
        Bytes p;
        ByteWriter(p).u32v(2).u32v(502).u64v(kPlayer).u32v(0).u32v(1).u32v(14047).u32v(0);
        AuctionBidderNotification n;
        REQUIRE(parseAuctionBidderNotification(p, n));
        CHECK(n.won());
        CHECK(n.itemId == 14047);
        checkTruncations(p, [](std::span<const u8> s) {
            AuctionBidderNotification x;
            return parseAuctionBidderNotification(s, x);
        });
    }
    {
        Bytes p;
        ByteWriter(p).u32v(501).u32v(800).u32v(40).u64v(0).u32v(2589).u32v(0);
        AuctionOwnerNotification n;
        REQUIRE(parseAuctionOwnerNotification(p, n));
        CHECK(n.sold());
        CHECK(n.bid == 800);
        checkTruncations(p, [](std::span<const u8> s) {
            AuctionOwnerNotification x;
            return parseAuctionOwnerNotification(s, x);
        });
    }
    {
        Bytes p;
        ByteWriter(p).u32v(502).u32v(14047).u32v(0);
        AuctionRemovedNotification n;
        REQUIRE(parseAuctionRemovedNotification(p, n));
        CHECK(n.itemId == 14047);
        checkTruncations(p, [](std::span<const u8> s) {
            AuctionRemovedNotification x;
            return parseAuctionRemovedNotification(s, x);
        });
    }
}

TEST_CASE("Auction: the client's bodies, outbid, minimum bid and deposit") {
    AuctionQuery q;
    q.from = 50;
    q.name = "linen";
    q.levelMin = 10;
    q.levelMax = 20;
    q.itemClass = 7;
    q.quality = 2;
    q.usable = true;
    Bytes list;
    writeAuctionListItems(list, kAuctioneer, q);
    Bytes expected;
    ByteWriter(expected)
        .u64v(kAuctioneer)
        .u32v(50)
        .cstring("linen")
        .u8v(10)
        .u8v(20)
        .u32v(kAuctionAny)
        .u32v(7);
    ByteWriter(expected).u32v(kAuctionAny).u32v(2).u8v(1);
    CHECK(list == expected);

    Bytes owner;
    writeAuctionListOwnerItems(owner, kAuctioneer, 0);
    CHECK(owner.size() == 12);
    Bytes bidder;
    const u32 refresh[] = {7, 9};
    writeAuctionListBidderItems(bidder, kAuctioneer, 0, refresh);
    Bytes bidderExpected;
    ByteWriter(bidderExpected).u64v(kAuctioneer).u32v(0).u32v(2).u32v(7).u32v(9);
    CHECK(bidder == bidderExpected);
    Bytes bid;
    writeAuctionPlaceBid(bid, kAuctioneer, 501, 450);
    Bytes bidExpected;
    ByteWriter(bidExpected).u64v(kAuctioneer).u32v(501).u32v(450);
    CHECK(bid == bidExpected);
    Bytes remove;
    writeAuctionRemoveItem(remove, kAuctioneer, 503);
    CHECK(remove.size() == 12);
    Bytes sell;
    writeAuctionSellItem(sell, kAuctioneer, kItem, 400, 1000, 480);
    Bytes sellExpected;
    ByteWriter(sellExpected).u64v(kAuctioneer).u64v(kItem).u32v(400).u32v(1000).u32v(480);
    CHECK(sell == sellExpected);

    // GetAuctionOutBid: 5 per whole 100 copper of the bid, 1 at least.
    CHECK(auctionOutbid(0) == 1);
    CHECK(auctionOutbid(99) == 1);
    CHECK(auctionOutbid(100) == 5);
    CHECK(auctionOutbid(6000) == 300);
    CHECK(auctionOutbid(6099) == 300);
    // GetAuctionDeposit: Linen Cloth (sells for 13 c) x 20 for 8 hours at a faction house's 5 %: 13 * 20 * 4
    // * 0.05.
    CHECK(auctionDeposit(13, 20, 480, 5) == 52);
    CHECK(auctionDeposit(13, 20, 120, 5) == 13);
    CHECK(auctionDeposit(13, 20, 1440, 25) == 780);
    CHECK(auctionDeposit(0, 1, 1440, 5) ==
          0); // what vendors do not buy costs nothing (Auction.Deposit.Min 0)
}

// --- Trade
// -----------------------------------------------------------------------------------------------------

TEST_CASE("Trade: SMSG_TRADE_STATUS forms") {
    TradeStatus m;
    {
        Bytes p;
        ByteWriter(p).u32v(kTradeBegin).u64v(kOther);
        REQUIRE(parseTradeStatus(p, m));
        CHECK(m.status == kTradeBegin);
        CHECK(m.initiator == kOther);
        checkTruncations(p, [](std::span<const u8> s) {
            TradeStatus x;
            return parseTradeStatus(s, x);
        });
    }
    {
        Bytes p;
        ByteWriter(p).u32v(kTradeCloseWindow).u32v(5).u8v(1).u32v(9);
        REQUIRE(parseTradeStatus(p, m));
        CHECK(m.closeResult == 5);
        CHECK(m.closeItemLimitCategory == 9);
        checkTruncations(p, [](std::span<const u8> s) {
            TradeStatus x;
            return parseTradeStatus(s, x);
        });
    }
    {
        Bytes p;
        ByteWriter(p).u32v(kTradeOnlyConjured).u8v(3);
        REQUIRE(parseTradeStatus(p, m));
        CHECK(m.slot == 3);
    }
    REQUIRE(parseTradeStatus(bytes({kTradeAccept, 0, 0, 0}), m));
    CHECK(m.status == kTradeAccept);
    CHECK(m.initiator == 0);
}

TEST_CASE("Trade: SMSG_TRADE_STATUS_EXTENDED and the client's bodies") {
    // SendUpdateTrade: side, 7, 7, money, spell, then each slot's number and 15 words.
    Bytes p;
    ByteWriter w(p);
    w.u8v(1).u32v(7).u32v(7).u32v(12345).u32v(0);
    for (u8 i = 0; i < 7; ++i) {
        w.u8v(i);
        if (i == 1) {
            w.u32v(2589)
                .u32v(7351)
                .u32v(20)
                .u32v(0)
                .u64v(0)
                .u32v(0)
                .u64v(kOther)
                .u32v(0)
                .u32v(0)
                .u32v(0)
                .u32v(0);
            w.u32v(0).u32v(0);
        } else if (i == 6) { // the non-traded slot: an item to enchant
            w.u32v(14047).u32v(12000).u32v(1).u32v(0).u64v(0).u32v(250).u64v(0).u32v(0).u32v(0).u32v(0).u32v(
                0);
            w.u32v(55).u32v(40);
        } else {
            for (u32 j = 0; j < 15; ++j) {
                w.u32v(0);
            }
        }
    }
    REQUIRE(p.size() == 1 + 16 + 7 * 61);
    TradeStatusExtended m;
    REQUIRE(parseTradeStatusExtended(p, m));
    CHECK(m.trader);
    CHECK(m.slotCount == 7);
    CHECK(m.money == 12345);
    CHECK(m.slots[0].empty());
    CHECK(m.slots[1].itemId == 2589);
    CHECK(m.slots[1].displayId == 7351);
    CHECK(m.slots[1].count == 20);
    CHECK(m.slots[1].creator == kOther);
    CHECK(m.slots[6].itemId == 14047);
    CHECK(m.slots[6].enchantment == 250);
    CHECK(m.slots[6].maxDurability == 55);
    CHECK(m.slots[6].durability == 40);
    // Every prefix that cuts a slot short fails; one that ends between slots is a shorter packet.
    for (usize length = 0; length < p.size(); ++length) {
        TradeStatusExtended x;
        const bool between = length >= 17 && (length - 17) % 61 == 0;
        INFO("truncated to " << length);
        CHECK(parseTradeStatusExtended(std::span<const u8>(p.data(), length), x) == between);
    }
    // A slot number out of range.
    Bytes bad;
    ByteWriter(bad).u8v(0).u32v(7).u32v(7).u32v(0).u32v(0).u8v(7);
    for (u32 j = 0; j < 15; ++j) {
        ByteWriter(bad).u32v(0);
    }
    CHECK_FALSE(parseTradeStatusExtended(bad, m));

    Bytes set;
    writeSetTradeItem(set, 2, 19, 4);
    CHECK(set == bytes({2, 19, 4}));
    Bytes accept;
    writeAcceptTrade(accept);
    CHECK(accept == bytes({1, 0, 0, 0}));
}
