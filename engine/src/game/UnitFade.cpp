#include "game/UnitFade.h"

#include "core/Log.h"
#include "db/DbcView.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>

namespace vd::game {
namespace {

namespace uf = net::uf;

// Field numbers of the 1.12.1 (5875) files (as game/SpellVisualData).
constexpr u32 kSpellVisual = 115, kSpellFields = 173;
constexpr u32 kVisualState = 4, kVisualFields = 16; // SpellVisual.dbc: the kit shown while the aura lasts
constexpr u32 kKitProcs = 15, kKitParams = 19, kKitFields = 35; // SpellVisualKit.dbc
constexpr u32 kAuraSlots = 48; // MAX_AURAS

std::optional<db::DbcView> table(const pack::PackReader& db, const char* name, u32 fields) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    if (!asset) {
        VD_LOG_WARN("%s.dbc missing: units are never translucent", name);
        return std::nullopt;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed || parsed.value().fieldCount() < fields) {
        VD_LOG_WARN("%s.dbc: unexpected layout, units are never translucent", name);
        return std::nullopt;
    }
    return parsed.value();
}

glm::vec3 procColor(f32 packed) {
    const u32 rgb = packed > 0.0f && packed < 16777216.0f ? static_cast<u32>(packed) : 0xFFFFFFu;
    return glm::vec3(static_cast<f32>((rgb >> 16) & 0xFF), static_cast<f32>((rgb >> 8) & 0xFF),
                     static_cast<f32>(rgb & 0xFF)) /
           255.0f;
}

} // namespace

UnitFade fadeFromProcs(const std::array<u32, 4>& types, const std::array<f32, 16>& params) {
    UnitFade fade;
    for (u32 i = 0; i < 4; ++i) {
        if (types[i] == UnitFades::kProcTranslucency) {
            fade.alpha = std::clamp(params[i], 0.0f, 1.0f);
            // Stealth and Banish fade in over 1000 (ms); Shadowform's 2 is no time.
            fade.fadeSeconds = params[8 + i] >= 100.0f ? params[8 + i] / 1000.0f : 0.0f;
        }
    }
    for (u32 i = 0; i < 4; ++i) {
        if (types[i] == UnitFades::kProcColor) {
            fade.tint = procColor(params[i]); // phase 14: the client reads p0 only (0x60D840)
        }
    }
    return fade;
}

UnitFade combineFades(const UnitFade& a, const UnitFade& b) {
    UnitFade c;
    c.alpha = std::min(a.alpha, b.alpha);
    c.fadeSeconds = a.alpha <= b.alpha ? a.fadeSeconds : b.fadeSeconds;
    c.tint = a.tint * b.tint;
    return c;
}

glm::vec4 approachFade(const glm::vec4& shown, const UnitFade& target, f32 dt) {
    const glm::vec4 goal(target.tint, target.alpha);
    if (target.fadeSeconds <= 0.0f || dt <= 0.0f) {
        return target.fadeSeconds <= 0.0f ? goal : shown;
    }
    const f32 step = dt / target.fadeSeconds;
    glm::vec4 out;
    for (u32 i = 0; i < 4; ++i) {
        const f32 delta = goal[i] - shown[i];
        out[i] = std::abs(delta) <= step ? goal[i] : shown[i] + (delta > 0.0f ? step : -step);
    }
    return out;
}

bool UnitFades::init(const pack::PackReader& db) {
    m_spells.clear();
    const std::optional<db::DbcView> spells = table(db, "Spell", kSpellFields);
    const std::optional<db::DbcView> visuals = table(db, "SpellVisual", kVisualFields);
    const std::optional<db::DbcView> kits = table(db, "SpellVisualKit", kKitFields);
    if (!spells || !visuals || !kits) {
        return false;
    }
    // Kits with a translucency or color proc, then the visuals showing one as their state, then the spells.
    std::unordered_map<u32, UnitFade> kitFades;
    for (u32 r = 0; r < kits->recordCount(); ++r) {
        std::array<u32, 4> types{};
        std::array<f32, 16> params{};
        bool any = false;
        for (u32 i = 0; i < 4; ++i) {
            types[i] = kits->getU32(r, kKitProcs + i);
            any = any || types[i] == kProcTranslucency || types[i] == kProcColor;
        }
        if (!any) {
            continue;
        }
        for (u32 p = 0; p < 16; ++p) {
            params[p] = kits->getF32(r, kKitParams + p);
        }
        const UnitFade fade = fadeFromProcs(types, params);
        if (!(fade == UnitFade{})) {
            kitFades[kits->getU32(r, 0)] = fade;
        }
    }
    std::unordered_map<u32, UnitFade> visualFades;
    for (u32 r = 0; r < visuals->recordCount(); ++r) {
        const auto it = kitFades.find(visuals->getU32(r, kVisualState));
        if (it != kitFades.end()) {
            visualFades[visuals->getU32(r, 0)] = it->second;
        }
    }
    for (u32 r = 0; r < spells->recordCount(); ++r) {
        const auto it = visualFades.find(spells->getU32(r, kSpellVisual));
        if (it != visualFades.end()) {
            m_spells.emplace_back(spells->getU32(r, 0), it->second);
        }
    }
    std::sort(m_spells.begin(), m_spells.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    m_mask.assign(m_spells.empty() ? 0 : m_spells.back().first / 64 + 1, 0);
    for (const auto& [spell, fade] : m_spells) {
        m_mask[spell / 64] |= 1ull << (spell % 64);
    }
    VD_LOG_INFO("Unit fades: %zu kits, %zu spells change how their unit looks", kitFades.size(), m_spells.size());
    return true;
}

const UnitFade* UnitFades::ofSpell(u32 spellId) const {
    if (spellId / 64 >= m_mask.size() || !(m_mask[spellId / 64] & (1ull << (spellId % 64)))) {
        return nullptr;
    }
    const auto it = std::lower_bound(m_spells.begin(), m_spells.end(), spellId,
                                     [](const std::pair<u32, UnitFade>& e, u32 id) { return e.first < id; });
    return it != m_spells.end() && it->first == spellId ? &it->second : nullptr;
}

UnitFade UnitFades::ofUnit(const Entity& unit) const {
    UnitFade fade;
    if (m_spells.empty() || (unit.type != ObjectType::Unit && unit.type != ObjectType::Player)) {
        return fade;
    }
    for (u32 slot = 0; slot < kAuraSlots; ++slot) {
        const u32 spellId = unit.value(static_cast<u16>(uf::UNIT_FIELD_AURA + slot));
        if (spellId == 0) {
            continue;
        }
        if (const UnitFade* aura = ofSpell(spellId)) {
            fade = combineFades(fade, *aura);
        }
    }
    return fade;
}

} // namespace vd::game
