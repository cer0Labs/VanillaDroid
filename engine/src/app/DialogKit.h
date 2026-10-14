#pragma once

#include "app/Dialogs.h"
#include "ui/Widgets.h"

#include <span>
#include <string_view>

namespace vd::kit {

// Building blocks shared by the touch UI's windows (app/Dialogs and the phase-8 windows): the screen
// profile's sizes, the modal panel with its header and footer, parchment scroll views with flowing content,
// list rows and quest-style item cells. Same rules as Dialogs: pure drawing, no game state.

// Sizes of a screen profile, in logical pixels.
struct Metrics {
    f32 text;    // body text
    f32 small;   // secondary text
    f32 title;   // quest titles (Morpheus)
    f32 heading; // section headings (Morpheus)
    f32 row;     // list rows
    f32 button;  // button height (their touch target is at least kTouchDp)
    f32 item;    // item icons
};

Metrics metrics(const DialogContext& c);

// A modal panel: its rectangles once drawn.
struct Dialog {
    ui::Rect panel;
    ui::Rect body;        // clear of the border, the header and the footer
    ui::Rect footer;      // the button row
    bool closed = false;  // its close button was tapped
    bool outside = false; // a tap outside the panel
};

// The blocker, the backdrop, the header with `title` (none if empty) and the close button of a centered panel
// of widthDp x heightDp (at most the safe area). Parity pass: with a place in c.panel, a panel there instead (no
// blocker, at most the place's size, aligned by c.panelAlign).
Dialog beginDialog(DialogContext& c, ui::Gui& g, std::string_view name, f32 widthDp, f32 heightDp,
                   std::string_view title, bool closeButton = true, bool footer = true);

// The option windows (video, sound, interface: all three over the whole screen as 1.12's UIOptionsFrame, where
// OptionsFrame and SoundOptionsFrame were dialogs): UIOptionsFrame's look (setAllPoints, FULLSCREEN strata): black
// under it (UIOptionsBlackground), the 1024 x 768 art of UIOptionsFrame-TopLeft / -TopRight / -BottomLeft /
// -BottomRight as tall as the screen, the ends of its frame at that scale and the parchment between them stretched to
// the screen's width, `title` in its top bar (UIOptionsFrameTitle). It takes every finger (nothing under it is reached;
// c.panel is ignored). The body is the parchment within the safe area, the footer its bottom row; no close button.
Dialog beginFullScreen(DialogContext& c, ui::Gui& g, std::string_view name, std::string_view title, bool footer = true);

// OptionFrameBoxTemplate (the option windows' sections): the tooltip backdrop tinted 0.5, its border 0.4.
void optionBox(ui::Gui& g, const ui::Rect& r);

f32 buttonWidth(const ui::Gui& g, std::string_view text, f32 height);

// Buttons at the ends of the footer (either may be empty). Returns which were tapped.
void footerButtons(ui::Gui& g, const ui::Rect& footer, std::string_view name, std::string_view left,
                   bool leftEnabled, bool& leftTapped, std::string_view right, bool& rightTapped);

// A column of flowing content inside a scroll view: text, headings and items stacked from `y` down.
struct Flow {
    ui::Gui* g;
    f32 x;
    f32 w;
    f32 y;

    f32 text(std::string_view s, const ui::TextStyle& style, f32 gapAfter) {
        if (s.empty()) {
            return 0.0f;
        }
        const f32 h = g->draw.wrappedText({x, y, w, 1e6f}, s, style);
        y += h + gapAfter;
        return h;
    }
    void title(std::string_view s, f32 size, f32 gapAfter) {
        if (!s.empty()) {
            y += ui::parchmentTitle(*g, {x, y, w, 1e6f}, s, size) + gapAfter;
        }
    }
};

// Parchment with a scroll view inside (QuestFrame's scroll frames): content starts at flow.y.
struct ParchmentScroll {
    ui::ScrollView view;
    f32 padding = 0.0f;
};

ParchmentScroll beginParchment(ui::Gui& g, ui::WidgetId id, const ui::Rect& r);

Flow parchmentFlow(ui::Gui& g, const ParchmentScroll& p);

void endParchment(ui::Gui& g, const ParchmentScroll& p, const Flow& flow);

// A tappable line of a list on parchment that wraps its text (gossip options, quest titles). Returns its
// height.
f32 wrappedRow(ui::Gui& g, ui::WidgetId id, f32 x, f32 y, f32 w, f32 minHeight, u64 icon, u32 iconTint,
               std::string_view text, const ui::TextStyle& style, bool& tappedOut);

// A quest item (QuestItemTemplate): the icon, then its name in the dark name frame. The whole cell is one
// target. Phase 13: `linkable` (the quest windows' items): with the chat's edit box open a tap puts the item's
// link in it instead (QuestItem_OnClick / QuestRewardItem_OnClick's Shift + click) and chooses nothing.
// QuestFrameItems_Update's tint of a reward the player cannot use (ItemView::usable false): a choice, its icon and
// name frame 0.9, 0, 0; an item always given, its icon 0.5, 0, 0 and its name frame 1, 0, 0.
enum class UnusableTint : u8 { None, Choice, Reward };

// Retouches (quest windows): `artScale` > 0, the cell is the 1.12 QuestItemTemplate at that scale (logical pixels per
// unit of its frame's art; the cell 147 x 41 units, narrower when two do not fit): the plain 39-unit icon with its
// count, UI-QuestItemNameFrame from 10 units into it and the name in GameFontHighlight (white), as the quest frames and
// the quest log show it.
bool questItem(DialogContext& c, ui::Gui& g, ui::WidgetId id, const ui::Rect& cell, const ItemView& item,
               bool selected, bool selectable, bool linkable = false, UnusableTint tint = UnusableTint::None,
               f32 artScale = 0.0f);

// The item cell (questItem: quest rewards, loot) whose tooltip was asked for this frame (a long press; a tap on an
// item the tap does not take, or on a reward choice), if any, and where: the caller shows its tooltip. Returns it
// once.
bool takeLongPressedItem(u32& entry, ui::Rect& anchor);
// Asks for the tooltip of `entry` beside `anchor` (what takeLongPressedItem returns next).
void requestItemTooltip(u32 entry, const ui::Rect& anchor);

// A grid of quest items, as many columns as fit. Returns the one tapped (-1 none). Retouches (quest windows): with
// `artScale` the 1.12's, two to a row (QuestFrameItems_Update: the first 3 units left of the text over it, the next 1
// right of it, a row 2 under the other).
i32 questItems(DialogContext& c, Flow& flow, std::string_view name, u64 idBase,
               std::span<const ItemView> items, i32 selected, bool selectable,
               UnusableTint tint = UnusableTint::None, f32 artScale = 0.0f);

// Retouches (quest windows): the 1.12 quest frames' and quest log's sizes at their art's scale `k` (logical pixels per
// unit of the art): their font objects (Fonts.xml: GameFontNormal / GameFontHighlight 12 units, GameFontNormalSmall 10;
// the quests' own QuestTitleFont 18, QuestFont 13 and QuestFontNormalSmall 12 a little bigger here, 20, 15 and 14,
// easier to read on a phone; never under 10 dp) and a list's line (16 units in the original), a finger's height at
// least.
struct QuestArtSizes {
    f32 k = 1.0f;
    f32 normal = 0.0f, small = 0.0f;                        // GameFontNormal / GameFontHighlight, GameFontNormalSmall
    f32 questTitle = 0.0f, quest = 0.0f, objective = 0.0f; // QuestTitleFont, QuestFont, QuestFontNormalSmall
    f32 row = 0.0f;
    f32 u(f32 units) const { return units * k; }
};
QuestArtSizes questArtSizes(const ui::Gui& g, f32 k);

// Retouches (quest windows): QuestFrameItems_Update at the art's scale, for the quest frames and the quest log:
// QUEST_REWARDS, the choices under `chooseText` (REWARD_CHOICES; REWARD_CHOOSE when completing) two to a row, the
// spell under `spellText` (REWARD_SPELL...; empty: none), then REWARD_ITEMS ("You will also receive:", after choices
// or a spell) or REWARD_ITEMS_ONLY with the coins 15 units right of it, and the items two to a row. Returns the choice
// tapped (-1 none).
struct QuestArtRewards {
    std::span<const ItemView> choices;
    std::span<const ItemView> rewards;
    std::string_view chooseText;
    std::string_view spellText;
    ItemView spell; // its icon and name
    u32 money = 0;
    i32 selected = -1;       // the chosen reward, highlighted
    bool selectable = false; // a tap chooses
};
i32 questArtRewards(DialogContext& c, Flow& flow, const QuestArtSizes& s, std::string_view name,
                    const QuestArtRewards& r);

} // namespace vd::kit
