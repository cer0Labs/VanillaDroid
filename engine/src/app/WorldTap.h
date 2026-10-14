#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"

#include <glm/vec3.hpp>

namespace vd {

// What a quick tap on the world does with the unit under the finger (parity: the client's left and right
// clicks, adapted to touch). A tap on another unit selects it (left click). A tap on the unit already
// selected acts on it like a right click: what the context button does with it (attack, talk, loot), so a
// double tap on a unit selects it and acts. A tap on nothing keeps the target (the client clears it only with
// Escape). Game objects are never selected (as in the client): a tap on one uses it at once
// (worldObjectTappable).
enum class WorldTap : u8 { None, Select, Act };

constexpr WorldTap worldTap(u64 picked, u64 target) {
    return picked == 0 ? WorldTap::None : picked == target ? WorldTap::Act : WorldTap::Select;
}

// Whether a tap that acts on `tapped` (HudOutput::unitTapped) runs the context button's action, which works
// on `contextSubject` (the target, or the object at hand when the target offers nothing): only when it is
// that unit.
constexpr bool tapRunsContext(u64 tapped, u64 contextSubject) {
    return tapped != 0 && tapped == contextSubject;
}

// A game object drawn last frame (app/ServerObjects): its model's world bounding sphere, to tap it.
struct WorldObjectView {
    u64 guid = 0;
    glm::dvec3 center{0.0};
    f32 radius = 0.0f;
};

// The game objects a right click uses in the client: the kinds the server's CMSG_GAMEOBJ_USE acts on (doors,
// buttons, quest objects, chests and the herbs and veins that are chests too, chairs, goobers, fishing
// bobbers, rituals, mailboxes, portals and other spell casters, meeting stones, flags), never one flagged
// no-interact (GO_FLAG_NO_INTERACT), and one flagged with an interaction condition (GO_FLAG_INTERACT_COND: a
// quest's) only while the server marks it active for this player (GO_DYNFLAG_LO_ACTIVATE). vMaNGOS
// GameObjectDefines.h, SpellHandler.cpp HandleGameObjectUseOpcode, GameObject::Use.
inline bool worldObjectTappable(const game::Entity& go) {
    if (go.type != game::ObjectType::GameObject) {
        return false;
    }
    constexpr u32 kFlagInteractCondition = 0x04;
    const u32 flags = go.value(net::uf::GAMEOBJECT_FLAGS);
    if ((flags & game::kGameObjectFlagNoInteract) != 0 ||
        ((flags & kFlagInteractCondition) != 0 &&
         (go.value(net::uf::GAMEOBJECT_DYN_FLAGS) & game::kGameObjectDynActivate) == 0)) {
        return false;
    }
    switch (game::gameObjectType(go)) {
    case 0:  // door
    case 1:  // button
    case 2:  // quest giver
    case 3:  // chest (herbs and mineral veins too)
    case 7:  // chair
    case 9:  // text: a plaque or sign, read by the client itself (phase 13)
    case 10: // goober
    case 13: // camera
    case 17: // fishing bobber
    case 18: // summoning ritual
    case 19: // mailbox
    case 22: // spell caster (portals, soulwells...)
    case 23: // meeting stone
    case 24: // flag stand
    case 26: // flag drop
        return true;
    default: return false;
    }
}

} // namespace vd
