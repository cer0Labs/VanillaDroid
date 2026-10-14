#include "app/SpellEffects.h"

#include "app/GameSession.h"
#include "app/Units.h"
#include "core/DevEnv.h"
#include "core/Profile.h"
#include "game/Appearance.h"
#include "game/GameObjectAnim.h"
#include "game/Gameplay.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "world/Actors.h"
#include "world/Effects.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;

f32 asFloat(u32 bits) {
    f32 value = 0.0f;
    std::memcpy(&value, &bits, 4);
    return value;
}

} // namespace

void SpellEffects::init(const game::SpellVisualData* data, Effects* effects, Actors* actors,
                        const Units* units) {
    bind(effects, actors);
    m_data = data;
    m_units = units;
    m_visuals.init(data, this);
    m_visuals.trace = VD_DEV_ENV("VD_TRACE_FX") != nullptr;
    m_watched.reserve(512);
    m_gone.reserve(64);
    m_lines.reserve(16);
}

void SpellEffects::setKitFx(const game::SpellKitFx* fx, CameraShakes* shakes) {
    m_kitFx = fx;
    m_visuals.setKitFx(fx, shakes);
}

void SpellEffects::clear() {
    m_visuals.clear();
    for (auto& [guid, w] : m_watched) {
        stopEnchant(w, 0);
        stopEnchant(w, 1);
    }
    m_watched.clear();
}

void SpellEffects::update(GameSession& session, u32 nowMs, f32 dt, const glm::dvec3& camera) {
    if (!m_effects || !m_actors || !m_actors->isOpen()) {
        return;
    }
    VD_PROFILE_ZONE("SpellEffects::update");
    ++m_frame;
    const u64 player = session.playerGuid();
    if (player != m_cursorsPlayer) {
        // Another world or character: what was shown goes, the rings are read from now on.
        m_cursorsPlayer = player;
        m_fxCursor = session.play().fx().end();
        clear();
    }
    m_visuals.setPlayer(player);
    readFx(session, nowMs);
    watchUnits(session, nowMs, camera);
    m_visuals.update(nowMs, dt, camera);
}

void SpellEffects::readFx(GameSession& session, u32 nowMs) {
    while (const game::FxEvent* e = session.play().fx().next(m_fxCursor)) {
        const glm::dvec3 at(e->position[0], e->position[1], e->position[2]);
        switch (e->kind) {
        case game::FxEventKind::SpellStart:
            m_visuals.spellStart(e->guid, e->id, e->other, static_cast<u32>(std::max(0, e->value[0])), nowMs);
            break;
        case game::FxEventKind::SpellGo:
            m_visuals.spellGo(e->guid, e->id, e->other, e->hasPosition ? &at : nullptr, nowMs);
            break;
        case game::FxEventKind::SpellHit: m_visuals.spellHit(e->guid, e->id, e->other, nowMs); break;
        case game::FxEventKind::SpellInterrupted: m_visuals.spellInterrupted(e->guid, nowMs); break;
        case game::FxEventKind::SpellVisual:
        case game::FxEventKind::SpellImpact: m_visuals.playKit(e->guid, e->id, nowMs); break;
        case game::FxEventKind::ChainTarget: // phase 14: the units a channel's beams go through
            m_visuals.chainTarget(e->guid, e->id, e->other, static_cast<u32>(std::max(0, e->value[0])),
                                  static_cast<u32>(std::max(0, e->value[1])), nowMs);
            break;
        default: break; // channels come from the units' fields; sounds and emotes are others'
        }
    }
}

void SpellEffects::stopEnchant(Watched& w, u32 hand) {
    for (EffectHandle& e : w.enchantFx[hand]) {
        if (e.valid()) {
            m_effects->stop(e);
            e = {};
        }
    }
    w.enchantShown[hand] = false;
}

void SpellEffects::watchEnchants(const game::Entity& e, Watched& w, const glm::dvec3& camera) {
    constexpr u32 kStride = uf::PLAYER_VISIBLE_ITEM_2_0 - uf::PLAYER_VISIBLE_ITEM_1_0;
    glm::dvec3 feet;
    f32 yaw = 0.0f;
    const bool placed = m_units->placement(e.guid, feet, yaw);
    const glm::dvec3 d = feet - camera;
    const f64 range = m_visuals.settings().holdDistance;
    const bool near = placed && (e.guid == m_cursorsPlayer || glm::dot(d, d) < range * range);
    for (u32 hand = 0; hand < 2; ++hand) {
        // The visible item's enchantments: permanent (+1) and temporary (+2, a poison or an oil: shown
        // first).
        const u8 slot = hand == 0 ? game::kSlotMainHand : game::kSlotOffHand;
        const u16 base = static_cast<u16>(uf::PLAYER_VISIBLE_ITEM_1_0 + slot * kStride);
        std::array<u64, game::SpellVisualData::kItemVisualSlots> models{};
        u32 enchant = e.value(static_cast<u16>(base + 2));
        if (!m_data->enchantModels(enchant, models)) {
            enchant = e.value(static_cast<u16>(base + 1));
            if (!m_data->enchantModels(enchant, models)) {
                enchant = 0;
            }
        }
        if (enchant != w.enchant[hand]) {
            stopEnchant(w, hand);
            w.enchant[hand] = enchant;
        }
        if (enchant == 0) {
            continue;
        }
        if (near && !w.enchantShown[hand]) {
            const u32 actor = m_units->actorOf(e.guid);
            if (e.guid == m_cursorsPlayer || m_effects->spellCount() < m_visuals.settings().maxLiveEffects) {
                for (u32 point = 0; point < models.size(); ++point) {
                    if (models[point] != 0) {
                        w.enchantFx[hand][point] = m_effects->spawnOnItem(models[point], actor, slot, point);
                    }
                }
            }
            w.enchantShown[hand] = true;
        } else if (!near && w.enchantShown[hand]) {
            stopEnchant(w, hand);
        }
    }
}

void SpellEffects::levelUp(u64 guid) {
    static const u64 kLevelUp = Effects::effectModelHash("Spells\\LevelUp\\LevelUp.mdl");
    const u32 actor = m_units ? m_units->actorOf(guid) : ~0u;
    if (actor != ~0u) { // at its feet, following it, its animation once
        u32 attachment = game::kitAttachment(game::KitSlot::Base);
        if (!hasAttachment(actor, attachment)) {
            attachment = game::kitFallbackAttachment(game::KitSlot::Base);
        }
        m_effects->spawnOn(kLevelUp, actor, attachment, 1.0f, Effects::kOnce);
        return;
    }
    glm::dvec3 feet;
    f32 yaw = 0.0f;
    if (m_units && m_units->placement(guid, feet, yaw)) {
        m_effects->spawnAt(kLevelUp, feet, yaw, 1.0f, Effects::kOnce);
    }
}

void SpellEffects::watchUnits(GameSession& session, u32 nowMs, const glm::dvec3& camera) {
    const game::EntityStore& entities = session.entities();
    m_lines.clear();
    entities.forEach([&](const game::Entity& e) {
        if (e.type == game::ObjectType::DynamicObject) {
            // A ground spell's area while the object lasts.
            const u32 spell = e.value(uf::DYNAMICOBJECT_SPELLID);
            if (spell != 0) {
                const glm::dvec3 center(asFloat(e.value(uf::DYNAMICOBJECT_POS_X)),
                                        asFloat(e.value(uf::DYNAMICOBJECT_POS_Y)),
                                        asFloat(e.value(uf::DYNAMICOBJECT_POS_Z)));
                m_visuals.area(e.guid, spell, center, asFloat(e.value(uf::DYNAMICOBJECT_RADIUS)), nowMs);
            }
            return;
        }
        if (!game::isUnit(e)) {
            return;
        }
        Watched& w = m_watched[e.guid];
        w.frame = m_frame;
        // Its channel (any unit's: MSG_CHANNEL_START is only the player's).
        const u32 channel = e.value(uf::UNIT_CHANNEL_SPELL);
        const u64 object =
            static_cast<u64>(e.value(uf::UNIT_FIELD_CHANNEL_OBJECT)) |
            (static_cast<u64>(e.value(static_cast<u16>(uf::UNIT_FIELD_CHANNEL_OBJECT + 1))) << 32);
        const game::Entity* target = object != 0 ? entities.find(object) : nullptr;
        const u64 channelTarget = target && game::isUnit(*target) && object != e.guid ? object : 0;
        if (channel != w.channel || channelTarget != w.channelTarget) {
            m_visuals.channel(e.guid, channel, channelTarget, nowMs);
            w.channel = channel;
            w.channelTarget = channelTarget;
        }
        // Phase 14: Fishing's line to its bobber (WoW.exe 0x5FC6C5: a channel at a fishing node, its kit's
        // line).
        if (channel != 0 && target && target->type == game::ObjectType::GameObject && m_kitFx &&
            game::gameObjectType(*target) == game::kGameObjectFishingNode) {
            const game::SpellVisualInfo* visual = m_data->spellVisual(channel);
            const game::KitFx* fx = visual ? m_kitFx->kit(visual->channelKit) : nullptr;
            const u32 actor = m_units ? m_units->actorOf(e.guid) : ~0u;
            if (fx && fx->line && actor != ~0u && m_lines.size() < m_lines.capacity()) {
                m_lines.push_back({actor, object});
            }
        }
        // Its auras: their state kits come and go with them.
        for (u32 slot = 0; slot < game::kMaxAuras; ++slot) {
            const u32 spell = e.value(static_cast<u16>(uf::UNIT_FIELD_AURA + slot));
            if (spell == w.auras[slot]) {
                continue;
            }
            if (w.auras[slot] != 0) {
                m_visuals.auraRemoved(e.guid, w.auras[slot]);
            }
            if (spell != 0) {
                m_visuals.auraAdded(e.guid, spell);
            }
            w.auras[slot] = spell;
        }
        // The level up glow (SpellVisualEffectName 21, "HARDCODED Unit Level Up": no kit plays it, the client does
        // when a unit it already knew gains a level; vMaNGOS only raises UNIT_FIELD_LEVEL, for the player, the
        // players around and a tamed pet, which it shows one level lower first "for the levelup visual").
        const u32 level = e.value(uf::UNIT_FIELD_LEVEL);
        if (w.level != 0 && level > w.level) {
            levelUp(e.guid);
        }
        w.level = level;
        if (e.type == game::ObjectType::Player) {
            watchEnchants(e, w, camera);
        }
        // Parity: the client's sparkles over a corpse with loot for this player (UNIT_DYNFLAG_LOOTABLE).
        const bool lootable = game::isDead(e) && game::isLootable(e);
        if (lootable && !m_effects->alive(w.lootFx)) {
            glm::dvec3 feet;
            f32 yaw = 0.0f;
            if (m_units && m_units->placement(e.guid, feet, yaw)) {
                static const u64 kLootFx = Effects::effectModelHash("Particles\\LootFX.m2");
                w.lootFx = m_effects->spawnAt(kLootFx, feet, yaw);
            }
        } else if (!lootable && w.lootFx.valid()) {
            m_effects->stop(w.lootFx);
            w.lootFx = {};
        }
    });
    // Units that left.
    m_gone.clear();
    for (const auto& [guid, w] : m_watched) {
        if (w.frame != m_frame) {
            m_gone.push_back(guid);
        }
    }
    for (u64 guid : m_gone) {
        m_visuals.unitGone(guid);
        Watched& w = m_watched[guid];
        stopEnchant(w, 0);
        stopEnchant(w, 1);
        if (w.lootFx.valid()) {
            m_effects->stop(w.lootFx);
        }
        m_watched.erase(guid);
    }
}

// --- SpellVisualWorld --------------------------------------------------------------------------------------

bool SpellEffects::unit(u64 key, Unit& out) const {
    if (!m_units || !m_units->placement(key, out.feet, out.yaw)) {
        return false;
    }
    out.actor = m_units->actorOf(key);
    out.height = heightOf(out.actor, out.feet);
    const game::UnitGear* gear = m_units->gear(key);
    out.ranged = gear ? gear->ranged : game::WeaponKind::None; // phase 14: the precast's load
    return true;
}

} // namespace vd
