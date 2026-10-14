#include "physics/CharacterController.h"

#include "core/Profile.h"
#include "physics/CollisionWorld.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>

namespace vd::physics {
namespace {

constexpr f32 kMaxStep = 1.0f / 60.0f; // longest simulation step
constexpr u32 kMaxSteps = 8;
constexpr f32 kSkin = 0.01f;           // gap kept from the surfaces touched
constexpr u32 kSlideIterations = 4;
constexpr f32 kGroundProbe = 0.05f;    // below the step height: still "on the ground"
constexpr f32 kPi = 3.14159265f;
constexpr u32 kWedgedSteps = 10;       // steps held still in the air before standing there
constexpr f32 kSlideMargin = 10.0f;    // degrees past the climbing limit before a character slides down
constexpr u32 kPersistentFlags =
    moveflag::kRoot | moveflag::kWalkMode | moveflag::kSafeFall | moveflag::kWaterWalking | moveflag::kHover;

} // namespace

CharacterController::CharacterController() : m_set(2048) {}

void CharacterController::setShape(f32 collisionRadius, f32 collisionHeight) {
    radius = collisionRadius;
    height = collisionHeight;
    // The client's step rise (reverse-engineered WoW 1.12.1, step-up resolver 0x636100): max(collision width *
    // tan 50, radius + 1/720). A human male (width 0.611) steps up 0.73 yd: the flower bed by Stormwind's Onyxia
    // gate, 0.39 to 0.72 yd high along its border, is walked into anywhere (a fixed 0.6 only let it at one end).
    stepHeight = std::max(2.0f * radius * std::tan(maxSlopeDegrees * kPi / 180.0f), radius + 1.0f / 720.0f);
}

void CharacterController::place(const glm::dvec3& position, f32 facing) {
    m_position = position;
    m_facing = facing;
    m_mode = MotionMode::Ground;
    m_velocity = glm::vec3(0.0f);
    m_airTime = 0.0f;
    m_stillInAir = 0;
    m_fallStartZ = static_cast<f32>(position.z);
    m_jump = Jump{};
    m_events = Events{};
    m_flags &= kPersistentFlags;
}

void CharacterController::carry(const glm::dvec3& delta, f32 turn) {
    m_position += delta;
    m_fallStartZ += static_cast<f32>(delta.z);
    m_facing += turn;
}

void CharacterController::launch(f32 cosAngle, f32 sinAngle, f32 speedXY, f32 speedZ) {
    m_mode = MotionMode::Air;
    m_velocity = glm::vec3(cosAngle * speedXY, sinAngle * speedXY, -speedZ);
    m_airTime = 0.0f;
    m_fallStartZ = static_cast<f32>(m_position.z);
    m_jump = Jump{speedZ, cosAngle, sinAngle, speedXY};
    m_flags = (m_flags & ~moveflag::kSwimming) | moveflag::kJumping;
}

void CharacterController::setRooted(bool rooted) {
    if (rooted) {
        m_flags = (m_flags & ~moveflag::kDirections) | moveflag::kRoot;
    } else {
        m_flags &= ~moveflag::kRoot;
    }
}

void CharacterController::setAbility(u32 flag, bool on) {
    m_flags = on ? (m_flags | flag) : (m_flags & ~flag);
}

f32 CharacterController::speed() const {
    return m_mode == MotionMode::Air ? m_jump.xySpeed : m_moveSpeed;
}

void CharacterController::updateFlags(const CharacterInput& input) {
    const bool forward = input.forward && !input.backward && !rooted();
    const bool backward = input.backward && !input.forward && !rooted();
    const bool left = input.strafeLeft && !input.strafeRight && !rooted();
    const bool right = input.strafeRight && !input.strafeLeft && !rooted();
    m_flags &= ~(moveflag::kDirections | moveflag::kWalkMode);
    m_flags |= (forward ? moveflag::kForward : 0) | (backward ? moveflag::kBackward : 0) |
               (left ? moveflag::kStrafeLeft : 0) | (right ? moveflag::kStrafeRight : 0) |
               (input.walk ? moveflag::kWalkMode : 0);
    f32 angle = 0.0f;
    bool moving = true;
    if (forward) {
        angle = left ? kPi * 0.25f : right ? -kPi * 0.25f : 0.0f;
    } else if (backward) {
        angle = left ? kPi * 0.75f : right ? -kPi * 0.75f : kPi;
    } else if (left) {
        angle = kPi * 0.5f;
    } else if (right) {
        angle = -kPi * 0.5f;
    } else {
        moving = false;
    }
    if (!moving) {
        m_moveDirection = glm::vec3(0.0f);
        m_moveSpeed = 0.0f;
        return;
    }
    const f32 heading = m_facing + angle;
    m_moveDirection = glm::vec3(std::cos(heading), std::sin(heading), 0.0f);
    // Unit::GetSpeedForMovementInfo: what the server extrapolates with.
    if (m_mode == MotionMode::Swim) {
        m_moveSpeed = backward ? speeds.swimBack : speeds.swim;
    } else if (input.walk) {
        m_moveSpeed = speeds.walk;
    } else {
        m_moveSpeed = backward ? speeds.runBack : speeds.run;
    }
}

void CharacterController::gatherAround(const CollisionWorld& world, f32 dt) {
    const f32 horizontal = (m_moveSpeed + glm::length(glm::vec2(m_velocity))) * dt + radius + 0.5f;
    const f32 vertical = std::abs(m_velocity.z) * dt + m_moveSpeed * dt;
    const glm::dvec3 min = m_position - glm::dvec3(horizontal, horizontal, stepHeight + kGroundProbe + vertical + 0.5);
    const glm::dvec3 max = m_position + glm::dvec3(horizontal, horizontal, height + stepHeight + vertical + 0.5);
    m_set.reset(m_position);
    world.gather(min, max, m_set);
}

std::optional<f32> CharacterController::groundBelow(const glm::vec3& p, f32 up, f32 down, glm::vec3* normal) const {
    const f32 standCos = std::cos((maxSlopeDegrees + kSlideMargin) * kPi / 180.0f);
    const glm::vec3 ray(0.0f, 0.0f, -(up + down));
    // Under the axis first: where the feet are.
    Hit hit;
    std::optional<f32> axis;
    glm::vec3 axisNormal{0.0f, 0.0f, 1.0f};
    if (raycast(m_set, p + glm::vec3(0.0f, 0.0f, up), ray, hit)) {
        axis = p.z + up - hit.t * (up + down);
        axisNormal = hit.normal;
        if (axisNormal.z >= standCos) {
            if (normal) {
                *normal = axisNormal;
            }
            return axis;
        }
    }
    // Steep or nothing under the axis: the feet still stand on whatever holds them within their width (the edge
    // of a stair tread next to its riser, a step of rough ground). The highest such support.
    static constexpr f32 kOffsets[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f},
                                           {-0.7f, -0.7f}};
    const f32 reach = radius * 0.6f;
    std::optional<f32> best;
    glm::vec3 bestNormal{0.0f, 0.0f, 1.0f};
    for (const auto& offset : kOffsets) {
        const glm::vec3 from = p + glm::vec3(offset[0] * reach, offset[1] * reach, up);
        if (raycast(m_set, from, ray, hit) && hit.normal.z >= standCos) {
            const f32 found = p.z + up - hit.t * (up + down);
            if (!best || found > *best) {
                best = found;
                bestNormal = hit.normal;
            }
        }
    }
    if (best) {
        if (normal) {
            *normal = bestNormal;
        }
        return best;
    }
    if (axis && normal) {
        *normal = axisNormal;
    }
    return axis;
}

void CharacterController::slideBody(glm::vec3& p, glm::vec3 motion, bool full, bool horizontalSlide) {
    // Spheres stacked along the capsule axis, no more than a radius apart. Without `full` the body starts at the
    // step height: whatever is lower is stepped onto by the ground probe instead.
    const f32 bottom = (full ? 0.0f : stepHeight) + radius;
    const f32 top = std::max(bottom, height - radius);
    const u32 spheres = std::min(5u, 1u + static_cast<u32>(std::ceil((top - bottom) / radius)));
    for (u32 iteration = 0; iteration < kSlideIterations; ++iteration) {
        const f32 length = glm::length(motion);
        if (length < 1e-5f) {
            return;
        }
        Hit best;
        for (u32 s = 0; s < spheres; ++s) {
            const f32 z = spheres == 1 ? bottom : bottom + (top - bottom) * static_cast<f32>(s) / (spheres - 1);
            Hit hit;
            if (sweepSphere(m_set, p + glm::vec3(0.0f, 0.0f, z), radius, motion, hit) && (!best.hit || hit.t < best.t)) {
                best = hit;
            }
        }
        if (!best.hit) {
            p += motion;
            return;
        }
        const f32 travel = std::max(0.0f, best.t * length - kSkin) / length;
        p += motion * travel;
        motion *= 1.0f - travel;
        glm::vec3 n = best.normal;
        if (horizontalSlide) {
            n.z = 0.0f;
        }
        const f32 nLength = glm::length(n);
        if (nLength < 1e-3f) {
            return;
        }
        n /= nLength;
        const f32 into = glm::dot(motion, n);
        if (into < 0.0f) {
            motion -= n * into;
        }
        if (horizontalSlide) {
            motion.z = 0.0f;
        }
    }
}

void CharacterController::startFalling(const glm::vec3& velocity, bool jumped) {
    m_mode = MotionMode::Air;
    m_velocity = velocity;
    m_airTime = 0.0f;
    m_stillInAir = 0;
    const glm::vec2 horizontal(velocity.x, velocity.y);
    const f32 xy = glm::length(horizontal);
    m_jump.zspeed = jumped ? -kJumpVelocity : 0.0f;
    m_jump.xySpeed = jumped ? m_moveSpeed : xy; // the exact speed: the server compares it with its own
    m_jump.cosAngle = xy > 1e-4f ? horizontal.x / xy : std::cos(m_facing);
    m_jump.sinAngle = xy > 1e-4f ? horizontal.y / xy : std::sin(m_facing);
    if (jumped) {
        m_events.jumped = true;
    } else {
        m_events.fell = true;
    }
}

void CharacterController::stepGround(glm::vec3& p, const glm::vec3& motion, f32 h) {
    const glm::vec3 start = p;
    const f32 slopeCos = std::cos(maxSlopeDegrees * kPi / 180.0f);
    const f32 slopeTan = std::tan(maxSlopeDegrees * kPi / 180.0f);
    const f32 slideCos = std::cos((maxSlopeDegrees + kSlideMargin) * kPi / 180.0f);
    // Walking up a steep face (not a step: its surface is steep too) faster than the slope limit is a wall. Across
    // or down a steep face is fine: the climb along the way is what the server's WallClimb check measures.
    auto tooSteep = [&](f32 ground, const glm::vec3& normal, const glm::vec3& at) {
        const f32 rise = ground - start.z;
        const f32 run = glm::length(glm::vec2(at.x - start.x, at.y - start.y));
        return rise > 0.01f && normal.z < slopeCos && rise > run * slopeTan;
    };
    if (glm::dot(motion, motion) > 0.0f) {
        slideBody(p, motion, false, true);
    }
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    std::optional<f32> ground = groundBelow(p, stepHeight, stepHeight + kGroundProbe, &normal);
    if (ground && glm::dot(motion, motion) > 0.0f && tooSteep(*ground, normal, p)) {
        // Slide along it from where we were.
        glm::vec3 away(normal.x, normal.y, 0.0f);
        const f32 length = glm::length(away);
        p = start;
        if (length > 1e-3f) {
            away /= length;
            glm::vec3 along = motion;
            const f32 into = glm::dot(along, away);
            if (into < 0.0f) {
                along -= away * into;
            }
            slideBody(p, along, false, true);
            ground = groundBelow(p, stepHeight, stepHeight + kGroundProbe, &normal);
            if (!ground || tooSteep(*ground, normal, p)) {
                p = start;
                ground = groundBelow(p, stepHeight, stepHeight + kGroundProbe, &normal);
            }
        }
    }
    if (ground && normal.z < slideCos && glm::dot(motion, motion) > 0.0f && *ground > start.z - 0.05f) {
        // Walking does not take a character onto a surface too steep to stand on (it would slide back and forth,
        // a fall and a landing every few frames): from standable ground it is a wall. Down onto one, it slides.
        glm::vec3 startNormal{0.0f, 0.0f, 1.0f};
        const std::optional<f32> startGround = groundBelow(start, stepHeight, stepHeight + kGroundProbe, &startNormal);
        if (startGround && startNormal.z >= slideCos) {
            p = start;
            ground = startGround;
            normal = startNormal;
        }
    }
    if (m_flags & moveflag::kWaterWalking && m_hasLiquid) {
        const f32 surface = m_liquidLevel - static_cast<f32>(m_set.origin().z);
        if (surface <= p.z + stepHeight && (!ground || surface > *ground)) {
            ground = surface;
            normal = glm::vec3(0.0f, 0.0f, 1.0f);
        }
    }
    if (!ground) {
        startFalling((p - start) / h, false); // off a ledge, with the momentum it had
        m_fallStartZ = static_cast<f32>(m_set.origin().z) + p.z;
        return;
    }
    if (normal.z < slideCos && canSlide(glm::vec3(p.x, p.y, *ground), normal)) {
        // Standing on (or stepping down onto) a slope too steep to stand on: it slides down, falling.
        startFalling(glm::vec3((p.x - start.x) / h, (p.y - start.y) / h, 0.0f), false);
        m_fallStartZ = static_cast<f32>(m_set.origin().z) + p.z;
        return;
    }
    p.z = *ground;
}

bool CharacterController::canSlide(const glm::vec3& feet, const glm::vec3& normal) const {
    // Wedged between steep faces (a crease, a corner of rocks) there is nowhere to slide to: it stands there. The
    // same capsule as the fall, moved downhill and down a little.
    glm::vec3 downhill(normal.x, normal.y, 0.0f);
    const f32 length = glm::length(downhill);
    if (length < 1e-3f) {
        return false;
    }
    const glm::vec3 motion = downhill / length * 0.1f + glm::vec3(0.0f, 0.0f, -0.1f);
    const f32 bottom = radius;
    const f32 top = std::max(bottom, height - radius);
    const u32 spheres = std::min(5u, 1u + static_cast<u32>(std::ceil((top - bottom) / radius)));
    for (u32 s = 0; s < spheres; ++s) {
        const f32 z = spheres == 1 ? bottom : bottom + (top - bottom) * static_cast<f32>(s) / (spheres - 1);
        Hit hit;
        if (sweepSphere(m_set, feet + glm::vec3(0.0f, 0.0f, z + 0.02f), radius, motion, hit) && hit.t < 0.3f) {
            return false;
        }
    }
    return true;
}

void CharacterController::stepAir(glm::vec3& p, f32 h) {
    // Ballistic, like the client: the horizontal speed it took off with stays for the whole flight (collisions stop
    // the body, not the momentum: pressed against a fence, a jump carries it over once the feet clear the top);
    // only the vertical speed changes, with gravity and with what it lands or slides on.
    const f32 standCos = std::cos((maxSlopeDegrees + kSlideMargin) * kPi / 180.0f);
    const f32 terminal = (m_flags & moveflag::kSafeFall) ? kSafeFallVelocity : kTerminalVelocity;
    m_velocity.z = std::max(m_velocity.z - kGravity * h, -terminal);
    m_airTime += h;
    glm::vec3 motion = m_velocity * h;
    const glm::vec3 start = p;
    bool supported = false; // resting on something below
    glm::vec3 previous{0.0f}; // the face slid along last
    const f32 bottom = radius;
    const f32 top = std::max(bottom, height - radius);
    const u32 spheres = std::min(5u, 1u + static_cast<u32>(std::ceil((top - bottom) / radius)));
    for (u32 iteration = 0; iteration < kSlideIterations; ++iteration) {
        const f32 length = glm::length(motion);
        if (length < 1e-6f) {
            break;
        }
        Hit best;
        for (u32 s = 0; s < spheres; ++s) {
            const f32 z = spheres == 1 ? bottom : bottom + (top - bottom) * static_cast<f32>(s) / (spheres - 1);
            Hit hit;
            if (sweepSphere(m_set, p + glm::vec3(0.0f, 0.0f, z), radius, motion, hit) && (!best.hit || hit.t < best.t)) {
                best = hit;
            }
        }
        if (!best.hit) {
            p += motion;
            break;
        }
        const f32 travel = std::max(0.0f, best.t * length - kSkin) / length;
        p += motion * travel;
        motion *= 1.0f - travel;
        const glm::vec3 n = best.normal;
        supported |= n.z > 0.05f;
        if (n.z >= standCos && m_velocity.z <= 0.0f) {
            // Landed: feet on the surface under the axis (the sphere may touch a slope off-center). The ground step
            // judges that surface: if it is too steep to stand on (and there is room), it keeps sliding instead of
            // landing and falling again every frame.
            glm::vec3 groundNormal{0.0f, 0.0f, 1.0f};
            const std::optional<f32> ground = groundBelow(p, radius, radius + kGroundProbe, &groundNormal);
            if (!ground || groundNormal.z >= standCos || !canSlide(glm::vec3(p.x, p.y, *ground), groundNormal)) {
                if (ground) {
                    p.z = *ground;
                }
                land();
                return;
            }
        }
        if (n.z < -0.3f && m_velocity.z > 0.0f) {
            m_velocity.z = 0.0f; // head against a ceiling: the jump ends there
        }
        if (n.z > 0.05f && m_velocity.z < 0.0f) {
            // Falling onto a slope too steep to stand on: it slides down it, slower than it falls.
            m_velocity.z *= 1.0f - n.z * n.z;
        }
        const f32 into = glm::dot(motion, n);
        if (into < 0.0f) {
            motion -= n * into;
        }
        if (glm::dot(motion, previous) < 0.0f) {
            // Back into the face before (an acute corner, a crease between two faces): only along their edge is
            // free. Sliding from one face into the other used up the iterations without moving at all: a jump
            // into the corner of two walls hung there, not even falling.
            const glm::vec3 crease = glm::cross(previous, n);
            const f32 creaseLength2 = glm::dot(crease, crease);
            motion = creaseLength2 > 1e-8f ? crease * (glm::dot(motion, crease) / creaseLength2) : glm::vec3(0.0f);
        }
        previous = n;
    }
    // Held in place while falling (wedged in a crease of steep faces, or on the edge of something the momentum
    // pushes against): after a moment without moving it stands where it is.
    m_stillInAir = supported && m_velocity.z <= 0.0f && glm::length(p - start) < 0.002f ? m_stillInAir + 1 : 0;
    if (m_stillInAir >= kWedgedSteps) {
        land();
    }
}

void CharacterController::land() {
    m_mode = MotionMode::Ground;
    m_velocity = glm::vec3(0.0f);
    m_stillInAir = 0;
    m_endedAirTime = m_airTime;
    m_events.landed = true;
}

void CharacterController::stepSwim(glm::vec3& p, const glm::vec3& motion) {
    // Horizontal part with the body over the step height (it walks out onto the shore), the vertical part with
    // the whole capsule (no diving through the bottom).
    slideBody(p, glm::vec3(motion.x, motion.y, 0.0f), false, true);
    if (motion.z != 0.0f) {
        slideBody(p, glm::vec3(0.0f, 0.0f, motion.z), true, false);
    }
    // Feet never below the bottom, never above the floating depth.
    if (const std::optional<f32> ground = groundBelow(p, stepHeight, kGroundProbe)) {
        p.z = std::max(p.z, *ground);
    }
    if (m_hasLiquid) {
        const f32 floating = m_liquidLevel - static_cast<f32>(m_set.origin().z) - swimDepth();
        p.z = std::min(p.z, floating);
    }
}

void CharacterController::checkLiquid(const CollisionWorld& world, glm::vec3& p) {
    const glm::dvec3 feet = m_set.toWorld(p);
    const std::optional<LiquidSample> liquid = world.liquidAt(feet.x, feet.y, feet.z + 60.0, feet.z - 1.0);
    m_hasLiquid = liquid.has_value();
    m_liquidLevel = liquid ? liquid->level : 0.0f;
    const f32 depth = liquid ? liquid->level - static_cast<f32>(feet.z) : -1.0f;
    const bool waterWalking = (m_flags & moveflag::kWaterWalking) != 0;
    if (m_mode != MotionMode::Swim) {
        if (liquid && depth > swimDepth() && !waterWalking) {
            if (m_mode == MotionMode::Ground) {
                p.z += depth - swimDepth(); // walked in: floats at the swim depth (a fall keeps its depth)
            } else {
                m_endedAirTime = m_airTime; // a fall ends here too (START_SWIM is a fall-end opcode)
            }
            m_mode = MotionMode::Swim;
            m_velocity = glm::vec3(0.0f);
            m_events.startedSwim = true;
        }
        return;
    }
    if (!liquid) { // swam out of a liquid volume (WMO water): falls
        m_mode = MotionMode::Air;
        m_velocity = glm::vec3(0.0f);
        m_airTime = 0.0f;
        m_fallStartZ = static_cast<f32>(feet.z);
        m_jump = Jump{0.0f, std::cos(m_facing), std::sin(m_facing), 0.0f};
        m_events.stoppedSwim = true;
        return;
    }
    // Shallow enough to stand: walks out.
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    const std::optional<f32> ground = groundBelow(p, stepHeight, kGroundProbe + 0.1f, &normal);
    const f32 standCos = std::cos((maxSlopeDegrees + kSlideMargin) * kPi / 180.0f);
    if (ground && normal.z >= standCos &&
        liquid->level - (static_cast<f32>(m_set.origin().z) + *ground) < swimDepth() - 0.05f) {
        p.z = *ground;
        m_mode = MotionMode::Ground;
        m_events.stoppedSwim = true;
    }
}

void CharacterController::step(const CollisionWorld& world, const CharacterInput& input, f32 dt) {
    VD_PROFILE_ZONE("CharacterController::step");
    m_events = Events{};
    m_facing = input.facing;
    m_pitch = std::clamp(input.pitch, -1.5f, 1.5f);
    updateFlags(input);
    if (dt <= 0.0f) {
        return;
    }
    gatherAround(world, dt);
    glm::vec3 p(0.0f); // local to the gathered set (its origin is the current position)
    const u32 steps = std::clamp(static_cast<u32>(std::ceil(dt / kMaxStep)), 1u, kMaxSteps);
    const f32 h = std::min(dt, kMaxStep * kMaxSteps) / static_cast<f32>(steps);
    for (u32 i = 0; i < steps; ++i) {
        switch (m_mode) {
        case MotionMode::Ground:
            if (input.jump && !rooted()) {
                startFalling(m_moveDirection * m_moveSpeed + glm::vec3(0.0f, 0.0f, kJumpVelocity), true);
                m_fallStartZ = static_cast<f32>(m_set.origin().z) + p.z;
                stepAir(p, h);
            } else {
                stepGround(p, m_moveDirection * (m_moveSpeed * h), h);
            }
            break;
        case MotionMode::Air: stepAir(p, h); break;
        case MotionMode::Swim: {
            glm::vec3 motion = m_moveDirection * (m_moveSpeed * h);
            if (m_flags & (moveflag::kForward | moveflag::kBackward)) {
                // Forward and back follow the pitch; strafing stays level.
                const f32 sign = (m_flags & moveflag::kForward) ? 1.0f : -1.0f;
                const f32 pitchCos = std::cos(m_pitch);
                motion = glm::vec3(motion.x * pitchCos, motion.y * pitchCos, sign * std::sin(m_pitch) * m_moveSpeed * h);
            }
            stepSwim(p, motion);
            break;
        }
        }
        checkLiquid(world, p);
    }
    m_position = m_set.toWorld(p);
    m_flags &= ~(moveflag::kJumping | moveflag::kFallingFar | moveflag::kSwimming);
    if (m_mode == MotionMode::Air) {
        m_flags |= moveflag::kJumping;
        if (m_position.z < m_fallStartZ) {
            m_flags |= moveflag::kFallingFar;
        }
    } else if (m_mode == MotionMode::Swim) {
        m_flags |= moveflag::kSwimming;
    }
}

} // namespace vd::physics
