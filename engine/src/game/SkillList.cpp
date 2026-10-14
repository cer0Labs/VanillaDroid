#include "game/SkillList.h"

#include "core/Profile.h"
#include "game/AbilityData.h"
#include "game/CharacterData.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>

namespace vd::game {
namespace {

namespace uf = net::uf;

// Case-insensitive order of two names (the client compares the skill lines' names that way).
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

u64 mix(u64 hash, u64 value) {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash;
}

} // namespace

u32 readSkillSources(const Entity& player, const AbilityData& abilities, const CharacterData& data,
                     std::span<SkillLineSource> out) {
    const u32 race = player.byteValue(uf::UNIT_FIELD_BYTES_0, 0);
    const u32 cls = player.byteValue(uf::UNIT_FIELD_BYTES_0, 1);
    u32 count = 0;
    for (u32 i = 0; i < kSkillSlots && count < out.size(); ++i) {
        const u16 base = static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + i * 3);
        const u32 idStep = player.value(base);
        const u32 skill = idStep & 0xFFFFu;
        if (skill == 0) {
            continue;
        }
        SkillLineInfo line;
        const SkillRaceClassInfo* info = data.skillInfo(skill, race, cls);
        if (!abilities.skillLine(skill, line) || !info) {
            continue;
        }
        const std::string_view categoryName = abilities.skillCategoryName(line.category);
        if (categoryName.empty()) {
            continue; // the builder wants the category's record
        }
        const u32 values = player.value(static_cast<u16>(base + 1));
        const u32 bonuses = player.value(static_cast<u16>(base + 2));
        SkillLineSource& s = out[count++];
        s.skill = skill;
        s.step = idStep >> 16;
        s.value = values & 0xFFFFu;
        s.max = values >> 16;
        s.tempBonus = static_cast<i16>(bonuses & 0xFFFFu);
        s.permBonus = bonuses >> 16;
        s.category = static_cast<u32>(line.category);
        s.categoryOrder = data.categoryOrder(s.category);
        s.name = line.name;
        s.description = line.description;
        s.categoryName = categoryName;
        s.flags = info->flags;
        s.minLevel = info->minLevel;
        s.costIndex = info->costIndex;
    }
    return count;
}

SkillList::SkillList() {
    m_lines.resize(kSkillSlots);
    m_entries.resize(kSkillSlots * 2);
    m_rows.resize(kSkillSlots * 2);
}

void SkillList::reset() {
    m_selected = -1;
    m_expandedMask = 0xFFFFFFFFu;
    m_signature = 0;
    m_lineCount = m_entryCount = m_categoryCount = m_shown = 0;
}

void SkillList::build(std::span<const SkillLineSource> lines, u32 level) {
    VD_PROFILE_ZONE("SkillList::build");
    u64 signature = 0x5157;
    for (const SkillLineSource& s : lines) {
        signature =
            mix(signature, s.skill | (static_cast<u64>(s.step) << 16) | (static_cast<u64>(s.value) << 32));
        signature = mix(signature, s.max | (static_cast<u64>(static_cast<u16>(s.tempBonus)) << 16) |
                                       (static_cast<u64>(s.permBonus) << 32));
    }
    m_changed = signature != m_signature;
    if (m_changed) {
        m_signature = signature;
        m_expandedMask = 0xFFFFFFFFu; // the rebuild (0x4D2CB0) expands every header again
    }

    // The builder: the lines in slot order, a header the first time a category shows up.
    m_lineCount = 0;
    m_entryCount = 0;
    m_categoryCount = 0;
    for (const SkillLineSource& s : lines) {
        if (m_lineCount == m_lines.size()) {
            break;
        }
        if (s.flags & kSkillFlagHidden) {
            continue;
        }
        const bool learned = s.max != 0;
        if (!learned && (s.flags & 1) == 0 && ((s.flags & kSkillFlagLearnable) == 0 || level < s.minLevel)) {
            continue;
        }
        const u32 line = m_lineCount;
        m_lines[m_lineCount++] = s;
        u32 category = 0;
        while (category < m_categoryCount && m_categories[category].id != s.category) {
            ++category;
        }
        if (category == m_categoryCount) {
            if (m_categoryCount == m_categories.size()) {
                --m_lineCount;
                continue;
            }
            m_categories[m_categoryCount++] = {s.category, s.categoryOrder, line};
            m_entries[m_entryCount++] = {line, category, true, true};
        }
        m_entries[m_entryCount++] = {line, category, false, learned};
    }
    // The headers by their category's sort index (0x4D3000), the entries pointing at them follow.
    std::array<u32, 32> order{};
    for (u32 i = 0; i < m_categoryCount; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.begin() + m_categoryCount, [&](u32 a, u32 b) {
        return m_categories[a].order != m_categories[b].order ? m_categories[a].order < m_categories[b].order
                                                              : a < b;
    });
    std::array<u32, 32> rank{};
    for (u32 i = 0; i < m_categoryCount; ++i) {
        rank[order[i]] = i;
    }
    std::array<Category, 32> sorted{};
    for (u32 i = 0; i < m_categoryCount; ++i) {
        sorted[i] = m_categories[order[i]];
    }
    m_categories = sorted;
    for (u32 i = 0; i < m_entryCount; ++i) {
        m_entries[i].category = rank[m_entries[i].category];
    }
    // The lines (0x4D3070): by header, the header first, learned ones first, then by name.
    // std::sort with the lines' order as the last key (stable_sort would allocate its buffer every frame).
    std::sort(m_entries.begin(), m_entries.begin() + m_entryCount, [&](const Entry& a, const Entry& b) {
        if (a.category != b.category) {
            return a.category < b.category;
        }
        if (a.header != b.header) {
            return a.header;
        }
        if (a.learned != b.learned) {
            return a.learned;
        }
        const std::string_view an = m_lines[a.source].name, bn = m_lines[b.source].name;
        if (nameLess(an, bn) || nameLess(bn, an)) {
            return nameLess(an, bn);
        }
        return a.source < b.source;
    });

    // The rows shown: lines under a collapsed header are left out.
    m_shown = 0;
    for (u32 i = 0; i < m_entryCount; ++i) {
        const Entry& e = m_entries[i];
        const bool expanded = e.category >= 32 || (m_expandedMask & (1u << e.category)) != 0;
        if (!e.header && !expanded) {
            continue;
        }
        const SkillLineSource& s = m_lines[e.source];
        SkillLineRow& row = m_rows[m_shown++];
        row = SkillLineRow{};
        row.header = e.header;
        row.expanded = expanded;
        row.category = s.category;
        if (e.header) {
            row.name = s.categoryName;
            continue;
        }
        // GetSkillLineInfo (0x4D3610).
        row.skill = s.skill;
        row.name = s.name;
        row.description = s.description;
        row.learned = e.learned;
        row.rank = s.value > 0 ? s.value + s.permBonus : 0;
        row.maxRank = s.max > 0 ? s.max + s.permBonus : 0;
        if (s.flags & kSkillFlagMaxRankOne) {
            row.maxRank = 1;
            row.rank = std::min<u32>(row.rank, 1);
        }
        row.modifier = s.tempBonus;
        row.abandonable = s.step != 0 && (s.flags & kSkillFlagUnlearnable) != 0;
        row.minLevel = s.minLevel;
        row.costType = s.costIndex + 1;
    }
}

void SkillList::collapse(i32 row) {
    if (row < 0) {
        m_expandedMask = 0; // CollapseSkillHeader(0): all
    } else if (static_cast<u32>(row) < m_shown && m_rows[static_cast<u32>(row)].header) {
        for (u32 i = 0; i < m_categoryCount && i < 32; ++i) {
            if (m_categories[i].id == m_rows[static_cast<u32>(row)].category) {
                m_expandedMask &= ~(1u << i);
            }
        }
    }
}

void SkillList::expand(i32 row) {
    if (row < 0) {
        m_expandedMask = 0xFFFFFFFFu;
    } else if (static_cast<u32>(row) < m_shown && m_rows[static_cast<u32>(row)].header) {
        for (u32 i = 0; i < m_categoryCount && i < 32; ++i) {
            if (m_categories[i].id == m_rows[static_cast<u32>(row)].category) {
                m_expandedMask |= 1u << i;
            }
        }
    }
}

bool SkillList::allExpanded() const {
    for (u32 i = 0; i < m_categoryCount && i < 32; ++i) {
        if ((m_expandedMask & (1u << i)) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace vd::game
