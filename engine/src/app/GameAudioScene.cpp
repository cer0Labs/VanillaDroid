#include "app/GameAudioScene.h"

#include "app/Units.h"
#include "world/Actors.h"
#include "world/Terrain.h"

namespace vd {

u32 GameAudioScene::eventCount() const {
    return actors ? static_cast<u32>(actors->events().size()) : 0;
}

AudioScene::Event GameAudioScene::event(u32 i) const {
    const Actors::EventHit& hit = actors->events()[i];
    return {hit.actor, hit.id, hit.data, hit.index, hit.position};
}

u64 GameAudioScene::actorModel(u32 actor) const {
    const game::ActorLook* look = actors ? actors->look(actor) : nullptr;
    return look ? look->model : 0;
}

u32 GameAudioScene::actorParent(u32 actor) const {
    return actors ? actors->parent(actor) : ~0u;
}

std::optional<glm::dvec3> GameAudioScene::eventPosition(u32 actor, u32 index) const {
    return actors ? actors->eventPosition(actor, index) : std::nullopt;
}

u32 GameAudioScene::unitActor(u64 guid) const {
    return units ? units->actorOf(guid) : ~0u;
}

std::optional<f32> GameAudioScene::groundHeight(f64 x, f64 y) const {
    return terrain ? terrain->heightAt(x, y) : std::nullopt;
}

} // namespace vd
