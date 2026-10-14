#pragma once

#include "core/Log.h"
#include "core/Types.h"
#include "db/DbcView.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace vd::game {

// A client table of the db pack with its records sorted by id (field 0): a binary search instead of a hash
// map (phase-8 tables: Spell, SkillLine, Talent, Faction...). The view points into the mapped pack.
struct DbcTable {
    static constexpr u32 kMissing = 0xFFFFFFFFu;

    db::DbcView view;
    std::vector<std::pair<u32, u32>> index; // (id, record)

    // `base`: the base client's table, not its language overlay's (phase 21: names that rules match, in enUS).
    bool load(const pack::PackReader& db, const char* name, u32 minFields = 1, bool base = false) {
        index.clear();
        const std::string path = std::string("DBFilesClient\\") + name + ".dbc";
        const std::optional<pack::PackAsset> asset = base ? db.findBase(path) : db.find(path);
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
            index.emplace_back(view.getU32(r, 0), r);
        }
        std::sort(index.begin(), index.end());
        return true;
    }
    bool empty() const { return index.empty(); }
    // The record of `id`, kMissing if absent.
    u32 find(u32 id) const {
        const auto it = std::lower_bound(index.begin(), index.end(), std::make_pair(id, 0u));
        return it != index.end() && it->first == id ? it->second : kMissing;
    }
};

} // namespace vd::game
