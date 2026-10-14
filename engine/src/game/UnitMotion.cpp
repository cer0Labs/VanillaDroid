#include "game/UnitMotion.h"

#include "anim/Animator.h"
#include "db/DbcView.h"
#include "game/Entities.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <algorithm>

namespace vd::game {
namespace {

namespace uf = net::uf;
namespace animation = anim::animation;

// MOVEFLAG_* (MovementInfo.h).
constexpr u32 kMoveForward = 0x1;
constexpr u32 kMoveBackward = 0x2;
constexpr u32 kMoveStrafeLeft = 0x4;
constexpr u32 kMoveStrafeRight = 0x8;
constexpr u32 kMoveTurns = 0x10 | 0x20;
constexpr u32 kMoveDirections = kMoveForward | kMoveBackward | kMoveStrafeLeft | kMoveStrafeRight;
constexpr u32 kMoveWalkMode = 0x100;
constexpr u32 kMoveLevitating = 0x400;
constexpr u32 kMoveRoot = 0x1000;
constexpr u32 kMoveJumping = 0x2000;
constexpr u32 kMoveWaterWalking = 0x10000000;

constexpr u32 kSplineFlying = 0x200; // MoveSplineFlag::Flying: "flying animation"

// UNIT_FIELD_FLAGS that let a unit swim (UnitDefines.h; WoW.exe 0x60310B tests these three bits).
constexpr u32 kUnitFlagPlayerControlled = 0x00000008;
constexpr u32 kUnitFlagPetInCombat = 0x00000800;
constexpr u32 kUnitFlagUseSwimAnimation = 0x00008000;
constexpr u32 kUnitFlagsSwimming =
    kUnitFlagPlayerControlled | kUnitFlagPetInCombat | kUnitFlagUseSwimAnimation;
// UNIT_FIELD_BYTES_1 byte 3: UNIT_VIS_FLAGS_CREEP (SPELL_AURA_MOD_STEALTH).
constexpr u32 kVisFlagCreep = 0x02;

constexpr f32 kSprintSpeed = 11.0f;            // [0x80C484]
constexpr f32 kSwimDepthFactor = 0.75f;        // [0x8012CC]
constexpr f32 kSwimDepthOffset = 1.0f / 36.0f; // [0x7FF9D0]

// CreatureDisplayInfo.dbc (5875): 0 id, 1 model (CreatureModelData). CreatureModelData.dbc: 0 id, 15
// collision height (app/Game's player shape reads the same columns).
constexpr u32 kDisplayModelField = 1;
constexpr u32 kModelCollisionHeightField = 15;

} // namespace

f32 currentSpeed(const Entity& e, u32 nowMs, bool swimming) {
    if (e.onSpline(nowMs)) {
        return e.speedAt(nowMs); // the spline's length over its duration
    }
    const u32 flags = e.moveFlags;
    if ((flags & kMoveDirections) == 0 || (flags & kMoveRoot)) {
        return 0.0f;
    }
    if (swimming) {
        const f32 swim = e.speed(SpeedType::Swim);
        return (flags & kMoveBackward) ? std::min(e.speed(SpeedType::SwimBack), swim) : swim;
    }
    const f32 run = e.speed(SpeedType::Run);
    if (flags & kMoveWalkMode) {
        return std::min(e.speed(SpeedType::Walk), run);
    }
    return (flags & kMoveBackward) ? std::min(e.speed(SpeedType::RunBack), run) : run;
}

MotionAnimation movementAnimation(const Entity& e, u32 nowMs, bool swimming) {
    MotionAnimation out;
    const bool spline = e.onSpline(nowMs);
    const u32 flags = e.moveFlags;
    out.speed = currentSpeed(e, nowMs, swimming);
    out.moving = out.speed > 0.1f && (spline || (flags & kMoveDirections) != 0);
    if (!out.moving) {
        out.speed = 0.0f;
        out.animation = swimming ? animation::kSwimIdle : animation::kStand;
        return out;
    }
    // A unit on a server spline moves forward along it (the server sets MOVEFLAG_FORWARD with the spline).
    const u32 directions = spline ? kMoveForward : flags;
    if (swimming) {
        if ((directions & (kMoveStrafeLeft | kMoveStrafeRight)) == 0 && (flags & kMoveTurns) == 0) {
            out.animation = (directions & kMoveBackward) ? animation::kSwimBackwards : animation::kSwim;
        } else if (flags & kMoveTurns) {
            out.animation = animation::kSwimIdle;
        } else {
            out.animation = (directions & kMoveStrafeLeft) ? animation::kSwimLeft : animation::kSwimRight;
        }
        return out;
    }
    if (spline && (e.splineFlags & kSplineFlying)) {
        out.animation = animation::kFly;
    } else if (directions & kMoveBackward) {
        out.animation = animation::kWalkBackwards;
    } else if (e.byteValue(uf::UNIT_FIELD_BYTES_1, 3) & kVisFlagCreep) {
        out.animation = animation::kStealthWalk;
    } else if (out.speed >= kSprintSpeed) {
        out.animation = animation::kSprint;
    } else if (out.speed > 2.0f * e.speed(SpeedType::Walk)) {
        out.animation = animation::kRun;
    } else {
        out.animation = animation::kWalk;
    }
    return out;
}

f32 swimDepth(f32 collisionHeight) {
    return collisionHeight * kSwimDepthFactor - kSwimDepthOffset;
}

bool unitSwims(const Entity& e, bool swimmingNow, f32 depth, f32 collisionHeight) {
    if (e.moveFlags & kMoveLevitating) {
        return swimmingNow;
    }
    const bool deep = depth > swimDepth(collisionHeight);
    if (swimmingNow) {
        return deep;
    }
    return deep && (e.value(uf::UNIT_FIELD_FLAGS) & kUnitFlagsSwimming) != 0 &&
           (e.moveFlags & (kMoveWaterWalking | kMoveJumping)) == 0;
}

void UnitShapes::init(const pack::PackReader& db) {
    m_heights.clear();
    const std::optional<pack::PackAsset> displays = db.find("DBFilesClient\\CreatureDisplayInfo.dbc");
    const std::optional<pack::PackAsset> models = db.find("DBFilesClient\\CreatureModelData.dbc");
    if (!displays || !models) {
        return;
    }
    Result<db::DbcView> displayTable = db::DbcView::parse(displays->data, displays->size);
    Result<db::DbcView> modelTable = db::DbcView::parse(models->data, models->size);
    if (!displayTable || !modelTable || modelTable.value().fieldCount() <= kModelCollisionHeightField ||
        displayTable.value().fieldCount() <= kDisplayModelField) {
        return;
    }
    std::unordered_map<u32, f32> modelHeights;
    modelHeights.reserve(modelTable.value().recordCount());
    for (u32 m = 0; m < modelTable.value().recordCount(); ++m) {
        modelHeights.emplace(modelTable.value().getU32(m, 0),
                             modelTable.value().getF32(m, kModelCollisionHeightField));
    }
    m_heights.reserve(displayTable.value().recordCount());
    for (u32 r = 0; r < displayTable.value().recordCount(); ++r) {
        const auto model = modelHeights.find(displayTable.value().getU32(r, kDisplayModelField));
        if (model != modelHeights.end() && model->second > 0.0f) {
            m_heights.emplace(displayTable.value().getU32(r, 0), model->second);
        }
    }
}

f32 UnitShapes::modelHeight(u32 displayId) const {
    const auto found = m_heights.find(displayId);
    return found == m_heights.end() ? 0.0f : found->second;
}

} // namespace vd::game
