#include "app/QuestWindows.h"

#include "app/ChatLinkInsert.h"
#include "app/DialogKit.h"
#include "app/QuestLogLayout.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/ChatLinks.h"
#include "game/SocialText.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace vd {
namespace {

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::TextStyle;
using ui::widgetId;
using namespace kit;

// QuestLogFrame's art (QuestLogFrame.xml): the frame's four textures and EmptyQuestLogFrame's, in QuestLogTexture's
// order (streamed from textures.vdpk: big pictures), and the parts in the atlas.
constexpr u64 kQuestLogArt[kQuestLogTextures] = {
    assetPathHash("Interface\\QuestFrame\\UI-QuestLog-TopLeft.blp"),
    assetPathHash("Interface\\QuestFrame\\UI-QuestLog-TopRight.blp"),
    assetPathHash("Interface\\QuestFrame\\UI-QuestLog-BotLeft.blp"),
    assetPathHash("Interface\\QuestFrame\\UI-QuestLog-BotRight.blp"),
    assetPathHash("Interface\\QuestFrame\\UI-QuestLog-Empty-TopLeft.blp"),
    assetPathHash("Interface\\QuestFrame\\UI-QuestLog-Empty-TopRight.blp"),
    assetPathHash("Interface\\QuestFrame\\UI-QuestLog-Empty-BotLeft.blp"),
    assetPathHash("Interface\\QuestFrame\\UI-QuestLog-Empty-BotRight.blp"),
};
constexpr u64 kBookIcon = assetPathHash("Interface\\QuestFrame\\UI-QuestLog-BookIcon.blp");
constexpr u64 kInputBorder = assetPathHash("Interface\\Common\\Common-Input-Border.blp"); // QuestLogCount*
constexpr u64 kQuestLogHighlight = assetPathHash("Interface\\QuestFrame\\UI-QuestLogTitleHighlight.blp");
constexpr u64 kSortTabLeft = assetPathHash("Interface\\QuestFrame\\UI-QuestLogSortTab-Left.blp");
constexpr u64 kSortTabMiddle = assetPathHash("Interface\\QuestFrame\\UI-QuestLogSortTab-Middle.blp");
constexpr u64 kSortTabRight = assetPathHash("Interface\\QuestFrame\\UI-QuestLogSortTab-Right.blp");
constexpr u64 kRadioButton = assetPathHash("Interface\\Buttons\\UI-RadioButton.blp");
constexpr u64 kCheck = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check.blp");
constexpr u64 kPlusUp = assetPathHash("Interface\\Buttons\\UI-PlusButton-Up.blp");
constexpr u64 kMinusUp = assetPathHash("Interface\\Buttons\\UI-MinusButton-Up.blp");
constexpr u64 kPlusHighlight = assetPathHash("Interface\\Buttons\\UI-PlusButton-Hilight.blp");
constexpr u64 kDialogHeader =
    assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Header.blp"); // QuestTimerHeader

constexpr u32 kMaxQuests = 20; // MAX_QUESTLOG_QUESTS
// QuestDifficultyColor["header"], and the log's objective colors (SetTextColor in
// QuestLog_UpdateQuestDetails).
constexpr u32 kHeaderColor = rgba(178, 178, 178);
constexpr u32 kObjectiveColor = rgba(0, 0, 0);
constexpr u32 kObjectiveDoneColor = rgba(51, 51, 51);
// QuestWatch_Update: a title gold-brown until its objectives are met (then NORMAL_FONT_COLOR), objectives 0.8
// grey until met (then HIGHLIGHT_FONT_COLOR).
constexpr u32 kWatchTitleColor = rgba(191, 156, 0);
constexpr u32 kWatchObjectiveColor = rgba(204, 204, 204);

// "(text)" for a title's tag.
std::string_view tagText(std::span<char> out, std::string_view word) {
    const int n = std::snprintf(out.data(), out.size(), "(%.*s)", static_cast<int>(word.size()), word.data());
    return {out.data(), static_cast<usize>(std::clamp(n, 0, static_cast<int>(out.size()) - 1))};
}

// A rectangle grown to the smallest touch target.
Rect touchTarget(const Gui& g, const Rect& r) {
    const f32 min = g.px(ui::kTouchDp);
    Rect out = r;
    if (out.w < min) {
        out.x -= (min - out.w) * 0.5f;
        out.w = min;
    }
    if (out.h < min) {
        out.y -= (min - out.h) * 0.5f;
        out.h = min;
    }
    return out;
}

// QuestLogFrame's sizes at the art's scale (kit::QuestArtSizes); its title a little bigger than its GameFontHighlight
// (easier to read on a phone).
f32 questLogTitleSize(const Gui& g, f32 k) {
    return std::max(13.0f * k, g.px(10.0f));
}

// The frame: the portrait under the art (its BACKGROUND layer), the art's pieces, and over the boxes of an empty log
// EmptyQuestLogFrame's cobwebs and book. While the textures are on their way (a frame or two after opening), a dark
// plate.
void questLogArt(DialogContext& c, Gui& g, const QuestLogLayout& l, bool empty) {
    std::array<TextureHandle, kQuestLogTextures> art{};
    bool frameReady = true;
    for (u32 i = 0; i < kQuestLogTextures; ++i) {
        if (i < 4 || empty) {
            art[i] = c.resources.texture(kQuestLogArt[i]);
        }
        frameReady = frameReady && (i >= 4 || art[i].valid());
    }
    if (!frameReady) {
        g.draw.solid(l.frame.inset(4.0f * l.k), rgba(12, 12, 12, 230));
        return;
    }
    g.draw.sprite(l.portrait, kBookIcon);
    for (u32 i = 0; i < l.pieceCount; ++i) {
        const QuestLogPiece& p = l.pieces[i];
        g.draw.texture(p.dst, art[static_cast<u32>(p.texture)], ui::kWhite, p.uv);
    }
    if (!empty) {
        return;
    }
    for (u32 i = 0; i < l.emptyPieceCount; ++i) {
        const QuestLogPiece& p = l.emptyPieces[i];
        if (const TextureHandle t = art[static_cast<u32>(p.texture)]; t.valid()) {
            g.draw.texture(p.dst, t, ui::kWhite, p.uv);
        }
    }
}

// The title, the close button and the header: the "All" tab (QuestLogExpandButtonFrame, hidden in an empty log), the
// Track Quest indicator (QuestLogTrack) and "Quests: n/20" in its box (QuestLogQuestCount).
void questLogHeader(Gui& g, const QuestLogLayout& l, const QuestArtSizes& s, const QuestLogPanelView& v,
                    QuestLogPanelResult& result) {
    {
        // QuestLogTitleText: GameFontHighlight (white).
        TextStyle style = ui::gameFont(questLogTitleSize(g, s.k), ui::kWhite);
        style.align = Align::Center;
        ui::label(g, l.title, text::get("QUEST_LOG"), style);
    }
    result.closed = ui::closeButton(g, widgetId("questLog.close"), l.close);

    if (!v.rows.empty()) {
        // The tab (UI-QuestLogSortTab-Left 8 x 32 six left of the button and eight above it, -Middle as wide as the
        // text + 24, -Right), the (+)/(-) 16 x 16 at 3 in the button (QuestLogTitleButtonTemplate's NormalTexture),
        // ALL at 20 (GameFontNormal). The whole tab takes the finger.
        const std::string_view all = text::get("ALL");
        const TextStyle style = ui::gameFont(s.normal);
        const f32 textW = g.draw.measure(all, style.font, style.size);
        const f32 middle = textW + s.u(24.0f);
        const Rect tab{l.expand.x - s.u(6.0f), l.expand.y - s.u(8.0f), middle + s.u(16.0f), s.u(32.0f)};
        g.draw.sprite({tab.x, tab.y, s.u(8.0f), tab.h}, kSortTabLeft);
        g.draw.sprite({tab.x + s.u(8.0f), tab.y, middle, tab.h}, kSortTabMiddle);
        g.draw.sprite({tab.x + s.u(8.0f) + middle, tab.y, s.u(8.0f), tab.h}, kSortTabRight);
        const Rect target = touchTarget(g, {tab.x, l.expand.y, tab.w, l.expand.h});
        const ui::WidgetState state = ui::hit(g, widgetId("questLog.all"), target);
        const Rect icon{l.expand.x + s.u(3.0f), l.expand.centerY() - s.u(16.0f) * 0.5f, s.u(16.0f), s.u(16.0f)};
        g.draw.sprite(icon, v.allCollapsed ? kPlusUp : kMinusUp);
        if (ui::held(state, target)) {
            g.draw.sprite(icon, kPlusHighlight);
        }
        g.draw.text(l.expand.x + s.u(20.0f), ui::centeredBaseline(g.draw, style, l.expand.centerY()), all, style);
        result.toggleAll = ui::tapped(state);
    }

    {
        // QuestLogTrack: the radio's ring and its dot (green while any quest is watched, red otherwise: OnLoad and
        // QuestWatch_Update), TRACK_QUEST right of it (GameFontHighlightSmall). The original only explains Shift +
        // click in its tooltip (its HitRectInsets reach 80 right); here a tap toggles the selected quest's watch.
        const std::string_view track = text::get("TRACK_QUEST");
        const TextStyle style = ui::gameFont(s.small, ui::kWhite);
        const f32 textW = g.draw.measure(track, style.font, style.size);
        g.draw.sprite(l.track, kRadioButton, ui::kWhite, {0.0f, 0.0f, 0.25f, 1.0f});
        g.draw.sprite(l.track, kRadioButton, v.anyWatched ? rgba(0, 255, 0) : rgba(255, 0, 0),
                      {0.25f, 0.0f, 0.5f, 1.0f});
        const Rect row{l.track.x, l.track.y, l.track.w + textW + s.u(4.0f), l.track.h};
        const Rect target = touchTarget(g, row);
        const ui::WidgetState state = ui::hit(g, widgetId("questLog.track"), target);
        if (ui::held(state, target)) {
            g.draw.solid(row, rgba(255, 255, 255, 40));
        }
        g.draw.text(l.track.right(), ui::centeredBaseline(g.draw, style, l.track.centerY()), track, style);
        result.track = ui::tapped(state) && v.selected >= 0;
    }

    {
        // QuestLogQuestCount (GameFontNormalSmall, "Quests: |cffffffff%d/%d|r") 6 in from the right of its box:
        // Common-Input-Border's ends (8 x 20) and its middle as wide as the text (QuestLogCountMiddle:SetWidth).
        char count[96];
        text::format(count, "QUEST_LOG_COUNT_TEMPLATE", v.quests, kMaxQuests);
        TextStyle style = ui::gameFont(s.small);
        style.align = Align::Right;
        const f32 textW = g.draw.measure(count, style.font, style.size);
        const f32 cap = s.u(8.0f), h = s.u(20.0f);
        const f32 right = l.countRight, top = l.countTop;
        g.draw.sprite({right - cap, top, cap, h}, kInputBorder, ui::kWhite, {0.9375f, 0.0f, 1.0f, 0.625f});
        g.draw.sprite({right - cap - textW, top, textW, h}, kInputBorder, ui::kWhite,
                      {0.0625f, 0.0f, 0.9375f, 0.625f});
        g.draw.sprite({right - 2.0f * cap - textW, top, cap, h}, kInputBorder, ui::kWhite,
                      {0.0f, 0.0f, 0.0625f, 0.625f});
        g.draw.text(right - s.u(6.0f), ui::centeredBaseline(g.draw, style, top + h * 0.5f), count, style);
    }
}

// The rows (QuestLog_Update): zone headers with their (+)/(-) in the header grey, quests in their difficulty color,
// the selected one under QuestLogHighlightFrame. Rows that overflow scroll, with the scroll bar shown and the rows
// narrowed for it (FauxScrollFrame_Update).
void questLogList(DialogContext& c, Gui& g, const QuestLogLayout& l, const QuestArtSizes& s,
                  const QuestLogPanelView& v, QuestLogPanelResult& result) {
    const f32 rowH = s.row;
    const bool overflow = static_cast<f32>(v.rows.size()) * rowH > l.list.h + 0.5f;
    Rect viewport = l.list;
    if (overflow) {
        viewport.w = std::max(0.0f, l.listBar.x - s.u(3.0f) - viewport.x);
    }
    const ui::ScrollView list = ui::beginScroll(g, widgetId("questLog.list"), viewport);
    // QuestLogTitleButtonTemplate: the (+)/(-) 16 x 16 at 3, the text at 20 (a quest's after "  "), the group mates at
    // 8, the tag 2 in from the right, the check 16 x 16 after the title.
    const f32 textX = s.u(20.0f);
    const bool linking = c.chatLink && c.chatLink->active();
    f32 y = list.top;
    for (usize i = 0; i < v.rows.size(); ++i) {
        const QuestLogRowView& r = v.rows[i];
        const Rect row{viewport.x, y, viewport.w, rowH};
        const ui::WidgetState state = ui::hit(g, widgetId("questLog.row", i), row);
        const bool down = ui::held(state, row);
        const bool selected = static_cast<i32>(i) == v.selected;
        if (r.header) {
            // A header: its (+)/(-) and the zone name in the header grey (QuestLog_Update).
            const Rect button{row.x + s.u(3.0f), row.centerY() - s.u(16.0f) * 0.5f, s.u(16.0f), s.u(16.0f)};
            g.draw.sprite(button, r.collapsed ? kPlusUp : kMinusUp);
            if (down) {
                g.draw.sprite(button, kPlusHighlight);
            }
            ui::label(g, {row.x + textX, row.y, row.w - textX, row.h}, r.title, ui::gameFont(s.normal, kHeaderColor));
        } else {
            const u32 color = ui::questDifficultyColor(r.level, c.playerLevel);
            if (selected || down) {
                // QuestLogHighlightFrame: the highlight tinted with the title's color; the title then white.
                g.draw.sprite(row, kQuestLogHighlight, selected ? color : ui::withAlpha(color, 0.5f));
            }
            char tagBuffer[48];
            const std::string_view tag = r.failed         ? tagText(tagBuffer, text::get("FAILED"))
                                         : r.complete     ? tagText(tagBuffer, text::get("COMPLETE"))
                                         : !r.tag.empty() ? tagText(tagBuffer, r.tag)
                                                          : std::string_view{};
            TextStyle tagStyle = ui::gameFont(s.normal, selected ? ui::kWhite : color);
            tagStyle.align = Align::Right;
            const f32 tagW = tag.empty() ? 0.0f : g.draw.measure(tag, tagStyle.font, tagStyle.size) + s.u(6.0f);
            if (!tag.empty()) {
                g.draw.text(row.right() - s.u(2.0f), ui::centeredBaseline(g.draw, tagStyle, row.centerY()), tag,
                            tagStyle);
            }
            // "[n]" nearby party members on it (QuestLogTitle<n>GroupMates, GameFontNormalSmall), left of the title.
            if (r.groupMates > 0) {
                char mates[16];
                std::snprintf(mates, sizeof(mates), "[%u]", r.groupMates);
                const TextStyle matesStyle = ui::gameFont(s.small, color);
                g.draw.text(row.x + s.u(2.0f), ui::centeredBaseline(g.draw, matesStyle, row.centerY()), mates,
                            matesStyle);
            }
            // The title after the client's "  " prefix, and the check of a watched quest right after it.
            const TextStyle titleStyle = ui::gameFont(s.normal, selected ? ui::kWhite : color);
            const f32 titleX = row.x + textX + g.draw.measure("  ", titleStyle.font, titleStyle.size);
            const f32 checkW = r.watched ? s.u(16.0f) + s.u(2.0f) : 0.0f;
            const f32 room = std::max(0.0f, row.right() - tagW - titleX - checkW);
            ui::label(g, {titleX, row.y, room, row.h}, r.title, titleStyle);
            if (r.watched) {
                const f32 shown = std::min(room, g.draw.measure(r.title, titleStyle.font, titleStyle.size));
                g.draw.sprite({titleX + shown + s.u(2.0f), row.centerY() - s.u(8.0f), s.u(16.0f), s.u(16.0f)},
                              kCheck);
            }
        }
        if (ui::tapped(state)) {
            if (linking && !r.header) {
                result.linkRow = static_cast<i32>(i); // QuestLogTitleButton_OnClick with the edit box open
            } else {
                result.row = static_cast<i32>(i);
            }
        }
        y += rowH;
    }
    ui::endScroll(g, list, y - list.top, false);
    if (overflow && ui::panelScrollBar(g, list, l.listBar)) {
        result.scrolled = true;
    }
    if (v.rows.empty()) {
        // QuestLogNoQuestsText (GameFontHighlight) over the cobwebs.
        TextStyle empty = ui::gameFont(s.normal, ui::kWhite);
        empty.align = Align::Center;
        ui::label(g, {l.noQuests.x, l.noQuests.y, l.noQuests.w, std::max(l.noQuests.h, s.normal * 1.4f)},
                  text::get("QUESTLOG_NO_QUESTS_TEXT"), empty);
    }
}

// QuestFrameItems_Update("QuestLog"): the rewards (kit::questArtRewards), QuestLogRewardTitleText 15 under the
// description.
void questLogRewards(DialogContext& c, Flow& flow, const QuestArtSizes& s, const QuestLogDetailView& d) {
    QuestArtRewards r;
    r.choices = d.choices;
    r.rewards = d.rewards;
    r.chooseText = text::get("REWARD_CHOICES");
    if (!d.spell.name.empty()) {
        r.spellText = text::get(d.spell.tradeskill ? "REWARD_TRADESKILL_SPELL" : "REWARD_SPELL");
        r.spell = {d.spell.iconHash, d.spell.name, 0, 1, 0};
    }
    r.money = d.money;
    questArtRewards(c, flow, s, "questLog.reward", r);
}

// QuestLogDetailScrollFrame over the art's parchment (QuestLog_UpdateQuestDetails), its scroll bar in the art's column.
void questLogDetail(DialogContext& c, Gui& g, const QuestLogLayout& l, const QuestArtSizes& s,
                    const QuestLogDetailView& d, QuestLogPanelResult& result) {
    const ui::ScrollView detail = ui::beginScroll(g, widgetId("questLog.detail"), l.detail);
    // QuestLogQuestTitle at (5, -5) of the scroll child, the texts 275..285 of its 300 units.
    const f32 pad = s.u(5.0f);
    Flow flow{&g, detail.viewport.x + pad, std::max(0.0f, detail.viewport.w - pad - s.u(15.0f)), detail.top + pad};
    const TextStyle body = ui::questFont(s.quest);
    // The title (QuestTitleFont), " - (Failed)" after it when failed; the objectives' text 5 under it (QuestFont).
    char title[192];
    if (d.failed) {
        const std::string_view failed = text::get("FAILED");
        std::snprintf(title, sizeof(title), "%.*s - (%.*s)", static_cast<int>(d.title.size()), d.title.data(),
                      static_cast<int>(failed.size()), failed.data());
        flow.title(title, s.questTitle, s.u(5.0f));
    } else {
        flow.title(d.title, s.questTitle, s.u(5.0f));
    }
    flow.text(d.objectivesText, body, s.u(10.0f));
    if (!d.timeLeft.empty()) {
        // TEXT(TIME_REMAINING).." "..SecondsToTime(...) (QuestFontNormalSmall), 10 over the leaderboard.
        char timer[96];
        const std::string_view label = text::get("TIME_REMAINING");
        std::snprintf(timer, sizeof(timer), "%.*s %.*s", static_cast<int>(label.size()), label.data(),
                      static_cast<int>(d.timeLeft.size()), d.timeLeft.data());
        flow.text(timer, ui::questFont(s.objective), s.u(10.0f));
    }
    // The leaderboard (QuestFontNormalSmall, 2 apart): black until met, then grey with " (Complete)".
    for (const ObjectiveView& o : d.objectives) {
        char line[192];
        const std::string_view shown = questObjectiveLine(line, o);
        const bool done = o.need > 0 && o.have >= o.need;
        if (done) {
            char complete[224];
            const std::string_view word = text::get("COMPLETE");
            std::snprintf(complete, sizeof(complete), "%.*s (%.*s)", static_cast<int>(shown.size()),
                          shown.data(), static_cast<int>(word.size()), word.data());
            flow.text(complete, ui::questFont(s.objective, kObjectiveDoneColor), s.u(2.0f));
        } else {
            flow.text(shown, ui::questFont(s.objective, kObjectiveColor), s.u(2.0f));
        }
    }
    if (d.requiredMoney > 0) {
        // REQUIRED_MONEY and the coins (red when the player has less).
        flow.y += s.u(2.0f);
        const std::string_view label = text::get("REQUIRED_MONEY");
        const TextStyle style =
            ui::questFont(s.objective, d.notEnoughMoney ? kObjectiveColor : kObjectiveDoneColor);
        const f32 h = s.objective * 1.4f;
        const f32 w = g.draw.measure(label, style.font, style.size) + s.u(10.0f);
        g.draw.text(flow.x, ui::centeredBaseline(g.draw, style, flow.y + h * 0.5f), label, style);
        ui::money(g, flow.x + w, flow.y + h * 0.5f, d.requiredMoney, s.objective, Align::Left);
        flow.y += h;
    }
    // QuestLogDescriptionTitle 10 under the leaderboard, the description 5 under it.
    flow.y += s.u(8.0f);
    flow.title(text::get("QUEST_DESCRIPTION"), s.questTitle, s.u(5.0f));
    flow.text(d.description, body, s.u(15.0f));
    questLogRewards(c, flow, s, d);
    ui::endScroll(g, detail, flow.y - detail.top + pad, false);
    if (ui::panelScrollBar(g, detail, l.detailBar)) {
        result.scrolled = true;
    }
}

} // namespace

std::string_view questObjectiveLine(std::span<char> out, const ObjectiveView& o) {
    if (o.need > 0) {
        return text::format(out, o.kill ? "QUEST_MONSTERS_KILLED" : "QUEST_ITEMS_NEEDED", o.text,
                            std::min(o.have, o.need), o.need);
    }
    const int n =
        std::snprintf(out.data(), out.size(), "%.*s", static_cast<int>(o.text.size()), o.text.data());
    return {out.data(), static_cast<usize>(std::clamp(n, 0, static_cast<int>(out.size()) - 1))};
}

QuestLogPanelResult questLogPanel(DialogContext& c, const QuestLogPanelView& v) {
    VD_PROFILE_ZONE("QuestWindows::questLog");
    Gui g{c.ui, c.draw};
    QuestLogPanelResult result;
    // The art at the scale of the window buttons (they fill its bar's slots). In its place it fills it (parity pass:
    // panels); over the world (no place), the side-by-side size or the original's, centered.
    const f32 k = questLogScale(metrics(c).button);
    Rect place = c.panel;
    bool wide = false;
    if (place.w > 0.0f) {
        wide = questLogWide(place, k);
    } else {
        ui::modalBlocker(g, widgetId("questLog", 1), 0.3f);
        const Rect safe = c.safeArea.inset(g.px(6.0f));
        wide = questLogWide(safe, k);
        const f32 w = std::min(safe.w, (wide ? 690.0f : kQuestLogMinWidth) * k);
        const f32 h = std::min(safe.h, (wide ? 390.0f : 462.0f) * k);
        place = {safe.centerX() - w * 0.5f, safe.centerY() - h * 0.5f, w, h};
    }
    // Wide: the list and the parchment halve the width (the picture); tall: the list over 40 % of the height.
    const QuestLogLayout l = questLogLayout(place, k, wide, wide ? 0.5f : 0.4f);
    const QuestArtSizes s = questArtSizes(g, l.k);
    ui::hit(g, widgetId("questLog", 2), l.frame); // the fingers that miss its controls are its own
    questLogArt(c, g, l, v.rows.empty());
    questLogHeader(g, l, s, v, result);
    questLogList(c, g, l, s, v, result);
    if (v.detail) {
        questLogDetail(c, g, l, s, *v.detail, result);
    }
    if (result.row >= 0 && !v.rows[static_cast<usize>(result.row)].header) {
        ui::scrollTo(widgetId("questLog.detail"), 0.0f); // QuestLog_UpdateQuestDetails: a new quest from its top
    }

    // QuestLogFrameAbandonButton in the bar's left slot; QuestFramePushQuestButton and QuestFrameExitButton in the
    // right ones.
    result.abandon =
        ui::button(g, widgetId("questLog.abandon"), l.abandon, text::get("ABANDON_QUEST"), v.detail != nullptr);
    result.share = ui::button(g, widgetId("questLog.share"), l.share, text::get("SHARE_QUEST"), v.canShare);
    result.closed = ui::button(g, widgetId("questLog.exit"), l.exit, text::get("EXIT")) || result.closed;
    return result;
}

QuestFrameResult questWatchFrame(DialogContext& c, f32 left, f32 top, f32 bottom, f32 maxWidth,
                                 std::span<const QuestWatchEntryView> quests) {
    VD_PROFILE_ZONE("QuestWindows::questWatch");
    Gui g{c.ui, c.draw};
    QuestFrameResult result;
    result.bottom = bottom;
    if (quests.empty() || bottom <= top || maxWidth <= 0.0f) {
        return result;
    }
    // QuestWatchFrame: GameFontNormal titles, GameFontHighlight objective lines (" - " first), 13 units a
    // line.
    const f32 titleSize = g.px(13.0f), lineSize = g.px(12.0f), pad = g.px(4.0f);
    const f32 titleH = g.draw.lineHeight(ui::FontId::Friz, titleSize);
    const f32 lineH = g.draw.lineHeight(ui::FontId::Friz, lineSize);
    // As wide as its longest line (SetWidth(questWatchMaxWidth + 10)), anchored at the left.
    f32 width = 0.0f;
    char line[200];
    const f32 dash = g.draw.measure(" - ", ui::FontId::Friz, lineSize);
    for (const QuestWatchEntryView& q : quests) {
        width = std::max(width, g.draw.measure(q.title, ui::FontId::Friz, titleSize));
        for (const ObjectiveView& o : q.objectives) {
            const std::string_view shown = questObjectiveLine({line + 3, sizeof(line) - 3}, o);
            width = std::max(width, g.draw.measure(shown, ui::FontId::Friz, lineSize) + dash);
        }
    }
    width = std::min(width + g.px(10.0f) + 2.0f * pad, maxWidth);
    const f32 x = left;
    // The entries that fit between `top` and `bottom`, the first ones first; then drawn down to `bottom`.
    const f32 gap = g.px(4.0f);
    const auto entryHeight = [&](const QuestWatchEntryView& q) {
        return pad + titleH + static_cast<f32>(q.objectives.size()) * lineH + pad;
    };
    usize shown = 0;
    f32 total = 0.0f;
    for (; shown < quests.size(); ++shown) {
        const f32 h = entryHeight(quests[shown]) + (shown > 0 ? gap : 0.0f);
        if (total + h > bottom - top) {
            break;
        }
        total += h;
    }
    if (shown == 0) {
        return result;
    }
    f32 y = bottom - total;
    result.bounds = {x, y, width, total};
    // A quick tap that no widget took and nothing moved (the stick may hold the finger: it takes no ownership).
    constexpr u64 kTapMs = 350;
    const ui::Touch* tap = nullptr;
    for (const ui::Touch& t : g.ui.touches()) {
        if (t.owner == 0 && t.released && !t.cancelled && !t.moved && g.ui.nowMs() - t.downMs < kTapMs &&
            result.bounds.contains(t.startX, t.startY)) {
            tap = &t;
        }
    }
    for (usize i = 0; i < shown; ++i) {
        const QuestWatchEntryView& q = quests[i];
        bool allDone = true;
        for (const ObjectiveView& o : q.objectives) {
            allDone = allDone && o.need > 0 && o.have >= o.need;
        }
        const f32 h = entryHeight(q);
        const Rect entry{x, y, width, h};
        ui::label(g, {x + pad, y + pad, width - 2.0f * pad, titleH}, q.title,
                  ui::gameFont(titleSize, allDone ? ui::kGold : kWatchTitleColor));
        f32 lineY = y + pad + titleH;
        for (const ObjectiveView& o : q.objectives) {
            line[0] = ' ';
            line[1] = '-';
            line[2] = ' ';
            questObjectiveLine({line + 3, sizeof(line) - 3}, o);
            const bool done = o.need > 0 && o.have >= o.need;
            ui::label(g, {x + pad, lineY, width - 2.0f * pad, lineH}, line,
                      ui::gameFont(lineSize, done ? ui::kWhite : kWatchObjectiveColor));
            lineY += lineH;
        }
        if (tap && entry.contains(tap->startX, tap->startY)) {
            result.tapped = static_cast<i32>(i);
        }
        y += h + gap;
    }
    return result;
}

QuestFrameResult questTimerFrame(DialogContext& c, f32 right, f32 top,
                                 std::span<const std::string_view> timers) {
    VD_PROFILE_ZONE("QuestWindows::questTimer");
    Gui g{c.ui, c.draw};
    QuestFrameResult result;
    result.bottom = top;
    if (timers.empty()) {
        return result;
    }
    // QuestTimerFrame: 160 units wide, 45 + 16 a timer high, the dialog box backdrop with QUEST_TIMERS on its
    // header (the plate 12 units over the top edge), the timers from 30 units down (GameFontHighlightSmall).
    const f32 k = g.unit(1.0f);
    const f32 w = std::max(160.0f * k, g.px(150.0f));
    const f32 rowH = std::max(16.0f * k, g.px(20.0f)); // a little taller for fingers
    const f32 h = 45.0f * k + rowH * static_cast<f32>(timers.size());
    const Rect frame{right - w, top + 12.0f * k, w, h};
    ui::hit(g, widgetId("questTimer.frame"),
            frame); // the frame takes the fingers that land on it (under its rows)
    ui::dialogPanel(g, frame);
    {
        // QuestTimerHeader: the 256-unit header art over a 160-unit frame (its plate overhangs the frame),
        // QUEST_TIMERS on it in GameFontNormal.
        const std::string_view title = text::get("QUEST_TIMERS");
        TextStyle style = ui::gameFont(std::max(12.0f * k, g.px(13.0f)));
        style.align = Align::Center;
        const f32 textW = g.draw.measure(title, style.font, style.size);
        const f32 plateW = std::max(139.0f * k, textW + 44.0f * k);
        const Rect plate{frame.centerX() - plateW * 0.5f, frame.y - 12.0f * k, plateW, 40.0f * k};
        ui::threeSlice(g.draw, plate, kDialogHeader, {58.0f / 256.0f, 0.0f, 197.0f / 256.0f, 40.0f / 64.0f},
                       24.0f / 256.0f, 24.0f * k);
        ui::label(g, {plate.x + 18.0f * k, plate.y, plate.w - 36.0f * k, 39.0f * k}, title, style);
    }
    TextStyle line = ui::gameFont(std::max(11.0f * k, g.px(12.0f)), ui::kWhite);
    line.align = Align::Center;
    for (usize i = 0; i < timers.size(); ++i) {
        const Rect row{frame.x + 10.0f * k, frame.y + 30.0f * k + rowH * static_cast<f32>(i),
                       frame.w - 20.0f * k, rowH};
        const ui::WidgetState state = ui::hit(g, widgetId("questTimer.row", i), row);
        if (ui::held(state, row)) {
            g.draw.solid(row, rgba(255, 255, 255, 40));
        }
        g.draw.text(row.centerX(), ui::centeredBaseline(g.draw, line, row.centerY()), timers[i], line);
        if (ui::tapped(state)) {
            result.tapped = static_cast<i32>(i);
        }
    }
    result.bottom = frame.bottom();
    return result;
}

} // namespace vd
