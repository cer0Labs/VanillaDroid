#include "app/Portraits.h"

#include "core/Clock.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "world/Actors.h"

#include <algorithm>
#include <cstdlib>

namespace vd {
namespace {

// The portraits' light: a white key from the viewer's front left, above, over a soft fill (the client's
// portraits are bright and evenly lit, darker on the right; no fog).
const glm::vec3 kDirectColor{0.78f, 0.76f, 0.72f};
const glm::vec3 kAmbientColor{0.46f, 0.46f, 0.48f};

glm::vec3 vec(const f32 v[3]) {
    return glm::vec3(v[0], v[1], v[2]);
}

} // namespace

void Portraits::beginFrame(const Actors& actors, const Renderer& renderer, bool drawing) {
    m_actors = &actors;
    m_renderer = &renderer;
    m_drawing = drawing;
    if (m_frame++ == 0) {
        m_trace = VD_DEV_ENV("VD_TRACE_PORTRAITS") != nullptr;
        m_redraw = VD_DEV_ENV("VD_PORTRAIT_REDRAW") != nullptr;
    }
    // The GPU time of the last frame that drew some (known a frame or two later).
    if (m_trace && m_tracedRenders > 0) {
        VD_LOG_INFO("Portraits: %u drawn, last GPU %.3f ms", m_stats.renders, renderer.stats().portraitGpuMs);
        m_tracedRenders = 0;
    }
    // The images were made again: empty (or development: every portrait drawn every frame, to measure).
    if (renderer.portraitGeneration() != m_generation || m_redraw) {
        m_generation = renderer.portraitGeneration();
        m_cache.invalidate();
    }
    m_cache.beginFrame(renderer.frameNumber());
    m_stats.slotsInUse = m_cache.slotsInUse();
}

TextureHandle Portraits::handle(u32 slot) const {
    return m_renderer && slot != PortraitCache::kNone ? m_renderer->portraitTexture(slot) : TextureHandle{};
}

TextureHandle Portraits::unit(u64 guid) {
    return m_query ? image(m_query(m_context, guid), guid) : TextureHandle{};
}

TextureHandle Portraits::actor(u32 actor) {
    return image(actor, (1ull << 63) | actor);
}

const game::ActorLook* Portraits::look(u64 guid) const {
    const u32 actor = m_query && m_actors ? m_query(m_context, guid) : ~0u;
    return actor != ~0u ? m_actors->look(actor) : nullptr;
}

const game::AppearanceDb* Portraits::appearance() const {
    return m_actors && m_actors->appearance().ready() ? &m_actors->appearance() : nullptr;
}

TextureHandle Portraits::image(u32 actor, u64 subject) {
    if (!m_actors || !m_renderer || actor == ~0u) {
        return {};
    }
    const u64 key = m_actors->portraitKey(actor);
    if (key == 0) {
        return {}; // nothing shown yet (out of range, loading): the stand-in art
    }
    // What this subject showed until now (or a place to remember it).
    Shown* shown = nullptr;
    for (Shown& s : m_shown) {
        if (s.subject == subject) {
            shown = &s;
            break;
        }
    }
    if (!shown) {
        shown = &*std::min_element(m_shown.begin(), m_shown.end(),
                                   [](const Shown& a, const Shown& b) { return a.frame < b.frame; });
        *shown = Shown{subject, 0, m_frame};
    }
    shown->frame = m_frame;
    // Its previous look's image is kept from other looks while the new one waits its turn.
    u32 previous = PortraitCache::kNone;
    if (shown->key != 0 && shown->key != key) {
        previous = m_cache.find(shown->key);
        if (previous != PortraitCache::kNone && m_cache.ready(previous)) {
            m_cache.touch(previous);
        } else {
            previous = PortraitCache::kNone;
        }
    }
    // Ready; or drawn this frame, before the interface (while the world is drawn and this frame's budget
    // allows).
    const u32 slot = m_cache.request(key, static_cast<u32>(actor));
    if (slot != PortraitCache::kNone &&
        (m_cache.ready(slot) ||
         (m_drawing && m_actors->portraitCamera(actor) && m_cache.claim(slot, kMaxPortraitRenders)))) {
        shown->key = key;
        return handle(slot);
    }
    return handle(previous);
}

void Portraits::render(Actors& actors, Renderer& renderer, ActorFrame& frame, std::vector<ActorDraw>& draws,
                       std::vector<PortraitDraw>& out) {
    std::array<u32, kMaxPortraitRenders> dirty{};
    const u32 count = m_cache.dirty(dirty);
    if (count == 0 || !renderer.portraitTexture(0).valid()) {
        return;
    }
    VD_PROFILE_ZONE("Portraits::render");
    const Clock::time_point start = Clock::now();
    for (u32 i = 0; i < count; ++i) {
        const u32 slot = dirty[i];
        const u32 subject = m_cache.subject(slot);
        PortraitDraw portrait;
        portrait.slot = slot;
        portrait.firstActor = static_cast<u32>(draws.size());
        // Its look changed since it asked (the new look asks again), or nothing frames it (the frame keeps
        // its stand-in art): not posed.
        const pack::ActorPortrait* camera =
            actors.portraitKey(subject) == m_cache.key(slot) ? actors.portraitCamera(subject) : nullptr;
        if (camera) {
            portrait.actorCount = actors.appendStill(subject, vec(camera->position), vec(camera->target),
                                                     renderer, frame, draws);
            portrait.viewProj = portraitViewProjection(*camera);
            portrait.lightDirection = portraitKeyLight(*camera);
            portrait.directColor = kDirectColor;
            portrait.ambientColor = kAmbientColor;
        } else if (actors.portraitKey(subject) == m_cache.key(slot)) {
            m_cache.failed(slot);
        }
        if (portrait.actorCount > 0) {
            m_cache.rendered(slot, renderer.frameNumber() + 1);
            ++m_stats.renders;
            out.push_back(portrait);
        } else if (m_cache.claimed(slot)) {
            out.push_back(
                portrait); // shown this frame: at least cleared (no room for its bones; drawn next frame)
        }
    }
    m_stats.lastRenderMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
    if (m_trace) {
        for (const PortraitDraw& p : out) {
            const u32 subject = m_cache.subject(p.slot);
            const pack::ActorPortrait* camera = actors.portraitCamera(subject);
            VD_LOG_INFO("Portraits: slot %u <- actor %u (look %016llx%s%s): %u draws", p.slot, subject,
                        static_cast<unsigned long long>(m_cache.key(p.slot)),
                        camera && (camera->flags & pack::kActorPortraitComputed) ? ", framed by the baker"
                                                                                 : "",
                        m_cache.claimed(p.slot) ? ", shown at once" : "", p.actorCount);
        }
        VD_LOG_INFO("Portraits: %zu posed and listed in %.3f ms", out.size(), m_stats.lastRenderMs);
        m_tracedRenders += static_cast<u32>(out.size());
    }
}

} // namespace vd
