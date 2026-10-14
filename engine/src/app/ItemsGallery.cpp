#include "app/ItemsGallery.h"

#include "app/CompareTooltips.h"
#include "app/DialogKit.h"
#include "app/ModelFrameGallery.h"
#include "app/TooltipLayout.h"
#include "app/UiResources.h"
#include "app/UnitMenuWindows.h"
#include "core/Clock.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;

constexpr u64 icon(const char* name) {
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

void logLine(const char* what, i32 value = 0) {
    VD_LOG_INFO("Items gallery: %s %d", what, value);
}

struct SampleItem {
    const char* icon;
    u32 count;
    u32 quality;
};
// What a level-12 human warrior carries.
constexpr SampleItem kBackpack[] = {
    {"INV_Sword_04", 1, 2},          {"INV_Misc_Food_14", 12, 1},
    {"INV_Drink_07", 5, 1},          {"INV_Potion_49", 3, 1},
    {"INV_Misc_Pelt_Wolf_01", 7, 1}, {nullptr, 0, 1},
    {"INV_Fabric_Linen_01", 14, 1},  {"INV_Misc_Bone_06", 4, 0},
    {"INV_Jewelry_Ring_05", 1, 2},   {nullptr, 0, 1},
    {"INV_Shirt_05", 1, 1},          {nullptr, 0, 1},
    {"INV_Misc_Book_09", 1, 1},      {nullptr, 0, 1},
    {"INV_Misc_Rune_01", 1, 1},      {nullptr, 0, 1},
};
constexpr const char* kBagIcons[5] = {nullptr, "INV_Misc_Bag_07", "INV_Misc_Bag_09", "INV_Misc_Bag_10",
                                      "INV_Misc_Bag_08"};
constexpr const char* kBagNames[5] = {nullptr, "Small Brown Pouch", "Linen Bag", "Woolen Bag",
                                      "Red Linen Bag"};
constexpr u32 kBagSizes[5] = {16, 6, 10, 12, 16};

TooltipLineView line(std::string_view left, u32 color = ui::kWhite, std::string_view right = {}) {
    TooltipLineView l;
    l.left = left;
    l.right = right;
    l.leftColor = color;
    l.rightColor = ui::kWhite;
    return l;
}

} // namespace

void ItemsGallery::sample() {
    if (m_sampled) {
        return;
    }
    m_sampled = true;
    for (u32 b = 0; b < 5; ++b) {
        m_sizes.bags[b] = kBagSizes[b];
        m_cells[b].clear();
        for (u32 i = 0; i < kBagSizes[b]; ++i) {
            ItemSlotView cell;
            cell.ref = {ItemRef::Kind::Slot, static_cast<u8>(b == 0 ? 255 : 18 + b),
                        static_cast<u8>(b == 0 ? 23 + i : i), 0};
            const SampleItem* item =
                b == 0 ? &kBackpack[i] : (i % 3 == 0 ? &kBackpack[(i + b) % 16] : nullptr);
            if (item && item->icon) {
                cell.empty = false;
                cell.iconHash = icon(item->icon);
                cell.count = item->count;
                cell.quality = item->quality;
            }
            m_cells[b].push_back(cell);
        }
    }
    m_sizes.keyring = keyringSize(12);
    m_cells[5].clear();
    for (u32 i = 0; i < m_sizes.keyring; ++i) {
        ItemSlotView cell;
        cell.ref = {ItemRef::Kind::Slot, 255, static_cast<u8>(81 + i), 0};
        if (i == 0) {
            cell.empty = false;
            cell.iconHash = icon("INV_Misc_Key_03");
        }
        m_cells[5].push_back(cell);
    }
}

void ItemsGallery::bagButton(i32 button, bool shift) {
    sample();
    const i32 id = button == 5 ? kKeyringContainer : button;
    bool shown = false;
    for (u32 i = 0; i < m_shownCount; ++i) {
        shown = shown || m_views[i].id == id;
    }
    if (!shift && m_stack.isOpen(id) && !shown) {
        m_stack.raise(id); // out of sight for lack of room: back as the newest
    } else if (shift && button >= 0 && button <= 4) {
        m_stack.openAllBags(m_sizes);
    } else if (button == 0) {
        m_stack.toggleBackpack(m_sizes);
    } else if (button >= 1 && button <= 4) {
        m_stack.toggleBag(button, m_sizes);
    } else if (button == 5) {
        m_stack.toggleKeyring(m_sizes);
    }
    logLine("bag button", button);
}

void ItemsGallery::bagBarState(MicroMenuView& v) const {
    v.bags = {assetPathHash("Interface\\Buttons\\Button-Backpack-Up.blp"), icon(kBagIcons[1]),
              icon(kBagIcons[2]), icon(kBagIcons[3]), icon(kBagIcons[4])};
    v.keyring = true;
    for (u32 i = 0; i < m_shownCount; ++i) { // checked: the frames on screen
        if (m_stack.isOpen(m_views[i].id)) {
            v.bagOpen[m_views[i].id == kKeyringContainer ? 5u : static_cast<u32>(m_views[i].id)] = true;
        }
    }
}

void ItemsGallery::drawContainers(DialogContext& c) {
    VD_PROFILE_ZONE("ItemsGallery::containers");
    sample();
    u32 count = 0;
    std::array<f32, kContainerFrames> heights{};
    for (const i32 id : m_stack.order()) {
        const u32 bag = id == kKeyringContainer ? 5u : static_cast<u32>(id);
        ContainerFrameView& v = m_views[count];
        v = ContainerFrameView{};
        v.id = id;
        v.cells = m_cells[bag];
        v.name = id == kKeyringContainer ? text::get("KEYRING")
                 : id == 0               ? text::get("BACKPACK_TOOLTIP")
                                         : std::string_view(kBagNames[bag]);
        v.portrait = id == kKeyringContainer
                         ? assetPathHash("Interface\\ContainerFrame\\KeyRing-Bag-Icon.blp")
                     : id == 0 ? 0
                               : icon(kBagIcons[bag]);
        v.money = 123456;
        heights[count] = containerGeometry(id, static_cast<u32>(v.cells.size())).height;
        ++count;
    }
    if (count == 0) {
        return;
    }
    const kit::Metrics m = kit::metrics(c);
    const Rect area = c.panel.w > 0.0f ? c.panel : c.safeArea;
    const f32 largest = m.item / 37.0f, smallest = std::min(largest, c.ui.px(ui::kUnitDp));
    std::array<u32, kContainerFrames> shown{};
    m_shownCount = shownContainers({heights.data(), count}, area, smallest, shown);
    for (u32 i = 0; i < m_shownCount; ++i) {
        m_views[i] = m_views[shown[i]];
        heights[i] = heights[shown[i]];
    }
    const std::span<const f32> visible{heights.data(), m_shownCount};
    const f32 scale = containerScale(visible, area, largest, smallest);
    layoutContainers(visible, area, scale, m_rects);
    m_drag.track(c.ui.touches());
    ItemWindowResult items;
    ContainerFramesResult r;
    containerFrames(c, {m_views.data(), m_shownCount}, {m_rects.data(), m_shownCount}, scale, m_drag, items,
                    r);
    if (items.tapped.valid()) {
        logLine("cell tapped (bag)", items.tapped.bag);
    }
    if (items.dropFrom.valid()) {
        logLine("drop on slot", items.dropOn.slot);
    }
    if (r.closed != ContainerFramesResult::kNone) {
        m_stack.closeBag(r.closed);
        logLine("frame closed", r.closed);
    }
    if (r.portrait != ContainerFramesResult::kNone) {
        logLine("portrait", r.portrait);
    }
    m_drag.finish();
}

void ItemsGallery::containersPage(DialogContext& c) {
    sample();
    if (!m_opened) { // everything open, as OpenAllBags and the keyring
        m_opened = true;
        m_stack.openAllBags(m_sizes, true);
        m_stack.toggleKeyring(m_sizes);
    }
    // The bag bar where the game puts it (unfolded from the rim's backpack), with the sample's icons.
    const MicroMenuLayout layout = microMenuLayout(c);
    m_micro.bagsUnfolded = true;
    MicroMenuView v;
    v.level = 12;
    v.freeSlots = 21;
    v.bagsOpen = m_stack.any();
    bagBarState(v);
    const MicroMenuResult r = microMenu(c, layout, v, m_micro);
    if (r.bag >= 0) {
        bagButton(r.bag, r.bagShift);
    }
    drawContainers(c);
}

void ItemsGallery::comparePage(DialogContext& c) {
    // A ring from the loot (its tooltip with Try On's neighbours) and both rings the player wears.
    static constexpr u32 kGreen = ui::rgba(30, 255, 0);
    static constexpr u32 kBlue = ui::rgba(0, 112, 221);
    const TooltipLineView tried[] = {line("Band of the Unicorn", kGreen), line("Finger"), line("+4 Stamina"),
                                     line("+3 Spirit"), line("Requires Level 10")};
    const TooltipLineView first[] = {line(text::get("CURRENTLY_EQUIPPED"), ui::kGold),
                                     line("Seal of Wrynn", kBlue),
                                     line("Binds when picked up"),
                                     line("Unique"),
                                     line("Finger"),
                                     line("+5 Strength"),
                                     line("+4 Stamina")};
    const TooltipLineView second[] = {line(text::get("CURRENTLY_EQUIPPED"), ui::kGold),
                                      line("Simple Ring", kGreen), line("Finger"), line("+2 Agility")};
    const std::string_view actions[] = {text::get("VD_INV_COMPARE")};
    ui::Gui g{c.ui, c.draw};
    const kit::Metrics m = kit::metrics(c);
    const Rect cell{c.safeArea.x + g.px(40.0f), c.safeArea.y + g.px(120.0f), m.item, m.item};
    ui::SlotView slot;
    slot.icon = c.resources.icon(icon("INV_Jewelry_Ring_02"));
    slot.quality = 2;
    ui::itemSlot(g, ui::widgetId("itemsGallery.cell"), cell, slot);
    TooltipView view;
    view.iconHash = icon("INV_Jewelry_Ring_02");
    view.lines = tried;
    view.actions = actions;
    view.anchor = cell;
    const Rect size = itemTooltipSize(c, view);
    const f32 heights[] = {compareTooltipHeight(c, first, size.w), compareTooltipHeight(c, second, size.w)};
    const TooltipPlacement p =
        placeTooltips(c.safeArea, cell, size.w, size.h, heights, c.ui.px(8.0f), c.ui.px(ui::kUnitDp) * 10.0f);
    view.placed = p.main;
    if (itemTooltip(c, view).action == 0) {
        logLine("compare");
    }
    compareTooltip(c, 0, p.compare[0], first);
    compareTooltip(c, 1, p.compare[1], second);
}

TextureHandle ItemsGallery::model(ModelViewId id, f32 facing) {
    return m_modelFrames ? m_modelFrames->sample(id, ModelFrameGallery::Sample::Human, facing)
                         : TextureHandle{};
}

void ItemsGallery::dressUpPage(DialogContext& c) {
    DressUpView v;
    v.title = text::get("DRESSUP_FRAME");
    v.instructions = text::get("VD_DRESSUP_INSTRUCTIONS");
    for (u32 i = 0; i < 4; ++i) {
        v.backgrounds[i] = dressUpBackground(1, i + 1);
    }
    v.model = model(ModelViewId::DressUp, m_dressUpFrame.facing);
    const DressUpResult r = dressUpWindow(c, v, m_dressUpFrame);
    if (r.reset) {
        logLine("dressing room reset");
    }
    if (r.closed) {
        logLine("dressing room closed");
    }
}

void ItemsGallery::drawAuctionDock(DialogContext& c, const ui::Rect& place) {
    DressUpView v;
    for (u32 i = 0; i < 4; ++i) {
        v.backgrounds[i] = dressUpBackground(2, i + 1);
    }
    v.model = model(ModelViewId::AuctionDressUp, m_auctionFrame.facing);
    const DressUpResult r = auctionDressUpWindow(c, place, v, m_auctionFrame);
    if (r.reset || r.closed) {
        logLine(r.reset ? "auction dressing room reset" : "auction dressing room closed");
    }
}

void ItemsGallery::masterLootPage(DialogContext& c, bool raid) {
    // The LootFrame of a boss with an epic, and GroupLootDropDown hanging from it.
    const LootItemView items[] = {
        {{icon("INV_Sword_27"), "Gutwrencher", 1, 4, 1}, 0, text::get("LOOT_MASTER_LOOTER")},
        {{icon("INV_Chest_Cloth_09"), "Robe of the Magi", 1, 3, 2}, 1, text::get("LOOT_MASTER_LOOTER")},
        {{icon("INV_Misc_Gem_Variety_01"), "Bloodstone", 2, 2, 3}, 2, {}},
    };
    const LootView view{"Mor'Ladim", items, 0, 0};
    DialogContext lc = c;
    const LootResult loot = lootWindow(lc, view);
    if (loot.take >= 0) {
        m_menuOpen = true;
        logLine("loot item tapped", loot.take);
    }
    if (!m_menuOpen) {
        return;
    }
    static constexpr const char* kParty[] = {"Arthel", "Aeryn", "Galen", "Brakk", "Tolvir"};
    static constexpr const char* kGroup1[] = {"Arthel", "Aeryn", "Galen", "Brakk", "Tolvir"};
    static constexpr const char* kGroup2[] = {"Mirelle", "Durgan", "Shaela"};
    std::array<DropDownItemView, 8> level1{};
    std::array<DropDownItemView, 5> level2{};
    u32 n1 = 0, n2 = 0;
    char groups[2][24];
    if (raid) {
        level1[n1++] = {text::get("GIVE_LOOT"), 0, true};
        for (u32 gi = 0; gi < 2; ++gi) {
            const std::string_view label = text::get("GROUP");
            std::snprintf(groups[gi], sizeof(groups[gi]), "%.*s %u", static_cast<int>(label.size()),
                          label.data(), gi + 1);
            DropDownItemView item{groups[gi]};
            item.hasArrow = true;
            level1[n1++] = item;
        }
        if (m_masterGroup == 0) {
            for (const char* name : kGroup1) {
                level2[n2++] = {name};
            }
        } else if (m_masterGroup == 1) {
            for (const char* name : kGroup2) {
                level2[n2++] = {name};
            }
        }
    } else {
        for (const char* name : kParty) {
            level1[n1++] = {name};
        }
    }
    // Under the first item's button (LootFrame.xml: 24, 80 of the 256-unit frame placed under the player
    // frame).
    ui::Gui g{c.ui, c.draw};
    DropDownMenuView menu;
    menu.x = c.safeArea.x + g.unit(24.0f) * 1.25f;
    menu.y = c.safeArea.y + g.unit(104.0f + 80.0f + 37.0f) * 1.25f;
    menu.level1 = {level1.data(), n1};
    menu.level2 = {level2.data(), n2};
    menu.level2Row = raid && m_masterGroup >= 0 ? m_masterGroup + 1 : -1;
    const DropDownMenuResult r = dropDownMenu(c, "itemsGallery.master", menu);
    if (r.toggled >= 1) {
        m_masterGroup = m_masterGroup == r.toggled - 1 ? -1 : r.toggled - 1;
    }
    if (r.picked >= 0) {
        logLine(r.level == 1 ? "give to party member" : "give to raid member", r.picked);
        m_menuOpen = false;
    }
    if (r.closed) {
        m_menuOpen = false;
    }
}

void ItemsGallery::lootPopupsPage(DialogContext& c) {
    if (!m_popups.any()) {
        m_popups.show(PopupKind::ConfirmLootRoll, false, 1);
        if (Popup* p = m_popups.show(PopupKind::ConfirmLootDistribution, false, 2)) {
            p->setName("|cffa335eeGutwrencher|r");
            p->setName2("Aeryn");
        }
    }
    for (u32 i = 0; i < PopupStack::kSlots; ++i) {
        const Popup& p = m_popups.slots()[i];
        PopupView& v = m_popupViews[i];
        v = PopupView{};
        if (!p.shown()) {
            continue;
        }
        const PopupInfo& info = popupInfo(p.kind);
        v.text = p.kind == PopupKind::ConfirmLootDistribution
                     ? text::format(m_popupTexts[i], info.text, p.nameView(), p.name2View())
                     : text::format(m_popupTexts[i], info.text);
        v.button1 = text::get(info.button1);
        v.button2 = text::get(info.button2);
    }
    const PopupTaps taps = popupStack(c, m_popupViews, m_popupHeights);
    if (taps.slot >= 0) {
        m_popups.click(static_cast<u32>(taps.slot), taps.button);
    }
    PopupOutcome o;
    while (m_popups.takeOutcome(o)) {
        logLine("popup answered", static_cast<i32>(o.end));
    }
}

void ItemsGallery::bankBagsPage(DialogContext& c) {
    // The bank with two bank bags bought and both open as ContainerFrames (their buttons toggle them), the player's
    // bags in the other pane.
    sample();
    std::array<ItemSlotView, 6> bagButtons{};
    for (u32 i = 0; i < 6; ++i) {
        bagButtons[i].ref = {ItemRef::Kind::Slot, 255, static_cast<u8>(63 + i), 0};
        if (i < 2) {
            bagButtons[i].empty = false;
            bagButtons[i].iconHash = icon(kBagIcons[i + 1]);
        }
    }
    std::array<BagSectionView, 1> sections{};
    sections[0] = {text::get("VD_INV_BANK"), 0, 16, false};
    std::array<ContainerFrameView, 2> frames{};
    u32 count = 0;
    for (u32 i = 0; i < 2; ++i) {
        if (m_bankFrames & (1u << i)) {
            frames[count].id = 5 + static_cast<i32>(i);
            frames[count].name = kBagNames[i + 1];
            frames[count].portrait = icon(kBagIcons[i + 1]);
            frames[count].cells = m_cells[i + 1];
            ++count;
        }
    }
    BankView view;
    view.bankBags = bagButtons;
    view.bankBagSlots = 2;
    view.nextSlotPrice = 10000;
    view.sections = sections;
    view.cells = m_cells[0];
    view.bagFrames = frames.data();
    view.bagFrameCount = count;
    m_drag.track(c.ui.touches());
    ContainerFramesResult r;
    const ItemWindowResult items = bankWindow(c, view, m_drag, &r);
    if (items.tapped.valid() && items.tapped.slot >= 63 && items.tapped.slot < 65) {
        m_bankFrames ^= 1u << (items.tapped.slot - 63);
        logLine("bank bag toggled", items.tapped.slot);
    }
    if (r.closed != ContainerFramesResult::kNone) {
        m_bankFrames &= ~(1u << static_cast<u32>(r.closed - 5));
        logLine("bank bag closed", r.closed);
    }
    m_drag.finish();
}

void ItemsGallery::draw(DialogContext& c, Page page) {
    // VD_TRACE_ITEMS=1 (development): the page's CPU time (building its draw list), mean and worst of 300
    // frames.
    static const bool trace = VD_DEV_ENV("VD_TRACE_ITEMS") != nullptr;
    const Clock::time_point start = Clock::now();
    drawPage(c, page);
    if (trace) {
        const f64 ms = secondsBetween(start, Clock::now()) * 1000.0;
        m_timeSum += ms;
        m_timeMax = std::max(m_timeMax, ms);
        if (++m_timeFrames == 300) {
            VD_LOG_INFO("Items gallery: page %u: %.3f ms mean, %.3f ms worst per frame",
                        static_cast<u32>(page), m_timeSum / 300.0, m_timeMax);
            m_timeSum = m_timeMax = 0.0;
            m_timeFrames = 0;
        }
    }
}

void ItemsGallery::drawPage(DialogContext& c, Page page) {
    switch (page) {
    case Page::Containers: containersPage(c); break;
    case Page::Compare: comparePage(c); break;
    case Page::DressUp: dressUpPage(c); break;
    case Page::AuctionDressUp:
        drawAuctionDock(c, c.safeArea);
        break; // UiGallery draws it beside the auction house
    case Page::MasterLoot: masterLootPage(c, false); break;
    case Page::MasterLootRaid: masterLootPage(c, true); break;
    case Page::LootPopups: lootPopupsPage(c); break;
    case Page::BankBags: bankBagsPage(c); break;
    }
}

} // namespace vd
