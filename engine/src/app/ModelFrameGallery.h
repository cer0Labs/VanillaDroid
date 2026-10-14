#pragma once

#include "app/DialogKit.h"
#include "app/ModelFrame.h"
#include "app/ModelViews.h"
#include "core/Types.h"
#include "game/Appearance.h"

#include <array>

namespace vd {

class Actors;

// Development (phase 15, VD_UI_GALLERY=modelframe): the interface's 3D model frames without a server, and sample
// models for the other gallery pages that show one (the character sheet, the dressing room, the pet's page...): a
// human in a helm and shoulder pads, an orc, a timber wolf (the looks of app/PortraitGallery). The actors' packs must
// be open (a map loaded) for the models to show; until then the frames show their art alone.
class ModelFrameGallery {
public:
    enum class Sample : u8 { Human, Orc, Wolf, Count };

    void setSources(Actors* actors, ModelViews* views) {
        m_actors = actors;
        m_views = views;
    }
    // The image of `sample` in the frame `id` (ModelViews::show), invalid while it loads or without sources.
    TextureHandle sample(ModelViewId id, Sample sample, f32 facing);
    // The look of a sample (nullptr before the appearance tables are ready).
    const game::ActorLook* look(Sample sample);
    // The page: two model frames over a window's art and the bare widget over the world.
    void draw(DialogContext& c);

private:
    Actors* m_actors = nullptr;
    ModelViews* m_views = nullptr;
    std::array<game::ActorLook, static_cast<usize>(Sample::Count)> m_looks{};
    bool m_ready = false;
    std::array<ModelFrameState, 3> m_frames{};
};

} // namespace vd
