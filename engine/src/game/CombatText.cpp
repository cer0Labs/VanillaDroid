#include "game/CombatText.h"

#include "core/Text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace vd::game {
namespace {

// Bounded appends of the table's texts into a caller's buffer.
struct Line {
    char* out;
    usize size;
    usize used = 0;

    template <typename... Args> void put(text::Key key, const Args&... args) {
        if (used + 1 >= size) {
            return;
        }
        used += text::format(std::span<char>(out + used, size - used), key, args...).size();
    }
};

// Who did it to whom, as the client's combat log texts name their forms: <BASE>SELFOTHER (the player to
// another unit), OTHERSELF (another unit to the player), OTHEROTHER, SELFSELF.
enum class Who : u8 { SelfOther, OtherSelf, OtherOther, SelfSelf };

Who who(const CombatEvent& e, u64 player) {
    const bool mine = e.attacker != 0 && e.attacker == player;
    const bool onMe = e.victim == player;
    return mine ? (onMe ? Who::SelfSelf : Who::SelfOther) : onMe ? Who::OtherSelf : Who::OtherOther;
}

// The forms of one text; a family without SELFSELF (swings, blocks, interrupts) writes OTHEROTHER for it.
struct Forms {
    text::Key selfOther, otherSelf, otherOther;
    text::Key selfSelf{""};
    constexpr bool hasSelfSelf() const { return !selfSelf.name.empty(); }
};

Who resolve(Who w, const Forms& f) {
    return w == Who::SelfSelf && !f.hasSelfSelf() ? Who::OtherOther : w;
}

// Hits, [crit][school]. Swings (COMBATHIT*): SELFOTHER (victim, amount, school), OTHERSELF (attacker, amount,
// school), OTHEROTHER (attacker, victim, amount, school); the forms without SCHOOL ignore the school.
constexpr Forms kSwingHit[2][2] = {
    {{"COMBATHITSELFOTHER", "COMBATHITOTHERSELF", "COMBATHITOTHEROTHER"},
     {"COMBATHITSCHOOLSELFOTHER", "COMBATHITSCHOOLOTHERSELF", "COMBATHITSCHOOLOTHEROTHER"}},
    {{"COMBATHITCRITSELFOTHER", "COMBATHITCRITOTHERSELF", "COMBATHITCRITOTHEROTHER"},
     {"COMBATHITCRITSCHOOLSELFOTHER", "COMBATHITCRITSCHOOLOTHERSELF", "COMBATHITCRITSCHOOLOTHEROTHER"}},
};
// Spells (SPELLLOG*): SELFOTHER (spell, victim, amount, school), OTHERSELF (caster, spell, amount, school),
// OTHEROTHER (caster, spell, victim, amount, school), SELFSELF (spell, amount, school).
constexpr Forms kSpellHit[2][2] = {
    {{"SPELLLOGSELFOTHER", "SPELLLOGOTHERSELF", "SPELLLOGOTHEROTHER", "SPELLLOGSELFSELF"},
     {"SPELLLOGSCHOOLSELFOTHER", "SPELLLOGSCHOOLOTHERSELF", "SPELLLOGSCHOOLOTHEROTHER",
      "SPELLLOGSCHOOLSELFSELF"}},
    {{"SPELLLOGCRITSELFOTHER", "SPELLLOGCRITOTHERSELF", "SPELLLOGCRITOTHEROTHER", "SPELLLOGCRITSELFSELF"},
     {"SPELLLOGCRITSCHOOLSELFOTHER", "SPELLLOGCRITSCHOOLOTHERSELF", "SPELLLOGCRITSCHOOLOTHEROTHER",
      "SPELLLOGCRITSCHOOLSELFSELF"}},
};
// Damage over time: SELFOTHER (victim, amount, school, spell), OTHERSELF (amount, school, caster, spell),
// OTHEROTHER (victim, amount, school, caster, spell), SELFSELF (amount, school, spell).
constexpr Forms kPeriodicDamage{"PERIODICAURADAMAGESELFOTHER", "PERIODICAURADAMAGEOTHERSELF",
                                "PERIODICAURADAMAGEOTHEROTHER", "PERIODICAURADAMAGESELFSELF"};
// Heals, [crit]: SELFOTHER (spell, victim, amount), OTHERSELF (healer, spell, amount), OTHEROTHER (healer,
// spell, victim, amount), SELFSELF (spell, amount).
constexpr Forms kHeal[2] = {
    {"HEALEDSELFOTHER", "HEALEDOTHERSELF", "HEALEDOTHEROTHER", "HEALEDSELFSELF"},
    {"HEALEDCRITSELFOTHER", "HEALEDCRITOTHERSELF", "HEALEDCRITOTHEROTHER", "HEALEDCRITSELFSELF"},
};
// Heals over time: SELFOTHER (victim, amount, spell), OTHERSELF (amount, healer, spell), OTHEROTHER (victim,
// amount, healer, spell), SELFSELF (amount, spell).
constexpr Forms kPeriodicHeal{"PERIODICAURAHEALSELFOTHER", "PERIODICAURAHEALOTHERSELF",
                              "PERIODICAURAHEALOTHEROTHER", "PERIODICAURAHEALSELFSELF"};
// Power gains: SELFOTHER (victim, amount, power, spell), OTHERSELF (amount, power, caster, spell), OTHEROTHER
// (victim, amount, power, caster, spell), SELFSELF (amount, power, spell).
constexpr Forms kPowerGain{"POWERGAINSELFOTHER", "POWERGAINOTHERSELF", "POWERGAINOTHEROTHER",
                           "POWERGAINSELFSELF"};
// Power drains: SELFOTHER (spell, amount, power, victim), OTHERSELF (caster, spell, amount, power),
// OTHEROTHER (caster, spell, amount, power, victim), SELFSELF (spell, amount, power).
constexpr Forms kPowerDrain{"SPELLPOWERDRAINSELFOTHER", "SPELLPOWERDRAINOTHERSELF",
                            "SPELLPOWERDRAINOTHEROTHER", "SPELLPOWERDRAINSELFSELF"};
// Interrupts: SELFOTHER (victim, spell), OTHERSELF (attacker, spell), OTHEROTHER (attacker, victim, spell).
constexpr Forms kInterrupt{"SPELLINTERRUPTSELFOTHER", "SPELLINTERRUPTOTHERSELF", "SPELLINTERRUPTOTHEROTHER"};
// Environmental damage by EnvironmentalDamageType (fatigue, drowning, falling, lava, slime, fire): _SELF
// (amount), _OTHER (victim, amount).
constexpr text::Key kEnvironmentSelf[] = {
    "VSENVIRONMENTALDAMAGE_FATIGUE_SELF", "VSENVIRONMENTALDAMAGE_DROWNING_SELF",
    "VSENVIRONMENTALDAMAGE_FALLING_SELF", "VSENVIRONMENTALDAMAGE_LAVA_SELF",
    "VSENVIRONMENTALDAMAGE_SLIME_SELF",   "VSENVIRONMENTALDAMAGE_FIRE_SELF"};
constexpr text::Key kEnvironmentOther[] = {
    "VSENVIRONMENTALDAMAGE_FATIGUE_OTHER", "VSENVIRONMENTALDAMAGE_DROWNING_OTHER",
    "VSENVIRONMENTALDAMAGE_FALLING_OTHER", "VSENVIRONMENTALDAMAGE_LAVA_OTHER",
    "VSENVIRONMENTALDAMAGE_SLIME_OTHER",   "VSENVIRONMENTALDAMAGE_FIRE_OTHER"};

// A spell that did not land (SPELLMISS*, SPELLDODGED*...): SELFOTHER (spell, victim), OTHERSELF (caster,
// spell), OTHEROTHER (caster, spell, victim), SELFSELF (spell).
const Forms& spellAvoided(CombatEvent::Kind kind) {
    using K = CombatEvent::Kind;
    static constexpr Forms kMiss{"SPELLMISSSELFOTHER", "SPELLMISSOTHERSELF", "SPELLMISSOTHEROTHER",
                                 "SPELLMISSSELFSELF"};
    static constexpr Forms kDodge{"SPELLDODGEDSELFOTHER", "SPELLDODGEDOTHERSELF", "SPELLDODGEDOTHEROTHER",
                                  "SPELLDODGEDSELFSELF"};
    static constexpr Forms kParry{"SPELLPARRIEDSELFOTHER", "SPELLPARRIEDOTHERSELF", "SPELLPARRIEDOTHEROTHER",
                                  "SPELLPARRIEDSELFSELF"};
    static constexpr Forms kBlock{"SPELLBLOCKEDSELFOTHER", "SPELLBLOCKEDOTHERSELF", "SPELLBLOCKEDOTHEROTHER"};
    static constexpr Forms kEvade{"SPELLEVADEDSELFOTHER", "SPELLEVADEDOTHERSELF", "SPELLEVADEDOTHEROTHER",
                                  "SPELLEVADEDSELFSELF"};
    static constexpr Forms kImmune{"SPELLIMMUNESELFOTHER", "SPELLIMMUNEOTHERSELF", "SPELLIMMUNEOTHEROTHER",
                                   "SPELLIMMUNESELFSELF"};
    static constexpr Forms kDeflect{"SPELLDEFLECTEDSELFOTHER", "SPELLDEFLECTEDOTHERSELF",
                                    "SPELLDEFLECTEDOTHEROTHER", "SPELLDEFLECTEDSELFSELF"};
    static constexpr Forms kResist{"SPELLRESISTSELFOTHER", "SPELLRESISTOTHERSELF", "SPELLRESISTOTHEROTHER",
                                   "SPELLRESISTSELFSELF"};
    static constexpr Forms kAbsorb{"SPELLLOGABSORBSELFOTHER", "SPELLLOGABSORBOTHERSELF",
                                   "SPELLLOGABSORBOTHEROTHER", "SPELLLOGABSORBSELFSELF"};
    static constexpr Forms kReflect{"SPELLREFLECTSELFOTHER", "SPELLREFLECTOTHERSELF",
                                    "SPELLREFLECTOTHEROTHER", "SPELLREFLECTSELFSELF"};
    switch (kind) {
    case K::Dodge: return kDodge;
    case K::Parry: return kParry;
    case K::Block: return kBlock;
    case K::Evade: return kEvade;
    case K::Immune: return kImmune;
    case K::Deflect: return kDeflect;
    case K::Resist: return kResist;
    case K::Absorb: return kAbsorb;
    case K::Reflect: return kReflect;
    default: return kMiss;
    }
}

// A swing that did not land (MISSED*, VSDODGE*...): SELFOTHER (victim), OTHERSELF (attacker), OTHEROTHER
// (attacker, victim).
const Forms& swingAvoided(CombatEvent::Kind kind) {
    using K = CombatEvent::Kind;
    static constexpr Forms kMiss{"MISSEDSELFOTHER", "MISSEDOTHERSELF", "MISSEDOTHEROTHER"};
    static constexpr Forms kDodge{"VSDODGESELFOTHER", "VSDODGEOTHERSELF", "VSDODGEOTHEROTHER"};
    static constexpr Forms kParry{"VSPARRYSELFOTHER", "VSPARRYOTHERSELF", "VSPARRYOTHEROTHER"};
    static constexpr Forms kBlock{"VSBLOCKSELFOTHER", "VSBLOCKOTHERSELF", "VSBLOCKOTHEROTHER"};
    static constexpr Forms kEvade{"VSEVADESELFOTHER", "VSEVADEOTHERSELF", "VSEVADEOTHEROTHER"};
    static constexpr Forms kImmune{"VSIMMUNESELFOTHER", "VSIMMUNEOTHERSELF", "VSIMMUNEOTHEROTHER"};
    static constexpr Forms kDeflect{"VSDEFLECTSELFOTHER", "VSDEFLECTOTHERSELF", "VSDEFLECTOTHEROTHER"};
    static constexpr Forms kResist{"VSRESISTSELFOTHER", "VSRESISTOTHERSELF", "VSRESISTOTHEROTHER"};
    static constexpr Forms kAbsorb{"VSABSORBSELFOTHER", "VSABSORBOTHERSELF", "VSABSORBOTHEROTHER"};
    switch (kind) {
    case K::Dodge: return kDodge;
    case K::Parry: return kParry;
    case K::Block: return kBlock;
    case K::Evade: return kEvade;
    case K::Immune: return kImmune;
    case K::Deflect: return kDeflect;
    case K::Resist: return kResist;
    case K::Absorb: return kAbsorb;
    default: return kMiss; // misses; swings are not reflected
    }
}

// What a hit lost on the way, after its sentence as the client appends it: " (glancing)", " (12 absorbed)"...
void trailers(Line& l, const CombatEvent& e) {
    if (e.flags & CombatEvent::kGlancing) {
        l.put("GLANCING_TRAILER");
    }
    if (e.flags & CombatEvent::kCrushing) {
        l.put("CRUSHING_TRAILER");
    }
    if (e.absorbed > 0) {
        l.put("ABSORB_TRAILER", e.absorbed);
    }
    if (e.resisted > 0) {
        l.put("RESIST_TRAILER", e.resisted);
    }
    if (e.blocked > 0) {
        l.put("BLOCK_TRAILER", e.blocked);
    }
}

// Phase 13 (combat): the lines of WoW.exe's UnitCombatLog_C for the packets the client did not read before (the
// keys and their arguments as its functions push them; a form the client has no text for writes no line).

// Damage shields: SELFOTHER (amount, school, victim), OTHERSELF (bearer, amount, school), OTHEROTHER (bearer,
// amount, school, victim).
constexpr Forms kDamageShield{"DAMAGESHIELDSELFOTHER", "DAMAGESHIELDOTHERSELF", "DAMAGESHIELDOTHEROTHER"};
// Leeches: SELFOTHER (spell, amount, power, victim, gain, power), OTHERSELF (caster, spell, amount, power, caster,
// gain, power), OTHEROTHER (caster, spell, amount, power, victim, caster, gain, power).
constexpr Forms kPowerLeech{"SPELLPOWERLEECHSELFOTHER", "SPELLPOWERLEECHOTHERSELF", "SPELLPOWERLEECHOTHEROTHER"};
// The generic line of an effect on a unit: SELFOTHER (spell, victim), OTHERSELF (caster, spell), OTHEROTHER
// (caster, spell, victim), SELFSELF (spell); [perform].
constexpr Forms kSimpleCast[2] = {
    {"SIMPLECASTSELFOTHER", "SIMPLECASTOTHERSELF", "SIMPLECASTOTHEROTHER", "SIMPLECASTSELFSELF"},
    {"SIMPLEPERFORMSELFOTHER", "SIMPLEPERFORMOTHERSELF", "SIMPLEPERFORMOTHEROTHER", "SIMPLEPERFORMSELFSELF"},
};
// Durability damage: SELFOTHER (spell, victim, item), OTHERSELF (caster, spell, item), OTHEROTHER (caster, spell,
// victim, item); [every item] without the item.
constexpr Forms kDurability[2] = {
    {"SPELLDURABILITYDAMAGESELFOTHER", "SPELLDURABILITYDAMAGEOTHERSELF", "SPELLDURABILITYDAMAGEOTHEROTHER"},
    {"SPELLDURABILITYDAMAGEALLSELFOTHER", "SPELLDURABILITYDAMAGEALLOTHERSELF", "SPELLDURABILITYDAMAGEALLOTHEROTHER"},
};
// Failed dispels: SELFOTHER (victim, spell), OTHERSELF (caster, spell), OTHEROTHER (caster, victim, spell),
// SELFSELF (spell).
constexpr Forms kDispelFailed{"DISPELFAILEDSELFOTHER", "DISPELFAILEDOTHERSELF", "DISPELFAILEDOTHEROTHER",
                              "DISPELFAILEDSELFSELF"};
// Enchantments put on an item: SELFOTHER (enchantment, owner, item), OTHERSELF (caster, enchantment, item),
// OTHEROTHER (caster, enchantment, owner, item), SELFSELF (enchantment, item).
constexpr Forms kEnchantAdded{"ITEMENCHANTMENTADDSELFOTHER", "ITEMENCHANTMENTADDOTHERSELF",
                              "ITEMENCHANTMENTADDOTHEROTHER", "ITEMENCHANTMENTADDSELFSELF"};

// The lines about something else than a blow (CombatEvent::logOnly()).
void logOnlyLine(Line& l, const CombatEvent& e, const CombatNames& names, Who w, CombatLineKind& k) {
    using K = CombatEvent::Kind;
    const bool mine = e.attacker != 0 && e.attacker == names.player;
    const bool onMe = e.victim == names.player;
    const std::string_view spell = names.spell(e.spellId);
    switch (e.kind) {
    case K::ExtraAttacks: // ..._SINGULAR for one
        if (onMe) {
            l.put(e.amount == 1 ? "SPELLEXTRAATTACKSSELF_SINGULAR" : "SPELLEXTRAATTACKSSELF", e.amount, spell);
        } else {
            l.put(e.amount == 1 ? "SPELLEXTRAATTACKSOTHER_SINGULAR" : "SPELLEXTRAATTACKSOTHER", names.unit(e.victim),
                  e.amount, spell);
        }
        break;
    case K::CreateItem:
        if (mine) {
            l.put("TRADESKILL_LOG_FIRSTPERSON", names.item(e.item));
        } else {
            l.put("TRADESKILL_LOG_THIRDPERSON", names.unit(e.attacker), names.item(e.item));
        }
        break;
    case K::FeedPet:
        if (mine) {
            l.put("FEEDPET_LOG_FIRSTPERSON", names.item(e.item));
        } else {
            l.put("FEEDPET_LOG_THIRDPERSON", names.unit(e.attacker), names.item(e.item));
        }
        break;
    case K::OpenLock:
        if (mine) {
            l.put("OPEN_LOCK_SELF", spell, names.unit(e.victim));
        } else {
            l.put("OPEN_LOCK_OTHER", names.unit(e.attacker), spell, names.unit(e.victim));
        }
        break;
    case K::DurabilityDamage: {
        // The ALL forms have no %s for the item: the extra argument is ignored as printf ignores it.
        const Forms& f = kDurability[e.item == 0 ? 1 : 0];
        const std::string_view item = names.item(e.item);
        switch (w) {
        case Who::SelfOther: l.put(f.selfOther, spell, names.unit(e.victim), item); break;
        case Who::OtherSelf: l.put(f.otherSelf, names.unit(e.attacker), spell, item); break;
        case Who::OtherOther: l.put(f.otherOther, names.unit(e.attacker), spell, names.unit(e.victim), item); break;
        case Who::SelfSelf: break; // no text
        }
        break;
    }
    case K::DismissPet:
        if (mine) {
            l.put("SPELLDISMISSPETSELF", names.unit(e.victim));
        } else {
            l.put("SPELLDISMISSPETOTHER", names.unit(e.attacker), names.unit(e.victim));
        }
        break;
    case K::SpellCast: {
        const bool perform = (e.flags & CombatEvent::kPerform) != 0;
        if (e.victim == 0) { // SPELLTERSE*: no target
            if (mine) {
                l.put(perform ? "SPELLTERSEPERFORM_SELF" : "SPELLTERSE_SELF", spell);
            } else {
                l.put(perform ? "SPELLTERSEPERFORM_OTHER" : "SPELLTERSE_OTHER", names.unit(e.attacker), spell);
            }
            break;
        }
        const Forms& f = kSimpleCast[perform ? 1 : 0];
        switch (w) {
        case Who::SelfOther: l.put(f.selfOther, spell, names.unit(e.victim)); break;
        case Who::OtherSelf: l.put(f.otherSelf, names.unit(e.attacker), spell); break;
        case Who::OtherOther: l.put(f.otherOther, names.unit(e.attacker), spell, names.unit(e.victim)); break;
        case Who::SelfSelf: l.put(f.selfSelf, spell); break;
        }
        break;
    }
    case K::Dispelled:
        k = CombatLineKind::AuraFade;
        if (onMe) {
            l.put("AURADISPELSELF", spell);
        } else {
            l.put("AURADISPELOTHER", names.unit(e.victim), spell);
        }
        break;
    case K::DispelFailed:
        k = CombatLineKind::Miss;
        switch (w) {
        case Who::SelfOther: l.put(kDispelFailed.selfOther, names.unit(e.victim), spell); break;
        case Who::OtherSelf: l.put(kDispelFailed.otherSelf, names.unit(e.attacker), spell); break;
        case Who::OtherOther:
            l.put(kDispelFailed.otherOther, names.unit(e.attacker), names.unit(e.victim), spell);
            break;
        case Who::SelfSelf: l.put(kDispelFailed.selfSelf, spell); break;
        }
        break;
    case K::InstaKill:
        k = CombatLineKind::Death;
        if (onMe) {
            l.put("INSTAKILLSELF", spell);
        } else {
            l.put("INSTAKILLOTHER", names.unit(e.victim), spell);
        }
        break;
    case K::EnchantAdded: {
        k = CombatLineKind::AuraGain;
        const std::string_view enchant = names.enchant(static_cast<u32>(e.amount));
        const std::string_view item = names.item(e.item);
        switch (w) {
        case Who::SelfOther: l.put(kEnchantAdded.selfOther, enchant, names.unit(e.victim), item); break;
        case Who::OtherSelf: l.put(kEnchantAdded.otherSelf, names.unit(e.attacker), enchant, item); break;
        case Who::OtherOther:
            l.put(kEnchantAdded.otherOther, names.unit(e.attacker), enchant, names.unit(e.victim), item);
            break;
        case Who::SelfSelf: l.put(kEnchantAdded.selfSelf, enchant, item); break;
        }
        break;
    }
    case K::EnchantFaded:
        k = CombatLineKind::AuraFade;
        if (onMe) {
            l.put("ITEMENCHANTMENTREMOVESELF", names.enchant(static_cast<u32>(e.amount)), names.item(e.item));
        } else {
            l.put("ITEMENCHANTMENTREMOVEOTHER", names.enchant(static_cast<u32>(e.amount)), names.unit(e.victim),
                  names.item(e.item));
        }
        break;
    default: break;
    }
}

} // namespace

std::string_view CombatNames::unit(u64 guid) const {
    const std::string_view name = guid && unitName ? unitName(context, guid) : std::string_view{};
    return name.empty() ? text::get("UNKNOWNOBJECT") : name;
}

std::string_view CombatNames::spell(u32 spellId) const {
    const std::string_view name = spellId && spellName ? spellName(context, spellId) : std::string_view{};
    return name.empty() ? text::get("UNKNOWN") : name;
}

std::string_view CombatNames::item(u32 entry) const {
    const std::string_view name = entry && itemName ? itemName(context, entry) : std::string_view{};
    return name.empty() ? text::get("UNKNOWN") : name;
}

std::string_view CombatNames::enchant(u32 enchantId) const {
    const std::string_view name = enchantId && enchantName ? enchantName(context, enchantId) : std::string_view{};
    return name.empty() ? text::get("UNKNOWN") : name;
}

std::string_view schoolName(u32 school) {
    static constexpr text::Key kSchools[] = {"SPELL_SCHOOL0_CAP", "SPELL_SCHOOL1_CAP", "SPELL_SCHOOL2_CAP",
                                             "SPELL_SCHOOL3_CAP", "SPELL_SCHOOL4_CAP", "SPELL_SCHOOL5_CAP",
                                             "SPELL_SCHOOL6_CAP"};
    return text::get(school < std::size(kSchools) ? kSchools[school] : kSchools[0]);
}

std::string_view powerName(u32 powerType) {
    static constexpr text::Key kPowers[] = {"MANA", "RAGE", "FOCUS", "ENERGY", "HAPPINESS"};
    if (powerType == 0xFEu || powerType == 0xFFFFFFFEu) { // POWER_HEALTH (-2)
        return text::get("HEALTH");
    }
    return text::get(powerType < std::size(kPowers) ? kPowers[powerType] : kPowers[0]);
}

usize formatCombatEvent(char* out, usize size, const CombatEvent& e, const CombatNames& names,
                        CombatLineKind* kind) {
    if (size == 0) {
        return 0;
    }
    Line l{out, size};
    out[0] = 0;
    using K = CombatEvent::Kind;
    const Who w = who(e, names.player);
    const bool mine = w == Who::SelfOther || w == Who::SelfSelf;
    const bool onMe = e.victim == names.player;
    const bool crit = (e.flags & CombatEvent::kCrit) != 0;
    const bool periodic = (e.flags & CombatEvent::kPeriodic) != 0;
    const bool environment = (e.flags & CombatEvent::kEnvironment) != 0;
    const bool swing = e.spellId == 0 && !environment;
    const std::string_view attacker = names.unit(e.attacker);
    const std::string_view victim = names.unit(e.victim);
    const std::string_view spell = names.spell(e.spellId);
    CombatLineKind k = mine || (names.pet && e.attacker == names.pet) ? CombatLineKind::Outgoing
                       : onMe                                         ? CombatLineKind::Incoming
                                                                      : CombatLineKind::Other;

    switch (e.kind) {
    case K::Interrupt:
        if (!swing) {
            k = CombatLineKind::Miss;
            switch (resolve(w, kInterrupt)) {
            case Who::SelfOther: l.put(kInterrupt.selfOther, victim, spell); break;
            case Who::OtherSelf: l.put(kInterrupt.otherSelf, attacker, spell); break;
            default: l.put(kInterrupt.otherOther, attacker, victim, spell); break;
            }
            break;
        }
        [[fallthrough]]; // a swing's "interrupt" victim state (the server never sends it): a hit
    case K::Damage: {
        const std::string_view school = schoolName(e.school);
        if (e.flags & CombatEvent::kDamageShield) { // phase 13: "You reflect 3 Nature damage to Young Wolf."
            switch (w) {
            case Who::SelfOther: l.put(kDamageShield.selfOther, e.amount, school, victim); break;
            case Who::OtherSelf: l.put(kDamageShield.otherSelf, attacker, e.amount, school); break;
            case Who::OtherOther: l.put(kDamageShield.otherOther, attacker, e.amount, school, victim); break;
            case Who::SelfSelf: break; // no text
            }
        } else if (environment) {
            const u32 type = e.spellId < std::size(kEnvironmentSelf) ? e.spellId : 2; // unknown: a fall
            if (onMe) {
                l.put(kEnvironmentSelf[type], e.amount);
            } else {
                l.put(kEnvironmentOther[type], victim, e.amount);
            }
        } else if (periodic) {
            switch (w) {
            case Who::SelfOther: l.put(kPeriodicDamage.selfOther, victim, e.amount, school, spell); break;
            case Who::OtherSelf: l.put(kPeriodicDamage.otherSelf, e.amount, school, attacker, spell); break;
            case Who::OtherOther:
                l.put(kPeriodicDamage.otherOther, victim, e.amount, school, attacker, spell);
                break;
            case Who::SelfSelf: l.put(kPeriodicDamage.selfSelf, e.amount, school, spell); break;
            }
        } else if (swing) {
            const Forms& f = kSwingHit[crit ? 1 : 0][e.school != 0 ? 1 : 0];
            switch (resolve(w, f)) {
            case Who::SelfOther: l.put(f.selfOther, victim, e.amount, school); break;
            case Who::OtherSelf: l.put(f.otherSelf, attacker, e.amount, school); break;
            default: l.put(f.otherOther, attacker, victim, e.amount, school); break;
            }
        } else {
            const Forms& f = kSpellHit[crit ? 1 : 0][e.school != 0 ? 1 : 0];
            switch (w) {
            case Who::SelfOther: l.put(f.selfOther, spell, victim, e.amount, school); break;
            case Who::OtherSelf: l.put(f.otherSelf, attacker, spell, e.amount, school); break;
            case Who::OtherOther: l.put(f.otherOther, attacker, spell, victim, e.amount, school); break;
            case Who::SelfSelf: l.put(f.selfSelf, spell, e.amount, school); break;
            }
        }
        trailers(l, e);
        break;
    }
    case K::Miss:
    case K::Dodge:
    case K::Parry:
    case K::Block:
    case K::Evade:
    case K::Immune:
    case K::Deflect:
    case K::Resist:
    case K::Absorb:
    case K::Reflect:
        k = CombatLineKind::Miss;
        if (swing) {
            const Forms& f = swingAvoided(e.kind);
            switch (resolve(w, f)) {
            case Who::SelfOther: l.put(f.selfOther, victim); break;
            case Who::OtherSelf: l.put(f.otherSelf, attacker); break;
            default: l.put(f.otherOther, attacker, victim); break;
            }
        } else {
            const Forms& f = spellAvoided(e.kind);
            switch (resolve(w, f)) {
            case Who::SelfOther: l.put(f.selfOther, spell, victim); break;
            case Who::OtherSelf: l.put(f.otherSelf, attacker, spell); break;
            case Who::OtherOther: l.put(f.otherOther, attacker, spell, victim); break;
            case Who::SelfSelf: l.put(f.selfSelf, spell); break;
            }
        }
        break;
    case K::Heal:
        k = CombatLineKind::Heal;
        if (periodic) {
            switch (w) {
            case Who::SelfOther: l.put(kPeriodicHeal.selfOther, victim, e.amount, spell); break;
            case Who::OtherSelf: l.put(kPeriodicHeal.otherSelf, e.amount, attacker, spell); break;
            case Who::OtherOther: l.put(kPeriodicHeal.otherOther, victim, e.amount, attacker, spell); break;
            case Who::SelfSelf: l.put(kPeriodicHeal.selfSelf, e.amount, spell); break;
            }
        } else {
            const Forms& f = kHeal[crit ? 1 : 0];
            switch (w) {
            case Who::SelfOther: l.put(f.selfOther, spell, victim, e.amount); break;
            case Who::OtherSelf: l.put(f.otherSelf, attacker, spell, e.amount); break;
            case Who::OtherOther: l.put(f.otherOther, attacker, spell, victim, e.amount); break;
            case Who::SelfSelf: l.put(f.selfSelf, spell, e.amount); break;
            }
        }
        break;
    case K::Energize: {
        k = CombatLineKind::Heal;
        const std::string_view power = powerName(e.school);
        switch (w) {
        case Who::SelfOther: l.put(kPowerGain.selfOther, victim, e.amount, power, spell); break;
        case Who::OtherSelf: l.put(kPowerGain.otherSelf, e.amount, power, attacker, spell); break;
        case Who::OtherOther: l.put(kPowerGain.otherOther, victim, e.amount, power, attacker, spell); break;
        case Who::SelfSelf: l.put(kPowerGain.selfSelf, e.amount, power, spell); break;
        }
        break;
    }
    case K::Drain: {
        const std::string_view power = powerName(e.school);
        if (e.gain > 0) { // phase 13: a leech (SMSG_SPELLLOGEXECUTE's power drain with a multiplier)
            switch (w) {
            case Who::SelfOther: l.put(kPowerLeech.selfOther, spell, e.amount, power, victim, e.gain, power); break;
            case Who::OtherSelf:
                l.put(kPowerLeech.otherSelf, attacker, spell, e.amount, power, attacker, e.gain, power);
                break;
            case Who::OtherOther:
                l.put(kPowerLeech.otherOther, attacker, spell, e.amount, power, victim, attacker, e.gain, power);
                break;
            case Who::SelfSelf: break; // no text
            }
            break;
        }
        switch (w) {
        case Who::SelfOther: l.put(kPowerDrain.selfOther, spell, e.amount, power, victim); break;
        case Who::OtherSelf: l.put(kPowerDrain.otherSelf, attacker, spell, e.amount, power); break;
        case Who::OtherOther: l.put(kPowerDrain.otherOther, attacker, spell, e.amount, power, victim); break;
        case Who::SelfSelf: l.put(kPowerDrain.selfSelf, spell, e.amount, power); break;
        }
        break;
    }
    case K::ExtraAttacks:
    case K::CreateItem:
    case K::FeedPet:
    case K::OpenLock:
    case K::DurabilityDamage:
    case K::DismissPet:
    case K::SpellCast:
    case K::Dispelled:
    case K::DispelFailed:
    case K::InstaKill:
    case K::EnchantAdded:
    case K::EnchantFaded: logOnlyLine(l, e, names, w, k); break; // phase 13
    }
    if (kind) {
        *kind = k;
    }
    return l.used;
}

usize formatAuraChange(char* out, usize size, u64 unit, u32 spellId, bool gained, bool harmful,
                       const CombatNames& names, CombatLineKind* kind) {
    if (size == 0) {
        return 0;
    }
    Line l{out, size};
    out[0] = 0;
    const bool me = unit == names.player;
    const std::string_view spell = names.spell(spellId);
    if (gained) {
        if (me) {
            l.put(harmful ? "AURAADDEDSELFHARMFUL" : "AURAADDEDSELFHELPFUL", spell);
        } else {
            l.put(harmful ? "AURAADDEDOTHERHARMFUL" : "AURAADDEDOTHERHELPFUL", names.unit(unit), spell);
        }
    } else if (me) {
        l.put("AURAREMOVEDSELF", spell);
    } else {
        l.put("AURAREMOVEDOTHER", spell, names.unit(unit));
    }
    if (kind) {
        *kind = gained ? CombatLineKind::AuraGain : CombatLineKind::AuraFade;
    }
    return l.used;
}

usize formatKill(char* out, usize size, u64 killer, u64 victim, const CombatNames& names) {
    if (size == 0) {
        return 0;
    }
    Line l{out, size};
    out[0] = 0;
    if (victim == names.player) {
        if (killer != 0 && killer != victim) {
            l.put("ERR_KILLED_BY_S", names.unit(killer));
        } else {
            l.put("UNITDIESSELF");
        }
    } else if (killer == names.player) {
        l.put("SELFKILLOTHER", names.unit(victim));
    } else {
        l.put("PARTYKILLOTHER", names.unit(victim), names.unit(killer));
    }
    return l.used;
}

usize formatExperience(char* out, usize size, const ExperienceGain& xp, const CombatNames& names) {
    if (size == 0) {
        return 0;
    }
    Line l{out, size};
    out[0] = 0;
    if (xp.loss) { // 0x637F70
        l.put("COMBATLOG_XPLOSS_FIRSTPERSON_UNNAMED", -xp.total);
        return l.used;
    }
    // The key of each rest state (0x864330; the third has none) and its suffix by what the group bonus
    // changed: `alone` is what the kill gives without it (truncated, as WoW.exe's conversion).
    static constexpr const char* kRestKeys[] = {
        nullptr, "COMBATLOG_XPGAIN_EXHAUSTION1", "COMBATLOG_XPGAIN_EXHAUSTION2",
        nullptr, "COMBATLOG_XPGAIN_EXHAUSTION4", "COMBATLOG_XPGAIN_EXHAUSTION5"};
    constexpr f32 kEpsilon = 2.3841858e-7f; // 0x8029D4
    const f32 groupBonus = xp.groupBonus != 0.0f ? xp.groupBonus : 1.0f;
    const auto suffix = [](i32 alone, i32 gained) {
        return alone == gained ? "" : alone < gained ? "_GROUP" : "_RAID";
    };
    const auto diff = [](i32 alone, i32 gained) { return gained > alone ? gained - alone : alone - gained; };
    char key[64];
    if (xp.victim != 0 && xp.restState < std::size(kRestKeys) && kRestKeys[xp.restState] &&
        std::fabs(xp.restFactor) > kEpsilon && std::fabs(xp.restFactor - 1.0f) > kEpsilon) {
        // "(+45 exp Rested bonus)": the bonus with its sign and the state's name.
        const i32 alone = static_cast<i32>(static_cast<f32>(xp.base) / groupBonus);
        char bonus[16];
        const i32 rest = xp.total - xp.base;
        std::snprintf(bonus, sizeof(bonus), "%s%d", rest >= 0 ? "+" : "", rest);
        std::snprintf(key, sizeof(key), "%s%s", kRestKeys[xp.restState], suffix(alone, xp.base));
        if (alone == xp.base) {
            l.put(text::Key(std::string_view(key)), names.unit(xp.victim), xp.total, bonus, xp.restName);
        } else {
            l.put(text::Key(std::string_view(key)), names.unit(xp.victim), xp.total, bonus, xp.restName,
                  diff(alone, xp.base));
        }
        return l.used;
    }
    const i32 alone = static_cast<i32>(static_cast<f32>(xp.total) / groupBonus);
    std::snprintf(key, sizeof(key), "%s%s",
                  xp.victim != 0 ? "COMBATLOG_XPGAIN_FIRSTPERSON" : "COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED",
                  suffix(alone, xp.total));
    const text::Key k{std::string_view(key)};
    if (xp.victim != 0 && alone == xp.total) {
        l.put(k, names.unit(xp.victim), xp.total);
    } else if (xp.victim != 0) {
        l.put(k, names.unit(xp.victim), xp.total, diff(alone, xp.total));
    } else if (alone == xp.total) {
        l.put(k, xp.total);
    } else {
        l.put(k, xp.total, diff(alone, xp.total));
    }
    return l.used;
}

std::string_view formatExperienceWorldText(std::span<char> out, i32 total) {
    if (out.empty()) {
        return {};
    }
    // WoW.exe's own "%s: %d" (0x860F5C) around the XP text.
    const std::string_view xp = text::get("XP");
    const int n =
        std::snprintf(out.data(), out.size(), "%.*s: %d", static_cast<int>(xp.size()), xp.data(), total);
    return {out.data(), static_cast<usize>(std::clamp(n, 0, static_cast<int>(out.size()) - 1))};
}

AuraWatch::Unit* AuraWatch::slotFor(u64 guid) {
    Unit* best = &m_units[0];
    for (Unit& u : m_units) {
        if (u.guid == 0) {
            best = &u;
            break;
        }
        if (u.lastSeen < best->lastSeen) {
            best = &u;
        }
    }
    *best = Unit{};
    best->guid = guid;
    return best;
}

} // namespace vd::game
