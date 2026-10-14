#include "app/CrowdFight.h"

#include "anim/Animator.h"
#include "app/Crowd.h"
#include "core/Clock.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/Appearance.h"
#include "game/SpellVisualData.h"
#include "game/UnitFade.h"
#include "world/Actors.h"
#include "world/Terrain.h"
#include "world/UnitBodyEffects.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace vd {
namespace {

namespace animation = anim::animation;
using game::WeaponKind;

constexpr f64 kPi = 3.14159265358979323846;
constexpr f32 kRunSpeed = 7.0f; // yd/s: runners circle their foe
constexpr f64 kRunRadius = 4.0;
constexpr u32 kDeathEveryMs = 9000; // a fighter falls about this often
constexpr u32 kDeadMs = 7000;       // and gets up after this

// Spells of 1.12 with their visuals (Spell.dbc ids).
constexpr u32 kFrostbolt = 116, kFireball = 133, kShadowBolt = 686, kLightningBolt = 403, kWrath = 5176,
              kSmite = 585;
constexpr u32 kFear = 5782, kFrostNova = 122, kHammerOfJustice = 853, kHuntersMark = 1130, kHeroicStrike = 78,
              kSinisterStrike = 1752, kShieldBash = 72, kHeal = 2054, kRenew = 139, kShield = 17,
              kAutoShot = 75, kArcaneShot = 3044, kBlizzard = 10, kRainOfFire = 5740, kDrainLife = 689,
              kMindFlay = 15407, kArcaneMissiles = 5143, kLightningShield = 324;
// Phase 14 (spellfx): beams (Chain Lightning, Chain Heal, Health Funnel, Drain Mana), a shake (Thunder
// Clap).
constexpr u32 kChainLightning = 421, kChainHeal = 1064, kHealthFunnel = 755, kDrainMana = 5138,
              kThunderClap = 6343;
constexpr u32 kBolts[] = {kFrostbolt, kFireball, kShadowBolt,    kLightningBolt,
                          kWrath,     kSmite,    kChainLightning};
// Enchantments with a glow (SpellItemEnchantment.dbc): Flametongue 1, Frostbrand 1, Windfury 1, Rockbiter 1,
// Deadly Poison.
constexpr u32 kEnchants[] = {5, 2, 283, 29, 7};
constexpr u32 kGroundChannels[] = {kBlizzard, kRainOfFire};
constexpr u32 kDrains[] = {kDrainLife, kMindFlay, kArcaneMissiles, kDrainMana, kHealthFunnel};

// Weapon displays by kind, told apart by their model files ("Sword_1H_...", "Stave_2H_...", "Bow_...").
WeaponKind kindOfModel(std::string_view model) {
    const auto starts = [&](std::string_view prefix) {
        if (model.size() < prefix.size()) {
            return false;
        }
        for (usize i = 0; i < prefix.size(); ++i) {
            const char c =
                model[i] >= 'A' && model[i] <= 'Z' ? static_cast<char>(model[i] - 'A' + 'a') : model[i];
            if (c != prefix[i]) {
                return false;
            }
        }
        return true;
    };
    if (starts("knife_")) {
        return WeaponKind::Dagger;
    }
    if (starts("stave_") || starts("polearm_")) {
        return WeaponKind::TwoHandLoose;
    }
    if (starts("bow_")) {
        return WeaponKind::Bow;
    }
    if (starts("firearm_")) {
        return WeaponKind::Gun;
    }
    if (starts("sword_2h") || starts("axe_2h") || starts("hammer_2h") || starts("mace_2h")) {
        return WeaponKind::TwoHand;
    }
    if (starts("sword_1h") || starts("axe_1h") || starts("hammer_1h") || starts("mace_1h") ||
        starts("club_1h")) {
        return WeaponKind::OneHand;
    }
    return WeaponKind::None;
}

} // namespace

u32 CrowdFight::random() {
    m_random ^= m_random << 13;
    m_random ^= m_random >> 17;
    m_random ^= m_random << 5;
    return m_random;
}

void CrowdFight::start(Actors& actors, Effects& effects, const game::SpellVisualData& data,
                       const glm::dvec3& center, u32 fighters, u32 seed, f32 floor) {
    clear();
    const game::AppearanceDb& db = actors.appearance();
    if (!db.ready() || fighters == 0) {
        return;
    }
    bind(&effects, &actors);
    m_data = &data;
    m_visuals.init(&data, this);
    m_visuals.trace = VD_DEV_ENV("VD_TRACE_FX") != nullptr;
    m_visuals.setKitFx(m_kitFx, m_shakes); // phase 14
    m_center = center;
    m_floor = floor;
    m_random = seed ? seed : 1u;
    m_started = false;
    m_stats = {};

    // The weapons by kind.
    std::array<std::vector<u32>, 16> weapons;
    for (u32 display : db.weaponDisplays()) {
        weapons[static_cast<u32>(kindOfModel(db.itemModel(display)))].push_back(display);
    }
    const auto pickWeapon = [&](WeaponKind kind) -> u32 {
        const std::vector<u32>& list = weapons[static_cast<u32>(kind)];
        return list.empty() ? 0 : list[below(static_cast<u32>(list.size()))];
    };

    // Two teams facing each other along x: melee in front, the others behind.
    constexpr Role kPattern[] = {Role::Shield, Role::Caster, Role::TwoHand,   Role::Healer, Role::DualWield,
                                 Role::Hunter, Role::Staff,  Role::Channeler, Role::Shield, Role::Caster};
    m_fighters.resize(fighters);
    std::array<u32, 2> meleeCount{}, rangedCount{};
    for (u32 i = 0; i < fighters; ++i) {
        Fighter& f = m_fighters[i];
        f.team = static_cast<u8>(i % 2);
        f.role = kPattern[(i / 2) % std::size(kPattern)];
        const bool meleeRole = f.role == Role::Shield || f.role == Role::TwoHand ||
                               f.role == Role::DualWield || f.role == Role::Staff;
        const f64 side = f.team == 0 ? -1.0 : 1.0;
        if (meleeRole) {
            const u32 n = meleeCount[f.team]++;
            f.home = center + glm::dvec3(side * 1.4, (static_cast<f64>(n) - 3.0) * 2.8, 0.0);
            f.runner = n % 4 == 3;
        } else {
            const u32 n = rangedCount[f.team]++;
            f.home = center + glm::dvec3(side * (13.0 + static_cast<f64>(n % 2) * 6.0),
                                         (static_cast<f64>(n / 2) - 2.0) * 4.5, 0.0);
        }
        f.feet = f.home;
        f.yaw = f.team == 0 ? 0.0f : static_cast<f32>(kPi);
        f.angle = static_cast<f64>(i) * 0.7;

        // Dressed at random, armed for its role (unsheathed: in the hands).
        u32 state = m_random;
        game::CharacterLook look = Crowd::randomCharacter(db, state);
        m_random = state ? state : 1u;
        const auto arm = [&](game::EquipSlot slot, WeaponKind kind, u8 inventoryType) {
            const u32 display =
                kind == WeaponKind::Shield
                    ? (db.shieldDisplays().empty()
                           ? 0
                           : db.shieldDisplays()[below(static_cast<u32>(db.shieldDisplays().size()))])
                    : pickWeapon(kind);
            if (display != 0) {
                look.items[slot] = display;
                look.inventoryTypes[slot] = inventoryType;
                (slot == game::kSlotOffHand ? f.gear.offHand : f.gear.mainHand) = kind;
            }
        };
        switch (f.role) {
        case Role::Shield:
            arm(game::kSlotMainHand, WeaponKind::OneHand, 13);
            arm(game::kSlotOffHand, WeaponKind::Shield, game::kInventoryShield);
            break;
        case Role::TwoHand: arm(game::kSlotMainHand, WeaponKind::TwoHand, 17); break;
        case Role::DualWield:
            arm(game::kSlotMainHand, WeaponKind::OneHand, 13);
            arm(game::kSlotOffHand, below(2) ? WeaponKind::Dagger : WeaponKind::OneHand, 22);
            break;
        case Role::Staff:
        case Role::Caster:
        case Role::Channeler: arm(game::kSlotMainHand, WeaponKind::TwoHandLoose, 17); break;
        case Role::Healer: arm(game::kSlotMainHand, WeaponKind::OneHand, 13); break;
        case Role::Hunter: {
            // Phase 13: the bow (or gun) in its ranged slot, drawn: a bow in the left hand, a gun in the right.
            const WeaponKind ranged = below(3) == 0 ? WeaponKind::Gun : WeaponKind::Bow;
            if (const u32 display = pickWeapon(ranged); display != 0) {
                look.items[game::kSlotRanged] = display;
                look.inventoryTypes[game::kSlotRanged] =
                    ranged == WeaponKind::Gun ? game::kInventoryRangedRight : game::kInventoryRanged;
                f.gear.ranged = ranged;
            }
            break;
        }
        default: break;
        }
        f.actor = actors.create();
        actors.setBody(f.actor, true); // phase 14 (bodyfx)
        actors.setLook(f.actor, db.character(look, false, f.role == Role::Hunter));
        actors.setPlacement(f.actor, f.feet, f.yaw, 1.0f);
        // Every other melee fighter's main hand glows.
        std::array<u64, game::SpellVisualData::kItemVisualSlots> glow{};
        if ((f.role == Role::TwoHand || f.role == Role::DualWield || f.role == Role::Shield) &&
            (i / 2) % 2 == 0 && data.enchantModels(kEnchants[(i / 4) % std::size(kEnchants)], glow)) {
            for (u32 point = 0; point < glow.size(); ++point) {
                if (glow[point] != 0) {
                    f.enchant[point] = effects.spawnOnItem(glow[point], f.actor, game::kSlotMainHand, point);
                }
            }
        }
    }
    // Its spells' models load now (the benchmark's warm-up), not at their first cast.
    for (u32 spell :
         {kFrostbolt,  kFireball,     kShadowBolt,      kLightningBolt,  kWrath,           kSmite,
          kFear,       kFrostNova,    kHammerOfJustice, kHuntersMark,    kHeroicStrike,    kSinisterStrike,
          kShieldBash, kHeal,         kRenew,           kShield,         kArcaneShot,      kBlizzard,
          kRainOfFire, kDrainLife,    kMindFlay,        kArcaneMissiles, kLightningShield, kChainLightning,
          kChainHeal,  kHealthFunnel, kDrainMana,       kThunderClap}) {
        m_visuals.prewarm(spell);
    }
    // Melee foes: the one across; the others pick theirs as they go.
    for (u32 i = 0; i < fighters; ++i) {
        Fighter& f = m_fighters[i];
        f.foe = i + 1 < fighters && f.team == 0 ? i + 1 : (i > 0 ? i - 1 : 0);
    }
    VD_LOG_INFO("Fight: %u fighters around (%.1f, %.1f)", fighters, center.x, center.y);
}

void CrowdFight::startDuel(Actors& actors, Effects& effects, const game::SpellVisualData& data,
                           const glm::dvec3& center, const glm::dvec3& across, u32 spell) {
    start(actors, effects, data, center, 2, 777);
    if (m_fighters.size() != 2) {
        return;
    }
    m_duelSpell = spell;
    for (u32 i = 0; i < 2; ++i) {
        Fighter& f = m_fighters[i];
        f.home = center + across * (i == 0 ? -9.0 : 9.0);
        f.feet = f.home;
        f.runner = false;
        f.foe = 1 - i;
    }
    m_fighters[0].role = Role::Caster;
    VD_LOG_INFO("Duel: spell %u", spell);
}

void CrowdFight::clear() {
    m_duelSpell = 0;
    if (m_actors) {
        m_visuals.clear();
        for (const Fighter& f : m_fighters) {
            for (const EffectHandle& e : f.enchant) {
                m_effects->stop(e);
            }
            m_actors->destroy(f.actor);
        }
    }
    m_fighters.clear();
}

// --- Decisions ---------------------------------------------------------------------------------------------

u32 CrowdFight::enemy(u32 index) {
    const Fighter& me = m_fighters[index];
    if (me.foe < m_fighters.size() && !m_fighters[me.foe].dead && m_fighters[me.foe].team != me.team &&
        below(3) != 0) {
        return me.foe;
    }
    for (u32 tries = 0; tries < 8; ++tries) {
        const u32 other = below(static_cast<u32>(m_fighters.size()));
        if (m_fighters[other].team != me.team && !m_fighters[other].dead) {
            return other;
        }
    }
    return me.foe;
}

u32 CrowdFight::ally(u32 index) {
    const Fighter& me = m_fighters[index];
    for (u32 tries = 0; tries < 8; ++tries) {
        const u32 other = below(static_cast<u32>(m_fighters.size()));
        if (m_fighters[other].team == me.team && !m_fighters[other].dead) {
            return other;
        }
    }
    return index;
}

void CrowdFight::addAura(u32 index, u32 spell, u32 seconds, u32 nowMs) {
    Fighter& f = m_fighters[index];
    Aura* slot = &f.auras[0];
    for (Aura& a : f.auras) {
        if (a.spell == spell || a.spell == 0) {
            slot = &a;
            break;
        }
        if (static_cast<i32>(a.untilMs - slot->untilMs) < 0) {
            slot = &a; // the oldest goes
        }
    }
    if (slot->spell != 0 && slot->spell != spell) {
        m_visuals.auraRemoved(key(index), slot->spell);
    }
    slot->spell = spell;
    slot->untilMs = nowMs + seconds * 1000;
    m_visuals.auraAdded(key(index), spell);
}

void CrowdFight::die(u32 index, u32 nowMs) {
    Fighter& f = m_fighters[index];
    f.dead = true;
    f.riseMs = nowMs + kDeadMs;
    if (f.cast != 0) {
        m_visuals.spellInterrupted(key(index), nowMs);
        if (f.channel) {
            m_visuals.channel(key(index), 0, 0, nowMs);
            m_visuals.areaGone(areaKey(index));
        }
        f.cast = 0;
    }
    for (Aura& a : f.auras) {
        if (a.spell != 0) {
            m_visuals.auraRemoved(key(index), a.spell);
            a = {};
        }
    }
    m_actors->stopAction(f.actor);
    ++m_stats.deaths;
}

void CrowdFight::melee(u32 index, u32 nowMs) {
    Fighter& f = m_fighters[index];
    const u32 target = enemy(index);
    Fighter& victim = m_fighters[target];
    ++f.swings;
    ++m_stats.swings;
    // Every few swings an ability with its own visual (and animation), else the auto attack.
    if (f.swings % 4 == 0) {
        const u32 spell = f.role == Role::DualWield ? kSinisterStrike
                          : f.role == Role::Shield  ? (f.swings % 8 == 0    ? kShieldBash
                                                       : f.swings % 12 == 4 ? kThunderClap // phase 14
                                                                            : kHeroicStrike)
                          : f.role == Role::TwoHand ? (f.swings % 12 == 0 ? kHammerOfJustice : kHeroicStrike)
                                                    : kHeroicStrike;
        m_visuals.spellGo(key(index), spell, key(target), nullptr, nowMs);
        m_visuals.spellHit(key(index), spell, key(target), nowMs);
        if (spell == kHammerOfJustice) {
            addAura(target, spell, 3, nowMs); // stunned: its state kit (the swirl and the Stun loop)
        }
        return;
    }
    const bool offHand = f.role == Role::DualWield && f.swings % 2 == 0;
    m_actors->playAction(f.actor, game::swingAnimation(f.gear, offHand));
    // The victim: a wound, a critical, a dodge, a parry or a block, as the combat log would say.
    game::CombatEvent e;
    const u32 roll = below(100);
    e.kind = roll < 67                                   ? game::CombatEvent::Kind::Damage
             : roll < 78                                 ? game::CombatEvent::Kind::Dodge
             : roll < 89                                 ? game::CombatEvent::Kind::Parry
             : victim.gear.offHand == WeaponKind::Shield ? game::CombatEvent::Kind::Block
                                                         : game::CombatEvent::Kind::Damage;
    e.amount = 100;
    e.flags = roll < 12 ? game::CombatEvent::kCrit : 0;
    if (!victim.dead && m_bodyFx && m_effects && e.kind == game::CombatEvent::Kind::Damage) {
        // Phase 14 (bodyfx): the blow bleeds; a critical one as the server's large spurts (HITINFO_BLOOD_SPURT).
        m_bodyFx->bleed(*m_effects, *m_actors, victim.actor, victim.feet, victim.yaw, &f.feet,
                        (e.flags & game::CombatEvent::kCrit) != 0);
    }
    if (!victim.dead) {
        const u16 reaction = game::reactionAnimation(e, victim.gear);
        if (reaction != game::kNoAnimation && m_actors->action(victim.actor) == Actors::kNoAction) {
            m_actors->playAction(victim.actor, reaction);
        }
    }
}

void CrowdFight::cast(u32 index, u32 spell, u32 target, u32 castMs, u32 nowMs) {
    Fighter& f = m_fighters[index];
    f.cast = spell;
    f.castTarget = target;
    f.castEndMs = nowMs + castMs;
    f.channel = false;
    f.interrupted = below(10) == 0;
    m_visuals.spellStart(key(index), spell, target ? key(target - 1) : 0, castMs, nowMs);
    ++m_stats.casts;
}

void CrowdFight::hit(u32 caster, u32 spell, u32 target, u32 nowMs) {
    m_visuals.spellGo(key(caster), spell, key(target), nullptr, nowMs);
    m_visuals.spellHit(key(caster), spell, key(target), nowMs);
}

void CrowdFight::chainHit(u32 caster, u32 spell, u32 first, u32 nowMs) {
    // The units it jumps to, in order (the hit list of SMSG_SPELL_GO): the first, then two more of its side.
    m_visuals.spellGo(key(caster), spell, key(first), nullptr, nowMs);
    m_visuals.spellHit(key(caster), spell, key(first), nowMs);
    const u8 side = m_fighters[first].team;
    u32 previous = first;
    for (u32 jump = 0; jump < 2; ++jump) {
        u32 next = previous;
        for (u32 tries = 0; tries < 8 && (next == previous || next == first || next == caster); ++tries) {
            const u32 other = below(static_cast<u32>(m_fighters.size()));
            if (m_fighters[other].team == side && !m_fighters[other].dead) {
                next = other;
            }
        }
        if (next == previous || next == first || next == caster) {
            break;
        }
        m_visuals.spellHit(key(caster), spell, key(next), nowMs);
        previous = next;
    }
}

void CrowdFight::finishCast(u32 index, u32 nowMs) {
    Fighter& f = m_fighters[index];
    const u32 spell = f.cast;
    f.cast = 0;
    if (f.channel) {
        m_visuals.channel(key(index), 0, 0, nowMs);
        m_visuals.areaGone(areaKey(index));
        f.nextMs = nowMs + 1500 + below(1500);
        return;
    }
    if (f.interrupted) {
        m_visuals.spellInterrupted(key(index), nowMs);
        f.nextMs = nowMs + 600;
        return;
    }
    const u32 target = f.castTarget > 0 ? f.castTarget - 1 : index;
    if (spell == kChainLightning || spell == kChainHeal) {
        chainHit(index, spell, target, nowMs); // phase 14: its jumps
    } else {
        hit(index, spell, target, nowMs);
    }
    if (spell == kFear) {
        addAura(target, kFear, 5, nowMs);
    }
    f.nextMs = nowMs + 400 + below(900);
}

void CrowdFight::duel(u32 nowMs) {
    Fighter& f = m_fighters[0];
    const game::SpellVisualInfo* visual = m_data->spellVisual(m_duelSpell);
    f.nextMs = nowMs + 4000;
    if (!visual) {
        hit(0, m_duelSpell, 1, nowMs);
        return;
    }
    if (visual->areaModel != 0 || visual->channelKit != 0) {
        // A channel: at the other fighter, or on the ground under it.
        const bool ground = visual->areaModel != 0;
        f.ground = m_fighters[1].feet;
        m_visuals.spellGo(key(0), m_duelSpell, ground ? 0 : key(1), ground ? &f.ground : nullptr, nowMs);
        if (visual->channelKit != 0) {
            m_visuals.channel(key(0), m_duelSpell, ground ? 0 : key(1), nowMs);
        }
        f.cast = m_duelSpell;
        f.castTarget = ground ? 0 : 2;
        f.channel = true;
        f.castEndMs = nowMs + 5000;
        f.nextMs = nowMs + 7000;
    } else if (visual->precastKit != 0) {
        cast(0, m_duelSpell, 2, 2000, nowMs);
        f.interrupted = false;
    } else {
        hit(0, m_duelSpell, 1, nowMs);
    }
    if (visual->stateKit != 0) {
        addAura(1, m_duelSpell, 6, nowMs);
    }
}

void CrowdFight::act(u32 index, u32 nowMs) {
    if (m_duelSpell != 0) {
        if (index == 0) {
            duel(nowMs);
        } else {
            m_fighters[index].nextMs = nowMs + 100000; // the target only stands there
        }
        return;
    }
    Fighter& f = m_fighters[index];
    switch (f.role) {
    case Role::Shield:
    case Role::TwoHand:
    case Role::DualWield:
    case Role::Staff:
        melee(index, nowMs);
        f.nextMs = nowMs + (f.role == Role::DualWield ? 1100 : 1700) + below(900);
        break;
    case Role::Caster: {
        const u32 roll = below(100);
        const u32 target = enemy(index);
        f.foe = target;
        if (roll < 70) {
            cast(index, kBolts[(index / 2 + f.swings++) % std::size(kBolts)], target + 1, 2500, nowMs);
        } else if (roll < 85) {
            cast(index, kFear, target + 1, 1500, nowMs);
        } else {
            // Frost Nova: instant, around the caster; the front line's melee are frozen.
            m_visuals.spellGo(key(index), kFrostNova, 0, nullptr, nowMs);
            addAura(target, kFrostNova, 4, nowMs);
            f.nextMs = nowMs + 1500 + below(1000);
        }
        break;
    }
    case Role::Healer: {
        const u32 roll = below(100);
        const u32 target = ally(index);
        if (roll < 35) {
            cast(index, kHeal, target + 1, 3000, nowMs);
        } else if (roll < 50) {
            cast(index, kChainHeal, target + 1, 2500, nowMs); // phase 14: its beam jumps between allies
        } else {
            const u32 spell = roll < 75 ? kRenew : kShield;
            hit(index, spell, target, nowMs);
            addAura(target, spell, spell == kRenew ? 8 : 10, nowMs);
            f.nextMs = nowMs + 1500 + below(1500);
        }
        break;
    }
    case Role::Hunter: {
        const u32 target = enemy(index);
        f.foe = target;
        ++f.swings;
        ++m_stats.swings;
        // Auto Shot (no visual of its own: the ranged attack animation), Arcane Shot's missile, a mark.
        m_actors->playAction(f.actor, game::rangedAttackAnimation(f.gear.ranged));
        if (f.swings % 3 == 0) {
            hit(index, kArcaneShot, target, nowMs);
        } else {
            m_visuals.spellGo(key(index), kAutoShot, key(target), nullptr, nowMs);
        }
        if (f.swings % 7 == 0) {
            hit(index, kHuntersMark, target, nowMs);
            addAura(target, kHuntersMark, 12, nowMs);
        }
        f.nextMs = nowMs + 2000 + below(600);
        break;
    }
    case Role::Channeler: {
        const u32 target = enemy(index);
        f.foe = target;
        if (f.swings++ % 2 == 0) {
            // Rain on the other side's front line: the channel, its ground pass and the area while it lasts.
            const u32 spell = kGroundChannels[(index / 2) % std::size(kGroundChannels)];
            f.ground = m_center +
                       glm::dvec3(f.team == 0 ? 3.0 : -3.0, (static_cast<f64>(below(5)) - 2.0) * 2.5, 0.0);
            m_visuals.spellGo(key(index), spell, 0, &f.ground, nowMs);
            m_visuals.channel(key(index), spell, 0, nowMs);
            f.cast = spell;
            f.castTarget = 0;
        } else {
            const u32 spell = kDrains[(index / 2 + f.swings) % std::size(kDrains)];
            const u32 at = spell == kHealthFunnel ? ally(index) : target; // phase 14: Funnel feeds an ally
            m_visuals.spellGo(key(index), spell, key(at), nullptr, nowMs);
            m_visuals.channel(key(index), spell, key(at), nowMs);
            f.cast = spell;
            f.castTarget = at + 1;
        }
        f.channel = true;
        f.castEndMs = nowMs + 6000;
        ++m_stats.casts;
        break;
    }
    default: break;
    }
}

// --- Per frame ---------------------------------------------------------------------------------------------

void CrowdFight::update(u32 nowMs, f32 dt, const glm::dvec3& camera, const Terrain* terrain) {
    if (m_fighters.empty() || !m_actors) {
        return;
    }
    VD_PROFILE_ZONE("CrowdFight::update");
    const Clock::time_point start = Clock::now();
    m_nowMs = nowMs;
    if (!m_started) {
        // Staggered starts; buffs on some, a lightning shield on the channelers.
        m_started = true;
        m_nextDeathMs = nowMs + kDeathEveryMs;
        for (u32 i = 0; i < m_fighters.size(); ++i) {
            m_fighters[i].nextMs = nowMs + 300 + below(2500);
            if (m_fighters[i].role == Role::Channeler) {
                addAura(i, kLightningShield, 600, nowMs);
            }
        }
    }
    if (m_duelSpell == 0 && static_cast<i32>(nowMs - m_nextDeathMs) >= 0) {
        // A melee fighter falls (and gets up later).
        m_nextDeathMs = nowMs + kDeathEveryMs;
        const u32 index = below(static_cast<u32>(m_fighters.size()));
        if (!m_fighters[index].dead && m_fighters[index].role != Role::Channeler) {
            die(index, nowMs);
        }
    }
    for (u32 i = 0; i < m_fighters.size(); ++i) {
        Fighter& f = m_fighters[i];
        // Auras run out.
        for (Aura& a : f.auras) {
            if (a.spell != 0 && static_cast<i32>(nowMs - a.untilMs) >= 0) {
                m_visuals.auraRemoved(key(i), a.spell);
                a = {};
            }
        }
        if (f.dead && static_cast<i32>(nowMs - f.riseMs) >= 0) {
            f.dead = false;
            f.nextMs = nowMs + 1500;
        }
        // Where it is: at home facing its foe, or running around it.
        const Fighter& foe = m_fighters[std::min<u32>(f.foe, static_cast<u32>(m_fighters.size()) - 1)];
        bool moving = false;
        if (f.runner && !f.dead) {
            f.angle += static_cast<f64>(kRunSpeed) * dt / kRunRadius;
            f.feet =
                foe.home + glm::dvec3(std::cos(f.angle) * kRunRadius, std::sin(f.angle) * kRunRadius, 0.0);
            f.yaw = static_cast<f32>(f.angle + kPi / 2.0);
            moving = true;
        } else {
            f.feet = f.home;
            const glm::dvec3 to = (f.channel && f.castTarget == 0 && f.cast != 0) ? f.ground : foe.feet;
            if (std::abs(to.x - f.feet.x) + std::abs(to.y - f.feet.y) > 0.1) {
                f.yaw = static_cast<f32>(std::atan2(to.y - f.feet.y, to.x - f.feet.x));
            }
        }
        if (terrain) {
            if (const std::optional<f32> ground = terrain->heightAt(f.feet.x, f.feet.y)) {
                f.feet.z = std::max(*ground, m_floor);
            }
        }
        const game::ActorLook* look = m_actors->look(f.actor);
        m_actors->setPlacement(f.actor, f.feet, f.yaw, look ? look->scale : 1.0f);
        if (m_fades) {
            // Phase 14: the colors of its auras (Frost Nova's blue), as the server's units get them.
            game::UnitFade fade;
            for (const Aura& a : f.auras) {
                if (const game::UnitFade* aura = a.spell != 0 ? m_fades->ofSpell(a.spell) : nullptr) {
                    fade = game::combineFades(fade, *aura);
                }
            }
            m_actors->setFade(f.actor, glm::vec4(fade.tint, fade.alpha));
        }
        // Its base animation, as the server's units pick theirs.
        game::UnitAnimState state;
        state.dead = f.dead;
        state.movement = moving ? animation::kRun : animation::kStand;
        state.inCombat = true;
        state.ranged = f.role == Role::Hunter;
        state.gear = f.gear;
        const game::BaseAnimation base = game::baseAnimation(state);
        m_actors->play(f.actor, base.animation, base.loop, 1.0f, moving ? kRunSpeed : 0.0f);
        if (f.dead) {
            continue;
        }
        // Its ground area lives while it channels.
        if (f.cast != 0 && f.channel && f.castTarget == 0) {
            m_visuals.area(areaKey(i), f.cast, f.ground, 8.0f, nowMs);
        }
        if (f.cast != 0) {
            if (static_cast<i32>(nowMs - f.castEndMs) >= 0) {
                finishCast(i, nowMs);
            }
            continue;
        }
        if (static_cast<i32>(nowMs - f.nextMs) >= 0) {
            act(i, nowMs);
        }
    }
    m_stats.updateMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
    m_visuals.update(nowMs, dt, camera);
}

bool CrowdFight::unit(u64 k, Unit& out) const {
    if (k == 0 || k > m_fighters.size()) {
        return false;
    }
    const Fighter& f = m_fighters[k - 1];
    out.actor = f.actor;
    out.feet = f.feet;
    out.yaw = f.yaw;
    out.height = heightOf(f.actor, f.feet);
    out.ranged = f.gear.ranged; // phase 14: the precast's load
    return true;
}

} // namespace vd
