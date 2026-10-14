#include "app/Crowd.h"

#include "anim/Animator.h"
#include "core/Log.h"
#include "game/Appearance.h"
#include "game/CombatAnimation.h"
#include "world/Actors.h"
#include "world/Terrain.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

namespace vd {
namespace {

constexpr f64 kPi = 3.14159265358979323846;
constexpr u16 kEmoteDance = 69;
constexpr f32 kWalkSpeed = 2.5f; // yd/s, the race models' walk cycle
constexpr f32 kRunSpeed = 7.0f;

struct Random {
    u32 state;
    u32 next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    u32 below(u32 n) { return n ? next() % n : 0; }
    f64 unit() { return static_cast<f64>(next() & 0xFFFFFF) / static_cast<f64>(0x1000000); }
    bool chance(f64 p) { return unit() < p; }
};

u32 pick(Random& random, const std::vector<u32>& list) {
    return list.empty() ? 0 : list[random.below(static_cast<u32>(list.size()))];
}

} // namespace

game::CharacterLook Crowd::randomCharacter(const game::AppearanceDb& db, u32& state) {
    Random random{state};
    constexpr u8 kRaces[] = {1, 2, 3, 4, 5, 6, 7, 8};
    game::CharacterLook look;
    look.race = kRaces[random.below(8)];
    look.gender = static_cast<u8>(random.below(2));
    look.skin = static_cast<u8>(random.below(db.skinCount(look.race, look.gender)));
    look.face = static_cast<u8>(random.below(db.faceCount(look.race, look.gender, look.skin)));
    look.hairStyle = static_cast<u8>(random.below(db.hairStyleCount(look.race, look.gender)));
    look.hairColor = static_cast<u8>(random.below(db.hairColorCount(look.race, look.gender)));
    look.facialHair = static_cast<u8>(random.below(db.facialHairCount(look.race, look.gender)));
    // Dressed like the NPCs of the world, piece by piece.
    const struct {
        game::EquipSlot slot;
        f64 chance;
    } kPieces[] = {{game::kSlotHead, 0.4},   {game::kSlotShoulders, 0.6}, {game::kSlotShirt, 0.7},
                   {game::kSlotChest, 0.85}, {game::kSlotWaist, 0.7},     {game::kSlotLegs, 0.9},
                   {game::kSlotFeet, 0.85},  {game::kSlotWrists, 0.5},    {game::kSlotHands, 0.6},
                   {game::kSlotTabard, 0.15}};
    for (const auto& piece : kPieces) {
        if (random.chance(piece.chance)) {
            look.items[piece.slot] = pick(random, db.npcItems(piece.slot));
        }
    }
    state = random.state;
    return look;
}

u32 Crowd::randomLook(Actors& actors, u32& state) {
    const game::AppearanceDb& db = actors.appearance();
    game::CharacterLook look = randomCharacter(db, state);
    Random random{state};
    if (random.chance(0.7)) {
        look.items[game::kSlotMainHand] = pick(random, db.weaponDisplays());
    }
    if (random.chance(0.3)) {
        look.items[game::kSlotOffHand] = pick(random, db.shieldDisplays());
        look.inventoryTypes[game::kSlotOffHand] = game::kInventoryShield;
    }
    const u32 actor = actors.create();
    actors.setBody(actor, true); // phase 14 (bodyfx)
    actors.setLook(actor, db.character(look, true));
    state = random.state;
    return actor;
}

void Crowd::spawnCharacters(Actors& actors, const glm::dvec3& center, u32 count, u32 seed, f32 yaw,
                            f32 floor) {
    const game::AppearanceDb& db = actors.appearance();
    if (!db.ready()) {
        return;
    }
    Random random{seed ? seed : 1u};
    for (u32 i = 0; i < count; ++i) {
        Member m;
        m.actor = randomLook(actors, random.state);
        m.mount = ~0u;
        const u32 roll = random.below(100);
        m.behavior = roll < 40 ? Behavior::Walk
                   : roll < 60 ? Behavior::Run
                   : roll < 85 ? Behavior::Stand
                   : roll < 95 ? Behavior::Dance
                               : Behavior::Sit;
        if (count == 1) {
            m.behavior = Behavior::Stand; // a single character: standing where it was placed, for inspection
        }
        const bool moving = m.behavior == Behavior::Walk || m.behavior == Behavior::Run;
        m.center = center;
        m.radius = count == 1 ? 0.0 : 3.0 + random.unit() * 14.0;
        m.angle = random.unit() * 2.0 * kPi;
        m.speed = (m.behavior == Behavior::Run ? kRunSpeed : kWalkSpeed) * (random.chance(0.5) ? 1.0f : -1.0f);
        m.z = std::max(static_cast<f32>(center.z), floor);
        m.floor = floor;
        m.yaw = count == 1 ? yaw : static_cast<f32>(random.unit() * 2.0 * kPi);
        // Some ride (a tenth of those walking or running).
        if (moving && random.chance(0.1)) {
            constexpr u32 kMounts[] = {2402, 207, 247, 2736, 4806, 6569, 11641, 5228};
            m.mount = actors.create();
            actors.setBody(m.mount, true); // phase 14 (bodyfx)
            actors.setLook(m.mount, db.creature(kMounts[random.below(static_cast<u32>(std::size(kMounts)))]));
            actors.setMount(m.actor, m.mount);
            m.speed = (m.behavior == Behavior::Run ? 14.0f : 7.0f) * (m.speed > 0.0f ? 1.0f : -1.0f);
        }
        u16 animation = anim::animation::kStand;
        switch (m.behavior) {
        case Behavior::Walk: animation = anim::animation::kWalk; break;
        case Behavior::Run: animation = anim::animation::kRun; break;
        case Behavior::Dance: animation = kEmoteDance; break;
        case Behavior::Sit: animation = anim::animation::kSitGround; break;
        default: break;
        }
        if (m.mount != ~0u) {
            actors.play(m.actor, anim::animation::kMount, true, 1.0f);
            actors.play(m.mount, animation, true, 1.0f, std::abs(m.speed));
        } else {
            actors.play(m.actor, animation, true, 1.0f, moving ? std::abs(m.speed) : 0.0f);
        }
        m_members.push_back(m);
    }
    VD_LOG_INFO("Crowd: %u characters around (%.1f, %.1f)", count, center.x, center.y);
}

void Crowd::spawnCreature(Actors& actors, const glm::dvec3& position, f32 yaw, u32 displayId) {
    Member m{};
    m.actor = actors.create();
    actors.setBody(m.actor, true); // phase 14 (bodyfx)
    m.center = position;
    m.radius = 0.0;
    m.behavior = Behavior::Stand;
    m.z = static_cast<f32>(position.z);
    m.floor = -1.0e6f;
    m.yaw = yaw;
    m.mount = ~0u;
    actors.setLook(m.actor, actors.appearance().creature(displayId));
    actors.play(m.actor, anim::animation::kStand, true, 1.0f);
    m_members.push_back(m);
}

void Crowd::spawnArcher(Actors& actors, const glm::dvec3& position, f32 yaw, u32 kind, u32 seed, u8 race,
                        bool sheathed) {
    const game::AppearanceDb& db = actors.appearance();
    if (!db.ready()) {
        return;
    }
    // The ranged weapon by its model file (Bow_1H_..., Firearm_2H_..., Bow_2H_Crossbow_..., Wand_1H_..., Thrown_1H_...)
    // and the inventory type its item has (INVTYPE_RANGED for bows, INVTYPE_RANGEDRIGHT, INVTYPE_THROWN).
    static constexpr struct {
        const char* prefix;
        u8 inventoryType;
        game::WeaponKind weapon;
    } kKinds[] = {{"bow_1h", game::kInventoryRanged, game::WeaponKind::Bow},
                  {"firearm_", game::kInventoryRangedRight, game::WeaponKind::Gun},
                  {"bow_2h_crossbow", game::kInventoryRangedRight, game::WeaponKind::Bow},
                  {"wand_", game::kInventoryRangedRight, game::WeaponKind::Wand},
                  {"thrown_", game::kInventoryThrown, game::WeaponKind::Thrown}};
    const auto& k = kKinds[kind % std::size(kKinds)];
    Random random{seed ? seed : 1u};
    std::vector<u32> matching;
    for (const u32 display : db.weaponDisplays()) {
        std::string model(db.itemModel(display));
        std::transform(model.begin(), model.end(), model.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (model.rfind(k.prefix, 0) == 0) {
            matching.push_back(display);
        }
    }
    u32 state = random.state;
    game::CharacterLook look = randomCharacter(db, state);
    if (race != 0) {
        look.race = race;
        look.skin = look.face = look.hairStyle = look.hairColor = look.facialHair = 0;
    }
    random.state = state;
    look.items[game::kSlotRanged] = pick(random, matching);
    look.inventoryTypes[game::kSlotRanged] = k.inventoryType;
    if (sheathed) {
        look.items[game::kSlotMainHand] = pick(random, db.weaponDisplays());
    }
    Member m{};
    m.actor = actors.create();
    actors.setBody(m.actor, true); // phase 14 (bodyfx)
    m.center = position;
    m.behavior = Behavior::Stand;
    m.z = static_cast<f32>(position.z);
    m.floor = -1.0e6f;
    m.yaw = yaw;
    m.mount = ~0u;
    actors.setLook(m.actor, db.character(look, true, !sheathed));
    game::UnitGear gear;
    gear.ranged = k.weapon;
    actors.play(m.actor, sheathed ? anim::animation::kStand : game::readyAnimation(gear, true), true, 1.0f);
    m_members.push_back(m);
    VD_LOG_INFO("Crowd: an archer (%s, %zu displays of that kind) %s", k.prefix, matching.size(),
                sheathed ? "with it put away" : "with it drawn");
}

void Crowd::spawnRider(Actors& actors, const glm::dvec3& position, f32 yaw, u32 mountDisplay, u32 seed) {
    u32 state = seed ? seed : 1u;
    Member m{};
    m.actor = randomLook(actors, state);
    m.mount = actors.create();
    actors.setBody(m.mount, true); // phase 14 (bodyfx)
    m.center = position;
    m.behavior = Behavior::Stand;
    m.z = static_cast<f32>(position.z);
    m.floor = -1.0e6f;
    m.yaw = yaw;
    actors.setLook(m.mount, actors.appearance().creature(mountDisplay));
    actors.setMount(m.actor, m.mount);
    actors.play(m.actor, anim::animation::kMount, true, 1.0f);
    actors.play(m.mount, anim::animation::kStand, true, 1.0f);
    m_members.push_back(m);
}

void Crowd::clear(Actors& actors) {
    for (const Member& m : m_members) {
        actors.destroy(m.actor);
        if (m.mount != ~0u) {
            actors.destroy(m.mount);
        }
    }
    m_members.clear();
}

void Crowd::setAnimation(Actors& actors, u16 animation) {
    m_override = animation;
    for (const Member& m : m_members) {
        if (animation != 0xFFFF && m.mount == ~0u) {
            actors.play(m.actor, animation, true, 1.0f);
        }
    }
}

void Crowd::setFade(Actors& actors, const glm::vec4& fade) {
    for (const Member& m : m_members) {
        actors.setFade(m.actor, fade);
    }
}

void Crowd::update(Actors& actors, f32 dt, const Terrain* terrain) {
    for (Member& m : m_members) {
        const bool moving = (m.behavior == Behavior::Walk || m.behavior == Behavior::Run) && m_override == 0xFFFF;
        glm::dvec3 position = m.center;
        f32 yaw = m.yaw;
        if (m.radius > 0.0) {
            if (moving) {
                m.angle += static_cast<f64>(m.speed) * dt / m.radius;
            }
            position.x += std::cos(m.angle) * m.radius;
            position.y += std::sin(m.angle) * m.radius;
            // Facing along the circle.
            yaw = static_cast<f32>(m.angle + (m.speed > 0.0f ? kPi / 2.0 : -kPi / 2.0));
            if (!moving) {
                yaw = m.yaw;
            }
        }
        if (terrain) {
            if (const std::optional<f32> ground = terrain->heightAt(position.x, position.y)) {
                m.z = std::max(*ground, m.floor);
            }
        }
        position.z = m.z;
        actors.setPlacement(m.actor, position, yaw, actors.look(m.actor) ? actors.look(m.actor)->scale : 1.0f);
        if (m.mount != ~0u) {
            actors.setPlacement(m.mount, position, yaw, actors.look(m.mount) ? actors.look(m.mount)->scale : 1.0f);
        }
    }
}

} // namespace vd
