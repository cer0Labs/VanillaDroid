#include "app/P3Gallery.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <cstring>

namespace vd {
namespace {

using ui::rgba;

// A sample chat-cache.txt as WoW.exe writes it: General and the Combat Log, a whispers window at 16 pt with a darker
// background, and one for the Trade channel.
constexpr const char* kSampleCache = "VERSION 2\n\nADDEDVERSION 2\n\nOPTION_GUILD_RECRUITMENT_CHANNEL AUTO\n\n"
                                     "CHANNELS\nEND\n\nZONECHANNELS 0\n\nCOLORS\nEND\n\n"
                                     "WINDOW 1\nSIZE 0\nCOLOR 0 0 0 0\nLOCKED 1\nDOCKED 1\nSHOWN 1\n\nMESSAGES\n"
                                     "SYSTEM\nSAY\nYELL\nPARTY\nGUILD\nCREATURE\nCHANNEL\nSKILL\nLOOT\nEND\n\n"
                                     "CHANNELS\nGeneral\nLocalDefense\nEND\n\nZONECHANNELS 0\n\nEND\n\n"
                                     "WINDOW 2\nSIZE 0\nCOLOR 0 0 0 0\nLOCKED 1\nDOCKED 2\nSHOWN 1\n\nMESSAGES\n"
                                     "COMBAT_SELF_HITS\nCOMBAT_XP_GAIN\nMONEY\nEND\n\nCHANNELS\nEND\n\n"
                                     "ZONECHANNELS 0\n\nEND\n\n"
                                     "WINDOW 3\nNAME Whispers\nSIZE 16\nCOLOR 0 0 0 128\nLOCKED 0\nDOCKED 3\nSHOWN 1\n\n"
                                     "MESSAGES\nWHISPER\nEND\n\nCHANNELS\nEND\n\nZONECHANNELS 0\n\nEND\n\n"
                                     "WINDOW 4\nSIZE 0\nCOLOR 0 0 64 64\nLOCKED 0\nDOCKED 4\nSHOWN 1\n\nMESSAGES\n"
                                     "END\n\nCHANNELS\nTrade\nEND\n\nZONECHANNELS 0\n\nEND\n\n";

struct SampleLine {
    u8 group;          // game::ChatGroup, or 0xFF: a channel's message
    const char* channel;
    const char* text;
    u32 color;
};
constexpr u8 kChannelLine = 0xFF;
const SampleLine kSampleLines[] = {
    {game::kGroupChannel, "", "Joined Channel: [1. General - Elwynn Forest]", rgba(255, 192, 192)},
    {kChannelLine, "General", "[1. General] [Galen]: anyone for Hogger?", rgba(255, 192, 192)},
    {game::kGroupSay, "", "[Arthel] says: almost got the 10 tails", rgba(255, 255, 255)},
    {kChannelLine, "Trade", "[2. Trade] [Brunna]: WTS [Militia Shortsword] 1g", rgba(255, 192, 192)},
    {game::kGroupLoot, "", "You receive loot: [Linen Cloth]x2.", rgba(0, 170, 0)},
    {game::kGroupWhisper, "", "[Aeryn] whispers: coming to the mine?", rgba(255, 128, 255)},
    {game::kGroupMoney, "", "You loot 7 Copper", rgba(255, 255, 0)},
    {game::kGroupParty, "", "[Party] [Aeryn]: the kobolds at the entrance are mine", rgba(170, 170, 255)},
    {game::kGroupCombatXpGain, "", "Kobold Vermin dies, you gain 45 experience.", rgba(111, 111, 255)},
    {game::kGroupWhisper, "", "To [Aeryn]: on my way", rgba(255, 128, 255)},
    {kChannelLine, "Trade", "[2. Trade] [Galen]: LF enchanter", rgba(255, 192, 192)},
};

// The who list's sample: SMSG_WHO's fields (name, guild, level, class, race, zone) and the tables' names.
const game::WhoEntry kWho[] = {
    {"Arthel", "Los Defias", 12, 1, 1, 12},    {"Aeryn", "", 14, 8, 4, 40},
    {"galen", "Knights of Lordaeron", 60, 2, 3, 1519}, {"Brunna", "los defias", 9, 5, 3, 12},
    {"Tolvir", "", 27, 3, 4, 1519},            {"Cassia", "Ashen Verdict", 14, 4, 1, 12},
};
std::string_view sampleZone(const void*, u32 id) {
    return id == 12 ? "Elwynn Forest" : id == 40 ? "Westfall" : id == 1519 ? "Stormwind City" : std::string_view{};
}
std::string_view sampleClass(const void*, u32 id) {
    static constexpr const char* kClasses[] = {"", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "", "", "Mage"};
    return id < std::size(kClasses) ? std::string_view(kClasses[id]) : std::string_view{};
}
std::string_view sampleRace(const void*, u32 id) {
    static constexpr const char* kRaces[] = {"", "Human", "Orc", "Dwarf", "Night Elf"};
    return id < std::size(kRaces) ? std::string_view(kRaces[id]) : std::string_view{};
}

// The auction house's sample: the templates the comparator reads and a page of auctions.
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
struct SampleItem {
    u32 entry;
    const char* icon;
    const char* name;
    u32 quality, level;
};
const SampleItem kItems[] = {{2589, "INV_Fabric_Linen_01", "Linen Cloth", 1, 0},
                             {2996, "INV_Fabric_Linen_02", "Bolt of Linen Cloth", 1, 0},
                             {2580, "INV_Chest_Cloth_09", "Linen Vest", 1, 8},
                             {4245, "INV_Misc_Bag_09", "Linen Bag", 1, 0},
                             {3587, "INV_Belt_02", "Heavy Linen Belt", 2, 12},
                             {12360, "INV_Ingot_08", "Arcanite Bar", 2, 0},
                             {12784, "INV_Axe_09", "Arcanite Reaper", 4, 58}};
constexpr u64 kMe = 0x99;
const net::AuctionItem kAuctions[] = {
    // id, item, enchant, prop, suffix, count, charges, owner, start, outbid, buyout, time left, bidder, bid
    {1, 2589, 0, 0, 0, 20, 0, 0x10, 380, 0, 600, 40'000'000, 0, 0},
    {2, 2996, 0, 0, 0, 10, 0, 0x11, 1200, 50, 2000, 6'000'000, 0x55, 1450},
    {3, 2580, 0, 0, 0, 1, 0, 0x12, 2500, 0, 0, 1'200'000, 0, 0},
    {4, 4245, 0, 0, 0, 1, 0, 0x13, 4000, 100, 5000, 600'000, kMe, 4200},
    {5, 3587, 0, 0, 0, 1, 0, 0x10, 9800, 0, 15000, 40'000'000, 0, 0},
    {6, 12360, 0, 0, 0, 4, 0, 0x14, 120000, 2000, 0, 20'000'000, 0x56, 130000},
    {7, 12784, 0, 0, 0, 1, 0, 0x11, 2500000, 0, 4000000, 43'000'000, 0, 0},
};
const char* sampleSeller(u64 guid) {
    switch (guid) {
    case 0x10: return "Galen";
    case 0x11: return "Tolvir";
    case 0x12: return "Brakk";
    case 0x13: return "Aeryn";
    case 0x14: return "brunna";
    case 0x55: return "Cassia";
    case 0x56: return "Vex";
    default: return nullptr;
    }
}
const net::ItemQueryInfo* sampleItem(const void*, u32 entry) {
    static std::array<net::ItemQueryInfo, std::size(kItems)> infos = [] {
        std::array<net::ItemQueryInfo, std::size(kItems)> out{};
        for (u32 i = 0; i < std::size(kItems); ++i) {
            out[i].entry = kItems[i].entry;
            out[i].found = true;
            out[i].name = kItems[i].name;
            out[i].quality = kItems[i].quality;
            out[i].requiredLevel = kItems[i].level;
        }
        return out;
    }();
    for (const net::ItemQueryInfo& i : infos) {
        if (i.entry == entry) {
            return &i;
        }
    }
    return nullptr;
}
const std::string* sampleName(const void*, u64 guid) {
    static std::array<std::string, 8> names;
    const char* n = sampleSeller(guid);
    if (!n) {
        return nullptr;
    }
    std::string& slot = names[guid % names.size()];
    slot = n;
    return &slot;
}
// AuctionFrame_GetTimeLeftText: Short (< 30 min), Medium (< 2 h), Long (< 8 h), Very Long.
const char* timeLeftKey(u32 ms) {
    return ms < 1'800'000 ? "AUCTION_TIME_LEFT1" : ms < 7'200'000 ? "AUCTION_TIME_LEFT2"
           : ms < 28'800'000 ? "AUCTION_TIME_LEFT3" : "AUCTION_TIME_LEFT4";
}

} // namespace

bool P3Gallery::select(const char* name) {
    static constexpr const char* kNames[] = {"tutorial",     "tutorial42",  "chatwindows", "chatconfig",
                                             "chatfilters",  "abandonitems", "deletemail", "deletemoney",
                                             "namechat",     "whosort",     "auctionsort", "auctionsort2",
                                             "auctionsort3"};
    for (u32 i = 0; i < std::size(kNames); ++i) {
        if (std::strcmp(name, kNames[i]) == 0) {
            m_page = static_cast<Page>(i);
            m_ready = false;
            return true;
        }
    }
    return false;
}

void P3Gallery::draw(DialogContext& c, u64 nowMs) {
    if (!m_ready) {
        m_ready = true;
        m_windows.parse(kSampleCache);
        if (m_page == Page::Tutorial) {
            m_tutorials.demo(static_cast<u32>(game::Tutorial::Backpack), 4, static_cast<u32>(nowMs));
            m_tutorials.open(static_cast<u32>(game::Tutorial::Backpack), false);
        } else if (m_page == Page::Tutorial42) {
            m_tutorials.demo(0, 1, static_cast<u32>(nowMs)); // Questgivers waits behind it
            m_tutorials.open(static_cast<u32>(game::Tutorial::Welcome), true);
        }
        m_config.open(0);
        if (m_page == Page::ChatFilters) {
            m_config.openFilter(1, game::ChatMenu::Combat);
            m_selected = 1;
        }
    }
    const f32 castTop = c.safeArea.bottom() - c.ui.px(76.0f);
    switch (m_page) {
    case Page::Tutorial:
    case Page::Tutorial42:
        m_tutorials.drawAlerts(nullptr, c, castTop, {}, static_cast<u32>(nowMs));
        m_tutorials.drawFrame(nullptr, c, castTop, {});
        break;
    case Page::ChatWindows:
    case Page::ChatConfig:
    case Page::ChatFilters: drawChat(c, nowMs); break;
    case Page::WhoSort: drawWho(c); break;
    case Page::AuctionSort: drawAuctions(c, 0); break;
    case Page::AuctionSortBids: drawAuctions(c, 1); break;
    case Page::AuctionSortOwned: drawAuctions(c, 2); break;
    default: drawPopup(c); break;
    }
}

void P3Gallery::drawChat(DialogContext& c, u64 nowMs) {
    (void)nowMs;
    std::array<u8, game::kChatWindowSlots> docked{};
    const u32 count = std::min(m_windows.docked(docked), game::kChatWindows);
    u32 selected = 0;
    for (u32 i = 0; i < count; ++i) {
        const game::ChatWindowConfig& w = m_windows.window(docked[i]);
        std::string_view name = w.name;
        if (name.empty()) {
            name = docked[i] == 0   ? text::get("GENERAL")
                   : docked[i] == 1 ? text::get("COMBAT_LOG")
                                    : text::format(m_tabNames[i], "CHAT_NAME_TEMPLATE", docked[i] + 1u);
        }
        m_tabs[i] = {name, 0.0f};
        if (docked[i] == m_selected) {
            selected = i;
        }
    }
    const game::ChatWindowConfig& w = m_windows.window(m_selected);
    m_lines.clear();
    u32 key = 1;
    for (const SampleLine& s : kSampleLines) {
        const bool shown = s.group == kChannelLine ? w.hasChannel(s.channel) : w.hasGroup(s.group);
        if (shown) {
            m_lines.push_back({s.text, s.color, key, {}});
        }
        ++key;
    }
    ChatFrameView view;
    view.lines = m_lines;
    view.tabs = {m_tabs.data(), count};
    view.selected = selected;
    view.expanded = true; // phase 20: the tabs (and their menus) show only open
    view.backgroundAlpha = 1.0f;
    view.fontScale = w.fontSize != 0 ? static_cast<f32>(w.fontSize) / 14.0f : 1.0f;
    view.backgroundColor = rgba(w.r, w.g, w.b);
    view.restAlpha = static_cast<f32>(w.a) / 255.0f;
    view.header = text::get("CHAT_SAY_SEND");
    view.headerColor = ui::kWhite;
    view.hint = text::get("VD_SOCIAL_CHAT_HINT");
    const ui::Rect frame = chatFrameRect(c, true, {}, {}, false, view.fontScale);
    const ChatFrameResult r = chatFrame(c, frame, view, m_chatState);
    if (r.tab >= 0 && static_cast<u32>(r.tab) < count) {
        m_selected = docked[static_cast<u32>(r.tab)];
    }
    if (r.tabMenu >= 0 && static_cast<u32>(r.tabMenu) < count) {
        m_config.open(docked[static_cast<u32>(r.tabMenu)]);
        m_menuOpen = true;
    }
    if (m_page != Page::ChatWindows && m_menuOpen) {
        m_menuOpen = m_config.draw(nullptr, c, m_windows);
        if (m_windows.changed()) {
            VD_LOG_INFO("P3 gallery: chat-cache.txt now\n%s", m_windows.format().c_str());
            m_windows.clearChanged();
        }
    }
}

void P3Gallery::drawWho(DialogContext& c) {
    const game::WhoSortNames names{nullptr, sampleZone, sampleClass, sampleRace};
    game::sortWho(kWho, m_whoKeys, names, m_whoOrder); // a gallery: every frame (the game sorts once per answer)
    m_whoRows.clear();
    for (const u16 i : m_whoOrder) {
        const game::WhoEntry& w = kWho[i];
        m_whoLevels[i] = std::to_string(w.level);
        WhoRowView& r = m_whoRows.emplace_back();
        r.name = w.name;
        r.variable = m_whoColumn == 1   ? std::string_view(w.guild)
                     : m_whoColumn == 2 ? sampleRace(nullptr, w.race)
                                        : sampleZone(nullptr, w.zone);
        r.level = m_whoLevels[i];
        r.cls = sampleClass(nullptr, w.cls);
    }
    static constexpr const char* kColumnKeys[] = {"ZONE", "GUILD", "RACE"};
    static constexpr game::WhoSort kColumnSorts[] = {game::WhoSort::Zone, game::WhoSort::Guild, game::WhoSort::Race};
    m_whoHeaders[0] = {text::get("NAME"), 83.0f};
    m_whoHeaders[1] = {text::get(kColumnKeys[m_whoColumn]), 120.0f, false, false, true};
    m_whoHeaders[2] = {text::get("LEVEL_ABBR"), 32.0f};
    m_whoHeaders[3] = {text::get("CLASS"), 92.0f};
    WhoFrameView v; // retouches (friends frame): WhoFrame with its 1.12 art
    v.rows = m_whoRows;
    v.headers = m_whoHeaders;
    v.selected = m_whoSelected;
    char totals[48];
    v.totals = text::format(totals, "WHO_FRAME_TOTAL_TEMPLATE_P1", static_cast<u32>(m_whoRows.size()));
    v.input = "z-\"Elwynn Forest\" 10-20";
    const WhoFrameResult r = whoFrame(c, v);
    if (r.header >= 0) {
        m_whoKeys.sort(r.header == 0   ? game::WhoSort::Name
                       : r.header == 1 ? kColumnSorts[m_whoColumn]
                       : r.header == 2 ? game::WhoSort::Level
                                       : game::WhoSort::Class);
        VD_LOG_INFO("P3 gallery: who sorted by header %d", r.header);
    }
    if (r.row >= 0) {
        m_whoSelected = r.row;
    }
    if (r.headerMenu == 1) {
        m_whoMenu = true;
    }
    if (m_whoMenu) {
        for (u32 i = 0; i < 3; ++i) {
            m_whoChoices[i] = {text::get(kColumnKeys[i]), ui::kWhite, i == m_whoColumn};
        }
        const i32 picked = choiceList(c, "p3.whocolumn", text::get("WHO"), m_whoChoices);
        if (picked >= 0) {
            m_whoColumn = static_cast<u32>(picked);
        }
        if (picked != -1) {
            m_whoMenu = false;
        }
    }
}

void P3Gallery::drawAuctions(DialogContext& c, u32 tab) {
    // The bids list: the auctions with a bid of the player's or outbid ones; the player's: three of them.
    std::array<net::AuctionItem, std::size(kAuctions)> page{};
    u32 count = 0;
    for (const net::AuctionItem& a : kAuctions) {
        const bool bid = a.bidder == kMe || a.id == 2 || a.id == 6;
        const bool mine = a.id == 3 || a.id == 6 || a.id == 7;
        if (tab == 0 || (tab == 1 && bid) || (tab == 2 && mine)) {
            page[count] = a;
            if (tab == 2) {
                page[count].owner = kMe;
            }
            ++count;
        }
    }
    const std::span<const net::AuctionItem> items{page.data(), count};
    game::AuctionSortKeys& keys = m_auctionKeys[tab];
    const game::AuctionSortData data{kMe, nullptr, sampleItem, sampleName};
    game::sortAuctions(items, keys, data, m_auctionOrder);
    m_auctionRows.clear();
    for (const u16 i : m_auctionOrder) {
        const net::AuctionItem& a = items[i];
        const net::ItemQueryInfo* info = sampleItem(nullptr, a.itemId);
        AuctionRowView& r = m_auctionRows.emplace_back();
        r.cell.empty = false;
        r.cell.count = a.count;
        r.cell.quality = info ? info->quality : 1;
        for (const SampleItem& s : kItems) {
            if (s.entry == a.itemId) {
                r.cell.iconHash = iconHash(s.icon);
            }
        }
        r.cell.ref = {ItemRef::Kind::Window, 0, 0, static_cast<u32>(m_auctionRows.size() - 1)};
        r.name = info ? std::string_view(info->name) : std::string_view("...");
        r.level = info ? info->requiredLevel : 0;
        // Phase 18 (extras): a level 10 player's red levels (Arcanite Reaper 58, Heavy Linen Belt 12, the reaper's
        // icon red too), its auctions without a bid at their starting price, at half alpha.
        r.levelTooHigh = r.level > 10;
        r.cell.usable = r.level <= 10;
        r.startingPrice = tab == 2 && a.bid == 0;
        r.timeLeft = text::get(timeLeftKey(a.timeLeftMs));
        r.hasBid = a.bid != 0;
        r.bid = a.bid != 0 ? a.bid : a.startBid;
        r.buyout = a.buyout;
        r.leading = a.bidder == kMe && a.bid != 0;
        r.outbid = tab == 1 && a.bidder != kMe;
        if (tab == 0) {
            const char* seller = sampleSeller(a.owner);
            r.seller = seller ? std::string_view(seller) : std::string_view("...");
        } else if (tab == 1) {
            r.seller = r.leading ? text::get("HIGH_BIDDER") : text::get("OUTBID");
        } else {
            const char* bidder = sampleSeller(a.bidder);
            r.seller = bidder ? std::string_view(bidder) : text::get("NO_BIDS");
        }
    }
    const std::span<const AuctionHeaderSpec> specs = auctionHeaderSpecs(tab);
    for (u32 i = 0; i < specs.size(); ++i) {
        m_auctionHeaders[i] = {text::get(specs[i].key), specs[i].width, true, keys.reversedOf(specs[i].sort)};
        m_auctionColumns[i] = specs[i].column;
    }
    AuctionView v;
    v.title = "Stormwind Auction House";
    v.tab = tab;
    v.filter.name = "linen";
    v.filter.category = text::get("ALL");
    v.filter.quality = text::get("ALL");
    v.rows = m_auctionRows;
    v.selected = m_auctionSelected;
    v.pages = 1;
    v.total = count;
    v.money = 1234567;
    v.sell.problem = " ";
    v.headers = {m_auctionHeaders.data(), specs.size()};
    v.columns = {m_auctionColumns.data(), specs.size()};
    const AuctionResult r = auctionWindow(c, v, m_drag);
    if (r.header >= 0 && static_cast<u32>(r.header) < specs.size()) {
        keys.sort(specs[static_cast<u32>(r.header)].sort);
        VD_LOG_INFO("P3 gallery: auctions sorted by header %d", r.header);
    }
    if (r.row >= 0) {
        m_auctionSelected = r.row;
    }
    m_drag.finish();
}

void P3Gallery::drawPopup(DialogContext& c) {
    PopupView& v = m_views[0];
    v = PopupView{};
    switch (m_page) {
    case Page::AbandonItems: {
        const PopupInfo& info = popupInfo(PopupKind::AbandonQuestWithItems);
        v.text = text::format(m_popupText, info.text, "Kobold Candles", "Large Candle, Gold Dust");
        v.button1 = text::get(info.button1);
        v.button2 = text::get(info.button2);
        break;
    }
    case Page::DeleteMail: {
        const PopupInfo& info = popupInfo(PopupKind::DeleteMail);
        v.text = text::format(m_popupText, info.text, "Wolfskin Bracers");
        v.button1 = text::get(info.button1);
        v.button2 = text::get(info.button2);
        v.alert = true;
        break;
    }
    case Page::DeleteMoney: {
        const PopupInfo& info = popupInfo(PopupKind::DeleteMoney);
        v.text = text::get(info.text);
        v.button1 = text::get(info.button1);
        v.button2 = text::get(info.button2);
        v.alert = true;
        v.money = true;
        v.copper = 12345;
        break;
    }
    case Page::NameChat: {
        const PopupInfo& info = popupInfo(PopupKind::NameChat);
        v.text = text::get(info.text);
        v.button1 = text::get(info.button1);
        v.button2 = text::get(info.button2);
        v.editBox = true;
        v.editText = "Whispers";
        break;
    }
    default: return;
    }
    const PopupTaps taps = popupStack(c, m_views, m_heights);
    if (taps.slot >= 0) {
        VD_LOG_INFO("P3 gallery: popup button %u", taps.button);
    }
}

} // namespace vd
