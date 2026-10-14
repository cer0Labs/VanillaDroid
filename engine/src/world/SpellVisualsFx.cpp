// Phase 14 (spellfx): the parts of SpellVisuals that come from the kits' procs and shakes (game/SpellKitFx):
// the beams of channels and chains, the tints and flashes of units, the camera shakes of kits and the
// precast's load and hold animations. The rules are WoW.exe 1.12.1's (addresses in the comments; read-only
// disassembly).
#include "world/SpellVisuals.h"

#include "core/Profile.h"
#include "world/CameraShakes.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>

namespace vd {
namespace {

constexpr u32 kHandLeft = 2; // the client's beams leave the model's $CSL event: the left hand of characters
// AnimationData ids of the ranged weapons' loads (WoW.exe 0x5FD48F picks one by the ranged weapon).
constexpr u16 kLoadBow = 105, kLoadRifle = 106, kHoldThrown = 111, kLoadThrown = 112;
// Spell.dbc EquippedItemSubClassMask bits of the ranged weapons (game::SpellVisualData::rangedWeaponMask).
constexpr u32 kMaskBow = 0x4, kMaskGun = 0x8, kMaskThrown = 0x10000, kMaskCrossbow = 0x40000,
              kMaskWand = 0x80000;

bool reached(u32 nowMs, u32 atMs) {
    return static_cast<i32>(nowMs - atMs) >= 0;
}

glm::vec3 rgb(u32 color) {
    return glm::vec3(static_cast<f32>((color >> 16) & 0xFF), static_cast<f32>((color >> 8) & 0xFF),
                     static_cast<f32>(color & 0xFF)) /
           255.0f;
}

u32 milliseconds(f32 seconds) {
    return static_cast<u32>(std::clamp(seconds * 1000.0f, 0.0f, 2.0e9f));
}

u16 loadAnimation(game::WeaponKind weapon, u32 mask) {
    switch (weapon) {
    case game::WeaponKind::Bow: return kLoadBow; // crossbows load like rifles in 1.12 (both are Bow here)
    case game::WeaponKind::Gun: return kLoadRifle;
    case game::WeaponKind::Thrown: return kLoadThrown;
    case game::WeaponKind::Wand: return kHoldThrown;
    default: break;
    }
    if (mask & kMaskBow) {
        return kLoadBow;
    }
    if (mask & (kMaskGun | kMaskCrossbow)) {
        return kLoadRifle;
    }
    if (mask & kMaskThrown) {
        return kLoadThrown;
    }
    return mask & kMaskWand ? kHoldThrown : game::kNoKitAnimation;
}

} // namespace

u32 missileAttachment(u32 index) {
    static constexpr std::array<u32, 8> kPoints = {20, 34, 19, 21, 22, 17, 23, 24};
    return index < kPoints.size() ? kPoints[index] : 34;
}

void SpellVisuals::precastAnimations(const game::VisualKit* kit, u32 spell,
                                     const SpellVisualWorld::Unit* caster, bool unitTarget, u16& once,
                                     u16& loop) const {
    u16 animation = kit ? kit->animation : game::kNoKitAnimation;
    if (animation == game::kNoKitAnimation && m_fx) {
        // A shot with a cast time (an NPC's Shoot, Aimed Shot): the ranged weapon's load, then its hold.
        const u32 mask = m_data->rangedWeaponMask(spell);
        if (mask != 0) {
            animation = loadAnimation(caster ? caster->ranged : game::WeaponKind::None, mask);
        }
    }
    // SpellVisualPrecastTransitions: once the load has played, the hold loops (WoW.exe's animation-finished
    // switch at 0x5FC3F0 does the same while the unit still casts).
    const u16 hold = m_fx ? m_fx->holdAfter(animation) : game::kNoKitAnimation;
    if (hold != game::kNoKitAnimation) {
        once = animation;
        loop = hold;
        return;
    }
    once = game::kNoKitAnimation;
    loop = game::precastAnimation(animation, unitTarget);
}

void SpellVisuals::kitPlayed(const game::VisualKit* kit, u64 unitKey, const SpellVisualWorld::Unit& unit,
                             u32 nowMs) {
    const game::KitFx* fx = m_fx && kit ? m_fx->kit(kit->id) : nullptr;
    if (!fx) {
        return;
    }
    // WoW.exe 0x60F4C8: a kit with a shake starts the three CameraShakes of its SpellEffectCameraShakes where
    // it plays (the camera's falloff decides how much reaches it).
    if (fx->shake != 0 && m_shakes && shakesEnabled) {
        m_shakes->addEffect(*m_fx, fx->shake, unit.feet, nowMs);
        ++m_stats.shakes;
    }
    // 0x60CAA0: a flash replaces the model's color for a while, then fades back to it.
    if (fx->flashes && tintsEnabled) {
        if (Tint* t = tintOf(unitKey)) {
            t->flashing = true;
            t->flash = rgb(fx->flashColor);
            t->flashStartMs = nowMs;
            t->flashHoldMs = milliseconds(fx->flashHoldSeconds);
            t->flashFadeMs = milliseconds(fx->flashFadeSeconds);
        }
    }
}

SpellVisuals::Chain* SpellVisuals::findChain(u64 caster, u32 spell, bool cast) {
    Chain* free = nullptr;
    Chain* oldest = nullptr;
    for (Chain& c : m_chains) {
        if (c.active && c.caster == caster && c.spell == spell && (c.kit != 0) == cast) {
            return &c;
        }
        if (!c.active && !free) {
            free = &c;
        }
        if (c.active && (!oldest || static_cast<i32>(c.ms - oldest->ms) < 0)) {
            oldest = &c;
        }
    }
    if (free) {
        return free;
    }
    if (oldest) {
        *oldest = Chain{};
    }
    return oldest;
}

void SpellVisuals::chainTarget(u64 caster, u32 spell, u64 target, u32 index, u32 count, u32 nowMs) {
    if (!m_world || spell == 0) {
        return;
    }
    Chain* c = findChain(caster, spell, false);
    if (!c) {
        return;
    }
    if (index == 0 || !c->active) {
        // A new list (WoW.exe 0x6E9820 -> 0x605780 replaces the unit's list each time one comes).
        *c = Chain{};
        c->active = true;
        c->caster = caster;
        c->spell = spell;
        c->ms = nowMs;
    }
    if (target != 0 && target != caster && c->count < kMaxChainTargets && !c->complete) {
        c->targets[c->count++] = target; // the caster itself is left out (0x6057BA)
    }
    if (index + 1 >= count) {
        c->complete = true;
        // A channel already shown takes its units now.
        for (u32 i = 0; i < m_holds.size(); ++i) {
            const Hold& h = m_holds[i];
            if (h.active && h.shown && h.unit == caster && h.spell == spell && h.kind == HoldKind::Channel) {
                startHoldBeams(i, nowMs);
            }
        }
    }
}

void SpellVisuals::startBeams(const game::KitFx& fx, u64 caster, u32 spell, std::span<const u64> targets,
                              u32 owner, u32 nowMs) {
    const game::ChainEffect* effect = m_fx ? m_fx->chainEffect(fx.chainEffect) : nullptr;
    if (!effect) {
        return;
    }
    // 0x6ECBD0 / 0x6EC980: jump k goes from the unit before it (the caster first) to the k-th, starting k
    // delays later and lasting one segment duration; a held chain (a channel's) shows every jump until its
    // kit ends.
    const bool held = fx.chainHeld && owner != ~0u;
    u32 next = 0;
    for (u32 k = 0; k < targets.size(); ++k) {
        for (u32 copy = 0; copy < std::max<u32>(1, fx.chainBeams); ++copy) {
            while (next < m_beams.size() && m_beams[next].active) {
                ++next;
            }
            if (next == m_beams.size()) {
                ++m_stats.skipped;
                return;
            }
            Beam& b = m_beams[next];
            b = Beam{};
            b.active = true;
            b.effect = effect;
            b.from = k == 0 ? caster : targets[k - 1];
            b.to = targets[k];
            b.fromCaster = k == 0;
            b.spell = spell;
            b.owner = owner;
            b.seed = (static_cast<u32>(caster) * 2654435761u) ^ (next * 40503u + copy * 977u + k) | 1u;
            b.startMs = held ? nowMs : nowMs + k * effect->delayMs;
            b.endMs = held ? 0 : b.startMs + std::max(1u, effect->segmentMs);
        }
    }
}

void SpellVisuals::startHoldBeams(u32 holdIndex, u32 nowMs) {
    if (!m_fx || holdIndex >= m_holds.size()) {
        return;
    }
    const Hold& h = m_holds[holdIndex];
    const game::KitFx* fx = m_fx->kit(h.kit);
    if (!fx || fx->chainEffect == 0 || !h.active) {
        return;
    }
    stopHoldBeams(holdIndex);
    // The units the server told for this channel (the latest list, if it names the channel's target), else
    // the target it channels at.
    std::array<u64, kMaxChainTargets> targets{};
    u32 count = 0;
    Chain* c = findChain(h.unit, h.spell, false);
    if (c && c->active && c->complete && c->count > 0 &&
        (h.target == 0 || std::find(c->targets.begin(), c->targets.begin() + c->count, h.target) !=
                              c->targets.begin() + c->count)) {
        count = c->count;
        std::copy(c->targets.begin(), c->targets.begin() + count, targets.begin());
    } else if (h.target != 0) {
        targets[count++] = h.target;
    }
    if (count > 0) {
        startBeams(*fx, h.unit, h.spell, std::span<const u64>(targets.data(), count), holdIndex, nowMs);
    }
}

void SpellVisuals::stopHoldBeams(u32 holdIndex) {
    // 0x6ECE10: a held chain ends with its kit, at once.
    for (Beam& b : m_beams) {
        if (b.active && b.owner == holdIndex) {
            b = Beam{};
        }
    }
}

void SpellVisuals::finishCastChains(u32 nowMs) {
    (void)nowMs;
    for (Chain& c : m_chains) {
        if (!c.active || c.kit == 0) {
            continue;
        }
        // Its hits were all told (they come with the SPELL_GO, before this update).
        const game::KitFx* fx = m_fx ? m_fx->kit(c.kit) : nullptr;
        SpellVisualWorld::Unit caster;
        const bool shown =
            fx && beamsEnabled && c.count > 0 && m_world->unit(c.caster, caster) &&
            (involvesPlayer(c.caster, c.targets[0]) || near(caster.feet, m_settings.kitDistance));
        if (shown) {
            startBeams(*fx, c.caster, c.spell, std::span<const u64>(c.targets.data(), c.count), ~0u, c.ms);
            log("chain", c.caster, c.spell);
        }
        c = Chain{};
    }
}

glm::dvec3 SpellVisuals::beamStart(const SpellVisualWorld::Unit& caster) const {
    // 0x6EC6F0: the caster model's $CSL event (at the left hand of the characters), else three quarters of
    // its height over its feet.
    glm::dvec3 at;
    if (caster.actor != ~0u && m_world->hasAttachment(caster.actor, kHandLeft) &&
        m_world->attachmentPosition(caster.actor, kHandLeft, at)) {
        return at;
    }
    return caster.feet + glm::dvec3(0.0, 0.0, caster.height * 0.75);
}

glm::dvec3 SpellVisuals::beamEnd(const SpellVisualWorld::Unit& target, u32 spell) const {
    // 0x6EC780: the point of the spell visual's missile destination on the model, else where the unit stands.
    const game::SpellVisualInfo* visual = m_data->spellVisual(spell);
    const u32 attachment = missileAttachment(visual ? visual->missileAttachment : 1);
    glm::dvec3 at;
    if (target.actor != ~0u && m_world->attachmentPosition(target.actor, attachment, at)) {
        return at;
    }
    return target.feet;
}

void SpellVisuals::updateBeams(u32 nowMs) {
    VD_PROFILE_ZONE("SpellVisuals::updateBeams");
    m_beamFrame.clear();
    for (Beam& b : m_beams) {
        if (!b.active) {
            continue;
        }
        if (b.endMs != 0 && reached(nowMs, b.endMs)) {
            b = Beam{};
            continue;
        }
        if (!beamsEnabled || (b.endMs != 0 && !reached(nowMs, b.startMs))) {
            continue; // a later jump waits for its turn
        }
        // Hidden while one of its units is not there (0x7B0BD0).
        SpellVisualWorld::Unit from, to;
        if (!m_world->unit(b.from, from) || !m_world->unit(b.to, to)) {
            continue;
        }
        BeamInstance beam;
        beam.effect = b.effect;
        beam.from = b.fromCaster ? beamStart(from) : beamEnd(from, b.spell);
        beam.to = beamEnd(to, b.spell);
        beam.age = static_cast<f32>(nowMs - b.startMs) * 0.001f;
        beam.seed = (b.seed ^ (m_frame * 2246822519u)) | 1u; // new joints every frame
        m_beamFrame.push_back(beam);
    }
    m_stats.beams = static_cast<u32>(m_beamFrame.size());
}

SpellVisuals::Tint* SpellVisuals::tintOf(u64 unit) {
    Tint* free = nullptr;
    for (Tint& t : m_tints) {
        if (t.active && t.unit == unit) {
            return &t;
        }
        if (!t.active && !free) {
            free = &t;
        }
    }
    if (free) {
        *free = Tint{};
        free->active = true;
        free->unit = unit;
    }
    return free;
}

void SpellVisuals::updateTints(u32 nowMs) {
    VD_PROFILE_ZONE("SpellVisuals::updateTints");
    m_stats.tints = 0;
    for (Tint& t : m_tints) {
        t.hold = glm::vec3(1.0f);
    }
    // The tints of the kits held now (precasts, channels: Evocation's blue); the newest kit's color wins in
    // the client (0x60CB50), here they multiply.
    if (m_fx && tintsEnabled) {
        for (const Hold& h : m_holds) {
            const game::KitFx* fx = h.active && h.shown ? m_fx->kit(h.kit) : nullptr;
            if (fx && fx->tints) {
                if (Tint* t = tintOf(h.unit)) {
                    t->hold *= rgb(fx->tint);
                }
            }
        }
    }
    for (Tint& t : m_tints) {
        if (!t.active) {
            continue;
        }
        glm::vec3 color = t.hold;
        if (t.flashing && tintsEnabled) {
            // 0x60CBF0: the flash's color while it holds, then blended back to the model's over the fade.
            const u32 elapsed = nowMs - t.flashStartMs;
            if (elapsed < t.flashHoldMs) {
                color = t.flash;
            } else if (elapsed - t.flashHoldMs < t.flashFadeMs) {
                const f32 f =
                    1.0f - static_cast<f32>(elapsed - t.flashHoldMs) / static_cast<f32>(t.flashFadeMs);
                color = glm::mix(t.hold, t.flash, f);
            } else {
                t.flashing = false;
            }
        } else {
            t.flashing = false;
        }
        SpellVisualWorld::Unit unit;
        if (!m_world->unit(t.unit, unit)) {
            t = Tint{}; // it left
            continue;
        }
        if (unit.actor != ~0u) {
            m_world->setTint(unit.actor, color);
        }
        if (!t.flashing && color == glm::vec3(1.0f)) {
            t = Tint{}; // back to its own color: done
        } else {
            ++m_stats.tints;
        }
    }
}

} // namespace vd
