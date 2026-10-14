#pragma once

#include "app/DialogKit.h"
#include "core/Types.h"

#include <array>

namespace vd {

class Actors;
class Portraits;

// Development (parity, VD_UI_GALLERY=portraits): the unit frames with real portraits, without a server.
// Sample units — a human with helm and shoulders, an orc, a tauren and a gnome (players: race,
// customization), a wolf, a kobold, a humanoid NPC with its baked skin, helm and shoulders, and a tiger cub
// (a model without a portrait camera: framed by the baker) — become actors (nowhere in the world: they load
// with it, so a map must be open) and their portraits go into the player and target frames as the HUD lays
// them out (app/Hud), the target of target as app/CombatScreens draws it, the party frames (partyFrames) and
// the pet frame (petFrame). Before an image is ready each frame shows its stand-in art.
class PortraitGallery {
public:
    void setSources(Actors* actors, Portraits* portraits) {
        m_actors = actors;
        m_portraits = portraits;
    }
    void draw(DialogContext& c);

private:
    void spawn();

    Actors* m_actors = nullptr;
    Portraits* m_portraits = nullptr;
    static constexpr u32 kSamples = 8;
    std::array<u32, kSamples> m_samples{};
    bool m_spawned = false;
};

} // namespace vd
