#include "game/ItemData.h"

#include "core/Log.h"
#include "core/Text.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace vd::game {
namespace {

// Spell.dbc of the 1.12.1 client (see GameData.cpp: vMaNGOS' SpellEntry numbering minus 3 from EffectMechanic on).
namespace spell {
constexpr u32 kProcChance = 25, kProcCharges = 26, kDuration = 30, kStackAmount = 39, kDieSides = 64, kBaseDice = 67,
              kBasePoints = 76, kRadius = 88, kAmplitude = 94, kChainTarget = 100, kMiscValue = 106,
              kPointsPerCombo = 112, kName = 120, kDescription = 138;
constexpr u32 kFieldCount = 173;
} // namespace spell

// Other tables (DBCfmt.h): field numbers of the enUS columns and the values read.
constexpr u32 kCostColumns = 29;          // DurabilityCosts: item level, then 21 weapon + 8 armor subclasses
constexpr u32 kSetName = 1, kSetItems = 10, kSetSpells = 27, kSetThresholds = 35, kSetSkill = 43;
constexpr u32 kEnchantmentText = 13;      // SpellItemEnchantment: description[8] at 13..20
constexpr u32 kRandomEnchants = 2, kRandomSuffix = 7; // ItemRandomProperties
constexpr u32 kSkillName = 3;             // SkillLine: name[8] at 3..10
constexpr u32 kFactionName = 19;          // Faction: name[8] at 19..26
constexpr u32 kClassName = 5, kClassFields = 17;                    // ChrClasses: name[8] at 5..12
constexpr u32 kRaceLanguage = 8, kRaceName = 17, kRaceFields = 29; // ChrRaces: m_BaseLanguage, name[8] at 17..24
constexpr u32 kLanguageOrcish = 1;
// ItemSubClass (not in vMaNGOS; checked against the data in tests/game/test_inventory.cpp): class, subclass, ...,
// m_displayName[8] at 10..17, m_verboseName[8] at 19..26.
constexpr u32 kSubclassName = 10, kSubclassFields = 28;

bool load(const pack::PackReader& db, const char* name, u32 minFields, db::DbcView& view,
          std::unordered_map<u32, u32>& index) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    if (!asset) {
        VD_LOG_WARN("%s.dbc missing", name);
        return false;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed) {
        VD_LOG_WARN("%s.dbc: %s", name, parsed.error().message.c_str());
        return false;
    }
    if (parsed.value().fieldCount() < minFields) {
        VD_LOG_WARN("%s.dbc has %u fields, expected %u", name, parsed.value().fieldCount(), minFields);
        return false;
    }
    view = parsed.value();
    index.reserve(view.recordCount());
    for (u32 r = 0; r < view.recordCount(); ++r) {
        index.emplace(view.getU32(r, 0), r);
    }
    return true;
}

std::string_view lookupString(const db::DbcView& view, const std::unordered_map<u32, u32>& index, u32 id,
                              u32 field) {
    const auto found = index.find(id);
    return found == index.end() ? std::string_view{} : view.getString(found->second, field);
}

// A value as the client writes it in spell texts: whole, or with one decimal.
void formatNumber(char (&text)[32], f32 value) {
    const f32 rounded = std::round(value);
    if (std::abs(value - rounded) < 0.01f) {
        std::snprintf(text, sizeof(text), "%d", static_cast<i32>(rounded));
    } else {
        std::snprintf(text, sizeof(text), "%.1f", value);
    }
}

void appendNumber(std::string& out, f32 value) {
    char text[32];
    formatNumber(text, value);
    out += text;
}

} // namespace

bool ItemData::init(const pack::PackReader& db) {
    bool ok = load(db, "DurabilityCosts", 1 + kCostColumns, m_costs, m_costRecords);
    ok &= load(db, "DurabilityQuality", 2, m_qualities, m_qualityRecords);
    ok &= load(db, "BankBagSlotPrices", 2, m_bankSlots, m_bankSlotRecords);
    ok &= load(db, "ItemSet", kSetSkill + 2, m_sets, m_setRecords);
    ok &= load(db, "SpellItemEnchantment", kEnchantmentText + 9, m_enchantments, m_enchantmentRecords);
    ok &= load(db, "ItemRandomProperties", kRandomSuffix + 9, m_randomProperties, m_randomPropertyRecords);
    ok &= load(db, "SkillLine", kSkillName + 9, m_skills, m_skillRecords);
    ok &= load(db, "Faction", kFactionName + 9, m_factions, m_factionRecords);
    ok &= load(db, "Spell", spell::kFieldCount, m_spells, m_spellRecords);
    ok &= load(db, "SpellDuration", 2, m_durations, m_durationRecords);
    ok &= load(db, "SpellRadius", 2, m_radii, m_radiusRecords);
    ok &= load(db, "ChrClasses", kClassFields, m_classes, m_classRecords);
    ok &= load(db, "ChrRaces", kRaceFields, m_races, m_raceRecords);
    // ItemSubClass has no id column: indexed by class and subclass.
    std::unordered_map<u32, u32> byClass;
    if (load(db, "ItemSubClass", kSubclassFields, m_subclasses, byClass)) {
        m_subclassRecords.reserve(m_subclasses.recordCount());
        for (u32 r = 0; r < m_subclasses.recordCount(); ++r) {
            m_subclassRecords.emplace(m_subclasses.getU32(r, 0) << 8 | (m_subclasses.getU32(r, 1) & 0xFF), r);
        }
    } else {
        ok = false;
    }
    m_ready = ok;
    return ok;
}

u32 ItemData::repairCost(u32 itemClass, u32 subclass, u32 quality, u32 itemLevel, u32 lost) const {
    if (lost == 0) {
        return 0;
    }
    const auto cost = m_costRecords.find(itemLevel);
    const auto mod = m_qualityRecords.find((quality + 1) * 2);
    if (cost == m_costRecords.end() || mod == m_qualityRecords.end()) {
        return 0; // the server refuses too ("Wrong item lvl")
    }
    // ItemSubClassToDurabilityMultiplierId: weapons by subclass, armor 21 on; anything else column 0.
    u32 column = 0;
    if (itemClass == 2) {
        column = subclass;
    } else if (itemClass == 4) {
        column = subclass + 21;
    }
    if (column >= kCostColumns) {
        return 0;
    }
    const u32 multiplier = m_costs.getU32(cost->second, 1 + column);
    const f32 qualityMod = m_qualities.getF32(mod->second, 1);
    const u32 total = static_cast<u32>(static_cast<f32>(lost * multiplier) * qualityMod);
    return std::max<u32>(total, 1);
}

u32 ItemData::bankSlotPrice(u32 slot) const {
    const auto found = m_bankSlotRecords.find(slot);
    return found == m_bankSlotRecords.end() ? 0 : m_bankSlots.getU32(found->second, 1);
}

std::optional<ItemSetInfo> ItemData::itemSet(u32 id) const {
    const auto found = m_setRecords.find(id);
    if (found == m_setRecords.end()) {
        return std::nullopt;
    }
    const u32 r = found->second;
    ItemSetInfo set;
    set.id = id;
    set.name = m_sets.getString(r, kSetName);
    for (u32 i = 0; i < set.items.size(); ++i) {
        const u32 item = m_sets.getU32(r, kSetItems + i);
        if (item != 0) {
            set.items[set.itemCount++] = item;
        }
    }
    for (u32 i = 0; i < 8; ++i) {
        set.spells[i] = m_sets.getU32(r, kSetSpells + i);
        set.thresholds[i] = m_sets.getU32(r, kSetThresholds + i);
    }
    set.requiredSkill = m_sets.getU32(r, kSetSkill);
    set.requiredSkillRank = m_sets.getU32(r, kSetSkill + 1);
    return set;
}

std::string_view ItemData::enchantmentText(u32 id) const {
    return lookupString(m_enchantments, m_enchantmentRecords, id, kEnchantmentText);
}

std::string_view ItemData::randomSuffix(u32 id) const {
    return lookupString(m_randomProperties, m_randomPropertyRecords, id, kRandomSuffix);
}

std::array<u32, 3> ItemData::randomEnchantments(u32 id) const {
    std::array<u32, 3> out{};
    if (const auto found = m_randomPropertyRecords.find(id); found != m_randomPropertyRecords.end()) {
        for (u32 i = 0; i < 3; ++i) {
            out[i] = m_randomProperties.getU32(found->second, kRandomEnchants + i);
        }
    }
    return out;
}

std::string_view ItemData::skillName(u32 id) const {
    return lookupString(m_skills, m_skillRecords, id, kSkillName);
}

std::string_view ItemData::factionName(u32 id) const {
    return lookupString(m_factions, m_factionRecords, id, kFactionName);
}

std::string_view ItemData::className(u32 id) const {
    return lookupString(m_classes, m_classRecords, id, kClassName);
}

std::string_view ItemData::raceName(u32 id) const {
    return lookupString(m_races, m_raceRecords, id, kRaceName);
}

bool ItemData::hordeRace(u32 id) const {
    const auto found = m_raceRecords.find(id);
    return found != m_raceRecords.end() && m_races.getU32(found->second, kRaceLanguage) == kLanguageOrcish;
}

std::string_view ItemData::subclassName(u32 itemClass, u32 subclass) const {
    const auto found = m_subclassRecords.find(itemClass << 8 | (subclass & 0xFF));
    return found == m_subclassRecords.end() ? std::string_view{} : m_subclasses.getString(found->second, kSubclassName);
}

std::string_view ItemData::spellName(u32 spellId) const {
    return lookupString(m_spells, m_spellRecords, spellId, spell::kName);
}

bool ItemData::spellRecord(u32 spellId, u32& record) const {
    const auto found = m_spellRecords.find(spellId);
    if (found == m_spellRecords.end()) {
        return false;
    }
    record = found->second;
    return true;
}

// -------------------------------------------------------------------------------------------------------
// Spell descriptions.

bool ItemData::spellDescription(u32 spellId, std::string& out) const {
    out.clear();
    u32 r = 0;
    if (!spellRecord(spellId, r)) {
        return false;
    }
    const std::string_view text = m_spells.getString(r, spell::kDescription);
    if (text.empty()) {
        return false;
    }
    f32 lastNumber = 0.0f; // $l picks singular / plural by the number written before it
    for (usize i = 0; i < text.size();) {
        if (text[i] != '$') {
            out += text[i++];
            continue;
        }
        const usize start = i++;
        // Optional factor: $/1000;s1 divides, $*5;s1 multiplies.
        f32 factor = 1.0f;
        if (i < text.size() && (text[i] == '/' || text[i] == '*')) {
            const bool divide = text[i] == '/';
            const usize semicolon = text.find(';', i);
            if (semicolon == std::string_view::npos) {
                out += text.substr(start);
                break;
            }
            const f32 n = static_cast<f32>(std::atof(std::string(text.substr(i + 1, semicolon - i - 1)).c_str()));
            factor = divide ? (n != 0.0f ? 1.0f / n : 1.0f) : n;
            i = semicolon + 1;
        }
        // Optional spell reference: $12345s1.
        u32 other = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            other = other * 10 + static_cast<u32>(text[i] - '0');
            ++i;
        }
        if (i >= text.size()) {
            out += text.substr(start);
            break;
        }
        const char token = static_cast<char>(text[i] | 0x20); // tokens are case-insensitive
        ++i;
        if (token == 'l' || token == 'g') {
            // $lsingular:plural;  $gmale:female;
            const usize colon = text.find(':', i);
            const usize semicolon = text.find(';', i);
            if (colon == std::string_view::npos || semicolon == std::string_view::npos || colon > semicolon) {
                out += text.substr(start, i - start);
                continue;
            }
            const bool first = token == 'g' || std::abs(lastNumber - 1.0f) < 0.001f;
            out += first ? text.substr(i, colon - i) : text.substr(colon + 1, semicolon - colon - 1);
            i = semicolon + 1;
            continue;
        }
        u32 index = 0;
        if (i < text.size() && text[i] >= '1' && text[i] <= '3') {
            index = static_cast<u32>(text[i] - '1');
            ++i;
        }
        const usize before = out.size();
        appendToken(out, other != 0 ? other : spellId, token, index, factor);
        if (out.size() == before) {
            out += text.substr(start, i - start); // unknown: as written
        } else {
            lastNumber = static_cast<f32>(std::atof(out.c_str() + before));
        }
    }
    return true;
}

void ItemData::appendToken(std::string& out, u32 spellId, char token, u32 index, f32 factor) const {
    u32 r = 0;
    if (token == 'z') {
        out += text::get("VD_INV_SPELL_HOME"); // Hearthstone: the bind point's name is the server's
        return;
    }
    if (!spellRecord(spellId, r)) {
        return;
    }
    const auto durationMs = [&]() -> i32 {
        const auto found = m_durationRecords.find(m_spells.getU32(r, spell::kDuration));
        return found == m_durationRecords.end() ? 0 : m_durations.getI32(found->second, 1);
    };
    const i32 basePoints = m_spells.getI32(r, spell::kBasePoints + index);
    const i32 baseDice = m_spells.getI32(r, spell::kBaseDice + index);
    const i32 dieSides = m_spells.getI32(r, spell::kDieSides + index);
    const f32 minValue = static_cast<f32>(basePoints + baseDice);
    const f32 maxValue = static_cast<f32>(basePoints + baseDice * std::max(dieSides, 1));
    switch (token) {
    case 's': {
        const f32 lo = std::abs(minValue * factor), hi = std::abs(maxValue * factor);
        if (std::abs(hi - lo) < 0.5f) {
            appendNumber(out, lo);
            return;
        }
        // "%d to %d": the numbers go in as text (they may have a decimal).
        char low[32], high[32], spread[96];
        formatNumber(low, std::min(lo, hi));
        formatNumber(high, std::max(lo, hi));
        out += text::format(spread, "INT_SPELL_POINTS_SPREAD_TEMPLATE", std::string_view(low), std::string_view(high));
        return;
    }
    case 'm': appendNumber(out, std::abs(minValue * factor)); return;
    case 'o': {
        // Total over the duration. Auras without a period (food, drink: health per 5 seconds) count in 5 s steps:
        // Tough Hunk of Bread's 17 over 18 sec reads 61, as the client shows it.
        const u32 amplitude = m_spells.getU32(r, spell::kAmplitude + index);
        const i32 duration = durationMs();
        const f32 period = amplitude > 0 ? static_cast<f32>(amplitude) : 5000.0f;
        const f32 ticks = duration > 0 ? static_cast<f32>(duration) / period : 1.0f;
        appendNumber(out, std::floor(std::abs(minValue * ticks * factor)));
        return;
    }
    case 't': appendNumber(out, static_cast<f32>(m_spells.getU32(r, spell::kAmplitude + index)) / 1000.0f * factor);
        return;
    case 'a': {
        const auto found = m_radiusRecords.find(m_spells.getU32(r, spell::kRadius + index));
        if (found != m_radiusRecords.end()) {
            appendNumber(out, m_radii.getF32(found->second, 1) * factor);
        }
        return;
    }
    case 'x': appendNumber(out, static_cast<f32>(m_spells.getU32(r, spell::kChainTarget + index))); return;
    case 'q': appendNumber(out, static_cast<f32>(m_spells.getI32(r, spell::kMiscValue + index))); return;
    case 'b': appendNumber(out, m_spells.getF32(r, spell::kPointsPerCombo + index) * factor); return;
    case 'h': appendNumber(out, static_cast<f32>(m_spells.getU32(r, spell::kProcChance))); return;
    case 'n': appendNumber(out, static_cast<f32>(m_spells.getU32(r, spell::kProcCharges))); return;
    case 'u': appendNumber(out, static_cast<f32>(m_spells.getU32(r, spell::kStackAmount))); return;
    case 'd': {
        const i32 ms = durationMs();
        if (ms <= 0) {
            out += text::get("VD_INV_SPELL_UNTIL_CANCELLED");
            return;
        }
        char text[64];
        const i32 s = ms / 1000;
        if (s < 60) {
            out += text::format(text, "INT_SPELL_DURATION_SEC", s);
        } else if (s < 3600) {
            out += text::format(text, "INT_SPELL_DURATION_MIN", s / 60);
        } else {
            out += text::format(text, s < 7200 ? "INT_SPELL_DURATION_HOURS" : "INT_SPELL_DURATION_HOURS_P1", s / 3600);
        }
        return;
    }
    default: return;
    }
}

} // namespace vd::game
