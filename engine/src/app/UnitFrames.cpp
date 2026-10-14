#include "app/UnitFrames.h"

#include "app/HudShared.h"
#include "app/UnitMenuWindows.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;
using ui::withAlpha;

constexpr u64 kPlayerStatus = assetPathHash("Interface\\CharacterFrame\\UI-Player-Status.blp");
constexpr u64 kStateIcon = assetPathHash("Interface\\CharacterFrame\\UI-StateIcon.blp");
constexpr u64 kAttackBackground =
    assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame-AttackBackground.blp");
constexpr u64 kPvpAlliance = assetPathHash("Interface\\TargetingFrame\\UI-PVP-Alliance.blp");
constexpr u64 kPvpHorde = assetPathHash("Interface\\TargetingFrame\\UI-PVP-Horde.blp");
constexpr u64 kPvpFreeForAll = assetPathHash("Interface\\TargetingFrame\\UI-PVP-FFA.blp");
constexpr u64 kLeaderIcon = assetPathHash("Interface\\GroupFrame\\UI-Group-LeaderIcon.blp");
constexpr u64 kMasterLooterIcon = assetPathHash("Interface\\GroupFrame\\UI-Group-MasterLooter.blp");
constexpr u64 kTickNormal = assetPathHash("Interface\\MainMenuBar\\UI-ExhaustionTickNormal.blp");
constexpr u64 kTickHighlight = assetPathHash("Interface\\MainMenuBar\\UI-ExhaustionTickHighlight.blp");
constexpr u64 kExpBarFrame = assetPathHash("Interface\\MainMenuBar\\UI-MainMenuBar-Dwarf.blp");

// ExhaustionTick_Update: the bar's color by rest state (rested blue, normal purple).
constexpr u32 kRestedColor = rgba(0, 99, 224);  // 0.0, 0.39, 0.88
constexpr u32 kNormalColor = rgba(148, 0, 140); // 0.58, 0.0, 0.55

u64 badgeTexture(game::PvpBadge badge) {
    switch (badge) {
    case game::PvpBadge::Alliance: return kPvpAlliance;
    case game::PvpBadge::Horde: return kPvpHorde;
    case game::PvpBadge::FreeForAll: return kPvpFreeForAll;
    default: return 0;
    }
}

u64 classificationArt(game::Classification c) {
    switch (c) { // TargetFrame_CheckClassification
    case game::Classification::WorldBoss:
    case game::Classification::RareElite:
    case game::Classification::Elite: return hud::kTargetingElite;
    case game::Classification::Rare: return hud::kTargetingRare;
    default: return hud::kTargetingFrame;
    }
}

// PlayerFrame_OnUpdate: the status glow's alpha climbs from 55 to 255 over half a second and falls back.
f32 statusPulse(u64 nowMs) {
    const f32 t = static_cast<f32>(nowMs % 1000) / 1000.0f;
    const f32 counter = std::fmod(t, 0.5f);
    return t < 0.5f ? (55.0f + counter * 400.0f) / 255.0f : (255.0f - counter * 400.0f) / 255.0f;
}

// GameFontNormalSmall (10 units), a little larger for the phone.
ui::TextStyle frameFont(const ui::Gui& g, u32 color) {
    ui::TextStyle style;
    style.size = g.unit(10.0f) * 1.25f;
    style.shadow = std::max(1.0f, g.unit(1.0f));
    style.align = ui::Align::Center;
    style.color = color;
    return style;
}

// TextStatusBarText (NumberFontNormal) at the frames' size.
ui::TextStyle barFont(const ui::Gui& g) {
    ui::TextStyle style = frameFont(g, ui::kWhite);
    style.size = g.unit(9.0f) * 1.2f;
    return style;
}

void bar(ui::Gui& g, const Rect& r, f32 fraction, u32 color) {
    const f32 f = std::clamp(fraction, 0.0f, 1.0f);
    g.draw.sprite({r.x, r.y, r.w * f, r.h}, hud::kStatusBar, color, {0.0f, 0.0f, f, 1.0f});
}

f32 fractionOf(u32 value, u32 max) {
    return max > 0 ? static_cast<f32>(value) / static_cast<f32>(max) : 0.0f;
}

} // namespace

Rect playerBadgeRect(const ui::Gui& g, const Rect& area) {
    return {area.x + g.unit(18.0f), area.y + g.unit(20.0f), g.unit(39.0f), g.unit(37.0f)};
}

void playerFramePlaceholder(ui::Gui& g, const Rect& area, std::string_view name) {
    const auto u = [&](f32 v) { return g.unit(v); };
    const f32 x = area.x, y = area.y;
    g.draw.solid({x + u(106.0f), y + u(22.0f), u(119.0f), u(41.0f)}, rgba(0, 0, 0, 128));
    g.draw.portrait({x + u(42.0f), y + u(12.0f), u(64.0f), u(64.0f)}, {}, hud::racePortrait(1, 0));
    g.draw.sprite({x, y, u(232.0f), u(100.0f)}, hud::kTargetingFrame, ui::kWhite,
                  {1.0f, 0.0f, 0.09375f, 0.78125f});
    const ui::TextStyle label = frameFont(g, ui::kGold);
    g.draw.text(x + u(166.0f), y + u(31.0f) + g.draw.ascent(label.font, label.size) * 0.5f, name, label);
}

UnitFrameResult playerFrame(ui::Gui& g, const Rect& area, const PlayerFrameView& v, u64 nowMs) {
    VD_PROFILE_ZONE("UnitFrames::player");
    const auto u = [&](f32 value) { return g.unit(value); };
    const f32 x = area.x, y = area.y;
    UnitFrameResult result;
    // PlayerFrame's HitRectInsets (6, 0, 4, 9); the PvP badge's hit area over it.
    const ui::WidgetState state =
        ui::hit(g, ui::widgetId("playerFrame"), {x + u(6.0f), y + u(4.0f), u(226.0f), u(87.0f)});
    result.tapped = ui::tapped(state);
    result.held = state.longPress;

    // Layout of PlayerFrame.xml: 232 x 100 units.
    g.draw.solid({x + u(106.0f), y + u(22.0f), u(119.0f), u(41.0f)}, rgba(0, 0, 0, 128));
    g.draw.portrait({x + u(42.0f), y + u(12.0f), u(64.0f), u(64.0f)}, v.portrait,
                    hud::racePortrait(v.race, v.gender));
    const u8 powerType = std::min<u8>(v.powerType, 4);
    bar(g, {x + u(106.0f), y + u(41.0f), u(119.0f), u(12.0f)},
        fractionOf(v.health, std::max(1u, v.maxHealth)), rgba(0, 255, 0));
    if (v.maxPower > 0) {
        bar(g, {x + u(106.0f), y + u(52.0f), u(119.0f), u(12.0f)}, fractionOf(v.power, v.maxPower),
            hud::kPowerColors[powerType]);
    }
    // The frame art over them (mirrored: the client draws the player's side so).
    g.draw.sprite({x, y, u(232.0f), u(100.0f)}, hud::kTargetingFrame, ui::kWhite,
                  {1.0f, 0.0f, 0.09375f, 0.78125f});

    // Name and level (GameFontNormalSmall: gold), health and power (TextStatusBarText).
    const ui::TextStyle label = frameFont(g, ui::kGold);
    const f32 ascent = g.draw.ascent(label.font, label.size);
    g.draw.text(x + u(166.0f), y + u(31.0f) + ascent * 0.5f, v.name, label);
    char text[32];
    if (v.level > 0) {
        std::snprintf(text, sizeof(text), "%u", v.level);
        g.draw.text(x + u(53.0f), y + u(66.0f) + ascent * 0.5f, text, label);
    }
    const ui::TextStyle numbers = barFont(g);
    const f32 numberAscent = g.draw.ascent(numbers.font, numbers.size);
    const bool barText = v.barText || state.pressed; // phase 15 (options): STATUS_BAR_TEXT, or the finger's hover
    std::snprintf(text, sizeof(text), "%u / %u", v.health, v.maxHealth);
    if (barText) {
        g.draw.text(x + u(166.0f), y + u(47.0f) + numberAscent * 0.5f, text, numbers);
    }
    if (v.maxPower > 0 && barText) {
        const u32 shown = powerType == 1 ? 10u : 1u; // rage is kept in tenths
        std::snprintf(text, sizeof(text), "%u / %u", v.power / shown, v.maxPower / shown);
        g.draw.text(x + u(166.0f), y + u(58.0f) + numberAscent * 0.5f, text, numbers);
    }

    // PlayerFrame_UpdateStatus: the frame's glow (ADD), the swords' red backdrop, then the icons and their
    // glows.
    const f32 pulse = statusPulse(nowMs);
    if (v.statusGlow && v.status != PlayerStatus::None) {
        const u32 tint = v.status == PlayerStatus::Resting ? rgba(255, 224, 64) : rgba(255, 0, 0);
        g.draw.spriteAdditive({x + u(35.0f), y + u(8.0f), u(190.0f), u(66.0f)}, kPlayerStatus,
                              withAlpha(tint, pulse), {0.0f, 0.0f, 0.74609375f, 0.53125f});
    }
    if (v.status == PlayerStatus::Attacking) {
        g.draw.sprite({x + u(37.0f), y + u(50.0f), u(32.0f), u(32.0f)}, kAttackBackground,
                      rgba(204, 26, 26, 102)); // (0.8, 0.1, 0.1), alpha 0.4
    }
    if (const u64 badge = badgeTexture(v.pvp)) {
        g.draw.sprite({x + u(18.0f), y + u(20.0f), u(64.0f), u(64.0f)}, badge);
        result.badgeTapped =
            ui::tapped(ui::hit(g, ui::widgetId("playerFrame.pvp"), playerBadgeRect(g, area)));
    }
    if (v.leader) {
        g.draw.sprite({x + u(44.0f), y + u(10.0f), u(16.0f), u(16.0f)}, kLeaderIcon);
    }
    if (v.masterLooter) {
        g.draw.sprite({x + u(80.0f), y + u(10.0f), u(16.0f), u(16.0f)}, kMasterLooterIcon);
    }
    if (v.status == PlayerStatus::Resting) {
        g.draw.sprite({x + u(37.0f), y + u(49.0f), u(31.0f), u(33.0f)}, kStateIcon, ui::kWhite,
                      {0.0f, 0.0f, 0.5f, 0.421875f});
        g.draw.spriteAdditive({x + u(37.0f), y + u(49.0f), u(32.0f), u(32.0f)}, kStateIcon,
                              withAlpha(ui::kWhite, pulse), {0.0f, 0.5f, 0.5f, 1.0f});
    } else if (v.status == PlayerStatus::Attacking || v.status == PlayerStatus::InCombat) {
        g.draw.sprite({x + u(38.0f), y + u(50.0f), u(32.0f), u(32.0f)}, kStateIcon, ui::kWhite,
                      {0.5f, 0.0f, 1.0f, 0.5f});
        if (v.status == PlayerStatus::Attacking) {
            g.draw.spriteAdditive({x + u(38.0f), y + u(50.0f), u(32.0f), u(32.0f)}, kStateIcon,
                                  withAlpha(rgba(255, 0, 0), pulse), {0.5f, 0.5f, 1.0f, 1.0f});
        }
    }
    return result;
}

UnitFrameResult targetFrame(ui::Gui& g, const Rect& area, const TargetFrameView& v) {
    VD_PROFILE_ZONE("UnitFrames::target");
    const auto u = [&](f32 value) { return g.unit(value); };
    const f32 x = area.x, y = area.y;
    UnitFrameResult result;
    const ui::WidgetState state = ui::hit(g, ui::widgetId("targetFrame"), area);
    result.tapped = ui::tapped(state);
    result.held = state.longPress;

    // Layout of TargetFrame.xml: the player frame mirrored.
    g.draw.solid({x + u(7.0f), y + u(22.0f), u(119.0f), u(41.0f)}, rgba(0, 0, 0, 128));
    g.draw.sprite({x + u(7.0f), y + u(22.0f), u(119.0f), u(19.0f)}, hud::kLevelBackground, v.nameBackground);
    g.draw.portrait({x + u(126.0f), y + u(12.0f), u(64.0f), u(64.0f)}, v.portrait, v.portraitFallback,
                    v.tapped ? rgba(128, 128, 128) : ui::kWhite);
    const u8 powerType = std::min<u8>(v.powerType, 4);
    const f32 healthFraction = fractionOf(v.health, std::max(1u, v.maxHealth));
    bar(g, {x + u(7.0f), y + u(41.0f), u(119.0f), u(12.0f)}, healthFraction, rgba(0, 255, 0));
    if (v.maxPower > 0) {
        bar(g, {x + u(7.0f), y + u(52.0f), u(119.0f), u(12.0f)}, fractionOf(v.power, v.maxPower),
            hud::kPowerColors[powerType]);
    }
    g.draw.sprite({x, y, u(232.0f), u(100.0f)}, classificationArt(v.classification), ui::kWhite,
                  {0.09375f, 0.0f, 1.0f, 0.78125f});

    // TargetName (GameFontNormalSmall: gold), TargetLevelText or TargetHighLevelTexture (the skull).
    ui::TextStyle label = frameFont(g, ui::kGold);
    const f32 ascent = g.draw.ascent(label.font, label.size);
    g.draw.text(x + u(66.0f), y + u(31.0f) + ascent * 0.5f, v.name, label);
    if (v.level < 0) {
        g.draw.sprite({x + u(171.0f), y + u(58.0f), u(16.0f), u(16.0f)}, hud::kSkull);
    } else if (v.level > 0) {
        char levelText[8];
        std::snprintf(levelText, sizeof(levelText), "%d", v.level);
        label.color = v.levelColor;
        g.draw.text(x + u(179.0f), y + u(66.0f) + ascent * 0.5f, levelText, label);
    }
    const ui::TextStyle numbers = barFont(g);
    const f32 numberAscent = g.draw.ascent(numbers.font, numbers.size);
    if (v.dead) {
        g.draw.text(x + u(66.0f), y + u(47.0f) + numberAscent * 0.5f, text::get("DEAD"), numbers);
    } else {
        char percent[16];
        std::snprintf(percent, sizeof(percent), "%u%%", static_cast<u32>(healthFraction * 100.0f + 0.5f));
        g.draw.text(x + u(66.0f), y + u(47.0f) + numberAscent * 0.5f, percent, numbers);
    }
    if (const u64 badge = badgeTexture(v.pvp)) {
        g.draw.sprite({x + u(171.0f), y + u(20.0f), u(64.0f), u(64.0f)}, badge);
    }
    if (v.leader) {
        g.draw.sprite({x + u(172.0f), y + u(10.0f), u(16.0f), u(16.0f)}, kLeaderIcon);
    }
    // Phase 13: TargetRaidTargetIcon, 26 units centered 73 left of and 14 under the frame's top right.
    drawRaidTargetIcon(g.draw, {x + u(232.0f - 73.0f - 13.0f), y + u(14.0f - 13.0f), u(26.0f), u(26.0f)},
                       v.raidTarget);
    return result;
}

Rect exhaustionTickRect(const ui::Gui& g, const Rect& area, const ExperienceBarView& v) {
    if (v.next == 0 || v.rested == 0) {
        return {};
    }
    // ExhaustionTick_Update: at (xp + rested) / max along the bar, hidden past its end.
    const f32 at = static_cast<f32>(v.xp + v.rested) / static_cast<f32>(v.next) * area.w;
    if (at > area.w) {
        return {};
    }
    const f32 size = g.unit(32.0f);
    return {area.x + at - size * 0.5f, area.centerY() - size * 0.5f, size, size};
}

bool experienceBar(ui::Gui& g, const Rect& area, const ExperienceBarView& v) {
    VD_PROFILE_ZONE("UnitFrames::experience");
    if (v.next == 0) {
        return false; // the maximum level
    }
    const bool tapped = ui::tapped(ui::hit(g, ui::widgetId("experienceBar"), area));
    const u32 color = v.restState == game::kRestStateRested ? kRestedColor : kNormalColor;
    g.draw.solid(area, rgba(0, 0, 0, 128));
    // ExhaustionLevelFillBar: the rest color, faint, out to the tick.
    const Rect tick = exhaustionTickRect(g, area, v);
    if (tick.w > 0.0f) {
        g.draw.solid({area.x, area.y, tick.centerX() - area.x, area.h}, withAlpha(color, 0.15f));
    }
    bar(g, area, fractionOf(v.xp, v.next), color);
    // MainMenuXPBarTexture0..3: the segmented frame over it, a quarter of the bar each (10 of its 13 units, 3
    // above its bottom).
    const f32 frameH = area.h * 10.0f / 13.0f;
    const f32 frameY = area.bottom() - area.h * 3.0f / 13.0f - frameH;
    constexpr f32 kRows[4] = {0.79296875f, 0.54296875f, 0.29296875f, 0.04296875f};
    for (u32 i = 0; i < 4; ++i) {
        g.draw.sprite({area.x + area.w * 0.25f * static_cast<f32>(i), frameY, area.w * 0.25f, frameH},
                      kExpBarFrame, ui::kWhite, {0.0f, kRows[i], 1.0f, kRows[i] + 0.0390625f});
    }
    if (tick.w > 0.0f) {
        g.draw.sprite(tick, kTickNormal);
        if (v.showText) {
            g.draw.spriteAdditive(tick, kTickHighlight, color);
        }
    }
    if (v.showText) { // TextStatusBar_UpdateTextString: "value / max", no prefix
        char text[32];
        std::snprintf(text, sizeof(text), "%u / %u", v.xp, v.next);
        ui::TextStyle style = ui::numberFont(std::max(area.h * 1.05f, g.unit(13.0f)), ui::kWhite);
        style.align = ui::Align::Center;
        g.draw.text(area.centerX(), ui::centeredBaseline(g.draw, style, area.centerY() - g.unit(1.0f)), text,
                    style);
    }
    return tapped;
}

} // namespace vd
