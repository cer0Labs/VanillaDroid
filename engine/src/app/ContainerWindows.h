#pragma once

#include "app/ContainerLayout.h"
#include "app/Dialogs.h"
#include "app/InventoryWindows.h"
#include "core/Types.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 15 (items): the ContainerFrames drawn (ContainerFrame.xml): each open bag in a frame of its own with
// the original's background for its size (app/ContainerLayout), the bag's icon in the portrait circle, its
// name, the close button, the backpack's money frame and the item buttons over the art. A pure view like
// app/InventoryWindows: the cells report taps, long presses (the carried item) and drops in the
// ItemWindowResult as every item window does.

struct ContainerFrameView {
    i32 id = 0; // the container (0 the backpack, 1..4 the bags, 5..10 the bank's, kKeyringContainer)
    std::string_view name; // GetBagName, KEYRING for the keyring
    u64 portrait = 0;      // the bag's icon (textures.vdpk path hash) for SetBagPortaitTexture; the keyring's
                           // KeyRing-Bag-Icon; 0: none (the backpack's art has its own)
    // Its slots in the client's order (GetContainerItemInfo(id, 1..size)).
    std::span<const ItemSlotView> cells;
    u32 money = 0; // the backpack's money frame
};

struct ContainerFramesResult {
    static constexpr i32 kNone = -100;
    i32 closed = kNone;   // the container whose close button was tapped (ToggleBag)
    i32 portrait = kNone; // its portrait tapped or held (PortraitButton's OnEnter: the bag's tooltip)
    ui::Rect portraitRect;
};

// The frames at `rects` (layoutContainers at `scale`, logical pixels per unit). A drop on a frame but off its
// cells is the result's zone 0 (into the bags, as the bags grid's).
void containerFrames(DialogContext& c, std::span<const ContainerFrameView> frames,
                     std::span<const ui::Rect> rects, f32 scale, ItemDrag& drag, ItemWindowResult& items,
                     ContainerFramesResult& out);

} // namespace vd
