#include "game/PetData.h"

#include "core/Hash.h"

#include <algorithm>
#include <cstring>

namespace vd::game {
namespace {

// Field numbers of the 1.12.1 client tables (vMaNGOS DBCStructure.h / DBCfmt.h, checked against db.vdpk and
// the offsets WoW.exe reads: GetPetIcon 0x4BEB10 reads the family record's +0x44, GetStablePetInfo 0x4CB230
// its name at +0x20 and the loyalty record's name at +0x04, GetPetHappiness 0x4BE900 the personality record's
// +0x28..).
namespace family { // CreatureFamily.dbc (18 fields): id, scales, skill lines, diet, names (8 locales +
                   // flags), icon
constexpr u32 kFoodMask = 7, kName = 8, kIcon = 17, kFields = 18;
}
namespace loyalty { // PetLoyalty.dbc (10 fields): id, names
constexpr u32 kName = 1, kFields = 10;
}
namespace food { // ItemPetFood.dbc (10 fields): id, names
constexpr u32 kName = 1, kFields = 10;
}
namespace slot { // StableSlotPrices.dbc (2 fields): slot, price
constexpr u32 kPrice = 1, kFields = 2;
}
namespace personality { // PetPersonality.dbc (19 fields): id, names, 3 thresholds, 3 damage modifiers, 3
                        // rates
constexpr u32 kThreshold = 10, kDamage = 13, kFields = 19;
}
namespace chrClass { // ChrClasses.dbc (17 fields): id, .., petNameToken 4 (HasPetSpells 0x4B4410 reads
                     // +0x10), name,
                     // .., filename 14
constexpr u32 kPetNameToken = 4, kFileName = 14, kFields = 17;
}

} // namespace

bool PetData::init(const pack::PackReader& db) {
    bool ok = m_families.load(db, "CreatureFamily", family::kFields);
    ok &= m_loyalty.load(db, "PetLoyalty", loyalty::kFields);
    ok &= m_foods.load(db, "ItemPetFood", food::kFields);
    ok &= m_slotPrices.load(db, "StableSlotPrices", slot::kFields);
    ok &= m_personality.load(db, "PetPersonality", personality::kFields);
    ok &= m_classes.load(db, "ChrClasses", chrClass::kFields);
    return ok;
}

std::string_view PetData::familyName(u32 family) const {
    const u32 r = m_families.find(family);
    return r == DbcTable::kMissing ? std::string_view{} : m_families.view.getString(r, family::kName);
}

u32 PetData::familyFoodMask(u32 family) const {
    const u32 r = m_families.find(family);
    return r == DbcTable::kMissing ? 0u : m_families.view.getU32(r, family::kFoodMask);
}

u64 PetData::familyIconHash(u32 family) const {
    const u32 r = m_families.find(family);
    if (r == DbcTable::kMissing) {
        return 0;
    }
    const std::string_view path = m_families.view.getString(r, family::kIcon);
    if (path.empty()) {
        return 0;
    }
    // The table names the icon without its extension (as SetTexture takes it): the pack keys end in ".blp".
    u64 hash = kFnvOffset;
    for (const std::string_view part : {path, std::string_view(".blp")}) {
        for (const char c : part) {
            hash ^= static_cast<u8>(normalizeAssetPathChar(c));
            hash *= kFnvPrime;
        }
    }
    return hash;
}

std::string_view PetData::dietList(u32 foodMask, std::span<char> out) const {
    if (out.empty()) {
        return {};
    }
    usize used = 0;
    out[0] = '\0';
    const db::DbcView& v = m_foods.view;
    // GetPetFoodTypes walks ItemPetFood.dbc in record order: the foods whose bit (id - 1) the family's mask
    // has.
    for (u32 r = 0; r < v.recordCount() && !m_foods.empty(); ++r) {
        const u32 id = v.getU32(r, 0);
        if (id == 0 || id > 32 || (foodMask & (1u << (id - 1))) == 0) {
            continue;
        }
        const std::string_view name = v.getString(r, food::kName);
        for (const std::string_view part : {used > 0 ? std::string_view(", ") : std::string_view{}, name}) {
            const usize n = std::min(part.size(), out.size() - 1 - used);
            std::memcpy(out.data() + used, part.data(), n);
            used += n;
        }
    }
    out[used] = '\0';
    return {out.data(), used};
}

std::string_view PetData::loyaltyName(u32 level) const {
    const u32 r = m_loyalty.find(level);
    return r == DbcTable::kMissing ? std::string_view{} : m_loyalty.view.getString(r, loyalty::kName);
}

u32 PetData::stableSlotPrice(u32 slotNumber) const {
    const u32 r = m_slotPrices.find(slotNumber);
    return r == DbcTable::kMissing ? 0u : m_slotPrices.view.getU32(r, slot::kPrice);
}

PetHappiness PetData::happiness(u32 power5) const {
    PetHappiness h;
    if (m_personality.empty()) {
        return h;
    }
    // WoW.exe 0x4BE900: the record of the pet's personality (every 1.12 pet has the first one), then the
    // number of thresholds the happiness reaches (0 counts as "unhappy": the first threshold is 0), and that
    // level's damage modifier.
    const u32 r = m_personality.index.front().second;
    const db::DbcView& v = m_personality.view;
    u32 level = 0;
    while (level < 3 && static_cast<i32>(power5) >= v.getI32(r, personality::kThreshold + level)) {
        ++level;
    }
    h.level = level;
    if (level > 0) {
        h.damagePercent = v.getF32(r, personality::kDamage + level - 1) * 100.0f;
    }
    return h;
}

std::string_view PetData::petNameToken(u32 cls) const {
    const u32 r = m_classes.find(cls);
    return r == DbcTable::kMissing ? std::string_view{}
                                   : m_classes.view.getString(r, chrClass::kPetNameToken);
}

std::string_view PetData::classFileName(u32 cls) const {
    const u32 r = m_classes.find(cls);
    return r == DbcTable::kMissing ? std::string_view{} : m_classes.view.getString(r, chrClass::kFileName);
}

} // namespace vd::game
