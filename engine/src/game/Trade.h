#pragma once

#include "core/Types.h"
#include "game/ExchangeContext.h"
#include "net/ExchangeMessages.h"

#include <array>
#include <span>
#include <string_view>

namespace vd::game {

struct ItemPos;

enum class TradePhase : u8 {
    None,
    Requested, // someone asked the player (SMSG_TRADE_STATUS BEGIN_TRADE): acceptRequest() / declineRequest()
    Proposed,  // the player asked someone (CMSG_INITIATE_TRADE): waiting for them
    Open,      // both see the trade window
};

// One side of the trade window.
struct TradeSide {
    // The player's own side: the item GUIDs it put in each slot (the server does not echo them; their objects
    // are in the inventory). The other side: nothing (its items are only described in `slots`).
    std::array<u64, net::kTradeSlotCount> items{};
    // What the server last described (always for the other side; for the player's own only after spells).
    std::array<net::TradeSlotInfo, net::kTradeSlotCount> slots{};
    u32 money = 0;
    u32 spell = 0; // an enchantment cast on the other side's non-traded item
    bool accepted = false;
};

// Trading with another player (phase 8): the request both ways (CMSG_INITIATE_TRADE, SMSG_TRADE_STATUS), the
// window with six traded slots, one only enchanted, and money (CMSG_SET_TRADE_ITEM / _CLEAR_TRADE_ITEM /
// _SET_TRADE_GOLD, SMSG_TRADE_STATUS_EXTENDED), accept / unaccept / cancel. The server keeps the truth; any
// change withdraws both acceptances (BACK_TO_TRADE). Part of game::Exchange; fixed storage.
class Trade {
public:
    explicit Trade(ExchangeContext& context) : m_c(context) {}

    void reset();
    void update(u32 nowMs);
    static bool handles(net::Opcode opcode);
    bool handle(net::Opcode opcode, std::span<const u8> payload);

    TradePhase phase() const { return m_phase; }
    bool open() const { return m_phase == TradePhase::Open; }
    u64 partner() const { return m_partner; }
    const TradeSide& mine() const { return m_mine; }
    const TradeSide& theirs() const { return m_theirs; }
    // Bumped whenever either side changes.
    u32 version() const { return m_version; }

    // --- Starting and ending ---
    // CMSG_INITIATE_TRADE to a player in sight within reach (net::kTradeDistance). False when not sent.
    bool initiate(u64 player);
    // Why the player cannot trade with `player` now (a text for the player), or empty.
    std::string_view initiateProblem(u64 player) const;
    // Phase 15 (options): BLOCK_TRADES: a request is refused at once (CMSG_BUSY_TRADE, as WoW.exe 0x4BF7BC does; the
    // other player hears TRADE_STATUS_BUSY) and said (TradeBlocked) instead of asked.
    bool blockRequests = false;
    void acceptRequest();  // CMSG_BEGIN_TRADE: both windows open
    void declineRequest(); // CMSG_CANCEL_TRADE
    void cancel();         // CMSG_CANCEL_TRADE: closes the window on both sides

    // --- The window (false when not sent) ---
    // An item of the bags into a trade slot (0..6; 6 is the slot whose item stays, for enchanting). The same
    // item in another slot moves.
    bool setItem(u8 tradeSlot, ItemPos from);
    bool clearItem(u8 tradeSlot);
    bool setMoney(u32 copper); // what the player gives (at most its money)
    bool accept();             // CMSG_ACCEPT_TRADE
    bool unaccept();           // CMSG_UNACCEPT_TRADE
    // The trade slot holding the player's item `guid` (-1 none): the bags dim it.
    i32 slotOf(u64 guid) const;
    // The first free traded slot (-1 when all six are used).
    i32 freeSlot() const;

private:
    void close(u32 status, bool tellServer);

    ExchangeContext& m_c;
    TradePhase m_phase = TradePhase::None;
    u64 m_partner = 0;
    TradeSide m_mine;
    TradeSide m_theirs;
    u32 m_version = 1;
};

} // namespace vd::game
