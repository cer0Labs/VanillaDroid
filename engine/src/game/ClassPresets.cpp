#include "game/ClassPresets.h"

#include "core/Hash.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/ActionBar.h"
#include "game/ClassPresetData.h"
#include "game/GameData.h"
#include "game/SpellText.h"
#include "game/TouchContext.h"

#include <algorithm>
#include <optional>

namespace vd::game {
namespace {

using presets::Cand;
using presets::CandKind;
using presets::GroupDef;
using presets::GroupKind;
using presets::TemplateDef;

// The order the slots take their candidates in (§7.1): the most pressed first, so that a spell two slots want goes
// to the one nearer the thumb's rest. The consumables last (they take no spells).
constexpr TouchPlace kFillOrder[kTouchSlots] = {
    TouchPlace::Main,    TouchPlace::RingA,       TouchPlace::RingC,       TouchPlace::RingD,
    TouchPlace::RingE,   TouchPlace::RingB,       TouchPlace::ColumnF,     TouchPlace::ColumnG,
    TouchPlace::ColumnH, TouchPlace::Consumable1, TouchPlace::Consumable2, TouchPlace::Consumable3};

constexpr u8 kSlotMainHand = 15, kSlotOffHand = 16, kSlotRanged = 17;
constexpr u32 kTalentPointsPerRow = 5; // a talent row opens with five points spent in its tree

// What the candidates are checked against: the known spells by name (a line: any of its ranks), the talent tree
// with the most points, the weapons worn.
class Knowledge {
public:
    explicit Knowledge(const PresetInput& in) : m_in(in) {
        m_names.reserve(in.known.size() + 1);
        for (const u32 id : in.known) {
            const std::string_view name = in.text->name(id);
            if (!name.empty()) {
                m_names.push_back(stringHash(name));
            }
        }
        // Attack is every character's (the auto attack toggle), listed by the server or not.
        m_names.push_back(stringHash(in.text->name(kAttackSpell)));
        std::sort(m_names.begin(), m_names.end());
        m_topTree = topTalentTree(in);
        m_worn[kSlotMainHand] = worn(in.mainHandClass, in.mainHandSubclass);
        m_worn[kSlotOffHand] = worn(in.offHandClass, in.offHandSubclass);
        m_worn[kSlotRanged] = worn(in.rangedClass, in.rangedSubclass);
    }

    u64 nameOf(u32 spellId) const { return stringHash(m_in.text->name(spellId)); }
    // Known, castable in `form` when that is a body change (Cat, Bear, Travel Form: a caster's spell or a racial
    // like War Stomp is not), and its equipped item worn (Backstab's dagger, Shield Bash's shield, Shoot's wand: the
    // action bars' rule, game/ActionBar wornFitsSpell; the main hand, off hand and ranged slot are what matter).
    bool usable(u32 spellId, u32 form) const {
        if (spellId == 0 || !std::binary_search(m_names.begin(), m_names.end(), nameOf(spellId))) {
            return false;
        }
        SpellDetails details;
        if (form != 0 && m_in.abilities && !m_in.abilities->formIsStance(form) &&
            m_in.text->details(spellId, details) && !m_in.abilities->castableInForm(details, form)) {
            return false;
        }
        if (!m_in.weaponsKnown || !m_in.data) {
            return true;
        }
        const std::optional<SpellInfo> info = m_in.data->spell(spellId);
        return !info || wornFitsSpell(*info, m_worn);
    }
    bool treeAllows(const Cand& c) const {
        if ((c.flags & presets::kIfTopTree) == 0) {
            return true;
        }
        return m_topTree == c.tree || (m_topTree == 0 && (c.flags & presets::kOrUntalented) != 0);
    }
    u32 topTree() const { return m_topTree; }

private:
    static WornItem worn(i32 itemClass, i32 subclass) {
        WornItem w;
        w.itemClass = itemClass;
        w.subclass = std::max(subclass, 0);
        return w;
    }

    const PresetInput& m_in;
    std::vector<u64> m_names; // sorted
    u32 m_topTree = 0;
    std::array<WornItem, kWornSlots> m_worn{};
};

// The spells a layer shows already (by name): a later slot does not take them again.
class Taken {
public:
    bool has(u64 name) const {
        return std::find(m_names.begin(), m_names.begin() + m_count, name) != m_names.begin() + m_count;
    }
    void add(u64 name) {
        if (m_count < m_names.size() && !has(name)) {
            m_names[m_count++] = name;
        }
    }

private:
    std::array<u64, 64> m_names{};
    u32 m_count = 0;
};

bool sameAction(const TouchAction& a, const TouchAction& b) {
    return a.kind == b.kind && a.id == b.id && a.hidden() == b.hidden();
}

// One refill of a template from its preset (§7.1).
class Refill {
public:
    Refill(const Knowledge& know, const TemplateDef& def, TouchTemplate& t)
        : m_know(know), m_def(def), m_t(t), m_old(t.groups) {}

    void run() {
        // The class bar's groups first (the shaman's totems: every one of each element), then the layers (the base
        // first: a layer may copy one before it).
        for (u32 i = 0; i < presets::kClassBarKinds && i < kTouchClassBarGroups; ++i) {
            const GroupKind kind = m_def.classBar[i];
            if (kind != GroupKind::None) {
                const i32 g = groupIndex(resolveKind(kind, nullptr));
                m_t.classBarGroups[i] = g >= 0 ? static_cast<u8>(g + 1) : 0;
            }
        }
        for (const presets::LayerDef& layer : m_def.layers) {
            if (layer.table) {
                fillLayer(static_cast<u32>(layer.context), *layer.table);
            }
        }
        sweepGroups();
    }

private:
    struct KindState {
        GroupKind kind = GroupKind::None;
        u32 count = 0;
        std::array<u32, TouchGroup::kMaxActions> spells{};
        i32 group = -1; // the template's group holding it, -1 none yet
    };

    void addTaken(const TouchAction& a, Taken& taken) const {
        if (a.kind == TouchActionKind::Spell) {
            taken.add(m_know.nameOf(a.id));
        } else if (a.kind == TouchActionKind::Group && a.id < kTouchGroups) {
            const TouchGroup& g = m_t.groups[a.id];
            for (u32 i = 0; i < g.count && i < TouchGroup::kMaxActions; ++i) {
                if (g.actions[i].kind == TouchActionKind::Spell) {
                    taken.add(m_know.nameOf(g.actions[i].id));
                }
            }
        }
    }

    void fillLayer(u32 layer, const presets::Table& table) {
        m_form = bodyForm(static_cast<TouchContext>(layer));
        std::array<TouchAction, kTouchSlots>& slots = m_t.layers[layer];
        Taken taken;
        for (const TouchAction& a : slots) {
            if (a.manual()) {
                addTaken(a, taken); // the player's own: the presets do not put it twice
            }
        }
        for (const TouchPlace place : kFillOrder) {
            const u32 s = touchIndex(place);
            if (!slots[s].manual()) {
                slots[s] = resolveSlot(table[s], s, taken);
            }
        }
        if (layer == 0) {
            return;
        }
        // A layer keeps only what differs from the base (§5.2); a base action it would show twice is hidden there.
        const std::array<TouchAction, kTouchSlots>& base = m_t.layers[0];
        for (u32 s = 0; s < kTouchSlots; ++s) {
            TouchAction& a = slots[s];
            if (a.manual()) {
                continue;
            }
            if (!a.empty() && sameAction(a, base[s])) {
                a = {};
            } else if (a.empty() && !a.hidden() && base[s].kind == TouchActionKind::Spell &&
                       taken.has(m_know.nameOf(base[s].id))) {
                a.flags = kTouchHidden;
            }
        }
    }

    TouchAction resolveSlot(const presets::Slot& slot, u32 s, Taken& taken) {
        for (const Cand& c : slot.cands) {
            switch (c.kind) {
            case CandKind::None: return {}; // no candidate left: the base shows (in the base: nothing)
            case CandKind::Spell:
                if (m_know.treeAllows(c) && m_know.usable(c.id, m_form) && !taken.has(m_know.nameOf(c.id))) {
                    taken.add(m_know.nameOf(c.id));
                    return TouchAction::spell(c.id);
                }
                break;
            case CandKind::Group: {
                const GroupKind kind = static_cast<GroupKind>(c.id);
                KindState& state = resolveKind(kind, &taken);
                if (state.count == 0) {
                    break;
                }
                // One member: a plain button (no fan to open); two or more, or the class bar's: the group.
                const i32 g = state.count > 1 || classBarKind(kind) ? groupIndex(state) : -1;
                const TouchAction a = g >= 0 ? TouchAction{TouchActionKind::Group, 0, 0, static_cast<u32>(g)}
                                             : TouchAction::spell(state.spells[0]);
                addTaken(a, taken);
                return a;
            }
            case CandKind::Consumable: return TouchAction::consumable(static_cast<TouchConsumable>(c.id));
            case CandKind::SameAs: {
                TouchAction a = c.id < kTouchContexts ? m_t.layers[c.id][s] : TouchAction{};
                a.flags = static_cast<u8>(a.flags & ~kTouchManual);
                addTaken(a, taken);
                return a;
            }
            case CandKind::Hide: {
                TouchAction a;
                a.flags = kTouchHidden;
                return a;
            }
            }
        }
        return {};
    }

    bool classBarKind(GroupKind kind) const {
        return std::find(m_def.classBar.begin(), m_def.classBar.end(), kind) != m_def.classBar.end();
    }

    // A group kind's members the player has, resolved once a refill, by its first slot (what that layer's earlier
    // slots took is left out; `taken` null: all of them, the class bar's).
    KindState& resolveKind(GroupKind kind, const Taken* taken) {
        for (u32 i = 0; i < m_kindCount; ++i) {
            if (m_kinds[i].kind == kind) {
                return m_kinds[i];
            }
        }
        KindState& state = m_kindCount < m_kinds.size() ? m_kinds[m_kindCount++] : m_kinds.back();
        state = {};
        state.kind = kind;
        if (const GroupDef* def = presets::groupOf(kind)) {
            for (const presets::Member& member : def->members) {
                // The member's first spell the player has (Ice Armor, else Frost Armor; the race's racial).
                for (const u32 spell : member.spells) {
                    if (spell == 0 || !m_know.usable(spell, taken ? m_form : 0)) {
                        continue;
                    }
                    if ((!taken || !taken->has(m_know.nameOf(spell))) && state.count < state.spells.size()) {
                        state.spells[state.count++] = spell;
                    }
                    break;
                }
            }
        }
        return state;
    }

    // The template's group of `state`'s kind, filled with its members; -1 when the template has no group free.
    i32 groupIndex(KindState& state) {
        if (state.group >= 0) {
            return state.group;
        }
        for (u32 i = 0; i < kTouchGroups && state.group < 0; ++i) {
            const TouchGroup& g = m_t.groups[i];
            if (g.autoKind == static_cast<u8>(state.kind) && (g.flags & kTouchManual) == 0) {
                state.group = static_cast<i32>(i);
            }
        }
        for (u32 i = 0; i < kTouchGroups && state.group < 0; ++i) {
            const TouchGroup& g = m_t.groups[i];
            if (g.count == 0 && g.autoKind == 0 && g.flags == 0 && !referenced(i)) {
                state.group = static_cast<i32>(i);
            }
        }
        if (state.group >= 0) {
            write(static_cast<u32>(state.group), state);
        }
        return state.group;
    }

    void write(u32 index, const KindState& state) {
        // The current one (§6.6): kept when the player had chosen another than the first; else the new first (a
        // spell just learned that leads the list takes over, as the preset's default).
        const TouchGroup& old = m_old[index];
        u32 current = 0;
        if (old.autoKind == static_cast<u8>(state.kind) && old.current > 0 && old.current < old.count) {
            for (u32 i = 0; i < state.count; ++i) {
                current = state.spells[i] == old.actions[old.current].id ? i : current;
            }
        }
        TouchGroup& g = m_t.groups[index];
        g = {};
        g.autoKind = static_cast<u8>(state.kind);
        g.count = static_cast<u8>(state.count);
        g.current = static_cast<u8>(current);
        for (u32 i = 0; i < state.count; ++i) {
            g.actions[i] = TouchAction::spell(state.spells[i]);
        }
    }

    bool referenced(u32 group) const {
        for (const auto& layer : m_t.layers) {
            for (const TouchAction& a : layer) {
                if (a.kind == TouchActionKind::Group && a.id == group) {
                    return true;
                }
            }
        }
        return std::find(m_t.classBarGroups.begin(), m_t.classBarGroups.end(), group + 1) !=
               m_t.classBarGroups.end();
    }

    // The automatic groups no slot shows any more are freed; one only a manual slot shows (moved there by hand)
    // keeps refilling.
    void sweepGroups() {
        for (u32 i = 0; i < kTouchGroups; ++i) {
            TouchGroup& g = m_t.groups[i];
            if (g.autoKind == 0 || (g.flags & kTouchManual) != 0) {
                continue;
            }
            if (!referenced(i)) {
                g = {};
                continue;
            }
            bool refilled = false;
            for (u32 k = 0; k < m_kindCount; ++k) {
                refilled = refilled || m_kinds[k].group == static_cast<i32>(i);
            }
            if (!refilled) {
                KindState& state = resolveKind(static_cast<GroupKind>(g.autoKind), nullptr);
                state.group = static_cast<i32>(i);
                write(i, state);
            }
        }
    }

    // The form of a layer whose body changes (its spells must be castable in it), 0 for the others.
    static u32 bodyForm(TouchContext layer) {
        switch (layer) {
        case TouchContext::Cat:
        case TouchContext::Prowl: return kFormCat;
        case TouchContext::Bear: return kFormBear;
        case TouchContext::Travel: return kFormTravel;
        default: return 0;
        }
    }

    const Knowledge& m_know;
    const TemplateDef& m_def;
    TouchTemplate& m_t;
    u32 m_form = 0; // the layer being filled's (bodyForm)
    std::array<TouchGroup, kTouchGroups> m_old;
    std::array<KindState, 24> m_kinds{};
    u32 m_kindCount = 0;
};

// The "New" bits (§4): the skills that show another action in some layer (a base slot's in every layer inheriting
// it), or a group with a member it did not have.
PresetChanges compare(const TouchTemplate& before, const TouchTemplate& after) {
    PresetChanges changes;
    for (u32 c = 0; c < kTouchContexts; ++c) {
        for (u32 s = 0; s < kTouchSkills; ++s) {
            const TouchAction a = resolveTouchAction(after, static_cast<TouchContext>(c), s);
            if (a.empty()) {
                continue;
            }
            const TouchAction b = resolveTouchAction(before, static_cast<TouchContext>(c), s);
            bool changed = !sameAction(a, b);
            if (!changed && a.kind == TouchActionKind::Group && a.id < kTouchGroups) {
                const TouchGroup& now = after.groups[a.id];
                const TouchGroup& then = before.groups[a.id];
                for (u32 i = 0; i < now.count && !changed; ++i) {
                    bool had = false;
                    for (u32 j = 0; j < then.count; ++j) {
                        had = had || sameAction(then.actions[j], now.actions[i]);
                    }
                    changed = !had;
                }
            }
            if (changed) {
                changes.slots[c] |= static_cast<u16>(1u << s);
            }
        }
    }
    return changes;
}

const TemplateDef* definitionOf(const PresetInput& in, u8 role) {
    const TemplateDef* def = presets::templateOf(static_cast<PresetRole>(role));
    return def && def->cls == in.cls && in.text ? def : nullptr;
}

} // namespace

bool buildClassLayout(const PresetInput& in, TouchLayout& out) {
    out = {};
    if (!in.text) {
        return false;
    }
    const Knowledge know(in);
    for (const TemplateDef& def : presets::templates()) {
        if (def.cls != in.cls || out.count >= kMaxTouchTemplates) {
            continue;
        }
        TouchTemplate& t = out.templates[out.count++];
        t = makeTouchTemplate(text::get(def.nameKey));
        t.role = static_cast<u8>(def.role);
        t.disabledLayers = def.disabledLayers;
        t.healerFrames = presetRoleHeals(def.role); // phase 20 (assists)
        Refill(know, def, t).run();
    }
    // The template of the tree with the most points comes up first (a holy priest's "Heal").
    const u32 top = know.topTree();
    for (u32 i = 0; i < out.count && top != 0; ++i) {
        const TemplateDef* def = presets::templateOf(static_cast<PresetRole>(out.templates[i].role));
        if (def && std::find(def->trees.begin(), def->trees.end(), top) != def->trees.end()) {
            out.active = i;
            break;
        }
    }
    return out.count > 0;
}

PresetChanges refreshClassTemplate(const PresetInput& in, TouchTemplate& t) {
    const TemplateDef* def = definitionOf(in, t.role);
    if (!def) {
        return {};
    }
    const TouchTemplate before = t;
    const Knowledge know(in);
    Refill(know, *def, t).run();
    return compare(before, t);
}

u32 classPresetRoles(u32 cls, std::span<PresetRole> out) {
    u32 count = 0;
    for (const TemplateDef& def : presets::templates()) {
        if (def.cls == cls && count < out.size()) {
            out[count++] = def.role;
        }
    }
    return count;
}

std::string_view presetRoleNameKey(PresetRole role) {
    const TemplateDef* def = presets::templateOf(role);
    return def ? std::string_view(def->nameKey) : std::string_view{};
}

bool resetClassTemplate(const PresetInput& in, TouchTemplate& t) {
    const TemplateDef* def = definitionOf(in, t.role);
    if (!def) {
        return false;
    }
    t.layers = {};
    t.groups = {};
    t.classBarGroups = {};
    t.disabledLayers = def->disabledLayers;
    const Knowledge know(in);
    Refill(know, *def, t).run();
    return true;
}

bool presetRoleHeals(PresetRole role) { // phase 20 (assists)
    return role == PresetRole::DruidHeal || role == PresetRole::PriestHeal || role == PresetRole::PaladinHeal ||
           role == PresetRole::ShamanHeal;
}

u32 topTalentTree(const PresetInput& in) {
    if (!in.abilities) {
        return 0;
    }
    std::array<TalentTabInfo, 3> tabs{};
    const u32 tabCount = in.abilities->talentTabs(in.cls, tabs);
    std::array<u32, 3> points{};
    for (const u32 id : in.known) {
        u32 rank = 0;
        if (const TalentInfo* talent = in.abilities->talentOfSpell(id, rank)) {
            for (u32 i = 0; i < tabCount; ++i) {
                // The server keeps a talent's highest rank only (it supersedes the lower ones): its points.
                points[i] += tabs[i].id == talent->tab ? rank + 1 : 0;
            }
        }
    }
    u32 best = 0;
    for (u32 i = 1; i < tabCount; ++i) {
        best = points[i] > points[best] ? i : best;
    }
    return tabCount > 0 && points[best] > 0 ? tabs[best].id : 0;
}

void classDemoKnown(u32 cls, u32 race, u32 level, u32 talentTab, const AbilityData& abilities,
                    std::vector<u32>& out) {
    out.clear();
    out.push_back(kAttackSpell);
    const SpellText& text = abilities.spellText();
    const auto add = [&](u32 spell) {
        if (spell == 0 || std::find(out.begin(), out.end(), spell) != out.end()) {
            return;
        }
        u32 rank = 0;
        SpellDetails details;
        if (abilities.talentOfSpell(spell, rank) || !text.details(spell, details)) {
            return; // the talents come from the sample below
        }
        const u32 races = presets::raceMaskOf(spell);
        if (races != 0 && (race < 1 || race > 32 || (races & (1u << (race - 1))) == 0)) {
            return; // another race's
        }
        if (details.spellLevel <= level) {
            out.push_back(spell);
        }
    };
    for (const TemplateDef& def : presets::templates()) {
        for (u32 l = 0; def.cls == cls && l < def.layers.size(); ++l) {
            for (u32 s = 0; def.layers[l].table && s < kTouchSlots; ++s) {
                for (const Cand& c : (*def.layers[l].table)[s].cands) {
                    if (c.kind == CandKind::Spell) {
                        add(c.id);
                    }
                }
            }
        }
    }
    for (const GroupDef& group : presets::groups()) {
        const u32 owner = presets::groupClass(group.kind);
        for (u32 m = 0; (owner == cls || owner == 0) && m < group.members.size(); ++m) {
            for (const u32 spell : group.members[m].spells) {
                add(spell);
            }
        }
    }
    for (const u32 spell : presets::demoExtraSpells(cls)) {
        add(spell);
    }
    // The talents: one point a level from 10, spent from the tree's top row down: in each row the talents that are
    // spells to cast first (to their last rank), then the passive ones only as far as the next row needs.
    if (talentTab == 0 || level < 10) {
        return;
    }
    const u32 points = level - 9;
    const std::span<const TalentInfo> talents = abilities.talents(talentTab); // by row, then column
    std::array<u32, 32> taken{};                                              // ranks taken of each talent
    u32 spent = 0;
    for (u32 row = 0; spent < points && spent >= row * kTalentPointsPerRow; ++row) {
        bool rowExists = false;
        for (u32 i = 0; i < talents.size() && i < taken.size() && spent < points; ++i) {
            SpellDetails details;
            if (talents[i].row == row && text.details(talents[i].ranks[0], details) && !details.passive()) {
                taken[i] = std::min(talents[i].rankCount(), points - spent);
                spent += taken[i];
            }
            rowExists = rowExists || talents[i].row == row;
        }
        if (!rowExists) {
            break; // past the tree's last row
        }
        // Passive ranks, from the top row down, until the next row opens.
        for (u32 i = 0; i < talents.size() && i < taken.size() && talents[i].row <= row; ++i) {
            SpellDetails details;
            if (!text.details(talents[i].ranks[0], details) || !details.passive()) {
                continue;
            }
            while (taken[i] < talents[i].rankCount() && spent < points &&
                   spent < (row + 1) * kTalentPointsPerRow) {
                ++taken[i];
                ++spent;
            }
        }
    }
    for (u32 i = 0; i < talents.size() && i < taken.size(); ++i) {
        if (taken[i] > 0) {
            out.push_back(talents[i].ranks[taken[i] - 1]);
        }
    }
}

u32 talentTreeByName(u32 cls, std::string_view name, const AbilityData& abilities) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    std::array<TalentTabInfo, 3> tabs{};
    const u32 count = abilities.talentTabs(cls, tabs);
    for (u32 i = 0; i < count && !name.empty(); ++i) {
        // "beastmastery" or "beast" for "Beast Mastery": `name` starts the tree's name, its spaces left out.
        usize j = 0;
        for (const char c : tabs[i].name) {
            if (j == name.size()) {
                break;
            }
            if (c == ' ') {
                continue;
            }
            if (lower(c) != lower(name[j])) {
                break;
            }
            ++j;
        }
        if (j == name.size()) {
            return tabs[i].id;
        }
    }
    return 0;
}

} // namespace vd::game
