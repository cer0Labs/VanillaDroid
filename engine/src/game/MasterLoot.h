#pragma once

#include "core/Types.h"

#include <array>
#include <span>

namespace vd::game {

struct GroupState;

// Phase 15 (items): the master looter's candidates as GetMasterLootCandidate(1..40) numbers them, which is
// what LootFrame.lua's GroupLootDropDown lists. WoW.exe (0x61C550, SMSG_LOOT_MASTER_LIST): in a party, the
// list's order; in a raid, five slots per subgroup (index = group * 5 + n), each candidate in the first free
// slot of its group (one whose group is not known is left out, as the client finds no raid member for it).
struct MasterLootSlots {
    static constexpr u32 kSlots = 40;
    static constexpr u32 kPartySlots = 5; // MAX_PARTY_MEMBERS + 1: what the party's menu lists
    std::array<u64, kSlots> guid{};
    bool raid = false;
    // Whether raid group `group` (0..7) has a candidate (its "Group n" entry).
    bool groupHasCandidates(u32 group) const;
};
MasterLootSlots masterLootSlots(std::span<const u64> candidates, const GroupState& group, u64 player);

} // namespace vd::game
