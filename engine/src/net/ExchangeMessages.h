#pragma once

#include "core/Types.h"
#include "net/ByteBuffer.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

// Group loot rolls and master loot, mail, the auction house and player trade (phase 8), as vmangos-core
// 4641790 reads and writes them for build 5875 (Group/Group.cpp, Handlers/GroupHandler.cpp, LootHandler.cpp,
// MailHandler.cpp, AuctionHouseHandler.cpp, TradeHandler.cpp; Server/Packets/Loot.*, Group.*, Mail.*,
// AuctionHouse.*, Trade.*; AuctionHouse/AuctionHouseMgr.cpp, Objects/TradeData.cpp). Same rules as
// net/GameMessages.h: parsers fill a caller-owned struct and return false on a malformed packet without
// reading past its end; strings are views into the payload; builders append a client packet's body to a byte
// vector.
//
// Bodies handled with the GameMessages helpers: CMSG_GET_MAIL_LIST, MSG_AUCTION_HELLO (client) and
// CMSG_INITIATE_TRADE (writeGuid), CMSG_SET_TRADE_GOLD (writeU32), CMSG_CLEAR_TRADE_ITEM (writeU8);
// MSG_QUERY_NEXT_MAIL_TIME (client), CMSG_BEGIN_TRADE, CMSG_UNACCEPT_TRADE and CMSG_CANCEL_TRADE are empty.
// SMSG_RECEIVED_MAIL carries a u32 (always 0, parseU32).
namespace vd::net {

// -------------------------------------------------------------------------------------------------------
// Group loot: rolls and the master looter.

// RollVote (Group/Group.h): what CMSG_LOOT_ROLL sends, and the type of SMSG_LOOT_ROLL_WON.
constexpr u8 kRollPass = 0;
constexpr u8 kRollNeed = 1;
constexpr u8 kRollGreed = 2;
constexpr u32 kLootRollTimeoutMs = 60000; // LOOT_ROLL_TIMEOUT (Group.cpp): the countdown the server sends
constexpr u32 kMaxRaidMembers = 40;

struct LootStartRoll { // SMSG_LOOT_START_ROLL (Group::SendLootStartRoll)
    u64 object = 0;    // the creature (or object) looted
    u32 slot = 0;      // its loot slot
    u32 itemId = 0;
    u32 randomSuffix = 0; // always 0 in 1.12
    u32 randomPropertyId = 0;
    u32 countdownMs = 0;
};
bool parseLootStartRoll(std::span<const u8> payload, LootStartRoll& out);

// What an SMSG_LOOT_ROLL line tells (Group::CountRollVote while voting, Group::CountTheRoll once everybody
// voted).
enum class LootRollKind : u8 {
    Passed,     // number 128, type 128
    ChoseNeed,  // number 0, type 0
    ChoseGreed, // number 128, type 2
    NeedRoll,   // number 1..100, type 1: the dice of a need roll
    GreedRoll,  // number 1..100, type 2
    Unknown,
};
struct LootRoll { // SMSG_LOOT_ROLL (Group::SendLootRoll)
    u64 object = 0;
    u32 slot = 0;
    u64 roller = 0;
    u32 itemId = 0;
    u32 randomSuffix = 0;
    u32 randomPropertyId = 0;
    u8 number = 0;
    u8 type = 0;
    LootRollKind kind() const;
};
bool parseLootRoll(std::span<const u8> payload, LootRoll& out);

struct LootRollWon { // SMSG_LOOT_ROLL_WON (Group::SendLootRollWon): type kRollNeed / kRollGreed
    u64 object = 0;
    u32 slot = 0;
    u32 itemId = 0;
    u32 randomSuffix = 0;
    u32 randomPropertyId = 0;
    u64 winner = 0;
    u8 number = 0; // the winning roll (100 when the winner was the only one who could roll)
    u8 type = 0;
};
bool parseLootRollWon(std::span<const u8> payload, LootRollWon& out);

struct LootAllPassed { // SMSG_LOOT_ALL_PASSED (Group::SendLootAllPassed): the property before the suffix here
    u64 object = 0;
    u32 slot = 0;
    u32 itemId = 0;
    u32 randomPropertyId = 0;
    u32 randomSuffix = 0;
};
bool parseLootAllPassed(std::span<const u8> payload, LootAllPassed& out);

struct LootMasterList { // SMSG_LOOT_MASTER_LIST (WorldPackets::Group::LootMasterList): who may receive the
                        // loot
    std::array<u64, kMaxRaidMembers> players{};
    u32 count = 0;
    std::span<const u64> list() const { return {players.data(), count}; }
};
bool parseLootMasterList(std::span<const u8> payload, LootMasterList& out);

// CMSG_LOOT_ROLL: the looted object, the loot slot (u32) and the vote (kRoll*).
void writeLootRoll(std::vector<u8>& out, u64 object, u32 slot, u8 vote);
// CMSG_LOOT_MASTER_GIVE: the loot window's object, the loot slot (u8) and the player who gets it.
void writeLootMasterGive(std::vector<u8>& out, u64 object, u8 slot, u64 player);

// -------------------------------------------------------------------------------------------------------
// Mail.

// MailMessageType (Mail/Mail.h).
constexpr u8 kMailNormal = 0;     // from a player: the sender's GUID follows
constexpr u8 kMailAuction = 2;    // from an auction house: its id follows
constexpr u8 kMailCreature = 3;   // from an NPC: its entry follows
constexpr u8 kMailGameObject = 4; // from an object: its entry follows
constexpr u8 kMailItem = 5;       // nothing follows
// MailCheckMask: a message's flags.
constexpr u32 kMailFlagRead = 0x01;
constexpr u32 kMailFlagReturned = 0x02; // it came back: cannot be returned again
constexpr u32 kMailFlagCopied = 0x04;   // its text was copied (or the server says there is nothing to copy)
constexpr u32 kMailFlagCodPayment = 0x08;
constexpr u32 kMailFlagHasBody = 0x10;
// MailResponseType (SharedDefines.h): the action an SMSG_SEND_MAIL_RESULT answers.
constexpr u32 kMailActionSend = 0;
constexpr u32 kMailActionMoneyTaken = 1;
constexpr u32 kMailActionItemTaken = 2;
constexpr u32 kMailActionReturned = 3;
constexpr u32 kMailActionDeleted = 4;
constexpr u32 kMailActionMadePermanent = 5;
// MailResponseResult.
constexpr u32 kMailOk = 0;
constexpr u32 kMailErrEquipError = 1; // the InventoryResult follows
constexpr u32 kMailErrCannotSendToSelf = 2;
constexpr u32 kMailErrNotEnoughMoney = 3;
constexpr u32 kMailErrRecipientNotFound = 4;
constexpr u32 kMailErrNotYourTeam = 5;
constexpr u32 kMailErrInternalError = 6;
constexpr u32 kMailErrDisabledForTrial = 14;
constexpr u32 kMailErrRecipientCapReached = 15;
// Limits of HandleSendMail / HandleSendMailCallback.
constexpr u32 kMailPostage = 30;       // copper the server takes for every message sent
constexpr u32 kMailMaxSubject = 64;    // bytes; longer messages are dropped without an answer
constexpr u32 kMailMaxBody = 500;      // bytes
constexpr u32 kMailMaxCod = 100000000; // copper (10000 gold)
constexpr u32 kMailStationeryDefault =
    41;                               // MAIL_STATIONERY_DEFAULT (the server ignores what the client sends)
constexpr u32 kMailBodyItem = 8383;   // MAIL_BODY_ITEM_TEMPLATE: the letter CMSG_MAIL_CREATE_TEXT_ITEM makes
constexpr u32 kMaxMailMessages = 254; // HandleGetMailList stops there (the count is a byte)

struct MailItemInfo {    // the attached item (1.12 carries at most one)
    u32 entry = 0;       // 0: none
    u32 enchantment = 0; // permanent enchantment
    u32 randomPropertyId = 0;
    u32 suffixFactor = 0;
    u8 count = 0;
    u32 charges = 0;
    u32 maxDurability = 0;
    u32 durability = 0;
};
struct MailEntry { // one message of SMSG_MAIL_LIST_RESULT
    u32 id = 0;
    u8 type = kMailNormal;
    u64 sender = 0;      // kMailNormal: the sender's player GUID
    u32 senderEntry = 0; // kMailCreature / kMailGameObject: the entry; kMailAuction: the auction house's id
    std::string_view subject;
    u32 itemTextId = 0; // the body (CMSG_ITEM_TEXT_QUERY), 0 none
    u32 package = 0;
    u32 stationery = 0;
    MailItemInfo item;
    u32 money = 0;
    u32 cod = 0;   // cash on delivery: what taking the item costs
    u32 flags = 0; // kMailFlag*
    f32 daysLeft = 0.0f;
    u32 templateId = 0; // MailTemplate.dbc (builds > 1.9.4)
};
struct MailList {                    // SMSG_MAIL_LIST_RESULT (WorldSession::HandleGetMailList)
    std::vector<MailEntry> messages; // reserve kMaxMailMessages: parsing then never allocates
};
bool parseMailList(std::span<const u8> payload, MailList& out);

struct SendMailResult { // SMSG_SEND_MAIL_RESULT (WorldPackets::Mail::SendMailResult)
    u32 mailId = 0;
    u32 action = 0;     // kMailAction*
    u32 error = 0;      // kMailOk / kMailErr*
    u32 equipError = 0; // kMailErrEquipError: the InventoryResult
    u32 itemId = 0;     // kMailActionItemTaken + kMailOk: the item's entry
    u32 count = 0;      // ... and how many
};
bool parseSendMailResult(std::span<const u8> payload, SendMailResult& out);

// MSG_QUERY_NEXT_MAIL_TIME (server): 0 when unread mail waits, -86400 (minus a day) otherwise.
bool parseNextMailTime(std::span<const u8> payload, f32& seconds);

// The mailbox and a message: CMSG_MAIL_MARK_AS_READ, CMSG_MAIL_TAKE_MONEY, CMSG_MAIL_TAKE_ITEM,
// CMSG_MAIL_DELETE, CMSG_MAIL_RETURN_TO_SENDER.
void writeMailAction(std::vector<u8>& out, u64 mailbox, u32 mailId);
// CMSG_MAIL_CREATE_TEXT_ITEM: the message and its template (builds > 1.9.4).
void writeMailCreateTextItem(std::vector<u8>& out, u64 mailbox, u32 mailId, u32 templateId);
struct MailDraft {
    std::string_view to;
    std::string_view subject;
    std::string_view body;
    u64 item = 0; // the item's GUID, 0 none
    u32 money = 0;
    u32 cod = 0; // only with an item
};
// CMSG_SEND_MAIL: mailbox, recipient, subject, body, stationery, package, item, money, COD, then a u64 and a
// u8 the server skips (builds > 1.9.4).
void writeSendMail(std::vector<u8>& out, u64 mailbox, const MailDraft& draft);

// -------------------------------------------------------------------------------------------------------
// Auction house.

// AuctionAction / AuctionError (AuctionHouse/AuctionHouseMgr.h).
constexpr u32 kAuctionActionStarted = 0;
constexpr u32 kAuctionActionRemoved = 1;
constexpr u32 kAuctionActionBidPlaced = 2;
constexpr u32 kAuctionOk = 0;
constexpr u32 kAuctionErrInventory = 1; // the InventoryResult follows
constexpr u32 kAuctionErrDatabase = 2;
constexpr u32 kAuctionErrNotEnoughMoney = 3;
constexpr u32 kAuctionErrItemNotFound = 4;
constexpr u32 kAuctionErrHigherBid = 5; // the new bidder, bid and outbid follow
constexpr u32 kAuctionErrBidIncrement = 7;
constexpr u32 kAuctionErrBidOwn = 10;
constexpr u32 kAuctionErrRestrictedAccount = 13;
constexpr u32 kAuctionAny = 0xFFFFFFFFu; // a list filter that takes everything
constexpr u32 kAuctionPageSize = 50;     // auctions per list answer
// The three durations the server accepts (minutes: 2, 8 and 24 hours; MIN_AUCTION_TIME is 2 hours).
constexpr std::array<u32, 3> kAuctionDurations = {120, 480, 1440};

struct AuctionHello { // MSG_AUCTION_HELLO (server): the window opens
    u64 auctioneer = 0;
    u32 houseId = 0; // AuctionHouse.dbc
};
bool parseAuctionHello(std::span<const u8> payload, AuctionHello& out);

struct AuctionItem { // one auction (AuctionEntry::BuildAuctionInfo)
    u32 id = 0;
    u32 itemId = 0;
    u32 enchantment = 0;
    u32 randomPropertyId = 0;
    u32 suffixFactor = 0;
    u32 count = 0;
    u32 charges = 0;
    u64 owner = 0;
    u32 startBid = 0;
    u32 minOutbid = 0; // what a new bid must add to `bid` (0 while nobody bid)
    u32 buyout = 0;    // 0: no buyout
    u32 timeLeftMs = 0;
    u64 bidder = 0;
    u32 bid = 0; // 0: nobody bid yet
};
struct AuctionListResult {          // SMSG_AUCTION_LIST_RESULT, _OWNER_LIST_RESULT, _BIDDER_LIST_RESULT
    std::vector<AuctionItem> items; // reserve kAuctionPageSize
    u32 total = 0;                  // matching auctions in all pages
};
bool parseAuctionList(std::span<const u8> payload, AuctionListResult& out);

struct AuctionCommandResult { // SMSG_AUCTION_COMMAND_RESULT (WorldSession::SendAuctionCommandResult)
    u32 auctionId = 0;
    u32 action = 0;         // kAuctionAction*
    u32 error = 0;          // kAuctionOk / kAuctionErr*
    u32 outbid = 0;         // kAuctionOk on a bid, kAuctionErrHigherBid
    u32 inventoryError = 0; // kAuctionErrInventory
    u64 newBidder = 0;      // kAuctionErrHigherBid
    u32 newBid = 0;
};
bool parseAuctionCommandResult(std::span<const u8> payload, AuctionCommandResult& out);

struct AuctionBidderNotification { // SMSG_AUCTION_BIDDER_NOTIFICATION: the player won, or was outbid
    u32 houseId = 0;
    u32 auctionId = 0;
    u64 bidder = 0;
    u32 bid = 0; // 0: won; else the new bid that outbid the player
    u32 outbid = 0;
    u32 itemId = 0;
    u32 randomPropertyId = 0;
    bool won() const { return bid == 0; }
};
bool parseAuctionBidderNotification(std::span<const u8> payload, AuctionBidderNotification& out);

struct AuctionOwnerNotification { // SMSG_AUCTION_OWNER_NOTIFICATION: someone bid on, or bought, an auction
    u32 auctionId = 0;
    u32 bid = 0;
    u32 outbid = 0;
    u64 bidder = 0; // 0: sold
    u32 itemId = 0;
    u32 randomPropertyId = 0;
    bool sold() const { return bidder == 0; }
};
bool parseAuctionOwnerNotification(std::span<const u8> payload, AuctionOwnerNotification& out);

struct AuctionRemovedNotification { // SMSG_AUCTION_REMOVED_NOTIFICATION: an auction the player bid on was
                                    // cancelled
    u32 auctionId = 0;
    u32 itemId = 0;
    u32 randomPropertyId = 0;
};
bool parseAuctionRemovedNotification(std::span<const u8> payload, AuctionRemovedNotification& out);

struct AuctionQuery { // CMSG_AUCTION_LIST_ITEMS' filters
    u32 from = 0;     // first auction (a multiple of kAuctionPageSize)
    std::string_view name;
    u8 levelMin = 0; // required level; 0: any
    u8 levelMax = 0;
    u32 inventoryType = kAuctionAny;
    u32 itemClass = kAuctionAny;
    u32 subclass = kAuctionAny;
    u32 quality = kAuctionAny; // this quality or better
    bool usable = false;       // only what the player can use
};
void writeAuctionListItems(std::vector<u8>& out, u64 auctioneer, const AuctionQuery& query);
// CMSG_AUCTION_LIST_OWNER_ITEMS.
void writeAuctionListOwnerItems(std::vector<u8>& out, u64 auctioneer, u32 from);
// CMSG_AUCTION_LIST_BIDDER_ITEMS: the auctions to refresh besides those the player leads (may be empty).
void writeAuctionListBidderItems(std::vector<u8>& out, u64 auctioneer, u32 from,
                                 std::span<const u32> refresh);
// CMSG_AUCTION_PLACE_BID: a bid, or the buyout price.
void writeAuctionPlaceBid(std::vector<u8>& out, u64 auctioneer, u32 auctionId, u32 price);
// CMSG_AUCTION_REMOVE_ITEM: cancels one of the player's auctions.
void writeAuctionRemoveItem(std::vector<u8>& out, u64 auctioneer, u32 auctionId);
// CMSG_AUCTION_SELL_ITEM: the item's GUID, starting bid, buyout (0: none) and duration in minutes.
void writeAuctionSellItem(std::vector<u8>& out, u64 auctioneer, u64 item, u32 bid, u32 buyout, u32 minutes);

// AuctionEntry::GetAuctionOutBid: what a new bid must add (5 % of the bid in whole silver... 1 copper at
// least).
constexpr u32 auctionOutbid(u32 bid) {
    const u32 outbid = (bid / 100) * 5;
    return outbid != 0 ? outbid : 1;
}
// The least a bid on `a` may be (the starting bid when nobody bid yet).
constexpr u32 auctionMinimumBid(const AuctionItem& a) {
    return a.bid != 0 ? a.bid + (a.minOutbid != 0 ? a.minOutbid : auctionOutbid(a.bid)) : a.startBid;
}
// AuctionHouseMgr::GetAuctionDeposit with the default rates (Rate.Auction.Deposit 1, Auction.Deposit.Min 0):
// the item's vendor price x count x (minutes / 120), times the house's deposit percentage.
constexpr u32 auctionDeposit(u32 sellPrice, u32 count, u32 minutes, u32 depositPercent) {
    const f32 deposit =
        static_cast<f32>(sellPrice * count * (minutes / 120u)) * static_cast<f32>(depositPercent) / 100.0f;
    return static_cast<u32>(deposit);
}

// -------------------------------------------------------------------------------------------------------
// Player trade.

// TradeStatus (SharedDefines.h).
constexpr u32 kTradeBusy = 0;
constexpr u32 kTradeBegin = 1; // someone wants to trade: the initiator's GUID follows
constexpr u32 kTradeOpenWindow = 2;
constexpr u32 kTradeCanceled = 3;
constexpr u32 kTradeAccept = 4; // the other side accepted
constexpr u32 kTradeBusy2 = 5;
constexpr u32 kTradeNoTarget = 6;
constexpr u32 kTradeBackToTrade = 7; // an acceptance was withdrawn (something changed)
constexpr u32 kTradeComplete = 8;
constexpr u32 kTradeRejected = 9;
constexpr u32 kTradeTargetTooFar = 10;
constexpr u32 kTradeWrongFaction = 11;
constexpr u32 kTradeCloseWindow = 12; // a result, a byte and an item limit category follow
constexpr u32 kTradeUnknown13 = 13;   // handled as cancelled
constexpr u32 kTradeIgnoreYou = 14;
constexpr u32 kTradeYouStunned = 15;
constexpr u32 kTradeTargetStunned = 16;
constexpr u32 kTradeYouDead = 17;
constexpr u32 kTradeTargetDead = 18;
constexpr u32 kTradeYouLogout = 19;
constexpr u32 kTradeTargetLogout = 20;
constexpr u32 kTradeTrialAccount = 21;
constexpr u32 kTradeOnlyConjured = 22; // a slot follows
// TradeSlots (Objects/TradeData.h): six traded slots and one whose item is only enchanted (never changes
// hands).
constexpr u32 kTradeSlotCount = 7;
constexpr u32 kTradeSlotsTraded = 6;
constexpr u32 kTradeSlotNonTraded = 6;
constexpr f32 kTradeDistance = 11.11f; // TRADE_DISTANCE (Objects/ObjectDefines.h)

struct TradeStatus { // SMSG_TRADE_STATUS (WorldPackets::Trade::TradeStatus)
    u32 status = 0;
    u64 initiator = 0;   // kTradeBegin
    u32 closeResult = 0; // kTradeCloseWindow
    u8 closeUnknown = 0;
    u32 closeItemLimitCategory = 0;
    u8 slot = 0; // kTradeOnlyConjured
};
bool parseTradeStatus(std::span<const u8> payload, TradeStatus& out);

struct TradeSlotInfo { // one slot of SMSG_TRADE_STATUS_EXTENDED (15 words)
    u32 itemId = 0;    // 0: empty
    u32 displayId = 0;
    u32 count = 0;
    bool wrapped = false; // a gift: its stats hidden
    u64 giftCreator = 0;
    u32 enchantment = 0;
    u64 creator = 0;
    u32 charges = 0;
    u32 suffixFactor = 0;
    u32 randomPropertyId = 0;
    u32 lockId = 0;
    u32 maxDurability = 0;
    u32 durability = 0;
    bool empty() const { return itemId == 0; }
};
struct TradeStatusExtended { // SMSG_TRADE_STATUS_EXTENDED (WorldSession::SendUpdateTrade)
    bool trader = false;     // true: the other player's side; false: the player's own (sent after spells)
    u32 slotCount = 0;
    u32 money = 0; // copper offered
    u32 spell = 0; // cast on the non-traded slot's item
    std::array<TradeSlotInfo, kTradeSlotCount> slots{};
};
bool parseTradeStatusExtended(std::span<const u8> payload, TradeStatusExtended& out);

// CMSG_SET_TRADE_ITEM: the trade slot and where the item is (bag 255 + player slot, or a bag's slot and
// index).
void writeSetTradeItem(std::vector<u8>& out, u8 tradeSlot, u8 bag, u8 slot);
// CMSG_ACCEPT_TRADE: a word the server skips (the client sends 1 once the window opened).
void writeAcceptTrade(std::vector<u8>& out);

} // namespace vd::net
