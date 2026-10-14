#include "app/ExchangeWindows.h"

#include "app/DialogKit.h"
#include "app/UiResources.h"
#include "app/WindowArt.h" // retouches (windows)
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

using kit::Dialog;
using kit::Metrics;
using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::WidgetId;
using ui::widgetId;

// "<label> <value>" into `out` (the client's labels carry their colon: "From:", "Subject:").
std::string_view labelled(std::span<char> out, std::string_view label, std::string_view value) {
    const i32 n = std::snprintf(out.data(), out.size(), "%.*s %.*s", static_cast<int>(label.size()),
                                label.data(), static_cast<int>(value.size()), value.data());
    return {out.data(), static_cast<usize>(std::clamp<i32>(n, 0, static_cast<i32>(out.size()) - 1))};
}

constexpr u64 kQuestTitleHighlight = assetPathHash("Interface\\QuestFrame\\UI-QuestTitleHighlight.blp");
constexpr const char* kRedFontColorCode = "|cffff2020"; // phase 18 (extras): RED_FONT_COLOR_CODE (Fonts.xml)
constexpr u64 kListboxHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight.blp");
constexpr u64 kItemNameFrame = assetPathHash("Interface\\QuestFrame\\UI-QuestItemNameFrame.blp");
constexpr u64 kMoneyIcons = assetPathHash("Interface\\MoneyFrame\\UI-MoneyIcons.blp");
constexpr u64 kDiceUp = assetPathHash("Interface\\Buttons\\UI-GroupLoot-Dice-Up.blp");
constexpr u64 kDiceDown = assetPathHash("Interface\\Buttons\\UI-GroupLoot-Dice-Down.blp");
constexpr u64 kCoinUp = assetPathHash("Interface\\Buttons\\UI-GroupLoot-Coin-Up.blp");
constexpr u64 kCoinDown = assetPathHash("Interface\\Buttons\\UI-GroupLoot-Coin-Down.blp");
constexpr u64 kPassUp = assetPathHash("Interface\\Buttons\\UI-GroupLoot-Pass-Up.blp");
constexpr u64 kPassDown = assetPathHash("Interface\\Buttons\\UI-GroupLoot-Pass-Down.blp");
constexpr u64 kLetterIcon = assetPathHash("Interface\\Icons\\INV_Letter_15.blp"); // MiniMapMailIcon
// Retouches (windows): MailItemTemplate's slot and checked state, OpenMailArithmeticLine
constexpr u64 kEmptySlotWhite = assetPathHash("Interface\\Buttons\\UI-EmptySlot-White.blp");
constexpr u64 kCheckedHighlight = assetPathHash("Interface\\Buttons\\CheckButtonHilight.blp");
constexpr u64 kInvoiceLine = assetPathHash("Interface\\MailFrame\\UI-MailFrame-InvoiceLine.blp");
constexpr u32 kWhiteText = rgba(255, 255, 255);
constexpr u32 kDimText = rgba(170, 170, 170);

// A row of tabs across `r` (MailFrameTab1..2, AuctionFrameTab1..3): retouches (windows):
// CharacterFrameTabButtonTemplate (app/WindowArt). Returns the one tapped (-1 none).
i32 tabBar(Gui& g, std::string_view name, const Rect& r, std::span<const std::string_view> tabs, u32 selected,
           f32 textSize) {
    return art::characterTabBar(g, name, r, tabs, selected, textSize, g.px(96.0f));
}

// Two panes: side by side on wide screens, stacked on square ones.
void splitPanes(const DialogContext& c, const Gui& g, const Rect& body, f32 firstShare, Rect& first,
                Rect& second) {
    const f32 gap = g.px(8.0f);
    if (c.profile == ScreenProfile::Wide) {
        const f32 w = (body.w - gap) * firstShare;
        first = {body.x, body.y, w, body.h};
        second = {body.x + w + gap, body.y, body.w - w - gap, body.h};
    } else {
        const f32 h = (body.h - gap) * firstShare;
        first = {body.x, body.y, body.w, h};
        second = {body.x, body.y + h + gap, body.w, body.h - h - gap};
    }
}

// The bags pane of a window: app/InventoryWindows' grid; its taps and drops go into `result` (a drop on the
// pane but not on a cell: `zone`).
void bagsPane(DialogContext& c, Gui& g, std::string_view name, const Rect& area, const BagsView& v,
              ItemDrag& drag, ItemWindowResult& result, i32 zone) {
    ItemWindowResult bags;
    bagsGrid(c, g, name, area, v, drag, bags);
    if (bags.tapped.valid()) {
        result.tapped = bags.tapped;
        result.tappedRect = bags.tappedRect;
    }
    if (bags.dropFrom.valid()) {
        result.dropFrom = bags.dropFrom;
        result.dropOn = bags.dropOn;
        result.dropZone = bags.dropOn.valid() ? -1 : zone;
    }
}

// A drop on `r` (not on a cell) reported as `zone`.
void dropZone(ItemDrag& drag, const Rect& r, i32 zone, ItemWindowResult& result) {
    if (drag.droppedIn(r)) {
        result.dropFrom = drag.from;
        result.dropOn = {};
        result.dropZone = zone;
        drag.taken = true;
    }
}

// The dark well behind a list (the inbox, the auctions): white and quality-colored text reads on it.
void listWell(Gui& g, const Rect& r) {
    g.draw.solid(r, rgba(0, 0, 0, 150));
    const f32 line = std::max(1.0f, g.px(1.0f));
    g.draw.solid({r.x, r.y, r.w, line}, rgba(255, 209, 0, 90));
    g.draw.solid({r.x, r.bottom() - line, r.w, line}, rgba(255, 209, 0, 90));
}

// A small caption.
void caption(Gui& g, const Rect& r, std::string_view s, f32 size, u32 color = ui::kGold,
             Align align = Align::Left) {
    TextStyle style = ui::gameFont(size, color);
    style.align = align;
    ui::label(g, r, s, style);
}

// An item cell with its name beside it in the quest item frame (the mail's attachment, the sale slot, trade
// slots).
void namedCell(DialogContext& c, Gui& g, WidgetId id, const Rect& r, const ItemSlotView& cell,
               std::string_view name, std::string_view hint, ItemDrag& drag, ItemWindowResult& result) {
    const Rect icon{r.x, r.y, r.h, r.h};
    const f32 frameX = icon.right() - icon.h * 0.12f;
    // The name frame takes the item's own tint (TradeFrame_UpdateTargetItem colours both alike).
    ui::threeSlice(g.draw, {frameX, r.y, r.right() - frameX, r.h}, kItemNameFrame,
                   {11.0f / 128.0f, 11.0f / 64.0f, 117.0f / 128.0f, 53.0f / 64.0f}, 16.0f / 128.0f,
                   r.h * 16.0f / 42.0f, cell.empty ? ui::kWhite : cell.tint);
    itemCell(c, g, id, icon, cell, drag, result);
    const Rect label{icon.right() + g.px(8.0f), r.y, r.right() - icon.right() - g.px(14.0f), r.h};
    const Metrics m = kit::metrics(c);
    if (!cell.empty) {
        TextStyle style = ui::gameFont(m.small, ui::qualityColor(cell.quality));
        const f32 h = g.draw.measureWrapped(label.w, name, style.font, style.size);
        g.draw.wrappedText({label.x, label.centerY() - h * 0.5f, label.w, h + 1.0f}, name, style);
    } else if (!hint.empty()) {
        TextStyle style = ui::gameFont(m.small * 0.92f, kDimText);
        const f32 h = g.draw.measureWrapped(label.w, hint, style.font, style.size);
        g.draw.wrappedText({label.x, label.centerY() - h * 0.5f, label.w, h + 1.0f}, hint, style);
    }
}

} // namespace

// -------------------------------------------------------------------------------------------------------
// Money typed in.

i32 moneyInput(DialogContext& c, Gui& g, WidgetId id, const Rect& r, u32 copper, i32 editing) {
    const ui::Coins coins = ui::splitMoney(copper);
    const u32 values[3] = {coins.gold, coins.silver, coins.copper};
    const f32 coin = std::min(r.h * 0.55f, g.px(16.0f));
    const f32 gap = g.px(4.0f);
    // Gold gets twice the room of silver and copper.
    const f32 unit = (r.w - 3.0f * (coin + gap) - 2.0f * gap) / 4.0f;
    i32 tapped = -1;
    f32 x = r.x;
    for (u32 part = 0; part < 3; ++part) {
        const f32 w = part == 0 ? unit * 2.0f : unit;
        char digits[12] = {};
        if (values[part] != 0 || editing == static_cast<i32>(part)) {
            std::snprintf(digits, sizeof(digits), "%u", values[part]);
        }
        if (editing == static_cast<i32>(part) && values[part] == 0) {
            digits[0] = '\0';
        }
        if (ui::textField(g, id + part * 104729, {x, r.y, w, r.h}, digits, "0",
                          editing == static_cast<i32>(part))) {
            tapped = static_cast<i32>(part);
        }
        x += w + g.px(2.0f);
        g.draw.sprite({x, r.centerY() - coin * 0.5f, coin, coin}, kMoneyIcons, ui::kWhite,
                      {static_cast<f32>(part) * 0.25f, 0.0f, static_cast<f32>(part) * 0.25f + 0.25f, 1.0f});
        x += coin + gap;
    }
    (void)c;
    return tapped;
}

u32 moneyWithPart(u32 copper, i32 part, std::string_view digits) {
    u64 value = 0;
    for (const char ch : digits) {
        if (ch >= '0' && ch <= '9') {
            value = std::min<u64>(value * 10 + static_cast<u64>(ch - '0'), 999999);
        }
    }
    ui::Coins coins = ui::splitMoney(copper);
    switch (part) {
    case kMoneyGold: coins.gold = static_cast<u32>(std::min<u64>(value, 214748)); break;
    case kMoneySilver: coins.silver = static_cast<u32>(std::min<u64>(value, 99)); break;
    case kMoneyCopper: coins.copper = static_cast<u32>(std::min<u64>(value, 99)); break;
    default: break;
    }
    return coins.gold * 10000u + coins.silver * 100u + coins.copper;
}

std::string_view moneyPartText(u32 copper, i32 part, std::array<char, 12>& buffer) {
    const ui::Coins coins = ui::splitMoney(copper);
    const u32 value = part == kMoneyGold ? coins.gold : part == kMoneySilver ? coins.silver : coins.copper;
    if (value == 0) {
        buffer[0] = '\0';
        return {};
    }
    const i32 n = std::snprintf(buffer.data(), buffer.size(), "%u", value);
    return {buffer.data(), static_cast<usize>(std::max(n, 0))};
}

bool keyboardCrowds(const DialogContext& c) {
    return c.safeArea.h < c.ui.height() * 0.62f;
}

bool compactEditor(DialogContext& c, std::string_view title, std::string_view label, std::string_view value,
                   bool multiline) {
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    const f32 fieldH = multiline ? std::min(g.px(150.0f), c.safeArea.h * 0.5f) : g.px(40.0f);
    const f32 heightDp = (fieldH + m.small * 1.6f + m.button) / c.ui.dp() + 60.0f;
    const Dialog d = kit::beginDialog(c, g, "exchange.edit", 560.0f, heightDp, title, false, true);
    caption(g, {d.body.x, d.body.y, d.body.w, m.small * 1.4f}, label, m.small);
    const Rect field{d.body.x, d.body.y + m.small * 1.5f, d.body.w, fieldH};
    if (multiline) {
        ui::parchment(g, field);
        g.draw.pushClip(field.inset(g.px(6.0f)));
        TextStyle style = ui::questFont(m.text);
        // The text with a blinking caret at its end; the end of a long text stays in sight.
        std::array<char, ui::TextInput::kCapacity + 2> shown{};
        const usize n = std::min(value.size(), shown.size() - 2);
        std::memcpy(shown.data(), value.data(), n);
        const bool blink = (c.ui.nowMs() / 500) % 2 == 0;
        shown[n] = blink ? '|' : ' ';
        const std::string_view typed{shown.data(), n + 1};
        const Rect inner = field.inset(g.px(10.0f));
        const f32 h = g.draw.measureWrapped(inner.w, typed, style.font, style.size);
        const f32 y = h > inner.h ? inner.bottom() - h : inner.y;
        g.draw.wrappedText({inner.x, y, inner.w, h + 1.0f}, typed, style);
        g.draw.popClip();
    } else {
        ui::textField(g, widgetId("exchange.edit.field"), field, value, {}, true);
    }
    const f32 w = std::min(d.footer.w, kit::buttonWidth(g, text::get("OKAY"), d.footer.h));
    const bool done =
        ui::button(g, widgetId("exchange.edit.done"),
                   {d.footer.centerX() - w * 0.5f, d.footer.y, w, d.footer.h}, text::get("OKAY"));
    return done || d.outside;
}

// -------------------------------------------------------------------------------------------------------
// Group loot rolls.

f32 rollFramesTop(const DialogContext& c) {
    return c.safeArea.y + c.ui.px(ui::kUnitDp) * 230.0f;
}

RollResult rollFrames(DialogContext& c, f32 top, std::span<const RollView> rolls) {
    VD_PROFILE_ZONE("ExchangeWindows::rolls");
    RollResult result;
    if (rolls.empty()) {
        return result;
    }
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    const f32 w =
        std::min(g.px(c.profile == ScreenProfile::Wide ? 420.0f : 400.0f), c.safeArea.w - g.px(16.0f));
    const f32 h = g.px(66.0f);
    const f32 x = c.safeArea.centerX() - w * 0.5f;
    f32 y = top;
    for (u32 i = 0; i < rolls.size() && y + h <= c.safeArea.bottom(); ++i) {
        const RollView& roll = rolls[i];
        const Rect panel{x, y, w, h};
        ui::hit(g, widgetId("roll.panel", i), panel); // nothing under it takes the finger
        ui::tooltipPanel(g, panel);
        const f32 pad = g.px(8.0f);
        const f32 iconSize = h - 2.0f * pad;
        const Rect icon{panel.x + pad, panel.y + pad, iconSize, iconSize};
        ui::SlotView slot;
        slot.icon = roll.item.iconHash ? c.resources.icon(roll.item.iconHash) : std::nullopt;
        slot.count = roll.item.count;
        slot.quality = roll.item.quality;
        if (ui::itemSlot(g, widgetId("roll.icon", i), icon, slot).clicked) {
            result.tooltip = static_cast<i32>(i);
            result.tooltipRect = icon;
        }
        // Need, greed, pass: the client's buttons with their names under them.
        const f32 button = std::min(g.px(44.0f), h - g.px(4.0f));
        const f32 buttonsW = button * 3.0f + g.px(8.0f);
        const f32 bx = panel.right() - pad - buttonsW;
        const std::string_view labels[3] = {text::get("NEED"), text::get("GREED"), text::get("PASS")};
        static constexpr u64 kUp[3] = {kDiceUp, kCoinUp, kPassUp};
        static constexpr u64 kDown[3] = {kDiceDown, kCoinDown, kPassDown};
        static constexpr u8 kVotes[3] = {1, 2, 0}; // net::kRollNeed, kRollGreed, kRollPass
        for (u32 b = 0; b < 3; ++b) {
            const Rect r{bx + static_cast<f32>(b) * (button + g.px(4.0f)), panel.y + g.px(2.0f), button,
                         h - g.px(4.0f)};
            const ui::WidgetState state = ui::hit(g, widgetId("roll.button", i * 4 + b), r);
            const f32 art = std::min(r.w, r.h - m.small) * 0.92f;
            const Rect artRect{r.centerX() - art * 0.5f, r.y, art, art};
            g.draw.sprite(artRect, ui::held(state, r) ? kDown[b] : kUp[b]);
            caption(g, {r.x - g.px(6.0f), artRect.bottom() - g.px(3.0f), r.w + g.px(12.0f), m.small},
                    labels[b], m.small * 0.85f, ui::kWhite, Align::Center);
            if (ui::tapped(state)) {
                result.roll = static_cast<i32>(i);
                result.vote = kVotes[b];
            }
        }
        // The item's name, then the countdown.
        const f32 textX = icon.right() + g.px(8.0f);
        const f32 textW = bx - g.px(8.0f) - textX;
        caption(g, {textX, panel.y + pad * 0.6f, textW, h * 0.48f}, roll.item.name, m.text,
                ui::qualityColor(roll.item.quality));
        const f32 share =
            std::clamp(static_cast<f32>(roll.remainingMs) / static_cast<f32>(std::max(roll.countdownMs, 1u)),
                       0.0f, 1.0f);
        // Retouches (windows): GroupLootFrameTimer: UI-Character-Skills-Bar in yellow (BarColor 1, 1, 0)
        // inside UI-Character-Skills-BarBorder, no seconds.
        const Rect bar{textX, panel.y + h * 0.6f, textW, g.px(10.0f)};
        art::skillsBar(g, bar, share, rgba(255, 255, 0), art::kLootTimerBorder);
        y += h + g.px(6.0f);
    }
    return result;
}

// -------------------------------------------------------------------------------------------------------
// The minimap's letter.

Rect mailIndicatorRect(const DialogContext& c) {
    // app/Hud's minimap (see backpackButtonRect): on its rim, lower right (300 degrees).
    const f32 unit = c.ui.px(ui::kUnitDp);
    const Rect& safe = c.safeArea;
    const f32 mapX = safe.right() - 87.0f * unit - c.ui.px(4.0f),
              mapY = safe.y + 92.0f * unit + c.ui.px(2.0f);
    const f32 size = c.ui.px(32.0f);
    const f32 distance = 70.0f * unit + 12.0f * unit;
    const f32 angle = 300.0f * 3.14159265f / 180.0f;
    return {mapX + std::cos(angle) * distance - size * 0.5f, mapY - std::sin(angle) * distance - size * 0.5f,
            size, size};
}

bool mailIndicator(DialogContext& c) {
    Gui g{c.ui, c.draw};
    const Rect r = mailIndicatorRect(c);
    ui::SlotView slot;
    slot.icon = c.resources.icon(kLetterIcon);
    return ui::itemSlot(g, widgetId("hud.mail"), r, slot).clicked;
}

// -------------------------------------------------------------------------------------------------------
// Mail.

namespace {

void mailRow(DialogContext& c, Gui& g, u32 index, const Rect& r, const MailRowView& row, bool selected,
             MailResult& result) {
    const Metrics m = kit::metrics(c);
    const ui::WidgetState state = ui::hit(g, widgetId("mail.row", index), r);
    if (selected || ui::held(state, r)) {
        g.draw.sprite(r, kListboxHighlight, rgba(255, 255, 255, selected ? 200 : 130));
    }
    if (ui::tapped(state)) {
        result.row = static_cast<i32>(index);
    }
    const f32 iconSize = std::min(m.item, r.h - g.px(6.0f));
    const Rect icon{r.x + g.px(4.0f), r.centerY() - iconSize * 0.5f, iconSize, iconSize};
    // Retouches (windows): MailItemTemplate's button: the icon over UI-EmptySlot-White, gold while unread,
    // grey (0.5) once read with the icon desaturated (InboxFrame_Update's SetDesaturation);
    // CheckButtonHilight (ADD) on the open one. No quick slot frame.
    const f32 s = icon.w / 37.0f;
    g.draw.sprite({icon.centerX() - 32.0f * s, icon.centerY() - 32.0f * s, 64.0f * s, 64.0f * s},
                  kEmptySlotWhite, row.unread ? rgba(255, 209, 0) : rgba(128, 128, 128));
    ui::iconOrPlaceholder(g.draw, icon, row.iconHash ? c.resources.icon(row.iconHash) : std::nullopt,
                          ui::kWhite, {0.0f, 0.0f, 1.0f, 1.0f}, !row.unread);
    if (selected) {
        g.draw.spriteAdditive(icon, kCheckedHighlight);
    }
    if (row.count > 1) {
        char count[12];
        std::snprintf(count, sizeof(count), "%u", row.count);
        TextStyle style = ui::numberFont(std::max(g.px(12.0f), icon.h * 0.34f));
        style.align = Align::Right;
        g.draw.text(icon.right() - icon.w * 0.08f, icon.bottom() - icon.h * 0.08f, count, style);
    }
    const f32 textX = icon.right() + g.px(8.0f);
    const f32 rightW = g.px(row.cod > 0 || row.money > 0 ? 110.0f : 56.0f);
    const f32 textW = r.right() - textX - rightW - g.px(4.0f);
    caption(g, {textX, r.y + r.h * 0.06f, textW, r.h * 0.46f}, row.sender, m.small,
            row.unread ? ui::kGold : kDimText);
    caption(g, {textX, r.y + r.h * 0.5f, textW, r.h * 0.44f}, row.subject, m.small,
            row.unread ? kWhiteText : kDimText);
    TextStyle expires = ui::gameFont(m.small * 0.9f, row.unread ? rgba(30, 255, 30) : kDimText);
    expires.align = Align::Right;
    ui::label(g, {r.right() - rightW, r.y + r.h * 0.06f, rightW - g.px(6.0f), r.h * 0.44f}, row.expires,
              expires);
    if (row.cod > 0) {
        TextStyle cod = ui::gameFont(m.small * 0.85f, ui::kRed);
        cod.align = Align::Right;
        const f32 moneyW = ui::moneyWidth(g.draw, row.cod, m.small * 0.85f);
        ui::label(g, {r.right() - rightW, r.y + r.h * 0.52f, rightW - moneyW - g.px(10.0f), r.h * 0.42f},
                  text::get("COD"), cod);
        ui::money(g, r.right() - g.px(6.0f), r.y + r.h * 0.73f, row.cod, m.small * 0.85f, Align::Right);
    } else if (row.money > 0) {
        ui::money(g, r.right() - g.px(6.0f), r.y + r.h * 0.73f, row.money, m.small * 0.85f, Align::Right);
    }
}

void inboxPane(DialogContext& c, Gui& g, const Rect& area, const MailView& v, MailResult& result) {
    const Metrics m = kit::metrics(c);
    listWell(g, area);
    const ui::ScrollView view = ui::beginScroll(g, widgetId("mail.list"), area.inset(g.px(3.0f)));
    const f32 rowH = m.item + g.px(c.profile == ScreenProfile::Wide ? 6.0f : 14.0f);
    f32 y = view.top + g.px(3.0f);
    for (u32 i = 0; i < v.rows.size(); ++i) {
        mailRow(c, g, i, {view.viewport.x + g.px(3.0f), y, view.viewport.w - g.px(6.0f), rowH}, v.rows[i],
                static_cast<i32>(i) == v.selected, result);
        y += rowH + g.px(2.0f);
    }
    if (v.rows.empty()) {
        TextStyle style = ui::gameFont(m.text, kDimText);
        style.align = Align::Center;
        ui::label(g, {view.viewport.x, view.top + g.px(12.0f), view.viewport.w, rowH},
                  v.loaded ? text::get("VD_EXCHANGE_NO_MAIL") : std::string_view("..."), style);
    }
    ui::endScroll(g, view, y - view.top + g.px(4.0f));
}

// Retouches (windows): OpenMailInvoiceFrame (OpenMail_Update) on the letter's parchment, in
// InvoiceTextFontNormal: the item and the other side, then the labels right-aligned 125 units in, each with
// its money frame 130 units further (SmallMoneyFrameTemplate), "+" before the deposit, "-" before the house's
// cut (red: SetMoneyFrameColor), UI-MailFrame-InvoiceLine above the total. The buyer's: the line under the
// seller, the price paid under it.
void mailInvoice(Gui& g, kit::Flow& flow, const MailInvoiceView& v, const Metrics& m) {
    const f32 u = std::min(m.small / 12.0f, flow.w / 300.0f); // one unit (InvoiceTextFontNormal is 12 high)
    const f32 fontH = 12.0f * u;
    const TextStyle normal = ui::questFont(m.small); // InvoiceTextFontNormal: (0.18, 0.12, 0.06)
    char line[160];
    char mode[48];
    const std::string_view how = v.buyout ? text::get("BUYOUT") : text::get("HIGH_BIDDER");
    std::snprintf(mode, sizeof(mode), "(%.*s)", static_cast<int>(how.size()), how.data());
    f32 y = flow.y + 4.0f * u;
    if (v.buyer) { // OpenMailInvoiceItemLabel: ITEM_PURCHASED_COLON.." "..itemName.."  "..buyMode
        const std::string_view label = text::get("ITEM_PURCHASED_COLON");
        std::snprintf(line, sizeof(line), "%.*s %.*s  %s", static_cast<int>(label.size()), label.data(),
                      static_cast<int>(v.item.size()), v.item.data(), mode);
        y += g.draw.wrappedText({flow.x, y, flow.w, 1e6f}, line, normal) + 5.0f * u;
        ui::label(g, {flow.x, y, flow.w, fontH}, labelled(line, text::get("SOLD_BY_COLON"), v.player),
                  normal);
    } else {
        y += g.draw.wrappedText({flow.x, y, flow.w, 1e6f},
                                labelled(line, text::get("ITEM_SOLD_COLON"), v.item), normal) +
             5.0f * u;
        // OpenMailInvoicePurchaser, OpenMailInvoiceBuyMode 5 units after it (InvoiceTextFontSmall).
        const std::string_view purchaser = labelled(line, text::get("PURCHASED_BY_COLON"), v.player);
        const f32 width = std::min(g.draw.measure(purchaser, normal.font, normal.size), flow.w);
        ui::label(g, {flow.x, y, flow.w, fontH}, purchaser, normal);
        const f32 modeX = flow.x + width + 5.0f * u;
        ui::label(g, {modeX, y, flow.x + flow.w - modeX, fontH}, mode,
                  ui::questFont(m.small * 10.0f / 12.0f));
    }
    const f32 purchaserBottom = y + fontH;
    const f32 labelRight = flow.x + 125.0f * u;
    const f32 moneyRight = labelRight + 130.0f * u;
    TextStyle right = normal;
    right.align = Align::Right;
    const auto moneyLine = [&](f32 top, std::string_view label, std::string_view sign, u32 copper,
                               u32 color) {
        ui::label(g, {flow.x, top, labelRight - flow.x, fontH}, label, right);
        const f32 width =
            ui::money(g, moneyRight, top + fontH * 0.5f, copper, m.small, Align::Right, 1.0f, color);
        if (!sign.empty()) { // its FontString's RIGHT 5 units left of the money frame
            const f32 signRight = moneyRight - width - 5.0f * u;
            ui::label(g, {signRight - 10.0f * u, top, 10.0f * u, fontH}, sign, right);
        }
    };
    const auto arithmeticLine = [&](f32 top) { // the texture's TOP; its line is 16 units down it
        g.draw.sprite({labelRight - 128.0f * u, top, 256.0f * u, 32.0f * u}, kInvoiceLine);
    };
    if (v.buyer) {
        arithmeticLine(purchaserBottom);                 // TOP at the purchaser's BOTTOMLEFT + (125, 0)
        const f32 paidTop = purchaserBottom + 26.0f * u; // where the sale price would be
        moneyLine(paidTop, text::get("AMOUNT_PAID_COLON"), {}, v.bid, ui::kWhite);
        flow.y = paidTop + fontH + 10.0f * u;
        return;
    }
    const f32 saleTop = purchaserBottom + 26.0f * u;
    const f32 depositTop = saleTop + fontH + 13.0f * u;
    const f32 cutTop = depositTop + fontH + 13.0f * u;
    const f32 receivedTop = cutTop + fontH + 18.0f * u;
    moneyLine(saleTop, text::get("SALE_PRICE_COLON"), {}, v.bid, ui::kWhite);
    moneyLine(depositTop, text::get("DEPOSIT_COLON"), "+", v.deposit, ui::kWhite);
    moneyLine(cutTop, text::get("AUCTION_HOUSE_CUT_COLON"), "-", v.cut, rgba(255, 0, 0));
    arithmeticLine(cutTop + fontH - 9.0f * u); // TOP at the cut's BOTTOMRIGHT + (0, 9)
    const u32 received = v.bid + v.deposit > v.cut ? v.bid + v.deposit - v.cut : 0;
    moneyLine(receivedTop, text::get("AMOUNT_RECEIVED_COLON"), {}, received, ui::kWhite);
    flow.y = receivedTop + fontH + 10.0f * u;
}

void messagePane(DialogContext& c, Gui& g, const Rect& area, const MailReadView& v, ItemDrag& drag,
                 MailResult& result) {
    const Metrics m = kit::metrics(c);
    const bool attachments = !v.item.empty || v.money > 0;
    const f32 takeH = attachments ? m.item + g.px(16.0f) + m.small * 1.4f : 0.0f;
    const Rect paper{area.x, area.y, area.w, area.h - takeH};
    const kit::ParchmentScroll p = kit::beginParchment(g, widgetId("mail.message"), paper);
    kit::Flow flow = kit::parchmentFlow(g, p);
    char line[192];
    flow.text(labelled(line, text::get("FROM"), v.sender), ui::questFont(m.small), g.px(2.0f));
    flow.text(labelled(line, text::get("MAIL_SUBJECT_LABEL"), v.subject), ui::questFont(m.small), g.px(8.0f));
    ui::horizontalBreak(g, flow.x, flow.y, flow.w);
    flow.y += g.px(10.0f);
    if (v.invoice.shown) {
        mailInvoice(g, flow, v.invoice, m);
    } else if (!v.body.empty()) {
        flow.text(v.body, ui::questFont(m.text), g.px(6.0f));
    } else if (v.waitingBody) {
        flow.text("...", ui::questFont(m.text), g.px(6.0f));
    }
    kit::endParchment(g, p, flow);
    if (!attachments) {
        return;
    }
    // What it carries, and the buttons that take it.
    const f32 y = paper.bottom() + g.px(6.0f);
    caption(g, {area.x, y, area.w, m.small * 1.3f}, text::get("TAKE_ATTACHMENTS"), m.small);
    const f32 rowY = y + m.small * 1.4f;
    const f32 buttonW = std::min(g.px(150.0f), area.w * 0.3f);
    f32 x = area.x;
    if (!v.item.empty) {
        const f32 cellW = std::min(area.w - buttonW * 2.0f - g.px(12.0f), g.px(260.0f));
        namedCell(c, g, widgetId("mail.item"), {x, rowY, cellW, m.item}, v.item, v.itemName, {}, drag,
                  result.items);
        x += cellW + g.px(6.0f);
        if (v.cod > 0) {
            ui::money(g, x, rowY + m.item * 0.25f, v.cod, m.small, Align::Left);
            caption(g, {x, rowY + m.item * 0.5f, buttonW, m.item * 0.5f}, text::get("COD"), m.small * 0.85f,
                    ui::kRed);
        }
    } else if (v.money > 0) {
        ui::money(g, x, rowY + m.item * 0.5f, v.money, m.text, Align::Left);
    }
    const f32 by = rowY + (m.item - m.button) * 0.5f;
    f32 bx = area.right() - buttonW;
    if (!v.item.empty &&
        ui::button(g, widgetId("mail.takeItem"), {bx, by, buttonW, m.button},
                   v.cod > 0 ? text::get("VD_EXCHANGE_PAY_COD") : text::get("VD_EXCHANGE_TAKE_ITEM"),
                   v.canTakeItem)) {
        result.button = kMailTakeItem;
    }
    bx -= buttonW + g.px(6.0f);
    if (v.money > 0 && ui::button(g, widgetId("mail.takeMoney"), {bx, by, buttonW, m.button},
                                  text::get("VD_EXCHANGE_TAKE_MONEY"), v.canTakeMoney)) {
        result.button = kMailTakeMoney;
    }
}

// A field of the message being written: its label over it; true when tapped.
bool sendField(DialogContext& c, Gui& g, WidgetId id, f32 x, f32& y, f32 w, std::string_view label,
               std::string_view value, bool editing) {
    const Metrics m = kit::metrics(c);
    caption(g, {x, y, w, m.small * 1.3f}, label, m.small);
    y += m.small * 1.35f;
    const f32 h = g.px(38.0f);
    const bool tapped = ui::textField(g, id, {x, y, w, h}, value, {}, editing);
    y += h + g.px(6.0f);
    return tapped;
}

void sendPane(DialogContext& c, Gui& g, const Rect& area, const MailSendView& v, ItemDrag& drag,
              MailResult& result) {
    const Metrics m = kit::metrics(c);
    const ui::ScrollView view = ui::beginScroll(g, widgetId("mail.compose"), area);
    const f32 x = view.viewport.x + g.px(4.0f), w = view.viewport.w - g.px(8.0f);
    f32 y = view.top + g.px(2.0f);
    // To and subject side by side when there is room.
    if (w >= g.px(440.0f)) {
        f32 y2 = y;
        const f32 half = (w - g.px(8.0f)) * 0.5f;
        if (sendField(c, g, widgetId("mail.to"), x, y, half * 0.8f, text::get("MAIL_TO_LABEL"), v.to,
                      v.editing == 0)) {
            result.field = 0;
        }
        if (sendField(c, g, widgetId("mail.subject"), x + half * 0.8f + g.px(8.0f), y2,
                      w - half * 0.8f - g.px(8.0f), text::get("MAIL_SUBJECT_LABEL"), v.subject,
                      v.editing == 1)) {
            result.field = 1;
        }
        y = std::max(y, y2);
    } else {
        if (sendField(c, g, widgetId("mail.to"), x, y, w, text::get("MAIL_TO_LABEL"), v.to, v.editing == 0)) {
            result.field = 0;
        }
        if (sendField(c, g, widgetId("mail.subject"), x, y, w, text::get("MAIL_SUBJECT_LABEL"), v.subject,
                      v.editing == 1)) {
            result.field = 1;
        }
    }
    // The body: parchment that grows with its text; a tap edits it.
    caption(g, {x, y, w, m.small * 1.3f}, text::get("VD_EXCHANGE_MESSAGE"), m.small);
    y += m.small * 1.35f;
    TextStyle body = ui::questFont(m.small);
    const f32 bodyTextH = g.draw.measureWrapped(w - g.px(20.0f), v.body, body.font, body.size);
    const Rect bodyRect{x, y, w, std::clamp(bodyTextH + g.px(20.0f), g.px(70.0f), g.px(150.0f))};
    ui::parchment(g, bodyRect);
    g.draw.pushClip(bodyRect.inset(g.px(4.0f)));
    g.draw.wrappedText(
        {bodyRect.x + g.px(10.0f), bodyRect.y + g.px(10.0f), w - g.px(20.0f), bodyTextH + 1.0f}, v.body,
        body);
    g.draw.popClip();
    if (v.editing == 2) {
        g.draw.sprite(bodyRect, kQuestTitleHighlight, rgba(255, 255, 255, 90));
    }
    if (ui::tapped(ui::hit(g, widgetId("mail.body"), bodyRect))) {
        result.field = 2;
    }
    y = bodyRect.bottom() + g.px(8.0f);
    // The attachment.
    namedCell(c, g, widgetId("mail.attachment"), {x, y, std::min(w, g.px(360.0f)), m.item}, v.attachment,
              v.attachmentName, text::get("ATTACHMENT_TEXT"), drag, result.items);
    dropZone(drag, {x, y, w, m.item}, 1, result.items);
    y += m.item + g.px(10.0f);
    // Money or cash on delivery.
    const std::string_view kinds[2] = {text::get("SEND_MONEY"), text::get("COD")};
    const f32 kindsW = std::min(w, g.px(260.0f));
    if (art::radioRow(g, widgetId("mail.cod"), {x, y, kindsW, m.button * 0.9f}, kinds, v.cod ? 1 : 0,
                      m.small) >= 0) {
        result.button = kMailToggleCod;
    }
    const f32 moneyX = x + kindsW + g.px(10.0f);
    const bool moneyBeside = w - kindsW - g.px(10.0f) >= g.px(200.0f);
    const Rect moneyRect =
        moneyBeside ? Rect{moneyX, y, std::min(w - kindsW - g.px(10.0f), g.px(260.0f)), m.button * 0.9f}
                    : Rect{x, y + m.button * 0.9f + g.px(6.0f), std::min(w, g.px(260.0f)), m.button * 0.9f};
    const i32 part = moneyInput(c, g, widgetId("mail.money"), moneyRect, v.money, v.editing - 3);
    if (part >= 0) {
        result.field = 3 + part;
    }
    y = moneyRect.bottom() + g.px(8.0f);
    // What it costs.
    TextStyle cost = ui::gameFont(m.small, ui::kGold);
    const f32 lh = m.small * 1.4f;
    ui::label(g, {x, y, w * 0.5f, lh}, text::get("SEND_MAIL_COST"), cost);
    ui::money(g, x + g.draw.measure(text::get("SEND_MAIL_COST"), ui::FontId::Friz, m.small) + g.px(8.0f),
              y + lh * 0.5f, v.cost, m.small, Align::Left);
    y += lh + g.px(4.0f);
    ui::endScroll(g, view, y - view.top);
}

} // namespace

MailResult mailWindow(DialogContext& c, const MailView& v, ItemDrag& drag) {
    VD_PROFILE_ZONE("ExchangeWindows::mail");
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    MailResult result;
    const Dialog d = kit::beginDialog(c, g, "mail", wide ? 920.0f : 680.0f, wide ? 440.0f : 780.0f,
                                      text::get("MAIL_LABEL"), true, true);
    result.closed = d.closed;
    const std::string_view tabs[] = {text::get("INBOX"), text::get("SENDMAIL")};
    const f32 tabH = g.px(30.0f);
    result.tab = tabBar(g, "mail.tabs", {d.body.x, d.body.y, d.body.w, tabH}, tabs, v.tab, m.text);
    const Rect body{d.body.x, d.body.y + tabH + g.px(6.0f), d.body.w, d.body.h - tabH - g.px(6.0f)};
    if (v.tab == 0) {
        // Square screens stack the list over the open message; with none open the list takes the whole body
        // (phase 18: it was cut to its top 40%).
        Rect list = body, message;
        if (wide || v.selected >= 0) {
            splitPanes(c, g, body, wide ? 0.42f : 0.4f, list, message);
        }
        inboxPane(c, g, list, v, result);
        if (v.selected >= 0) {
            messagePane(c, g, message, v.read, drag, result);
        }
        // Footer: what can be done with the open message.
        struct Button {
            std::string_view label;
            bool enabled;
            MailButton id;
        };
        // Phase 18 (p3): OpenMailDeleteButton is Delete or Return (InboxItemCanDelete), never both.
        const Button buttons[] = {
            {text::get("REPLY_MESSAGE"), v.read.canReply, kMailReply},
            v.read.canDelete ? Button{text::get("DELETE"), true, kMailDelete}
                             : Button{text::get("MAIL_RETURN"), v.read.canReturn, kMailReturn},
            {text::get("VD_EXCHANGE_MAKE_COPY"), v.read.canCopy, kMailCopy}};
        if (v.selected >= 0) {
            const f32 gap = g.px(6.0f);
            const f32 bw = std::min(g.px(170.0f), (d.footer.w - gap * 2.0f) / 3.0f);
            f32 x = d.footer.right() - bw;
            for (u32 i = 3; i-- > 0;) {
                if (ui::button(g, widgetId("mail.footer", i), {x, d.footer.y, bw, d.footer.h},
                               buttons[i].label, buttons[i].enabled)) {
                    result.button = buttons[i].id;
                }
                x -= bw + gap;
            }
        }
        return result;
    }
    // Writing: the form, the bags to take the attachment from.
    Rect form, bags;
    splitPanes(c, g, body, wide ? 0.55f : 0.56f, form, bags);
    sendPane(c, g, form, v.send, drag, result);
    bagsPane(c, g, "mail.bags", bags, v.send.bags, drag, result.items, 0);
    const f32 sendW = std::min(d.footer.w * 0.4f, kit::buttonWidth(g, text::get("SEND_LABEL"), d.footer.h));
    if (ui::button(g, widgetId("mail.send"), {d.footer.right() - sendW, d.footer.y, sendW, d.footer.h},
                   text::get("SEND_LABEL"), v.send.problem.empty() && !v.send.sending)) {
        result.button = kMailSend;
    }
    if (!v.send.problem.empty()) {
        caption(g, {d.footer.x, d.footer.y, d.footer.w - sendW - g.px(8.0f), d.footer.h}, v.send.problem,
                m.small, rgba(255, 120, 100));
    }
    return result;
}

// -------------------------------------------------------------------------------------------------------
// Trade.

namespace {

void tradeSide(DialogContext& c, Gui& g, std::string_view name, const Rect& area, const TradeSideView& v,
               bool mine, i32 editing, ItemDrag& drag, TradeResult& result) {
    const Metrics m = kit::metrics(c);
    if (v.accepted) {
        art::tradeHighlight(g, area); // retouches (windows): TradeHighlightPlayer / TradeHighlightRecipient
    }
    g.draw.solid({area.x, area.y, area.w, std::max(1.0f, g.px(1.0f))}, rgba(255, 209, 0, 120));
    caption(g, {area.x + g.px(4.0f), area.y + g.px(2.0f), area.w - g.px(8.0f), m.text * 1.4f}, v.name, m.text,
            v.accepted ? ui::kGreen : ui::kGold);
    // Six traded slots, then the one that is only enchanted, then the money; scrolling when the pane is
    // short. A short pane (wide screens) shows the items as a grid of icons (their names in the tooltip).
    const Rect inner{area.x, area.y + m.text * 1.6f, area.w, area.h - m.text * 1.6f};
    const ui::ScrollView view = ui::beginScroll(g, widgetId(name, 900), inner);
    const f32 top = view.top;
    const bool icons = inner.h < g.px(300.0f);
    const f32 step = m.item + g.px(6.0f);
    const u32 columns = icons ? std::clamp(static_cast<u32>((area.w - g.px(8.0f)) / step), 1u, 6u)
                              : (area.w >= g.px(300.0f) ? 2u : 1u);
    const f32 colW = icons ? step : (area.w - g.px(8.0f)) / static_cast<f32>(columns);
    for (u32 i = 0; i < 6; ++i) {
        const Rect r{area.x + g.px(4.0f) + static_cast<f32>(i % columns) * colW,
                     top + static_cast<f32>(i / columns) * step, icons ? m.item : colW - g.px(6.0f), m.item};
        if (icons) {
            itemCell(c, g, widgetId(name, i), r, v.slots[i], drag, result.items);
        } else {
            namedCell(c, g, widgetId(name, i), r, v.slots[i], v.names[i], {}, drag, result.items);
        }
        if (mine) {
            dropZone(drag, r, static_cast<i32>(1 + i), result.items);
        }
    }
    f32 y = top + static_cast<f32>((6 + columns - 1) / columns) * step + g.px(2.0f);
    caption(g, {area.x + g.px(4.0f), y, area.w, m.small * 1.3f}, text::get("TRADEFRAME_ENCHANT_SLOT_LABEL"),
            m.small * 0.9f, kDimText);
    y += m.small * 1.35f;
    const Rect last{area.x + g.px(4.0f), y,
                    icons ? std::min(area.w - g.px(8.0f), g.px(260.0f)) : colW - g.px(6.0f), m.item};
    namedCell(c, g, widgetId(name, 6), last, v.slots[6], v.names[6], {}, drag, result.items);
    if (mine) {
        dropZone(drag, last, 7, result.items);
    }
    y += m.item + g.px(8.0f);
    // The money: typed on the player's side.
    const Rect moneyRect{area.x + g.px(4.0f), y, std::min(area.w - g.px(8.0f), g.px(260.0f)),
                         m.button * 0.9f};
    if (mine) {
        const i32 part = moneyInput(c, g, widgetId(name, 50), moneyRect, v.money, editing);
        if (part >= 0) {
            result.field = part;
        }
    } else {
        ui::money(g, moneyRect.x, moneyRect.centerY(), v.money, m.text, Align::Left);
    }
    y = moneyRect.bottom() + g.px(4.0f);
    ui::endScroll(g, view, y - top);
}

} // namespace

TradeResult tradeWindow(DialogContext& c, const TradeView& v, ItemDrag& drag) {
    VD_PROFILE_ZONE("ExchangeWindows::trade");
    Gui g{c.ui, c.draw};
    const bool wide = c.profile == ScreenProfile::Wide;
    TradeResult result;
    const Dialog d = kit::beginDialog(c, g, "trade", wide ? 940.0f : 680.0f, wide ? 440.0f : 780.0f,
                                      text::get("TRADE"), true, true);
    result.closed = d.closed;
    Rect sides, bags;
    splitPanes(c, g, d.body, wide ? 0.6f : 0.55f, sides, bags);
    const f32 gap = g.px(8.0f);
    const Rect mine{sides.x, sides.y, (sides.w - gap) * 0.5f, sides.h};
    const Rect theirs{mine.right() + gap, sides.y, sides.w - mine.w - gap, sides.h};
    TradeSideView me = v.mine;
    if (me.name.empty()) {
        me.name = text::get("YOU");
    }
    tradeSide(c, g, "trade.mine", mine, me, true, v.editing, drag, result);
    tradeSide(c, g, "trade.theirs", theirs, v.theirs, false, -1, drag, result);
    bagsPane(c, g, "trade.bags", bags, v.bags, drag, result.items, 0);
    bool cancel = false, accept = false;
    kit::footerButtons(g, d.footer, "trade.footer", text::get("CANCEL"), true, cancel,
                       v.mine.accepted ? text::get("VD_EXCHANGE_TRADE_CHANGE") : text::get("TRADE"), accept);
    if (cancel) {
        result.button = kTradeCancel;
    } else if (accept) {
        result.button = v.mine.accepted ? kTradeUnaccept : kTradeAccept;
    }
    return result;
}

// -------------------------------------------------------------------------------------------------------
// Auction house.

namespace {

void auctionRow(DialogContext& c, Gui& g, u32 index, const Rect& r, const AuctionRowView& row, bool selected,
                bool wide, ItemDrag& drag, AuctionResult& result) {
    const Metrics m = kit::metrics(c);
    const ui::WidgetState state = ui::hit(g, widgetId("auction.row", index), r);
    if (selected || ui::held(state, r)) {
        g.draw.sprite(r, kListboxHighlight, rgba(255, 255, 255, selected ? 200 : 130));
    }
    if (ui::tapped(state)) {
        result.row = static_cast<i32>(index);
    }
    const f32 iconSize = std::min(m.item, r.h - g.px(6.0f));
    const Rect icon{r.x + g.px(4.0f), r.centerY() - iconSize * 0.5f, iconSize, iconSize};
    itemCell(c, g, widgetId("auction.cell", index), icon, row.cell, drag, result.items);
    const f32 textX = icon.right() + g.px(8.0f);
    const f32 priceW = g.px(wide ? 230.0f : 150.0f);
    const f32 infoW = wide ? g.px(190.0f) : 0.0f;
    const f32 nameW = r.right() - textX - priceW - infoW - g.px(8.0f);
    caption(g, {textX, r.y + r.h * 0.06f, nameW, r.h * 0.5f}, row.name, m.small,
            ui::qualityColor(row.cell.quality));
    char line[96];
    if (row.level > 0) {
        const std::string_view lvl = text::get("LEVEL_ABBR");
        // Phase 18 (extras): RED_FONT_COLOR_CODE .. level .. FONT_COLOR_CODE_CLOSE above the player's level.
        std::snprintf(line, sizeof(line), "%.*s %s%u%s  %.*s", static_cast<int>(lvl.size()), lvl.data(),
                      row.levelTooHigh ? kRedFontColorCode : "", row.level, row.levelTooHigh ? "|r" : "",
                      static_cast<int>(row.timeLeft.size()), row.timeLeft.data());
    } else {
        std::snprintf(line, sizeof(line), "%.*s", static_cast<int>(row.timeLeft.size()), row.timeLeft.data());
    }
    caption(g, {textX, r.y + r.h * 0.52f, nameW, r.h * 0.44f}, line, m.small * 0.9f, kDimText);
    if (wide) {
        const u32 color = row.outbid ? ui::kRed : row.leading ? ui::kGreen : kWhiteText;
        caption(g, {textX + nameW + g.px(4.0f), r.y, infoW, r.h}, row.seller, m.small * 0.95f, color);
    } else if (row.leading || row.outbid) {
        caption(g, {textX, r.y + r.h * 0.52f, nameW, r.h * 0.44f},
                row.leading ? text::get("HIGH_BIDDER") : text::get("OUTBID"), m.small * 0.9f,
                row.outbid ? ui::kRed : ui::kGreen, Align::Right);
    }
    // The bid (or the starting price) over the buyout.
    const f32 right = r.right() - g.px(6.0f);
    const f32 size = m.small * 0.95f;
    if (row.hasBid || row.bid > 0) {
        ui::money(g, right, r.y + r.h * 0.3f, row.bid, size, Align::Right, row.startingPrice ? 0.5f : 1.0f);
    }
    if (!row.hasBid) {
        caption(g, {right - priceW, r.y + r.h * 0.06f, priceW * 0.4f, r.h * 0.48f}, text::get("NO_BIDS"),
                size * 0.85f, kDimText);
    }
    if (row.buyout > 0) {
        ui::money(g, right, r.y + r.h * 0.74f, row.buyout, size, Align::Right);
        caption(g, {right - priceW, r.y + r.h * 0.52f, priceW * 0.45f, r.h * 0.44f}, text::get("BUYOUT"),
                size * 0.85f, kDimText);
    }
}

// Phase 18 (p3): a row as a table under the sort headers (`edges`: the columns' x, as sortColumns lays them out).
void auctionTableRow(DialogContext& c, Gui& g, u32 tab, u32 index, const Rect& r, const AuctionRowView& row,
                     bool selected, std::span<const f32> edges, std::span<const AuctionColumn> columns, ItemDrag& drag,
                     AuctionResult& result) {
    const Metrics m = kit::metrics(c);
    const ui::WidgetState state = ui::hit(g, widgetId("auction.row", index), r);
    if (selected || ui::held(state, r)) {
        g.draw.sprite(r, kListboxHighlight, rgba(255, 255, 255, selected ? 200 : 130));
    }
    if (ui::tapped(state)) {
        result.row = static_cast<i32>(index);
    }
    const f32 size = m.small * 0.95f;
    for (u32 col = 0; col < columns.size() && col + 1 < edges.size(); ++col) {
        const Rect cell{edges[col] + g.px(4.0f), r.y, edges[col + 1] - edges[col] - g.px(8.0f), r.h};
        if (cell.w <= 0.0f) {
            continue;
        }
        switch (columns[col]) {
        case AuctionColumn::Item: {
            const f32 iconSize = std::min(m.item, r.h - g.px(6.0f));
            const Rect icon{cell.x, r.centerY() - iconSize * 0.5f, iconSize, iconSize};
            itemCell(c, g, widgetId("auction.cell", index), icon, row.cell, drag, result.items);
            const f32 textX = icon.right() + g.px(6.0f);
            g.draw.pushClip({textX, r.y, std::max(cell.right() - textX, 0.0f), r.h});
            caption(g, {textX, r.y, cell.right() - textX, r.h}, row.name, m.small, ui::qualityColor(row.cell.quality));
            g.draw.popClip();
            break;
        }
        case AuctionColumn::Level: {
            if (row.level > 0) {
                char level[24];
                std::snprintf(level, sizeof(level), "%s%u%s", row.levelTooHigh ? kRedFontColorCode : "", row.level,
                              row.levelTooHigh ? "|r" : ""); // phase 18 (extras): red above the player's level
                caption(g, cell, level, size, kWhiteText, Align::Center);
            }
            break;
        }
        case AuctionColumn::TimeLeft: caption(g, cell, row.timeLeft, size, kWhiteText, Align::Center); break;
        case AuctionColumn::Info: {
            // The browse list's seller (white); the bids' status (HIGH_BIDDER green, OUTBID red); the player's
            // auctions' high bidder, or NO_BIDS in red.
            const u32 color = tab == 1   ? (row.outbid ? ui::kRed : row.leading ? ui::kGreen : kWhiteText)
                              : tab == 2 ? (row.hasBid ? kWhiteText : ui::kRed)
                                         : kWhiteText;
            g.draw.pushClip(cell);
            caption(g, cell, row.seller, size, color, Align::Center);
            g.draw.popClip();
            break;
        }
        case AuctionColumn::BidAndBuyout: {
            // The bid (or the starting one), the buyout under it (BrowseButtonTemplate's MoneyFrame and
            // BuyoutMoneyFrame); browsing, "Your bid:" left of the player's bid and "Buyout" left of the buyout.
            const f32 bidY = row.buyout > 0 ? r.y + r.h * 0.3f : r.centerY();
            if (row.hasBid || row.bid > 0) { // phase 18 (extras): a starting price of the player's at half alpha
                ui::money(g, cell.right(), bidY, row.bid, size, Align::Right, row.startingPrice ? 0.5f : 1.0f);
                if (tab == 0 && row.leading) {
                    const f32 w = ui::moneyWidth(g.draw, row.bid, size) + g.px(5.0f);
                    caption(g, {cell.x, bidY - r.h * 0.22f, cell.w - w, r.h * 0.44f}, text::get("YOUR_BID"),
                            size * 0.9f, ui::kGold, Align::Right);
                }
            }
            if (row.buyout > 0) {
                const f32 buyoutY = r.y + r.h * 0.72f;
                ui::money(g, cell.right(), buyoutY, row.buyout, size * 0.9f, Align::Right);
                if (tab == 0) {
                    const f32 w = ui::moneyWidth(g.draw, row.buyout, size * 0.9f) + g.px(5.0f);
                    caption(g, {cell.x, buyoutY - r.h * 0.22f, cell.w - w, r.h * 0.44f}, text::get("BUYOUT_COST"),
                            size * 0.85f, ui::kGold, Align::Right);
                }
            }
            break;
        }
        case AuctionColumn::Buyout:
            if (row.buyout > 0) {
                ui::money(g, cell.right(), r.centerY(), row.buyout, size, Align::Right);
            }
            break;
        case AuctionColumn::Bid: ui::money(g, cell.right(), r.centerY(), row.bid, size, Align::Right); break;
        }
    }
}

void auctionList(DialogContext& c, Gui& g, const Rect& area, const AuctionView& v, ItemDrag& drag,
                 AuctionResult& result) {
    const Metrics m = kit::metrics(c);
    const bool wide = area.w >= g.px(560.0f);
    // Phase 18 (p3): the sort headers over the list, its rows a table under them.
    Rect listArea = area;
    const bool table = !v.headers.empty() && v.columns.size() == v.headers.size();
    // A narrow list (the player's auctions beside the sale on wide screens) gives the item's column more room, and
    // the money's less.
    std::array<SortHeaderView, 8> headers{};
    const usize n = std::min(v.headers.size(), headers.size());
    const bool narrow = area.w < g.px(520.0f);
    for (usize i = 0; i < n && table; ++i) {
        headers[i] = v.headers[i];
        if (narrow && v.columns[i] == AuctionColumn::Item) {
            headers[i].weight *= 1.25f;
        } else if (narrow && (v.columns[i] == AuctionColumn::BidAndBuyout || v.columns[i] == AuctionColumn::Bid)) {
            headers[i].weight *= 0.7f;
        }
    }
    if (table) {
        const f32 headerH = std::max(m.small * 1.8f, g.px(26.0f));
        const SortHeaderResult h =
            sortHeaders(g, "auction.header", {area.x + g.px(6.0f), area.y, area.w - g.px(12.0f), headerH},
                        {headers.data(), n}, m.small * 0.95f);
        result.header = h.tapped;
        listArea.y += headerH + g.px(2.0f);
        listArea.h -= headerH + g.px(2.0f);
    }
    listWell(g, listArea);
    const ui::ScrollView view = ui::beginScroll(g, widgetId("auction.list", v.tab), listArea.inset(g.px(3.0f)));
    const f32 rowH = m.item + g.px(c.profile == ScreenProfile::Wide ? 6.0f : 14.0f);
    f32 y = view.top + g.px(3.0f);
    std::array<f32, 9> edges{};
    for (u32 i = 0; i < v.rows.size(); ++i) {
        const Rect r{view.viewport.x + g.px(3.0f), y, view.viewport.w - g.px(6.0f), rowH};
        if (table) {
            sortColumns(r, {headers.data(), n}, {edges.data(), n + 1});
            auctionTableRow(c, g, v.tab, i, r, v.rows[i], static_cast<i32>(i) == v.selected, {edges.data(), n + 1},
                            v.columns.first(n), drag, result);
        } else {
            auctionRow(c, g, i, r, v.rows[i], static_cast<i32>(i) == v.selected, wide, drag, result);
        }
        y += rowH + g.px(2.0f);
    }
    if (v.rows.empty()) {
        TextStyle style = ui::gameFont(m.text, kDimText);
        style.align = Align::Center;
        const f32 h = g.draw.measureWrapped(view.viewport.w - g.px(20.0f),
                                            v.pending ? text::get("SEARCHING_FOR_ITEMS") : v.empty,
                                            style.font, style.size);
        g.draw.wrappedText(
            {view.viewport.x + g.px(10.0f), view.top + g.px(16.0f), view.viewport.w - g.px(20.0f), h + 1.0f},
            v.pending ? text::get("SEARCHING_FOR_ITEMS") : v.empty, style);
    }
    ui::endScroll(g, view, y - view.top + g.px(4.0f));
}

// Browse's filters: name, levels, category, quality, usable and "Search". Returns their height.
f32 auctionFilters(DialogContext& c, Gui& g, const Rect& area, const AuctionView& v, AuctionResult& result) {
    const Metrics m = kit::metrics(c);
    const AuctionFilterView& f = v.filter;
    const f32 fieldH = g.px(36.0f);
    const f32 gap = g.px(6.0f);
    // Wide screens: one row, the labels as the fields' placeholders (the list keeps the height); square ones:
    // two rows under their labels.
    const bool oneRow = c.profile == ScreenProfile::Wide;
    const f32 labelH = oneRow ? 0.0f : m.small * 1.3f;
    const f32 levelW = g.px(52.0f);
    // One row: name, levels, category, quality, usable, search share what is left after the levels.
    const f32 flexible = area.w - levelW * 2.0f - gap * 7.0f;
    f32 x = area.x, y = area.y;
    const f32 nameW = oneRow ? flexible * 0.24f : area.w * 0.42f;
    if (!oneRow) {
        caption(g, {x, y, nameW, labelH}, text::get("NAME"), m.small);
        caption(g, {x + nameW + gap, y, levelW * 2.0f + gap, labelH}, text::get("LEVEL_RANGE"), m.small);
    }
    if (ui::textField(g, widgetId("auction.name"), {x, y + labelH, nameW, fieldH}, f.name,
                      oneRow ? text::get("NAME") : std::string_view{}, v.editing == 0)) {
        result.field = 0;
    }
    x += nameW + gap;
    // A level of 0 shows the empty field.
    const auto levelText = [](u32 value, char(&out)[8]) {
        out[0] = '\0';
        if (value != 0) {
            std::snprintf(out, sizeof(out), "%u", value);
        }
        return out;
    };
    char level[8];
    levelText(f.levelMin, level);
    if (ui::textField(g, widgetId("auction.min"), {x, y + labelH, levelW, fieldH}, level,
                      oneRow ? text::get("LEVEL_ABBR") : std::string_view{}, v.editing == 1)) {
        result.field = 1;
    }
    levelText(f.levelMax, level);
    if (ui::textField(g, widgetId("auction.max"), {x + levelW + gap, y + labelH, levelW, fieldH}, level,
                      oneRow ? text::get("LEVEL_ABBR") : std::string_view{}, v.editing == 2)) {
        result.field = 2;
    }
    x += levelW * 2.0f + gap * 2.0f;
    if (!oneRow) {
        // The rest on a second row; "Search" ends the first.
        const f32 searchW = area.right() - x;
        if (ui::button(g, widgetId("auction.search"), {x, y + labelH, searchW, fieldH}, text::get("SEARCH"),
                       !v.pending)) {
            result.button = kAuctionSearch;
        }
        x = area.x;
        y += labelH + fieldH + gap;
    }
    // Category and quality: buttons that open their pickers.
    const f32 pickW = oneRow ? flexible * 0.22f : (area.w - gap * 2.0f) * 0.38f;
    if (ui::button(g, widgetId("auction.category"), {x, y, pickW, fieldH}, f.category)) {
        result.button = kAuctionCategory;
    }
    x += pickW + gap;
    const f32 qualityW = oneRow ? flexible * 0.14f : (area.w - gap * 2.0f) * 0.27f;
    if (ui::button(g, widgetId("auction.quality"), {x, y, qualityW, fieldH}, f.quality)) {
        result.button = kAuctionQuality;
    }
    x += qualityW + gap;
    bool usable = f.usable;
    const f32 usableW = oneRow ? flexible * 0.24f : area.right() - x;
    if (ui::checkbox(g, widgetId("auction.usable"), {x, y, usableW, fieldH}, text::get("USABLE_ITEMS"),
                     usable)) {
        result.button = kAuctionUsable;
    }
    x += usableW + gap;
    if (oneRow) {
        if (ui::button(g, widgetId("auction.search"), {x, y, area.right() - x, fieldH}, text::get("SEARCH"),
                       !v.pending)) {
            result.button = kAuctionSearch;
        }
        return fieldH + gap;
    }
    return labelH + fieldH * 2.0f + gap * 2.0f;
}

// The footer of the browse and bid lists: pages, then what the player offers and Bid / Buyout.
void auctionFooter(DialogContext& c, Gui& g, const Rect& footer, const AuctionView& v,
                   AuctionResult& result) {
    const Metrics m = kit::metrics(c);
    const f32 gap = g.px(6.0f);
    const f32 buttonW = std::min(g.px(110.0f), footer.w * 0.25f);
    // The bid first, wide enough to read its three parts; the pages in what is left.
    const f32 moneyMax = g.px(300.0f);
    f32 x = footer.x;
    if (v.tab == 0) {
        char page[32];
        const std::string_view pageText = text::format(page, "PAGE_NUMBER", static_cast<int>(v.page + 1));
        const f32 pageW = g.draw.measure(pageText, ui::FontId::Friz, m.small) + g.px(16.0f);
        const f32 navRoom =
            (footer.w - buttonW * 2.0f - std::min(moneyMax, footer.w * 0.4f) - pageW - gap * 6.0f) * 0.5f;
        // Retouches (windows): BrowsePrevPageButton / BrowseNextPageButton: the spell book's page arrows,
        // PREV right of the first, NEXT left of the second (GameFontNormal); the arrows alone when the words
        // do not fit.
        const f32 arrow = footer.h;
        std::string_view prev = text::get("PREV"), next = text::get("NEXT");
        f32 navW = std::max(art::pageButtonWidth(g, arrow, prev, m.small),
                            art::pageButtonWidth(g, arrow, next, m.small));
        if (navW > navRoom) {
            prev = next = {};
            navW = arrow;
        }
        if (art::pageButton(g, widgetId("auction.prev"), {x, footer.y, navW, footer.h}, false,
                            v.page > 0 && !v.pending, prev, m.small)) {
            result.button = kAuctionPrevious;
        }
        x += navW + gap;
        caption(g, {x, footer.y, pageW, footer.h}, pageText, m.small, ui::kGold, Align::Center);
        x += pageW + gap;
        if (art::pageButton(g, widgetId("auction.next"), {x, footer.y, navW, footer.h}, true,
                            v.page + 1 < v.pages && !v.pending, next, m.small)) {
            result.button = kAuctionNext;
        }
        x += navW + gap * 2.0f;
    }
    const f32 moneyW = std::min(moneyMax, footer.right() - x - buttonW * 2.0f - gap * 3.0f);
    if (moneyW > g.px(120.0f)) {
        const i32 part = moneyInput(c, g, widgetId("auction.bid"), {x, footer.y, moneyW, footer.h},
                                    v.bidAmount, v.editing - 3);
        if (part >= 0) {
            result.field = 3 + part;
        }
    }
    const f32 bx = footer.right() - buttonW * 2.0f - gap;
    if (ui::button(g, widgetId("auction.bidButton"), {bx, footer.y, buttonW, footer.h}, text::get("BID"),
                   v.canBid && !v.busy)) {
        result.button = kAuctionBid;
    }
    if (ui::button(g, widgetId("auction.buyout"), {bx + buttonW + gap, footer.y, buttonW, footer.h},
                   text::get("BUYOUT"), v.canBuyout && !v.busy)) {
        result.button = kAuctionBuyout;
    }
}

// The player's sale: the item, prices, duration, deposit and "Create Auction".
void sellPanel(DialogContext& c, Gui& g, const Rect& area, const AuctionView& v, ItemDrag& drag,
               AuctionResult& result) {
    const Metrics m = kit::metrics(c);
    const AuctionSellView& s = v.sell;
    const ui::ScrollView view = ui::beginScroll(g, widgetId("auction.sell"), area);
    const f32 x = view.viewport.x + g.px(4.0f), w = view.viewport.w - g.px(8.0f);
    f32 y = view.top + g.px(2.0f);
    const f32 labelH = m.small * 1.3f;
    caption(g, {x, y, w, labelH}, text::get("AUCTION_ITEM"), m.small);
    y += labelH + g.px(2.0f);
    const Rect cell{x, y, std::min(w, g.px(340.0f)), m.item};
    namedCell(c, g, widgetId("auction.saleCell"), cell, s.cell, s.itemName, text::get("AUCTION_ITEM_TEXT"),
              drag, result.items);
    dropZone(drag, {x, y, w, m.item}, 1, result.items);
    y += m.item + g.px(8.0f);
    const f32 moneyW = std::min(w, g.px(260.0f));
    const f32 moneyH = m.button * 0.9f;
    caption(g, {x, y, w, labelH}, text::get("STARTING_PRICE"), m.small);
    y += labelH;
    i32 part = moneyInput(c, g, widgetId("auction.start"), {x, y, moneyW, moneyH}, s.startBid, v.editing - 6);
    if (part >= 0) {
        result.field = 6 + part;
    }
    y += moneyH + g.px(6.0f);
    caption(g, {x, y, w, labelH}, text::get("BUYOUT_PRICE"), m.small);
    y += labelH;
    part = moneyInput(c, g, widgetId("auction.buyoutPrice"), {x, y, moneyW, moneyH}, s.buyout, v.editing - 9);
    if (part >= 0) {
        result.field = 9 + part;
    }
    y += moneyH + g.px(6.0f);
    caption(g, {x, y, w, labelH}, text::get("AUCTION_DURATION"), m.small);
    y += labelH;
    char hours[3][16];
    std::array<std::string_view, 3> labels;
    static constexpr u32 kHours[3] = {2, 8, 24};
    const std::string_view unit = text::get("HOURS_P1");
    for (u32 i = 0; i < 3; ++i) {
        // "2 Hours", as the client's AuctionFrame builds it
        std::snprintf(hours[i], sizeof(hours[i]), "%u %.*s", kHours[i], static_cast<int>(unit.size()),
                      unit.data());
        labels[i] = hours[i];
    }
    const i32 duration =
        art::radioRow(g, widgetId("auction.duration"), {x, y, moneyW, moneyH}, labels, s.duration, m.small);
    if (duration >= 0) {
        result.duration = duration;
    }
    y += moneyH + g.px(8.0f);
    caption(g, {x, y, w * 0.4f, labelH}, text::get("DEPOSIT_COLON"), m.small);
    ui::money(g, x + g.draw.measure(text::get("DEPOSIT_COLON"), ui::FontId::Friz, m.small) + g.px(8.0f),
              y + labelH * 0.5f, s.deposit, m.small, Align::Left);
    y += labelH + g.px(8.0f);
    const f32 bw = std::min(w, g.px(200.0f));
    if (ui::button(g, widgetId("auction.create"), {x, y, bw, m.button}, text::get("CREATE_AUCTION"),
                   s.problem.empty() && !v.busy)) {
        result.button = kAuctionCreate;
    }
    y += m.button + g.px(4.0f);
    if (!s.problem.empty()) {
        TextStyle problem = ui::gameFont(m.small * 0.9f, rgba(255, 120, 100));
        y += g.draw.wrappedText({x, y, w, 1e6f}, s.problem, problem) + g.px(4.0f);
    }
    ui::endScroll(g, view, y - view.top);
}

} // namespace

std::span<const AuctionHeaderSpec> auctionHeaderSpecs(u32 tab) {
    using game::AuctionSort;
    static constexpr AuctionHeaderSpec kBrowse[] = {
        {"RARITY", AuctionSort::Quality, 214.0f, AuctionColumn::Item},
        {"LEVEL_ABBR", AuctionSort::Level, 57.0f, AuctionColumn::Level},
        {"CLOSES_IN", AuctionSort::Duration, 91.0f, AuctionColumn::TimeLeft},
        {"AUCTION_CREATOR", AuctionSort::Seller, 76.0f, AuctionColumn::Info},
        {"CURRENT_BID", AuctionSort::Bid, 163.0f, AuctionColumn::BidAndBuyout}};
    static constexpr AuctionHeaderSpec kBids[] = {{"RARITY", AuctionSort::Quality, 195.0f, AuctionColumn::Item},
                                                  {"LEVEL_ABBR", AuctionSort::Level, 65.0f, AuctionColumn::Level},
                                                  {"CLOSES_IN", AuctionSort::Duration, 79.0f, AuctionColumn::TimeLeft},
                                                  {"BUYOUT_PRICE", AuctionSort::Buyout, 162.0f, AuctionColumn::Buyout},
                                                  {"BID_STATUS", AuctionSort::Status, 94.0f, AuctionColumn::Info},
                                                  {"CURRENT_BID", AuctionSort::Bid, 169.0f, AuctionColumn::Bid}};
    static constexpr AuctionHeaderSpec kOwned[] = {
        {"RARITY", AuctionSort::Quality, 185.0f, AuctionColumn::Item},
        {"CLOSES_IN", AuctionSort::Duration, 100.0f, AuctionColumn::TimeLeft},
        {"HIGH_BIDDER", AuctionSort::Status, 108.0f, AuctionColumn::Info},
        {"CURRENT_BID", AuctionSort::Bid, 193.0f, AuctionColumn::BidAndBuyout}};
    return tab == 0 ? std::span<const AuctionHeaderSpec>(kBrowse)
           : tab == 1 ? std::span<const AuctionHeaderSpec>(kBids)
                      : std::span<const AuctionHeaderSpec>(kOwned);
}

AuctionResult auctionWindow(DialogContext& c, const AuctionView& v, ItemDrag& drag) {
    VD_PROFILE_ZONE("ExchangeWindows::auction");
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    AuctionResult result;
    const Dialog d = kit::beginDialog(c, g, "auction", wide ? 980.0f : 700.0f, wide ? 450.0f : 800.0f,
                                      v.title, true, true);
    result.closed = d.closed;
    const std::string_view tabs[] = {text::get("BROWSE"), text::get("BIDS"), text::get("AUCTIONS")};
    const f32 tabH = g.px(30.0f);
    result.tab = tabBar(g, "auction.tabs", {d.body.x, d.body.y, d.body.w, tabH}, tabs, v.tab, m.text);
    // The player's money, right of the tabs.
    ui::money(g, d.body.right() - g.px(4.0f), d.body.y + tabH * 0.5f, v.money, m.small, Align::Right);
    Rect body{d.body.x, d.body.y + tabH + g.px(6.0f), d.body.w, d.body.h - tabH - g.px(6.0f)};
    if (v.tab == 0) {
        const f32 filters = auctionFilters(c, g, body, v, result);
        body.y += filters;
        body.h -= filters;
        auctionList(c, g, body, v, drag, result);
        auctionFooter(c, g, d.footer, v, result);
        return result;
    }
    if (v.tab == 1) {
        auctionList(c, g, body, v, drag, result);
        auctionFooter(c, g, d.footer, v, result);
        return result;
    }
    // The player's auctions, and selling.
    Rect list, side;
    splitPanes(c, g, body, wide ? 0.4f : 0.38f, list, side);
    auctionList(c, g, list, v, drag, result);
    Rect sell, bags;
    if (wide) {
        sell = {side.x, side.y, side.w * 0.42f, side.h};
        bags = {sell.right() + g.px(6.0f), side.y, side.w - sell.w - g.px(6.0f), side.h};
    } else {
        sell = {side.x, side.y, side.w * 0.5f, side.h};
        bags = {sell.right() + g.px(6.0f), side.y, side.w - sell.w - g.px(6.0f), side.h};
    }
    sellPanel(c, g, sell, v, drag, result);
    bagsPane(c, g, "auction.bags", bags, v.bags, drag, result.items, 0);
    const f32 bw = std::min(d.footer.w * 0.4f, kit::buttonWidth(g, text::get("CANCEL_AUCTION"), d.footer.h));
    if (ui::button(g, widgetId("auction.cancel"), {d.footer.right() - bw, d.footer.y, bw, d.footer.h},
                   text::get("CANCEL_AUCTION"), v.canCancel && !v.busy)) {
        result.button = kAuctionCancel;
    }
    return result;
}

} // namespace vd
