#include "app/ModelViews.h"

#include "anim/Animator.h"
#include "core/Clock.h"
#include "core/Profile.h"
#include "world/Actors.h"
#include "world/PortraitCache.h"

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

// A model nobody showed for this many frames gives its actor back (~10 s at 60 fps).
constexpr u32 kIdleFrames = 600;
// The portraits' light (app/Portraits): a white key from the viewer's front left, above, over a soft fill.
const glm::vec3 kDirectColor{0.78f, 0.76f, 0.72f};
const glm::vec3 kAmbientColor{0.46f, 0.46f, 0.48f};

} // namespace

void ModelViews::beginFrame(Actors& actors, const Renderer& renderer, bool drawing) {
    m_actors = &actors;
    m_renderer = &renderer;
    m_drawing = drawing;
    ++m_frame;
    m_stats.shown = 0;
    m_stats.actors = 0;
    for (View& v : m_views) {
        v.shown = false;
        if (v.actor != ~0u && m_frame - v.frame > kIdleFrames) {
            actors.destroy(v.actor);
            v = View{};
        }
        m_stats.actors += v.actor != ~0u ? 1 : 0;
    }
}

TextureHandle ModelViews::show(ModelViewId id, const game::ActorLook& look, f32 facing, f32 zoom) {
    if (!m_actors || !m_renderer || !m_actors->isOpen() || look.model == 0) {
        return {};
    }
    // Its frame, or the place of the one unseen the longest (none while every slot is on screen).
    View* view = nullptr;
    for (View& v : m_views) {
        if (v.id == id && v.actor != ~0u) {
            view = &v;
            break;
        }
    }
    if (!view) {
        for (View& v : m_views) {
            if (v.actor != ~0u && v.frame == m_frame) {
                continue; // on screen this frame
            }
            if (!view || (view->actor != ~0u && (v.actor == ~0u || v.frame < view->frame))) {
                view = &v;
            }
        }
        if (!view) {
            return {};
        }
        if (view->actor != ~0u) {
            m_actors->destroy(view->actor);
        }
        *view = View{};
        view->id = id;
        view->actor = m_actors->create();
        m_actors->setOffscreen(view->actor, true);
        m_actors->play(view->actor, anim::animation::kStand, true, 1.0f);
    }
    const u64 key = portraitLookKey(look);
    if (key != view->key) {
        m_actors->setLook(view->actor, look);
        view->key = key;
    }
    view->facing = facing;
    view->zoom = zoom;
    view->frame = m_frame;
    if (!m_drawing || m_actors->portraitKey(view->actor) == 0) {
        return {}; // loading (or not drawn this frame): the window shows its art alone
    }
    if (!view->shown) {
        view->shown = true;
        ++m_stats.shown;
    }
    return m_renderer->modelViewTexture(static_cast<u32>(view - m_views.data()));
}

void ModelViews::render(Actors& actors, Renderer& renderer, ActorFrame& frame, std::vector<ActorDraw>& draws,
                        std::vector<PortraitDraw>& out) {
    if (m_stats.shown == 0) {
        return;
    }
    VD_PROFILE_ZONE("ModelViews::render");
    const Clock::time_point start = Clock::now();
    for (u32 slot = 0; slot < m_views.size(); ++slot) {
        const View& v = m_views[slot];
        glm::vec3 min, max;
        if (!v.shown || !actors.shownBounds(v.actor, min, max)) {
            continue;
        }
        const ModelViewCamera camera =
            modelViewCamera(min, max, static_cast<f32>(kModelViewWidth) / static_cast<f32>(kModelViewHeight), v.zoom);
        PortraitDraw draw;
        draw.slot = slot;
        draw.firstActor = static_cast<u32>(draws.size());
        // The camera looks down -X with +Y on its right: a positive yaw would turn the front to the right.
        draw.actorCount = actors.appendPosed(v.actor, camera.eye, camera.target, -v.facing, renderer, frame, draws);
        draw.viewProj = camera.viewProj;
        draw.lightDirection = camera.lightDirection;
        draw.directColor = kDirectColor;
        draw.ambientColor = kAmbientColor;
        out.push_back(draw); // shown this frame: at least cleared (no room for its bones: drawn next frame)
    }
    m_stats.lastRenderMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
}

void ModelViews::reset(Actors& actors) {
    for (View& v : m_views) {
        if (v.actor != ~0u && actors.isOpen()) {
            actors.destroy(v.actor);
        }
        v = View{};
    }
}

} // namespace vd
