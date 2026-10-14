#include "net/ExchangeMessages.h"

namespace vd::net {

// -------------------------------------------------------------------------------------------------------
// Group loot.

bool parseLootStartRoll(std::span<const u8> payload, LootStartRoll& out) {
    ByteReader r(payload);
    out.object = r.u64v();
    out.slot = r.u32v();
    out.itemId = r.u32v();
    out.randomSuffix = r.u32v();
    out.randomPropertyId = r.u32v();
    out.countdownMs = r.u32v();
    return r.ok();
}

LootRollKind LootRoll::kind() const {
    if (type == 128) {
        return LootRollKind::Passed;
    }
    if (type == 0 && number == 0) {
        return LootRollKind::ChoseNeed;
    }
    if (type == kRollGreed && number == 128) {
        return LootRollKind::ChoseGreed;
    }
    if (number >= 1 && number <= 100) {
        if (type == kRollNeed) {
            return LootRollKind::NeedRoll;
        }
        if (type == kRollGreed) {
            return LootRollKind::GreedRoll;
        }
    }
    return LootRollKind::Unknown;
}

bool parseLootRoll(std::span<const u8> payload, LootRoll& out) {
    ByteReader r(payload);
    out.object = r.u64v();
    out.slot = r.u32v();
    out.roller = r.u64v();
    out.itemId = r.u32v();
    out.randomSuffix = r.u32v();
    out.randomPropertyId = r.u32v();
    out.number = r.u8v();
    out.type = r.u8v();
    return r.ok();
}

bool parseLootRollWon(std::span<const u8> payload, LootRollWon& out) {
    ByteReader r(payload);
    out.object = r.u64v();
    out.slot = r.u32v();
    out.itemId = r.u32v();
    out.randomSuffix = r.u32v();
    out.randomPropertyId = r.u32v();
    out.winner = r.u64v();
    out.number = r.u8v();
    out.type = r.u8v();
    return r.ok();
}

bool parseLootAllPassed(std::span<const u8> payload, LootAllPassed& out) {
    ByteReader r(payload);
    out.object = r.u64v();
    out.slot = r.u32v();
    out.itemId = r.u32v();
    out.randomPropertyId = r.u32v();
    out.randomSuffix = r.u32v();
    return r.ok();
}

bool parseLootMasterList(std::span<const u8> payload, LootMasterList& out) {
    ByteReader r(payload);
    out.count = 0;
    const u8 count = r.u8v();
    if (count > kMaxRaidMembers) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        out.players[i] = r.u64v();
    }
    if (r.failed()) {
        return false;
    }
    out.count = count;
    return true;
}

void writeLootRoll(std::vector<u8>& out, u64 object, u32 slot, u8 vote) {
    ByteWriter(out).u64v(object).u32v(slot).u8v(vote);
}

void writeLootMasterGive(std::vector<u8>& out, u64 object, u8 slot, u64 player) {
    ByteWriter(out).u64v(object).u8v(slot).u64v(player);
}

// -------------------------------------------------------------------------------------------------------
// Mail.

bool parseMailList(std::span<const u8> payload, MailList& out) {
    ByteReader r(payload);
    out.messages.clear();
    const u8 count = r.u8v();
    if (count > kMaxMailMessages) {
        return false;
    }
    for (u32 i = 0; i < count && r.ok(); ++i) {
        MailEntry& m = out.messages.emplace_back();
        m.id = r.u32v();
        m.type = r.u8v();
        switch (m.type) {
        case kMailNormal: m.sender = r.u64v(); break;
        case kMailAuction:
        case kMailCreature:
        case kMailGameObject: m.senderEntry = r.u32v(); break;
        case kMailItem: break;
        default: r.fail(); break; // the server writes nothing else: the rest cannot be read
        }
        m.subject = r.cstring();
        m.itemTextId = r.u32v();
        m.package = r.u32v();
        m.stationery = r.u32v();
        m.item.entry = r.u32v();
        m.item.enchantment = r.u32v();
        m.item.randomPropertyId = r.u32v();
        m.item.suffixFactor = r.u32v();
        m.item.count = r.u8v();
        m.item.charges = r.u32v();
        m.item.maxDurability = r.u32v();
        m.item.durability = r.u32v();
        m.money = r.u32v();
        m.cod = r.u32v();
        m.flags = r.u32v();
        m.daysLeft = r.f32v();
        m.templateId = r.u32v();
    }
    if (r.failed()) {
        out.messages.clear();
        return false;
    }
    return true;
}

bool parseSendMailResult(std::span<const u8> payload, SendMailResult& out) {
    ByteReader r(payload);
    out = SendMailResult{};
    out.mailId = r.u32v();
    out.action = r.u32v();
    out.error = r.u32v();
    if (out.error == kMailErrEquipError) {
        out.equipError = r.u32v();
    } else if (out.action == kMailActionItemTaken) {
        out.itemId = r.u32v();
        out.count = r.u32v();
    }
    return r.ok();
}

bool parseNextMailTime(std::span<const u8> payload, f32& seconds) {
    ByteReader r(payload);
    seconds = r.f32v();
    return r.ok();
}

void writeMailAction(std::vector<u8>& out, u64 mailbox, u32 mailId) {
    ByteWriter(out).u64v(mailbox).u32v(mailId);
}

void writeMailCreateTextItem(std::vector<u8>& out, u64 mailbox, u32 mailId, u32 templateId) {
    ByteWriter(out).u64v(mailbox).u32v(mailId).u32v(templateId);
}

void writeSendMail(std::vector<u8>& out, u64 mailbox, const MailDraft& draft) {
    ByteWriter w(out);
    w.u64v(mailbox).cstring(draft.to).cstring(draft.subject).cstring(draft.body);
    w.u32v(kMailStationeryDefault).u32v(0); // stationery, package
    w.u64v(draft.item).u32v(draft.money).u32v(draft.cod);
    w.u64v(0).u8v(0);
}

// -------------------------------------------------------------------------------------------------------
// Auction house.

bool parseAuctionHello(std::span<const u8> payload, AuctionHello& out) {
    ByteReader r(payload);
    out.auctioneer = r.u64v();
    out.houseId = r.u32v();
    return r.ok();
}

bool parseAuctionList(std::span<const u8> payload, AuctionListResult& out) {
    ByteReader r(payload);
    out.items.clear();
    out.total = 0;
    const u32 count = r.u32v();
    constexpr u32 kItemBytes = 64;
    if (r.failed() || count > r.remaining() / kItemBytes) {
        return false; // more than the payload can hold
    }
    for (u32 i = 0; i < count; ++i) {
        AuctionItem& a = out.items.emplace_back();
        a.id = r.u32v();
        a.itemId = r.u32v();
        a.enchantment = r.u32v();
        a.randomPropertyId = r.u32v();
        a.suffixFactor = r.u32v();
        a.count = r.u32v();
        a.charges = r.u32v();
        a.owner = r.u64v();
        a.startBid = r.u32v();
        a.minOutbid = r.u32v();
        a.buyout = r.u32v();
        a.timeLeftMs = r.u32v();
        a.bidder = r.u64v();
        a.bid = r.u32v();
    }
    out.total = r.u32v();
    if (r.failed()) {
        out.items.clear();
        out.total = 0;
        return false;
    }
    return true;
}

bool parseAuctionCommandResult(std::span<const u8> payload, AuctionCommandResult& out) {
    ByteReader r(payload);
    out = AuctionCommandResult{};
    out.auctionId = r.u32v();
    out.action = r.u32v();
    out.error = r.u32v();
    switch (out.error) {
    case kAuctionOk:
        if (out.action == kAuctionActionBidPlaced) {
            out.outbid = r.u32v();
        }
        break;
    case kAuctionErrInventory: out.inventoryError = r.u32v(); break;
    case kAuctionErrHigherBid:
        out.newBidder = r.u64v();
        out.newBid = r.u32v();
        out.outbid = r.u32v();
        break;
    default: break;
    }
    return r.ok();
}

bool parseAuctionBidderNotification(std::span<const u8> payload, AuctionBidderNotification& out) {
    ByteReader r(payload);
    out.houseId = r.u32v();
    out.auctionId = r.u32v();
    out.bidder = r.u64v();
    out.bid = r.u32v();
    out.outbid = r.u32v();
    out.itemId = r.u32v();
    out.randomPropertyId = r.u32v();
    return r.ok();
}

bool parseAuctionOwnerNotification(std::span<const u8> payload, AuctionOwnerNotification& out) {
    ByteReader r(payload);
    out.auctionId = r.u32v();
    out.bid = r.u32v();
    out.outbid = r.u32v();
    out.bidder = r.u64v();
    out.itemId = r.u32v();
    out.randomPropertyId = r.u32v();
    return r.ok();
}

bool parseAuctionRemovedNotification(std::span<const u8> payload, AuctionRemovedNotification& out) {
    ByteReader r(payload);
    out.auctionId = r.u32v();
    out.itemId = r.u32v();
    out.randomPropertyId = r.u32v();
    return r.ok();
}

void writeAuctionListItems(std::vector<u8>& out, u64 auctioneer, const AuctionQuery& q) {
    ByteWriter w(out);
    w.u64v(auctioneer).u32v(q.from).cstring(q.name).u8v(q.levelMin).u8v(q.levelMax);
    w.u32v(q.inventoryType).u32v(q.itemClass).u32v(q.subclass).u32v(q.quality).u8v(q.usable ? 1 : 0);
}

void writeAuctionListOwnerItems(std::vector<u8>& out, u64 auctioneer, u32 from) {
    ByteWriter(out).u64v(auctioneer).u32v(from);
}

void writeAuctionListBidderItems(std::vector<u8>& out, u64 auctioneer, u32 from,
                                 std::span<const u32> refresh) {
    ByteWriter w(out);
    w.u64v(auctioneer).u32v(from).u32v(static_cast<u32>(refresh.size()));
    for (const u32 id : refresh) {
        w.u32v(id);
    }
}

void writeAuctionPlaceBid(std::vector<u8>& out, u64 auctioneer, u32 auctionId, u32 price) {
    ByteWriter(out).u64v(auctioneer).u32v(auctionId).u32v(price);
}

void writeAuctionRemoveItem(std::vector<u8>& out, u64 auctioneer, u32 auctionId) {
    ByteWriter(out).u64v(auctioneer).u32v(auctionId);
}

void writeAuctionSellItem(std::vector<u8>& out, u64 auctioneer, u64 item, u32 bid, u32 buyout, u32 minutes) {
    ByteWriter(out).u64v(auctioneer).u64v(item).u32v(bid).u32v(buyout).u32v(minutes);
}

// -------------------------------------------------------------------------------------------------------
// Trade.

bool parseTradeStatus(std::span<const u8> payload, TradeStatus& out) {
    ByteReader r(payload);
    out = TradeStatus{};
    out.status = r.u32v();
    switch (out.status) {
    case kTradeBegin: out.initiator = r.u64v(); break;
    case kTradeCloseWindow:
        out.closeResult = r.u32v();
        out.closeUnknown = r.u8v();
        out.closeItemLimitCategory = r.u32v();
        break;
    case kTradeOnlyConjured: out.slot = r.u8v(); break;
    default: break;
    }
    return r.ok();
}

bool parseTradeStatusExtended(std::span<const u8> payload, TradeStatusExtended& out) {
    ByteReader r(payload);
    out.trader = r.u8v() != 0;
    out.slotCount = r.u32v();
    r.u32v(); // the count again
    out.money = r.u32v();
    out.spell = r.u32v();
    out.slots = {};
    // Each slot: its number, then 15 words (all zero when empty). The server writes all seven in order; a
    // packet may end early ("if not specified, then end of packet").
    while (r.ok() && !r.atEnd()) {
        const u8 index = r.u8v();
        if (index >= kTradeSlotCount) {
            return false;
        }
        TradeSlotInfo& s = out.slots[index];
        s.itemId = r.u32v();
        s.displayId = r.u32v();
        s.count = r.u32v();
        s.wrapped = r.u32v() != 0;
        s.giftCreator = r.u64v();
        s.enchantment = r.u32v();
        s.creator = r.u64v();
        s.charges = r.u32v();
        s.suffixFactor = r.u32v();
        s.randomPropertyId = r.u32v();
        s.lockId = r.u32v();
        s.maxDurability = r.u32v();
        s.durability = r.u32v();
    }
    return r.ok();
}

void writeSetTradeItem(std::vector<u8>& out, u8 tradeSlot, u8 bag, u8 slot) {
    ByteWriter(out).u8v(tradeSlot).u8v(bag).u8v(slot);
}

void writeAcceptTrade(std::vector<u8>& out) {
    ByteWriter(out).u32v(1);
}

} // namespace vd::net
