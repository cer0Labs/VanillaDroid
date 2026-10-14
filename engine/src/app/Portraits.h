#pragma once

#include "core/Types.h"
#include "game/Appearance.h"
#include "render/Renderer.h"
#include "world/PortraitCache.h"

#include <array>
#include <vector>

namespace vd {

class Actors;
struct ActorFrame;

// The unit frames' portraits (parity with the client's SetPortraitTexture): a still image of the unit's model
// as it looks in the world — skin, face, hair, helm, a creature's display, a humanoid NPC's baked skin —
// standing, seen by its model's portrait camera over black, lit by one key light from the front left. The
// frames ask for a unit's image while they are built (unit(), cheap); after the world's actors are collected,
// render() draws the new looks asked for into the renderer's portrait slots, at most kMaxPortraitRenders a
// frame, before the interface: a new target's face shows with its frame. An image is redrawn only when the
// unit's shown look changes (target change, helm, shapeshift or display change...). A unit without a loaded
// model (out of range) shows its frame's TemporaryPortrait art; one whose new look waits its turn keeps the
// image of its previous look. Game thread only.
class Portraits {
public:
    // Unit (GUID) -> its actor (Actors), ~0u when it has none: the frame shows its stand-in art.
    using ActorQuery = u32 (*)(const void* context, u64 guid);
    void setActorQuery(ActorQuery query, const void* context) {
        m_query = query;
        m_context = context;
    }

    // Once a frame, before the frames are built. `drawing`: render() runs this frame (the world is drawn).
    void beginFrame(const Actors& actors, const Renderer& renderer, bool drawing);
    // A unit's (or an actor's) image for the frames built this frame; invalid when there is none to show.
    TextureHandle unit(u64 guid);
    TextureHandle actor(u32 actor);
    // Phase 15 (pets): a unit's look as its actor shows it in the world (the pet's page and the stable show
    // it in a model frame), nullptr when it has none; and the appearance tables (a stabled pet's look from
    // its display).
    const game::ActorLook* look(u64 guid) const;
    const game::AppearanceDb* appearance() const;
    // After Actors::finishCollect: the portraits asked for this frame that need drawing, appended to `frame`
    // (as the world's actors) with their draws in `draws`, one PortraitDraw each in `out`
    // (FrameParams::portraits and portraitActors).
    void render(Actors& actors, Renderer& renderer, ActorFrame& frame, std::vector<ActorDraw>& draws,
                std::vector<PortraitDraw>& out);

    struct Stats {
        u32 slotsInUse = 0;
        u32 renders = 0;         // since start
        f32 lastRenderMs = 0.0f; // main-thread time of the last frame that drew portraits (posing + lists)
    };
    const Stats& stats() const { return m_stats; }

private:
    // `subject`: who asks (a unit's GUID, or an actor) — what keeps showing its previous image.
    TextureHandle image(u32 actor, u64 subject);
    TextureHandle handle(u32 slot) const;

    PortraitCache m_cache{kPortraitSlots};
    ActorQuery m_query = nullptr;
    const void* m_context = nullptr;
    const Actors* m_actors = nullptr;
    const Renderer* m_renderer = nullptr;
    bool m_drawing = false;
    u32 m_generation = 0;
    u32 m_frame = 0;
    // The look each recent subject last showed: kept on screen while its new look waits to be drawn.
    struct Shown {
        u64 subject = 0;
        u64 key = 0;
        u32 frame = 0;
    };
    std::array<Shown, kPortraitSlots> m_shown{};
    Stats m_stats;
    // Development: VD_TRACE_PORTRAITS (each portrait drawn, its CPU and GPU time to the log),
    // VD_PORTRAIT_REDRAW (every portrait drawn again every frame, to measure what a draw costs).
    bool m_trace = false;
    bool m_redraw = false;
    u32 m_tracedRenders = 0;
};

} // namespace vd
