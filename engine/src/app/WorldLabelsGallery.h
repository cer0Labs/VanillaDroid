#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"

namespace vd {

// Development (parity, VD_UI_GALLERY=worldlabels): what the client's engine writes over units
// (app/WorldLabels) with Northshire sample data: names in their reaction colours with an NPC's title, a
// player's guild and a pet's owner under them, the thin health bar of a wounded unit, the name plates of the
// V / Shift-V keys (hostile, neutral, friendly, "??" for a unit far above the player) and chat bubbles (say,
// monster yell, party, one fading out). Over the world when the gallery runs with --map.
class WorldLabelsGallery {
public:
    void draw(DialogContext& c);
};

} // namespace vd
