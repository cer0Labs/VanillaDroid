#include "game/SpellText.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "core/Text.h"
#include "game/CombatText.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace vd::game {
namespace {

// Spell.dbc of the 1.12.1 client (173 fields): vMaNGOS' SpellEntry.h numbers minus 3 from EffectMechanic on
// (see GameData.cpp).
namespace spell {
constexpr u32 kDispel = 4, kActiveIcon = 118; // parity (auras): SpellDispelType.dbc, SpellIcon.dbc
constexpr u32 kCastUi = 3; // phase 13 (professions): the craft list (castUI)
constexpr u32 kAttributes = 6, kAttributesEx = 7, kAttributesEx2 = 8, kStances = 11, kStancesNot = 12,
              kCasterAuraState = 16, kTargetAuraState = 17, kCastTime = 18, kRecovery = 19,
              kCategoryRecovery = 20, kProcChance = 25, kProcCharges = 26, kMaxLevel = 27, kBaseLevel = 28,
              kSpellLevel = 29, kDuration = 30, kPowerType = 31, kPowerCost = 32, kManaPerSecond = 34,
              kRange = 36, kStackAmount = 39, kEffect = 61, kDieSides = 64, kBaseDice = 67,
              kDicePerLevel = 70, kRealPointsPerLevel = 73, kBasePoints = 76, kRadius = 88, kAura = 91,
              kAmplitude = 94, kMultipleValue = 97, kChainTarget = 100, kMisc = 106, kTrigger = 109,
              kPointsPerCombo = 112, kName = 120, kRank = 129, kDescription = 138, kAuraText = 147,
              kPowerCostPercent = 156, kMaxTargetLevel = 159, kMaxTargets = 163, kDamageMultiplier = 167;
constexpr u32 kFieldCount = 173;
constexpr u32 kSchool = 1, kAttributesEx3 = 9, kSpellFamily = 160, kFamilyFlags = 161; // phase 13 (spells)
constexpr u32 kAuraInterruptFlags = 22; // phase 20 (smart)
constexpr u32 kMechanic = 5, kEffectMechanic = 79; // phase 20 (pets)
} // namespace spell

// A number as the tooltips print it: whole numbers plain, others with one decimal.
void formatNumber(char (&out)[32], f64 value) {
    const f64 rounded = std::round(value);
    if (std::fabs(value - rounded) < 0.05) {
        std::snprintf(out, sizeof(out), "%lld", static_cast<long long>(rounded));
    } else {
        std::snprintf(out, sizeof(out), "%.1f", value);
    }
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

// The factor of "$*10;" / "$/1000;" (digits, maybe a decimal point); 0 when it is not a number.
f64 parseFactor(std::string_view s) {
    f64 value = 0.0, fraction = 0.0;
    bool point = false;
    for (const char c : s) {
        if (c == '.' && !point) {
            point = true;
            fraction = 0.1;
        } else if (isDigit(c)) {
            if (point) {
                value += (c - '0') * fraction;
                fraction *= 0.1;
            } else {
                value = value * 10.0 + (c - '0');
            }
        } else {
            return 0.0;
        }
    }
    return value;
}

// "$lsingular:plural;" / "$gmale:female;": the chosen half of the choice that starts at `text[i]`. Advances
// `i` past the ';'. False (nothing consumed) when it is not a choice.
bool choice(std::string_view text, usize& i, bool second, std::string& out) {
    const usize colon = text.find(':', i);
    const usize end = text.find(';', i);
    if (colon == std::string_view::npos || end == std::string_view::npos || colon > end) {
        return false;
    }
    out.append(second ? text.substr(colon + 1, end - colon - 1) : text.substr(i, colon - i));
    i = end + 1;
    return true;
}

// Phase 13 (spells): the spell modifier ops WoW.exe applies to an effect's points (its description parser's $s, $m,
// $o): SPELLMOD_ALL_EFFECTS, then SPELLMOD_DOT for periodic damage or SPELLMOD_DAMAGE for the other damage effects,
// then the op of some auras (threat, speed, attack power, haste). Returns how many were written.
u32 pointOps(u32 effect, u32 aura, std::array<SpellModOp, 3>& out) {
    u32 count = 0;
    out[count++] = SpellModOp::AllEffects;
    bool damage = false;
    switch (effect) {
    case 2:   // SCHOOL_DAMAGE
    case 9:   // HEALTH_LEECH
    case 17:  // WEAPON_DAMAGE_NOSCHOOL
    case 31:  // WEAPON_PERCENT_DAMAGE
    case 58:  // WEAPON_DAMAGE
    case 121: // NORMALIZED_WEAPON_DMG
        damage = true;
        break;
    case 6:   // APPLY_AURA
    case 27:  // PERSISTENT_AREA_AURA
    case 35:  // APPLY_AREA_AURA_PARTY
    case 119: // APPLY_AREA_AURA_PET
    case 128: // area auras the client knows of
    case 129:
        // PERIODIC_DAMAGE, DAMAGE_SHIELD, PROC_TRIGGER_DAMAGE, PERIODIC_LEECH, PERIODIC_DAMAGE_PERCENT
        damage = aura == 3 || aura == 15 || aura == 43 || aura == 53 || aura == 89;
        break;
    default: break;
    }
    if (damage) {
        out[count++] = aura == 3 ? SpellModOp::Dot : SpellModOp::Damage;
    }
    switch (aura) {
    case 10: // MOD_THREAT and the other threat auras
    case 103:
    case 183: out[count++] = SpellModOp::Threat; break;
    case 31: // MOD_INCREASE_SPEED and the other speeds
    case 32:
    case 33:
    case 58:
    case 129:
    case 130:
    case 171:
    case 172: out[count++] = SpellModOp::Speed; break;
    case 65: out[count++] = SpellModOp::SpellBonusDamage; break;
    case 99: out[count++] = SpellModOp::AttackPower; break; // MOD_ATTACK_POWER
    case 138: out[count++] = SpellModOp::Haste; break;       // MOD_HASTE
    default: break;
    }
    return count;
}

} // namespace

bool SpellText::init(const pack::PackReader& db) {
    bool ok = m_spells.load(db, "Spell", spell::kFieldCount);
    ok &= m_durations.load(db, "SpellDuration", 2);
    ok &= m_radii.load(db, "SpellRadius", 2);
    ok &= m_ranges.load(db, "SpellRange", 3);
    ok &= m_castTimes.load(db, "SpellCastTimes", 2);
    ok &= m_icons.load(db, "SpellIcon", 2);
    ok &= m_dispelTypes.load(db, "SpellDispelType", 2); // parity (auras): id, name
    return ok;
}

u32 SpellText::record(u32 spellId) const {
    return m_spells.find(spellId);
}

u32 SpellText::durationMs(u32 r) const {
    const u32 d = m_durations.find(m_spells.view.getU32(r, spell::kDuration));
    if (d == kMissing) {
        return 0;
    }
    const i32 ms = m_durations.view.getI32(d, 1);
    return ms > 0 ? static_cast<u32>(ms) : 0;
}

bool SpellText::details(u32 spellId, SpellDetails& out) const {
    const u32 r = record(spellId);
    if (r == kMissing) {
        return false;
    }
    out = SpellDetails{};
    out.id = spellId;
    out.name = m_spells.view.getString(r, spell::kName);
    out.rank = m_spells.view.getString(r, spell::kRank);
    out.description = m_spells.view.getString(r, spell::kDescription);
    out.auraText = m_spells.view.getString(r, spell::kAuraText);
    out.attributes = m_spells.view.getU32(r, spell::kAttributes);
    out.castUi = m_spells.view.getU32(r, spell::kCastUi);
    out.attributesEx = m_spells.view.getU32(r, spell::kAttributesEx);
    out.attributesEx2 = m_spells.view.getU32(r, spell::kAttributesEx2);
    out.stances = m_spells.view.getU32(r, spell::kStances);
    out.stancesNot = m_spells.view.getU32(r, spell::kStancesNot);
    out.casterAuraState = m_spells.view.getU32(r, spell::kCasterAuraState);
    out.targetAuraState = m_spells.view.getU32(r, spell::kTargetAuraState);
    out.maxLevel = m_spells.view.getU32(r, spell::kMaxLevel);
    out.baseLevel = m_spells.view.getU32(r, spell::kBaseLevel);
    out.spellLevel = m_spells.view.getU32(r, spell::kSpellLevel);
    out.durationMs = durationMs(r);
    out.procChance = m_spells.view.getU32(r, spell::kProcChance);
    out.procCharges = m_spells.view.getU32(r, spell::kProcCharges);
    out.stackAmount = m_spells.view.getU32(r, spell::kStackAmount);
    out.maxTargets = m_spells.view.getU32(r, spell::kMaxTargets);
    out.maxTargetLevel = m_spells.view.getU32(r, spell::kMaxTargetLevel);
    out.manaPerSecond = m_spells.view.getU32(r, spell::kManaPerSecond);
    out.powerType = m_spells.view.getU32(r, spell::kPowerType);
    out.powerCost = m_spells.view.getU32(r, spell::kPowerCost);
    out.powerCostPercent = m_spells.view.getU32(r, spell::kPowerCostPercent);
    out.recoveryMs = m_spells.view.getU32(r, spell::kRecovery);
    out.categoryRecoveryMs = m_spells.view.getU32(r, spell::kCategoryRecovery);
    if (const u32 c = m_castTimes.find(m_spells.view.getU32(r, spell::kCastTime)); c != kMissing) {
        out.castTimeMs = static_cast<u32>(std::max(0, m_castTimes.view.getI32(c, 1)));
    }
    if (const u32 range = m_ranges.find(m_spells.view.getU32(r, spell::kRange)); range != kMissing) {
        out.minRange = m_ranges.view.getF32(range, 1);
        out.maxRange = m_ranges.view.getF32(range, 2);
    }
    for (u32 i = 0; i < 3; ++i) {
        SpellEffectData& e = out.effects[i];
        e.effect = m_spells.view.getU32(r, spell::kEffect + i);
        e.aura = m_spells.view.getU32(r, spell::kAura + i);
        e.basePoints = m_spells.view.getI32(r, spell::kBasePoints + i);
        e.dieSides = m_spells.view.getI32(r, spell::kDieSides + i);
        e.baseDice = m_spells.view.getU32(r, spell::kBaseDice + i);
        e.dicePerLevel = m_spells.view.getF32(r, spell::kDicePerLevel + i);
        e.realPointsPerLevel = m_spells.view.getF32(r, spell::kRealPointsPerLevel + i);
        e.amplitudeMs = m_spells.view.getU32(r, spell::kAmplitude + i);
        e.radiusIndex = m_spells.view.getU32(r, spell::kRadius + i);
        e.chainTargets = m_spells.view.getU32(r, spell::kChainTarget + i);
        e.multipleValue = m_spells.view.getF32(r, spell::kMultipleValue + i);
        e.miscValue = m_spells.view.getI32(r, spell::kMisc + i);
        e.triggerSpell = m_spells.view.getU32(r, spell::kTrigger + i);
        e.pointsPerComboPoint = m_spells.view.getF32(r, spell::kPointsPerCombo + i);
        e.damageMultiplier = m_spells.view.getF32(r, spell::kDamageMultiplier + i);
        e.mechanic = m_spells.view.getU32(r, spell::kEffectMechanic + i); // phase 20 (pets)
    }
    out.dispel = m_spells.view.getU32(r, spell::kDispel);
    out.activeIcon = m_spells.view.getU32(r, spell::kActiveIcon);
    out.school = m_spells.view.getU32(r, spell::kSchool); // phase 13 (spells)
    out.attributesEx3 = m_spells.view.getU32(r, spell::kAttributesEx3);
    out.spellFamily = m_spells.view.getU32(r, spell::kSpellFamily);
    out.familyFlags = m_spells.view.getU32(r, spell::kFamilyFlags) |
                      static_cast<u64>(m_spells.view.getU32(r, spell::kFamilyFlags + 1)) << 32;
    out.auraInterruptFlags = m_spells.view.getU32(r, spell::kAuraInterruptFlags); // phase 20 (smart)
    out.mechanic = m_spells.view.getU32(r, spell::kMechanic);                     // phase 20 (pets)
    return true;
}

u32 SpellText::dispelType(u32 spellId) const {
    const u32 r = record(spellId);
    return r == kMissing ? 0 : m_spells.view.getU32(r, spell::kDispel);
}

std::string_view SpellText::dispelName(u32 dispelType) const {
    const u32 r = dispelType == 0 ? kMissing : m_dispelTypes.find(dispelType);
    return r == kMissing ? std::string_view{} : m_dispelTypes.view.getString(r, 1);
}

std::string_view SpellText::name(u32 spellId) const {
    const u32 r = record(spellId);
    return r == kMissing ? std::string_view{} : m_spells.view.getString(r, spell::kName);
}

std::string_view SpellText::rank(u32 spellId) const {
    const u32 r = record(spellId);
    return r == kMissing ? std::string_view{} : m_spells.view.getString(r, spell::kRank);
}

u64 SpellText::iconHash(u32 spellIconId) const {
    const u32 r = m_icons.find(spellIconId);
    if (r == kMissing) {
        return 0;
    }
    const std::string_view path = m_icons.view.getString(r, 1);
    if (path.empty()) {
        return 0;
    }
    u64 hash = kFnvOffset;
    for (const char c : path) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(c));
        hash *= kFnvPrime;
    }
    for (const char c : std::string_view(".blp")) {
        hash ^= static_cast<u8>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

f32 SpellText::radius(u32 radiusIndex) const {
    const u32 r = m_radii.find(radiusIndex);
    return r == kMissing ? 0.0f : m_radii.view.getF32(r, 1);
}

bool SpellText::tokenValue(u32 spellId, char letter, u32 index, const SpellTextContext& context, f64& value,
                           f64& max) const {
    const u32 r = record(spellId);
    if (r == kMissing) {
        return false;
    }
    index = std::min<u32>(index, 2);
    // Phase 13 (spells): the player's spell modifiers, as WoW.exe's description parser applies them.
    const SpellMods* mods = context.mods;
    const SpellModTarget target{m_spells.view.getU32(r, spell::kSpellFamily),
                                m_spells.view.getU32(r, spell::kFamilyFlags) |
                                    static_cast<u64>(m_spells.view.getU32(r, spell::kFamilyFlags + 1)) << 32,
                                m_spells.view.getU32(r, spell::kAttributesEx3)};
    const auto modded = [&](SpellModOp op, f64 v) {
        return mods ? static_cast<f64>(mods->apply(target, op, static_cast<f32>(v))) : v;
    };
    const auto moddedInt = [&](SpellModOp op, f64 v) {
        return mods ? static_cast<f64>(mods->apply(target, op, static_cast<i32>(v))) : v;
    };
    // The effect's points at the caster's level, as SpellCaster::CalculateSpellEffectValue grows them.
    const auto points = [&](u32 i, f64& low, f64& high) {
        f64 level = 0.0;
        if (context.casterLevel != 0) {
            f64 caster = context.casterLevel;
            const u32 maxLevel = m_spells.view.getU32(r, spell::kMaxLevel);
            if (maxLevel > 0 && caster > maxLevel) {
                caster = maxLevel;
            } else if (caster < m_spells.view.getU32(r, spell::kBaseLevel)) {
                caster = m_spells.view.getU32(r, spell::kBaseLevel);
            }
            level = caster - m_spells.view.getU32(r, spell::kSpellLevel);
        }
        const f64 base = m_spells.view.getI32(r, spell::kBasePoints + i) +
                         std::trunc(level * m_spells.view.getF32(r, spell::kRealPointsPerLevel + i));
        const i32 random = m_spells.view.getI32(r, spell::kDieSides + i) +
                           static_cast<i32>(level * m_spells.view.getF32(r, spell::kDicePerLevel + i));
        if (random <= 1) {
            low = high = base + random;
        } else {
            low = base + static_cast<f64>(m_spells.view.getU32(r, spell::kBaseDice + i));
            high = base + random;
        }
        if (mods) {
            std::array<SpellModOp, 3> ops{};
            const u32 count = pointOps(m_spells.view.getU32(r, spell::kEffect + i),
                                       m_spells.view.getU32(r, spell::kAura + i), ops);
            for (u32 k = 0; k < count; ++k) {
                low = modded(ops[k], low);
                high = modded(ops[k], high);
            }
        }
        low = std::fabs(low); // slows and reductions are negative: the texts show their size
        high = std::fabs(high);
        if (high < low) {
            std::swap(low, high);
        }
    };
    max = -1.0;
    switch (letter) {
    case 's':
    case 'S': points(index, value, max); break;
    case 'm': {
        f64 high;
        points(index, value, high);
        break;
    }
    case 'M': {
        f64 low;
        points(index, low, value);
        break;
    }
    case 'o':
    case 'O': {
        f64 low, high;
        points(index, low, high);
        const u32 amplitude = m_spells.view.getU32(r, spell::kAmplitude + index);
        const f64 duration = moddedInt(SpellModOp::Duration, static_cast<f64>(durationMs(r)));
        const f64 ticks = amplitude > 0 ? std::floor(duration / amplitude) : 1.0;
        value = low * ticks;
        max = high * ticks;
        break;
    }
    case 't':
    case 'T':
        value = moddedInt(SpellModOp::ActivationTime, m_spells.view.getU32(r, spell::kAmplitude + index)) / 1000.0;
        break;
    case 'a':
    case 'A': value = modded(SpellModOp::Radius, radius(m_spells.view.getU32(r, spell::kRadius + index))); break;
    case 'r':
    case 'R':
        if (const u32 range = m_ranges.find(m_spells.view.getU32(r, spell::kRange)); range != kMissing) {
            value = modded(SpellModOp::Range, m_ranges.view.getF32(range, 2));
        } else {
            value = 0.0;
        }
        break;
    case 'h':
    case 'H': value = moddedInt(SpellModOp::ChanceOfSuccess, m_spells.view.getU32(r, spell::kProcChance)); break;
    case 'n':
    case 'N': value = moddedInt(SpellModOp::Charges, m_spells.view.getU32(r, spell::kProcCharges)); break;
    case 'u':
    case 'U': value = m_spells.view.getU32(r, spell::kStackAmount); break;
    case 'i':
    case 'I': value = m_spells.view.getU32(r, spell::kMaxTargets); break;
    case 'v':
    case 'V': value = m_spells.view.getU32(r, spell::kMaxTargetLevel); break;
    case 'x':
    case 'X':
        value = moddedInt(SpellModOp::JumpTargets, m_spells.view.getU32(r, spell::kChainTarget + index));
        break;
    case 'q':
    case 'Q': value = m_spells.view.getI32(r, spell::kMisc + index); break;
    case 'e':
    case 'E': value = modded(SpellModOp::MultipleValue, m_spells.view.getF32(r, spell::kMultipleValue + index)); break;
    case 'b':
    case 'B': value = m_spells.view.getF32(r, spell::kPointsPerCombo + index); break;
    case 'f':
    case 'F': value = m_spells.view.getF32(r, spell::kDamageMultiplier + index); break;
    default: return false;
    }
    if (max < 0.0) {
        max = value;
    }
    return true;
}

void SpellText::resolve(u32 spellId, std::string_view text, const SpellTextContext& context,
                        std::string& out) const {
    f64 lastNumber = 2.0; // $l picks its plural until a number was printed
    usize i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c != '$' || i + 1 >= text.size()) {
            out.push_back(c);
            ++i;
            continue;
        }
        usize j = i + 1;
        if (text[j] == '$') { // "$$": a dollar sign
            out.push_back('$');
            i = j + 1;
            continue;
        }
        // Scaling ("$*10;F1", "$/1000;s1") and another spell's id ("$6136s2"), in either order.
        f64 scale = 1.0;
        u32 otherSpell = 0;
        bool parsed = true;
        for (u32 part = 0; part < 2 && j < text.size(); ++part) {
            if ((text[j] == '*' || text[j] == '/') && j + 1 < text.size()) {
                const bool divide = text[j] == '/';
                const usize semicolon = text.find(';', j);
                if (semicolon == std::string_view::npos) {
                    parsed = false;
                    break;
                }
                const f64 factor = parseFactor(text.substr(j + 1, semicolon - j - 1));
                if (factor != 0.0) {
                    scale = divide ? scale / factor : scale * factor;
                }
                j = semicolon + 1;
            } else if (isDigit(text[j])) {
                while (j < text.size() && isDigit(text[j])) {
                    otherSpell = otherSpell * 10 + static_cast<u32>(text[j] - '0');
                    ++j;
                }
            }
        }
        if (!parsed || j >= text.size()) {
            out.push_back('$');
            ++i;
            continue;
        }
        const char letter = text[j];
        const u32 source = otherSpell != 0 ? otherSpell : spellId;
        ++j;
        if (letter == 'l' || letter == 'L') {
            if (choice(text, j, std::fabs(lastNumber - 1.0) > 0.001, out)) {
                i = j;
                continue;
            }
        } else if (letter == 'g' || letter == 'G') {
            if (choice(text, j, context.gender == 1, out)) {
                i = j;
                continue;
            }
        } else if (letter == 'z' || letter == 'Z') {
            out.append(vd::text::get("VD_COMBAT_SPELL_HOME")); // the hearthstone's place, not known here
            i = j;
            continue;
        } else if (letter == 'd' || letter == 'D') {
            const u32 r = record(source);
            u32 ms = r == kMissing ? 0 : durationMs(r);
            if (context.mods && ms > 0) { // phase 13 (spells): SPELLMOD_DURATION
                const SpellModTarget target{m_spells.view.getU32(r, spell::kSpellFamily),
                                            m_spells.view.getU32(r, spell::kFamilyFlags) |
                                                static_cast<u64>(m_spells.view.getU32(r, spell::kFamilyFlags + 1))
                                                    << 32,
                                            m_spells.view.getU32(r, spell::kAttributesEx3)};
                ms = static_cast<u32>(std::max(0, context.mods->apply(target, SpellModOp::Duration, static_cast<i32>(ms))));
            }
            char duration[32];
            formatSpellDuration(duration, sizeof(duration), ms);
            out.append(duration);
            i = j;
            continue;
        } else {
            u32 index = 0;
            if (j < text.size() && text[j] >= '1' && text[j] <= '3') {
                index = static_cast<u32>(text[j] - '1');
                ++j;
            }
            f64 value = 0.0, max = 0.0;
            if (tokenValue(source, letter, index, context, value, max)) {
                value *= scale;
                max *= scale;
                char low[32];
                formatNumber(low, value);
                if (max > value + 0.05) {
                    char high[32];
                    formatNumber(high, max);
                    vd::text::append(out, "INT_SPELL_POINTS_SPREAD_TEMPLATE", low, high); // "14 to 22"
                } else {
                    out.append(low);
                }
                lastNumber = value;
                i = j;
                continue;
            }
        }
        // Not a token this client knows: the text as it is.
        out.push_back('$');
        ++i;
    }
}

bool spellDescription(const SpellText& text, u32 spellId, std::string& out, const SpellTextContext& context) {
    out.clear();
    SpellDetails details;
    if (!text.details(spellId, details)) {
        return false;
    }
    text.resolve(spellId, details.description, context, out);
    return true;
}

bool auraDescription(const SpellText& text, u32 spellId, std::string& out, const SpellTextContext& context) {
    out.clear();
    SpellDetails details;
    if (!text.details(spellId, details)) {
        return false;
    }
    text.resolve(spellId, details.auraText, context, out);
    return true;
}

// -------------------------------------------------------------------------------------------------------
// Tooltip lines.

namespace {

// The cost texts of a power type: MANA_COST / MANA_COST_PER_TIME... (cost, per second, power name);
// happiness has none of the client's.
struct CostTexts {
    text::Key cost;
    text::Key perTime;
};
CostTexts costTexts(u32 powerType) {
    switch (powerType) {
    case 0: return {"MANA_COST", "MANA_COST_PER_TIME"};
    case 1: return {"RAGE_COST", "RAGE_COST_PER_TIME"};
    case 2: return {"FOCUS_COST", "FOCUS_COST_PER_TIME"};
    case 3: return {"ENERGY_COST", "ENERGY_COST_PER_TIME"};
    case 0xFFFFFFFEu: return {"HEALTH_COST", "HEALTH_COST_PER_TIME"}; // POWER_HEALTH (-2): Life Tap
    default: return {"VD_COMBAT_COST", "VD_COMBAT_COST_PER_TIME"};
    }
}

} // namespace

usize formatSpellCost(char* out, usize size, const SpellDetails& spell) {
    if (size == 0) {
        return 0;
    }
    out[0] = 0;
    if (spell.passive()) {
        return 0;
    }
    const std::span<char> line(out, size);
    const std::string_view power = powerName(spell.powerType);
    if (spell.powerCostPercent > 0) {
        return text::format(line, "VD_COMBAT_COST_PERCENT", spell.powerCostPercent, power).size();
    }
    if (spell.powerCost == 0 && spell.manaPerSecond == 0) {
        return 0;
    }
    if (spell.powerCost == 0) {
        return text::format(line, "VD_COMBAT_COST_PER_SEC", spell.manaPerSecond, power).size();
    }
    const u32 cost = spell.powerType == 1 ? spell.powerCost / 10 : spell.powerCost; // rage is kept in tenths
    const CostTexts texts = costTexts(spell.powerType);
    if (spell.manaPerSecond > 0) {
        return text::format(line, texts.perTime, cost, spell.manaPerSecond, power).size();
    }
    return text::format(line, texts.cost, cost, power).size();
}

usize formatSpellRange(char* out, usize size, const SpellDetails& spell) {
    if (size == 0) {
        return 0;
    }
    out[0] = 0;
    if (spell.passive() || spell.maxRange <= 0.0f) {
        return 0;
    }
    const std::span<char> line(out, size);
    if (spell.maxRange <= 5.5f && spell.minRange <= 0.0f) {
        return text::format(line, "VD_COMBAT_MELEE_RANGE").size();
    }
    char high[32], range[72];
    formatNumber(high, spell.maxRange);
    if (spell.minRange > 0.0f) {
        char low[32];
        formatNumber(low, spell.minRange);
        std::snprintf(range, sizeof(range), "%s-%s", low, high);
    } else {
        std::snprintf(range, sizeof(range), "%s", high);
    }
    return text::format(line, "SPELL_RANGE", range).size(); // "8-25 yd range"
}

usize formatSpellCastTime(char* out, usize size, const SpellDetails& spell) {
    if (size == 0) {
        return 0;
    }
    out[0] = 0;
    if (spell.passive()) {
        return 0;
    }
    const std::span<char> line(out, size);
    if (spell.channeled()) {
        return text::format(line, "SPELL_CAST_CHANNELED").size();
    }
    if (spell.castTimeMs == 0) {
        // "Instant cast" for spells that cost mana, "Instant" for abilities.
        const bool mana = spell.powerType == 0 && (spell.powerCost > 0 || spell.powerCostPercent > 0);
        return text::format(line, mana ? "SPELL_CAST_TIME_INSTANT" : "SPELL_CAST_TIME_INSTANT_NO_MANA")
            .size();
    }
    if (spell.castTimeMs >= 60000) {
        return text::format(line, "SPELL_CAST_TIME_MIN", spell.castTimeMs / 60000.0).size();
    }
    return text::format(line, "SPELL_CAST_TIME_SEC", spell.castTimeMs / 1000.0).size();
}

usize formatSpellCooldown(char* out, usize size, const SpellDetails& spell) {
    if (size == 0) {
        return 0;
    }
    out[0] = 0;
    const u32 ms = std::max(spell.recoveryMs, spell.categoryRecoveryMs);
    if (spell.passive() || ms < 1500) {
        return 0; // the global cooldown is not shown
    }
    const std::span<char> line(out, size);
    if (ms >= 60000) {
        return text::format(line, "SPELL_RECAST_TIME_MIN", ms / 60000.0).size();
    }
    return text::format(line, "SPELL_RECAST_TIME_SEC", ms / 1000.0).size();
}

usize formatSpellDuration(char* out, usize size, u32 ms) {
    if (size == 0) {
        return 0;
    }
    // The client's duration texts of the descriptions ($d): "30 sec", "10 min", "1 hour", "2 hrs", "1.50
    // sec".
    const std::span<char> line(out, size);
    if (ms >= 3600000 && ms % 3600000 == 0) {
        const u32 hours = ms / 3600000;
        return text::format(line, hours == 1 ? "INT_SPELL_DURATION_HOURS" : "INT_SPELL_DURATION_HOURS_P1",
                            hours)
            .size();
    }
    if (ms >= 60000 && ms % 60000 == 0) {
        return text::format(line, "INT_SPELL_DURATION_MIN", ms / 60000).size();
    }
    if (ms % 1000 == 0) {
        return text::format(line, "INT_SPELL_DURATION_SEC", ms / 1000).size();
    }
    return text::format(line, "SPELL_DURATION_SEC", ms / 1000.0).size();
}

} // namespace vd::game
