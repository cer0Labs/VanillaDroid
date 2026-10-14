#include "game/SpellLines.h"

#include "core/Hash.h"
#include "game/SpellText.h"

#include <algorithm>

namespace vd::game {

u32 SpellLines::rankNumber(std::string_view rank) {
    // The last number in the text ("Rank 3"; other locales put the word elsewhere).
    u32 value = 0;
    bool digits = false;
    for (const char c : rank) {
        if (c >= '0' && c <= '9') {
            value = digits ? value * 10 + static_cast<u32>(c - '0') : static_cast<u32>(c - '0');
            digits = true;
        } else if (digits) {
            digits = false;
        }
    }
    return value;
}

void SpellLines::update(std::span<const u32> known, const SpellText& text, u32 version) {
    if (version == m_version && m_text == &text) {
        return;
    }
    m_version = version;
    m_text = &text;
    m_entries.clear();
    m_entries.reserve(known.size());
    for (const u32 id : known) {
        const std::string_view name = text.name(id);
        if (!name.empty()) {
            m_entries.push_back({stringHash(name), rankNumber(text.rank(id)), id});
        }
    }
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        return a.name != b.name ? a.name < b.name : a.rank != b.rank ? a.rank < b.rank : a.spellId < b.spellId;
    });
}

u32 SpellLines::highest(u32 spellId) const {
    if (!m_text || spellId == 0) {
        return spellId;
    }
    const std::string_view name = m_text->name(spellId);
    if (name.empty()) {
        return spellId;
    }
    const u64 key = stringHash(name);
    const auto end = std::upper_bound(m_entries.begin(), m_entries.end(), key,
                                      [](u64 k, const Entry& e) { return k < e.name; });
    return end != m_entries.begin() && (end - 1)->name == key ? (end - 1)->spellId : spellId;
}

bool SpellLines::knows(u32 spellId) const {
    if (!m_text || spellId == 0) {
        return false;
    }
    const std::string_view name = m_text->name(spellId);
    const u64 key = stringHash(name);
    const auto it = std::lower_bound(m_entries.begin(), m_entries.end(), key,
                                     [](const Entry& e, u64 k) { return e.name < k; });
    return !name.empty() && it != m_entries.end() && it->name == key;
}

} // namespace vd::game
