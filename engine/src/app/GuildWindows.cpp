#include "app/GuildWindows.h"

#include "app/DialogKit.h"
#include "app/SocialWindows.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>

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

constexpr u64 kColumnTabs = assetPathHash("Interface\\FriendsFrame\\WhoFrame-ColumnTabs.blp");
// Retouches (friends frame): the rows' HighlightTexture and GuildFrameGuildListToggleButton's.
constexpr u64 kHighlightBar = assetPathHash("Interface\\FriendsFrame\\UI-FriendsFrame-HighlightBar.blp");
constexpr u64 kMouseHighlight = assetPathHash("Interface\\Buttons\\UI-Common-MouseHilight.blp");
constexpr u64 kListboxHighlight = assetPathHash("Interface\\Buttons\\UI-Listbox-Highlight.blp");
constexpr u64 kNextPageUp = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-NextPage-Up.blp");
constexpr u64 kNextPageDown = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-NextPage-Down.blp");
constexpr u64 kPrevPageUp = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-PrevPage-Up.blp");
constexpr u64 kPrevPageDown = assetPathHash("Interface\\Buttons\\UI-SpellbookIcon-PrevPage-Down.blp");
constexpr u64 kPromoteUp = assetPathHash("Interface\\MainMenuBar\\UI-MainMenu-ScrollUpButton-Up.blp");
constexpr u64 kPromoteDown = assetPathHash("Interface\\MainMenuBar\\UI-MainMenu-ScrollUpButton-Down.blp");
constexpr u64 kPromoteDisabled =
    assetPathHash("Interface\\MainMenuBar\\UI-MainMenu-ScrollUpButton-Disabled.blp");
constexpr u64 kDemoteUp = assetPathHash("Interface\\MainMenuBar\\UI-MainMenu-ScrollDownButton-Up.blp");
constexpr u64 kDemoteDown = assetPathHash("Interface\\MainMenuBar\\UI-MainMenu-ScrollDownButton-Down.blp");
constexpr u64 kDemoteDisabled =
    assetPathHash("Interface\\MainMenuBar\\UI-MainMenu-ScrollDownButton-Disabled.blp");
constexpr u64 kGuildMemberPatch = assetPathHash("Interface\\FriendsFrame\\UI-GuildMember-Patch.blp");
constexpr u64 kDropDownArrow = assetPathHash("Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Up.blp");
constexpr u64 kCharterIcon = assetPathHash("Interface\\PetitionFrame\\GuildCharter-Icon.blp");
constexpr u64 kTabardBackground = assetPathHash("Interface\\TabardFrame\\TabardFrameBackground.blp");
constexpr u64 kLabelFrame =
    assetPathHash("Interface\\Glues\\CharacterCreate\\CharacterCreate-LabelFrame.blp");

constexpr u32 kWhiteText = ui::kWhite; // GameFontHighlight
constexpr u32 kGrayText =
    rgba(166, 166, 166); // SetTextColor(0.65, 0.65, 0.65): a text the player may not edit
constexpr u32 kOfflineText = rgba(128, 128, 128); // SetTextColor(0.5, 0.5, 0.5)

// The column widths of GuildFrameColumnHeader1..4 (83, 105, 32, 92) and GuildFrameGuildStatusColumnHeader1..4
// (83, 75, 75, 75), as shares of the row.
constexpr f32 kPlayerColumns[kGuildColumns] = {83.0f, 105.0f, 32.0f, 92.0f};
constexpr f32 kStatusColumns[kGuildColumns] = {83.0f, 75.0f, 75.0f, 75.0f};

// GuildFrameColumnHeaderTemplate: WhoFrame-ColumnTabs in three slices (5-unit ends at `k` pixels a unit), its label in
// GameFontHighlightSmall 8 in.
bool columnHeader(Gui& g, WidgetId id, const Rect& r, std::string_view text, f32 size, f32 k) {
    const ui::WidgetState state = ui::hit(g, id, r);
    ui::threeSlice(g.draw, r, kColumnTabs, {0.0f, 0.0f, 0.96875f, 0.75f}, 0.078125f / 0.96875f, 5.0f * k,
                   ui::held(state, r) ? rgba(200, 200, 200) : ui::kWhite);
    g.draw.pushClip(r);
    ui::label(g, {r.x + 8.0f * k, r.y, r.w - 10.0f * k, r.h}, text, ui::gameFont(size, kWhiteText));
    g.draw.popClip();
    return ui::tapped(state);
}

// A box of text the player may tap to edit (GuildMOTDEditButton, the note backgrounds: UI-Tooltip backdrop at
// a quarter of its tint). Returns true when tapped and `enabled`.
bool noteBox(Gui& g, WidgetId id, const Rect& r, std::string_view text, const TextStyle& style, bool enabled,
             bool backdrop) {
    if (backdrop) {
        ui::tooltipPanel(g, r, rgba(23, 23, 48, 64));
    }
    const Rect inner = r.inset(g.px(backdrop ? 7.0f : 2.0f));
    g.draw.pushClip(r);
    g.draw.wrappedText({inner.x, inner.y, inner.w, inner.h + style.size}, text, style);
    g.draw.popClip();
    if (!enabled) {
        return false;
    }
    const ui::WidgetState state = ui::hit(g, id, r);
    if (ui::held(state, r)) {
        g.draw.sprite(r, kListboxHighlight, rgba(255, 255, 255, 90));
    }
    return ui::tapped(state);
}

// An arrow button inside the touch target `r` (art with its pushed state).
bool artButton(Gui& g, WidgetId id, const Rect& r, u64 up, u64 down, u64 disabled, bool enabled) {
    const ui::WidgetState state = ui::hit(g, id, r);
    const f32 size = std::min(r.w, r.h);
    const Rect art{r.centerX() - size * 0.5f, r.centerY() - size * 0.5f, size, size};
    g.draw.sprite(art,
                  !enabled             ? (disabled ? disabled : up)
                  : ui::held(state, r) ? down
                                       : up,
                  enabled || disabled ? ui::kWhite : rgba(110, 110, 110));
    return enabled && ui::tapped(state);
}

// The frame of a side window (GuildMemberDetailFrame, GuildInfoFrame: UI-DialogBox backdrop, the corner
// decoration, the close X). Returns the inside and whether the X was tapped.
Rect sideFrame(Gui& g, WidgetId id, const Rect& area, std::string_view title, f32 titleSize, bool& closed) {
    ui::dialogPanel(g, area);
    const Rect inner = ui::dialogContent(g, area);
    const f32 x = g.px(30.0f);
    closed = ui::closeButton(g, id, {area.right() - x - g.px(4.0f), area.y + g.px(4.0f), x, x});
    if (!title.empty()) {
        ui::label(g, {inner.x + g.px(4.0f), inner.y, inner.w - x, titleSize * 1.6f}, title,
                  ui::gameFont(titleSize));
        return {inner.x, inner.y + titleSize * 1.6f, inner.w, inner.h - titleSize * 1.6f};
    }
    return inner;
}

} // namespace

// --- GuildFrame
// ------------------------------------------------------------------------------------------------

ui::Rect guildSideFrame(const Gui& g, const DialogContext& c, const FriendsFrameLayout& l, bool member) {
    // GuildMemberDetailFrame (212 x 270) from 28 under the frame's top, GuildInfoFrame and GuildControlPopupFrame
    // (297 x 298) from 65; wide and tall enough for their touch controls.
    const f32 w = std::max((member ? 212.0f : 297.0f) * l.k, g.px(member ? 280.0f : 320.0f));
    const f32 h = std::max((member ? 270.0f : 298.0f) * l.k, g.px(300.0f));
    return friendsSideFrame(l, c.safeArea, member ? 28.0f : 65.0f, w, h);
}

GuildTabResult guildTab(DialogContext& c, const FriendsFrameLayout& l, const QuestArtSizes& s, const GuildTabView& v) {
    VD_PROFILE_ZONE("GuildWindows::tab");
    Gui g{c.ui, c.draw};
    GuildTabResult result;
    const f32 k = l.k;
    const GuildFrameLayout& gl = l.guild;
    // The column headers from (20, -70), each overlapping the one before by 2.
    const f32* widths = v.guildStatus ? kStatusColumns : kPlayerColumns;
    {
        f32 x = gl.headers.x;
        for (u32 i = 0; i < kGuildColumns; ++i) {
            if (columnHeader(g, widgetId("guild.header", i), {x, gl.headers.y, widths[i] * k, gl.headers.h},
                             v.headers[i], s.small, k)) {
                result.header = static_cast<i32>(i);
            }
            x += (widths[i] - 2.0f) * k;
        }
    }
    // The rows (FriendsFrameGuildPlayerStatusButtonTemplate / FriendsFrameGuildStatusButtonTemplate, 298 x 16, a
    // finger's height at least): their texts where the templates put them, the name in GameFontNormalSmall (gold),
    // the rest in GameFontHighlightSmall, offline members gray; UI-FriendsFrame-HighlightBar (ADD).
    struct Cell {
        f32 x, w;
    };
    constexpr Cell kPlayerCells[kGuildColumns] = {{14.0f, 88.0f}, {95.0f, 90.0f}, {189.0f, 20.0f}, {221.0f, 77.0f}};
    constexpr Cell kStatusCells[kGuildColumns] = {{14.0f, 75.0f}, {94.0f, 55.0f}, {160.0f, 85.0f}, {255.0f, 43.0f}};
    const Cell* cells = v.guildStatus ? kStatusCells : kPlayerCells;
    const FriendsListLayout& list = gl.list;
    const f32 rowH = std::max(16.0f * k, s.row);
    const FriendsListScroll scroll = beginFriendsList(c, widgetId("guild.list", v.guildStatus ? 1 : 0), list,
                                                      static_cast<u32>(v.rows.size()), rowH);
    f32 y = scroll.top;
    for (u32 i = 0; i < v.rows.size(); ++i, y += rowH) {
        const Rect r{list.rowX, y, list.rowW, rowH};
        if (r.bottom() < scroll.view.viewport.y || r.y > scroll.view.viewport.bottom()) {
            continue;
        }
        const GuildRowView& row = v.rows[i];
        const ui::WidgetState state = friendsListRow(c, widgetId("guild.row", i), scroll, r,
                                                     static_cast<i32>(i) == v.selected, kHighlightBar);
        if (ui::tapped(state)) {
            result.row = static_cast<i32>(i);
        }
        if (state.longPress) {
            result.rowHeld = static_cast<i32>(i);
            result.rowRect = r;
        }
        for (u32 col = 0; col < kGuildColumns; ++col) {
            const u32 color = !row.online ? kOfflineText : col == 0 ? ui::kGold : kWhiteText;
            TextStyle style = ui::gameFont(s.small, color);
            if (!v.guildStatus && col == 2) {
                style.align = Align::Center;
            }
            const Rect cell{r.x + cells[col].x * k, r.y, cells[col].w * k, rowH};
            g.draw.pushClip(cell);
            ui::label(g, cell, row.cells[col], style);
            g.draw.popClip();
        }
    }
    result.scrolled = endFriendsList(c, scroll, list);
    {
        // GuildFrameTotals centered at BOTTOM (-122, 181), GuildFrameOnlineTotals 3 right of it.
        const TextStyle style = ui::gameFont(s.small);
        const f32 w = g.draw.measure(v.totals, style.font, style.size);
        const f32 left = std::max(gl.totals.x, gl.totalsCenterX - w * 0.5f);
        ui::label(g, {left, gl.totals.y, w + 1.0f, gl.totals.h}, v.totals, style);
        ui::label(g, {left + w + 3.0f * k, gl.totals.y, gl.toggle.x - left - w - 6.0f * k, gl.totals.h}, v.online,
                  style);
    }
    {
        // GuildFrameGuildListToggleButton: the page arrow, its text (GameFontNormalSmall) right-aligned to its left;
        // the whole a target, a finger high.
        const Rect& arrow = gl.toggle;
        const TextStyle base = ui::gameFont(s.small);
        const f32 textW = g.draw.measure(v.toggleLabel, base.font, base.size);
        const f32 finger = g.px(ui::kTouchDp);
        const Rect target{arrow.x - textW - 4.0f * k, std::min(arrow.y, arrow.centerY() - finger * 0.5f),
                          arrow.w + textW + 4.0f * k, std::max(arrow.h, finger)};
        const ui::WidgetState state = ui::hit(g, widgetId("guild.toggle"), target);
        TextStyle style = base;
        style.align = Align::Right;
        ui::label(g, {arrow.x - textW - 4.0f * k, arrow.y, textW + 1.0f, arrow.h}, v.toggleLabel, style);
        g.draw.sprite(arrow, ui::held(state, target) ? kNextPageDown : kNextPageUp);
        if (ui::held(state, target)) {
            g.draw.spriteAdditive(arrow, kMouseHighlight);
        }
        result.toggleView = ui::tapped(state);
    }
    {
        // GuildFrameNotesLabel and GuildFrameNotesText (white where the player may edit it: GuildMOTDEditButton).
        ui::label(g, gl.motdLabel, text::get("GUILD_MOTD_LABEL"), ui::gameFont(s.small));
        result.motd = noteBox(g, widgetId("guild.motd"), gl.motd, v.motd,
                              ui::gameFont(s.small, v.motdEditable ? kWhiteText : kGrayText), v.motdEditable, false);
    }
    result.information = ui::button(g, widgetId("guild.info"), gl.information, text::get("GUILD_INFORMATION"));
    if (v.infoOpen) {
        g.draw.sprite(gl.information.inset(2.0f * k), kListboxHighlight, rgba(255, 255, 255, 70));
    }
    result.addMember = ui::button(g, widgetId("guild.add"), gl.addMember, text::get("ADDMEMBER"), v.addEnabled);
    result.control = ui::button(g, widgetId("guild.control"), gl.control, text::get("GUILDCONTROL"), v.controlEnabled);
    if (v.controlOpen) {
        g.draw.sprite(gl.control.inset(2.0f * k), kListboxHighlight, rgba(255, 255, 255, 70));
    }
    return result;
}

// --- GuildMemberDetailFrame
// ------------------------------------------------------------------------------------

GuildMemberResult guildMemberDetail(DialogContext& c, Gui& g, const Rect& area, const GuildMemberView& v) {
    VD_PROFILE_ZONE("GuildWindows::member");
    const Metrics m = metrics(c);
    GuildMemberResult result;
    bool closed = false;
    const Rect in = sideFrame(g, widgetId("guildMember.close"), area, {}, m.text, closed);
    result.closed = closed;
    const ui::ScrollView view =
        ui::beginScroll(g, widgetId("guildMember.scroll"), {in.x, in.y, in.w, in.h - m.button - g.px(6.0f)});
    f32 y = view.top;
    const f32 x = in.x + g.px(6.0f);
    const f32 w = in.w - g.px(12.0f);
    // GuildMemberDetailName (GameFontNormal) and GuildMemberDetailLevel (GameFontHighlightSmall) under it;
    // the promote / demote arrows at the right of the name.
    const f32 nameH = m.text * 1.5f;
    const f32 arrow = std::max(g.px(30.0f), nameH);
    ui::label(g, {x, y, w - arrow * 2.0f, nameH}, v.name, ui::gameFont(m.text));
    if (v.promoteShown) {
        const Rect up{in.right() - arrow * 2.0f - g.px(6.0f), y, arrow, arrow};
        result.promote = artButton(g, widgetId("guildMember.promote"), up, kPromoteUp, kPromoteDown,
                                   kPromoteDisabled, v.promoteEnabled);
        result.demote = artButton(g, widgetId("guildMember.demote"), {up.right(), y, arrow, arrow}, kDemoteUp,
                                  kDemoteDown, kDemoteDisabled, v.demoteEnabled);
    }
    y += nameH;
    ui::label(g, {x, y, w, m.small * 1.4f}, v.levelClass, ui::gameFont(m.small, kWhiteText));
    y += m.small * 1.4f + g.px(6.0f);
    // ZONE_COLON / RANK_COLON / LAST_ONLINE_COLON (GameFontNormalSmall) with their values
    // (GameFontHighlight).
    const auto field = [&](text::Key labelKey, std::string_view value) {
        const std::string_view label = text::get(labelKey);
        const f32 labelW = std::min(g.draw.measure(label, ui::FontId::Friz, m.small) + g.px(6.0f), w * 0.5f);
        ui::label(g, {x, y, labelW, m.text * 1.4f}, label, ui::gameFont(m.small));
        ui::label(g, {x + labelW, y, w - labelW, m.text * 1.4f}, value,
                  ui::gameFont(m.text * 0.92f, kWhiteText));
        y += m.text * 1.4f;
    };
    field("ZONE_COLON", v.zone);
    field("RANK_COLON", v.rank);
    field("LAST_ONLINE_COLON", v.online);
    y += g.px(4.0f);
    // NOTE_COLON and its box; OFFICER_NOTE_COLON and its box for those who may see it.
    const f32 boxH = m.text * 2.6f + g.px(10.0f);
    ui::label(g, {x, y, w, m.small * 1.4f}, text::get("NOTE_COLON"), ui::gameFont(m.small));
    y += m.small * 1.4f;
    result.note =
        noteBox(g, widgetId("guildMember.note"), {x, y, w, boxH}, v.note,
                ui::gameFont(m.text * 0.92f, v.noteEditable ? kWhiteText : kGrayText), v.noteEditable, true);
    y += boxH + g.px(4.0f);
    if (v.officerNoteShown) {
        ui::label(g, {x, y, w, m.small * 1.4f}, text::get("OFFICER_NOTE_COLON"), ui::gameFont(m.small));
        y += m.small * 1.4f;
        result.officerNote =
            noteBox(g, widgetId("guildMember.officer"), {x, y, w, boxH}, v.officerNote,
                    ui::gameFont(m.text * 0.92f, v.officerNoteEditable ? kWhiteText : kGrayText),
                    v.officerNoteEditable, true);
        y += boxH + g.px(4.0f);
    }
    ui::endScroll(g, view, y - view.top);
    // GuildMemberRemoveButton (REMOVE) and GuildMemberGroupInviteButton (GROUP_INVITE) along the bottom.
    const f32 bw = (in.w - g.px(6.0f)) * 0.5f;
    const Rect remove{in.x, in.bottom() - m.button, bw, m.button};
    result.remove =
        ui::button(g, widgetId("guildMember.remove"), remove, text::get("REMOVE"), v.removeEnabled);
    result.groupInvite =
        ui::button(g, widgetId("guildMember.invite"), {remove.right() + g.px(6.0f), remove.y, bw, m.button},
                   text::get("GROUP_INVITE"), v.groupInviteEnabled);
    return result;
}

// --- GuildInfoFrame
// --------------------------------------------------------------------------------------------

GuildInfoResult guildInfoFrame(DialogContext& c, Gui& g, const Rect& area, const GuildInfoView& v) {
    VD_PROFILE_ZONE("GuildWindows::info");
    const Metrics m = metrics(c);
    GuildInfoResult result;
    bool closed = false;
    const Rect in =
        sideFrame(g, widgetId("guildInfo.close"), area, text::get("GUILD_INFORMATION"), m.text, closed);
    // UI-GuildMember-Patch under the title, then the text box (GuildInfoTextBackground: 276 x 230), Accept /
    // Close.
    g.draw.sprite({in.x, in.y - g.px(4.0f), in.w, g.px(8.0f)}, kGuildMemberPatch, rgba(255, 255, 255, 160),
                  {0.0f, 0.0f, 1.0f, 0.5f});
    const Rect box{in.x, in.y + g.px(4.0f), in.w, in.h - m.button - g.px(12.0f)};
    ui::tooltipPanel(g, box, rgba(0, 0, 0, 120));
    const Rect textArea = box.inset(g.px(8.0f));
    const ui::ScrollView view = ui::beginScroll(g, widgetId("guildInfo.scroll"), textArea);
    TextStyle s = ui::gameFont(m.text * 0.92f, v.editable ? kWhiteText : kGrayText);
    const f32 h = g.draw.wrappedText({textArea.x, view.top, textArea.w, 1e6f}, v.text, s);
    if (v.editing) { // the caret at the end
        g.draw.solid({textArea.x, view.top + h, g.px(2.0f), s.size}, ui::kWhite);
    }
    ui::endScroll(g, view, h + s.size);
    if (v.editable) {
        const ui::WidgetState state = ui::hit(g, widgetId("guildInfo.text"), box);
        result.tapped = ui::tapped(state);
    }
    const f32 bw = (in.w - g.px(6.0f)) * 0.5f;
    const Rect save{in.x, in.bottom() - m.button, bw, m.button};
    result.save = ui::button(g, widgetId("guildInfo.save"), save, text::get("ACCEPT"), v.editable);
    result.closed =
        closed || ui::button(g, widgetId("guildInfo.cancel"),
                             {save.right() + g.px(6.0f), save.y, bw, m.button}, text::get("CLOSE"));
    return result;
}

// --- GuildControlPopupFrame
// ------------------------------------------------------------------------------------

GuildControlResult guildControlFrame(DialogContext& c, Gui& g, const Rect& area, const GuildControlView& v) {
    VD_PROFILE_ZONE("GuildWindows::control");
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    GuildControlResult result;
    bool closed = false;
    const Rect in = sideFrame(g, widgetId("guildControl.close"), area, {}, m.text, closed);
    result.closed = closed;
    const ui::ScrollView view =
        ui::beginScroll(g, widgetId("guildControl.scroll"), {in.x, in.y, in.w, in.h - m.button - g.px(6.0f)});
    f32 y = view.top;
    const f32 x = in.x + g.px(4.0f);
    const f32 w = in.w - g.px(8.0f);
    // GUILDCONTROL_SELECTRANK, the rank drop-down with the add (+) and remove (-) buttons beside it.
    ui::label(g, {x, y, w, m.text * 1.4f}, text::get("GUILDCONTROL_SELECTRANK"), ui::gameFont(m.text));
    y += m.text * 1.4f;
    const f32 fieldH = std::max(g.px(34.0f), m.text * 1.8f);
    const f32 pm = fieldH;
    {
        const Rect drop{x, y, w - pm * 2.0f - g.px(8.0f), fieldH};
        const ui::WidgetState state = ui::hit(g, widgetId("guildControl.rank"), drop);
        ui::tooltipPanel(g, drop, rgba(0, 0, 0, 160));
        ui::label(g, {drop.x + g.px(8.0f), drop.y, drop.w - fieldH, drop.h}, v.rankName,
                  ui::gameFont(m.text, kWhiteText));
        g.draw.sprite({drop.right() - fieldH, drop.y, fieldH, fieldH}, kDropDownArrow);
        result.rankMenu = ui::tapped(state);
        result.addRank = ui::plusMinusButton(g, widgetId("guildControl.add"),
                                             {drop.right() + g.px(4.0f), y, pm, pm}, true, v.addEnabled);
        if (v.removeShown) {
            result.removeRank =
                ui::plusMinusButton(g, widgetId("guildControl.remove"),
                                    {drop.right() + g.px(4.0f) + pm, y, pm, pm}, false, v.removeEnabled);
        }
        y += fieldH + g.px(6.0f);
    }
    // GUILDCONTROL_RANKLABEL and the edit box (15 letters).
    {
        const std::string_view label = text::get("GUILDCONTROL_RANKLABEL");
        const f32 labelW = std::min(g.draw.measure(label, ui::FontId::Friz, m.small) + g.px(8.0f), w * 0.45f);
        ui::label(g, {x, y, labelW, fieldH}, label, ui::gameFont(m.small));
        result.labelTapped = ui::textField(g, widgetId("guildControl.label"),
                                           {x + labelW, y, w - labelW, fieldH}, v.label, {}, v.editing);
        y += fieldH + g.px(6.0f);
    }
    // GUILDCONTROL_ALLOWRANK and the thirteen check boxes in two columns (odd ones left, as the XML lays
    // them).
    ui::label(g, {x, y, w, m.small * 1.5f}, text::get("GUILDCONTROL_ALLOWRANK"),
              ui::gameFont(m.small, kWhiteText));
    y += m.small * 1.5f;
    const u32 cols = wide || w > g.px(330.0f) ? 2u : 1u;
    const f32 rowH = std::max(g.px(30.0f), m.small * 2.0f);
    const f32 colW = w / static_cast<f32>(cols);
    const u32 rows = (13u + cols - 1u) / cols;
    for (u32 i = 0; i < 13; ++i) {
        const u32 col = cols == 2 ? i % 2 : 0;
        const u32 row = cols == 2 ? i / 2 : i;
        bool checked = v.flags[i];
        if (ui::checkbox(g, widgetId("guildControl.flag", i),
                         {x + static_cast<f32>(col) * colW, y + static_cast<f32>(row) * rowH, colW, rowH},
                         v.flagLabels[i], checked)) {
            result.toggled = static_cast<i32>(i);
        }
    }
    y += static_cast<f32>(rows) * rowH;
    ui::endScroll(g, view, y - view.top);
    // GuildControlPopupAcceptButton and GuildControlPopupFrameCancelButton at the bottom right.
    const f32 bw = std::min((in.w - g.px(6.0f)) * 0.5f, g.px(150.0f));
    const Rect cancel{in.right() - bw, in.bottom() - m.button, bw, m.button};
    result.accept =
        ui::button(g, widgetId("guildControl.accept"), {cancel.x - g.px(6.0f) - bw, cancel.y, bw, m.button},
                   text::get("ACCEPT"), v.acceptEnabled);
    result.closed =
        result.closed || ui::button(g, widgetId("guildControl.cancel"), cancel, text::get("CANCEL"));
    return result;
}

// --- GuildRegistrarFrame
// ---------------------------------------------------------------------------------------

GuildRegistrarResult guildRegistrarWindow(DialogContext& c, const GuildRegistrarView& v) {
    VD_PROFILE_ZONE("GuildWindows::registrar");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    GuildRegistrarResult result;
    const Dialog d = beginDialog(c, g, "guildRegistrar", 460.0f, 560.0f, v.npcName, true, true);
    result.closed = d.closed;
    const ParchmentScroll p = beginParchment(g, widgetId("guildRegistrar.text"), d.body);
    Flow flow = parchmentFlow(g, p);
    if (!v.purchase) {
        // GuildRegistrarGreetingFrame: GuildRegistrarText (the NPC's greeting is in its gossip; 1.12 shows
        // none here), AVAILABLE_SERVICES and the two services as quest title buttons.
        flow.title(text::get("AVAILABLE_SERVICES"), m.heading, g.px(8.0f));
        bool tapped = false;
        flow.y += wrappedRow(g, widgetId("guildRegistrar.buy"), flow.x, flow.y, flow.w, m.row,
                             assetPathHash("Interface\\GossipFrame\\PetitionGossipIcon.blp"), ui::kWhite,
                             text::get("GUILD_CHARTER_PURCHASE"), ui::questFont(m.text), tapped);
        result.showPurchase = tapped;
        tapped = false;
        flow.y += wrappedRow(g, widgetId("guildRegistrar.turnIn"), flow.x, flow.y, flow.w, m.row,
                             assetPathHash("Interface\\GossipFrame\\PetitionGossipIcon.blp"), ui::kWhite,
                             text::get("GUILD_CHARTER_REGISTER"), ui::questFont(m.text), tapped);
        result.turnIn = tapped;
    } else {
        // GuildRegistrarPurchaseFrame: GUILD_REGISTRAR_PURCHASE_TEXT, the name box (24 letters), COSTS_LABEL
        // and the money.
        flow.text(text::get("GUILD_REGISTRAR_PURCHASE_TEXT"), ui::questFont(m.text), g.px(14.0f));
        const f32 fieldH = std::max(g.px(38.0f), m.text * 2.0f);
        result.nameTapped =
            ui::textField(g, widgetId("guildRegistrar.name"), {flow.x, flow.y, flow.w, fieldH}, v.name,
                          text::get("GUILD_NAME"), v.editing);
        flow.y += fieldH + g.px(12.0f);
        const std::string_view costs = text::get("COSTS_LABEL");
        const TextStyle label = ui::questFont(m.text);
        ui::label(g, {flow.x, flow.y, flow.w, m.text * 1.6f}, costs, label);
        ui::money(g, flow.x + g.draw.measure(costs, label.font, label.size) + g.px(10.0f),
                  flow.y + m.text * 0.8f, v.cost, m.text, Align::Left);
        flow.y += m.text * 1.6f;
    }
    endParchment(g, p, flow);
    if (v.purchase) {
        bool buy = false, back = false;
        footerButtons(g, d.footer, "guildRegistrar", text::get("PURCHASE"), !v.name.empty(), buy,
                      text::get("CANCEL"), back);
        result.buy = buy;
        result.back = back;
    } else {
        bool unused = false, cancel = false;
        footerButtons(g, d.footer, "guildRegistrar", {}, false, unused, text::get("CANCEL"), cancel);
        result.closed = result.closed || cancel;
    }
    return result;
}

// --- PetitionFrame
// ---------------------------------------------------------------------------------------------

PetitionResult petitionWindow(DialogContext& c, const PetitionView& v) {
    VD_PROFILE_ZONE("GuildWindows::petition");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    PetitionResult result;
    const Dialog d = beginDialog(c, g, "petition", 460.0f, 600.0f, v.title, true, true);
    result.closed = d.closed;
    const ParchmentScroll p = beginParchment(g, widgetId("petition.text"), d.body);
    Flow flow = parchmentFlow(g, p);
    // PetitionFramePortrait: the charter's icon, then GUILD_NAME / the name, GUILD_RANK0_DESC / the master,
    // MEMBERS and the nine names (NOT_YET_SIGNED), the instructions.
    const f32 icon = m.item;
    g.draw.sprite({flow.x + flow.w - icon, flow.y, icon, icon}, kCharterIcon);
    const TextStyle body = ui::questFont(m.text);
    flow.title(text::get("GUILD_NAME"), m.heading, g.px(2.0f));
    flow.text(v.charterName, body, g.px(8.0f));
    flow.title(text::get("GUILD_RANK0_DESC"), m.heading, g.px(2.0f));
    flow.text(v.master, body, g.px(8.0f));
    flow.title(text::get("MEMBERS"), m.heading, g.px(2.0f));
    for (const std::string_view name : v.members) {
        flow.text(name, body, g.px(1.0f));
    }
    flow.y += g.px(8.0f);
    flow.text(v.instructions, body, g.px(4.0f));
    endParchment(g, p, flow);
    // PetitionFrameRequestButton + PetitionFrameRenameButton for the originator, PetitionFrameSignButton
    // otherwise; PetitionFrameCancelButton (CLOSE) at the right.
    const Rect& f = d.footer;
    const f32 gap = g.px(6.0f);
    const f32 bw = (f.w - gap * 2.0f) / 3.0f;
    const Rect left{f.x, f.y, bw, f.h};
    if (v.originator) {
        result.request = ui::button(g, widgetId("petition.request"), left, text::get("REQUEST_SIGNATURE"),
                                    v.requestEnabled);
        result.rename = ui::button(g, widgetId("petition.rename"), {left.right() + gap, f.y, bw, f.h},
                                   text::get("RENAME_GUILD"));
    } else {
        result.sign =
            ui::button(g, widgetId("petition.sign"), left, text::get("SIGN_CHARTER"), v.signEnabled);
    }
    result.closed = result.closed || ui::button(g, widgetId("petition.close"), {f.right() - bw, f.y, bw, f.h},
                                                text::get("CLOSE"));
    return result;
}

// --- TabardFrame
// -----------------------------------------------------------------------------------------------

TabardResult tabardWindow(DialogContext& c, const TabardView& v, ModelFrameState& model) {
    VD_PROFILE_ZONE("GuildWindows::tabard");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    TabardResult result;
    const Dialog d =
        beginDialog(c, g, "tabard", wide ? 700.0f : 460.0f, wide ? 440.0f : 700.0f, v.npcName, true, true);
    result.closed = d.closed;
    const Rect& b = d.body;
    const f32 gap = g.px(6.0f);
    // TabardFrameGreetingText over everything.
    TextStyle greeting = ui::gameFont(m.small, kWhiteText);
    const f32 greetH =
        std::min(g.draw.measureWrapped(b.w, v.greeting, greeting.font, greeting.size), m.small * 3.0f);
    g.draw.wrappedText({b.x, b.y, b.w, greetH + 1.0f}, v.greeting, greeting);
    const f32 top = b.y + greetH + gap;
    // The model in TabardFrameBackground (with the decorative emblems at 40% behind it) and the
    // customizations (TabardFrameCustomization1..5: a label in CharacterCreate-LabelFrame between the page
    // arrows): beside it on wide places, under it on square ones; the cost (TABARDVENDORCOST and the money)
    // last.
    const f32 rowH = std::max(g.px(36.0f), m.text * 1.9f);
    const f32 customH = rowH * 5.0f + gap * 4.0f;
    const f32 costH = m.text * 1.8f;
    Rect modelArea, customArea;
    if (wide) {
        modelArea = {b.x, top, b.w * 0.5f - gap * 0.5f, b.bottom() - top};
        customArea = {modelArea.right() + gap, top, b.w - modelArea.w - gap, b.bottom() - top - costH - gap};
    } else {
        modelArea = {b.x, top, b.w, std::max(b.bottom() - top - customH - costH - gap * 2.0f, g.px(120.0f))};
        customArea = {b.x, modelArea.bottom() + gap, b.w, customH};
    }
    g.draw.sprite(modelArea, kTabardBackground, ui::kWhite, {0.0f, 0.0f, 316.0f / 512.0f, 325.0f / 512.0f});
    // TabardFrameEmblemTopLeft / TopRight (128 x 128) over BottomLeft / BottomRight (128 x 64) at the frame's
    // top right: the left half of each region's texture (u 0..0.5, the emblem's right half), mirrored for the
    // left quad.
    const f32 ew = std::min(modelArea.w * 0.16f, modelArea.h * 0.2f);
    const f32 ex = modelArea.right() - ew * 2.0f - g.px(6.0f);
    const f32 ey = modelArea.y + g.px(6.0f);
    const u32 faded = ui::withAlpha(ui::kWhite, 0.4f);
    if (v.upperEmblem.valid()) {
        g.draw.texture({ex, ey, ew, ew}, v.upperEmblem, faded, {0.5f, 0.0f, 0.0f, 1.0f});
        g.draw.texture({ex + ew, ey, ew, ew}, v.upperEmblem, faded, {0.0f, 0.0f, 0.5f, 1.0f});
    }
    if (v.lowerEmblem.valid()) {
        g.draw.texture({ex, ey + ew, ew, ew * 0.5f}, v.lowerEmblem, faded, {0.5f, 0.0f, 0.0f, 1.0f});
        g.draw.texture({ex + ew, ey + ew, ew, ew * 0.5f}, v.lowerEmblem, faded, {0.0f, 0.0f, 0.5f, 1.0f});
    }
    result.rotated = modelFrame(c, g, widgetId("tabard.model"), modelArea, v.model, model);
    for (u32 i = 0; i < 5; ++i) {
        const Rect row{customArea.x, customArea.y + static_cast<f32>(i) * (rowH + gap), customArea.w, rowH};
        const f32 arrow = rowH;
        const Rect label{row.x + arrow, row.y, row.w - arrow * 2.0f, row.h};
        g.draw.sprite(label, kLabelFrame, ui::kWhite, {0.0f, 0.0f, 1.0f, 0.25f});
        TextStyle s = ui::gameFont(m.small, kWhiteText);
        s.align = Align::Center;
        ui::label(g, label, v.labels[i], s);
        if (artButton(g, widgetId("tabard.left", i), {row.x, row.y, arrow, arrow}, kPrevPageUp, kPrevPageDown,
                      0, true)) {
            result.kind = static_cast<i32>(i);
            result.step = -1;
        }
        if (artButton(g, widgetId("tabard.right", i), {row.right() - arrow, row.y, arrow, arrow}, kNextPageUp,
                      kNextPageDown, 0, true)) {
            result.kind = static_cast<i32>(i);
            result.step = 1;
        }
    }
    {
        // TabardFrameCostFrame: TABARDVENDORCOST and the small money frame, at the bottom right.
        const Rect cost{wide ? customArea.x : b.x, b.bottom() - costH, wide ? customArea.w : b.w, costH};
        ui::tooltipPanel(g, cost, ui::kTooltipBackgroundColor);
        const f32 moneyW = ui::moneyWidth(g.draw, v.cost, m.small);
        ui::money(g, cost.right() - g.px(8.0f), cost.centerY(), v.cost, m.small, Align::Right);
        TextStyle s = ui::gameFont(m.small, kWhiteText);
        s.align = Align::Right;
        ui::label(g, {cost.x, cost.y, cost.w - moneyW - g.px(14.0f), cost.h}, text::get("TABARDVENDORCOST"),
                  s);
    }
    bool accept = false, cancel = false;
    footerButtons(g, d.footer, "tabard", text::get("ACCEPT"), v.acceptEnabled, accept, text::get("CANCEL"),
                  cancel);
    result.accept = accept;
    result.closed = result.closed || cancel;
    return result;
}

} // namespace vd
