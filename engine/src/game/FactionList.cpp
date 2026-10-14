#include "game/FactionList.h"

#include "core/Profile.h"

#include <algorithm>

namespace vd::game {
namespace {

constexpr u8 kFlagHeader =
    0x08; // the client's reading of the reputation flags' 0x08 (vMaNGOS: INVISIBLE_FORCED)
constexpr i32 kNoSelection = 0x7FFFFFFF;
constexpr i32 kRankBounds[9] = {-42000, -6000, -3000, 0, 3000, 9000, 21000, 42000, 43000};

// Case-insensitive order of two names (WoW.exe compares the factions' names that way).
bool nameLess(std::string_view a, std::string_view b) {
    const usize n = std::min(a.size(), b.size());
    for (usize i = 0; i < n; ++i) {
        const char ca = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char cb = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (ca != cb) {
            return static_cast<u8>(ca) < static_cast<u8>(cb);
        }
    }
    return a.size() < b.size();
}

bool hasBit(u32 mask, u32 id) {
    return id > 0 && id <= 32 && (mask & (1u << (id - 1))) != 0;
}

} // namespace

i32 clientBaseReputation(const FactionInfo& faction, u32 race, u32 cls) {
    // WoW.exe 0x4D54D5: each of the four rows that fits overwrites the base (the last one wins); rows with
    // neither mask are skipped.
    i32 base = 0;
    for (u32 k = 0; k < 4; ++k) {
        const u32 raceMask = faction.raceMask[k], classMask = faction.classMask[k];
        if (raceMask == 0 && classMask == 0) {
            continue;
        }
        if ((raceMask != 0 && !hasBit(raceMask, race)) || (classMask != 0 && !hasBit(classMask, cls))) {
            continue;
        }
        base = faction.baseValue[k];
    }
    return base;
}

i32 reputationRankMin(u32 rank) {
    return kRankBounds[std::min<u32>(rank, 7)];
}

i32 reputationRankMax(u32 rank) {
    return kRankBounds[std::min<u32>(rank, 7) + 1];
}

u32 clientReputationRank(i32 value) {
    for (u32 rank = 7; rank > 0; --rank) {
        if (value >= kRankBounds[rank]) {
            return rank;
        }
    }
    return 0;
}

FactionList::Source FactionList::sourceOf(const AbilityData& data) {
    Source s;
    s.byList = [](const void* context, u32 listId) {
        return static_cast<const AbilityData*>(context)->factionByList(listId);
    };
    s.byId = [](const void* context, u32 factionId) {
        return static_cast<const AbilityData*>(context)->faction(factionId);
    };
    s.context = &data;
    return s;
}

FactionList::FactionList() {
    m_entries.resize(net::kFactionListSize * 2 + 2);
    m_rows.resize(net::kFactionListSize * 2 + 2);
}

void FactionList::reset() {
    m_entryCount = m_headerCount = m_lastHeaderCount = m_shown = 0;
    m_expandedMask = 0xFFFFFFFFu;
    m_selectedFaction = kNoSelection;
    m_hasWatched = false;
}

void FactionList::build(const ReputationList& list, const Source& source, u32 race, u32 cls,
                        i32 watchedListId) {
    VD_PROFILE_ZONE("FactionList::build");
    m_entryCount = 0;
    m_headerCount = 0;
    m_shown = 0;
    m_hasWatched = false;
    if (!source.byList || !source.byId) {
        return;
    }
    const auto addHeader = [&](i32 faction) {
        for (u32 i = 0; i < m_headerCount; ++i) {
            if (m_headers[i].faction == faction) {
                return;
            }
        }
        if (m_headerCount < m_headers.size()) {
            const FactionInfo* f =
                faction > 0 ? source.byId(source.context, static_cast<u32>(faction)) : nullptr;
            m_headers[m_headerCount++] = {faction, 0, f ? f->name : std::string_view{}, f != nullptr};
        }
    };
    const auto addEntry = [&](const Entry& e) {
        if (m_entryCount < m_entries.size()) {
            m_entries[m_entryCount++] = e;
        }
    };
    if (list.initialized()) {
        // 0x4D5A70 for every visible faction.
        for (u32 listId = 0; listId < list.size(); ++listId) {
            const u8 flags = list.entry(listId).flags;
            const FactionInfo* f = source.byList(source.context, listId);
            if (!f || (flags & net::kFactionVisible) == 0) {
                continue;
            }
            bool exists = false;
            for (u32 i = 0; i < m_entryCount && !exists; ++i) {
                exists = m_entries[i].listId == static_cast<i32>(listId);
            }
            if (exists) {
                continue; // added already, as the header of a faction under it
            }
            Entry e;
            e.listId = static_cast<i32>(listId);
            e.faction = static_cast<i32>(f->id);
            e.header = (flags & kFlagHeader) != 0;
            if (e.header) {
                e.group = e.faction;
                addEntry(e);
                addHeader(e.faction);
                continue;
            }
            const i32 parent = static_cast<i32>(f->parent);
            e.group = (flags & net::kFactionInactive) ? kFactionInactiveHeader : parent;
            addEntry(e);
            bool parentListed = false;
            for (u32 i = 0; i < m_entryCount && !parentListed; ++i) {
                parentListed = m_entries[i].header && m_entries[i].faction == parent;
            }
            if (!parentListed) {
                const FactionInfo* p =
                    parent > 0 ? source.byId(source.context, static_cast<u32>(parent)) : nullptr;
                Entry h;
                h.listId = p ? p->listId : -1;
                h.faction = parent;
                h.group = parent;
                h.header = true;
                addEntry(h);
                addHeader(parent);
            }
        }
        // The Inactive header (0x4D5579), always there.
        Entry inactive;
        inactive.listId = -1;
        inactive.faction = kFactionInactiveHeader;
        inactive.group = kFactionInactiveHeader;
        inactive.header = true;
        addEntry(inactive);
        addHeader(kFactionInactiveHeader);
    }
    if (m_headerCount > m_lastHeaderCount) {
        m_expandedMask = 0xFFFFFFFFu; // a header was added: every header expands (0x4D5C20)
    }
    m_lastHeaderCount = m_headerCount;

    // 0x4D5C40: the headers' counts, their order (0x4D5DC0), then the entries' (0x4D5E70).
    for (u32 h = 0; h < m_headerCount; ++h) {
        m_headers[h].count = 0;
        for (u32 i = 0; i < m_entryCount; ++i) {
            m_headers[h].count += !m_entries[i].header && m_entries[i].group == m_headers[h].faction ? 1 : 0;
        }
    }
    // std::sort with a total order (stable_sort would allocate its buffer every frame).
    std::sort(m_headers.begin(), m_headers.begin() + m_headerCount, [](const Header& a, const Header& b) {
        if ((a.count == 0) != (b.count == 0)) {
            return a.count != 0;
        }
        if (a.count != 0 && a.hasRow != b.hasRow) {
            return a.hasRow; // named headers before FACTION_OTHER and FACTION_INACTIVE
        }
        if (a.count != 0 && a.hasRow && (nameLess(a.name, b.name) || nameLess(b.name, a.name))) {
            return nameLess(a.name, b.name);
        }
        return a.faction > b.faction;
    });
    const auto expanded = [&](u32 header) { return header >= 32 || (m_expandedMask & (1u << header)) != 0; };
    for (u32 i = 0; i < m_entryCount; ++i) {
        Entry& e = m_entries[i];
        e.headerIndex = 0;
        for (u32 h = 0; h < m_headerCount; ++h) {
            if (m_headers[h].faction == e.group) {
                e.headerIndex = h;
            }
        }
    }
    const auto entryName = [&](const Entry& e) {
        const FactionInfo* f =
            e.faction > 0 ? source.byId(source.context, static_cast<u32>(e.faction)) : nullptr;
        return f ? f->name : std::string_view{};
    };
    std::sort(m_entries.begin(), m_entries.begin() + m_entryCount, [&](const Entry& a, const Entry& b) {
        if (a.headerIndex != b.headerIndex) {
            return a.headerIndex < b.headerIndex;
        }
        if (a.header != b.header) {
            return a.header;
        }
        const std::string_view an = entryName(a), bn = entryName(b);
        if (nameLess(an, bn) || nameLess(bn, an)) {
            return nameLess(an, bn);
        }
        return a.listId < b.listId;
    });
    for (u32 i = 0; i < m_entryCount; ++i) {
        const Entry& e = m_entries[i];
        if (e.header ? m_headers[e.headerIndex].count == 0 : !expanded(e.headerIndex)) {
            continue; // an empty header, or a faction under a collapsed one
        }
        FactionRow& row = m_rows[m_shown++];
        row = rowOf(e, list, source, race, cls, watchedListId);
        row.collapsed = e.header && !expanded(e.headerIndex);
    }

    // GetWatchedFactionInfo (0x4D6820): the list id's faction, listed or not.
    if (watchedListId >= 0 && watchedListId < static_cast<i32>(net::kFactionListSize) && list.initialized()) {
        if (const FactionInfo* f = source.byList(source.context, static_cast<u32>(watchedListId))) {
            Entry e;
            e.listId = watchedListId;
            e.faction = static_cast<i32>(f->id);
            m_watched = rowOf(e, list, source, race, cls, watchedListId);
            m_hasWatched = true;
        }
    }
}

FactionRow FactionList::rowOf(const Entry& e, const ReputationList& list, const Source& source, u32 race,
                              u32 cls, i32 watchedListId) const {
    // GetFactionInfo (0x4D64F0).
    FactionRow row;
    row.faction = e.faction;
    row.header = e.header;
    const FactionInfo* f = e.faction > 0 ? source.byId(source.context, static_cast<u32>(e.faction)) : nullptr;
    if (!f) { // FACTION_OTHER / FACTION_INACTIVE
        row.standing = 1;
        return row;
    }
    row.listId = f->listId;
    row.name = f->name;
    row.description = f->description;
    const bool listed = f->listId >= 0 && f->listId < static_cast<i32>(list.size());
    const ReputationList::Entry state =
        listed ? list.entry(static_cast<u32>(f->listId)) : ReputationList::Entry{};
    const i32 value = listed ? state.standing + clientBaseReputation(*f, race, cls) : 0;
    const u32 rank = clientReputationRank(value);
    row.standing = rank + 1;
    row.barMin = reputationRankMin(rank);
    row.barMax = reputationRankMax(rank);
    row.barValue = value;
    row.atWar = (state.flags & net::kFactionAtWar) != 0;
    row.canToggleAtWar = value >= -3000 && (state.flags & net::kFactionPeaceForced) == 0;
    row.watched = f->listId >= 0 && f->listId == watchedListId;
    row.inactive = (state.flags & net::kFactionInactive) != 0;
    return row;
}

void FactionList::collapse(i32 row) {
    if (row < 0) {
        m_expandedMask = 0;
        return;
    }
    if (static_cast<u32>(row) >= m_shown || !m_rows[static_cast<u32>(row)].header) {
        return;
    }
    for (u32 h = 0; h < m_headerCount && h < 32; ++h) {
        if (m_headers[h].faction == m_rows[static_cast<u32>(row)].faction) {
            m_expandedMask &= ~(1u << h);
        }
    }
}

void FactionList::expand(i32 row) {
    if (row < 0) {
        m_expandedMask = 0xFFFFFFFFu;
        return;
    }
    if (static_cast<u32>(row) >= m_shown || !m_rows[static_cast<u32>(row)].header) {
        return;
    }
    for (u32 h = 0; h < m_headerCount && h < 32; ++h) {
        if (m_headers[h].faction == m_rows[static_cast<u32>(row)].faction) {
            m_expandedMask |= 1u << h;
        }
    }
}

i32 FactionList::selected() const {
    for (u32 i = 0; i < m_shown; ++i) {
        if (!m_rows[i].header && m_rows[i].faction == m_selectedFaction) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

void FactionList::select(i32 row) {
    m_selectedFaction =
        row >= 0 && static_cast<u32>(row) < m_shown ? m_rows[static_cast<u32>(row)].faction : kNoSelection;
}

} // namespace vd::game
