#pragma once

#include "app/Dialogs.h"
#include "app/ModelFrame.h"
#include "core/Types.h"
#include "render/Renderer.h"

#include <array>
#include <string_view>

namespace vd {

// Phase 15 (items): the dressing rooms, pure views (view-model in, what the player did out).
// DressUpFrame (DressUpFrame.xml): DRESSUP_FRAME over the race's background (DressUpBackground-<Race>1..4:
// Gnomes get the Dwarf's, Trolls the Orc's), the model (DressUpModel, app/ModelFrame with its rotate
// buttons), Reset (DressUpModel:Dress) and Close. A panel window in the left slot (UIPanelWindows: left,
// pushable 2). AuctionDressUpFrame (Blizzard_AuctionDressUp.xml): the auction house's narrow one docked at
// its right edge, with the left parts of backgrounds 1 and 3, the model, Reset and its close button.

struct DressUpView {
    std::string_view title;           // DRESSUP_FRAME
    std::string_view instructions;    // the frame's description line (touch wording)
    std::array<u64, 4> backgrounds{}; // dressUpBackground(race, 1..4): textures.vdpk path hashes
    TextureHandle model;              // app/ModelViews' image (invalid while the model loads)
};
struct DressUpResult {
    bool reset = false;   // Reset: DressUpModel:Dress()
    bool closed = false;  // Close / the close button
    bool rotated = false; // a rotate button went down (igInventoryRotateCharacter)
};

// DressUpTexturePath()..n: the background `part` (1..4) for `race` (ChrRaces id).
u64 dressUpBackground(u8 race, u32 part);

DressUpResult dressUpWindow(DialogContext& c, const DressUpView& v, ModelFrameState& model);
// In `place` (the strip right of the auction house).
DressUpResult auctionDressUpWindow(DialogContext& c, const ui::Rect& place, const DressUpView& v,
                                   ModelFrameState& model);

} // namespace vd
