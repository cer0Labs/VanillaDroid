#include "app/CharacterPreview.h"

#include "anim/Animator.h"
#include "world/Actors.h"

#include <iterator>

namespace vd {

void CharacterPreview::update(Actors& actors, const game::CharacterLook* look, const glm::dvec3& feet, f32 yaw) {
    if (!look || !actors.isOpen() || !actors.appearance().ready()) {
        clear(actors);
        return;
    }
    if (m_actor == Actors::kNone) {
        m_actor = actors.create();
        m_hasLook = false;
        actors.play(m_actor, anim::animation::kStand, true, 1.0f);
    }
    if (!m_hasLook || !(*look == m_look)) {
        m_look = *look;
        m_hasLook = true;
        actors.setLook(m_actor, actors.appearance().character(*look, true));
    }
    actors.setPlacement(m_actor, feet, yaw, 1.0f);
}

namespace {

// About how tall each race stands (yards): gnomes are a third of a tauren.
f32 raceHeight(u8 race) {
    static constexpr f32 kHeights[] = {1.9f, 1.9f, 2.0f, 1.4f, 2.3f, 1.8f, 2.6f, 1.0f, 2.3f};
    return race < std::size(kHeights) ? kHeights[race] : 1.9f;
}

} // namespace

f32 CharacterPreview::cameraDistance(u8 race) {
    return raceHeight(race) * 1.25f + 1.2f;
}

f32 CharacterPreview::cameraHeight(u8 race) {
    return raceHeight(race) * 0.55f + 0.6f;
}

void CharacterPreview::clear(Actors& actors) {
    if (m_actor != Actors::kNone) {
        actors.destroy(m_actor);
        m_actor = Actors::kNone;
        m_hasLook = false;
    }
}

} // namespace vd
