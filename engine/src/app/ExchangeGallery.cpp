#include "app/ExchangeGallery.h"

#include "app/DialogKit.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace vd {
namespace {

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

struct Sample {
    const char* icon;
    const char* name;
    u32 count;
    u32 quality;
};

// A level-20 character's backpack and pouch (the same names the server would send).
constexpr Sample kBackpack[16] = {
    {"INV_Misc_Rune_01", "Hearthstone", 1, 1},
    {"INV_Fabric_Linen_01", "Linen Cloth", 20, 1},
    {"INV_Fabric_Wool_01", "Wool Cloth", 14, 1},
    {"INV_Misc_Food_15", "Tough Hunk of Bread", 5, 1},
    {"INV_Potion_51", "Lesser Healing Potion", 4, 1},
    {"INV_Jewelry_Ring_15", "Sapphire Ring", 1, 2},
    {"INV_Sword_27", "Cruel Barb", 1, 3},
    {nullptr, nullptr, 0, 0},
    {"INV_Ore_Copper_01", "Copper Ore", 12, 1},
    {"INV_Misc_Pelt_Wolf_01", "Light Leather", 9, 1},
    {nullptr, nullptr, 0, 0},
    {"INV_Misc_Bone_06", "Broken Fang", 2, 0},
    {nullptr, nullptr, 0, 0},
    {nullptr, nullptr, 0, 0},
    {"INV_Misc_Gem_Variety_01", "Malachite", 3, 1},
    {nullptr, nullptr, 0, 0},
};
constexpr Sample kPouch[6] = {{"INV_Misc_Food_14", "Tough Wolf Meat", 8, 1},
                              {nullptr, nullptr, 0, 0},
                              {"INV_Misc_Book_09", "Old Book", 1, 1},
                              {nullptr, nullptr, 0, 0},
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

ItemView itemOf(const Sample& s) {
    return {iconHash(s.icon), s.name, s.count, s.quality};
}

constexpr Sample kRollItems[] = {{"INV_Sword_27", "Cruel Barb", 1, 3},
                                 {"INV_Chest_Leather_03", "Defias Leather Vest", 1, 2},
                                 {"INV_Misc_Gem_Variety_01", "Malachite", 1, 1}};

// Subjects and expiries of the auction house's letters are the interface's (formatted in Page::Mail).
const MailRowView kMailRows[] = {
    {"Aeryn", "Linen for your bandages", {}, iconHash("INV_Fabric_Linen_01"), 1, 20, 0, 1500, true, false},
    {"Stormwind Auction House", {}, {}, iconHash("INV_Misc_Coin_01"), 1, 0, 4750, 0, true, false},
    {"Brakk", "Guild meeting", {}, iconHash("INV_Letter_15"), 1, 0, 0, 0, false, false},
    {"Stormwind Auction House", {}, {}, iconHash("INV_Misc_Coin_01"), 1, 0, 6000, 0, false, false},
    {"Marshal McBride", "Your first step", {}, iconHash("INV_Letter_15"), 1, 0, 0, 0, false, false},
};

AuctionRowView auctionRow(const char* icon, const char* name, u32 count, u32 quality, u32 level,
                          std::string_view time, const char* seller, u32 bid, bool hasBid, u32 buyout,
                          bool leading = false, bool outbid = false) {
    AuctionRowView r;
    r.cell.empty = false;
    r.cell.iconHash = iconHash(icon);
    r.cell.count = count;
    r.cell.quality = quality;
    r.name = name;
    r.level = level;
    r.timeLeft = time;
    r.seller = seller;
    r.bid = bid;
    r.hasBid = hasBid;
    r.buyout = buyout;
    r.leading = leading;
    r.outbid = outbid;
    return r;
}

} // namespace

void ExchangeGallery::log(const char* what, i32 value) {
    VD_LOG_INFO("UI gallery: %s %d", what, value);
}

void ExchangeGallery::bags(BagsView& out) {
    m_bagBar.clear();
    m_cells.clear();
    m_sections.clear();
    ItemSlotView backpack;
    backpack.ref = {ItemRef::Kind::Slot, 255, 255, 0};
    backpack.empty = false;
    backpack.iconHash = assetPathHash("Interface\\Buttons\\Button-Backpack-Up.blp");
    m_bagBar.push_back(backpack);
    m_bagBar.push_back(
        cellOf({"INV_Misc_Bag_09", "Small Brown Pouch", 1, 1}, {ItemRef::Kind::Slot, 255, 19, 0}));
    for (u8 i = 1; i < 4; ++i) {
        m_bagBar.push_back(
            cellOf({nullptr, nullptr, 0, 0}, {ItemRef::Kind::Slot, 255, static_cast<u8>(19 + i), 0}));
    }
    m_sections.push_back({text::get("BACKPACK_TOOLTIP"), 0, 16, false});
    for (u8 i = 0; i < 16; ++i) {
        m_cells.push_back(cellOf(kBackpack[i], {ItemRef::Kind::Slot, 255, static_cast<u8>(23 + i), 0}));
    }
    m_sections.push_back({"Small Brown Pouch", 16, 6, false});
    for (u8 i = 0; i < 6; ++i) {
        m_cells.push_back(cellOf(kPouch[i], {ItemRef::Kind::Slot, 19, i, 0}));
    }
    out.bagBar = m_bagBar;
    out.sections = m_sections;
    out.cells = m_cells;
    out.money = 1234567;
    out.freeSlots = 11;
    out.totalSlots = 22;
}

void ExchangeGallery::draw(DialogContext& c, Page page, u64 nowMs) {
    if (m_startMs == 0) {
        m_startMs = nowMs;
    }
    m_drag.track(c.ui.touches());
    switch (page) {
    case Page::Rolls: {
        // Three rolls at different points of their minute.
        const u32 elapsed = static_cast<u32>(nowMs - m_startMs) % 60000;
        RollView rolls[3];
        for (u32 i = 0; i < 3; ++i) {
            rolls[i].item = itemOf(kRollItems[i]);
            rolls[i].countdownMs = 60000;
            const u32 start = i * 17000;
            rolls[i].remainingMs = 60000 - (elapsed + start) % 60000;
        }
        const RollResult r = rollFrames(c, rollFramesTop(c), rolls);
        if (r.roll >= 0) {
            log(r.vote == 1 ? "need" : r.vote == 2 ? "greed" : "pass", r.roll);
        }
        if (r.tooltip >= 0) {
            log("roll tooltip", r.tooltip);
        }
        mailIndicator(c);
        break;
    }
    case Page::Mail: {
        MailView v;
        v.tab = 0;
        v.loaded = true;
        MailRowView rows[std::size(kMailRows)];
        std::copy(std::begin(kMailRows), std::end(kMailRows), rows);
        // As ExchangeScreens writes them: the house's subjects and the time left.
        rows[1].subject = text::format(m_text[0], "AUCTION_SOLD_MAIL_SUBJECT", "Copper Ore");
        rows[3].subject = text::format(m_text[1], "AUCTION_OUTBID_MAIL_SUBJECT", "Cruel Barb");
        rows[0].expires = text::format(m_text[2], "DAY_ONELETTER_ABBR", 29);
        rows[1].expires = rows[0].expires;
        rows[2].expires = text::format(m_text[3], "DAY_ONELETTER_ABBR", 2);
        rows[3].expires = text::format(m_text[4], "HOUR_ONELETTER_ABBR", 23);
        rows[4].expires = text::format(m_text[5], "MINUTE_ONELETTER_ABBR", 12);
        v.rows = rows;
        v.selected = m_mailSelected;
        v.read.sender = "Aeryn";
        v.read.subject = "Linen for your bandages";
        v.read.body =
            "Here are the twenty bolts I promised you. Pay what we agreed and keep your bandages "
            "coming: the kobolds in the Jasperlode Mine are no joke.\n\nSee you in Goldshire,\nAeryn";
        v.read.item = cellOf({"INV_Fabric_Linen_01", "Linen Cloth", 20, 1}, {ItemRef::Kind::Window, 0, 0, 0});
        v.read.itemName = "Linen Cloth";
        v.read.cod = 1500;
        v.read.canTakeItem = true;
        v.read.canReply = v.read.canReturn = v.read.canCopy = true;
        const MailResult r = mailWindow(c, v, m_drag);
        if (r.row >= 0) {
            m_mailSelected = r.row;
            log("mail row", r.row);
        }
        if (r.button >= 0) {
            log("mail button", r.button);
        }
        break;
    }
    case Page::MailSend: {
        MailView v;
        v.tab = 1;
        MailSendView& s = v.send;
        s.to = "Aeryn";
        s.subject = "Wool for the tailor";
        s.body = "Fourteen bolts of wool, as you asked. Send me the shirt when it is done!";
        s.attachment = cellOf(kBackpack[2], {ItemRef::Kind::Window, 0, 0, 1});
        s.attachmentName = kBackpack[2].name;
        s.cod = m_cod;
        s.money = 2500;
        s.cost = 30;
        s.playerMoney = 1234567;
        s.editing = 1;
        bags(s.bags);
        m_cells[2].dimmed = true; // the attachment
        const MailResult r = mailWindow(c, v, m_drag);
        if (r.button == kMailToggleCod) {
            m_cod = !m_cod;
        }
        if (r.button >= 0 || r.field >= 0) {
            log("mail send button/field", r.button >= 0 ? r.button : 100 + r.field);
        }
        if (r.items.dropFrom.valid()) {
            log("mail drop zone", r.items.dropZone);
        }
        break;
    }
    case Page::Trade: {
        TradeView v;
        v.mine.slots[0] = cellOf(kBackpack[1], {ItemRef::Kind::Window, 0, 0, 0});
        v.mine.names[0] = kBackpack[1].name;
        v.mine.slots[1] = cellOf(kBackpack[4], {ItemRef::Kind::Window, 0, 0, 1});
        v.mine.names[1] = kBackpack[4].name;
        for (u32 i = 2; i < 7; ++i) {
            v.mine.slots[i].ref = {ItemRef::Kind::Window, 0, 0, i};
        }
        v.mine.money = 1500;
        v.mine.accepted = m_accepted;
        v.theirs.name = "Aeryn";
        v.theirs.slots[0] = cellOf(kRollItems[1], {ItemRef::Kind::Window, 0, 0, 100});
        v.theirs.names[0] = kRollItems[1].name;
        v.theirs.slots[6] =
            cellOf({"INV_Sword_04", "Militia Shortsword", 1, 1}, {ItemRef::Kind::Window, 0, 0, 106});
        v.theirs.names[6] = "Militia Shortsword";
        v.theirs.money = 0;
        v.theirs.accepted = true;
        v.playerMoney = 1234567;
        bags(v.bags);
        m_cells[1].dimmed = m_cells[4].dimmed = true;
        const TradeResult r = tradeWindow(c, v, m_drag);
        if (r.button == kTradeAccept || r.button == kTradeUnaccept) {
            m_accepted = r.button == kTradeAccept;
        }
        if (r.button >= 0) {
            log("trade button", r.button);
        }
        if (r.items.dropFrom.valid()) {
            log("trade drop zone", r.items.dropZone);
        }
        break;
    }
    case Page::MailEdit:
        if (compactEditor(c, text::get("MAIL_LABEL"), text::get("VD_EXCHANGE_MESSAGE"),
                          "Fourteen bolts of wool, as you asked. Send me the shirt when it is done!", true)) {
            log("editor done", 0);
        }
        break;
    case Page::Auction:
    case Page::AuctionBids:
    case Page::AuctionSell: {
        AuctionView v;
        v.title = "Stormwind Auction House";
        v.tab = page == Page::Auction ? 0 : page == Page::AuctionBids ? 1 : 2;
        v.filter.name = "linen";
        v.filter.category = "Trade Goods";
        v.filter.quality = text::get("ALL");
        v.filter.levelMin = 10;
        v.filter.levelMax = 25;
        AuctionRowView rows[5] = {auctionRow("INV_Fabric_Linen_01", "Linen Cloth", 20, 1, 0,
                                             text::get("AUCTION_TIME_LEFT4"), "Galen", 380, false, 600),
                                  auctionRow("INV_Fabric_Linen_02", "Bolt of Linen Cloth", 10, 1, 0,
                                             text::get("AUCTION_TIME_LEFT3"), "Tolvir", 1450, true, 2000),
                                  auctionRow("INV_Chest_Cloth_09", "Linen Vest", 1, 1, 8,
                                             text::get("AUCTION_TIME_LEFT2"), "Brakk", 2500, false, 0),
                                  auctionRow("INV_Misc_Bag_09", "Linen Bag", 1, 1, 0,
                                             text::get("AUCTION_TIME_LEFT1"), "Aeryn", 4200, true, 5000),
                                  auctionRow("INV_Belt_02", "Heavy Linen Belt", 1, 2, 12,
                                             text::get("AUCTION_TIME_LEFT4"), "Galen", 9800, false, 15000)};
        if (page == Page::AuctionBids) {
            rows[0].seller = text::get("HIGH_BIDDER");
            rows[0].leading = true;
            rows[0].hasBid = true;
            rows[1].seller = text::get("OUTBID");
            rows[1].outbid = true;
        }
        for (u32 i = 0; i < 5; ++i) {
            rows[i].cell.ref = {ItemRef::Kind::Window, 0, 0, i};
        }
        v.rows = {rows, page == Page::AuctionSell ? 2u : page == Page::AuctionBids ? 2u : 5u};
        v.selected = m_auctionSelected;
        v.page = 0;
        v.pages = 3;
        v.total = 137;
        v.bidAmount = 1525;
        v.canBid = v.canBuyout = page != Page::AuctionSell;
        v.canCancel = page == Page::AuctionSell;
        v.money = 1234567;
        v.sell.cell = cellOf(kBackpack[8], {ItemRef::Kind::Window, 0, 0, 1000});
        v.sell.itemName = kBackpack[8].name;
        v.sell.startBid = 1200;
        v.sell.buyout = 2000;
        v.sell.duration = m_duration;
        v.sell.deposit = 12 * (m_duration == 0 ? 1 : m_duration == 1 ? 4 : 12);
        bags(v.bags);
        m_cells[8].dimmed = true;
        const AuctionResult r = auctionWindow(c, v, m_drag);
        if (r.row >= 0) {
            m_auctionSelected = r.row;
        }
        if (r.duration >= 0) {
            m_duration = static_cast<u32>(r.duration);
        }
        if (r.button >= 0 || r.tab >= 0) {
            log("auction button/tab", r.button >= 0 ? r.button : 100 + r.tab);
        }
        break;
    }
    }
    m_drag.finish();
}

} // namespace vd
