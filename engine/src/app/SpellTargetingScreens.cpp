#include "app/SpellTargetingScreens.h"

#include "app/GameSession.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/ItemDefs.h"
#include "game/TouchAssistRules.h" // phase 20 (assists)
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;

constexpr f32 kDefaultDistance = 10.0f;              // yards in front of the player where the circle starts
constexpr f32 kPickDistance = 200.0f;                // how far a finger's line looks for the ground
constexpr f32 kCircleMinDp = 56.0f;                  // the circle's drag area, never smaller than a finger
constexpr u32 kDemoRearmMs = 1500;                   // VD_TARGETING_DEMO: the next place after a cast
constexpr i32 kItemClassWeapon = 2, kItemClassArmor = 4;
constexpr u32 kLockTypePicklock = 1; // LockType.dbc: lockboxes open with Pick Lock

f32 floatField(const game::Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

void SpellTargetingScreens::init(const game::GameData* data) {
    m_data = data;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    if (const char* demo = VD_DEV_ENV("VD_TARGETING_DEMO"); demo && data) {
        if (const std::optional<game::SpellInfo> spell = data->spell(static_cast<u32>(std::atoi(demo)))) {
            m_demoSpell = *spell; // development: chosen offline (--walk)
        }
    }
}

// --- The state
// -----------------------------------------------------------------------------------------------

const game::SpellInfo* SpellTargetingScreens::groundSpell(const GameSession& session) const {
    if (session.inWorld()) {
        const game::SpellTargeting& t = session.play().targeting();
        return t.active() && t.aim() == game::SpellAim::Location ? &t.spell() : nullptr;
    }
    return m_demoActive ? &m_demoSpell : nullptr;
}

bool SpellTargetingScreens::groundActive(const GameSession& session) const {
    return groundSpell(session) != nullptr;
}

bool SpellTargetingScreens::pickingItems(const GameSession& session) const {
    if (!session.inWorld()) {
        return false;
    }
    const game::SpellTargeting& t = session.play().targeting();
    return t.active() && (t.aim() == game::SpellAim::Item || t.aim() == game::SpellAim::Locked);
}

void SpellTargetingScreens::prepareHud(GameSession& session, HudFrame& hud, u32 nowMs) {
    VD_PROFILE_ZONE("SpellTargeting::prepare");
    m_viewProj = hud.viewProj;
    m_camera = hud.camera;
    m_width = std::max(1.0f, hud.width);
    m_height = std::max(1.0f, hud.height);
    m_position = hud.position;
    m_pick = hud.groundPick;
    m_pickContext = hud.groundContext;
    ++m_frame; // phase 20 (assists): the touch cluster's ground aim lasts the frame it is set in
    // Development: the demo's place comes back a moment after each cast, offline with a character.
    m_nowMs = nowMs;
    if (m_demoSpell.id != 0 && !session.inWorld() && hud.player && !m_demoActive &&
        static_cast<i32>(nowMs - m_demoRearmAt) >= 0) {
        m_demoActive = true;
        m_placed = false;
        m_windowsKnown = false;
    }
    if (session.inWorld()) {
        const game::SpellTargeting& t = session.play().targeting();
        if (t.version() != m_version) { // another spell, or none: its place starts over
            m_version = t.version();
            m_placed = false;
            m_windowsKnown = false;
        }
    }
    const bool ground = groundActive(session);
    if (ground && !m_placed) {
        placeDefault(session);
    }
    if (ground) {
        updateRange(session);
    }
    hud.groundTargeting = ground;
    m_groundShown = ground;
    // The item choice: the windows it wants.
    const bool items = pickingItems(session);
    if (items && !m_itemChoice) {
        const game::SpellTargeting& t = session.play().targeting();
        const game::SpellInfo& spell = t.spell();
        m_itemChoiceStarted = true;
        m_wantsBags = t.aim() == game::SpellAim::Item || game::spellLockType(spell) == kLockTypePicklock;
        m_wantsCharacter = t.aim() == game::SpellAim::Item && (spell.equippedItemClass == kItemClassWeapon ||
                                                               spell.equippedItemClass == kItemClassArmor);
        if (m_trace) {
            VD_LOG_INFO("Targeting: spell %u waits for an item (bags %d, character %d)", spell.id,
                        m_wantsBags, m_wantsCharacter);
        }
    } else if (!items && m_itemChoice) {
        m_itemChoiceEnded = true;
        m_wantsBags = m_wantsCharacter = false;
    }
    m_itemChoice = items;
}

bool SpellTargetingScreens::takeItemChoiceStarted() {
    const bool started = m_itemChoiceStarted;
    m_itemChoiceStarted = false;
    return started;
}

bool SpellTargetingScreens::takeItemChoiceEnded() {
    const bool ended = m_itemChoiceEnded;
    m_itemChoiceEnded = false;
    return ended;
}

// --- The place
// -----------------------------------------------------------------------------------------------

bool SpellTargetingScreens::project(const glm::dvec3& world, f32& x, f32& y) const {
    const glm::vec4 clip = m_viewProj * glm::vec4(glm::vec3(world - m_camera), 1.0f);
    if (clip.w < 0.1f) {
        return false;
    }
    x = (clip.x / clip.w * 0.5f + 0.5f) * m_width;
    y = (clip.y / clip.w * 0.5f + 0.5f) * m_height;
    return true;
}

bool SpellTargetingScreens::pickGround(f32 x, f32 y, glm::dvec3& out) const {
    if (!m_pick) {
        return false;
    }
    // The screen point back through the camera-relative projection: a line from the near plane to the far
    // one.
    const glm::mat4 inverse = glm::inverse(m_viewProj);
    const f32 nx = x / m_width * 2.0f - 1.0f, ny = y / m_height * 2.0f - 1.0f;
    const glm::vec4 nearPoint = inverse * glm::vec4(nx, ny, 0.0f, 1.0f);
    const glm::vec4 farPoint = inverse * glm::vec4(nx, ny, 1.0f, 1.0f);
    if (std::fabs(nearPoint.w) < 1e-6f || std::fabs(farPoint.w) < 1e-6f) {
        return false;
    }
    const glm::vec3 a = glm::vec3(nearPoint) / nearPoint.w;
    const glm::vec3 b = glm::vec3(farPoint) / farPoint.w;
    const glm::vec3 direction = b - a;
    if (glm::dot(direction, direction) < 1e-8f) {
        return false;
    }
    const std::optional<glm::dvec3> hit =
        m_pick(m_pickContext, m_camera + glm::dvec3(a), glm::normalize(direction), kPickDistance);
    if (!hit) {
        return false;
    }
    out = *hit;
    return true;
}

bool SpellTargetingScreens::groundBelow(const glm::dvec3& at, glm::dvec3& out) const {
    const std::optional<glm::dvec3> hit =
        m_pick ? m_pick(m_pickContext, at + glm::dvec3(0.0, 0.0, 8.0), glm::vec3(0.0f, 0.0f, -1.0f), 40.0f)
               : std::nullopt;
    out = hit ? *hit : at;
    return hit.has_value();
}

void SpellTargetingScreens::placeDefault(const GameSession& session) {
    const game::SpellInfo* spell = groundSpell(session);
    if (!spell) {
        return;
    }
    const game::SpellMods* mods = session.inWorld() ? &session.play().spellMods() : nullptr;
    const game::SpellModTarget target = game::modTarget(*spell);
    m_radius = game::spellAreaRadius(*spell);
    m_maxRange = spell->maxRange;
    if (mods) {
        m_radius = mods->apply(target, game::SpellModOp::Radius, m_radius);
        m_maxRange = mods->apply(target, game::SpellModOp::Range, m_maxRange);
    }
    // In front of the player as the camera looks (the camera stands behind it), a few yards away.
    glm::dvec2 forward(m_position.x - m_camera.x, m_position.y - m_camera.y);
    if (glm::dot(forward, forward) < 0.01) {
        forward = glm::dvec2(1.0, 0.0);
    }
    forward = glm::normalize(forward);
    const f64 distance = m_maxRange > 0.0f ? std::min<f64>(kDefaultDistance, m_maxRange) : kDefaultDistance;
    groundBelow(m_position + glm::dvec3(forward * distance, 0.0), m_center);
    m_placed = true;
}

void SpellTargetingScreens::updateRange(const GameSession& session) {
    const game::SpellInfo* spell = groundSpell(session);
    if (!spell) {
        return;
    }
    glm::vec3 caster(m_position);
    f32 bounds = 0.0f;
    const game::SpellMods* mods = nullptr;
    if (session.inWorld()) {
        mods = &session.play().spellMods();
        if (const game::Entity* me = session.entities().find(session.playerGuid())) {
            caster = me->positionAt(m_nowMs);
            bounds = floatField(*me, uf::UNIT_FIELD_BOUNDINGRADIUS);
        }
    }
    m_inRange = game::placeRange(*spell, mods, caster, bounds, glm::vec3(m_center)) == game::PlaceRange::In;
}

void SpellTargetingScreens::confirm(GameSession& session) {
    if (!session.inWorld()) {
        if (m_demoActive) {
            VD_LOG_INFO("Targeting (demo): spell %u at (%.2f, %.2f, %.2f), %s", m_demoSpell.id, m_center.x,
                        m_center.y, m_center.z, m_inRange ? "cast" : "out of range: still waiting");
            if (!m_inRange) {
                return; // as Gameplay::targetLocation: SPELL_FAILED_OUT_OF_RANGE, the place still chosen
            }
            m_demoActive = false;
            m_demoRearmAt = m_nowMs + kDemoRearmMs; // when it comes back
        }
        return;
    }
    const game::Gameplay::TargetResult r = session.play().targetLocation(
        static_cast<f32>(m_center.x), static_cast<f32>(m_center.y), static_cast<f32>(m_center.z));
    if (m_trace) {
        VD_LOG_INFO("Targeting: spell %u at (%.2f, %.2f, %.2f): %u", session.play().targeting().spellId(),
                    m_center.x, m_center.y, m_center.z, static_cast<u32>(r));
    }
}

void SpellTargetingScreens::cancel(GameSession& session) {
    if (!session.inWorld()) {
        if (m_demoActive) {
            m_demoActive = false;
            m_demoRearmAt = m_nowMs + kDemoRearmMs;
        }
        return;
    }
    session.play().stopTargeting();
}

bool SpellTargetingScreens::actOnObject(GameSession& session, u64 gameObject) {
    if (gameObject == 0 || !session.inWorld()) {
        return false;
    }
    const game::SpellTargeting& t = session.play().targeting();
    if (!t.active() || t.aim() != game::SpellAim::Locked) {
        return false;
    }
    session.play().targetObject(gameObject);
    return true;
}

void SpellTargetingScreens::drawWorld(GameSession& session, DialogContext& c, const HudOutput& out) {
    if (!groundActive(session)) {
        return;
    }
    VD_PROFILE_ZONE("SpellTargeting::world");
    // The circle's box on screen (its centre and the ends of two diameters that are in front of the camera),
    // never smaller than a finger: a finger that lands there drags it, a tap there casts.
    f32 cx, cy;
    if (project(m_center, cx, cy)) {
        const f32 minSize = c.ui.px(kCircleMinDp);
        f32 left = cx - minSize * 0.5f, right = cx + minSize * 0.5f;
        f32 top = cy - minSize * 0.5f, bottom = cy + minSize * 0.5f;
        for (u32 i = 0; i < 4; ++i) {
            glm::dvec3 p = m_center;
            p[i / 2] += (i % 2 ? -1.0 : 1.0) * m_radius;
            f32 x, y;
            if (project(p, x, y)) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
        const ui::Rect box{left, top, right - left, bottom - top};
        const ui::WidgetState w = c.ui.widget(ui::widgetId("targeting.circle"), box);
        if (w.pressed && w.touch && w.touch->moved) {
            glm::dvec3 ground;
            if (pickGround(w.touch->x, w.touch->y, ground)) {
                m_center = ground;
            }
        }
        if (w.clicked && w.touch && !w.touch->moved) { // a tap, not the end of a drag
            confirm(session);
            return;
        }
    }
    if (out.groundTapped) {
        glm::dvec3 ground;
        const bool picked = pickGround(out.groundTapX, out.groundTapY, ground);
        if (picked) {
            m_center = ground;
        }
        if (m_trace) {
            VD_LOG_INFO("Targeting: tap at (%.0f, %.0f): %s (%.2f, %.2f, %.2f), player (%.2f, %.2f, %.2f)",
                        out.groundTapX, out.groundTapY, picked ? "ground" : "nothing", ground.x, ground.y,
                        ground.z, m_position.x, m_position.y, m_position.z);
        }
    }
    updateRange(session);
}

void SpellTargetingScreens::drawBar(GameSession& session, DialogContext& c, const ui::Rect& actionBar) {
    const game::SpellInfo* ground = groundSpell(session);
    const bool items = pickingItems(session);
    if (!ground && !items) {
        return;
    }
    const game::SpellInfo& spell = ground ? *ground : session.play().targeting().spell();
    TargetingBarView view;
    view.above = actionBar;
    view.iconHash = spell.iconHash;
    view.title = spell.name;
    if (items && session.play().targeting().fromItem()) { // a poison, a bomb: the item's icon
        if (const net::ItemQueryInfo* info = session.item(session.play().targeting().itemEntry())) {
            view.iconHash = m_data ? m_data->itemIconHash(info->displayId) : view.iconHash;
        }
    }
    if (ground) {
        view.showCast = true;
        view.inRange = m_inRange;
        view.hint = text::get(m_inRange ? "VD_TARGETING_PLACE" : "SPELL_FAILED_OUT_OF_RANGE");
    } else {
        view.hint =
            text::get(session.play().targeting().aim() == game::SpellAim::Locked ? "VD_TARGETING_LOCKED"
                                                                                 : "VD_TARGETING_ITEM");
    }
    const TargetingBarResult r = targetingBar(c, view);
    if (r.cast) {
        confirm(session);
    } else if (r.cancel) {
        cancel(session);
    }
}

void SpellTargetingScreens::windowsShown(GameSession& session, u64 mask) {
    if (!groundActive(session)) {
        return;
    }
    if (!m_windowsKnown) {
        m_windowsKnown = true;
        m_windowsAtStart = mask;
        return;
    }
    if ((mask & ~m_windowsAtStart) != 0) {
        cancel(session); // a window opened over the world
        return;
    }
    m_windowsAtStart &= mask; // one that closes and opens again counts as opening
}

bool SpellTargetingScreens::circle(Circle& out) const {
    if (m_aimFrame == m_frame && m_aimRadius > 0.0f) { // phase 20 (assists): a touch button aiming
        out.center = m_aimCenter;
        out.radius = m_aimRadius;
        out.acceptable = m_aimInRange;
        return true;
    }
    if (!m_placed || m_radius <= 0.0f || !m_groundShown) {
        return false;
    }
    out.center = m_center;
    out.radius = m_radius;
    out.acceptable = m_inRange;
    return true;
}

// --- Phase 20 (assists): the touch cluster's ground aim
// --------------------------------------------------------------------------

void SpellTargetingScreens::aimAt(const game::SpellInfo& spell, const game::SpellMods* mods, const game::Entity* me,
                                  f32 nx, f32 ny) {
    // The spell's area and range with the player's modifiers, as placeDefault.
    const game::SpellModTarget target = game::modTarget(spell);
    f32 radius = game::spellAreaRadius(spell);
    f32 range = spell.maxRange;
    if (mods) {
        radius = mods->apply(target, game::SpellModOp::Radius, radius);
        range = mods->apply(target, game::SpellModOp::Range, range);
    }
    glm::vec3 caster(m_position);
    f32 bounds = 0.0f;
    if (me) {
        caster = me->positionAt(m_nowMs);
        bounds = floatField(*me, uf::UNIT_FIELD_BOUNDINGRADIUS);
    }
    // The camera's frame on the ground: away from it, and the side the screen's right is on.
    glm::dvec2 forward(m_position.x - m_camera.x, m_position.y - m_camera.y);
    forward = glm::dot(forward, forward) < 0.01 ? glm::dvec2(1.0, 0.0) : glm::normalize(forward);
    glm::dvec2 right(forward.y, -forward.x);
    f32 x0, y0, x1, y1;
    if (project(m_position, x0, y0) && project(m_position + glm::dvec3(right, 0.0), x1, y1) && x1 < x0) {
        right = -right;
    }
    // Inside the range by a little (the server measures from the caster's feet to the place).
    const glm::vec2 offset = game::touchAimOffset(nx, ny, glm::vec2(forward), glm::vec2(right),
                                                  std::max(0.0f, range - 0.5f));
    groundBelow(glm::dvec3(caster) + glm::dvec3(offset.x, offset.y, 0.0), m_aimCenter);
    m_aimRadius = radius;
    m_aimInRange = game::placeRange(spell, mods, caster, bounds, glm::vec3(m_aimCenter)) == game::PlaceRange::In;
    m_aimFrame = m_frame;
}

bool SpellTargetingScreens::aimPoint(glm::dvec3& out) const {
    if (m_aimFrame != m_frame) {
        return false;
    }
    out = m_aimCenter;
    return true;
}

// --- The item choice
// -------------------------------------------------------------------------------------------

SpellTargetingScreens::Mark SpellTargetingScreens::mark(const GameSession& session,
                                                        const game::ItemInstance* item,
                                                        const net::ItemQueryInfo* info) const {
    if (!pickingItems(session) || !item || item->entry == 0) {
        return Mark::None;
    }
    const game::SpellTargeting& t = session.play().targeting();
    if (!info || (t.fromItem() && item->guid == t.itemGuid())) {
        return Mark::Dim; // the poison itself
    }
    return game::itemFitsSpell(t.spell(), *info, item) == game::ItemFit::Fits ? Mark::Lit : Mark::Dim;
}

void SpellTargetingScreens::pickItem(GameSession& session, game::ItemPos pos) {
    const game::Gameplay::TargetResult r = session.play().targetItem(pos);
    if (m_trace) {
        VD_LOG_INFO("Targeting: item %u/%u: %u", pos.bag, pos.slot, static_cast<u32>(r));
    }
}

void SpellTargetingScreens::pickTradeItem(GameSession& session) {
    const game::Gameplay::TargetResult r = session.play().targetTradeItem();
    if (m_trace) {
        VD_LOG_INFO("Targeting: the trade slot: %u", static_cast<u32>(r));
    }
}

} // namespace vd
