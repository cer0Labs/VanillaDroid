#include "world/PortraitCache.h"

#include "core/Hash.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

glm::vec3 vec(const f32 v[3]) {
    return glm::vec3(v[0], v[1], v[2]);
}

// The camera's axes: forward (towards the target), right and up (rolled about forward).
void cameraAxes(const pack::ActorPortrait& camera, glm::vec3& forward, glm::vec3& right, glm::vec3& up) {
    forward = vec(camera.target) - vec(camera.position);
    forward = glm::length(forward) > 1e-5f ? glm::normalize(forward) : glm::vec3(-1.0f, 0.0f, 0.0f);
    const glm::vec3 worldUp =
        std::abs(forward.z) > 0.99f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    right = glm::normalize(glm::cross(forward, worldUp));
    up = glm::cross(right, forward);
    if (camera.roll != 0.0f) {
        const f32 c = std::cos(camera.roll), s = std::sin(camera.roll);
        const glm::vec3 r = right * c + up * s;
        up = up * c - right * s;
        right = r;
    }
}

} // namespace

glm::mat4 portraitViewProjection(const pack::ActorPortrait& camera) {
    glm::vec3 forward, right, up;
    cameraAxes(camera, forward, right, up);
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f), forward, up);
    const f32 fov = std::clamp(camera.fov, 0.1f, 2.8f);
    const f32 fovY = 2.0f * std::atan(std::tan(fov * 0.5f) / std::sqrt(2.0f));
    const f32 nearClip = std::max(0.01f, camera.nearClip);
    const f32 farClip = std::max(nearClip * 2.0f, camera.farClip);
    glm::mat4 projection = glm::perspectiveRH_ZO(fovY, 1.0f, nearClip, farClip);
    projection[1][1] = -projection[1][1]; // Vulkan: clip-space y points down (as world/Camera)
    return projection * view;
}

glm::vec3 portraitKeyLight(const pack::ActorPortrait& camera) {
    glm::vec3 forward, right, up;
    cameraAxes(camera, forward, right, up);
    return glm::normalize(-forward * 0.7f - right * 0.55f + up * 0.45f);
}

// Phase 15: the model frames' lens (narrow: little perspective, like the client's model frames) and their margin.
constexpr f32 kModelViewFovY = 0.52f; // ~30 degrees
constexpr f32 kModelViewMargin = 1.06f;
constexpr f32 kModelViewFloor = 0.06f; // of the image's half height, under the feet

ModelViewCamera modelViewCamera(const glm::vec3& boundsMin, const glm::vec3& boundsMax, f32 aspect, f32 zoom) {
    ModelViewCamera camera;
    // The model turns about its origin: what it may sweep is a cylinder of the box's farthest corner.
    const f32 radius = std::sqrt(std::max({boundsMin.x * boundsMin.x, boundsMax.x * boundsMax.x}) +
                                 std::max({boundsMin.y * boundsMin.y, boundsMax.y * boundsMax.y}));
    const f32 bottom = std::min(boundsMin.z, 0.0f);
    const f32 height = std::max(boundsMax.z - bottom, 0.1f);
    const f32 tanHalf = std::tan(kModelViewFovY * 0.5f);
    // Far enough that the height and the turning width both fit, measured at the cylinder's front.
    const f32 fit = std::max(height * 0.5f / tanHalf, std::max(radius, 0.05f) / (tanHalf * aspect)) * kModelViewMargin;
    const f32 distance = fit / std::max(zoom, 0.1f) + std::max(radius, 0.05f);
    // The feet a little above the image's bottom, whatever the model's shape, measured at the front of what it sweeps
    // (a foot, a tail or a blade turned towards the camera stays in the image).
    const f32 frontHalfView = (distance - std::max(radius, 0.05f)) * tanHalf;
    camera.target = glm::vec3(0.0f, 0.0f, bottom + frontHalfView * (1.0f - kModelViewFloor));
    camera.eye = camera.target + glm::vec3(distance, 0.0f, 0.0f);
    const glm::vec3 forward(-1.0f, 0.0f, 0.0f);
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const glm::vec3 right = glm::cross(forward, up); // +Y: the viewer's right
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f), forward, up);
    const f32 nearClip = std::max(0.02f, (distance - radius) * 0.25f);
    const f32 farClip = distance + radius * 2.0f + 1.0f;
    glm::mat4 projection = glm::perspectiveRH_ZO(kModelViewFovY, aspect, nearClip, farClip);
    projection[1][1] = -projection[1][1]; // Vulkan: clip-space y points down
    camera.viewProj = projection * view;
    camera.lightDirection = glm::normalize(-forward * 0.7f - right * 0.55f + up * 0.45f);
    return camera;
}

u64 portraitLookKey(const game::ActorLook& look) {
    u64 hash = fnv1a64(&look.model, sizeof(look.model));
    hash = fnv1a64(look.textures.data(), sizeof(look.textures), hash);
    for (const game::SkinLayer& layer : look.skin) {
        hash = fnv1a64(&layer.texture, sizeof(layer.texture), hash);
        const u8 region[2] = {static_cast<u8>(layer.region), layer.order};
        hash = fnv1a64(region, sizeof(region), hash);
    }
    const u32 geosets = static_cast<u32>(look.geosets.size());
    hash = fnv1a64(&geosets, sizeof(geosets), hash);
    hash = fnv1a64(look.geosets.data(), look.geosets.size() * sizeof(u16), hash);
    for (const game::AttachedModel& a : look.attachments) {
        hash = fnv1a64(&a.attachment, sizeof(a.attachment), hash);
        hash = fnv1a64(&a.model, sizeof(a.model), hash);
        hash = fnv1a64(&a.texture, sizeof(a.texture), hash);
    }
    return hash == 0 ? 1 : hash;
}

PortraitCache::PortraitCache(u32 slots) : m_slots(slots) {}

void PortraitCache::beginFrame(u64 submittedFrames) {
    ++m_frame;
    m_order = 0;
    m_claims = 0;
    for (Slot& s : m_slots) {
        if (s.state == State::Rendering) {
            s.state = s.renderFrame <= submittedFrames ? State::Ready : State::Dirty;
        }
    }
}

u32 PortraitCache::find(u64 key) const {
    for (u32 i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i].state != State::Empty && m_slots[i].key == key) {
            return i;
        }
    }
    return kNone;
}

void PortraitCache::touch(u32 slot) {
    if (slot >= m_slots.size()) {
        return;
    }
    Slot& s = m_slots[slot];
    if (s.usedFrame != m_frame) {
        s.usedFrame = m_frame;
        s.askedOrder = m_order++;
    }
}

u32 PortraitCache::request(u64 key, u32 subject) {
    u32 slot = find(key);
    if (slot == kNone) {
        // The least recently used slot not asked for this frame (never used ones, frame 0, first).
        for (u32 i = 0; i < m_slots.size(); ++i) {
            const u32 used = m_slots[i].usedFrame;
            if (used != m_frame && (slot == kNone || used < m_slots[slot].usedFrame)) {
                slot = i;
            }
        }
        if (slot == kNone) {
            return kNone;
        }
        m_slots[slot] = Slot{};
        m_slots[slot].key = key;
        m_slots[slot].state = State::Dirty;
    }
    touch(slot);
    m_slots[slot].subject = subject;
    return slot;
}

bool PortraitCache::claim(u32 slot, u32 budget) {
    if (slot >= m_slots.size()) {
        return false;
    }
    Slot& s = m_slots[slot];
    if (s.claimedFrame == m_frame) {
        return true;
    }
    if (s.state != State::Dirty || s.usedFrame != m_frame || m_claims >= budget) {
        return false;
    }
    s.claimedFrame = m_frame;
    ++m_claims;
    return true;
}

u32 PortraitCache::dirty(std::span<u32> out) const {
    // The next one after the last picked, a few times (a handful of slots, one or two picks).
    u32 count = 0;
    while (count < out.size()) {
        u32 best = kNone;
        for (u32 i = 0; i < m_slots.size(); ++i) {
            const Slot& s = m_slots[i];
            if (s.state == State::Dirty && s.usedFrame == m_frame &&
                (count == 0 || rank(s) > rank(m_slots[out[count - 1]])) &&
                (best == kNone || rank(s) < rank(m_slots[best]))) {
                best = i;
            }
        }
        if (best == kNone) {
            break;
        }
        out[count++] = best;
    }
    return count;
}

void PortraitCache::rendered(u32 slot, u64 frame) {
    if (slot < m_slots.size() && m_slots[slot].state != State::Empty) {
        m_slots[slot].state = State::Rendering;
        m_slots[slot].renderFrame = frame;
    }
}

void PortraitCache::failed(u32 slot) {
    if (slot < m_slots.size() && m_slots[slot].state != State::Empty) {
        m_slots[slot].state = State::Failed;
    }
}

void PortraitCache::invalidate() {
    for (Slot& s : m_slots) {
        if (s.state == State::Ready || s.state == State::Rendering) {
            s.state = State::Dirty;
        }
    }
}

u32 PortraitCache::slotsInUse() const {
    return static_cast<u32>(
        std::count_if(m_slots.begin(), m_slots.end(), [](const Slot& s) { return s.state != State::Empty; }));
}

} // namespace vd
