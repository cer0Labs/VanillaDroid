#pragma once

#include "app/RaidPulloutWindows.h"
#include "core/Types.h"

#include <array>
#include <span>

namespace vd {

// Phase 16 (pullouts, development): a full raid of 40 with sample data for the gallery's "pullouts" page and
// VD_RAID_DEMO (app/RaidPullouts over the --walk HUD): eight subgroups of five, the classes of an Alliance
// raid, two dead, two offline, debuffs of every dispel type and the usual buffs. Names and data are invented
// (English, as the server sends names); the icons are the client's.

struct RaidSampleAura {
    u64 iconHash = 0;
    u8 dispel = 0; // SpellDispelType.dbc: 1 magic, 2 curse, 3 disease, 4 poison (0: none)
};

struct RaidSampleMember {
    const char* name = "";
    u8 cls = 0; // ChrClasses
    u8 race = 1, gender = 0;
    u8 subgroup = 0; // 0..7
    u8 powerType = 0;
    u8 level = 60;
    f32 health = 1.0f, power = 1.0f; // 0..1
    bool dead = false, offline = false, inSight = true;
    std::array<RaidSampleAura, 4> debuffs{};
    u32 debuffCount = 0;
    std::array<RaidSampleAura, 4> buffs{};
    u32 buffCount = 0;
};

// The raid in raid index order; raidSampleMembers()[kRaidSampleSelf] is the player (subgroup 2: "Group 3").
std::span<const RaidSampleMember> raidSampleMembers();
constexpr u32 kRaidSampleSelf = 12;

// A member's pullout view at `nowMs`, its health and mana moving a little as in a fight; `buffs`: the
// pullout's "Show Buffs" (else its debuffs).
RaidPulloutMemberView raidSampleView(const RaidSampleMember& m, bool buffs, u32 nowMs);

} // namespace vd
