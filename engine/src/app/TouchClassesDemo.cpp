// Phase 20 (classes): the class presets in the running client. The presets refilled when the weapons change (as the
// foundation refills them when a spell is learned), and VD_TOUCH_DEMO=<class>:<level>[:...]: the presets' cluster
// for a sample of what a character of that class knows at that level, to look at every class without a server.
#include "app/GameSession.h"
#include "app/Hud.h"
#include "app/TouchBarScreens.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/AbilityData.h"
#include "game/GameData.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <charconv>
#include <string>

namespace vd {

namespace uf = net::uf;
using game::TouchAction;
using game::TouchActionKind;
using game::TouchContext;
using game::touchIndex;
using game::TouchPlace;

namespace {

constexpr u8 kSlotMainHand = 15, kSlotOffHand = 16, kSlotRanged = 17; // EQUIPMENT_SLOT_*

struct DemoClass {
    std::string_view name;
    u32 cls;
    u32 race;        // the demo's race unless one is given (ChrRaces)
    u32 tree;        // the talent tree it spends its points in unless another is given (TalentTab)
    i32 mainHand[2]; // item class and subclass worn (-1: nothing)
    i32 offHand[2];
    i32 ranged[2];
};
// A sample character of each class: a race that is not the only one with its racials, a tree, the weapons its
// presets ask for (a dagger for Ambush, a shield for Shield Bash and Shield Slam, a wand for Shoot, a bow).
constexpr DemoClass kDemoClasses[] = {
    {"warrior", 1, 1, 161, {2, 7}, {4, 6}, {2, 2}},    // human, Arms; sword and shield, a bow
    {"paladin", 2, 3, 381, {2, 5}, {-1, 0}, {-1, 0}},  // dwarf, Retribution; two-handed mace
    {"hunter", 3, 4, 363, {2, 1}, {-1, 0}, {2, 2}},    // night elf, Marksmanship; two-handed axe, a bow
    {"rogue", 4, 1, 181, {2, 15}, {2, 7}, {2, 16}},    // human, Combat; dagger and sword, thrown
    {"priest", 5, 1, 202, {2, 10}, {-1, 0}, {2, 19}},  // human, Holy; staff and wand
    {"shaman", 7, 2, 261, {2, 4}, {4, 6}, {-1, 0}},    // orc, Elemental; mace and shield
    {"mage", 8, 7, 61, {2, 10}, {-1, 0}, {2, 19}},     // gnome, Frost; staff and wand
    {"warlock", 9, 5, 302, {2, 10}, {-1, 0}, {2, 19}}, // undead, Affliction; staff and wand
    {"druid", 11, 4, 283, {2, 10}, {-1, 0}, {-1, 0}},  // night elf, Balance; staff
};
constexpr std::string_view kRaceNames[] = {"human",  "orc",    "dwarf", "nightelf",
                                           "undead", "tauren", "gnome", "troll"};

// The form a layer stands for (what its spells are greyed against in the demo).
u32 formOfLayer(TouchContext layer, bool direBear) {
    switch (layer) {
    case TouchContext::BattleStance: return game::kFormBattleStance;
    case TouchContext::DefensiveStance: return game::kFormDefensiveStance;
    case TouchContext::BerserkerStance: return game::kFormBerserkerStance;
    case TouchContext::Stealth: return game::kFormStealth;
    case TouchContext::Cat:
    case TouchContext::Prowl: return game::kFormCat;
    case TouchContext::Bear: return direBear ? game::kFormDireBear : game::kFormBear;
    case TouchContext::Moonkin: return game::kFormMoonkin;
    case TouchContext::Travel: return game::kFormTravel;
    case TouchContext::Shadowform: return game::kFormShadowform;
    default: return 0;
    }
}

} // namespace

void TouchBarScreens::refreshPresetsOnWeapons(GameSession& session) {
    VD_PROFILE_ZONE("TouchBarScreens::refreshPresetsOnWeapons");
    // §7.1: the automatic slots resolve again when the weapons change (Backstab with a dagger, Shield Bash with a
    // shield, Shoot with a wand), as when a spell is learned; also once a worn weapon's item query is answered.
    const game::Inventory& inventory = session.play().inventory();
    u64 key = kFnvOffset;
    for (const u8 slot : {kSlotMainHand, kSlotOffHand, kSlotRanged}) {
        const game::ItemInstance* item = inventory.equipped(slot);
        const u32 entry = item && !item->empty() ? item->entry : 0;
        const bool known = entry != 0 && session.item(entry) != nullptr;
        key = (key ^ (entry * 2u + (known ? 1u : 0u))) * kFnvPrime;
    }
    if (key == m_presetWeapons) {
        return;
    }
    const bool first = m_presetWeapons == 0;
    m_presetWeapons = key;
    if (first || !m_haveLayout) {
        return; // the layout was just read or made with these weapons
    }
    const game::PresetInput in = presetInput(session);
    bool any = false;
    for (u32 i = 0; i < m_layout.count; ++i) {
        const game::PresetChanges changes = game::refreshClassTemplate(in, m_layout.templates[i]);
        for (u32 c = 0; c < game::kTouchContexts && i == m_layout.active; ++c) {
            m_newSlots[c] |= changes.slots[c];
        }
        any = any || changes.any();
    }
    if (any) {
        changed();
    }
}

bool TouchBarScreens::prepareClassesDemo(HudFrame& hud, u32 nowMs) {
    // VD_TOUCH_DEMO=<class>:<level>[:<layer>][:<tree>|none][:<race>][:t<template>], any order after the level:
    // "rogue:30:stealth", "mage:60:fire", "priest:40:shadowform:shadow:t2", "warrior:12:defensive:dwarf".
    if (!m_classesDemoParsed) {
        m_classesDemoParsed = true;
        const std::string_view demo(m_demo.data());
        const usize colon = demo.find(':');
        const DemoClass* sample = nullptr;
        for (const DemoClass& c : kDemoClasses) {
            sample = demo.substr(0, colon) == c.name ? &c : sample;
        }
        u32 level = 0;
        const std::string_view rest =
            colon == std::string_view::npos ? std::string_view{} : demo.substr(colon + 1);
        const std::string_view levelText = rest.substr(0, rest.find(':'));
        if (!sample || !m_abilities || !m_data ||
            std::from_chars(levelText.data(), levelText.data() + levelText.size(), level).ec != std::errc{} ||
            level == 0) {
            return false; // the foundation's fixed sample ("rogue", "warrior:defensive")
        }
        level = std::min(level, 60u);
        u32 race = sample->race;
        u32 tree = sample->tree;
        i32 templateIndex = -1;
        usize at = levelText.size() + 1;
        while (at < rest.size()) {
            const usize end = std::min(rest.find(':', at), rest.size());
            const std::string_view token = rest.substr(at, end - at);
            at = end + 1;
            game::TouchContext layer;
            if (game::touchContextByName(token, layer)) {
                m_classesDemoLayer = layer;
            } else if (token == "none") {
                tree = 0;
            } else if (token.size() == 2 && token[0] == 't' && token[1] >= '1' && token[1] <= '6') {
                templateIndex = token[1] - '1';
            } else if (const auto r = std::find(std::begin(kRaceNames), std::end(kRaceNames), token);
                       r != std::end(kRaceNames)) {
                race = static_cast<u32>(r - std::begin(kRaceNames)) + 1;
            } else if (const u32 t = game::talentTreeByName(sample->cls, token, *m_abilities); t != 0) {
                tree = t;
            } else {
                VD_LOG_WARN("VD_TOUCH_DEMO: '%.*s' is no layer, talent tree, race or template",
                            static_cast<int>(token.size()), token.data());
            }
        }
        // The layout made a level before, refreshed at this one: what this level brings shows "New".
        game::PresetInput in;
        in.cls = sample->cls;
        in.race = race;
        in.text = &m_abilities->spellText();
        in.abilities = m_abilities;
        in.data = m_data;
        in.mainHandClass = sample->mainHand[0];
        in.mainHandSubclass = sample->mainHand[1];
        in.offHandClass = sample->offHand[0];
        in.offHandSubclass = sample->offHand[1];
        in.rangedClass = sample->ranged[0];
        in.rangedSubclass = sample->ranged[1];
        in.level = level > 1 ? level - 1 : level;
        game::classDemoKnown(in.cls, race, in.level, tree, *m_abilities, m_classesKnown);
        in.known = m_classesKnown;
        if (!game::buildClassLayout(in, m_classesLayout)) {
            return false;
        }
        in.level = level;
        game::classDemoKnown(in.cls, race, level, tree, *m_abilities, m_classesKnown);
        in.known = m_classesKnown;
        for (u32 i = 0; i < m_classesLayout.count; ++i) {
            const game::PresetChanges changes = game::refreshClassTemplate(in, m_classesLayout.templates[i]);
            if (i == m_classesLayout.active) {
                m_classesNew = changes.slots;
            }
        }
        if (templateIndex >= 0 && static_cast<u32>(templateIndex) < m_classesLayout.count) {
            m_classesLayout.active = static_cast<u32>(templateIndex);
            m_classesNew = {};
        }
        std::sort(m_classesKnown.begin(), m_classesKnown.end()); // as the spell book keeps them
        m_lines.update(m_classesKnown, m_abilities->spellText(), 1);
        m_classesFormCount = game::shapeshiftForms(m_classesKnown, m_abilities->spellText(), m_classesForms);
        m_classesDemo = true;
        // The layout in the log, to read every slot of every template.
        VD_LOG_INFO("VD_TOUCH_DEMO %.*s level %u race %u tree %u: %u spells, %u template(s), active %u",
                    static_cast<int>(sample->name.size()), sample->name.data(), level, race, tree,
                    static_cast<u32>(m_classesKnown.size()), m_classesLayout.count, m_classesLayout.active);
        const std::string text = game::formatTouchLayout(m_classesLayout);
        bool places = false;
        for (usize pos = 0; pos < text.size();) {
            const usize end = std::min(text.find('\n', pos), text.size());
            const std::string_view line(text.data() + pos, end - pos);
            pos = end + 1;
            // The places (each template's geometry) are left out.
            places = line.rfind("  geometry", 0) == 0 || (places && line.rfind("    ", 0) == 0);
            if (places) {
                continue;
            }
            const usize spellAt = line.find("spell ");
            u32 id = 0;
            if (spellAt != std::string_view::npos) {
                const std::string_view digits = line.substr(spellAt + 6);
                std::from_chars(digits.data(), digits.data() + digits.size(), id);
            }
            const std::string_view name =
                id ? m_abilities->spellText().name(m_lines.highest(id)) : std::string_view{};
            VD_LOG_INFO("  %.*s%s%.*s", static_cast<int>(line.size()), line.data(),
                        name.empty() ? "" : "  # ", static_cast<int>(name.size()), name.data());
        }
    }
    if (!m_classesDemo) {
        return false;
    }
    m_profile = hud.width >= hud.height * 1.6f ? game::TouchProfile::Wide : game::TouchProfile::Square;
    fillClassesDemoView(nowMs);
    hud.touch = &m_view;
    return true;
}

void TouchBarScreens::fillClassesDemoView(u32 nowMs) {
    VD_PROFILE_ZONE("TouchBarScreens::fillClassesDemoView");
    (void)nowMs;
    const game::TouchTemplate& t = m_classesLayout.current();
    const TouchContext layer = t.layerEnabled(m_classesDemoLayer) ? m_classesDemoLayer : TouchContext::Base;
    const bool direBear =
        std::any_of(m_classesKnown.begin(), m_classesKnown.end(), [](u32 id) { return id == 9634; });
    const u32 form = formOfLayer(layer, direBear);
    m_view = {};
    m_view.geometry = &t.geometry[static_cast<u32>(m_profile)];
    m_view.mirrored = m_mirrored;
    // A spell's icon (its highest rank known) and whether it can be cast in the layer's form.
    const auto spellView = [&](u32 spellId, HudActionSlot& slot) {
        const u32 spell = m_lines.highest(spellId);
        const std::optional<game::SpellInfo> info = m_data->spell(spell);
        slot.iconHash = info ? info->iconHash : 0;
        game::SpellDetails details;
        slot.usable =
            !m_abilities->spellText().details(spell, details) || m_abilities->castableInForm(details, form);
    };
    const auto actionView = [&](const TouchAction& a, TouchButtonView& b) {
        b = {};
        b.shown = !a.empty();
        switch (a.kind) {
        case TouchActionKind::Spell: spellView(a.id, b.slot); break;
        case TouchActionKind::Group:
            if (a.id < game::kTouchGroups && t.groups[a.id].count > 0) {
                const game::TouchGroup& g = t.groups[a.id];
                spellView(g.actions[std::min<u32>(g.current, g.count - 1u)].id, b.slot);
                b.group = true;
            } else {
                b.shown = false;
            }
            break;
        case TouchActionKind::Consumable:
            b.slot.iconHash = m_smart.consumableIcon(static_cast<game::TouchConsumable>(a.id));
            break;
        default: b.shown = false; break;
        }
    };
    for (u32 s = 0; s < game::kTouchSlots; ++s) {
        TouchButtonView& b = m_view.places[s];
        actionView(game::resolveTouchAction(t, layer, s), b);
        b.isNew = b.shown && (m_classesNew[static_cast<u32>(layer)] & (1u << s)) != 0;
    }
    for (const TouchPlace p : {TouchPlace::Templates, TouchPlace::Drawer}) {
        TouchButtonView& b = m_view.places[touchIndex(p)];
        b.shown = true;
        b.slot.iconHash =
            assetPathHash(p == TouchPlace::Templates ? "Interface\\Icons\\INV_Misc_Gear_01.blp"
                                                     : "Interface\\Icons\\INV_Misc_Book_09.blp");
    }
    m_view.templateName = t.nameView();
    m_view.templateIndex = m_classesLayout.active;
    m_view.templateCount = m_classesLayout.count;
    if (layer == TouchContext::Stealth || layer == TouchContext::Cat || layer == TouchContext::Prowl ||
        (layer == TouchContext::Base && t.role == static_cast<u8>(game::PresetRole::Rogue))) {
        m_view.comboPoints = 3;
    }
    // The class bar: the template's groups (the shaman's totems), else the forms.
    m_view.classBarCount = 0;
    if (t.classBarGroups[0] != 0) {
        for (u32 i = 0; i < game::kTouchClassBarGroups && m_view.classBarCount < kTouchClassBarItems; ++i) {
            if (t.classBarGroups[i] != 0) {
                actionView({TouchActionKind::Group, 0, 0, t.classBarGroups[i] - 1u},
                           m_view.classBar[m_view.classBarCount]);
                m_view.classBarCount += m_view.classBar[m_view.classBarCount].shown ? 1 : 0;
            }
        }
    } else {
        for (u32 i = 0; i < m_classesFormCount && m_view.classBarCount < kTouchClassBarItems; ++i) {
            TouchButtonView& b = m_view.classBar[m_view.classBarCount++];
            b = {};
            b.shown = true;
            spellView(m_classesForms[i].spellId, b.slot);
            b.slot.usable = true;
            b.slot.checked = m_classesForms[i].form == form ||
                             (form == game::kFormDireBear && m_classesForms[i].form == game::kFormBear);
        }
    }
    // The fan of a held group (its place, or the class bar's).
    m_view.fanPlace = -1;
    if (m_fanPlace >= 0) {
        const game::TouchGroup* g = nullptr;
        if (m_fanPlace >= kTouchClassBarFan) {
            u32 n = 0;
            for (u32 k = 0; k < game::kTouchClassBarGroups && !g; ++k) {
                if (t.classBarGroups[k] != 0 && n++ == static_cast<u32>(m_fanPlace - kTouchClassBarFan)) {
                    g = &t.groups[t.classBarGroups[k] - 1u];
                }
            }
        } else if (static_cast<u32>(m_fanPlace) < game::kTouchSlots) {
            const TouchAction a = game::resolveTouchAction(t, layer, static_cast<u32>(m_fanPlace));
            g = a.kind == TouchActionKind::Group && a.id < game::kTouchGroups ? &t.groups[a.id] : nullptr;
        }
        if (g && g->count > 0) {
            m_view.fanPlace = m_fanPlace;
            m_view.fanCurrent = g->current;
            for (u32 i = 0; i < g->count && i < kTouchFanItems; ++i) {
                spellView(g->actions[i].id, m_view.fan[i]);
                m_view.fanCount = i + 1;
            }
        }
    }
}

} // namespace vd
