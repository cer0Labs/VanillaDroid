#include "net/ItemMessages.h"

namespace vd::net {

const char* buyResultName(u8 result) {
    switch (result) {
    case kBuyErrCantFindItem: return "CANT_FIND_ITEM";
    case kBuyErrItemAlreadySold: return "ITEM_ALREADY_SOLD";
    case kBuyErrNotEnoughMoney: return "NOT_ENOUGHT_MONEY";
    case kBuyErrSellerDontLikeYou: return "SELLER_DONT_LIKE_YOU";
    case kBuyErrDistanceTooFar: return "DISTANCE_TOO_FAR";
    case kBuyErrItemSoldOut: return "ITEM_SOLD_OUT";
    case kBuyErrCantCarryMore: return "CANT_CARRY_MORE";
    case kBuyErrRankRequire: return "RANK_REQUIRE";
    case kBuyErrReputationRequire: return "REPUTATION_REQUIRE";
    default: return "UNKNOWN";
    }
}

const char* sellResultName(u8 result) {
    switch (result) {
    case kSellErrCantFindItem: return "CANT_FIND_ITEM";
    case kSellErrCantSellItem: return "CANT_SELL_ITEM";
    case kSellErrCantFindVendor: return "CANT_FIND_VENDOR";
    case kSellErrYouDontOwnThatItem: return "YOU_DONT_OWN_THAT_ITEM";
    case kSellErrUnknown: return "UNK";
    case kSellErrOnlyEmptyBag: return "ONLY_EMPTY_BAG";
    default: return "UNKNOWN";
    }
}

const char* bankSlotResultName(u32 result) {
    switch (result) {
    case kBankSlotFailedTooMany: return "FAILED_TOO_MANY";
    case kBankSlotInsufficientFunds: return "INSUFFICIENT_FUNDS";
    case kBankSlotNotBanker: return "NOTBANKER";
    case kBankSlotOk: return "OK";
    default: return "UNKNOWN";
    }
}

// -------------------------------------------------------------------------------------------------------
// Server -> client.

bool parseListInventory(std::span<const u8> payload, VendorInventory& out) {
    ByteReader r(payload);
    out.vendor = r.u64v();
    out.count = 0;
    out.error = 0;
    const u8 count = r.u8v();
    if (count == 0) {
        out.error = r.u8v(); // "Vendor has no inventory"
        return r.ok();
    }
    if (count > kMaxVendorItems) {
        return false; // the server stops at MAX_VENDOR_ITEMS
    }
    for (u32 i = 0; i < count && r.ok(); ++i) {
        VendorInventory::Item& item = out.items[i];
        item.index = r.u32v();
        item.itemId = r.u32v();
        item.displayId = r.u32v();
        item.stock = r.u32v();
        item.price = r.u32v();
        item.maxDurability = r.u32v();
        item.buyCount = r.u32v();
    }
    if (r.failed()) {
        return false;
    }
    out.count = count;
    return true;
}

bool parseBuyItem(std::span<const u8> payload, BuyItemResult& out) {
    ByteReader r(payload);
    out.vendor = r.u64v();
    out.vendorSlot = r.u32v();
    out.newStock = r.u32v();
    out.count = r.u32v();
    return r.ok();
}

bool parseBuyFailed(std::span<const u8> payload, BuyFailed& out) {
    ByteReader r(payload);
    out.vendor = r.u64v();
    out.itemId = r.u32v();
    out.reason = r.u8v();
    return r.ok();
}

bool parseSellItem(std::span<const u8> payload, SellItemResult& out) {
    ByteReader r(payload);
    out.vendor = r.u64v();
    out.item = r.u64v();
    out.reason = r.u8v();
    return r.ok();
}

bool parseReadItemOk(std::span<const u8> payload, u64& item) {
    ByteReader r(payload);
    item = r.u64v();
    r.u64v(); // the same GUID again
    return r.ok();
}

bool parseReadItemFailed(std::span<const u8> payload, u64& item, u8& reason) {
    ByteReader r(payload);
    item = r.u64v();
    reason = r.u8v();
    r.u64v(); // the same GUID again
    return r.ok();
}

bool parsePageTextResponse(std::span<const u8> payload, PageText& out) {
    ByteReader r(payload);
    out.pageId = r.u32v();
    out.text = r.cstring();
    out.nextPageId = r.u32v();
    return r.ok();
}

bool parseItemTextResponse(std::span<const u8> payload, u32& textId, std::string_view& text) {
    ByteReader r(payload);
    textId = r.u32v();
    text = r.cstring();
    return r.ok();
}

bool parseItemCooldown(std::span<const u8> payload, u64& item, u32& spellId) {
    ByteReader r(payload);
    item = r.u64v();
    spellId = r.u32v();
    return r.ok();
}

bool parseSetProficiency(std::span<const u8> payload, u8& itemClass, u32& subclassMask) {
    ByteReader r(payload);
    itemClass = r.u8v();
    subclassMask = r.u32v();
    return r.ok();
}

bool parseItemEnchantTimeUpdate(std::span<const u8> payload, ItemEnchantTimeUpdate& out) {
    ByteReader r(payload);
    out.item = r.u64v();
    out.slot = r.u32v();
    out.durationS = r.u32v();
    out.player = r.u64v();
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Client -> server.

void writeSwapInvItem(std::vector<u8>& out, u8 srcSlot, u8 dstSlot) {
    ByteWriter(out).u8v(srcSlot).u8v(dstSlot);
}

void writeSwapItem(std::vector<u8>& out, u8 dstBag, u8 dstSlot, u8 srcBag, u8 srcSlot) {
    ByteWriter(out).u8v(dstBag).u8v(dstSlot).u8v(srcBag).u8v(srcSlot);
}

void writeBagSlot(std::vector<u8>& out, u8 bag, u8 slot) {
    ByteWriter(out).u8v(bag).u8v(slot);
}

void writeAutoStoreBagItem(std::vector<u8>& out, u8 srcBag, u8 srcSlot, u8 dstBag) {
    ByteWriter(out).u8v(srcBag).u8v(srcSlot).u8v(dstBag);
}

void writeSplitItem(std::vector<u8>& out, u8 srcBag, u8 srcSlot, u8 dstBag, u8 dstSlot, u8 count) {
    ByteWriter(out).u8v(srcBag).u8v(srcSlot).u8v(dstBag).u8v(dstSlot).u8v(count);
}

void writeDestroyItem(std::vector<u8>& out, u8 bag, u8 slot, u8 count) {
    ByteWriter(out).u8v(bag).u8v(slot).u8v(count).u8v(0).u8v(0).u8v(0);
}

void writeAutoEquipItemSlot(std::vector<u8>& out, u64 item, u8 dstSlot) {
    ByteWriter(out).u64v(item).u8v(dstSlot);
}

void writeBuyItem(std::vector<u8>& out, u64 vendor, u32 itemId, u8 count) {
    ByteWriter(out).u64v(vendor).u32v(itemId).u8v(count).u8v(0);
}

void writeBuyItemInSlot(std::vector<u8>& out, u64 vendor, u32 itemId, u64 bag, u8 slot, u8 count) {
    ByteWriter(out).u64v(vendor).u32v(itemId).u64v(bag).u8v(slot).u8v(count);
}

void writeSellItem(std::vector<u8>& out, u64 vendor, u64 item, u8 count) {
    ByteWriter(out).u64v(vendor).u64v(item).u8v(count);
}

void writeBuybackItem(std::vector<u8>& out, u64 vendor, u32 slot) {
    ByteWriter(out).u64v(vendor).u32v(slot);
}

void writeRepairItem(std::vector<u8>& out, u64 npc, u64 item) {
    ByteWriter(out).u64v(npc).u64v(item);
}

void writePageTextQuery(std::vector<u8>& out, u32 pageId, u64 item) {
    ByteWriter(out).u32v(pageId).u64v(item);
}

void writeItemTextQuery(std::vector<u8>& out, u32 textId) {
    ByteWriter(out).u32v(textId).u32v(0).u32v(0);
}

} // namespace vd::net
