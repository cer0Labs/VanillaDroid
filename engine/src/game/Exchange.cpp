#include "game/Exchange.h"

namespace vd::game {

Exchange::Exchange(const EntityStore& entities, QueryCache& queries, Events& events, Inventory& inventory)
    : m_context(entities, queries, events, inventory) {}

void Exchange::setData(const ExchangeData* data) {
    m_data = data;
    m_auction.setData(data);
}

void Exchange::enterWorld(u64 player, bool newLogin, u32 nowMs) {
    m_context.player = player;
    m_context.inWorld = true;
    m_context.nowMs = nowMs;
    if (newLogin) {
        m_rolls.reset();
    }
    m_rolls.clearMasterCandidates();
    m_trade.reset(); // a trade never survives a map change (the server cancels it)
    m_mail.enterWorld(newLogin);
    m_auction.enterWorld(newLogin);
}

void Exchange::leaveWorld() {
    m_context.inWorld = false;
    m_rolls.reset();
    m_trade.reset();
    m_mail.leaveWorld();
    m_auction.leaveWorld();
}

void Exchange::update(u32 nowMs) {
    m_context.nowMs = nowMs;
    m_rolls.update(nowMs);
    m_auction.update(nowMs);
    m_trade.update(nowMs);
}

bool Exchange::handles(net::Opcode opcode) {
    return LootRolls::handles(opcode) || Mail::handles(opcode) || AuctionHouse::handles(opcode) ||
           Trade::handles(opcode);
}

bool Exchange::handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    m_context.nowMs = nowMs;
    if (LootRolls::handles(opcode)) {
        return m_rolls.handle(opcode, payload);
    }
    if (Mail::handles(opcode)) {
        return m_mail.handle(opcode, payload);
    }
    if (AuctionHouse::handles(opcode)) {
        return m_auction.handle(opcode, payload);
    }
    return m_trade.handle(opcode, payload);
}

} // namespace vd::game
