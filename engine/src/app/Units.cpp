#include "app/Units.h"

#include "anim/Animator.h"
#include "app/GameSession.h"
#include "core/Clock.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "db/DbcView.h"
#include "game/Appearance.h"
#include "game/CombatAnimation.h"
#include "game/UnitFade.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"
#include "world/Actors.h"
#include "world/UnitBodyEffects.h"

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;
using game::ObjectType;

constexpr f32 kPi = 3.14159265f;
constexpr f32 kTurnRate = 12.0f; // radians per second
constexpr u32 kVisibleItemStride = uf::PLAYER_VISIBLE_ITEM_2_0 - uf::PLAYER_VISIBLE_ITEM_1_0;
constexpr u32 kPlayerFlagHideHelm = 0x400;  // PLAYER_FLAGS_HIDE_HELM
constexpr u32 kPlayerFlagHideCloak = 0x800; // PLAYER_FLAGS_HIDE_CLOAK
constexpr u32 kHeadSlot = 0, kBackSlot = 14; // EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_BACK
constexpr u32 kSheathMelee = 1;   // UNIT_FIELD_BYTES_2 byte 0 (SheathState)
constexpr u32 kSheathRanged = 2;  // phase 13: the ranged weapon in the hands
constexpr u32 kSwimming = 0x00200000;
constexpr u32 kFallingFar = 0x00004000;

constexpr u32 kInCombat = 0x00080000; // UNIT_FLAG_IN_COMBAT (UnitDefines.h)
// UNIT_FLAG_STUNNED | UNIT_FLAG_CONFUSED | UNIT_FLAG_FLEEING (UnitDefines.h): it does not turn to its victim.
constexpr u32 kCannotTurn = 0x00040000 | 0x00400000 | 0x00800000;

f32 asFloat(u32 bits) {
    f32 value = 0.0f;
    std::memcpy(&value, &bits, 4);
    return value;
}

// Phase 18 (motion): the update-mask words of the fields a look is built from (the change mask: a unit whose
// fields there did not change keeps its look without reading them).
constexpr u64 lookWords(bool player) {
    using game::EntityStore;
    u64 words = EntityStore::wordOf(uf::UNIT_FIELD_DISPLAYID) | EntityStore::wordOf(uf::UNIT_FIELD_NATIVEDISPLAYID) |
                EntityStore::wordOf(uf::UNIT_FIELD_BYTES_0) | EntityStore::wordOf(uf::UNIT_FIELD_BYTES_2);
    for (u32 i = 0; i < 3; ++i) {
        words |= EntityStore::wordOf(static_cast<u16>(uf::UNIT_VIRTUAL_ITEM_SLOT_DISPLAY + i));
    }
    for (u32 i = 0; i < 6; ++i) {
        words |= EntityStore::wordOf(static_cast<u16>(uf::UNIT_VIRTUAL_ITEM_INFO + i));
    }
    if (player) {
        words |= EntityStore::wordOf(uf::PLAYER_BYTES) | EntityStore::wordOf(uf::PLAYER_BYTES_2) |
                 EntityStore::wordOf(uf::PLAYER_FLAGS) | EntityStore::wordOf(uf::PLAYER_GUILDID);
        for (u32 slot = 0; slot < game::kEquipSlots; ++slot) {
            words |= EntityStore::wordOf(static_cast<u16>(uf::PLAYER_VISIBLE_ITEM_1_0 + slot * kVisibleItemStride));
        }
    }
    return words;
}
constexpr u64 kUnitLookWords = lookWords(false);
constexpr u64 kPlayerLookWords = lookWords(true);
// The visible auras (UNIT_FIELD_AURA .. UNIT_FIELD_AURAFLAGS - 1): what game::UnitFades::ofUnit reads.
constexpr u64 auraWords() {
    u64 words = 0;
    for (u16 field = uf::UNIT_FIELD_AURA; field < uf::UNIT_FIELD_AURAFLAGS; ++field) {
        words |= game::EntityStore::wordOf(field);
    }
    return words;
}
constexpr u64 kAuraWords = auraWords();

constexpr u32 kLiquidCheckMs = 250;      // phase 18 (motion): a unit's liquid looked at again after this...
constexpr f32 kLiquidCheckYards = 0.5f;  // ...or when it moved this far
constexpr u32 kCostWindowFrames = 300;   // the update's cost averaged over this many frames

f32 wrapAngle(f32 a) {
    while (a > kPi) {
        a -= 2.0f * kPi;
    }
    while (a < -kPi) {
        a += 2.0f * kPi;
    }
    return a;
}

} // namespace

void Units::init(const pack::PackReader& db) {
    m_shapes.init(db); // phase 18 (motion): the creatures' heights, for their swimming
    m_fullLooks = VD_DEV_ENV("VD_MOTION_FULL_LOOKS") != nullptr;
    m_traceMotion = VD_DEV_ENV("VD_TRACE_MOTION") != nullptr;
    m_logCost = VD_DEV_ENV("VD_MOTION_COST") != nullptr;
    m_emoteAnimation.clear();
    const std::optional<pack::PackAsset> emotes = db.find("DBFilesClient\\Emotes.dbc");
    if (!emotes) {
        return;
    }
    Result<db::DbcView> view = db::DbcView::parse(emotes->data, emotes->size);
    if (!view || view.value().fieldCount() < 3) {
        return;
    }
    // Emotes.dbc (5875): 0 id, 1 name, 2 AnimationData id.
    for (u32 r = 0; r < view.value().recordCount(); ++r) {
        const u32 id = view.value().getU32(r, 0);
        if (id < 4096) {
            if (m_emoteAnimation.size() <= id) {
                m_emoteAnimation.resize(id + 1, 0);
            }
            m_emoteAnimation[id] = static_cast<u16>(view.value().getU32(r, 2));
        }
    }
}

u32 Units::actorOf(u64 guid) const {
    const auto it = m_units.find(guid);
    return it == m_units.end() ? ~0u : it->second.actor;
}

const game::UnitGear* Units::gear(u64 guid) const {
    const auto it = m_units.find(guid);
    return it == m_units.end() ? nullptr : &it->second.gear;
}

bool Units::placement(u64 guid, glm::dvec3& feet, f32& yaw) const {
    const auto it = m_units.find(guid);
    if (it == m_units.end() || it->second.actor == ~0u) {
        return false;
    }
    feet = it->second.feet;
    yaw = it->second.yaw;
    return true;
}

f32 Units::selectionHalfSize(u64 guid, const Actors& actors) const {
    const auto it = m_units.find(guid);
    if (it == m_units.end()) {
        return 0.0f;
    }
    const Unit& unit = it->second;
    return actors.selectionHalfSize(unit.mount != ~0u ? unit.mount : unit.actor) * unit.scale;
}

bool Units::hasModel(u64 guid) const {
    const auto it = m_units.find(guid);
    return it != m_units.end() && it->second.drawn;
}

void Units::clear(Actors& actors) {
    for (auto& [guid, unit] : m_units) {
        actors.destroy(unit.actor);
        if (unit.mount != ~0u) {
            actors.destroy(unit.mount);
        }
    }
    m_units.clear();
}

void Units::update(GameSession& session, Actors& actors, u32 nowMs, f32 dt, GroundQuery ground,
                   const void* groundContext) {
    if (!actors.isOpen()) {
        return;
    }
    VD_PROFILE_ZONE("Units::update");
    const Clock::time_point costStart = Clock::now();
    ++m_frame;
    m_pendingLooks = 0;
    const game::AppearanceDb& db = actors.appearance();
    // Phase 18 (motion): the change mask. Right after the last update, only the units whose look fields changed in
    // this frame (or whose look still waits for an answer) read them again; after a gap, everyone does.
    const game::EntityStore& entities = session.entities();
    const u32 storeFrame = entities.frame();
    const bool continuous = storeFrame == m_storeFrame + 1 && !m_fullLooks;
    m_storeFrame = storeFrame;
    u32 looksRead = 0, looksRebuilt = 0, unitCount = 0;
    entities.forEach([&](const game::Entity& e) {
        if ((e.type != ObjectType::Unit && e.type != ObjectType::Player) || !e.hasPosition) {
            return;
        }
        ++unitCount;
        Unit& unit = m_units[e.guid];
        unit.seen = m_frame;
        const bool known = unit.actor != ~0u; // phase 14 (bodyfx): seen before this frame
        if (unit.actor == ~0u) {
            unit.actor = actors.create();
            actors.setBody(unit.actor, true); // phase 14 (bodyfx)
        }
        const bool player = e.type == ObjectType::Player;

        // Where its weapons are (WoW.exe 0x5FDF80): the server's state for the others whenever it changes, the
        // client's own for the player; each new animation (its action, else its base one, as last chosen) may put
        // them away (a spell's) or draw them (an armed swing), and the player tells the server.
        const bool local = e.guid == session.playerGuid();
        game::Gameplay& play = session.play();
        const u8 sheathField = e.byteValue(uf::UNIT_FIELD_BYTES_2, 0);
        if (sheathField != unit.sheathField) {
            unit.sheathField = sheathField;
            unit.sheath = sheathField;
        }
        if (known && actors.ready(unit.actor)) {
            const u16 action = actors.action(unit.actor);
            const u16 animation = action != Actors::kNoAction ? action : actors.playing(unit.actor);
            if (animation != unit.sheathAnimation && animation != Actors::kNoAction) {
                unit.sheathAnimation = animation;
                game::SheathInput in;
                in.current = local ? play.sheath() : unit.sheath;
                in.animation = animation;
                in.weaponFlags = actors.fallbacks().weaponFlagsOf(animation);
                in.mounted = e.value(uf::UNIT_FIELD_MOUNTDISPLAYID) != 0;
                in.meleeAttacking = local ? play.melee().attacking() : unit.attacking;
                in.local = local;
                in.field = sheathField;
                const u8 next = game::sheathAfterAnimation(in);
                if (!local) {
                    unit.sheath = next;
                } else if (next != in.current) {
                    play.setSheath(next);
                }
            }
        }
        const u8 sheath = local ? play.sheath() : unit.sheath;

        // Look: rebuilt when the fields it depends on change (or an item answer it waited for arrives), and when its
        // weapons go to or from its hands.
        const bool readLook = !continuous || !known || !unit.complete || unit.lookWaiting ||
                              sheath != unit.lookSheath ||
                              e.changedIn(storeFrame, player ? kPlayerLookWords : kUnitLookWords);
        if (readLook) {
            ++looksRead;
            bool waiting = false;
            u64 key = kFnvOffset;
            auto mix = [&](u32 v) { key = fnv1a64(&v, sizeof(v), key); };
            mix(static_cast<u32>(e.type));
            mix(e.value(uf::UNIT_FIELD_DISPLAYID));
            mix(e.value(uf::UNIT_FIELD_NATIVEDISPLAYID));
            mix(e.value(uf::UNIT_FIELD_BYTES_0));
            mix(sheath); // phase 13: the ranged state too
            unit.lookSheath = sheath;
            for (u32 i = 0; i < 3; ++i) {
                mix(e.value(static_cast<u16>(uf::UNIT_VIRTUAL_ITEM_SLOT_DISPLAY + i)));
            }
            for (u32 i = 0; i < 6; ++i) {
                mix(e.value(static_cast<u16>(uf::UNIT_VIRTUAL_ITEM_INFO + i)));
            }
            if (player) {
                mix(e.value(uf::PLAYER_BYTES));
                mix(e.byteValue(uf::PLAYER_BYTES_2, 0));
                mix(e.value(uf::PLAYER_FLAGS) & (kPlayerFlagHideHelm | kPlayerFlagHideCloak)); // phase 15 (options)
                for (u32 slot = 0; slot < game::kEquipSlots; ++slot) {
                    mix(e.value(static_cast<u16>(uf::PLAYER_VISIBLE_ITEM_1_0 + slot * kVisibleItemStride)));
                }
                // Phase 15 (guild): a guild tabard shows the guild's design (asked for with the guild's name).
                if (const u32 guildId = e.value(uf::PLAYER_GUILDID); guildId != 0) {
                    game::Social& social = session.play().social();
                    social.guildName(guildId);
                    if (const game::GuildEmblem* emblem = social.guildEmblem(guildId)) {
                        for (u32 i = 0; i < game::kEmblemChoiceKinds; ++i) {
                            mix(static_cast<u32>(emblem->choice(i)));
                        }
                    } else {
                        waiting = true; // phase 18 (motion): read again until the guild's answer comes
                    }
                }
            }
            if (!unit.complete) {
                mix(session.queryVersion());
            }
            unit.lookWaiting = waiting;
            const bool sheathed = sheath != kSheathMelee;
            const bool rangedDrawn = sheath == kSheathRanged; // phase 13
            if (key != unit.lookKey) {
                ++looksRebuilt;
                unit.lookKey = key;
                unit.complete = true;
                const u32 display = e.value(uf::UNIT_FIELD_DISPLAYID);
                game::ActorLook look;
                if (player && display == e.value(uf::UNIT_FIELD_NATIVEDISPLAYID)) {
                    game::CharacterLook c;
                    c.race = e.byteValue(uf::UNIT_FIELD_BYTES_0, 0);
                    c.gender = e.byteValue(uf::UNIT_FIELD_BYTES_0, 2);
                    c.skin = e.byteValue(uf::PLAYER_BYTES, 0);
                    c.face = e.byteValue(uf::PLAYER_BYTES, 1);
                    c.hairStyle = e.byteValue(uf::PLAYER_BYTES, 2);
                    c.hairColor = e.byteValue(uf::PLAYER_BYTES, 3);
                    c.facialHair = e.byteValue(uf::PLAYER_BYTES_2, 0);
                    // Phase 15 (options): Show Helm / Show Cloak off (PLAYER_FLAGS_HIDE_HELM / _CLOAK): the server
                    // still sends the item; the client leaves it off the model.
                    const u32 flags = e.value(uf::PLAYER_FLAGS);
                    for (u32 slot = 0; slot < game::kEquipSlots; ++slot) {
                        const u32 entry =
                            e.value(static_cast<u16>(uf::PLAYER_VISIBLE_ITEM_1_0 + slot * kVisibleItemStride));
                        if (entry == 0 || (slot == kHeadSlot && (flags & kPlayerFlagHideHelm) != 0) ||
                            (slot == kBackSlot && (flags & kPlayerFlagHideCloak) != 0)) {
                            continue;
                        }
                        if (const net::ItemQueryInfo* item = session.item(entry)) {
                            c.items[slot] = item->displayId;
                            c.inventoryTypes[slot] = static_cast<u8>(item->inventoryType);
                            c.sheaths[slot] = static_cast<u8>(item->sheath);
                        } else {
                            unit.complete = false; // asked; the look is rebuilt when the answer comes
                        }
                    }
                    if (const game::GuildEmblem* emblem =
                            session.play().social().guildEmblem(e.value(uf::PLAYER_GUILDID))) { // phase 15 (guild)
                        c.emblem = *emblem;
                    }
                    look = db.character(c, sheathed, rangedDrawn);
                } else {
                    std::array<u32, 3> weapons{};
                    std::array<u8, 3> types{};
                    for (u32 i = 0; i < 3; ++i) {
                        weapons[i] = e.value(static_cast<u16>(uf::UNIT_VIRTUAL_ITEM_SLOT_DISPLAY + i));
                        types[i] = e.byteValue(static_cast<u16>(uf::UNIT_VIRTUAL_ITEM_INFO + i * 2), 3);
                    }
                    look = db.creature(display, weapons, types, sheathed, rangedDrawn);
                }
                actors.setLook(unit.actor, look);
                unit.gear = gearOf(session, e, sheath); // phase 9: swings and stances by weapon
            }
        }
        unit.drawn = actors.ready(unit.actor);
        m_pendingLooks += unit.drawn ? 0 : 1;
        // Phase 11: translucent and tinted by its auras' visual kits (a ghost, stealth, invisibility, Shadowform).
        // Phase 18 (motion): the auras' look read again only when they change (the change mask).
        if (m_fades) {
            if (!continuous || !known || e.changedIn(storeFrame, kAuraWords)) {
                unit.fadeTarget = m_fades->ofUnit(e);
            }
            unit.fade = game::approachFade(unit.fade, unit.fadeTarget, dt);
            actors.setFade(unit.actor, unit.fade);
        }

        // Mount: a second actor carrying this one.
        const u32 mountDisplay = e.value(uf::UNIT_FIELD_MOUNTDISPLAYID);
        if (mountDisplay != unit.mountDisplay) {
            unit.mountDisplay = mountDisplay;
            if (mountDisplay == 0) {
                actors.destroy(unit.mount);
                unit.mount = ~0u;
                actors.setMount(unit.actor, Actors::kNone);
            } else {
                if (unit.mount == ~0u) {
                    unit.mount = actors.create();
                    actors.setBody(unit.mount, true); // phase 14 (bodyfx)
                }
                actors.setLook(unit.mount, db.creature(mountDisplay));
                actors.setMount(unit.actor, unit.mount);
                if (known && m_bodyFx && m_effects) { // phase 14 (bodyfx): mounting (not one seen mounted)
                    const glm::vec3 at = e.positionAt(nowMs);
                    m_bodyFx->mounted(*m_effects, glm::dvec3(at), unit.yaw, unit.scale);
                }
            }
        }

        // Placement: facing along its spline while on it, then its final facing (an angle, or whoever it faces);
        // else the server's orientation. Turned smoothly. Phase 18 (motion): a passenger's place and facing turn
        // with its transport (the entity store places it on board).
        glm::vec3 position = e.positionAt(nowMs);
        // The whole scale: a 1.12 server puts the display's (CreatureDisplayInfo x CreatureModelData, or the
        // creature's own) in OBJECT_FIELD_SCALE_X, and the client does not apply the display's again.
        const f32 objectScale = e.value(uf::OBJECT_FIELD_SCALE_X) ? asFloat(e.value(uf::OBJECT_FIELD_SCALE_X)) : 1.0f;
        const f32 scale = std::clamp(objectScale, 0.05f, 20.0f);
        // Phase 18 (motion): swimming is the players' own word (MSG_MOVE_START_SWIM); the server's creatures swim by
        // the client's rule, from the liquid over their feet (CGUnit_C 0x6030C0).
        bool swimming = (e.moveFlags & kSwimming) != 0;
        if (!player && m_liquidQuery) {
            const bool due = static_cast<i32>(nowMs - unit.liquidAtMs) >= static_cast<i32>(kLiquidCheckMs) ||
                             glm::length(position - unit.liquidFrom) > kLiquidCheckYards || !known;
            if (due) {
                unit.liquidAtMs = nowMs;
                unit.liquidFrom = position;
                const std::optional<f32> level = m_liquidQuery(m_liquidContext, position.x, position.y, position.z);
                unit.depth = level ? std::max(0.0f, *level - position.z) : 0.0f;
                f32 height = m_shapes.modelHeight(e.value(uf::UNIT_FIELD_DISPLAYID)) * scale;
                if (height <= 0.0f) {
                    height = 2.0f * scale; // a model the tables do not size: about a human's
                }
                unit.swimming = game::unitSwims(e, unit.swimming, unit.depth, height);
            }
            swimming = swimming || unit.swimming;
        }
        const game::MotionAnimation motion = game::movementAnimation(e, nowMs, swimming);
        const f32 speed = motion.speed;
        f32 targetYaw = e.facingAt(nowMs);
        f32 heading = 0.0f;
        if (e.moving) {
            if (e.headingAt(nowMs, heading)) {
                targetYaw = heading;
            } else if (e.hasFinalAngle) {
                targetYaw = e.worldAngle(e.finalAngle, nowMs);
            } else if (unit.turned) {
                targetYaw = unit.yaw; // arrived: keeps the way it went
            }
        }
        // Whom it faces when standing: the server's spline facing, else (a creature fighting) its victim, which the
        // client keeps turning to between the server's facing orders as the player circles around it.
        u64 faceGuid = e.facingTarget;
        if (faceGuid == 0 && e.type == ObjectType::Unit && (e.value(uf::UNIT_FIELD_FLAGS) & kInCombat) &&
            !(e.value(uf::UNIT_FIELD_FLAGS) & kCannotTurn) && e.value(uf::UNIT_FIELD_HEALTH) != 0) {
            faceGuid = static_cast<u64>(e.value(uf::UNIT_FIELD_TARGET)) |
                       (static_cast<u64>(e.value(static_cast<u16>(uf::UNIT_FIELD_TARGET + 1))) << 32);
        }
        if (faceGuid != 0 && faceGuid != e.guid && !(e.moving && speed > 0.0f)) {
            if (const game::Entity* target = entities.find(faceGuid); target && target->hasPosition) {
                const glm::vec3 other = target->positionAt(nowMs);
                if (glm::length(glm::vec2(other - position)) > 0.1f) {
                    targetYaw = std::atan2(other.y - position.y, other.x - position.x);
                }
            }
        }
        // A jumping or falling player is carried by its momentum between packets: never below the ground.
        if (ground && e.airborne()) {
            if (const std::optional<f32> floor = ground(groundContext, position.x, position.y, position.z)) {
                position.z = std::max(position.z, *floor);
            }
        }
        if (!unit.turned) {
            unit.yaw = targetYaw;
            unit.turned = true;
        } else {
            const f32 delta = wrapAngle(targetYaw - unit.yaw);
            const f32 step = kTurnRate * dt;
            unit.yaw = wrapAngle(unit.yaw + std::clamp(delta, -step, step));
        }
        actors.setPlacement(unit.actor, glm::dvec3(position), unit.yaw, scale);
        unit.feet = glm::dvec3(position);
        unit.scale = scale;

        // Animation from the state. Phase 18 (motion): the movement one as WoW.exe chooses it (game/UnitMotion:
        // walk or run by the speed against the walk speed, sprint, stealth, fly, the swimming ones).
        const bool dead = e.value(uf::UNIT_FIELD_HEALTH) == 0 || e.byteValue(uf::UNIT_FIELD_BYTES_1, 0) == 7;
        unit.aliveSeen = unit.aliveSeen || !dead;
        u16 movement = motion.animation;
        if (e.airborne()) {
            movement = (e.moveFlags & kFallingFar) ? anim::animation::kFall : anim::animation::kJump;
        }
        if (m_traceMotion && movement != unit.motionShown) {
            unit.motionShown = movement;
            VD_LOG_INFO("Motion %llx: animation %u at %.2f yd/s (walk speed %.2f, flags 0x%08X, spline 0x%X%s%s; path "
                        "%u points, %.1f yd in %u ms)",
                        static_cast<unsigned long long>(e.guid), movement, speed, e.speed(game::SpeedType::Walk),
                        e.moveFlags, e.onSpline(nowMs) ? e.splineFlags : 0u, swimming ? ", swimming" : "",
                        e.transportGuid != 0 ? ", aboard" : "", static_cast<u32>(e.path.size()), e.pathLength,
                        e.splineDuration);
        }
        if (unit.mount != ~0u) {
            const game::ActorLook* mountLook = actors.look(unit.mount);
            actors.setPlacement(unit.mount, glm::dvec3(position), unit.yaw,
                                std::clamp(objectScale, 0.05f, 20.0f) * (mountLook ? mountLook->scale : 1.0f));
            actors.play(unit.mount, movement, true, 1.0f, speed > 0.1f ? speed : 0.0f);
            actors.play(unit.actor, anim::animation::kMount, true, 1.0f);
        } else {
            // Phase 9: dead (falling, or a corpse found lying), moving, sitting, an NPC's emote state, the ready
            // stance in combat (by weapon; the ranged one while shooting), standing.
            game::UnitAnimState state;
            state.dead = dead;
            state.corpse = dead && !unit.aliveSeen;
            state.movement = movement;
            state.standState = e.byteValue(uf::UNIT_FIELD_BYTES_1, 0);
            if (const u32 emote = e.value(uf::UNIT_NPC_EMOTESTATE); emote != 0 && emote < m_emoteAnimation.size()) {
                state.emoteState = m_emoteAnimation[emote];
            }
            state.inCombat = (e.value(uf::UNIT_FIELD_FLAGS) & kInCombat) != 0 || unit.attacking ||
                             static_cast<i32>(unit.combatUntilMs - nowMs) > 0;
            state.ranged = static_cast<i32>(unit.rangedUntilMs - nowMs) > 0;
            state.gear = unit.gear;
            const game::BaseAnimation base = game::baseAnimation(state);
            const bool walking = base.animation != anim::animation::kJump && base.animation != anim::animation::kFall &&
                                 movement != anim::animation::kStand && base.animation == movement;
            // Ours (seated animations): what the emotes need to know (UnitCombat), and the legs a stand state keeps.
            unit.standState = static_cast<u8>(dead ? 0 : state.standState);
            unit.channeling = e.value(uf::UNIT_CHANNEL_SPELL) != 0;
            actors.play(unit.actor, base.animation, base.loop, 1.0f, walking && speed > 0.1f ? speed : 0.0f,
                        !dead && movement == anim::animation::kStand && state.standState != 0);
        }
    });
    // Phase 16 (alerts): queued at a meeting stone: "HARDCODED Meeting Stone Join" on the player (0x4CA338).
    if (const u32 joins = session.play().meetingStone().joins(); joins != m_meetingStoneJoins) {
        m_meetingStoneJoins = joins;
        glm::dvec3 feet{0.0};
        f32 yaw = 0.0f;
        if (m_bodyFx && m_effects && placement(session.playerGuid(), feet, yaw)) {
            m_bodyFx->meetingStoneJoined(*m_effects, feet, yaw, 1.0f);
        }
    }
    readCombat(session.play(), actors, session.playerGuid(), nowMs); // phase 9

    // Units the server no longer shows.
    m_gone.clear();
    for (const auto& [guid, unit] : m_units) {
        if (unit.seen != m_frame) {
            m_gone.push_back(guid);
        }
    }
    for (u64 guid : m_gone) {
        Unit& unit = m_units[guid];
        actors.destroy(unit.actor);
        if (unit.mount != ~0u) {
            actors.destroy(unit.mount);
        }
        m_units.erase(guid);
    }

    // Phase 18 (motion): what this cost, averaged over a window of frames.
    const f32 ms = static_cast<f32>(secondsBetween(costStart, Clock::now()) * 1000.0);
    m_window.totalMs += ms;
    if (ms < 2.0f) {
        m_window.steadyMs += ms;
        ++m_window.steadyFrames;
    }
    m_window.worstMs = std::max(m_window.worstMs, ms);
    m_window.units += unitCount;
    m_window.looksRead += looksRead;
    m_window.looksRebuilt += looksRebuilt;
    if (++m_window.frames == kCostWindowFrames) {
        const f32 frames = static_cast<f32>(m_window.frames);
        m_cost.meanMs = static_cast<f32>(m_window.totalMs / m_window.frames);
        m_cost.steadyMs =
            m_window.steadyFrames != 0 ? static_cast<f32>(m_window.steadyMs / m_window.steadyFrames) : m_cost.meanMs;
        m_cost.worstMs = m_window.worstMs;
        m_cost.frames = m_window.frames;
        m_cost.units = static_cast<u32>(std::lround(static_cast<f32>(m_window.units) / frames));
        m_cost.looksRead = static_cast<f32>(m_window.looksRead) / frames;
        m_cost.looksRebuilt = static_cast<f32>(m_window.looksRebuilt) / frames;
        m_window = CostWindow{};
        if (m_logCost) { // development (VD_MOTION_COST=1): online, in a city, with and without VD_MOTION_FULL_LOOKS
            VD_LOG_INFO("Units::update: %u units, mean %.3f ms (steady %.3f), worst %.3f ms | looks read %.2f, "
                        "rebuilt %.2f per frame",
                        m_cost.units, m_cost.meanMs, m_cost.steadyMs, m_cost.worstMs, m_cost.looksRead,
                        m_cost.looksRebuilt);
        }
    }
}

void Units::collectViews(const Actors& actors, std::vector<UnitView>& out) const {
    out.clear();
    for (const auto& [guid, unit] : m_units) {
        if (unit.actor == ~0u || !actors.visible(unit.actor)) {
            continue;
        }
        const std::optional<glm::dvec3> top = actors.headPosition(unit.actor);
        if (!top) {
            continue;
        }
        UnitView view;
        view.guid = guid;
        view.feet = unit.feet;
        view.head = *top;
        if (const std::optional<glm::dvec3> name = actors.attachmentPosition(unit.actor, game::attach::kName)) {
            view.head = *name;
        }
        const f64 tall = std::max(0.5, view.head.z - view.feet.z);
        view.radius = static_cast<f32>(std::clamp(tall * 0.3, 0.4, 4.0));
        out.push_back(view);
    }
}

void Units::drawNamePlates(GameSession& session, const Actors& actors, const glm::mat4& viewProj,
                           const glm::dvec3& camera, f32 width, f32 height, f32 distance) const {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    for (const auto& [guid, unit] : m_units) {
        std::optional<glm::dvec3> head = actors.attachmentPosition(unit.actor, game::attach::kName);
        if (!head) {
            head = actors.headPosition(unit.actor);
        }
        if (!head) {
            continue;
        }
        const glm::vec3 relative(*head - camera + glm::dvec3(0.0, 0.0, 0.35));
        if (glm::length(relative) > distance) {
            continue;
        }
        const glm::vec4 clip = viewProj * glm::vec4(relative, 1.0f);
        if (clip.w < 0.1f) {
            continue;
        }
        const ImVec2 at((clip.x / clip.w * 0.5f + 0.5f) * width, (clip.y / clip.w * 0.5f + 0.5f) * height);
        const game::Entity* e = session.entities().find(guid);
        if (!e) {
            continue;
        }
        const bool player = e->type == ObjectType::Player;
        const char* name = nullptr;
        if (player) {
            const std::string* found = session.playerName(guid);
            name = found ? found->c_str() : nullptr;
        } else if (const net::CreatureQueryInfo* creature = session.creature(e->value(uf::OBJECT_FIELD_ENTRY))) {
            name = creature->name.c_str();
        }
        char text[96];
        std::snprintf(text, sizeof(text), "%s (%u)", name ? name : "...", e->value(uf::UNIT_FIELD_LEVEL));
        const ImVec2 size = ImGui::CalcTextSize(text);
        const ImU32 color = player ? IM_COL32(110, 190, 255, 255) : IM_COL32(255, 225, 90, 255);
        draw->AddText(ImVec2(at.x - size.x * 0.5f + 1.0f, at.y - size.y + 1.0f), IM_COL32(0, 0, 0, 200), text);
        draw->AddText(ImVec2(at.x - size.x * 0.5f, at.y - size.y), color, text);
        const u32 maxHealth = e->value(uf::UNIT_FIELD_MAXHEALTH);
        if (maxHealth > 0) {
            const f32 fraction = std::clamp(static_cast<f32>(e->value(uf::UNIT_FIELD_HEALTH)) / static_cast<f32>(maxHealth),
                                            0.0f, 1.0f);
            const f32 barWidth = std::max(size.x, 60.0f);
            const ImVec2 min(at.x - barWidth * 0.5f, at.y + 2.0f);
            const ImVec2 max(at.x + barWidth * 0.5f, at.y + 7.0f);
            draw->AddRectFilled(min, max, IM_COL32(20, 20, 20, 190));
            draw->AddRectFilled(min, ImVec2(min.x + (max.x - min.x) * fraction, max.y), IM_COL32(40, 200, 60, 230));
        }
    }
}

} // namespace vd
