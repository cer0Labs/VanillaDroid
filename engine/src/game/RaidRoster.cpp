#include "game/RaidRoster.h"

#include <algorithm>

namespace vd::game {

void layoutRaidGroups(std::span<const PartyMemberView> roster, RaidSlots& out) {
    out.slotOf.fill(-1);
    out.memberIn.fill(-1);
    out.filled.fill(0);
    const u32 count = static_cast<u32>(std::min<usize>(roster.size(), kMaxRaidMembers));
    for (u32 i = 0; i < count; ++i) {
        const u32 group = roster[i].subgroup;
        if (group >= kRaidGroups || out.filled[group] >= kRaidGroupSize) {
            continue; // "To prevent errors when the server hiccups"
        }
        const u32 slot = group * kRaidGroupSize + out.filled[group]++;
        out.slotOf[i] = static_cast<i8>(slot);
        out.memberIn[slot] = static_cast<i8>(i);
    }
}

RaidDrop raidDrop(const RaidSlots& slots, std::span<const PartyMemberView> roster, u32 member, i32 slot) {
    RaidDrop drop;
    if (member >= roster.size() || member >= kMaxRaidMembers || slot < 0 ||
        slot >= static_cast<i32>(kRaidSlots)) {
        return drop;
    }
    const u32 group = static_cast<u32>(slot) / kRaidGroupSize;
    if (group == roster[member].subgroup) {
        return drop; // TARGET_RAID_SLOT:GetParent():GetID() == raidButton.subgroup: back to its slot
    }
    const i32 other = slots.memberIn[static_cast<u32>(slot)];
    if (other >= 0) {
        drop.kind = RaidDropKind::Swap;
        drop.other = other;
    } else {
        drop.kind = RaidDropKind::Move;
        drop.subgroup = static_cast<u8>(group);
    }
    return drop;
}

u32 raidClassColor(u8 cls) {
    // RAID_CLASS_COLORS by the classes' file names (ChrClasses.dbc m_filename of 1.12's ids), r g b as
    // rounded bytes.
    constexpr auto rgb = [](f32 r, f32 g, f32 b) {
        const auto byte = [](f32 v) { return static_cast<u32>(v * 255.0f + 0.5f); };
        return byte(r) | (byte(g) << 8) | (byte(b) << 16) | (0xFFu << 24);
    };
    switch (cls) {
    case 1: return rgb(0.78f, 0.61f, 0.43f); // WARRIOR
    case 2: return rgb(0.96f, 0.55f, 0.73f); // PALADIN
    case 3: return rgb(0.67f, 0.83f, 0.45f); // HUNTER
    case 4: return rgb(1.0f, 0.96f, 0.41f);  // ROGUE
    case 5: return rgb(1.0f, 1.0f, 1.0f);    // PRIEST
    case 7: return rgb(0.96f, 0.55f, 0.73f); // SHAMAN
    case 8: return rgb(0.41f, 0.8f, 0.94f);  // MAGE
    case 9: return rgb(0.58f, 0.51f, 0.79f); // WARLOCK
    case 11: return rgb(1.0f, 0.49f, 0.04f); // DRUID
    default: return 0;
    }
}

bool raidRosterDead(const PartyMemberView& m) {
    if (m.inSight) {
        return m.health == 0;
    }
    return m.online && m.dead; // the group status' MEMBER_STATUS_DEAD (a released ghost has it too)
}

bool canConvertToRaid(const GroupState& group, u64 player) {
    return group.active && !group.raid && group.leader == player && player != 0 && !group.members.empty();
}

} // namespace vd::game
