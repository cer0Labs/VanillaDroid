#pragma once

#include "core/Types.h"
#include "game/ExchangeContext.h"
#include "net/ExchangeMessages.h"

#include <array>
#include <span>

namespace vd::game {

// An item of a group's loot the player may roll for (SMSG_LOOT_START_ROLL), until its result arrives or its
// countdown is long over.
struct PendingRoll {
    u64 object = 0; // the creature (or object) looted
    u32 slot = 0;   // its loot slot
    u32 itemId = 0;
    u32 randomPropertyId = 0;
    u32 startMs = 0; // client clock
    u32 countdownMs = 0;
    bool voted = false; // the player chose (the window goes away)
    u8 vote = 0;        // net::kRoll* once voted

    u32 remainingMs(u32 nowMs) const {
        const u32 elapsed = nowMs - startMs;
        return elapsed >= countdownMs ? 0 : countdownMs - elapsed;
    }
};

// Group loot (phase 8): the rolls the server starts for the player (need / greed / pass with a countdown),
// their results for the chat, and the master looter's candidates. Part of game::Exchange. Fixed storage:
// nothing allocates.
class LootRolls {
public:
    static constexpr u32 kMaxRolls = 16;
    static constexpr u32 kGraceMs = 5000; // a roll whose result never came goes this long after its countdown

    explicit LootRolls(ExchangeContext& context) : m_c(context) {}

    void reset();
    void update(u32 nowMs);
    static bool handles(net::Opcode opcode);
    bool handle(net::Opcode opcode, std::span<const u8> payload);

    // Rolls in progress, oldest first (the voted ones too, until their result: skip them in the roll
    // windows).
    std::span<const PendingRoll> rolls() const { return {m_rolls.data(), m_count}; }
    // Rolls still waiting for the player's choice.
    u32 waiting() const;
    // CMSG_LOOT_ROLL: net::kRollNeed / kRollGreed / kRollPass on a roll in progress. False when unknown or
    // not sent.
    bool roll(u64 object, u32 slot, u8 vote);

    // Master loot: who may receive the items of the loot window the player opened (SMSG_LOOT_MASTER_LIST,
    // sent before the loot response); empty when the group does not use it.
    std::span<const u64> masterCandidates() const { return m_master.list(); }
    void clearMasterCandidates() { m_master.count = 0; }
    // CMSG_LOOT_MASTER_GIVE: the master looter hands the loot window's item in `slot` to `player`.
    bool giveMasterLoot(u64 object, u8 slot, u64 player);

private:
    PendingRoll* find(u64 object, u32 slot);
    void remove(u64 object, u32 slot);

    ExchangeContext& m_c;
    std::array<PendingRoll, kMaxRolls> m_rolls{};
    u32 m_count = 0;
    net::LootMasterList m_master;
};

} // namespace vd::game
