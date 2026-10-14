#include "game/MasterLoot.h"

#include "game/Social.h"

namespace vd::game {

bool MasterLootSlots::groupHasCandidates(u32 group) const {
    for (u32 i = group * 5; i < group * 5 + 5 && i < kSlots; ++i) {
        if (guid[i] != 0) {
            return true;
        }
    }
    return false;
}

MasterLootSlots masterLootSlots(std::span<const u64> candidates, const GroupState& group, u64 player) {
    MasterLootSlots out;
    out.raid = group.raid;
    u32 next = 0;
    for (const u64 candidate : candidates) {
        if (!out.raid) {
            if (next < MasterLootSlots::kSlots) {
                out.guid[next++] = candidate;
            }
            continue;
        }
        // Its subgroup: the player's own, or the roster's.
        i32 subgroup = candidate == player ? group.ownSubgroup : -1;
        for (const GroupMember& m : group.members) {
            subgroup = m.guid == candidate ? m.subgroup : subgroup;
        }
        if (subgroup < 0 || subgroup >= 8) {
            continue;
        }
        for (u32 i = static_cast<u32>(subgroup) * 5; i < static_cast<u32>(subgroup) * 5 + 5; ++i) {
            if (out.guid[i] == 0) {
                out.guid[i] = candidate;
                break;
            }
        }
    }
    return out;
}

} // namespace vd::game
