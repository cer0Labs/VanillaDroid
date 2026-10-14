#pragma once

#include "app/ChatConfigScreens.h"
#include "app/ChatFrame.h"
#include "app/Dialogs.h"
#include "app/ExchangeWindows.h"
#include "app/PopupStack.h"
#include "app/PopupWindows.h"
#include "app/SocialWindows.h"
#include "app/TutorialScreens.h"
#include "core/Types.h"
#include "game/ChatWindows.h"
#include "game/ListSort.h"

#include <array>
#include <string>
#include <vector>

namespace vd {

// Development (phase 18, p3): the last details of the 1.12 interface with sample data, as VD_UI_GALLERY pages:
//  - "tutorial": four alert buttons pulsing over the bottom centre and TutorialFrame on "Backpack" (taps show theirs);
//    "tutorial42": the welcome under the screen's centre, as after the intro (TutorialFrame_CheckIntro).
//  - "chatwindows": the chat with four docked windows of a sample chat-cache.txt (one renamed, 16 pt, a darker
//    background); "chatconfig": a tab's menu (FCFOptionsDropDown); "chatfilters": its Combat Messages.
//  - "abandonitems", "deletemail", "deletemoney", "namechat": ABANDON_QUEST_WITH_ITEMS, DELETE_MAIL, DELETE_MONEY,
//    NAME_CHAT.
//  - "whosort": the who list's sortable columns (a header tapped sorts by it, again turns it round; the second one's
//    dropdown picks zone, guild or race); "auctionsort", "auctionsort2", "auctionsort3": the auction house's browse,
//    bids and auctions lists under their sort headers (the arrows show each key's direction).
// Taps go to the log.
class P3Gallery {
public:
    enum class Page : u8 { Tutorial, Tutorial42, ChatWindows, ChatConfig, ChatFilters, AbandonItems,
                           DeleteMail, DeleteMoney, NameChat, WhoSort, AuctionSort, AuctionSortBids, AuctionSortOwned };
    bool select(const char* name);
    void draw(DialogContext& c, u64 nowMs);

private:
    void drawChat(DialogContext& c, u64 nowMs);
    void drawPopup(DialogContext& c);
    void drawWho(DialogContext& c);
    void drawAuctions(DialogContext& c, u32 tab);

    Page m_page = Page::Tutorial;
    bool m_ready = false;
    TutorialScreens m_tutorials;
    game::ChatWindows m_windows;
    ChatConfigScreens m_config;
    bool m_menuOpen = true;
    std::vector<std::string> m_texts;
    std::vector<ChatFrameLine> m_lines;
    std::array<ChatFrameTab, game::kChatWindows> m_tabs{};
    std::array<std::array<char, 48>, game::kChatWindows> m_tabNames{};
    ChatFrameState m_chatState;
    u32 m_selected = 0;
    std::array<f32, PopupStack::kSlots> m_heights{};
    std::array<PopupView, PopupStack::kSlots> m_views{};
    std::array<char, 256> m_popupText{};
    // The sortable lists.
    game::WhoSortKeys m_whoKeys = game::whoSortDefaults();
    std::vector<u16> m_whoOrder;
    std::array<std::string, 16> m_whoLevels;
    std::vector<WhoRowView> m_whoRows;
    std::array<SortHeaderView, 4> m_whoHeaders{};
    std::array<ChoiceView, 3> m_whoChoices{};
    u32 m_whoColumn = 0;
    bool m_whoMenu = false;
    i32 m_whoSelected = -1;
    std::array<game::AuctionSortKeys, 3> m_auctionKeys{game::auctionSortDefaults(game::AuctionList::Browse),
                                                       game::auctionSortDefaults(game::AuctionList::Bids),
                                                       game::auctionSortDefaults(game::AuctionList::Owned)};
    std::vector<u16> m_auctionOrder;
    std::vector<AuctionRowView> m_auctionRows;
    std::array<SortHeaderView, 6> m_auctionHeaders{};
    std::array<AuctionColumn, 6> m_auctionColumns{};
    ItemDrag m_drag;
    i32 m_auctionSelected = -1;
};

} // namespace vd
