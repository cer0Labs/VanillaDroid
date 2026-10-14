#include "app/TouchEditScreens.h"

#include "app/CombatScreens.h"
#include "app/GameSession.h"
#include "app/Hud.h"
#include "app/HudShared.h"
#include "app/InventoryWindows.h"
#include "app/MacroScreens.h"
#include "app/TouchBarScreens.h"
#include "app/UiResources.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/ActionBar.h"
#include "game/ClassPresets.h"
#include "game/GameData.h"
#include "game/Macros.h"
#include "game/SpellText.h"
#include "game/TouchLayoutEdit.h"
#include "game/UnitInfo.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace vd {

namespace uf = net::uf;
using game::TouchAction;
using game::TouchActionKind;
using game::TouchContext;
using game::touchIndex;
using game::TouchPlace;

namespace {

// CombatScreens::touchView's caches of the drawer's 72 slots (the cluster's own end at 28: slots, fixed buttons, the
// fan, the class bar).
constexpr u32 kCacheDrawer = 40;
static_assert(kCacheDrawer + kTouchDrawerSlots <= CombatScreens::kTouchViews);
constexpr u64 kRenameField = ui::widgetId("touchlist.rename.field");
constexpr u32 kEquipmentSlots = 19; // EQUIPMENT_SLOT_END: trinkets and other worn items with a "Use:"

// The fixed places' icons in the edit mode (they show there even when they have nothing to do).
constexpr u64 kIconReactive = assetPathHash("Interface\\Icons\\Ability_MeleeDamage.blp");
constexpr u64 kIconPet = assetPathHash("Interface\\Icons\\Ability_Hunter_BeastCall.blp");
constexpr u64 kIconMount = assetPathHash("Interface\\Icons\\Ability_Mount_RidingHorse.blp");
constexpr u64 kIconForms = assetPathHash("Interface\\Icons\\Ability_Warrior_OffensiveStance.blp");
constexpr u64 kIconTemplates = assetPathHash("Interface\\Icons\\INV_Misc_Gear_01.blp");
constexpr u64 kIconDrawer = assetPathHash("Interface\\Icons\\INV_Misc_Book_09.blp");
constexpr u64 kIconUnknown = assetPathHash("Interface\\Icons\\INV_Misc_QuestionMark.blp");

constexpr u32 kClassWarrior = 1;
constexpr u32 kClassHunter = 3;
constexpr u32 kClassRogue = 4;
constexpr u32 kClassPriest = 5;
constexpr u32 kClassShaman = 7;
constexpr u32 kClassDruid = 11;

// The spells whose names name the layers (Spell.dbc, as the client shows them): Battle, Defensive and Berserker
// Stance, Stealth, Cat Form, Prowl, Bear Form, Moonkin Form, Travel Form, Shadowform; a shaman's travel layer is
// Ghost Wolf's. The base and the hunter's melee range are ours.
constexpr u32 kLayerSpells[game::kTouchContexts] = {0, 2457, 71, 2458, 1784, 768, 5215, 5487, 24858, 783, 15473, 0};
constexpr u32 kGhostWolf = 2645;

// The layers of each class (§5.2), after the base.
std::span<const TouchContext> classLayers(u32 cls) {
    static constexpr TouchContext kWarrior[] = {TouchContext::BattleStance, TouchContext::DefensiveStance,
                                                TouchContext::BerserkerStance};
    static constexpr TouchContext kRogue[] = {TouchContext::Stealth};
    static constexpr TouchContext kDruid[] = {TouchContext::Cat, TouchContext::Prowl, TouchContext::Bear,
                                              TouchContext::Moonkin, TouchContext::Travel};
    static constexpr TouchContext kPriest[] = {TouchContext::Shadowform};
    static constexpr TouchContext kHunter[] = {TouchContext::Melee};
    static constexpr TouchContext kShaman[] = {TouchContext::Travel};
    switch (cls) {
    case kClassWarrior: return kWarrior;
    case kClassRogue: return kRogue;
    case kClassDruid: return kDruid;
    case kClassPriest: return kPriest;
    case kClassHunter: return kHunter;
    case kClassShaman: return kShaman;
    default: return {};
    }
}

bool layerHasOwn(const game::TouchTemplate& t, TouchContext layer) {
    for (const TouchAction& a : t.layers[static_cast<u32>(layer)]) {
        if (!a.empty() || a.hidden()) {
            return true;
        }
    }
    return false;
}

u64 mix(u64 key, u64 part) {
    return (key ^ part) * 1099511628211ull;
}

// VD_TOUCH_EDIT_DEMO's sample layout: the foundation's rogue (VD_TOUCH_DEMO) with Stealth, a group and consumables, or
// its warrior with a defensive layer; two templates. Their names are the sample's data (a player's names), not texts
// of the interface.
constexpr u32 kDemoRogue[9] = {1752, 2098, 5171, 408, 1766, 1784, 13750, 5277, 1856};
constexpr u32 kDemoRogueStealth[5] = {8676, 1833, 703, 6770, 921};
constexpr u32 kDemoRogueGroup[4] = {13877, 13750, 14177, 2983};
constexpr u32 kDemoWarrior[9] = {78, 12294, 772, 6343, 1715, 100, 6673, 20230, 2687};
constexpr u32 kDemoWarriorDefensive[3] = {6572, 72, 355};
constexpr u32 kDemoWarriorGroup[4] = {6673, 1160, 5246, 1161};
// More of the class for the picker: other spells, ranks of the filler (Sinister Strike, Heroic Strike), Attack.
constexpr u32 kDemoRogueKnown[] = {1757, 1758, 1759, 1760, 8621, 2983, 1776, 2094, 1725, 1966,
                                   1804, 2836, 8647, 1943, 1833, 6603, 2764, 3127, 674,  921};
constexpr u32 kDemoWarriorKnown[] = {284,  285, 1608, 11564, 2457,  71,  2458,  7384, 6572, 72,  355,
                                     1680, 845, 676,  694,   18499, 871, 23922, 6603, 2764, 3127};
constexpr TouchPlace kRoleOrder[game::kTouchSkills] = {TouchPlace::Main,    TouchPlace::RingA,   TouchPlace::RingC,
                                                       TouchPlace::RingD,   TouchPlace::RingE,   TouchPlace::RingB,
                                                       TouchPlace::ColumnF, TouchPlace::ColumnG, TouchPlace::ColumnH};

} // namespace

void TouchEditScreens::init(UiResources* resources) {
    m_resources = resources;
    m_entries.reserve(160);
    m_entryViews.reserve(160);
    m_subTabNames.reserve(8);
    m_names.reserve(64);
    if (const char* demo = VD_DEV_ENV("VD_TOUCH_EDIT_DEMO"); demo && *demo) {
        std::strncpy(m_demo.data(), demo, m_demo.size() - 1);
    }
}

void TouchEditScreens::openTemplates() {
    if (m_editing) {
        return;
    }
    m_listOpen = !m_listOpen;
    m_drawerOpen = false;
    m_menuRow = -1;
    m_restoreChecked = false;
    if (!m_listOpen) {
        stopRenaming();
    }
}

void TouchEditScreens::toggleDrawer() {
    if (m_editing) {
        return;
    }
    m_drawerOpen = !m_drawerOpen;
    if (m_drawerOpen) {
        m_listOpen = false;
        stopRenaming();
    }
}

void TouchEditScreens::showTooltip(i32 place, const ui::Rect& anchor) {
    if (place < 0 || static_cast<u32>(place) >= game::kTouchSlots) {
        return; // the fixed buttons' holds are theirs (the mount's choice, the target's fan: f20/smart)
    }
    m_tipPlace = place;
    m_tipAnchor = anchor;
    m_tipPending = true;
}

void TouchEditScreens::prepareView(TouchBarView& view) const {
    view.editing = m_editing;
    view.editSelected = m_editing ? m_selected : -1;
    view.editLayout = m_layoutMode;
    view.editPanel = m_editing ? m_panelRect : ui::Rect{};
}

bool TouchEditScreens::escape() {
    if (m_ask != Ask::None) {
        m_ask = Ask::None;
        return true;
    }
    if (m_renaming >= 0) {
        stopRenaming();
        return true;
    }
    if (m_picking) {
        if (m_ranks) {
            m_ranks = false;
        } else {
            m_picking = false;
            m_grouping = false;
        }
        return true;
    }
    if (m_editing) {
        m_closeWanted = true; // Back keeps what was done (Done); the bar applies it next frame
        return true;
    }
    if (m_listOpen || m_drawerOpen) {
        m_listOpen = false;
        m_drawerOpen = false;
        return true;
    }
    return false;
}

void TouchEditScreens::closeAll() {
    stopRenaming();
    m_listOpen = false;
    m_menuRow = -1;
    m_ask = Ask::None;
    m_editing = false;
    m_closeWanted = false;
    m_picking = false;
    m_grouping = false;
    m_ranks = false;
    m_selected = -1;
    m_panelRect = {};
    m_drawerOpen = false;
    m_tipPending = false;
    m_placingShown = false;
}

void TouchEditScreens::stopRenaming() {
    if (m_input && m_input->active() && m_input->field() == kRenameField) {
        m_input->end();
    }
    m_renaming = -1;
}

// --- The world's facts (the demo's offline) -------------------------------------------------------------------

std::span<const u32> TouchEditScreens::knownSpells(World world) const {
    if (world.session) {
        return world.session->play().spells().known();
    }
    return m_demoKnown;
}

u32 TouchEditScreens::playerClass(World world) const {
    if (world.session) {
        const game::Entity* me = world.session->entities().find(world.session->playerGuid());
        return me ? me->byteValue(uf::UNIT_FIELD_BYTES_0, 1) : 0;
    }
    return m_demoWarrior ? kClassWarrior : kClassRogue;
}

u32 TouchEditScreens::playerRace(World world) const {
    if (world.session) {
        const game::Entity* me = world.session->entities().find(world.session->playerGuid());
        return me ? me->byteValue(uf::UNIT_FIELD_BYTES_0, 0) : 0;
    }
    return 1; // the demo's human
}

bool TouchEditScreens::inCombat(World world) const {
    if (world.session) {
        const game::Entity* me = world.session->entities().find(world.session->playerGuid());
        return me && game::inCombat(*me);
    }
    return m_demoCombat;
}

std::string_view TouchEditScreens::layerName(TouchBarScreens& bar, World world, TouchContext context) const {
    if (context == TouchContext::Base) {
        return text::get("VD_TOUCH_EDIT_LAYER_BASE");
    }
    if (context == TouchContext::Melee) {
        return text::get("VD_TOUCH_EDIT_LAYER_MELEE");
    }
    u32 spell = kLayerSpells[static_cast<u32>(context)];
    if (context == TouchContext::Travel && playerClass(world) == kClassShaman) {
        spell = kGhostWolf;
    }
    const game::GameData* data = bar.gameData();
    const std::optional<game::SpellInfo> info = data && spell ? data->spell(spell) : std::nullopt;
    return info ? info->name : game::touchContextName(context);
}

std::string_view TouchEditScreens::placeName(TouchPlace place) {
    const u32 p = touchIndex(place);
    if (p >= touchIndex(TouchPlace::RingA) && p <= touchIndex(TouchPlace::RingE)) {
        return text::format(m_placeName, "VD_TOUCH_EDIT_PLACE_RING", static_cast<int>(p));
    }
    if (p >= touchIndex(TouchPlace::ColumnF) && p <= touchIndex(TouchPlace::ColumnH)) {
        return text::format(m_placeName, "VD_TOUCH_EDIT_PLACE_COLUMN",
                            static_cast<int>(p - touchIndex(TouchPlace::ColumnF) + 1));
    }
    if (p >= touchIndex(TouchPlace::Consumable1) && p <= touchIndex(TouchPlace::Consumable3)) {
        return text::format(m_placeName, "VD_TOUCH_EDIT_PLACE_CONSUMABLE",
                            static_cast<int>(p - touchIndex(TouchPlace::Consumable1) + 1));
    }
    switch (place) {
    case TouchPlace::Main: return text::get("VD_TOUCH_EDIT_PLACE_MAIN");
    case TouchPlace::Jump: return text::get("VD_TOUCH_EDIT_PLACE_JUMP");
    case TouchPlace::Mount: return text::get("VD_TOUCH_EDIT_PLACE_MOUNT");
    case TouchPlace::Reactive: return text::get("VD_TOUCH_EDIT_PLACE_REACTIVE");
    case TouchPlace::Interact: return text::get("VD_TOUCH_EDIT_PLACE_INTERACT");
    case TouchPlace::Target: return text::get("VD_TOUCH_EDIT_PLACE_TARGET");
    case TouchPlace::Templates: return text::get("VD_TOUCH_EDIT_TEMPLATES");
    case TouchPlace::Drawer: return text::get("ACTIONBAR_LABEL");
    case TouchPlace::Pet: return text::get("PET");
    case TouchPlace::ClassBar: return text::get("VD_TOUCH_EDIT_PLACE_CLASSBAR");
    default: return {};
    }
}

void TouchEditScreens::listLayers(TouchBarScreens& bar, World world) {
    const game::TouchTemplate& t = bar.layout().current();
    m_layerCount = 0;
    m_layers[m_layerCount++] = TouchContext::Base;
    for (const TouchContext c : classLayers(playerClass(world))) {
        m_layers[m_layerCount++] = c;
    }
    // Any other layer with actions of its own (a template from another class's preset, a hand-made file).
    for (u32 c = 1; c < game::kTouchContexts; ++c) {
        const TouchContext context = static_cast<TouchContext>(c);
        const bool listed =
            std::find(m_layers.begin(), m_layers.begin() + m_layerCount, context) != m_layers.begin() + m_layerCount;
        if (!listed && layerHasOwn(t, context)) {
            m_layers[m_layerCount++] = context;
        }
    }
    if (std::find(m_layers.begin(), m_layers.begin() + m_layerCount, m_layer) == m_layers.begin() + m_layerCount) {
        m_layer = TouchContext::Base;
    }
}

u64 TouchEditScreens::iconOf(TouchBarScreens& bar, World world, const TouchAction& action) const {
    const game::GameData* data = bar.gameData();
    switch (action.kind) {
    case TouchActionKind::Spell: {
        const std::optional<game::SpellInfo> info = data ? data->spell(bar.spellOf(action)) : std::nullopt;
        return info ? info->iconHash : kIconUnknown;
    }
    case TouchActionKind::Item: {
        const net::ItemQueryInfo* info = world.session ? world.session->item(action.id) : nullptr;
        return info && data ? data->itemIconHash(info->displayId) : kIconUnknown;
    }
    case TouchActionKind::Macro: {
        const MacroScreens* macros = world.combat ? world.combat->macros() : nullptr;
        const game::Macro* macro = macros ? macros->macro(action.id) : nullptr;
        return macro ? macro->iconHash : kIconUnknown;
    }
    case TouchActionKind::Group: {
        const game::TouchTemplate& t = bar.layout().current();
        if (action.id < game::kTouchGroups && t.groups[action.id].count > 0) {
            const game::TouchGroup& g = t.groups[action.id];
            return iconOf(bar, world, g.actions[std::min<u32>(g.current, g.count - 1u)]);
        }
        return kIconUnknown;
    }
    case TouchActionKind::PetAction:
        return world.session && world.combat
                   ? world.combat->petActionView(*world.session, action.id, world.session->lastUpdateMs()).iconHash
                   : kIconPet;
    case TouchActionKind::Consumable: return bar.smart().consumableIcon(static_cast<game::TouchConsumable>(action.id));
    case TouchActionKind::Special:
        switch (static_cast<game::TouchSpecial>(action.id)) {
        case game::TouchSpecial::Attack: {
            const std::optional<game::SpellInfo> info = data ? data->spell(game::kAttackSpell) : std::nullopt;
            return info ? info->iconHash : hud::kIconAttack;
        }
        case game::TouchSpecial::Interact: return hud::kIconAttack;
        case game::TouchSpecial::Mount: return kIconMount;
        case game::TouchSpecial::Pet: return kIconPet;
        case game::TouchSpecial::Forms: return kIconForms;
        case game::TouchSpecial::Drawer: return kIconDrawer;
        default: return kIconUnknown;
        }
    default: return 0;
    }
}

game::ActionButton TouchEditScreens::buttonOf(TouchBarScreens& bar, World world, const TouchAction& action,
                                              u32 nowMs) const {
    switch (action.kind) {
    case TouchActionKind::Spell: return {net::kActionSpell, bar.spellOf(action)};
    case TouchActionKind::Item: return {net::kActionItem, action.id};
    case TouchActionKind::Macro: return game::macroButton(action.id);
    case TouchActionKind::Group: {
        const game::TouchTemplate& t = bar.layout().current();
        if (action.id < game::kTouchGroups && t.groups[action.id].count > 0) {
            const game::TouchGroup& g = t.groups[action.id];
            return buttonOf(bar, world, g.actions[std::min<u32>(g.current, g.count - 1u)], nowMs);
        }
        return {};
    }
    case TouchActionKind::Special:
        return static_cast<game::TouchSpecial>(action.id) == game::TouchSpecial::Attack
                   ? game::ActionButton{net::kActionSpell, game::kAttackSpell}
                   : game::ActionButton{};
    default: return {}; // consumables and the mount are f20/smart's (their holds list the candidates)
    }
}

ui::Rect TouchEditScreens::sideArea(const DialogContext& c, f32 maxWidthDp) const {
    // The side of the screen away from the cluster: its windows never cover the buttons they edit.
    const ui::Rect& safe = c.safeArea;
    const f32 gap = c.ui.px(6.0f);
    const f32 least = std::min(safe.w, c.ui.px(280.0f));
    f32 x0 = safe.x + gap, x1 = safe.right() - gap;
    if (m_cluster.w > 0.0f) {
        if (!m_mirrored) {
            x1 = std::max(x0 + least, m_cluster.x - gap);
        } else {
            x0 = std::min(x1 - least, m_cluster.right() + gap);
        }
    } else if (!m_mirrored) {
        x1 = safe.centerX();
    } else {
        x0 = safe.centerX();
    }
    const f32 w = std::min(x1 - x0, c.ui.px(maxWidthDp));
    return {m_mirrored ? x1 - w : x0, safe.y + gap, w, safe.h - 2.0f * gap};
}

bool TouchEditScreens::canRestore(TouchBarScreens& bar, World world) {
    // Built once when the list opens: the class's presets for the player now (game/ClassPresets, f20/classes).
    if (!world.session) {
        return false;
    }
    m_scratch = {};
    if (!game::buildClassLayout(bar.presetInput(*world.session), m_scratch) || m_scratch.count == 0) {
        return false;
    }
    return true;
}

// --- The edit mode --------------------------------------------------------------------------------------------

void TouchEditScreens::openEdit(TouchBarScreens& bar, World world, i32 place, bool picker) {
    if (!bar.haveLayout() || inCombat(world)) {
        return;
    }
    m_editing = true;
    m_closeWanted = false;
    m_listOpen = false;
    m_drawerOpen = false;
    m_menuRow = -1;
    stopRenaming();
    m_backup = bar.layout().current();
    m_layer = bar.layout().current().layerEnabled(bar.context()) ? bar.context() : TouchContext::Base;
    m_layoutMode = false;
    m_selected = place;
    m_picking = false;
    m_grouping = false;
    m_ranks = false;
    m_pinching = false;
    m_panelRect = {};
    listLayers(bar, world);
    if (picker && place >= 0 && static_cast<u32>(place) < game::kTouchSlots) {
        openPicker(bar, world, static_cast<u32>(place));
    }
}

void TouchEditScreens::closeEdit(TouchBarScreens& bar, bool keep) {
    if (!keep) {
        bar.layout().current() = m_backup; // Cancel: as it was when the mode opened
        bar.changed();
    }
    bar.saveNow(); // §9: saved at once (the server's copy waits its 30 s)
    m_editing = false;
    m_closeWanted = false;
    m_picking = false;
    m_grouping = false;
    m_ranks = false;
    m_selected = -1;
    m_pinching = false;
    m_panelRect = {};
}

void TouchEditScreens::openPicker(TouchBarScreens& bar, World world, u32 slot) {
    m_picking = true;
    m_pickSlot = slot;
    m_grouping = false;
    m_ranks = false;
    m_groupCount = 0;
    m_entriesKey = 0;
    // The tab of what the slot holds (a consumable slot: the specials).
    bool inherited = false;
    const TouchAction shown = game::touchSlotShown(bar.layout().current(), m_layer, slot, inherited);
    switch (shown.kind) {
    case TouchActionKind::Item: m_tab = TouchPickerTab::Items; break;
    case TouchActionKind::Macro: m_tab = TouchPickerTab::Macros; break;
    case TouchActionKind::PetAction: m_tab = TouchPickerTab::Pet; break;
    case TouchActionKind::Consumable:
    case TouchActionKind::Special: m_tab = TouchPickerTab::Special; break;
    default: m_tab = slot >= game::kTouchSkills ? TouchPickerTab::Special : TouchPickerTab::Spells; break;
    }
    m_subTab = 0;
    if (shown.kind == TouchActionKind::Spell && bar.abilityData()) {
        m_book.update(*bar.abilityData(), knownSpells(world), static_cast<u8>(playerRace(world)),
                      static_cast<u8>(playerClass(world)), false);
        // The page its spell is on (by name: the book lists the highest rank, the slot may keep another).
        const std::string_view name = bar.abilityData()->spellText().name(shown.id);
        for (u32 tab = 0; tab < m_book.tabs().size(); ++tab) {
            for (const game::SpellBookEntry& e : m_book.entries(tab)) {
                m_subTab = e.name == name ? tab : m_subTab;
            }
        }
    }
}

void TouchEditScreens::editView(TouchBarScreens& bar, World world, TouchBarView& view, u32 nowMs) {
    VD_PROFILE_ZONE("TouchEditScreens::editView");
    const game::TouchTemplate& t = bar.layout().current();
    view.editInherited = 0;
    view.editHidden = 0;
    for (u32 s = 0; s < game::kTouchSlots; ++s) {
        bool inherited = false;
        const TouchAction a = game::touchSlotShown(t, m_layer, s, inherited);
        TouchButtonView& b = view.places[s];
        b = {};
        if (inherited) {
            view.editInherited |= static_cast<u16>(1u << s);
        }
        if (a.hidden()) {
            view.editHidden |= static_cast<u16>(1u << s);
            continue;
        }
        if (a.empty()) {
            continue;
        }
        if (world.session && world.combat) {
            bar.viewOf(*world.session, *world.combat, a, s, nowMs, b);
            // Only what it is: no cooldowns, glows or badges while editing.
            const HudActionSlot shown = b.slot;
            b.slot = {};
            b.slot.iconHash = shown.iconHash != 0 ? shown.iconHash : iconOf(bar, world, a);
            b.slot.label = shown.label;
            b.slot.count = shown.count;
        } else {
            b.slot.iconHash = iconOf(bar, world, a);
            b.group = a.kind == TouchActionKind::Group;
        }
        b.shown = true;
        b.glow = false;
        b.isNew = false;
        b.group = a.kind == TouchActionKind::Group;
    }
    // Every fixed place shows, with what it is.
    const auto fixed = [&](TouchPlace place, u64 icon) {
        TouchButtonView& b = view.places[touchIndex(place)];
        const u64 own = b.slot.iconHash;
        b = {};
        b.shown = true;
        b.slot.iconHash = own != 0 ? own : icon;
    };
    fixed(TouchPlace::Jump, hud::kIconJump);
    fixed(TouchPlace::Target, hud::kIconNextTarget);
    fixed(TouchPlace::Interact, hud::kIconAttack);
    fixed(TouchPlace::Mount, kIconMount);
    fixed(TouchPlace::Reactive, kIconReactive);
    fixed(TouchPlace::Templates, kIconTemplates);
    fixed(TouchPlace::Drawer, kIconDrawer);
    fixed(TouchPlace::Pet, kIconPet);
    fixed(TouchPlace::ClassBar, view.classBarCount > 0 ? view.classBar[0].slot.iconHash : kIconForms);
}

bool TouchEditScreens::applyEdit(TouchBarScreens& bar, World world, const TouchBarOutput& out) {
    game::TouchTemplate& t = bar.layout().current();
    const game::TouchProfile profile = bar.profile();
    game::TouchGeometry& g = t.geometry[static_cast<u32>(profile)];
    const game::TouchArea area{out.editAreaLeft, out.editAreaRight, out.editAreaBottom, out.editAreaTop};
    bool changed = false;
    if (m_closeWanted) {
        closeEdit(bar, true);
        return true;
    }
    if (m_picking) {
        return true; // the picker takes the frame (the cluster under it is only shown)
    }
    if (out.editMoved >= 0 && static_cast<u32>(out.editMoved) < game::kTouchPlaces) {
        m_selected = out.editMoved;
        changed = game::moveTouchPlace(g, static_cast<TouchPlace>(out.editMoved), out.editX, out.editY, area);
    }
    if (out.editTapped >= 0 && out.editDropOn >= 0) {
        // Actions: a slot dropped on another swaps what they show in the layer edited.
        game::swapTouchSlots(t, m_layer, static_cast<u32>(out.editTapped), static_cast<u32>(out.editDropOn));
        m_selected = out.editDropOn;
        changed = true;
    } else if (out.editTapped >= 0) {
        m_selected = out.editTapped;
        if (!m_layoutMode && static_cast<u32>(out.editTapped) < game::kTouchSlots) {
            openPicker(bar, world, static_cast<u32>(out.editTapped));
        }
    }
    // Two fingers: the selected place's size, else the whole cluster's scale (the layout mode).
    if (out.editPinch > 0.0f && m_layoutMode) {
        if (!m_pinching) {
            m_pinching = true;
            m_pinchStart = m_selected >= 0
                               ? game::touchPlaceSizePercent(g, profile, static_cast<TouchPlace>(m_selected))
                               : g.scale * 100.0f;
        }
        if (m_selected >= 0) {
            game::resizeTouchPlace(g, profile, static_cast<TouchPlace>(m_selected), m_pinchStart * out.editPinch);
        } else {
            game::setTouchScale(g, m_pinchStart * out.editPinch / 100.0f);
        }
        changed = true;
    } else if (out.editPinch <= 0.0f) {
        m_pinching = false;
    }
    if (out.held >= 0 && static_cast<u32>(out.held) < game::kTouchSlots) {
        m_tipPlace = out.held;
        m_tipAnchor = out.heldRect;
        m_tipPending = true;
        m_selected = out.held;
    }
    if (changed) {
        bar.changed();
    }
    return true;
}

void TouchEditScreens::prepareHud(GameSession& session, TouchBarScreens& bar, CombatScreens& combat, TouchBarView& view,
                                  u32 nowMs) {
    VD_PROFILE_ZONE("TouchEditScreens::prepareHud");
    m_mirrored = view.mirrored;
    const World world{&session, &combat};
    if (session.playerGuid() != m_player) {
        m_player = session.playerGuid();
        closeAll(); // another character: what was open was the other one's
    }
    if (m_editing && inCombat(world)) {
        closeEdit(bar, true); // §9: no editing in combat; a fight that starts keeps what was done
    }
    if (m_editing) {
        editView(bar, world, view, nowMs);
        return;
    }
    m_placingShown = !combat.placing().empty();
    if (m_placingShown) {
        // The spell book's "Place on bar" (or a macro picked up): every content slot of the layer in use lights up; a
        // tap puts it there (actOnHud), as manual.
        for (u32 s = 0; s < game::kTouchSlots; ++s) {
            TouchButtonView& b = view.places[s];
            b.shown = true;
            b.slot.highlight = true;
            b.onPress = false;
            b.repeat = false;
            b.group = false;
            b.aim = false;
        }
    }
}

bool TouchEditScreens::actOnHud(GameSession& session, TouchBarScreens& bar, CombatScreens& combat,
                                const TouchBarOutput& out, u32 nowMs, u64& aim) {
    (void)nowMs;
    const World world{&session, &combat};
    aim = std::exchange(m_drawerAim, 0);
    if (m_editing) {
        return applyEdit(bar, world, out);
    }
    const game::ActionButton placing = combat.placing();
    if (m_placingShown && !placing.empty() && out.pressed >= 0 && static_cast<u32>(out.pressed) < game::kTouchSlots) {
        TouchAction action;
        if (const u32 macro = game::macroIdOf(placing); macro != 0) {
            action = {TouchActionKind::Macro, 0, 0, macro};
        } else if (placing.type == net::kActionSpell) {
            action = TouchAction::spell(placing.action);
        } else if (placing.type == net::kActionItem) {
            action = TouchAction::item(placing.action);
        }
        if (!action.empty() && bar.haveLayout()) {
            game::putTouchAction(bar.layout().current(), bar.context(), static_cast<u32>(out.pressed), action);
            bar.changed();
            VD_LOG_INFO("Touch: %u (kind %u) placed on slot %d of layer %u", action.id, static_cast<u32>(action.kind),
                        out.pressed, static_cast<u32>(bar.context()));
        }
        combat.stopPlacing();
        m_placingShown = false;
        return true;
    }
    return false;
}

// --- The windows ----------------------------------------------------------------------------------------------

void TouchEditScreens::draw(GameSession& session, TouchBarScreens& bar, CombatScreens& combat, DialogContext& c,
                            u32 nowMs) {
    VD_PROFILE_ZONE("TouchEditScreens::draw");
    const World world{&session, &combat};
    if (!bar.haveLayout() || !bar.enabled()) {
        closeAll(); // the classic bars chosen: nothing of the cluster stays open
        return;
    }
    drawWindows(bar, world, c, nowMs);
    // A held cluster button: its tooltip (the action bars' own, CombatScreens), "Edit" opening the edit mode on it; in
    // the edit mode a spell's "Fixed Rank".
    if (m_tipPending && m_tipPlace >= 0) {
        m_tipPending = false;
        bool inherited = false;
        const TouchAction action =
            m_editing ? game::touchSlotShown(bar.layout().current(), m_layer, static_cast<u32>(m_tipPlace), inherited)
                      : bar.resolved(static_cast<u32>(m_tipPlace));
        const game::ActionButton button = buttonOf(bar, world, action, nowMs);
        m_tipEditAction = !m_editing;
        m_tipEditPlace = m_tipPlace;
        std::string_view act;
        if (m_editing) {
            act = action.kind == TouchActionKind::Spell ? text::get("VD_TOUCH_EDIT_FIXED_RANK") : std::string_view{};
        } else if (!inCombat(world)) {
            act = text::get("VD_ACTIONBAR_EDIT");
        }
        combat.showTouchTooltip(button, m_tipAnchor, act);
    }
    combat.drawTouchTooltip(session, c);
    if (combat.takeTouchTooltipAction() && m_tipEditPlace >= 0) {
        if (m_tipEditAction) {
            openEdit(bar, world, m_tipEditPlace, true);
        } else if (m_editing) {
            openPicker(bar, world, static_cast<u32>(m_tipEditPlace));
            m_ranks = true;
        }
    }
}

void TouchEditScreens::drawWindows(TouchBarScreens& bar, World world, DialogContext& c, u32 nowMs) {
    if (m_listOpen) {
        drawList(bar, world, c, nowMs);
    }
    if (m_editing) {
        if (m_picking) {
            drawPicker(bar, world, c, nowMs);
        } else {
            drawPanel(bar, world, c);
        }
    }
    if (m_drawerOpen) {
        drawDrawer(bar, world, c, nowMs);
    }
    if (m_ask != Ask::None) {
        drawAsk(bar, world, c);
    }
}

void TouchEditScreens::drawList(TouchBarScreens& bar, World world, DialogContext& c, u32 nowMs) {
    game::TouchLayout& layout = bar.layout();
    if (!m_restoreChecked) {
        m_restoreChecked = true;
        m_restorable = canRestore(bar, world);
    }
    for (u32 i = 0; i < layout.count; ++i) {
        m_rows[i] = {layout.templates[i].nameView(), i == layout.active};
    }
    const bool typing = m_renaming >= 0 && m_input && m_input->active() && m_input->field() == kRenameField;
    if (typing) {
        m_renameText.set(m_input->text());
    }
    TouchTemplateListView v;
    v.rows = {m_rows.data(), layout.count};
    v.menuRow = m_menuRow;
    v.renaming = m_renaming;
    v.renameText = m_renameText.view();
    v.canAdd = layout.count < game::kMaxTouchTemplates;
    v.canDelete = layout.count > 1;
    const bool fighting = inCombat(world);
    v.canEdit = !fighting;
    v.canRestore = m_restorable && !fighting;
    v.note = fighting ? text::get("ERR_NOT_IN_COMBAT") : std::string_view{};
    v.area = sideArea(c, 10000.0f);
    // Beside the cluster's upper half, where the templates' button is.
    v.anchor = m_cluster.w > 0.0f ? ui::Rect{m_cluster.x, m_cluster.y, m_cluster.w, m_cluster.h * 0.5f} : ui::Rect{};
    if (m_mirrored && v.anchor.w > 0.0f) {
        v.anchor = {m_cluster.x, m_cluster.y, m_cluster.w, m_cluster.h * 0.5f};
    }
    const TouchTemplateListResult r = touchTemplateList(c, v);
    const auto copyOf = [&](u32 index) -> i32 {
        game::TouchTemplate copy = layout.templates[index];
        std::array<char, game::kTouchNameBytes> name{};
        game::uniqueTouchTemplateName(layout, copy.nameView(), name);
        copy.setName(name.data());
        const i32 added = game::addTouchTemplate(layout, copy);
        if (added >= 0) {
            bar.changed();
        }
        return added;
    };
    const auto startRenaming = [&](u32 index) {
        m_renaming = static_cast<i32>(index);
        m_menuRow = -1;
        m_renameText.set(layout.templates[index].nameView());
        if (m_input) {
            m_input->begin(kRenameField, m_renameText.view(), game::kTouchNameBytes - 1, ui::TextKind::Plain);
        }
    };
    if (m_renaming >= 0) {
        const bool submitted = typing && m_input->submitted();
        if ((r.renameDone || submitted) && !m_renameText.empty() && static_cast<u32>(m_renaming) < layout.count) {
            layout.templates[static_cast<u32>(m_renaming)].setName(m_renameText.view());
            bar.changed();
            stopRenaming();
        } else if ((typing && m_input->cancelled()) || r.closed) {
            stopRenaming();
        } else if (r.renameField && !typing && m_input) {
            m_input->begin(kRenameField, m_renameText.view(), game::kTouchNameBytes - 1, ui::TextKind::Plain);
        }
        if (!r.closed) {
            return;
        }
    }
    if (r.select >= 0) {
        bar.selectTemplate(static_cast<u32>(r.select), nowMs);
        m_listOpen = false;
    } else if (r.menu >= 0) {
        m_menuRow = m_menuRow == r.menu ? -1 : r.menu;
    } else if (r.rename >= 0) {
        startRenaming(static_cast<u32>(r.rename));
    } else if (r.duplicate >= 0) {
        copyOf(static_cast<u32>(r.duplicate));
        m_menuRow = -1;
    } else if (r.remove >= 0) {
        m_ask = Ask::Delete;
        m_askIndex = r.remove;
        m_menuRow = -1;
    } else if (r.edit) {
        openEdit(bar, world, -1, false);
    } else if (r.add) {
        // §5.3 "Nueva…": a copy of the one in use, chosen, its name typed.
        if (const i32 added = copyOf(layout.active); added >= 0) {
            bar.selectTemplate(static_cast<u32>(added), nowMs);
            startRenaming(static_cast<u32>(added));
        }
    } else if (r.restore) {
        m_ask = Ask::Restore;
        m_askIndex = static_cast<i32>(layout.active);
    } else if (r.copyLayout) {
        m_ask = Ask::CopyLayout;
        m_askIndex = static_cast<i32>(layout.active);
    } else if (r.closed) {
        m_listOpen = false;
        m_menuRow = -1;
    }
}

void TouchEditScreens::drawAsk(TouchBarScreens& bar, World world, DialogContext& c) {
    (void)world;
    game::TouchLayout& layout = bar.layout();
    if (m_askIndex < 0 || static_cast<u32>(m_askIndex) >= layout.count) {
        m_ask = Ask::None;
        return;
    }
    const u32 index = static_cast<u32>(m_askIndex);
    char question[192];
    std::string_view yes = text::get("ACCEPT");
    switch (m_ask) {
    case Ask::Delete:
        text::format(question, "VD_TOUCH_EDIT_DELETE_ASK", layout.templates[index].nameView());
        yes = text::get("DELETE");
        break;
    case Ask::Restore: text::format(question, "VD_TOUCH_EDIT_RESTORE_ASK", layout.templates[index].nameView()); break;
    default: text::format(question, "VD_TOUCH_EDIT_COPY_ASK"); break;
    }
    const ConfirmResult r = confirmDialog(c, question, yes, text::get("CANCEL"));
    if (r.yes) {
        bool done = false;
        switch (m_ask) {
        case Ask::Delete: done = game::removeTouchTemplate(layout, index); break;
        case Ask::Restore: done = game::restoreTouchTemplate(layout.templates[index], m_scratch); break;
        default:
            game::copyTouchGeometryToAll(layout, index);
            done = true;
            break;
        }
        if (done) {
            bar.changed();
        }
    }
    if (r.yes || r.no) {
        m_ask = Ask::None;
    }
}

void TouchEditScreens::drawPanel(TouchBarScreens& bar, World world, DialogContext& c) {
    game::TouchTemplate& t = bar.layout().current();
    const game::TouchProfile profile = bar.profile();
    game::TouchGeometry& g = t.geometry[static_cast<u32>(profile)];
    u32 current = 0;
    for (u32 i = 0; i < m_layerCount; ++i) {
        m_layerViews[i] = {layerName(bar, world, m_layers[i]), layerHasOwn(t, m_layers[i]),
                           t.layerEnabled(m_layers[i])};
        current = m_layers[i] == m_layer ? i : current;
    }
    TouchEditPanelView v;
    v.templateName = t.nameView();
    v.wide = profile == game::TouchProfile::Wide;
    v.layers = {m_layerViews.data(), m_layerCount};
    v.layer = current;
    v.layout = m_layoutMode;
    if (m_selected >= 0 && static_cast<u32>(m_selected) < game::kTouchPlaces) {
        const TouchPlace place = static_cast<TouchPlace>(m_selected);
        v.selected = placeName(place);
        v.size = game::touchPlaceSizePercent(g, profile, place);
        v.tooSmall = g.places[static_cast<u32>(m_selected)].size * g.scale < game::kTouchTooSmallMm;
    }
    v.scale = g.scale * 100.0f;
    v.hint = text::get(m_layoutMode ? "VD_TOUCH_EDIT_HINT_LAYOUT" : "VD_TOUCH_EDIT_HINT_ACTIONS");
    v.area = sideArea(c, 380.0f);
    v.site = t.site; // phase 20 (assists)
    v.healerFrames = t.healerFrames;
    const TouchEditPanelResult r = touchEditPanel(c, v);
    m_panelRect = r.panel;
    bool changed = false;
    if (r.layer >= 0 && static_cast<u32>(r.layer) < m_layerCount) {
        m_layer = m_layers[static_cast<u32>(r.layer)];
    }
    if (r.toggleLayer && m_layer != TouchContext::Base) {
        t.disabledLayers ^= static_cast<u16>(1u << static_cast<u32>(m_layer));
        changed = true;
    }
    if (r.mode >= 0) {
        m_layoutMode = r.mode == 1;
    }
    if (r.size >= 0.0f && m_selected >= 0) {
        game::resizeTouchPlace(g, profile, static_cast<TouchPlace>(m_selected), r.size);
        changed = true;
    }
    if (r.scale >= 0.0f) {
        game::setTouchScale(g, r.scale / 100.0f);
        changed = true;
    }
    if (r.reset) {
        game::resetTouchGeometry(g, profile); // the places of the screen edited
        changed = true;
    }
    if (r.site >= 0) { // phase 20 (assists)
        t.site = static_cast<u8>(r.site);
        changed = true;
    }
    if (r.toggleHealer) {
        t.healerFrames = !t.healerFrames;
        changed = true;
    }
    if (changed) {
        bar.changed();
    }
    if (r.done) {
        closeEdit(bar, true);
    } else if (r.cancel) {
        closeEdit(bar, false);
    }
}

void TouchEditScreens::buildEntries(TouchBarScreens& bar, World world) {
    const game::GameData* data = bar.gameData();
    const game::AbilityData* abilities = bar.abilityData();
    // Rebuilt when what the tab lists changed.
    u64 key = mix(mix(1469598103934665603ull, static_cast<u64>(m_tab) + 1), m_subTab);
    const std::span<const u32> known = knownSpells(world);
    if (world.session) {
        game::Gameplay& play = world.session->play();
        key = mix(key, play.spells().knownVersion());
        key = mix(key, play.inventory().version());
        key = mix(key, world.session->queryVersion());
        key = mix(key, play.abilities().pet().guid);
        MacroScreens* macros = world.combat ? world.combat->macros() : nullptr;
        key = mix(key, macros ? macros->store().version() : 0);
    } else {
        key = mix(key, known.size());
    }
    if (key == m_entriesKey) {
        return;
    }
    m_entriesKey = key;
    m_entries.clear();
    m_entryViews.clear();
    m_subTabNames.clear();
    m_names.clear();
    const auto add = [&](const TouchAction& action, u64 icon, std::string_view name, std::string_view detail) {
        m_entries.push_back({action, icon});
        m_entryViews.push_back({icon, name, detail, false});
    };
    switch (m_tab) {
    case TouchPickerTab::Spells: {
        if (!abilities) {
            break;
        }
        // The spell book's pages and their spells at their highest rank (the slot casts the highest it knows).
        m_book.update(*abilities, known, static_cast<u8>(playerRace(world)), static_cast<u8>(playerClass(world)),
                      false);
        const std::span<const game::SpellBookTab> tabs = m_book.tabs();
        for (const game::SpellBookTab& tab : tabs) {
            m_subTabNames.push_back(tab.kind == game::SpellTabKind::General ? text::get("GENERAL") : tab.name);
        }
        m_subTab = tabs.empty() ? 0 : std::min<u32>(m_subTab, static_cast<u32>(tabs.size()) - 1);
        for (const game::SpellBookEntry& e : m_book.entries(m_subTab)) {
            if (e.passive) {
                continue;
            }
            const std::optional<game::SpellInfo> info = data ? data->spell(e.spellId) : std::nullopt;
            add(TouchAction::spell(e.spellId), info ? info->iconHash : kIconUnknown, e.name, e.rank);
        }
        break;
    }
    case TouchPickerTab::Items: {
        if (!world.session) {
            break;
        }
        GameSession& session = *world.session;
        const game::Entity* me = session.entities().find(session.playerGuid());
        // The bags' items and the worn ones with a "Use:" (or that start a quest), one entry per kind of item.
        const auto consider = [&](u32 entry) {
            if (entry == 0) {
                return;
            }
            for (const Entry& seen : m_entries) {
                if (seen.action.id == entry) {
                    return;
                }
            }
            const net::ItemQueryInfo* info = session.item(entry);
            if (!info) {
                return; // asked for: listed when known (queryVersion)
            }
            bool usable = info->startQuest != 0;
            for (const net::ItemQueryInfo::Spell& spell : info->spells) {
                usable = usable || (spell.id != 0 && spell.trigger == net::kItemSpellTriggerOnUse);
            }
            if (usable) {
                add(TouchAction::item(entry), data ? data->itemIconHash(info->displayId) : kIconUnknown, info->name,
                    {});
            }
        };
        for (u8 slot = 0; slot < kEquipmentSlots; ++slot) {
            const game::ItemInstance* worn = session.play().inventory().equipped(slot);
            consider(worn ? worn->entry : 0);
        }
        if (me) {
            game::forEachInventoryItem(session.entities(), *me,
                                       [&](const game::InventoryItem& item) { consider(item.entry); });
        }
        break;
    }
    case TouchPickerTab::Macros: {
        MacroScreens* macros = world.combat ? world.combat->macros() : nullptr;
        if (!macros) {
            break;
        }
        for (u32 i = 0; i < game::kMacroSlots; ++i) {
            if (const game::Macro* macro = macros->store().slot(i)) {
                add({TouchActionKind::Macro, 0, 0, macro->id}, macro->iconHash, macro->name, {});
            }
        }
        break;
    }
    case TouchPickerTab::Pet: {
        if (!world.session || !world.combat) {
            break;
        }
        // The pet's spells, commands and reactions (§6.8: Spell Lock, Seduction... anywhere in the cluster). Phase 20
        // (pets): by spell, not by slot of the bar (app/TouchPetScreens: they follow the pet that knows them).
        std::array<TouchPetScreens::PickerEntry, 24> pets;
        const u32 count = bar.pets().pickerEntries(*world.session, *world.combat, pets);
        for (u32 i = 0; i < count; ++i) {
            add(pets[i].action, pets[i].icon, pets[i].name, {});
        }
        break;
    }
    default: {
        // The client's own buttons and the automatic consumables (§5.1, §6.1).
        const std::optional<game::SpellInfo> attack = data ? data->spell(game::kAttackSpell) : std::nullopt;
        add(TouchAction::special(game::TouchSpecial::Attack), attack ? attack->iconHash : hud::kIconAttack,
            text::get("ATTACK"), {});
        add(TouchAction::special(game::TouchSpecial::Mount), kIconMount, text::get("VD_TOUCH_EDIT_PLACE_MOUNT"), {});
        add(TouchAction::special(game::TouchSpecial::Drawer), kIconDrawer, text::get("ACTIONBAR_LABEL"), {});
        static constexpr const char* kConsumables[static_cast<u32>(game::TouchConsumable::Count)] = {
            "VD_TOUCH_EDIT_CONSUMABLE_HEALTH", "VD_TOUCH_EDIT_CONSUMABLE_MANA",    "VD_TOUCH_EDIT_CONSUMABLE_RAGE",
            "VD_TOUCH_EDIT_CONSUMABLE_ENERGY", "VD_TOUCH_EDIT_CONSUMABLE_BANDAGE", "VD_TOUCH_EDIT_CONSUMABLE_FOOD",
            "VD_TOUCH_EDIT_CONSUMABLE_DRINK"};
        for (u32 k = 0; k < static_cast<u32>(game::TouchConsumable::Count); ++k) {
            const auto kind = static_cast<game::TouchConsumable>(k);
            add(TouchAction::consumable(kind), bar.smart().consumableIcon(kind), text::get(kConsumables[k]), {});
        }
        break;
    }
    }
}

void TouchEditScreens::buildRanks(TouchBarScreens& bar, World world) {
    // §5.4 "Rango fijo": the ranks the player knows of the slot's spell; the first entry keeps the highest.
    bool inherited = false;
    const TouchAction shown = game::touchSlotShown(bar.layout().current(), m_layer, m_pickSlot, inherited);
    const game::AbilityData* abilities = bar.abilityData();
    const game::GameData* data = bar.gameData();
    const std::span<const u32> known = knownSpells(world);
    const u64 key = mix(mix(mix(0x52414E4Bull, shown.id), shown.flags), known.size());
    if (key == m_entriesKey) {
        return;
    }
    m_entriesKey = key;
    m_entries.clear();
    m_entryViews.clear();
    m_subTabNames.clear();
    m_names.clear();
    if (shown.kind != TouchActionKind::Spell || !abilities) {
        return;
    }
    m_allRanks.update(*abilities, known, static_cast<u8>(playerRace(world)), static_cast<u8>(playerClass(world)), true);
    const std::string_view name = abilities->spellText().name(shown.id);
    const bool fixed = (shown.flags & game::kTouchFixedRank) != 0;
    const std::optional<game::SpellInfo> info = data ? data->spell(shown.id) : std::nullopt;
    const u64 icon = info ? info->iconHash : kIconUnknown;
    m_entries.push_back({TouchAction::spell(shown.id), icon});
    m_entryViews.push_back({icon, name, text::get("VD_TOUCH_EDIT_HIGHEST_RANK"), !fixed});
    for (u32 tab = 0; tab < m_allRanks.tabs().size(); ++tab) {
        for (const game::SpellBookEntry& e : m_allRanks.entries(tab)) {
            if (e.name == name && !e.passive) {
                m_entries.push_back({TouchAction::spell(e.spellId, game::kTouchFixedRank), icon});
                m_entryViews.push_back({icon, e.name, e.rank, fixed && e.spellId == shown.id});
            }
        }
    }
}

void TouchEditScreens::drawPicker(TouchBarScreens& bar, World world, DialogContext& c, u32 nowMs) {
    game::TouchTemplate& t = bar.layout().current();
    if (m_ranks) {
        buildRanks(bar, world);
    } else {
        buildEntries(bar, world);
    }
    // The group being made: its actions marked in the list, and as a strip.
    if (m_grouping) {
        for (u32 i = 0; i < m_entries.size(); ++i) {
            bool in = false;
            for (u32 k = 0; k < m_groupCount; ++k) {
                in = in || (m_group[k].kind == m_entries[i].action.kind && m_group[k].id == m_entries[i].action.id);
            }
            m_entryViews[i].marked = in;
        }
        for (u32 k = 0; k < m_groupCount; ++k) {
            m_groupViews[k] = {iconOf(bar, world, m_group[k]), {}, {}, false};
        }
    } else if (!m_ranks) {
        for (TouchPickerEntry& e : m_entryViews) {
            e.marked = false;
        }
    }
    const std::string_view place = placeName(static_cast<TouchPlace>(m_pickSlot));
    text::format(m_title, "VD_TOUCH_EDIT_PICKER_TITLE", place, layerName(bar, world, m_layer));
    bool inherited = false;
    const TouchAction shown = game::touchSlotShown(t, m_layer, m_pickSlot, inherited);
    TouchPickerView v;
    v.title = m_title.data();
    v.tab = m_tab;
    v.subTabs = m_tab == TouchPickerTab::Spells && !m_ranks ? std::span<const std::string_view>(m_subTabNames)
                                                            : std::span<const std::string_view>{};
    v.subTab = m_subTab;
    v.entries = m_entryViews;
    v.layer = m_layer != TouchContext::Base;
    v.grouping = m_grouping;
    v.group = {m_groupViews.data(), m_groupCount};
    v.canRank = shown.kind == TouchActionKind::Spell;
    v.ranks = m_ranks;
    v.empty = text::get(m_tab == TouchPickerTab::Pet ? "VD_TOUCH_EDIT_NO_PET" : "VD_TOUCH_EDIT_NOTHING");
    v.area = sideArea(c, 440.0f);
    const TouchPickerResult r = touchActionPicker(c, v);
    m_panelRect = r.panel;
    const auto closePicker = [&]() {
        m_picking = false;
        m_grouping = false;
        m_ranks = false;
        m_groupCount = 0;
    };
    if (r.tab >= 0 && static_cast<TouchPickerTab>(r.tab) != m_tab) {
        m_tab = static_cast<TouchPickerTab>(r.tab);
        m_subTab = 0;
        m_entriesKey = 0;
        ui::scrollTo(ui::widgetId("touchpick.list"), 0.0f);
    }
    if (r.subTab >= 0 && static_cast<u32>(r.subTab) != m_subTab) {
        m_subTab = static_cast<u32>(r.subTab);
        m_entriesKey = 0;
        ui::scrollTo(ui::widgetId("touchpick.list"), 0.0f);
    }
    if (r.entryHeld >= 0 && static_cast<u32>(r.entryHeld) < m_entries.size() && world.combat) {
        world.combat->showTouchTooltip(buttonOf(bar, world, m_entries[static_cast<u32>(r.entryHeld)].action, nowMs),
                                       r.entryRect, {});
    }
    if (r.entry >= 0 && static_cast<u32>(r.entry) < m_entries.size()) {
        const TouchAction picked = m_entries[static_cast<u32>(r.entry)].action;
        if (m_grouping) {
            // In the group or out of it.
            u32 at = m_groupCount;
            for (u32 k = 0; k < m_groupCount; ++k) {
                at = m_group[k].kind == picked.kind && m_group[k].id == picked.id ? k : at;
            }
            if (at < m_groupCount) {
                std::copy(m_group.begin() + at + 1, m_group.begin() + m_groupCount, m_group.begin() + at);
                --m_groupCount;
            } else if (m_groupCount < m_group.size()) {
                m_group[m_groupCount++] = picked;
            }
        } else {
            game::putTouchAction(t, m_layer, m_pickSlot, picked);
            bar.changed();
            closePicker();
        }
        return;
    }
    if (r.groupEntry >= 0 && static_cast<u32>(r.groupEntry) < m_groupCount) {
        std::copy(m_group.begin() + r.groupEntry + 1, m_group.begin() + m_groupCount, m_group.begin() + r.groupEntry);
        --m_groupCount;
    }
    if (r.empty) {
        game::hideTouchSlot(t, m_layer, m_pickSlot);
        bar.changed();
        closePicker();
    } else if (r.inherit) {
        game::inheritTouchSlot(t, m_layer, m_pickSlot);
        bar.changed();
        closePicker();
    } else if (r.hide) {
        game::hideTouchSlot(t, m_layer, m_pickSlot);
        bar.changed();
        closePicker();
    } else if (r.rank) {
        m_ranks = true;
        m_entriesKey = 0;
    } else if (r.group) {
        if (!m_grouping) {
            // §6.6: a group of up to eight, starting with what the slot shows (a group's own actions).
            m_grouping = true;
            m_groupCount = 0;
            const i32 g = game::touchSlotGroup(t, m_layer, m_pickSlot);
            if (g >= 0) {
                const game::TouchGroup& group = t.groups[static_cast<u32>(g)];
                for (u32 k = 0; k < group.count && m_groupCount < m_group.size(); ++k) {
                    m_group[m_groupCount++] = group.actions[k];
                }
            } else if (!shown.empty() && shown.kind != TouchActionKind::Special &&
                       shown.kind != TouchActionKind::Consumable) {
                m_group[m_groupCount++] = shown;
            }
        } else if (game::putTouchGroup(t, m_layer, m_pickSlot, {m_group.data(), m_groupCount}) >= 0) {
            bar.changed();
            closePicker();
        }
    } else if (r.closed) {
        if (m_ranks) {
            m_ranks = false;
            m_entriesKey = 0;
        } else {
            closePicker();
        }
    }
}

void TouchEditScreens::drawDrawer(TouchBarScreens& bar, World world, DialogContext& c, u32 nowMs) {
    const bool placing = world.combat && !world.combat->placing().empty();
    // The drawer's button -> the server's slot: the first page shows the form's bonus bar, as the 1.12 main bar does
    // (a warrior's stance, Stealth, Cat and Bear form: ActionButton_GetPagedID).
    u32 bonus = 0;
    if (world.session) {
        if (const game::Entity* me = world.session->entities().find(world.session->playerGuid())) {
            bonus = game::bonusBarOffset(me->byteValue(net::uf::UNIT_FIELD_BYTES_1, 2), bar.abilityData());
        }
    }
    const auto serverSlot = [bonus](u32 index) {
        return index < game::kActionBarButtons ? game::mainBarSlot(0, index, bonus) : index;
    };
    if (world.session && world.combat) {
        // The bars' six pages with their states (CombatScreens: as the action bars show them).
        const game::SpellBook& spells = world.session->play().spells();
        for (u32 slot = 0; slot < kTouchDrawerSlots; ++slot) {
            m_drawerSlots[slot] = world.combat->touchView(*world.session, spells.button(serverSlot(slot)),
                                                          kCacheDrawer + slot, nowMs);
            m_drawerSlots[slot].highlight = placing;
        }
    }
    TouchDrawerView v;
    v.slots = m_drawerSlots;
    v.keepOpen = m_drawerKeepOpen;
    v.placing = placing;
    v.area = sideArea(c, 10000.0f);
    const TouchDrawerResult r = touchDrawer(c, v);
    if (r.keepOpen) {
        m_drawerKeepOpen = !m_drawerKeepOpen;
    }
    if (r.used >= 0) {
        if (world.session && world.combat) {
            // §6.9: used (or the spell being placed put there), and closed unless kept open.
            m_drawerAim = world.combat->useActionSlot(*world.session, serverSlot(static_cast<u32>(r.used)));
        }
        if (!m_drawerKeepOpen && !placing) {
            m_drawerOpen = false;
        }
    }
    if (r.held >= 0 && world.session && world.combat) {
        world.combat->holdActionSlot(*world.session, serverSlot(static_cast<u32>(r.held)), r.heldRect);
    }
    if (r.closed) {
        m_drawerOpen = false;
    }
}

// --- Offline: VD_TOUCH_EDIT_DEMO -----------------------------------------------------------------------------

bool TouchEditScreens::prepareOffline(TouchBarScreens& bar, HudFrame& hud, TouchBarView& view, u32 nowMs) {
    if (m_demo[0] == '\0') {
        return false;
    }
    const World world{};
    const std::string_view demo(m_demo.data());
    const usize colon = demo.find(':');
    const std::string_view what = demo.substr(0, colon);
    if (!m_demoReady) {
        m_demoReady = true;
        const char* cls = VD_DEV_ENV("VD_TOUCH_DEMO");
        m_demoWarrior = cls && std::strncmp(cls, "warrior", 7) == 0;
        // The sample layout: two templates, the class's spells on the skills in the order of their roles, a layer, a
        // group in F, the three consumables.
        game::TouchLayout layout;
        game::TouchTemplate t = game::makeTouchTemplate("Solo");
        const u32* base = m_demoWarrior ? kDemoWarrior : kDemoRogue;
        for (u32 i = 0; i < game::kTouchSkills; ++i) {
            t.layers[0][touchIndex(kRoleOrder[i])] = TouchAction::spell(base[i]);
        }
        const u32* group = m_demoWarrior ? kDemoWarriorGroup : kDemoRogueGroup;
        t.groups[0].count = 4;
        for (u32 i = 0; i < 4; ++i) {
            t.groups[0].actions[i] = TouchAction::spell(group[i]);
        }
        t.layers[0][touchIndex(TouchPlace::ColumnF)] = {TouchActionKind::Group, 0, 0, 0};
        if (m_demoWarrior) {
            const TouchPlace places[3] = {TouchPlace::RingC, TouchPlace::RingE, TouchPlace::RingB};
            for (u32 i = 0; i < 3; ++i) {
                t.layers[static_cast<u32>(TouchContext::DefensiveStance)][touchIndex(places[i])] =
                    TouchAction::spell(kDemoWarriorDefensive[i]);
            }
        } else {
            for (u32 i = 0; i < 5; ++i) {
                t.layers[static_cast<u32>(TouchContext::Stealth)][touchIndex(kRoleOrder[i])] =
                    TouchAction::spell(kDemoRogueStealth[i]);
            }
            t.layers[static_cast<u32>(TouchContext::Stealth)][touchIndex(TouchPlace::RingB)] =
                TouchAction{TouchActionKind::Empty, game::kTouchHidden, 0, 0};
        }
        t.layers[0][touchIndex(TouchPlace::Consumable1)] = TouchAction::consumable(game::TouchConsumable::Health);
        t.layers[0][touchIndex(TouchPlace::Consumable2)] =
            TouchAction::consumable(m_demoWarrior ? game::TouchConsumable::Rage : game::TouchConsumable::Energy);
        t.layers[0][touchIndex(TouchPlace::Consumable3)] = TouchAction::consumable(game::TouchConsumable::Bandage);
        game::addTouchTemplate(layout, t);
        t.setName("Dungeon");
        game::addTouchTemplate(layout, t);
        bar.adoptLayout(layout);
        // What the picker lists.
        m_demoKnown.clear();
        for (const u32* list : {base, group}) {
            m_demoKnown.insert(m_demoKnown.end(), list, list + (list == base ? 9 : 4));
        }
        if (m_demoWarrior) {
            m_demoKnown.insert(m_demoKnown.end(), std::begin(kDemoWarriorKnown), std::end(kDemoWarriorKnown));
            m_demoKnown.insert(m_demoKnown.end(), std::begin(kDemoWarriorDefensive), std::end(kDemoWarriorDefensive));
        } else {
            m_demoKnown.insert(m_demoKnown.end(), std::begin(kDemoRogueKnown), std::end(kDemoRogueKnown));
            m_demoKnown.insert(m_demoKnown.end(), std::begin(kDemoRogueStealth), std::end(kDemoRogueStealth));
        }
        // The drawer's sample bars: the first page the class's spells, the second some others.
        const game::GameData* data = bar.gameData();
        for (u32 i = 0; i < kTouchDrawerSlots && i < m_demoKnown.size(); ++i) {
            const std::optional<game::SpellInfo> info = data ? data->spell(m_demoKnown[i]) : std::nullopt;
            m_drawerSlots[i].iconHash = info ? info->iconHash : 0;
        }
        m_drawerSlots[3].cooldownLeft = 6.0f;
        m_drawerSlots[3].cooldownTotal = 10.0f;
        m_drawerSlots[5].usable = false;
        // The windows asked for.
        const std::string_view arg = colon == std::string_view::npos ? std::string_view{} : demo.substr(colon + 1);
        TouchContext layer = TouchContext::Base;
        game::touchContextByName(arg, layer);
        m_demoCombat = what == "combat";
        if (what == "list" || what == "rename" || what == "delete" || what == "combat") {
            m_listOpen = true;
            if (what == "rename") {
                m_renaming = 1;
                m_renameText.set("Dungeon");
            } else if (what == "delete") {
                m_ask = Ask::Delete;
                m_askIndex = 1;
            }
        } else if (what == "edit" || what == "layout" || what == "picker" || what == "group" || what == "ranks") {
            openEdit(bar, world, what == "layout" ? static_cast<i32>(touchIndex(TouchPlace::RingA)) : -1, false);
            m_layer = layer;
            m_layoutMode = what == "layout";
            if (what != "edit" && what != "layout") {
                openPicker(bar, world, touchIndex(TouchPlace::Main));
                m_selected = static_cast<i32>(touchIndex(TouchPlace::Main));
                if (what == "group") {
                    m_grouping = true;
                    m_groupCount = 3;
                    for (u32 i = 0; i < 3; ++i) {
                        m_group[i] = TouchAction::spell(group[i]);
                    }
                }
                m_ranks = what == "ranks";
            }
        } else if (what == "drawer") {
            m_drawerOpen = true;
        } else if (what == "tooltip") {
            m_demoTip = static_cast<i32>(touchIndex(TouchPlace::Main));
        }
        VD_LOG_INFO("VD_TOUCH_EDIT_DEMO: %.*s", static_cast<int>(demo.size()), demo.data());
    }
    // The sample cluster, as the bar shows it (icons only), or as the edit mode does.
    const game::TouchTemplate& t = bar.layout().current();
    m_mirrored = bar.mirrored();
    view = {};
    view.geometry = &t.geometry[static_cast<u32>(bar.profile())];
    view.mirrored = m_mirrored;
    for (u32 s = 0; s < game::kTouchSlots; ++s) {
        const TouchAction a = game::resolveTouchAction(t, bar.context(), s);
        TouchButtonView& b = view.places[s];
        b.shown = !a.empty();
        b.slot.iconHash = b.shown ? iconOf(bar, world, a) : 0;
        b.group = a.kind == TouchActionKind::Group;
    }
    for (const auto& [place, icon] :
         {std::pair{TouchPlace::Templates, kIconTemplates}, std::pair{TouchPlace::Drawer, kIconDrawer},
          std::pair{TouchPlace::Mount, kIconMount}}) {
        view.places[touchIndex(place)].shown = true;
        view.places[touchIndex(place)].slot.iconHash = icon;
    }
    view.templateName = t.nameView();
    view.templateIndex = bar.layout().active;
    view.templateCount = bar.layout().count;
    if (what == "placing") {
        m_placingShown = true;
        for (u32 s = 0; s < game::kTouchSlots; ++s) {
            view.places[s].shown = true;
            view.places[s].slot.highlight = true;
        }
    }
    if (m_editing) {
        editView(bar, world, view, nowMs);
    }
    prepareView(view);
    hud.context = HudContextAction::Loot; // the interaction button shows
    hud.touch = &view;
    return true;
}

void TouchEditScreens::actOffline(TouchBarScreens& bar, const TouchBarOutput& out, u32 nowMs) {
    if (m_demo[0] == '\0') {
        return;
    }
    const World world{};
    if (m_editing) {
        applyEdit(bar, world, out);
        return;
    }
    if (m_placingShown && out.pressed >= 0 && static_cast<u32>(out.pressed) < game::kTouchSlots) {
        // VD_TOUCH_EDIT_DEMO=placing: Gouge "from the book" onto the slot tapped.
        game::putTouchAction(bar.layout().current(), bar.context(), static_cast<u32>(out.pressed),
                             TouchAction::spell(1776));
        bar.changed();
        m_placingShown = false;
        std::strncpy(m_demo.data(), "done", m_demo.size() - 1);
        return;
    }
    if (out.pressed == static_cast<i32>(touchIndex(TouchPlace::Templates))) {
        openTemplates();
    } else if (out.pressed == static_cast<i32>(touchIndex(TouchPlace::Drawer))) {
        toggleDrawer();
    }
    if (out.swipe != 0 && bar.layout().count > 1) {
        const u32 n = bar.layout().count;
        bar.selectTemplate((bar.layout().active + (out.swipe > 0 ? 1u : n - 1u)) % n, nowMs);
    }
    if (out.held >= 0 && static_cast<u32>(out.held) < game::kTouchSlots) {
        m_demoTip = out.held;
        m_tipAnchor = out.heldRect;
    }
}

void TouchEditScreens::drawOffline(TouchBarScreens& bar, DialogContext& c, u32 nowMs) {
    if (m_demo[0] == '\0' || !bar.haveLayout()) {
        return;
    }
    drawWindows(bar, World{}, c, nowMs);
    drawOfflineTooltip(bar, c);
}

void TouchEditScreens::drawOfflineTooltip(TouchBarScreens& bar, DialogContext& c) {
    // Without a world CombatScreens has no tooltip to show: the spell's name, rank and description, with "Edit".
    if (m_demoTip < 0 || !bar.abilityData()) {
        return;
    }
    const TouchAction action =
        game::resolveTouchAction(bar.layout().current(), bar.context(), static_cast<u32>(m_demoTip));
    if (action.kind != TouchActionKind::Spell) {
        m_demoTip = -1;
        return;
    }
    const game::SpellText& spells = bar.abilityData()->spellText();
    if (m_demoTipText.empty()) {
        game::spellDescription(spells, action.id, m_demoTipText);
    }
    const TooltipLineView lines[2] = {{spells.name(action.id), spells.rank(action.id), ui::kWhite, ui::kGray},
                                      {m_demoTipText, {}, ui::kGold, ui::kWhite, true}};
    const std::string_view edit = text::get("VD_ACTIONBAR_EDIT");
    TooltipView view;
    view.iconHash = iconOf(bar, World{}, action);
    view.lines = lines;
    view.actions = {&edit, 1};
    if (m_tipAnchor.w <= 0.0f && m_cluster.w > 0.0f) {
        m_tipAnchor = {m_cluster.right() - m_cluster.w * 0.3f, m_cluster.bottom() - m_cluster.h * 0.35f,
                       m_cluster.w * 0.2f, m_cluster.w * 0.2f};
    }
    view.anchor = m_tipAnchor;
    const TooltipResult r = itemTooltip(c, view);
    if (r.action == 0) {
        openEdit(bar, World{}, m_demoTip, true);
        m_demoTip = -1;
    } else if (r.closed) {
        m_demoTip = -1;
    }
}

} // namespace vd
