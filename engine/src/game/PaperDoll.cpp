#include "game/PaperDoll.h"

#include "core/Profile.h"
#include "core/Text.h"
#include "game/CharacterData.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace vd::game {
namespace {

namespace uf = net::uf;

// The Lua's colour codes (GREEN_FONT_COLOR_CODE / RED_FONT_COLOR_CODE / HIGHLIGHT_FONT_COLOR_CODE,
// FONT_COLOR_CODE_CLOSE, and SetDamage's own colorPos / colorNeg).
constexpr const char* kGreen = "|cff20ff20";
constexpr const char* kRed = "|cffff2020";
constexpr const char* kWhite = "|cffffffff";
constexpr const char* kClose = "|r";
constexpr f32 kAttackPowerPerDps = 14.0f; // ATTACK_POWER_MAGIC_NUMBER
constexpr u32 kItemClassWeapon = 2;
constexpr u32 kSubclassWand = 19;

i32 intField(const Entity& e, u16 index) {
    return static_cast<i32>(e.value(index));
}

f32 floatField(const Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// UnitResistance / UnitArmor (0x5EFCD0): the total, its positive and negative buff parts, and the base under
// them.
struct Resistance {
    i32 total = 0, base = 0, positive = 0, negative = 0;
};
Resistance resistanceOf(const Entity& player, u32 school) {
    Resistance r;
    r.total = intField(player, static_cast<u16>(uf::UNIT_FIELD_RESISTANCES + school));
    r.positive = intField(player, static_cast<u16>(uf::PLAYER_FIELD_RESISTANCEBUFFMODSPOSITIVE + school));
    r.negative = intField(player, static_cast<u16>(uf::PLAYER_FIELD_RESISTANCEBUFFMODSNEGATIVE + school));
    r.base = r.total - r.positive - r.negative;
    r.total = std::max(r.total, 0);
    return r;
}

// The first damage of a ranged weapon that deals any: its school picks UnitRangedDamage's bonuses (a wand's).
u32 rangedSchool(const net::ItemQueryInfo* item) {
    if (item) {
        for (const net::ItemQueryInfo::Damage& d : item->damages) {
            if (d.min > 0.0f) {
                return std::min<u32>(d.school, 6);
            }
        }
    }
    return 0;
}

bool weapon(const net::ItemQueryInfo* item) {
    return item && item->itemClass == kItemClassWeapon;
}

} // namespace

WeaponRating weaponRating(const Entity& player, u32 skill) {
    WeaponRating r;
    if (skill == 0) {
        return r;
    }
    // 0x5EA460: the skill's slot; its value plus the permanent bonus (when it has a value), its temporary
    // bonus.
    for (u32 i = 0; i < 128; ++i) {
        const u16 base = static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + i * 3);
        if ((player.value(base) & 0xFFFFu) != skill) {
            continue;
        }
        const u32 values = player.value(static_cast<u16>(base + 1));
        const u32 bonuses = player.value(static_cast<u16>(base + 2));
        const i32 value = static_cast<i32>(values & 0xFFFFu);
        r.base = value > 0 ? value + static_cast<i32>(bonuses >> 16) : 0;
        r.modifier = static_cast<i16>(bonuses & 0xFFFFu);
        break;
    }
    if (r.base + r.modifier < 0) {
        r.modifier = -r.base; // UnitAttackBothHands keeps the rating at 0 or more
    }
    return r;
}

PaperDoll::PaperDoll() {
    m_lines.resize(kPaperDollStats);
    m_arena.resize(kArenaSize);
}

std::string_view PaperDoll::keep(std::string_view s) {
    if (m_used + s.size() + 1 > m_arena.size()) {
        return {};
    }
    char* at = m_arena.data() + m_used;
    std::memcpy(at, s.data(), s.size());
    at[s.size()] = '\0';
    m_used += static_cast<u32>(s.size()) + 1;
    return {at, s.size()};
}

std::string_view PaperDoll::print(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return n > 0 ? keep({buffer, std::min<usize>(static_cast<usize>(n), sizeof(buffer) - 1)})
                 : std::string_view{};
}

TooltipLine& PaperDoll::add(PaperDollLine& line, std::string_view left, u32 color, bool wrap) {
    TooltipLine& t =
        line.tooltip[std::min<u32>(line.tooltipLines, static_cast<u32>(line.tooltip.size()) - 1)];
    line.tooltipLines = std::min<u32>(line.tooltipLines + 1, static_cast<u32>(line.tooltip.size()));
    t = TooltipLine{};
    t.left = left;
    t.leftColor = color;
    t.wrap = wrap;
    return t;
}

// PaperDollFormatStat: the value (green with buffs, red with any debuff) and the tooltip's first line
// "<name> <effective> (<base>+<pos> <neg>)".
void PaperDoll::formatStat(PaperDollLine& line, std::string_view name, i32 base, i32 posBuff, i32 negBuff) {
    const i32 effective = std::max(0, base + posBuff + negBuff);
    const int n = static_cast<int>(name.size());
    if (posBuff == 0 && negBuff == 0) {
        line.value = print("%d", effective);
        add(line, print("%s%.*s %d%s", kWhite, n, name.data(), effective, kClose), kTooltipWhite);
        return;
    }
    char out[256];
    int length = std::snprintf(out, sizeof(out), "%s%.*s %d", kWhite, n, name.data(), effective);
    const auto append = [&](const char* format, auto... args) {
        if (length >= 0 && length < static_cast<int>(sizeof(out))) {
            length += std::snprintf(out + length, sizeof(out) - static_cast<usize>(length), format, args...);
        }
    };
    if (posBuff > 0 || negBuff < 0) {
        append(" (%d%s", base, kClose);
    }
    if (posBuff > 0) {
        append("%s%s+%d%s", kClose, kGreen, posBuff, kClose);
    }
    if (negBuff < 0) {
        append("%s %d%s", kRed, negBuff, kClose);
    }
    if (posBuff > 0 || negBuff < 0) {
        append("%s)%s", kWhite, kClose);
    }
    add(line, keep({out, std::min<usize>(static_cast<usize>(std::max(length, 0)), sizeof(out) - 1)}),
        kTooltipWhite);
    line.value = print("%s%d%s", negBuff < 0 ? kRed : kGreen, effective, kClose);
}

// PaperDollFrame_SetDamage / SetRangedDamage and their OnEnter: the value, and (`tooltip`) the weapon's block
// of speed, damage and damage per second.
void PaperDoll::damage(PaperDollLine& line, std::string_view title, f32 speed, f32 minDamage, f32 maxDamage,
                       i32 bonusPos, i32 bonusNeg, f32 percent, bool tooltip) {
    if (percent <= 0.0f) {
        percent = 1.0f; // not sent yet
    }
    const i32 displayMin = std::max(static_cast<i32>(std::floor(minDamage)), 1);
    const i32 displayMax = std::max(static_cast<i32>(std::ceil(maxDamage)), 1);
    const f32 baseMin = minDamage / percent - static_cast<f32>(bonusPos) - static_cast<f32>(bonusNeg);
    const f32 baseMax = maxDamage / percent - static_cast<f32>(bonusPos) - static_cast<f32>(bonusNeg);
    const f32 baseDamage = (baseMin + baseMax) * 0.5f;
    const f32 fullDamage = (baseDamage + static_cast<f32>(bonusPos) + static_cast<f32>(bonusNeg)) * percent;
    const f32 totalBonus = fullDamage - baseDamage;
    const f32 dps = speed > 0.0f ? std::max(fullDamage, 1.0f) / speed : 0.0f;
    const bool small = displayMin < 100 && displayMax < 100;
    if (line.value.empty()) {
        if (totalBonus == 0.0f) {
            line.value = print(small ? "%d - %d" : "%d-%d", displayMin, displayMax);
        } else {
            line.value = print(small ? "%s%d - %d|r" : "%s%d-%d|r", totalBonus > 0.0f ? kGreen : kRed,
                               displayMin, displayMax);
        }
    }
    if (!tooltip) {
        return;
    }
    char out[192];
    int length =
        std::snprintf(out, sizeof(out), "%d - %d", std::max(static_cast<i32>(std::floor(baseMin)), 1),
                      std::max(static_cast<i32>(std::ceil(baseMax)), 1));
    const auto append = [&](const char* format, auto... args) {
        if (length >= 0 && length < static_cast<int>(sizeof(out))) {
            length += std::snprintf(out + length, sizeof(out) - static_cast<usize>(length), format, args...);
        }
    };
    if (totalBonus != 0.0f) {
        if (bonusPos > 0) {
            append("%s +%d|r", kGreen, bonusPos);
        }
        if (bonusNeg < 0) {
            append("%s %d|r", kRed, bonusNeg);
        }
        const i32 shown = static_cast<i32>(std::floor(percent * 100.0f + 0.5f));
        if (percent > 1.0f) {
            append("%s x%d%%|r", kGreen, shown);
        } else if (percent < 1.0f) {
            append("%s x%d%%|r", kRed, shown);
        }
    }
    TooltipLine& head = add(line, title, kTooltipWhite);
    head.gapBefore = line.tooltipLines > 1; // the off hand's block: AddLine("\n") before it
    TooltipLine& s = add(line, text::get("ATTACK_SPEED_COLON"), kTooltipGold);
    s.right = print("%.2f", speed);
    TooltipLine& d = add(line, text::get("DAMAGE_COLON"), kTooltipGold);
    d.right = keep({out, std::min<usize>(static_cast<usize>(std::max(length, 0)), sizeof(out) - 1)});
    TooltipLine& p = add(line, text::get("DAMAGE_PER_SECOND"), kTooltipGold);
    p.right = print("%.1f", dps);
}

void PaperDoll::build(const Entity& player, const PaperDollInput& in) {
    VD_PROFILE_ZONE("PaperDoll::build");
    m_used = 0;
    for (PaperDollLine& line : m_lines) {
        line.label = {};
        line.value = {};
        line.tooltipLines = 0;
    }
    const u32 level = player.value(uf::UNIT_FIELD_LEVEL);
    char label[96];

    // PaperDollFrame_SetStats and PaperDollStatTooltip.
    static constexpr const char* kStatTokens[5] = {"STRENGTH", "AGILITY", "STAMINA", "INTELLECT", "SPIRIT"};
    static constexpr text::Key kStatNames[5] = {"SPELL_STAT0_NAME", "SPELL_STAT1_NAME", "SPELL_STAT2_NAME",
                                                "SPELL_STAT3_NAME", "SPELL_STAT4_NAME"};
    for (u32 i = 0; i < 5; ++i) {
        PaperDollLine& line = m_lines[i];
        const std::string_view name = text::get(kStatNames[i]);
        line.label = keep(text::format(label, "VD_CHAR_STAT_LABEL", name));
        const i32 stat = intField(player, static_cast<u16>(uf::UNIT_FIELD_STAT0 + i));
        const i32 pos = intField(player, static_cast<u16>(uf::PLAYER_FIELD_POSSTAT0 + i));
        const i32 neg = intField(player, static_cast<u16>(uf::PLAYER_FIELD_NEGSTAT0 + i));
        const int n = static_cast<int>(name.size());
        if (pos == 0 && neg == 0) {
            line.value = print("%d", stat);
            add(line, print("%s%.*s %d%s", kWhite, n, name.data(), stat, kClose), kTooltipWhite);
        } else {
            char out[256];
            int length = std::snprintf(out, sizeof(out), "%s%.*s %d", kWhite, n, name.data(), stat);
            const auto append = [&](const char* format, auto... args) {
                if (length >= 0 && length < static_cast<int>(sizeof(out))) {
                    length += std::snprintf(out + length, sizeof(out) - static_cast<usize>(length), format,
                                            args...);
                }
            };
            if (pos > 0 || neg < 0) {
                append(" (%d%s", stat - pos - neg, kClose);
            }
            if (pos > 0) {
                append("%s%s+%d%s", kClose, kGreen, pos, kClose);
            }
            if (neg < 0) {
                append("%s %d%s", kRed, neg, kClose);
            }
            if (pos > 0 || neg < 0) {
                append("%s)%s", kWhite, kClose);
            }
            add(line, keep({out, std::min<usize>(static_cast<usize>(std::max(length, 0)), sizeof(out) - 1)}),
                kTooltipWhite);
            line.value = print("%s%d%s", neg < 0 ? kRed : kGreen, stat, kClose);
        }
        // The class's text, else the default one (getglobal returns nil for a missing key).
        char key[64];
        std::snprintf(key, sizeof(key), "%.*s_%s_TOOLTIP", static_cast<int>(in.classFile.size()),
                      in.classFile.data(), kStatTokens[i]);
        if (in.classFile.empty() || !text::has(text::Key(std::string_view(key)))) {
            std::snprintf(key, sizeof(key), "DEFAULT_%s_TOOLTIP", kStatTokens[i]);
        }
        add(line, text::get(text::Key(std::string_view(key))), kTooltipGold, true);
    }

    // PaperDollFrame_SetArmor.
    {
        PaperDollLine& line = m_lines[static_cast<u32>(PaperDollStat::Armor)];
        line.label = text::get("ARMOR_COLON");
        const Resistance armor = resistanceOf(player, 0);
        formatStat(line, text::get("ARMOR"), armor.base, armor.positive, armor.negative);
        const f32 attacker = static_cast<f32>(level);
        const f32 r = static_cast<f32>(armor.total) / (85.0f * attacker + 400.0f);
        char subtext[384];
        add(line, keep(text::format(subtext, "ARMOR_TOOLTIP", level, 100.0f * (r / (r + 1.0f)))),
            kTooltipGold, true);
    }

    // PaperDollFrame_SetAttackBothHands: the main hand's weapon skill (no weapon: a fist).
    {
        PaperDollLine& line = m_lines[static_cast<u32>(PaperDollStat::Attack)];
        line.label = text::get("MELEE_ATTACK");
        const u32 skill =
            in.mainHand ? weaponSkill(in.mainHand->itemClass, in.mainHand->subclass) : kSkillUnarmed;
        const WeaponRating rating = weaponRating(player, skill);
        line.value = rating.modifier == 0 ? print("%d", rating.base)
                                          : print("%s%d%s", rating.modifier > 0 ? kGreen : kRed,
                                                  rating.base + rating.modifier, kClose);
        add(line, text::get("ATTACK_TOOLTIP"), kTooltipWhite);
        add(line, text::get("ATTACK_TOOLTIP_SUBTEXT"), kTooltipGold, true);
    }

    // PaperDollFrame_SetAttackPower (UnitAttackPower: each part times 1 + the multiplier, rounded).
    {
        PaperDollLine& line = m_lines[static_cast<u32>(PaperDollStat::AttackPower)];
        line.label = text::get("ATTACK_POWER_COLON");
        const f32 scale = 1.0f + floatField(player, uf::UNIT_FIELD_ATTACK_POWER_MULTIPLIER);
        const u32 mods = player.value(uf::UNIT_FIELD_ATTACK_POWER_MODS);
        const i32 base = static_cast<i32>(
            std::lround(static_cast<f32>(intField(player, uf::UNIT_FIELD_ATTACK_POWER)) * scale));
        const i32 pos =
            static_cast<i32>(std::lround(static_cast<f32>(static_cast<i16>(mods & 0xFFFF)) * scale));
        const i32 neg = static_cast<i32>(std::lround(static_cast<f32>(static_cast<i16>(mods >> 16)) * scale));
        formatStat(line, text::get("MELEE_ATTACK_POWER"), base, pos, neg);
        char subtext[256];
        add(line,
            keep(text::format(subtext, "MELEE_ATTACK_POWER_TOOLTIP",
                              static_cast<f32>(std::max(base + pos + neg, 0)) / kAttackPowerPerDps)),
            kTooltipGold, true);
    }

    // PaperDollFrame_SetDamage and CharacterDamageFrame_OnEnter (the off hand's block with a weapon there).
    {
        PaperDollLine& line = m_lines[static_cast<u32>(PaperDollStat::Damage)];
        line.label = text::get("DAMAGE_COLON");
        const i32 pos = intField(player, uf::PLAYER_FIELD_MOD_DAMAGE_DONE_POS);
        const i32 neg = intField(player, uf::PLAYER_FIELD_MOD_DAMAGE_DONE_NEG);
        const f32 percent = floatField(player, uf::PLAYER_FIELD_MOD_DAMAGE_DONE_PCT);
        const f32 speed = static_cast<f32>(player.value(uf::UNIT_FIELD_BASEATTACKTIME)) * 0.001f;
        damage(line, text::get("INVTYPE_WEAPONMAINHAND"), speed, floatField(player, uf::UNIT_FIELD_MINDAMAGE),
               floatField(player, uf::UNIT_FIELD_MAXDAMAGE), pos, neg, percent, true);
        if (weapon(in.offHand)) {
            const f32 offSpeed =
                static_cast<f32>(player.value(static_cast<u16>(uf::UNIT_FIELD_BASEATTACKTIME + 1))) * 0.001f;
            damage(line, text::get("INVTYPE_WEAPONOFFHAND"), offSpeed,
                   floatField(player, uf::UNIT_FIELD_MINOFFHANDDAMAGE),
                   floatField(player, uf::UNIT_FIELD_MAXOFFHANDDAMAGE), pos, neg, percent, true);
        }
    }

    // PaperDollFrame_SetRangedAttack / SetRangedAttackPower / SetRangedDamage: N/A without a ranged weapon or
    // with a relic slot; a wand has no attack power ("--").
    const bool noRanged = !in.rangedEquipped || in.relicSlot;
    const std::string_view notApplicable = text::get("NOT_APPLICABLE");
    {
        PaperDollLine& line = m_lines[static_cast<u32>(PaperDollStat::RangedAttack)];
        line.label = text::get("RANGED_ATTACK");
        if (noRanged) {
            line.value = notApplicable;
        } else {
            const u32 skill = in.ranged ? weaponSkill(in.ranged->itemClass, in.ranged->subclass) : 0;
            const WeaponRating rating = weaponRating(player, skill);
            line.value = rating.modifier == 0 ? print("%d", rating.base)
                                              : print("%s%d%s", rating.modifier > 0 ? kGreen : kRed,
                                                      rating.base + rating.modifier, kClose);
        }
        add(line, text::get("RANGED_ATTACK_TOOLTIP"), kTooltipWhite);
        add(line, text::get("ATTACK_TOOLTIP_SUBTEXT"), kTooltipGold, true);
    }
    {
        PaperDollLine& line = m_lines[static_cast<u32>(PaperDollStat::RangedAttackPower)];
        line.label = text::get("ATTACK_POWER_COLON");
        const bool wand =
            in.ranged && in.ranged->itemClass == kItemClassWeapon && in.ranged->subclass == kSubclassWand;
        if (noRanged) {
            line.value = notApplicable;
        } else if (wand) {
            line.value = keep("--");
        } else {
            const f32 scale = 1.0f + floatField(player, uf::UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER);
            const u32 mods = player.value(uf::UNIT_FIELD_RANGED_ATTACK_POWER_MODS);
            const i32 base = static_cast<i32>(
                std::lround(static_cast<f32>(intField(player, uf::UNIT_FIELD_RANGED_ATTACK_POWER)) * scale));
            const i32 pos =
                static_cast<i32>(std::lround(static_cast<f32>(static_cast<i16>(mods & 0xFFFF)) * scale));
            const i32 neg =
                static_cast<i32>(std::lround(static_cast<f32>(static_cast<i16>(mods >> 16)) * scale));
            formatStat(line, text::get("RANGED_ATTACK_POWER"), base, pos, neg);
            char subtext[256];
            add(line,
                keep(text::format(subtext, "RANGED_ATTACK_POWER_TOOLTIP",
                                  static_cast<f32>(base) / kAttackPowerPerDps)),
                kTooltipGold, true); // the base alone, as 1.12 wrote it
        }
    }
    {
        PaperDollLine& line = m_lines[static_cast<u32>(PaperDollStat::RangedDamage)];
        line.label = text::get("DAMAGE_COLON");
        if (noRanged) {
            line.value = notApplicable;
        } else {
            const u32 school = rangedSchool(in.ranged);
            const i32 pos = intField(player, static_cast<u16>(uf::PLAYER_FIELD_MOD_DAMAGE_DONE_POS + school));
            const i32 neg = intField(player, static_cast<u16>(uf::PLAYER_FIELD_MOD_DAMAGE_DONE_NEG + school));
            const f32 percent =
                floatField(player, static_cast<u16>(uf::PLAYER_FIELD_MOD_DAMAGE_DONE_PCT + school));
            const f32 speed = static_cast<f32>(player.value(uf::UNIT_FIELD_RANGEDATTACKTIME)) * 0.001f;
            damage(line, text::get("INVTYPE_RANGED"), speed,
                   floatField(player, uf::UNIT_FIELD_MINRANGEDDAMAGE),
                   floatField(player, uf::UNIT_FIELD_MAXRANGEDDAMAGE), pos, neg, percent, true);
        }
    }

    // PaperDollFrame_SetResistances.
    static constexpr text::Key kResistanceNames[7] = {
        "RESISTANCE0_NAME", "RESISTANCE1_NAME", "RESISTANCE2_NAME", "RESISTANCE3_NAME",
        "RESISTANCE4_NAME", "RESISTANCE5_NAME", "RESISTANCE6_NAME"};
    static constexpr text::Key kResistanceTypes[7] = {
        "RESISTANCE_TYPE0", "RESISTANCE_TYPE1", "RESISTANCE_TYPE2", "RESISTANCE_TYPE3",
        "RESISTANCE_TYPE4", "RESISTANCE_TYPE5", "RESISTANCE_TYPE6"};
    for (u32 i = 0; i < kPaperDollResistances; ++i) {
        PaperDollLine& line = m_lines[static_cast<u32>(PaperDollStat::ArcaneResistance) + i];
        const u32 school = kPaperDollResistanceSchools[i];
        const Resistance r = resistanceOf(player, school);
        const i32 total = r.total; // 0x5EFCD0 shows a negative total as 0
        if (std::abs(r.negative) > r.positive) {
            line.value = print("%s%d%s", kRed, total, kClose);
        } else if (std::abs(r.negative) == r.positive) {
            line.value = print("%d", total);
        } else {
            line.value = print("%s%d%s", kGreen, total, kClose);
        }
        const std::string_view name = text::get(kResistanceNames[school]);
        char title[256];
        int length =
            std::snprintf(title, sizeof(title), "%.*s %d", static_cast<int>(name.size()), name.data(), total);
        if ((r.positive != 0 || r.negative != 0) && length > 0 && length < static_cast<int>(sizeof(title))) {
            length += std::snprintf(title + length, sizeof(title) - static_cast<usize>(length), " ( %s%d",
                                    kWhite, r.base);
            if (r.positive > 0 && length < static_cast<int>(sizeof(title))) {
                length += std::snprintf(title + length, sizeof(title) - static_cast<usize>(length), "%s +%d",
                                        kGreen, r.positive);
            }
            if (r.negative < 0 && length < static_cast<int>(sizeof(title))) {
                length += std::snprintf(title + length, sizeof(title) - static_cast<usize>(length), " %s%d",
                                        kRed, r.negative);
            }
            if (length < static_cast<int>(sizeof(title))) {
                length +=
                    std::snprintf(title + length, sizeof(title) - static_cast<usize>(length), "%s )", kClose);
            }
        }
        add(line, keep({title, std::min<usize>(static_cast<usize>(std::max(length, 0)), sizeof(title) - 1)}),
            kTooltipWhite);
        const u32 unitLevel = std::max<u32>(level, 20);
        const f32 perLevel = static_cast<f32>(total) / static_cast<f32>(unitLevel);
        const text::Key grade = perLevel > 5.0f    ? text::Key("RESISTANCE_EXCELLENT")
                                : perLevel > 3.75f ? text::Key("RESISTANCE_VERYGOOD")
                                : perLevel > 2.5f  ? text::Key("RESISTANCE_GOOD")
                                : perLevel > 1.25f ? text::Key("RESISTANCE_FAIR")
                                : perLevel > 0.0f  ? text::Key("RESISTANCE_POOR")
                                                   : text::Key("RESISTANCE_NONE");
        char subtext[384];
        add(line,
            keep(text::format(subtext, "RESISTANCE_TOOLTIP_SUBTEXT", text::get(kResistanceTypes[school]),
                              unitLevel, text::get(grade))),
            kTooltipGold, true);
    }
}

} // namespace vd::game
