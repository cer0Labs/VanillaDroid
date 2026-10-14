#pragma once

#include "core/Types.h"
#include "game/Auction.h"
#include "game/ExchangeContext.h"
#include "game/LootRolls.h"
#include "game/Mail.h"
#include "game/Trade.h"

#include <span>

namespace vd::game {

class ExchangeData;

// The item economy between players (phase 8): group loot rolls and master loot, mail, the auction house and
// trade. Gameplay owns one and hands it the packets of these four (handles()); each part keeps its state for
// the windows and sends the player's requests. Results and notifications go to Gameplay's event ring
// (GameEventKind::LootRollStarted .. TradeClosed). Main thread; nothing allocates per frame once warmed up.
class Exchange {
public:
    using Events = ExchangeContext::Events;

    Exchange(const EntityStore& entities, QueryCache& queries, Events& events, Inventory& inventory);

    void setSink(PacketSink sink) { m_context.sink = sink; }
    // AuctionHouse.dbc (deposits) and MailTemplate.dbc (NPC letters' subjects); may be null.
    void setData(const ExchangeData* data);
    const ExchangeData* data() const { return m_data; }
    // `newLogin`: another character (everything starts over).
    void enterWorld(u64 player, bool newLogin, u32 nowMs);
    void leaveWorld();
    void update(u32 nowMs);
    static bool handles(net::Opcode opcode);
    // A packet of handles(): false when malformed.
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);

    LootRolls& rolls() { return m_rolls; }
    const LootRolls& rolls() const { return m_rolls; }
    Mail& mail() { return m_mail; }
    const Mail& mail() const { return m_mail; }
    AuctionHouse& auction() { return m_auction; }
    const AuctionHouse& auction() const { return m_auction; }
    Trade& trade() { return m_trade; }
    const Trade& trade() const { return m_trade; }

private:
    ExchangeContext m_context;
    const ExchangeData* m_data = nullptr;
    LootRolls m_rolls{m_context};
    Mail m_mail{m_context};
    AuctionHouse m_auction{m_context};
    Trade m_trade{m_context};
};

} // namespace vd::game
