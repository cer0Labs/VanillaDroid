#pragma once

#include "core/Types.h"
#include "net/ByteBuffer.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

// Item, vendor, bank and reading packets (phase 8), as vmangos-core 4641790 reads and writes them for build 5875
// (Handlers/ItemHandler.cpp, NPCHandler.cpp, QueryHandler.cpp, MailHandler.cpp; Server/Packets/Item.*, Npc.*,
// Query.*, Mail.*, Misc.*, Skill.*, Spell.*). Same rules as net/GameMessages.h: parsers fill a caller-owned struct
// and return false on a malformed packet without reading past its end; strings are views into the payload;
// builders append a client packet's body to a byte vector.
//
// Bodies handled with the GameMessages helpers: SMSG_SHOW_BANK and SMSG_OPEN_CONTAINER (parseGuid),
// SMSG_BUY_BANK_SLOT_RESULT (parseU32), SMSG_DURABILITY_DAMAGE_DEATH (empty); CMSG_LIST_INVENTORY,
// CMSG_BANKER_ACTIVATE and CMSG_BUY_BANK_SLOT (writeGuid), CMSG_SET_AMMO (writeU32: the ammo's entry, 0 removes).
namespace vd::net {

// -------------------------------------------------------------------------------------------------------
// Protocol values (Objects/ItemDefines.h, Objects/Player.h).

// BuyResult of SMSG_BUY_FAILED.
constexpr u8 kBuyErrCantFindItem = 0;
constexpr u8 kBuyErrItemAlreadySold = 1;
constexpr u8 kBuyErrNotEnoughMoney = 2;
constexpr u8 kBuyErrSellerDontLikeYou = 4;
constexpr u8 kBuyErrDistanceTooFar = 5;
constexpr u8 kBuyErrItemSoldOut = 7;
constexpr u8 kBuyErrCantCarryMore = 8;
constexpr u8 kBuyErrRankRequire = 11;
constexpr u8 kBuyErrReputationRequire = 12;
// SellResult of SMSG_SELL_ITEM (it is only sent on failure).
constexpr u8 kSellErrCantFindItem = 1;
constexpr u8 kSellErrCantSellItem = 2;
constexpr u8 kSellErrCantFindVendor = 3;
constexpr u8 kSellErrYouDontOwnThatItem = 4;
constexpr u8 kSellErrUnknown = 5;
constexpr u8 kSellErrOnlyEmptyBag = 6;
// BuyBankSlotResult of SMSG_BUY_BANK_SLOT_RESULT (success sends nothing: PLAYER_BYTES_2 changes).
constexpr u32 kBankSlotFailedTooMany = 0;
constexpr u32 kBankSlotInsufficientFunds = 1;
constexpr u32 kBankSlotNotBanker = 2;
constexpr u32 kBankSlotOk = 3;

constexpr u32 kVendorUnlimited = 0xFFFFFFFFu; // stock of an item a vendor never runs out of
constexpr u32 kMaxVendorItems = 128;          // MAX_VENDOR_ITEMS (Objects/CreatureDefines.h)

// Enum names ("NOT_ENOUGHT_MONEY", "CANT_SELL_ITEM", "INSUFFICIENT_FUNDS"); "UNKNOWN" beyond the tables.
const char* buyResultName(u8 result);
const char* sellResultName(u8 result);
const char* bankSlotResultName(u32 result);

// -------------------------------------------------------------------------------------------------------
// Server -> client.

struct VendorInventory { // SMSG_LIST_INVENTORY (WorldSession::SendListInventory)
    struct Item {
        u32 index = 0; // the server's row number, from 1 (SMSG_BUY_ITEM names it)
        u32 itemId = 0;
        u32 displayId = 0;
        u32 stock = kVendorUnlimited; // how many are left of a limited item
        u32 price = 0;                // copper per purchase, reputation discount applied
        u32 maxDurability = 0;
        u32 buyCount = 1; // items per purchase (a stack of 5 arrows...)
    };
    u64 vendor = 0;
    u8 error = 0; // without items: 0 "the vendor has no inventory"
    std::array<Item, kMaxVendorItems> items{};
    u32 count = 0;
    std::span<const Item> list() const { return {items.data(), count}; }
};
bool parseListInventory(std::span<const u8> payload, VendorInventory& out);

struct BuyItemResult { // SMSG_BUY_ITEM
    u64 vendor = 0;
    u32 vendorSlot = 0; // VendorInventory::Item::index of the row bought
    u32 newStock = kVendorUnlimited;
    u32 count = 0; // purchases made (each gives buyCount items)
};
bool parseBuyItem(std::span<const u8> payload, BuyItemResult& out);

struct BuyFailed { // SMSG_BUY_FAILED
    u64 vendor = 0;
    u32 itemId = 0;
    u8 reason = 0; // BuyResult
};
bool parseBuyFailed(std::span<const u8> payload, BuyFailed& out);

struct SellItemResult { // SMSG_SELL_ITEM
    u64 vendor = 0;
    u64 item = 0;
    u8 reason = 0; // SellResult
};
bool parseSellItem(std::span<const u8> payload, SellItemResult& out);

// SMSG_READ_ITEM_OK: the item GUID, twice.
bool parseReadItemOk(std::span<const u8> payload, u64& item);
// SMSG_READ_ITEM_FAILED: item GUID, reason, item GUID.
bool parseReadItemFailed(std::span<const u8> payload, u64& item, u8& reason);

struct PageText { // SMSG_PAGE_TEXT_QUERY_RESPONSE: one page; the server sends the whole chain at once
    u32 pageId = 0;
    std::string_view text;
    u32 nextPageId = 0; // 0: the last page
};
bool parsePageTextResponse(std::span<const u8> payload, PageText& out);

// SMSG_ITEM_TEXT_QUERY_RESPONSE: text id + text (letters copied from the mail).
bool parseItemTextResponse(std::span<const u8> payload, u32& textId, std::string_view& text);

// SMSG_ITEM_COOLDOWN: the 30 s an item's on-use spell waits after equipping it.
bool parseItemCooldown(std::span<const u8> payload, u64& item, u32& spellId);

// SMSG_SET_PROFICIENCY: the subclasses of an item class the player may use (weapons 2, armor 4).
bool parseSetProficiency(std::span<const u8> payload, u8& itemClass, u32& subclassMask);

struct ItemEnchantTimeUpdate { // SMSG_ITEM_ENCHANT_TIME_UPDATE (builds > 1.10.2: with the player)
    u64 item = 0;
    u32 slot = 0;       // EnchantmentSlot
    u32 durationS = 0;  // seconds left, 0: gone
    u64 player = 0;
};
bool parseItemEnchantTimeUpdate(std::span<const u8> payload, ItemEnchantTimeUpdate& out);

// -------------------------------------------------------------------------------------------------------
// Client -> server bodies. Bag 255 is the player's own row of slots (equipment 0..18, worn bags 19..22,
// backpack 23..38, bank 39..62, bank bags 63..68, keyring 81..96); a worn or bank bag's slot number (19..22,
// 63..68) addresses its contents.

// CMSG_SWAP_INV_ITEM: two slots of bag 255.
void writeSwapInvItem(std::vector<u8>& out, u8 srcSlot, u8 dstSlot);
// CMSG_SWAP_ITEM: the destination first, as the server reads it.
void writeSwapItem(std::vector<u8>& out, u8 dstBag, u8 dstSlot, u8 srcBag, u8 srcSlot);
// Bag + slot: CMSG_AUTOEQUIP_ITEM, CMSG_READ_ITEM, CMSG_OPEN_ITEM, CMSG_AUTOBANK_ITEM, CMSG_AUTOSTORE_BANK_ITEM.
void writeBagSlot(std::vector<u8>& out, u8 bag, u8 slot);
// CMSG_AUTOSTORE_BAG_ITEM: into the first room of `dstBag`.
void writeAutoStoreBagItem(std::vector<u8>& out, u8 srcBag, u8 srcSlot, u8 dstBag);
// CMSG_SPLIT_ITEM: `count` of the stack to an empty slot (or onto a stack of the same item).
void writeSplitItem(std::vector<u8>& out, u8 srcBag, u8 srcSlot, u8 dstBag, u8 dstSlot, u8 count);
// CMSG_DESTROYITEM: `count` 0 destroys the whole stack (three unused bytes follow).
void writeDestroyItem(std::vector<u8>& out, u8 bag, u8 slot, u8 count);
// CMSG_AUTOEQUIP_ITEM_SLOT: an item (anywhere) into equipment slot `dstSlot`.
void writeAutoEquipItemSlot(std::vector<u8>& out, u64 item, u8 dstSlot);
// CMSG_BUY_ITEM: `count` purchases (each one gives the row's buyCount items) wherever they fit.
void writeBuyItem(std::vector<u8>& out, u64 vendor, u32 itemId, u8 count);
// CMSG_BUY_ITEM_IN_SLOT: `bag` is the player's GUID for bag 255, else the worn bag's GUID.
void writeBuyItemInSlot(std::vector<u8>& out, u64 vendor, u32 itemId, u64 bag, u8 slot, u8 count);
// CMSG_SELL_ITEM: `count` 0 sells the whole stack.
void writeSellItem(std::vector<u8>& out, u64 vendor, u64 item, u8 count);
// CMSG_BUYBACK_ITEM: the buyback slot as a player slot (69..80).
void writeBuybackItem(std::vector<u8>& out, u64 vendor, u32 slot);
// CMSG_REPAIR_ITEM: `item` 0 repairs everything worn and carried.
void writeRepairItem(std::vector<u8>& out, u64 npc, u64 item);
// CMSG_PAGE_TEXT_QUERY: the first page (and the item, which the server ignores).
void writePageTextQuery(std::vector<u8>& out, u32 pageId, u64 item);
// CMSG_ITEM_TEXT_QUERY: text id, then two words the server ignores (mail id, unknown).
void writeItemTextQuery(std::vector<u8>& out, u32 textId);

} // namespace vd::net
