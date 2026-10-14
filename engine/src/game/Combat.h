#pragma once

#include "core/Types.h"
#include "game/EventRing.h"
#include "net/GameMessages.h"
#include "net/SpellLogMessages.h"

namespace vd::game {

// One line of the combat log, for floating combat text and the combat log frame.
struct CombatEvent {
    enum class Kind : u8 {
        Damage, // amount = damage taken (after absorb, resist and block)
        Miss,
        Dodge,
        Parry,
        Block, // blocked completely (a partial block is Damage with `blocked` > 0)
        Evade,
        Immune,
        Deflect,
        Resist, // resisted completely
        Absorb, // absorbed completely
        Reflect,
        Interrupt,
        Heal,     // amount = health restored
        Energize, // amount = power gained, school = the power type
        Drain,    // mana leech: amount = power drained from the victim, school = the power type
        // Phase 13 (combat): what SMSG_SPELLLOGEXECUTE, dispels, instant kills and item enchantments add (their
        // lines: game/CombatText). Drain also comes from SPELLLOGEXECUTE (a drain, or a leech when `gain` > 0).
        ExtraAttacks,     // victim = who gains them, amount = how many
        CreateItem,       // attacker = who makes it, item = its entry
        FeedPet,          // attacker = the pet's owner, item = the food
        OpenLock,         // attacker = who opens, victim = the game object or item opened
        DurabilityDamage, // attacker, victim, item = the item damaged (0: every equipped item)
        DismissPet,       // attacker = the owner, victim = the pet
        SpellCast,        // an effect without a line of its own, on `victim` (0: none); kPerform for abilities
        Dispelled,        // victim lost its aura `spellId` (attacker = the dispeller)
        DispelFailed,     // attacker failed to remove victim's aura `spellId`
        InstaKill,        // `spellId` killed victim outright
        EnchantAdded,     // attacker put enchantment `amount` (SpellItemEnchantment) on victim's item `item`
        EnchantFaded,     // enchantment `amount` faded from victim's item `item`
    };
    static constexpr u8 kCrit = 0x01;
    static constexpr u8 kGlancing = 0x02;
    static constexpr u8 kCrushing = 0x04;
    static constexpr u8 kPeriodic = 0x08; // damage / heal over time
    static constexpr u8 kOffHand = 0x10;
    static constexpr u8 kEnvironment = 0x20; // falling, drowning, lava...: spellId = the environment type
    static constexpr u8 kDamageShield = 0x40; // Damage: a damage shield (Thorns) struck its bearer's attacker
    static constexpr u8 kPerform = 0x80;      // SpellCast: an ability (SPELL_ATTR_IS_ABILITY): "perform", not "cast"

    u32 timeMs = 0;
    u64 attacker = 0; // who did it (caster, healer); 0 for the environment
    u64 victim = 0;   // who it happened to
    u32 spellId = 0;  // 0 = melee swing
    Kind kind = Kind::Damage;
    u8 flags = 0;
    u8 school = 0; // SpellSchools index 0..6 (0 physical); the power type for Energize / Drain
    i32 amount = 0;
    u32 absorbed = 0;
    u32 resisted = 0;
    u32 blocked = 0;
    u32 item = 0; // phase 13: an item entry (CreateItem, FeedPet, DurabilityDamage, enchantments)
    u32 gain = 0; // phase 13: Drain: what the caster gained (a leech; 0: a plain drain)
    u32 hitInfo = 0; // phase 14 (bodyfx): a melee swing's HitInfo (net::kHitInfo*), 0 otherwise
    // Phase 15 (pets): where it came from, for the portraits' damage numbers (game/CombatFeedback: WoW.exe fires
    // UNIT_COMBAT from some packets only).
    bool melee = false;      // SMSG_ATTACKERSTATEUPDATE (a swing, or an "on next swing" ability)
    bool noFeedback = false; // SMSG_SPELLORDAMAGE_IMMUNE / SMSG_PROCRESIST: no UNIT_COMBAT

    // A melee swing: no spell, not the environment, not a damage shield.
    bool swing() const { return spellId == 0 && (flags & (kEnvironment | kDamageShield)) == 0; }
    // Phase 13: a line about something else than a blow (an item made, a dispel, an enchantment...): no animation,
    // no sound, no floating text.
    bool logOnly() const { return kind >= Kind::ExtraAttacks; }
};

// Phase 13 (combat): what the lines of SMSG_SPELLLOGEXECUTE need from a spell (Spell.dbc), as UnitCombatLog_C
// decides them in WoW.exe.
struct ExecuteLogSpell {
    bool perform = false; // SPELL_ATTR_ABILITY (0x10): SIMPLEPERFORM / SPELLTERSEPERFORM instead of *CAST
    bool hidden = false;  // hidden from the log (attributes 0x180) or without a name: drains, extra attacks, lock
                          // openings, durability damage and interrupts write no line
    bool quiet = false;   // no generic line either: hidden, or a LEARN_SPELL / OPEN_LOCK effect
};
struct ExecuteLogSpells {
    ExecuteLogSpell (*of)(const void* context, u32 spellId) = nullptr;
    const void* context = nullptr;
    ExecuteLogSpell operator()(u32 spellId) const { return of ? of(context, spellId) : ExecuteLogSpell{}; }
};

// The combat log packets turned into CombatEvents (every unit the server tells about: the HUD filters by
// GUID).
class CombatLog {
public:
    static constexpr u32 kCapacity = 128;
    using Ring = EventRing<CombatEvent, kCapacity>;

    void onAttackerStateUpdate(const net::AttackerStateUpdate& m, u32 nowMs);
    void onSpellDamage(const net::SpellDamageLog& m, u32 nowMs);
    void onSpellHeal(const net::SpellHealLog& m, u32 nowMs);
    void onSpellEnergize(const net::SpellEnergizeLog& m, u32 nowMs);
    void onPeriodicAura(const net::PeriodicAuraLog& m, u32 nowMs);
    void onSpellLogMiss(const net::SpellLogMiss& m, u32 nowMs);
    // The misses of a SMSG_SPELL_GO (a resisted Frostbolt only shows there).
    void onSpellGoMisses(const net::SpellGo& m, u32 nowMs);
    void onImmune(const net::SpellImmuneLog& m, u32 nowMs);
    void onProcResist(const net::SpellImmuneLog& m, u32 nowMs);
    void onEnvironmental(const net::EnvironmentalDamageLog& m, u32 nowMs);
    // Phase 13 (combat): the packets WoW.exe's UnitCombatLog_C reads and the client ignored until now.
    void onSpellLogExecute(const net::SpellLogExecute& m, const ExecuteLogSpells& spells, u32 nowMs);
    void onDamageShield(const net::SpellDamageShield& m, u32 nowMs);
    void onDispelLog(const net::SpellDispelLog& m, u32 nowMs);
    void onDispelFailed(const net::DispelFailed& m, u32 nowMs);
    void onInstaKill(const net::SpellInstaKillLog& m, u32 nowMs);
    void onEnchantmentLog(const net::EnchantmentLog& m, u32 nowMs);

    const Ring& events() const { return m_events; }

private:
    CombatEvent& push(u32 nowMs, u64 attacker, u64 victim, u32 spellId);
    void pushMiss(u32 nowMs, u64 attacker, u64 victim, u32 spellId, net::SpellMiss miss);

    Ring m_events;
};

// The player's auto attack (melee), as the server confirms it.
class MeleeState {
public:
    bool attacking() const { return m_attacking; }
    u64 target() const { return m_target; }

    // CMSG_ATTACKSWING sent: shown at once, SMSG_ATTACKSTART / ATTACKSTOP settle it.
    void requested(u64 target);
    // True when this changed the player's state.
    bool onAttackStart(const net::AttackStart& m, u64 player);
    bool onAttackStop(const net::AttackStop& m, u64 player);
    void stop();

private:
    bool m_attacking = false;
    u64 m_target = 0;
};

} // namespace vd::game
