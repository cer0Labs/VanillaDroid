#include "game/ListSort.h"

#include <algorithm>

namespace vd::game {
namespace {

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// What WoW.exe's comparators do with two numbers: equal, else 1 when `a` goes after `b`.
i32 ascending(u32 a, u32 b) {
    return a == b ? 0 : a > b ? 1 : -1;
}

// WoW.exe takes a missing table row (or item template) as equal to anything; that is no order std::sort may use
// (it waits for the templates before sorting, and the tables have every id the server sends): an unknown one goes
// last instead.
i32 unknownLast(bool knownA, bool knownB) {
    return knownA == knownB ? 0 : knownA ? -1 : 1;
}

i32 byNames(std::string_view a, std::string_view b) {
    return a.empty() || b.empty() ? unknownLast(!a.empty(), !b.empty()) : compareNoCase(a, b);
}

template <typename T, typename Keys, typename Compare>
void sortIndices(std::span<const T> items, const Keys& keys, Compare&& compareKey, std::vector<u16>& order) {
    order.clear();
    const usize n = std::min<usize>(items.size(), 0xFFFF);
    for (usize i = 0; i < n; ++i) {
        order.push_back(static_cast<u16>(i));
    }
    std::sort(order.begin(), order.end(), [&](u16 x, u16 y) {
        const i32 r = keys.compare(items[x], items[y], compareKey);
        return r != 0 ? r < 0 : x < y;
    });
}

} // namespace

i32 compareNoCase(std::string_view a, std::string_view b) {
    const usize n = std::min(a.size(), b.size());
    for (usize i = 0; i < n; ++i) {
        const u8 x = static_cast<u8>(lower(a[i]));
        const u8 y = static_cast<u8>(lower(b[i]));
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

// --- Who ----------------------------------------------------------------------------------------------------------

WhoSortKeys whoSortDefaults() {
    return WhoSortKeys({WhoSort::Zone, WhoSort::Level, WhoSort::Class, WhoSort::Group, WhoSort::Name, WhoSort::Race,
                        WhoSort::Guild});
}

WhoSort whoSortFromName(std::string_view name) {
    static constexpr const char* kNames[] = {"zone", "level", "class", "group", "name", "race", "guild"};
    for (u32 i = 0; i < kWhoSortKeys; ++i) {
        if (compareNoCase(name, kNames[i]) == 0) {
            return static_cast<WhoSort>(i);
        }
    }
    return WhoSort::Name; // 0x5AD8BB: the default
}

i32 compareWho(const WhoEntry& a, const WhoEntry& b, WhoSort key, const WhoSortNames& names) {
    const auto named = [&](std::string_view (*of)(const void*, u32), u32 x, u32 y) {
        return of ? byNames(of(names.user, x), of(names.user, y)) : 0;
    };
    switch (key) {
    case WhoSort::Zone: return named(names.zone, a.zone, b.zone);
    case WhoSort::Level: return ascending(a.level, b.level);
    case WhoSort::Class: return named(names.cls, a.cls, b.cls);
    case WhoSort::Name: return compareNoCase(a.name, b.name);
    case WhoSort::Race: return named(names.race, a.race, b.race);
    case WhoSort::Guild: return compareNoCase(a.guild, b.guild);
    default: return 0; // "group"
    }
}

void sortWho(std::span<const WhoEntry> entries, const WhoSortKeys& keys, const WhoSortNames& names,
             std::vector<u16>& order) {
    sortIndices(entries, keys,
                [&](const WhoEntry& a, const WhoEntry& b, WhoSort key) { return compareWho(a, b, key, names); },
                order);
}

// --- The auction house --------------------------------------------------------------------------------------------

AuctionSortKeys auctionSortDefaults(AuctionList list) {
    if (list == AuctionList::Browse) {
        return AuctionSortKeys({AuctionSort::Level, AuctionSort::Quality, AuctionSort::Bid, AuctionSort::Duration,
                                AuctionSort::Buyout, AuctionSort::Status, AuctionSort::Name, AuctionSort::Seller});
    }
    return AuctionSortKeys({AuctionSort::Status, AuctionSort::Duration, AuctionSort::Bid, AuctionSort::Level,
                            AuctionSort::Name, AuctionSort::Quality, AuctionSort::Buyout, AuctionSort::Seller});
}

i32 compareAuctions(const net::AuctionItem& a, const net::AuctionItem& b, AuctionSort key, const AuctionSortData& data) {
    const net::ItemQueryInfo* x = nullptr;
    const net::ItemQueryInfo* y = nullptr;
    i32 unknown = 0;
    const auto items = [&]() {
        x = data.item ? data.item(data.user, a.itemId) : nullptr;
        y = data.item ? data.item(data.user, b.itemId) : nullptr;
        unknown = unknownLast(x != nullptr, y != nullptr);
        return x && y;
    };
    switch (key) {
    case AuctionSort::Level:
        return items() ? ascending(std::max<u32>(x->requiredLevel, 1), std::max<u32>(y->requiredLevel, 1)) : unknown;
    case AuctionSort::Quality: return items() ? -ascending(x->quality, y->quality) : unknown;
    case AuctionSort::Bid: return ascending(a.bid != 0 ? a.bid : a.startBid, b.bid != 0 ? b.bid : b.startBid);
    case AuctionSort::Duration: return ascending(a.timeLeftMs, b.timeLeftMs);
    case AuctionSort::Buyout: return -ascending(a.buyout, b.buyout);
    case AuctionSort::Status: {
        const auto status = [&](const net::AuctionItem& i) -> u32 {
            return i.bidder != 0 && i.bidder == data.me ? 2u : i.bidder != 0 ? 1u : 0u;
        };
        return -ascending(status(a), status(b));
    }
    case AuctionSort::Name: return items() ? compareNoCase(x->name, y->name) : unknown;
    case AuctionSort::Seller: {
        const std::string* p = data.playerName && a.owner ? data.playerName(data.user, a.owner) : nullptr;
        const std::string* q = data.playerName && b.owner ? data.playerName(data.user, b.owner) : nullptr;
        if (!p) {
            return q ? 1 : 0;
        }
        return q ? compareNoCase(*p, *q) : -1;
    }
    default: return 0;
    }
}

void sortAuctions(std::span<const net::AuctionItem> items, const AuctionSortKeys& keys, const AuctionSortData& data,
                  std::vector<u16>& order) {
    sortIndices(items, keys,
                [&](const net::AuctionItem& a, const net::AuctionItem& b, AuctionSort key) {
                    return compareAuctions(a, b, key, data);
                },
                order);
}

} // namespace vd::game
