#pragma once

#include "core/Types.h"
#include "game/Appearance.h"
#include "pack/ActorFormat.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <span>
#include <vector>

namespace vd {

// What a look is to a portrait: its model, textures, skin layers, geosets and what it carries — not its scale
// (the portrait frames the model with the model's own camera). Equal keys look the same; never 0.
u64 portraitLookKey(const game::ActorLook& look);

// How a portrait camera (pack::ActorPortrait, model space) sees its model: camera-relative positions (the
// camera at the origin) to Vulkan clip space (y down, depth 0..1) over a square image, the model's field of
// view being the diagonal one (tan(fov / 2) / sqrt(2) on each axis), rolled by its roll.
glm::mat4 portraitViewProjection(const pack::ActorPortrait& camera);
// The portraits' key light, towards it (model space): from the camera's front left and above.
glm::vec3 portraitKeyLight(const pack::ActorPortrait& camera);

// Phase 15: how an interface model frame (app/ModelViews) sees its model, standing at its origin facing +X (the
// model turns about +Z in front of it): a camera on +X looking straight at it, far enough for its height and the
// cylinder it sweeps while turning to fit an image of `aspect` (width / height) with a small margin, its feet near the
// image's bottom (it stands on the frame's floor), lit from the viewer's front left like the portraits. `zoom` > 1
// brings the camera closer (a wide creature then fills the frame, its ends leaving it as it turns). Camera-relative
// positions (the eye at the origin) to Vulkan clip space (y down, depth 0..1).
struct ModelViewCamera {
    glm::vec3 eye{0.0f};
    glm::vec3 target{0.0f};
    glm::mat4 viewProj{1.0f};
    glm::vec3 lightDirection{0.0f, 0.0f, 1.0f};
};
ModelViewCamera modelViewCamera(const glm::vec3& boundsMin, const glm::vec3& boundsMax, f32 aspect, f32 zoom = 1.0f);

// The unit frames' portrait images (parity with the client's SetPortraitTexture): which look each of a fixed
// number of slots shows. A look asked for gets the least recently used slot that was not asked for this
// frame; it is dirty until its portrait is rendered, then ready once the frame that rendered it was
// submitted. A dirty slot can be claimed: drawn this very frame, before the interface that shows it (a new
// target's face appears with its frame). Identical looks (a pack of wolves) share one image. Pure
// bookkeeping, Vulkan-free: app/Portraits renders what it says.
class PortraitCache {
public:
    static constexpr u32 kNone = ~0u;

    explicit PortraitCache(u32 slots);
    u32 slotCount() const { return static_cast<u32>(m_slots.size()); }

    // Once a frame, before anything is asked: `submittedFrames` (frames handed to the GPU so far) settles the
    // renders of the frame before (ready, or dirty again if that frame never reached the GPU).
    void beginFrame(u64 submittedFrames);
    // The slot of `key` this frame: the one that has it, or the least recently used one given to it (dirty).
    // Kept from other looks until the next frame. `subject` (an actor) is whom to render it from: the last
    // one to ask. kNone when every slot was asked for this frame.
    u32 request(u64 key, u32 subject);
    // The slot that has `key`, without asking for it (kNone if none); touch() keeps it this frame.
    u32 find(u64 key) const;
    void touch(u32 slot);

    bool ready(u32 slot) const { return slot < m_slots.size() && m_slots[slot].state == State::Ready; }
    // A dirty slot asked for this frame is drawn this frame (whatever happens, something is drawn into it:
    // the frames built this frame may already show it), at most `budget` of them a frame. True if it is
    // claimed.
    bool claim(u32 slot, u32 budget);
    bool claimed(u32 slot) const { return slot < m_slots.size() && m_slots[slot].claimedFrame == m_frame; }
    u64 key(u32 slot) const { return slot < m_slots.size() ? m_slots[slot].key : 0; }
    u32 subject(u32 slot) const { return slot < m_slots.size() ? m_slots[slot].subject : kNone; }
    // The dirty slots asked for this frame, the claimed ones first, each group in the order they were first
    // asked (the target before the party), at most out.size(). Returns how many.
    u32 dirty(std::span<u32> out) const;
    // `slot` was rendered by the frame numbered `frame` (Renderer::frameNumber() + 1 while it is being
    // prepared).
    void rendered(u32 slot, u64 frame);
    // Its subject cannot be framed (a model without a portrait): never ready, until the slot goes to another
    // look.
    void failed(u32 slot);
    // The images were lost (the renderer made them again): every slot is rendered anew when asked.
    void invalidate();

    u32 slotsInUse() const;

private:
    enum class State : u8 { Empty, Dirty, Rendering, Ready, Failed };
    struct Slot {
        u64 key = 0;
        State state = State::Empty;
        u32 subject = kNone;
        u64 renderFrame = 0;
        u32 usedFrame = 0;  // the frame it was last asked for or touched (0: never)
        u32 askedOrder = 0; // order of its first request this frame
        u32 claimedFrame = 0;
    };
    // Order among this frame's dirty slots: claimed ones first, then by request.
    u32 rank(const Slot& s) const { return (s.claimedFrame == m_frame ? 0u : 0x80000000u) | s.askedOrder; }

    std::vector<Slot> m_slots;
    u32 m_frame = 1;
    u32 m_order = 0;
    u32 m_claims = 0; // this frame's
};

} // namespace vd
