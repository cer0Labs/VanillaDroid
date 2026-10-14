#include "app/Dialogs.h"

#include "app/DialogKit.h"
#include "app/QuestFrameLayout.h"
#include "app/QuestLogLayout.h"
#include "app/QualityGovernor.h"
#include "app/SettingsFile.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::WidgetId;
using ui::widgetId;
using namespace kit;

// Client art of the dialogs (FrameXML: GossipFrame, QuestFrameTemplates, LootFrame, ChatFrame).
constexpr u64 kAvailableQuestIcon = assetPathHash("Interface\\GossipFrame\\AvailableQuestIcon.blp");
constexpr u64 kActiveQuestIcon = assetPathHash("Interface\\GossipFrame\\ActiveQuestIcon.blp");
// GossipFrame.lua: "Interface\\GossipFrame\\<type>GossipIcon" by the option's icon (vMaNGOS GossipOptionIcon
// order).
constexpr u64 kGossipIcons[] = {
    assetPathHash("Interface\\GossipFrame\\GossipGossipIcon.blp"),       // 0 chat
    assetPathHash("Interface\\GossipFrame\\VendorGossipIcon.blp"),       // 1 vendor
    assetPathHash("Interface\\GossipFrame\\TaxiGossipIcon.blp"),         // 2 flight master
    assetPathHash("Interface\\GossipFrame\\TrainerGossipIcon.blp"),      // 3 trainer
    assetPathHash("Interface\\GossipFrame\\HealerGossipIcon.blp"),       // 4 spirit healer
    assetPathHash("Interface\\GossipFrame\\BinderGossipIcon.blp"),       // 5 innkeeper
    assetPathHash("Interface\\GossipFrame\\BankerGossipIcon.blp"),       // 6 banker
    assetPathHash("Interface\\GossipFrame\\PetitionGossipIcon.blp"),     // 7 guild petition
    assetPathHash("Interface\\GossipFrame\\TabardGossipIcon.blp"),       // 8 tabard vendor
    assetPathHash("Interface\\GossipFrame\\BattleMasterGossipIcon.blp"), // 9 battlemaster
};
// Retouches (quest windows): QuestFramePanelTemplate's art (in the atlas), the left slot's patch (streamed: big),
// the portrait's stand-ins (an object's book, a unit's image still on its way) and GossipTitleButtonTemplate's
// highlight.
constexpr u64 kGreetingArt[4] = {
    assetPathHash("Interface\\QuestFrame\\UI-QuestGreeting-TopLeft.blp"),
    assetPathHash("Interface\\QuestFrame\\UI-QuestGreeting-TopRight.blp"),
    assetPathHash("Interface\\QuestFrame\\UI-QuestGreeting-BotLeft.blp"),
    assetPathHash("Interface\\QuestFrame\\UI-QuestGreeting-BotRight.blp"),
};
constexpr u64 kBotLeftPatch = assetPathHash("Interface\\QuestFrame\\UI-Quest-BotLeftPatch.blp");
constexpr u64 kBookIcon = assetPathHash("Interface\\QuestFrame\\UI-QuestLog-BookIcon.blp");
constexpr u64 kMonsterPortrait = assetPathHash("Interface\\CharacterFrame\\TemporaryPortrait-Monster.blp");
constexpr u64 kTitleHighlight = assetPathHash("Interface\\QuestFrame\\UI-QuestTitleHighlight.blp");
constexpr u64 kChatBackground = assetPathHash("Interface\\ChatFrame\\ChatFrameBackground.blp");
constexpr u64 kChatButtonUp = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-Chat-Up.blp");
constexpr u64 kChatButtonDown = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-Chat-Down.blp");
constexpr u64 kChatBottomUp = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollEnd-Up.blp");
constexpr u64 kChatBottomDown = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollEnd-Down.blp");
constexpr u64 kItemNameFrame = assetPathHash("Interface\\QuestFrame\\UI-QuestItemNameFrame.blp");
constexpr u64 kCoinIcon = assetPathHash("Interface\\Icons\\INV_Misc_Coin_01.blp"); // an icon of textures.vdpk
// LootFrame (parity: the original art).
constexpr u64 kLootPanel = assetPathHash("Interface\\LootFrame\\UI-LootPanel.blp");
constexpr u64 kTargetDead = assetPathHash("Interface\\TargetingFrame\\TargetDead.blp");
constexpr u64 kButtonHighlight = assetPathHash("Interface\\Buttons\\ButtonHilight-Square.blp");
constexpr u64 kScrollUpUp = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollUp-Up.blp");
constexpr u64 kScrollUpDown = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollUp-Down.blp");
constexpr u64 kScrollDownUp = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Up.blp");
constexpr u64 kScrollDownDown = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Down.blp");
constexpr f32 kLootFrameScale = 1.25f; // the original's size, a little larger for fingers
constexpr u32 kLootButtons = 4;        // LOOTFRAME_NUMBUTTONS

constexpr usize kMaxChatLines = 256; // the chat shows the newest ones

// A button drawn with its own art (Up / Down textures), its touch area a little larger than the art.
bool artButton(Gui& g, ui::WidgetId id, const Rect& r, u64 up, u64 down) {
    const Rect target = r.inset(-r.w * 0.25f);
    const ui::WidgetState state = ui::hit(g, id, target);
    g.draw.sprite(r, ui::held(state, target) ? down : up);
    return ui::tapped(state);
}

} // namespace

// --- NPC conversation and quests: GossipFrame and QuestFrame ----------------------------------------------------

namespace {

// QuestFrame / GossipFrame drawn up to their scroll frame (with the 1.12's art).
struct QuestFrameDraw {
    QuestFrameLayout l;
    QuestArtSizes s;
    bool closed = false;
};

// The frame at the scale of the window buttons (the quest log's), in its place's top left (parity pass: panels; the
// original's left slot) or centered over the world: the portrait under the art's ring (SetPortraitTexture, or the quest
// log's book for an object), the four UI-QuestGreeting textures, UI-Quest-BotLeftPatch over the left button's slot when
// it has none (the greeting panels), the NPC's name (GameFontHighlight) and the close button.
QuestFrameDraw beginQuestFrame(DialogContext& c, Gui& g, std::string_view name, std::string_view npcName,
                               const NpcPortraitView& npc, bool leftButton) {
    QuestFrameDraw f;
    const f32 k = questLogScale(metrics(c).button);
    const bool panel = c.panel.w > 0.0f;
    if (!panel) {
        ui::modalBlocker(g, widgetId(name, 1), 0.3f);
    }
    f.l = questFrameLayout(panel ? c.panel : c.safeArea.inset(g.px(6.0f)), k,
                           !panel || c.panelAlign == PanelAlign::Center);
    f.s = questArtSizes(g, f.l.k);
    ui::hit(g, widgetId(name, 2), f.l.frame); // the fingers that miss its controls are its own
    g.draw.portrait(f.l.portrait, npc.portrait, npc.unit ? kMonsterPortrait : kBookIcon);
    const Rect& a = f.l.art;
    const f32 half = a.w * 256.0f / 384.0f;
    g.draw.sprite({a.x, a.y, half, a.h * 0.5f}, kGreetingArt[0]);
    g.draw.sprite({a.x + half, a.y, a.w - half, a.h * 0.5f}, kGreetingArt[1]);
    g.draw.sprite({a.x, a.y + a.h * 0.5f, half, a.h * 0.5f}, kGreetingArt[2]);
    g.draw.sprite({a.x + half, a.y + a.h * 0.5f, a.w - half, a.h * 0.5f}, kGreetingArt[3]);
    if (!leftButton) {
        if (const TextureHandle patch = c.resources.texture(kBotLeftPatch); patch.valid()) {
            g.draw.texture(f.l.patch, patch);
        }
    }
    TextStyle title = ui::gameFont(f.s.normal, ui::kWhite);
    title.align = Align::Center;
    ui::label(g, f.l.title, npcName, title);
    f.closed = ui::closeButton(g, widgetId(name, 3), f.l.close);
    return f;
}

// The scroll child's text column: the title 5 in and 10 down (QuestTitleText), the texts 275 units wide.
Flow questFrameFlow(Gui& g, const QuestFrameDraw& f, const ui::ScrollView& view) {
    return {&g, view.viewport.x + f.s.u(5.0f), f.s.u(275.0f), view.top + f.s.u(10.0f)};
}

// The scroll frame's end: QuestSpacerFrame's 25 units under the last line, and its scroll bar.
bool endQuestFrameScroll(Gui& g, const QuestFrameDraw& f, const ui::ScrollView& view, f32 bottom) {
    ui::endScroll(g, view, bottom - view.top + f.s.u(25.0f), false);
    return ui::panelScrollBar(g, view, f.l.scrollBar);
}

// GossipTitleButtonTemplate: the icon (16 x 16) at its top left, the text (QuestFont, 275 wide) 20 in, as high as the
// text and 2 more (GossipResize), a finger's height at least; UI-QuestTitleHighlight while pressed.
f32 gossipRow(Gui& g, WidgetId id, const QuestArtSizes& s, f32 x, f32 y, u64 icon, u32 tint, std::string_view text,
              bool& tapped) {
    const TextStyle style = ui::questFont(s.quest);
    const f32 textW = s.u(275.0f);
    const f32 textH = g.draw.measureWrapped(textW, text, style.font, style.size);
    const f32 h = std::max(textH + s.u(2.0f), s.row);
    const Rect r{x, y, s.u(300.0f), h};
    const ui::WidgetState state = ui::hit(g, id, r);
    if (ui::held(state, r)) {
        g.draw.sprite(r, kTitleHighlight);
    }
    const f32 textY = y + (h - textH) * 0.5f;
    const f32 firstLine = textY + g.draw.lineHeight(style.font, style.size) * 0.5f;
    g.draw.sprite({x, firstLine - s.u(8.0f), s.u(16.0f), s.u(16.0f)}, icon, tint);
    g.draw.wrappedText({x + s.u(20.0f), textY, textW, textH + 1.0f}, text, style);
    tapped = ui::tapped(state);
    return h;
}

} // namespace

GossipResult gossipDialog(DialogContext& c, const GossipView& v) {
    VD_PROFILE_ZONE("Dialogs::gossip");
    Gui g{c.ui, c.draw};
    GossipResult result;
    const QuestFrameDraw f = beginQuestFrame(c, g, "gossip", v.npcName, v.npc, false);
    const QuestArtSizes& s = f.s;
    const ui::ScrollView view = ui::beginScroll(g, widgetId("gossip.text"), f.l.scroll);
    // GossipGreetingText (QuestFont, 270 wide at 10, -10), then the title buttons 20 under it from the scroll child's
    // left: the quests to take, a line's gap, the quests taken, a gap, the options (GossipFrameUpdate).
    Flow flow{&g, view.viewport.x + s.u(10.0f), s.u(270.0f), view.top + s.u(10.0f)};
    flow.text(v.text, ui::questFont(s.quest), 0.0f);
    f32 y = flow.y + s.u(20.0f);
    const f32 x = view.viewport.x;
    for (const bool active : {false, true}) {
        bool group = false;
        for (usize i = 0; i < v.quests.size(); ++i) {
            const GossipQuestView& q = v.quests[i];
            if (q.active != active) {
                continue;
            }
            const u64 icon = q.active ? kActiveQuestIcon : kAvailableQuestIcon;
            const u32 tint = q.active && !q.completable ? rgba(150, 150, 150) : ui::kWhite;
            bool tapped = false;
            y += gossipRow(g, widgetId("gossip.quest", i), s, x, y, icon, tint, q.title, tapped);
            if (tapped) {
                result.quest = static_cast<i32>(i);
            }
            group = true;
        }
        if (group) {
            y += s.u(16.0f); // the hidden button after a group
        }
    }
    for (usize i = 0; i < v.options.size(); ++i) {
        const GossipOptionView& o = v.options[i];
        const u64 icon = kGossipIcons[o.icon < std::size(kGossipIcons) ? o.icon : 0];
        bool tapped = false;
        y += gossipRow(g, widgetId("gossip.option", i), s, x, y, icon, ui::kWhite, o.text, tapped);
        if (tapped) {
            result.option = static_cast<i32>(i);
        }
    }
    result.scrolled = endQuestFrameScroll(g, f, view, y);
    // GossipFrameGreetingGoodbyeButton in the right slot.
    const bool goodbye = ui::button(g, widgetId("gossip.goodbye"), f.l.rightButton, text::get("GOODBYE"));
    result.closed = f.closed || goodbye;
    return result;
}

QuestDetailsResult questDetailsDialog(DialogContext& c, const QuestDetailsView& v) {
    VD_PROFILE_ZONE("Dialogs::questDetails");
    Gui g{c.ui, c.draw};
    QuestDetailsResult result;
    const QuestFrameDraw f = beginQuestFrame(c, g, "details", v.npcName, v.npc, true);
    const QuestArtSizes& s = f.s;
    const TextStyle body = ui::questFont(s.quest);
    // QuestFrameDetailPanel: QuestTitleText, QuestDescription 5 under it, QUEST_OBJECTIVES 15 under that, the
    // objectives 5 under it, the rewards 15 under them.
    const ui::ScrollView view = ui::beginScroll(g, widgetId("details.text"), f.l.scroll);
    Flow flow = questFrameFlow(g, f, view);
    flow.title(v.title, s.questTitle, s.u(5.0f));
    flow.text(v.description, body, s.u(15.0f));
    flow.title(text::get("QUEST_OBJECTIVES"), s.questTitle, s.u(5.0f));
    flow.text(v.objectives, body, s.u(15.0f));
    QuestArtRewards rewards;
    rewards.choices = v.choices;
    rewards.rewards = v.rewards;
    rewards.chooseText = text::get("REWARD_CHOICES");
    rewards.money = v.money;
    questArtRewards(c, flow, s, "details.choice", rewards);
    result.scrolled = endQuestFrameScroll(g, f, view, flow.y);
    // QuestFrameAcceptButton in the left slot, QuestFrameDeclineButton in the right one.
    result.accept = ui::button(g, widgetId("details.accept"), f.l.accept, text::get("ACCEPT"));
    const bool decline = ui::button(g, widgetId("details.decline"), f.l.rightButton, text::get("DECLINE"));
    result.decline = decline || f.closed;
    return result;
}

QuestProgressResult questProgressDialog(DialogContext& c, const QuestProgressView& v) {
    VD_PROFILE_ZONE("Dialogs::questProgress");
    Gui g{c.ui, c.draw};
    QuestProgressResult result;
    const QuestFrameDraw f = beginQuestFrame(c, g, "progress", v.npcName, v.npc, true);
    const QuestArtSizes& s = f.s;
    // QuestFrameProgressPanel: QuestProgressTitleText, QuestProgressText 5 under it, TURN_IN_ITEMS 10 under that,
    // REQUIRED_MONEY (QuestFontNormalSmall) with its coins 10 right of it, the items two to a row (QuestProgressItem).
    const ui::ScrollView view = ui::beginScroll(g, widgetId("progress.text"), f.l.scroll);
    Flow flow = questFrameFlow(g, f, view);
    flow.title(v.title, s.questTitle, s.u(5.0f));
    flow.text(v.text, ui::questFont(s.quest), s.u(10.0f));
    if (!v.required.empty() || v.money > 0) {
        flow.title(text::get("TURN_IN_ITEMS"), s.questTitle, s.u(5.0f));
        if (v.money > 0) {
            flow.y += s.u(5.0f);
            const TextStyle style = ui::questFont(s.objective);
            const std::string_view label = text::get("REQUIRED_MONEY");
            const f32 h = s.objective * 1.4f;
            g.draw.text(flow.x, ui::centeredBaseline(g.draw, style, flow.y + h * 0.5f), label, style);
            ui::money(g, flow.x + g.draw.measure(label, style.font, style.size) + s.u(10.0f), flow.y + h * 0.5f,
                      v.money, s.objective, Align::Left);
            flow.y += h + s.u(10.0f);
        }
        questItems(c, flow, "progress.item", 0, v.required, -1, false, UnusableTint::None, s.k);
    }
    result.scrolled = endQuestFrameScroll(g, f, view, flow.y);
    // QuestFrameCompleteButton (CONTINUE, only when it can be completed) and QuestFrameGoodbyeButton (CANCEL).
    result.proceed =
        ui::button(g, widgetId("progress.continue"), f.l.leftButton, text::get("CONTINUE"), v.completable);
    const bool cancel = ui::button(g, widgetId("progress.cancel"), f.l.rightButton, text::get("CANCEL"));
    result.closed = cancel || f.closed;
    return result;
}

QuestRewardResult questRewardDialog(DialogContext& c, const QuestRewardView& v, i32& selectedChoice) {
    VD_PROFILE_ZONE("Dialogs::questReward");
    Gui g{c.ui, c.draw};
    QuestRewardResult result;
    if (selectedChoice >= static_cast<i32>(v.choices.size())) {
        selectedChoice = -1;
    }
    const QuestFrameDraw f = beginQuestFrame(c, g, "reward", v.npcName, v.npc, true);
    const QuestArtSizes& s = f.s;
    // QuestFrameRewardPanel: QuestRewardTitleText, QuestRewardText 5 under it, the rewards 10 under that (a choice is
    // tapped to choose it: QuestRewardItem_OnClick).
    const ui::ScrollView view = ui::beginScroll(g, widgetId("reward.text"), f.l.scroll);
    Flow flow = questFrameFlow(g, f, view);
    flow.title(v.title, s.questTitle, s.u(5.0f));
    flow.text(v.text, ui::questFont(s.quest), s.u(10.0f));
    QuestArtRewards rewards;
    rewards.choices = v.choices;
    rewards.rewards = v.rewards;
    rewards.chooseText = text::get("REWARD_CHOOSE");
    rewards.money = v.money;
    rewards.selected = selectedChoice;
    rewards.selectable = true;
    const i32 tapped = questArtRewards(c, flow, s, "reward.choice", rewards);
    if (tapped >= 0) {
        selectedChoice = tapped;
    }
    result.scrolled = endQuestFrameScroll(g, f, view, flow.y);
    // QuestFrameCompleteQuestButton and QuestFrameCancelButton.
    const bool canComplete = v.choices.empty() || selectedChoice >= 0;
    const bool complete =
        ui::button(g, widgetId("reward.complete"), f.l.leftButton, text::get("COMPLETE_QUEST"), canComplete);
    if (complete && canComplete) {
        result.complete = true;
        result.choice = v.choices.empty() ? -1 : selectedChoice;
    }
    const bool cancel = ui::button(g, widgetId("reward.cancel"), f.l.rightButton, text::get("CANCEL"));
    result.closed = cancel || f.closed;
    return result;
}

// --- Loot --------------------------------------------------------------------------------------------------

LootResult lootWindow(DialogContext& c, const LootView& v) {
    VD_PROFILE_ZONE("Dialogs::loot");
    Gui g{c.ui, c.draw};
    LootResult result;
    // LootFrame (LootFrame.xml): the 256-unit UI-LootPanel under the player frame (TOPLEFT 0, -104), not modal,
    // a little larger for fingers. The coins are its first slot, as LootSlotIsCoin.
    const f32 k = g.unit(1.0f) * kLootFrameScale;
    const Rect& safe = c.safeArea;
    const f32 frameX = safe.x;
    const f32 frameY = std::clamp(safe.y + 104.0f * k, safe.y, std::max(safe.y, safe.bottom() - 256.0f * k));
    const auto at = [&](f32 x, f32 y, f32 w, f32 h) { return Rect{frameX + x * k, frameY + y * k, w * k, h * k}; };
    // The art's hit area (HitRectInsets right 70) takes the fingers that land on it.
    ui::hit(g, widgetId("loot.frame"), at(0.0f, 0.0f, 186.0f, 256.0f));
    g.draw.sprite(at(10.0f, 8.0f, 58.0f, 58.0f), kTargetDead); // LootFramePortraitOverlay, under the panel
    g.draw.sprite(at(0.0f, 0.0f, 256.0f, 256.0f), kLootPanel);
    TextStyle title = ui::gameFont(12.0f * k, ui::kGold);
    title.align = Align::Center;
    g.draw.text(frameX + 116.0f * k, ui::centeredBaseline(g.draw, title, frameY + 26.0f * k), text::get("ITEMS"),
                title);
    result.closed = ui::closeButton(g, widgetId("loot.close"), at(175.0f - 16.0f, 26.0f - 16.0f, 32.0f, 32.0f));

    // LootFrame_Update: four buttons a page, three plus the page arrows when there are more.
    const u32 slots = static_cast<u32>(v.items.size()) + (v.money > 0 ? 1u : 0u);
    const u32 perPage = slots > kLootButtons ? kLootButtons - 1 : kLootButtons;
    const u32 pages = std::max(1u, (slots + perPage - 1) / perPage);
    const u32 page = std::min(v.page, pages - 1);
    for (u32 index = 0; index < perPage; ++index) {
        const u32 slot = page * perPage + index;
        if (slot >= slots) {
            break;
        }
        // LootButtonTemplate: a 37-unit item button at (24, 80) and every 41 below, its name frame from 30 units
        // in, the name (GameFontNormal in the quality's colour) in 93 x 38 right of the button.
        const Rect icon = at(24.0f, 80.0f + 41.0f * static_cast<f32>(index), 37.0f, 37.0f);
        g.draw.sprite(at(54.0f, 80.0f + 41.0f * static_cast<f32>(index) + 18.5f - 31.0f, 130.0f, 62.0f),
                      kItemNameFrame);
        const Rect name{icon.right() + 8.0f * k, icon.centerY() - 19.0f * k, 93.0f * k, 38.0f * k};
        const Rect row{icon.x, icon.y, name.right() - icon.x, icon.h};
        const bool coins = v.money > 0 && slot == 0;
        const LootItemView* item = coins ? nullptr : &v.items[slot - (v.money > 0 ? 1u : 0u)];
        ui::SlotView view;
        view.icon = c.resources.icon(coins ? kCoinIcon : item->item.iconHash);
        view.count = coins ? 0 : item->item.count;
        view.quality = coins ? 1 : item->item.quality;
        ui::itemSlot(g, widgetId("loot.icon", slot), icon, view);
        const ui::WidgetState state = ui::hit(g, widgetId("loot.slot", slot), row);
        if (ui::held(state, row)) {
            g.draw.sprite(icon, kButtonHighlight); // the item button's highlight while pressed
        }
        if (coins) {
            ui::money(g, name.x, name.centerY(), v.money, 12.0f * k, Align::Left);
            result.takeMoney = ui::tapped(state);
            continue;
        }
        TextStyle style = ui::gameFont(12.0f * k, ui::qualityColor(item->item.quality));
        const f32 height = g.draw.measureWrapped(name.w, item->item.name, style.font, style.size);
        g.draw.wrappedText({name.x, name.centerY() - std::min(height, name.h) * 0.5f, name.w, name.h + 1.0f},
                           item->item.name, style);
        if (!item->note.empty()) { // phase 8: a group's item the player cannot take now
            g.draw.solid(row, rgba(0, 0, 0, 110));
            TextStyle note = ui::gameFont(9.0f * k, rgba(255, 140, 90));
            note.align = Align::Right;
            ui::label(g, {name.x, name.bottom() - 11.0f * k, name.w, 11.0f * k}, item->note, note);
        }
        if (state.longPress && item->item.entry != 0) { // the client's tooltip on hover
            requestItemTooltip(item->item.entry, row);
        } else if (ui::tapped(state)) {
            result.take = static_cast<i32>(slot - (v.money > 0 ? 1u : 0u));
            result.takeRect = row;
        }
    }
    if (slots == 0) {
        TextStyle empty = ui::gameFont(12.0f * k, ui::kDisabledText);
        ui::label(g, at(24.0f, 80.0f, 150.0f, 37.0f), text::get("VD_LOOT_NOTHING"), empty);
    }
    // The pages: LootFrameUpButton (BOTTOMLEFT 25, 16) with "Prev", LootFrameDownButton 86 units right of it with
    // "Next".
    if (pages > 1) {
        TextStyle label = ui::gameFont(12.0f * k, ui::kGold);
        if (page > 0) {
            if (artButton(g, widgetId("loot.prev"), at(25.0f, 208.0f, 32.0f, 32.0f), kScrollUpUp, kScrollUpDown)) {
                result.page = static_cast<i32>(page) - 1;
            }
            g.draw.text(frameX + 57.0f * k, frameY + 229.0f * k, text::get("PREV"), label);
        }
        if (page + 1 < pages) {
            if (artButton(g, widgetId("loot.next"), at(143.0f, 208.0f, 32.0f, 32.0f), kScrollDownUp,
                               kScrollDownDown)) {
                result.page = static_cast<i32>(page) + 1;
            }
            label.align = Align::Right;
            g.draw.text(frameX + 142.0f * k, frameY + 229.0f * k, text::get("NEXT"), label);
        }
    }
    // Everything at once is the Auto Loot option's (app/GameScreens), no button here.
    return result;
}

// --- Chat --------------------------------------------------------------------------------------------------

ChatResult chatWindow(DialogContext& c, const Rect& area, const ChatView& v) {
    VD_PROFILE_ZONE("Dialogs::chat");
    Gui g{c.ui, c.draw};
    const bool wide = c.profile == ScreenProfile::Wide;
    ChatResult result;
    const f32 size = g.px(wide ? 13.5f : 14.5f);
    const f32 buttonSize = g.px(36.0f);
    TextStyle style = ui::chatFont(size);

    // The chat button (ChatFrameMenuButton art): expands / collapses.
    const Rect toggle{area.right() - buttonSize, area.y, buttonSize, buttonSize};
    auto lines = [&](const Rect& box, bool scrollable) {
        // Newest at the bottom, each line measured once (wrapped); only those inside the box are drawn.
        if (!scrollable) {
            g.draw.pushClip(box);
            f32 y = box.bottom();
            for (usize i = v.lines.size(); i-- > 0 && y > box.y;) {
                const ChatLineView& line = v.lines[i];
                const f32 h = g.draw.measureWrapped(box.w, line.text, style.font, style.size);
                y -= h;
                style.color = line.color;
                g.draw.wrappedText({box.x, y, box.w, h + 1.0f}, line.text, style);
            }
            g.draw.popClip();
            return;
        }
        const usize first = v.lines.size() > kMaxChatLines ? v.lines.size() - kMaxChatLines : 0;
        const usize count = v.lines.size() - first;
        const f32 width = box.w - g.px(6.0f); // clear of the scroll indicator
        std::array<f32, kMaxChatLines> heights;
        f32 total = 0.0f;
        for (usize i = 0; i < count; ++i) {
            heights[i] = g.draw.measureWrapped(width, v.lines[first + i].text, style.font, style.size);
            total += heights[i];
        }
        const ui::ScrollView view = ui::beginScroll(g, widgetId("chat.lines"), box, true);
        f32 y = view.top + std::max(0.0f, box.h - total); // a few lines sit at the bottom
        for (usize i = 0; i < count; ++i) {
            const ChatLineView& line = v.lines[first + i];
            if (y + heights[i] >= box.y && y <= box.bottom()) {
                style.color = line.color;
                g.draw.wrappedText({box.x, y, width, heights[i] + 1.0f}, line.text, style);
            }
            y += heights[i];
        }
        ui::endScroll(g, view, y - view.top);
    };

    if (!v.expanded) {
        // Collapsed: the last lines over the world (no target: fingers there still move the camera) and the
        // button.
        lines({area.x, area.y, area.w - buttonSize - g.px(4.0f), area.h}, false);
        const ui::WidgetState state = ui::hit(g, widgetId("chat.toggle"), toggle);
        const bool down = ui::held(state, toggle);
        g.draw.sprite(toggle, down ? kChatButtonDown : kChatButtonUp);
        result.toggleExpanded = ui::tapped(state);
        return result;
    }

    // Expanded: ChatFrameBackground, the scrolling lines, then the input row (channel, text box, send).
    ui::hit(g, widgetId("chat.area"), area); // the world stays out of it
    g.draw.sprite(area, kChatBackground, rgba(0, 0, 0, 140));
    const f32 inputH = g.px(40.0f), pad = g.px(6.0f);
    const Rect input{area.x + pad, area.bottom() - inputH - pad, area.w - 2.0f * pad, inputH};
    lines({area.x + pad, area.y + pad, area.w - buttonSize - 2.0f * pad, input.y - pad - area.y - pad}, true);

    const ui::WidgetState state = ui::hit(g, widgetId("chat.toggle"), toggle);
    const bool down = ui::held(state, toggle);
    g.draw.sprite(toggle, down ? kChatBottomDown : kChatBottomUp);
    result.toggleExpanded = ui::tapped(state);

    const std::string_view channel = v.channel.empty() ? text::get("CHAT_MSG_SAY") : v.channel;
    const std::string_view send = text::get("SEND_LABEL");
    const f32 channelW =
        std::max(g.px(76.0f), g.draw.measure(channel, ui::FontId::Friz, g.px(14.0f)) + g.px(34.0f));
    const f32 sendW = std::max(g.px(90.0f), buttonWidth(g, send, inputH) * 0.8f);
    result.cycleChannel =
        ui::button(g, widgetId("chat.channel"), {input.x, input.y, channelW, inputH}, channel);
    result.tappedInput =
        ui::textField(g, widgetId("chat.input"),
                      {input.x + channelW + pad, input.y, input.w - channelW - sendW - 2.0f * pad, inputH},
                      v.input, text::get("VD_CHAT_INPUT_HINT"), v.editing);
    result.send = ui::button(g, widgetId("chat.send"), {input.right() - sendW, input.y, sendW, inputH}, send,
                             !v.input.empty());
    return result;
}

// --- Menu, settings, confirmation --------------------------------------------------------------------------

MenuChoice gameMenu(DialogContext& c, bool logoutEnabled) {
    VD_PROFILE_ZONE("Dialogs::gameMenu");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    // GameMenuFrame: a column under the MAIN_MENU header. Phase 15 (options): its buttons as 1.12 has them, video,
    // sound and interface options, macros (key bindings have no touch equivalent: there are no keys to bind). Parity
    // pass: the character's windows moved to the micro menu (app/MicroMenu), skills and reputation to the character
    // window's tabs.
    static constexpr text::Key kLabels[] = {"VIDEOOPTIONS_MENU", "SOUNDOPTIONS_MENU", "UIOPTIONS_MENU", "MACROS",
                                            "LOGOUT",            "EXIT_GAME",         "RETURN_TO_GAME"};
    constexpr MenuChoice kChoices[] = {MenuChoice::Settings,  MenuChoice::SoundOptions, MenuChoice::UiOptions,
                                       MenuChoice::Macros,    MenuChoice::Logout,       MenuChoice::Quit,
                                       MenuChoice::Resume};
    constexpr u32 kCount = sizeof(kChoices) / sizeof(kChoices[0]);
    const f32 gap = g.px(c.profile == ScreenProfile::Wide ? 4.0f : 10.0f);
    const f32 buttonH = std::max(m.button, g.px(c.profile == ScreenProfile::Wide ? 34.0f : 40.0f));
    const f32 columnH = kCount * buttonH + (kCount - 1) * gap + gap; // Return to Game a little apart, as there
    const f32 heightDp = columnH / c.ui.dp() + 60.0f;
    const Dialog d = beginDialog(c, g, "gameMenu", 300.0f, heightDp, text::get("MAIN_MENU"), false, false);
    MenuChoice choice = d.outside ? MenuChoice::Resume : MenuChoice::None;
    const f32 width = std::min(d.body.w - g.px(8.0f), g.px(220.0f));
    const f32 left = d.body.centerX() - width * 0.5f;
    f32 y = d.body.y + std::max(0.0f, (d.body.h - columnH) * 0.5f);
    for (u32 i = 0; i < kCount; ++i) {
        y += i + 1 == kCount ? gap : 0.0f;
        // Parity (popups): Logout and Exit wait while a CAMP / QUIT countdown runs, as in GameMenuFrame.
        const bool enabled =
            logoutEnabled || (kChoices[i] != MenuChoice::Logout && kChoices[i] != MenuChoice::Quit);
        if (ui::button(g, widgetId("gameMenu.button", i), {left, y, width, buttonH}, text::get(kLabels[i]),
                       enabled)) {
            choice = kChoices[i];
        }
        y += buttonH + gap;
    }
    return choice;
}

bool settingsPanel(DialogContext& c, SettingsView& v) {
    VD_PROFILE_ZONE("Dialogs::settings");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    // Over the whole screen with UIOptionsFrame's art, as the interface and sound options (1.12's OptionsFrame was a
    // dialog): each column in an OptionFrameBoxTemplate box, the rows scrolling inside.
    const Dialog d = beginFullScreen(c, g, "settings", text::get("VIDEOOPTIONS_MENU"));
    const f32 gap = g.px(12.0f);
    const Rect inner = d.body.inset(g.px(4.0f), 0.0f);
    const bool twoColumns = inner.w >= g.px(600.0f);
    const f32 columnW = twoColumns ? (inner.w - gap * 2.0f) * 0.5f : inner.w;
    optionBox(g, {inner.x, d.body.y, columnW, d.body.h});
    if (twoColumns) {
        optionBox(g, {inner.x + columnW + gap * 2.0f, d.body.y, columnW, d.body.h});
    }

    const ui::ScrollView view = ui::beginScroll(g, widgetId("settings.scroll"), d.body.inset(0.0f, g.px(4.0f)));
    f32 y = view.top + g.px(8.0f);
    f32 x = inner.x + g.px(12.0f);
    const f32 w = columnW - g.px(24.0f);
    // OptionsSliderTemplate rows: the name and value above the bar. True when the player moved it.
    auto sliderRow = [&](u64 index, std::string_view name, f32& value, f32 min, f32 max, f32 step,
                         std::string_view shown) {
        const f32 labelH = m.text * 1.5f;
        TextStyle nameStyle = ui::gameFont(m.text);
        ui::label(g, {x, y, w * 0.7f, labelH}, name, nameStyle);
        TextStyle valueStyle = ui::gameFont(m.text, ui::kWhite);
        valueStyle.align = Align::Right;
        ui::label(g, {x + w * 0.7f, y, w * 0.3f, labelH}, shown, valueStyle);
        const bool moved = ui::slider(g, widgetId("settings.slider", index),
                                      {x + g.px(4.0f), y + labelH, w - g.px(8.0f), g.px(40.0f)}, value, min, max, step);
        y += labelH + g.px(40.0f) + g.px(10.0f);
        return moved;
    };
    const f32 checkH = g.px(44.0f);
    auto checkRow = [&](u64 index, std::string_view name, bool& value) {
        ui::checkbox(g, widgetId("settings.check", index), {x + g.px(4.0f), y, w, checkH}, name, value);
        y += checkH + g.px(6.0f);
    };
    char shown[32];

    // Video options (phase 10): a preset, then its four knobs; moving one of them makes the choice Custom.
    f32 preset = static_cast<f32>(v.graphicsQuality < kQualityPresets ? v.graphicsQuality : 2u);
    if (sliderRow(9, text::get("VD_SETTINGS_QUALITY"), preset, 0.0f, static_cast<f32>(kQualityPresets - 1), 1.0f,
                  text::get(qualityPresetKey(static_cast<QualityPreset>(v.graphicsQuality))))) {
        applyQualityPreset(v, static_cast<u32>(preset + 0.5f));
    }
    bool knobMoved = false;
    std::snprintf(shown, sizeof(shown), "%.0f%%", v.renderScale * 100.0f);
    knobMoved |= sliderRow(10, text::get("VD_SETTINGS_RENDER_SCALE"), v.renderScale, 0.5f, 1.0f, 0.05f, shown);
    text::format(shown, "VD_SETTINGS_YARDS", v.viewDistance);
    knobMoved |= sliderRow(2, text::get("FARCLIP"), v.viewDistance, 200.0f, 1600.0f, 50.0f, shown);
    std::snprintf(shown, sizeof(shown), "%.0f%%", v.environmentDetail * 100.0f);
    knobMoved |= sliderRow(11, text::get("ENVIRONMENT_DETAIL"), v.environmentDetail, 0.5f, 1.5f, 0.1f, shown);
    std::snprintf(shown, sizeof(shown), "%.0f%%", v.spellDetail * 100.0f);
    knobMoved |= sliderRow(12, text::get("SPELL_DETAIL"), v.spellDetail, 0.25f, 1.0f, 0.05f, shown);
    // Phase 11: the client's Weather Intensity slider (0..3).
    f32 weather = static_cast<f32>(std::min(v.weatherDensity, 3u));
    std::snprintf(shown, sizeof(shown), "%u", std::min(v.weatherDensity, 3u));
    if (sliderRow(13, text::get("WEATHER_DETAIL"), weather, 0.0f, 3.0f, 1.0f, shown)) {
        v.weatherDensity = static_cast<u32>(weather + 0.5f);
        knobMoved = true;
    }
    // Phase 14 (grass): the ground clutter (0..3); 1.12 has no such option (its frillDensity stays at 16).
    {
        constexpr const char* kClutterKeys[] = {"NONE", "LOW", "VD_GROUND_CLUTTER_NORMAL", "HIGH"};
        const u32 level = std::min(v.groundClutter, 3u);
        f32 clutter = static_cast<f32>(level);
        if (sliderRow(14, text::get("VD_GROUND_CLUTTER"), clutter, 0.0f, 3.0f, 1.0f, text::get(kClutterKeys[level]))) {
            v.groundClutter = static_cast<u32>(clutter + 0.5f);
            knobMoved = true;
        }
    }
    // Phase 18 (world): OptionsFrame's ANISOTROPIC slider (1, 2, 4, 8, 16) and MULTISAMPLE drop-down (a slider over the
    // sample counts here: the touch control the other knobs use), as far as the GPU goes.
    {
        constexpr u32 kAnisotropy[] = {1, 2, 4, 8, 16};
        u32 steps = 0;
        while (steps + 1 < std::size(kAnisotropy) && kAnisotropy[steps + 1] <= v.maxAnisotropy) {
            ++steps;
        }
        u32 index = 0;
        while (index < steps && kAnisotropy[index] < v.anisotropy) {
            ++index;
        }
        f32 level = static_cast<f32>(index);
        std::snprintf(shown, sizeof(shown), "%ux", kAnisotropy[index]);
        if (steps > 0 && sliderRow(15, text::get("ANISOTROPIC"), level, 0.0f, static_cast<f32>(steps), 1.0f, shown)) {
            v.anisotropy = kAnisotropy[static_cast<u32>(level + 0.5f)];
            knobMoved = true;
        }
        u32 sampleSteps = 0;
        while ((2u << sampleSteps) <= v.maxMultisample && sampleSteps < 3) {
            ++sampleSteps;
        }
        u32 sampleIndex = 0;
        while (sampleIndex < sampleSteps && (1u << sampleIndex) < v.multisample) {
            ++sampleIndex;
        }
        f32 samples = static_cast<f32>(sampleIndex);
        std::snprintf(shown, sizeof(shown), "%ux", 1u << sampleIndex); // 1.12's entries name bits too: too long here
        if (sampleSteps > 0 &&
            sliderRow(16, text::get("MULTISAMPLE"), samples, 0.0f, static_cast<f32>(sampleSteps), 1.0f, shown)) {
            v.multisample = 1u << static_cast<u32>(samples + 0.5f);
            knobMoved = true;
        }
    }
    if (knobMoved) {
        v.graphicsQuality = currentQualityPreset(v);
    }
    checkRow(8, text::get("VD_SETTINGS_DYNAMIC_QUALITY"), v.dynamicQuality);
    checkRow(9, text::get("VD_SETTINGS_LIMIT_FPS"), v.limitFps);
    checkRow(10, text::get("FULL_SCREEN_GLOW"), v.fullScreenGlow); // phase 11: the client's options
    checkRow(11, text::get("DEATH_EFFECT"), v.deathEffect);
    checkRow(21, text::get("TERRAIN_HIGHLIGHTS"), v.terrainHighlights); // phase 11: specular terrain and water
    f32 bottom = y;
    if (twoColumns) {
        x = inner.x + columnW + gap * 2.0f + g.px(12.0f);
        y = view.top + g.px(8.0f);
    }
    std::snprintf(shown, sizeof(shown), "%.0f%%", v.uiScale * 100.0f);
    sliderRow(0, text::get("UI_SCALE"), v.uiScale, 0.75f, 1.5f, 0.05f, shown);
    checkRow(1, text::get("VD_SETTINGS_SHOW_FPS"), v.showFps);
    // Phase 15 (options): the interface's and the sound's options are their own windows now (UIOptionsFrame,
    // SoundOptionsFrame: app/OptionsScreens), as GameMenuFrame has them; this is OptionsFrame (video).
    bottom = std::max(bottom, y);
    ui::endScroll(g, view, bottom - view.top);

    const std::string_view closeText = text::get("CLOSE");
    const f32 buttonW = std::min(d.footer.w, buttonWidth(g, closeText, d.footer.h));
    const bool close = ui::button(g, widgetId("settings.close"),
                                  {d.footer.right() - buttonW, d.footer.y, buttonW, d.footer.h}, closeText);
    return close || d.closed;
}

ConfirmResult confirmDialog(DialogContext& c, std::string_view text, std::string_view yes,
                            std::string_view no) {
    VD_PROFILE_ZONE("Dialogs::confirm");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    // StaticPopup: centered white text over two buttons, the panel as tall as the text.
    TextStyle style = ui::gameFont(g.px(16.0f), ui::kWhite);
    style.align = Align::Center;
    const f32 widthDp = 420.0f;
    const f32 textW = std::min(g.px(widthDp), c.safeArea.w - g.px(12.0f)) - g.unit(32.0f) - g.px(16.0f);
    const f32 textH = g.draw.measureWrapped(textW, text, style.font, style.size);
    const f32 heightDp = (textH + m.button) / c.ui.dp() + 70.0f;
    const Dialog d = beginDialog(c, g, "confirm", widthDp, heightDp, {}, false);
    ConfirmResult result;
    const ui::ScrollView view = ui::beginScroll(g, widgetId("confirm.text"), d.body);
    const f32 top = view.top + std::max(g.px(8.0f), (d.body.h - textH) * 0.5f);
    const f32 used =
        g.draw.wrappedText({d.body.centerX() - textW * 0.5f, top, textW, textH + 1.0f}, text, style);
    ui::endScroll(g, view, top - view.top + used + g.px(8.0f));
    const f32 buttonW = std::min((d.footer.w - g.px(12.0f)) * 0.5f,
                                 std::max(buttonWidth(g, yes, d.footer.h), buttonWidth(g, no, d.footer.h)));
    const f32 x = d.footer.centerX();
    if (no.empty()) { // a single answer (StaticPopup with one button), centered
        result.yes = ui::button(g, widgetId("confirm.yes"), {x - buttonW * 0.5f, d.footer.y, buttonW, d.footer.h}, yes);
        return result;
    }
    result.yes = ui::button(g, widgetId("confirm.yes"),
                            {x - g.px(6.0f) - buttonW, d.footer.y, buttonW, d.footer.h}, yes);
    result.no = ui::button(g, widgetId("confirm.no"), {x + g.px(6.0f), d.footer.y, buttonW, d.footer.h}, no);
    return result;
}

} // namespace vd
