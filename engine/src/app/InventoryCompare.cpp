// Phase 15 (items): InventoryScreens' comparison tooltips (the same class as InventoryScreens.cpp): what an
// item's tooltip is compared with (game::compareSlots: ShoppingTooltip1 / 2), their lines under
// CURRENTLY_EQUIPPED, and where they go beside the tooltip (app/TooltipLayout).

#include "app/InventoryScreens.h"

#include "app/CompareTooltips.h"
#include "app/GameSession.h"
#include "core/Text.h"
#include "game/ItemCompare.h"
#include "ui/Widgets.h"

namespace vd {

void InventoryScreens::buildCompare(GameSession& session, const net::ItemQueryInfo& info,
                                    const game::ItemInstance* item, const game::TooltipContext& context) {
    m_compareCount = 0;
    // The tooltip of an equipped item is not compared (the desktop compares what it would replace).
    if (m_tipRef.kind == ItemRef::Kind::Slot && m_tipRef.bag == 255 &&
        m_tipRef.slot < game::kEquipmentSlotEnd) {
        return;
    }
    const game::Inventory& inv = session.play().inventory();
    const game::ItemInstance* off = inv.equipped(game::kSlotOffHand);
    const net::ItemQueryInfo* offInfo = off && off->entry ? session.item(off->entry) : nullptr;
    const u32 offType = offInfo ? offInfo->inventoryType : off ? 0xFFu : 0u; // 0xFF: something not known yet
    const game::CompareSlots slots = game::compareSlots(info.inventoryType, offType);
    for (u32 i = 0; i < slots.count && m_compareCount < m_compareTips.size(); ++i) {
        const game::ItemInstance* worn = inv.equipped(slots.slot[i]);
        if (!worn || worn->entry == 0 || (item && worn->guid == item->guid)) {
            continue;
        }
        const net::ItemQueryInfo* wornInfo = session.item(worn->entry); // asked for: shown once it arrives
        if (!wornInfo) {
            continue;
        }
        game::ItemTooltip& tip = m_compareTips[m_compareCount];
        game::buildItemTooltip(*wornInfo, worn, context, tip);
        std::vector<TooltipLineView>& lines = m_compareLines[m_compareCount];
        lines.clear();
        lines.push_back({text::get("CURRENTLY_EQUIPPED"), {}, ui::kGold, ui::kWhite}); // GameFontNormalSmall
        for (const game::TooltipLine& line : tip.lines()) {
            if (lines.size() < lines.capacity()) {
                lines.push_back({line.left, line.right, line.leftColor, line.rightColor, line.wrap,
                                 line.money, line.gapBefore});
            }
        }
        ++m_compareCount;
    }
}

void InventoryScreens::drawCompare(DialogContext& c, const TooltipPlacement& placement, u32 count) {
    for (u32 i = 0; i < count && i < m_compareCount; ++i) {
        compareTooltip(c, i, placement.compare[i], m_compareLines[i]);
    }
}

} // namespace vd
