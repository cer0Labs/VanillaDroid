#include "game/MinimapData.h"

#include "core/Log.h"
#include "db/DbcView.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <optional>

namespace vd::game {
namespace {

// Lock.dbc (1.12.1).
constexpr u32 kLockType = 1, kLockIndex = 9, kLockFieldCount = 33;
constexpr u32 kLockKeySkill = 2;
// Spell.dbc (1.12.1, 173 fields): Effect at 61..63 (6 = SPELL_EFFECT_APPLY_AURA), EffectApplyAuraName
// at 91..93.
constexpr u32 kSpellEffect = 61, kSpellAura = 91, kSpellFieldCount = 173;
constexpr u32 kEffectApplyAura = 6;
constexpr u32 kAuraTrackCreatures = 44, kAuraTrackResources = 45;

std::optional<db::DbcView> table(const pack::PackReader& db, const char* path, u32 minFields) {
    const std::optional<pack::PackAsset> asset = db.find(path);
    if (!asset) {
        VD_LOG_WARN("%s missing", path);
        return std::nullopt;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed || parsed.value().fieldCount() < minFields) {
        VD_LOG_WARN("%s: unexpected layout", path);
        return std::nullopt;
    }
    return parsed.value();
}

} // namespace

bool MinimapData::init(const pack::PackReader& db) {
    m_locks.clear();
    m_trackingSpells.clear();
    bool ok = true;
    if (const std::optional<db::DbcView> locks = table(db, "DBFilesClient\\Lock.dbc", kLockFieldCount)) {
        m_locks.reserve(locks->recordCount());
        for (u32 r = 0; r < locks->recordCount(); ++r) {
            u32 mask = 0;
            for (u32 i = 0; i < 8; ++i) {
                const u32 type = locks->getU32(r, kLockIndex + i);
                if (locks->getU32(r, kLockType + i) == kLockKeySkill && type >= 1 && type <= 32) {
                    mask |= 1u << (type - 1);
                }
            }
            m_locks.emplace_back(locks->getU32(r, 0), mask);
        }
        std::sort(m_locks.begin(), m_locks.end());
    } else {
        ok = false;
    }
    if (const std::optional<db::DbcView> spells = table(db, "DBFilesClient\\Spell.dbc", kSpellFieldCount)) {
        for (u32 r = 0; r < spells->recordCount(); ++r) {
            for (u32 i = 0; i < 3; ++i) {
                const u32 aura = spells->getU32(r, kSpellAura + i);
                if (spells->getU32(r, kSpellEffect + i) == kEffectApplyAura &&
                    (aura == kAuraTrackCreatures || aura == kAuraTrackResources)) {
                    m_trackingSpells.push_back(spells->getU32(r, 0));
                    break;
                }
            }
        }
        std::sort(m_trackingSpells.begin(), m_trackingSpells.end());
    } else {
        ok = false;
    }
    return ok;
}

void MinimapData::set(std::vector<std::pair<u32, u32>> lockTypes, std::vector<u32> trackingSpells) {
    m_locks = std::move(lockTypes);
    m_trackingSpells = std::move(trackingSpells);
    std::sort(m_locks.begin(), m_locks.end());
    std::sort(m_trackingSpells.begin(), m_trackingSpells.end());
}

u32 MinimapData::lockTypeMask(u32 lockId) const {
    const auto it = std::lower_bound(m_locks.begin(), m_locks.end(), std::make_pair(lockId, 0u));
    return it != m_locks.end() && it->first == lockId ? it->second : 0;
}

bool MinimapData::isTrackingSpell(u32 spellId) const {
    return std::binary_search(m_trackingSpells.begin(), m_trackingSpells.end(), spellId);
}

} // namespace vd::game
