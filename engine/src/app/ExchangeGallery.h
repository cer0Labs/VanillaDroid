#pragma once

#include "app/Dialogs.h"
#include "app/ExchangeWindows.h"
#include "app/InventoryWindows.h"
#include "core/Types.h"

#include <array>
#include <vector>

namespace vd {

// Development (phase 8): the exchange windows with sample data, for UiGallery's pages rolls (group loot),
// mail, mailsend, trade, auction, auctionbids, auctionsell and mailedit (the compact editor the soft keyboard
// leaves room for). Tabs, rows and buttons work; what the player does goes to the log ("UI gallery: ...").
class ExchangeGallery {
public:
    enum class Page : u8 { Rolls, Mail, MailSend, Trade, Auction, AuctionBids, AuctionSell, MailEdit };

    void draw(DialogContext& c, Page page, u64 nowMs);

private:
    void bags(BagsView& out);
    void log(const char* what, i32 value);

    ItemDrag m_drag;
    std::vector<ItemSlotView> m_bagBar;
    std::vector<ItemSlotView> m_cells;
    std::vector<BagSectionView> m_sections;
    u64 m_startMs = 0;
    i32 m_mailSelected = 0;
    i32 m_auctionSelected = 1;
    bool m_cod = false;
    u32 m_duration = 1;
    bool m_accepted = false;
    std::array<std::array<char, 64>, 6> m_text{}; // the sample texts formatted from the table
};

} // namespace vd
