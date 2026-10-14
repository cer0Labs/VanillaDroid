#include "game/WorldStateUi.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "db/DbcView.h"
#include "game/Social.h"
#include "game/WorldStates.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vd::game {
namespace {

// WorldStateUI.dbc (1.12.1, 39 fields): id, map, area, icon, string (8 locales + flags), tooltip (8 + flags),
// an unused field, state variable, type, dynamic icon, dynamic tooltip (8 + flags), extended UI, 3 extended
// states. The client is enUS: the first locale of each string.
namespace wsui {
constexpr u32 kFields = 39;
constexpr u32 kMap = 1, kArea = 2, kIcon = 3, kText = 4, kTooltip = 13, kStateVariable = 23, kType = 24,
              kDynamicIcon = 25, kDynamicTooltip = 26, kExtendedUi = 35, kExtendedStates = 36;
} // namespace wsui
// ChatChannels: id, flags (CHANNEL_DBC_FLAG_DEFENSE 0x10000, _ZONE_DEP 0x2: what 0x49BD9B tests), faction
// group, name pattern (enUS).
constexpr u32 kChannelFlags = 1, kChannelPattern = 3;
constexpr u32 kChannelDefense = 0x10000, kChannelZoneDep = 0x2;

// `base`: the base client's table (phase 21: the channels' names as the server knows them, in enUS).
bool loadTable(const pack::PackReader& db, const char* name, db::DbcView& out, bool base = false) {
    char path[96];
    std::snprintf(path, sizeof(path), "DBFilesClient\\%s.dbc", name);
    const std::optional<pack::PackAsset> asset = base ? db.findBase(path) : db.find(path);
    if (!asset) {
        VD_LOG_WARN("World state UI: %s missing", path);
        return false;
    }
    Result<db::DbcView> view = db::DbcView::parse(asset->data, asset->size);
    if (!view) {
        VD_LOG_WARN("World state UI: %s unreadable", path);
        return false;
    }
    out = view.value();
    return true;
}

} // namespace

bool WorldStateUiData::init(const pack::PackReader& db) {
    db::DbcView rows, channels;
    // The defense channels' patterns match the names the server sends: the base client's (phase 21).
    if (!loadTable(db, "WorldStateUI", rows) || !loadTable(db, "ChatChannels", channels, true)) {
        return false;
    }
    if (rows.fieldCount() < wsui::kFields) {
        VD_LOG_WARN("WorldStateUI.dbc has %u fields, expected %u", rows.fieldCount(), wsui::kFields);
        return false;
    }
    m_rows.clear();
    m_rows.reserve(rows.recordCount());
    for (u32 r = 0; r < rows.recordCount(); ++r) {
        WorldStateUiRow row;
        row.id = rows.getU32(r, 0);
        row.map = rows.getI32(r, wsui::kMap);
        row.area = rows.getU32(r, wsui::kArea);
        row.icon = rows.getString(r, wsui::kIcon);
        row.text = rows.getString(r, wsui::kText);
        row.tooltip = rows.getString(r, wsui::kTooltip);
        row.stateVariable = rows.getU32(r, wsui::kStateVariable);
        row.type = rows.getU32(r, wsui::kType);
        row.dynamicIcon = rows.getString(r, wsui::kDynamicIcon);
        row.dynamicTooltip = rows.getString(r, wsui::kDynamicTooltip);
        row.extendedUi = rows.getString(r, wsui::kExtendedUi);
        for (u32 i = 0; i < 3; ++i) {
            row.extendedStates[i] = rows.getU32(r, wsui::kExtendedStates + i);
        }
        m_rows.push_back(row);
    }
    std::vector<u32> ids;
    std::vector<std::string_view> patterns;
    for (u32 r = 0; r < channels.recordCount(); ++r) {
        const u32 flags = channels.getU32(r, kChannelFlags);
        if ((flags & kChannelDefense) != 0 && (flags & kChannelZoneDep) != 0) {
            ids.push_back(channels.getU32(r, 0));
            patterns.push_back(channels.getString(r, kChannelPattern));
        }
    }
    std::vector<WorldStateUiRow> kept = std::move(m_rows);
    set(std::move(kept), std::move(ids), std::move(patterns));
    return true;
}

void WorldStateUiData::set(std::vector<WorldStateUiRow> rows, std::vector<u32> defenseChannels,
                           std::vector<std::string_view> defensePatterns) {
    m_rows = std::move(rows);
    m_defenseChannels = std::move(defenseChannels);
    m_defensePrefixes.clear();
    for (const std::string_view pattern : defensePatterns) {
        const usize at = pattern.find("%s");
        const std::string_view prefix = at == std::string_view::npos ? pattern : pattern.substr(0, at);
        if (!prefix.empty()) {
            m_defensePrefixes.push_back(prefix);
        }
    }
}

const WorldStateUiRow* WorldStateUiData::row(u32 id) const {
    for (const WorldStateUiRow& r : m_rows) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

bool WorldStateUiData::defenseChannel(u32 chatChannel, std::string_view name) const {
    if (chatChannel != 0) {
        return std::find(m_defenseChannels.begin(), m_defenseChannels.end(), chatChannel) !=
               m_defenseChannels.end();
    }
    for (const std::string_view prefix : m_defensePrefixes) {
        if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix) {
            return true;
        }
    }
    return false;
}

u32 WorldStateUiData::statColumns(u32 map, std::span<const WorldStateUiRow*> out) const {
    // 0x4AA9C3: rows of the map (or -1) and type 2 counted from the first; the first other row after them
    // ends it.
    u32 count = 0;
    for (const WorldStateUiRow& r : m_rows) {
        const bool column =
            (r.map == -1 || static_cast<u32>(r.map) == map) && r.type == kWorldStateUiScoreColumn;
        if (column) {
            if (count < out.size()) {
                out[count] = &r;
            }
            ++count;
        } else if (count > 0) {
            break;
        }
    }
    return std::min<u32>(count, static_cast<u32>(out.size()));
}

std::string_view formatWorldStateText(std::string_view text, const WorldStates& states, std::span<char> out) {
    if (out.empty()) {
        return {};
    }
    usize used = 0;
    const auto put = [&](std::string_view s) {
        const usize n = std::min(s.size(), out.size() - 1 - used);
        std::memcpy(out.data() + used, s.data(), n);
        used += n;
    };
    usize i = 0;
    while (i < text.size()) {
        const usize percent = text.find('%', i);
        if (percent == std::string_view::npos) {
            put(text.substr(i));
            break;
        }
        put(text.substr(i, percent - i));
        usize j = percent + 1;
        u32 id = 0;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
            id = id * 10 + static_cast<u32>(text[j] - '0');
            ++j;
        }
        if (j < text.size() && (text[j] == 'w' || text[j] == 'W')) {
            char number[16];
            const int n = std::snprintf(number, sizeof(number), "%d", static_cast<i32>(states.value(id)));
            put(std::string_view(number, static_cast<usize>(std::max(n, 0))));
            ++j; // past the 'w'
        } else {
            put("%"); // 0x5085F7: the '%' alone, the text goes on after the digits
        }
        i = j;
    }
    out[used] = 0;
    return {out.data(), used};
}

WorldStateUi::WorldStateUi(const WorldStates& states, const Social& social, Events& events)
    : m_states(states), m_social(social), m_events(events) {
    m_list.reserve(32);
}

bool WorldStateUi::localDefenseNow() const {
    if (!m_data) {
        return false;
    }
    for (const ChatChannel& c : m_social.channels()) {
        if (c.joined && !c.name.empty() && m_data->defenseChannel(c.zoneChannel, c.name)) {
            return true;
        }
    }
    return false;
}

void WorldStateUi::update(u32 nowMs) {
    VD_PROFILE_ZONE("WorldStateUi::update");
    const bool defense = localDefenseNow() || m_forceZone;
    const bool defenseChanged = defense != m_localDefense;
    m_localDefense = defense;
    const bool known = m_states.known();
    if (known != m_built || (known && (m_map != m_states.mapId() || m_zone != m_states.zoneId())) ||
        defenseChanged || m_listData != m_data) {
        rebuild();
        if (known && defenseChanged) { // 0x49BDD6: UPDATE_WORLD_STATES after the new list
            GameEvent& e = m_events.push();
            e = GameEvent{};
            e.kind = GameEventKind::WorldStates;
            e.timeMs = nowMs;
        }
    }
}

void WorldStateUi::leaveWorld() {
    m_list.clear();
    m_built = false;
    m_map = m_zone = 0;
    ++m_changes;
}

void WorldStateUi::rebuild() {
    m_list.clear();
    m_listData = m_data;
    m_built = m_states.known();
    m_map = m_states.mapId();
    m_zone = m_states.zoneId();
    ++m_changes;
    if (!m_built || !m_data) {
        return;
    }
    for (const WorldStateUiRow& r : m_data->rows()) {
        if (r.map != -1 && static_cast<u32>(r.map) != m_map) {
            continue;
        }
        if (r.area != 0 && r.area != m_zone) {
            continue;
        }
        if (r.type == kWorldStateUiAlwaysUp || (r.type == kWorldStateUiZone && m_localDefense)) {
            m_list.push_back(&r);
        }
    }
}

WorldStateUiInfo WorldStateUi::info(u32 index, std::span<char> text) const {
    WorldStateUiInfo out;
    if (index >= m_list.size()) {
        if (!text.empty()) {
            text[0] = 0;
        }
        return out; // 0x4C5BE5: a state of 0 and nothing else
    }
    const WorldStateUiRow& r = *m_list[index];
    out.id = r.id;
    out.state = r.stateVariable != 0 ? m_states.value(r.stateVariable) : 1;
    out.text = formatWorldStateText(r.text, m_states, text);
    out.icon = r.icon;
    out.dynamicIcon = r.dynamicIcon;
    out.tooltip = r.tooltip;
    out.dynamicTooltip = r.dynamicTooltip;
    out.extendedUi = r.extendedUi;
    for (u32 i = 0; i < 3; ++i) {
        out.extendedStates[i] = static_cast<i32>(m_states.value(r.extendedStates[i]));
    }
    return out;
}

} // namespace vd::game
