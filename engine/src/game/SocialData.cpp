#include "game/SocialData.h"

#include "core/Log.h"
#include "core/Text.h"
#include "db/DbcView.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <cstdio>

namespace vd::game {
namespace {

// AreaTable (DBCfmt "niiiixxxxxissssssssxixxxi"): id, map, zone, explore flag, flags, ..., name (enUS at 11).
namespace area {
constexpr u32 kZone = 2, kFlags = 4, kName = 11;
constexpr u32 kFlagSlaveCapital = 0x008; // the zone has a trade channel
constexpr u32 kFlagCity = 0x200;         // its name is the city channels' ("Trade - City")
} // namespace area
// ChatChannels ("nixssssssssxxxxxxxxxx"): id, flags, faction group, name pattern (enUS at 3).
namespace channels {
constexpr u32 kFlags = 1, kPattern = 3;
constexpr u32 kInitial = 0x00001;  // CHANNEL_DBC_FLAG_INITIAL: joined automatically
constexpr u32 kZoneDep = 0x00002;  // the name carries the zone's
constexpr u32 kCityOnly = 0x00010; // only in cities
constexpr u32 kLookingForGroup = 24;
} // namespace channels
// EmotesText ("nxixxxxxxxxxxxxxxxx"): id, name, emote, 16 EmotesTextData ids. EmotesTextData: id, text (enUS).
namespace emotes {
constexpr u32 kToken = 1, kEmote = 2, kTexts = 3;
}
// ChrRaces ("niixiixxiiiiixixissssssssxxxx"): the name (enUS) at 17. ChrClasses ("nxxixssssssssxxix"): at 5.
constexpr u32 kRaceName = 17, kClassName = 5;

// `base`: the base client's table (phase 21), not the language overlay's.
bool loadTable(const pack::PackReader& db, const char* name, db::DbcView& out, bool base = false) {
    char path[96];
    std::snprintf(path, sizeof(path), "DBFilesClient\\%s.dbc", name);
    const std::optional<pack::PackAsset> asset = base ? db.findBase(path) : db.find(path);
    if (!asset) {
        VD_LOG_WARN("Social data: %s missing", path);
        return false;
    }
    Result<db::DbcView> view = db::DbcView::parse(asset->data, asset->size);
    if (!view) {
        VD_LOG_WARN("Social data: %s unreadable", path);
        return false;
    }
    out = view.value();
    return true;
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool sameToken(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

bool SocialData::init(const pack::PackReader& db) {
    db::DbcView areas, chatChannels, texts, lines;
    if (!loadTable(db, "AreaTable", areas) || !loadTable(db, "ChatChannels", chatChannels) ||
        !loadTable(db, "EmotesText", texts) || !loadTable(db, "EmotesTextData", lines) ||
        areas.fieldCount() <= area::kName || chatChannels.fieldCount() <= channels::kPattern ||
        texts.fieldCount() < emotes::kTexts + 16 || lines.fieldCount() < 2) {
        return false;
    }
    // Phase 21: the names the server knows (the base client's tables, under a language overlay).
    db::DbcView wireAreas = areas, wireChannels = chatChannels;
    if (db.hasOverlay() && loadTable(db, "AreaTable", wireAreas, true) &&
        loadTable(db, "ChatChannels", wireChannels, true) && wireAreas.recordCount() == areas.recordCount() &&
        wireChannels.recordCount() == chatChannels.recordCount() && wireAreas.fieldCount() > area::kName &&
        wireChannels.fieldCount() > channels::kPattern) {
        m_translatedChannels = true;
    } else {
        wireAreas = areas;
        wireChannels = chatChannels;
    }
    m_areas.reserve(areas.recordCount());
    if (m_translatedChannels) {
        m_displayAreaNames.reserve(areas.recordCount());
    }
    u32 cityArea = 0;
    for (u32 r = 0; r < areas.recordCount(); ++r) {
        const u32 id = areas.getU32(r, 0);
        Area a{areas.getU32(r, area::kZone), areas.getU32(r, area::kFlags), areas.getString(r, area::kName),
               wireAreas.getString(r, area::kName)};
        if ((a.flags & area::kFlagCity) && id >= cityArea) {
            cityArea = id;
            m_cityName = a.name;
            m_wireCityName = a.wireName;
        }
        if (m_translatedChannels) {
            m_displayAreaNames.emplace(a.wireName, a.name);
        }
        m_areas[id] = a;
        if (a.zone == 0) { // a top-level zone, in the table's order (SendWho's z-)
            m_topZones.push_back(id);
            m_topZoneNames.push_back(a.name);
        }
    }
    for (u32 r = 0; r < chatChannels.recordCount(); ++r) {
        m_channels.push_back({chatChannels.getU32(r, 0), chatChannels.getU32(r, channels::kFlags),
                              chatChannels.getString(r, channels::kPattern),
                              wireChannels.getString(r, channels::kPattern)});
    }
    m_tokens.reserve(texts.recordCount()); // the emotes point into these: never reallocated
    m_emotes.reserve(texts.recordCount());
    for (u32 r = 0; r < texts.recordCount(); ++r) {
        std::string& token = m_tokens.emplace_back(texts.getString(r, emotes::kToken));
        for (char& c : token) {
            c = lower(c);
        }
        TextEmote e;
        e.id = texts.getU32(r, 0);
        e.token = token;
        e.emote = texts.getU32(r, emotes::kEmote);
        for (u32 i = 0; i < 16; ++i) {
            e.texts[i] = texts.getU32(r, emotes::kTexts + i);
        }
        m_emotes.push_back(e);
    }
    m_emoteLines.reserve(lines.recordCount());
    for (u32 r = 0; r < lines.recordCount(); ++r) {
        m_emoteLines[lines.getU32(r, 0)] = lines.getString(r, 1);
    }
    for (std::string& name : m_zoneNames) {
        name.reserve(64);
    }
    // Race and class names (optional: without them the interface shows none).
    db::DbcView races, classes;
    if (loadTable(db, "ChrRaces", races) && races.fieldCount() > kRaceName) {
        for (u32 r = 0; r < races.recordCount(); ++r) {
            const u32 id = races.getU32(r, 0);
            if (id < m_raceNames.size()) {
                m_raceNames[id] = races.getString(r, kRaceName);
            }
        }
    }
    if (loadTable(db, "ChrClasses", classes) && classes.fieldCount() > kClassName) {
        for (u32 r = 0; r < classes.recordCount(); ++r) {
            const u32 id = classes.getU32(r, 0);
            if (id < m_classNames.size()) {
                m_classNames[id] = classes.getString(r, kClassName);
            }
        }
    }
    m_ready = !m_areas.empty() && !m_channels.empty();
    VD_LOG_INFO("Social data: %zu areas, %zu chat channels, %zu text emotes", m_areas.size(), m_channels.size(),
                m_emotes.size());
    return m_ready;
}

u32 SocialData::zoneOf(u32 areaId) const {
    const auto it = m_areas.find(areaId);
    if (it == m_areas.end()) {
        return 0;
    }
    return it->second.zone != 0 ? it->second.zone : areaId;
}

std::string_view SocialData::raceName(u32 race) const {
    return race < m_raceNames.size() ? m_raceNames[race] : std::string_view{};
}

std::string_view SocialData::className(u32 cls) const {
    return cls < m_classNames.size() ? m_classNames[cls] : std::string_view{};
}

std::string_view SocialData::areaName(u32 areaId) const {
    const auto it = m_areas.find(areaId);
    return it != m_areas.end() ? it->second.name : std::string_view{};
}

bool SocialData::isCity(u32 zoneId) const {
    const auto it = m_areas.find(zoneId);
    return it != m_areas.end() && (it->second.flags & area::kFlagSlaveCapital) != 0;
}

std::span<const ZoneChannel> SocialData::zoneChannels(u32 zoneId) {
    const auto zone = m_areas.find(zoneId);
    const std::string_view zoneName = zone != m_areas.end() ? zone->second.wireName : std::string_view{};
    u32 count = 0;
    for (const Channel& c : m_channels) {
        const bool initial = (c.flags & channels::kInitial) != 0;
        if ((!initial && c.id != channels::kLookingForGroup) || count == m_zoneChannels.size()) {
            continue;
        }
        std::string& name = m_zoneNames[count];
        name.clear();
        const bool joined = !(c.flags & channels::kCityOnly) || isCity(zoneId);
        if (joined && !(zoneName.empty() && (c.flags & channels::kZoneDep))) {
            const std::string_view suffix = (c.flags & channels::kCityOnly) ? m_wireCityName : zoneName;
            const usize at = c.wirePattern.find("%s");
            if (at == std::string_view::npos) {
                name.assign(c.wirePattern);
            } else {
                name.assign(c.wirePattern.substr(0, at));
                name.append(suffix);
                name.append(c.wirePattern.substr(at + 2));
            }
        }
        m_zoneChannels[count++] = {c.id, name};
    }
    return {m_zoneChannels.data(), count};
}

u32 SocialData::channelKind(std::string_view name) const {
    // GetChannelEntryFor of the server: the pattern without "%s" found in the name.
    for (const Channel& c : m_channels) {
        std::string_view pattern = c.wirePattern;
        const usize at = pattern.find("%s");
        const std::string_view head = at == std::string_view::npos ? pattern : pattern.substr(0, at);
        if (!head.empty() && name.find(head) != std::string_view::npos) {
            return c.id;
        }
    }
    return 0;
}

std::string_view SocialData::displayChannel(std::string_view name, std::span<char> out) const {
    if (!m_translatedChannels || out.empty()) {
        return name;
    }
    // "2. " before the name, as the chat writes it.
    usize prefix = 0;
    while (prefix < name.size() && name[prefix] >= '0' && name[prefix] <= '9') {
        ++prefix;
    }
    prefix = prefix > 0 && name.substr(prefix, 2) == ". " ? prefix + 2 : 0;
    const std::string_view bare = name.substr(prefix);
    for (const Channel& c : m_channels) {
        const usize at = c.wirePattern.find("%s");
        const std::string_view head = at == std::string_view::npos ? c.wirePattern : c.wirePattern.substr(0, at);
        const std::string_view tail = at == std::string_view::npos ? std::string_view{} : c.wirePattern.substr(at + 2);
        if (head.empty() || bare.substr(0, head.size()) != head || bare.size() < head.size() + tail.size() ||
            bare.substr(bare.size() - tail.size()) != tail || (at == std::string_view::npos && bare != head)) {
            continue;
        }
        std::string_view zone = bare.substr(head.size(), bare.size() - head.size() - tail.size());
        if (const auto it = m_displayAreaNames.find(zone); it != m_displayAreaNames.end()) {
            zone = it->second;
        }
        usize used = 0;
        const auto put = [&](std::string_view part) {
            const usize n = std::min(part.size(), out.size() - 1 - used);
            std::copy_n(part.data(), n, out.data() + used);
            used += n;
        };
        put(name.substr(0, prefix));
        const usize shownAt = c.pattern.find("%s");
        if (shownAt == std::string_view::npos) {
            put(c.pattern);
        } else {
            put(c.pattern.substr(0, shownAt));
            put(zone);
            put(c.pattern.substr(shownAt + 2));
        }
        out[used] = 0;
        return {out.data(), used};
    }
    return name;
}

std::string_view SocialData::displayChannelBase(std::string_view name, std::span<char> out) const {
    const std::string_view shown = displayChannel(name, out);
    const usize dash = shown.find(" - ");
    return dash == std::string_view::npos ? shown : shown.substr(0, dash);
}

const SocialData::TextEmote* SocialData::textEmote(u32 id) const {
    for (const TextEmote& e : m_emotes) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

const SocialData::TextEmote* SocialData::textEmoteByCommand(std::string_view word) const {
    if (word.empty() || !text::loaded()) {
        return nullptr;
    }
    for (int n = 1; n < 1000; ++n) {
        char key[32];
        std::snprintf(key, sizeof(key), "EMOTE%d_TOKEN", n);
        if (!text::has(key)) {
            break; // ChatFrame.lua numbers them from 1 without gaps (170 in 1.12)
        }
        const std::string_view token = text::get(key);
        for (int m = 1; m <= 9; ++m) {
            std::snprintf(key, sizeof(key), "EMOTE%d_CMD%d", n, m);
            if (!text::has(key)) {
                continue;
            }
            const std::string_view command = text::get(key);
            if (command.size() == word.size() + 1 && command.front() == '/' && sameToken(command.substr(1), word)) {
                return textEmoteByToken(token);
            }
        }
    }
    return nullptr;
}

const SocialData::TextEmote* SocialData::textEmoteByToken(std::string_view token) const {
    for (const TextEmote& e : m_emotes) {
        if (sameToken(e.token, token)) {
            return &e;
        }
    }
    return nullptr;
}

std::string_view SocialData::textEmoteLine(u32 id, u32 variant) const {
    const TextEmote* e = textEmote(id);
    if (!e || variant >= e->texts.size() || e->texts[variant] == 0) {
        return {};
    }
    const auto it = m_emoteLines.find(e->texts[variant]);
    return it != m_emoteLines.end() ? it->second : std::string_view{};
}

} // namespace vd::game
