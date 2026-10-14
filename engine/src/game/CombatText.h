#pragma once

#include "core/Types.h"
#include "game/Auras.h"
#include "game/Combat.h"
#include "game/Entities.h"
#include "net/UpdateFields.gen.h"

#include <array>
#include <span>
#include <string_view>

namespace vd::game {

// The combat log as the client's combat log frame reads it (phase 8): one line per CombatEvent, auras gained
// and faded, deaths, experience, with the client's own texts (core/Text: SPELLLOG*, COMBATHIT*,
// PERIODICAURA*, AURAADDED*, COMBATLOG_XPGAIN*...: "Your Fireball hits Young Wolf for 25 Fire damage."), in
// the form of who did it to whom (<BASE>SELFOTHER, OTHERSELF, OTHEROTHER, SELFSELF). Spell and unit names
// stay as the client data and the server give them. No UI here: the panel draws the lines.

// How a line reads (its color in the panel).
enum class CombatLineKind : u8 {
    Outgoing, // what the player (or its pet) does to others
    Incoming, // what others do to the player
    Heal,     // heals and power gains
    Miss,     // misses, dodges, immunities...
    AuraGain, // buffs and debuffs gained
    AuraFade,
    Death, // kills, deaths, experience
    Other, // between other units
};

// Names for the lines: the unit's (player, creature, pet) and the spell's; empty when not known yet.
struct CombatNames {
    u64 player = 0;
    u64 pet = 0;
    const void* context = nullptr;
    std::string_view (*unitName)(const void* context, u64 guid) = nullptr;
    std::string_view (*spellName)(const void* context, u32 spellId) = nullptr;
    // Phase 13 (combat): an item's name (the item cache) and an enchantment's (SpellItemEnchantment.dbc). `unitName`
    // also names the game objects and items a lock-opening spell opens.
    std::string_view (*itemName)(const void* context, u32 entry) = nullptr;
    std::string_view (*enchantName)(const void* context, u32 enchantId) = nullptr;

    std::string_view unit(u64 guid) const;
    std::string_view spell(u32 spellId) const;
    std::string_view item(u32 entry) const;
    std::string_view enchant(u32 enchantId) const;
};

// Name of a spell school (SPELL_SCHOOL<n>_CAP: 0 Physical .. 6 Arcane) and of a power type (MANA, RAGE,
// FOCUS, ENERGY, HAPPINESS; 0xFE health), as the combat log writes them.
std::string_view schoolName(u32 school);
std::string_view powerName(u32 powerType);

// The line of one combat event into `out` (NUL-terminated, cut to `size`). Returns the characters written (0: the
// client writes no line for it, as a leech or a durability loss the player causes itself).
usize formatCombatEvent(char* out, usize size, const CombatEvent& e, const CombatNames& names,
                        CombatLineKind* kind = nullptr);
// AURAADDED* / AURAREMOVED*: "You gain Frost Armor.", "Young Wolf is afflicted by Rend.", "Frost Armor fades
// from you."
usize formatAuraChange(char* out, usize size, u64 unit, u32 spellId, bool gained, bool harmful,
                       const CombatNames& names, CombatLineKind* kind = nullptr);
// SELFKILLOTHER, PARTYKILLOTHER, ERR_KILLED_BY_S: "You have slain Young Wolf!", "Brakk is slain by Kobold!"
usize formatKill(char* out, usize size, u64 killer, u64 victim, const CombatNames& names);
// The experience of SMSG_LOG_XPGAIN (game::GameEventKind::XpGained) and what the line needs of the player.
struct ExperienceGain {
    u64 victim = 0;        // a kill's victim the client knows as a unit; 0: the UNNAMED lines (quests...)
    bool loss = false;     // the player itself as victim
    i32 total = 0;         // with the rest bonus
    i32 base = 0;          // without it
    f32 groupBonus = 1.0f; // the server's factor (vMaNGOS always 1): base / it is what the kill gives alone
    u32 restState = 0;     // the player's (PLAYER_BYTES_2 byte 3), with Exhaustion.dbc's factor and name
    f32 restFactor = 0.0f;
    std::string_view restName;
};
// COMBATLOG_XPGAIN_* as WoW.exe picks them (UnitCombatLogXP_C.cpp, 0x637AC0 / 0x637CD0): a kill while the
// rest state changes the experience (Exhaustion.dbc factor not 0 nor 1) writes
// COMBATLOG_XPGAIN_EXHAUSTION<state> ("Young Wolf dies, you gain 90 experience. (+45 exp Rested bonus)"),
// else FIRSTPERSON; without a victim the UNNAMED forms ("You gain 120 experience."); _GROUP / _RAID when the
// group bonus changed it; COMBATLOG_XPLOSS_FIRSTPERSON_UNNAMED for a loss.
usize formatExperience(char* out, usize size, const ExperienceGain& xp, const CombatNames& names);
// "XP: 45": the text over the player's head for any experience gained (WoW.exe 0x607260, world text type 4).
std::string_view formatExperienceWorldText(std::span<char> out, i32 total);

// What auras a few units gained and lost since the last look, from their aura update fields (the server sends
// no packet for them): the combat log's aura lines. Fixed capacity, no allocation.
class AuraWatch {
public:
    static constexpr u32 kUnits = 4; // the player, its target, its pet, one spare

    // Looks at `units` (0 entries are skipped) and calls change(guid, spellId, gained, harmful) for every
    // aura that appeared or vanished since the last call. A unit seen for the first time reports nothing (its
    // auras were already there).
    template <typename F> void update(const EntityStore& entities, std::span<const u64> units, F&& change);
    void reset() { m_units.fill(Unit{}); }

private:
    struct Unit {
        u64 guid = 0;
        std::array<u32, kMaxAuras> spells{};
        u32 lastSeen = 0; // update() round
    };
    Unit* slotFor(u64 guid);

    std::array<Unit, kUnits> m_units{};
    u32 m_round = 0;
};

// -------------------------------------------------------------------------------------------------------

template <typename F>
void AuraWatch::update(const EntityStore& entities, std::span<const u64> units, F&& change) {
    ++m_round;
    for (const u64 guid : units) {
        const Entity* e = guid ? entities.find(guid) : nullptr;
        if (!e) {
            continue;
        }
        bool fresh = false;
        Unit* u = nullptr;
        for (Unit& candidate : m_units) {
            if (candidate.guid == guid) {
                u = &candidate;
            }
        }
        if (!u) {
            u = slotFor(guid);
            fresh = true;
        }
        if (u->lastSeen == m_round) {
            continue; // listed twice (the target is the pet)
        }
        u->lastSeen = m_round;
        for (u32 slot = 0; slot < kMaxAuras; ++slot) {
            const u32 now = e->value(static_cast<u16>(net::uf::UNIT_FIELD_AURA + slot));
            const u32 before = u->spells[slot];
            if (now == before) {
                continue;
            }
            u->spells[slot] = now;
            if (fresh) {
                continue;
            }
            const bool harmful = slot >= kMaxPositiveAuras;
            if (before != 0) {
                change(guid, before, false, harmful);
            }
            if (now != 0) {
                change(guid, now, true, harmful);
            }
        }
    }
}

} // namespace vd::game
