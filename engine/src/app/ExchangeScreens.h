#pragma once

#include "app/Dialogs.h"
#include "app/ExchangeWindows.h"
#include "app/InventoryWindows.h"
#include "app/SocialWindows.h"
#include "app/UnitMenuWindows.h"
#include "core/Text.h"
#include "core/Types.h"
#include "game/Auction.h"
#include "game/GameEvents.h"
#include "game/ListSort.h" // phase 18 (p3)
#include "ui/TextInput.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd {

class GameSession;
class InventoryScreens;
class PopupScreens;
class SpellTargetingScreens;
class UiResources;
namespace game {
class GameData;
class ExchangeData;
struct MailMessage;
} // namespace game

// The game's side of the exchange windows (phase 8): fills the views of app/ExchangeWindows from the
// session's game::Exchange (group loot rolls, mail, the auction house, trade) every frame and applies what
// the player does there; turns their events into chat lines and messages. GameScreens owns one: the roll
// frames and the master looter's picker over the windows, the minimap's letter with the HUD, the mailbox /
// auction house / trade in the modal chain. Item cells share InventoryScreens' drag, bags pane and tooltip
// (InventoryScreens::hostWindow). Main thread; storage reserved once (nothing allocates per frame once warmed
// up).
class ExchangeScreens {
public:
    void init(const game::GameData* data, const game::ExchangeData* exchange, UiResources* resources);
    // A new character in the world (or none): windows, drafts and cursors start over.
    void reset();
    // Phase 15 (options): SHOW_LOOT_SPAM ("showLootSpam") off: no line per choice and roll, only the winner's with
    // its roll (WoW.exe 0x61c0b0 / 0x61ba3a).
    void setShowLootSpam(bool on) { m_lootSpam = on; }

    // Once a frame before drawing: the exchange's events into lines; windows left behind close.
    void update(GameSession& session, InventoryScreens& items, u32 nowMs);
    // A line for the chat (`chat`) or the middle of the screen; GameScreens shows them with its own.
    struct Line {
        std::array<char, 256> text{};
        u32 color = 0;
        bool chat = false;
        bool big = false;
    };
    bool popLine(Line& out);

    // Not modal, with the HUD: the minimap's letter while unread mail waits.
    void drawHud(GameSession& session, DialogContext& c);
    // A window of its own this frame: a trade (or its request), the auction house or the mailbox.
    bool windowOpen(const GameSession& session) const;
    // Parity pass (panels): which window that is (TradeFrame, AuctionFrame, MailFrame are panels; a trade's request
    // and the wait for an answer are StaticPopups: app/PopupScreens), and closing it when the panels' rules replace
    // it.
    enum class Window : u8 { None, Trade, Auction, Mail };
    Window window(const GameSession& session) const;
    void closeWindow(GameSession& session, Window window);
    // `which`: that window only (parity pass: each in its panel place); None: the
    // first one open.
    void drawWindow(GameSession& session, DialogContext& c, ui::TextInput& text, InventoryScreens& items,
                    u32 nowMs, Window which = Window::None);
    // Over every window: the group loot roll frames and the master looter's picker.
    void drawOverlay(GameSession& session, DialogContext& c, InventoryScreens& items, u32 nowMs);

    // The loot window asks what a tap on its item `slot` does: true when handled here (an item being rolled
    // for, the master looter's picker, a locked one). Phase 15 (items): `row` is the item's button (the master
    // looter's GroupLootDropDown hangs from it).
    bool lootTapped(GameSession& session, u8 slot, const ui::Rect& row = {});
    // Phase 15 (items): where CONFIRM_LOOT_ROLL and CONFIRM_LOOT_DISTRIBUTION are asked (null: never asked).
    void setPopups(PopupScreens* popups) { m_popups = popups; }
    // What a loot row shows under its name for its LootSlotType (empty: nothing).
    std::string_view lootNote(GameSession& session, u8 slotType) const;
    // "Trade" on a player's sheet: CMSG_INITIATE_TRADE, or why not (a message).
    void startTrade(GameSession& session, u64 player);
    // Phase 13 (spells): a spell waiting for an item takes a tap on the bags pane, and the trader's "will not be
    // traded" slot (TradeFrame's ClickTargetTradeButton) lights up for it (null: never).
    void setTargeting(SpellTargetingScreens* targeting) { m_targeting = targeting; }

private:
    SpellTargetingScreens* m_targeting = nullptr; // phase 13 (spells)
    // The fields the keyboard can edit.
    enum class Field : u8 {
        None,
        MailTo,
        MailSubject,
        MailBody,
        MailGold,
        MailSilver,
        MailCopper,
        TradeGold,
        TradeSilver,
        TradeCopper,
        AuctionName,
        AuctionMin,
        AuctionMax,
        BidGold,
        BidSilver,
        BidCopper,
        StartGold,
        StartSilver,
        StartCopper,
        BuyoutGold,
        BuyoutSilver,
        BuyoutCopper,
    };
    enum class Picker : u8 { None, Category, Quality };

    // A line (the oldest goes when they are full): a text of the table with its arguments, or one as it is.
    template <typename... Args> void line(bool chat, u32 color, text::Key key, const Args&... args) {
        Line& l = addLine(chat, color);
        text::format(l.text, key, args...);
        lineAdded(l);
    }
    void lineText(bool chat, u32 color, std::string_view s);
    Line& addLine(bool chat, u32 color);
    void lineAdded(Line& l);
    void readEvents(GameSession& session, InventoryScreens& items);
    std::string_view itemName(GameSession& session, u32 itemId);
    std::string_view playerName(GameSession& session, u64 guid);
    // A text of the table formatted into this frame's arena (valid until the next drawWindow()).
    template <typename... Args> std::string_view arena(text::Key key, const Args&... args) {
        if (m_arenaUsed + 1 >= m_arena.size()) {
            return {};
        }
        const std::string_view s = text::format(std::span<char>(m_arena).subspan(m_arenaUsed), key, args...);
        m_arenaUsed += static_cast<u32>(s.size()) + 1;
        return s;
    }
    std::string_view categoryLabel(u32 index) const;
    ItemSlotView itemView(GameSession& session, u32 entry, u32 count) const;
    ItemSlotView bagItemView(GameSession& session, u64 guid) const;
    // Keyboard.
    static ui::WidgetId fieldId(Field f);
    void sync(ui::TextInput& text);
    void beginEdit(ui::TextInput& text, Field f);
    void endEdit(ui::TextInput& text);
    std::string_view fieldText(Field f, std::array<char, 12>& digits) const;
    bool drawEditor(DialogContext& c, ui::TextInput& text);
    // Windows.
    void tradeWindow(GameSession& session, DialogContext& c, ui::TextInput& text, InventoryScreens& items,
                     ItemDrag& drag);
    void mailWindow(GameSession& session, DialogContext& c, ui::TextInput& text, InventoryScreens& items,
                    ItemDrag& drag, u32 nowMs);
    void auctionWindow(GameSession& session, DialogContext& c, ui::TextInput& text, InventoryScreens& items,
                       ItemDrag& drag, u32 nowMs);
    std::string_view mailSender(GameSession& session, const game::MailMessage& m);
    std::string_view mailSubject(GameSession& session, const game::MailMessage& m);
    std::string_view mailBody(GameSession& session, const game::MailMessage& m, MailInvoiceView& invoice);
    void applyItems(GameSession& session, InventoryScreens& items, const ItemWindowResult& r,
                    std::span<const ItemRef> windowCells, std::span<const u32> windowEntries);
    void search(GameSession& session, u32 page);

    const game::GameData* m_data = nullptr;
    const game::ExchangeData* m_exchange = nullptr;
    UiResources* m_resources = nullptr;
    bool m_trace = false;
    u32 m_eventCursor = 0;
    u64 m_cursorsPlayer = 0;
    std::array<Line, 12> m_lines{};
    u32 m_lineFirst = 0, m_lineCount = 0;

    // Keyboard.
    Field m_field = Field::None;

    // Group loot.
    std::vector<RollView> m_rollViews;
    bool m_masterOpen = false;
    bool m_lootSpam = true; // phase 15 (options)
    u8 m_masterSlot = 0;
    // Phase 15 (items): GroupLootDropDown (LootFrame.lua): the candidates, by raid group in a raid.
    PopupScreens* m_popups = nullptr;
    ui::Rect m_masterRow; // the loot button it hangs from
    i32 m_masterGroup = -1; // the raid group whose candidates show (level 2), -1 none
    std::vector<DropDownItemView> m_masterItems, m_masterItems2;
    std::vector<u64> m_masterGuids, m_masterGuids2; // per row (0: a title or a group)
    std::array<std::array<char, 24>, 8> m_groupLabels{};
    std::vector<ChoiceView> m_choices;

    // Mail.
    u32 m_mailTab = 0;
    u32 m_selectedMail = 0; // message id, 0 none
    std::string m_to, m_subject, m_body;
    u32 m_mailMoney = 0;
    bool m_cod = false;
    std::vector<MailRowView> m_mailRows;
    std::vector<u32> m_mailIds;

    // Trade.
    u32 m_tradeMoney = 0; // typed, sent when the keyboard closes
    bool m_tradeMoneyTyped = false;

    // Auction house.
    u32 m_auctionTab = 0;
    u32 m_selectedAuction = 0; // auction id, 0 none
    u32 m_bidAmount = 0;
    game::AuctionSearch m_search;
    u32 m_category = 0; // index into game::auctionCategories() + 1 (0: all)
    u32 m_quality = 0;  // 0 all, 1 + ItemQuality
    u32 m_startBid = 0, m_buyout = 0, m_duration = 1;
    u64 m_stagedShown = 0; // the staged item the prices were suggested for
    Picker m_picker = Picker::None;
    std::vector<AuctionRowView> m_auctionRows;
    std::vector<u32> m_auctionIds;
    // Phase 18 (p3): SortAuctionItems' keys of the three lists for the whole session (as WoW.exe keeps them), each
    // page's order (sorted again with a new page, a header tapped, or templates and names that arrived), and the
    // shown tab's headers (AuctionSortButtonTemplate) with what their columns show.
    static constexpr u32 kAuctionLists = static_cast<u32>(game::AuctionList::Count);
    std::array<game::AuctionSortKeys, kAuctionLists> m_auctionSort{
        game::auctionSortDefaults(game::AuctionList::Browse), game::auctionSortDefaults(game::AuctionList::Bids),
        game::auctionSortDefaults(game::AuctionList::Owned)};
    std::array<std::vector<u16>, kAuctionLists> m_auctionOrder;
    std::array<u32, kAuctionLists> m_auctionSortedPage{~0u, ~0u, ~0u};
    std::array<u32, kAuctionLists> m_auctionSortedKeys{};
    std::array<u32, kAuctionLists> m_auctionSortedUnknown{};
    std::array<SortHeaderView, 6> m_auctionHeaders{};
    std::array<AuctionColumn, 6> m_auctionColumns{};
    std::string m_auctionMyName;
    std::vector<std::string> m_categoryLabels; // from the DBCs; [0] stands for ALL (categoryLabel())

    std::array<char, 8192> m_arena{};
    u32 m_arenaUsed = 0;
    std::vector<ItemSlotView> m_bagCells; // the bags pane with what the window holds dimmed
};

} // namespace vd
