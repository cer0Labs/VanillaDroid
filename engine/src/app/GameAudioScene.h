#pragma once

#include "core/Types.h"

#include <glm/vec3.hpp>

#include <optional>

namespace vd {

class Actors;
class Terrain;
class Units;

// Phase 14 (sound): what GameAudio asks the world for the models' sounds (the doodads' come baked:
// audio/WorldSoundTables). GameAudioScene implements it over the world (Actors, Units, Terrain); the tests over fakes.
// Game thread.
class AudioScene {
public:
    virtual ~AudioScene() = default;

    // The actors' animation events of the last frame (Actors::events): who, the event's id (four characters, little
    // endian) and data word, its index in the model and its world point.
    struct Event {
        u32 actor = ~0u;
        u32 id = 0;
        u32 data = 0;
        u32 index = 0;
        glm::dvec3 position{0.0};
    };
    virtual u32 eventCount() const = 0;
    virtual Event event(u32 i) const = 0;
    // An actor's model (actors.vdpk asset hash), 0 once it is gone; what it rides (~0u none); where an event's point
    // of its model is now.
    virtual u64 actorModel(u32 actor) const = 0;
    virtual u32 actorParent(u32 actor) const = 0;
    virtual std::optional<glm::dvec3> eventPosition(u32 actor, u32 index) const = 0;

    // The actor showing a unit (~0u none); the terrain's height under a point (nullopt where unknown).
    virtual u32 unitActor(u64 guid) const = 0;
    virtual std::optional<f32> groundHeight(f64 x, f64 y) const = 0;
};

// The world's: Game fills one per frame.
class GameAudioScene final : public AudioScene {
public:
    const Actors* actors = nullptr;
    const Units* units = nullptr;
    const Terrain* terrain = nullptr;

    u32 eventCount() const override;
    Event event(u32 i) const override;
    u64 actorModel(u32 actor) const override;
    u32 actorParent(u32 actor) const override;
    std::optional<glm::dvec3> eventPosition(u32 actor, u32 index) const override;
    u32 unitActor(u64 guid) const override;
    std::optional<f32> groundHeight(f64 x, f64 y) const override;
};

} // namespace vd
