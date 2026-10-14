#include "app/ExchangeScreens.h"

#include "app/DialogKit.h"
#include "app/GameSession.h"
#include "app/InventoryScreens.h"
#include "app/PopupScreens.h"
#include "app/SpellTargetingScreens.h"
#include "app/UiResources.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/ExchangeData.h"
#include "game/GameData.h"
#include "game/ItemActions.h"
#include "game/MasterLoot.h"
#include "game/UnitInfo.h"
#include "net/ExchangeMessages.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;
using game::AuctionList;
using game::GameEventKind;
using ui::rgba;

// The LOOT_ROLL_* texts carry the item link ("%s|Hitem:%d:%d:%d:%d|h[%s]|h%s": its color, entry, enchantment,
// property, unique id, name and "|r"); the chat shows the link's text (stripLinks).
constexpr u32 kErrorColor = rgba(255, 26, 26);
constexpr u32 kInfoColor = rgba(255, 255, 0);   // UIErrorsFrame.lua UI_INFO_MESSAGE
constexpr u32 kSystemColor = rgba(255, 255, 0); // ChatTypeInfo SYSTEM
constexpr u32 kLootColor = rgba(0, 170, 0);     // ChatTypeInfo LOOT: the group's rolls are loot lines
constexpr f32 kAuctioneerReach = 10.0f; // the auction house closes beyond this (as the vendor)
// The mailbox: INTERACTION_DISTANCE (5 yd) around its bounds, as GameObject::IsAtInteractDistance measures
// it.
constexpr f32 kMailboxReach = 6.5f;
constexpr u64 kLetterIcon = assetPathHash("Interface\\Icons\\INV_Letter_15.blp");
constexpr u64 kCoinIcon = assetPathHash("Interface\\Icons\\INV_Misc_Coin_01.blp");
// ITEM_QUALITY0_DESC..4: the qualities the auction house filters by.
constexpr const char* kQualityKeys[] = {"ITEM_QUALITY0_DESC", "ITEM_QUALITY1_DESC", "ITEM_QUALITY2_DESC",
                                        "ITEM_QUALITY3_DESC", "ITEM_QUALITY4_DESC"};
// ITEM_QUALITY_COLORS as chat color codes.
constexpr const char* kQualityCodes[] = {"|cff9d9d9d", "|cffffffff", "|cff1eff00", "|cff0070dd",
                                         "|cffa335ee", "|cffff8000", "|cffe6cc80"};

// The chat's text of a line with item links: "|Hitem:...|h[Name]|h" shows as "[Name]".
void stripLinks(char* s) {
    char* out = s;
    for (const char* in = s; *in;) {
        if (in[0] == '|' && in[1] == 'H') {
            const char* end = std::strstr(in + 2, "|h");
            if (!end) {
                break;
            }
            in = end + 2;
            continue;
        }
        if (in[0] == '|' && in[1] == 'h') {
            in += 2;
            continue;
        }
        *out++ = *in++;
    }
    *out = '\0';
}

// A view as a NUL-terminated string for sscanf (cut to the buffer).
const char* terminated(std::string_view s, char* out, usize size) {
    const usize n = std::min(s.size(), size - 1);
    std::memcpy(out, s.data(), n);
    out[n] = '\0';
    return out;
}

std::string_view timeLeftText(u32 ms) {
    constexpr u32 kMinute = 60000;
    return ms < 30 * kMinute    ? text::get("AUCTION_TIME_LEFT1")
           : ms < 120 * kMinute ? text::get("AUCTION_TIME_LEFT2")
           : ms < 480 * kMinute ? text::get("AUCTION_TIME_LEFT3")
                                : text::get("AUCTION_TIME_LEFT4");
}

bool moneyField(u32 f, u32 first) {
    return f >= first && f < first + 3;
}

// Phase 18 (extras): GetAuctionItemInfo's canUse (Blizzard_AuctionUI tints the icon 1.0, 0.1, 0.1 without it).
bool canUse(GameSession& session, const net::ItemQueryInfo& item) {
    return session.play().inventory().usable(item);
}

} // namespace

void ExchangeScreens::init(const game::GameData* data, const game::ExchangeData* exchange,
                           UiResources* resources) {
    m_data = data;
    m_exchange = exchange && exchange->ready() ? exchange : nullptr;
    m_resources = resources;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    m_rollViews.reserve(game::LootRolls::kMaxRolls);
    m_choices.reserve(96); // the categories (61) and the raid's members (40)
    m_masterItems.reserve(game::MasterLootSlots::kSlots + 1); // phase 15 (items)
    m_masterItems2.reserve(5);
    m_masterGuids.reserve(game::MasterLootSlots::kSlots + 1);
    m_masterGuids2.reserve(5);
    m_to.reserve(48);
    m_subject.reserve(net::kMailMaxSubject + 8);
    m_body.reserve(net::kMailMaxBody + 8);
    m_mailRows.reserve(net::kMaxMailMessages);
    m_mailIds.reserve(net::kMaxMailMessages);
    m_search.name.reserve(64);
    m_auctionRows.reserve(net::kAuctionPageSize);
    for (std::vector<u16>& order : m_auctionOrder) { // phase 18 (p3)
        order.reserve(net::kAuctionPageSize);
    }
    m_auctionIds.reserve(net::kAuctionPageSize);
    m_bagCells.reserve(game::kBackpackSize + 4 * game::kMaxBagSize + 32);
    m_categoryLabels.reserve(game::auctionCategories().size() + 1);
    m_categoryLabels.emplace_back(); // ALL: a text of the table, loaded after this
    for (const game::AuctionCategory& category : game::auctionCategories()) {
        std::string label;
        const std::string_view className =
            m_exchange ? m_exchange->itemClassName(category.itemClass) : std::string_view{};
        if (category.subclass == net::kAuctionAny) {
            label.assign(className.empty() ? std::string_view("?") : className);
        } else {
            const std::string_view sub =
                m_exchange ? m_exchange->itemSubclassName(category.itemClass, category.subclass)
                           : std::string_view{};
            label.assign("   ");
            label.append(sub.empty() ? std::string_view("?") : sub);
        }
        m_categoryLabels.push_back(std::move(label));
    }
}

void ExchangeScreens::reset() {
    m_lineFirst = m_lineCount = 0;
    m_field = Field::None;
    m_masterOpen = false;
    m_mailTab = 0;
    m_selectedMail = 0;
    m_to.clear();
    m_subject.clear();
    m_body.clear();
    m_mailMoney = 0;
    m_cod = false;
    m_tradeMoney = 0;
    m_tradeMoneyTyped = false;
    m_auctionTab = 0;
    m_selectedAuction = 0;
    m_bidAmount = 0;
    m_search = game::AuctionSearch{};
    m_category = 0;
    m_quality = 0;
    m_startBid = m_buyout = 0;
    m_duration = 1;
    m_stagedShown = 0;
    m_picker = Picker::None;
}

// -------------------------------------------------------------------------------------------------------
// Lines.

ExchangeScreens::Line& ExchangeScreens::addLine(bool chat, u32 color) {
    Line& l = m_lines[(m_lineFirst + m_lineCount) % m_lines.size()];
    if (m_lineCount < m_lines.size()) {
        ++m_lineCount;
    } else {
        m_lineFirst = (m_lineFirst + 1) % m_lines.size(); // the oldest goes
    }
    l.text[0] = '\0';
    l.color = color;
    l.chat = chat;
    l.big = false;
    return l;
}

void ExchangeScreens::lineAdded(Line& l) {
    stripLinks(l.text.data());
    if (m_trace) {
        VD_LOG_INFO("Exchange: %s", l.text.data());
    }
}

void ExchangeScreens::lineText(bool chat, u32 color, std::string_view s) {
    Line& l = addLine(chat, color);
    const usize n = std::min(s.size(), l.text.size() - 1);
    std::memcpy(l.text.data(), s.data(), n);
    l.text[n] = '\0';
    lineAdded(l);
}

bool ExchangeScreens::popLine(Line& out) {
    if (m_lineCount == 0) {
        return false;
    }
    out = m_lines[m_lineFirst];
    m_lineFirst = (m_lineFirst + 1) % m_lines.size();
    --m_lineCount;
    return true;
}

std::string_view ExchangeScreens::categoryLabel(u32 index) const {
    return index == 0 || index >= m_categoryLabels.size() ? text::get("ALL")
                                                          : std::string_view(m_categoryLabels[index]);
}

std::string_view ExchangeScreens::itemName(GameSession& session, u32 itemId) {
    const net::ItemQueryInfo* info = itemId ? session.item(itemId) : nullptr;
    return info ? std::string_view(info->name) : std::string_view("...");
}

std::string_view ExchangeScreens::playerName(GameSession& session, u64 guid) {
    const std::string* name = guid ? session.playerName(guid) : nullptr;
    return name ? std::string_view(*name) : std::string_view("...");
}

ItemSlotView ExchangeScreens::itemView(GameSession& session, u32 entry, u32 count) const {
    ItemSlotView v;
    if (entry == 0) {
        return v;
    }
    v.empty = false;
    v.count = count;
    if (const net::ItemQueryInfo* info = session.item(entry)) {
        v.quality = info->quality;
        v.iconHash = m_data ? m_data->itemIconHash(info->displayId) : 0;
    }
    return v;
}

ItemSlotView ExchangeScreens::bagItemView(GameSession& session, u64 guid) const {
    const game::ItemInstance* item = guid ? session.play().inventory().find(guid) : nullptr;
    return item ? itemView(session, item->entry, item->count) : ItemSlotView{};
}

// -------------------------------------------------------------------------------------------------------
// Events.

void ExchangeScreens::update(GameSession& session, InventoryScreens& items, u32 nowMs) {
    (void)nowMs;
    game::Gameplay& play = session.play();
    if (m_cursorsPlayer != session.playerGuid()) {
        m_cursorsPlayer = session.playerGuid();
        m_eventCursor = play.events().end();
        reset();
    }
    readEvents(session, items);
    // The mailbox and the auction house close when left behind (or gone), as the client's frames.
    game::Exchange& ex = play.exchange();
    const game::Entity* me = session.entities().find(session.playerGuid());
    const auto tooFar = [&](u64 guid, bool unit) {
        const game::Entity* other = session.entities().find(guid);
        if (!me || !other) {
            return true;
        }
        u32 radiusBits = unit ? other->value(uf::UNIT_FIELD_BOUNDINGRADIUS) : 0;
        f32 radius = 0.0f;
        std::memcpy(&radius, &radiusBits, sizeof(radius));
        return game::distanceAt(*me, *other, nowMs) - radius > (unit ? kAuctioneerReach : kMailboxReach);
    };
    if (ex.mail().open() && tooFar(ex.mail().mailbox(), false)) {
        ex.mail().close();
    }
    if (ex.auction().open() && tooFar(ex.auction().auctioneer(), true)) {
        ex.auction().close();
    }
    if (m_masterOpen && !play.loot().open()) {
        m_masterOpen = false;
    }
}

void ExchangeScreens::readEvents(GameSession& session, InventoryScreens& items) {
    game::Gameplay& play = session.play();
    const u64 me = session.playerGuid();
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        using K = GameEventKind;
        const net::ItemQueryInfo* info = e->id ? session.item(e->id) : nullptr;
        const char* color = kQualityCodes[std::min<u32>(info ? info->quality : 1, 6)];
        const char* name = info ? info->name.c_str() : "...";
        const i32 entry = static_cast<i32>(e->id);
        switch (e->kind) {
        case K::LootRolled: {
            if (!m_lootSpam) {
                break;
            }
            const bool mine = e->other == me;
            const std::string_view who = playerName(session, e->other);
            switch (static_cast<net::LootRollKind>(e->value[0])) {
            case net::LootRollKind::ChoseNeed:
                if (mine) {
                    line(true, kLootColor, "LOOT_ROLL_NEED_SELF", color, entry, 0, 0, 0, name, "|r");
                } else {
                    line(true, kLootColor, "LOOT_ROLL_NEED", who, color, entry, 0, 0, 0, name, "|r");
                }
                break;
            case net::LootRollKind::ChoseGreed:
                if (mine) {
                    line(true, kLootColor, "LOOT_ROLL_GREED_SELF", color, entry, 0, 0, 0, name, "|r");
                } else {
                    line(true, kLootColor, "LOOT_ROLL_GREED", who, color, entry, 0, 0, 0, name, "|r");
                }
                break;
            case net::LootRollKind::Passed:
                if (mine) {
                    line(true, kLootColor, "LOOT_ROLL_PASSED_SELF", color, entry, 0, 0, 0, name, "|r");
                } else {
                    line(true, kLootColor, "LOOT_ROLL_PASSED", who, color, entry, 0, 0, 0, name, "|r");
                }
                break;
            case net::LootRollKind::NeedRoll:
            case net::LootRollKind::GreedRoll:
                line(true, kLootColor,
                     static_cast<net::LootRollKind>(e->value[0]) == net::LootRollKind::NeedRoll
                         ? "LOOT_ROLL_ROLLED_NEED"
                         : "LOOT_ROLL_ROLLED_GREED",
                     e->value[1], color, entry, 0, 0, 0, name, "|r", who);
                break;
            default: break;
            }
            break;
        }
        case K::LootRollWon:
            if (!m_lootSpam) { // the roll in grey after the item: "(Need - 87)"
                const bool need = e->value[1] == 1; // ROLL_NEED (else ROLL_GREED)
                if (e->other == me) {
                    line(true, kLootColor, need ? "LOOT_ROLL_YOU_WON_NO_SPAM_NEED" : "LOOT_ROLL_YOU_WON_NO_SPAM_GREED",
                         e->value[0], color, entry, 0, 0, 0, name, "|r");
                } else {
                    line(true, kLootColor, need ? "LOOT_ROLL_WON_NO_SPAM_NEED" : "LOOT_ROLL_WON_NO_SPAM_GREED",
                         playerName(session, e->other), e->value[0], color, entry, 0, 0, 0, name, "|r");
                }
            } else if (e->other == me) {
                line(true, kLootColor, "LOOT_ROLL_YOU_WON", color, entry, 0, 0, 0, name, "|r");
            } else {
                line(true, kLootColor, "LOOT_ROLL_WON", playerName(session, e->other), color, entry, 0, 0,
                     0, name, "|r");
            }
            break;
        case K::LootAllPassed:
            line(true, kLootColor, "LOOT_ROLL_ALL_PASSED", color, entry, 0, 0, 0, name, "|r");
            break;
        case K::NewMail: line(true, kSystemColor, "HAVE_MAIL"); break;
        case K::MailResult: {
            const u32 action = static_cast<u32>(e->value[0]), error = static_cast<u32>(e->value[1]);
            if (error == net::kMailOk) {
                if (action == net::kMailActionSend) {
                    line(false, kInfoColor, "ERR_MAIL_SENT");
                    m_to.clear(); // the client empties the form after a message went
                    m_subject.clear();
                    m_body.clear();
                    m_mailMoney = 0;
                    m_cod = false;
                }
                break;
            }
            const char* message = "ERR_MAIL_DATABASE_ERROR"; // a key
            switch (error) {
            case net::kMailErrEquipError: {
                game::GameEvent inventory{};
                inventory.kind = K::InventoryError;
                inventory.value[0] = e->value[2];
                char out[160];
                u32 c = kErrorColor;
                if (items.eventMessage(session, inventory, out, sizeof(out), c)) {
                    lineText(false, c, out);
                }
                message = nullptr;
                break;
            }
            case net::kMailErrCannotSendToSelf: message = "ERR_MAIL_TO_SELF"; break;
            case net::kMailErrNotEnoughMoney: message = "ERR_NOT_ENOUGH_MONEY"; break;
            case net::kMailErrRecipientNotFound: message = "ERR_MAIL_TARGET_NOT_FOUND"; break;
            case net::kMailErrNotYourTeam: message = "ERR_PLAYER_WRONG_FACTION"; break;
            case net::kMailErrDisabledForTrial: message = "ERR_RESTRICTED_ACCOUNT"; break;
            case net::kMailErrRecipientCapReached: message = "ERR_MAIL_REACHED_CAP"; break;
            default: break;
            }
            if (message) {
                line(false, kErrorColor, message);
            }
            break;
        }
        case K::AuctionOpened:
            m_auctionTab = 0;
            m_selectedAuction = 0;
            m_picker = Picker::None;
            items.open(InventoryScreens::Window::None); // the auction house replaces the bags
            break;
        case K::AuctionResult: {
            const u32 action = static_cast<u32>(e->value[0]), error = static_cast<u32>(e->value[1]);
            const char* message = nullptr; // a key
            switch (error) {
            case net::kAuctionOk:
                message = action == net::kAuctionActionStarted   ? "ERR_AUCTION_STARTED"
                          : action == net::kAuctionActionRemoved ? "ERR_AUCTION_REMOVED"
                                                                 : "ERR_AUCTION_BID_PLACED";
                break;
            case net::kAuctionErrInventory: {
                game::GameEvent inventory{};
                inventory.kind = K::InventoryError;
                inventory.value[0] = e->value[2];
                char out[160];
                u32 c = kErrorColor;
                if (items.eventMessage(session, inventory, out, sizeof(out), c)) {
                    lineText(false, c, out);
                }
                break;
            }
            case net::kAuctionErrNotEnoughMoney: message = "ERR_NOT_ENOUGH_MONEY"; break;
            case net::kAuctionErrItemNotFound: message = "ERR_ITEM_NOT_FOUND"; break;
            case net::kAuctionErrHigherBid: message = "ERR_AUCTION_HIGHER_BID"; break;
            case net::kAuctionErrBidIncrement: message = "ERR_AUCTION_BID_INCREMENT"; break;
            case net::kAuctionErrBidOwn: message = "ERR_AUCTION_BID_OWN"; break;
            case net::kAuctionErrRestrictedAccount: message = "ERR_RESTRICTED_ACCOUNT"; break;
            default: message = "ERR_AUCTION_DATABASE_ERROR"; break;
            }
            if (message) {
                line(false, error == net::kAuctionOk ? kInfoColor : kErrorColor, message);
            }
            break;
        }
        case K::AuctionOutbid:
            line(true, kSystemColor, "ERR_AUCTION_OUTBID_S", name);
            line(false, kInfoColor, "ERR_AUCTION_OUTBID_S", name);
            break;
        case K::AuctionWon:
            line(true, kSystemColor, "ERR_AUCTION_WON_S", name);
            line(false, kInfoColor, "ERR_AUCTION_WON_S", name);
            break;
        case K::AuctionSold:
            line(true, kSystemColor, "ERR_AUCTION_SOLD_S", name);
            line(false, kInfoColor, "ERR_AUCTION_SOLD_S", name);
            break;
        case K::AuctionBid: line(true, kSystemColor, "VD_EXCHANGE_AUCTION_BID_ON_YOURS", name); break;
        case K::AuctionRemoved: line(true, kSystemColor, "ERR_AUCTION_REMOVED_S", name); break;
        case K::TradeOpened:
            m_tradeMoney = 0;
            m_tradeMoneyTyped = false;
            items.open(InventoryScreens::Window::None);
            break;
        case K::TradeClosed: {
            const std::string_view partner = playerName(session, e->guid);
            const u32 status = static_cast<u32>(e->value[0]);
            if (m_field >= Field::TradeGold && m_field <= Field::TradeCopper) {
                m_field = Field::None;
            }
            switch (status) {
            case net::kTradeComplete: line(false, kInfoColor, "ERR_TRADE_COMPLETE"); break;
            case net::kTradeCanceled:
            case net::kTradeUnknown13:
            case net::kTradeCloseWindow:
            case net::kTradeRejected: line(false, kInfoColor, "ERR_TRADE_CANCELLED"); break;
            case net::kTradeBusy:
            case net::kTradeBusy2: line(false, kErrorColor, "ERR_PLAYER_BUSY_S", partner); break;
            case net::kTradeNoTarget: line(false, kErrorColor, "PLAYER_NOT_FOUND"); break;
            case net::kTradeTargetTooFar: line(false, kErrorColor, "ERR_TRADE_TOO_FAR"); break;
            case net::kTradeWrongFaction: line(false, kErrorColor, "ERR_PLAYER_WRONG_FACTION"); break;
            case net::kTradeIgnoreYou: line(false, kErrorColor, "ERR_IGNORING_YOU_S", partner); break;
            case net::kTradeYouStunned: line(false, kErrorColor, "ERR_GENERIC_STUNNED"); break;
            case net::kTradeTargetStunned: line(false, kErrorColor, "ERR_TARGET_STUNNED"); break;
            case net::kTradeYouDead: line(false, kErrorColor, "ERR_PLAYER_DEAD"); break;
            case net::kTradeTargetDead: line(false, kErrorColor, "ERR_TRADE_TARGET_DEAD"); break;
            case net::kTradeYouLogout: line(false, kErrorColor, "ERR_LOGGING_OUT"); break;
            case net::kTradeTargetLogout: line(false, kErrorColor, "ERR_TARGET_LOGGING_OUT"); break;
            case net::kTradeTrialAccount: line(false, kErrorColor, "ERR_RESTRICTED_ACCOUNT"); break;
            case net::kTradeOnlyConjured: line(false, kErrorColor, "ERR_TRADE_WRONG_REALM"); break;
            default: break;
            }
            break;
        }
        default: break;
        }
    }
}

// -------------------------------------------------------------------------------------------------------
// The keyboard.

ui::WidgetId ExchangeScreens::fieldId(Field f) {
    return ui::widgetId("exchange.field", static_cast<u64>(f));
}

std::string_view ExchangeScreens::fieldText(Field f, std::array<char, 12>& digits) const {
    switch (f) {
    case Field::MailTo: return m_to;
    case Field::MailSubject: return m_subject;
    case Field::MailBody: return m_body;
    case Field::AuctionName: return m_search.name;
    case Field::AuctionMin:
    case Field::AuctionMax: {
        const u32 value = f == Field::AuctionMin ? m_search.levelMin : m_search.levelMax;
        if (value == 0) {
            return {};
        }
        const i32 n = std::snprintf(digits.data(), digits.size(), "%u", value);
        return {digits.data(), static_cast<usize>(std::max(n, 0))};
    }
    default: break;
    }
    const u32 i = static_cast<u32>(f);
    const auto part = [&](Field first) { return static_cast<i32>(i - static_cast<u32>(first)); };
    if (moneyField(i, static_cast<u32>(Field::MailGold))) {
        return moneyPartText(m_mailMoney, part(Field::MailGold), digits);
    }
    if (moneyField(i, static_cast<u32>(Field::TradeGold))) {
        return moneyPartText(m_tradeMoney, part(Field::TradeGold), digits);
    }
    if (moneyField(i, static_cast<u32>(Field::BidGold))) {
        return moneyPartText(m_bidAmount, part(Field::BidGold), digits);
    }
    if (moneyField(i, static_cast<u32>(Field::StartGold))) {
        return moneyPartText(m_startBid, part(Field::StartGold), digits);
    }
    if (moneyField(i, static_cast<u32>(Field::BuyoutGold))) {
        return moneyPartText(m_buyout, part(Field::BuyoutGold), digits);
    }
    return {};
}

void ExchangeScreens::beginEdit(ui::TextInput& text, Field f) {
    if (m_field == f && text.active() && text.field() == fieldId(f)) {
        endEdit(text); // a second tap closes the keyboard
        return;
    }
    m_field = f;
    std::array<char, 12> digits{};
    const std::string_view initial = fieldText(f, digits);
    u32 maxBytes = 12;
    ui::TextKind kind = ui::TextKind::Plain;
    switch (f) {
    case Field::MailTo: maxBytes = 47; break;
    case Field::MailSubject: maxBytes = net::kMailMaxSubject; break;
    case Field::MailBody:
        maxBytes = net::kMailMaxBody;
        kind = ui::TextKind::Message;
        break;
    case Field::AuctionName: maxBytes = 63; break;
    case Field::AuctionMin:
    case Field::AuctionMax: maxBytes = 2; break;
    default: maxBytes = static_cast<u32>(f) % 3 == static_cast<u32>(Field::MailGold) % 3 ? 6 : 2; break;
    }
    text.begin(fieldId(f), initial, maxBytes, kind);
}

void ExchangeScreens::endEdit(ui::TextInput& text) {
    if (m_field == Field::None) {
        return;
    }
    if (text.active() && text.field() == fieldId(m_field)) {
        text.end();
    }
    m_field = Field::None;
}

void ExchangeScreens::sync(ui::TextInput& text) {
    if (m_field == Field::None) {
        return;
    }
    if (!text.active() || text.field() != fieldId(m_field)) {
        m_field = Field::None; // closed (Back), or another field took the keyboard
        return;
    }
    const std::string_view typed = text.text();
    const u32 i = static_cast<u32>(m_field);
    const auto part = [&](Field first) { return static_cast<i32>(i - static_cast<u32>(first)); };
    switch (m_field) {
    case Field::MailTo: m_to.assign(typed); break;
    case Field::MailSubject: m_subject.assign(typed); break;
    case Field::MailBody: m_body.assign(typed); break;
    case Field::AuctionName: m_search.name.assign(typed); break;
    case Field::AuctionMin:
    case Field::AuctionMax: {
        u32 value = 0;
        for (const char ch : typed) {
            value =
                ch >= '0' && ch <= '9' ? std::min<u32>(value * 10 + static_cast<u32>(ch - '0'), 60) : value;
        }
        (m_field == Field::AuctionMin ? m_search.levelMin : m_search.levelMax) = value;
        break;
    }
    default:
        if (moneyField(i, static_cast<u32>(Field::MailGold))) {
            m_mailMoney = moneyWithPart(m_mailMoney, part(Field::MailGold), typed);
        } else if (moneyField(i, static_cast<u32>(Field::TradeGold))) {
            m_tradeMoney = moneyWithPart(m_tradeMoney, part(Field::TradeGold), typed);
            m_tradeMoneyTyped = true;
        } else if (moneyField(i, static_cast<u32>(Field::BidGold))) {
            m_bidAmount = moneyWithPart(m_bidAmount, part(Field::BidGold), typed);
        } else if (moneyField(i, static_cast<u32>(Field::StartGold))) {
            m_startBid = moneyWithPart(m_startBid, part(Field::StartGold), typed);
        } else if (moneyField(i, static_cast<u32>(Field::BuyoutGold))) {
            m_buyout = moneyWithPart(m_buyout, part(Field::BuyoutGold), typed);
        }
        break;
    }
    if (text.submitted()) {
        endEdit(text);
    }
}

bool ExchangeScreens::drawEditor(DialogContext& c, ui::TextInput& text) {
    if (m_field == Field::None || !keyboardCrowds(c)) {
        return false;
    }
    std::array<char, 12> digits{};
    const std::string_view value = fieldText(m_field, digits);
    std::string_view title = text::get("MAIL_LABEL");
    std::string_view label = text::get("NAME");
    const u32 i = static_cast<u32>(m_field);
    switch (m_field) {
    case Field::MailTo: label = text::get("MAIL_TO_LABEL"); break;
    case Field::MailSubject: label = text::get("MAIL_SUBJECT_LABEL"); break;
    case Field::MailBody: label = text::get("VD_EXCHANGE_MESSAGE"); break;
    case Field::AuctionName: title = text::get("AUCTIONS"); break;
    case Field::AuctionMin:
    case Field::AuctionMax:
        title = text::get("AUCTIONS");
        label = text::get("LEVEL_RANGE");
        break;
    default: {
        static constexpr const char* kParts[3] = {"GOLD", "SILVER", "COPPER"};
        label = text::get(kParts[(i - static_cast<u32>(Field::MailGold)) % 3]);
        title = moneyField(i, static_cast<u32>(Field::TradeGold)) ? text::get("TRADE")
                : i >= static_cast<u32>(Field::BidGold)           ? text::get("AUCTIONS")
                                                                  : text::get("MAIL_LABEL");
        break;
    }
    }
    DialogContext modal = c; // parity pass (panels): over the screen, not in the window's place
    modal.panel = {};
    if (compactEditor(modal, title, label, value, m_field == Field::MailBody)) {
        endEdit(text);
    }
    return true;
}

// -------------------------------------------------------------------------------------------------------
// The HUD and the windows.

void ExchangeScreens::drawHud(GameSession& session, DialogContext& c) {
    if (session.play().exchange().mail().hasNewMail() && mailIndicator(c)) {
        line(false, kInfoColor, "HAVE_MAIL");
    }
}

bool ExchangeScreens::windowOpen(const GameSession& session) const {
    const game::Exchange& ex = session.play().exchange();
    // Parity (popups): a trade request either way is a StaticPopup (TRADE), not the window.
    return ex.trade().phase() == game::TradePhase::Open || ex.auction().open() || ex.mail().open();
}

ExchangeScreens::Window ExchangeScreens::window(const GameSession& session) const {
    const game::Exchange& ex = session.play().exchange();
    return ex.trade().phase() == game::TradePhase::Open ? Window::Trade
           : ex.auction().open()                         ? Window::Auction
           : ex.mail().open()                            ? Window::Mail
                                                         : Window::None;
}

void ExchangeScreens::closeWindow(GameSession& session, Window window) {
    game::Exchange& ex = session.play().exchange();
    switch (window) {
    case Window::Trade: ex.trade().cancel(); break;
    case Window::Auction: ex.auction().close(); break;
    case Window::Mail: ex.mail().close(); break;
    default: break;
    }
}

void ExchangeScreens::drawWindow(GameSession& session, DialogContext& c, ui::TextInput& text,
                                 InventoryScreens& items, u32 nowMs, Window which) {
    VD_PROFILE_ZONE("ExchangeScreens::window");
    m_arenaUsed = 0;
    ItemDrag& drag = items.hostWindow(c);
    game::Exchange& ex = session.play().exchange();
    sync(text);
    const bool any = which == Window::None;
    // Parity (popups): the trade request and the wait for its answer are StaticPopups (app/PopupScreens).
    if (ex.trade().phase() == game::TradePhase::Open && (any || which == Window::Trade)) {
        tradeWindow(session, c, text, items, drag);
    } else if (ex.auction().open() && (any || which == Window::Auction)) {
        auctionWindow(session, c, text, items, drag, nowMs);
    } else if (ex.mail().open() && (any || which == Window::Mail)) {
        mailWindow(session, c, text, items, drag, nowMs);
    }
}

void ExchangeScreens::applyItems(GameSession& session, InventoryScreens& items, const ItemWindowResult& r,
                                 std::span<const ItemRef> windowCells, std::span<const u32> windowEntries) {
    (void)session;
    if (!r.tapped.valid()) {
        return;
    }
    if (m_targeting && m_targeting->pickingItems(session)) { // phase 13 (spells): the waiting spell's item
        if (r.tapped.kind == ItemRef::Kind::Slot) {
            m_targeting->pickItem(session, {r.tapped.bag, r.tapped.slot});
            return;
        }
        if (r.tapped.kind == ItemRef::Kind::Window && r.tapped.index == 100 + net::kTradeSlotNonTraded) {
            m_targeting->pickTradeItem(session);
            return;
        }
    }
    if (r.tapped.kind == ItemRef::Kind::Slot) {
        items.openItemTooltip(r.tapped, r.tappedRect); // with its actions (Attach, Auction, Trade...)
        return;
    }
    for (u32 i = 0; i < windowCells.size() && i < windowEntries.size(); ++i) {
        if (windowCells[i] == r.tapped && windowEntries[i] != 0) {
            items.showItemTooltip(windowEntries[i], r.tappedRect);
        }
    }
}

// --- Trade
// ---------------------------------------------------------------------------------------------------

void ExchangeScreens::tradeWindow(GameSession& session, DialogContext& c, ui::TextInput& text,
                                  InventoryScreens& items, ItemDrag& drag) {
    game::Gameplay& play = session.play();
    game::Trade& trade = play.exchange().trade();
    const std::string_view partner = playerName(session, trade.partner());
    // Parity (popups): the request and the wait for its answer are popups (TRADE: app/PopupScreens).
    // The money typed goes once its keyboard closed (each change withdraws both acceptances on the server).
    const bool typingMoney = m_field >= Field::TradeGold && m_field <= Field::TradeCopper;
    if (m_tradeMoneyTyped && !typingMoney) {
        m_tradeMoneyTyped = false;
        trade.setMoney(std::min(m_tradeMoney, play.inventory().money()));
    }
    if (drawEditor(c, text)) {
        return;
    }
    game::Inventory& inv = play.inventory();
    TradeView view;
    const game::TradeSide& mine = trade.mine();
    const game::TradeSide& theirs = trade.theirs();
    std::array<ItemRef, 14> cells{};
    std::array<u32, 14> entries{};
    for (u32 i = 0; i < net::kTradeSlotCount; ++i) {
        const game::ItemInstance* item = mine.items[i] ? inv.find(mine.items[i]) : nullptr;
        ItemSlotView& cell = view.mine.slots[i];
        cell = item ? itemView(session, item->entry, item->count) : ItemSlotView{};
        cell.ref = {ItemRef::Kind::Window, 0, 0, i};
        view.mine.names[i] = item ? itemName(session, item->entry) : std::string_view{};
        cells[i] = cell.ref;
        entries[i] = item ? item->entry : 0;
        const net::TradeSlotInfo& s = theirs.slots[i];
        ItemSlotView& other = view.theirs.slots[i];
        other = s.empty() ? ItemSlotView{} : itemView(session, s.itemId, s.count);
        if (!s.empty() && other.iconHash == 0 && m_data) {
            other.iconHash = m_data->itemIconHash(s.displayId); // known before the template arrives
        }
        // TradeFrame_UpdateTargetItem: what the player could not use, its icon and name frame 0.9, 0, 0.
        if (const net::ItemQueryInfo* info = s.empty() ? nullptr : session.item(s.itemId);
            info && !canUse(session, *info)) {
            other.tint = ui::rgba(230, 0, 0);
        }
        other.ref = {ItemRef::Kind::Window, 0, 0, 100 + i};
        // Phase 13 (spells): the trader's item that will not be traded is what a waiting enchantment may take.
        if (m_targeting && m_targeting->pickingItems(session) && !s.empty()) {
            const bool slotForSpells = i == net::kTradeSlotNonTraded && play.targeting().aim() == game::SpellAim::Item;
            other.lit = slotForSpells;
            other.dimmed = !slotForSpells;
        }
        view.theirs.names[i] = s.empty() ? std::string_view{} : itemName(session, s.itemId);
        cells[7 + i] = other.ref;
        entries[7 + i] = s.itemId;
    }
    const bool editingMoney = m_field >= Field::TradeGold && m_field <= Field::TradeCopper;
    view.mine.money = editingMoney ? m_tradeMoney : mine.money;
    view.mine.accepted = mine.accepted;
    view.theirs.name = partner;
    view.theirs.money = theirs.money;
    view.theirs.accepted = theirs.accepted;
    view.editing = editingMoney ? static_cast<i32>(m_field) - static_cast<i32>(Field::TradeGold) : -1;
    view.playerMoney = inv.money();
    items.bags(session, 0, view.bags);
    // The items offered are dimmed in the bags.
    m_bagCells.assign(view.bags.cells.begin(), view.bags.cells.end());
    for (ItemSlotView& cell : m_bagCells) {
        const game::ItemInstance* item =
            cell.ref.kind == ItemRef::Kind::Slot ? inv.at({cell.ref.bag, cell.ref.slot}) : nullptr;
        cell.dimmed = cell.dimmed || (item && trade.slotOf(item->guid) >= 0);
    }
    view.bags.cells = m_bagCells;

    const TradeResult r = vd::tradeWindow(c, view, drag);
    if (r.field >= 0) {
        if (!editingMoney) {
            m_tradeMoney = mine.money;
        }
        beginEdit(text, static_cast<Field>(static_cast<u32>(Field::TradeGold) + static_cast<u32>(r.field)));
    }
    switch (r.button) {
    case kTradeAccept: trade.accept(); break;
    case kTradeUnaccept: trade.unaccept(); break;
    case kTradeCancel: trade.cancel(); break;
    default: break;
    }
    if (r.closed) {
        trade.cancel();
    }
    applyItems(session, items, r.items, cells, entries);
    // Drops: bags <-> the player's trade slots (and between them).
    const ItemWindowResult& d = r.items;
    if (d.dropFrom.valid()) {
        using Kind = game::ItemPlace::Kind;
        const auto placeOf = [](const ItemRef& ref) {
            if (ref.kind == ItemRef::Kind::Slot) {
                return game::ItemPlace::slot({ref.bag, ref.slot});
            }
            if (ref.kind == ItemRef::Kind::Window && ref.index < net::kTradeSlotCount) {
                return game::ItemPlace::of(Kind::Trade, ref.index);
            }
            return game::ItemPlace{};
        };
        game::ItemPlace to = placeOf(d.dropOn);
        if (!d.dropOn.valid()) {
            to = d.dropZone == 0 ? game::ItemPlace::of(Kind::Bags)
                 : d.dropZone >= 1 && d.dropZone <= 7
                     ? game::ItemPlace::of(Kind::Trade, static_cast<u32>(d.dropZone - 1))
                     : game::ItemPlace{};
        }
        game::dropItem(play, placeOf(d.dropFrom), to);
    }
}

// --- Mail
// ----------------------------------------------------------------------------------------------------

std::string_view ExchangeScreens::mailSender(GameSession& session, const game::MailMessage& m) {
    switch (m.type) {
    case net::kMailNormal: return playerName(session, m.sender);
    case net::kMailAuction: {
        const std::string_view house =
            m_exchange ? m_exchange->house(m.senderEntry).name : std::string_view{};
        return house.empty() ? text::get("VD_EXCHANGE_AUCTION_HOUSE") : house;
    }
    case net::kMailCreature: {
        const net::CreatureQueryInfo* info = session.creature(m.senderEntry);
        return info ? std::string_view(info->name) : std::string_view("...");
    }
    case net::kMailGameObject: {
        const game::GameObjectInfo* info = session.gameObject(m.senderEntry);
        return info ? std::string_view(info->name) : std::string_view("...");
    }
    default: return text::get("UNKNOWN");
    }
}

std::string_view ExchangeScreens::mailSubject(GameSession& session, const game::MailMessage& m) {
    std::string_view subject = m.subject;
    if (subject.empty() && m.templateId != 0 && m_exchange) {
        subject = m_exchange->mailTemplateSubject(m.templateId);
    }
    // The auction house's letters: "<item>:0:<what happened>".
    if (m.type == net::kMailAuction) {
        u32 item = 0, kind = 0;
        char raw[96];
        if (std::sscanf(terminated(subject, raw, sizeof(raw)), "%u:%*u:%u", &item, &kind) == 2) {
            static constexpr const char* kKeys[] = {
                "AUCTION_OUTBID_MAIL_SUBJECT",  "AUCTION_WON_MAIL_SUBJECT",
                "AUCTION_SOLD_MAIL_SUBJECT",    "AUCTION_EXPIRED_MAIL_SUBJECT",
                "AUCTION_REMOVED_MAIL_SUBJECT", "AUCTION_REMOVED_MAIL_SUBJECT",
                "VD_EXCHANGE_MAIL_SALE_PENDING"};
            const std::string_view name = itemName(session, item);
            if (kind < std::size(kKeys)) {
                return arena(kKeys[kind], name);
            }
        }
    }
    if (m.flags & net::kMailFlagCodPayment) {
        return arena("COD_PAYMENT", subject);
    }
    return subject;
}

std::string_view ExchangeScreens::mailBody(GameSession& session, const game::MailMessage& m,
                                           MailInvoiceView& invoice) {
    game::Mail& mail = session.play().exchange().mail();
    const std::string_view body = mail.body(m.id);
    invoice = {};
    if (m.type != net::kMailAuction || body.empty()) {
        return body;
    }
    // The auction house's invoices: "<player hex>:<bid>:<buyout>[:<deposit>:<cut>]". Retouches (windows): as
    // GetInboxInvoiceInfo gives them to OpenMail_Update (its OpenMailInvoiceFrame, with money frames): the
    // seller's (five numbers: the buyer, the bid, the buyout, the deposit, the cut) or the buyer's (three:
    // the seller...); the item from the subject ("<item>:0:<what happened>").
    char who[24] = {};
    u32 bid = 0, buyout = 0, deposit = 0, cut = 0;
    char raw[128];
    const i32 n = std::sscanf(terminated(body, raw, sizeof(raw)), "%16[0-9a-fA-F ]:%u:%u:%u:%u", who, &bid,
                              &buyout, &deposit, &cut);
    if (n < 3) {
        return body;
    }
    u32 item = 0;
    char subject[96];
    std::sscanf(terminated(m.subject, subject, sizeof(subject)), "%u", &item);
    invoice.shown = true;
    invoice.buyer = n < 5;
    invoice.item = itemName(session, item);
    invoice.player = playerName(session, std::strtoull(who, nullptr, 16)); // a player's low GUID
    invoice.buyout = bid == buyout && buyout != 0;
    invoice.bid = bid;
    invoice.deposit = n >= 5 ? deposit : 0;
    invoice.cut = n >= 5 ? cut : 0;
    return {};
}

void ExchangeScreens::mailWindow(GameSession& session, DialogContext& c, ui::TextInput& text,
                                 InventoryScreens& items, ItemDrag& drag, u32 nowMs) {
    game::Gameplay& play = session.play();
    game::Mail& mail = play.exchange().mail();
    game::Inventory& inv = play.inventory();
    const game::MailMessage* selected = m_selectedMail ? mail.find(m_selectedMail) : nullptr;
    if (!selected) {
        m_selectedMail = 0;
    }
    if (drawEditor(c, text)) {
        return;
    }
    MailView view;
    view.tab = m_mailTab;
    view.loaded = mail.loaded();
    m_mailRows.clear();
    m_mailIds.clear();
    for (const game::MailMessage& m : mail.messages()) {
        MailRowView row;
        row.sender = mailSender(session, m);
        row.subject = mailSubject(session, m);
        const f32 days = m.daysLeft;
        row.expires = days >= 1.0f ? arena("DAY_ONELETTER_ABBR", static_cast<int>(days))
                      : days >= 0.042f
                          ? arena("HOUR_ONELETTER_ABBR", static_cast<int>(days * 24.0f))
                          : arena("MINUTE_ONELETTER_ABBR", std::max(1, static_cast<int>(days * 1440.0f)));
        if (m.hasItem()) {
            const ItemSlotView item = itemView(session, m.item.entry, m.item.count);
            row.iconHash = item.iconHash;
            row.quality = item.quality;
            row.count = m.item.count;
        } else {
            row.iconHash = m.money > 0 ? kCoinIcon : kLetterIcon;
        }
        row.money = m.money;
        row.cod = m.cod;
        row.unread = !m.read();
        row.returned = (m.flags & net::kMailFlagReturned) != 0;
        if (m.id == m_selectedMail) {
            view.selected = static_cast<i32>(m_mailRows.size());
        }
        m_mailRows.push_back(row);
        m_mailIds.push_back(m.id);
    }
    view.rows = m_mailRows;
    std::array<ItemRef, 2> cells{ItemRef{ItemRef::Kind::Window, 0, 0, 0},
                                 ItemRef{ItemRef::Kind::Window, 0, 0, 1}};
    std::array<u32, 2> entries{};
    if (selected) {
        MailReadView& read = view.read;
        read.sender = mailSender(session, *selected);
        read.subject = mailSubject(session, *selected);
        read.body = mailBody(session, *selected, read.invoice);
        read.waitingBody = selected->itemTextId != 0 && read.body.empty() && !read.invoice.shown;
        if (selected->hasItem()) {
            read.item = itemView(session, selected->item.entry, selected->item.count);
            read.itemName = itemName(session, selected->item.entry);
            entries[0] = selected->item.entry;
        }
        read.item.ref = cells[0];
        read.money = selected->money;
        read.cod = selected->cod;
        read.canTakeMoney = selected->money > 0;
        read.canTakeItem = selected->hasItem();
        read.canReply = selected->fromPlayer();
        // Phase 18 (p3): one button, Delete or Return (OpenMail_Update: InboxItemCanDelete).
        read.canDelete = selected->canDelete() && selected->cod == 0;
        read.canReturn = !read.canDelete && selected->canReturn();
        read.canCopy = selected->canCopyText();
    }
    MailSendView& send = view.send;
    send.to = m_to;
    send.subject = m_subject;
    send.body = m_body;
    const i32 field = static_cast<i32>(m_field);
    send.editing = m_field == Field::MailTo        ? 0
                   : m_field == Field::MailSubject ? 1
                   : m_field == Field::MailBody    ? 2
                   : moneyField(static_cast<u32>(field), static_cast<u32>(Field::MailGold))
                       ? 3 + field - static_cast<i32>(Field::MailGold)
                       : -1;
    const u64 attachment = mail.attachment();
    send.attachment = bagItemView(session, attachment);
    send.attachment.ref = cells[1];
    if (const game::ItemInstance* item = attachment ? inv.find(attachment) : nullptr) {
        send.attachmentName = itemName(session, item->entry);
        entries[1] = item->entry;
    }
    send.cod = m_cod;
    send.money = m_mailMoney;
    game::MailDraftView draft;
    draft.to = m_to;
    draft.subject = m_subject;
    draft.body = m_body;
    draft.item = attachment;
    draft.money = m_cod ? 0 : m_mailMoney;
    draft.cod = m_cod ? m_mailMoney : 0;
    send.cost = net::kMailPostage; // the money sent shows in its own field
    send.playerMoney = inv.money();
    send.problem = mail.sendProblem(draft);
    send.sending = mail.sending();
    if (m_mailTab == 1) {
        items.bags(session, nowMs, send.bags);
        m_bagCells.assign(send.bags.cells.begin(), send.bags.cells.end());
        for (ItemSlotView& cell : m_bagCells) {
            const game::ItemInstance* item =
                cell.ref.kind == ItemRef::Kind::Slot ? inv.at({cell.ref.bag, cell.ref.slot}) : nullptr;
            cell.dimmed = cell.dimmed || (item && item->guid == attachment);
        }
        send.bags.cells = m_bagCells;
    }

    const MailResult r = vd::mailWindow(c, view, drag);
    if (r.tab >= 0 && static_cast<u32>(r.tab) != m_mailTab) {
        m_mailTab = static_cast<u32>(r.tab);
        endEdit(text);
    }
    if (r.row >= 0 && static_cast<u32>(r.row) < m_mailIds.size()) {
        m_selectedMail = m_mailIds[static_cast<u32>(r.row)];
        mail.markRead(m_selectedMail); // reading it (unread ones only)
    }
    if (r.field >= 0) {
        const Field f =
            r.field == 0   ? Field::MailTo
            : r.field == 1 ? Field::MailSubject
            : r.field == 2
                ? Field::MailBody
                : static_cast<Field>(static_cast<u32>(Field::MailGold) + static_cast<u32>(r.field - 3));
        beginEdit(text, f);
    }
    switch (r.button) {
    case kMailTakeMoney:
        if (selected) {
            mail.takeMoney(selected->id);
        }
        break;
    case kMailTakeItem:
        if (selected && selected->cod > 0 && m_popups) {
            // Retouches (windows): OpenMailPackageButton: COD_ALERT, or COD_CONFIRMATION with its money
            // frame.
            m_popups->askCod(session, selected->id, selected->cod);
        } else if (selected) {
            mail.takeItem(selected->id);
        }
        break;
    case kMailReply:
        if (selected) {
            m_to.assign(mailSender(session, *selected));
            m_subject.assign(text::get("MAIL_REPLY_PREFIX"));
            m_subject.append(" ");
            m_subject.append(mailSubject(session, *selected).substr(0, net::kMailMaxSubject - 4));
            m_body.clear();
            m_mailTab = 1;
        }
        break;
    case kMailReturn:
        if (selected) {
            mail.returnToSender(selected->id);
        }
        break;
    case kMailDelete:
        // Phase 18 (p3): OpenMail_Delete: an item goes with it (DELETE_MAIL), or money (DELETE_MONEY), or at once.
        if (selected && selected->hasItem() && m_popups) {
            m_popups->askDeleteMail(session, selected->id, itemName(session, selected->item.entry));
        } else if (selected && selected->money > 0 && m_popups) {
            m_popups->askDeleteMoney(session, selected->id, selected->money);
        } else if (selected) {
            mail.remove(selected->id);
        }
        break;
    case kMailCopy:
        if (selected) {
            mail.copyText(selected->id);
        }
        break;
    case kMailSend:
        endEdit(text);
        mail.send(draft);
        break;
    case kMailToggleCod: m_cod = !m_cod; break;
    default: break;
    }
    if (r.closed) {
        endEdit(text);
        mail.close();
    }
    applyItems(session, items, r.items, cells, entries);
    const ItemWindowResult& d = r.items;
    if (d.dropFrom.valid()) {
        using Kind = game::ItemPlace::Kind;
        const auto placeOf = [&](const ItemRef& ref) {
            if (ref.kind == ItemRef::Kind::Slot) {
                return game::ItemPlace::slot({ref.bag, ref.slot});
            }
            return ref == cells[1] ? game::ItemPlace::of(Kind::MailAttachment) : game::ItemPlace{};
        };
        game::ItemPlace to = placeOf(d.dropOn);
        if (!d.dropOn.valid()) {
            to = d.dropZone == 0   ? game::ItemPlace::of(Kind::Bags)
                 : d.dropZone == 1 ? game::ItemPlace::of(Kind::MailAttachment)
                                   : game::ItemPlace{};
        }
        game::dropItem(play, placeOf(d.dropFrom), to);
    }
}

// --- Auction house
// -------------------------------------------------------------------------------------------

void ExchangeScreens::search(GameSession& session, u32 page) {
    const std::span<const game::AuctionCategory> categories = game::auctionCategories();
    if (m_category > 0 && m_category <= categories.size()) {
        m_search.itemClass = categories[m_category - 1].itemClass;
        m_search.subclass = categories[m_category - 1].subclass;
    } else {
        m_search.itemClass = m_search.subclass = net::kAuctionAny;
    }
    m_search.quality = m_quality == 0 ? net::kAuctionAny : m_quality - 1;
    m_selectedAuction = 0;
    session.play().exchange().auction().search(m_search, page * net::kAuctionPageSize);
}

void ExchangeScreens::auctionWindow(GameSession& session, DialogContext& c, ui::TextInput& text,
                                    InventoryScreens& items, ItemDrag& drag, u32 nowMs) {
    game::Gameplay& play = session.play();
    game::AuctionHouse& ah = play.exchange().auction();
    game::Inventory& inv = play.inventory();
    // The pickers over the window.
    if (m_picker != Picker::None) {
        m_choices.clear();
        if (m_picker == Picker::Category) {
            for (u32 i = 0; i < m_categoryLabels.size(); ++i) {
                m_choices.push_back({categoryLabel(i),
                                     i == 0 || m_categoryLabels[i][0] != ' ' ? ui::kGold : ui::kWhite,
                                     i == m_category, true});
            }
        } else {
            m_choices.push_back({text::get("ALL"), ui::kWhite, m_quality == 0, true});
            for (u32 q = 0; q < std::size(kQualityKeys); ++q) {
                m_choices.push_back(
                    {text::get(kQualityKeys[q]), ui::qualityColor(q), m_quality == q + 1, true});
            }
        }
        const i32 picked = choiceList(c, "auction.picker",
                                      m_picker == Picker::Category ? text::get("VD_EXCHANGE_CATEGORY")
                                                                   : text::get("QUALITY"),
                                      m_choices);
        if (picked >= 0) {
            (m_picker == Picker::Category ? m_category : m_quality) = static_cast<u32>(picked);
        }
        if (picked != -1) {
            m_picker = Picker::None;
        }
        return;
    }
    if (drawEditor(c, text)) {
        return;
    }
    const AuctionList listKind = m_auctionTab == 0   ? AuctionList::Browse
                                 : m_auctionTab == 1 ? AuctionList::Bids
                                                     : AuctionList::Owned;
    const game::AuctionPage& page = ah.page(listKind);
    AuctionView view;
    const game::ExchangeData::House house =
        m_exchange ? m_exchange->house(ah.houseId()) : game::ExchangeData::House{};
    view.title = house.name.empty() ? text::get("AUCTIONS") : house.name;
    view.tab = m_auctionTab;
    view.filter.name = m_search.name;
    view.filter.category = categoryLabel(m_category);
    while (!view.filter.category.empty() && view.filter.category.front() == ' ') {
        view.filter.category.remove_prefix(1);
    }
    view.filter.quality =
        m_quality == 0 ? text::get("ALL") : text::get(kQualityKeys[std::min<u32>(m_quality - 1, 4)]);
    view.filter.levelMin = m_search.levelMin;
    view.filter.levelMax = m_search.levelMax;
    view.filter.usable = m_search.usable;
    // The rows.
    m_auctionRows.clear();
    m_auctionIds.clear();
    std::array<ItemRef, net::kAuctionPageSize + 1> cells{};
    std::array<u32, net::kAuctionPageSize + 1> entries{};
    const u64 me = session.playerGuid();
    const game::Entity* player = session.entities().find(me); // phase 18 (extras)
    const u32 playerLevel = player ? player->value(uf::UNIT_FIELD_LEVEL) : 0;
    const net::AuctionItem* selected = nullptr;
    // Phase 18 (p3): the page in SortAuctionItems' order (WoW.exe sorts it once its templates are known: here again
    // whenever more of them, or of the sellers' names, are).
    const u32 listIndex = static_cast<u32>(listKind);
    {
        const game::QueryCache& queries = session.play().queries();
        u32 unknown = 0;
        for (const net::AuctionItem& a : page.items) {
            unknown += queries.findItem(a.itemId) ? 0u : 1u;
            unknown += a.owner == 0 || a.owner == me || queries.findPlayerName(a.owner) ? 0u : 1u;
        }
        struct Lookup {
            const game::QueryCache* queries;
            u64 me;
            const std::string* myName; // the player's own: never queried (game::AuctionHouse)
        };
        m_auctionMyName.assign(session.characterName());
        const Lookup lookup{&queries, me, &m_auctionMyName};
        game::AuctionSortKeys& keys = m_auctionSort[listIndex];
        if (page.version != m_auctionSortedPage[listIndex] || keys.version() != m_auctionSortedKeys[listIndex] ||
            unknown != m_auctionSortedUnknown[listIndex]) {
            m_auctionSortedPage[listIndex] = page.version;
            m_auctionSortedKeys[listIndex] = keys.version();
            m_auctionSortedUnknown[listIndex] = unknown;
            game::AuctionSortData data;
            data.me = me;
            data.user = &lookup;
            data.item = [](const void* user, u32 entry) {
                return static_cast<const Lookup*>(user)->queries->findItem(entry);
            };
            data.playerName = [](const void* user, u64 guid) -> const std::string* {
                const Lookup* l = static_cast<const Lookup*>(user);
                if (guid == l->me && !l->myName->empty()) {
                    return l->myName;
                }
                return l->queries->findPlayerName(guid);
            };
            game::sortAuctions(page.items, keys, data, m_auctionOrder[listIndex]);
        }
        // The headers (the browse list's seller column shows the seller key's arrow: 1.12's SortButton_UpdateArrow
        // reads "status" there).
        const std::span<const AuctionHeaderSpec> headers = auctionHeaderSpecs(m_auctionTab);
        for (u32 i = 0; i < headers.size(); ++i) {
            m_auctionHeaders[i] = {text::get(headers[i].key), headers[i].width, true,
                                   keys.reversedOf(headers[i].sort)};
            m_auctionColumns[i] = headers[i].column;
        }
        view.headers = {m_auctionHeaders.data(), headers.size()};
        view.columns = {m_auctionColumns.data(), headers.size()};
    }
    for (const u16 index : m_auctionOrder[listIndex]) {
        if (m_auctionRows.size() >= net::kAuctionPageSize) {
            break;
        }
        if (index >= page.items.size()) {
            continue;
        }
        const net::AuctionItem& a = page.items[index];
        AuctionRowView row;
        const u32 i = static_cast<u32>(m_auctionRows.size());
        row.cell = itemView(session, a.itemId, a.count);
        row.cell.ref = {ItemRef::Kind::Window, 0, 0, i};
        cells[i] = row.cell.ref;
        entries[i] = a.itemId;
        row.name = itemName(session, a.itemId);
        if (const net::ItemQueryInfo* info = session.item(a.itemId)) {
            row.level = info->requiredLevel;
            row.cell.usable = canUse(session, *info); // phase 18 (extras)
        }
        // Phase 18 (extras): Blizzard_AuctionUI's red level above the player's, and the player's auctions without a
        // bid showing their starting price at half alpha (AuctionFrameAuctions_Update).
        row.levelTooHigh = row.level > playerLevel;
        row.startingPrice = listKind == AuctionList::Owned && a.bid == 0;
        row.timeLeft = timeLeftText(ah.timeLeftMs(page, a));
        row.hasBid = a.bid != 0;
        row.bid = a.bid != 0 ? a.bid : a.startBid;
        row.buyout = a.buyout;
        row.leading = a.bidder == me && a.bid != 0;
        row.outbid = listKind == AuctionList::Bids && a.bidder != me;
        if (listKind == AuctionList::Browse) {
            row.seller = a.owner == me ? text::get("YOU") : playerName(session, a.owner);
        } else if (listKind == AuctionList::Bids) {
            row.seller = row.leading ? text::get("HIGH_BIDDER") : text::get("OUTBID");
        } else {
            row.seller = a.bidder != 0 ? playerName(session, a.bidder) : text::get("NO_BIDS");
        }
        if (a.id == m_selectedAuction) {
            view.selected = static_cast<i32>(i);
            selected = &a;
        }
        m_auctionRows.push_back(row);
        m_auctionIds.push_back(a.id);
    }
    if (!selected) {
        m_selectedAuction = 0;
    }
    view.rows = m_auctionRows;
    view.page = page.from / net::kAuctionPageSize;
    view.pages = page.pages();
    view.total = page.total;
    view.pending = page.pending;
    view.empty = listKind == AuctionList::Browse
                     ? (page.loaded ? text::get("BROWSE_NO_RESULTS") : text::get("BROWSE_SEARCH_TEXT"))
                 : listKind == AuctionList::Bids ? text::get("VD_EXCHANGE_NO_BIDS_YET")
                                                 : text::get("VD_EXCHANGE_NO_AUCTIONS");
    const bool editingBid = m_field >= Field::BidGold && m_field <= Field::BidCopper;
    if (selected && !editingBid && m_bidAmount < net::auctionMinimumBid(*selected)) {
        m_bidAmount = net::auctionMinimumBid(*selected);
    }
    view.bidAmount = m_bidAmount;
    const bool notMine = selected && selected->owner != me;
    view.canBid = notMine && listKind != AuctionList::Owned &&
                  m_bidAmount >= net::auctionMinimumBid(*selected) && m_bidAmount <= inv.money() &&
                  !(selected->bidder == me && selected->bid != 0 && m_bidAmount <= selected->bid);
    view.canBuyout =
        notMine && listKind != AuctionList::Owned && selected->buyout > 0 && selected->buyout <= inv.money();
    view.canCancel = selected && listKind == AuctionList::Owned;
    const u32 f = static_cast<u32>(m_field);
    view.editing = m_field == Field::AuctionName  ? 0
                   : m_field == Field::AuctionMin ? 1
                   : m_field == Field::AuctionMax ? 2
                   : moneyField(f, static_cast<u32>(Field::BidGold))
                       ? 3 + static_cast<i32>(f - static_cast<u32>(Field::BidGold))
                   : moneyField(f, static_cast<u32>(Field::StartGold))
                       ? 6 + static_cast<i32>(f - static_cast<u32>(Field::StartGold))
                   : moneyField(f, static_cast<u32>(Field::BuyoutGold))
                       ? 9 + static_cast<i32>(f - static_cast<u32>(Field::BuyoutGold))
                       : -1;
    // The sale.
    const u64 staged = ah.staged();
    if (staged != m_stagedShown) {
        m_stagedShown = staged;
        // A starting suggestion: what a vendor pays for it (the player changes it).
        const game::ItemInstance* item = staged ? inv.find(staged) : nullptr;
        const net::ItemQueryInfo* info = item ? session.item(item->entry) : nullptr;
        if (info && m_startBid == 0) {
            m_startBid = std::max<u32>(1, info->sellPrice * item->count);
        }
    }
    AuctionSellView& sell = view.sell;
    sell.cell = bagItemView(session, staged);
    sell.cell.ref = {ItemRef::Kind::Window, 0, 0, net::kAuctionPageSize};
    cells[net::kAuctionPageSize] = sell.cell.ref;
    if (const game::ItemInstance* item = staged ? inv.find(staged) : nullptr) {
        sell.itemName = itemName(session, item->entry);
        entries[net::kAuctionPageSize] = item->entry;
    }
    sell.startBid = m_startBid;
    sell.buyout = m_buyout;
    sell.duration = m_duration;
    sell.deposit = staged ? ah.deposit(staged, m_duration) : 0;
    sell.problem = staged ? ah.sellProblem(staged, m_startBid, m_buyout) : std::string_view{};
    if (sell.problem.empty() && staged && sell.deposit > inv.money()) {
        sell.problem = text::get("ERR_NOT_ENOUGH_MONEY");
    }
    if (!staged) {
        sell.problem = " "; // nothing to sell: "Create Auction" stays disabled without a message
    }
    view.money = inv.money();
    view.busy = ah.busy();
    if (m_auctionTab == 2) {
        items.bags(session, nowMs, view.bags);
        m_bagCells.assign(view.bags.cells.begin(), view.bags.cells.end());
        for (ItemSlotView& cell : m_bagCells) {
            const game::ItemInstance* item =
                cell.ref.kind == ItemRef::Kind::Slot ? inv.at({cell.ref.bag, cell.ref.slot}) : nullptr;
            cell.dimmed = cell.dimmed || (item && item->guid == staged);
        }
        view.bags.cells = m_bagCells;
    }

    const AuctionResult r = vd::auctionWindow(c, view, drag);
    if (r.header >= 0) { // phase 18 (p3): SortAuctionItems(type, key), the header's
        const std::span<const AuctionHeaderSpec> headers = auctionHeaderSpecs(m_auctionTab);
        if (static_cast<u32>(r.header) < headers.size()) {
            m_auctionSort[listIndex].sort(headers[static_cast<u32>(r.header)].sort);
        }
    }
    if (r.tab >= 0 && static_cast<u32>(r.tab) != m_auctionTab) {
        m_auctionTab = static_cast<u32>(r.tab);
        m_selectedAuction = 0;
        endEdit(text);
        if (m_auctionTab == 1) {
            ah.listBids();
        } else if (m_auctionTab == 2) {
            ah.listOwned();
        }
    }
    if (r.row >= 0 && static_cast<u32>(r.row) < m_auctionIds.size()) {
        m_selectedAuction = m_auctionIds[static_cast<u32>(r.row)];
        if (const net::AuctionItem* a = ah.find(m_selectedAuction)) {
            m_bidAmount = net::auctionMinimumBid(*a);
        }
    }
    if (r.field >= 0) {
        const Field fields[] = {Field::AuctionName, Field::AuctionMin,   Field::AuctionMax,
                                Field::BidGold,     Field::BidSilver,    Field::BidCopper,
                                Field::StartGold,   Field::StartSilver,  Field::StartCopper,
                                Field::BuyoutGold,  Field::BuyoutSilver, Field::BuyoutCopper};
        if (static_cast<u32>(r.field) < std::size(fields)) {
            beginEdit(text, fields[r.field]);
        }
    }
    if (r.duration >= 0) {
        m_duration = static_cast<u32>(r.duration);
    }
    switch (r.button) {
    case kAuctionSearch:
        endEdit(text);
        search(session, 0);
        break;
    case kAuctionPrevious:
        if (view.page > 0) {
            search(session, view.page - 1);
        }
        break;
    case kAuctionNext: search(session, view.page + 1); break;
    case kAuctionBid:
        endEdit(text);
        if (selected) {
            ah.bid(selected->id, m_bidAmount);
        }
        break;
    case kAuctionBuyout:
        if (selected) {
            ah.buyout(selected->id);
        }
        break;
    case kAuctionCancel:
        if (selected) {
            ah.cancel(selected->id);
        }
        break;
    case kAuctionCreate:
        endEdit(text);
        if (staged && ah.sell(staged, m_startBid, m_buyout, m_duration)) {
            m_startBid = m_buyout = 0;
        }
        break;
    case kAuctionCategory: m_picker = Picker::Category; break;
    case kAuctionQuality: m_picker = Picker::Quality; break;
    case kAuctionUsable: m_search.usable = !m_search.usable; break;
    default: break;
    }
    if (r.closed) {
        endEdit(text);
        ah.close();
    }
    applyItems(session, items, r.items, cells, entries);
    const ItemWindowResult& d = r.items;
    if (d.dropFrom.valid()) {
        using Kind = game::ItemPlace::Kind;
        const auto placeOf = [&](const ItemRef& ref) {
            if (ref.kind == ItemRef::Kind::Slot) {
                return game::ItemPlace::slot({ref.bag, ref.slot});
            }
            return ref == cells[net::kAuctionPageSize] ? game::ItemPlace::of(Kind::AuctionSale)
                                                       : game::ItemPlace{};
        };
        game::ItemPlace to = placeOf(d.dropOn);
        if (!d.dropOn.valid()) {
            to = d.dropZone == 0   ? game::ItemPlace::of(Kind::Bags)
                 : d.dropZone == 1 ? game::ItemPlace::of(Kind::AuctionSale)
                                   : game::ItemPlace{};
        }
        game::dropItem(play, placeOf(d.dropFrom), to);
    }
}

// -------------------------------------------------------------------------------------------------------
// Over the windows: rolls and the master looter's picker.

void ExchangeScreens::drawOverlay(GameSession& session, DialogContext& c, InventoryScreens& items,
                                  u32 nowMs) {
    VD_PROFILE_ZONE("ExchangeScreens::overlay");
    game::Gameplay& play = session.play();
    game::LootRolls& rolls = play.exchange().rolls();
    if (m_masterOpen && play.loot().open()) {
        // Phase 15 (items): GroupLootDropDown_Initialize. A party: its candidates; a raid: GIVE_LOOT, then a "Group n"
        // per raid group with candidates, its members one level down.
        const game::MasterLootSlots slots =
            game::masterLootSlots(rolls.masterCandidates(), play.social().group(), session.playerGuid());
        m_masterItems.clear();
        m_masterGuids.clear();
        m_masterItems2.clear();
        m_masterGuids2.clear();
        i32 level2Row = -1;
        if (slots.raid) {
            m_masterItems.push_back({text::get("GIVE_LOOT"), 0, true});
            m_masterGuids.push_back(0);
            for (u32 group = 0; group < 8; ++group) {
                if (!slots.groupHasCandidates(group)) {
                    continue;
                }
                const std::string_view label = text::get("GROUP");
                std::snprintf(m_groupLabels[group].data(), m_groupLabels[group].size(), "%.*s %u",
                              static_cast<int>(label.size()), label.data(), group + 1);
                if (m_masterGroup == static_cast<i32>(group)) {
                    level2Row = static_cast<i32>(m_masterItems.size());
                    for (u32 i = group * 5; i < group * 5 + 5; ++i) {
                        if (slots.guid[i] != 0) {
                            m_masterItems2.push_back({playerName(session, slots.guid[i])});
                            m_masterGuids2.push_back(slots.guid[i]);
                        }
                    }
                }
                DropDownItemView item{m_groupLabels[group].data()};
                item.hasArrow = true;
                m_masterItems.push_back(item);
                m_masterGuids.push_back(group); // its group (no player is guid 0..7)
            }
        } else {
            for (u32 i = 0; i < game::MasterLootSlots::kPartySlots; ++i) {
                if (slots.guid[i] != 0) {
                    m_masterItems.push_back({playerName(session, slots.guid[i])});
                    m_masterGuids.push_back(slots.guid[i]);
                }
            }
        }
        // ToggleDropDownMenu(1, nil, GroupLootDropDown, LootFrame.selectedLootButton, 0, 0): under the button.
        DropDownMenuView view;
        view.x = m_masterRow.x;
        view.y = m_masterRow.bottom();
        view.level1 = m_masterItems;
        view.level2 = m_masterItems2;
        view.level2Row = level2Row;
        const DropDownMenuResult r = dropDownMenu(c, "loot.master", view);
        if (r.toggled >= 0 && static_cast<u32>(r.toggled) < m_masterGuids.size()) {
            const i32 group = static_cast<i32>(m_masterGuids[static_cast<u32>(r.toggled)]);
            m_masterGroup = m_masterGroup == group ? -1 : group;
        }
        u64 target = 0;
        if (r.level == 1 && r.picked >= 0 && static_cast<u32>(r.picked) < m_masterGuids.size() && !slots.raid) {
            target = m_masterGuids[static_cast<u32>(r.picked)];
        } else if (r.level == 2 && r.picked >= 0 && static_cast<u32>(r.picked) < m_masterGuids2.size()) {
            target = m_masterGuids2[static_cast<u32>(r.picked)];
        }
        if (target != 0) {
            // GroupLootDropDown_GiveLoot: CONFIRM_LOOT_DISTRIBUTION for epics and better, else GiveMasterLoot.
            const game::LootSlot* item = play.loot().find(m_masterSlot);
            const net::ItemQueryInfo* info = item ? session.item(item->itemId) : nullptr;
            if (!m_popups || !info ||
                !m_popups->askLootDistribution(session, m_masterSlot, info->quality, info->name, target,
                                               playerName(session, target))) {
                rolls.giveMasterLoot(play.loot().guid(), m_masterSlot, target);
            }
            m_masterOpen = false; // CloseDropDownMenus
        }
        if (r.closed) {
            m_masterOpen = false;
        }
    }
    m_rollViews.clear();
    std::array<const game::PendingRoll*, game::LootRolls::kMaxRolls> shown{};
    for (const game::PendingRoll& roll : rolls.rolls()) {
        if (roll.voted) {
            continue;
        }
        RollView view;
        const ItemSlotView item = itemView(session, roll.itemId, 1);
        view.item.iconHash = item.iconHash;
        view.item.quality = item.quality;
        view.item.count = 1;
        view.item.name = itemName(session, roll.itemId);
        view.remainingMs = roll.remainingMs(nowMs);
        view.countdownMs = roll.countdownMs;
        shown[m_rollViews.size()] = &roll;
        m_rollViews.push_back(view);
    }
    const RollResult r = rollFrames(c, rollFramesTop(c), m_rollViews);
    if (r.roll >= 0 && static_cast<u32>(r.roll) < m_rollViews.size()) {
        const game::PendingRoll* roll = shown[static_cast<u32>(r.roll)];
        // Phase 15 (items): RollOnLoot asks CONFIRM_LOOT_ROLL first for Need / Greed on what binds when picked up.
        if (!m_popups || !m_popups->askLootRoll(session, roll->object, roll->slot, r.vote, roll->itemId)) {
            rolls.roll(roll->object, roll->slot, r.vote);
        }
    }
    if (r.tooltip >= 0 && static_cast<u32>(r.tooltip) < m_rollViews.size()) {
        items.showItemTooltip(shown[static_cast<u32>(r.tooltip)]->itemId, r.tooltipRect);
    }
}

bool ExchangeScreens::lootTapped(GameSession& session, u8 slot, const ui::Rect& row) {
    game::Gameplay& play = session.play();
    const game::LootSlot* item = play.loot().find(slot);
    if (!item) {
        return false;
    }
    switch (item->slotType) {
    case 1: // LOOT_SLOT_TYPE_ROLL_ONGOING
        line(false, kErrorColor, "ERR_LOOT_ROLL_PENDING");
        return true;
    case 2: { // LOOT_SLOT_TYPE_MASTER: the master looter hands it out
        const game::GroupState& group = play.social().group();
        if (group.looter == session.playerGuid() && !play.exchange().rolls().masterCandidates().empty()) {
            m_masterOpen = true;
            m_masterSlot = slot;
            m_masterRow = row; // phase 15 (items): LootFrame.selectedLootButton
            m_masterGroup = -1;
        } else {
            line(false, kErrorColor, "VD_EXCHANGE_MASTER_LOOTER_ONLY");
        }
        return true;
    }
    case 3: // LOOT_SLOT_TYPE_LOCKED
        line(false, kErrorColor, "ERR_LOOT_CANT_LOOT_THAT_NOW");
        return true;
    default: return false;
    }
}

std::string_view ExchangeScreens::lootNote(GameSession& session, u8 slotType) const {
    (void)session;
    switch (slotType) {
    case 1: return text::get("VD_EXCHANGE_LOOT_ROLLING");
    case 2: return text::get("LOOT_MASTER_LOOTER");
    default: return {};
    }
}

void ExchangeScreens::startTrade(GameSession& session, u64 player) {
    game::Trade& trade = session.play().exchange().trade();
    if (const std::string_view problem = trade.initiateProblem(player); !problem.empty()) {
        lineText(false, kErrorColor, problem);
        return;
    }
    trade.initiate(player);
}

} // namespace vd
