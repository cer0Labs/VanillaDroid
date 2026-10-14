#pragma once

#include "core/Types.h"
#include "ui/Context.h"
#include "ui/DrawList.h"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace vd::ui {

// The touch UI's controls (phase 7): immediate mode on a Context (input) and a DrawList (quads), drawn with
// the original client's interface art as its FrameXML templates lay it out (UIPanelTemplates, DialogFrame,
// QuestFrame, MoneyFrame, OptionsFrameTemplates...), at sizes a finger can hit. Each call registers its hit
// rectangle and appends its quads in call order (later = on top). Rectangles in logical pixels; the *Dp
// constants are density-independent pixels (Context::px). Game thread only: scroll views keep their state in
// a small table of this module.

constexpr f32 kUnitDp = 0.78f;  // one unit of the client's 768-high interface, in dp (as the HUD)
constexpr f32 kTouchDp = 44.0f; // the smallest touch target

// Text colors of the client (Fonts.xml, QuestFrame.lua).
constexpr u32 kDisabledText = rgba(128, 128, 128);             // GameFontDisable
constexpr u32 kParchmentText = rgba(46, 31, 15);               // MATERIAL_TEXT_COLOR_TABLE.Parchment
constexpr u32 kParchmentTitle = rgba(0, 0, 0);                 // QuestTitleFont
constexpr u32 kParchmentTitleShadow = rgba(125, 89, 13);       // its shadow
constexpr u32 kTooltipBackgroundColor = rgba(23, 23, 48, 230); // TOOLTIP_DEFAULT_BACKGROUND_COLOR

// Item quality colors (ITEM_QUALITY_COLORS): 0 poor, 1 common, 2 uncommon, 3 rare, 4 epic, 5 legendary, 6
// artifact.
u32 qualityColor(u32 quality);
// Level color of a quest against the player's level (GetDifficultyColor in QuestLogFrame.lua); playerLevel 0:
// gold.
u32 questDifficultyColor(u32 questLevel, u32 playerLevel);

// What every widget draws with.
struct Gui {
    Context& ui;
    DrawList& draw;

    f32 px(f32 dp) const { return ui.px(dp); }
    // Client interface units (the FrameXML sizes) in logical pixels.
    f32 unit(f32 units) const { return ui.px(units * kUnitDp); }
};

// --- Text --------------------------------------------------------------------------------------------------

// The client's font objects (Fonts.xml) at a size in logical pixels.
TextStyle gameFont(f32 size, u32 color = kGold);           // GameFontNormal: Friz, 1 px shadow
TextStyle numberFont(f32 size, u32 color = kWhite);        // NumberFontNormal: Arial Narrow, outlined
TextStyle chatFont(f32 size, u32 color = kWhite);          // ChatFontNormal: Arial Narrow, 1 px shadow
TextStyle questFont(f32 size, u32 color = kParchmentText); // QuestFont on parchment: Friz, no shadow
// A texture region stretched horizontally: caps of capU (of the region's uv) drawn capWidth pixels wide, the
// middle stretched (UIPanelButtonTemplate2, InputBoxTemplate, the dialog header).
void threeSlice(DrawList& draw, const Rect& r, u64 hash, TexCoords uv, f32 capU, f32 capWidth,
                u32 color = kWhite);
// Baseline that centers a line of `style` vertically on `centerY`.
f32 centeredBaseline(const DrawList& draw, const TextStyle& style, f32 centerY);
// Phase 20 (retouches): the baseline of one line of a FontString of FontHeight `fontHeight` (Fonts.xml: 12
// for GameFontNormal; its TextStyle size is fontHeight * 1.2) centred on `centerY`, as WoW.exe lays it: the
// line is fontHeight tall and its baseline fontHeight * ascender / (ascender + |descender|) under its top
// (0x5CA0A6).
f32 fontStringBaseline(const DrawList& draw, FontId font, f32 fontHeight, f32 centerY);
// One line of text inside `r` (vertically centered, aligned by style.align), shortened with "..." when too
// wide.
void label(Gui& g, const Rect& r, std::string_view text, const TextStyle& style);
// Title in the parchment style (QuestTitleFont: Morpheus, black with a golden shadow). Returns the height
// used.
f32 parchmentTitle(Gui& g, const Rect& r, std::string_view text, f32 size);

// --- Panels ------------------------------------------------------------------------------------------------

// The dialog backdrop (UI-DialogBox-Background inside UI-DialogBox-Border), as StaticPopup and GameMenuFrame.
// Its background is a uniform 60% black: the world shows through, as in the client.
void dialogPanel(Gui& g, const Rect& r);
// The UI-DialogBox-Header plate on top of a panel with `title` in it; stretched when the title is long.
// Returns the y below which the panel's content is clear of it.
f32 dialogHeader(Gui& g, const Rect& panel, std::string_view title);
// Inside of a dialogPanel clear of its border.
Rect dialogContent(const Gui& g, const Rect& panel);
// The tooltip backdrop (UI-Tooltip-Background tinted, UI-Tooltip-Border).
void tooltipPanel(Gui& g, const Rect& r, u32 background = kTooltipBackgroundColor);
// The quest frame's parchment (UI-QuestGreeting-*), stretched over `r`.
void parchment(Gui& g, const Rect& r);
// The ornamental line between quest sections (UI-HorizontalBreak), centered on y.
void horizontalBreak(Gui& g, f32 x, f32 centerY, f32 width);
// A modal layer: every finger that lands anywhere belongs to it (nothing reaches the world or the HUD below),
// with the screen dimmed by `dim` (0..1). Returns true when tapped (outside whatever is drawn on top of it).
bool modalBlocker(Gui& g, WidgetId id, f32 dim = 0.35f);

// --- Buttons and controls ----------------------------------------------------------------------------------

// Context::widget with the hit rectangle cut to the current clip (content scrolled out of a view can't be
// touched).
WidgetState hit(Gui& g, WidgetId id, const Rect& r);
// A tap on a widget that counts: released on it and not taken over by a scroll view dragging.
bool tapped(const WidgetState& state);
// A finger is holding the widget down (and no scroll view took it).
bool held(const WidgetState& state, const Rect& r);

// Red panel button (UIPanelButtonTemplate, 3-slice so it stretches). Returns true when tapped.
bool button(Gui& g, WidgetId id, const Rect& r, std::string_view text, bool enabled = true);
// The text of a red button `height` high: UIPanelButtonTemplate's proportion (GameFontNormal's 12 units on 22, as
// CharacterSelect's "Back" and "Delete Character" show it: one size for every window button), at most 15 dp.
f32 buttonFontSize(const Gui& g, f32 height);
// The panel's close "X" (UIPanelCloseButton) inside the touch target `r`.
bool closeButton(Gui& g, WidgetId id, const Rect& r);
// Round (+) / (-) button (UI-PlusButton / UI-MinusButton) inside the touch target `r`.
bool plusMinusButton(Gui& g, WidgetId id, const Rect& r, bool plus, bool enabled = true);
// Check box (UICheckButtonTemplate) with its label; the whole row toggles it. Returns true when it changed.
bool checkbox(Gui& g, WidgetId id, const Rect& r, std::string_view text, bool& checked);
// Horizontal slider (OptionsSliderTemplate): the finger drags the thumb anywhere along the row. `step` > 0
// snaps. Returns true while the value changes.
bool slider(Gui& g, WidgetId id, const Rect& r, f32& value, f32 min, f32 max, f32 step = 0.0f);
// A text box (InputBoxTemplate: Common-Input-Border) showing `text`, or `placeholder` in gray when empty,
// with a blinking caret when `focused`; the end of a long text stays visible. Draws only: returns true when
// tapped (the caller starts the platform's text input).
bool textField(Gui& g, WidgetId id, const Rect& r, std::string_view text, std::string_view placeholder,
               bool focused);

// --- Scroll view -------------------------------------------------------------------------------------------

// State of one vertical scroll view between frames. Pure (no drawing, no Context): scrollStep drives it.
struct ScrollState {
    WidgetId id = 0;
    f32 offset = 0.0f;       // content pixels scrolled out above the viewport
    f32 velocity = 0.0f;     // pixels per second the offset grows by (fling)
    f32 content = 0.0f;      // content height last frame
    f32 viewport = 0.0f;     // viewport height
    i32 touch = -1;          // the finger dragging it (-1: none)
    u64 touchDownMs = 0;     // when that finger went down (ids are reused)
    u64 lastMs = 0;          // last step
    bool stickToEnd = false; // stays at the end while it is there and the content grows (chat)
    bool atEnd = true;
    // Nothing in it to pick up (a text): a finger held still past its long press before moving still scrolls.
    bool longPressScrolls = false;
    // Where the dragging finger was lately (ring): its speed over the last kVelocityWindowMs, whatever the
    // frame rate and the touch sampling rate.
    struct Sample {
        u64 ms = 0;
        f32 y = 0.0f;
    };
    static constexpr u32 kSamples = 16;
    static constexpr u64 kVelocityWindowMs = 100;
    std::array<Sample, kSamples> samples{};
    u32 sampleCount = 0;

    f32 maxOffset() const { return content > viewport ? content - viewport : 0.0f; }
};

// One frame of a scroll view: a finger that went down inside `viewport` and moves vertically (beyond the tap
// slop, more than sideways) takes it and drags the content; released, the content keeps the finger's speed
// (measured over its last 100 ms) and slows down (fling); a finger landing on a flinging view stops it (and
// taps nothing). The offset never leaves [0, content - viewport]. `dp`: logical pixels per dp.
void scrollStep(ScrollState& s, std::span<const Touch> touches, const Rect& viewport, f32 dp, u64 nowMs);
// Content height changed (known after drawing it): clamps, and keeps a view that sticks to the end at the
// end.
void scrollSetContent(ScrollState& s, f32 contentHeight, f32 viewportHeight);

struct ScrollView {
    WidgetId id = 0;
    Rect viewport;
    f32 top = 0.0f; // y of the content's top this frame: draw the content from here down
    f32 offset = 0.0f;
    bool scrolling = false; // dragged or flinging
};
// Starts a clipped, vertically scrolling region: draw its content between beginScroll and endScroll from
// view.top down, then pass the height it took (known only after drawing). Widgets inside ignore fingers the
// view takes. `stickToEnd`: it opens at the end and follows new content while there (chat). A view not drawn
// for half a second (a closed dialog) starts over. `indicator`: the thin bar along the right edge (off where the
// finger alone is enough, e.g. the touch drawer).
ScrollView beginScroll(Gui& g, WidgetId id, const Rect& viewport, bool stickToEnd = false);
void endScroll(Gui& g, const ScrollView& view, f32 contentHeight, bool indicator = true);
// Retouches (quest log): UIPanelScrollBarTemplate for a scroll view, drawn after endScroll (without its indicator):
// `bar` is the column it takes, 16 units wide, the up and down buttons (UI-ScrollBar-ScrollUpButton and
// -ScrollDownButton, disabled at their end) at its ends and the knob (UI-ScrollBar-Knob) between them where the view
// is. With nothing to scroll both buttons are disabled and the knob hidden (ScrollFrame_OnScrollRangeChanged). A tap
// on a button scrolls half the bar's height (their OnClick); a finger on the bar takes the knob there. Each target is
// a finger wide, grown to the right of the bar only (the view's content is at its left), up to `rightLimit` (retouches:
// the character frame's turned tabs right of its lists; past it the target grows to the left instead). Returns true
// when a button scrolled (their PlaySound("UChatScrollButton")).
bool panelScrollBar(Gui& g, const ScrollView& view, const Rect& bar, f32 rightLimit = 1e30f);
// Whether a scroll view has taken this finger (a drag, not a tap).
bool scrollClaimed(const Touch* touch);
// Moves a scroll view (e.g. to the top when its content is replaced).
void scrollTo(WidgetId id, f32 offset);

// --- Lists, items, money -----------------------------------------------------------------------------------

enum class RowHighlight : u8 {
    Quest,    // UI-QuestTitleHighlight: gossip and quest titles on parchment
    QuestLog, // UI-QuestLogTitleHighlight: the quest log (tinted by the text color)
    Listbox,  // UI-Listbox-Highlight
};
struct RowStyle {
    TextStyle text;
    u64 icon = 0;          // atlas sprite (client texture path hash) left of the text, 0: none
    f32 iconSize = 0.0f;   // logical pixels (0: the row's text size)
    bool selected = false; // highlighted, as when pressed
    RowHighlight highlight = RowHighlight::Quest;
};
// One tappable line of a list: optional icon, then the text (shortened to fit). Returns true when tapped.
bool listRow(Gui& g, WidgetId id, const Rect& r, std::string_view text, const RowStyle& style);

struct SlotView {
    std::optional<u32> icon; // icon cache slot (UiResources::icon); nullopt: still loading or none
    u32 count = 0;           // stack size, shown when > 1
    u32 quality = 1;         // not drawn (retouches: 1.12's ItemButtonTemplate shows none)
    f32 cooldown = 0.0f;     // remaining share of a cooldown, 0..1: darkened sweep
    bool usable = true;      // false: tinted red, as out of range or unusable
    bool empty = false;      // an empty slot (UI-EmptySlot)
    bool selected = false;   // highlighted (a chosen reward)
    // Retouches (windows): SetItemButtonDesaturated (a locked item, read mail, a talent out of reach) and the
    // SetItemButtonTextureVertexColor + NormalTextureVertexColor pair (the merchant's sold-out grey).
    bool desaturated = false;
    u32 tint = kWhite;
    bool emptyArt = true;    // phase 15 (items): false: an empty slot shows what is under it (a ContainerFrame's art)
};
// Retouches (windows): an icon of the icon cache over `r` (`uv` as DrawList::icon), or while it loads (or has
// none) the client's INV_Misc_QuestionMark from the atlas, as it shows an icon it does not know.
void iconOrPlaceholder(DrawList& draw, const Rect& r, std::optional<u32> icon, u32 color = kWhite,
                       TexCoords uv = {0.0f, 0.0f, 1.0f, 1.0f}, bool desaturated = false);
// An item or spell button (ItemButtonTemplate: the icon in UI-Quickslot2). `r` is the icon; the frame art
// reaches past it. The state's `clicked` is only set for real taps (scroll drags filtered out).
WidgetState itemSlot(Gui& g, WidgetId id, const Rect& r, const SlotView& slot);

struct Coins {
    u32 gold = 0;
    u32 silver = 0;
    u32 copper = 0;
};
constexpr Coins splitMoney(u32 copper) {
    return {copper / 10000u, copper / 100u % 100u, copper % 100u};
}
// Which denominations the client shows for an amount (MoneyFrame_Update): gold if any; silver if any, or
// between gold and copper; copper if any, or when the amount is 0.
struct CoinsShown {
    bool gold = false;
    bool silver = false;
    bool copper = false;
};
CoinsShown coinsShown(u32 copper);
// "1g 5s 30c" (the shown denominations, as in text; VD_MONEY_* of core/Text). Returns the characters written (without
// the terminator).
usize formatMoney(char* out, usize capacity, u32 copper);
// Money as the MoneyFrame draws it: each amount (NumberFontNormal) followed by its coin (UI-MoneyIcons),
// ending at `right` (Align::Right) or starting at `x` (Align::Left), centered on centerY. Returns the width.
// Phase 18 (extras): `alpha` as the frame's SetAlpha (the auctions' starting price at 0.5). Retouches
// (windows): `color` as SetMoneyFrameColor, the numbers only (RED_FONT_COLOR kRed: what cannot be paid).
f32 money(Gui& g, f32 x, f32 centerY, u32 copper, f32 size, Align align = Align::Right, f32 alpha = 1.0f,
          u32 color = kWhite);
f32 moneyWidth(const DrawList& draw, u32 copper, f32 size);

} // namespace vd::ui
