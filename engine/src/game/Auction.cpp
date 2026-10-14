#include "game/Auction.h"

#include "core/Text.h"
#include "game/ExchangeData.h"
#include "game/Inventory.h"
#include "net/GameMessages.h"

#include <algorithm>

namespace vd::game {

using net::Opcode;

AuctionHouse::AuctionHouse(ExchangeContext& context) : m_c(context) {
    for (AuctionPage& p : m_pages) {
        p.items.reserve(net::kAuctionPageSize);
    }
    m_result.items.reserve(net::kAuctionPageSize);
    m_search.name.reserve(64);
}

void AuctionHouse::enterWorld(bool newLogin) {
    close();
    if (newLogin) {
        m_inFlight = false;
        m_commandPending = false;
    }
}

void AuctionHouse::leaveWorld() {
    close();
    m_inFlight = false;
    m_commandPending = false;
}

void AuctionHouse::update(u32 nowMs) {
    if (m_inFlight && nowMs - m_inFlightMs > kListTimeoutMs) {
        m_inFlight = false; // the server dropped it (no answer comes then): the queue goes on
        m_pages[static_cast<u32>(m_inFlightList)].pending = m_queue[static_cast<u32>(m_inFlightList)].wanted;
        sendNext();
    }
    if (m_commandPending && nowMs - m_commandMs > kListTimeoutMs) {
        m_commandPending = false;
    }
}

bool AuctionHouse::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::MSG_AUCTION_HELLO:
    case Opcode::SMSG_AUCTION_LIST_RESULT:
    case Opcode::SMSG_AUCTION_OWNER_LIST_RESULT:
    case Opcode::SMSG_AUCTION_BIDDER_LIST_RESULT:
    case Opcode::SMSG_AUCTION_COMMAND_RESULT:
    case Opcode::SMSG_AUCTION_BIDDER_NOTIFICATION:
    case Opcode::SMSG_AUCTION_OWNER_NOTIFICATION:
    case Opcode::SMSG_AUCTION_REMOVED_NOTIFICATION: return true;
    default: return false;
    }
}

bool AuctionHouse::handle(Opcode opcode, std::span<const u8> p) {
    switch (opcode) {
    case Opcode::MSG_AUCTION_HELLO: {
        net::AuctionHello m;
        if (!net::parseAuctionHello(p, m)) {
            return false;
        }
        close();
        m_auctioneer = m.auctioneer;
        m_houseId = m.houseId;
        m_c.event(GameEventKind::AuctionOpened, m.houseId, m.auctioneer);
        return true;
    }
    case Opcode::SMSG_AUCTION_LIST_RESULT:
    case Opcode::SMSG_AUCTION_OWNER_LIST_RESULT:
    case Opcode::SMSG_AUCTION_BIDDER_LIST_RESULT: {
        if (!net::parseAuctionList(p, m_result)) {
            return false;
        }
        const AuctionList list = opcode == Opcode::SMSG_AUCTION_LIST_RESULT         ? AuctionList::Browse
                                 : opcode == Opcode::SMSG_AUCTION_OWNER_LIST_RESULT ? AuctionList::Owned
                                                                                    : AuctionList::Bids;
        onList(list, m_result);
        return true;
    }
    case Opcode::SMSG_AUCTION_COMMAND_RESULT: {
        net::AuctionCommandResult m;
        if (!net::parseAuctionCommandResult(p, m)) {
            return false;
        }
        m_commandPending = false;
        GameEvent& e = m_c.event(GameEventKind::AuctionResult, m.auctionId);
        e.value = {static_cast<i32>(m.action), static_cast<i32>(m.error), static_cast<i32>(m.inventoryError)};
        if (m.error == net::kAuctionErrHigherBid) {
            // Someone bid first: the auction shows the new bid (the player may bid again over it).
            for (AuctionPage& page : m_pages) {
                for (net::AuctionItem& a : page.items) {
                    if (a.id == m.auctionId) {
                        a.bid = m.newBid;
                        a.bidder = m.newBidder;
                        a.minOutbid = m.outbid;
                    }
                }
            }
        }
        if (m.error == net::kAuctionOk) {
            switch (m.action) {
            case net::kAuctionActionStarted:
                m_staged = 0; // it is in the auction house now
                request(AuctionList::Owned, page(AuctionList::Owned).from);
                break;
            case net::kAuctionActionRemoved:
                request(AuctionList::Owned, page(AuctionList::Owned).from);
                break;
            case net::kAuctionActionBidPlaced:
                if (page(AuctionList::Browse).loaded) {
                    request(AuctionList::Browse,
                            page(AuctionList::Browse).from); // the new bid, or the item gone
                }
                request(AuctionList::Bids, 0);
                break;
            default: break;
            }
        }
        return true;
    }
    case Opcode::SMSG_AUCTION_BIDDER_NOTIFICATION: {
        net::AuctionBidderNotification m;
        if (!net::parseAuctionBidderNotification(p, m)) {
            return false;
        }
        m_c.queries.item(m.itemId);
        if (m.won()) {
            m_c.event(GameEventKind::AuctionWon, m.itemId);
        } else {
            m_c.event(GameEventKind::AuctionOutbid, m.itemId).value[0] = static_cast<i32>(m.bid);
        }
        return true;
    }
    case Opcode::SMSG_AUCTION_OWNER_NOTIFICATION: {
        net::AuctionOwnerNotification m;
        if (!net::parseAuctionOwnerNotification(p, m)) {
            return false;
        }
        m_c.queries.item(m.itemId);
        m_c.event(m.sold() ? GameEventKind::AuctionSold : GameEventKind::AuctionBid, m.itemId).value[0] =
            static_cast<i32>(m.bid);
        return true;
    }
    case Opcode::SMSG_AUCTION_REMOVED_NOTIFICATION: {
        net::AuctionRemovedNotification m;
        if (!net::parseAuctionRemovedNotification(p, m)) {
            return false;
        }
        m_c.queries.item(m.itemId);
        m_c.event(GameEventKind::AuctionRemoved, m.itemId);
        return true;
    }
    default: return false;
    }
}

// -------------------------------------------------------------------------------------------------------
// The window.

void AuctionHouse::requestOpen(u64 auctioneer) {
    if (auctioneer == 0) {
        return;
    }
    m_c.body.clear();
    net::writeGuid(m_c.body, auctioneer);
    m_c.send(Opcode::MSG_AUCTION_HELLO); // the window opens on the answer
}

void AuctionHouse::close() {
    m_auctioneer = 0;
    m_houseId = 0;
    for (AuctionPage& p : m_pages) {
        p.items.clear();
        p.total = 0;
        p.from = 0;
        p.loaded = false;
        p.pending = false;
        ++p.version;
    }
    m_queue = {};
    m_staged = 0;
    // An answer still on its way keeps m_inFlight: the server ignores other requests until it has sent it.
}

// -------------------------------------------------------------------------------------------------------
// Lists.

bool AuctionHouse::search(const AuctionSearch& s, u32 from) {
    if (!open()) {
        return false;
    }
    m_search = s;
    return request(AuctionList::Browse, from);
}

bool AuctionHouse::listBids(u32 from) {
    return request(AuctionList::Bids, from);
}

bool AuctionHouse::listOwned(u32 from) {
    return request(AuctionList::Owned, from);
}

bool AuctionHouse::request(AuctionList list, u32 from) {
    if (!open()) {
        return false;
    }
    const u32 i = static_cast<u32>(list);
    m_queue[i] = {true, from};
    m_pages[i].pending = true;
    sendNext();
    return true;
}

void AuctionHouse::sendNext() {
    if (m_inFlight || !open()) {
        return;
    }
    for (u32 i = 0; i < m_queue.size(); ++i) {
        if (m_queue[i].wanted) {
            m_queue[i].wanted = false;
            sendList(static_cast<AuctionList>(i), m_queue[i].from);
            return;
        }
    }
}

void AuctionHouse::sendList(AuctionList list, u32 from) {
    m_c.body.clear();
    Opcode opcode = Opcode::CMSG_AUCTION_LIST_ITEMS;
    switch (list) {
    case AuctionList::Browse: {
        net::AuctionQuery q;
        q.from = from;
        q.name = m_search.name;
        q.levelMin = static_cast<u8>(std::min<u32>(m_search.levelMin, 255));
        q.levelMax = static_cast<u8>(std::min<u32>(m_search.levelMax, 255));
        q.inventoryType = m_search.inventoryType;
        q.itemClass = m_search.itemClass;
        q.subclass = m_search.subclass;
        q.quality = m_search.quality;
        q.usable = m_search.usable;
        net::writeAuctionListItems(m_c.body, m_auctioneer, q);
        break;
    }
    case AuctionList::Owned:
        opcode = Opcode::CMSG_AUCTION_LIST_OWNER_ITEMS;
        net::writeAuctionListOwnerItems(m_c.body, m_auctioneer, from);
        break;
    default:
        opcode = Opcode::CMSG_AUCTION_LIST_BIDDER_ITEMS;
        net::writeAuctionListBidderItems(m_c.body, m_auctioneer, from, {});
        break;
    }
    if (m_c.send(opcode)) {
        m_inFlight = true;
        m_inFlightList = list;
        m_inFlightMs = m_c.nowMs;
        m_pages[static_cast<u32>(list)].from = from;
    } else {
        m_pages[static_cast<u32>(list)].pending = false;
    }
}

void AuctionHouse::onList(AuctionList list, const net::AuctionListResult& result) {
    if (m_inFlight && m_inFlightList == list) {
        m_inFlight = false;
    }
    AuctionPage& page = m_pages[static_cast<u32>(list)];
    if (open()) {
        page.items = result.items; // both reserved for a page: no allocation
        page.total = result.total;
        page.receivedMs = m_c.nowMs;
        page.loaded = true;
        page.pending = m_queue[static_cast<u32>(list)].wanted; // another page of it may be queued
        ++page.version;
        for (const net::AuctionItem& a : page.items) {
            m_c.queries.item(a.itemId); // names, icons and tooltips of the rows
            if (a.owner != 0 && a.owner != m_c.player) {
                m_c.queries.playerName(a.owner);
            }
        }
    }
    sendNext();
}

const net::AuctionItem* AuctionHouse::find(u32 auctionId) const {
    for (const AuctionPage& page : m_pages) {
        for (const net::AuctionItem& a : page.items) {
            if (a.id == auctionId) {
                return &a;
            }
        }
    }
    return nullptr;
}

u32 AuctionHouse::timeLeftMs(const AuctionPage& page, const net::AuctionItem& a) const {
    const u32 elapsed = m_c.nowMs - page.receivedMs;
    return a.timeLeftMs > elapsed ? a.timeLeftMs - elapsed : 0;
}

// -------------------------------------------------------------------------------------------------------
// Bids and sales.

bool AuctionHouse::command(Opcode opcode) {
    if (!open() || m_commandPending || !m_c.send(opcode)) {
        return false;
    }
    m_commandPending = true;
    m_commandMs = m_c.nowMs;
    return true;
}

bool AuctionHouse::bid(u32 auctionId, u32 price) {
    const net::AuctionItem* a = find(auctionId);
    if (!a || a->owner == m_c.player || price == 0) {
        return false;
    }
    const bool buys = a->buyout != 0 && price >= a->buyout;
    if (!buys && price < net::auctionMinimumBid(*a)) {
        return false; // the server would refuse it (AUCTION_ERR_BID_INCREMENT)
    }
    if (price > m_c.inventory.money()) {
        return false; // the server ignores it without an answer
    }
    m_c.body.clear();
    net::writeAuctionPlaceBid(m_c.body, m_auctioneer, auctionId, buys ? a->buyout : price);
    return command(Opcode::CMSG_AUCTION_PLACE_BID);
}

bool AuctionHouse::buyout(u32 auctionId) {
    const net::AuctionItem* a = find(auctionId);
    return a && a->buyout != 0 && bid(auctionId, a->buyout);
}

std::string_view AuctionHouse::sellProblem(u64 item, u32 startBid, u32 buyoutPrice) const {
    if (!open()) {
        return text::get("VD_EXCHANGE_NO_AUCTIONEER");
    }
    if (const std::string_view problem = itemProblem(item); !problem.empty()) {
        return problem;
    }
    if (startBid == 0) {
        return text::get("VD_EXCHANGE_AUCTION_NO_START_PRICE");
    }
    if (startBid > 2000000000u || buyoutPrice > 2000000000u) {
        return text::get("VD_EXCHANGE_AUCTION_PRICE_TOO_HIGH");
    }
    if (buyoutPrice != 0 && buyoutPrice < startBid) {
        return text::get("AUCTION_BUYOUT_ERROR");
    }
    return {};
}

std::string_view AuctionHouse::itemProblem(u64 item) const {
    const ItemInstance* it = item ? m_c.inventory.find(item) : nullptr;
    const i32 container = it ? m_c.inventory.containerOf(it->pos) : 99;
    if (!it || container < kContainerBackpack || container > kContainerLastBag) {
        return text::get("VD_EXCHANGE_AUCTION_NOT_IN_BAGS");
    }
    if (it->soulbound()) {
        return text::get("ERR_AUCTION_BOUND_ITEM");
    }
    const net::ItemQueryInfo* info = m_c.queries.findItem(it->entry);
    if (it->durationS != 0 || (info && (info->flags & kItemFlagConjured))) {
        return text::get("ERR_AUCTION_CONJURED_ITEM");
    }
    return {};
}

bool AuctionHouse::stage(u64 item) {
    if (!open() || !itemProblem(item).empty()) {
        return false;
    }
    m_staged = item;
    return true;
}

u32 AuctionHouse::deposit(u64 item, u32 durationIndex) const {
    const ItemInstance* it = item ? m_c.inventory.find(item) : nullptr;
    const net::ItemQueryInfo* info = it ? m_c.queries.findItem(it->entry) : nullptr;
    if (!info || durationIndex >= net::kAuctionDurations.size()) {
        return 0;
    }
    const u32 percent = m_data ? m_data->house(m_houseId).depositPercent : 5; // the faction houses'
    return net::auctionDeposit(info->sellPrice, it->count, net::kAuctionDurations[durationIndex], percent);
}

bool AuctionHouse::sell(u64 item, u32 startBid, u32 buyoutPrice, u32 durationIndex) {
    if (!sellProblem(item, startBid, buyoutPrice).empty() || durationIndex >= net::kAuctionDurations.size() ||
        deposit(item, durationIndex) > m_c.inventory.money()) {
        return false;
    }
    m_c.body.clear();
    net::writeAuctionSellItem(m_c.body, m_auctioneer, item, startBid, buyoutPrice,
                              net::kAuctionDurations[durationIndex]);
    return command(Opcode::CMSG_AUCTION_SELL_ITEM);
}

bool AuctionHouse::cancel(u32 auctionId) {
    bool mine = false;
    for (const net::AuctionItem& a : page(AuctionList::Owned).items) {
        mine = mine || a.id == auctionId;
    }
    if (!mine) {
        return false;
    }
    m_c.body.clear();
    net::writeAuctionRemoveItem(m_c.body, m_auctioneer, auctionId);
    return command(Opcode::CMSG_AUCTION_REMOVE_ITEM);
}

} // namespace vd::game
