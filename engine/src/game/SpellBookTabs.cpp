#include "game/SpellBookTabs.h"

#include "core/Hash.h"

#include <algorithm>
#include <array>

namespace vd::game {
namespace {

constexpr u32 kGeneralKey = 0;
constexpr u32 kHidden = 0xFFFFFFFFu;
constexpr u32 kAttack = 6603;

u32 rankNumber(std::string_view rank) {
    u32 value = 0;
    bool digits = false;
    for (const char c : rank) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<u32>(c - '0');
            digits = true;
        } else if (digits) {
            break;
        }
    }
    return value;
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

} // namespace

void SpellBookTabs::update(const AbilityData& data, std::span<const u32> known, u8 race, u8 cls,
                           bool allRanks) {
    u64 signature = kFnvOffset;
    const auto mix = [&](u64 value) {
        signature ^= value;
        signature *= kFnvPrime;
    };
    for (const u32 id : known) {
        mix(id);
    }
    mix(known.size());
    mix(race);
    mix(static_cast<u64>(cls) << 8 | (allRanks ? 1 : 0));
    signature |= 1; // never 0 (= invalidated)
    if (signature == m_signature) {
        return;
    }
    m_signature = signature;

    // The class's trees name its class tabs, in tree order ("Shadow Magic" is the Shadow tree's line).
    std::array<TalentTabInfo, 4> trees{};
    const u32 treeCount = data.talentTabs(cls, trees);
    const SpellText& text = data.spellText();

    std::vector<Keyed>& keyed = m_keyed;
    keyed.clear();
    for (const u32 id : known) {
        SpellDetails details;
        if (!text.details(id, details) || details.hidden() || details.tradeSpell() || details.castUi != 0) {
            continue; // phase 13: recipes and crafts are in their profession's window (WoW.exe leaves them out)
        }
        const u32 skillId = data.skillOfSpell(id, race, cls);
        SkillLineInfo line;
        u32 key = kHidden;
        if (id == kAttack) {
            line.category =
                SkillCategory::Attributes; // in no skill line, listed in General like the client does
        } else if (skillId == 0 || !data.skillLine(skillId, line)) {
            continue; // not in the spell book (internal spells, "Duel"...)
        }
        switch (line.category) {
        case SkillCategory::Class: {
            // Matched by the base client's (enUS) names: "Magia sombría" is no "Sombra" (phase 21).
            key = startsWith(line.baseName, "Pet - ") ? kHidden : kGeneralKey;
            for (u32 t = 0; t < treeCount; ++t) {
                if (!trees[t].baseName.empty() && startsWith(line.baseName, trees[t].baseName)) {
                    key = 1 + t;
                }
            }
            break;
        }
        case SkillCategory::Profession:
        case SkillCategory::Secondary: key = kGeneralKey; break; // phase 13: no professions tab in 1.12
        case SkillCategory::Weapon:
        case SkillCategory::Armor:
        case SkillCategory::Language:
        case SkillCategory::Attributes: key = details.passive() ? kHidden : kGeneralKey; break;
        default: key = kHidden; break;
        }
        if (key == kHidden) {
            continue;
        }
        SpellBookEntry e;
        e.spellId = id;
        e.name = details.name;
        e.rank = details.rank;
        e.rankNumber = rankNumber(details.rank);
        e.passive = details.passive();
        keyed.push_back({key, key >= 1 && key <= treeCount ? skillId : 0, e});
    }
    std::sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        if (a.key != b.key) {
            return a.key < b.key;
        }
        if (a.entry.name != b.entry.name) {
            return a.entry.name < b.entry.name;
        }
        return a.entry.rankNumber < b.entry.rankNumber;
    });
    // The highest rank of each name in its tab.
    for (usize i = 0; i < keyed.size(); ++i) {
        const bool next = i + 1 < keyed.size() && keyed[i + 1].key == keyed[i].key &&
                          keyed[i + 1].entry.name == keyed[i].entry.name;
        keyed[i].entry.highest = !next;
    }

    m_tabs.clear();
    m_entries.clear();
    const auto addTab = [&](SpellTabKind kind, u32 key) {
        SpellBookTab tab;
        tab.kind = kind;
        tab.first = static_cast<u32>(m_entries.size());
        for (const Keyed& k : keyed) {
            if (k.key != key || (!allRanks && !k.entry.highest)) {
                continue;
            }
            m_entries.push_back(k.entry);
            if (k.skillLine != 0 && tab.skillLine == 0) {
                tab.skillLine = k.skillLine;
            }
        }
        tab.count = static_cast<u32>(m_entries.size()) - tab.first;
        if (kind == SpellTabKind::Class) {
            if (tab.count == 0) {
                return; // a tree without a known spell yet
            }
            SkillLineInfo line;
            if (data.skillLine(tab.skillLine, line)) {
                tab.name = line.name;
                tab.spellIcon = line.spellIcon;
            }
        }
        m_tabs.push_back(tab);
    };
    addTab(SpellTabKind::General, kGeneralKey);
    for (u32 t = 0; t < treeCount; ++t) {
        addTab(SpellTabKind::Class, 1 + t);
    }
}

std::span<const SpellBookEntry> SpellBookTabs::entries(u32 tab) const {
    if (tab >= m_tabs.size()) {
        return {};
    }
    return {m_entries.data() + m_tabs[tab].first, m_tabs[tab].count};
}

bool SpellBookTabs::find(u32 spellId, u32& tab, u32& index) const {
    for (u32 t = 0; t < m_tabs.size(); ++t) {
        const std::span<const SpellBookEntry> list = entries(t);
        for (u32 i = 0; i < list.size(); ++i) {
            if (list[i].spellId == spellId) {
                tab = t;
                index = i;
                return true;
            }
        }
    }
    return false;
}

u32 SpellBookTabs::highestRank(u32 spellId) const {
    std::string_view name;
    for (const SpellBookEntry& e : m_entries) {
        if (e.spellId == spellId) {
            name = e.name;
        }
    }
    if (name.empty()) {
        return spellId;
    }
    u32 best = spellId, bestRank = 0;
    for (const SpellBookEntry& e : m_entries) {
        if (e.name == name && e.rankNumber >= bestRank) {
            best = e.spellId;
            bestRank = e.rankNumber;
        }
    }
    return best;
}

} // namespace vd::game
