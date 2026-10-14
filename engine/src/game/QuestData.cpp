#include "game/QuestData.h"

#include "core/Log.h"
#include "game/AreaData.h"
#include "pack/PackReader.h"

#include <optional>
#include <utility>

namespace vd::game {
namespace {

// QuestSort.dbc and QuestInfo.dbc (5875): id, the name in 8 locales, their flags.
constexpr u32 kNameField = 1;
constexpr u32 kFieldCount = 10;

void loadNames(const pack::PackReader& db, const char* path, std::unordered_map<u32, std::string_view>& out) {
    const std::optional<pack::PackAsset> asset = db.find(path);
    if (!asset) {
        VD_LOG_WARN("%s missing: the quest log names fewer headers", path);
        return;
    }
    Result<db::DbcView> table = db::DbcView::parse(asset->data, asset->size);
    if (!table || table.value().fieldCount() < kFieldCount) {
        VD_LOG_WARN("%s: unexpected layout", path);
        return;
    }
    const db::DbcView& view = table.value();
    out.reserve(view.recordCount());
    for (u32 r = 0; r < view.recordCount(); ++r) {
        out.emplace(view.getU32(r, 0), view.getString(r, kNameField));
    }
}

} // namespace

bool QuestData::init(const pack::PackReader& db, const AreaData* areas) {
    m_areas = areas;
    loadNames(db, "DBFilesClient\\QuestSort.dbc", m_sorts);
    loadNames(db, "DBFilesClient\\QuestInfo.dbc", m_types);
    return !m_sorts.empty();
}

void QuestData::set(std::unordered_map<u32, std::string_view> sorts,
                    std::unordered_map<u32, std::string_view> types, const AreaData* areas,
                    std::unordered_map<u32, std::string_view> zones) {
    m_sorts = std::move(sorts);
    m_types = std::move(types);
    m_areas = areas;
    m_zones = std::move(zones);
}

std::string_view QuestData::headerName(i32 zoneOrSort) const {
    if (zoneOrSort > 0) {
        if (const AreaInfo* area = m_areas ? m_areas->area(static_cast<u32>(zoneOrSort)) : nullptr) {
            return area->name;
        }
        const auto found = m_zones.find(static_cast<u32>(zoneOrSort));
        return found != m_zones.end() ? found->second : std::string_view{};
    }
    if (zoneOrSort < 0) {
        const auto found = m_sorts.find(static_cast<u32>(-zoneOrSort));
        return found != m_sorts.end() ? found->second : std::string_view{};
    }
    return {};
}

std::string_view QuestData::typeName(u32 type) const {
    const auto found = type != 0 ? m_types.find(type) : m_types.end();
    return found != m_types.end() ? found->second : std::string_view{};
}

} // namespace vd::game
