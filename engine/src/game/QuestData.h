#pragma once

#include "core/Types.h"
#include "db/DbcView.h"

#include <string_view>
#include <unordered_map>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

class AreaData;

// Phase 13 (quests): the client tables the quest log names things with (db.vdpk): QuestSort.dbc (the headers
// of quests sorted by class, profession or event: zoneOrSort < 0) and QuestInfo.dbc (the tag after a title:
// Elite, PvP, Raid, Dungeon...), plus the zones of AreaTable through AreaData. Names in the client's language
// (enUS).
class QuestData {
public:
    bool init(const pack::PackReader& db, const AreaData* areas);
    // The tables from elsewhere (tests, samples): QuestSort and QuestInfo rows as (id, name), and zone names
    // for when there is no AreaData.
    void set(std::unordered_map<u32, std::string_view> sorts, std::unordered_map<u32, std::string_view> types,
             const AreaData* areas, std::unordered_map<u32, std::string_view> zones = {});

    // A header's name: the zone of AreaTable (> 0) or the sort of QuestSort (< 0); empty when unknown.
    std::string_view headerName(i32 zoneOrSort) const;
    // QuestInfo's name of a quest type (the template's `type`), empty for 0 or unknown.
    std::string_view typeName(u32 type) const;

private:
    const AreaData* m_areas = nullptr;
    std::unordered_map<u32, std::string_view> m_sorts;
    std::unordered_map<u32, std::string_view> m_types;
    std::unordered_map<u32, std::string_view> m_zones; // set()'s
};

} // namespace vd::game
