#include "app/SocialWindows.h"

#include "app/AuraLayout.h"
#include "app/DialogKit.h"
#include "app/FriendsFrameLayout.h"
#include "app/QuestLogLayout.h"
#include "app/UiResources.h"
#include "app/WindowArt.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

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

constexpr u64 kButtonHighlight = assetPathHash("Interface\\Buttons\\ButtonHilight-Square.blp");
// Party frames (PartyMemberFrame.xml) and groups.
constexpr u64 kPartyFrame = assetPathHash("Interface\\TargetingFrame\\UI-PartyFrame.blp");
constexpr u64 kLeaderIcon = assetPathHash("Interface\\GroupFrame\\UI-Group-LeaderIcon.blp");
constexpr u64 kDebuffOverlays = assetPathHash("Interface\\Buttons\\UI-Debuff-Overlays.blp"); // parity: auras
constexpr u64 kStatusBar = assetPathHash("Interface\\TargetingFrame\\UI-StatusBar.blp");
constexpr u64 kPetPortraitArt = assetPathHash("Interface\\CharacterFrame\\TemporaryPortrait-Monster.blp"); // phase 15
// Retouches (friends frame): FriendsFrame's art per page (FriendsFrame_Update: the picture's quarters, streamed from
// textures.vdpk: big), its portrait, TabButtonTemplate's art, the rows' highlight and the scroll bars' track (in the
// atlas).
constexpr u64 kGeneralTop[2] = {assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-General-TopLeft.blp"),
                                assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-General-TopRight.blp")};
constexpr u64 kTrainerTop[2] = {assetPathHash("Interface\\ClassTrainerFrame\\UI-ClassTrainer-TopLeft.blp"),
                                assetPathHash("Interface\\ClassTrainerFrame\\UI-ClassTrainer-TopRight.blp")};
constexpr u64 kFriendsArt[5][4] = { // FriendsPage order
    {kGeneralTop[0], kGeneralTop[1], assetPathHash("Interface\\FriendsFrame\\UI-FriendsFrame-BotLeft.blp"),
     assetPathHash("Interface\\FriendsFrame\\UI-FriendsFrame-BotRight.blp")},
    {kGeneralTop[0], kGeneralTop[1], assetPathHash("Interface\\FriendsFrame\\UI-IgnoreFrame-BotLeft.blp"),
     assetPathHash("Interface\\FriendsFrame\\UI-IgnoreFrame-BotRight.blp")},
    {kTrainerTop[0], kTrainerTop[1], assetPathHash("Interface\\FriendsFrame\\WhoFrame-BotLeft.blp"),
     assetPathHash("Interface\\FriendsFrame\\WhoFrame-BotRight.blp")},
    {kTrainerTop[0], kTrainerTop[1], assetPathHash("Interface\\FriendsFrame\\GuildFrame-BotLeft.blp"),
     assetPathHash("Interface\\FriendsFrame\\GuildFrame-BotRight.blp")},
    {kGeneralTop[0], kGeneralTop[1], assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-General-BottomLeft.blp"),
     assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-General-BottomRight.blp")},
};
constexpr u64 kFriendsIcon = assetPathHash("Interface\\FriendsFrame\\FriendsFrameScrollIcon.blp");
constexpr u64 kToggleTabActive = assetPathHash("Interface\\HelpFrame\\HelpFrameTab-Active.blp");
constexpr u64 kToggleTabInactive = assetPathHash("Interface\\HelpFrame\\HelpFrameTab-Inactive.blp");
constexpr u64 kTabHighlight = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-Tab-Highlight.blp");
constexpr u64 kTitleHighlight = assetPathHash("Interface\\QuestFrame\\UI-QuestTitleHighlight.blp");
constexpr u64 kScrollTrack = assetPathHash("Interface\\PaperDollInfoFrame\\UI-Character-ScrollBar.blp");

constexpr u32 kWhiteText = ui::kWhite;

// The races in the portraits' file names (ChrRaces m_clientFileString: not texts).
const char* kRaceFiles[] = {"Human", "Orc", "Dwarf", "NightElf", "Scourge", "Tauren", "Gnome", "Troll"};

u64 racePortrait(u32 race, u32 gender) {
    char path[96];
    std::snprintf(path, sizeof(path), "Interface\\CharacterFrame\\TemporaryPortrait-%s-%s.blp",
                  gender == 1 ? "Female" : "Male", kRaceFiles[(race >= 1 && race <= 8) ? race - 1 : 0]);
    return assetPathHash(path);
}

void centeredLabel(Gui& g, const Rect& r, std::string_view text, TextStyle style) {
    style.align = Align::Center;
    ui::label(g, r, text, style);
}

// Friends frame: FriendsFrameTab1..4 (FRIENDS, WHO, GUILD, RAID) turned along the window's right edge (not 1.12's),
// from the top in that order, the guild's disabled out of a guild; each at least a finger long, its target a finger
// wide. Returns the SocialTab tapped (-1 none).
i32 sideTabs(Gui& g, const Rect& column, SocialTab tab, bool guildEnabled, f32 textSize) {
    static constexpr SocialTab kTabs[] = {SocialTab::Friends, SocialTab::Who, SocialTab::Guild, SocialTab::Group};
    const std::string_view names[] = {text::get("FRIENDS"), text::get("WHO"), text::get("GUILD"), text::get("RAID")};
    const u32 selected = tab == SocialTab::Who ? 1u : tab == SocialTab::Guild ? 2u : tab == SocialTab::Group ? 3u : 0u;
    const f32 finger = g.px(ui::kTouchDp);
    const i32 tapped = art::characterTabColumn(g, "social.side", column, names, selected, textSize,
                                               finger + 14.0f * column.w / 32.0f, guildEnabled ? 0u : 4u, finger);
    return tapped < 0 ? -1 : static_cast<i32>(kTabs[tapped]);
}

} // namespace

// --- Party frames ------------------------------------------------------------------------------------------

namespace {
constexpr f32 kPartyScale = 1.6f; // the client's units, larger for a finger and a phone's distance
constexpr f32 kPartyStep = 50.0f;  // units between two frames
// Phase 15 (pets): PartyMemberFrame_UpdatePet moves the pet's frame from (23, -27) to (23, -43) when it shows, and the
// next member frame hangs under it (16 units lower in 1.12, whose frames are 63 apart: here they are 50, so the pet's
// frame, 26 high with its debuffs reaching 31, takes its own height to stay clear of the next one).
constexpr f32 kPartyPetStep = 26.0f;

// Phase 15 (pets): PartyMemberPetFrameTemplate (64 x 26 at the member frame's (23, 43)): the portrait (18 x 18 at
// 3, 3), UI-PartyFrame at half size over it, the health bar (35 x 4 at 23, 6), four debuffs from (24, 16); the name is
// hidden. A tap targets the pet (PartyMemberPetFrame_OnClick: the left button only). Returns true when tapped.
bool partyPetFrame(DialogContext& c, Gui& g, u32 index, f32 x, f32 y, f32 k, const PartyFrameView& p) {
    // HitRectInsets (3, 33, 3, 3): the portrait and the bar.
    const ui::WidgetState state =
        ui::hit(g, widgetId("party.pet", index), {x + 3.0f * k, y + 3.0f * k, 28.0f * k, 20.0f * k});
    const Rect bar{x + 23.0f * k, y + 6.0f * k, 35.0f * k, 4.0f * k};
    g.draw.solid(bar, rgba(0, 0, 0, 160));
    const f32 hf = std::clamp(p.petHealth, 0.0f, 1.0f);
    g.draw.sprite({bar.x, bar.y, bar.w * hf, bar.h}, kStatusBar, rgba(0, 255, 0), {0.0f, 0.0f, hf, 1.0f});
    const Rect portrait{x + 3.0f * k, y + 3.0f * k, 18.0f * k, 18.0f * k};
    g.draw.portrait(portrait, p.petPortrait, kPetPortraitArt);
    g.draw.sprite({x, y + 1.0f * k, 64.0f * k, 32.0f * k}, kPartyFrame);
    for (u32 d = 0; d < p.petDebuffCount && d < p.petDebuffs.size(); ++d) {
        const Rect icon{x + (24.0f + static_cast<f32>(d) * 17.0f) * k, y + 16.0f * k, 15.0f * k, 15.0f * k};
        if (const std::optional<u32> slot = c.resources.icon(p.petDebuffs[d].iconHash)) {
            g.draw.icon(icon, *slot, ui::kWhite, {0.0f, 0.0f, 1.0f, 1.0f});
        } else {
            g.draw.solid(icon, rgba(0, 0, 0, 120));
        }
        g.draw.sprite(icon.inset(-1.0f * k), kDebuffOverlays, debuffTypeColor(p.petDebuffs[d].dispel),
                      {0.296875f, 0.0f, 0.5703125f, 0.515625f});
    }
    if (p.petTargeted) {
        g.draw.spriteAdditive(portrait.inset(-2.0f * k), kButtonHighlight, rgba(255, 220, 90, 180));
    }
    return ui::tapped(state);
}
constexpr u32 kPowerColors[] = {rgba(0, 0, 255), rgba(255, 0, 0), rgba(255, 128, 64), rgba(255, 255, 0),
                                rgba(0, 255, 255)};
} // namespace

f32 partyFramesHeight(const DialogContext& c, u32 count) {
    const f32 k = c.ui.px(ui::kUnitDp) * kPartyScale;
    return count == 0 ? 0.0f : (kPartyStep * static_cast<f32>(count - 1) + 50.0f) * k;
}

f32 partyFramesHeight(const DialogContext& c, std::span<const PartyFrameView> members) {
    const f32 k = c.ui.px(ui::kUnitDp) * kPartyScale;
    const u32 count = static_cast<u32>(std::min<usize>(members.size(), 4));
    f32 h = partyFramesHeight(c, count);
    for (u32 i = 0; i < count; ++i) {
        h += members[i].pet && members[i].online ? kPartyPetStep * k : 0.0f; // phase 15 (pets)
    }
    return h;
}

i32 partyFrames(DialogContext& c, const Rect& area, std::span<const PartyFrameView> members, i32* held,
                ui::Rect* heldFrame, ui::Rect* tappedFrame, i32* tappedPet, ui::Rect* frames) {
    VD_PROFILE_ZONE("SocialWindows::partyFrames");
    Gui g{c.ui, c.draw};
    const f32 k = g.unit(1.0f) * kPartyScale;
    i32 tapped = -1;
    f32 petShift = 0.0f; // phase 15 (pets): the pets' frames above push the member down
    for (u32 i = 0; i < members.size() && i < 4; ++i) {
        const PartyFrameView& p = members[i];
        const f32 x = area.x, y = area.y + static_cast<f32>(i) * kPartyStep * k + petShift;
        const f32 alpha = p.inSight && p.online ? 1.0f : 0.6f;
        const Rect frame{x, y, 120.0f * k, 48.0f * k};
        const ui::WidgetState state = ui::hit(g, widgetId("party.frame", i), frame);
        if (frames) { // phase 20 (assists)
            frames[i] = frame;
            frames[4 + i] = p.pet && p.online ? Rect{x + 23.0f * k, y + 43.0f * k, 60.0f * k, 26.0f * k} : Rect{};
        }
        if (ui::tapped(state)) {
            tapped = static_cast<i32>(i);
            if (tappedFrame) {
                *tappedFrame = frame;
            }
        }
        if (state.longPress && held) { // parity (auras): the member's aura list (PartyMemberBuffTooltip)
            *held = static_cast<i32>(i);
            if (heldFrame) {
                *heldFrame = frame;
            }
        }
        // Behind the bars, the bars, the portrait, then the frame's art over them.
        const Rect health{x + 47.0f * k, y + 12.0f * k, 70.0f * k, 8.0f * k};
        const Rect power{x + 47.0f * k, y + 20.0f * k, 70.0f * k, 8.0f * k};
        g.draw.solid({health.x, health.y, health.w, 16.0f * k}, ui::withAlpha(rgba(0, 0, 0, 160), alpha));
        if (p.online && !p.dead) {
            const f32 hf = std::clamp(p.health, 0.0f, 1.0f), pf = std::clamp(p.power, 0.0f, 1.0f);
            g.draw.sprite({health.x, health.y, health.w * hf, health.h}, kStatusBar,
                          ui::withAlpha(rgba(0, 255, 0), alpha), {0.0f, 0.0f, hf, 1.0f});
            g.draw.sprite({power.x, power.y, power.w * pf, power.h}, kStatusBar,
                          ui::withAlpha(kPowerColors[std::min<u32>(p.powerType, 4)], alpha), {0.0f, 0.0f, pf, 1.0f});
        }
        const Rect portrait{x + 7.0f * k, y + 6.0f * k, 37.0f * k, 37.0f * k};
        g.draw.portrait(portrait, p.portrait, racePortrait(p.race ? p.race : 1, p.gender),
                        p.online ? ui::withAlpha(ui::kWhite, alpha) : rgba(90, 90, 90));
        g.draw.sprite({x, y, 128.0f * k, 64.0f * k}, kPartyFrame, ui::withAlpha(ui::kWhite, alpha));
        // Parity (auras): the "Status" glow round the portrait (UI-Debuff-Overlays' left cell, 36 units), then the four
        // debuffs (PartyBuffButtonTemplate: 15 units, 2 apart, from (48, 32); their borders 17).
        if (p.statusColor != 0) {
            g.draw.sprite({portrait.centerX() - 18.0f * k, portrait.centerY() - 18.0f * k, 36.0f * k, 36.0f * k},
                          kDebuffOverlays, ui::withAlpha(p.statusColor, p.statusAlpha),
                          {0.0f, 0.0f, 0.2734375f, 0.5625f});
        }
        for (u32 d = 0; d < p.debuffCount && d < p.debuffs.size(); ++d) {
            const Rect icon{x + (48.0f + static_cast<f32>(d) * 17.0f) * k, y + 32.0f * k, 15.0f * k, 15.0f * k};
            if (const std::optional<u32> slot = c.resources.icon(p.debuffs[d].iconHash)) {
                g.draw.icon(icon, *slot, ui::withAlpha(ui::kWhite, alpha), {0.0f, 0.0f, 1.0f, 1.0f});
            } else {
                g.draw.solid(icon, rgba(0, 0, 0, 120));
            }
            g.draw.sprite(icon.inset(-1.0f * k), kDebuffOverlays,
                          ui::withAlpha(debuffTypeColor(p.debuffs[d].dispel), alpha),
                          {0.296875f, 0.0f, 0.5703125f, 0.515625f});
        }
        if (p.targeted) {
            g.draw.spriteAdditive(portrait.inset(-3.0f * k), kButtonHighlight, rgba(255, 220, 90, 180));
        }
        if (p.leader) {
            g.draw.sprite({x, y, 16.0f * k, 16.0f * k}, kLeaderIcon);
        }
        // The name over the bars (with <AFK> / <DND>), the level under the portrait, the state over the bars.
        TextStyle nameStyle = ui::gameFont(10.0f * k, ui::withAlpha(ui::kGold, alpha));
        nameStyle.shadow = std::max(1.0f, g.px(1.0f));
        char text[48];
        const std::string_view flag = p.afk ? text::get("CHAT_FLAG_AFK") : p.dnd ? text::get("CHAT_FLAG_DND") : "";
        std::snprintf(text, sizeof(text), "%.*s%s%.*s", static_cast<int>(p.name.size()), p.name.data(),
                      flag.empty() ? "" : " ", static_cast<int>(flag.size()), flag.data());
        ui::label(g, {x + 47.0f * k, y, 78.0f * k, 11.0f * k}, text, nameStyle);
        if (p.level > 0) {
            std::snprintf(text, sizeof(text), "%u", p.level);
            TextStyle levelStyle = ui::numberFont(9.0f * k, ui::withAlpha(ui::kGold, alpha));
            levelStyle.outline = std::max(1.0f, g.px(1.0f));
            centeredLabel(g, {x, y + 34.0f * k, 16.0f * k, 12.0f * k}, text, levelStyle);
        }
        const std::string_view status = !p.online ? text::get("PLAYER_OFFLINE")
                                        : p.ghost ? text::get("VD_SOCIAL_GHOST")
                                        : p.dead  ? text::get("DEAD")
                                                  : std::string_view{};
        if (!status.empty()) {
            TextStyle s = ui::numberFont(9.0f * k, ui::kWhite);
            s.outline = std::max(1.0f, g.px(1.0f));
            centeredLabel(g, {health.x, health.y, health.w, 16.0f * k}, status, s);
        }
        if (p.pet && p.online) { // phase 15 (pets)
            if (partyPetFrame(c, g, i, x + 23.0f * k, y + 43.0f * k, k, p) && tappedPet) {
                *tappedPet = static_cast<i32>(i);
            }
            petShift += kPartyPetStep * k;
        }
    }
    return tapped;
}

// --- Choices -----------------------------------------------------------------------------------------------

i32 choiceList(DialogContext& c, std::string_view name, std::string_view title, std::span<const ChoiceView> choices) {
    VD_PROFILE_ZONE("SocialWindows::choices");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const f32 rowH = m.row;
    const f32 heightDp = (rowH * static_cast<f32>(choices.size()) + g.px(8.0f)) / c.ui.dp() + 64.0f;
    const Dialog d = beginDialog(c, g, name, 380.0f, heightDp, title, true, false);
    i32 picked = d.closed || d.outside ? -2 : -1;
    const ui::ScrollView view = ui::beginScroll(g, widgetId(name, 20), d.body);
    f32 y = view.top + g.px(4.0f);
    for (u32 i = 0; i < choices.size(); ++i) {
        const ChoiceView& ch = choices[i];
        ui::RowStyle row;
        row.text = ui::gameFont(m.text, ch.enabled ? ch.color : ui::kDisabledText);
        row.selected = ch.selected;
        row.highlight = ui::RowHighlight::Listbox;
        if (ui::listRow(g, widgetId(name, 100 + i), {d.body.x, y, d.body.w, rowH}, ch.text, row) && ch.enabled) {
            picked = static_cast<i32>(i);
        }
        y += rowH;
    }
    ui::endScroll(g, view, y - view.top + g.px(4.0f));
    return picked;
}

// --- Raid information (phase 12) ---------------------------------------------------------------------------

bool raidInfoPanel(DialogContext& c, std::span<const RaidInfoRowView> rows) {
    VD_PROFILE_ZONE("SocialWindows::raidInfo");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    // RaidInfoInstanceTemplate: 40 px a row (the name in GameFontNormal, the reset under it); up to four
    // without scrolling.
    const f32 rowH = m.text * 1.2f + m.small * 1.3f + g.px(6.0f);
    const f32 headH = m.small * 1.4f;
    const u32 shown = std::clamp<u32>(static_cast<u32>(rows.size()), 1u, 4u);
    const f32 heightDp = (headH * 2.0f + rowH * static_cast<f32>(shown) + g.px(8.0f)) / c.ui.dp() + 64.0f;
    const Dialog d = beginDialog(c, g, "raidInfo", 400.0f, heightDp, text::get("RAID_INFORMATION"), true, false);
    // RaidInfoSubheader (GameFontHighlightSmall), then the column labels INSTANCE and ID (GameFontDisableSmall).
    f32 y = d.body.y;
    ui::label(g, {d.body.x, y, d.body.w, headH}, text::get("RAID_INFO_DESC"), ui::gameFont(m.small, kWhiteText));
    y += headH;
    TextStyle columns = ui::gameFont(m.small, ui::kDisabledText);
    ui::label(g, {d.body.x, y, d.body.w, headH}, text::get("INSTANCE"), columns);
    columns.align = Align::Right;
    ui::label(g, {d.body.x, y, d.body.w, headH}, text::get("ID"), columns);
    y += headH;
    const ui::ScrollView view =
        ui::beginScroll(g, widgetId("raidInfo.scroll"), {d.body.x, y, d.body.w, d.body.bottom() - y});
    f32 rowY = view.top;
    TextStyle id = ui::gameFont(m.small, ui::kDisabledText);
    id.align = Align::Right;
    for (const RaidInfoRowView& r : rows) {
        ui::label(g, {d.body.x, rowY, d.body.w, m.text * 1.2f}, r.name, ui::gameFont(m.text));
        ui::label(g, {d.body.x, rowY, d.body.w, m.text * 1.2f}, r.id, id);
        ui::label(g, {d.body.x, rowY + m.text * 1.2f, d.body.w, m.small * 1.3f}, r.reset,
                  ui::gameFont(m.small, kWhiteText));
        rowY += rowH;
    }
    ui::endScroll(g, view, rowY - view.top);
    return d.closed || d.outside;
}

// --- Emotes ------------------------------------------------------------------------------------------------

i32 emoteMenu(DialogContext& c, std::span<const EmoteButtonView> emotes, std::string_view target) {
    VD_PROFILE_ZONE("SocialWindows::emotes");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    const u32 cols = wide ? 5u : 3u;
    const u32 rows = static_cast<u32>((emotes.size() + cols - 1) / cols);
    const f32 cellH = m.button + g.px(6.0f);
    const f32 heightDp = (cellH * static_cast<f32>(rows) + m.small * 2.0f) / c.ui.dp() + 70.0f;
    const Dialog d =
        beginDialog(c, g, "emotes", wide ? 640.0f : 440.0f, heightDp, text::get("EMOTE_MESSAGE"), true, false);
    i32 picked = d.closed || d.outside ? -2 : -1;
    f32 y = d.body.y;
    if (!target.empty()) {
        char line[96];
        centeredLabel(g, {d.body.x, y, d.body.w, m.small * 1.8f}, text::format(line, "VD_SOCIAL_EMOTE_TARGET", target),
                      ui::gameFont(m.small, ui::kWhite));
        y += m.small * 1.8f;
    }
    const ui::ScrollView view = ui::beginScroll(g, widgetId("emotes.scroll"), {d.body.x, y, d.body.w, d.body.bottom() - y});
    const f32 gap = g.px(6.0f);
    const f32 cellW = (d.body.w - gap * static_cast<f32>(cols - 1)) / static_cast<f32>(cols);
    for (u32 i = 0; i < emotes.size(); ++i) {
        const Rect r{d.body.x + static_cast<f32>(i % cols) * (cellW + gap),
                     view.top + static_cast<f32>(i / cols) * cellH, cellW, m.button};
        if (ui::button(g, widgetId("emotes.button", i), r, emotes[i].label)) {
            picked = static_cast<i32>(i);
        }
    }
    ui::endScroll(g, view, cellH * static_cast<f32>(rows));
    return picked;
}

// --- A player's sheet --------------------------------------------------------------------------------------

i32 playerSheet(DialogContext& c, const PlayerSheetView& v) {
    VD_PROFILE_ZONE("SocialWindows::sheet");
    Gui g{c.ui, c.draw};
    const Metrics m = metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    const u32 cols = wide ? 2u : 1u;
    const u32 rows = static_cast<u32>((v.actions.size() + cols - 1) / cols);
    const f32 gap = g.px(6.0f);
    const f32 rowH = m.button;
    const f32 heightDp = ((rowH + gap) * static_cast<f32>(rows) + m.small * 2.2f) / c.ui.dp() + 70.0f;
    const Dialog d = beginDialog(c, g, "sheet", wide ? 560.0f : 380.0f, heightDp, v.name, true, false);
    i32 picked = d.closed || d.outside ? -2 : -1;
    f32 y = d.body.y;
    if (!v.detail.empty()) {
        centeredLabel(g, {d.body.x, y, d.body.w, m.small * 2.0f}, v.detail, ui::gameFont(m.small, ui::kWhite));
        y += m.small * 2.2f;
    }
    const ui::ScrollView view = ui::beginScroll(g, widgetId("sheet.scroll"), {d.body.x, y, d.body.w, d.body.bottom() - y});
    const f32 colW = (d.body.w - gap * static_cast<f32>(cols - 1)) / static_cast<f32>(cols);
    for (u32 i = 0; i < v.actions.size(); ++i) {
        const Rect r{d.body.x + static_cast<f32>(i % cols) * (colW + gap),
                     view.top + static_cast<f32>(i / cols) * (rowH + gap), colW, rowH};
        if (ui::button(g, widgetId("sheet.action", i), r, v.actions[i].text, v.actions[i].enabled)) {
            picked = static_cast<i32>(i);
        }
    }
    ui::endScroll(g, view, (rowH + gap) * static_cast<f32>(rows));
    return picked;
}

// --- The friends frame -------------------------------------------------------------------------------------

namespace {

// TabButtonTemplate (FriendsFrameToggleTab1 / 2, IgnoreFrameToggleTab1 / 2): HelpFrameTab-Inactive's 16-unit ends and
// its middle; the selected one (PanelTemplates_SelectTab: the *Disabled textures) HelpFrameTab-Active 3 units lower,
// with GameFontHighlightSmall instead of GameFontNormalSmall; the text 13 high 5 over the bottom; UI-Character-Tab-
// Highlight (ADD, as wide as the text + 31, its bottom 8 under the tab's) while pressed. `r`: the tab, 32 units high.
// Its target reaches up a finger's height from its bottom (the title bar over it has nothing to tap there).
bool toggleTab(Gui& g, WidgetId id, const Rect& r, f32 k, std::string_view text, f32 textSize, bool active) {
    const f32 finger = g.px(ui::kTouchDp);
    const Rect target{r.x, std::min(r.y, r.bottom() - finger), r.w, std::max(r.h, finger)};
    const ui::WidgetState state = active ? ui::WidgetState{} : ui::hit(g, id, target);
    const Rect art = active ? Rect{r.x, r.y + 3.0f * k, r.w, r.h} : r;
    ui::threeSlice(g.draw, art, active ? kToggleTabActive : kToggleTabInactive, {0.0f, 0.0f, 1.0f, 1.0f}, 0.25f,
                   16.0f * k);
    const f32 textW = g.draw.measure(text, ui::FontId::Friz, textSize);
    if (!active && ui::held(state, target)) {
        const f32 w = textW + 31.0f * k;
        g.draw.spriteAdditive({r.centerX() + 2.0f * k - w * 0.5f, r.y + 8.0f * k, w, 32.0f * k}, kTabHighlight);
    }
    TextStyle style = ui::gameFont(textSize, active ? ui::kHighlight : ui::kGold);
    style.align = Align::Center;
    const f32 lineH = std::max(13.0f * k, textSize);
    ui::label(g, {r.x, r.bottom() - 5.0f * k - 6.5f * k - lineH * 0.5f, r.w, lineH}, text, style);
    return ui::tapped(state);
}

// The part of `r` inside `clip` (a row's target inside its list).
Rect clipped(const Rect& r, const Rect& clip) {
    const f32 x = std::max(r.x, clip.x), y = std::max(r.y, clip.y);
    return {x, y, std::max(0.0f, std::min(r.right(), clip.right()) - x), std::max(0.0f, std::min(r.bottom(), clip.bottom()) - y)};
}

} // namespace

FriendsFrameShell beginFriendsFrame(DialogContext& c, FriendsPage page, std::string_view title) {
    Gui g{c.ui, c.draw};
    FriendsFrameShell f;
    const bool panel = c.panel.w > 0.0f;
    if (!panel) {
        f.closed = ui::modalBlocker(g, widgetId("friends", 1), 0.3f);
    }
    f.l = friendsFrameLayout(panel ? c.panel : c.safeArea.inset(g.px(6.0f)), questLogScale(metrics(c).button),
                             !panel || c.panelAlign == PanelAlign::Center);
    f.s = questArtSizes(g, f.l.k);
    const FriendsFrameLayout& l = f.l;
    f.panel = {l.frame.x, l.frame.y, l.tabs.right() - l.frame.x, l.frame.h};
    ui::hit(g, widgetId("friends", 2), l.frame);
    std::array<TextureHandle, 4> art{};
    bool ready = true;
    const u32 set = static_cast<u32>(page);
    for (u32 i = 0; i < 4; ++i) {
        art[i] = c.resources.texture(kFriendsArt[set][i]);
        ready = ready && art[i].valid();
    }
    if (const TextureHandle icon = c.resources.texture(kFriendsIcon); icon.valid()) {
        g.draw.texture(l.portrait, icon);
    }
    if (ready) {
        const Rect& a = l.art;
        const f32 left = a.w * 256.0f / 384.0f, top = a.h * 0.5f;
        g.draw.texture({a.x, a.y, left, top}, art[0]);
        g.draw.texture({a.x + left, a.y, a.w - left, top}, art[1]);
        g.draw.texture({a.x, a.y + top, left, a.h - top}, art[2]);
        g.draw.texture({a.x + left, a.y + top, a.w - left, a.h - top}, art[3]);
    } else {
        g.draw.solid(l.frame.inset(8.0f * l.k), rgba(12, 12, 12, 230));
    }
    TextStyle style = ui::gameFont(f.s.normal);
    style.align = Align::Center;
    ui::label(g, l.title, title, style);
    f.closed = ui::closeButton(g, widgetId("friends.close"), l.close) || f.closed;
    return f;
}

i32 endFriendsFrame(DialogContext& c, const FriendsFrameShell& f, SocialTab current, bool guildEnabled) {
    Gui g{c.ui, c.draw};
    const i32 tab = sideTabs(g, f.l.tabs, current, guildEnabled, f.s.small);
    const SocialTab shown = current == SocialTab::Ignored ? SocialTab::Friends : current;
    return tab >= 0 && static_cast<SocialTab>(tab) != shown ? tab : -1;
}

FriendsListScroll beginFriendsList(DialogContext& c, WidgetId id, const FriendsListLayout& list, u32 rows, f32 rowH) {
    Gui g{c.ui, c.draw};
    FriendsListScroll s;
    s.content = list.rowTop - list.view.y + static_cast<f32>(rows) * rowH;
    s.overflow = s.content > list.view.h + 0.5f;
    if (s.overflow) { // FauxScrollFrame_Update: the scroll frame (its track and its bar) only when the rows overflow
        g.draw.sprite(list.track[0], kScrollTrack, ui::kWhite, {0.0f, 0.0f, 0.484375f, list.trackV});
        g.draw.sprite(list.track[1], kScrollTrack, ui::kWhite, {0.515625f, 0.0f, 1.0f, 0.4140625f});
    }
    s.view = ui::beginScroll(g, id, list.view);
    s.top = s.view.top + list.rowTop - list.view.y;
    return s;
}

bool endFriendsList(DialogContext& c, const FriendsListScroll& s, const FriendsListLayout& list, f32 barRightLimit) {
    Gui g{c.ui, c.draw};
    ui::endScroll(g, s.view, s.content, false);
    return s.overflow && ui::panelScrollBar(g, s.view, list.bar, barRightLimit);
}

ui::WidgetState friendsListRow(DialogContext& c, WidgetId id, const FriendsListScroll& s, const Rect& r, bool selected,
                               u64 highlight) {
    Gui g{c.ui, c.draw};
    const Rect target = clipped(r, s.view.viewport);
    const ui::WidgetState state = ui::hit(g, id, target);
    if (selected || ui::held(state, target)) { // the HighlightTexture (ADD): locked on the selected row, and pressed
        g.draw.spriteAdditive(r, highlight);
    }
    return state;
}

FriendsFrameResult friendsFrame(DialogContext& c, const FriendsFrameView& v) {
    VD_PROFILE_ZONE("SocialWindows::friendsFrame");
    Gui g{c.ui, c.draw};
    FriendsFrameResult result;
    const FriendsFrameShell f = beginFriendsFrame(c, v.ignore ? FriendsPage::Ignore : FriendsPage::Friends,
                                                  text::get(v.ignore ? "IGNORE_LIST" : "FRIENDS_LIST"));
    const FriendsFrameLayout& l = f.l;
    const QuestArtSizes& s = f.s;
    const f32 k = l.k;
    result.panel = f.panel;
    result.closed = f.closed;
    {
        // The toggle tabs, side by side from (70, -39), each as wide as PanelTemplates_TabResize(0) makes it (the
        // text and the two 16-unit ends), a finger's width at least.
        const std::string_view names[2] = {text::get("FRIENDS"), text::get("IGNORE")};
        f32 x = l.toggleTabs.x;
        for (u32 i = 0; i < 2; ++i) {
            const f32 w = std::max(g.draw.measure(names[i], ui::FontId::Friz, s.small) + 32.0f * k, g.px(ui::kTouchDp));
            const bool active = (i == 1) == v.ignore;
            if (toggleTab(g, widgetId("friends.toggle", i), {x, l.toggleTabs.y, w, l.toggleTabs.h}, k, names[i],
                          s.small, active)) {
                (i == 0 ? result.showFriends : result.showIgnore) = true;
            }
            x += w;
        }
    }

    // The list: FriendsFrameFriendButton1..10 (298 x 31, the name and location over the level and class) or
    // FriendsFrameIgnoreButton1..20 (298 x 16, the name), a finger's height at least; UI-QuestTitleHighlight.
    const FriendsListLayout& list = v.ignore ? l.ignore : l.friends;
    const f32 rowH = std::max((v.ignore ? 16.0f : 31.0f) * k, s.row);
    const FriendsListScroll scroll =
        beginFriendsList(c, widgetId("friends.list", v.ignore ? 1 : 0), list, static_cast<u32>(v.rows.size()), rowH);
    const TextStyle nameStyle = ui::gameFont(s.normal);           // GameFontNormal (the template's own colors inside)
    const TextStyle infoStyle = ui::gameFont(s.small, ui::kWhite); // GameFontHighlightSmall
    f32 y = scroll.top;
    for (u32 i = 0; i < v.rows.size(); ++i, y += rowH) {
        const Rect r{list.rowX, y, list.rowW, rowH};
        if (r.bottom() < scroll.view.viewport.y || r.y > scroll.view.viewport.bottom()) {
            continue;
        }
        const ui::WidgetState state = friendsListRow(c, widgetId("friends.row", (v.ignore ? 1000u : 0u) + i), scroll, r,
                                                     static_cast<i32>(i) == v.selected, kTitleHighlight);
        if (ui::tapped(state)) {
            result.row = static_cast<i32>(i);
        }
        if (state.longPress) {
            result.rowHeld = static_cast<i32>(i);
            result.rowRect = r;
        }
        // The texts 10 in (TOPLEFT 10, -3): over a taller row the block keeps the original's share of its room.
        const Rect text{r.x + 10.0f * k, r.y, r.w - 14.0f * k, rowH};
        if (v.ignore) {
            ui::label(g, text, v.rows[i].nameLocation, nameStyle);
            continue;
        }
        const f32 top = r.y + (rowH - s.normal - s.small) / 3.0f;
        ui::label(g, {text.x, top, text.w, s.normal}, v.rows[i].nameLocation, nameStyle);
        ui::label(g, {text.x, top + s.normal, text.w, s.small}, v.rows[i].info, infoStyle);
    }
    result.scrolled = endFriendsList(c, scroll, list);

    // The buttons (FriendsList_Update / IgnoreList_Update enable them).
    const bool any = !v.rows.empty();
    if (!v.ignore) {
        result.addFriend = ui::button(g, widgetId("friends.add"), l.addFriend, text::get("ADD_FRIEND"));
        result.removeFriend = ui::button(g, widgetId("friends.remove"), l.removeFriend, text::get("REMOVE_FRIEND"), any);
        result.sendMessage =
            ui::button(g, widgetId("friends.message"), l.sendMessage, text::get("SEND_MESSAGE"), any && v.connected);
        result.groupInvite =
            ui::button(g, widgetId("friends.invite"), l.groupInvite, text::get("GROUP_INVITE"), any && v.connected);
    } else {
        result.ignorePlayer = ui::button(g, widgetId("friends.ignore"), l.ignorePlayer, text::get("IGNORE_PLAYER"));
        result.stopIgnore = ui::button(g, widgetId("friends.unignore"), l.stopIgnore, text::get("STOP_IGNORE"), any);
    }
    result.tab = endFriendsFrame(c, f, SocialTab::Friends, v.guildEnabled);
    return result;
}

// --- WhoFrame ----------------------------------------------------------------------------------------------

WhoFrameResult whoFrame(DialogContext& c, const WhoFrameView& v) {
    VD_PROFILE_ZONE("SocialWindows::whoFrame");
    Gui g{c.ui, c.draw};
    WhoFrameResult result;
    const FriendsFrameShell f = beginFriendsFrame(c, FriendsPage::Who, text::get("WHO_LIST"));
    const FriendsFrameLayout& l = f.l;
    const QuestArtSizes& s = f.s;
    const f32 k = l.k;
    result.closed = f.closed;
    // The column headers: WhoFrameColumn_SetWidth's 83, 105 (120 without the scroll bar), 32 and 92, overlapping by 2.
    const FriendsListLayout& list = l.who.list;
    const f32 rowH = std::max(16.0f * k, s.row);
    const bool bar = list.rowTop - list.view.y + static_cast<f32>(v.rows.size()) * rowH > list.view.h + 0.5f;
    std::array<SortHeaderView, 4> headers{};
    std::array<f32, 5> edges{};
    const u32 columns = static_cast<u32>(std::min<usize>(v.headers.size(), headers.size()));
    constexpr f32 kWidths[4] = {83.0f, 105.0f, 32.0f, 92.0f};
    f32 total = 0.0f;
    for (u32 i = 0; i < columns; ++i) {
        headers[i] = v.headers[i];
        headers[i].weight = (i == 1 && !bar ? 120.0f : kWidths[i]) - (i + 1 < columns ? 2.0f : 0.0f);
        total += headers[i].weight;
    }
    const Rect headerRow{l.who.headers.x, l.who.headers.y, total * k + 2.0f * k, l.who.headers.h};
    if (columns > 0) {
        const SortHeaderResult h = sortHeaders(g, "who.header", headerRow, std::span(headers.data(), columns), s.small);
        result.header = h.tapped;
        result.headerMenu = h.menu;
        sortColumns(headerRow, std::span(headers.data(), columns), std::span(edges.data(), columns + 1));
    }
    // FriendsFrameWhoButtonTemplate: the name 10 in (GameFontNormalSmall), the other columns under their headers
    // (GameFontHighlightSmall, the level centered); UI-QuestTitleHighlight.
    const FriendsListScroll scroll = beginFriendsList(c, widgetId("who.list"), list, static_cast<u32>(v.rows.size()), rowH);
    f32 y = scroll.top;
    for (u32 i = 0; i < v.rows.size(); ++i, y += rowH) {
        const Rect r{list.rowX, y, list.rowW, rowH};
        if (r.bottom() < scroll.view.viewport.y || r.y > scroll.view.viewport.bottom()) {
            continue;
        }
        const ui::WidgetState state =
            friendsListRow(c, widgetId("who.row", i), scroll, r, static_cast<i32>(i) == v.selected, kTitleHighlight);
        if (ui::tapped(state)) {
            result.row = static_cast<i32>(i);
        }
        if (state.longPress) {
            result.rowHeld = static_cast<i32>(i);
            result.rowRect = r;
        }
        const WhoRowView& row = v.rows[i];
        const std::string_view cells[4] = {row.name, row.variable, row.level, row.cls};
        for (u32 col = 0; col < columns; ++col) {
            const f32 x0 = col == 0 ? r.x + 10.0f * k : edges[col] + 4.0f * k;
            const Rect cell{x0, r.y, std::max(0.0f, edges[col + 1] - x0 - 2.0f * k), rowH};
            TextStyle style = ui::gameFont(s.small, col == 0 ? ui::kGold : ui::kWhite);
            if (col == 2) {
                style.align = Align::Center;
            }
            g.draw.pushClip(cell);
            ui::label(g, cell, cells[col], style);
            g.draw.popClip();
        }
    }
    result.scrolled = endFriendsList(c, scroll, list);
    {
        // WhoFrameTotals (GameFontNormalSmall, centered).
        TextStyle style = ui::gameFont(s.small);
        style.align = Align::Center;
        ui::label(g, l.who.totals, v.totals, style);
    }
    const bool selected = v.selected >= 0 && static_cast<usize>(v.selected) < v.rows.size();
    result.refresh = ui::button(g, widgetId("who.refresh"), l.who.refresh, text::get("REFRESH"));
    result.addFriend = ui::button(g, widgetId("who.add"), l.who.addFriend, text::get("ADD_FRIEND"), selected);
    result.groupInvite = ui::button(g, widgetId("who.invite"), l.who.invite, text::get("GROUP_INVITE"), selected);
    {
        // WhoFrameEditBox in the art's slot: ChatFontNormal, 8 in, a caret while typing; a tap opens the keyboard. Its
        // target after the buttons' (theirs grow to a finger's height over the slot), from a little over it down to the
        // buttons' art.
        const Rect& box = l.who.edit;
        const f32 top = box.y - 6.0f * k;
        const Rect target{box.x, top, box.w, l.who.refresh.y - top};
        const ui::WidgetState state = ui::hit(g, widgetId("who.edit"), target);
        result.tappedEdit = ui::tapped(state);
        const TextStyle style = ui::chatFont(std::max(14.0f * k, g.px(12.0f)));
        const Rect text{box.x + 8.0f * k, box.y, box.w - 16.0f * k, box.h};
        g.draw.pushClip(box);
        const f32 width = g.draw.measure(v.input, style.font, style.size);
        const f32 shift = std::max(0.0f, width - text.w); // the end stays in sight while typing
        const f32 baseline = ui::centeredBaseline(g.draw, style, text.centerY());
        g.draw.text(text.x - shift, baseline, v.input, style);
        if (v.editing && (g.ui.nowMs() / 500) % 2 == 0) {
            const f32 caretX = text.x - shift + width + g.px(1.0f);
            g.draw.solid({caretX, text.centerY() - style.size * 0.5f, std::max(1.0f, g.px(1.5f)), style.size},
                         ui::kWhite);
        }
        g.draw.popClip();
    }
    result.tab = endFriendsFrame(c, f, SocialTab::Who, v.guildEnabled);
    return result;
}

} // namespace vd
