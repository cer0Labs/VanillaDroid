#include "app/InventoryGallery.h"

#include "app/DialogKit.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Text.h"
#include "game/ItemActions.h"
#include "game/ItemTooltip.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vd {
namespace {

using ui::rgba;

constexpr u64 iconHash(const char* name) {
    u64 hash = assetPathHash("Interface\\Icons\\");
    for (const char* c = name; *c; ++c) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(*c));
        hash *= kFnvPrime;
    }
    for (const char c : std::string_view(".blp")) {
        hash ^= static_cast<u8>(c);
        hash *= kFnvPrime;
    }
    return hash;
}
constexpr u64 slotArt(const char* name) {
    u64 hash = assetPathHash("Interface\\PaperDoll\\UI-PaperDoll-Slot-");
    for (const char* c = name; *c; ++c) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(*c));
        hash *= kFnvPrime;
    }
    for (const char c : std::string_view(".blp")) {
        hash ^= static_cast<u8>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

struct Sample {
    const char* icon;
    const char* name;
    u32 count;
    u32 quality;
};
// The backpack of a level-5 human warrior in Northshire, then a 6-slot pouch and a quiver.
constexpr Sample kBackpack[16] = {
    {"INV_Misc_Rune_01", "Hearthstone", 1, 1},     {"INV_Fabric_Linen_01", "Linen Cloth", 7, 1},
    {"INV_Misc_Food_15", "Tough Hunk of Bread", 5, 1}, {"INV_Drink_07", "Refreshing Spring Water", 6, 1},
    {"INV_Potion_49", "Minor Healing Potion", 3, 1}, {"INV_Misc_Bone_06", "Broken Fang", 2, 0},
    {"INV_Bracer_02", "Wolfskin Bracers", 1, 2},   {nullptr, nullptr, 0, 0},
    {"INV_Misc_Candle_02", "Large Candle", 4, 1},  {"INV_Misc_Note_01", "A Letter to Arthel", 1, 1},
    {nullptr, nullptr, 0, 0},                      {"INV_Misc_Pelt_Wolf_Ruin_02", "Ruined Pelt", 3, 0},
    {nullptr, nullptr, 0, 0},                      {nullptr, nullptr, 0, 0},
    {"INV_Misc_Shell_01", "Small Barnacled Clam", 1, 1}, {nullptr, nullptr, 0, 0},
};
constexpr Sample kPouch[6] = {{"INV_Misc_Food_14", "Tough Wolf Meat", 8, 1},
                              {nullptr, nullptr, 0, 0},
                              {"INV_Misc_Book_09", "Old Book", 1, 1},
                              {nullptr, nullptr, 0, 0},
                              {nullptr, nullptr, 0, 0},
                              {nullptr, nullptr, 0, 0}};
constexpr Sample kQuiver[4] = {{"INV_Ammo_Arrow_01", "Rough Arrow", 200, 1},
                               {"INV_Ammo_Arrow_01", "Rough Arrow", 143, 1},
                               {nullptr, nullptr, 0, 0},
                               {nullptr, nullptr, 0, 0}};
ItemSlotView cellOf(const Sample& s, ItemRef ref) {
    ItemSlotView v;
    v.ref = ref;
    if (s.icon) {
        v.empty = false;
        v.iconHash = iconHash(s.icon);
        v.count = s.count;
        v.quality = s.quality;
    }
    return v;
}

constexpr std::string_view kLetter =
    "Arthel,$B$BThe kobolds of Echo Ridge have been stealing candles from the abbey again.  Brother Neals "
    "says they use them to light the mines, and that there must be dozens of them down there by now.$B$BBe "
    "careful.  And bring back what you can.$B$B- Marshal McBride";

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

} // namespace

void InventoryGallery::bags(BagsView& out) {
    m_bagBar.clear();
    m_cells.clear();
    m_sections.clear();
    ItemSlotView backpack;
    backpack.ref = {ItemRef::Kind::Slot, 255, 255, 0};
    backpack.empty = false;
    backpack.iconHash = assetPathHash("Interface\\Buttons\\Button-Backpack-Up.blp");
    m_bagBar.push_back(backpack);
    m_bagBar.push_back(cellOf({"INV_Misc_Bag_07", "Small Brown Pouch", 1, 1}, {ItemRef::Kind::Slot, 255, 19, 0}));
    m_bagBar.push_back(cellOf({"INV_Misc_Quiver_01", "Light Quiver", 1, 1}, {ItemRef::Kind::Slot, 255, 20, 0}));
    for (u8 i = 21; i < 23; ++i) {
        ItemSlotView empty;
        empty.ref = {ItemRef::Kind::Slot, 255, i, 0};
        empty.emptyArt = slotArt("Bag");
        m_bagBar.push_back(empty);
    }
    const auto add = [&](std::string_view title, std::span<const Sample> items, u8 bag, u8 firstSlot, bool special) {
        BagSectionView section{title, static_cast<u32>(m_cells.size()), static_cast<u32>(items.size()), special};
        for (u32 i = 0; i < items.size(); ++i) {
            m_cells.push_back(cellOf(items[i], {ItemRef::Kind::Slot, bag, static_cast<u8>(firstSlot + i), 0}));
        }
        m_sections.push_back(section);
    };
    add(text::get("BACKPACK_TOOLTIP"), kBackpack, 255, 23, false);
    add("Small Brown Pouch", kPouch, 19, 0, false);
    add("Light Quiver", kQuiver, 20, 0, true);
    m_cells[6].usable = false; // a level the player lacks: red
    m_cells[9].cooldown = 0.4f;
    out.bagBar = m_bagBar;
    out.sections = m_sections;
    out.cells = m_cells;
    out.money = 1 * 10000 + 23 * 100 + 45;
    out.freeSlots = 13;
    out.totalSlots = 26;
}

void InventoryGallery::apply(const ItemWindowResult& r, const char* window) {
    char text[128];
    if (r.tapped.valid()) {
        std::snprintf(text, sizeof(text), "%s tap kind %u bag %u slot %u index %u", window,
                      static_cast<u32>(r.tapped.kind), r.tapped.bag, r.tapped.slot, r.tapped.index);
        logResult(text);
        m_tooltip = true;
        m_tipRef = r.tapped;
        m_tipAnchor = r.tappedRect;
    }
    if (r.dropFrom.valid()) {
        std::snprintf(text, sizeof(text), "%s drop %u/%u -> %u/%u zone %d", window, r.dropFrom.bag, r.dropFrom.slot,
                      r.dropOn.bag, r.dropOn.slot, r.dropZone);
        logResult(text);
    }
    if (r.button >= 0) {
        std::snprintf(text, sizeof(text), "%s button %d", window, r.button);
        logResult(text);
    }
    if (r.closed) {
        std::snprintf(text, sizeof(text), "%s closed", window);
        logResult(text);
    }
}

void InventoryGallery::draw(DialogContext& c, Page page) {
    m_drag.track(c.ui.touches());
    switch (page) {
    case Page::Bags: {
        BagsView view;
        bags(view);
        apply(bagsWindow(c, view, m_drag), "bags");
        break;
    }
    case Page::Tooltip: {
        BagsView view;
        bags(view);
        const ItemWindowResult r = bagsWindow(c, view, m_drag);
        apply(r, "bags");
        m_tooltip = true;
        if (!m_tipRef.valid()) {
            m_tipRef = view.cells[1].ref;
        }
        break;
    }
    case Page::Vendor: {
        m_vendor.clear();
        const Sample goods[] = {
            {"INV_Misc_Food_15", "Tough Hunk of Bread", 1, 1}, {"INV_Drink_07", "Refreshing Spring Water", 1, 1},
            {"INV_Misc_Food_04", "Darnassian Bleu", 1, 1},     {"INV_Potion_49", "Minor Healing Potion", 1, 1},
            {"INV_Misc_Quiver_01", "Light Quiver", 1, 1},      {"INV_Ammo_Arrow_01", "Rough Arrow", 200, 1},
            {"INV_Misc_Bag_09", "Small Black Pouch", 1, 1},    {"INV_Fabric_Linen_01", "Linen Bandage", 1, 1},
        };
        const u32 prices[] = {25, 25, 25, 100, 400, 10, 250, 50};
        if (m_buyback) {
            VendorRowView row;
            row.cell = cellOf(kBackpack[5], {ItemRef::Kind::Buyback, 0, 0, 0});
            row.name = "Broken Fang";
            row.price = 4;
            m_vendor.push_back(row);
        } else {
            for (u32 i = 0; i < std::size(goods); ++i) {
                VendorRowView row;
                row.cell = cellOf(goods[i], {ItemRef::Kind::Vendor, 0, 0, i});
                row.name = goods[i].name;
                row.price = prices[i];
                row.buyCount = goods[i].count;
                row.limited = i == 4 || i == 3;
                row.stock = i == 3 ? 2 : 0;
                row.cell.dimmed = i == 4;
                row.affordable = i != 6;
                m_vendor.push_back(row);
            }
        }
        VendorView view;
        view.npcName = "Brother Danil";
        view.buyback = m_buyback;
        view.rows = m_vendor;
        view.canRepair = true;
        view.repairAllCost = 1 * 100 + 37;
        view.junkValue = 9;
        view.money = 12345;
        bags(view.bags);
        const ItemWindowResult r = vendorWindow(c, view, m_drag);
        if (r.tab >= 0) {
            m_buyback = r.tab == 1;
        }
        apply(r, "vendor");
        break;
    }
    case Page::Bank: {
        m_bankBags.clear();
        m_bankCells.clear();
        m_bankSections.clear();
        m_bankBags.push_back(cellOf({"INV_Misc_Bag_09", "Small Black Pouch", 1, 1}, {ItemRef::Kind::Slot, 255, 63, 0}));
        for (u8 i = 1; i < 6; ++i) {
            ItemSlotView empty;
            empty.ref = {ItemRef::Kind::Slot, 255, static_cast<u8>(63 + i), 0};
            empty.emptyArt = slotArt("Bag");
            m_bankBags.push_back(empty);
        }
        m_bankSections.push_back({text::get("VD_INV_BANK"), 0, 24, false});
        for (u32 i = 0; i < 24; ++i) {
            const Sample s = i == 0   ? Sample{"INV_Misc_Book_08", "Old Book", 1, 1}
                             : i == 1 ? Sample{"INV_Fabric_Linen_01", "Linen Cloth", 20, 1}
                             : i == 5 ? Sample{"INV_Jewelry_Ring_05", "Seal of Wrynn", 1, 3}
                                      : Sample{nullptr, nullptr, 0, 0};
            m_bankCells.push_back(cellOf(s, {ItemRef::Kind::Slot, 255, static_cast<u8>(39 + i), 0}));
        }
        m_bankSections.push_back({"Small Black Pouch", 24, 6, false});
        for (u8 i = 0; i < 6; ++i) {
            m_bankCells.push_back(cellOf(i == 0 ? Sample{"INV_Misc_Food_14", "Tough Wolf Meat", 12, 1}
                                                : Sample{nullptr, nullptr, 0, 0},
                                         {ItemRef::Kind::Slot, 63, i, 0}));
        }
        BankView view;
        view.bankBags = m_bankBags;
        view.bankBagSlots = 1;
        view.nextSlotPrice = 10000;
        view.sections = m_bankSections;
        view.cells = m_bankCells;
        bags(view.bags);
        apply(bankWindow(c, view, m_drag), "bank");
        break;
    }
    case Page::Reading: {
        ReadingView view;
        view.title = "A Letter to Arthel";
        view.text = m_page == 0 ? kLetter : std::string_view("P.S.  The candles burn blue in the deep tunnels.");
        view.page = m_page;
        view.pageCount = 2;
        const ReadingResult r = readingWindow(c, view);
        if (r.turn != 0) {
            m_page = static_cast<u32>(std::clamp(static_cast<i32>(m_page) + r.turn, 0, 1));
            logResult(r.turn > 0 ? "reading next" : "reading previous");
        }
        if (r.closed) {
            logResult("reading closed");
        }
        break;
    }
    case Page::Quantity: {
        QuantityView view;
        view.title = text::get("VD_INV_BUY");
        view.itemName = "Rough Arrow";
        view.iconHash = iconHash("INV_Ammo_Arrow_01");
        view.min = 1;
        view.max = 20;
        view.unitPrice = 10;
        view.confirm = view.title;
        const QuantityResult r = quantityDialog(c, view, m_quantity);
        if (r.confirmed) {
            char text[48];
            std::snprintf(text, sizeof(text), "quantity %u", m_quantity);
            logResult(text);
        }
        if (r.cancelled) {
            logResult("quantity cancelled");
        }
        break;
    }
    case Page::Durability: {
        // The figure as the HUD shows it, and big in the middle.
        constexpr u8 kParts[11] = {1, 0, 2, 0, 0, 1, 0, 0, 2, 0, 1};
        const ui::Rect& safe = c.safeArea;
        const ui::Rect hud = durabilityHudRect(c);
        durabilityFigure(c, hud.x, hud.y, hud.h, kParts, false);
        if (backpackButton(c).clicked) {
            logResult("backpack button");
        }
        if (durabilityFigure(c, safe.centerX() - safe.h * 0.3f, safe.y + safe.h * 0.2f, safe.h * 0.6f, kParts, true)) {
            logResult("durability tapped");
        }
        break;
    }
    }

    // The tooltip over the window, its actions; the carried item last.
    if (m_tooltip && (page == Page::Bags || page == Page::Tooltip || page == Page::Vendor ||
                      page == Page::Bank)) {
        // The sample sword's lines as game/ItemTooltip writes them.
        char damage[64], speed[32], dps[64], durability[64], use[192];
        std::string_view useText = text::get("ITEM_SPELL_TRIGGER_ONUSE");
        const std::string_view spell =
            " Restores 70 to 90 health over 10 sec. A long description that has to wrap over two lines.";
        const usize useLength = std::min(useText.size(), sizeof(use) - spell.size() - 1);
        std::memcpy(use, useText.data(), useLength);
        std::memcpy(use + useLength, spell.data(), spell.size());
        useText = {use, useLength + spell.size()};
        char description[96];
        const TooltipLineView lines[] = {
            {"Militia Shortsword", {}, ui::qualityColor(1)},
            {game::inventoryTypeName(game::kInvTypeWeaponMainHand), "Sword"}, // ItemSubClass.dbc
            {text::format(damage, "DAMAGE_TEMPLATE", 6, 12), text::format(speed, "VD_INV_WEAPON_SPEED", 2.2f)},
            {text::format(dps, "DPS_TEMPLATE", 4.1f)},
            {text::format(durability, "DURABILITY_TEMPLATE", 0, 20), {}, ui::kRed},
            {useText, {}, rgba(26, 255, 26), ui::kWhite, true},
            {text::format(description, "VD_INV_ITEM_DESCRIPTION", "Standard issue of the Northshire militia."), {},
             ui::kGold, ui::kWhite, true},
            {text::get("VD_INV_SELL_PRICE"), {}, ui::kWhite, ui::kWhite, false, 52},
        };
        const std::string_view actions[] = {game::actionLabel(game::ItemAction::Equip),
                                            game::actionLabel(game::ItemAction::Sell),
                                            game::actionLabel(game::ItemAction::Repair),
                                            game::actionLabel(game::ItemAction::Destroy)};
        TooltipView view;
        view.iconHash = iconHash("INV_Sword_04");
        view.lines = lines;
        view.actions = actions;
        view.anchor = m_tipAnchor;
        const TooltipResult r = itemTooltip(c, view);
        if (r.action >= 0) {
            char text[48];
            std::snprintf(text, sizeof(text), "tooltip action %d", r.action);
            logResult(text);
        }
        if (r.closed || r.action >= 0) {
            m_tooltip = page == Page::Tooltip && !r.closed;
            logResult("tooltip closed");
        }
    }
    if (m_drag.active) {
        ui::Gui g{c.ui, c.draw};
        const f32 size = kit::metrics(c).item * 1.1f;
        if (const std::optional<u32> icon = m_drag.iconHash ? c.resources.icon(m_drag.iconHash) : std::nullopt) {
            g.draw.icon({m_drag.x - size * 0.5f, m_drag.y - size * 1.25f, size, size}, *icon, rgba(255, 255, 255, 230));
        }
    }
    if (m_drag.active && m_drag.released && !m_drag.taken) {
        logResult("drag dropped nowhere");
    }
    m_drag.finish();
}

} // namespace vd
