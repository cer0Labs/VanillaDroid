#include "game/ExchangeData.h"

#include "core/Log.h"
#include "game/ItemDefs.h"
#include "net/ExchangeMessages.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <string>

namespace vd::game {
namespace {

// AuctionHouse.dbc: 0 id, 1 faction, 2 deposit %, 3 cut %, 4..11 name (4: enUS), 12 flags.
namespace house {
constexpr u32 kFaction = 1, kDeposit = 2, kCut = 3, kName = 4, kFieldCount = 13;
}
// MailTemplate.dbc: 0 id, 1..8 subject (1: enUS), 9 flags.
constexpr u32 kTemplateSubject = 1;
constexpr u32 kTemplateFieldCount = 10;
// ItemClass.dbc: 0 class, 3..10 name (3: enUS).
constexpr u32 kClassName = 3;
constexpr u32 kClassFieldCount = 12;
// ItemSubClass.dbc: 0 class, 1 subclass, 10..17 display name (10: enUS), 19..26 verbose name (19: enUS).
constexpr u32 kSubclassDisplayName = 10;
constexpr u32 kSubclassVerboseName = 19;
constexpr u32 kSubclassFieldCount = 28;

constexpr u32 kAny = net::kAuctionAny;
// AuctionFrame's lists (1.12): weapons, armor, containers, consumables, trade goods, projectiles, quivers,
// recipes, reagents, miscellaneous; a class's own line (kAny) before its subclasses.
constexpr AuctionCategory kCategories[] = {
    {kItemClassWeapon, kAny},     {kItemClassWeapon, 0},    // One-Handed Axes
    {kItemClassWeapon, 1},                                  // Two-Handed Axes
    {kItemClassWeapon, 2},                                  // Bows
    {kItemClassWeapon, 3},                                  // Guns
    {kItemClassWeapon, 4},                                  // One-Handed Maces
    {kItemClassWeapon, 5},                                  // Two-Handed Maces
    {kItemClassWeapon, 6},                                  // Polearms
    {kItemClassWeapon, 7},                                  // One-Handed Swords
    {kItemClassWeapon, 8},                                  // Two-Handed Swords
    {kItemClassWeapon, 10},                                 // Staves
    {kItemClassWeapon, 13},                                 // Fist Weapons
    {kItemClassWeapon, 14},                                 // Miscellaneous
    {kItemClassWeapon, 15},                                 // Daggers
    {kItemClassWeapon, 16},                                 // Thrown
    {kItemClassWeapon, 18},                                 // Crossbows
    {kItemClassWeapon, 19},                                 // Wands
    {kItemClassWeapon, 20},                                 // Fishing Pole
    {kItemClassArmor, kAny},      {kItemClassArmor, 0},     // Miscellaneous
    {kItemClassArmor, 1},                                   // Cloth
    {kItemClassArmor, 2},                                   // Leather
    {kItemClassArmor, 3},                                   // Mail
    {kItemClassArmor, 4},                                   // Plate
    {kItemClassArmor, 6},                                   // Shields
    {kItemClassArmor, 7},                                   // Librams
    {kItemClassArmor, 8},                                   // Idols
    {kItemClassArmor, 9},                                   // Totems
    {kItemClassContainer, kAny},  {kItemClassContainer, 0}, // Bag
    {kItemClassContainer, 1},                               // Soul Bag
    {kItemClassContainer, 2},                               // Herb Bag
    {kItemClassContainer, 3},                               // Enchanting Bag
    {kItemClassContainer, 4},                               // Engineering Bag
    {kItemClassConsumable, kAny}, {kItemClassTradeGoods, kAny}, {kItemClassTradeGoods, 0}, // Trade Goods
    {kItemClassTradeGoods, 1},                                                             // Parts
    {kItemClassTradeGoods, 2},                                                             // Explosives
    {kItemClassTradeGoods, 3},                                                             // Devices
    {kItemClassProjectile, kAny}, {kItemClassProjectile, 2},                               // Arrow
    {kItemClassProjectile, 3},                                                             // Bullet
    {kItemClassQuiver, kAny},     {kItemClassQuiver, 2},                                   // Quiver
    {kItemClassQuiver, 3},                                                                 // Ammo Pouch
    {kItemClassRecipe, kAny},     {kItemClassRecipe, 0},                                   // Book
    {kItemClassRecipe, 1},                                                                 // Leatherworking
    {kItemClassRecipe, 2},                                                                 // Tailoring
    {kItemClassRecipe, 3},                                                                 // Engineering
    {kItemClassRecipe, 4},                                                                 // Blacksmithing
    {kItemClassRecipe, 5},                                                                 // Cooking
    {kItemClassRecipe, 6},                                                                 // Alchemy
    {kItemClassRecipe, 7},                                                                 // First Aid
    {kItemClassRecipe, 8},                                                                 // Enchanting
    {kItemClassRecipe, 9},                                                                 // Fishing
    {kItemClassReagent, kAny},    {kItemClassJunk, kAny},
};

} // namespace

std::span<const AuctionCategory> auctionCategories() {
    return kCategories;
}

bool ExchangeData::init(const pack::PackReader& db) {
    const bool houses = m_houses.load(db, "AuctionHouse", house::kFieldCount);
    const bool templates = m_templates.load(db, "MailTemplate", kTemplateFieldCount);
    const bool classes = m_classes.load(db, "ItemClass", kClassFieldCount);
    m_subclassIndex.clear();
    bool subclasses = false;
    if (const std::optional<pack::PackAsset> asset = db.find("DBFilesClient\\ItemSubClass.dbc")) {
        Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
        if (parsed && parsed.value().fieldCount() >= kSubclassFieldCount) {
            m_subclasses = parsed.value();
            m_subclassIndex.reserve(m_subclasses.recordCount());
            for (u32 r = 0; r < m_subclasses.recordCount(); ++r) {
                const u32 key = m_subclasses.getU32(r, 0) << 16 | (m_subclasses.getU32(r, 1) & 0xFFFF);
                m_subclassIndex.emplace_back(key, r);
            }
            std::sort(m_subclassIndex.begin(), m_subclassIndex.end());
            subclasses = true;
        } else {
            VD_LOG_WARN("ItemSubClass.dbc: unexpected layout");
        }
    }
    return houses && templates && classes && subclasses;
}

ExchangeData::House ExchangeData::house(u32 houseId) const {
    House h;
    const u32 r = m_houses.find(houseId);
    if (r == DbcTable::kMissing) {
        return h;
    }
    h.faction = m_houses.view.getU32(r, house::kFaction);
    h.depositPercent = m_houses.view.getU32(r, house::kDeposit);
    h.cutPercent = m_houses.view.getU32(r, house::kCut);
    h.name = m_houses.view.getString(r, house::kName);
    h.known = true;
    return h;
}

std::string_view ExchangeData::mailTemplateSubject(u32 templateId) const {
    const u32 r = m_templates.find(templateId);
    return r == DbcTable::kMissing ? std::string_view{} : m_templates.view.getString(r, kTemplateSubject);
}

std::string_view ExchangeData::itemClassName(u32 itemClass) const {
    const u32 r = m_classes.find(itemClass);
    return r == DbcTable::kMissing ? std::string_view{} : m_classes.view.getString(r, kClassName);
}

std::string_view ExchangeData::itemSubclassName(u32 itemClass, u32 subclass) const {
    const u32 key = itemClass << 16 | (subclass & 0xFFFF);
    const auto it = std::lower_bound(m_subclassIndex.begin(), m_subclassIndex.end(), std::make_pair(key, 0u));
    if (it == m_subclassIndex.end() || it->first != key) {
        return {};
    }
    const std::string_view verbose = m_subclasses.getString(it->second, kSubclassVerboseName);
    return verbose.empty() ? m_subclasses.getString(it->second, kSubclassDisplayName) : verbose;
}

} // namespace vd::game
