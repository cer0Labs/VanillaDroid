#pragma once

#include "core/Types.h"
#include "game/ExchangeContext.h"
#include "net/ExchangeMessages.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::game {

class ExchangeData;

// The three lists the auction house window shows.
enum class AuctionList : u8 { Browse, Bids, Owned, Count };

// What the browse list asks for (CMSG_AUCTION_LIST_ITEMS' filters; net::kAuctionAny = any).
struct AuctionSearch {
    std::string name;
    u32 levelMin = 0, levelMax = 0; // required level; 0: any
    u32 itemClass = net::kAuctionAny;
    u32 subclass = net::kAuctionAny;
    u32 inventoryType = net::kAuctionAny;
    u32 quality = net::kAuctionAny; // this quality or better
    bool usable = false;
};

// One page of a list as the server last answered it.
struct AuctionPage {
    std::vector<net::AuctionItem> items;
    u32 total = 0;      // in every page
    u32 from = 0;       // the first auction of this page
    u32 receivedMs = 0; // client clock when it arrived: time left counts down from there
    u32 version = 0;    // bumps with every answer
    bool loaded = false;
    bool pending = false; // asked, the answer not here yet
    u32 pages() const { return (total + net::kAuctionPageSize - 1) / net::kAuctionPageSize; }
};

// The auction house (phase 8): an auctioneer's window (MSG_AUCTION_HELLO), browsing with filters and pages,
// the player's bids and auctions, bidding, buying out, selling with the deposit the server will take,
// cancelling, and the notifications (outbid, won, sold) for the HUD. The server answers one list request at a
// time and ignores the others while one is pending: requests wait in a queue here. Part of game::Exchange;
// storage reserved once.
class AuctionHouse {
public:
    static constexpr u32 kListTimeoutMs = 8000; // an answer that never came (the server dropped the request)

    explicit AuctionHouse(ExchangeContext& context);
    void setData(const ExchangeData* data) { m_data = data; }

    void enterWorld(bool newLogin);
    void leaveWorld();
    void update(u32 nowMs);
    static bool handles(net::Opcode opcode);
    bool handle(net::Opcode opcode, std::span<const u8> payload);

    // --- The window ---
    // MSG_AUCTION_HELLO to an auctioneer (Gameplay::interact sends it for auctioneers without a gossip menu);
    // the window opens on the answer (AuctionOpened).
    void requestOpen(u64 auctioneer);
    bool open() const { return m_auctioneer != 0; }
    u64 auctioneer() const { return m_auctioneer; }
    u32 houseId() const { return m_houseId; }
    void close();

    // --- Lists ---
    // Asks for a page of the list (`from` a multiple of net::kAuctionPageSize); browsing remembers `search`
    // for its next pages and the refreshes after a bid.
    bool search(const AuctionSearch& search, u32 from = 0);
    bool listBids(u32 from = 0);
    bool listOwned(u32 from = 0);
    const AuctionPage& page(AuctionList list) const { return m_pages[static_cast<u32>(list)]; }
    const AuctionSearch& lastSearch() const { return m_search; }
    // An auction in any of the lists.
    const net::AuctionItem* find(u32 auctionId) const;
    // The auction's time left now (counted down from when its page arrived).
    u32 timeLeftMs(const AuctionPage& page, const net::AuctionItem& a) const;

    // --- Bids and sales (false when not sent) ---
    // A bid of `price` (at least net::auctionMinimumBid; at or above the buyout it buys out).
    bool bid(u32 auctionId, u32 price);
    bool buyout(u32 auctionId);
    // The item (in the bags) for a starting bid, an optional buyout (0) and one of net::kAuctionDurations
    // (index).
    bool sell(u64 item, u32 startBid, u32 buyoutPrice, u32 durationIndex);
    // Why an item cannot be sold (a text for the player), or empty.
    std::string_view sellProblem(u64 item, u32 startBid, u32 buyoutPrice) const;
    // What putting `item` up for durationIndex costs (AuctionHouseMgr::GetAuctionDeposit with this house's
    // rate); 0 while its template is unknown.
    u32 deposit(u64 item, u32 durationIndex) const;
    // One of the player's auctions (its bidder gets the money back, and the player pays the house's cut).
    bool cancel(u32 auctionId);
    // A bid, sale or cancel waiting for its SMSG_AUCTION_COMMAND_RESULT.
    bool busy() const { return m_commandPending; }
    // The item being put up for sale (game/ItemActions: "Subastar", a drop on the sale slot): an item of the
    // bags while the window is open; 0 none. Closing or a sale started clears it.
    bool stage(u64 item);
    void unstage() { m_staged = 0; }
    u64 staged() const { return m_staged; }
    // Why an item cannot be auctioned (not in the bags, bound, temporary), or empty.
    std::string_view itemProblem(u64 item) const;

private:
    struct Request {
        bool wanted = false;
        u32 from = 0;
    };
    bool request(AuctionList list, u32 from);
    void sendNext();
    void sendList(AuctionList list, u32 from);
    void onList(AuctionList list, const net::AuctionListResult& result);
    bool command(net::Opcode opcode);

    ExchangeContext& m_c;
    const ExchangeData* m_data = nullptr;
    u64 m_auctioneer = 0;
    u32 m_houseId = 0;
    std::array<AuctionPage, static_cast<u32>(AuctionList::Count)> m_pages;
    std::array<Request, static_cast<u32>(AuctionList::Count)> m_queue{};
    AuctionSearch m_search;
    bool m_inFlight = false; // one list request at a time (WorldSession::ReceivedAHListRequest)
    AuctionList m_inFlightList = AuctionList::Browse;
    u32 m_inFlightMs = 0;
    bool m_commandPending = false;
    u32 m_commandMs = 0;
    u64 m_staged = 0;
    net::AuctionListResult m_result; // parsed, reused
};

} // namespace vd::game
