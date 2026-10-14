#pragma once

#include "core/Types.h"
#include "game/Auction.h"
#include "game/Social.h"
#include "net/ExchangeMessages.h"
#include "net/Messages.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::game {

// Phase 18 (p3): the lists the 1.12 interface sorts by its column headers, as WoW.exe 1.12.1 sorts them (read-only
// disassembly): SortWho (0x5AD890, comparator 0x5ADA00) and SortAuctionItems (0x4CFC00: "list" 0x4CD850, "owner"
// 0x4CDC70, "bidder" 0x4CDD60; one comparator for the three, 0x4CD940). SortGuildRoster keeps its own list in
// game/GuildControl. The keys are the client's, not the server's: neither the who list nor the auction lists ask
// vMaNGOS to sort (1.12's CMSG_AUCTION_LIST_ITEMS has no sort); each answer is sorted when it arrives (the who list
// in SMSG_WHO's handler 0x5AE0D2, an auction page once its items' templates are known, 0x4CC9F1 / 0x4CCA21).

// A list of sort keys as WoW.exe keeps them: every key once, the first one decides and the next ones break the ties,
// each with its own "reversed" flag (they all start false, the order a fixed one).
template <typename Key, u32 N>
class SortKeyList {
public:
    constexpr SortKeyList() = default;
    constexpr explicit SortKeyList(const std::array<Key, N>& order) : m_keys(order) {}

    // SortWho / SortAuctionItems(type, key): the key moves to the front keeping its own flag; the first one again
    // turns its order round. False for a key not in the list.
    bool sort(Key key) {
        u32 at = 0;
        while (at < N && m_keys[at] != key) {
            ++at;
        }
        if (at == N) {
            return false;
        }
        bool reversed = m_reversed[at];
        if (at == 0) {
            reversed = !reversed;
        }
        for (u32 i = at; i > 0; --i) {
            m_keys[i] = m_keys[i - 1];
            m_reversed[i] = m_reversed[i - 1];
        }
        m_keys[0] = key;
        m_reversed[0] = reversed;
        ++m_version;
        return true;
    }
    Key key(u32 position) const { return m_keys[position]; }
    bool reversed(u32 position) const { return m_reversed[position]; }
    // IsAuctionSortReversed (0x4D0030): the key's flag wherever it is in the list.
    bool reversedOf(Key key) const {
        for (u32 i = 0; i < N; ++i) {
            if (m_keys[i] == key) {
                return m_reversed[i];
            }
        }
        return false;
    }
    // Bumps with every sort(): the lists sorted by it sort again.
    u32 version() const { return m_version; }
    // a against b by the keys in order (`compare(a, b, key)`: < 0, 0, > 0), each turned round when reversed.
    template <typename T, typename Compare>
    i32 compare(const T& a, const T& b, Compare&& compareKey) const {
        for (u32 i = 0; i < N; ++i) {
            const i32 r = compareKey(a, b, m_keys[i]);
            if (r != 0) {
                return m_reversed[i] ? -r : r;
            }
        }
        return 0;
    }

private:
    std::array<Key, N> m_keys{};
    std::array<bool, N> m_reversed{};
    u32 m_version = 0;
};

// SStrCmpI as the comparators call it: ASCII letters without case, byte by byte.
i32 compareNoCase(std::string_view a, std::string_view b);

// --- Who (FriendsFrame's WhoFrame) --------------------------------------------------------------------------------

// SortWho's keys by WoW.exe's index ("zone", "level", "class", "group", "name", "race", "guild"; anything else is
// "name"). "group" compares nothing in 1.12 (SMSG_WHO no longer sends the party status; its column is commented out
// in FriendsFrame.xml).
enum class WhoSort : u8 { Zone, Level, Class, Group, Name, Race, Guild, Count };
constexpr u32 kWhoSortKeys = static_cast<u32>(WhoSort::Count);
using WhoSortKeys = SortKeyList<WhoSort, kWhoSortKeys>;
// 0x5ADD04: the keys in index order, none reversed.
WhoSortKeys whoSortDefaults();
WhoSort whoSortFromName(std::string_view name);

// The names the comparator reads from the tables (AreaTable, ChrClasses, ChrRaces in the client's language); an
// empty one (no row) goes last (WoW.exe: equal to anything, which is no order to sort by).
struct WhoSortNames {
    const void* user = nullptr;
    std::string_view (*zone)(const void* user, u32 id) = nullptr;
    std::string_view (*cls)(const void* user, u32 id) = nullptr;
    std::string_view (*race)(const void* user, u32 id) = nullptr;
};
// 0x5ADA00 for one key: the level ascending, the names and the guild alphabetically.
i32 compareWho(const WhoEntry& a, const WhoEntry& b, WhoSort key, const WhoSortNames& names);
// The entries' indices in the keys' order into `order` (reserved by the caller); ties keep the server's order.
void sortWho(std::span<const WhoEntry> entries, const WhoSortKeys& keys, const WhoSortNames& names,
             std::vector<u16>& order);

// --- The auction house (Blizzard_AuctionUI) ---------------------------------------------------------------------

// SortAuctionItems' keys by WoW.exe's index ("level", "quality", "bid", "duration", "buyout", "status", "name",
// "seller").
enum class AuctionSort : u8 { Level, Quality, Bid, Duration, Buyout, Status, Name, Seller, Count };
constexpr u32 kAuctionSortKeys = static_cast<u32>(AuctionSort::Count);
using AuctionSortKeys = SortKeyList<AuctionSort, kAuctionSortKeys>;
// 0x4CC238: the browse list in index order; the bids and the player's auctions status, duration, bid, level, name,
// quality, buyout, seller (0x849064 / 0x849084). None reversed.
AuctionSortKeys auctionSortDefaults(AuctionList list);

// What the comparator looks up: the items' templates (the level, the quality, the name) and the sellers' names; the
// player for "status".
struct AuctionSortData {
    u64 me = 0;
    const void* user = nullptr;
    const net::ItemQueryInfo* (*item)(const void* user, u32 entry) = nullptr; // null: not known yet
    const std::string* (*playerName)(const void* user, u64 guid) = nullptr;   // null: not known yet
};
// 0x4CD940 for one key: the required level (at least 1) ascending, the quality best first, the bid (the starting one
// while nobody bid) ascending, the time left ascending, the buyout highest first, the status (the player's bids, then
// others', then none), the item's name and the seller's alphabetically (unknown sellers last). An item whose template
// is not known yet goes last (WoW.exe sorts a page once they all are).
i32 compareAuctions(const net::AuctionItem& a, const net::AuctionItem& b, AuctionSort key, const AuctionSortData& data);
// The page's indices in the keys' order into `order` (reserved by the caller); ties keep the server's order.
void sortAuctions(std::span<const net::AuctionItem> items, const AuctionSortKeys& keys, const AuctionSortData& data,
                  std::vector<u16>& order);

} // namespace vd::game
