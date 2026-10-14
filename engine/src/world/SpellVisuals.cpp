#include "world/SpellVisuals.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/CombatAnimation.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

constexpr u32 kPrecastGraceMs =
    2000; // a precast outlives its cast time by this (pushback) before it is given up
constexpr u32 kAreaTimeoutMs = 2000; // an area not seen for this long is gone
constexpr u16 kNoAction = 0xFFFF;    // Actors::kNoAction
constexpr u32 kChest = 34, kChestFront = 15, kBase = 19, kHead = 20, kLeftHand = 21, kRightHand = 22;

f32 yawTowards(const glm::dvec3& from, const glm::dvec3& to) {
    const glm::dvec3 d = to - from;
    return std::abs(d.x) + std::abs(d.y) > 1e-6 ? static_cast<f32>(std::atan2(d.y, d.x)) : 0.0f;
}

bool reached(u32 nowMs, u32 atMs) {
    return static_cast<i32>(nowMs - atMs) >= 0;
}

} // namespace

void SpellVisuals::init(const game::SpellVisualData* data, SpellVisualWorld* world) {
    clear();
    m_data = data;
    m_world = world;
    m_holds.assign(kMaxHolds, Hold{});
    m_missiles.assign(kMaxMissiles, Missile{});
    m_auras.assign(kMaxAuras, Aura{});
    m_areas.assign(kMaxAreas, Area{});
    m_beams.assign(kMaxBeams, Beam{}); // phase 14
    m_chains.assign(kMaxChains, Chain{});
    m_tints.assign(kMaxTints, Tint{});
    m_beamFrame.clear();
    m_beamFrame.reserve(kMaxBeams);
    m_stats = {};
}

void SpellVisuals::clear() {
    if (!m_world) {
        return;
    }
    for (Hold& h : m_holds) {
        endHold(h);
    }
    for (Aura& a : m_auras) {
        endAura(a);
    }
    for (Area& a : m_areas) {
        endArea(a);
    }
    for (Missile& m : m_missiles) {
        if (m.active) {
            m_world->stop(m.model);
            m.active = false;
        }
    }
    // Phase 14: no beams, chains or colors left.
    for (Beam& b : m_beams) {
        b = Beam{};
    }
    for (Chain& c : m_chains) {
        c = Chain{};
    }
    for (Tint& t : m_tints) {
        SpellVisualWorld::Unit unit;
        if (t.active && m_world->unit(t.unit, unit) && unit.actor != ~0u) {
            m_world->setTint(unit.actor, glm::vec3(1.0f));
        }
        t = Tint{};
    }
    m_beamFrame.clear();
}

// --- Helpers -----------------------------------------------------------------------------------------------

bool SpellVisuals::near(const glm::dvec3& at, f32 distance) const {
    const glm::dvec3 d = at - m_camera;
    return glm::dot(d, d) < static_cast<f64>(distance) * static_cast<f64>(distance);
}

bool SpellVisuals::budget(bool important) const {
    return important || m_world->liveEffects() < m_settings.maxLiveEffects;
}

void SpellVisuals::log(const char* what, u64 unit, u32 id) const {
    if (trace) {
        VD_LOG_INFO("FX %s unit %llx id %u", what, static_cast<unsigned long long>(unit), id);
    }
}

u32 SpellVisuals::spawnKit(const game::VisualKit& kit, const SpellVisualWorld::Unit& unit, bool once,
                           std::array<EffectHandle, kHoldModels>* held) {
    u32 spawned = 0;
    const f32 seconds = once ? Effects::kOnce : 0.0f;
    for (u32 slot = 0; slot < game::kKitSlots; ++slot) {
        const u64 model = kit.models[slot];
        if (model == 0) {
            continue;
        }
        if (held && spawned >= held->size()) {
            break;
        }
        const game::KitSlot where = static_cast<game::KitSlot>(slot);
        EffectHandle effect;
        if (where == game::KitSlot::World) {
            effect = m_world->spawnAt(model, unit.feet, unit.yaw, seconds);
        } else if (unit.actor != ~0u) {
            u32 attachment = game::kitAttachment(where);
            if (!m_world->hasAttachment(unit.actor, attachment)) {
                attachment = game::kitFallbackAttachment(where);
            }
            effect = m_world->spawnOn(model, unit.actor, attachment, seconds);
        }
        if (!effect.valid()) {
            continue;
        }
        if (held) {
            (*held)[spawned] = effect;
        }
        ++spawned;
    }
    return spawned;
}

void SpellVisuals::playKitOnce(u32 kitId, u64 unitKey, bool important, u32 nowMs, u16 animation) {
    SpellVisualWorld::Unit unit;
    if (!m_world->unit(unitKey, unit)) {
        return;
    }
    if (!important && !near(unit.feet, m_settings.kitDistance)) {
        ++m_stats.skipped;
        return;
    }
    if (animation != game::kNoKitAnimation && unit.actor != ~0u) {
        m_world->playAction(unit.actor, animation, false);
    }
    const game::VisualKit* kit = m_data->kit(kitId);
    kitPlayed(kit, unitKey, unit, nowMs); // phase 14: its flash and camera shake
    if (!kit || kit->modelCount == 0) {
        return;
    }
    if (!budget(important)) {
        ++m_stats.skipped;
        return;
    }
    spawnKit(*kit, unit, true, nullptr);
    ++m_stats.kits;
    log("kit", unitKey, kitId);
}

void SpellVisuals::stopModels(std::array<EffectHandle, kHoldModels>& models) {
    for (EffectHandle& e : models) {
        if (e.valid()) {
            m_world->stop(e);
            e = {};
        }
    }
}

void SpellVisuals::keepLoop(const SpellVisualWorld::Unit& unit, u16 loop) {
    if (loop == game::kNoKitAnimation || unit.actor == ~0u) {
        return;
    }
    // A one-shot (a swing, a wound, the cast) may take the action for a moment: the loop comes back after it.
    if (m_world->action(unit.actor) == kNoAction) {
        m_world->playAction(unit.actor, loop, true);
    }
}

void SpellVisuals::releaseLoop(u64 unitKey, u16 loop) {
    SpellVisualWorld::Unit unit;
    if (loop != game::kNoKitAnimation && m_world->unit(unitKey, unit) && unit.actor != ~0u &&
        m_world->action(unit.actor) == loop) {
        m_world->stopAction(unit.actor);
    }
}

void SpellVisuals::endHold(Hold& hold) {
    if (!hold.active) {
        return;
    }
    stopHoldBeams(static_cast<u32>(&hold - m_holds.data())); // phase 14
    stopModels(hold.models);
    releaseLoop(hold.unit, hold.loop);
    hold = Hold{};
}

void SpellVisuals::endAura(Aura& aura) {
    if (!aura.active) {
        return;
    }
    stopModels(aura.models);
    releaseLoop(aura.unit, aura.loop);
    aura = Aura{};
}

void SpellVisuals::endArea(Area& area) {
    if (!area.active) {
        return;
    }
    for (EffectHandle& e : area.models) {
        if (e.valid()) {
            m_world->stop(e);
            e = {};
        }
    }
    area = Area{};
}

glm::dvec3 SpellVisuals::launchPoint(const SpellVisualWorld::Unit& caster) const {
    glm::dvec3 left, right;
    if (caster.actor != ~0u) {
        const bool hasLeft = m_world->attachmentPosition(caster.actor, kLeftHand, left);
        const bool hasRight = m_world->attachmentPosition(caster.actor, kRightHand, right);
        if (hasLeft && hasRight) {
            return (left + right) * 0.5; // between the hands, where the precast glowed
        }
        if (hasRight) {
            return right;
        }
        glm::dvec3 chest;
        if (m_world->attachmentPosition(caster.actor, kChest, chest)) {
            return chest;
        }
    }
    return caster.feet + glm::dvec3(0.0, 0.0, caster.height * 0.6);
}

glm::dvec3 SpellVisuals::landingPoint(const SpellVisualWorld::Unit& target, u32 attachment) const {
    glm::dvec3 at;
    if (target.actor != ~0u && (m_world->attachmentPosition(target.actor, attachment, at) ||
                                m_world->attachmentPosition(target.actor, kChestFront, at))) {
        return at;
    }
    return target.feet + glm::dvec3(0.0, 0.0, attachment == kBase ? 0.0 : target.height * 0.55);
}

// --- Events ------------------------------------------------------------------------------------------------

void SpellVisuals::spellStart(u64 caster, u32 spell, u64 target, u32 castMs, u32 nowMs) {
    if (!m_world || castMs == 0) {
        return; // instants: their cast kit plays on spellGo
    }
    Hold* slot = nullptr;
    for (Hold& h : m_holds) {
        if (h.active && h.unit == caster && h.kind == HoldKind::Precast) {
            endHold(h); // one cast at a time
        }
        if (!slot && !h.active) {
            slot = &h;
        }
    }
    if (!slot) {
        ++m_stats.skipped;
        return;
    }
    const game::SpellVisualInfo* visual = m_data->spellVisual(spell);
    const game::VisualKit* kit = visual ? m_data->kit(visual->precastKit) : nullptr;
    Hold& h = *slot;
    h = Hold{};
    h.active = true;
    h.kind = HoldKind::Precast;
    h.unit = caster;
    h.target = target;
    h.spell = spell;
    h.kit = kit ? kit->id : 0;
    h.endMs = nowMs + castMs + kPrecastGraceMs;
    // Phase 14: a load animation plays once before its hold loops (LoadBow -> HoldBow), and the kit's shake.
    SpellVisualWorld::Unit unit;
    const bool known = m_world->unit(caster, unit);
    u16 once = game::kNoKitAnimation;
    precastAnimations(kit, spell, known ? &unit : nullptr, target != 0 && target != caster, once, h.loop);
    if (known) {
        if (once != game::kNoKitAnimation && unit.actor != ~0u) {
            m_world->playAction(unit.actor, once, false);
        }
        kitPlayed(kit, caster, unit, nowMs);
    }
    log("precast", caster, spell);
}

void SpellVisuals::spellGo(u64 caster, u32 spell, u64 target, const glm::dvec3* destination, u32 nowMs) {
    if (!m_world) {
        return;
    }
    bool hadCastTime = false;
    for (Hold& h : m_holds) {
        if (h.active && h.unit == caster && h.kind == HoldKind::Precast && h.spell == spell) {
            hadCastTime = true;
            endHold(h);
        }
    }
    const game::SpellVisualInfo* visual = m_data->spellVisual(spell);
    const game::VisualKit* kit = visual ? m_data->kit(visual->castKit) : nullptr;
    const bool player = involvesPlayer(caster, target);
    const u16 animation = game::castAnimation(kit ? kit->animation : game::kNoKitAnimation,
                                              target != 0 && target != caster, hadCastTime);
    playKitOnce(kit ? kit->id : 0, caster, player, nowMs, animation);
    // Phase 14: a chain (Chain Lightning, Chain Heal): its jumps go through the units it hits, told next.
    if (const game::KitFx* fx = m_fx && kit ? m_fx->kit(kit->id) : nullptr; fx && fx->chainEffect != 0) {
        if (Chain* chain = findChain(caster, spell, true)) {
            *chain = Chain{};
            chain->active = true;
            chain->caster = caster;
            chain->spell = spell;
            chain->kit = kit->id;
            chain->ms = nowMs;
        }
    }
    // A ground spell: a pass of its area model where it lands (a dynamic object keeps it if the spell leaves
    // one).
    if (destination && visual && visual->areaModel != 0 &&
        (player || near(*destination, m_settings.areaDistance)) && budget(player)) {
        m_world->spawnAt(visual->areaModel, *destination, 0.0f, Effects::kOnce);
        log("area pass", caster, spell);
    }
}

void SpellVisuals::spellHit(u64 caster, u32 spell, u64 target, u32 nowMs) {
    if (!m_world) {
        return;
    }
    const game::SpellVisualInfo* visual = m_data->spellVisual(spell);
    if (!visual) {
        return;
    }
    const bool player = involvesPlayer(caster, target);
    // Phase 14: the next unit of a chain cast this frame.
    for (Chain& c : m_chains) {
        if (c.active && c.kit != 0 && !c.complete && c.caster == caster && c.spell == spell &&
            c.count < kMaxChainTargets) {
            c.targets[c.count++] = target;
        }
    }
    const game::VisualKit* impact = m_data->kit(visual->impactKit);
    const u16 impactAnimation = impact ? impact->animation : game::kNoKitAnimation;
    const f32 speed = m_data->missileSpeed(spell);
    SpellVisualWorld::Unit from, to;
    if (speed > 0.0f && m_world->unit(caster, from) && m_world->unit(target, to)) {
        // The audio's flight time: the distance between the two units over the spell's speed.
        const f64 distance = glm::length(to.feet - from.feet);
        if (distance > 1.0) {
            Missile* slot = nullptr;
            for (Missile& m : m_missiles) {
                if (!m.active) {
                    slot = &m;
                    break;
                }
            }
            if (slot) {
                Missile& m = *slot;
                m = Missile{};
                m.active = true;
                m.caster = caster;
                m.target = target;
                m.spell = spell;
                m.impactKit = visual->impactKit;
                m.startMs = nowMs;
                m.arriveMs =
                    nowMs + std::max<u32>(1, static_cast<u32>(distance / static_cast<f64>(speed) * 1000.0));
                m.targetAttachment = missileAttachment(visual->missileAttachment); // phase 14: 0x860A18
                m.arched = visual->missilePath == 2;
                m.from = launchPoint(from);
                m.last = m.from;
                if (visual->missileModel != 0 &&
                    (player || near(from.feet, m_settings.kitDistance) ||
                     near(to.feet, m_settings.kitDistance)) &&
                    budget(player)) {
                    m.model =
                        m_world->spawnAt(visual->missileModel, m.from, yawTowards(from.feet, to.feet), 0.0f);
                    log("missile", caster, spell);
                }
                return;
            }
        }
    }
    playKitOnce(visual->impactKit, target, player, nowMs, impactAnimation);
}

void SpellVisuals::spellInterrupted(u64 caster, u32 nowMs) {
    (void)nowMs;
    for (Hold& h : m_holds) {
        if (h.active && h.unit == caster && h.kind == HoldKind::Precast) {
            log("interrupted", caster, h.spell);
            endHold(h);
        }
    }
}

void SpellVisuals::channel(u64 caster, u32 spell, u64 target, u32 nowMs) {
    if (!m_world) {
        return;
    }
    Hold* slot = nullptr;
    for (Hold& h : m_holds) {
        if (h.active && h.unit == caster && h.kind == HoldKind::Channel) {
            if (h.spell == spell && h.target == target) {
                return; // already shown
            }
            endHold(h);
        }
        if (h.active && h.unit == caster && h.kind == HoldKind::Precast && spell != 0) {
            endHold(h); // the cast became a channel
        }
        if (!slot && !h.active) {
            slot = &h;
        }
    }
    if (spell == 0) {
        return;
    }
    if (!slot) {
        ++m_stats.skipped;
        return;
    }
    const game::SpellVisualInfo* visual = m_data->spellVisual(spell);
    const game::VisualKit* kit = visual ? m_data->kit(visual->channelKit) : nullptr;
    Hold& h = *slot;
    h = Hold{};
    h.active = true;
    h.kind = HoldKind::Channel;
    h.unit = caster;
    h.target = target;
    h.spell = spell;
    h.kit = kit ? kit->id : 0;
    h.loop =
        game::channelAnimation(kit ? kit->animation : game::kNoKitAnimation, target != 0 && target != caster);
    if (SpellVisualWorld::Unit unit; m_world->unit(caster, unit)) {
        kitPlayed(kit, caster, unit, nowMs); // phase 14: its shake and flash
    }
    log("channel", caster, spell);
}

void SpellVisuals::playKit(u64 unit, u32 kitId, u32 nowMs) {
    if (!m_world) {
        return;
    }
    const game::VisualKit* kit = m_data->kit(kitId);
    if (kit) {
        playKitOnce(kitId, unit, involvesPlayer(unit, 0), nowMs, kit->animation);
    }
}

void SpellVisuals::auraAdded(u64 unit, u32 spell) {
    if (!m_world) {
        return;
    }
    const game::SpellVisualInfo* visual = m_data->spellVisual(spell);
    const game::VisualKit* kit = visual ? m_data->kit(visual->stateKit) : nullptr;
    const game::KitFx* fx = m_fx && kit ? m_fx->kit(kit->id) : nullptr;
    if (!kit || (kit->modelCount == 0 && kit->animation == game::kNoKitAnimation && !fx)) {
        return; // most auras show nothing
    }
    Aura* slot = nullptr;
    for (Aura& a : m_auras) {
        if (a.active && a.unit == unit && a.spell == spell) {
            return;
        }
        if (!slot && !a.active) {
            slot = &a;
        }
    }
    if (!slot) {
        ++m_stats.skipped;
        return;
    }
    *slot = Aura{};
    slot->active = true;
    slot->unit = unit;
    slot->spell = spell;
    slot->kit = kit->id;
    slot->loop = kit->animation;
    if (SpellVisualWorld::Unit u; m_world->unit(unit, u)) {
        kitPlayed(kit, unit, u, m_lastMs); // phase 14: its shake and flash
    }
    log("state", unit, spell);
}

void SpellVisuals::auraRemoved(u64 unit, u32 spell) {
    for (Aura& a : m_auras) {
        if (a.active && a.unit == unit && a.spell == spell) {
            endAura(a);
        }
    }
}

void SpellVisuals::area(u64 object, u32 spell, const glm::dvec3& center, f32 radius, u32 nowMs) {
    if (!m_world) {
        return;
    }
    Area* slot = nullptr;
    for (Area& a : m_areas) {
        if (a.active && a.object == object) {
            a.seenMs = nowMs;
            a.center = center;
            return;
        }
        if (!slot && !a.active) {
            slot = &a;
        }
    }
    const game::SpellVisualInfo* visual = m_data->spellVisual(spell);
    if (!slot || !visual || visual->areaModel == 0) {
        return;
    }
    *slot = Area{};
    slot->active = true;
    slot->object = object;
    slot->spell = spell;
    slot->seenMs = nowMs;
    slot->nextMs = nowMs;
    slot->random = static_cast<u32>(object * 2654435761u) | 1u;
    slot->center = center;
    slot->radius = radius;
    log("area", object, spell);
}

void SpellVisuals::areaGone(u64 object) {
    for (Area& a : m_areas) {
        if (a.active && a.object == object) {
            endArea(a);
        }
    }
}

void SpellVisuals::unitGone(u64 unit) {
    for (Hold& h : m_holds) {
        if (h.active && h.unit == unit) {
            endHold(h);
        }
    }
    for (Aura& a : m_auras) {
        if (a.active && a.unit == unit) {
            endAura(a);
        }
    }
    for (Missile& m : m_missiles) {
        if (m.active && m.target == unit) {
            m.target = 0; // lands where it was going, without an impact
        }
    }
}

void SpellVisuals::prewarm(u32 spell) {
    const game::SpellVisualInfo* visual = m_world && m_data ? m_data->spellVisual(spell) : nullptr;
    if (!visual) {
        return;
    }
    for (u32 kitId :
         {visual->precastKit, visual->castKit, visual->impactKit, visual->stateKit, visual->channelKit}) {
        if (const game::VisualKit* kit = m_data->kit(kitId)) {
            for (u64 model : kit->models) {
                if (model != 0) {
                    m_world->preload(model);
                }
            }
        }
    }
    for (u64 model : {visual->missileModel, visual->areaModel}) {
        if (model != 0) {
            m_world->preload(model);
        }
    }
}

// --- Per frame ---------------------------------------------------------------------------------------------

void SpellVisuals::land(Missile& m, u32 nowMs) {
    m_world->stop(m.model);
    m.active = false;
    if (m.target != 0) {
        const game::VisualKit* impact = m_data->kit(m.impactKit);
        playKitOnce(m.impactKit, m.target, involvesPlayer(m.caster, m.target), nowMs,
                    impact ? impact->animation : game::kNoKitAnimation);
    }
}

void SpellVisuals::update(u32 nowMs, f32 dt, const glm::dvec3& camera) {
    (void)dt;
    if (!m_world || !m_data) {
        return;
    }
    VD_PROFILE_ZONE("SpellVisuals::update");
    const Clock::time_point start = Clock::now();
    m_camera = camera;
    m_lastMs = nowMs;
    ++m_frame;
    finishCastChains(nowMs); // phase 14: the jumps of the chains cast since the last frame
    m_stats.holds = m_stats.missiles = m_stats.auras = m_stats.shownAuras = m_stats.areas = 0;

    // Missiles: towards where the target is now, landing at the flight time the audio uses.
    for (Missile& m : m_missiles) {
        if (!m.active) {
            continue;
        }
        SpellVisualWorld::Unit target;
        const bool known = m.target != 0 && m_world->unit(m.target, target);
        if (!known) {
            m.target = 0;
        }
        if (reached(nowMs, m.arriveMs)) {
            land(m, nowMs);
            continue;
        }
        ++m_stats.missiles;
        if (!m.model.valid()) {
            continue; // too far to be drawn: only its impact will show
        }
        const glm::dvec3 to = known ? landingPoint(target, m.targetAttachment) : m.last;
        const f64 t =
            static_cast<f64>(nowMs - m.startMs) / static_cast<f64>(std::max(1u, m.arriveMs - m.startMs));
        glm::dvec3 at = glm::mix(m.from, to, std::clamp(t, 0.0, 1.0));
        if (m.arched) {
            at.z += std::sin(t * 3.14159265) * glm::length(to - m.from) * 0.2;
        }
        m_world->move(m.model, at, yawTowards(m.last, at));
        m.last = at;
    }

    // Precasts and channels: shown while near, given up when no cast came.
    for (Hold& h : m_holds) {
        if (!h.active) {
            continue;
        }
        if (h.kind == HoldKind::Precast && reached(nowMs, h.endMs)) {
            endHold(h);
            continue;
        }
        ++m_stats.holds;
        SpellVisualWorld::Unit unit;
        if (!m_world->unit(h.unit, unit)) {
            continue;
        }
        const bool player = involvesPlayer(h.unit, h.target);
        const bool wanted = player || near(unit.feet, m_settings.holdDistance);
        if (wanted && !h.shown) {
            const game::VisualKit* kit = m_data->kit(h.kit);
            if (kit && kit->modelCount > 0 && budget(player)) {
                spawnKit(*kit, unit, false, &h.models);
            }
            h.shown = true;
            startHoldBeams(static_cast<u32>(&h - m_holds.data()), nowMs); // phase 14
        } else if (!wanted && h.shown) {
            stopModels(h.models);
            stopHoldBeams(static_cast<u32>(&h - m_holds.data()));
            h.shown = false;
        }
        keepLoop(unit, h.loop);
    }

    // Aura states, the same way.
    for (Aura& a : m_auras) {
        if (!a.active) {
            continue;
        }
        ++m_stats.auras;
        SpellVisualWorld::Unit unit;
        if (!m_world->unit(a.unit, unit)) {
            continue;
        }
        const bool player = involvesPlayer(a.unit, 0);
        const bool wanted = player || near(unit.feet, m_settings.holdDistance);
        if (wanted && !a.shown) {
            const game::VisualKit* kit = m_data->kit(a.kit);
            if (kit && kit->modelCount > 0 && budget(player)) {
                spawnKit(*kit, unit, false, &a.models);
            }
            a.shown = true;
        } else if (!wanted && a.shown) {
            stopModels(a.models);
            a.shown = false;
        }
        m_stats.shownAuras += a.shown ? 1 : 0;
        keepLoop(unit, a.loop);
    }

    // Ground areas: the area model falls at random points of the radius, one after another (a few alive at
    // once).
    for (Area& a : m_areas) {
        if (!a.active) {
            continue;
        }
        if (reached(nowMs, a.seenMs + kAreaTimeoutMs)) {
            endArea(a);
            continue;
        }
        ++m_stats.areas;
        if (!reached(nowMs, a.nextMs) || !near(a.center, m_settings.areaDistance + a.radius)) {
            continue;
        }
        // Wider areas rain faster: about one model every 300 ms over 8 yards.
        a.nextMs = nowMs + std::clamp<u32>(static_cast<u32>(2400.0f / std::max(1.0f, a.radius)), 120, 600);
        const u32 slot = a.spawned++ % std::min<u32>(m_settings.maxAreaCopies, kAreaCopies);
        const game::SpellVisualInfo* visual = m_data->spellVisual(a.spell);
        if (!visual || m_world->alive(a.models[slot]) || !budget(false)) {
            continue; // as many alive as allowed already
        }
        const auto next = [&]() {
            a.random ^= a.random << 13;
            a.random ^= a.random >> 17;
            a.random ^= a.random << 5;
            return static_cast<f64>(a.random & 0xFFFF) / 65536.0;
        };
        const f64 r = static_cast<f64>(a.radius) * 0.85 * std::sqrt(next()); // uniform over the disc
        const f64 angle = next() * 6.2831853;
        const glm::dvec3 at = a.center + glm::dvec3(std::cos(angle) * r, std::sin(angle) * r, 0.0);
        a.models[slot] =
            m_world->spawnAt(visual->areaModel, at, static_cast<f32>(next() * 6.2831853), Effects::kOnce);
    }
    updateBeams(nowMs); // phase 14
    updateTints(nowMs);
    m_stats.effects = m_world->liveEffects();
    m_stats.updateMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
}

} // namespace vd
