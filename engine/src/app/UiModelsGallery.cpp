#include "app/UiModelsGallery.h"

#include "app/HudActionSlot.h"
#include "app/TouchRoundIcon.h"
#include "app/UiResources.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "ui/DrawList.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;
using ui::withAlpha;
using Models = ui::UiModels;

constexpr u64 kIcons[] = {assetPathHash("Interface\\Icons\\Ability_Warrior_Charge.blp"),
                          assetPathHash("Interface\\Icons\\Spell_Holy_Heal.blp"),
                          assetPathHash("Interface\\Icons\\Ability_Rogue_Sprint.blp"),
                          assetPathHash("Interface\\Icons\\INV_Sword_04.blp")};
constexpr u64 kPetIcons[] = {assetPathHash("Interface\\Icons\\Ability_Physical_Taunt.blp"),
                             assetPathHash("Interface\\Icons\\Ability_Druid_Rake.blp"),
                             assetPathHash("Interface\\Icons\\Ability_Racial_Cannibalize.blp")};
constexpr u64 kQuickslot = assetPathHash("Interface\\Buttons\\UI-Quickslot2.blp");
constexpr u64 kQuickslotEmpty = assetPathHash("Interface\\Buttons\\UI-Quickslot.blp");
constexpr u64 kAutocastOverlay = assetPathHash("Interface\\Buttons\\UI-AutoCastableOverlay.blp");
constexpr f32 kProgress[] = {0.0f, 0.1f, 0.2f, 0.25f, 0.4f, 0.5f, 0.6f, 0.75f, 0.85f, 0.95f};
constexpr f32 kFlashMs[] = {0.0f, 100.0f, 200.0f, 333.0f, 500.0f, 650.0f, 800.0f, 950.0f};

ui::TextStyle labelStyle(f32 size) {
    ui::TextStyle s;
    s.size = size;
    s.color = ui::kGold;
    s.outline = 1.0f;
    s.align = ui::Align::Center;
    return s;
}

// An action button as HudActionBar draws one: the icon in UI-Quickslot2 (64 units around 36).
void actionButton(DialogContext& c, const Rect& r, u64 iconHash) {
    const f32 pad = r.w * (64.0f - 36.0f) / 36.0f * 0.5f;
    c.draw.sprite({r.x - pad, r.y - pad, r.w + 2.0f * pad, r.h + 2.0f * pad}, kQuickslot);
    if (const std::optional<u32> icon = c.resources.icon(iconHash)) {
        c.draw.icon(r, *icon);
    }
}

// The Cooldown model of an action button at a fixed moment of a sequence.
void cooldownAt(DialogContext& c, const Rect& r, u16 animation, f32 ms) {
    Models& models = c.draw.models();
    if (!models.ready()) {
        return;
    }
    const f32 unit = r.w / 36.0f;
    Models::Placement at;
    at.x = r.x;
    at.y = r.y + unit + r.h;
    at.scale = 1280.0f * 0.75f * unit;
    at.frameX = r.x;
    at.frameY = r.y + unit;
    at.frameW = r.w;
    at.frameH = r.h;
    models.drawModel(c.draw, Models::Id::Cooldown, models.sequence(Models::Id::Cooldown, animation), ms, 0.0f, at);
}

// The same over a round icon.
void cooldownRoundAt(DialogContext& c, f32 cx, f32 cy, f32 radius, u16 animation, f32 ms) {
    Models& models = c.draw.models();
    if (!models.ready()) {
        return;
    }
    Models::Placement at;
    at.x = cx - radius;
    at.y = cy + radius;
    at.scale = 1280.0f * 0.75f * (2.0f * radius / 36.0f);
    at.frameX = cx - radius;
    at.frameY = cy - radius;
    at.frameW = at.frameH = 2.0f * radius;
    at.round = radius;
    models.drawModel(c.draw, Models::Id::Cooldown, models.sequence(Models::Id::Cooldown, animation), ms, 0.0f, at);
}

} // namespace

bool UiModelsGallery::select(const char* name) {
    static constexpr const char* kNames[] = {"cooldowns", "autocast", "minimapping", "roundicons"};
    for (u32 i = 0; i < std::size(kNames); ++i) {
        if (std::strcmp(name, kNames[i]) == 0) {
            m_page = static_cast<Page>(i);
            return true;
        }
    }
    return false;
}

void UiModelsGallery::draw(DialogContext& c, u64 nowMs) {
    if (m_startMs == 0) {
        m_startMs = nowMs;
    }
    c.draw.solid(c.safeArea, rgba(40, 46, 52)); // a plain background: the models' light shows
    switch (m_page) {
    case Page::Cooldowns: drawCooldowns(c, nowMs); break;
    case Page::AutoCast: drawAutoCast(c, nowMs); break;
    case Page::MinimapPing: drawMinimapPing(c, nowMs); break;
    case Page::RoundIcons: drawRoundIcons(c, nowMs); break;
    }
}

void UiModelsGallery::drawCooldowns(DialogContext& c, u64 nowMs) {
    const Rect& safe = c.safeArea;
    const u32 columns = static_cast<u32>(std::size(kProgress));
    const f32 cell = std::min(safe.w / static_cast<f32>(columns + 1), safe.h / 7.4f);
    const f32 size = cell * 0.62f;
    const ui::TextStyle label = labelStyle(std::max(10.0f, cell * 0.16f));
    const f32 x0 = safe.x + cell * 0.5f;
    char text[32];
    // Row 1: the model's Stand at each point of the cooldown. Row 2: the look it replaced (a dark fill from the top and
    // the seconds of a 10 s cooldown).
    for (u32 i = 0; i < columns; ++i) {
        const f32 cx = x0 + cell * (static_cast<f32>(i) + 0.5f);
        const Rect r{cx - size * 0.5f, safe.y + cell * 0.45f, size, size};
        actionButton(c, r, kIcons[i % std::size(kIcons)]);
        cooldownAt(c, r, 0, kProgress[i] * 1000.0f);
        std::snprintf(text, sizeof(text), "%d%%", static_cast<int>(kProgress[i] * 100.0f + 0.5f));
        c.draw.text(cx, r.y - cell * 0.12f, text, label);
        const Rect old{r.x, r.y + cell, size, size};
        actionButton(c, old, kIcons[i % std::size(kIcons)]);
        const f32 left = 1.0f - kProgress[i];
        c.draw.solid({old.x, old.y, old.w, old.h * left}, rgba(0, 0, 0, 150));
        std::snprintf(text, sizeof(text), "%d", static_cast<int>(left * 10.0f + 0.5f));
        ui::TextStyle seconds = labelStyle(old.h * 0.42f);
        seconds.color = rgba(255, 255, 160);
        c.draw.text(old.centerX(), old.centerY() + seconds.size * 0.35f, text, seconds);
    }
    // Row 3: its Death (the flash at the end) at fixed moments; row 4: the round icon at the same points as row 1.
    for (u32 i = 0; i < std::size(kFlashMs); ++i) {
        const f32 cx = x0 + cell * (static_cast<f32>(i) + 0.5f);
        const Rect r{cx - size * 0.5f, safe.y + cell * 2.6f, size, size};
        actionButton(c, r, kIcons[i % std::size(kIcons)]);
        cooldownAt(c, r, 1, kFlashMs[i]);
        std::snprintf(text, sizeof(text), "+%d ms", static_cast<int>(kFlashMs[i]));
        c.draw.text(cx, r.bottom() + cell * 0.22f, text, label);
    }
    for (u32 i = 0; i < columns; ++i) {
        const f32 cx = x0 + cell * (static_cast<f32>(i) + 0.5f), cy = safe.y + cell * 4.0f;
        const f32 radius = size * 0.6f;
        if (const std::optional<u32> icon = c.resources.icon(kIcons[i % std::size(kIcons)])) {
            c.draw.iconCircle({cx - radius, cy - radius, 2.0f * radius, 2.0f * radius}, *icon);
        }
        cooldownRoundAt(c, cx, cy, radius, 0, kProgress[i] * 1000.0f);
    }
    // Row 6: the flash over the round icon (clipped to its square: the Model frame's viewport).
    for (u32 i = 0; i < std::size(kFlashMs); ++i) {
        const f32 cx = x0 + cell * (static_cast<f32>(i) + 0.5f), cy = safe.y + cell * 6.7f;
        const f32 radius = size * 0.6f;
        if (const std::optional<u32> icon = c.resources.icon(kIcons[i % std::size(kIcons)])) {
            c.draw.iconCircle({cx - radius, cy - radius, 2.0f * radius, 2.0f * radius}, *icon);
        }
        cooldownRoundAt(c, cx, cy, radius, 1, kFlashMs[i]);
    }
    // Row 5: live, through UiModels::cooldown (a 4 s cooldown every 5 s: the sweep, then the flash), square and round,
    // with the seconds known and with only the share (the bags' and the spell book's).
    const f32 t = static_cast<f32>((nowMs - m_startMs) % 5000) / 1000.0f;
    const f32 left = t < 4.0f ? 4.0f - t : 0.0f;
    for (u32 i = 0; i < 4; ++i) {
        const f32 cx = x0 + cell * (static_cast<f32>(i) * 1.5f + 0.5f), cy = safe.y + cell * 5.4f;
        const ui::WidgetId id = ui::widgetId("gallery.cooldown", i);
        if (i < 2) {
            const Rect r{cx - size * 0.5f, cy - size * 0.5f, size, size};
            actionButton(c, r, kIcons[i]);
            const f32 unit = size / 36.0f;
            c.draw.models().cooldown(c.draw, id, {r.x, r.y + unit, r.w, r.h}, unit, left / 4.0f, i == 0 ? left : -1.0f,
                                     nowMs);
        } else {
            const f32 radius = size * 0.6f;
            if (const std::optional<u32> icon = c.resources.icon(kIcons[i])) {
                c.draw.iconCircle({cx - radius, cy - radius, 2.0f * radius, 2.0f * radius}, *icon);
            }
            c.draw.models().cooldownRound(c.draw, id, cx, cy, radius, left / 4.0f, i == 2 ? left : -1.0f, nowMs);
        }
    }
}

void UiModelsGallery::drawAutoCast(DialogContext& c, u64 nowMs) {
    const Rect& safe = c.safeArea;
    static const u32 limit = [] {
        const char* v = VD_DEV_ENV("VD_UIMODELS_PARTICLES");
        return v ? static_cast<u32>(std::max(0, std::atoi(v))) : 0u;
    }();
    // Three sizes of the pet bar's button (30 units, AutoCastable 58 x 58, the model at 1.2 over the button) and of
    // the spell book's (36 units, AutoCastable 60 x 60, the model 36 x 36 at (1, 1) at 1.22); every particle on the
    // left half, at most `limit` per emitter on the right (none set: the HUD's).
    const f32 sizes[] = {safe.h * 0.07f, safe.h * 0.11f, safe.h * 0.17f};
    for (u32 half = 0; half < 2; ++half) {
        c.draw.models().setParticleLimit(half == 0 ? 0u : (limit > 0 ? limit : Models::kParticleLimit));
        f32 x = safe.x + safe.w * (half == 0 ? 0.04f : 0.54f);
        for (u32 s = 0; s < 3; ++s) {
            const f32 size = sizes[s];
            // Pet bar button.
            {
                const f32 unit = size / 30.0f;
                const Rect r{x, safe.y + safe.h * 0.1f, size, size};
                if (const std::optional<u32> icon = c.resources.icon(kPetIcons[s % std::size(kPetIcons)])) {
                    c.draw.icon(r, *icon);
                }
                c.draw.sprite(r.inset(-14.0f * unit), kAutocastOverlay);
                c.draw.models().autocast(c.draw, ui::widgetId("gallery.autocast.pet", half * 8 + s), r, unit, 1.2f,
                                         nowMs);
            }
            // Spell book button.
            {
                const f32 unit = size / 36.0f;
                const Rect r{x, safe.y + safe.h * 0.55f, size, size};
                if (const std::optional<u32> icon = c.resources.icon(kPetIcons[(s + 1) % std::size(kPetIcons)])) {
                    c.draw.icon(r, *icon);
                }
                c.draw.sprite(r.inset(-12.0f * unit), kAutocastOverlay);
                c.draw.models().autocast(c.draw, ui::widgetId("gallery.autocast.book", half * 8 + s),
                                         {r.x + unit, r.y - unit, r.w, r.h}, unit, 1.22f, nowMs);
            }
            x += size * 1.5f;
        }
    }
    c.draw.models().setParticleLimit(Models::kParticleLimit);
}

void UiModelsGallery::drawMinimapPing(DialogContext& c, u64 nowMs) {
    const Rect& safe = c.safeArea;
    // Moments of the Birth (0..1333 ms) and the Stand (each 833 ms from 1333 on), then a live one every 6 s.
    static constexpr f32 kAges[] = {0.0f, 200.0f, 450.0f, 700.0f, 1000.0f, 1300.0f, 1500.0f, 1750.0f, 2000.0f, 2150.0f};
    const u32 count = static_cast<u32>(std::size(kAges));
    const f32 cell = std::min(safe.w / static_cast<f32>(count / 2 + 1), safe.h / 2.6f);
    const f32 unit = cell / 60.0f; // a 50-unit ping frame in each cell
    const ui::TextStyle label = labelStyle(std::max(10.0f, cell * 0.12f));
    char text[32];
    for (u32 i = 0; i <= count; ++i) {
        const u32 row = i / (count / 2), column = i % (count / 2);
        const f32 cx = safe.x + cell * (static_cast<f32>(column) + 0.75f);
        const f32 cy = safe.y + cell * (static_cast<f32>(row) * 1.15f + 0.6f);
        c.draw.ring(cx, cy, cell * 0.48f, 0.0f, rgba(16, 24, 20)); // a dark map under it
        const f32 age = i < count ? kAges[i] : static_cast<f32>((nowMs - m_startMs) % 6000);
        // The ping's frame centered on the point (the cross); the model draws its centre up and right of it.
        c.draw.solid({cx - unit * 4.0f, cy - 0.5f, unit * 8.0f, 1.0f}, rgba(255, 255, 255, 120));
        c.draw.solid({cx - 0.5f, cy - unit * 4.0f, 1.0f, unit * 8.0f}, rgba(255, 255, 255, 120));
        c.draw.models().minimapPing(c.draw, cx, cy, unit, age);
        std::snprintf(text, sizeof(text), i < count ? "%d ms" : "live %d", static_cast<int>(age));
        c.draw.text(cx, cy + cell * 0.6f, text, label);
    }
}

void UiModelsGallery::drawRoundIcons(DialogContext& c, u64 nowMs) {
    const Rect& safe = c.safeArea;
    primeRoundIconTextures(c.resources);
    const f32 radius = std::min(safe.w / 25.0f, safe.h / 8.0f);
    // normal, pushed, checked, flash (auto attack), equipped, unusable, out of range, short of power, selected (edit),
    // the empty edit place.
    for (u32 i = 0; i < 9; ++i) {
        HudActionSlot slot;
        slot.iconHash = kIcons[i % std::size(kIcons)];
        RoundIconState state;
        switch (i) {
        case 1: break; // pressed: smaller (below)
        case 2: slot.checked = true; break;
        case 3: slot.flashing = true; break;
        case 4: slot.equipped = true; break;
        case 5: slot.usable = false; break;
        case 6: slot.inRange = false; break;
        case 7: slot.noMana = true; break;
        case 8: state.selected = true; break;
        default: break;
        }
        for (u32 big = 0; big < 2; ++big) {
            const f32 r = (big ? radius * 1.6f : radius) * (i == 1 ? kRoundIconPressed : 1.0f);
            const f32 cx = safe.x + radius * 1.4f + static_cast<f32>(i) * radius * 2.4f;
            const f32 cy = safe.y + (big ? radius * 4.5f : radius * 1.5f);
            drawRoundIcon(c.draw, c.resources, cx, cy, r, slot, state, nowMs);
        }
    }
    // The edit mode's empty place: UI-Quickslot's 36-unit middle in the circle (app/HudTouchEdit).
    if (const std::optional<u32> empty = c.resources.icon(kQuickslotEmpty)) {
        for (u32 big = 0; big < 2; ++big) {
            const f32 r = big ? radius * 1.6f : radius;
            const f32 cx = safe.x + radius * 1.4f + 9.0f * radius * 2.4f;
            const f32 cy = safe.y + (big ? radius * 4.5f : radius * 1.5f);
            c.draw.iconCircle({cx - r, cy - r, 2.0f * r, 2.0f * r}, *empty, ui::kWhite, (64.0f - 36.0f) * 0.5f / 64.0f);
        }
    }
}

} // namespace vd
