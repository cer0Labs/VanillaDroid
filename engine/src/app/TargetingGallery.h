#pragma once

#include "app/Dialogs.h"
#include "app/InventoryWindows.h"
#include "app/PopupStack.h"
#include "app/PopupWindows.h"
#include "core/Types.h"

#include <array>
#include <vector>

namespace vd {

// Development (phase 13, spells): a spell waiting for its target with sample data. VD_UI_GALLERY=targeting:
// Blizzard's bar (in range, then out of range every few seconds) and the questions before an enchantment
// (BIND_ENCHANT, REPLACE_ENCHANT through the real PopupStack rules). targeting2: Instant Poison waiting for a
// weapon, the bags with the weapons lit and the rest dimmed. Taps go to the log ("Targeting gallery: ...").
class TargetingGallery {
public:
    void draw(DialogContext& c, u32 page, u64 nowMs);

private:
    void popups(DialogContext& c, u64 nowMs);

    PopupStack m_stack;
    std::array<f32, PopupStack::kSlots> m_heights{};
    std::array<PopupView, PopupStack::kSlots> m_views{};
    std::array<std::array<char, 400>, PopupStack::kSlots> m_texts{};
    u64 m_lastMs = 0;
    u64 m_emptySinceMs = 0;
    bool m_filled = false;
    ItemDrag m_drag;
    std::vector<ItemSlotView> m_cells;
};

} // namespace vd
