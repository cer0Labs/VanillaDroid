#include "app/AuraLayout.h"

#include "core/Text.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vd {

using ui::Rect;
using ui::rgba;

u32 debuffTypeColor(u32 dispel) {
    switch (dispel) {
    case 1: return rgba(51, 153, 255); // Magic (0.20, 0.60, 1.00)
    case 2: return rgba(153, 0, 255);  // Curse (0.60, 0.00, 1.00)
    case 3: return rgba(153, 102, 0);  // Disease (0.60, 0.40, 0)
    case 4: return rgba(0, 153, 0);    // Poison (0.00, 0.60, 0)
    default: return rgba(204, 0, 0);   // none (0.80, 0, 0)
    }
}

bool hasDebuffType(u32 dispel) {
    return dispel >= 1 && dispel <= 4;
}

f32 buffFlashAlpha(u64 nowMs) {
    // BuffFrame_OnUpdate alternates two 0.75 s states: rising to 1 ("on"), then falling back ("off").
    const u64 phase = nowMs % (2 * kBuffFlashMs);
    const f32 t = static_cast<f32>(phase < kBuffFlashMs ? phase : 2 * kBuffFlashMs - phase) /
                  static_cast<f32>(kBuffFlashMs);
    return t * (1.0f - kBuffMinAlpha) + kBuffMinAlpha;
}

f32 auraButtonAlpha(f32 remaining, u64 nowMs) {
    return remaining > 0.0f && remaining < kBuffWarningSeconds ? buffFlashAlpha(nowMs) : 1.0f;
}

std::string_view secondsToTimeAbbrev(std::span<char> out, f32 seconds) {
    if (seconds > 86400.0f) {
        return text::format(out, "DAY_ONELETTER_ABBR", static_cast<int>(std::ceil(seconds / 86400.0f)));
    }
    if (seconds > 3600.0f) {
        return text::format(out, "HOUR_ONELETTER_ABBR", static_cast<int>(std::ceil(seconds / 3600.0f)));
    }
    if (seconds > 60.0f) {
        return text::format(out, "MINUTE_ONELETTER_ABBR", static_cast<int>(std::ceil(seconds / 60.0f)));
    }
    return text::format(out, "SECOND_ONELETTER_ABBR",
                        static_cast<int>(std::max(0.0f, seconds))); // %d truncates
}

std::string_view auraTimeRemaining(std::span<char> out, f32 seconds) {
    struct Unit {
        f32 seconds;
        text::Key one, many;
    };
    static constexpr Unit kUnits[] = {
        {86400.0f, "SPELL_TIME_REMAINING_DAYS", "SPELL_TIME_REMAINING_DAYS_P1"},
        {3600.0f, "SPELL_TIME_REMAINING_HOURS", "SPELL_TIME_REMAINING_HOURS_P1"},
        {60.0f, "SPELL_TIME_REMAINING_MIN", "SPELL_TIME_REMAINING_MIN_P1"}};
    for (const Unit& u : kUnits) {
        if (seconds > u.seconds) {
            const int n = static_cast<int>(std::ceil(seconds / u.seconds));
            return text::format(out, n == 1 ? u.one : u.many, n);
        }
    }
    const int n = static_cast<int>(std::max(0.0f, seconds));
    return text::format(
        out, n == 1 ? text::Key("SPELL_TIME_REMAINING_SEC") : text::Key("SPELL_TIME_REMAINING_SEC_P1"), n);
}

u32 auraDurationColor(f32 remaining) {
    return remaining < kBuffDurationWarningSeconds ? rgba(255, 255, 255) : rgba(255, 209, 0);
}

namespace {

// A button of `size` units with its top-left at (x, y) units from the origin: its icon, and its hit rectangle
// grown by half the gaps around it (`gapX` / `gapY` units) and `below` more units under it.
AuraButtonPlace place(u32 aura, f32 originX, f32 originY, f32 x, f32 y, f32 size, f32 gapX, f32 gapY,
                      f32 below, f32 unit) {
    AuraButtonPlace p;
    p.aura = aura;
    p.icon = {originX + x * unit, originY + y * unit, size * unit, size * unit};
    p.hit = {p.icon.x - gapX * 0.5f * unit, p.icon.y - gapY * 0.5f * unit, (size + gapX) * unit,
             (size + gapY + below) * unit};
    return p;
}

} // namespace

u32 buffFrameLayout(std::span<const HudAura> auras, f32 right, f32 top, f32 unit,
                    std::span<AuraButtonPlace> out, bool durations) {
    constexpr f32 kSize = 30.0f, kStep = 35.0f; // BuffButtonTemplate 30 x 30, 5 apart
    const f32 row = durations ? 45.0f : 35.0f;  // BuffButton8: 15 under the first row with durations, 5 without
    constexpr f32 kEnchantWidth = 32.0f;        // TemporaryEnchantFrame:SetWidth(enchantIndex * 32)
    u32 enchants = 0;
    for (const HudAura& a : auras) {
        enchants += a.enchant && enchants < kMaxTempEnchants ? 1u : 0u;
    }
    // BuffFrame's TOPRIGHT: the enchantment frame's TOPLEFT - 5 with enchantments, its TOPRIGHT without.
    const f32 buffRight = enchants > 0 ? -(static_cast<f32>(enchants) * kEnchantWidth + 5.0f) : 0.0f;
    u32 count = 0, enchant = 0, buff = 0, debuff = 0;
    for (u32 i = 0; i < auras.size() && count < out.size(); ++i) {
        const HudAura& a = auras[i];
        f32 x = 0.0f, y = 0.0f;
        if (a.enchant) {
            if (enchant == kMaxTempEnchants) {
                continue;
            }
            x = -(static_cast<f32>(enchant++) * kStep) - kSize;
        } else if (a.harmful) {
            if (debuff == kMaxPlayerDebuffs) {
                continue;
            }
            x = -(static_cast<f32>(debuff++) * kStep) - kSize;
            y = 2.0f * row; // BuffButton16: TemporaryEnchantFrame's TOPRIGHT + (0, -90)
        } else {
            if (buff == kMaxPlayerBuffs) {
                continue;
            }
            if (buff < 8) {
                x = buffRight - static_cast<f32>(buff) * kStep - kSize;
            } else {
                x = -(static_cast<f32>(buff - 8) * kStep) - kSize; // BuffButton8: under TempEnchant1
                y = row;
            }
            ++buff;
        }
        out[count++] = place(i, right, top, x, y, kSize, kStep - kSize, kStep - kSize, row - kStep, unit);
    }
    return count;
}

u32 targetAuraLayout(std::span<const HudAura> auras, bool friendly, bool targetOfTarget, const Rect& frame,
                     f32 unit, std::span<AuraButtonPlace> out, f32& bottom) {
    // UnitBuff("target", 1..5) and UnitDebuff("target", 1..16): the first of each kind in slot order.
    std::array<u32, kMaxTargetBuffs> buffs{};
    std::array<u32, kMaxTargetDebuffs> debuffs{};
    u32 numBuffs = 0, numDebuffs = 0;
    for (u32 i = 0; i < auras.size(); ++i) {
        if (auras[i].enchant) {
            continue;
        }
        if (auras[i].harmful) {
            if (numDebuffs < kMaxTargetDebuffs) {
                debuffs[numDebuffs++] = i;
            }
        } else if (numBuffs < kMaxTargetBuffs) {
            buffs[numBuffs++] = i;
        }
    }
    constexpr f32 kGap = 3.0f, kRowGap = 2.0f;
    const u32 wrap = targetOfTarget ? 5u : 6u;
    const f32 size = (targetOfTarget && numBuffs == kMaxTargetBuffs) || numDebuffs >= wrap ? 17.0f : 21.0f;
    // Sizes (debuffs 1..6 resized, the others keep the template's 17) and top-left corners, in units from the
    // first row's corner; index 0 unused (FrameXML's 1-based names).
    std::array<f32, kMaxTargetDebuffs + 1> ds{}, dx{}, dy{};
    for (u32 i = 1; i <= kMaxTargetDebuffs; ++i) {
        ds[i] = i <= 6 ? size : 17.0f;
    }
    const f32 debuffTop = friendly ? size + kRowGap : 0.0f; // TargetFrameDebuff1 under TargetFrameBuff1
    dx[1] = 0.0f;
    dy[1] = debuffTop;
    for (u32 i = 2; i <= kMaxTargetDebuffs; ++i) {
        if (i == wrap + 1) { // TOPLEFT to Debuff1's BOTTOMLEFT
            dx[i] = dx[1];
            dy[i] = dy[1] + ds[1] + kRowGap;
        } else if (wrap == 5 && i == 11) { // TOPLEFT to Debuff6's BOTTOMLEFT
            dx[i] = dx[6];
            dy[i] = dy[6] + ds[6] + kRowGap;
        } else { // LEFT to the previous one's RIGHT (centred on it)
            dx[i] = dx[i - 1] + ds[i - 1] + kGap;
            dy[i] = dy[i - 1] + (ds[i - 1] - ds[i]) * 0.5f;
        }
    }
    f32 buffY = 0.0f;
    if (!friendly) {
        const u32 under = targetOfTarget ? (numDebuffs >= 10 ? 11u : 6u) : 7u;
        buffY = dy[under] + ds[under] + kRowGap;
    }
    const f32 originX = frame.x + 5.0f * unit, originY = frame.bottom() - 32.0f * unit;
    u32 count = 0;
    bottom = frame.bottom();
    const auto add = [&](const AuraButtonPlace& p) {
        if (count < out.size()) {
            out[count++] = p;
            bottom = std::max(bottom, p.icon.bottom());
        }
    };
    for (u32 b = 0; b < numBuffs; ++b) {
        add(place(buffs[b], originX, originY, static_cast<f32>(b) * (size + kGap), buffY, size, kGap, kRowGap,
                  0.0f, unit));
    }
    for (u32 d = 0; d < numDebuffs; ++d) {
        add(place(debuffs[d], originX, originY, dx[d + 1], dy[d + 1], ds[d + 1], kGap, kRowGap, 0.0f, unit));
    }
    return count;
}

} // namespace vd
