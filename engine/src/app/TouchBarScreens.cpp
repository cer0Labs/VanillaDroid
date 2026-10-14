#include "app/TouchBarScreens.h"

#include "app/CombatScreens.h"
#include "app/GameSession.h"
#include "app/Hud.h"
#include "app/HudShared.h"
#include "app/WtfFolders.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/ActionBar.h"
#include "game/GameData.h"
#include "game/Macros.h"
#include "game/Progress.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory> // phase 20 (edit)

namespace vd {

namespace uf = net::uf;
using game::Entity;
using game::TouchAction;
using game::TouchActionKind;
using game::TouchPlace;
using game::touchIndex;

namespace {

constexpr u32 kSaveDelayMs = 2000;   // a change is written this long after the last one
constexpr u32 kRepeatEveryMs = 250;  // §8: a held button asks at most this often
constexpr u32 kClassWarrior = 1;
constexpr u32 kClassRogue = 4;
constexpr u32 kClassHunter = 3;
constexpr u32 kClassDruid = 11;
constexpr u8 kSlotMainHand = 15; // EQUIPMENT_SLOT_MAINHAND, OFFHAND, RANGED
constexpr u8 kSlotOffHand = 16;
constexpr u8 kSlotRanged = 17;
constexpr u32 kAttrOnNextSwing = 0x00000004 | 0x00000400; // SPELL_ATTR_ON_NEXT_SWING_1 / _2
constexpr u32 kAuraModShapeshift = 36;

// The caches of CombatScreens::touchView: the twelve slots, then the fixed buttons, the fan, the class bar.
constexpr u32 kCacheMount = game::kTouchSlots;
constexpr u32 kCacheReactive = game::kTouchSlots + 1;
constexpr u32 kCacheFan = game::kTouchSlots + 2;                     // 8 of them
constexpr u32 kCacheClassBar = kCacheFan + kTouchFanItems;           // 6 of them
static_assert(kCacheClassBar + kTouchClassBarItems <= CombatScreens::kTouchViews);

constexpr u64 kIconTemplates = assetPathHash("Interface\\Icons\\INV_Misc_Gear_01.blp");
constexpr u64 kIconDrawer = assetPathHash("Interface\\Icons\\INV_Misc_Book_09.blp");

// The skills in the order of their roles (§7.1): what a template made from the server's bars takes first.
constexpr TouchPlace kRoleOrder[game::kTouchSkills] = {
    TouchPlace::Main,  TouchPlace::RingA,   TouchPlace::RingC,   TouchPlace::RingD,  TouchPlace::RingE,
    TouchPlace::RingB, TouchPlace::ColumnF, TouchPlace::ColumnG, TouchPlace::ColumnH};

std::string readText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

TouchAction actionOfButton(const game::ActionButton& b) {
    if (b.empty()) {
        return {};
    }
    if (const u32 macro = game::macroIdOf(b); macro != 0) {
        return {TouchActionKind::Macro, 0, 0, macro};
    }
    if (b.type == net::kActionSpell) {
        return TouchAction::spell(b.action);
    }
    if (b.type == net::kActionItem) {
        return TouchAction::item(b.action);
    }
    return {};
}

} // namespace

void TouchBarScreens::init(const game::GameData* data, const game::AbilityData* abilities, UiResources* resources) {
    m_data = data;
    m_abilities = abilities;
    m_resources = resources;
    m_smart.init(data, abilities);
    m_edit.init(resources);
    m_assists.init(data, abilities); // phase 20 (assists)
    m_smart.setFormReach(&TouchAssists::formReachable, &m_assists);
    m_pets.init(data, abilities); // phase 20 (pets)
    const char* trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY");
    m_trace = trace && *trace && *trace != '0';
    if (const char* demo = VD_DEV_ENV("VD_TOUCH_DEMO"); demo && *demo) {
        std::strncpy(m_demo.data(), demo, m_demo.size() - 1);
    }
}

void TouchBarScreens::setStorage(std::filesystem::path folder, FileWriter writer, void* context) {
    m_folder = std::move(folder);
    m_writer = writer;
    m_writerContext = context;
}

void TouchBarScreens::enterWorld(GameSession& session) {
    leaveWorld();
    m_smart.enterWorld(); // phase 20 (smart): another character's bags and spells
    m_assists.enterWorld(session); // phase 20 (assists): what waited for the last one
    m_pets.enterWorld(session); // phase 20 (pets): another character's pet
    m_layout = {};
    m_haveLayout = false;
    m_needBuild = false;
    m_file.clear();
    m_presetVersion = 0;
    m_newSlots = {};
    m_fanPlace = -1;
    m_hunterMelee = false;
    if (!m_folder.empty() && !session.characterName().empty()) {
        m_file = m_folder / "WTF" / "Account" / wtfFolderName(session.loginParams().account, true) /
                 wtfFolderName(session.realmName(), false) / wtfFolderName(session.characterName(), false) /
                 "touch-layout.txt";
        m_haveLayout = game::parseTouchLayout(readText(m_file), m_layout) && m_layout.count > 0;
    }
    // A character without a file: made from its class (or its bars) once its spells are known (they come after the
    // login).
    m_needBuild = !m_haveLayout;
    VD_LOG_INFO("Touch layout: %s", m_haveLayout ? "read" : "to be made");
}

void TouchBarScreens::leaveWorld() {
    if (m_dirty) {
        save();
    }
    m_dirty = false;
}

void TouchBarScreens::changed() {
    if (!m_dirty) {
        m_dirtySinceMs = m_nowMs;
    }
    m_dirty = true;
}

void TouchBarScreens::save() {
    m_dirty = false;
    if (!m_writer || m_file.empty() || !m_haveLayout) {
        return;
    }
    m_writer(m_writerContext, m_file, game::formatTouchLayout(m_layout));
    // phase 20 (edit): the server's copy (account data type 6) here
    if (m_saveHook) {
        m_saveHook(m_saveHookContext);
    }
}

void TouchBarScreens::selectTemplate(u32 index, u32 nowMs) {
    if (index >= m_layout.count || index == m_layout.active) {
        return;
    }
    m_layout.active = index;
    m_newSlots = {};
    m_fanPlace = -1;
    m_templateShownMs = nowMs;
    changed();
}

u32 TouchBarScreens::spellOf(const TouchAction& action) const {
    if (action.kind != TouchActionKind::Spell) {
        return 0;
    }
    return (action.flags & game::kTouchFixedRank) != 0 ? action.id : m_lines.highest(action.id);
}

game::PresetInput TouchBarScreens::presetInput(GameSession& session) const {
    game::PresetInput in;
    const Entity* me = session.entities().find(session.playerGuid());
    if (!me) {
        return in;
    }
    const game::Gameplay& play = session.play();
    in.race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
    in.cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
    in.level = me->value(uf::UNIT_FIELD_LEVEL);
    in.known = play.spells().known();
    in.text = m_abilities ? &m_abilities->spellText() : nullptr;
    in.abilities = m_abilities;
    in.data = m_data;
    const auto worn = [&](u8 slot, i32& cls, i32& subclass) {
        const game::ItemInstance* item = play.inventory().equipped(slot);
        const net::ItemQueryInfo* info = item && item->entry ? session.item(item->entry) : nullptr;
        if (info) {
            cls = static_cast<i32>(info->itemClass);
            subclass = static_cast<i32>(info->subclass);
        }
        in.weaponsKnown = in.weaponsKnown && (info || !item || item->empty()); // phase 20 (classes)
    };
    worn(kSlotMainHand, in.mainHandClass, in.mainHandSubclass);
    worn(kSlotOffHand, in.offHandClass, in.offHandSubclass);
    worn(kSlotRanged, in.rangedClass, in.rangedSubclass);
    return in;
}

void TouchBarScreens::fillFromBars(GameSession& session, game::TouchTemplate& t) const {
    // The first page of the 1.12 bars on the skills in the order of their roles (Attack left out: the interaction
    // button attacks), each bonus bar of the class (the warrior's stances, the rogue's Stealth, the druid's Cat and
    // Bear forms: their bars share slots) on its layer where it differs.
    const game::SpellBook& spells = session.play().spells();
    const Entity* me = session.entities().find(session.playerGuid());
    const u32 cls = me ? me->byteValue(uf::UNIT_FIELD_BYTES_0, 1) : 0;
    const auto classLayer = [cls](game::TouchContext context) {
        switch (context) {
        case game::TouchContext::BattleStance:
        case game::TouchContext::DefensiveStance:
        case game::TouchContext::BerserkerStance: return cls == kClassWarrior;
        case game::TouchContext::Stealth: return cls == kClassRogue;
        case game::TouchContext::Cat:
        case game::TouchContext::Bear: return cls == kClassDruid;
        default: return false;
        }
    };
    for (u32 c = 0; c < game::kTouchContexts; ++c) {
        const game::TouchContext context = static_cast<game::TouchContext>(c);
        const u32 bonus = game::touchContextBonusBar(context);
        if (c != 0 && (bonus == 0 || !classLayer(context))) {
            continue;
        }
        u32 n = 0;
        for (u32 b = 0; b < game::kActionBarButtons && n < game::kTouchSkills; ++b) {
            const game::ActionButton button = spells.button(game::mainBarSlot(0, b, bonus));
            if (button.empty() || (button.type == net::kActionSpell && button.action == game::kAttackSpell)) {
                continue;
            }
            const TouchAction action = actionOfButton(button);
            const u32 slot = touchIndex(kRoleOrder[n++]);
            if (c == 0 || !(t.layers[0][slot] == action)) {
                t.layers[c][slot] = action;
            }
        }
    }
    // The consumables: the player's health, its power (mana, rage, energy) and the bandages.
    const u32 power = me ? me->byteValue(uf::UNIT_FIELD_BYTES_0, 3) : 0;
    t.layers[0][touchIndex(TouchPlace::Consumable1)] = TouchAction::consumable(game::TouchConsumable::Health);
    t.layers[0][touchIndex(TouchPlace::Consumable2)] = TouchAction::consumable(
        power == 1 ? game::TouchConsumable::Rage
                   : power == 3 ? game::TouchConsumable::Energy : game::TouchConsumable::Mana);
    t.layers[0][touchIndex(TouchPlace::Consumable3)] = TouchAction::consumable(game::TouchConsumable::Bandage);
}

void TouchBarScreens::buildLayout(GameSession& session) {
    m_layout = {};
    const game::PresetInput in = presetInput(session);
    if (!game::buildClassLayout(in, m_layout) || m_layout.count == 0) {
        m_layout = {};
        m_layout.templates[0] = game::makeTouchTemplate(text::get("VD_TOUCH_TEMPLATE_MAIN"));
        m_layout.count = 1;
        fillFromBars(session, m_layout.templates[0]);
    }
    m_layout.active = 0;
    m_haveLayout = true;
    m_needBuild = false;
    m_presetVersion = session.play().spells().knownVersion();
    changed();
    VD_LOG_INFO("Touch layout made: %u template(s)", m_layout.count);
}

const TouchBarScreens::SpellFacts& TouchBarScreens::facts(u32 cache, u32 spellId) {
    SpellFacts& f = m_facts[cache % m_facts.size()];
    if (f.spellId == spellId) {
        return f;
    }
    f = {};
    f.spellId = spellId;
    if (const std::optional<game::SpellInfo> info = m_data ? m_data->spell(spellId) : std::nullopt) {
        f.hostile = info->targetKind() == game::SpellTargetKind::Enemy;
        bool form = false;
        for (u32 i = 0; i < 3; ++i) {
            form = form || info->effectAura[i] == kAuraModShapeshift;
        }
        game::SpellDetails details;
        const bool autoRepeat =
            m_abilities && m_abilities->spellText().details(spellId, details) && details.autoRepeat();
        f.toggle = spellId == game::kAttackSpell || autoRepeat || form || (info->attributes & kAttrOnNextSwing) != 0;
    }
    return f;
}

game::TouchGroup* TouchBarScreens::groupOf(const TouchAction& action) {
    if (action.kind != TouchActionKind::Group || action.id >= game::kTouchGroups || !m_haveLayout) {
        return nullptr;
    }
    game::TouchGroup& g = m_layout.current().groups[action.id];
    return g.count > 0 ? &g : nullptr;
}

void TouchBarScreens::actionView(GameSession& session, CombatScreens& combat, const TouchAction& action, u32 cache,
                                 u32 nowMs, TouchButtonView& out, u32 depth) {
    out = {};
    out.shown = !action.empty();
    switch (action.kind) {
    case TouchActionKind::Empty: break;
    case TouchActionKind::Spell: {
        const u32 spell = spellOf(action);
        out.slot = combat.touchView(session, {net::kActionSpell, spell}, cache, nowMs);
        if (!m_lines.knows(spell)) {
            out.slot.usable = false; // a talent unlearned, a rank of another class: kept, greyed
        }
        const SpellFacts& f = facts(cache, spell);
        out.onPress = f.hostile;
        out.repeat = f.hostile && !f.toggle;
        break;
    }
    case TouchActionKind::Item:
        out.slot = combat.touchView(session, {net::kActionItem, action.id}, cache, nowMs);
        out.onPress = true;
        break;
    case TouchActionKind::Macro: out.slot = combat.touchView(session, game::macroButton(action.id), cache, nowMs); break;
    case TouchActionKind::Group:
        if (const game::TouchGroup* g = groupOf(action); g && depth == 0) {
            actionView(session, combat, g->actions[std::min<u32>(g->current, g->count - 1u)], cache, nowMs, out,
                       depth + 1);
            out.group = true;
            out.onPress = false;
            out.repeat = false;
        } else {
            out.shown = false;
        }
        break;
    case TouchActionKind::PetAction:
        out.slot = combat.petActionView(session, action.id, nowMs);
        out.shown = out.slot.iconHash != 0;
        out.onPress = m_pets.firesOnPress(session, action.id); // phase 20 (pets): the Attack, an enemy's spell (§4)
        break;
    case TouchActionKind::Consumable: {
        const auto kind = static_cast<game::TouchConsumable>(action.id);
        if (const u32 item = m_smart.consumableItem(session, kind, nowMs); item != 0) {
            out.slot = combat.touchView(session, {net::kActionItem, item}, cache, nowMs);
        } else {
            out.slot.iconHash = m_smart.consumableIcon(kind); // none carried: the category's icon, faint
            out.slot.usable = false;
        }
        out.onPress = true;
        break;
    }
    case TouchActionKind::Special:
        switch (static_cast<game::TouchSpecial>(action.id)) {
        case game::TouchSpecial::Attack:
            out.slot = combat.touchView(session, {net::kActionSpell, game::kAttackSpell}, cache, nowMs);
            break;
        case game::TouchSpecial::Mount:
            if (const TouchAction mount = m_smart.mountAction(session, nowMs); !mount.empty() && depth == 0) {
                actionView(session, combat, mount, cache, nowMs, out, depth + 1);
            } else {
                out.shown = false;
            }
            break;
        case game::TouchSpecial::Drawer: out.slot.iconHash = kIconDrawer; break;
        default: out.shown = false; break; // phase 20 (pets): the pet's button; the forms' fan
        }
        break;
    }
}

void TouchBarScreens::fillClassBar(GameSession& session, CombatScreens& combat, u32 nowMs) {
    // §6.7: the template's groups when it sets them (the shaman's totems), else the player's forms (the stance bar).
    const game::TouchTemplate& t = m_layout.current();
    m_view.classBarCount = 0;
    if (t.classBarGroups[0] != 0) {
        for (u32 i = 0; i < game::kTouchClassBarGroups && m_view.classBarCount < kTouchClassBarItems; ++i) {
            if (t.classBarGroups[i] == 0) {
                continue;
            }
            const TouchAction group{TouchActionKind::Group, 0, 0, t.classBarGroups[i] - 1u};
            TouchButtonView& b = m_view.classBar[m_view.classBarCount];
            actionView(session, combat, group, kCacheClassBar + m_view.classBarCount, nowMs, b);
            if (b.shown) {
                ++m_view.classBarCount;
            }
        }
        return;
    }
    const std::span<const HudActionSlot> forms = combat.forms();
    for (u32 i = 0; i < forms.size() && m_view.classBarCount < kTouchClassBarItems; ++i) {
        // A form already on a button of the cluster (the rogue's Stealth) is not shown twice.
        bool onButton = false;
        for (u32 s = 0; s < game::kTouchSkills && !onButton; ++s) {
            onButton = m_view.places[s].shown && m_view.places[s].slot.iconHash == forms[i].iconHash;
        }
        if (onButton) {
            continue;
        }
        TouchButtonView& b = m_view.classBar[m_view.classBarCount++];
        b = {};
        b.shown = true;
        b.slot = forms[i];
        b.glow = false;
    }
}

void TouchBarScreens::fillFan(GameSession& session, CombatScreens& combat, u32 nowMs) {
    m_view.fanPlace = -1;
    m_view.fanCount = 0;
    if (m_fanPlace < 0) {
        return;
    }
    const game::TouchGroup* g = nullptr;
    if (m_fanPlace >= kTouchClassBarFan) {
        const u32 i = static_cast<u32>(m_fanPlace - kTouchClassBarFan);
        const game::TouchTemplate& t = m_layout.current();
        u32 n = 0;
        for (u32 k = 0; k < game::kTouchClassBarGroups && !g; ++k) {
            if (t.classBarGroups[k] != 0 && n++ == i) {
                g = groupOf({TouchActionKind::Group, 0, 0, t.classBarGroups[k] - 1u});
            }
        }
    } else if (static_cast<u32>(m_fanPlace) < game::kTouchSlots) {
        g = groupOf(m_resolved[static_cast<u32>(m_fanPlace)]);
    }
    if (!g) {
        return;
    }
    m_view.fanPlace = m_fanPlace;
    m_view.fanCurrent = g->current;
    for (u32 i = 0; i < g->count && i < kTouchFanItems; ++i) {
        TouchButtonView b;
        actionView(session, combat, g->actions[i], kCacheFan + i, nowMs, b, 1);
        m_view.fan[i] = b.slot;
        m_view.fanCount = i + 1;
    }
}

void TouchBarScreens::prepareHud(GameSession& session, HudFrame& hud, CombatScreens& combat, u32 nowMs) {
    VD_PROFILE_ZONE("TouchBarScreens::prepareHud");
    m_nowMs = nowMs;
    m_profile = hud.width >= hud.height * 1.6f ? game::TouchProfile::Wide : game::TouchProfile::Square;
    if (m_dirty && nowMs - m_dirtySinceMs >= kSaveDelayMs) {
        save();
    }
    if (!m_enabled || !session.inWorld()) {
        return;
    }
    game::Gameplay& play = session.play();
    const Entity* me = session.entities().find(session.playerGuid());
    if (!me) {
        return;
    }
    const game::SpellBook& spells = play.spells();
    if (m_needBuild && !spells.known().empty()) {
        buildLayout(session);
    }
    if (!m_haveLayout) {
        return;
    }
    if (m_abilities) {
        m_lines.update(spells.known(), m_abilities->spellText(), spells.knownVersion());
    }
    if (m_presetVersion != spells.knownVersion()) {
        // A spell learned, the talents reset: the presets' automatic slots and groups refilled ("New" on what came).
        m_presetVersion = spells.knownVersion();
        const game::PresetInput in = presetInput(session);
        bool any = false;
        for (u32 i = 0; i < m_layout.count; ++i) {
            const game::PresetChanges changes = game::refreshClassTemplate(in, m_layout.templates[i]);
            if (i == m_layout.active) {
                for (u32 c = 0; c < game::kTouchContexts; ++c) {
                    m_newSlots[c] |= changes.slots[c];
                }
            }
            any = any || changes.any();
        }
        if (any) {
            changed();
        }
    }
    m_smart.update(session, hud, m_layout.mount, nowMs);
    m_assists.update(session, *this, combat, nowMs); // phase 20 (assists): the answers, a spell waiting, the site
    m_pets.update(session, *this, nowMs); // phase 20 (pets): the pet's death, the last demon, the food eaten

    // The layer (§5.2).
    const game::TouchTemplate& t = m_layout.current();
    const u32 cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
    const u32 form = me->byteValue(uf::UNIT_FIELD_BYTES_1, 2);
    const bool prowling = form == game::kFormCat && (me->byteValue(uf::UNIT_FIELD_BYTES_1, 3) & game::kUnitByte1Creep);
    const u64 targetGuid = play.target();
    const Entity* target = targetGuid ? session.entities().find(targetGuid) : nullptr;
    if (cls == kClassHunter && t.layerEnabled(game::TouchContext::Melee)) {
        const auto reach = [](const Entity& e) {
            const u32 bits = e.value(uf::UNIT_FIELD_COMBATREACH);
            f32 value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        };
        const f32 gap = target ? game::distanceAt(*me, *target, nowMs) - reach(*target) - reach(*me) : 0.0f;
        m_hunterMelee = game::hunterMeleeLayer(m_hunterMelee, target && !game::isDead(*target), gap);
    } else {
        m_hunterMelee = false;
    }
    m_context = game::touchContextOf(form, prowling, m_hunterMelee);
    if (!t.layerEnabled(m_context)) {
        m_context = game::TouchContext::Base;
    }

    // The view.
    m_view = {};
    m_view.geometry = &t.geometry[static_cast<u32>(m_profile)];
    m_view.mirrored = m_mirrored;
    std::array<u32, game::kTouchSlots> visibleSpells{};
    for (u32 s = 0; s < game::kTouchSlots; ++s) {
        m_resolved[s] = game::resolveTouchAction(t, m_context, s);
        TouchButtonView& b = m_view.places[s];
        actionView(session, combat, m_resolved[s], s, nowMs, b);
        b.isNew = b.shown && (m_newSlots[static_cast<u32>(m_context)] & (1u << s)) != 0;
        visibleSpells[s] = m_resolved[s].kind == TouchActionKind::Spell ? spellOf(m_resolved[s]) : 0;
    }
    // The fixed buttons.
    m_mountAction = m_smart.mountAction(session, nowMs);
    if (!m_mountAction.empty()) {
        actionView(session, combat, m_mountAction, kCacheMount, nowMs, m_view.places[touchIndex(TouchPlace::Mount)]);
    }
    m_reactiveSpell = m_smart.reactiveSpell(session, visibleSpells, nowMs);
    if (m_reactiveSpell != 0) {
        TouchButtonView& b = m_view.places[touchIndex(TouchPlace::Reactive)];
        actionView(session, combat, TouchAction::spell(m_reactiveSpell, game::kTouchFixedRank), kCacheReactive,
                   nowMs, b);
        b.glow = true;
    }
    {
        TouchButtonView& b = m_view.places[touchIndex(TouchPlace::Templates)];
        b.shown = true;
        b.slot.iconHash = kIconTemplates;
        m_view.templateName = t.nameView();
        m_view.templateIndex = m_layout.active;
        m_view.templateCount = m_layout.count;
        m_view.templateShownMs = m_templateShownMs;
    }
    {
        TouchButtonView& b = m_view.places[touchIndex(TouchPlace::Drawer)];
        b.shown = true;
        b.slot.iconHash = kIconDrawer;
    }
    // phase 20 (pets): the pet's button here
    m_pets.fillPetButton(session, combat, *this, m_view, nowMs);
    fillClassBar(session, combat, nowMs);
    if (cls == kClassRogue || (cls == kClassDruid && (form == game::kFormCat))) {
        m_view.comboPoints = game::comboPoints(*me, targetGuid);
    }
    m_view.dimmed = !play.control().playerHasControl();
    // phase 20 (smart): after the views (the reactive glow on visible buttons...)
    m_smart.finishView(combat, m_resolved, m_view, nowMs);
    // phase 20 (pets): after the views: another unit controlled, no control, death (§5.2 rows 1-3); the assists
    // (aiming, stances, the automatic target) only on the player's own cluster.
    if (!m_edit.editing() && m_pets.takeOver(session, combat, *this, m_resolved, m_view, nowMs)) {
        m_reactiveSpell = 0; // the reactive place is the Exit's, the soulstone's
    } else {
        m_assists.finishView(session, *this, m_resolved, m_reactiveSpell, m_view, nowMs); // phase 20 (assists)
    }
    // phase 20 (edit): after the views (a spell being placed lights the slots...)
    m_edit.prepareHud(session, *this, combat, m_view, nowMs);
    // phase 20 (classes): after the views
    refreshPresetsOnWeapons(session);
    fillFan(session, combat, nowMs);
    m_smart.fillFan(session, combat, m_fanPlace, m_view, nowMs); // phase 20 (smart): the reactive button's fan
    m_pets.fillFan(session, combat, m_fanPlace, m_view, nowMs);  // phase 20 (pets): the demons' fan
    m_edit.prepareView(m_view);
    m_view.places[touchIndex(TouchPlace::Mount)].glyph = hud::kTouchGlyphMount; // the saddle (art/touch), any mount
    hud.touch = &m_view;
}

u64 TouchBarScreens::execute(GameSession& session, CombatScreens& combat, const TouchAction& action, u32 nowMs,
                             u32 depth, bool repeat) {
    switch (action.kind) {
    case TouchActionKind::Spell: return m_assists.cast(session, combat, spellOf(action), nowMs, repeat); // assists
    case TouchActionKind::Item: return combat.useButton(session, {net::kActionItem, action.id});
    case TouchActionKind::Macro: return combat.useButton(session, game::macroButton(action.id));
    case TouchActionKind::Group:
        if (const game::TouchGroup* g = groupOf(action); g && depth == 0) {
            return execute(session, combat, g->actions[std::min<u32>(g->current, g->count - 1u)], nowMs, depth + 1,
                           repeat);
        }
        return 0;
    case TouchActionKind::PetAction: combat.usePetAction(session, action.id); return 0;
    case TouchActionKind::Consumable:
        if (const u32 item = m_smart.consumableItem(session, static_cast<game::TouchConsumable>(action.id), nowMs);
            item != 0) {
            return combat.useButton(session, {net::kActionItem, item});
        }
        return 0;
    case TouchActionKind::Special:
        switch (static_cast<game::TouchSpecial>(action.id)) {
        case game::TouchSpecial::Attack: return combat.useButton(session, {net::kActionSpell, game::kAttackSpell});
        case game::TouchSpecial::Mount: m_smart.useMount(session, combat, nowMs); return 0;
        case game::TouchSpecial::Drawer: m_edit.toggleDrawer(); return 0;
        default: return 0;
        }
    default: return 0;
    }
}

u64 TouchBarScreens::actOnHud(GameSession& session, const HudOutput& hudOut, CombatScreens& combat, u32 nowMs) {
    const TouchBarOutput& out = hudOut.touch;
    if (!m_enabled || !session.inWorld() || !m_haveLayout) {
        return 0;
    }
    u64 aim = 0;
    m_assists.beginActions(out); // phase 20 (assists): the Target button's unit for a press without a target
    if (m_edit.actOnHud(session, *this, combat, out, nowMs, aim)) { // phase 20 (edit): the edit mode, a spell placed
        return aim;
    }
    if (out.swipe != 0 && m_layout.count > 1) { // §5.3: the previous / next template
        selectTemplate((m_layout.active + (out.swipe > 0 ? 1u : m_layout.count - 1u)) % m_layout.count, nowMs);
    }
    if (out.fanOpen >= 0) {
        m_fanPlace = out.fanOpen; // filled from the next frame on
    }
    if (out.fanPlace >= 0 && out.fanChosen >= 0) {
        // A group's action chosen: the current one from now on, and used (§6.6).
        game::TouchGroup* g = nullptr;
        if (out.fanPlace < kTouchClassBarFan && static_cast<u32>(out.fanPlace) < game::kTouchSlots) {
            g = groupOf(m_resolved[static_cast<u32>(out.fanPlace)]);
        } else if (out.fanPlace >= kTouchClassBarFan) {
            const game::TouchTemplate& t = m_layout.current();
            u32 n = 0;
            for (u32 k = 0; k < game::kTouchClassBarGroups && !g; ++k) {
                if (t.classBarGroups[k] != 0 && n++ == static_cast<u32>(out.fanPlace - kTouchClassBarFan)) {
                    g = groupOf({TouchActionKind::Group, 0, 0, t.classBarGroups[k] - 1u});
                }
            }
        }
        if (g && static_cast<u32>(out.fanChosen) < g->count) {
            if (g->current != static_cast<u8>(out.fanChosen)) {
                g->current = static_cast<u8>(out.fanChosen);
                changed();
            }
            aim = execute(session, combat, g->actions[g->current], nowMs, 1);
        }
        m_fanPlace = -1;
    }
    if (out.classBar >= 0) {
        const game::TouchTemplate& t = m_layout.current();
        if (t.classBarGroups[0] != 0) {
            u32 n = 0;
            for (u32 k = 0; k < game::kTouchClassBarGroups; ++k) {
                if (t.classBarGroups[k] != 0 && n++ == static_cast<u32>(out.classBar)) {
                    aim = execute(session, combat, {TouchActionKind::Group, 0, 0, t.classBarGroups[k] - 1u}, nowMs);
                }
            }
        } else {
            combat.useForm(session, static_cast<u32>(out.classBar));
        }
    }
    if (out.pressed >= 0) {
        const u32 p = static_cast<u32>(out.pressed);
        if (p < game::kTouchSlots) {
            m_newSlots[static_cast<u32>(m_context)] &= static_cast<u16>(~(1u << p));
            aim = execute(session, combat, m_resolved[p], nowMs);
            m_lastRepeatMs = nowMs;
            if (m_trace) {
                VD_LOG_INFO("Touch: %.*s (%u %u)", static_cast<int>(game::touchPlaceName(static_cast<TouchPlace>(p)).size()),
                            game::touchPlaceName(static_cast<TouchPlace>(p)).data(),
                            static_cast<u32>(m_resolved[p].kind), m_resolved[p].id);
            }
        } else if (p == touchIndex(TouchPlace::Mount)) {
            m_smart.useMount(session, combat, nowMs);
        } else if (p == touchIndex(TouchPlace::Reactive) && m_reactiveSpell != 0) {
            aim = m_assists.cast(session, combat, m_reactiveSpell, nowMs); // phase 20 (assists)
        } else if (p == touchIndex(TouchPlace::Templates)) {
            m_edit.openTemplates(); // §5.3: the list (the swipe goes to the previous / next one without it)
        } else if (p == touchIndex(TouchPlace::Drawer)) {
            m_edit.toggleDrawer();
        }
        // phase 20 (pets): the pet's button here
    }
    if (out.repeating >= 0 && static_cast<u32>(out.repeating) < game::kTouchSlots) {
        // §8: a held button fires again once it can: its cooldown and the global one over, no cast going on.
        const u32 p = static_cast<u32>(out.repeating);
        // Phase 20 (assists): one request on its way at a time, none for a while after a refusal (TouchRepeatGate).
        const HudActionSlot& slot = m_view.places[p].slot;
        if (nowMs - m_lastRepeatMs >= kRepeatEveryMs && m_assists.repeatReady(session, slot, nowMs)) {
            m_lastRepeatMs = nowMs;
            aim = execute(session, combat, m_resolved[p], nowMs, 0, true);
        }
    }
    // phase 20 (smart): presses and holds of its buttons here (a consumable's list, the mount's choice, the target's)
    const bool smartHeld = m_smart.actOnHud(session, *this, out, combat, aim); // a list instead of the tooltip
    // phase 20 (pets): the pet button (a tap, its panel, the demons' fan), the Exit, the death's buttons
    const bool petHeld = m_pets.actOnHud(session, *this, out, combat, nowMs, aim);
    // phase 20 (edit): presses here (a spell being placed goes into the slot tapped)
    if (out.held >= 0 && !smartHeld && !petHeld) {
        m_edit.showTooltip(out.held, out.heldRect);
    }
    // phase 20 (assists): the drag (aim) here
    m_assists.drag(session, out, nowMs); // a helpful spell onto a unit frame, a ground spell where it aims
    return aim;
}

void TouchBarScreens::draw(GameSession& session, CombatScreens& combat, DialogContext& c, u32 nowMs) {
    m_edit.draw(session, *this, combat, c, nowMs);
    m_smart.draw(session, *this, c, nowMs); // phase 20 (smart): the list of a consumable or the mount held
    m_assists.draw(c);                       // phase 20 (assists): the unit frame under a dragged spell
    m_pets.draw(session, *this, combat, c, nowMs); // phase 20 (pets): the pet's panel, Feed Pet's list
}

// phase 20 (classes): VD_TOUCH_DEMO=<class>:<level> from the presets here
void TouchBarScreens::prepareOffline(HudFrame& hud, u32 nowMs) {
    // VD_TOUCH_DEMO=<class>[:<layer>]: a sample cluster with some states (a cooldown, the global cooldown, a reactive
    // ability, combo points, a group, "New"), to look at without a server.
    m_nowMs = nowMs;
    if (m_demo[0] == '\0' || !m_enabled) {
        return;
    }
    if (prepareClassesDemo(hud, nowMs)) { // phase 20 (classes): <class>:<level>
        // phase 20 (assists): VD_TOUCH_ASSIST_DEMO: its buttons aim
        m_assists.prepareOffline(*this, m_classesLayout.current(), m_classesDemoLayer, m_view, nowMs);
        m_pets.prepareOffline(m_view, m_fanPlace, nowMs); // phase 20 (pets): VD_TOUCH_PETS_DEMO over it
        return;
    }
    m_profile = hud.width >= hud.height * 1.6f ? game::TouchProfile::Wide : game::TouchProfile::Square;
    if (m_edit.prepareOffline(*this, hud, m_view, nowMs)) { // phase 20 (edit): VD_TOUCH_EDIT_DEMO
        return;
    }
    const std::string_view demo(m_demo.data());
    const usize colon = demo.find(':');
    const std::string_view cls = demo.substr(0, colon);
    game::TouchContext layer = game::TouchContext::Base;
    if (colon != std::string_view::npos) {
        game::touchContextByName(demo.substr(colon + 1), layer);
    }
    const bool warrior = cls == "warrior";
    // Rogue: Sinister Strike, Eviscerate, Slice and Dice, Kidney Shot, Kick, Stealth, Adrenaline Rush, Evasion,
    // Vanish; in Stealth: Ambush, Cheap Shot, Garrote, Sap, Pick Pocket. Warrior: Heroic Strike, Mortal Strike, Rend,
    // Thunder Clap, Hamstring, Charge, Battle Shout, Retaliation, Bloodrage.
    static constexpr u32 kRogue[9] = {1752, 2098, 5171, 408, 1766, 1784, 13750, 5277, 1856};
    static constexpr u32 kRogueStealth[5] = {8676, 1833, 703, 6770, 921};
    static constexpr u32 kWarrior[9] = {78, 12294, 772, 6343, 1715, 100, 6673, 20230, 2687};
    const u32* base = warrior ? kWarrior : kRogue;
    const u32* over = layer == game::TouchContext::Stealth && !warrior ? kRogueStealth : nullptr;
    m_demoTemplate = game::makeTouchTemplate("Demo");
    m_view = {};
    m_view.geometry = &m_demoTemplate.geometry[static_cast<u32>(m_profile)];
    m_view.mirrored = m_mirrored;
    const auto icon = [&](u32 spell) -> u64 {
        const std::optional<game::SpellInfo> info = m_data ? m_data->spell(spell) : std::nullopt;
        return info ? info->iconHash : 0;
    };
    for (u32 i = 0; i < game::kTouchSkills; ++i) {
        const u32 slot = touchIndex(kRoleOrder[i]);
        TouchButtonView& b = m_view.places[slot];
        b.shown = true;
        b.slot.iconHash = icon(over && i < 5 ? over[i] : base[i]);
        b.onPress = true;
    }
    const f32 cycle = static_cast<f32>(nowMs % 8000) / 1000.0f;
    m_view.places[touchIndex(TouchPlace::RingA)].slot.cooldownLeft = 8.0f - cycle; // a cooldown
    m_view.places[touchIndex(TouchPlace::RingA)].slot.cooldownTotal = 8.0f;
    const f32 gcd = static_cast<f32>(nowMs % 3000) / 1000.0f;
    if (gcd < 1.5f) { // the global cooldown on the others
        for (const TouchPlace p : {TouchPlace::Main, TouchPlace::RingC, TouchPlace::RingD, TouchPlace::RingE}) {
            m_view.places[touchIndex(p)].slot.cooldownLeft = 1.5f - gcd;
            m_view.places[touchIndex(p)].slot.cooldownTotal = 1.5f;
        }
    }
    m_view.places[touchIndex(TouchPlace::RingB)].slot.inRange = false;
    m_view.places[touchIndex(TouchPlace::ColumnF)].group = true;
    m_view.places[touchIndex(TouchPlace::ColumnH)].isNew = true;
    m_view.places[touchIndex(TouchPlace::ColumnG)].slot.noMana = true;
    for (const TouchPlace p : {TouchPlace::Consumable1, TouchPlace::Consumable2, TouchPlace::Consumable3}) {
        TouchButtonView& b = m_view.places[touchIndex(p)];
        b.shown = true;
        b.slot.iconHash = m_smart.consumableIcon(p == TouchPlace::Consumable1   ? game::TouchConsumable::Health
                                                 : p == TouchPlace::Consumable2 ? game::TouchConsumable::Mana
                                                                                : game::TouchConsumable::Bandage);
        b.slot.count = p == TouchPlace::Consumable1 ? 5 : 0;
        b.slot.usable = p != TouchPlace::Consumable2;
    }
    TouchButtonView& mount = m_view.places[touchIndex(TouchPlace::Mount)];
    mount.shown = true;
    mount.slot.iconHash = assetPathHash("Interface\\Icons\\Ability_Mount_RidingHorse.blp");
    TouchButtonView& reactive = m_view.places[touchIndex(TouchPlace::Reactive)];
    reactive.shown = (nowMs / 3000) % 2 == 0;
    reactive.glow = true;
    reactive.slot.iconHash = icon(warrior ? 7384 : 14251); // Overpower / Riposte
    m_view.places[touchIndex(TouchPlace::Templates)].shown = true;
    m_view.places[touchIndex(TouchPlace::Templates)].slot.iconHash = kIconTemplates;
    m_view.places[touchIndex(TouchPlace::Drawer)].shown = true;
    m_view.places[touchIndex(TouchPlace::Drawer)].slot.iconHash = kIconDrawer;
    m_view.templateName = "Demo";
    m_view.templateCount = 2;
    m_view.comboPoints = warrior ? 0 : 3;
    if (warrior) { // the three stances
        static constexpr u32 kStances[3] = {2457, 71, 2458};
        for (u32 i = 0; i < 3; ++i) {
            m_view.classBar[i].shown = true;
            m_view.classBar[i].slot.iconHash = icon(kStances[i]);
            m_view.classBar[i].slot.checked = i == 0;
        }
        m_view.classBarCount = 3;
    }
    if (m_fanPlace >= 0 && m_fanPlace < kTouchClassBarFan) { // the group's fan: the warrior's shouts
        static constexpr u32 kShouts[4] = {6673, 1160, 5246, 1161};
        m_view.fanPlace = m_fanPlace;
        m_view.fanCount = 4;
        for (u32 i = 0; i < 4; ++i) {
            m_view.fan[i].iconHash = icon(kShouts[i]);
        }
    }
    hud.context = HudContextAction::Loot;
    m_smart.prepareOffline(hud, m_view, m_fanPlace, nowMs); // phase 20 (smart): VD_TOUCH_SMART_DEMO
    m_pets.prepareOffline(m_view, m_fanPlace, nowMs);       // phase 20 (pets): VD_TOUCH_PETS_DEMO
    m_view.places[touchIndex(TouchPlace::Mount)].glyph = hud::kTouchGlyphMount; // the saddle (art/touch), any mount
    hud.touch = &m_view;
}

void TouchBarScreens::actOffline(const HudOutput& out) {
    m_smart.actOffline(out.touch); // phase 20 (smart): VD_TOUCH_SMART_DEMO's lists
    m_pets.actOffline(out.touch);  // phase 20 (pets): VD_TOUCH_PETS_DEMO's panel
    m_edit.actOffline(*this, out.touch, m_nowMs); // phase 20 (edit): VD_TOUCH_EDIT_DEMO
    m_assists.actOffline(out.touch, m_nowMs);     // phase 20 (assists): VD_TOUCH_ASSIST_DEMO's drags
    if (out.touch.fanOpen >= 0) {
        m_fanPlace = out.touch.fanOpen;
    }
    if (out.touch.fanChosen >= 0 || out.touch.pressed >= 0) {
        m_fanPlace = -1;
    }
}

// phase 20 (edit)
bool TouchBarScreens::adoptLayout(std::string_view text) {
    // Parsed apart first: a text that is not ours leaves the layout as it is (on the heap: a login's, once).
    auto parsed = std::make_unique<game::TouchLayout>();
    if (!game::parseTouchLayout(text, *parsed) || parsed->count == 0) {
        return false;
    }
    adoptLayout(*parsed);
    return true;
}

void TouchBarScreens::adoptLayout(const game::TouchLayout& layout) {
    m_layout = layout;
    m_haveLayout = true;
    m_needBuild = false;
    m_presetVersion = 0; // the presets' automatic slots refreshed for this character's spells
    m_newSlots = {};
    m_fanPlace = -1;
}

void TouchBarScreens::drawOffline(DialogContext& c, ui::TextInput& input, const ui::Rect& cluster, u32 nowMs) {
    m_edit.setFrame(&input, cluster);
    m_edit.drawOffline(*this, c, nowMs);
}

} // namespace vd
