#pragma once

#include "core/Types.h"
#include "db/DbcView.h"
#include "game/DbcIndex.h"

#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// One category of the auction house's browse list: an item class, and one of its subclasses
// (net::kAuctionAny: the whole class).
struct AuctionCategory {
    u32 itemClass = 0;
    u32 subclass = 0;
};

// The categories the 1.12 client's AuctionFrame lists (its GetAuctionItemClasses / GetAuctionItemSubClasses),
// in its order; the ids are item_template's class and subclass (vmangos-core Objects/ItemPrototype.h).
std::span<const AuctionCategory> auctionCategories();

// The client tables of mail and the auction house (phase 8): AuctionHouse.dbc (an auction house's deposit and
// cut percentages, AuctionHouseEntryfmt "niiixxxxxxxxx"), MailTemplate.dbc (the subject of NPC letters,
// MailTemplateEntryfmt "nxxxxxxxxx": id, subject in 8 locales, flags), and the item class names the auction
// house's categories show: ItemClass.dbc (12 fields: 0 class, 3..10 name, enUS at 3) and ItemSubClass.dbc (28
// fields: 0 class, 1 subclass, 10..17 display name, enUS at 10, 19..26 verbose name, enUS at 19). Indexes
// into the mapped db pack; names are enUS views into it.
class ExchangeData {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return !m_houses.empty(); }

    struct House {
        u32 depositPercent = 5; // what the faction houses take per 2 hours (AuctionHouse.dbc ids 1 to 6)
        u32 cutPercent = 5;
        u32 faction = 0;
        std::string_view name; // enUS
        bool known = false;
    };
    // An auction house by its id (MSG_AUCTION_HELLO); the faction houses' rates when unknown.
    House house(u32 houseId) const;
    // A mail template's subject (enUS; empty when unknown).
    std::string_view mailTemplateSubject(u32 templateId) const;
    // "Weapon", "Armor"...; empty when unknown.
    std::string_view itemClassName(u32 itemClass) const;
    // "One-Handed Axes" (the verbose name), else the display name ("Cloth", "Parts"); empty when unknown.
    std::string_view itemSubclassName(u32 itemClass, u32 subclass) const;

private:
    DbcTable m_houses;
    DbcTable m_templates;
    DbcTable m_classes;
    db::DbcView m_subclasses;
    std::vector<std::pair<u32, u32>> m_subclassIndex; // (class << 16 | subclass, record), sorted
};

} // namespace vd::game
