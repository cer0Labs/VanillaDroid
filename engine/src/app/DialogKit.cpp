#include "app/DialogKit.h"

#include "app/ChatLinkInsert.h"
#include "app/GlueWindows.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Text.h"
#include "game/ChatLinks.h"
#include "game/SocialText.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vd::kit {
namespace {

constexpr u64 kItemNameFrame = assetPathHash("Interface\\QuestFrame\\UI-QuestItemNameFrame.blp");
constexpr u64 kItemHighlight = assetPathHash("Interface\\QuestFrame\\UI-QuestItemHighlight.blp");
constexpr u64 kTitleHighlight = assetPathHash("Interface\\QuestFrame\\UI-QuestTitleHighlight.blp");
// UIOptionsFrame's art: four textures (512 x 512 above, 512 x 256 below) making 1024 x 768 pixels, split at (512, 512).
constexpr u64 kFullScreenArt[4] = {
    assetPathHash("Interface\\OptionsFrame\\UIOptionsFrame-TopLeft.blp"),
    assetPathHash("Interface\\OptionsFrame\\UIOptionsFrame-TopRight.blp"),
    assetPathHash("Interface\\OptionsFrame\\UIOptionsFrame-BottomLeft.blp"),
    assetPathHash("Interface\\OptionsFrame\\UIOptionsFrame-BottomRight.blp"),
};
// In the art's pixels: the stone bars (top 0..68, bottom 737..768), the side edges (16 and 14), and the ends kept at
// scale (the gold corners and the torn parchment are within 160 of either side).
constexpr f32 kArtWidth = 1024.0f, kArtHeight = 768.0f, kArtTopBar = 68.0f, kArtBottomBar = 32.0f, kArtSide = 16.0f,
              kArtEnd = 160.0f;
constexpr u64 kBoxBackground = assetPathHash("Interface\\Tooltips\\UI-Tooltip-Background.blp");
constexpr u64 kBoxBorder = assetPathHash("Interface\\Tooltips\\UI-Tooltip-Border.blp");

// The last long press on an item cell (immediate mode: drawn and read back on the main thread, one frame).
u32 g_longPressedEntry = 0;
ui::Rect g_longPressedRect;

} // namespace

using ui::Align;
using ui::Gui;
using ui::Rect;
using ui::TextStyle;
using ui::WidgetId;
using ui::widgetId;

// The red buttons: one size for every window button, CharacterSelect's "Back" and "Delete Character" as they show
// (GlueButtonSmallTemplate's 35 units, whose art is solid over 29 of its 48 rows: a body of about 21 units), which is
// UIPanelButtonTemplate's 22 in the glue's units (app/GlueWindows); the touch target stays a finger's (ui::kTouchDp).
constexpr f32 kButtonUnits = 22.0f;

Metrics metrics(const DialogContext& c) {
    const bool wide = c.profile == ScreenProfile::Wide;
    Metrics m;
    if (c.panel.w > 0.0f) {
        // Parity pass (panels): a window beside the world gets less room than a dialog over it; a little smaller,
        // still a finger's size.
        m.text = c.ui.px(13.5f);
        m.small = c.ui.px(12.0f);
        m.title = c.ui.px(18.0f);
        m.heading = c.ui.px(15.5f);
        m.row = c.ui.px(38.0f);
        m.button = glueLayout(c).u(kButtonUnits);
        m.item = c.ui.px(36.0f);
        return m;
    }
    m.text = c.ui.px(wide ? 14.0f : 15.0f);
    m.small = c.ui.px(wide ? 12.5f : 13.0f);
    m.title = c.ui.px(wide ? 19.0f : 21.0f);
    m.heading = c.ui.px(wide ? 16.5f : 18.0f);
    m.row = c.ui.px(wide ? 42.0f : 46.0f);
    m.button = glueLayout(c).u(kButtonUnits);
    m.item = c.ui.px(wide ? 38.0f : 42.0f);
    return m;
}

Dialog beginDialog(DialogContext& c, Gui& g, std::string_view name, f32 widthDp, f32 heightDp,
                   std::string_view title, bool closeButton, bool footer) {
    const Metrics m = metrics(c);
    Dialog d;
    const f32 overhang = title.empty() ? 0.0f : g.unit(12.0f);
    if (c.panel.w > 0.0f) {
        // Parity pass (panels): in its place, no blocker; a tap outside is the world's (d.outside stays false).
        const Rect& place = c.panel;
        const f32 w = std::min(g.px(widthDp), place.w);
        const f32 h = std::min(g.px(heightDp), place.h - overhang);
        const f32 x = c.panelAlign == PanelAlign::BottomRight ? place.right() - w
                      : c.panelAlign == PanelAlign::Center    ? place.centerX() - w * 0.5f
                                                              : place.x;
        const f32 y = c.panelAlign == PanelAlign::BottomRight ? place.bottom() - h
                      : c.panelAlign == PanelAlign::Center    ? place.y + overhang + (place.h - overhang - h) * 0.5f
                                                              : place.y + overhang;
        d.panel = {x, y, w, h};
    } else {
        d.outside = ui::modalBlocker(g, widgetId(name, 1), 0.3f);
        const Rect& safe = c.safeArea;
        const f32 margin = g.px(6.0f);
        const f32 w = std::min(g.px(widthDp), safe.w - 2.0f * margin);
        const f32 h = std::min(g.px(heightDp), safe.h - 2.0f * margin - overhang);
        d.panel = {safe.centerX() - w * 0.5f, safe.y + overhang + (safe.h - overhang - h) * 0.5f, w, h};
    }
    ui::hit(g, widgetId(name, 2), d.panel);
    if (c.panel.w > 0.0f && c.panelOpaque) {
        g.draw.solid(d.panel.inset(g.unit(8.0f)), ui::rgba(0, 0, 0)); // under the 60% black background
    }
    ui::dialogPanel(g, d.panel);
    const Rect content = ui::dialogContent(g, d.panel);
    f32 top = content.y;
    if (!title.empty()) {
        top = std::max(top, ui::dialogHeader(g, d.panel, title) + g.px(2.0f));
    }
    if (closeButton) {
        const f32 size = g.px(32.0f);
        const Rect r{d.panel.right() - size - g.unit(4.0f), d.panel.y + g.unit(4.0f), size, size};
        d.closed = ui::closeButton(g, widgetId(name, 3), r);
        top = std::max(top, r.bottom() - g.px(6.0f));
    }
    d.footer = footer ? Rect{content.x, content.bottom() - m.button, content.w, m.button}
                      : Rect{content.x, content.bottom(), content.w, 0.0f};
    d.body = {content.x, top, content.w, (footer ? d.footer.y - g.px(6.0f) : content.bottom()) - top};
    return d;
}

Dialog beginFullScreen(DialogContext& c, Gui& g, std::string_view name, std::string_view title, bool footer) {
    const Metrics m = metrics(c);
    Dialog d;
    const Rect screen{0.0f, 0.0f, g.ui.width(), g.ui.height()};
    d.panel = screen;
    ui::modalBlocker(g, widgetId(name, 1), 0.0f);
    g.draw.solid(screen, ui::rgba(0, 0, 0)); // UIOptionsBlackground
    // The art as tall as the screen; across, its ends at that scale and the middle stretched (or squeezed) to fit.
    const f32 s = screen.h / kArtHeight;
    const f32 end = std::min(kArtEnd * s, screen.w * 0.25f);
    const auto screenX = [&](f32 artX) {
        if (artX <= kArtEnd) {
            return screen.x + artX / kArtEnd * end;
        }
        if (artX >= kArtWidth - kArtEnd) {
            return screen.right() - (kArtWidth - artX) / kArtEnd * end;
        }
        return screen.x + end + (artX - kArtEnd) / (kArtWidth - 2.0f * kArtEnd) * (screen.w - 2.0f * end);
    };
    for (u32 i = 0; i < 4; ++i) {
        const TextureHandle art = c.resources.texture(kFullScreenArt[i]);
        if (!art.valid()) {
            continue; // still loading: black for a frame or two
        }
        const f32 artX = i % 2 == 0 ? 0.0f : 512.0f;
        const f32 artY = i < 2 ? 0.0f : 512.0f;
        const f32 artH = i < 2 ? 512.0f : 256.0f;
        // Two pieces across each texture: an end (kept) and its half of the middle (stretched).
        const f32 cuts[3] = {artX, i % 2 == 0 ? kArtEnd : kArtWidth - kArtEnd, artX + 512.0f};
        for (u32 k = 0; k < 2; ++k) {
            const f32 x0 = screenX(cuts[k]), x1 = screenX(cuts[k + 1]);
            g.draw.texture({x0, screen.y + artY * s, x1 - x0, artH * s}, art, ui::kWhite,
                           {(cuts[k] - artX) / 512.0f, 0.0f, (cuts[k + 1] - artX) / 512.0f, 1.0f});
        }
    }
    const f32 topBar = kArtTopBar * s;
    if (!title.empty()) { // UIOptionsFrameTitle: GameFontNormal in the top bar
        TextStyle style = ui::gameFont(std::min(g.px(16.0f), topBar * 0.5f), ui::kGold);
        style.align = Align::Center;
        ui::label(g, {c.safeArea.x, std::max(screen.y, c.safeArea.y), c.safeArea.w, std::max(topBar, g.px(20.0f))},
                  title, style);
    }
    // The parchment, within the safe area.
    const f32 margin = g.px(8.0f);
    const f32 left = std::max(screen.x + kArtSide * s, c.safeArea.x) + margin;
    const f32 right = std::min(screen.right() - kArtSide * s, c.safeArea.right()) - margin;
    const f32 top = std::max(screen.y + topBar, c.safeArea.y + g.px(20.0f)) + g.px(4.0f);
    const f32 bottom = std::min(screen.bottom() - kArtBottomBar * s, c.safeArea.bottom()) - margin;
    const Rect content{left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
    d.footer = footer ? Rect{content.x, content.bottom() - m.button, content.w, m.button}
                      : Rect{content.x, content.bottom(), content.w, 0.0f};
    d.body = {content.x, content.y, content.w,
              std::max(0.0f, (footer ? d.footer.y - g.px(8.0f) : content.bottom()) - content.y)};
    return d;
}

void optionBox(Gui& g, const Rect& r) {
    g.draw.sprite(r.inset(g.unit(5.0f)), kBoxBackground, ui::rgba(128, 128, 128)); // SetBackdropColor(0.5, 0.5, 0.5)
    g.draw.backdropBorder(r, kBoxBorder, g.unit(16.0f), ui::rgba(102, 102, 102)); // SetBackdropBorderColor(0.4, ...)
}

// As the glue's small buttons show: "Back" about 92 units across a body 22 high, CharSelectCreateCharacterButton its
// text and about 46 more.
f32 buttonWidth(const Gui& g, std::string_view text, f32 height) {
    return std::max(height * (92.0f / kButtonUnits),
                    g.draw.measure(text, ui::FontId::Friz, ui::buttonFontSize(g, height)) +
                        height * (46.0f / kButtonUnits));
}

void footerButtons(Gui& g, const Rect& footer, std::string_view name, std::string_view left, bool leftEnabled,
                   bool& leftTapped, std::string_view right, bool& rightTapped) {
    leftTapped = rightTapped = false;
    if (!left.empty()) {
        const f32 w = std::min(buttonWidth(g, left, footer.h), footer.w * 0.48f);
        leftTapped = ui::button(g, widgetId(name, 10), {footer.x, footer.y, w, footer.h}, left, leftEnabled);
    }
    if (!right.empty()) {
        const f32 w = std::min(buttonWidth(g, right, footer.h), footer.w * 0.48f);
        rightTapped = ui::button(g, widgetId(name, 11), {footer.right() - w, footer.y, w, footer.h}, right);
    }
}

ParchmentScroll beginParchment(Gui& g, WidgetId id, const Rect& r) {
    ui::parchment(g, r);
    ParchmentScroll p;
    p.view = ui::beginScroll(g, id, r.inset(g.px(3.0f)));
    p.padding = g.px(10.0f);
    return p;
}

Flow parchmentFlow(Gui& g, const ParchmentScroll& p) {
    return {&g, p.view.viewport.x + p.padding, p.view.viewport.w - 2.0f * p.padding,
            p.view.top + p.padding * 0.8f};
}

void endParchment(Gui& g, const ParchmentScroll& p, const Flow& flow) {
    ui::endScroll(g, p.view, flow.y - p.view.top + p.padding);
}

f32 wrappedRow(Gui& g, WidgetId id, f32 x, f32 y, f32 w, f32 minHeight, u64 icon, u32 iconTint,
               std::string_view text, const TextStyle& style, bool& tappedOut) {
    const f32 iconSize = style.size * 1.25f;
    const f32 textX = x + g.px(6.0f) + (icon ? iconSize + g.px(8.0f) : 0.0f);
    const f32 textW = x + w - g.px(6.0f) - textX;
    const f32 textH = g.draw.measureWrapped(textW, text, style.font, style.size);
    const f32 h = std::max(minHeight, textH + g.px(12.0f));
    const Rect r{x, y, w, h};
    const ui::WidgetState state = ui::hit(g, id, r);
    if (ui::held(state, r)) {
        g.draw.sprite(r, kTitleHighlight);
    }
    const f32 textY = y + (h - textH) * 0.5f;
    if (icon) {
        const f32 firstLine = textY + g.draw.lineHeight(style.font, style.size) * 0.5f;
        g.draw.sprite({x + g.px(6.0f), firstLine - iconSize * 0.5f, iconSize, iconSize}, icon, iconTint);
    }
    g.draw.wrappedText({textX, textY, textW, textH + 1.0f}, text, style);
    tappedOut = ui::tapped(state);
    return h;
}

bool questItem(DialogContext& c, Gui& g, WidgetId id, const Rect& cell, const ItemView& item, bool selected,
               bool selectable, bool linkable, UnusableTint tint, f32 artScale) {
    // Retouches (quest windows): at the art's scale, QuestItemTemplate's 39-unit icon at the top left of its 147 x 41.
    const bool art = artScale > 0.0f;
    const f32 k = artScale;
    const Rect icon = art ? Rect{cell.x, cell.y, 39.0f * k, 39.0f * k} : Rect{cell.x, cell.y, cell.h, cell.h};
    const bool red = !item.usable && tint != UnusableTint::None;
    const u32 iconTint = !red ? ui::kWhite : tint == UnusableTint::Choice ? ui::rgba(230, 0, 0) : ui::rgba(128, 0, 0);
    const u32 frameTint = !red ? ui::kWhite : tint == UnusableTint::Choice ? ui::rgba(230, 0, 0) : ui::rgba(255, 0, 0);
    // UI-QuestItemNameFrame: the frame is texels 11..117 x 11..53, starting under the icon's right edge (at the art's
    // scale: the 128 x 64 texture 10 units into the icon, centered on it, its frame from 40 units across, 1.5 above).
    const f32 frameX = art ? cell.x + 40.0f * k : icon.right() - cell.h * 0.12f;
    const Rect frame = art ? Rect{frameX, icon.centerY() - 21.0f * k, cell.right() - k - frameX, 42.0f * k}
                           : Rect{frameX, cell.y, cell.right() - frameX, cell.h};
    ui::threeSlice(g.draw, frame, kItemNameFrame,
                   {11.0f / 128.0f, 11.0f / 64.0f, 117.0f / 128.0f, 53.0f / 64.0f}, 16.0f / 128.0f,
                   frame.h * 16.0f / 42.0f, frameTint);
    ui::SlotView slot;
    slot.icon = item.iconHash ? c.resources.icon(item.iconHash) : std::nullopt;
    slot.count = item.count;
    slot.quality = item.quality;
    slot.tint = iconTint;
    if (art) {
        // SetItemButtonTexture on the plain icon (no slot frame), SetItemButtonCount at its bottom right (-4, 1) in
        // NumberFontNormal.
        ui::iconOrPlaceholder(g.draw, icon, slot.icon, iconTint);
        if (item.count > 1) {
            char count[12];
            std::snprintf(count, sizeof(count), "%u", item.count);
            ui::TextStyle style = ui::numberFont(std::max(14.0f * k, g.px(11.0f)));
            style.align = Align::Right;
            g.draw.text(icon.right() - 4.0f * k, icon.bottom() - 3.0f * k, count, style);
        }
    } else {
        ui::itemSlot(g, widgetId("questItem.icon", id), icon, slot);
    }
    const ui::WidgetState state = ui::hit(g, id, cell); // over the icon: the cell takes the finger
    // Phase 13 (chat links): the chat open, a tap links the item (GetQuestItemLink) and does nothing else.
    const bool link = linkable && c.chatLink && c.chatLink->active() && item.entry != 0 && ui::tapped(state);
    if (link) {
        char text[256];
        c.chatLink->insert(game::formatItemLink(text, item.entry, game::itemQualityColor(static_cast<u8>(item.quality)),
                                                item.name));
    }
    // Its tooltip (the client's on hover): a long press, or a tap when the tap has nothing else to do.
    if (!link && (state.longPress || (!selectable && ui::tapped(state))) && item.entry != 0) {
        g_longPressedEntry = item.entry;
        g_longPressedRect = cell;
    }
    const bool down = selectable && ui::held(state, cell);
    if (selected || down) {
        // QuestRewardItemHighlight (QuestFrame.xml: UI-QuestItemHighlight, alphaMode ADD), which
        // QuestRewardItem_OnClick puts 8 units left of and 7 above the 147 x 41 button: its icon box (texels 9..43
        // square) lies over the 39-unit icon from (1, 2), its name box (texels 49..151 x 7..44) over the name frame's
        // visible part (texels 11..116 x 11..52 of UI-QuestItemNameFrame, from 1 unit right of the icon), 1 unit inside
        // it across, 1.5 below its top and 2.5 above its bottom. Each box whole on its piece; the name's 3-sliced so
        // its glowing ends keep their size.
        const u32 color = ui::withAlpha(ui::kWhite, down && !selected ? 0.6f : 1.0f);
        const f32 iu = icon.h / 39.0f; // a unit of the icon
        g.draw.spriteAdditive({icon.x + iu, icon.y + 2.0f * iu, 35.0f * iu, 35.0f * iu}, kItemHighlight, color,
                              {9.0f / 256.0f, 9.0f / 64.0f, 44.0f / 256.0f, 44.0f / 64.0f});
        const f32 fu = frame.h / 42.0f; // a texel row of the name frame's visible part
        const f32 left = std::max(frame.x + fu, icon.right() + 2.0f * iu);
        const Rect box{left, frame.y + 1.5f * fu, frame.right() - fu - left, frame.h - 4.0f * fu};
        constexpr ui::TexCoords kNameBox{49.0f / 256.0f, 7.0f / 64.0f, 152.0f / 256.0f, 45.0f / 64.0f};
        constexpr f32 kCapU = 12.0f / 256.0f;
        const f32 cap = std::min(12.0f * box.h / 38.0f, box.w * 0.5f);
        g.draw.spriteAdditive({box.x, box.y, cap, box.h}, kItemHighlight, color,
                              {kNameBox.u0, kNameBox.v0, kNameBox.u0 + kCapU, kNameBox.v1});
        g.draw.spriteAdditive({box.x + cap, box.y, box.w - 2.0f * cap, box.h}, kItemHighlight, color,
                              {kNameBox.u0 + kCapU, kNameBox.v0, kNameBox.u1 - kCapU, kNameBox.v1});
        g.draw.spriteAdditive({box.right() - cap, box.y, cap, box.h}, kItemHighlight, color,
                              {kNameBox.u1 - kCapU, kNameBox.v0, kNameBox.u1, kNameBox.v1});
    }
    // The name: up to two lines, in the item's quality color; at the art's scale QuestItemTemplate's
    // (GameFontHighlight, white, in 90 x 36 units 15 into the name frame's texture).
    TextStyle style = art ? ui::gameFont(std::max(12.0f * k, g.px(10.0f)), ui::kWhite)
                          : ui::gameFont(std::min(g.px(14.0f), cell.h * 0.34f), ui::qualityColor(item.quality));
    const Rect text = art ? Rect{cell.x + 44.0f * k, icon.centerY() - 18.0f * k, std::max(0.0f, cell.w - 57.0f * k),
                                 36.0f * k}
                          : Rect{icon.right() + g.px(8.0f), cell.y, frame.right() - icon.right() - g.px(16.0f), cell.h};
    // At the art's scale a long name in a cell narrower than the original's may take a third line within the cell.
    const f32 height = g.draw.measureWrapped(text.w, item.name, style.font, style.size);
    const f32 lineH = g.draw.lineHeight(style.font, style.size);
    if (height <= lineH * 2.05f || (art && height <= std::min(lineH * 3.05f, cell.h))) {
        g.draw.wrappedText({text.x, text.centerY() - height * 0.5f, text.w, height + 1.0f}, item.name, style);
    } else {
        ui::label(g, text, item.name, style);
    }
    return !link && selectable && ui::tapped(state);
}

void requestItemTooltip(u32 entry, const Rect& anchor) {
    g_longPressedEntry = entry;
    g_longPressedRect = anchor;
}

bool takeLongPressedItem(u32& entry, Rect& anchor) {
    if (g_longPressedEntry == 0) {
        return false;
    }
    entry = g_longPressedEntry;
    anchor = g_longPressedRect;
    g_longPressedEntry = 0;
    return true;
}

i32 questItems(DialogContext& c, Flow& flow, std::string_view name, u64 idBase,
               std::span<const ItemView> items, i32 selected, bool selectable, UnusableTint tint, f32 artScale) {
    if (items.empty()) {
        return -1;
    }
    Gui& g = *flow.g;
    const Metrics m = metrics(c);
    const bool art = artScale > 0.0f;
    // Retouches (quest windows): the 1.12's two to a row, 147 x 41 units (narrower when two do not fit), 1 apart
    // across and 2 down, the first 3 units left of the text.
    const f32 k = artScale;
    const f32 gap = art ? k : g.px(8.0f);
    const f32 rowGap = art ? 2.0f * k : g.px(8.0f);
    const f32 x0 = art ? flow.x - 3.0f * k : flow.x;
    const u32 columns = art || flow.w >= g.px(400.0f) ? 2u : 1u;
    const f32 room = art ? flow.w + 3.0f * k : flow.w;
    const f32 cellW = art ? std::min(147.0f * k, (room - gap) * 0.5f)
                          : (flow.w - gap * static_cast<f32>(columns - 1)) / static_cast<f32>(columns);
    const f32 cellH = art ? 41.0f * k : m.item;
    i32 tapped = -1;
    for (usize i = 0; i < items.size(); ++i) {
        const u32 column = static_cast<u32>(i % columns);
        const Rect cell{x0 + static_cast<f32>(column) * (cellW + gap), flow.y, cellW, cellH};
        if (questItem(c, g, widgetId(name, idBase + i), cell, items[i], static_cast<i32>(i) == selected,
                      selectable, true, tint, artScale)) {
            tapped = static_cast<i32>(i);
            if (items[i].entry != 0) { // a reward chosen: its tooltip too, to compare before completing
                g_longPressedEntry = items[i].entry;
                g_longPressedRect = cell;
            }
        }
        if (column + 1 == columns || i + 1 == items.size()) {
            flow.y += cellH + (art && i + 1 == items.size() ? 0.0f : rowGap);
        }
    }
    return tapped;
}

QuestArtSizes questArtSizes(const Gui& g, f32 k) {
    const auto font = [&](f32 units) { return std::max(units * k, g.px(10.0f)); };
    QuestArtSizes s;
    s.k = k;
    s.normal = font(12.0f);
    s.small = font(10.0f);
    // The quests' own texts a little bigger than the original's (easier to read on a phone): 20, 15 and 14.
    s.questTitle = font(20.0f);
    s.quest = font(15.0f);
    s.objective = font(14.0f);
    s.row = std::round(std::max(17.0f * k, g.px(24.0f)));
    return s;
}

i32 questArtRewards(DialogContext& c, Flow& flow, const QuestArtSizes& s, std::string_view name,
                    const QuestArtRewards& r) {
    const bool spell = !r.spellText.empty();
    if (r.choices.empty() && r.rewards.empty() && !spell && r.money == 0) {
        return -1;
    }
    Gui& g = *flow.g;
    const TextStyle body = ui::questFont(s.quest);
    flow.title(text::get("QUEST_REWARDS"), s.questTitle, s.u(5.0f));
    i32 tapped = -1;
    if (!r.choices.empty()) {
        flow.text(r.chooseText, body, s.u(5.0f));
        tapped = questItems(c, flow, name, 0, r.choices, r.selected, r.selectable, UnusableTint::Choice, s.k);
        flow.y += s.u(5.0f);
    }
    if (spell) {
        flow.text(r.spellText, body, s.u(5.0f));
        const ItemView cell[] = {r.spell};
        questItems(c, flow, name, 500, cell, -1, false, UnusableTint::None, s.k);
        flow.y += s.u(5.0f);
    }
    if (!r.rewards.empty() || r.money > 0) {
        // "You will also receive:" after choices or a spell, "You will receive:" alone; the coins 15 right of it
        // (under it when they do not fit beside).
        const std::string_view receive = text::get(!r.choices.empty() || spell ? "REWARD_ITEMS" : "REWARD_ITEMS_ONLY");
        const f32 lineH = g.draw.lineHeight(body.font, body.size);
        const f32 textW = std::min(flow.w, g.draw.measure(receive, body.font, body.size));
        const f32 h = flow.text(receive, body, 0.0f);
        if (r.money > 0) {
            const f32 coinsX = flow.x + textW + s.u(15.0f);
            if (coinsX + ui::moneyWidth(g.draw, r.money, s.quest) <= flow.x + flow.w) {
                ui::money(g, coinsX, flow.y - h + lineH * 0.5f, r.money, s.quest, Align::Left);
            } else {
                ui::money(g, flow.x, flow.y + lineH * 0.5f, r.money, s.quest, Align::Left);
                flow.y += lineH;
            }
        }
        flow.y += s.u(5.0f);
        questItems(c, flow, name, 1000, r.rewards, -1, false, UnusableTint::Reward, s.k);
    }
    return tapped;
}

} // namespace vd::kit
