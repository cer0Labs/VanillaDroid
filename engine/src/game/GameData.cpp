#include "game/GameData.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "pack/PackReader.h"

#include <initializer_list>
#include <string>
#include <string_view>

namespace vd::game {
namespace {

// Spell.dbc of the 1.12.1 client (173 fields). vMaNGOS' SpellEntry.h numbers its database columns, which add
// EffectBonusCoefficient (79-81) to the client's layout: from EffectMechanic on, the client fields are 3 lower
// (checked against the baked db.vdpk: Fireball's StartRecoveryCategory 133 is at 157, its name at 120).
namespace spell {
constexpr u32 kStances = 11, kRequiresSpellFocus = 15, kTotem = 40, kReagent = 42, kReagentCount = 50;
constexpr u32 kTargetCreatureType = 14; // phase 20 (classes)
constexpr u32 kSchool = 1, kCategory = 2, kAttributes = 6, kTargets = 13, kCastTime = 18, kRecovery = 19,
              kCategoryRecovery = 20, kDuration = 30, kPowerType = 31, kPowerCost = 32, kRange = 36, kEffect = 61,
              kImplicitTargetA = 82, kImplicitTargetB = 85, kEffectMisc = 106, kIcon = 117, kName = 120, kRank = 129,
              kPowerCostPercent = 156, kGcdCategory = 157, kGcdTime = 158;
constexpr u32 kFieldCount = 173;
// Phase 13 (spells): item and place targets, spell modifiers.
constexpr u32 kAttributesEx2 = 8, kAttributesEx3 = 9, kBaseLevel = 28, kEquippedItemClass = 58,
              kEquippedItemSubClassMask = 59, kEquippedItemInventoryTypeMask = 60, kRadius = 88, kAura = 91,
              kSpellFamily = 160, kFamilyFlags = 161; // SpellFamilyFlags: two words, low first
} // namespace spell
// Phase 13 (spells): SpellItemEnchantment.dbc (24 fields: m_name_lang at 13, m_flags at 23), ChrClasses.dbc (17:
// m_spellClassSet at 15).
constexpr u32 kEnchantmentName = 13, kEnchantmentFlags = 23, kEnchantmentFields = 24;
constexpr u32 kClassSpellFamily = 15, kClassFields = 17;
constexpr u32 kItemIconField = 5; // ItemDisplayInfo.dbc: inventory icon name
// Names (m_name_lang, the client's language first) by vMaNGOS' DBCfmt.h: SpellFocusObject "nxxxxxxxxx",
// SpellShapeshiftForm "nxssssssssxiix", AreaTable "niiiixxxxxissssssssxixxxi".
constexpr u32 kSpellFocusNameField = 1, kSpellFocusFields = 10;
constexpr u32 kShapeshiftNameField = 2, kShapeshiftFields = 14;
constexpr u32 kAreaNameField = 11, kAreaFields = 25;
constexpr u32 kServerMessageTextField = 1, kServerMessageFields = 10; // ServerMessages.dbc: id, text[8], flags
// Phase 20 (retouches): Exhaustion.dbc (15 fields): id, XP, the experience factor, outdoor and inn hours,
// m_name_lang (5..13), threshold. WoW.exe reads the factor at +0x08 and the name at +0x14 (0x637B3E,
// 0x637C39).
constexpr u32 kExhaustionFactor = 2, kExhaustionName = 5, kExhaustionFields = 15;

// assetPathHash of the concatenated parts, without building the string (spell() runs every frame for the action
// bar: no allocation).
u64 pathHash(std::initializer_list<std::string_view> parts) {
    u64 hash = kFnvOffset;
    for (const std::string_view part : parts) {
        for (const char c : part) {
            hash ^= static_cast<u8>(normalizeAssetPathChar(c));
            hash *= kFnvPrime;
        }
    }
    return hash;
}

bool load(const pack::PackReader& db, const char* name, db::DbcView& view, std::unordered_map<u32, u32>& index) {
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
    view = parsed.value();
    index.reserve(view.recordCount());
    for (u32 r = 0; r < view.recordCount(); ++r) {
        index.emplace(view.getU32(r, 0), r);
    }
    return true;
}

// A table read by linear search (rare lookups): checked against its 1.12.1 field count.
bool loadView(const pack::PackReader& db, const char* name, u32 fields, db::DbcView& view) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    Result<db::DbcView> result = asset ? db::DbcView::parse(asset->data, asset->size)
                                       : Result<db::DbcView>(Error{"missing"});
    if (!result || result.value().fieldCount() != fields) {
        VD_LOG_WARN("%s.dbc: %s", name, result ? "unexpected layout" : result.error().message.c_str());
        return false;
    }
    view = result.value();
    return true;
}

std::string_view nameById(const db::DbcView& view, u32 id, u32 field) {
    const u32 r = id != 0 && view.recordCount() > 0 ? view.findById(id) : view.recordCount();
    return r < view.recordCount() ? view.getString(r, field) : std::string_view{};
}

} // namespace

bool GameData::init(const pack::PackReader& db) {
    bool ok = load(db, "Spell", m_spells, m_spellRecords);
    ok &= load(db, "SpellIcon", m_icons, m_iconRecords);
    ok &= load(db, "SpellCastTimes", m_castTimes, m_castTimeRecords);
    ok &= load(db, "SpellRange", m_ranges, m_rangeRecords);
    ok &= load(db, "SpellDuration", m_durations, m_durationRecords);
    ok &= load(db, "ItemDisplayInfo", m_itemDisplays, m_itemDisplayRecords);
    ok &= load(db, "Lock", m_locks, m_lockRecords);
    ok &= loadView(db, "SpellFocusObject", kSpellFocusFields, m_spellFocus);
    ok &= loadView(db, "SpellShapeshiftForm", kShapeshiftFields, m_shapeshiftForms);
    ok &= loadView(db, "AreaTable", kAreaFields, m_areas);
    ok &= loadView(db, "ServerMessages", kServerMessageFields, m_serverMessages); // phase 13
    ok &= load(db, "SpellRadius", m_radii, m_radiusRecords); // phase 13 (spells)
    ok &= load(db, "SpellItemEnchantment", m_enchantments, m_enchantmentRecords);
    ok &= loadView(db, "ChrClasses", kClassFields, m_classes);
    ok &= loadView(db, "Exhaustion", kExhaustionFields, m_exhaustion); // phase 20 (retouches)
    if (m_enchantments.fieldCount() < kEnchantmentFields) {
        m_enchantmentRecords.clear();
    }
    if (m_spells.fieldCount() < spell::kFieldCount) {
        VD_LOG_WARN("Spell.dbc has %u fields, expected %u: spells unavailable", m_spells.fieldCount(),
                    spell::kFieldCount);
        m_spells = {};
        return false;
    }
    return ok;
}

std::optional<SpellInfo> GameData::spell(u32 id) const {
    const auto found = m_spellRecords.find(id);
    if (found == m_spellRecords.end() || m_spells.recordCount() == 0) {
        return std::nullopt;
    }
    const u32 r = found->second;
    SpellInfo info;
    info.id = id;
    info.name = m_spells.getString(r, spell::kName);
    info.rank = m_spells.getString(r, spell::kRank);
    info.school = m_spells.getU32(r, spell::kSchool);
    info.attributes = m_spells.getU32(r, spell::kAttributes);
    info.targets = m_spells.getU32(r, spell::kTargets);
    info.recoveryMs = m_spells.getU32(r, spell::kRecovery);
    info.categoryRecoveryMs = m_spells.getU32(r, spell::kCategoryRecovery);
    info.category = m_spells.getU32(r, spell::kCategory);
    info.powerType = m_spells.getU32(r, spell::kPowerType);
    info.powerCost = m_spells.getU32(r, spell::kPowerCost);
    info.powerCostPercent = m_spells.getU32(r, spell::kPowerCostPercent);
    info.gcdCategory = m_spells.getU32(r, spell::kGcdCategory);
    info.gcdMs = m_spells.getU32(r, spell::kGcdTime);
    for (u32 i = 0; i < 3; ++i) {
        info.effects[i] = m_spells.getU32(r, spell::kEffect + i);
        info.implicitTargetA[i] = m_spells.getU32(r, spell::kImplicitTargetA + i);
        info.implicitTargetB[i] = m_spells.getU32(r, spell::kImplicitTargetB + i);
        info.effectMisc[i] = m_spells.getI32(r, spell::kEffectMisc + i);
        info.effectAura[i] = m_spells.getU32(r, spell::kAura + i); // phase 13 (spells)
        if (const auto radius = m_radiusRecords.find(m_spells.getU32(r, spell::kRadius + i));
            radius != m_radiusRecords.end()) {
            info.effectRadius[i] = m_radii.getF32(radius->second, 1);
        }
    }
    // Phase 13 (spells).
    info.attributesEx2 = m_spells.getU32(r, spell::kAttributesEx2);
    info.attributesEx3 = m_spells.getU32(r, spell::kAttributesEx3);
    info.baseLevel = m_spells.getU32(r, spell::kBaseLevel);
    info.equippedItemClass = m_spells.getI32(r, spell::kEquippedItemClass);
    info.equippedItemSubClassMask = m_spells.getU32(r, spell::kEquippedItemSubClassMask);
    info.equippedItemInventoryTypeMask = m_spells.getU32(r, spell::kEquippedItemInventoryTypeMask);
    info.spellFamily = m_spells.getU32(r, spell::kSpellFamily);
    info.familyFlags = m_spells.getU32(r, spell::kFamilyFlags) |
                       static_cast<u64>(m_spells.getU32(r, spell::kFamilyFlags + 1)) << 32;
    if (const auto icon = m_iconRecords.find(m_spells.getU32(r, spell::kIcon)); icon != m_iconRecords.end()) {
        const std::string_view path = m_icons.getString(icon->second, 1);
        if (!path.empty()) {
            info.iconHash = pathHash({path, ".blp"});
        }
    }
    if (const auto cast = m_castTimeRecords.find(m_spells.getU32(r, spell::kCastTime)); cast != m_castTimeRecords.end()) {
        info.castTimeMs = static_cast<u32>(std::max(0, m_castTimes.getI32(cast->second, 1)));
    }
    if (const auto range = m_rangeRecords.find(m_spells.getU32(r, spell::kRange)); range != m_rangeRecords.end()) {
        info.minRange = m_ranges.getF32(range->second, 1);
        info.maxRange = m_ranges.getF32(range->second, 2);
    }
    if (const auto duration = m_durationRecords.find(m_spells.getU32(r, spell::kDuration));
        duration != m_durationRecords.end()) {
        const i32 ms = m_durations.getI32(duration->second, 1);
        info.durationMs = ms > 0 ? static_cast<u32>(ms) : 0;
    }
    return info;
}

u64 GameData::itemIconHash(u32 displayId) const {
    const auto found = m_itemDisplayRecords.find(displayId);
    if (found == m_itemDisplayRecords.end()) {
        return 0;
    }
    const std::string_view icon = m_itemDisplays.getString(found->second, kItemIconField);
    if (icon.empty()) {
        return 0;
    }
    return pathHash({"Interface\\Icons\\", icon, ".blp"});
}

u32 GameData::lockOpenType(u32 lockId) const {
    // Lock.dbc (1.12.1): ID, Type[8], Index[8], Skill[8], Action[8]. Type 2 = LOCK_KEY_SKILL: Index is the LockType.
    const auto found = m_lockRecords.find(lockId);
    if (found == m_lockRecords.end() || m_locks.fieldCount() < 17) {
        return 0;
    }
    for (u32 i = 0; i < 8; ++i) {
        if (m_locks.getU32(found->second, 1 + i) == 2) {
            return m_locks.getU32(found->second, 9 + i);
        }
    }
    return 0;
}

std::optional<SpellRequirements> GameData::requirements(u32 spellId) const {
    const auto found = m_spellRecords.find(spellId);
    if (found == m_spellRecords.end() || m_spells.recordCount() == 0) {
        return std::nullopt;
    }
    const u32 r = found->second;
    SpellRequirements req;
    req.stances = m_spells.getU32(r, spell::kStances);
    req.spellFocus = m_spells.getU32(r, spell::kRequiresSpellFocus);
    for (u32 i = 0; i < req.totems.size(); ++i) {
        req.totems[i] = m_spells.getU32(r, spell::kTotem + i);
    }
    for (u32 i = 0; i < req.reagents.size(); ++i) {
        const i32 reagent = m_spells.getI32(r, spell::kReagent + i);
        req.reagents[i] = reagent > 0 ? static_cast<u32>(reagent) : 0;
        req.reagentCounts[i] = m_spells.getU32(r, spell::kReagentCount + i);
    }
    req.targetCreatureType = m_spells.getU32(r, spell::kTargetCreatureType); // phase 20 (classes)
    return req;
}

std::string_view GameData::spellFocusName(u32 id) const {
    return nameById(m_spellFocus, id, kSpellFocusNameField);
}

std::string_view GameData::shapeshiftFormName(u32 id) const {
    return nameById(m_shapeshiftForms, id, kShapeshiftNameField);
}

std::string_view GameData::areaName(u32 id) const {
    return nameById(m_areas, id, kAreaNameField);
}

std::string_view GameData::serverMessage(u32 id) const {
    return nameById(m_serverMessages, id, kServerMessageTextField);
}

std::optional<EnchantmentInfo> GameData::enchantment(u32 id) const {
    const auto found = id != 0 ? m_enchantmentRecords.find(id) : m_enchantmentRecords.end();
    if (found == m_enchantmentRecords.end()) {
        return std::nullopt;
    }
    return EnchantmentInfo{m_enchantments.getString(found->second, kEnchantmentName),
                           m_enchantments.getU32(found->second, kEnchantmentFlags)};
}

u32 GameData::classSpellFamily(u32 cls) const {
    const u32 r = cls != 0 && m_classes.recordCount() > 0 ? m_classes.findById(cls) : m_classes.recordCount();
    return r < m_classes.recordCount() ? m_classes.getU32(r, kClassSpellFamily) : 0;
}

RestStateInfo GameData::restState(u32 id) const {
    const u32 r =
        id != 0 && m_exhaustion.recordCount() > 0 ? m_exhaustion.findById(id) : m_exhaustion.recordCount();
    if (r >= m_exhaustion.recordCount()) {
        return {};
    }
    return {m_exhaustion.getF32(r, kExhaustionFactor), m_exhaustion.getString(r, kExhaustionName)};
}

SpellTargetKind SpellInfo::targetKind() const {
    // SpellTarget values (Spells/SpellDefines.h).
    constexpr u32 kUnitEnemy = 6, kUnitFriend = 21, kGameObject = 23, kUnit = 25, kLocked = 26, kUnitParty = 35,
                  kFriendAndParty = 37, kChainHeal = 45, kCasterTargetPosition = 53, kUnitRaid = 57,
                  kRaidAndClass = 61;
    bool enemy = false, any = false, friendly = false, object = false;
    for (u32 i = 0; i < 3; ++i) {
        if (effects[i] == 0) {
            continue;
        }
        switch (implicitTargetA[i]) {
        case kUnitEnemy:
        case kCasterTargetPosition: enemy = true; break;
        case kUnit: any = true; break;
        case kUnitFriend:
        case kUnitParty:
        case kFriendAndParty:
        case kChainHeal:
        case kUnitRaid:
        case kRaidAndClass: friendly = true; break;
        case kGameObject:
        case kLocked: object = true; break;
        default: break;
        }
    }
    if (enemy) {
        return SpellTargetKind::Enemy;
    }
    if (any) {
        return SpellTargetKind::AnyUnit;
    }
    if (friendly) {
        return SpellTargetKind::Friend;
    }
    if (object) {
        return SpellTargetKind::GameObject;
    }
    return (targets & 0x40) ? SpellTargetKind::Location : SpellTargetKind::None; // TARGET_FLAG_DEST_LOCATION
}

} // namespace vd::game
