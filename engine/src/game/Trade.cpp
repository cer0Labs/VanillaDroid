#include "game/Trade.h"

#include "core/Text.h"
#include "game/Inventory.h"
#include "game/UnitInfo.h"
#include "net/GameMessages.h"

namespace vd::game {

using net::Opcode;

void Trade::reset() {
    m_phase = TradePhase::None;
    m_partner = 0;
    m_mine = TradeSide{};
    m_theirs = TradeSide{};
    ++m_version;
}

void Trade::update(u32 nowMs) {
    (void)nowMs;
    if (m_phase != TradePhase::Open) {
        return;
    }
    // An item that left the bags (used, destroyed, moved to the bank) is no longer offered.
    for (u64& guid : m_mine.items) {
        if (guid != 0 && !m_c.inventory.find(guid)) {
            guid = 0;
            ++m_version;
        }
    }
}

bool Trade::handles(Opcode opcode) {
    return opcode == Opcode::SMSG_TRADE_STATUS || opcode == Opcode::SMSG_TRADE_STATUS_EXTENDED;
}

bool Trade::handle(Opcode opcode, std::span<const u8> p) {
    if (opcode == Opcode::SMSG_TRADE_STATUS_EXTENDED) {
        net::TradeStatusExtended m;
        if (!net::parseTradeStatusExtended(p, m)) {
            return false;
        }
        TradeSide& side = m.trader ? m_theirs : m_mine;
        side.slots = m.slots;
        side.money = m.money;
        side.spell = m.spell;
        ++m_version;
        return true;
    }
    net::TradeStatus m;
    if (!net::parseTradeStatus(p, m)) {
        return false;
    }
    switch (m.status) {
    case net::kTradeBegin:
        reset();
        if (blockRequests) { // phase 15 (options): BLOCK_TRADES
            m_c.sendEmpty(Opcode::CMSG_BUSY_TRADE);
            m_c.queries.playerName(m.initiator);
            m_c.event(GameEventKind::TradeBlocked, 0, m.initiator);
            break;
        }
        m_phase = TradePhase::Requested;
        m_partner = m.initiator;
        m_c.queries.playerName(m.initiator);
        m_c.event(GameEventKind::TradeRequested, 0, m.initiator);
        break;
    case net::kTradeOpenWindow: {
        const u64 partner = m_partner;
        reset();
        m_phase = TradePhase::Open;
        m_partner = partner;
        m_c.event(GameEventKind::TradeOpened, 0, partner);
        break;
    }
    case net::kTradeAccept:
        m_theirs.accepted = true;
        ++m_version;
        break;
    case net::kTradeBackToTrade: // a change (or the other side) withdrew the acceptances
        m_mine.accepted = false;
        m_theirs.accepted = false;
        ++m_version;
        break;
    case net::kTradeCanceled:
    case net::kTradeUnknown13:
    case net::kTradeComplete:
        if (m_phase != TradePhase::None) {
            close(m.status, false); // the server has already let go of it
        }
        break;
    default:
        // Errors: the window closes (the client's frame does CloseTrade, which tells the server).
        close(m.status, m_phase == TradePhase::Open || m_phase == TradePhase::Requested);
        break;
    }
    return true;
}

void Trade::close(u32 status, bool tellServer) {
    const u64 partner = m_partner;
    if (tellServer) {
        m_c.sendEmpty(Opcode::CMSG_CANCEL_TRADE);
    }
    reset();
    m_c.event(GameEventKind::TradeClosed, 0, partner).value[0] = static_cast<i32>(status);
}

// -------------------------------------------------------------------------------------------------------
// Starting and ending.

std::string_view Trade::initiateProblem(u64 player) const {
    if (m_phase != TradePhase::None) {
        return text::get("ERR_ALREADY_TRADING");
    }
    const Entity* me = m_c.me();
    const Entity* other = player != 0 && player != m_c.player ? m_c.entities.find(player) : nullptr;
    if (!me || !other || other->type != ObjectType::Player) {
        return text::get("PLAYER_NOT_FOUND");
    }
    if (isDead(*me)) {
        return text::get("ERR_PLAYER_DEAD");
    }
    if (isDead(*other)) {
        return text::get("ERR_TRADE_TARGET_DEAD");
    }
    const glm::vec3 d = me->positionAt(m_c.nowMs) - other->positionAt(m_c.nowMs);
    if (d.x * d.x + d.y * d.y + d.z * d.z > net::kTradeDistance * net::kTradeDistance) {
        return text::get("ERR_TRADE_TOO_FAR");
    }
    return {};
}

bool Trade::initiate(u64 player) {
    if (!initiateProblem(player).empty()) {
        return false;
    }
    m_c.body.clear();
    net::writeGuid(m_c.body, player);
    if (!m_c.send(Opcode::CMSG_INITIATE_TRADE)) {
        return false;
    }
    reset();
    m_phase = TradePhase::Proposed; // nothing comes back until the other player answers
    m_partner = player;
    return true;
}

void Trade::acceptRequest() {
    if (m_phase == TradePhase::Requested) {
        m_c.sendEmpty(Opcode::CMSG_BEGIN_TRADE); // OPEN_WINDOW comes to both
    }
}

void Trade::declineRequest() {
    if (m_phase == TradePhase::Requested) {
        cancel();
    }
}

void Trade::cancel() {
    if (m_phase == TradePhase::None) {
        return;
    }
    m_c.sendEmpty(Opcode::CMSG_CANCEL_TRADE); // TRADE_CANCELED comes to both
    const u64 partner = m_partner;
    reset();
    m_c.event(GameEventKind::TradeClosed, 0, partner).value[0] = static_cast<i32>(net::kTradeCanceled);
}

// -------------------------------------------------------------------------------------------------------
// The window.

bool Trade::setItem(u8 tradeSlot, ItemPos from) {
    const ItemInstance* item = m_c.inventory.at(from);
    if (!open() || tradeSlot >= net::kTradeSlotCount || !item || item->entry == 0) {
        return false;
    }
    const i32 container = m_c.inventory.containerOf(from);
    if (container < kContainerBackpack || container > kContainerLastBag) {
        return false; // the server cancels the whole trade for bank or worn items
    }
    if (tradeSlot != net::kTradeSlotNonTraded && item->soulbound()) {
        return false; // likewise for what cannot change hands
    }
    const i32 old = slotOf(item->guid);
    if (old == tradeSlot) {
        return false;
    }
    if (old >= 0) {
        clearItem(static_cast<u8>(old)); // the same item twice cancels the trade on the server
    }
    m_c.body.clear();
    net::writeSetTradeItem(m_c.body, tradeSlot, from.bag, from.slot);
    if (!m_c.send(Opcode::CMSG_SET_TRADE_ITEM)) {
        return false;
    }
    m_mine.items[tradeSlot] = item->guid;
    m_mine.accepted = false;
    ++m_version;
    return true;
}

bool Trade::clearItem(u8 tradeSlot) {
    if (!open() || tradeSlot >= net::kTradeSlotCount || m_mine.items[tradeSlot] == 0) {
        return false;
    }
    m_c.body.clear();
    net::writeU8(m_c.body, tradeSlot);
    if (!m_c.send(Opcode::CMSG_CLEAR_TRADE_ITEM)) {
        return false;
    }
    m_mine.items[tradeSlot] = 0;
    m_mine.accepted = false;
    ++m_version;
    return true;
}

bool Trade::setMoney(u32 copper) {
    if (!open() || copper > m_c.inventory.money() || copper == m_mine.money) {
        return false;
    }
    m_c.body.clear();
    net::writeU32(m_c.body, copper);
    if (!m_c.send(Opcode::CMSG_SET_TRADE_GOLD)) {
        return false;
    }
    m_mine.money = copper;
    m_mine.accepted = false;
    ++m_version;
    return true;
}

bool Trade::accept() {
    if (!open() || m_mine.accepted) {
        return false;
    }
    m_c.body.clear();
    net::writeAcceptTrade(m_c.body);
    if (!m_c.send(Opcode::CMSG_ACCEPT_TRADE)) {
        return false;
    }
    m_mine.accepted = true;
    ++m_version;
    return true;
}

bool Trade::unaccept() {
    if (!open() || !m_mine.accepted || !m_c.sendEmpty(Opcode::CMSG_UNACCEPT_TRADE)) {
        return false;
    }
    m_mine.accepted = false;
    ++m_version;
    return true;
}

i32 Trade::slotOf(u64 guid) const {
    for (u32 i = 0; guid != 0 && i < net::kTradeSlotCount; ++i) {
        if (m_mine.items[i] == guid) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

i32 Trade::freeSlot() const {
    for (u32 i = 0; i < net::kTradeSlotsTraded; ++i) {
        if (m_mine.items[i] == 0) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

} // namespace vd::game
