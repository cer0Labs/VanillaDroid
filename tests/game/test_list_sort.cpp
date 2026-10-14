// Phase 18 (p3): the sortable lists as WoW.exe 1.12.1 sorts them: SortWho (0x5AD890, comparator 0x5ADA00) and
// SortAuctionItems (0x4CFC00, the key lists 0x4CD850 / 0x4CDC70 / 0x4CDD60, comparator 0x4CD940).
#include "game/ListSort.h"

#include <doctest/doctest.h>

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace vd;
using game::AuctionSort;
using game::WhoSort;

std::vector<std::string> whoNames(const std::vector<game::WhoEntry>& entries, const std::vector<u16>& order) {
    std::vector<std::string> out;
    for (const u16 i : order) {
        out.push_back(entries[i].name);
    }
    return out;
}

std::string_view zoneName(const void*, u32 id) {
    switch (id) {
    case 12: return "Elwynn Forest";
    case 1519: return "Stormwind City";
    case 40: return "Westfall";
    default: return {};
    }
}
std::string_view className(const void*, u32 id) {
    return id == 1 ? "Warrior" : id == 8 ? "Mage" : id == 5 ? "Priest" : std::string_view{};
}
std::string_view raceName(const void*, u32 id) {
    return id == 1 ? "Human" : id == 3 ? "Dwarf" : id == 4 ? "Night Elf" : std::string_view{};
}

struct AuctionWorld {
    std::map<u32, net::ItemQueryInfo> items;
    std::map<u64, std::string> names;
    static const net::ItemQueryInfo* item(const void* user, u32 entry) {
        const auto& m = static_cast<const AuctionWorld*>(user)->items;
        const auto it = m.find(entry);
        return it != m.end() ? &it->second : nullptr;
    }
    static const std::string* name(const void* user, u64 guid) {
        const auto& m = static_cast<const AuctionWorld*>(user)->names;
        const auto it = m.find(guid);
        return it != m.end() ? &it->second : nullptr;
    }
    void addItem(u32 entry, const char* itemName, u32 quality, u32 level) {
        net::ItemQueryInfo& i = items[entry];
        i.entry = entry;
        i.found = true;
        i.name = itemName;
        i.quality = quality;
        i.requiredLevel = level;
    }
};

net::AuctionItem auction(u32 id, u32 item, u64 owner, u32 startBid, u32 bid, u64 bidder, u32 buyout, u32 timeLeft) {
    net::AuctionItem a;
    a.id = id;
    a.itemId = item;
    a.count = 1;
    a.owner = owner;
    a.startBid = startBid;
    a.bid = bid;
    a.bidder = bidder;
    a.buyout = buyout;
    a.timeLeftMs = timeLeft;
    return a;
}

std::vector<u32> ids(const std::vector<net::AuctionItem>& items, const std::vector<u16>& order) {
    std::vector<u32> out;
    for (const u16 i : order) {
        out.push_back(items[i].id);
    }
    return out;
}

} // namespace

TEST_CASE("Sort keys: the key goes first keeping its flag; the first one again turns round") {
    game::WhoSortKeys keys = game::whoSortDefaults();
    CHECK(keys.key(0) == WhoSort::Zone);
    CHECK(keys.key(4) == WhoSort::Name);
    const u32 v0 = keys.version();
    REQUIRE(keys.sort(WhoSort::Name));
    CHECK(keys.version() == v0 + 1);
    CHECK(keys.key(0) == WhoSort::Name);
    CHECK_FALSE(keys.reversed(0));
    CHECK(keys.key(1) == WhoSort::Zone); // the others moved down in their order
    CHECK(keys.key(4) == WhoSort::Group);
    CHECK(keys.key(5) == WhoSort::Race);
    keys.sort(WhoSort::Name); // again: reversed
    CHECK(keys.reversed(0));
    CHECK(keys.reversedOf(WhoSort::Name));
    keys.sort(WhoSort::Level); // another one: Name keeps its flag behind it
    CHECK(keys.key(0) == WhoSort::Level);
    CHECK_FALSE(keys.reversed(0));
    CHECK(keys.key(1) == WhoSort::Name);
    CHECK(keys.reversed(1));
    keys.sort(WhoSort::Name); // back to the front reversed, as it was (not turned round: it was not first)
    CHECK(keys.key(0) == WhoSort::Name);
    CHECK(keys.reversed(0));
    CHECK_FALSE(keys.sort(WhoSort::Count));
    CHECK(game::whoSortFromName("LEVEL") == WhoSort::Level);
    CHECK(game::whoSortFromName("nonsense") == WhoSort::Name); // 0x5AD8BB's default
}

TEST_CASE("SortWho: zone, level, class... by the tables' names, the level as a number, no case") {
    std::vector<game::WhoEntry> e(5);
    e[0] = {"bob", "Knights", 12, 1, 1, 12};
    e[1] = {"Alice", "", 60, 8, 4, 1519};
    e[2] = {"carl", "Ashen", 9, 5, 3, 12};
    e[3] = {"Dana", "knights", 12, 8, 1, 40};
    e[4] = {"eve", "Zeal", 3, 1, 1, 12};
    game::WhoSortNames names{nullptr, zoneName, className, raceName};
    game::WhoSortKeys keys = game::whoSortDefaults();
    std::vector<u16> order;
    game::sortWho(e, keys, names, order);
    // Zone (Elwynn Forest, Stormwind City, Westfall), then the level within Elwynn.
    CHECK(whoNames(e, order) == std::vector<std::string>{"eve", "carl", "bob", "Alice", "Dana"});
    keys.sort(WhoSort::Name);
    game::sortWho(e, keys, names, order);
    CHECK(whoNames(e, order) == std::vector<std::string>{"Alice", "bob", "carl", "Dana", "eve"});
    keys.sort(WhoSort::Name);
    game::sortWho(e, keys, names, order);
    CHECK(whoNames(e, order) == std::vector<std::string>{"eve", "Dana", "carl", "bob", "Alice"});
    keys.sort(WhoSort::Level); // 3, 9, 12 (bob and Dana: the next key, Name reversed), 60
    game::sortWho(e, keys, names, order);
    CHECK(whoNames(e, order) == std::vector<std::string>{"eve", "carl", "Dana", "bob", "Alice"});
    keys.sort(WhoSort::Guild); // "" first, then Ashen, Knights = knights (no case: Level then decides), Zeal
    game::sortWho(e, keys, names, order);
    CHECK(whoNames(e, order) == std::vector<std::string>{"Alice", "carl", "Dana", "bob", "eve"});
    keys.sort(WhoSort::Class); // Mage, Priest, Warrior
    game::sortWho(e, keys, names, order);
    CHECK(e[order[0]].cls == 8);
    CHECK(e[order[2]].cls == 5);
    CHECK(e[order[4]].cls == 1);
    // "group" compares nothing: the order stays.
    const std::vector<u16> before = order;
    keys.sort(WhoSort::Group);
    game::sortWho(e, keys, names, order);
    CHECK(order == before);
    // A zone the table does not have goes last.
    e[4].zone = 9999;
    keys = game::whoSortDefaults();
    game::sortWho(e, keys, names, order);
    CHECK(order.back() == 4);
}

TEST_CASE("SortAuctionItems: WoW.exe's keys, directions and starting lists") {
    AuctionWorld world;
    world.addItem(100, "Linen Cloth", 1, 0);
    world.addItem(200, "Arcanite Reaper", 4, 58);
    world.addItem(300, "Copper Bar", 1, 0);
    world.addItem(400, "Blue Pearl", 2, 25);
    world.names[0x10] = "Zed";
    world.names[0x11] = "anna";
    const u64 me = 0x99;
    std::vector<net::AuctionItem> items = {
        auction(1, 100, 0x10, 50, 0, 0, 0, 7'200'000),
        auction(2, 200, 0x11, 900'000, 950'000, 0x55, 2'000'000, 600'000),
        auction(3, 300, 0x12, 40, 60, me, 500, 30'000'000),
        auction(4, 400, 0x10, 10'000, 0, 0, 25'000, 43'200'000),
    };
    game::AuctionSortData data{me, &world, AuctionWorld::item, AuctionWorld::name};
    std::vector<u16> order;

    // The browse list starts with the level (at least 1: 0 and 1 the same), then the quality, best first.
    game::AuctionSortKeys browse = game::auctionSortDefaults(game::AuctionList::Browse);
    game::sortAuctions(items, browse, data, order);
    // Level 1: Linen and Copper (both common), then the bid: Copper's 60, Linen's starting 50 (nobody bid): Linen.
    CHECK(ids(items, order) == std::vector<u32>{1, 3, 4, 2});
    CHECK(game::compareAuctions(items[0], items[2], AuctionSort::Bid, data) < 0);
    CHECK(game::compareAuctions(items[0], items[2], AuctionSort::Level, data) == 0);
    CHECK(game::compareAuctions(items[1], items[3], AuctionSort::Quality, data) < 0); // epic before rare
    CHECK(game::compareAuctions(items[3], items[1], AuctionSort::Buyout, data) > 0);  // the highest buyout first
    CHECK(game::compareAuctions(items[1], items[0], AuctionSort::Duration, data) < 0); // the shortest time first
    CHECK(game::compareAuctions(items[2], items[1], AuctionSort::Status, data) < 0);   // the player's bid first...
    CHECK(game::compareAuctions(items[1], items[0], AuctionSort::Status, data) < 0);   // ...then others', then none
    CHECK(game::compareAuctions(items[3], items[0], AuctionSort::Name, data) < 0);     // Blue Pearl < Linen Cloth
    CHECK(game::compareAuctions(items[1], items[0], AuctionSort::Seller, data) < 0);   // anna < Zed, no case
    CHECK(game::compareAuctions(items[2], items[0], AuctionSort::Seller, data) > 0);   // an unknown seller last
    CHECK(game::compareAuctions(items[0], items[0], AuctionSort::Seller, data) == 0);

    // A header tapped: SortAuctionItems("list", "buyout"), then again (reversed).
    browse.sort(AuctionSort::Buyout);
    game::sortAuctions(items, browse, data, order);
    CHECK(ids(items, order) == std::vector<u32>{2, 4, 3, 1});
    browse.sort(AuctionSort::Buyout);
    game::sortAuctions(items, browse, data, order);
    CHECK(ids(items, order) == std::vector<u32>{1, 3, 4, 2});
    CHECK(browse.reversedOf(AuctionSort::Buyout));
    CHECK_FALSE(browse.reversedOf(AuctionSort::Level));

    // The bids and the player's auctions start with the status, then the time left.
    const game::AuctionSortKeys bids = game::auctionSortDefaults(game::AuctionList::Bids);
    CHECK(bids.key(0) == AuctionSort::Status);
    CHECK(bids.key(1) == AuctionSort::Duration);
    CHECK(bids.key(7) == AuctionSort::Seller);
    game::sortAuctions(items, bids, data, order);
    CHECK(ids(items, order) == std::vector<u32>{3, 2, 1, 4});

    // An item whose template has not arrived goes last (until it does).
    items[0].itemId = 777;
    game::AuctionSortKeys byName = game::auctionSortDefaults(game::AuctionList::Browse);
    byName.sort(AuctionSort::Name);
    game::sortAuctions(items, byName, data, order);
    CHECK(ids(items, order) == std::vector<u32>{2, 4, 3, 1});
}

TEST_CASE("Sorting a full who answer and a full auction page: the cost (desktop)") {
    std::vector<game::WhoEntry> who(50);
    std::vector<net::AuctionItem> page(50);
    AuctionWorld world;
    for (u32 i = 0; i < 50; ++i) {
        who[i] = {"Player" + std::to_string((i * 37) % 50), i % 3 ? "Some Guild" : "", (i * 13) % 60 + 1, i % 9 + 1,
                  i % 8 + 1, i % 2 ? 12u : 1519u};
        world.addItem(1000 + i, ("Item " + std::to_string((i * 17) % 50)).c_str(), i % 5, (i * 7) % 60);
        world.names[0x100 + i] = "Seller" + std::to_string(i % 13);
        page[i] = auction(i + 1, 1000 + i, 0x100 + i, 100 + i * 7, i % 4 ? 0 : 200 + i, i % 4 ? 0 : 0x55,
                          i % 3 ? 5000 + i : 0, (i * 991) % 43'200'000);
    }
    const game::WhoSortNames names{nullptr, zoneName, className, raceName};
    const game::AuctionSortData data{0x99, &world, AuctionWorld::item, AuctionWorld::name};
    game::WhoSortKeys whoKeys = game::whoSortDefaults();
    game::AuctionSortKeys auctionKeys = game::auctionSortDefaults(game::AuctionList::Browse);
    auctionKeys.sort(AuctionSort::Name);
    std::vector<u16> order;
    order.reserve(64);
    constexpr u32 kRuns = 200;
    auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kRuns; ++i) {
        game::sortWho(who, whoKeys, names, order);
    }
    const f64 whoUs = std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - start).count() / kRuns;
    start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kRuns; ++i) {
        game::sortAuctions(page, auctionKeys, data, order);
    }
    const f64 auctionUs =
        std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - start).count() / kRuns;
    MESSAGE("sortWho (50): " << whoUs << " us; sortAuctions (50, by name): " << auctionUs << " us");
    CHECK(order.size() == 50);
}
