#pragma once

#include "app/Dialogs.h"
#include "app/InventoryWindows.h"
#include "app/SortHeaders.h" // phase 18 (p3)
#include "core/Types.h"
#include "game/ListSort.h" // phase 18 (p3)
#include "ui/Context.h"
#include "ui/DrawList.h"
#include "ui/Widgets.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

// The touch windows of the item economy between players (phase 8): the group loot roll frames, the minimap's
// letter, the mailbox (inbox, a message, writing one), the trade window and the auction house (browse, bids,
// the player's auctions and selling). Pure views like app/Dialogs and app/InventoryWindows: a view struct in,
// what the player did this frame out; item cells are app/InventoryWindows' (taps, the shared drag, drops on
// cells and zones). Interface words in Spanish; item names and the server's texts as they come.

// --- Money typed in
// ------------------------------------------------------------------------------------------

// Which part of a money amount the keyboard edits: gold, silver or copper of the window's amounts (each
// window says which amounts it has: 0..2 the first, 3..5 the second).
constexpr i32 kMoneyGold = 0, kMoneySilver = 1, kMoneyCopper = 2;

// Three boxes (oro, plata, cobre) with their coins, the one being edited lit. Returns the part tapped (0..2)
// or -1.
i32 moneyInput(DialogContext& c, ui::Gui& g, ui::WidgetId id, const ui::Rect& r, u32 copper, i32 editing);
// The amount with one part replaced by typed digits (silver and copper at most 99).
u32 moneyWithPart(u32 copper, i32 part, std::string_view digits);
// The digits of one part, for the keyboard ("12"; empty for 0).
std::string_view moneyPartText(u32 copper, i32 part, std::array<char, 12>& buffer);

// A text being typed while the soft keyboard leaves little room: only that field, its label and "Listo" (the
// login screen's way). Returns true when "Listo" (or outside) was tapped.
bool compactEditor(DialogContext& c, std::string_view title, std::string_view label, std::string_view text,
                   bool multiline);
// Whether the area left by the keyboard is too short for a window: then compactEditor() instead.
bool keyboardCrowds(const DialogContext& c);

// --- Group loot rolls
// ------------------------------------------------------------------------------------------

struct RollView {
    ItemView item;
    u32 remainingMs = 0; // of the countdown
    u32 countdownMs = 1;
};
struct RollResult {
    i32 roll = -1;    // index of the roll answered...
    u8 vote = 0;      // ...with net::kRoll* (0 pass, 1 need, 2 greed)
    i32 tooltip = -1; // index whose item was tapped
    ui::Rect tooltipRect;
};
// GroupLootFrame: a frame per roll stacked down from `top`, centered (not modal: drawn over the windows). The
// item (tap: its tooltip), its name in its quality's color, the countdown, and Necesidad / Codicia / Pasar.
RollResult rollFrames(DialogContext& c, f32 top, std::span<const RollView> rolls);
// Where the frames start: under the target and the messages, centered.
f32 rollFramesTop(const DialogContext& c);

// --- The minimap's letter
// --------------------------------------------------------------------------------------

// MiniMapMailFrame: the letter on the minimap's rim while unread mail waits. Returns true when tapped.
bool mailIndicator(DialogContext& c);
ui::Rect mailIndicatorRect(const DialogContext& c);

// --- Mail
// ------------------------------------------------------------------------------------------------------

struct MailRowView {
    std::string_view sender;
    std::string_view subject;
    std::string_view expires; // "29 días", "3 horas"
    u64 iconHash = 0;         // the item's icon, or a letter / coins
    u32 quality = 1;
    u32 count = 0;
    u32 money = 0;
    u32 cod = 0;
    bool unread = false;
    bool returned = false;
};
// Retouches (windows): an auction house invoice (OpenMailInvoiceFrame: GetInboxInvoiceInfo), drawn instead of
// the body with its money frames.
struct MailInvoiceView {
    bool shown = false;
    bool buyer =
        false; // the buyer's: "Item Purchased:", "Sold By:", "Amount Paid:" (else the seller's lines)
    std::string_view item;   // the item's name
    std::string_view player; // the other side's name (empty: not known yet)
    bool buyout = false;     // bid == buyout: "(Buyout)", else "(High Bidder)"
    u32 bid = 0, deposit = 0, cut = 0;
};
struct MailReadView {
    std::string_view sender;
    std::string_view subject;
    std::string_view body; // empty: none (or still on its way: waitingBody)
    bool waitingBody = false;
    ItemSlotView item; // the attachment (empty: none); ref Window #0
    std::string_view itemName;
    u32 money = 0;
    u32 cod = 0;
    bool canTakeMoney = false, canTakeItem = false, canReply = false, canReturn = false, canDelete = false,
         canCopy = false;
    MailInvoiceView invoice; // retouches (windows)
};
struct MailSendView {
    std::string_view to, subject, body;
    i32 editing = -1;        // 0 to, 1 subject, 2 body, 3..5 the money's gold / silver / copper
    ItemSlotView attachment; // ref Window #1
    std::string_view attachmentName;
    bool cod = false; // the money is cash on delivery
    u32 money = 0;    // sent, or asked on delivery
    u32 cost = 0;     // the postage
    u32 playerMoney = 0;
    std::string_view problem; // why it cannot go (shown, "Enviar" disabled); empty: it can
    bool sending = false;
    BagsView bags;
};
struct MailView {
    u32 tab = 0; // 0 the inbox, 1 writing
    bool loaded = false;
    std::span<const MailRowView> rows;
    i32 selected = -1;
    MailReadView read;
    MailSendView send;
};
enum MailButton : i32 {
    kMailTakeMoney,
    kMailTakeItem,
    kMailReply,
    kMailReturn,
    kMailDelete,
    kMailCopy,
    kMailSend,
    kMailToggleCod,
    kMailBack, // square screens: from the message back to the list
};
struct MailResult {
    i32 tab = -1;
    i32 row = -1;    // a message tapped
    i32 button = -1; // MailButton
    i32 field = -1;  // a field tapped (MailSendView::editing's numbers)
    bool closed = false;
    // Item cells: taps (tooltips) and drops; zones: 0 the bags pane, 1 the attachment.
    ItemWindowResult items;
};
MailResult mailWindow(DialogContext& c, const MailView& v, ItemDrag& drag);

// --- Trade
// -----------------------------------------------------------------------------------------------------

struct TradeSideView {
    std::string_view name;
    std::array<ItemSlotView, 7> slots{}; // six traded, then the one that is only enchanted (refs Window #0..6
                                         // for the player's side)
    std::array<std::string_view, 7> names{};
    u32 money = 0;
    bool accepted = false;
};
struct TradeView {
    TradeSideView mine;
    TradeSideView theirs;
    i32 editing = -1; // the money's part being typed (0..2)
    u32 playerMoney = 0;
    BagsView bags;
};
enum TradeButton : i32 { kTradeAccept, kTradeUnaccept, kTradeCancel };
struct TradeResult {
    i32 button = -1; // TradeButton
    i32 field = -1;  // the money's part tapped
    bool closed = false;
    // Item cells: taps and drops; zones: 0 the bags pane, 1..7 the player's trade slots 0..6 (not on a cell).
    ItemWindowResult items;
};
TradeResult tradeWindow(DialogContext& c, const TradeView& v, ItemDrag& drag);

// --- Auction house
// ---------------------------------------------------------------------------------------------

struct AuctionRowView {
    ItemSlotView cell; // ref Window #row
    std::string_view name;
    u32 level = 0;             // required level (0: none shown)
    bool levelTooHigh = false; // phase 18 (extras): above the player's (RED_FONT_COLOR_CODE, AuctionFrameBrowse_Update)
    bool startingPrice = false; // phase 18 (extras): the player's auction without a bid: `bid` at half alpha
    std::string_view timeLeft; // "Corto", "Medio", "Largo", "Muy largo"
    std::string_view seller;   // the browse list; "Tú" for the player's; the bids list: the state
    u32 bid = 0;               // the current bid, or the starting bid while none
    bool hasBid = false;       // someone bid (else `bid` is the starting bid)
    u32 buyout = 0;            // 0: none
    bool leading = false;      // the player has the highest bid
    bool outbid = false;       // the player bid and someone bid more
};
// Phase 18 (p3): what a column of the lists shows under its sort header (BrowseButtonTemplate, BidButtonTemplate and
// AuctionsButtonTemplate's parts): the item (its icon and name), its level, the time left, the seller / the bid's
// status / the high bidder (AuctionRowView::seller), the bid with the buyout under it, the buyout, the bid.
enum class AuctionColumn : u8 { Item, Level, TimeLeft, Info, BidAndBuyout, Buyout, Bid };
// The sort headers of a tab (0 browse, 1 bids, 2 the player's auctions) as Blizzard_AuctionUI.xml lays them out:
// BrowseQualitySort.. / BidQualitySort.. / AuctionsQualitySort.., their labels, SortAuctionItems' keys, widths.
struct AuctionHeaderSpec {
    const char* key; // the label's text key
    game::AuctionSort sort;
    f32 width;
    AuctionColumn column;
};
std::span<const AuctionHeaderSpec> auctionHeaderSpecs(u32 tab);
struct AuctionFilterView {
    std::string_view name;
    std::string_view category; // "Todas", "Armas: Espadas de una mano"...
    std::string_view quality;  // "Todas", "Poco común"...
    u32 levelMin = 0, levelMax = 0;
    bool usable = false;
};
struct AuctionSellView {
    ItemSlotView cell; // ref Window #1000
    std::string_view itemName;
    u32 startBid = 0;
    u32 buyout = 0;
    u32 duration = 1; // index into net::kAuctionDurations (2, 8, 24 hours)
    u32 deposit = 0;
    std::string_view problem; // why "Crear subasta" is disabled; empty: it can
};
struct AuctionView {
    std::string_view title;
    u32 tab = 0; // 0 browse, 1 bids, 2 the player's auctions
    AuctionFilterView filter;
    std::span<const AuctionRowView> rows;
    i32 selected = -1;
    u32 page = 0, pages = 0, total = 0;
    bool pending = false;
    std::string_view empty; // shown without rows
    u32 bidAmount = 0;      // what "Pujar" offers (the minimum by default)
    bool canBid = false, canBuyout = false, canCancel = false;
    i32 editing =
        -1; // 0 the name, 1 / 2 the levels, 3..5 the bid's parts, 6..8 the start bid's, 9..11 the buyout's
    AuctionSellView sell;
    BagsView bags;
    u32 money = 0;
    bool busy = false; // a command waiting for its answer
    // Phase 18 (p3): the list's sort headers (AuctionSortButtonTemplate) and what each column shows; with them the rows
    // are a table under them.
    std::span<const SortHeaderView> headers;
    std::span<const AuctionColumn> columns;
};
enum AuctionButton : i32 {
    kAuctionSearch,
    kAuctionPrevious,
    kAuctionNext,
    kAuctionBid,
    kAuctionBuyout,
    kAuctionCancel,
    kAuctionCreate,
    kAuctionCategory,  // the category picker
    kAuctionQuality,   // the quality picker
    kAuctionUsable,    // toggled
    kAuctionClearSale, // the sale slot emptied
};
struct AuctionResult {
    i32 tab = -1;
    i32 row = -1;    // a row selected
    i32 button = -1; // AuctionButton
    i32 field = -1;  // AuctionView::editing's numbers
    i32 duration = -1;
    i32 header = -1; // phase 18 (p3): a sort header tapped
    bool closed = false;
    // Item cells: taps and drops; zones: 0 the bags pane, 1 the sale slot.
    ItemWindowResult items;
};
AuctionResult auctionWindow(DialogContext& c, const AuctionView& v, ItemDrag& drag);

} // namespace vd
