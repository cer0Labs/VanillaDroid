#include "app/CharacterWindows.h"

#include "app/CharacterFrameLayout.h"
#include "app/DialogKit.h"
#include "app/FriendsFrameLayout.h" // retouches (character frame): the reputation list
#include "app/SocialWindows.h"
#include "app/UiResources.h"
#include "app/UnitTooltip.h"
#include "app/WindowArt.h" // retouches (windows)
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

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

constexpr u64 kStatBackground =
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-StatBackground.blp");
constexpr u64 kResistanceIcons =
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-ResistanceIcons.blp");
constexpr u64 kSkillsBar = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Skills-Bar.blp");
constexpr u64 kSkillsBarBorder =
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Skills-BarBorder.blp");
constexpr u64 kSkillsBarHighlight =
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Skills-BarBorderHighlight.blp");
constexpr u64 kReputationBar = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-ReputationBar.blp");
constexpr u64 kReputationBarHighlight =
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-ReputationBar-Highlight.blp");
constexpr u64 kReputationDetail =
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Reputation-DetailBackground.blp");
constexpr u64 kReputationWatchBar = assetPathHash("Interface\\PaperDollInfoFrame\\UI-ReputationWatchBar.blp");
constexpr u64 kExpBarFrame = assetPathHash("Interface\\MainMenuBar\\UI-MainMenuBar-Dwarf.blp");
constexpr u64 kStatusBar = assetPathHash("Interface\\TargetingFrame\\UI-StatusBar.blp");
constexpr u64 kCheckUp = assetPathHash("Interface\\Buttons\\UI-CheckBox-Up.blp");
constexpr u64 kCheckDown = assetPathHash("Interface\\Buttons\\UI-CheckBox-Down.blp");
constexpr u64 kCheckMark = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check.blp");
constexpr u64 kCheckDisabled = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check-Disabled.blp");
constexpr u64 kSwordCheck = assetPathHash("Interface\\Buttons\\UI-CheckBox-SwordCheck.blp");
constexpr u64 kCancelUp = assetPathHash("Interface\\Buttons\\CancelButton-Up.blp");
constexpr u64 kCancelDown = assetPathHash("Interface\\Buttons\\CancelButton-Down.blp");
constexpr u64 kCancelHighlight = assetPathHash("Interface\\Buttons\\CancelButton-Highlight.blp");
constexpr u64 kPlusUp = assetPathHash("Interface\\Buttons\\UI-PlusButton-Up.blp");
constexpr u64 kMinusUp = assetPathHash("Interface\\Buttons\\UI-MinusButton-Up.blp");
constexpr u64 kPlusHighlight = assetPathHash("Interface\\Buttons\\UI-PlusButton-Hilight.blp");
constexpr u64 kSortTabLeft = assetPathHash("Interface\\QuestFrame\\UI-QuestLogSortTab-Left.blp");
constexpr u64 kSortTabMiddle = assetPathHash("Interface\\QuestFrame\\UI-QuestLogSortTab-Middle.blp");
constexpr u64 kSortTabRight = assetPathHash("Interface\\QuestFrame\\UI-QuestLogSortTab-Right.blp");
constexpr u64 kTrainerScrollBar = assetPathHash("Interface\\ClassTrainerFrame\\UI-ClassTrainer-ScrollBar.blp");
// Retouches (character frame): streamed from textures.vdpk (not in ui.vdpk's atlas): SkillFrame's divider,
// ReputationDetailFrame's corner and divider.
constexpr u64 kHorizontalBar = assetPathHash("Interface\\ClassTrainerFrame\\UI-ClassTrainer-HorizontalBar.blp");
constexpr u64 kDialogCorner = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Corner.blp");
constexpr u64 kDialogDivider = assetPathHash("Interface\\DialogFrame\\UI-DialogBox-Divider.blp");

constexpr u32 kWhiteText = ui::kWhite;
constexpr u32 kRedText = rgba(255, 26, 26);   // RED_FONT_COLOR
constexpr u32 kGreenText = rgba(26, 255, 26); // GREEN_FONT_COLOR / GameFontGreenSmall
constexpr u32 kGrayText = rgba(128, 128, 128);

// FACTION_BAR_COLORS (ReputationFrame.lua), standing 1..8.
constexpr u32 kFactionBarColors[8] = {rgba(204, 77, 56), rgba(204, 77, 56), rgba(191, 69, 0),
                                      rgba(230, 179, 0), rgba(0, 153, 26),  rgba(0, 153, 26),
                                      rgba(0, 153, 26),  rgba(0, 153, 26)};

// Retouches (character frame): CharacterFrame's art (streamed from textures.vdpk: big): PaperDollFrame's picture, the
// plain frame of the other tabs and of the wide arrangement (FriendsFrame's UI-Character-General-*), and
// CharacterAmmoSlot's.
constexpr u64 kPaperDollArt[4] = {
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-CharacterTab-L1.blp"),
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-CharacterTab-R1.blp"),
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-CharacterTab-BottomLeft.blp"),
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-CharacterTab-BottomRight.blp"),
};
constexpr u64 kPlainArt[4] = {
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-General-TopLeft.blp"),
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-General-TopRight.blp"),
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-General-BottomLeft.blp"),
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-General-BottomRight.blp"),
};
constexpr u64 kAmmoSlotArt = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-AmmoSlot.blp");
// SkillFrame's bottom half (its list's box, the detail's and Close's slot) and HonorFrame's boxes (TOPLEFT (22, -69),
// (275, -69), (22, -325), (275, -325) over the plain frame).
constexpr u64 kSkillArt[2] = {
    assetPathHash("Interface\\PaperDollInfoFrame\\SkillFrame-BotLeft.blp"),
    assetPathHash("Interface\\PaperDollInfoFrame\\SkillFrame-BotRight.blp"),
};
constexpr u64 kHonorArt[4] = {
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Honor-TopLeft.blp"),
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Honor-TopRight.blp"),
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Honor-BottomLeft.blp"),
    assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Honor-BottomRight.blp"),
};
constexpr f32 kHonorBoxes[4][4] = {{22.0f, 69.0f, 256.0f, 256.0f},
                                   {275.0f, 69.0f, 128.0f, 256.0f},
                                   {22.0f, 325.0f, 256.0f, 128.0f},
                                   {275.0f, 325.0f, 128.0f, 128.0f}};
// MagicResFrame1..5's TexCoords in UI-Character-ResistanceIcons (arcane, fire, nature, frost, shadow).
constexpr f32 kResistanceRows[5][2] = {{0.2265625f, 0.33984375f},
                                       {0.0f, 0.11328125f},
                                       {0.11328125f, 0.2265625f},
                                       {0.33984375f, 0.453125f},
                                       {0.453125f, 0.56640625f}};
constexpr u32 kStatCount = 17; // game::PaperDollStat::Count
constexpr u32 kResistanceFirst = 12;

// Tooltip owners of a panel this frame: a tap or a long press on one shows its tooltip (the desktop's hover)
// until the next tap; a tap anywhere else in the panel hides it.
struct Tips {
    static constexpr u32 kMax = 48;
    i32& shown;
    std::array<Rect, kMax> rects{};
    std::array<i32, kMax> ids{};
    u32 count = 0;
    Rect shownRect;

    void owner(Gui& g, WidgetId id, const Rect& r, i32 tip) {
        const ui::WidgetState s = ui::hit(g, id, r);
        if (ui::tapped(s)) {
            shown = shown == tip ? -1 : tip;
        } else if (s.longPress) {
            shown = tip;
        }
        add(r, tip);
    }
    // A rectangle whose widget acts on taps itself (a button): only its long press shows the tooltip (the
    // caller).
    void add(const Rect& r, i32 tip) {
        if (count < kMax) {
            rects[count] = r;
            ids[count++] = tip;
        }
        if (shown == tip) {
            shownRect = r;
        }
    }
    // After the panel: a finger that began on it outside every owner hides the tooltip; the tooltip's
    // rectangle.
    bool update(const ui::Context& ui, const Rect& area) {
        for (const ui::Touch& t : ui.touches()) {
            if (!t.began || !area.contains(t.x, t.y)) {
                continue;
            }
            bool onOwner = false;
            for (u32 i = 0; i < count && !onOwner; ++i) {
                onOwner = rects[i].contains(t.x, t.y);
            }
            if (!onOwner) {
                shown = -1;
            }
        }
        bool found = false;
        for (u32 i = 0; i < count && !found; ++i) {
            found = ids[i] == shown;
        }
        if (!found) {
            shown = -1;
        }
        return shown >= 0;
    }
};

// A GameTooltip beside `owner` (ANCHOR_RIGHT, kept on screen).
void tooltipBeside(DialogContext& c, Gui& g, WidgetId id, const Rect& owner,
                   std::span<const game::TooltipLine> lines) {
    if (lines.empty()) {
        return;
    }
    HudTooltipView view;
    view.lines = lines;
    const f32 maxWidth = std::min(g.px(300.0f), c.safeArea.w * 0.6f);
    const HudTooltipSize size = measureHudTooltip(g, view, maxWidth);
    hudTooltip(g, id, besideTooltipAnchor(g, c.safeArea, owner, true, size), view);
}

// A tooltip of a title (white) and a text (gold, wrapped): the HonorFrame's, ReputationFrame's and
// SkillFrame's labels and buttons.
void simpleTooltip(DialogContext& c, Gui& g, WidgetId id, const Rect& owner, text::Key title,
                   text::Key body) {
    std::array<game::TooltipLine, 2> lines{};
    u32 n = 0;
    if (!title.name.empty()) {
        lines[n].left = text::get(title);
        lines[n++].leftColor = game::kTooltipWhite;
    }
    if (!body.name.empty()) {
        lines[n].left = text::get(body);
        lines[n].leftColor = game::kTooltipGold;
        lines[n++].wrap = true;
    }
    tooltipBeside(c, g, id, owner, {lines.data(), n});
}

// UI-Character-StatBackground's three pieces (top, stretched middle, bottom) over `r`.
void statBackground(Gui& g, const Rect& r, f32 s) {
    const f32 cap = std::min(16.0f * s, r.h * 0.5f);
    constexpr f32 kRight = 0.8984375f;
    g.draw.sprite({r.x, r.y, r.w, cap}, kStatBackground, ui::kWhite, {0.0f, 0.0f, kRight, 0.125f});
    g.draw.sprite({r.x, r.y + cap, r.w, std::max(0.0f, r.h - 2.0f * cap)}, kStatBackground, ui::kWhite,
                  {0.0f, 0.125f, kRight, 0.1953125f});
    g.draw.sprite({r.x, r.bottom() - cap, r.w, cap}, kStatBackground, ui::kWhite,
                  {0.0f, 0.484375f, kRight, 0.609375f});
}

// A StatFrameTemplate line: the label (GameFontNormalSmall) on the left, the value (GameFontHighlightSmall)
// right aligned; the whole line owns the stat's tooltip.
void statLine(Gui& g, Tips& tips, const Rect& r, const PaperDollStatView& stat, i32 index, f32 size) {
    const f32 valueW = std::min(g.draw.measure(stat.value, ui::FontId::Friz, size), r.w * 0.7f);
    TextStyle label = ui::gameFont(size, ui::kGold);
    ui::label(g, {r.x, r.y, std::max(r.w * 0.3f, r.w - valueW - size * 0.4f), r.h}, stat.label, label);
    TextStyle value = ui::gameFont(size, kWhiteText);
    value.align = Align::Right;
    ui::label(g, {r.right() - valueW - size * 0.4f, r.y, valueW + size * 0.4f, r.h}, stat.value, value);
    tips.owner(g, widgetId("character.stat", static_cast<u64>(index)), r, index);
}

// A MagicResistanceFrameTemplate: the school's icon with its value at the bottom.
void resistanceIcon(Gui& g, Tips& tips, const Rect& r, const PaperDollStatView& stat, u32 school, i32 index) {
    g.draw.sprite(r, kResistanceIcons, ui::kWhite,
                  {0.0f, kResistanceRows[school][0], 1.0f, kResistanceRows[school][1]});
    TextStyle value = ui::gameFont(std::max(r.h * 0.42f, g.px(9.0f)), kWhiteText);
    value.align = Align::Center;
    value.outline = std::max(1.0f, g.px(1.0f));
    ui::label(g, {r.x, r.bottom() - r.h * 0.55f, r.w, r.h * 0.5f}, stat.value, value);
    tips.owner(g, widgetId("character.stat", static_cast<u64>(index)), r, index);
}

// The text size at which the label and the value of every line of `stats[first, first + count)` fit `width`
// (at most `size`; smaller when a translation or a big number needs it, never below 70 %).
f32 fittingSize(const Gui& g, std::span<const PaperDollStatView> stats, u32 first, u32 count, f32 width,
                f32 size) {
    f32 fit = size;
    for (u32 i = first; i < first + count && i < stats.size(); ++i) {
        const f32 need = g.draw.measure(stats[i].label, ui::FontId::Friz, size) +
                         g.draw.measure(stats[i].value, ui::FontId::Friz, size) + size * 0.6f;
        if (need > width && need > 0.0f) {
            fit = std::min(fit, size * width / need);
        }
    }
    return std::max(fit, size * 0.7f);
}

} // namespace

// -------------------------------------------------------------------------------------------------------
// Paper doll.

PaperDollResult paperDollWindow(DialogContext& c, const PaperDollView& v, ItemDrag& drag) {
    VD_PROFILE_ZONE("CharacterWindows::paperDoll");
    Gui g{c.ui, c.draw};
    PaperDollResult result;
    ItemWindowResult& items = result.items;
    // Character frame: CharacterFrame with its 1.12 art at the window buttons' scale, in its place's top left
    // (UIPanelWindows: left) or centered over the world; the paper doll (and the pet's page) tall as the original
    // where that shows bigger, wide elsewhere (app/CharacterFrameLayout); the reputation, skills and honor pages
    // always the original's tall frame (as the 1.12's).
    const bool panel = c.panel.w > 0.0f;
    bool outside = false;
    if (!panel) {
        outside = ui::modalBlocker(g, widgetId("character", 1), 0.3f);
    }
    const Rect place = panel ? c.panel : c.safeArea.inset(g.px(6.0f));
    const f32 k0 = characterFrameScale(kit::metrics(c).button);
    const bool dollLike = v.page == CharacterPage::PaperDoll || v.page == CharacterPage::Pet;
    const CharacterFrameLayout l = characterFrameLayout(place, k0, dollLike && characterFrameWide(place, k0),
                                                       !panel || c.panelAlign == PanelAlign::Center);
    const kit::QuestArtSizes s = kit::questArtSizes(g, l.k);
    const f32 k = l.k;
    const bool doll = v.page == CharacterPage::PaperDoll && v.equipment.size() >= kCharacterCells;
    ui::hit(g, widgetId("character", 2), l.frame); // the fingers that miss its controls are its own

    // CharacterFramePortrait under the ring (SetPortraitTexture), then the art: PaperDollFrame's own on its tall page,
    // the plain frame on the others and in the wide arrangement (SkillFrame with its own bottom half, HonorFrame with
    // its boxes over it). A dark plate while the textures stream in.
    g.draw.ring(l.portrait.centerX(), l.portrait.centerY(), l.portrait.w * 0.5f, 0.0f, rgba(0, 0, 0));
    g.draw.portrait(l.portrait, v.portrait, 0);
    const bool dollArt = doll && !l.wide;
    std::array<TextureHandle, 4> art{};
    bool ready = true;
    for (u32 i = 0; i < 4; ++i) {
        u64 id = dollArt ? kPaperDollArt[i] : kPlainArt[i];
        if (v.page == CharacterPage::Skills && i >= 2) {
            id = kSkillArt[i - 2];
        }
        art[i] = c.resources.texture(id);
        ready = ready && art[i].valid();
    }
    const std::span<const CharacterArtPiece> pieces =
        l.wide || dollArt ? std::span<const CharacterArtPiece>(l.pieces.data(), l.pieceCount)
                          : std::span<const CharacterArtPiece>(l.plainPieces.data(), l.plainPieceCount);
    if (ready) {
        for (const CharacterArtPiece& p : pieces) {
            g.draw.texture(p.dst, art[p.texture], ui::kWhite, p.uv);
        }
        if (v.page == CharacterPage::Honor) {
            for (u32 i = 0; i < 4; ++i) {
                if (const TextureHandle box = c.resources.texture(kHonorArt[i]); box.valid()) {
                    g.draw.texture(l.at(kHonorBoxes[i][0], kHonorBoxes[i][1], kHonorBoxes[i][2], kHonorBoxes[i][3]),
                                   box);
                }
            }
        }
    } else {
        g.draw.solid(l.frame.inset(8.0f * k), rgba(12, 12, 12, 230));
    }
    {
        // CharacterNameText (white), CharacterLevelText and CharacterGuildText
        // (GameFontNormalSmall).
        TextStyle name = ui::gameFont(s.normal, kWhiteText);
        name.align = Align::Center;
        ui::label(g, l.name, v.title, name);
        if (doll) { // PaperDollFrame's own (HonorFrame has its HonorLevelText)
            TextStyle line = ui::gameFont(s.small);
            line.align = Align::Center;
            ui::label(g, l.level, v.level, line);
            ui::label(g, l.guild, v.guild, line);
        }
    }
    items.closed = ui::closeButton(g, widgetId("character.close"), l.close) || (outside && !drag.active);
    result.frame = {l, s};

    i32 shown = v.statShown;
    Tips tips{shown};
    const bool stats = v.stats.size() >= kStatCount;
    if (doll) {
        // CharacterModelFrame (InspectModelFrame is taller in the tall frame: no numbers under it), turned by a finger
        // only (unlike 1.12: no rotate buttons).
        if (v.modelState) {
            // The wide arrangement's narrow column cuts the image's empty sides (the model fills its height).
            result.rotated = modelFrame(c, g, widgetId("character.model"), stats ? l.model : l.inspectModel, v.model,
                                        *v.modelState, l.wide ? 0.55f : 1.0f, false);
        }
        // The slots, over their riveted cells in the wide arrangement.
        if (l.wide) {
            if (const TextureHandle cell = c.resources.texture(kPaperDollArt[2]); cell.valid()) {
                for (u32 i = 0; i < kCharacterCells; ++i) {
                    if (l.cells[i].w > 0.0f) {
                        g.draw.texture(l.cells[i], cell, ui::kWhite, kCharacterCellUv);
                    }
                }
            }
        }
        for (u32 i = 0; i < kCharacterCells; ++i) {
            if (i == kCharacterAmmoCell && !v.ammoSlot) {
                continue;
            }
            if (i == kCharacterAmmoCell) {
                // CharacterAmmoSlot's art: its frame and the arrow from the ranged slot (UI-Character-AmmoSlot).
                if (const TextureHandle ammo = c.resources.texture(kAmmoSlotArt); ammo.valid()) {
                    g.draw.texture(l.ammoFrame, ammo, ui::kWhite, {0.0f, 0.0f, 0.640625f, 0.640625f});
                    g.draw.texture(l.ammoArrow, ammo, ui::kWhite, {0.640625f, 0.0f, 1.0f, 0.640625f});
                }
            }
            itemCell(c, g, widgetId("character.slot", i), l.slots[i], v.equipment[i], drag, items);
        }
        if (stats) {
            // CharacterResistanceFrame's five icons and CharacterAttributesFrame's three boxes.
            for (u32 i = 0; i < 5; ++i) {
                resistanceIcon(g, tips, l.resistances[i], v.stats[kResistanceFirst + i], i,
                               static_cast<i32>(kResistanceFirst + i));
            }
            for (const Rect& box : l.statBoxes) {
                statBackground(g, box, k);
            }
            const f32 size = fittingSize(g, v.stats, 0, kCharacterStatLines, 99.0f * k, std::max(s.small, 10.0f * k));
            for (u32 i = 0; i < kCharacterStatLines; ++i) {
                statLine(g, tips, l.statLines[i], v.stats[i], static_cast<i32>(i), size);
            }
        }
        // CharacterModelFrame_OnMouseUp / OnReceiveDrag: an item dropped on the equipment equips it where it goes.
        if (drag.droppedIn(l.frame)) {
            items.dropFrom = drag.from;
            items.dropZone = 0;
            drag.taken = true;
        }
    }
    // CharacterFrameTab1..5 turned along the right edge, from the top in their order (the friends frame's), last:
    // over the border.
    if (v.tabs.size() > 1) {
        const f32 finger = g.px(ui::kTouchDp);
        const i32 tab = art::characterTabColumn(g, "character.tabs", l.tabs, v.tabs, v.tab, s.small,
                                                finger + 14.0f * k, 0u, finger, 4.0f);
        if (tab >= 0 && static_cast<u32>(tab) != v.tab) {
            items.tab = tab;
        }
    }
    if (doll && stats) {
        const i32 before = shown;
        tips.update(c.ui, l.frame);
        result.statTapped = shown;
        result.tappedElsewhere = before >= 0 && shown < 0;
        if (shown >= 0 && static_cast<u32>(shown) < v.stats.size()) {
            tooltipBeside(c, g, widgetId("character.statTip"), tips.shownRect, v.stats[static_cast<u32>(shown)].tooltip);
        }
    }
    return result;
}

// -------------------------------------------------------------------------------------------------------
// Honor.

void honorPanel(DialogContext& c, const CharacterPageFrame& f, const HonorView& v, i32& shownTip) {
    VD_PROFILE_ZONE("CharacterWindows::honor");
    Gui g{c.ui, c.draw};
    const CharacterFrameLayout& l = f.l;
    const kit::QuestArtSizes& s = f.s;
    const f32 k = l.k;
    Tips tips{shownTip};
    enum Tip : i32 { HK, DK, Contribution, Standing, HighestRank, Rank, Position };
    {
        // HonorLevelText and HonorGuildText where PaperDollFrame has its own.
        TextStyle line = ui::gameFont(s.small);
        line.align = Align::Center;
        ui::label(g, l.level, v.level, line);
        ui::label(g, l.guild, v.guild, line);
    }
    // HonorFrameProgressBar (315 x 29 at (22, -77)) in the faction's colour: UI-Character-Skills-Bar.
    const Rect bar = l.at(22.0f, 77.0f, 315.0f, 29.0f);
    const u32 barColor = v.alliance ? rgba(13, 38, 92) : rgba(161, 23, 23);
    const f32 fill = std::clamp(v.progress, 0.0f, 1.0f);
    g.draw.sprite({bar.x, bar.y, bar.w * fill, bar.h}, kSkillsBar, barColor, {0.0f, 0.0f, fill, 1.0f});
    tips.owner(g, widgetId("honor.progress"), bar, Position);
    {
        // HonorFrameCurrentPVPTitle (GameFontNormal) at TOP (0, -87), HonorFrameCurrentPVPRank (GameFontNormalSmall)
        // 5 right of it, HonorFramePvPIcon (24 x 24) 5 left of it; HonorFrameRankButton over the three.
        const TextStyle title = ui::gameFont(s.normal);
        const TextStyle number = ui::gameFont(s.small);
        const f32 titleW = g.draw.measure(v.rankTitle, title.font, title.size);
        const f32 numberW = g.draw.measure(v.rankNumber, number.font, number.size);
        const f32 cx = l.art.x + 192.0f * k;
        const f32 top = l.art.y + 87.0f * k;
        const f32 lineH = std::max(12.0f * k, s.normal);
        const Rect titleRect{cx - titleW * 0.5f, top, titleW + 1.0f, lineH};
        ui::label(g, titleRect, v.rankTitle, title);
        ui::label(g, {titleRect.right() + 5.0f * k, top + k, numberW + 1.0f, lineH}, v.rankNumber, number);
        const Rect icon{titleRect.x - 5.0f * k - 24.0f * k, titleRect.centerY() - 12.0f * k, 24.0f * k, 24.0f * k};
        if (v.rankIcon) {
            g.draw.sprite(icon, v.rankIcon);
        }
        tips.owner(g, widgetId("honor.rank"), {icon.x, icon.y, titleRect.right() + 5.0f * k + numberW - icon.x, icon.h},
                   Rank);
    }
    // The sections' titles (GameFontNormal) from (36, -112), each under the one before with its lines between; the
    // lines (HonorFrameTextButtonTemplate, 278 x 12) 10 in: the label in GameFontHighlightSmall, the value right
    // aligned at 278 (honorable kills green, dishonorable red, the rest gold).
    struct Line {
        text::Key label = "";
        u32 value = 0;
        u32 color = 0;
        i32 tip = 0;
        std::string_view text;
        f32 gap = 0.0f; // units over it: 3 under the title, 2..4 between lines
    };
    struct Section {
        text::Key title;
        f32 gap; // units between the previous section's title and this one's (HonorFrame.xml: 41, 43, 42, 64)
        std::array<Line, 3> lines;
        u32 count;
    };
    const Section sections[5] = {
        {"HONOR_THIS_SESSION",
         0.0f,
         {{{"HONORABLE_KILLS", v.todayHonorable, kGreenText, HK, {}, 3.0f},
           {"DISHONORABLE_KILLS", v.todayDishonorable, kRedText, DK, {}, 2.0f},
           {}}},
         2},
        {"HONOR_YESTERDAY",
         41.0f,
         {{{"HONORABLE_KILLS", v.yesterdayHonorable, kGreenText, HK, {}, 3.0f},
           {"HONOR_CONTRIBUTION_POINTS", v.yesterdayHonor, ui::kGold, Contribution, {}, 3.0f},
           {}}},
         2},
        {"HONOR_THISWEEK",
         43.0f,
         {{{"HONORABLE_KILLS", v.thisWeekHonorable, kGreenText, HK, {}, 3.0f},
           {"HONOR_CONTRIBUTION_POINTS", v.thisWeekHonor, ui::kGold, Contribution, {}, 3.0f},
           {}}},
         2},
        {"HONOR_LASTWEEK",
         42.0f,
         {{{"HONORABLE_KILLS", v.lastWeekHonorable, kGreenText, HK, {}, 3.0f},
           {"HONOR_CONTRIBUTION_POINTS", v.lastWeekHonor, ui::kGold, Contribution, {}, 2.0f},
           {"HONOR_STANDING", v.lastWeekStanding, ui::kGold, Standing, {}, 2.0f}}},
         3},
        {"HONOR_LIFETIME",
         64.0f,
         {{{"HONORABLE_KILLS", v.lifetimeHonorable, kGreenText, HK, {}, 4.0f},
           {"DISHONORABLE_KILLS", v.lifetimeDishonorable, kRedText, DK, {}, 4.0f},
           {"HONOR_HIGHEST_RANK", 0, ui::kGold, HighestRank, v.highestRank, 4.0f}}},
         3},
    };
    f32 titleY = 112.0f;
    u32 index = 0;
    for (const Section& section : sections) {
        titleY += section.gap + (index > 0 ? 12.0f : 0.0f);
        ui::label(g, l.at(36.0f, titleY, 278.0f, 12.0f), text::get(section.title), ui::gameFont(s.normal));
        f32 lineY = titleY + 12.0f;
        for (u32 i = 0; i < section.count; ++i) {
            const Line& line = section.lines[i];
            lineY += line.gap;
            const Rect r = l.at(46.0f, lineY, 278.0f, 12.0f);
            ui::label(g, r, text::get(line.label), ui::gameFont(s.small, kWhiteText));
            char value[24];
            std::string_view shown = line.text;
            if (shown.empty() && !v.waiting) {
                std::snprintf(value, sizeof(value), "%u", line.value);
                shown = value;
            }
            TextStyle style = ui::gameFont(s.small, line.color);
            style.align = Align::Right;
            ui::label(g, r, shown, style);
            tips.owner(g, widgetId("honor.line", index * 4 + i), r, line.tip);
            lineY += 12.0f;
        }
        ++index;
    }
    if (tips.update(c.ui, l.frame)) {
        static constexpr text::Key kTitles[7] = {"HONORABLE_KILLS",
                                                 "DISHONORABLE_KILLS",
                                                 "HONOR_CONTRIBUTION_POINTS",
                                                 "HONOR_STANDING",
                                                 "RANK",
                                                 "RANK",
                                                 "RANK_POSITION"};
        static constexpr text::Key kBodies[7] = {"NEWBIE_TOOLTIP_HONORABLE_KILLS",
                                                 "NEWBIE_TOOLTIP_DISHONORABLE_KILLS",
                                                 "NEWBIE_TOOLTIP_HONOR_CONTRIBUTION_POINTS",
                                                 "NEWBIE_TOOLTIP_HONOR_STANDING",
                                                 "NEWBIE_TOOLTIP_RANK",
                                                 "NEWBIE_TOOLTIP_RANK",
                                                 "NEWBIE_TOOLTIP_RANK_POSITION"};
        const u32 tip = static_cast<u32>(std::min(shownTip, 6));
        simpleTooltip(c, g, widgetId("honor.tip"), tips.shownRect, kTitles[tip], kBodies[tip]);
    }
}

// -------------------------------------------------------------------------------------------------------
// Reputation.

namespace {

// An OptionsCheckButtonTemplate (26 x 26 at `k` pixels a unit) with its label right of it (2 units in): UI-CheckBox
// art, the at-war one's check UI-CheckBox-SwordCheck (32 x 32 at (3, -5)); disabled, the disabled check. The box and
// its label are one target, a finger's height at least. Returns true when tapped and enabled.
bool artCheck(Gui& g, WidgetId id, const Rect& box, f32 k, std::string_view label, bool checked, bool enabled,
              bool sword, u32 color, f32 size) {
    const f32 labelW = g.draw.measure(label, ui::FontId::Friz, size);
    const f32 finger = g.px(ui::kTouchDp);
    const Rect target{box.x, std::min(box.y, box.centerY() - finger * 0.5f), box.w + labelW,
                      std::max(box.h, finger)};
    const ui::WidgetState state = enabled ? ui::hit(g, id, target) : ui::WidgetState{};
    g.draw.sprite(box, ui::held(state, target) ? kCheckDown : kCheckUp);
    if (checked) {
        if (!enabled) {
            g.draw.sprite(box, kCheckDisabled);
        } else if (sword) {
            g.draw.sprite({box.x + 3.0f * k, box.y + 5.0f * k, 32.0f * k, 32.0f * k}, kSwordCheck);
        } else {
            g.draw.sprite(box, kCheckMark);
        }
    }
    ui::label(g, {box.right() - 2.0f * k, box.y, labelW + 1.0f, box.h}, label,
              ui::gameFont(size, enabled ? color : kGrayText));
    return enabled && ui::tapped(state);
}

} // namespace

ReputationPanelResult reputationPanel(DialogContext& c, const CharacterPageFrame& f,
                                      std::span<const FactionRowView> rows, const FactionDetailView& detail,
                                      i32& shownTip) {
    VD_PROFILE_ZONE("CharacterWindows::reputation");
    Gui g{c.ui, c.draw};
    const CharacterFrameLayout& l = f.l;
    const kit::QuestArtSizes& s = f.s;
    const f32 k = l.k;
    ReputationPanelResult result;
    Tips tips{shownTip};
    enum Tip : i32 { FactionLabel, StandingLabel, AtWarIndicator, AtWarBox };
    {
        // ReputationFrameFactionLabel (70, -57) and ReputationFrameStandingLabel (215, -59): GameFontHighlight, their
        // newbie tooltips (FactionMouseOver, StandingMouseOver).
        const TextStyle style = ui::gameFont(s.normal, kWhiteText);
        const std::string_view faction = text::get("FACTION"), standing = text::get("STANDING");
        const Rect factionLabel = l.at(70.0f, 57.0f, g.draw.measure(faction, style.font, style.size) / k + 1.0f, 12.0f);
        const Rect standingLabel =
            l.at(215.0f, 59.0f, g.draw.measure(standing, style.font, style.size) / k + 1.0f, 12.0f);
        ui::label(g, factionLabel, faction, style);
        ui::label(g, standingLabel, standing, style);
        tips.owner(g, widgetId("reputation.factionLabel"), factionLabel, FactionLabel);
        tips.owner(g, widgetId("reputation.standingLabel"), standingLabel, StandingLabel);
    }
    // ReputationListScrollFrame (296 x 354 at TOPRIGHT (-66, -76)): the rows 23 units apart (a finger's height at
    // least) from ReputationBar1's art at (24, -82); the scroll frame's track and bar only when they overflow.
    FriendsListLayout list;
    list.view = l.at(22.0f, 76.0f, 296.0f, 354.0f);
    list.rowX = l.art.x + 24.0f * k;
    list.rowW = 272.0f * k;
    list.rowTop = l.art.y + 82.0f * k;
    list.bar = l.at(22.0f + 296.0f + 6.0f, 76.0f, 16.0f, 354.0f);
    list.track = {l.at(316.0f, 71.0f, 31.0f, 256.0f), l.at(316.0f, 76.0f + 354.0f + 4.0f - 108.0f, 31.0f, 108.0f)};
    const f32 step = std::max(23.0f * k, s.row);
    const FriendsListScroll scroll =
        beginFriendsList(c, widgetId("reputation.rows"), list, static_cast<u32>(rows.size()), step);
    f32 y = scroll.top;
    for (usize i = 0; i < rows.size(); ++i, y += step) {
        const FactionRowView& row = rows[i];
        // The row's art at its top; its target the whole step (inside the list).
        const Rect rowRect{list.rowX, y, list.rowW, step};
        if (rowRect.bottom() < scroll.view.viewport.y || rowRect.y > scroll.view.viewport.bottom()) {
            continue;
        }
        const f32 artY = y + (step - 22.0f * k) * 0.5f; // the bar's art centred in the step
        const Rect bar{l.art.x + 150.0f * k, artY + 4.0f * k, 137.0f * k, 13.0f * k};
        const Rect target{rowRect.x, std::max(rowRect.y, scroll.view.viewport.y), rowRect.w,
                          std::min(rowRect.bottom(), scroll.view.viewport.bottom()) -
                              std::max(rowRect.y, scroll.view.viewport.y)};
        if (row.header) {
            // ReputationHeaderTemplate (302 x 13 at the bar's (-125, 0)): UI-MinusButton / UI-PlusButton (16 x 16 at
            // 3) and the name in GameFontNormal at 20; a tap collapses or expands it.
            const ui::WidgetState state = ui::hit(g, widgetId("reputation.header", i), target);
            const Rect header{l.art.x + 25.0f * k, bar.y, 302.0f * k, 13.0f * k};
            const Rect b{header.x + 3.0f * k, header.centerY() - 8.0f * k, 16.0f * k, 16.0f * k};
            g.draw.sprite(b, row.collapsed ? kPlusUp : kMinusUp);
            if (ui::held(state, target)) {
                g.draw.spriteAdditive(b, kPlusHighlight);
            }
            ui::label(g, {header.x + 20.0f * k, header.y, 275.0f * k, header.h}, row.name,
                      ui::gameFont(s.normal, ui::held(state, target) ? kWhiteText : ui::kGold));
            if (ui::tapped(state)) {
                result.headerTapped = static_cast<i32>(i);
            }
            continue;
        }
        // ReputationBarTemplate: ReputationBarLeft (256 x 22, 126 left of the bar and 4 above it) and its right cap
        // (16 x 24), the bar in its standing's colour with the standing over it (the numbers while held: its
        // OnEnter), the name (GameFontHighlightSmall) 119 left of the bar, the watched check after it, the at-war
        // swords right of the cap; the selected one's highlight (ADD).
        const ui::WidgetState state = ui::hit(g, widgetId("reputation.row", i), target);
        const bool held = ui::held(state, target);
        const Rect left{bar.x - 126.0f * k, bar.y - 4.0f * k, 256.0f * k, 22.0f * k};
        g.draw.sprite(left, kReputationBar, ui::kWhite, {0.0f, 0.0f, 1.0f, 0.34375f});
        g.draw.sprite({left.right(), left.y, 16.0f * k, 24.0f * k}, kReputationBar, ui::kWhite,
                      {0.0f, 0.34375f, 0.0625f, 0.71875f});
        const f32 fillShare =
            row.max > 0 ? std::clamp(static_cast<f32>(row.value) / static_cast<f32>(row.max), 0.0f, 1.0f) : 0.0f;
        g.draw.sprite({bar.x, bar.y, bar.w * fillShare, bar.h}, kSkillsBar,
                      kFactionBarColors[std::clamp<u32>(row.standingId, 1, 8) - 1], {0.0f, 0.0f, fillShare, 1.0f});
        if (row.selected || held) {
            const Rect h1{left.x - 2.0f * k, left.y - 3.0f * k, 256.0f * k, 28.0f * k};
            g.draw.spriteAdditive(h1, kReputationBarHighlight, ui::kWhite, {0.0f, 0.0f, 1.0f, 0.4375f});
            g.draw.spriteAdditive({h1.right(), h1.y, 17.0f * k, 28.0f * k}, kReputationBarHighlight, ui::kWhite,
                                  {0.0f, 0.4375f, 0.06640625f, 0.875f});
        }
        const TextStyle nameStyle = ui::gameFont(s.small, kWhiteText);
        const f32 nameX = bar.x - 119.0f * k;
        const f32 nameRoom = (row.watched ? 100.0f : 110.0f) * k; // ReputationFrame_Update's widths
        ui::label(g, {nameX, bar.y, nameRoom, bar.h}, row.name, nameStyle);
        if (row.watched) { // ReputationBarNCheck right of the name's text
            const f32 nameEnd = nameX + std::min(g.draw.measure(row.name, nameStyle.font, nameStyle.size), nameRoom);
            g.draw.sprite({nameEnd, bar.centerY() - 8.0f * k, 16.0f * k, 16.0f * k}, kCheckMark);
        }
        char numbers[32];
        std::string_view barText = row.standing;
        if (held) {
            std::snprintf(numbers, sizeof(numbers), " %d / %d", row.value, row.max);
            barText = numbers;
        }
        TextStyle standing = ui::gameFont(s.small, kWhiteText);
        standing.align = Align::Center;
        ui::label(g, bar, barText, standing);
        if (row.atWar) { // ReputationBarNAtWarCheck: 24 x 22, 2 into the cap's right, its own tooltip
            const Rect w{left.right() + 14.0f * k, left.y + 1.0f * k, 24.0f * k, 22.0f * k};
            g.draw.sprite(w, kReputationBar, ui::kWhite, {0.0625f, 0.34375f, 0.15625f, 0.71875f});
            tips.owner(g, widgetId("reputation.war", i), w, AtWarIndicator);
        }
        if (ui::tapped(state)) {
            result.rowTapped = static_cast<i32>(i);
        }
    }
    endFriendsList(c, scroll, list, l.tabs.x); // the bar's targets clear of the turned tabs (drawn before)

    // ReputationDetailFrame (212 x 203) at the frame's right: the dialog backdrop, UI-Character-Reputation-
    // DetailBackground (11, -11), the corner, the divider (9, -131), the name (20, -21, GameFontNormal), its
    // description (170 wide) under it, the close button, At War (14 over the bottom), Move to Inactive after its label
    // and Show as Experience Bar under it.
    if (detail.shown) {
        const Rect d = characterSideFrame(l, c.safeArea, 28.0f, 212.0f * k, 203.0f * k);
        const auto at = [&](f32 ux, f32 uy, f32 uw, f32 uh) {
            return Rect{d.x + ux * k, d.y + uy * k, uw * k, uh * k};
        };
        ui::hit(g, widgetId("reputation.detailFrame"), d);
        ui::dialogPanel(g, d);
        g.draw.sprite(at(11.0f, 11.0f, 256.0f, 128.0f), kReputationDetail); // its right part is clear
        if (const TextureHandle t = c.resources.texture(kDialogCorner); t.valid()) {
            g.draw.texture(at(174.0f, 7.0f, 32.0f, 32.0f), t);
        }
        if (const TextureHandle t = c.resources.texture(kDialogDivider); t.valid()) {
            g.draw.texture(at(9.0f, 131.0f, 256.0f, 32.0f), t);
        }
        ui::label(g, at(20.0f, 21.0f, 150.0f, 12.0f), detail.name, ui::gameFont(s.normal));
        const Rect descArea = at(20.0f, 35.0f, 170.0f, 131.0f - 35.0f);
        const ui::ScrollView descView = ui::beginScroll(g, widgetId("reputation.detailText"), descArea);
        const f32 h = g.draw.wrappedText({descArea.x, descView.top, descArea.w, 1e6f}, detail.description,
                                         ui::gameFont(s.small, kWhiteText));
        ui::endScroll(g, descView, h + s.small * 0.5f, false);
        if (ui::closeButton(g, widgetId("reputation.detailClose"), at(212.0f - 3.0f - 32.0f, 3.0f, 32.0f, 32.0f))) {
            result.closeDetail = true;
        }
        const Rect atWarBox = at(14.0f, 203.0f - 34.0f - 26.0f, 26.0f, 26.0f);
        const std::string_view atWar = text::get("AT_WAR");
        if (artCheck(g, widgetId("reputation.atWar"), atWarBox, k, atWar, detail.atWar, detail.canToggleAtWar, true,
                     kRedText, s.small)) {
            result.toggleAtWar = true;
        }
        tips.add(atWarBox, AtWarBox); // its long press: REPUTATION_AT_WAR_DESCRIPTION
        const f32 afterAtWar =
            atWarBox.right() - 2.0f * k + g.draw.measure(atWar, ui::FontId::Friz, s.small) + 3.0f * k;
        if (artCheck(g, widgetId("reputation.inactive"), {afterAtWar, atWarBox.y, 26.0f * k, 26.0f * k}, k,
                     text::get("MOVE_TO_INACTIVE"), detail.inactive, true, false, ui::kGold, s.small)) {
            result.toggleInactive = true;
        }
        const Rect watchBox{atWarBox.x, atWarBox.bottom() - 3.0f * k, 26.0f * k, 26.0f * k};
        if (artCheck(g, widgetId("reputation.watch"), watchBox, k, text::get("SHOW_FACTION_ON_MAINSCREEN"),
                     detail.watched, true, false, ui::kGold, s.small)) {
            result.toggleWatched = true;
        }
    }
    if (tips.update(c.ui, l.frame)) {
        static constexpr text::Key kTitles[4] = {"FACTION", "STANDING", "", ""};
        static constexpr text::Key kBodies[4] = {"REPUTATION_FACTION_DESCRIPTION", "REPUTATION_STANDING_DESCRIPTION",
                                                 "REPUTATION_STATUS_AT_WAR", "REPUTATION_AT_WAR_DESCRIPTION"};
        const u32 tip = static_cast<u32>(std::min(shownTip, 3));
        simpleTooltip(c, g, widgetId("reputation.tip"), tips.shownRect, kTitles[tip], kBodies[tip]);
    }
    return result;
}

// -------------------------------------------------------------------------------------------------------
// Skills.

namespace {

// SkillStatusBarTemplate over `r` (271 x 15 units at `k`): the background (white at 0.2; a proficiency's), the bar
// (UI-Character-Skills-Bar: blue for skills, a full grey one for proficiencies), its border (281 x 32, 5 left of it;
// lit: BarBorderHighlight), the name (GameFontNormalSmall, 6 in) and the rank (GameFontHighlightSmall) after it.
void skillBar(Gui& g, const Rect& r, f32 k, std::string_view name, std::string_view rank, f32 fill, bool proficiency,
              bool lit, f32 size) {
    g.draw.solid(r, proficiency ? rgba(255, 255, 255, 128) : rgba(0, 0, 191, 128));
    const f32 f = proficiency ? 1.0f : std::clamp(fill, 0.0f, 1.0f);
    g.draw.sprite({r.x, r.y, r.w * f, r.h}, kSkillsBar, proficiency ? rgba(128, 128, 128) : rgba(0, 0, 255, 128),
                  {0.0f, 0.0f, f, 1.0f});
    const Rect border{r.x - 5.0f * k, r.centerY() - 16.0f * k, r.w + 10.0f * k, 32.0f * k};
    g.draw.sprite(border, kSkillsBarBorder);
    if (lit) {
        g.draw.spriteAdditive(border, kSkillsBarHighlight);
    }
    const TextStyle nameStyle = ui::gameFont(size, ui::kGold);
    const f32 nameW = std::min(g.draw.measure(name, nameStyle.font, nameStyle.size) + 1.0f, r.w * 0.7f);
    ui::label(g, {r.x + 6.0f * k, r.y + k, nameW, r.h}, name, nameStyle);
    if (!rank.empty()) {
        ui::label(g, {r.x + 6.0f * k + nameW + 8.0f * k, r.y + k, r.w - nameW - 16.0f * k, r.h}, rank,
                  ui::gameFont(size, kWhiteText));
    }
}

} // namespace

SkillPanelResult skillPanel(DialogContext& c, const CharacterPageFrame& f, std::span<const SkillLineView> rows,
                            bool allExpanded, const SkillDetailView& detail, i32& shownTip) {
    VD_PROFILE_ZONE("CharacterWindows::skills");
    Gui g{c.ui, c.draw};
    const CharacterFrameLayout& l = f.l;
    const kit::QuestArtSizes& s = f.s;
    const f32 k = l.k;
    SkillPanelResult result;
    Tips tips{shownTip};
    const f32 finger = g.px(ui::kTouchDp);
    {
        // SkillFrameExpandButtonFrame (54 x 32 at (70, -49)): the quest log's sort tab (UI-QuestLogSortTab-Left and
        // -Right 8 x 32, 6 up, the middle between), SkillFrameCollapseAllButton in it: the +/- (16 x 16 at 3) and
        // ALL (GameFontHighlight) at 25.
        const std::string_view all = text::get("ALL");
        const TextStyle style = ui::gameFont(s.normal, kWhiteText);
        const f32 w = std::max(54.0f * k, 35.0f * k + g.draw.measure(all, style.font, style.size));
        const Rect tab{l.art.x + 70.0f * k, l.art.y + 43.0f * k, w, 32.0f * k};
        g.draw.sprite({tab.x, tab.y, 8.0f * k, tab.h}, kSortTabLeft);
        g.draw.sprite({tab.x + 8.0f * k, tab.y, tab.w - 16.0f * k, tab.h}, kSortTabMiddle);
        g.draw.sprite({tab.right() - 8.0f * k, tab.y, 8.0f * k, tab.h}, kSortTabRight);
        const Rect button{tab.x + 5.0f * k, tab.y + 9.0f * k, tab.w - 8.0f * k, 22.0f * k};
        const Rect target{button.x, std::min(button.y, button.centerY() - finger * 0.5f), button.w,
                          std::max(button.h, finger)};
        const ui::WidgetState state = ui::hit(g, widgetId("skills.all"), target);
        const Rect b{button.x + 3.0f * k, button.centerY() - 8.0f * k, 16.0f * k, 16.0f * k};
        g.draw.sprite(b, allExpanded ? kMinusUp : kPlusUp);
        if (ui::held(state, target)) {
            g.draw.spriteAdditive(b, kPlusHighlight);
        }
        ui::label(g, {button.x + 25.0f * k, button.y, button.w - 25.0f * k, button.h}, all, style);
        result.allTapped = ui::tapped(state);
    }
    // SkillListScrollFrame (296 x 220 at TOPRIGHT (-67, -75)): SkillTypeLabelN (the headers: +/- at 3, the name in
    // GameFontHighlight at 25) and SkillRankFrameN (271 x 15 at (38, -79)), 18 units apart (a finger's height at
    // least); its track (UI-ClassTrainer-ScrollBar) and bar when they overflow.
    const Rect listView = l.at(21.0f, 75.0f, 296.0f, 220.0f);
    const f32 step = std::max(18.0f * k, s.row);
    const f32 content = 4.0f * k + static_cast<f32>(rows.size()) * step;
    const bool overflow = content > listView.h + 0.5f;
    if (overflow) {
        g.draw.sprite(l.at(314.0f, 70.0f, 30.0f, 128.0f), kTrainerScrollBar, ui::kWhite, {0.0f, 0.0f, 0.46875f, 1.0f});
        g.draw.sprite(l.at(314.0f, 75.0f + 220.0f + 2.0f - 128.0f, 30.0f, 128.0f), kTrainerScrollBar, ui::kWhite,
                      {0.53125f, 0.0f, 1.0f, 1.0f});
    }
    const ui::ScrollView view = ui::beginScroll(g, widgetId("skills.rows"), listView);
    f32 y = view.top + 4.0f * k;
    for (usize i = 0; i < rows.size(); ++i, y += step) {
        const SkillLineView& row = rows[i];
        const Rect rowRect{l.art.x + 22.0f * k, y, 285.0f * k, step};
        if (rowRect.bottom() < view.viewport.y || rowRect.y > view.viewport.bottom()) {
            continue;
        }
        const f32 top = std::max(rowRect.y, view.viewport.y);
        const Rect target{rowRect.x, top, rowRect.w, std::min(rowRect.bottom(), view.viewport.bottom()) - top};
        const f32 cy = y + step * 0.5f;
        if (row.header) {
            const ui::WidgetState state = ui::hit(g, widgetId("skills.header", i), target);
            const Rect b{rowRect.x + 3.0f * k, cy - 8.0f * k, 16.0f * k, 16.0f * k};
            g.draw.sprite(b, row.expanded ? kMinusUp : kPlusUp);
            if (ui::held(state, target)) {
                g.draw.spriteAdditive(b, kPlusHighlight);
            }
            ui::label(g, {rowRect.x + 25.0f * k, cy - 7.0f * k, 260.0f * k, 14.0f * k}, row.name,
                      ui::gameFont(s.normal, kWhiteText));
            if (ui::tapped(state)) {
                result.headerTapped = static_cast<i32>(i);
            }
            continue;
        }
        const ui::WidgetState state = ui::hit(g, widgetId("skills.row", i), target);
        const Rect bar{l.art.x + 38.0f * k, cy - 7.5f * k, 271.0f * k, 15.0f * k};
        skillBar(g, bar, k, row.name, row.rank, row.fill, row.proficiency, row.selected || ui::held(state, target),
                 s.small);
        if (ui::tapped(state)) {
            result.rowTapped = static_cast<i32>(i);
        }
    }
    ui::endScroll(g, view, content, false);
    if (overflow) {
        ui::panelScrollBar(g, view, l.at(21.0f + 296.0f + 6.0f, 75.0f, 16.0f, 220.0f), l.tabs.x); // clear of the tabs
    }
    // SkillFrameHorizontalBarLeft (256 x 16 at (15, -290)) and its end (75 x 16).
    if (const TextureHandle t = c.resources.texture(kHorizontalBar); t.valid()) {
        g.draw.texture(l.at(15.0f, 290.0f, 256.0f, 16.0f), t, ui::kWhite, {0.0f, 0.0f, 1.0f, 0.25f});
        g.draw.texture(l.at(271.0f, 290.0f, 75.0f, 16.0f), t, ui::kWhite, {0.0f, 0.25f, 0.29296875f, 0.5f});
    }

    // SkillDetailScrollFrame (296 x 107, 8 under the list): SkillDetailStatusBar (211 x 15 centred 20 under its top,
    // 10 left of its middle) with the Unlearn button right of its border, SkillDetailDescriptionText (275 wide, 35
    // down).
    if (detail.shown) {
        const Rect bar = l.at(171.0f - 105.5f, 323.0f - 7.5f, 211.0f, 15.0f);
        skillBar(g, bar, k, detail.name, detail.rank, detail.fill, detail.proficiency, false, s.small);
        if (detail.abandonable) { // SkillDetailStatusBarUnlearnButton (CancelButton art, 32 x 32)
            const Rect b = l.at(278.5f, 323.0f - 15.0f, 32.0f, 32.0f);
            const Rect target{b.centerX() - std::max(b.w, finger) * 0.5f, b.centerY() - std::max(b.h, finger) * 0.5f,
                              std::max(b.w, finger), std::max(b.h, finger)};
            const ui::WidgetState state = ui::hit(g, widgetId("skills.unlearn"), target);
            g.draw.sprite(b, ui::held(state, target) ? kCancelDown : kCancelUp);
            if (ui::held(state, target)) {
                g.draw.spriteAdditive(b, kCancelHighlight);
            }
            result.unlearn = ui::tapped(state);
            if (state.longPress) {
                shownTip = 0; // its OnEnter: UNLEARN_SKILL_TOOLTIP
            }
            tips.add(b, 0);
        }
        const Rect textArea = l.at(38.5f, 338.0f, 275.0f, 410.0f - 338.0f);
        const ui::ScrollView scroll = ui::beginScroll(g, widgetId("skills.detail"), textArea);
        const f32 h = g.draw.wrappedText({textArea.x, scroll.top, textArea.w, 1e6f}, detail.description,
                                         ui::gameFont(s.small, kWhiteText));
        ui::endScroll(g, scroll, h + s.small * 0.5f, false);
    }
    // SkillFrameCancelButton (CLOSE, 80 x 22 centred at (305, -422)).
    result.close = ui::button(g, widgetId("skills.close"), l.at(305.0f - 40.0f, 422.0f - 11.0f, 80.0f, 22.0f),
                              text::get("CLOSE"));
    if (tips.update(c.ui, l.frame)) {
        simpleTooltip(c, g, widgetId("skills.tip"), tips.shownRect, "UNLEARN_SKILL_TOOLTIP", "");
    }
    return result;
}

// -------------------------------------------------------------------------------------------------------
// ReputationWatchBar.

bool reputationWatchBar(Gui& g, const Rect& xpBar, const ReputationWatchView& v) {
    VD_PROFILE_ZONE("CharacterWindows::watchBar");
    if (!v.shown) {
        return false;
    }
    const f32 unitH = xpBar.h / 13.0f; // the experience bar is 13 units tall
    const u32 color = kFactionBarColors[std::clamp<u32>(v.standingId, 1, 8) - 1];
    Rect bar;
    if (v.maxLevel) {
        bar = xpBar; // replaces MainMenuExpBar, framed by its ReputationXPBarTexture0-3
    } else {
        bar = {xpBar.x, xpBar.y - 8.0f * unitH, xpBar.w,
               8.0f * unitH}; // 8 units on top of it (BOTTOM to TOP, -3)
    }
    const bool tapped = ui::tapped(ui::hit(g, widgetId("reputationWatchBar"), bar));
    g.draw.solid(bar, rgba(0, 0, 0, 128)); // ReputationWatchStatusBarBackground
    const f32 fill = std::clamp(v.fill, 0.0f, 1.0f);
    g.draw.sprite({bar.x, bar.y, bar.w * fill, bar.h}, kStatusBar, color, {0.0f, 0.0f, fill, 1.0f});
    if (v.maxLevel) {
        const f32 frameH = bar.h * 10.0f / 13.0f;
        const f32 frameY = bar.bottom() - bar.h * 3.0f / 13.0f - frameH;
        constexpr f32 kRows[4] = {0.79296875f, 0.54296875f, 0.29296875f, 0.04296875f};
        for (u32 i = 0; i < 4; ++i) {
            g.draw.sprite({bar.x + bar.w * 0.25f * static_cast<f32>(i), frameY, bar.w * 0.25f, frameH},
                          kExpBarFrame, ui::kWhite, {0.0f, kRows[i], 1.0f, kRows[i] + 0.0390625f});
        }
    } else {
        // ReputationWatchBarTexture0-3: 11 units tall, 2 above the bar's top.
        constexpr f32 kRows[4] = {0.0f, 0.171875f, 0.34375f, 0.515625f};
        for (u32 i = 0; i < 4; ++i) {
            g.draw.sprite({bar.x + bar.w * 0.25f * static_cast<f32>(i), bar.y - 2.0f * unitH, bar.w * 0.25f,
                           11.0f * unitH},
                          kReputationWatchBar, ui::kWhite, {0.0f, kRows[i], 1.0f, kRows[i] + 0.171875f});
        }
    }
    if (v.showText && !v.text.empty()) { // ReputationWatchStatusBarText (NumberFontNormal)
        TextStyle style = ui::numberFont(std::max(13.0f * unitH * 1.05f, g.unit(13.0f)), kWhiteText);
        style.align = Align::Center;
        const f32 centerY = v.maxLevel ? bar.centerY() - unitH : bar.centerY() - 3.0f * unitH;
        g.draw.text(bar.centerX(), ui::centeredBaseline(g.draw, style, centerY), v.text, style);
    }
    return tapped;
}

} // namespace vd
