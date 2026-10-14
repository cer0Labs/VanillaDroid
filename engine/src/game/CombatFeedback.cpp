#include "game/CombatFeedback.h"

#include <iterator>

namespace vd::game {
namespace {

constexpr u32 kHitCritical = 0x80;
constexpr u32 kHitGlancing = 0x4000;
constexpr u32 kHitCrushing = 0x8000;
constexpr u32 kHitAbsorb = 0x20;
constexpr u32 kHitBlock = 0x800;
constexpr u32 kHitResist = 0x40;

// 0x494600's choice of the flags string.
UnitCombatFlag flagOf(u32 bits, i32 amount) {
    if (amount > 0) {
        return (bits & kHitCritical)   ? UnitCombatFlag::Critical
               : (bits & kHitGlancing) ? UnitCombatFlag::Glancing
               : (bits & kHitCrushing) ? UnitCombatFlag::Crushing
                                       : UnitCombatFlag::None;
    }
    return (bits & kHitAbsorb)   ? UnitCombatFlag::Absorb
           : (bits & kHitBlock)  ? UnitCombatFlag::Block
           : (bits & kHitResist) ? UnitCombatFlag::Resist
                                 : UnitCombatFlag::None;
}

// The environment's school (WoW.exe's table at 0x809274: exhausted, drowning and falling physical, lava fire,
// slime nature, fire fire).
u32 environmentSchool(u32 type) {
    static constexpr u32 kSchools[] = {0, 0, 0, 2, 3, 2};
    return type < std::size(kSchools) ? kSchools[type] : 0;
}

UnitCombatEvent missEvent(CombatEvent::Kind kind) {
    using K = CombatEvent::Kind;
    switch (kind) {
    case K::Miss: return UnitCombatEvent::Miss;
    case K::Resist: return UnitCombatEvent::Resist;
    case K::Dodge: return UnitCombatEvent::Dodge;
    case K::Parry: return UnitCombatEvent::Parry;
    case K::Block: return UnitCombatEvent::Block;
    case K::Evade: return UnitCombatEvent::Evade;
    case K::Immune: return UnitCombatEvent::Immune;
    case K::Deflect: return UnitCombatEvent::Deflect;
    case K::Absorb: return UnitCombatEvent::Absorb;
    case K::Reflect: return UnitCombatEvent::Reflect;
    case K::Interrupt: return UnitCombatEvent::Interrupt;
    default: return UnitCombatEvent::None;
    }
}

} // namespace

u32 unitCombats(const CombatEvent& e, const SpellLogFilter& hiddenSpell, std::span<UnitCombat> out) {
    using K = CombatEvent::Kind;
    u32 count = 0;
    const auto add = [&](u64 unit, UnitCombatEvent event, i32 amount, u32 type, u32 bits) {
        if (unit != 0 && count < out.size()) {
            out[count++] = {unit, event, flagOf(bits, amount), amount, type};
        }
    };
    if (e.noFeedback || e.logOnly() || e.kind == K::Energize || e.kind == K::Drain) {
        return 0;
    }
    if (e.melee) {
        // 0x4946D0: the victim state's name; a normal hit is WOUND whatever it did (absorbed, resisted,
        // blocked).
        UnitCombatEvent event = missEvent(e.kind);
        if (e.kind == K::Damage || e.kind == K::Absorb || e.kind == K::Resist) {
            event = UnitCombatEvent::Wound;
        }
        add(e.victim, event, e.amount, e.school, e.hitInfo);
        return count;
    }
    if (e.flags & CombatEvent::kEnvironment) {
        add(e.victim, UnitCombatEvent::Wound, e.amount, environmentSchool(e.spellId),
            (e.absorbed ? kHitAbsorb : 0u) | (e.resisted ? kHitResist : 0u));
        return count;
    }
    if (e.flags & CombatEvent::kDamageShield) {
        add(e.victim, UnitCombatEvent::Wound, e.amount, e.school, 0);
        return count;
    }
    const bool periodic = (e.flags & CombatEvent::kPeriodic) != 0;
    if (e.kind == K::Heal) {
        // SMSG_SPELLHEALLOG always (with its crit); periodic heals unless the spell is hidden from the log.
        if (!periodic || !hiddenSpell(e.spellId)) {
            add(e.victim, UnitCombatEvent::Heal, e.amount, 0,
                !periodic && (e.flags & CombatEvent::kCrit) ? kHitCritical : 0u);
        }
        return count;
    }
    if (hiddenSpell(e.spellId)) {
        return 0;
    }
    // Spell damage (a wholly absorbed or resisted one too: Absorb / Resist with the amounts), or a miss.
    const bool damage = e.kind == K::Damage || (e.kind == K::Absorb && e.absorbed > 0) ||
                        (e.kind == K::Resist && e.resisted > 0);
    if (damage) {
        u32 bits = (e.absorbed ? kHitAbsorb : 0u) | (e.resisted ? kHitResist : 0u);
        if (!periodic) {
            bits |= ((e.flags & CombatEvent::kCrit) ? kHitCritical : 0u) | (e.blocked ? kHitBlock : 0u);
        }
        add(e.victim, UnitCombatEvent::Wound, e.amount, e.school, bits);
        return count;
    }
    const UnitCombatEvent miss = missEvent(e.kind);
    if (miss == UnitCombatEvent::None || miss == UnitCombatEvent::Interrupt) {
        return 0; // an interrupt is not a miss of a spell (SPELLLOGEXECUTE: its own line, no UNIT_COMBAT)
    }
    add(e.victim, miss, 0, 0, 0);
    if (miss == UnitCombatEvent::Reflect) {
        add(e.attacker, miss, 0, 0, 0); // 0x6E7DED: a reflected spell tells its caster too
    }
    return count;
}

void CombatFeedback::onCombatEvent(const UnitCombat& c, u32 nowMs) {
    FeedbackView v;
    f32 height = kFontHeight;
    v.color = FeedbackColor::White;
    switch (c.event) {
    case UnitCombatEvent::Immune:
        height *= 0.5f;
        v.word = c.event;
        break;
    case UnitCombatEvent::Wound:
        if (c.amount != 0) {
            if (c.flag == UnitCombatFlag::Critical || c.flag == UnitCombatFlag::Crushing) {
                height *= 1.5f;
            } else if (c.flag == UnitCombatFlag::Glancing) {
                height *= 0.75f;
            }
            if (c.type > 0) {
                v.color = FeedbackColor::Yellow;
            }
            v.amount = c.amount;
        } else if (c.flag == UnitCombatFlag::Absorb) {
            height *= 0.75f;
            v.word = UnitCombatEvent::Absorb;
        } else if (c.flag == UnitCombatFlag::Block) {
            height *= 0.75f;
            v.word = UnitCombatEvent::Block;
        } else if (c.flag == UnitCombatFlag::Resist) {
            height *= 0.75f;
            v.word = UnitCombatEvent::Resist;
        } else {
            v.word = UnitCombatEvent::Miss;
        }
        break;
    case UnitCombatEvent::Block:
        height *= 0.75f;
        v.word = c.event;
        break;
    case UnitCombatEvent::Heal:
        v.amount = c.amount;
        v.color = FeedbackColor::Green;
        if (c.flag == UnitCombatFlag::Critical) {
            height *= 1.5f;
        }
        break;
    case UnitCombatEvent::None: m_shown = false; return; // CombatFeedbackText["NONE"]: no text
    default: v.word = c.event; break;
    }
    v.height = height;
    v.shown = true;
    m_view = v;
    m_startMs = nowMs;
    m_shown = true;
}

FeedbackView CombatFeedback::view(u32 nowMs) const {
    if (!m_shown) {
        return {};
    }
    FeedbackView v = m_view;
    const u32 elapsed = nowMs - m_startMs;
    if (elapsed < kFadeInMs) {
        v.alpha = static_cast<f32>(elapsed) / static_cast<f32>(kFadeInMs);
    } else if (elapsed < kFadeInMs + kHoldMs) {
        v.alpha = 1.0f;
    } else if (elapsed < kFadeInMs + kHoldMs + kFadeOutMs) {
        v.alpha = 1.0f - static_cast<f32>(elapsed - kHoldMs - kFadeInMs) / static_cast<f32>(kFadeOutMs);
    } else {
        return {};
    }
    return v;
}

} // namespace vd::game
