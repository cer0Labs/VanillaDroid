#include "game/Combat.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace vd::game {
namespace {

CombatEvent::Kind missKind(net::SpellMiss miss) {
    using K = CombatEvent::Kind;
    switch (miss) {
    case net::SpellMiss::Resist: return K::Resist;
    case net::SpellMiss::Dodge: return K::Dodge;
    case net::SpellMiss::Parry: return K::Parry;
    case net::SpellMiss::Block: return K::Block;
    case net::SpellMiss::Evade: return K::Evade;
    case net::SpellMiss::Immune:
    case net::SpellMiss::Immune2: return K::Immune;
    case net::SpellMiss::Deflect: return K::Deflect;
    case net::SpellMiss::Absorb: return K::Absorb;
    case net::SpellMiss::Reflect: return K::Reflect;
    default: return K::Miss;
    }
}

u32 positive(i32 value) {
    return value > 0 ? static_cast<u32>(value) : 0;
}

} // namespace

CombatEvent& CombatLog::push(u32 nowMs, u64 attacker, u64 victim, u32 spellId) {
    CombatEvent& e = m_events.push();
    e = CombatEvent{};
    e.timeMs = nowMs;
    e.attacker = attacker;
    e.victim = victim;
    e.spellId = spellId;
    return e;
}

void CombatLog::pushMiss(u32 nowMs, u64 attacker, u64 victim, u32 spellId, net::SpellMiss miss) {
    push(nowMs, attacker, victim, spellId).kind = missKind(miss);
}

void CombatLog::onAttackerStateUpdate(const net::AttackerStateUpdate& m, u32 nowMs) {
    CombatEvent& e = push(nowMs, m.attacker, m.victim, m.meleeSpellId);
    using K = CombatEvent::Kind;
    for (u32 i = 0; i < m.subDamageCount; ++i) {
        e.absorbed += positive(m.subDamages[i].absorbed);
        e.resisted += positive(m.subDamages[i].resisted);
    }
    e.school = m.subDamageCount > 0 ? static_cast<u8>(m.subDamages[0].school) : 0;
    e.amount = m.totalDamage;
    e.blocked = positive(m.blocked);
    e.hitInfo = m.hitInfo; // phase 14 (bodyfx): the blood spurts
    e.melee = true;        // phase 15 (pets)
    e.flags |= (m.hitInfo & net::kHitInfoCritical) ? CombatEvent::kCrit : 0;
    e.flags |= (m.hitInfo & net::kHitInfoGlancing) ? CombatEvent::kGlancing : 0;
    e.flags |= (m.hitInfo & net::kHitInfoCrushing) ? CombatEvent::kCrushing : 0;
    e.flags |= (m.hitInfo & net::kHitInfoLeftSwing) ? CombatEvent::kOffHand : 0;
    if (m.hitInfo & net::kHitInfoMiss) {
        e.kind = K::Miss;
        return;
    }
    switch (m.victimState) {
    case net::VictimState::Unaffected: e.kind = K::Miss; break;
    case net::VictimState::Dodge: e.kind = K::Dodge; break;
    case net::VictimState::Parry: e.kind = K::Parry; break;
    case net::VictimState::Interrupt: e.kind = K::Interrupt; break;
    case net::VictimState::Blocks: e.kind = K::Block; break;
    case net::VictimState::Evades: e.kind = K::Evade; break;
    case net::VictimState::Immune: e.kind = K::Immune; break;
    case net::VictimState::Deflects: e.kind = K::Deflect; break;
    default:
        if (m.totalDamage <= 0 && e.absorbed > 0) {
            e.kind = K::Absorb;
        } else if (m.totalDamage <= 0 && e.resisted > 0) {
            e.kind = K::Resist;
        } else {
            e.kind = K::Damage;
        }
        break;
    }
}

void CombatLog::onSpellDamage(const net::SpellDamageLog& m, u32 nowMs) {
    CombatEvent& e = push(nowMs, m.attacker, m.target, m.spellId);
    e.school = m.school;
    e.amount = static_cast<i32>(m.damage);
    e.absorbed = m.absorbed;
    e.resisted = positive(m.resisted);
    e.blocked = m.blocked;
    e.flags |= (m.hitType & net::kSpellHitCrit) ? CombatEvent::kCrit : 0;
    e.flags |= m.periodic ? CombatEvent::kPeriodic : 0;
    if (m.damage == 0 && m.absorbed > 0) {
        e.kind = CombatEvent::Kind::Absorb;
    } else if (m.damage == 0 && m.resisted > 0) {
        e.kind = CombatEvent::Kind::Resist;
    }
}

void CombatLog::onSpellHeal(const net::SpellHealLog& m, u32 nowMs) {
    CombatEvent& e = push(nowMs, m.healer, m.target, m.spellId);
    e.kind = CombatEvent::Kind::Heal;
    e.amount = static_cast<i32>(m.amount);
    e.flags = m.critical ? CombatEvent::kCrit : 0;
}

void CombatLog::onSpellEnergize(const net::SpellEnergizeLog& m, u32 nowMs) {
    CombatEvent& e = push(nowMs, m.caster, m.target, m.spellId);
    e.kind = CombatEvent::Kind::Energize;
    e.amount = static_cast<i32>(m.amount);
    e.school = static_cast<u8>(m.powerType);
}

void CombatLog::onPeriodicAura(const net::PeriodicAuraLog& m, u32 nowMs) {
    CombatEvent& e = push(nowMs, m.caster, m.target, m.spellId);
    e.flags = CombatEvent::kPeriodic;
    e.amount = static_cast<i32>(m.amount);
    switch (m.auraType) {
    case net::kAuraPeriodicDamage:
    case net::kAuraPeriodicDamagePercent:
        e.kind = CombatEvent::Kind::Damage;
        e.school = static_cast<u8>(m.school);
        e.absorbed = m.absorbed;
        e.resisted = positive(m.resisted);
        if (m.amount == 0 && m.absorbed > 0) {
            e.kind = CombatEvent::Kind::Absorb;
        } else if (m.amount == 0 && m.resisted > 0) {
            e.kind = CombatEvent::Kind::Resist;
        }
        break;
    case net::kAuraPeriodicHeal:
    case net::kAuraObsModHealth: e.kind = CombatEvent::Kind::Heal; break;
    case net::kAuraPeriodicManaLeech:
        e.kind = CombatEvent::Kind::Drain;
        e.school = static_cast<u8>(m.powerType);
        break;
    default:
        e.kind = CombatEvent::Kind::Energize;
        e.school = static_cast<u8>(m.powerType);
        break;
    }
}

void CombatLog::onSpellLogMiss(const net::SpellLogMiss& m, u32 nowMs) {
    for (u32 i = 0; i < m.count; ++i) {
        pushMiss(nowMs, m.caster, m.entries[i].target, m.spellId, m.entries[i].miss);
    }
}

void CombatLog::onSpellGoMisses(const net::SpellGo& m, u32 nowMs) {
    for (const net::SpellGo::Miss& miss : m.misses) {
        if (miss.miss != net::SpellMiss::None) {
            pushMiss(nowMs, m.unitCaster ? m.unitCaster : m.caster, miss.target, m.spellId, miss.miss);
        }
    }
}

void CombatLog::onImmune(const net::SpellImmuneLog& m, u32 nowMs) {
    CombatEvent& e = push(nowMs, m.caster, m.target, m.spellId);
    e.kind = CombatEvent::Kind::Immune;
    e.noFeedback = true; // phase 15 (pets): its handler (WoW.exe 0x5E8CA0) fires no UNIT_COMBAT
}

void CombatLog::onProcResist(const net::SpellImmuneLog& m, u32 nowMs) {
    CombatEvent& e = push(nowMs, m.caster, m.target, m.spellId);
    e.kind = CombatEvent::Kind::Resist;
    e.noFeedback = true; // phase 15 (pets)
}

void CombatLog::onEnvironmental(const net::EnvironmentalDamageLog& m, u32 nowMs) {
    CombatEvent& e = push(nowMs, 0, m.victim, m.type);
    e.flags = CombatEvent::kEnvironment;
    e.amount = static_cast<i32>(m.damage);
    e.absorbed = m.absorbed;
    e.resisted = positive(m.resisted);
}

// Phase 13 (combat) -------------------------------------------------------------------------------------------

namespace {

// What a power's amount reads as in the log: rage is kept in tenths (WoW.exe's table at 0x86F978: mana 1, rage 10,
// focus 1, energy 1, happiness 1000).
u32 powerDivisor(u32 powerType) {
    static constexpr u32 kDivisors[] = {1, 10, 1, 1, 1000};
    return powerType < std::size(kDivisors) ? kDivisors[powerType] : 1;
}

} // namespace

void CombatLog::onSpellLogExecute(const net::SpellLogExecute& m, const ExecuteLogSpells& spells, u32 nowMs) {
    using K = CombatEvent::Kind;
    const ExecuteLogSpell spell = spells(m.spellId);
    // One line per effect target, as UnitCombatLog_C's SMSG_SPELLLOGEXECUTE handler (0x5E7F90) picks them by effect;
    // the effects without a line of their own get the generic "You cast X on Y." / "You cast X.".
    const auto generic = [&](u64 target) {
        if (!spell.quiet) {
            CombatEvent& e = push(nowMs, m.caster, target, m.spellId);
            e.kind = K::SpellCast;
            e.flags = spell.perform ? CombatEvent::kPerform : 0;
        }
    };
    for (u32 i = 0; i < m.effectCount; ++i) {
        const net::SpellLogExecute::Effect& effect = m.effects[i];
        for (u32 j = effect.first; j < effect.first + effect.count && j < m.entries.size(); ++j) {
            const net::SpellLogExecute::Entry& entry = m.entries[j];
            switch (effect.effect) {
            case net::kEffectPowerDrain: {
                if (spell.hidden) {
                    break;
                }
                CombatEvent& e = push(nowMs, m.caster, entry.target, m.spellId);
                e.kind = K::Drain;
                e.school = static_cast<u8>(entry.value2);
                const u32 divisor = powerDivisor(entry.value2);
                e.amount = static_cast<i32>(entry.value / divisor);
                if (std::abs(entry.multiplier) > 1e-5f) { // a leech: the caster gains amount x multiplier
                    const long gained = std::lround(static_cast<f64>(entry.value) * entry.multiplier);
                    e.gain = std::max(static_cast<u32>(std::max(0l, gained)) / divisor, 1u);
                }
                break;
            }
            case net::kEffectAddExtraAttacks: {
                if (spell.hidden) {
                    break;
                }
                CombatEvent& e = push(nowMs, m.caster, entry.target, m.spellId);
                e.kind = K::ExtraAttacks;
                e.amount = static_cast<i32>(entry.value);
                break;
            }
            case net::kEffectCreateItem:
            case net::kEffectFeedPet: {
                CombatEvent& e = push(nowMs, m.caster, 0, m.spellId);
                e.kind = effect.effect == net::kEffectCreateItem ? K::CreateItem : K::FeedPet;
                e.item = entry.value;
                break;
            }
            case net::kEffectInstaKill:
                if (entry.target != m.caster) { // killing itself: no line
                    generic(entry.target);
                }
                break;
            case net::kEffectInterruptCast: // the spell interrupted, as SPELLINTERRUPT* names it
                if (!spells(entry.value).hidden) {
                    push(nowMs, m.caster, entry.target, entry.value).kind = K::Interrupt;
                }
                break;
            case net::kEffectOpenLock:
            case net::kEffectOpenLockItem:
                if (!spell.hidden) {
                    push(nowMs, m.caster, entry.target, m.spellId).kind = K::OpenLock;
                }
                break;
            case net::kEffectDurabilityDamage: {
                if (spell.hidden) {
                    break;
                }
                CombatEvent& e = push(nowMs, m.caster, entry.target, m.spellId);
                e.kind = K::DurabilityDamage;
                e.item = entry.value == 0xFFFFFFFFu && entry.value2 == 0xFFFFFFFFu ? 0 : entry.value;
                break;
            }
            case net::kEffectDismissPet: push(nowMs, m.caster, entry.target, m.spellId).kind = K::DismissPet; break;
            case net::kEffectResurrect:
            case net::kEffectDispel:
            case net::kEffectThreat:
            case net::kEffectDistract:
            case net::kEffectSanctuary:
            case net::kEffectThreatAll:
            case net::kEffectDispelMechanic:
            case net::kEffectResurrectNew:
            case net::kEffectAttackMe:
            case net::kEffectSkinPlayerCorpse:
            case net::kEffectModifyThreatPercent:
            case net::kEffect126: generic(entry.target); break;
            default: generic(0); break; // summons, totems...: the client reads no target for them
            }
        }
    }
}

void CombatLog::onDamageShield(const net::SpellDamageShield& m, u32 nowMs) {
    // DAMAGESHIELD*: the bearer "reflects" the damage to whoever struck it.
    CombatEvent& e = push(nowMs, m.bearer, m.attacker, 0);
    e.kind = CombatEvent::Kind::Damage;
    e.flags = CombatEvent::kDamageShield;
    e.school = static_cast<u8>(m.school);
    e.amount = static_cast<i32>(m.damage);
}

void CombatLog::onDispelLog(const net::SpellDispelLog& m, u32 nowMs) {
    for (u32 i = 0; i < m.count; ++i) {
        push(nowMs, m.caster, m.victim, m.spells[i]).kind = CombatEvent::Kind::Dispelled;
    }
}

void CombatLog::onDispelFailed(const net::DispelFailed& m, u32 nowMs) {
    for (u32 i = 0; i < m.count; ++i) {
        push(nowMs, m.caster, m.victim, m.spells[i]).kind = CombatEvent::Kind::DispelFailed;
    }
}

void CombatLog::onInstaKill(const net::SpellInstaKillLog& m, u32 nowMs) {
    push(nowMs, 0, m.victim, m.spellId).kind = CombatEvent::Kind::InstaKill;
}

void CombatLog::onEnchantmentLog(const net::EnchantmentLog& m, u32 nowMs) {
    CombatEvent& e = push(nowMs, m.caster, m.owner, 0);
    e.kind = m.caster != 0 ? CombatEvent::Kind::EnchantAdded : CombatEvent::Kind::EnchantFaded;
    e.item = m.itemEntry;
    e.amount = static_cast<i32>(m.enchantId);
}

void MeleeState::requested(u64 target) {
    m_attacking = true;
    m_target = target;
}

bool MeleeState::onAttackStart(const net::AttackStart& m, u64 player) {
    if (m.attacker != player) {
        return false;
    }
    const bool changed = !m_attacking || m_target != m.victim;
    m_attacking = true;
    m_target = m.victim;
    return changed;
}

bool MeleeState::onAttackStop(const net::AttackStop& m, u64 player) {
    if (m.attacker != player || !m_attacking || (m.victim != 0 && m.victim != m_target)) {
        return false; // not ours, or about a victim it already switched away from
    }
    stop();
    return true;
}

void MeleeState::stop() {
    m_attacking = false;
    m_target = 0;
}

} // namespace vd::game
