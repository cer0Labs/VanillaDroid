#include "app/RaidPulloutSamples.h"

#include "app/AuraLayout.h"
#include "core/Hash.h"
#include "game/RaidRoster.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace vd {
namespace {

constexpr u64 kFortitude = assetPathHash("Interface\\Icons\\Spell_Holy_WordFortitude.blp");
constexpr u64 kWild = assetPathHash("Interface\\Icons\\Spell_Nature_Regeneration.blp");
constexpr u64 kIntellect = assetPathHash("Interface\\Icons\\Spell_Holy_ArcaneIntellect.blp");
constexpr u64 kShout = assetPathHash("Interface\\Icons\\Ability_Warrior_BattleShout.blp");
constexpr u64 kMight = assetPathHash("Interface\\Icons\\Spell_Holy_FistOfJustice.blp");
constexpr u64 kRenew = assetPathHash("Interface\\Icons\\Spell_Holy_Renew.blp");
constexpr u64 kShield = assetPathHash("Interface\\Icons\\Spell_Holy_PowerWordShield.blp");
constexpr u64 kRejuvenation = assetPathHash("Interface\\Icons\\Spell_Nature_Rejuvenation.blp");
constexpr u64 kThorns = assetPathHash("Interface\\Icons\\Spell_Nature_Thorns.blp");
constexpr u64 kCurse = assetPathHash("Interface\\Icons\\Spell_Shadow_CurseOfSargeras.blp");
constexpr u64 kCursed = assetPathHash("Interface\\Icons\\Ability_Creature_Cursed_02.blp");
constexpr u64 kPain = assetPathHash("Interface\\Icons\\Spell_Shadow_ShadowWordPain.blp");
constexpr u64 kDisease = assetPathHash("Interface\\Icons\\Ability_Creature_Disease_02.blp");
constexpr u64 kPoison = assetPathHash("Interface\\Icons\\Ability_Creature_Poison_02.blp");
constexpr u64 kFrostNova = assetPathHash("Interface\\Icons\\Spell_Frost_FrostNova.blp");
constexpr u64 kImmolation = assetPathHash("Interface\\Icons\\Spell_Fire_Immolation.blp");
constexpr u64 kWeakenedSoul = assetPathHash("Interface\\Icons\\Spell_Holy_AshesToAshes.blp");

enum : u8 { Warrior = 1, Paladin = 2, Hunter = 3, Rogue = 4, Priest = 5, Mage = 8, Warlock = 9, Druid = 11 };
enum : u8 { Human = 1, Dwarf = 3, NightElf = 4, Gnome = 7 };

constexpr u8 powerOf(u8 cls) {
    return cls == Warrior ? 1 : cls == Rogue ? 3 : 0; // rage, energy, mana
}

struct Row {
    const char* name;
    u8 cls, race, gender, subgroup;
    f32 health, power;
    u8 state; // 1 dead, 2 offline
    std::array<RaidSampleAura, 4> debuffs;
    std::array<RaidSampleAura, 4> buffs;
};

constexpr RaidSampleAura aura(u64 icon, u8 dispel = 0) {
    return {icon, dispel};
}

// Join order: the raid index order of the roster (Group 1 is index 0's, the player's Group 3).
constexpr Row kRows[] = {
    {"Brakmor",
     Warrior,
     Dwarf,
     0,
     0,
     0.42f,
     0.65f,
     0,
     {aura(kCurse, 2), aura(kPain, 1), aura(kDisease, 3), aura(kPoison, 4)},
     {aura(kFortitude), aura(kWild), aura(kShout), aura(kMight)}},
    {"Aeryn",
     Priest,
     Human,
     1,
     0,
     0.93f,
     0.48f,
     0,
     {aura(kWeakenedSoul)},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kRenew)}},
    {"Kaelra",
     Rogue,
     NightElf,
     1,
     1,
     0.88f,
     0.70f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kShout), aura(kMight)}},
    {"Tolvir", Warrior, Human, 0, 0, 0.0f, 0.0f, 1, {}, {}},
    {"Durgan",
     Warrior,
     Dwarf,
     0,
     1,
     0.71f,
     0.30f,
     0,
     {aura(kFrostNova, 1)},
     {aura(kFortitude), aura(kShout), aura(kShield)}},
    {"Lunara",
     Druid,
     NightElf,
     1,
     0,
     1.0f,
     0.82f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kThorns)}},
    {"Mirabel", Priest, Dwarf, 1, 1, 0.99f, 0.36f, 0, {}, {aura(kFortitude), aura(kWild), aura(kIntellect)}},
    {"Sevrin",
     Mage,
     Gnome,
     0,
     0,
     0.64f,
     0.55f,
     0,
     {aura(kImmolation, 1)},
     {aura(kFortitude), aura(kWild), aura(kIntellect)}},
    {"Fenwick", Warlock, Gnome, 0, 1, 1.0f, 1.0f, 2, {}, {}},
    {"Ostra",
     Hunter,
     Dwarf,
     1,
     1,
     0.80f,
     0.74f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kShout)}},
    {"Hildra",
     Paladin,
     Dwarf,
     1,
     2,
     0.95f,
     0.61f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kMight)}},
    {"Coralie",
     Mage,
     Human,
     1,
     2,
     0.58f,
     0.27f,
     0,
     {aura(kCursed, 2)},
     {aura(kFortitude), aura(kWild), aura(kIntellect)}},
    {"Arthel",
     Warrior,
     Human,
     0,
     2,
     0.77f,
     0.40f,
     0,
     {aura(kPoison, 4)},
     {aura(kFortitude), aura(kWild), aura(kShout), aura(kMight)}},
    {"Thane", Rogue, Human, 0, 2, 0.90f, 0.95f, 0, {}, {aura(kFortitude), aura(kWild), aura(kShout)}},
    {"Ysolde",
     Priest,
     NightElf,
     1,
     2,
     0.51f,
     0.62f,
     0,
     {aura(kImmolation, 1), aura(kCursed, 2)},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kRenew)}},
    {"Grimbold",
     Warrior,
     Dwarf,
     0,
     3,
     0.35f,
     0.85f,
     0,
     {aura(kPoison, 4), aura(kDisease, 3)},
     {aura(kFortitude), aura(kWild), aura(kShout), aura(kShield)}},
    {"Nyssa",
     Druid,
     NightElf,
     1,
     3,
     0.98f,
     0.44f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kRejuvenation)}},
    {"Pell", Mage, Gnome, 0, 3, 0.0f, 0.0f, 1, {}, {}},
    {"Vashti",
     Warlock,
     Human,
     1,
     3,
     0.67f,
     0.71f,
     0,
     {aura(kPain, 1)},
     {aura(kFortitude), aura(kWild), aura(kIntellect)}},
    {"Orrin",
     Hunter,
     NightElf,
     0,
     3,
     0.86f,
     0.58f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kShout)}},
    {"Talwyn", Rogue, NightElf, 0, 4, 0.74f, 0.33f, 0, {}, {aura(kFortitude), aura(kWild), aura(kShout)}},
    {"Bram",
     Warrior,
     Human,
     0,
     4,
     0.61f,
     0.52f,
     0,
     {aura(kCursed, 2)},
     {aura(kFortitude), aura(kWild), aura(kShout), aura(kMight)}},
    {"Celestine",
     Priest,
     Human,
     1,
     4,
     0.97f,
     0.73f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kShield)}},
    {"Quill", Mage, Gnome, 1, 4, 0.83f, 0.66f, 0, {}, {aura(kFortitude), aura(kWild), aura(kIntellect)}},
    {"Dorran",
     Paladin,
     Human,
     0,
     4,
     0.89f,
     0.47f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kMight)}},
    {"Ashka", Hunter, Dwarf, 1, 5, 0.92f, 0.81f, 0, {}, {aura(kFortitude), aura(kWild), aura(kIntellect)}},
    {"Fizzlebolt",
     Mage,
     Gnome,
     0,
     5,
     0.47f,
     0.19f,
     0,
     {aura(kFrostNova, 1)},
     {aura(kFortitude), aura(kWild), aura(kIntellect)}},
    {"Rowena", Priest, Human, 1, 5, 1.0f, 0.90f, 0, {}, {aura(kFortitude), aura(kWild), aura(kIntellect)}},
    {"Garrick",
     Warrior,
     Human,
     0,
     5,
     0.56f,
     0.12f,
     0,
     {aura(kPain, 1)},
     {aura(kFortitude), aura(kWild), aura(kShout)}},
    {"Ilsa", Rogue, Gnome, 1, 5, 0.81f, 0.60f, 0, {}, {aura(kFortitude), aura(kWild), aura(kShout)}},
    {"Morwen", Warlock, Human, 1, 6, 0.78f, 0.57f, 0, {}, {aura(kFortitude), aura(kWild), aura(kIntellect)}},
    {"Thalric",
     Warrior,
     Dwarf,
     0,
     6,
     0.69f,
     0.38f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kShout), aura(kMight)}},
    {"Elowen",
     Druid,
     NightElf,
     1,
     6,
     0.94f,
     0.68f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kThorns)}},
    {"Pip", Rogue, Gnome, 0, 6, 0.85f, 1.0f, 0, {}, {aura(kFortitude), aura(kWild)}},
    {"Saranna", Priest, Dwarf, 1, 6, 0.90f, 0.29f, 0, {}, {aura(kFortitude), aura(kWild), aura(kIntellect)}},
    {"Kord",
     Warrior,
     Human,
     0,
     7,
     0.28f,
     0.91f,
     0,
     {aura(kDisease, 3), aura(kCurse, 2), aura(kFrostNova, 1)},
     {aura(kFortitude), aura(kWild), aura(kShout)}},
    {"Vex", Warlock, Gnome, 0, 7, 0.73f, 0.80f, 0, {}, {aura(kFortitude), aura(kWild), aura(kIntellect)}},
    {"Liora", Hunter, NightElf, 1, 7, 1.0f, 1.0f, 2, {}, {}},
    {"Marek",
     Paladin,
     Dwarf,
     0,
     7,
     0.88f,
     0.54f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kMight)}},
    {"Wynn",
     Druid,
     NightElf,
     1,
     7,
     0.97f,
     0.76f,
     0,
     {},
     {aura(kFortitude), aura(kWild), aura(kIntellect), aura(kRejuvenation)}},
};
static_assert(std::size(kRows) == 40, "a full raid");

struct Samples {
    std::array<RaidSampleMember, std::size(kRows)> members{};
    Samples() {
        for (u32 i = 0; i < members.size(); ++i) {
            const Row& r = kRows[i];
            RaidSampleMember& m = members[i];
            m.name = r.name;
            m.cls = r.cls;
            m.race = r.race;
            m.gender = r.gender;
            m.subgroup = r.subgroup;
            m.powerType = powerOf(r.cls);
            m.health = r.health;
            m.power = r.power;
            m.dead = r.state == 1;
            m.offline = r.state == 2;
            m.inSight = r.subgroup < 4; // Groups 5 to 8 are far away (their stats come from the server)
            for (const RaidSampleAura& a : r.debuffs) {
                if (a.iconHash != 0) {
                    m.debuffs[m.debuffCount++] = a;
                }
            }
            for (const RaidSampleAura& a : r.buffs) {
                if (a.iconHash != 0) {
                    m.buffs[m.buffCount++] = a;
                }
            }
        }
    }
};

} // namespace

std::span<const RaidSampleMember> raidSampleMembers() {
    static const Samples samples;
    return samples.members;
}

RaidPulloutMemberView raidSampleView(const RaidSampleMember& m, bool buffs, u32 nowMs) {
    RaidPulloutMemberView v;
    v.name = m.name;
    if (m.offline) {
        // RaidPullout_Update's offline branch: gray name, full gray health and mana bars.
        v.nameColor = kRaidOfflineColor;
        v.health = 1.0f;
        v.healthColor = kRaidOfflineColor;
        v.power = 1.0f;
        v.powerColor = kRaidOfflineColor;
        return v;
    }
    const u32 colour = game::raidClassColor(m.cls);
    v.nameColor = m.dead ? kRaidDeadColor : colour != 0 ? colour : kRaidTitleColor;
    // A fight's slow ebb and flow, different for each member.
    const f32 phase = static_cast<f32>(nowMs % 20000u) / 20000.0f * 6.2831853f + static_cast<f32>(m.name[0]);
    v.health = m.dead ? 0.0f : std::clamp(m.health + 0.08f * std::sin(phase), 0.02f, 1.0f);
    v.power = m.dead ? 0.0f : std::clamp(m.power + 0.05f * std::cos(phase * 1.3f), 0.0f, 1.0f);
    v.powerColor = raidPowerColor(m.powerType);
    const std::array<RaidSampleAura, 4>& auras = buffs ? m.buffs : m.debuffs;
    const u32 count = buffs ? m.buffCount : m.debuffCount;
    for (u32 i = 0; i < count && i < kRaidPulloutAuras; ++i) {
        // RefreshBuffs: the debuff's type colour, "none" red for the rest (the buffs' too).
        v.auras[v.auraCount++] = {auras[i].iconHash, debuffTypeColor(buffs ? 0 : auras[i].dispel)};
    }
    return v;
}

} // namespace vd
