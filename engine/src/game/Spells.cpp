#include "game/Spells.h"

#include <algorithm>

namespace vd::game {
namespace {

constexpr u32 kGlobalCooldownCategory = 133; // StartRecoveryCategory of the spells on the global cooldown
// SMSG_INITIAL_SPELLS: the category recovery bit of permanent ("on event") cooldowns.
constexpr u32 kPermanentCooldown = 0x80000000u;
constexpr u32 kUnitCastGraceMs = 500; // a unit cast bar outlives its end by this (the GO may lag)
// SPELL_ATTR_ON_NEXT_SWING_NO_DAMAGE | SPELL_ATTR_ON_NEXT_SWING: cast on the next melee swing (IsNextMeleeSwingSpell).
constexpr u32 kAttrOnNextSwing = 0x00000004 | 0x00000400;

bool ends(u32 startMs, u32 durationMs, u32 nowMs) {
    return nowMs - startMs >= durationMs;
}

// The view that ends last of two (an "until event" one never ends).
CooldownView later(const CooldownView& a, const CooldownView& b, u32 nowMs) {
    if (a.untilEvent || b.untilEvent) {
        return a.untilEvent ? a : b;
    }
    return a.remaining(nowMs) >= b.remaining(nowMs) ? a : b;
}

} // namespace

SpellSource spellSource(const GameData& data) {
    SpellSource source;
    source.context = &data;
    source.lookup = [](const void* context, u32 spellId, SpellInfo& out) {
        std::optional<SpellInfo> info = static_cast<const GameData*>(context)->spell(spellId);
        if (!info) {
            return false;
        }
        out = *info;
        return true;
    };
    return source;
}

SpellBook::SpellBook() {
    m_known.reserve(256);
    m_cooldowns.reserve(64);
}

void SpellBook::reset() {
    m_known.clear();
    ++m_knownVersion;
    m_queuedSwing = 0;
    m_buttons.fill(0);
    m_cast = CastBar{};
    m_unitCasts.fill(UnitCast{});
    m_cooldowns.clear();
    m_gcds.fill(Gcd{});
}

// -------------------------------------------------------------------------------------------------------
// Known spells and buttons.

void SpellBook::onInitialSpells(const net::InitialSpells& m, u32 nowMs) {
    m_known.clear();
    m_known.reserve(std::max<usize>(m.spells.size(), 256));
    for (const u16 id : m.spells) {
        m_known.push_back(id);
    }
    std::sort(m_known.begin(), m_known.end());
    m_known.erase(std::unique(m_known.begin(), m_known.end()), m_known.end());
    ++m_knownVersion;

    m_cooldowns.clear();
    m_cooldowns.reserve(64);
    for (const net::InitialSpells::Cooldown& c : m.cooldowns) {
        const u32 categoryMs = static_cast<u32>(c.categoryRecoveryMs);
        const bool untilEvent = (categoryMs & kPermanentCooldown) != 0;
        startCooldown(c.spellId, c.category, c.recoveryMs > 0 ? static_cast<u32>(c.recoveryMs) : 0,
                      untilEvent ? 0 : categoryMs, untilEvent, nowMs);
    }
}

void SpellBook::onLearned(u32 spellId) {
    const auto it = std::lower_bound(m_known.begin(), m_known.end(), spellId);
    if (it == m_known.end() || *it != spellId) {
        m_known.insert(it, spellId);
        ++m_knownVersion;
    }
}

void SpellBook::onRemoved(u32 spellId) {
    const auto it = std::lower_bound(m_known.begin(), m_known.end(), spellId);
    if (it != m_known.end() && *it == spellId) {
        m_known.erase(it);
        ++m_knownVersion;
    }
}

void SpellBook::onSuperceded(u32 oldSpellId, u32 newSpellId) {
    onRemoved(oldSpellId);
    onLearned(newSpellId);
    // The client moves the new rank onto the buttons of the old one.
    for (u32& packed : m_buttons) {
        const ActionButton b = ActionButton::unpack(packed);
        if (b.type == net::kActionSpell && b.action == oldSpellId && oldSpellId != 0) {
            packed = ActionButton{net::kActionSpell, newSpellId}.packed();
        }
    }
}

bool SpellBook::knows(u32 spellId) const {
    return std::binary_search(m_known.begin(), m_known.end(), spellId);
}

void SpellBook::onActionButtons(const std::array<u32, net::kActionButtonCount>& buttons) {
    m_buttons = buttons;
}

ActionButton SpellBook::button(u32 slot) const {
    return slot < m_buttons.size() ? ActionButton::unpack(m_buttons[slot]) : ActionButton{};
}

void SpellBook::setButton(u32 slot, ActionButton button) {
    if (slot < m_buttons.size()) {
        m_buttons[slot] = button.packed();
    }
}

// -------------------------------------------------------------------------------------------------------
// Casting.

void SpellBook::onCastRequested(u32 spellId, u32 nowMs) {
    SpellInfo info;
    if (!m_source.get(spellId, info)) {
        return;
    }
    if (info.attributes & kAttrOnNextSwing) { // waits for the next swing: the current action until then
        m_queuedSwing = spellId;
        m_queuedSwingMs = nowMs;
    }
    if (info.gcdCategory == 0 || info.gcdMs == 0) {
        return;
    }
    for (const Gcd& g : m_gcds) {
        if (g.category == info.gcdCategory && !ends(g.startMs, g.durationMs, nowMs)) {
            return; // tapping again during the global cooldown does not restart it
        }
    }
    startGcd(info.gcdCategory, gcdOf(info), nowMs, spellId);
}

u32 SpellBook::gcdOf(const SpellInfo& info) const {
    if (!m_mods) {
        return info.gcdMs;
    }
    return static_cast<u32>(std::max(0, m_mods->apply(modTarget(info), SpellModOp::GlobalCooldown,
                                                      static_cast<i32>(info.gcdMs))));
}

void SpellBook::onSpellStart(const net::SpellStart& m, u64 player, u32 nowMs) {
    const u64 caster = m.unitCaster ? m.unitCaster : m.caster;
    if (caster == player) {
        // The server started it: a predicted global cooldown stays; one not predicted (a cast from a macro or
        // an item) starts now.
        SpellInfo info;
        if (m_source.get(m.spellId, info) && info.gcdCategory != 0 && info.gcdMs != 0) {
            bool running = false;
            for (Gcd& g : m_gcds) {
                if (g.category == info.gcdCategory && !ends(g.startMs, g.durationMs, nowMs)) {
                    running = true;
                    g.predictedSpell = 0; // confirmed
                }
            }
            if (!running) {
                startGcd(info.gcdCategory, gcdOf(info), nowMs, 0);
            }
        }
        if (m.castTimeMs > 0) {
            m_cast = CastBar{m.spellId, caster, m.targets.unit, nowMs, m.castTimeMs, false};
        }
        return;
    }
    if (m.castTimeMs == 0) {
        return;
    }
    // Another unit: its slot, a free one, or the one ending first.
    UnitCast* slot = nullptr;
    for (UnitCast& c : m_unitCasts) {
        if (c.guid == caster) {
            slot = &c;
            break;
        }
    }
    if (!slot) {
        for (UnitCast& c : m_unitCasts) {
            if (c.guid == 0 || ends(c.bar.startMs, c.bar.durationMs + kUnitCastGraceMs, nowMs)) {
                slot = &c;
                break;
            }
        }
    }
    if (!slot) {
        slot = &m_unitCasts[0];
        for (UnitCast& c : m_unitCasts) {
            if (c.bar.remaining(nowMs) < slot->bar.remaining(nowMs)) {
                slot = &c;
            }
        }
    }
    slot->guid = caster;
    slot->bar = CastBar{m.spellId, caster, m.targets.unit, nowMs, m.castTimeMs, false};
}

bool SpellBook::onSpellGo(const net::SpellGo& m, u64 player, u32 nowMs, const ItemSpellCooldown* item) {
    const u64 caster = m.unitCaster ? m.unitCaster : m.caster;
    if (caster != player) {
        for (UnitCast& c : m_unitCasts) {
            if (c.guid == caster && c.bar.spellId == m.spellId) {
                c = UnitCast{};
            }
        }
        return false;
    }
    if (m.spellId == m_queuedSwing) {
        m_queuedSwing = 0; // the swing came
    }
    // The spell went off: its own and its category's cooldowns start (the server does not send them). From an
    // item, the item's values win (Player::AddCooldown with the item's prototype).
    SpellInfo info;
    if (m_source.get(m.spellId, info)) {
        if (item) {
            info.category = item->category != 0 ? item->category : info.category;
            info.recoveryMs = item->recoveryMs >= 0 ? static_cast<u32>(item->recoveryMs) : info.recoveryMs;
            info.categoryRecoveryMs =
                item->categoryRecoveryMs >= 0 ? static_cast<u32>(item->categoryRecoveryMs) : info.categoryRecoveryMs;
        }
        if (m_mods) { // phase 13 (spells): SPELLMOD_COOLDOWN on both
            const SpellModTarget target = modTarget(info);
            info.recoveryMs = static_cast<u32>(
                std::max(0, m_mods->apply(target, SpellModOp::Cooldown, static_cast<i32>(info.recoveryMs))));
            info.categoryRecoveryMs = static_cast<u32>(
                std::max(0, m_mods->apply(target, SpellModOp::Cooldown, static_cast<i32>(info.categoryRecoveryMs))));
        }
        if (info.attributes & kSpellAttrCooldownOnEvent) {
            startCooldown(m.spellId, info.category, 0, 0, true, nowMs);
        } else if (info.recoveryMs != 0 || (info.category != 0 && info.categoryRecoveryMs != 0)) {
            startCooldown(m.spellId, info.category, info.recoveryMs,
                          info.category ? info.categoryRecoveryMs : 0, false, nowMs);
        }
    }
    for (Gcd& g : m_gcds) {
        if (g.predictedSpell == m.spellId) {
            g.predictedSpell = 0;
        }
    }
    if (m_cast.active() && !m_cast.channel && m_cast.spellId == m.spellId) {
        m_cast = CastBar{};
        return true;
    }
    return false;
}

void SpellBook::onSpellFailedOther(const net::SpellFailedOther& m, u64 player) {
    if (m.caster == player) {
        if (m.spellId == m_queuedSwing && m.spellId != 0) {
            m_queuedSwing = 0;
        }
        if (m_cast.active() && (m.spellId == m_cast.spellId || m.spellId == 0)) {
            m_cast = CastBar{};
        }
        return;
    }
    for (UnitCast& c : m_unitCasts) {
        if (c.guid == m.caster) {
            c = UnitCast{};
        }
    }
}

void SpellBook::onCastResult(const net::CastResult& m) {
    if (m.status != net::kCastStatusFailed) {
        return;
    }
    if (m_cast.active() && m_cast.spellId == m.spellId) {
        m_cast = CastBar{};
    }
    if (m.spellId == m_queuedSwing) {
        m_queuedSwing = 0;
    }
    // A refused cast never started its global cooldown.
    for (Gcd& g : m_gcds) {
        if (g.predictedSpell != 0 && g.predictedSpell == m.spellId) {
            g = Gcd{};
        }
    }
}

void SpellBook::onSpellDelayed(const net::SpellDelayed& m, u64 player) {
    if (m.caster == player && m_cast.active() && !m_cast.channel) {
        m_cast.durationMs += m.delayMs; // pushback: the end moves later
    }
}

void SpellBook::onChannelStart(const net::ChannelStart& m, u64 player, u32 nowMs) {
    m_cast = CastBar{m.spellId, player, 0, nowMs, m.durationMs, true};
}

void SpellBook::onChannelUpdate(u32 remainingMs, u32 nowMs) {
    if (!m_cast.active() || !m_cast.channel) {
        return;
    }
    if (remainingMs == 0) {
        m_cast = CastBar{};
        return;
    }
    m_cast.durationMs = (nowMs - m_cast.startMs) + remainingMs;
}

const CastBar* SpellBook::unitCast(u64 guid, u32 nowMs) const {
    if (guid == 0) {
        return nullptr;
    }
    if (m_cast.active() && m_cast.caster == guid && !ends(m_cast.startMs, m_cast.durationMs, nowMs)) {
        return &m_cast;
    }
    for (const UnitCast& c : m_unitCasts) {
        if (c.guid == guid && c.bar.active() && !ends(c.bar.startMs, c.bar.durationMs, nowMs)) {
            return &c.bar;
        }
    }
    return nullptr;
}

// -------------------------------------------------------------------------------------------------------
// Cooldowns.

void SpellBook::startCooldown(u32 spellId, u32 category, u32 spellMs, u32 categoryMs, bool untilEvent,
                              u32 nowMs) {
    prune(nowMs);
    Cooldown* entry = nullptr;
    for (Cooldown& c : m_cooldowns) {
        if (c.spellId == spellId) {
            entry = &c;
            break;
        }
    }
    if (!entry) {
        entry = &m_cooldowns.emplace_back();
    }
    *entry = Cooldown{spellId, category, nowMs, spellMs, categoryMs, untilEvent};
}

void SpellBook::startGcd(u32 category, u32 durationMs, u32 nowMs, u32 predictedSpell) {
    Gcd* slot = &m_gcds[0];
    for (Gcd& g : m_gcds) {
        if (g.category == category) {
            slot = &g;
            break;
        }
        if (g.category == 0 || ends(g.startMs, g.durationMs, nowMs)) {
            slot = &g;
        }
    }
    *slot = Gcd{category, nowMs, durationMs, predictedSpell};
}

void SpellBook::prune(u32 nowMs) {
    std::erase_if(m_cooldowns, [&](const Cooldown& c) {
        return !c.untilEvent && ends(c.startMs, std::max(c.spellMs, c.categoryMs), nowMs);
    });
}

void SpellBook::onSpellCooldown(const net::SpellCooldown& m, u64 player, u32 nowMs) {
    if (m.caster != player) {
        return; // a pet's or charmed unit's (no pet bar yet)
    }
    for (const net::SpellCooldown::Entry& e : m.entries) {
        if (e.cooldownMs == 0) {
            // Player::AddGCD(updateClient): the spell's global cooldown.
            SpellInfo info;
            if (m_source.get(e.spellId, info) && info.gcdCategory != 0 && info.gcdMs != 0) {
                startGcd(info.gcdCategory, info.gcdMs, nowMs, 0);
            }
        } else {
            startCooldown(e.spellId, 0, e.cooldownMs, 0, false, nowMs); // school lockout, scripts
        }
    }
}

void SpellBook::onCooldownEvent(u32 spellId, u32 nowMs) {
    SpellInfo info;
    u32 category = 0, spellMs = 0, categoryMs = 0;
    if (m_source.get(spellId, info)) {
        category = info.category;
        spellMs = info.recoveryMs;
        categoryMs = info.category ? info.categoryRecoveryMs : 0;
    }
    if (spellMs == 0 && categoryMs == 0) {
        onClearCooldown(spellId);
        return;
    }
    startCooldown(spellId, category, spellMs, categoryMs, false, nowMs);
}

void SpellBook::onClearCooldown(u32 spellId) {
    std::erase_if(m_cooldowns, [&](const Cooldown& c) { return c.spellId == spellId; });
}

CooldownView SpellBook::cooldown(u32 spellId, u32 nowMs) const {
    SpellInfo info;
    return m_source.get(spellId, info) ? cooldownOf(spellId, info.category, info.gcdCategory, nowMs)
                                       : cooldownOf(spellId, 0, 0, nowMs);
}

CooldownView SpellBook::cooldown(u32 spellId, u32 category, u32 nowMs) const {
    SpellInfo info;
    return cooldownOf(spellId, category, m_source.get(spellId, info) ? info.gcdCategory : 0, nowMs);
}

CooldownView SpellBook::cooldown(const SpellInfo& spell, u32 category, u32 nowMs) const {
    return cooldownOf(spell.id, category, spell.gcdCategory, nowMs);
}

CooldownView SpellBook::cooldownOf(u32 spellId, u32 category, u32 gcdCategory, u32 nowMs) const {
    CooldownView best;
    for (const Cooldown& c : m_cooldowns) {
        if (c.spellId == spellId) {
            best = later(best, CooldownView{c.startMs, c.spellMs, c.untilEvent}, nowMs);
            if (c.untilEvent) {
                return best;
            }
        }
        // A category cooldown locks every spell of the category (an "on event" one too: potions share
        // theirs).
        if (category != 0 && c.category == category) {
            best = later(best, CooldownView{c.startMs, c.categoryMs, c.untilEvent}, nowMs);
        }
    }
    if (gcdCategory != 0) {
        for (const Gcd& g : m_gcds) {
            if (g.category == gcdCategory) {
                best = later(best, CooldownView{g.startMs, g.durationMs, false}, nowMs);
            }
        }
    }
    return best;
}

CooldownView SpellBook::itemCooldown(const net::ItemQueryInfo& item, u32 nowMs) const {
    CooldownView best;
    for (const net::ItemQueryInfo::Spell& spell : item.spells) {
        if (spell.id == 0 || spell.trigger != net::kItemSpellTriggerOnUse) {
            continue;
        }
        SpellInfo info;
        const bool known = m_source.get(spell.id, info);
        const u32 category = spell.category != 0 ? spell.category : (known ? info.category : 0);
        best = later(best, cooldownOf(spell.id, category, known ? info.gcdCategory : 0, nowMs), nowMs);
    }
    return best;
}

u32 SpellBook::queuedSwing(u32 nowMs) const {
    return m_queuedSwing != 0 && nowMs - m_queuedSwingMs < kQueuedSwingMs ? m_queuedSwing : 0;
}

CooldownView SpellBook::globalCooldown(u32 nowMs) const {
    for (const Gcd& g : m_gcds) {
        if (g.category == kGlobalCooldownCategory && !ends(g.startMs, g.durationMs, nowMs)) {
            return CooldownView{g.startMs, g.durationMs, false};
        }
    }
    return CooldownView{};
}

} // namespace vd::game
