#include "game/LootRolls.h"

#include "net/GameMessages.h"

namespace vd::game {

using net::Opcode;

void LootRolls::reset() {
    m_count = 0;
    m_master.count = 0;
}

void LootRolls::update(u32 nowMs) {
    for (u32 i = 0; i < m_count;) {
        const PendingRoll& r = m_rolls[i];
        if (nowMs - r.startMs > r.countdownMs + kGraceMs) {
            remove(r.object, r.slot); // the result never came (out of range, the corpse despawned...)
        } else {
            ++i;
        }
    }
}

bool LootRolls::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::SMSG_LOOT_START_ROLL:
    case Opcode::SMSG_LOOT_ROLL:
    case Opcode::SMSG_LOOT_ROLL_WON:
    case Opcode::SMSG_LOOT_ALL_PASSED:
    case Opcode::SMSG_LOOT_MASTER_LIST: return true;
    default: return false;
    }
}

bool LootRolls::handle(Opcode opcode, std::span<const u8> p) {
    switch (opcode) {
    case Opcode::SMSG_LOOT_START_ROLL: {
        net::LootStartRoll m;
        if (!net::parseLootStartRoll(p, m)) {
            return false;
        }
        PendingRoll* r = find(m.object, m.slot);
        if (!r) {
            if (m_count == kMaxRolls) {
                remove(m_rolls[0].object,
                       m_rolls[0].slot); // the oldest goes (its countdown is the furthest along)
            }
            r = &m_rolls[m_count++];
        }
        *r = PendingRoll{};
        r->object = m.object;
        r->slot = m.slot;
        r->itemId = m.itemId;
        r->randomPropertyId = m.randomPropertyId;
        r->startMs = m_c.nowMs;
        r->countdownMs = m.countdownMs;
        m_c.queries.item(m.itemId); // its name, icon and tooltip for the window
        GameEvent& e = m_c.event(GameEventKind::LootRollStarted, m.itemId, m.object);
        e.value = {static_cast<i32>(m.slot), static_cast<i32>(m.countdownMs), 0};
        return true;
    }
    case Opcode::SMSG_LOOT_ROLL: {
        net::LootRoll m;
        if (!net::parseLootRoll(p, m)) {
            return false;
        }
        const net::LootRollKind kind = m.kind();
        if (m.roller == m_c.player) {
            if (PendingRoll* r = find(m.object, m.slot)) {
                r->voted = true; // the server took the choice (made here, or elsewhere after a reconnect)
                r->vote = kind == net::LootRollKind::Passed ? net::kRollPass
                          : kind == net::LootRollKind::ChoseGreed || kind == net::LootRollKind::GreedRoll
                              ? net::kRollGreed
                              : net::kRollNeed;
            }
        } else if (m.roller != 0) {
            m_c.queries.playerName(m.roller);
        }
        m_c.queries.item(m.itemId);
        GameEvent& e = m_c.event(GameEventKind::LootRolled, m.itemId, m.object);
        e.other = m.roller;
        e.value = {static_cast<i32>(kind), m.number, static_cast<i32>(m.slot)};
        return true;
    }
    case Opcode::SMSG_LOOT_ROLL_WON: {
        net::LootRollWon m;
        if (!net::parseLootRollWon(p, m)) {
            return false;
        }
        remove(m.object, m.slot);
        if (m.winner != m_c.player && m.winner != 0) {
            m_c.queries.playerName(m.winner);
        }
        m_c.queries.item(m.itemId);
        GameEvent& e = m_c.event(GameEventKind::LootRollWon, m.itemId, m.object);
        e.other = m.winner;
        e.value = {m.number, m.type, static_cast<i32>(m.slot)};
        return true;
    }
    case Opcode::SMSG_LOOT_ALL_PASSED: {
        net::LootAllPassed m;
        if (!net::parseLootAllPassed(p, m)) {
            return false;
        }
        remove(m.object, m.slot);
        m_c.queries.item(m.itemId);
        m_c.event(GameEventKind::LootAllPassed, m.itemId, m.object).value[2] = static_cast<i32>(m.slot);
        return true;
    }
    case Opcode::SMSG_LOOT_MASTER_LIST:
        if (!net::parseLootMasterList(p, m_master)) {
            return false;
        }
        for (const u64 guid : m_master.list()) {
            if (guid != m_c.player) {
                m_c.queries.playerName(guid); // the picker shows their names
            }
        }
        return true;
    default: return false;
    }
}

u32 LootRolls::waiting() const {
    u32 n = 0;
    for (const PendingRoll& r : rolls()) {
        n += r.voted ? 0 : 1;
    }
    return n;
}

bool LootRolls::roll(u64 object, u32 slot, u8 vote) {
    PendingRoll* r = find(object, slot);
    if (!r || vote > net::kRollGreed) {
        return false;
    }
    m_c.body.clear();
    net::writeLootRoll(m_c.body, object, slot, vote);
    if (!m_c.send(Opcode::CMSG_LOOT_ROLL)) {
        return false;
    }
    r->voted = true;
    r->vote = vote;
    return true;
}

bool LootRolls::giveMasterLoot(u64 object, u8 slot, u64 player) {
    if (object == 0 || player == 0) {
        return false;
    }
    m_c.body.clear();
    net::writeLootMasterGive(m_c.body, object, slot, player);
    return m_c.send(Opcode::CMSG_LOOT_MASTER_GIVE);
}

PendingRoll* LootRolls::find(u64 object, u32 slot) {
    for (u32 i = 0; i < m_count; ++i) {
        if (m_rolls[i].object == object && m_rolls[i].slot == slot) {
            return &m_rolls[i];
        }
    }
    return nullptr;
}

void LootRolls::remove(u64 object, u32 slot) {
    for (u32 i = 0; i < m_count; ++i) {
        if (m_rolls[i].object == object && m_rolls[i].slot == slot) {
            for (u32 j = i + 1; j < m_count; ++j) {
                m_rolls[j - 1] = m_rolls[j];
            }
            --m_count;
            return;
        }
    }
}

} // namespace vd::game
