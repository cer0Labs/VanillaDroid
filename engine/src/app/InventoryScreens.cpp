#include "app/InventoryScreens.h"

#include "app/CharacterScreens.h"
#include "app/ChatLinkInsert.h"
#include "app/CompareTooltips.h"
#include "app/DialogKit.h"
#include "app/GameSession.h"
#include "app/PopupScreens.h"
#include "app/SpellTargetingScreens.h"
#include "app/UiResources.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/ChatLinks.h"
#include "game/GameData.h"
#include "game/ItemActions.h"
#include "game/ItemData.h"
#include "game/SocialText.h"
#include "game/UnitInfo.h"
#include "net/ItemMessages.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace vd {
namespace {

namespace uf = net::uf;
using game::ItemInstance;
using game::ItemPos;
using ui::rgba;

constexpr u32 kErrorColor = rgba(255, 26, 26);
constexpr u32 kInfoColor = rgba(255, 255, 0); // UIErrorsFrame.lua UI_INFO_MESSAGE
constexpr u32 kLowDurabilityPercent = 20; // the cell's yellow underline, as the DurabilityFrame's yellow
// Pseudo slots of bag 255 the windows use as drop targets: never real player slots (those end at 97).
constexpr u8 kBackpackCell = 255; // the backpack in the bag bar: "into the backpack"
constexpr u8 kAmmoCell = 200;     // the character window's ammo slot
constexpr f32 kWindowReach = 10.0f; // the vendor and bank close beyond this many yards (as the loot window)
// What each window's zones are (InventoryWindows: the zone numbers of drops not on a cell).
using Zone = game::ItemPlace::Kind;
constexpr Zone kEquipZones[] = {Zone::Equip};
constexpr Zone kVendorZones[] = {Zone::Sell, Zone::Bags};
constexpr Zone kBankZones[] = {Zone::Bank, Zone::Bags};

constexpr u64 hashOf(const char* prefix, const char* name) {
    u64 hash = assetPathHash(prefix);
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
    return hashOf("Interface\\PaperDoll\\UI-PaperDoll-Slot-", name);
}
static_assert(slotArt("Head") == assetPathHash("Interface\\PaperDoll\\UI-PaperDoll-Slot-Head.blp"));

// GetInventorySlotInfo's textures for the 19 equipment slots, then the ammo slot.
constexpr u64 kSlotArt[20] = {slotArt("Head"),     slotArt("Neck"),   slotArt("Shoulder"),      slotArt("Shirt"),
                              slotArt("Chest"),    slotArt("Waist"),  slotArt("Legs"),          slotArt("Feet"),
                              slotArt("Wrists"),   slotArt("Hands"),  slotArt("Finger"),        slotArt("Finger"),
                              slotArt("Trinket"),  slotArt("Trinket"), slotArt("Chest"),        slotArt("MainHand"),
                              slotArt("SecondaryHand"), slotArt("Ranged"), slotArt("Tabard"),   slotArt("Ammo")};
constexpr u64 kBagSlotArt = slotArt("Bag");
constexpr u64 kRelicSlotArt = slotArt("Relic"); // phase 15: the ranged slot of a class with a relic slot
constexpr u64 kBackpackIcon = assetPathHash("Interface\\Buttons\\Button-Backpack-Up.blp");

// The client's error texts for InventoryResult (GlobalStrings.lua), as vMaNGOS' ItemDefines.h names them for each
// value. 59 (EQUIP_ERR_NONE) is the server's way to clear a pending move: no text. Values beyond the table show as
// "bag full", as the client does.
constexpr const char* kInventoryErrors[] = {
    nullptr,
    "ERR_CANT_EQUIP_LEVEL_I",
    "ERR_CANT_EQUIP_SKILL",
    "ERR_WRONG_SLOT",
    "ERR_BAG_FULL",
    "ERR_BAG_IN_BAG",
    "ERR_TRADE_EQUIPPED_BAG",
    "ERR_AMMO_ONLY",
    "ERR_PROFICIENCY_NEEDED",
    "ERR_NO_SLOT_AVAILABLE",
    "ERR_CANT_EQUIP_EVER",
    "ERR_CANT_EQUIP_EVER",
    "ERR_NO_SLOT_AVAILABLE",
    "ERR_2HANDED_EQUIPPED",
    "ERR_2HSKILLNOTFOUND",
    "ERR_WRONG_BAG_TYPE",
    "ERR_WRONG_BAG_TYPE",
    "ERR_ITEM_MAX_COUNT",
    "ERR_NO_SLOT_AVAILABLE",
    "ERR_CANT_STACK",
    "ERR_NOT_EQUIPPABLE",
    "ERR_CANT_SWAP",
    "ERR_SLOT_EMPTY",
    "ERR_ITEM_NOT_FOUND",
    "ERR_DROP_BOUND_ITEM",
    "ERR_OUT_OF_RANGE",
    "ERR_TOO_FEW_TO_SPLIT",
    "ERR_SPLIT_FAILED",
    "ERR_SPELL_FAILED_REAGENTS_GENERIC",
    "ERR_NOT_ENOUGH_MONEY",
    "ERR_NOT_A_BAG",
    "ERR_DESTROY_NONEMPTY_BAG",
    "ERR_NOT_OWNER",
    "ERR_ONLY_ONE_QUIVER",
    "ERR_NO_BANK_SLOT",
    "ERR_NO_BANK_HERE",
    "ERR_ITEM_LOCKED",
    "ERR_GENERIC_STUNNED",
    "ERR_PLAYER_DEAD",
    "ERR_CLIENT_LOCKED_OUT",
    "ERR_INTERNAL_BAG_ERROR",
    "ERR_ONLY_ONE_BOLT",
    "ERR_ONLY_ONE_AMMO",
    "ERR_CANT_WRAP_STACKABLE",
    "ERR_CANT_WRAP_EQUIPPED",
    "ERR_CANT_WRAP_WRAPPED",
    "ERR_CANT_WRAP_BOUND",
    "ERR_CANT_WRAP_UNIQUE",
    "ERR_CANT_WRAP_BAGS",
    "ERR_LOOT_GONE",
    "ERR_INV_FULL",
    "ERR_BAG_FULL", // BANK_FULL
    "ERR_VENDOR_SOLD_OUT",
    "ERR_BAG_FULL",
    "ERR_ITEM_NOT_FOUND",
    "ERR_CANT_STACK",
    "ERR_BAG_FULL",
    "ERR_VENDOR_SOLD_OUT",
    "ERR_OBJECT_IS_BUSY",
    nullptr, // NONE
    "ERR_NOT_IN_COMBAT",
    "ERR_NOT_WHILE_DISARMED",
    "ERR_BAG_FULL",
    "ERR_CANT_EQUIP_RANK",
    "ERR_CANT_EQUIP_REPUTATION",
    "ERR_TOO_MANY_SPECIAL_BAGS",
    "ERR_LOOT_CANT_LOOT_THAT_NOW",
};
static_assert(std::size(kInventoryErrors) == 67);

// SMSG_BUY_FAILED's BuyResult.
const char* buyErrorKey(u8 reason) {
    switch (reason) {
    case net::kBuyErrCantFindItem: return "ERR_ITEM_NOT_FOUND";
    case net::kBuyErrItemAlreadySold:
    case net::kBuyErrItemSoldOut: return "ERR_VENDOR_SOLD_OUT";
    case net::kBuyErrNotEnoughMoney: return "ERR_NOT_ENOUGH_MONEY";
    case net::kBuyErrSellerDontLikeYou: return "ERR_VENDOR_HATES_YOU";
    case net::kBuyErrDistanceTooFar: return "ERR_VENDOR_TOO_FAR";
    case net::kBuyErrCantCarryMore: return "ERR_ITEM_MAX_COUNT";
    case net::kBuyErrRankRequire: return "ERR_CANT_EQUIP_RANK";
    case net::kBuyErrReputationRequire: return "ERR_CANT_EQUIP_REPUTATION";
    default: return "VD_INV_CANT_BUY";
    }
}

// SMSG_SELL_ITEM's SellResult (the comments of vMaNGOS' ItemDefines.h say what the client shows).
const char* sellErrorKey(u8 reason) {
    switch (reason) {
    case net::kSellErrCantFindItem: return "ERR_ITEM_NOT_FOUND";
    case net::kSellErrCantSellItem: return "ERR_VENDOR_NOT_INTERESTED";
    case net::kSellErrCantFindVendor: return "ERR_VENDOR_HATES_YOU";
    case net::kSellErrYouDontOwnThatItem: return "ERR_NOT_OWNER";
    case net::kSellErrOnlyEmptyBag: return "ERR_DESTROY_NONEMPTY_BAG";
    default: return nullptr; // SELL_ERR_UNK: nothing appears
    }
}

f32 floatField(const game::Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

void InventoryScreens::init(const game::GameData* data, const game::ItemData* items, UiResources* resources) {
    m_data = data;
    m_items = items;
    m_resources = resources;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    m_bagBar.reserve(5);
    m_cells.reserve(game::kBackpackSize + 4 * game::kMaxBagSize + (game::kKeyringSlotEnd - game::kKeyringSlotStart));
    m_sections.reserve(6);
    m_bankBags.reserve(6);
    m_bankCells.reserve(game::kBankSize + 6 * game::kMaxBagSize);
    m_bankSections.reserve(7);
    m_vendorRows.reserve(net::kMaxVendorItems);
    m_equipment.reserve(20);
    m_tipLines.reserve(game::ItemTooltip::kMaxLines);
    m_containerViews.reserve(kContainerFrames); // phase 15 (items)
    m_bankFrameViews.reserve(6);
    m_containerCells.reserve(game::kBackpackSize + 4 * game::kMaxBagSize + (game::kKeyringSlotEnd - game::kKeyringSlotStart));
    for (std::vector<TooltipLineView>& lines : m_compareLines) {
        lines.reserve(game::ItemTooltip::kMaxLines + 1);
    }
    // CharacterFrameTab1 (CHARACTER: the paper doll), always there.
    m_tabs[static_cast<usize>(CharacterTab::Character)].label = "CHARACTER";
}

void InventoryScreens::registerCharacterTab(CharacterTab tab, text::Key label, TabDrawer draw, void* context,
                                            TabShown shown) {
    if (tab != CharacterTab::Character && tab < CharacterTab::Count) {
        m_tabs[static_cast<usize>(tab)] = {label, draw, context, shown};
    }
}

ItemDrag& InventoryScreens::hostWindow(const DialogContext& c) {
    m_chatLink = c.chatLink; // phase 13: the bags inside another window link too
    m_drawn = true; // as drawWindow: the tooltip, the questions and the drag stay
    m_drag.track(c.ui.touches());
    return m_drag;
}

void InventoryScreens::open(Window window) {
    if (window == Window::Bags && !m_containers.any()) { // phase 15 (items): the backpack's frame at least
        ContainerSizes sizes;
        sizes.bags[0] = game::kBackpackSize;
        m_containers.toggleBackpack(sizes);
    }
    m_characterOpen = m_characterOpen || window == Window::Character;
    m_tipOpen = false;
    m_ask = Ask::None;
}

void InventoryScreens::close(Window window) {
    if (window == Window::Bags) {
        m_containers.closeAll(); // phase 15 (items): every ContainerFrame
    } else if (window == Window::Character) {
        m_characterOpen = false;
    }
    m_tipOpen = false;
    m_ask = Ask::None;
}

void InventoryScreens::setCharacterTab(u32 tab) {
    if (tab < m_tabs.size() && tab != m_characterTab && (tab == 0 || m_tabs[tab].draw)) {
        m_characterTab = tab;
        m_tipOpen = false;
    }
}

void InventoryScreens::closeAll() {
    m_characterOpen = false;
    m_containers.closeAll(); // phase 15 (items)
    m_tipOpen = false;
    m_ask = Ask::None;
    m_drag = ItemDrag{};
}

bool InventoryScreens::serverPanelOpen(const GameSession& session, Panel panel) const {
    const game::Inventory& inv = session.play().inventory();
    switch (panel) {
    case Panel::Reading: return inv.reading().open();
    case Panel::Vendor: return inv.vendor().open();
    case Panel::Bank: return inv.banker() != 0;
    default: return false;
    }
}

void InventoryScreens::bagBar(GameSession& session, std::array<u64, 5>& icons, bool& keyring, u32& freeSlots) const {
    const game::Inventory& inv = session.play().inventory();
    icons[0] = kBackpackIcon;
    for (u8 i = 0; i < 4; ++i) {
        const ItemInstance* bag = inv.at({255, static_cast<u8>(19 + i)}); // CharacterBag0Slot..3
        const net::ItemQueryInfo* info = bag ? session.item(bag->entry) : nullptr;
        icons[1u + i] = info && m_data ? m_data->itemIconHash(info->displayId) : 0;
    }
    keyring = false;
    for (u32 i = 0; i < inv.containerSize(game::kContainerKeyring) && !keyring; ++i) {
        keyring = inv.containerItem(game::kContainerKeyring, i) != nullptr; // SHOW_KEYRING: once a key went in
    }
    freeSlots = inv.freeSlots();
}

// -------------------------------------------------------------------------------------------------------
// Views.

std::string_view InventoryScreens::arena(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const i32 n = std::vsnprintf(m_arena.data() + m_arenaUsed, m_arena.size() - m_arenaUsed, format, args);
    va_end(args);
    if (n < 0 || m_arenaUsed + static_cast<u32>(n) >= m_arena.size()) {
        return {};
    }
    const std::string_view text{m_arena.data() + m_arenaUsed, static_cast<usize>(n)};
    m_arenaUsed += static_cast<u32>(n) + 1;
    return text;
}

bool InventoryScreens::usable(GameSession& session, const net::ItemQueryInfo& item) const {
    return session.play().inventory().usable(item);
}

ItemSlotView InventoryScreens::slotView(GameSession& session, const ItemInstance* item, ItemPos pos, u32 nowMs) {
    ItemSlotView v;
    v.ref = {ItemRef::Kind::Slot, pos.bag, pos.slot, 0};
    if (!item) {
        return v;
    }
    v.empty = false;
    v.count = item->count;
    v.broken = item->broken();
    v.lowDurability = item->maxDurability > 0 && item->durability * 100 <= item->maxDurability * kLowDurabilityPercent;
    if (const net::ItemQueryInfo* info = item->entry ? session.item(item->entry) : nullptr) {
        v.quality = info->quality;
        v.iconHash = m_data ? m_data->itemIconHash(info->displayId) : 0;
        v.usable = usable(session, *info);
        // The share of its use cooldown left (the spell's, or the 30 s after equipping it).
        for (const net::ItemQueryInfo::Spell& spell : info->spells) {
            if (spell.id == 0 || spell.trigger != net::kItemSpellTriggerOnUse) {
                continue;
            }
            const game::CooldownView cd = session.play().spells().cooldown(spell.id, nowMs);
            if (cd.active(nowMs) && cd.durationMs > 0 && !cd.untilEvent) {
                v.cooldown = std::max(v.cooldown, static_cast<f32>(cd.remaining(nowMs)) / static_cast<f32>(cd.durationMs));
            }
            const u32 equipLeft = session.play().inventory().itemCooldownLeft(spell.id, nowMs);
            v.cooldown = std::max(v.cooldown, static_cast<f32>(equipLeft) / 30000.0f);
        }
    }
    if (m_targeting) { // phase 13 (spells): what a waiting spell suits
        const SpellTargetingScreens::Mark mark =
            m_targeting->mark(session, item, item->entry ? session.item(item->entry) : nullptr);
        v.lit = mark == SpellTargetingScreens::Mark::Lit;
        v.dimmed = v.dimmed || mark == SpellTargetingScreens::Mark::Dim;
    }
    return v;
}

void InventoryScreens::bagsView(GameSession& session, u32 nowMs, BagsView& out) {
    const game::Inventory& inv = session.play().inventory();
    m_bagBar.clear();
    m_cells.clear();
    m_sections.clear();
    // The bag bar: the backpack (a drop target: "into the backpack"), then the bag slots.
    ItemSlotView backpack;
    backpack.ref = {ItemRef::Kind::Slot, 255, kBackpackCell, 0};
    backpack.empty = false;
    backpack.iconHash = kBackpackIcon;
    m_bagBar.push_back(backpack);
    for (u8 i = 0; i < 4; ++i) {
        const ItemPos pos{255, static_cast<u8>(19 + i)};
        ItemSlotView cell = slotView(session, inv.at(pos), pos, nowMs);
        cell.emptyArt = kBagSlotArt;
        cell.count = 0;
        m_bagBar.push_back(cell);
    }
    for (i32 c = game::kContainerBackpack; c <= game::kContainerLastBag; ++c) {
        const u32 size = inv.containerSize(c);
        if (size == 0) {
            continue;
        }
        BagSectionView section;
        section.first = static_cast<u32>(m_cells.size());
        section.count = size;
        section.special = inv.containerFamily(c) != game::kBagFamilyNone;
        section.title = text::get("BACKPACK_TOOLTIP");
        if (const ItemInstance* bag = inv.bagOf(c)) {
            const net::ItemQueryInfo* info = session.item(bag->entry);
            section.title = info ? std::string_view(info->name) : text::get("BAGSLOT");
        }
        for (u32 i = 0; i < size; ++i) {
            m_cells.push_back(slotView(session, inv.containerItem(c, i), inv.containerPos(c, i), nowMs));
        }
        m_sections.push_back(section);
    }
    // The keyring (keys go there by themselves): only while it holds something, in rows of four up to the last key.
    u32 keys = 0;
    for (u32 i = 0; i < inv.containerSize(game::kContainerKeyring); ++i) {
        keys = inv.containerItem(game::kContainerKeyring, i) ? i + 1 : keys;
    }
    if (keys > 0) {
        BagSectionView section;
        section.title = text::get("KEYRING");
        section.first = static_cast<u32>(m_cells.size());
        section.count = std::min((keys + 3) / 4 * 4, inv.containerSize(game::kContainerKeyring));
        section.special = true;
        for (u32 i = 0; i < section.count; ++i) {
            m_cells.push_back(slotView(session, inv.containerItem(game::kContainerKeyring, i),
                                       inv.containerPos(game::kContainerKeyring, i), nowMs));
        }
        m_sections.push_back(section);
    }
    out.bagBar = m_bagBar;
    out.sections = m_sections;
    out.cells = m_cells;
    out.money = inv.money();
    out.freeSlots = inv.freeSlots();
    out.totalSlots = inv.totalSlots();
}

void InventoryScreens::bankView(GameSession& session, u32 nowMs, BankView& out) {
    const game::Inventory& inv = session.play().inventory();
    m_bankBags.clear();
    m_bankCells.clear();
    m_bankSections.clear();
    for (u8 i = 0; i < 6; ++i) {
        const ItemPos pos{255, static_cast<u8>(game::kBankBagSlotStart + i)};
        ItemSlotView cell = slotView(session, inv.at(pos), pos, nowMs);
        cell.emptyArt = kBagSlotArt;
        cell.count = 0;
        m_bankBags.push_back(cell);
    }
    const auto addContainer = [&](i32 c, std::string_view title) {
        const u32 size = inv.containerSize(c);
        if (size == 0) {
            return;
        }
        BagSectionView section;
        section.first = static_cast<u32>(m_bankCells.size());
        section.count = size;
        section.title = title;
        section.special = inv.containerFamily(c) != game::kBagFamilyNone;
        if (const ItemInstance* bag = inv.bagOf(c)) {
            const net::ItemQueryInfo* info = session.item(bag->entry);
            section.title = info ? std::string_view(info->name) : text::get("BAGSLOT");
        }
        for (u32 i = 0; i < size; ++i) {
            m_bankCells.push_back(slotView(session, inv.containerItem(c, i), inv.containerPos(c, i), nowMs));
        }
        m_bankSections.push_back(section);
    };
    addContainer(game::kContainerBank, text::get("VD_INV_BANK"));
    // Phase 15 (items): each bank bag as a ContainerFrame of its own once its button opened it (BankFrame's
    // ToggleBag(5..10)), not a section of the grid.
    m_bankFrameViews.clear();
    std::array<u32, 6> first{};
    for (i32 c = game::kContainerFirstBankBag; c <= game::kContainerLastBankBag; ++c) {
        const u32 bit = 1u << static_cast<u32>(c - game::kContainerFirstBankBag);
        const u32 size = inv.containerSize(c);
        if ((m_bankFrames & bit) == 0 || size == 0) {
            continue;
        }
        first[m_bankFrameViews.size()] = static_cast<u32>(m_bankCells.size());
        for (u32 i = 0; i < size; ++i) {
            m_bankCells.push_back(slotView(session, inv.containerItem(c, i), inv.containerPos(c, i), nowMs));
        }
        ContainerFrameView frame;
        frame.id = c;
        if (const ItemInstance* bag = inv.bagOf(c)) {
            const net::ItemQueryInfo* info = session.item(bag->entry);
            frame.name = info ? std::string_view(info->name) : text::get("BAGSLOT");
            frame.portrait = info && m_data ? m_data->itemIconHash(info->displayId) : 0;
        }
        m_bankFrameViews.push_back(frame);
    }
    for (u32 i = 0; i < m_bankFrameViews.size(); ++i) { // the cells once they no longer move
        const u32 end = i + 1 < m_bankFrameViews.size() ? first[i + 1] : static_cast<u32>(m_bankCells.size());
        m_bankFrameViews[i].cells = std::span<const ItemSlotView>(m_bankCells).subspan(first[i], end - first[i]);
    }
    out.bagFrames = m_bankFrameViews.data();
    out.bagFrameCount = static_cast<u32>(m_bankFrameViews.size());
    out.bankBags = m_bankBags;
    out.bankBagSlots = inv.bankBagSlots();
    out.nextSlotPrice = inv.nextBankSlotPrice();
    out.sections = m_bankSections;
    out.cells = m_bankCells;
}

void InventoryScreens::vendorRows(GameSession& session, u32 nowMs, bool buyback) {
    const game::Inventory& inv = session.play().inventory();
    m_vendorRows.clear();
    if (buyback) {
        for (u32 i = 0; i < 12; ++i) {
            const ItemPos pos{255, static_cast<u8>(game::kBuybackSlotStart + i)};
            const ItemInstance* item = inv.at(pos);
            if (!item) {
                continue;
            }
            VendorRowView row;
            row.cell = slotView(session, item, pos, nowMs);
            row.cell.ref = {ItemRef::Kind::Buyback, 0, 0, i};
            const net::ItemQueryInfo* info = session.item(item->entry);
            row.name = info ? std::string_view(info->name) : std::string_view("...");
            row.price = inv.buybackPrice(i);
            row.affordable = row.price <= inv.money();
            m_vendorRows.push_back(row);
        }
        return;
    }
    const std::span<const net::VendorInventory::Item> items = inv.vendor().items();
    for (u32 i = 0; i < items.size(); ++i) {
        const net::VendorInventory::Item& item = items[i];
        VendorRowView row;
        row.cell.ref = {ItemRef::Kind::Vendor, 0, 0, i};
        row.cell.empty = false;
        row.cell.iconHash = m_data ? m_data->itemIconHash(item.displayId) : 0; // known before the template
        if (const net::ItemQueryInfo* info = session.item(item.itemId)) {
            row.name = info->name;
            row.cell.quality = info->quality;
            row.cell.usable = usable(session, *info);
        } else {
            row.name = "...";
        }
        row.price = item.price;
        row.limited = item.stock != net::kVendorUnlimited;
        row.stock = row.limited ? item.stock : 0;
        row.buyCount = std::max<u32>(item.buyCount, 1);
        row.cell.count = row.buyCount;
        row.cell.dimmed = row.limited && row.stock == 0;
        row.affordable = item.price <= inv.money();
        m_vendorRows.push_back(row);
    }
}

void InventoryScreens::characterView(GameSession& session, u32 nowMs, PaperDollView& out) {
    const game::Inventory& inv = session.play().inventory();
    if (m_sheet) {
        out.portrait = m_sheet->portrait(session.playerGuid()); // SetPortraitTexture(CharacterFramePortrait, "player")
    }
    m_equipment.clear();
    for (u8 slot = 0; slot < game::kEquipmentSlotEnd; ++slot) {
        ItemSlotView cell = slotView(session, inv.equipped(slot), {255, slot}, nowMs);
        cell.emptyArt = kSlotArt[slot];
        m_equipment.push_back(cell);
    }
    // The ammo slot: the projectile chosen (PLAYER_AMMO_ID) and how many are carried.
    ItemSlotView ammo;
    ammo.ref = {ItemRef::Kind::Slot, 255, kAmmoCell, 0};
    ammo.emptyArt = kSlotArt[19];
    if (const u32 entry = inv.ammo()) {
        ammo.empty = false;
        ammo.count = inv.count(entry);
        if (const net::ItemQueryInfo* info = session.item(entry)) {
            ammo.iconHash = m_data ? m_data->itemIconHash(info->displayId) : 0;
            ammo.quality = info->quality;
        }
        ammo.usable = ammo.count > 0;
    }
    m_equipment.push_back(ammo);
    if (m_sheet && m_sheet->relicSlot(session)) {
        m_equipment[17].emptyArt = kRelicSlotArt; // PaperDollItemSlotButton_Update: UI-PaperDoll-Slot-Relic
    }
    // CharacterFrame's tabs: the shown ones in their order (phase 15: the Pet tab only with a pet).
    m_tabCount = 0;
    for (u32 i = 0; i < m_tabs.size(); ++i) {
        const CharacterTabEntry& t = m_tabs[i];
        if (i == 0 || (t.draw && (!t.shown || t.shown(t.context, session)))) {
            m_tabIds[m_tabCount] = i;
            m_tabLabels[m_tabCount++] = text::get(t.label); // looked up here: the table loads after init
        }
    }
    u32 selected = 0;
    for (u32 i = 0; i < m_tabCount; ++i) {
        selected = m_tabIds[i] == m_characterTab ? i : selected;
    }
    if (m_tabIds[selected] != m_characterTab) {
        m_characterTab = 0; // its tab went (the pet was dismissed): the paper doll
    }
    out.tabs = {m_tabLabels.data(), m_tabCount};
    out.tab = selected;
    out.page = static_cast<CharacterPage>(m_tabIds[selected]); // CharacterTab's order is CharacterPage's
    out.equipment = m_equipment;
    if (m_sheet) {
        m_sheet->fillPaperDoll(session, out); // phase 15 (character): title, level, guild, model, numbers
    }
    if (out.title.empty()) {
        const std::string* name = session.playerName(session.playerGuid());
        out.title = name ? std::string_view(*name) : text::get("CHARACTER");
    }
}

// -------------------------------------------------------------------------------------------------------
// Windows.

void InventoryScreens::drawPanel(GameSession& session, DialogContext& c, u32 nowMs, Panel panel) {
    VD_PROFILE_ZONE("InventoryScreens::window");
    m_nowMs = nowMs;
    m_chatLink = c.chatLink; // phase 13 (chat links)
    if (!m_drawn) {
        m_arenaUsed = 0; // the first window this frame (the bags and the character sheet may both draw)
    }
    m_drawn = true;
    m_drag.track(c.ui.touches());
    game::Gameplay& play = session.play();
    game::Inventory& inv = play.inventory();
    // The vendor and the bank close when left behind or gone (as the client's frames).
    const game::Entity* me = session.entities().find(session.playerGuid());
    const auto tooFar = [&](u64 npc) {
        const game::Entity* other = session.entities().find(npc);
        return !me || !other || game::distanceAt(*me, *other, nowMs) - floatField(*other, uf::UNIT_FIELD_BOUNDINGRADIUS) > kWindowReach;
    };
    if (panel == Panel::Vendor && inv.vendor().open() && tooFar(inv.vendor().npc)) {
        inv.closeVendor();
    }
    if (panel == Panel::Bank && inv.banker() != 0 && tooFar(inv.banker())) {
        inv.closeBank();
    }

    switch (panel) {
    case Panel::Reading: {
        if (!inv.reading().open()) {
            return;
        }
        const game::ReadingWindow& r = inv.reading();
        const net::ItemQueryInfo* info = r.title.empty() ? session.item(r.entry) : nullptr;
        ReadingView view;
        view.title = !r.title.empty() ? r.title : info ? std::string_view(info->name) : std::string_view("...");
        view.text = r.pageCount > 0 ? r.pages[r.page] : std::string_view{};
        view.page = r.page;
        view.pageCount = r.pageCount;
        view.waiting = r.waiting;
        const ReadingResult result = readingWindow(c, view);
        if (result.turn != 0) {
            inv.turnPage(result.turn);
        }
        if (result.closed) {
            inv.closeReading();
        }
        return;
    }
    case Panel::Vendor: {
        if (!inv.vendor().open()) {
            return;
        }
        if (m_vendorShown != inv.vendor().npc) {
            m_vendorShown = inv.vendor().npc;
            m_vendorBuyback = false;
        }
        VendorView view;
        const game::Entity* npc = session.entities().find(inv.vendor().npc);
        if (npc) {
            if (const net::CreatureQueryInfo* info = session.creature(npc->value(uf::OBJECT_FIELD_ENTRY))) {
                view.npcName = info->name;
            }
        }
        view.buyback = m_vendorBuyback;
        vendorRows(session, nowMs, m_vendorBuyback);
        view.rows = m_vendorRows;
        view.canRepair = inv.vendor().canRepair;
        view.repairAllCost = inv.repairAllCost();
        view.junkValue = inv.junkValue();
        view.money = inv.money();
        bagsView(session, nowMs, view.bags);
        const ItemWindowResult r = vendorWindow(c, view, m_drag);
        if (r.tab >= 0) {
            m_vendorBuyback = r.tab == 1;
        }
        if (r.button == 0) {
            inv.repairAll();
        } else if (r.button == 1) {
            inv.sellJunk();
        }
        apply(session, r, kVendorZones, r.tappedRect);
        if (r.closed) {
            inv.closeVendor();
            m_tipOpen = false;
        }
        return;
    }
    case Panel::Bank: {
        if (inv.banker() == 0) {
            return;
        }
        if (m_bankShown != inv.banker()) { // a new visit: the bank bags start closed (CloseBankBagFrames)
            m_bankShown = inv.banker();
            m_bankFrames = 0;
        }
        BankView view;
        bankView(session, nowMs, view);
        bagsView(session, nowMs, view.bags);
        ContainerFramesResult frames;
        ItemWindowResult r = bankWindow(c, view, m_drag, &frames);
        // Phase 15 (items): a bank bag's button opens or closes its frame (BankFrameItemButtonBag_OnClick's
        // ToggleBag); the frame's close button and portrait (its bag's tooltip).
        if (r.tapped.kind == ItemRef::Kind::Slot && r.tapped.bag == 255 && r.tapped.slot >= game::kBankBagSlotStart &&
            r.tapped.slot < game::kBankBagSlotEnd) {
            const i32 container = game::kContainerFirstBankBag + (r.tapped.slot - game::kBankBagSlotStart);
            if (inv.containerSize(container) > 0) {
                m_bankFrames ^= 1u << static_cast<u32>(r.tapped.slot - game::kBankBagSlotStart);
                m_uiSound(UiSound::BagMenuButton);
                r.tapped = ItemRef{};
            }
        }
        if (frames.closed >= game::kContainerFirstBankBag && frames.closed <= game::kContainerLastBankBag) {
            m_bankFrames &= ~(1u << static_cast<u32>(frames.closed - game::kContainerFirstBankBag));
        }
        if (frames.portrait >= game::kContainerFirstBankBag && frames.portrait <= game::kContainerLastBankBag) {
            openTooltip({ItemRef::Kind::Slot, 255,
                         static_cast<u8>(game::kBankBagSlotStart + frames.portrait - game::kContainerFirstBankBag), 0},
                        frames.portraitRect);
        }
        if (r.button == 0) {
            if (m_popups) {
                m_popups->askBuyBankSlot(session); // parity (popups): CONFIRM_BUY_BANK_SLOT first
            } else {
                inv.buyBankSlot();
            }
        }
        apply(session, r, kBankZones, r.tappedRect);
        if (r.closed) {
            inv.closeBank();
            m_tipOpen = false;
        }
        return;
    }
    case Panel::Bags: drawContainers(session, c, nowMs); return; // phase 15 (items): a ContainerFrame per bag
    case Panel::Character: {
        PaperDollView view;
        characterView(session, nowMs, view);
        const PaperDollResult result = paperDollWindow(c, view, m_drag);
        if (m_sheet) {
            m_sheet->paperDollDone(result);
        }
        const ItemWindowResult& r = result.items;
        bool closed = r.closed;
        if (r.tab >= 0 && static_cast<u32>(r.tab) < m_tabCount) {
            const u32 tab = m_tabIds[static_cast<u32>(r.tab)];
            if (m_characterTab != tab) {
                m_uiSound(UiSound::CharacterTab); // parity pass: CharacterFrame's igCharacterInfoTab
            }
            setCharacterTab(tab);
        } else if (m_characterTab != 0 && m_characterTab < m_tabs.size() && m_tabs[m_characterTab].draw) {
            closed = m_tabs[m_characterTab].draw(m_tabs[m_characterTab].context, session, c, result.frame) || closed;
        }
        apply(session, r, kEquipZones, r.tappedRect);
        if (closed) {
            close(Window::Character);
        }
        return;
    }
    }
}

void InventoryScreens::drawHud(GameSession& session, DialogContext& c, u32 nowMs, bool backpack) {
    (void)nowMs;
    const game::Inventory& inv = session.play().inventory();
    if (backpack) {
        const ui::WidgetState state = backpackButton(c);
        // Parity pass: app/GameScreens opens the bags through the panel rules; phase 20: held, the micro menu's bag bar.
        m_backpackTapped = m_backpackTapped || state.clicked;
        m_backpackHeld = m_backpackHeld || state.longPress;
    }
    const game::DurabilityAlerts& alerts = inv.durability();
    if (!alerts.any()) {
        return;
    }
    std::array<u8, game::DurabilityAlerts::kParts> parts{};
    for (u32 i = 0; i < parts.size(); ++i) {
        parts[i] = static_cast<u8>(alerts.parts[i]);
    }
    const ui::Rect r = durabilityHudRect(c);
    if (durabilityFigure(c, r.x, r.y, r.h, parts, alerts.offhandWeapon)) {
        m_durabilityTapped = true; // see what needs repairing: the character window (app/GameScreens opens it)
    }
}

// -------------------------------------------------------------------------------------------------------
// What the player did.

const ItemInstance* InventoryScreens::itemAt(GameSession& session, const ItemRef& ref) const {
    const game::Inventory& inv = session.play().inventory();
    switch (ref.kind) {
    case ItemRef::Kind::Slot: return inv.at({ref.bag, ref.slot});
    case ItemRef::Kind::Buyback: return inv.at({255, static_cast<u8>(game::kBuybackSlotStart + ref.index)});
    default: return nullptr;
    }
}

u32 InventoryScreens::entryOf(GameSession& session, const ItemRef& ref) const {
    if (ref.kind == ItemRef::Kind::Vendor) {
        const auto items = session.play().inventory().vendor().items();
        return ref.index < items.size() ? items[ref.index].itemId : 0;
    }
    if (ref.kind == ItemRef::Kind::Slot && ref.bag == 255 && ref.slot == kAmmoCell) {
        return session.play().inventory().ammo();
    }
    const ItemInstance* item = itemAt(session, ref);
    return item ? item->entry : 0;
}

game::ItemPlace InventoryScreens::place(const ItemRef& ref) {
    using Kind = game::ItemPlace::Kind;
    switch (ref.kind) {
    case ItemRef::Kind::Slot:
        if (ref.bag == 255 && ref.slot == kBackpackCell) {
            return game::ItemPlace::of(Kind::Backpack);
        }
        if (ref.bag == 255 && ref.slot == kAmmoCell) {
            return game::ItemPlace::of(Kind::Ammo);
        }
        return game::ItemPlace::slot({ref.bag, ref.slot});
    case ItemRef::Kind::Vendor: return game::ItemPlace::of(Kind::Vendor, ref.index);
    case ItemRef::Kind::Buyback: return game::ItemPlace::of(Kind::Buyback, ref.index);
    default: return {};
    }
}

void InventoryScreens::apply(GameSession& session, const ItemWindowResult& r, std::span<const game::ItemPlace::Kind> zones,
                             const ui::Rect& anchor) {
    if (r.tapped.valid()) {
        const bool pseudo = r.tapped.kind == ItemRef::Kind::Slot && r.tapped.bag == 255 &&
                            (r.tapped.slot == kBackpackCell || r.tapped.slot == kAmmoCell);
        // With the chat open the tap links first (ContainerFrameItemButton_OnClick: Shift + click before
        // PickupContainerItem, which is what casts a waiting spell on the item).
        const bool linking = m_chatLink && m_chatLink->active();
        if (!linking && !pseudo && r.tapped.kind == ItemRef::Kind::Slot && m_targeting &&
            m_targeting->pickingItems(session) && itemAt(session, r.tapped)) {
            m_targeting->pickItem(session, {r.tapped.bag, r.tapped.slot}); // phase 13 (spells): the spell's item
        } else if ((!pseudo || r.tapped.slot == kAmmoCell) && entryOf(session, r.tapped) != 0) {
            openTooltip(r.tapped, anchor);
        }
    }
    if (!r.dropFrom.valid()) {
        return;
    }
    // A cell, else the zone of the window it fell on (the window's zone numbers index `zones`).
    game::ItemPlace to = place(r.dropOn);
    if (!r.dropOn.valid() && r.dropZone >= 0 && static_cast<u32>(r.dropZone) < zones.size()) {
        to = game::ItemPlace::of(zones[static_cast<u32>(r.dropZone)]);
    }
    // Parity (popups): equipping what binds when equipped asks first (EQUIP_BIND / AUTOEQUIP_BIND).
    const bool asked = m_popups && m_popups->askBindForDrop(session, place(r.dropFrom), to);
    const bool sent = !asked && game::dropItem(session.play(), place(r.dropFrom), to);
    if (m_trace) {
        VD_LOG_INFO("Item drop: kind %u %u/%u -> kind %u %u/%u zone %d: %s", static_cast<u32>(r.dropFrom.kind),
                    r.dropFrom.bag, r.dropFrom.slot, static_cast<u32>(r.dropOn.kind), r.dropOn.bag, r.dropOn.slot,
                    r.dropZone, sent ? "sent" : "nothing");
    }
}

void InventoryScreens::openTooltip(const ItemRef& ref, const ui::Rect& anchor) {
    // Phase 13 (chat links): with the chat's edit box open the tap links the item (ContainerFrameItemButton_OnClick,
    // BankFrameItemButtonGeneric_OnClick, PaperDollItemSlotButton_OnClick, MerchantItemButton_OnClick: Shift + click
    // inserts GetContainerItemLink / GetInventoryItemLink / GetMerchantItemLink); drawOverlay does it.
    if (m_chatLink && m_chatLink->active() && ref.kind != ItemRef::Kind::Buyback) {
        m_pendingLink = ref;
        return;
    }
    m_tipOpen = true;
    m_tipRef = ref;
    m_tipEntry = 0;
    m_tipAnchor = anchor;
    m_tipKey = 0;
    m_compareOpen = false;
}

void InventoryScreens::showItemTooltip(u32 itemEntry, const ui::Rect& anchor, const game::ItemInstance* instance) {
    m_tipOpen = itemEntry != 0;
    m_tipRef = ItemRef{};
    m_tipEntry = itemEntry;
    m_tipAnchor = anchor;
    m_tipKey = 0;
    m_compareOpen = false;
    m_tipHasInstance = instance != nullptr;
    m_tipInstance = instance ? *instance : ItemInstance{};
}

u64 InventoryScreens::equipmentSlotArt(u32 slot) {
    return slot < std::size(kSlotArt) ? kSlotArt[slot] : 0;
}

bool InventoryScreens::insertItemLink(GameSession& session, const DialogContext& c, const ItemInstance& item) {
    if (!c.chatLink || !c.chatLink->active() || item.entry == 0) {
        return false;
    }
    const net::ItemQueryInfo* info = session.item(item.entry);
    if (!info) {
        return true; // asked for: nothing inserted yet (the tap is still the chat's, as the client's)
    }
    const std::string_view suffix =
        m_items && item.randomPropertyId != 0 ? m_items->randomSuffix(item.randomPropertyId) : std::string_view{};
    char link[256];
    const std::string_view text =
        game::formatItemLink(link, item.entry, game::itemQualityColor(static_cast<u8>(info->quality)), info->name,
                             item.enchantments[0], item.randomPropertyId, suffix);
    c.chatLink->insert(text);
    return true;
}

void InventoryScreens::tooltip(GameSession& session, DialogContext& c, u32 nowMs) {
    if (!m_tipOpen) {
        return;
    }
    game::Gameplay& play = session.play();
    game::Inventory& inv = play.inventory();
    const u32 entry = m_tipEntry ? m_tipEntry : entryOf(session, m_tipRef);
    const ItemInstance* item = m_tipEntry ? (m_tipHasInstance ? &m_tipInstance : nullptr) : itemAt(session, m_tipRef);
    const net::ItemQueryInfo* info = entry ? session.item(entry) : nullptr;
    if (entry == 0) {
        m_tipOpen = false; // the item went away (sold, moved, destroyed)
        return;
    }
    // Rebuilt only when something it shows changed.
    u64 key = 1469598103934665603ull;
    for (const u64 part : {static_cast<u64>(entry), item ? item->guid : 0, static_cast<u64>(inv.version()),
                           static_cast<u64>(session.queryVersion()), static_cast<u64>(m_tipRef.kind),
                           static_cast<u64>(m_tipRef.bag) << 8 | m_tipRef.slot, static_cast<u64>(m_tipRef.index),
                           inv.vendor().npc, inv.banker(), static_cast<u64>(info != nullptr)}) {
        key = (key ^ part) * 1099511628211ull;
    }
    if (key != m_tipKey) {
        m_tipKey = key;
        m_tipLines.clear();
        m_actionCount = 0;
        m_compareCount = 0; // phase 15 (items)
        m_compareAction = -1;
        if (info) {
            game::TooltipContext context;
            if (const game::Entity* me = session.entities().find(session.playerGuid())) {
                context.level = me->value(uf::UNIT_FIELD_LEVEL);
                context.race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
                context.cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
            }
            context.inventory = &inv;
            context.queries = &play.queries();
            context.data = m_items;
            context.spells = &play.spells();
            game::buildItemTooltip(*info, item, context, m_tooltip);
            for (const game::TooltipLine& line : m_tooltip.lines()) {
                m_tipLines.push_back({line.left, line.right, line.leftColor, line.rightColor, line.wrap, line.money,
                                      line.gapBefore});
            }
            buildCompare(session, *info, item, context); // phase 15 (items): ShoppingTooltip1 / 2
            // At a repairer: what repairing it costs (the client's repair cursor shows it).
            if (item && inv.vendor().open() && inv.vendor().canRepair && m_tipLines.size() < m_tipLines.capacity()) {
                if (const u32 cost = inv.repairCost(*item)) {
                    m_tipLines.push_back({text::get("REPAIR_COST"), {}, ui::kWhite, ui::kWhite, false, cost, false});
                }
            }
        } else {
            m_tipLines.push_back({"...", {}, ui::kWhite, ui::kWhite});
        }
        // The actions that apply here (game/ItemActions).
        if (!m_tipEntry) {
            const game::ItemPlace at = place(m_tipRef);
            m_actionCount = game::itemActions(play, at, m_actions);
            for (u32 i = 0; i < m_actionCount; ++i) {
                m_actionLabels[i] = m_actions[i] == game::ItemAction::Use ? game::useLabel(play, at)
                                                                           : game::actionLabel(m_actions[i]);
            }
        }
        // Ours: with something worn to compare it with, a "Compare" button before Destroy shows ShoppingTooltip1 / 2
        // (the "always compare" option shows them at once, without the button).
        if (m_compareCount > 0 && !m_alwaysCompare && m_actionCount < m_actionLabels.size()) {
            u32 at = m_actionCount;
            if (at > 0 && m_actions[at - 1] == game::ItemAction::Destroy) {
                m_actions[at] = m_actions[at - 1];
                m_actionLabels[at] = m_actionLabels[at - 1];
                --at;
            }
            m_actionLabels[at] = text::get("VD_INV_COMPARE");
            m_compareAction = static_cast<i32>(at);
            ++m_actionCount;
        }
    }
    const u32 compareShown = m_alwaysCompare || m_compareOpen ? m_compareCount : 0;
    TooltipView view;
    view.iconHash = info && m_data ? m_data->itemIconHash(info->displayId) : 0;
    view.lines = m_tipLines;
    view.actions = {m_actionLabels.data(), m_actionCount};
    view.anchor = m_tipAnchor;
    // Phase 15 (items): beside its comparison tooltips (Blizzard_AuctionUI's ShoppingTooltip placement).
    TooltipPlacement placement;
    if (compareShown > 0) {
        const ui::Rect size = itemTooltipSize(c, view);
        std::array<f32, 2> heights{};
        for (u32 i = 0; i < compareShown; ++i) {
            heights[i] = compareTooltipHeight(c, m_compareLines[i], size.w);
        }
        placement = placeTooltips(c.safeArea, m_tipAnchor, size.w, size.h, {heights.data(), compareShown},
                                  c.ui.px(8.0f), c.ui.px(ui::kUnitDp) * 10.0f);
        view.placed = placement.main;
    }
    const TooltipResult r = itemTooltip(c, view);
    drawCompare(c, placement, compareShown);
    if (r.action >= 0 && r.action == m_compareAction) {
        m_compareOpen = !m_compareOpen; // a second tap puts them away
    } else if (r.action >= 0 && static_cast<u32>(r.action) < m_actionCount) {
        act(session, m_actions[static_cast<u32>(r.action)]);
    } else if (r.closed) {
        m_tipOpen = false;
    }
    (void)nowMs;
}

void InventoryScreens::act(GameSession& session, game::ItemAction action) {
    using game::ItemAction;
    const ItemInstance* item = itemAt(session, m_tipRef);
    if (m_trace) {
        VD_LOG_INFO("Item action %u on kind %u %u/%u #%u", static_cast<u32>(action), static_cast<u32>(m_tipRef.kind),
                    m_tipRef.bag, m_tipRef.slot, m_tipRef.index);
    }
    switch (action) {
    case ItemAction::Split:
        if (item && item->count > 1) {
            m_ask = Ask::Split;
            m_askRef = m_tipRef;
            m_askValue = item->count / 2;
        }
        break;
    case ItemAction::BuyMany:
        m_ask = Ask::Buy;
        m_askRef = m_tipRef;
        m_askValue = 1;
        break;
    case ItemAction::Destroy:
        if (item && m_popups) { // parity (popups): DELETE_ITEM / DELETE_GOOD_ITEM
            m_popups->askDestroyItem(session, place(m_tipRef).pos);
        } else if (item) {
            const net::ItemQueryInfo* info = session.item(item->entry);
            const std::string_view name = info ? std::string_view(info->name) : std::string_view("...");
            text::format(m_askText, item->count > 1 ? "VD_INV_DESTROY_STACK" : "DELETE_ITEM", name, item->count);
            m_ask = Ask::Destroy;
            m_askRef = m_tipRef;
        }
        break;
    default: // parity (popups): AUTOEQUIP_BIND / USE_BIND first when the item binds then
        if (!m_popups || !m_popups->askBindForAction(session, place(m_tipRef), action)) {
            game::doItemAction(session.play(), place(m_tipRef), action);
        }
        break;
    }
    m_tipOpen = action == ItemAction::Buy; // tap again to buy another
}

void InventoryScreens::quantity(GameSession& session, DialogContext& c) {
    game::Inventory& inv = session.play().inventory();
    if (m_ask == Ask::Destroy) {
        const ConfirmResult r =
            confirmDialog(c, m_askText.data(), text::get("VD_INV_DESTROY"), text::get("CANCEL"));
        if (r.yes) {
            inv.destroy({m_askRef.bag, m_askRef.slot});
        }
        if (r.yes || r.no) {
            m_ask = Ask::None;
        }
        return;
    }
    const u32 entry = entryOf(session, m_askRef);
    const net::ItemQueryInfo* info = entry ? session.item(entry) : nullptr;
    if (!info) {
        m_ask = Ask::None;
        return;
    }
    QuantityView view;
    view.itemName = info->name;
    view.quality = info->quality;
    view.iconHash = m_data ? m_data->itemIconHash(info->displayId) : 0;
    if (m_ask == Ask::Split) {
        const ItemInstance* item = itemAt(session, m_askRef);
        if (!item || item->count < 2) {
            m_ask = Ask::None;
            return;
        }
        view.title = text::get("VD_INV_SPLIT");
        view.confirm = view.title;
        view.min = 1;
        view.max = item->count - 1;
    } else {
        const auto items = inv.vendor().items();
        if (!inv.vendor().open() || m_askRef.index >= items.size()) {
            m_ask = Ask::None;
            return;
        }
        const net::VendorInventory::Item& row = items[m_askRef.index];
        view.title = text::get("VD_INV_BUY");
        view.confirm = view.title;
        view.unitPrice = row.price;
        // As many purchases as fit in one stack, can be paid and are in stock.
        u32 most = std::max<u32>(1, info->stackable / std::max<u32>(row.buyCount, 1));
        if (row.price > 0) {
            most = std::min(most, std::max<u32>(1, inv.money() / row.price));
        }
        if (row.stock != net::kVendorUnlimited) {
            most = std::min(most, std::max<u32>(1, row.stock));
        }
        view.max = std::min<u32>(most, 255);
    }
    const QuantityResult r = quantityDialog(c, view, m_askValue);
    if (r.confirmed) {
        if (m_ask == Ask::Split) {
            if (!inv.split({m_askRef.bag, m_askRef.slot}, m_askValue)) {
                // No empty slot: nothing to split into.
            }
        } else {
            inv.buy(m_askRef.index, m_askValue);
        }
    }
    if (r.confirmed || r.cancelled) {
        m_ask = Ask::None;
    }
}

void InventoryScreens::drawOverlay(GameSession& session, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("InventoryScreens::overlay");
    if (m_pendingLink.valid()) { // phase 13: a cell tapped with the chat open
        ItemInstance link;
        if (const ItemInstance* item = itemAt(session, m_pendingLink)) {
            link = *item;
        }
        link.entry = entryOf(session, m_pendingLink); // a vendor's row, the ammo: the template
        insertItemLink(session, c, link);
        m_pendingLink = ItemRef{};
    }
    m_drag.track(c.ui.touches()); // again: the windows may not have drawn this frame (it only reads the fingers)
    if (!m_drawn) {
        // Another window covers ours (the menu, a quest, death): its tooltip, questions and drag go away; a
        // template's tooltip (loot, rewards) stays.
        m_tipOpen = m_tipOpen && m_tipEntry != 0;
        m_ask = Ask::None;
        m_drag = ItemDrag{};
    }
    m_drawn = false;
    if (m_ask != Ask::None) {
        quantity(session, c);
    } else {
        tooltip(session, c, nowMs);
    }
    // The carried item over everything, above the finger.
    if (m_drag.active) {
        ui::Gui g{c.ui, c.draw};
        const f32 size = kit::metrics(c).item * 1.1f;
        const ui::Rect r{m_drag.x - size * 0.5f, m_drag.y - size * 1.25f, size, size};
        if (const std::optional<u32> icon = m_drag.iconHash ? m_resources->icon(m_drag.iconHash) : std::nullopt) {
            g.draw.icon(r, *icon, rgba(255, 255, 255, 230));
        } else {
            g.draw.solid(r, rgba(30, 30, 30, 200));
        }
        if (m_drag.count > 1) {
            char text[12];
            std::snprintf(text, sizeof(text), "%u", m_drag.count);
            ui::TextStyle style = ui::numberFont(size * 0.34f);
            style.align = ui::Align::Right;
            g.draw.text(r.right() - size * 0.06f, r.bottom() - size * 0.08f, text, style);
        }
    }
    // Parity pass: the carried item's pick-up and put-down sounds (its ItemGroupSounds: GameAudio plays them).
    if (m_drag.active && !m_dragSounded) {
        m_dragSounded = true;
        m_dragEntry = entryOf(session, m_drag.from);
        m_uiSound.itemSound(m_dragEntry, true);
    } else if (!m_drag.active) {
        m_dragSounded = false;
    }
    if (m_drag.active && m_drag.released && m_dragSounded) {
        m_uiSound.itemSound(m_dragEntry, false);
        m_dragSounded = false;
    }
    m_drag.finish();
}

bool InventoryScreens::eventMessage(GameSession& session, const game::GameEvent& e, char* out, usize size,
                                    u32& color) const {
    (void)session;
    using K = game::GameEventKind;
    color = kErrorColor;
    const std::span<char> line{out, size};
    switch (e.kind) {
    case K::InventoryError: {
        const u32 result = static_cast<u32>(e.value[0]);
        const char* key = result < std::size(kInventoryErrors) ? kInventoryErrors[result] : "ERR_BAG_FULL";
        if (!key) {
            return false;
        }
        text::format(line, key, e.value[1]); // ERR_CANT_EQUIP_LEVEL_I: the level
        return true;
    }
    case K::BuyFailed: text::format(line, buyErrorKey(static_cast<u8>(e.value[0]))); return true;
    case K::SellFailed: {
        const char* key = sellErrorKey(static_cast<u8>(e.value[0]));
        if (!key) {
            return false;
        }
        text::format(line, key);
        return true;
    }
    case K::BankSlotFailed:
        switch (static_cast<u32>(e.value[0])) {
        case net::kBankSlotFailedTooMany: text::format(line, "ERR_BANKSLOT_FAILED_TOO_MANY"); break;
        case net::kBankSlotInsufficientFunds: text::format(line, "ERR_BANKSLOT_INSUFFICIENT_FUNDS"); break;
        default: text::format(line, "ERR_BANKSLOT_NOTBANKER"); break;
        }
        return true;
    case K::BankSlotBought:
        color = kInfoColor;
        text::format(line, "VD_INV_BANK_SLOT_BOUGHT");
        return true;
    case K::ReadItemFailed: text::format(line, "VD_INV_CANT_READ"); return true;
    case K::DurabilityLoss: text::format(line, "DURABILITYDAMAGE_DEATH"); return true; // "10%%": a format
    default: return false;
    }
}

} // namespace vd
