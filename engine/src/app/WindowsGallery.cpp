#include "app/WindowsGallery.h"

#include "app/DialogKit.h"
#include "app/PopupStack.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <cstring>
#include <iterator>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;

void logResult(const char* what, i32 value) {
    VD_LOG_INFO("UI gallery: %s %d", what, value);
}

constexpr u64 icon(const char* name) {
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

constexpr const char* kPageNames[] = {"talents2", "mailinvoice", "mailinvoice2", "codpopup", "slots"};

// A tree with every shape of line (TalentFrame_DrawLines): {icon, tier, column, rank, max, available,
// prerequisite}. Two points to spend: the talents that could take one are lit, so are those with points; the
// rest grey.
const TalentButtonView kTalents[] = {
    {icon("Ability_Rogue_Ambush"), 0, 0, 3, 3, false},        // 0: full
    {icon("Ability_Parry"), 0, 1, 5, 5, false},               // 1: full
    {icon("Ability_Gouge"), 0, 2, 1, 3, true},                // 2: growing
    {icon("Ability_Warrior_Charge"), 1, 1, 0, 5, true, 1},    // 3: down from 1
    {icon("Spell_Nature_EnchantArmor"), 2, 2, 0, 3, true, 2}, // 4: down through an empty cell
    {icon("Ability_ThunderClap"), 2, 0, 0, 2, false, 0},      // 5: grey, through an empty cell
    {icon("INV_Sword_05"), 3, 1, 0, 1, false, 3},             // 6: grey, down through an empty cell
    {icon("Ability_BackStab"), 3, 3, 0, 2, false, 4},         // 7: across tier 2, then down (a corner)
    {icon("Ability_SearingArrow"), 4, 0, 0, 5, false, 5},     // 8
    {icon("INV_Axe_09"), 4, 3, 0, 3, false, 8}, // 9: across from the left (its arrow on the left)
    {icon("Ability_Warrior_SavageBlow"), 5, 2, 0, 1, false, 8}, // 10: across tier 4, then down (a tee)
    {icon("Ability_MeleeDamage"), 6, 1, 0, 1, false, 12},       // 11: across from the right (its arrow there)
    {icon("Ability_Warrior_Cleave"), 6, 2, 0, 3, false, 10},    // 12: down from 10
};
const TalentTreeView kTrees[] = {{"Arms", "WarriorArms", 9, kTalents},
                                 {"Fury", "WarriorFury", 0, {}},
                                 {"Protection", "WarriorProtection", 0, {}}};

ItemSlotView cell(const char* iconName, u32 count, u32 index) {
    ItemSlotView v;
    v.ref = {ItemRef::Kind::Window, 0, 0, index};
    v.empty = false;
    v.iconHash = iconName ? icon(iconName) : icon("INV_VanillaDroid_NoSuchIcon");
    v.count = count;
    return v;
}

} // namespace

bool WindowsGallery::select(const char* name) {
    for (u32 i = 0; i < std::size(kPageNames); ++i) {
        if (std::strcmp(name, kPageNames[i]) == 0) {
            m_page = static_cast<Page>(i);
            return true;
        }
    }
    return false;
}

void WindowsGallery::draw(DialogContext& c, u64 nowMs) {
    (void)nowMs;
    switch (m_page) {
    case Page::Talents2: drawTalents(c); break;
    case Page::MailInvoice: drawInvoice(c, false); break;
    case Page::MailInvoice2: drawInvoice(c, true); break;
    case Page::CodPopup: drawPopups(c); break;
    case Page::Slots: drawSlots(c); break;
    }
}

void WindowsGallery::drawTalents(DialogContext& c) {
    TalentView view;
    view.trees = kTrees;
    view.tree = 0;
    view.selected = m_talent;
    view.freePoints = 2;
    const TalentResult r = talentWindow(c, view);
    if (r.select >= 0) {
        m_talent = r.select;
        logResult("talents2 select", r.select);
    }
    if (r.tree >= 0) {
        logResult("talents2 tree", r.tree);
    }
}

void WindowsGallery::drawInvoice(DialogContext& c, bool buyer) {
    // The letters as ExchangeScreens lists them: the auction house's, the first one open.
    MailRowView rows[2];
    rows[0].sender = "Stormwind Auction House";
    rows[0].subject = buyer ? "Auction won: Copper Ore" : "Auction successful: Copper Ore";
    rows[0].expires = "29 d";
    rows[0].iconHash = icon(buyer ? "INV_Ore_Copper_01" : "INV_Misc_Coin_01");
    rows[0].money = buyer ? 0 : 4750;
    rows[0].unread = true;
    rows[1].sender = "Brakk";
    rows[1].subject = "Guild meeting";
    rows[1].expires = "2 d";
    rows[1].iconHash = icon("INV_Misc_Note_01");
    MailView v;
    v.tab = 0;
    v.loaded = true;
    v.rows = rows;
    v.selected = 0;
    v.read.sender = rows[0].sender;
    v.read.subject = rows[0].subject;
    v.read.canReply = false;
    v.read.canDelete = v.read.canCopy = true;
    v.read.invoice.shown = true;
    v.read.invoice.buyer = buyer;
    v.read.invoice.item = "Copper Ore";
    v.read.invoice.player = buyer ? "Galen" : "Aeryn";
    v.read.invoice.buyout = !buyer;
    v.read.invoice.bid = buyer ? 3200 : 5000;
    v.read.invoice.deposit = buyer ? 0 : 60;
    v.read.invoice.cut = buyer ? 0 : 310;
    if (buyer) {
        v.read.item = cell("INV_Ore_Copper_01", 20, 0);
        v.read.itemName = "Copper Ore";
        v.read.canTakeItem = true;
    } else {
        v.read.money = 4750;
        v.read.canTakeMoney = true;
    }
    const MailResult r = mailWindow(c, v, m_drag);
    if (r.button >= 0) {
        logResult("mail button", r.button);
    }
    m_drag.finish();
}

void WindowsGallery::drawPopups(DialogContext& c) {
    const PopupInfo& confirm = popupInfo(PopupKind::CodConfirmation);
    m_popups[0] = PopupView{};
    m_popups[0].text = text::get(confirm.text);
    m_popups[0].button1 = text::get(confirm.button1);
    m_popups[0].button2 = text::get(confirm.button2);
    m_popups[0].money = true;
    m_popups[0].copper = 1500;
    const PopupInfo& alert = popupInfo(PopupKind::CodAlert);
    m_popups[1] = PopupView{};
    m_popups[1].text = text::get(alert.text);
    m_popups[1].button1 = text::get(alert.button1);
    const PopupTaps taps = popupStack(c, m_popups, m_popupHeights);
    if (taps.slot >= 0) {
        logResult("popup button", static_cast<i32>(taps.slot * 10 + taps.button));
    }
}

void WindowsGallery::drawSlots(DialogContext& c) {
    ui::Gui g{c.ui, c.draw};
    const kit::Metrics m = kit::metrics(c);
    const kit::Dialog d = kit::beginDialog(c, g, "windows.slots", 640.0f, 300.0f, {}, true, false);
    // An icon; the question mark while one loads (or is unknown); locked, with and without its icon; a
    // merchant's sold out and not usable tints; a broken item (red as unusable); a stack.
    struct Sample {
        const char* icon;
        bool locked;
        u32 tint;
        bool usable;
        u32 count;
    };
    static constexpr Sample kSamples[] = {
        {"INV_Sword_05", false, 0xFFFFFFFFu, true, 0},
        {nullptr, false, 0xFFFFFFFFu, true, 0},
        {"INV_Sword_05", true, 0xFFFFFFFFu, true, 0},
        {nullptr, true, 0xFFFFFFFFu, true, 0},
        {"INV_Misc_Food_10", false, rgba(128, 128, 128), true, 0},
        {"INV_Axe_09", false, rgba(230, 0, 0), true, 0},
        {"INV_Axe_09", false, 0xFFFFFFFFu, false, 0},
        {"INV_Misc_Food_10", false, 0xFFFFFFFFu, true, 20},
    };
    const f32 step = m.item + g.px(16.0f);
    for (u32 i = 0; i < std::size(kSamples); ++i) {
        const Sample& s = kSamples[i];
        ui::SlotView slot;
        slot.icon = s.icon ? c.resources.icon(icon(s.icon)) : std::nullopt;
        slot.desaturated = s.locked;
        slot.tint = s.tint;
        slot.usable = s.usable;
        slot.count = s.count;
        const Rect r{d.body.x + g.px(12.0f) + static_cast<f32>(i) * step, d.body.y + g.px(16.0f), m.item,
                     m.item};
        if (ui::itemSlot(g, ui::widgetId("windows.slot", i), r, slot).clicked) {
            logResult("slot", static_cast<i32>(i));
        }
    }
    // Money frames: white, and red as SetMoneyFrameColor paints what cannot be paid.
    const f32 y = d.body.y + g.px(32.0f) + m.item + m.text;
    ui::money(g, d.body.x + g.px(12.0f), y, 123456, m.text, ui::Align::Left);
    ui::money(g, d.body.x + g.px(12.0f), y + m.text * 2.0f, 123456, m.text, ui::Align::Left, 1.0f, ui::kRed);
}

} // namespace vd
