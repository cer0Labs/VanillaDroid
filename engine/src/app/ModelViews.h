#pragma once

#include "core/Types.h"
#include "game/Appearance.h"
#include "render/Renderer.h"

#include <array>
#include <vector>

namespace vd {

class Actors;
struct ActorFrame;

// Phase 15: who shows a model frame. Each id has at most one model on screen; the windows that cannot be open
// together may share images (at most kModelViewSlots models are shown at once).
enum class ModelViewId : u8 {
    Character,      // PaperDollFrame's CharacterModelFrame
    DressUp,        // DressUpFrame
    AuctionDressUp, // the auction house's AuctionDressUpFrame
    PetPaperDoll,   // PetPaperDollFrame
    Stable,         // PetStable's model
    Inspect,        // InspectPaperDollFrame
    Tabard,         // TabardFrame
    Gallery,        // the offline gallery's sample
    Count
};

// The interface's 3D model frames (the client's PlayerModel / DressUpModel / TabardModel: the character sheet, the
// dressing room, the pet's page, the stable, inspect, the tabard designer): a look rendered live — playing its Stand
// animation, framed whole, turned as the player turns it — into an image of its own with a transparent background,
// which the window draws over its art (app/ModelFrame). The windows' screens ask for their image every frame they
// are built (show(), cheap); after the world's actors are collected, render() poses the asked ones and lists their
// passes (FrameParams::modelViews), drawn before the interface. Each model is an offscreen actor (Actors::
// setOffscreen) kept a while after its window closes, so reopening it shows at once. Game thread only.
class ModelViews {
public:
    // Once a frame, before the windows are built. `drawing`: render() runs this frame (the world is drawn).
    void beginFrame(Actors& actors, const Renderer& renderer, bool drawing);
    // The image showing `look` in the frame `id` this frame, turned by `facing` radians (1.12's SetRotation: 0 faces
    // the viewer, positive turns its front to the viewer's left, as holding Model_OnUpdate's left button does). Invalid while its model loads or when every slot is shown
    // already: draw nothing then. A new look replaces the shown one once loaded (the previous stays until then).
    // `zoom` > 1 brings the camera closer than framing the whole turning model (world/PortraitCache: modelViewCamera).
    TextureHandle show(ModelViewId id, const game::ActorLook& look, f32 facing, f32 zoom = 1.0f);
    // After Actors::finishCollect: the frames shown this frame, posed and appended to `frame` (as the world's actors)
    // with their draws in `draws`, one PortraitDraw each in `out` (FrameParams::modelViews; draws in portraitActors).
    void render(Actors& actors, Renderer& renderer, ActorFrame& frame, std::vector<ActorDraw>& draws,
                std::vector<PortraitDraw>& out);
    // Before the actors close (logout, a new world): their models go.
    void reset(Actors& actors);

    struct Stats {
        u32 shown = 0;           // frames shown this frame
        u32 actors = 0;          // models kept (shown or recently)
        f32 lastRenderMs = 0.0f; // main-thread time of the last render() that posed some
    };
    const Stats& stats() const { return m_stats; }
    // Phase 15 (guild): the actors of the models (their appearance tables build the looks the windows show); null before
    // the first beginFrame.
    const Actors* actors() const { return m_actors; }

private:
    struct View {
        ModelViewId id = ModelViewId::Count;
        u32 actor = ~0u;
        u64 key = 0; // the look asked for (world/PortraitCache: portraitLookKey)
        f32 facing = 0.0f;
        f32 zoom = 1.0f;
        u32 frame = 0; // last frame it was shown
        bool shown = false;
    };

    std::array<View, kModelViewSlots> m_views{};
    Actors* m_actors = nullptr;
    const Renderer* m_renderer = nullptr;
    bool m_drawing = false;
    u32 m_frame = 0;
    Stats m_stats;
};

} // namespace vd
