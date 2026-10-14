#include "app/CombatScreens.h"

#include "app/DialogKit.h"
#include "app/GameSession.h"
#include "app/MacroScreens.h"
#include "app/PopupScreens.h"
#include "app/Portraits.h"
#include "app/UiResources.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/TouchPet.h" // phase 20 (pets)
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <span>

namespace vd {
namespace {

namespace uf = net::uf;
using game::Entity;
using ui::rgba;

constexpr u32 kDismissPetSpell = 2641; // hunters dismiss with a spell (a cast time)
constexpr u32 kEffectLearnSpell = 36; // SPELL_EFFECT_LEARN_SPELL: a trainer's spell teaches its trigger spell
constexpr u32 kClassHunter = 3;
constexpr u32 kErrorColor = rgba(255, 26, 26);

// Appends to a NUL-terminated buffer of `size` bytes holding `used` characters; truncates silently.
void appendFormat(char* out, usize size, usize& used, const char* format, ...) VD_PRINTF_FORMAT(4, 5);
void appendFormat(char* out, usize size, usize& used, const char* format, ...) {
    if (used + 1 >= size) {
        return;
    }
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(out + used, size - used, format, args);
    va_end(args);
    used = n > 0 ? std::min(size - 1, used + static_cast<usize>(n)) : used;
}

// The same with a text of the table (core/Text) and its arguments.
template <typename... Args>
void appendText(char* out, usize size, usize& used, text::Key key, const Args&... args) {
    if (used + 1 >= size) {
        return;
    }
    used += text::format(std::span<char>(out + used, size - used), key, args...).size();
}

constexpr u64 kIconGeneral = assetPathHash("Interface\\Icons\\INV_Misc_Book_09.blp");
// The pet bar's command and reaction icons (PetActionBarFrame.lua: PET_*_TEXTURE).
constexpr u64 kPetAttack = assetPathHash("Interface\\Icons\\Ability_GhoulFrenzy.blp");
constexpr u64 kPetFollow = assetPathHash("Interface\\Icons\\Ability_Tracking.blp");
constexpr u64 kPetStay = assetPathHash("Interface\\Icons\\Spell_Nature_TimeStop.blp");
constexpr u64 kPetAggressive = assetPathHash("Interface\\Icons\\Ability_Racial_BloodRage.blp");
constexpr u64 kPetDefensive = assetPathHash("Interface\\Icons\\Ability_Defend.blp");
constexpr u64 kPetPassive = assetPathHash("Interface\\Icons\\Ability_Seal.blp");
constexpr u64 kComboPoint = assetPathHash("Interface\\ComboFrame\\ComboPoint.blp");
constexpr u64 kStatusBar = assetPathHash("Interface\\TargetingFrame\\UI-StatusBar.blp");
constexpr u64 kTargetOfTarget = assetPathHash("Interface\\TargetingFrame\\UI-TargetofTargetFrame.blp");
constexpr u64 kMonsterPortrait = assetPathHash("Interface\\CharacterFrame\\TemporaryPortrait-Monster.blp");

// Combat log colors (the client's combat log defaults, roughly).
constexpr u32 kLogOutgoing = rgba(255, 255, 255);
constexpr u32 kLogIncoming = rgba(255, 90, 90);
constexpr u32 kLogHeal = rgba(90, 255, 90);
constexpr u32 kLogMiss = rgba(200, 200, 200);
constexpr u32 kLogAura = rgba(120, 180, 255);
constexpr u32 kLogDeath = rgba(255, 209, 0);
constexpr u32 kLogOther = rgba(170, 170, 170);

f32 floatValue(const Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// A pet's refused cast as the client words it: ERR_PET_SPELL_<reason> when there is one ("Your pet is out of
// range."), SPELL_FAILED_<reason> otherwise; the reason's name when the table has neither.
std::string_view petCastText(const char* name) {
    if (std::strcmp(name, "CASTER_DEAD") == 0) {
        return text::get("ERR_PET_SPELL_DEAD");
    }
    char key[64];
    for (const char* prefix : {"ERR_PET_SPELL_", "SPELL_FAILED_"}) {
        std::snprintf(key, sizeof(key), "%s%s", prefix, name);
        if (text::has(key)) {
            return text::get(key);
        }
    }
    return name;
}

// PetTameFailureReason (SharedDefines.h) -> the client's PETTAME_* texts.
std::string_view tameFailureText(i32 reason) {
    static constexpr text::Key kTexts[] = {"PETTAME_UNKNOWNERROR",  "PETTAME_INVALIDCREATURE",
                                           "PETTAME_TOOMANY",       "PETTAME_CREATUREALREADYOWNED",
                                           "PETTAME_NOTTAMEABLE",   "PETTAME_ANOTHERSUMMONACTIVE",
                                           "PETTAME_UNITSCANTTAME", "PETTAME_NOPETAVAILABLE",
                                           "PETTAME_INTERNALERROR", "PETTAME_TOOHIGHLEVEL",
                                           "PETTAME_DEAD",          "PETTAME_NOTDEAD",
                                           "PETTAME_UNKNOWNERROR"};
    return text::get(reason > 0 && static_cast<usize>(reason) < std::size(kTexts) ? kTexts[reason]
                                                                                  : kTexts[0]);
}

} // namespace

void CombatScreens::init(const game::GameData* data, const game::AbilityData* abilities,
                         UiResources* resources, MessageSink sink, void* sinkContext) {
    m_data = data;
    m_abilities = abilities;
    m_resources = resources;
    m_sink = sink;
    m_sinkContext = sinkContext;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    m_usableItems.reserve(64);
    m_itemViews.reserve(64);
    m_tabViews.reserve(8);
    m_entryViews.reserve(256);
    m_serviceViews.reserve(128);
    m_serviceIndex.reserve(128);
    m_talentViews.reserve(64);
    m_talentInfos.reserve(64);
    m_petName.reserve(32);
    for (Tooltip* t : {&m_bookTip, &m_trainerTip, &m_talentTip, &m_actionTip}) {
        t->description.reserve(512);
        t->next.reserve(512);
    }
}

void CombatScreens::reset() {
    m_windows = 0;
    m_modal = Modal::None;
    m_placing = {};
    m_page = 0;
    m_tipKind = TipKind::None;
    m_formsVersion = 0;
    m_formCount = 0;
    m_bookSelected = -1;
    m_bookTab = 0;
    m_book.invalidate();
    m_bookFlash = {}; // phase 18 (extras)
    m_bookShown = false;
    m_trainerSeen = 0;
    m_talentSelected = -1;
    m_logNext = m_logCount = 0;
    m_auraWatch.reset();
}

void CombatScreens::open(CombatWindow window) {
    m_windows |= windowBit(window);
    m_modal = Modal::None;
    m_placing = {};
    m_placingPet = 0;
    m_tipKind = TipKind::None;
    if (window == CombatWindow::Spellbook) {
        m_bookType = 0; // phase 15 (pets): the micro button's ToggleSpellBook(BOOKTYPE_SPELL)
    }
}

void CombatScreens::message(u32 color, bool big, std::string_view line) {
    if (!m_sink) {
        return;
    }
    char copy[200];
    const usize n = std::min(line.size(), sizeof(copy) - 1);
    std::memcpy(copy, line.data(), n);
    copy[n] = 0;
    m_sink(m_sinkContext, color, big, copy);
}

void CombatScreens::trace(const char* format, ...) const {
    if (!m_trace) {
        return;
    }
    char text[160];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    VD_LOG_INFO("Abilities: %s", text);
}

void CombatScreens::log(std::string_view what, u32 color, bool mine) {
    LogLine& line = m_log[m_logNext];
    m_logNext = (m_logNext + 1) % static_cast<u32>(m_log.size());
    m_logCount = std::min(m_logCount + 1, static_cast<u32>(m_log.size()));
    ++m_logTotal;
    const usize n = std::min(what.size(), line.text.size() - 1);
    std::memcpy(line.text.data(), what.data(), n);
    line.text[n] = 0;
    line.color = color;
    line.timeMs = m_nowMs;
    line.mine = mine;
}

CombatScreens::LogEntry CombatScreens::logEntry(u32 index) const {
    const LogLine& line = m_log[(m_logNext + m_log.size() - m_logCount + index) % m_log.size()];
    return {line.text.data(), line.color, line.timeMs, line.mine};
}

game::CombatNames CombatScreens::names(GameSession& session) {
    m_nameSource = {&session, m_abilities, m_items};
    game::CombatNames n;
    n.player = session.playerGuid();
    n.pet = session.play().abilities().pet().guid;
    n.context = &m_nameSource;
    n.unitName = [](const void* context, u64 guid) -> std::string_view {
        GameSession& s = *static_cast<const NameSource*>(context)->session;
        if ((guid >> 48) == 0) { // a player
            const std::string* name = s.playerName(guid);
            return name ? std::string_view(*name) : std::string_view{};
        }
        if (guid == s.play().abilities().pet().guid) {
            const std::string_view pet = s.play().abilities().petName();
            if (!pet.empty()) {
                return pet;
            }
        }
        const Entity* e = s.entities().find(guid);
        if (e && e->type == game::ObjectType::GameObject) { // phase 13: a chest a lock spell opened
            const game::GameObjectInfo* object = s.gameObject(e->value(uf::OBJECT_FIELD_ENTRY));
            return object ? std::string_view(object->name) : std::string_view{};
        }
        const net::CreatureQueryInfo* c = e ? s.creature(e->value(uf::OBJECT_FIELD_ENTRY)) : nullptr;
        return c ? std::string_view(c->name) : std::string_view{};
    };
    n.spellName = [](const void* context, u32 spellId) -> std::string_view {
        const game::AbilityData* data = static_cast<const NameSource*>(context)->data;
        return data ? data->spellText().name(spellId) : std::string_view{};
    };
    // Phase 13: items made, fed, damaged or enchanted (asked for when unknown), and the enchantments.
    n.itemName = [](const void* context, u32 entry) -> std::string_view {
        const net::ItemQueryInfo* item = static_cast<const NameSource*>(context)->session->item(entry);
        return item ? std::string_view(item->name) : std::string_view{};
    };
    n.enchantName = [](const void* context, u32 id) -> std::string_view {
        const game::ItemData* items = static_cast<const NameSource*>(context)->items;
        return items ? items->enchantmentText(id) : std::string_view{};
    };
    return n;
}

// --- Action bar --------------------------------------------------------------------------------------------

u32 CombatScreens::mainSlot(const Entity& me, u32 button) const {
    // Stances, cat / bear form and stealth swap the first page for their own bar (ActionButton_GetPagedID: the
    // form's m_bonusActionBar).
    const u32 form = me.byteValue(uf::UNIT_FIELD_BYTES_1, 2); // UNIT_BYTES_1_OFFSET_SHAPESHIFT_FORM
    return game::mainBarSlot(m_page, button, game::bonusBarOffset(form, m_abilities));
}

const CombatScreens::SlotSpell& CombatScreens::slotSpell(SlotSpell& cache, game::ActionButton button) const {
    // Spell.dbc is read when the button changes, not every frame.
    if (cache.packed != button.packed()) {
        cache = {};
        cache.packed = button.packed();
        if (button.type == net::kActionSpell && button.action != 0) {
            if (const std::optional<game::SpellInfo> info = m_data ? m_data->spell(button.action) : std::nullopt) {
                cache.info = *info;
                cache.hasInfo = true;
            }
            cache.hasDetails = m_abilities && m_abilities->spellText().details(button.action, cache.details);
            if (const std::optional<game::SpellRequirements> needs = // phase 20 (classes)
                m_data ? m_data->requirements(button.action) : std::nullopt) {
                cache.needs = *needs;
                cache.hasNeeds = true;
            }
        }
    }
    return cache;
}

HudActionSlot CombatScreens::actionSlot(GameSession& session, const BarContext& bar, u32 slot, SlotSpell& cache,
                                        u32 nowMs) {
    const game::ActionButton button = session.play().spells().button(slot);
    // Phase 15 (options): a macro shows its own icon and name (GetActionTexture, GetActionText) and the state of its
    // first /cast spell (WoW.exe 0x4E5A50: the macro's spell stands for it in IsUsableAction, GetActionCooldown,
    // IsActionInRange, IsCurrentAction).
    if (const u32 macroId = game::macroIdOf(button); macroId != 0) {
        const game::Macro* macro = m_macros ? m_macros->macro(macroId) : nullptr;
        if (!macro) {
            return {}; // not among this character's macros: an empty-looking button (GetActionTexture is nil)
        }
        const u32 spell = m_macros->macroSpell(session, macroId);
        HudActionSlot view = spell != 0 ? buttonView(session, bar, {net::kActionSpell, spell}, cache, nowMs)
                                        : HudActionSlot{};
        view.iconHash = macro->iconHash;
        view.label = macro->name;
        return view;
    }
    return buttonView(session, bar, button, cache, nowMs);
}

HudActionSlot CombatScreens::buttonView(GameSession& session, const BarContext& bar, game::ActionButton button,
                                        SlotSpell& cache, u32 nowMs) {
    game::Gameplay& play = session.play();
    const game::SpellBook& spells = play.spells();
    const Entity& me = *bar.me;
    HudActionSlot view;
    if (button.empty()) {
        return view;
    }
    // Where a spell lands (Gameplay::spellTarget's rule: helpful spells on the player when the target is not
    // friendly): the unit its range is measured to (IsActionInRange), none for the player.
    const auto landsOn = [&](const game::SpellInfo& spell) -> const Entity* {
        const u64 aim = game::spellLandsOn(spell.targetKind(), me.guid, bar.targetGuid, bar.targetFriendly);
        return aim != 0 && aim == bar.targetGuid && aim != me.guid ? bar.target : nullptr;
    };
    // GetActionCooldown: the shade and seconds; a cooldown waiting for its event (Stealth while stealthed) is not
    // shown (CooldownFrame_SetTimer's enable 0).
    const auto showCooldown = [&](const game::CooldownView& cooldown) {
        if (!cooldown.untilEvent && cooldown.durationMs > 0 && cooldown.active(nowMs)) {
            view.cooldownLeft = static_cast<f32>(cooldown.remaining(nowMs)) / 1000.0f;
            view.cooldownTotal = static_cast<f32>(cooldown.durationMs) / 1000.0f;
        }
    };
    if (button.type == net::kActionSpell) {
        const SlotSpell& known = slotSpell(cache, button);
        if (!known.hasInfo) {
            return view;
        }
        const game::SpellInfo& spell = known.info;
        view.iconHash = spell.iconHash;
        // Its own cooldown, its category's and the global cooldown.
        showCooldown(spells.cooldown(spell, spell.category, nowMs));
        // IsUsableAction: power (short: blue), the aura state the caster and the target need (Revenge, Overpower),
        // combo points (finishers), the form it is cast in.
        // Phase 13 (spells): the cost and the range with the talents' and auras' spell modifiers.
        const game::SpellMods& mods = play.spellMods();
        const game::ActionUsable usable =
            game::spellUsable(spell, known.hasDetails ? &known.details : nullptr, me, bar.target, bar.comboPoints,
                              m_abilities, &mods, known.hasNeeds ? &known.needs : nullptr, bar.needs);
        view.usable = usable.usable;
        view.noMana = usable.noMana;
        view.inRange = game::spellRange(spell, me, landsOn(spell), nowMs, &mods) != game::ActionRange::Out;
        // Phase 13 (spells): the spell waiting for its place or item is the current action (checked).
        view.checked = play.targeting().spellId() == button.action;
        // The current action (ActionButton_UpdateState: casting, waiting for the next swing, auto-repeating, the
        // auto attack on) and the red flash of the auto attack and auto-repeat (ActionButton_UpdateFlash).
        const bool attacking = button.action == game::kAttackSpell && play.melee().attacking();
        const bool repeating = play.abilities().autoRepeat(bar.targetGuid) == button.action;
        view.checked = view.checked || attacking || repeating || spells.cast().spellId == button.action ||
                       spells.queuedSwing(nowMs) == button.action;
        view.flashing = attacking || repeating;
    } else if (button.type == net::kActionItem) {
        const game::Inventory& inventory = play.inventory();
        const u32 carried = inventory.count(button.action);
        view.equipped = inventory.isEquipped(button.action); // IsEquippedAction: the green border
        view.usable = carried > 0 || view.equipped;
        const net::ItemQueryInfo* info = session.item(button.action);
        if (!info) {
            return view; // asked for: its icon comes with the answer
        }
        view.iconHash = m_data ? m_data->itemIconHash(info->displayId) : 0;
        if (info->requiredLevel > me.value(uf::UNIT_FIELD_LEVEL)) {
            view.usable = false;
        }
        // IsConsumableAction: stacks, and items used up with their last charge, show how many are left.
        bool consumable = info->stackable > 1;
        const game::CooldownView cooldown = spells.itemCooldown(*info, nowMs);
        u32 equipCooldownMs = 0; // SMSG_ITEM_COOLDOWN: 30 s after equipping an item with an on-use spell
        for (const net::ItemQueryInfo::Spell& use : info->spells) {
            if (use.id == 0 || use.trigger != net::kItemSpellTriggerOnUse) {
                continue;
            }
            consumable = consumable || use.charges < 0;
            equipCooldownMs = std::max(equipCooldownMs, inventory.itemCooldownLeft(use.id, nowMs));
            if (const std::optional<game::SpellInfo> spell = m_data ? m_data->spell(use.id) : std::nullopt) {
                view.inRange =
                    view.inRange && game::spellRange(*spell, me, landsOn(*spell), nowMs) != game::ActionRange::Out;
                // Phase 20 (classes): food and drink in combat (SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL).
                view.usable = view.usable && game::spellNeedMissing(*spell, nullptr, {}, me, nullptr, {}) ==
                                                 game::SpellNeed::Met;
            }
            view.checked = view.checked || spells.cast().spellId == use.id || // the Hearthstone's cast
                           play.targeting().spellId() == use.id; // phase 13 (spells): a bomb, a poison waiting
        }
        view.count = consumable ? carried : 0;
        showCooldown(cooldown);
        if (static_cast<f32>(equipCooldownMs) / 1000.0f > view.cooldownLeft) {
            view.cooldownLeft = static_cast<f32>(equipCooldownMs) / 1000.0f;
            view.cooldownTotal = 30.0f;
        }
    }
    return view;
}

void CombatScreens::prepareStances(GameSession& session, const Entity& me, HudFrame& hud, u32 nowMs) {
    const game::SpellBook& spells = session.play().spells();
    if (!m_abilities || !m_abilities->ready()) {
        return;
    }
    if (m_formsVersion != spells.knownVersion()) { // ShapeshiftBar_Update on UPDATE_SHAPESHIFT_FORMS
        m_formsVersion = spells.knownVersion();
        m_formCount = game::shapeshiftForms(spells.known(), m_abilities->spellText(), m_forms);
    }
    // ShapeshiftBar_UpdateState: the form in use checked, its cooldown, grey when it cannot be cast.
    const u32 current = me.byteValue(uf::UNIT_FIELD_BYTES_1, 2);
    for (u32 i = 0; i < m_formCount; ++i) {
        HudActionSlot& view = m_stances[i];
        view = {};
        const SlotSpell& known =
            slotSpell(m_slotSpells[(1 + kMultiBars) * game::kActionBarButtons + i], {net::kActionSpell, m_forms[i].spellId});
        if (!known.hasInfo) {
            continue;
        }
        view.iconHash = known.info.iconHash;
        view.checked = current == m_forms[i].form;
        const game::CooldownView cooldown = spells.cooldown(known.info, known.info.category, nowMs);
        if (!cooldown.untilEvent && cooldown.durationMs > 0 && cooldown.active(nowMs)) {
            view.cooldownLeft = static_cast<f32>(cooldown.remaining(nowMs)) / 1000.0f;
            view.cooldownTotal = static_cast<f32>(cooldown.durationMs) / 1000.0f;
        }
        const game::ActionUsable usable = game::spellUsable(known.info, nullptr, me, nullptr, 0, nullptr);
        view.usable = usable.usable;
        view.noMana = usable.noMana;
    }
    hud.stances = {m_stances.data(), m_formCount};
}

void CombatScreens::prepareActionBar(GameSession& session, HudFrame& hud, u32 nowMs) {
    VD_PROFILE_ZONE("CombatScreens::actionBar");
    m_nowMs = nowMs;
    hud.actions = {};
    hud.extraActions = {};
    hud.stances = {};
    m_bar = {};
    if (!session.inWorld()) {
        return;
    }
    BarContext bar;
    bar.me = session.entities().find(session.playerGuid());
    if (!bar.me) {
        return;
    }
    bar.targetGuid = session.target();
    bar.target = bar.targetGuid ? session.entities().find(bar.targetGuid) : nullptr;
    bar.targetFriendly = bar.target && (bar.target == bar.me || game::reactionTo(*bar.me, *bar.target, hud.reactions) ==
                                                                    game::Reaction::Friendly);
    bar.comboPoints = game::comboPoints(*bar.me, bar.targetGuid);
    prepareNeeds(session, bar); // phase 20 (classes)
    m_bar = bar; // phase 20: touchView
    m_pages = kPages; // ActionButton.lua: six pages (phase 15: those an extra bar shows are skipped, pageUp)
    m_page = std::min(m_page, m_pages - 1);
    if ((m_hiddenPages & (1u << m_page)) != 0) {
        m_page = 0; // its bar came up: the first page (never an extra bar's)
    }
    const bool placing = !m_placing.empty();
    for (u32 i = 0; i < m_actions.size(); ++i) {
        m_actions[i] = actionSlot(session, bar, mainSlot(*bar.me, i), m_slotSpells[i], nowMs);
        m_actions[i].highlight = placing;
    }
    hud.actions = {m_actions.data(), m_actions.size()};
    hud.actionPage = m_page;
    hud.actionPages = m_pages;
    hud.actionPlacing = placing;
    if (hud.showExtraBar) { // the extra bars' slots (the HUD draws those that fit): phase 15, all four
        for (u32 b = 0; b < kMultiBars; ++b) {
            if ((hud.multiBars & (1u << b)) == 0) {
                continue;
            }
            for (u32 i = 0; i < game::kActionBarButtons; ++i) {
                m_extraActions[b][i] = actionSlot(session, bar, game::multiBarSlot(kMultiBarPages[b], i),
                                                  m_slotSpells[(1 + b) * game::kActionBarButtons + i], nowMs);
                m_extraActions[b][i].highlight = placing;
            }
            const std::span<const HudActionSlot> slots{m_extraActions[b].data(), game::kActionBarButtons};
            if (b == 0) {
                hud.extraActions = slots;
            } else {
                hud.moreBars[b - 1] = slots;
            }
        }
    }
    prepareStances(session, *bar.me, hud, nowMs);
}

u64 CombatScreens::useSlot(GameSession& session, u32 slot) {
    game::Gameplay& play = session.play();
    if (!m_placing.empty()) {
        play.setActionButton(slot, m_placing);
        trace("action button %u <- %u (type %u), placed", slot, m_placing.action, m_placing.type);
        m_placing = {};
        return 0;
    }
    trace("action button %u", slot);
    return useButton(session, play.spells().button(slot));
}

u64 CombatScreens::useButton(GameSession& session, game::ActionButton button) {
    game::Gameplay& play = session.play();
    if (button.empty()) {
        return 0;
    }
    if (m_macros) {
        m_macros->saveText(); // phase 15 (options): ActionButtonUp's MacroFrame_SaveMacro, before any UseAction
        if (const u32 macroId = game::macroIdOf(button); macroId != 0) {
            trace("macro %u", macroId);
            m_macros->run(session, macroId); // RunMacro: each line as typed in the chat
            return 0;
        }
    }
    if (button.type != net::kActionSpell) {
        // Items; parity (popups): bind on use asks first (USE_BIND).
        const bool asked = button.type == net::kActionItem && m_popups &&
                           m_popups->askBindForUse(session, button.action);
        if (!asked && button.type == net::kActionItem) {
            play.useItem(button.action);
        }
        return 0;
    }
    const u64 target = play.target();
    if (button.action == game::kAttackSpell) { // "Attack" on the bar: the auto attack toggle
        if (play.melee().attacking()) {
            play.stopAttack();
            return 0;
        }
        return play.startAttack(target) ? target : 0;
    }
    game::SpellDetails details;
    const bool autoRepeat =
        m_abilities && m_abilities->spellText().details(button.action, details) && details.autoRepeat();
    if (autoRepeat && play.abilities().autoRepeat(target) == button.action) {
        play.abilities().cancelAutoRepeat(); // a second tap stops Auto Shot / Shoot
        trace("auto repeat %u cancelled", button.action);
        return 0;
    }
    const u64 aim = play.spellTarget(button.action);
    if (play.castSpell(button.action, aim) && autoRepeat) {
        play.abilities().startedAutoRepeat(button.action, aim);
        play.setSheath(game::kSheathRanged); // Shoot / Auto Shot draw the ranged weapon
        trace("auto repeat %u started at %llx", button.action, static_cast<unsigned long long>(aim));
    }
    return aim != session.playerGuid() ? aim : 0;
}

HudActionSlot CombatScreens::touchView(GameSession& session, game::ActionButton button, u32 cacheIndex, u32 nowMs) {
    if (!m_bar.me || cacheIndex >= m_touchSpells.size()) {
        return {};
    }
    SlotSpell& cache = m_touchSpells[cacheIndex];
    if (const u32 macroId = game::macroIdOf(button); macroId != 0) { // as actionSlot shows a macro
        const game::Macro* macro = m_macros ? m_macros->macro(macroId) : nullptr;
        if (!macro) {
            return {};
        }
        const u32 spell = m_macros->macroSpell(session, macroId);
        HudActionSlot view =
            spell != 0 ? buttonView(session, m_bar, {net::kActionSpell, spell}, cache, nowMs) : HudActionSlot{};
        view.iconHash = macro->iconHash;
        view.label = macro->name;
        return view;
    }
    HudActionSlot view = buttonView(session, m_bar, button, cache, nowMs);
    if (button.type == net::kActionSpell && cache.hasNeeds && m_bar.needs) {
        // Phase 20 (classes, §4): the reagent left (soul shards, runes...), the ammunition of Auto Shot.
        view.count = game::spellShownCount(cache.info, cache.needs, *m_bar.me, *m_bar.needs);
    }
    return view;
}

void CombatScreens::prepareNeeds(GameSession& session, BarContext& bar) {
    VD_PROFILE_ZONE("CombatScreens::prepareNeeds");
    // Phase 20 (classes, docs/MOBILE_CONTROLS.md §10.3): what every button's spell is checked against (the equipped
    // item, the reagents and totems carried, the target's creature type). The worn items are read again only when
    // the inventory changes or an item query is answered.
    const game::Inventory& inventory = session.play().inventory();
    if (m_wornInventory != inventory.version() || m_wornQueries != session.queryVersion()) {
        m_wornInventory = inventory.version();
        m_wornQueries = session.queryVersion();
        for (u8 slot = 0; slot < game::kWornSlots; ++slot) {
            game::WornItem& worn = m_worn[slot];
            worn = {};
            const game::ItemInstance* item = inventory.equipped(slot);
            if (!item || item->empty()) {
                continue;
            }
            const net::ItemQueryInfo* info = item->entry ? session.item(item->entry) : nullptr;
            worn.entry = item->entry;
            worn.count = item->count;
            worn.unknown = info == nullptr; // its object or its query still on the way: it fits meanwhile
            if (info && !item->broken()) {
                worn.itemClass = static_cast<i32>(info->itemClass);
                worn.subclass = static_cast<i32>(info->subclass);
            }
        }
    }
    m_needs.worn = m_worn;
    m_needs.targetCreatureType = 0;
    if (bar.target && bar.target->type == game::ObjectType::Unit) {
        const net::CreatureQueryInfo* creature = session.creature(bar.target->value(uf::OBJECT_FIELD_ENTRY));
        m_needs.targetCreatureType = creature ? creature->type : 0;
    }
    m_carriedInventory = &inventory;
    m_needs.context = this;
    m_needs.carried = [](const void* context, u32 entry) {
        const CombatScreens& self = *static_cast<const CombatScreens*>(context);
        return self.m_carried.count(*self.m_carriedInventory, entry);
    };
    bar.needs = &m_needs;
}

HudActionSlot CombatScreens::petActionView(GameSession& session, u32 slot, u32 nowMs) {
    // Phase 20 (pets): a PetAction's id (game/TouchPet.h) as the pet bar would show the button that does it now
    // (PetActionBar_Update: the command and the reaction in use checked, a spell's autocast on), with what the bar
    // leaves to its cooldown frame and tints: the seconds of the pet's cooldown, its power, the range from the pet.
    HudActionSlot view;
    game::Gameplay& play = session.play();
    const game::Abilities& abilities = play.abilities();
    const game::PetState& pet = abilities.pet();
    const game::SpellText* text = m_abilities ? &m_abilities->spellText() : nullptr;
    const game::TouchPetAction a = game::decodeTouchPetAction(slot);
    const i32 at = game::touchPetSlot(pet, slot, text);
    u8 type = 0;
    u32 action = 0;
    if (at >= 0) {
        type = net::petButtonType(pet.bar[static_cast<u32>(at)]);
        action = net::petButtonAction(pet.bar[static_cast<u32>(at)]);
    } else if (a.kind == game::TouchPetKind::Command || a.kind == game::TouchPetKind::Reaction) {
        type = a.kind == game::TouchPetKind::Command ? net::kPetActCommand : net::kPetActReaction;
        action = a.value; // not on the bar: CMSG_PET_ACTION names it all the same
    } else if (a.kind == game::TouchPetKind::Spell) {
        action = pet.active() ? game::touchPetBookSpell(abilities.petBook(), a.value, text) : 0;
        type = action != 0 ? abilities.petSpellState(action) : 0;
        if (action == 0) { // another pet out, or none: the spell's own icon, grey (the slot keeps its place)
            const std::optional<game::SpellInfo> info = m_data ? m_data->spell(a.value) : std::nullopt;
            view.iconHash = info ? info->iconHash : 0;
            view.usable = false;
            return view;
        }
    } else {
        return view; // an empty slot of the bar
    }
    const Entity* unit = pet.active() ? session.entities().find(pet.guid) : nullptr;
    const bool alive = unit && !game::isDead(*unit) && pet.enabled;
    if (type == net::kPetActCommand) {
        view.iconHash = action == net::kPetCommandAttack   ? kPetAttack
                        : action == net::kPetCommandFollow ? kPetFollow
                        : action == net::kPetCommandStay   ? kPetStay
                                                           : 0;
        view.checked = action != net::kPetCommandAttack && pet.commandState == action;
        view.usable = alive;
        return view;
    }
    if (type == net::kPetActReaction) {
        view.iconHash = action == net::kPetReactAggressive  ? kPetAggressive
                        : action == net::kPetReactDefensive ? kPetDefensive
                                                            : kPetPassive;
        view.checked = action == pet.reactState;
        view.usable = alive;
        return view;
    }
    const std::optional<game::SpellInfo> info = m_data ? m_data->spell(action) : std::nullopt;
    view.iconHash = info ? info->iconHash : 0;
    view.checked = type == net::kPetActEnabled; // autocast on
    // A possessed unit's spells come as ACT_PASSIVE and are cast all the same (HandlePetAction): only
    // SPELL_ATTR_PASSIVE ones are not.
    view.usable = alive && !(info && info->passive());
    if (const game::PetCooldown cd = abilities.petCooldown(action, nowMs); cd.durationMs > 0) {
        const u32 gone = nowMs - cd.startMs;
        view.cooldownLeft = gone < cd.durationMs ? static_cast<f32>(cd.durationMs - gone) / 1000.0f : 0.0f;
        view.cooldownTotal = static_cast<f32>(cd.durationMs) / 1000.0f;
    }
    if (unit && info) {
        const u32 powerType = std::min<u32>(info->powerType, 4);
        if (view.usable && unit->value(static_cast<u16>(uf::UNIT_FIELD_POWER1 + powerType)) < info->powerCost) {
            view.usable = false;
            view.noMana = true;
        }
        // Where it lands (the player's target for a pet's enemy or unit spell, as CastPetAction aims): its range
        // from the pet.
        const game::SpellTargetKind kind = info->targetKind();
        const u64 lands = kind == game::SpellTargetKind::Enemy || kind == game::SpellTargetKind::AnyUnit ? play.target()
                                                                                                           : 0;
        const Entity* target = lands != 0 && lands != pet.guid ? session.entities().find(lands) : nullptr;
        if (target) {
            view.inRange = game::spellRange(*info, *unit, target, nowMs) != game::ActionRange::Out;
        }
    }
    return view;
}

void CombatScreens::usePetAction(GameSession& session, u32 slot) {
    // Phase 20 (pets): the button that does it now (CastPetAction); a command, a reaction or a spell of the book not
    // on the bar goes out by itself (CMSG_PET_ACTION names the action, not the slot).
    game::Gameplay& play = session.play();
    game::Abilities& abilities = play.abilities();
    const game::SpellText* text = m_abilities ? &m_abilities->spellText() : nullptr;
    const game::TouchPetAction a = game::decodeTouchPetAction(slot);
    if (const i32 at = game::touchPetSlot(abilities.pet(), slot, text); at >= 0) {
        abilities.petAction(static_cast<u32>(at), play.target());
    } else if (a.kind == game::TouchPetKind::Command) {
        abilities.petCommand(a.value, play.target());
    } else if (a.kind == game::TouchPetKind::Reaction) {
        abilities.petReact(a.value);
    } else if (a.kind == game::TouchPetKind::Spell) {
        if (const u32 spell = game::touchPetBookSpell(abilities.petBook(), a.value, text); spell != 0) {
            abilities.castPetSpell(spell, play.target());
        }
    }
    trace("pet action %u", slot);
}

u64 CombatScreens::unitAt(f32 x, f32 y, ui::Rect& frame) const {
    // Phase 20 (assists): the pet frame and the target of target's, as last drawn.
    if (m_totRect.w > 0.0f && m_totRect.contains(x, y)) {
        frame = m_totRect;
        return m_totGuid;
    }
    if (m_petFrameRect.w > 0.0f && m_petFrameRect.contains(x, y)) {
        frame = m_petFrameRect;
        return m_petFrameGuid;
    }
    return 0;
}

void CombatScreens::useForm(GameSession& session, u32 index) {
    const Entity* me = session.entities().find(session.playerGuid());
    if (!me || index >= m_formCount) {
        return;
    }
    // CastShapeshiftForm: into the form; out of it again when it is the one in use (not a warrior's stance).
    game::Gameplay& play = session.play();
    const game::ShapeshiftForm& form = m_forms[index];
    const bool active = me->byteValue(uf::UNIT_FIELD_BYTES_1, 2) == form.form;
    game::SpellDetails details;
    const game::FormTap tap = m_abilities && m_abilities->spellText().details(form.spellId, details)
                                  ? game::formTap(details, active)
                                  : (active ? game::FormTap::Nothing : game::FormTap::Cast);
    if (tap == game::FormTap::Cast) {
        play.castSpell(form.spellId, 0);
        trace("form %u: cast %u", form.form, form.spellId);
    } else if (tap == game::FormTap::Cancel) {
        play.cancelAura(form.spellId);
        trace("form %u: cancelled %u", form.form, form.spellId);
    }
}

void CombatScreens::holdSlot(GameSession& session, u32 slot, u32 page, const ui::Rect& anchor) {
    m_placing = {};
    m_tipOverTouch = false; // phase 20 (edit)
    const game::ActionButton button = session.play().spells().button(slot);
    const bool shown = !button.empty() && (game::macroIdOf(button) == 0 ||
                                           (m_macros && m_macros->macro(game::macroIdOf(button))));
    if (!shown) { // nothing to show: straight to the editor
        m_tipKind = TipKind::None;
        m_editSlot = slot;
        m_editPage = page;
        m_modal = Modal::ActionEdit; // modal over the panels (they stay)
        return;
    }
    // The touch screen's hover: the button's tooltip (GameTooltip:SetAction), its "Edit" action inside.
    m_tipKind = TipKind::Action;
    m_tipSlot = slot;
    m_tipPage = page;
    m_tipAnchor = anchor;
    m_tipKey = 0;
}

// phase 20 (edit)
void CombatScreens::holdActionSlot(GameSession& session, u32 slot, const ui::Rect& anchor) {
    holdSlot(session, slot, slot / game::kActionBarButtons + 1, anchor);
    m_tipOverTouch = true;
}

void CombatScreens::showTouchTooltip(game::ActionButton button, const ui::Rect& anchor, std::string_view action) {
    m_tipKind = button.empty() ? TipKind::None : TipKind::Button;
    m_tipButton = button;
    m_tipAction = action;
    m_tipAnchor = anchor;
    m_tipKey = 0;
    m_tipOverTouch = true;
    m_touchTipAction = false;
}

void CombatScreens::drawTouchTooltip(GameSession& session, DialogContext& c) {
    if (m_tipOverTouch && session.inWorld()) {
        actionTooltip(session, c);
    }
}

u64 CombatScreens::actOnActionBar(GameSession& session, const HudOutput& out) {
    if (!session.inWorld()) {
        return 0;
    }
    const Entity* me = session.entities().find(session.playerGuid());
    if (!me) {
        return 0;
    }
    constexpr i32 kButtons = static_cast<i32>(game::kActionBarButtons);
    if (out.pageNext) { // ActionBar_PageUp: 1 -> 6, then 1 again, past the pages an extra bar shows
        for (u32 step = 0; step < m_pages; ++step) {
            m_page = (m_page + 1) % std::max(1u, m_pages);
            if ((m_hiddenPages & (1u << m_page)) == 0) {
                break;
            }
        }
    }
    if (out.pageHome) {
        m_page = 0;
    }
    if (out.actionHold >= 0 && out.actionHold < kButtons) {
        holdSlot(session, mainSlot(*me, static_cast<u32>(out.actionHold)), m_page + 1, out.holdRect);
        return 0;
    }
    const u32 extraPage = kMultiBarPages[std::min<u32>(out.extraBar, kMultiBars - 1)]; // phase 15: its bar's page
    if (out.extraActionHold >= 0 && out.extraActionHold < kButtons) {
        holdSlot(session, game::multiBarSlot(extraPage, static_cast<u32>(out.extraActionHold)), extraPage, out.holdRect);
        return 0;
    }
    if (out.stanceHold >= 0 && static_cast<u32>(out.stanceHold) < m_formCount) {
        m_placing = {};
        m_tipKind = TipKind::Form; // GameTooltip:SetShapeshift
        m_tipOverTouch = false;    // phase 20 (edit)
        m_tipSlot = static_cast<u32>(out.stanceHold);
        m_tipAnchor = out.holdRect;
        m_tipKey = 0;
        return 0;
    }
    if (out.stance >= 0 && static_cast<u32>(out.stance) < m_formCount) {
        useForm(session, static_cast<u32>(out.stance));
        return 0;
    }
    if (out.extraAction >= 0 && out.extraAction < kButtons) {
        return useSlot(session, game::multiBarSlot(extraPage, static_cast<u32>(out.extraAction)));
    }
    if (out.action >= 0 && out.action < kButtons) {
        return useSlot(session, mainSlot(*me, static_cast<u32>(out.action)));
    }
    return 0;
}

void CombatScreens::actionTooltip(GameSession& session, DialogContext& c) {
    if (m_tipKind == TipKind::None) {
        return;
    }
    game::Gameplay& play = session.play();
    const Entity* me = session.entities().find(session.playerGuid());
    u32 spellId = 0, itemEntry = 0;
    const game::Macro* macro = nullptr; // phase 15 (options): its first /cast's tooltip, or its name alone
    if (m_tipKind == TipKind::Form) {
        spellId = m_tipSlot < m_formCount ? m_forms[m_tipSlot].spellId : 0;
    } else {
        // Phase 20 (edit): a touch cluster's button is on no server slot.
        const game::ActionButton button = m_tipKind == TipKind::Button ? m_tipButton : play.spells().button(m_tipSlot);
        spellId = button.type == net::kActionSpell ? button.action : 0;
        itemEntry = button.type == net::kActionItem ? button.action : 0;
        if (const u32 macroId = game::macroIdOf(button); macroId != 0 && m_macros) {
            macro = m_macros->macro(macroId);
            spellId = macro ? m_macros->macroSpell(session, macroId) : 0;
        }
    }
    if (!me || (spellId == 0 && itemEntry == 0 && !macro)) {
        m_tipKind = TipKind::None; // the button was emptied
        return;
    }
    const net::ItemQueryInfo* info = itemEntry ? session.item(itemEntry) : nullptr;
    // The cooldown left (COOLDOWN_REMAINING / ITEM_COOLDOWN_TIME_*): the tooltip's last line.
    game::CooldownView cooldown;
    if (spellId != 0) {
        cooldown = play.spells().cooldown(spellId, m_nowMs);
        if (cooldown.durationMs <= 1500 && !cooldown.untilEvent) {
            cooldown = {}; // the global cooldown is not the spell's
        }
    } else if (info) {
        cooldown = play.spells().itemCooldown(*info, m_nowMs);
    }
    const u32 secondsLeft = !cooldown.untilEvent && cooldown.active(m_nowMs)
                                ? (cooldown.remaining(m_nowMs) + 999) / 1000
                                : 0;
    // Rebuilt only when what it shows changed.
    u64 key = 1469598103934665603ull;
    for (const u64 part : {static_cast<u64>(spellId), static_cast<u64>(itemEntry), static_cast<u64>(info != nullptr),
                           static_cast<u64>(session.queryVersion()), static_cast<u64>(play.inventory().version()),
                           static_cast<u64>(me->value(uf::UNIT_FIELD_LEVEL)), static_cast<u64>(secondsLeft),
                           static_cast<u64>(play.spellMods().version()), // phase 13 (spells)
                           reinterpret_cast<u64>(macro)}) {              // phase 15 (options)
        key = (key ^ part) * 1099511628211ull;
    }
    if (key != m_tipKey) {
        m_tipKey = key;
        m_tipLineCount = 0;
        const auto add = [&](const TooltipLineView& line) {
            if (m_tipLineCount < m_tipLines.size()) {
                m_tipLines[m_tipLineCount++] = line;
            }
        };
        if (spellId != 0) { // GameTooltip:SetSpell
            SpellTooltipView v;
            tooltip(session, m_actionTip, spellId, 0, v);
            m_tipLineCount = spellTooltipLines(v, m_tipLines);
        } else if (info) {
            game::TooltipContext context;
            context.level = me->value(uf::UNIT_FIELD_LEVEL);
            context.race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
            context.cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
            context.inventory = &play.inventory();
            context.queries = &play.queries();
            context.data = play.inventory().data();
            context.spells = &play.spells();
            context.showSellPrice = false;
            game::buildItemTooltip(*info, nullptr, context, m_itemTip);
            for (const game::TooltipLine& line : m_itemTip.lines()) {
                add({line.left, line.right, line.leftColor, line.rightColor, line.wrap, line.money, line.gapBefore});
            }
        } else if (macro) {
            add({macro->name, {}, ui::kWhite}); // a macro without a known spell: its name
        } else {
            add({"...", {}});
        }
        if (secondsLeft > 0) {
            if (secondsLeft >= 3600) {
                text::format(m_tipCooldown, "ITEM_COOLDOWN_TIME_HOURS_P1", static_cast<int>(secondsLeft / 3600));
            } else if (secondsLeft >= 60) {
                text::format(m_tipCooldown, "ITEM_COOLDOWN_TIME_MIN", static_cast<int>((secondsLeft + 59) / 60));
            } else {
                text::format(m_tipCooldown, "ITEM_COOLDOWN_TIME_SEC", static_cast<int>(secondsLeft));
            }
            add({m_tipCooldown.data(), {}});
        }
    }
    TooltipView view;
    if (spellId != 0) {
        const std::optional<game::SpellInfo> spell = m_data ? m_data->spell(spellId) : std::nullopt;
        view.iconHash = spell ? spell->iconHash : 0;
    } else if (info && m_data) {
        view.iconHash = m_data->itemIconHash(info->displayId);
    }
    if (macro) {
        view.iconHash = macro->iconHash;
    }
    view.lines = {m_tipLines.data(), m_tipLineCount};
    const std::string_view edit = text::get("VD_ACTIONBAR_EDIT");
    // Phase 15 (options): LOCK_ACTIONBAR keeps the buttons from being picked up: no editing then (an empty one still
    // takes a spell, as a locked bar still takes what is dropped on it).
    if (m_tipKind == TipKind::Action && !m_lockActionBars) {
        view.actions = {&edit, 1};
    }
    if (m_tipKind == TipKind::Button && !m_tipAction.empty()) { // phase 20 (edit): the touch cluster's own action
        view.actions = {&m_tipAction, 1};
    }
    view.anchor = m_tipAnchor;
    const TooltipResult r = itemTooltip(c, view);
    if (r.action == 0 && m_tipKind == TipKind::Action) {
        m_editSlot = m_tipSlot;
        m_editPage = m_tipPage;
        m_modal = Modal::ActionEdit;
        m_tipKind = TipKind::None;
    } else if (r.action == 0 && m_tipKind == TipKind::Button) {
        m_touchTipAction = true; // phase 20 (edit): TouchEditScreens takes it
        m_tipKind = TipKind::None;
    } else if (r.closed) {
        m_tipKind = TipKind::None;
    }
}

// --- Frame -------------------------------------------------------------------------------------------------

void CombatScreens::update(GameSession& session, u32 nowMs) {
    VD_PROFILE_ZONE("CombatScreens::update");
    m_nowMs = nowMs;
    if (!session.inWorld()) {
        return;
    }
    game::Gameplay& play = session.play();
    if (m_player != session.playerGuid()) {
        m_player = session.playerGuid();
        m_eventCursor = play.events().end();
        m_combatCursor = play.combatLog().end();
        reset();
    }
    // A trainer's list replaces its gossip window; out of reach, it closes (as the client's frames).
    const game::TrainerWindow& trainerWindow = play.abilities().trainer();
    if (trainerWindow.open() && trainerWindow.trainer() != m_trainerSeen) {
        m_trainerSeen = trainerWindow.trainer();
        m_trainerSelected = -1;
        if (play.dialog().open()) {
            play.closeDialog();
        }
        m_modal = Modal::None; // parity pass: the panels' rules decide what the trainer's window replaces
    }
    if (!trainerWindow.open()) {
        m_trainerSeen = 0;
    } else {
        const Entity* me = session.entities().find(session.playerGuid());
        const Entity* npc = session.entities().find(trainerWindow.trainer());
        if (!me || !npc || game::distanceAt(*me, *npc, nowMs) - floatValue(*npc, uf::UNIT_FIELD_BOUNDINGRADIUS) > 10.0f) {
            play.abilities().closeTrainer();
        }
    }
    followBook(nowMs); // phase 18 (extras)
    readEvents(session);
    readCombat(session);
    watchAuras(session);
}

void CombatScreens::followBook(u32 nowMs) {
    // Phase 18 (extras): SpellBookFrame_OnHide (the flash stops, every texture hides) and OnShow (they flash). Turning
    // to the pet's page or back hides and shows the frame (ToggleSpellBook).
    const bool shown = isOpen(CombatWindow::Spellbook);
    if (shown == m_bookShown && (!shown || m_bookType == m_bookTypeShown)) {
        return;
    }
    if (m_bookShown) {
        m_bookFlash.hidden();
    }
    if (shown) {
        m_bookFlash.shown(nowMs);
    }
    m_bookShown = shown;
    m_bookTypeShown = m_bookType;
}

void CombatScreens::learnedSpellInTab(GameSession& session, u32 spellId) {
    // Phase 18 (extras): LEARNED_SPELL_IN_TAB (WoW.exe 0x4B2B8D, AddSpell with its notice: SMSG_LEARNED_SPELL and
    // SMSG_SUPERCEDED_SPELL): not for a spell hidden from the book, a recipe or a craft (0x4B290F..0x4B29C4); the tab
    // of the spell's skill line, General (the first) when its line has none.
    const Entity* me = session.entities().find(session.playerGuid());
    game::SpellDetails details;
    if (!m_abilities || !me || !m_abilities->spellText().details(spellId, details) || details.hidden() ||
        details.tradeSpell() || details.castUi != 0) {
        return;
    }
    m_book.update(*m_abilities, session.play().spells().known(), me->byteValue(uf::UNIT_FIELD_BYTES_0, 0),
                  me->byteValue(uf::UNIT_FIELD_BYTES_0, 1), m_allRanks);
    u32 tab = 0, index = 0;
    if (!m_book.find(spellId, tab, index)) {
        tab = 0;
    }
    m_bookFlash.learned(tab, m_bookType != 0);
}

void CombatScreens::readEvents(GameSession& session) {
    game::Gameplay& play = session.play();
    const game::CombatNames n = names(session);
    char line[200];
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        using K = game::GameEventKind;
        switch (e->kind) {
        case K::TrainerBuyFailed: {
            const i32 reason = e->value[0];
            message(kErrorColor, false,
                    text::get(reason == static_cast<i32>(net::kTrainFailNotEnoughMoney)
                                  ? "ERR_NOT_ENOUGH_MONEY"
                              : reason == static_cast<i32>(net::kTrainFailNotEnoughSkill)
                                  ? "VD_COMBAT_TRAIN_FAIL_SKILL"
                                  : "VD_COMBAT_TRAIN_FAIL"));
            break;
        }
        case K::SpellLearned: learnedSpellInTab(session, e->id); break; // phase 18 (extras)
        case K::TalentWipeOffer:
            if (e->guid == 0) {
                message(kErrorColor, false, text::get("ERR_TALENT_WIPE_ERROR"));
            }
            break;
        case K::PetFeedback:
            switch (e->value[0]) {
            case 0: message(kErrorColor, false, text::get("ERR_PET_BROKEN")); break;
            case net::kPetFeedbackDead: message(kErrorColor, false, text::get("ERR_PET_SPELL_DEAD")); break;
            case net::kPetFeedbackNothingToAttack:
                message(kErrorColor, false, text::get("ERR_NO_ATTACK_TARGET"));
                break;
            case net::kPetFeedbackCantAttackTarget:
                message(kErrorColor, false, text::get("ERR_INVALID_ATTACK_TARGET"));
                break;
            case net::kPetFeedbackNoPath: message(kErrorColor, false, text::get("PET_SPELL_NOPATH")); break;
            default: break;
            }
            break;
        case K::PetCastFailed: {
            const char* name = net::spellCastResultName(static_cast<u8>(e->value[0]));
            if (std::strcmp(name, "DONT_REPORT") != 0) {
                message(kErrorColor, false, petCastText(name));
            }
            break;
        }
        case K::PetTameFailed: message(kErrorColor, false, tameFailureText(e->value[0])); break;
        case K::PetChanged:
            if (e->guid == 0) {
                m_modal = m_modal == Modal::PetMenu || m_modal == Modal::AbandonPet ? Modal::None : m_modal;
            }
            m_placingPet = 0; // phase 15 (pets): another pet, or none
            break;
        // Reputation, experience and level-ups: GameScreens::readNotices (game/Notices) writes their lines.
        case K::PartyKill:
            game::formatKill(line, sizeof(line), e->other, e->guid, n);
            log(line, kLogDeath, e->other == n.player || e->guid == n.player);
            break;
        case K::PlayerDied: log(text::get("UNITDIESSELF"), kLogDeath, true); break;
        default: break;
        }
    }
}

void CombatScreens::readCombat(GameSession& session) {
    game::Gameplay& play = session.play();
    const game::CombatNames n = names(session);
    char line[200];
    while (const game::CombatEvent* e = play.combatLog().next(m_combatCursor)) {
        game::CombatLineKind kind = game::CombatLineKind::Other;
        if (game::formatCombatEvent(line, sizeof(line), *e, n, &kind) == 0) {
            continue;
        }
        u32 color = kLogOther;
        switch (kind) {
        case game::CombatLineKind::Outgoing: color = kLogOutgoing; break;
        case game::CombatLineKind::Incoming: color = kLogIncoming; break;
        case game::CombatLineKind::Heal: color = kLogHeal; break;
        case game::CombatLineKind::Miss: color = kLogMiss; break;
        default: break;
        }
        const bool mine = e->attacker == n.player || e->victim == n.player ||
                          (n.pet && (e->attacker == n.pet || e->victim == n.pet));
        log(line, color, mine);
    }
}

void CombatScreens::watchAuras(GameSession& session) {
    game::Gameplay& play = session.play();
    const game::CombatNames n = names(session);
    const std::array<u64, 3> units{session.playerGuid(), play.target(), play.abilities().pet().guid};
    char line[200];
    m_auraWatch.update(session.entities(), units, [&](u64 guid, u32 spellId, bool gained, bool harmful) {
        if (m_abilities) {
            game::SpellDetails d;
            if (!m_abilities->spellText().details(spellId, d) || d.hidden() || d.passive()) {
                return; // passives and hidden auras are not news
            }
        }
        game::formatAuraChange(line, sizeof(line), guid, spellId, gained, harmful, n);
        log(line, kLogAura, guid == n.player || guid == n.pet);
    });
}

// --- Tooltips ----------------------------------------------------------------------------------------------

const SpellTooltipView& CombatScreens::tooltip(GameSession& session, Tooltip& t, u32 spellId, u32 nextSpellId,
                                               SpellTooltipView& view) {
    const Entity* me = session.entities().find(session.playerGuid());
    const u32 level = me ? me->value(uf::UNIT_FIELD_LEVEL) : 0;
    view = SpellTooltipView{};
    game::SpellDetails details;
    if (!m_abilities || spellId == 0 || !m_abilities->spellText().details(spellId, details)) {
        t.spellId = 0;
        return view;
    }
    // Phase 13 (spells): the numbers with the player's spell modifiers, as WoW.exe's tooltip shows them (the cost,
    // range, cast time and cooldown lines and the description's tokens).
    const game::SpellMods& mods = session.play().spellMods();
    game::SpellTextContext context{level, me ? me->byteValue(uf::UNIT_FIELD_BYTES_0, 2) : u8{0}};
    context.mods = &mods;
    game::applySpellMods(details, me, &mods);
    if (t.spellId != spellId || t.nextSpellId != nextSpellId || t.level != level || t.mods != mods.version()) {
        t.spellId = spellId;
        t.nextSpellId = nextSpellId;
        t.level = level;
        t.mods = mods.version();
        game::formatSpellCost(t.cost.data(), t.cost.size(), details);
        game::formatSpellRange(t.range.data(), t.range.size(), details);
        game::formatSpellCastTime(t.cast.data(), t.cast.size(), details);
        game::formatSpellCooldown(t.cooldown.data(), t.cooldown.size(), details);
        std::snprintf(t.rank.data(), t.rank.size(), "%.*s", static_cast<int>(details.rank.size()),
                      details.rank.data());
        if (details.passive()) {
            text::format(t.rank, "SPELL_PASSIVE");
        }
        game::spellDescription(m_abilities->spellText(), spellId, t.description, context);
        t.next.clear();
        if (nextSpellId != 0) {
            game::spellDescription(m_abilities->spellText(), nextSpellId, t.next, context);
        }
    }
    if (const std::optional<game::SpellInfo> info = m_data ? m_data->spell(spellId) : std::nullopt) {
        view.iconHash = info->iconHash;
    }
    view.name = details.name;
    view.rank = t.rank.data();
    view.cost = t.cost.data();
    view.range = t.range.data();
    view.castTime = t.cast.data();
    view.cooldown = t.cooldown.data();
    view.description = t.description;
    if (nextSpellId != 0) {
        view.nextTitle = text::get("TOOLTIP_TALENT_NEXT_RANK");
        view.nextDescription = t.next;
    }
    return view;
}

// --- Windows -----------------------------------------------------------------------------------------------

bool CombatScreens::hasWindow(const GameSession& session) const {
    const game::Abilities& abilities = session.play().abilities();
    (void)abilities;
    return m_modal != Modal::None;
}

void CombatScreens::drawWindow(GameSession& session, DialogContext& c, u32 nowMs) {
    m_nowMs = nowMs;
    game::Abilities& abilities = session.play().abilities();
    // The modal steps of the HUD (parity pass: the trainer and the player's windows are panels: drawTrainer,
    // drawPanel; a trainer's talent reset offer is a popup: app/PopupScreens).
    switch (m_modal) {
    case Modal::ActionEdit: actionEdit(session, c); return;
    case Modal::ItemPicker: pickItem(session, c); return;
    case Modal::PickSpell: spellbook(session, c); return;
    case Modal::PetMenu: {
        const bool hunter = [&] {
            const Entity* me = session.entities().find(session.playerGuid());
            return me && me->byteValue(uf::UNIT_FIELD_BYTES_0, 1) == kClassHunter;
        }();
        const ConfirmResult r =
            confirmDialog(c, text::get(hunter ? "VD_COMBAT_PET_MENU" : "VD_COMBAT_DISMISS_MINION"),
                          text::get("PET_DISMISS"), text::get(hunter ? "PET_ABANDON" : "CANCEL"));
        if (r.yes) {
            if (hunter) {
                session.play().castSpell(kDismissPetSpell, 0);
                trace("pet dismissed (Dismiss Pet)");
            } else {
                abilities.dismissPet();
                trace("pet dismissed");
            }
            m_modal = Modal::None;
        } else if (r.no) {
            m_modal = hunter && !m_popups ? Modal::AbandonPet : Modal::None;
            if (hunter && m_popups) {
                m_popups->askAbandonPet(session); // parity (popups): ABANDON_PET
            }
        }
        return;
    }
    case Modal::ConfirmProfession: {
        // PROFESSION_CONFIRMATION1 while both primary professions are free ("...as your first one?"), 2
        // after.
        char question[256];
        const std::string_view name =
            m_abilities ? m_abilities->spellText().name(m_confirmService) : std::string_view{};
        const Entity* me = session.entities().find(session.playerGuid());
        const bool first = !me || me->value(uf::PLAYER_CHARACTER_POINTS2) >= 2;
        text::format(question, first ? "PROFESSION_CONFIRMATION1" : "PROFESSION_CONFIRMATION2", name);
        const ConfirmResult r = confirmDialog(c, question, text::get("ACCEPT"), text::get("CANCEL"));
        if (r.yes) {
            abilities.buyTrainerSpell(m_confirmService);
            trace("trainer: buy %u (a primary profession)", m_confirmService);
        }
        if (r.yes || r.no) {
            m_modal = Modal::None;
        }
        return;
    }
    case Modal::AbandonPet: {
        const ConfirmResult r =
            confirmDialog(c, text::get("ABANDON_PET"), text::get("PET_ABANDON"), text::get("CANCEL"));
        if (r.yes) {
            abilities.abandonPet();
            trace("pet abandoned");
        }
        if (r.yes || r.no) {
            m_modal = Modal::None;
        }
        return;
    }
    default: break;
    }
}

bool CombatScreens::trainerOpen(const GameSession& session) const {
    return session.play().abilities().trainer().open();
}

void CombatScreens::drawTrainer(GameSession& session, DialogContext& c, u32 nowMs) {
    m_nowMs = nowMs;
    if (trainerOpen(session)) {
        trainer(session, c);
    }
}

void CombatScreens::close(CombatWindow window) {
    m_windows &= static_cast<u8>(~windowBit(window));
}

void CombatScreens::drawPanel(GameSession& session, DialogContext& c, u32 nowMs, CombatWindow window) {
    m_nowMs = nowMs;
    if (!isOpen(window)) {
        return;
    }
    switch (window) {
    case CombatWindow::Spellbook:
        if (m_modal != Modal::PickSpell) { // the action editor's picker shows the book itself (modal)
            spellbook(session, c);
        }
        break;
    case CombatWindow::Talents: talents(session, c); break;
    default: break;
    }
}

void CombatScreens::spellbook(GameSession& session, DialogContext& c) {
    game::Gameplay& play = session.play();
    const Entity* me = session.entities().find(session.playerGuid());
    if (!m_abilities || !me) {
        close(CombatWindow::Spellbook);
        m_modal = Modal::None;
        return;
    }
    // Phase 15 (pets): HasPetSpells: the tabs "Spellbook" and the pet's (PET_TYPE_<ChrClasses' petNameToken>) while
    // the pet's book has spells; without them the pet's page closes the window (SpellBookFrame_Update).
    const std::span<const u32> petBook = play.abilities().petBook();
    const bool picking = m_modal == Modal::PickSpell;
    if (m_bookType != 0 && (petBook.empty() || picking)) {
        m_bookType = 0;
        if (!picking) {
            close(CombatWindow::Spellbook);
            return;
        }
    }
    const u8 race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
    const u8 cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
    char petKey[32];
    const std::string_view token = m_petData ? m_petData->petNameToken(cls) : std::string_view{};
    std::snprintf(petKey, sizeof(petKey), "PET_TYPE_%.*s", static_cast<int>(token.size()), token.data());
    const std::string_view petTitle = text::get(token.empty() ? "PET_TYPE_PET" : petKey);
    m_bookNames = {text::get("SPELLBOOK"), petTitle};
    if (m_bookType != 0) {
        petSpellbook(session, c, petTitle);
        return;
    }
    m_book.update(*m_abilities, play.spells().known(), race, cls, m_allRanks);
    const std::span<const game::SpellBookTab> tabs = m_book.tabs();
    m_bookTab = std::min<u32>(m_bookTab, tabs.empty() ? 0 : static_cast<u32>(tabs.size()) - 1);
    m_tabViews.clear();
    for (const game::SpellBookTab& tab : tabs) {
        SpellbookTabView v;
        switch (tab.kind) {
        case game::SpellTabKind::General:
            v.name = text::get("GENERAL");
            v.iconHash = kIconGeneral;
            break;
        default:
            v.name = tab.name;
            v.iconHash = m_abilities->spellText().iconHash(tab.spellIcon);
            break;
        }
        v.flash = m_bookFlash.alpha(static_cast<u32>(m_tabViews.size()), m_nowMs); // phase 18 (extras)
        m_tabViews.push_back(v);
    }
    const std::span<const game::SpellBookEntry> entries = m_book.entries(m_bookTab);
    if (m_bookSelected >= static_cast<i32>(entries.size())) {
        m_bookSelected = -1;
    }
    m_entryViews.clear();
    for (const game::SpellBookEntry& e : entries) {
        SpellbookEntryView v;
        const std::optional<game::SpellInfo> info = m_data ? m_data->spell(e.spellId) : std::nullopt;
        v.iconHash = info ? info->iconHash : 0;
        v.name = e.name;
        v.rank = e.rank;
        v.passive = e.passive;
        if (info) {
            const game::CooldownView cd = play.spells().cooldown(e.spellId, m_nowMs);
            if (cd.active(m_nowMs) && cd.durationMs > 0) {
                v.cooldown =
                    std::min(1.0f, static_cast<f32>(cd.remaining(m_nowMs)) / static_cast<f32>(cd.durationMs));
            }
            const u32 powerType = std::min<u32>(info->powerType, 4);
            v.usable = me->value(static_cast<u16>(uf::UNIT_FIELD_POWER1 + powerType)) >=
                       game::spellPowerCost(*info, *me, &play.spellMods()); // phase 13 (spells)
        }
        m_entryViews.push_back(v);
    }
    SpellbookView view;
    view.tabs = m_tabViews;
    view.tab = m_bookTab;
    view.entries = m_entryViews;
    view.selected = m_bookSelected;
    view.allRanks = m_allRanks;
    view.picking = picking;
    if (!petBook.empty() && !picking) {
        view.books = m_bookNames; // phase 15 (pets)
        view.book = 0;
    }
    SpellTooltipView tip;
    const u32 selectedSpell = m_bookSelected >= 0 ? entries[static_cast<u32>(m_bookSelected)].spellId : 0;
    view.tooltip = tooltip(session, m_bookTip, selectedSpell, 0, tip);
    const SpellbookResult r = spellbookWindow(c, view);
    if (r.tab >= 0) {
        m_bookFlash.clicked(static_cast<u32>(r.tab)); // phase 18 (extras): SpellBookSkillLineTab_OnClick
    }
    if (r.tab >= 0 && static_cast<u32>(r.tab) != m_bookTab) {
        m_bookTab = static_cast<u32>(r.tab);
        m_uiSound(UiSound::PageTurn); // the book's other pages (SpellBookFrame's igAbiliityPageTurn)
        m_bookSelected = -1;
        ui::scrollTo(ui::widgetId("spellbook.grid"), 0.0f);
    }
    // Phase 15 (options): SpellButton_OnModifiedClick with the macro window up: "/cast Name(Rank n)" into the macro
    // being edited (touch: its open keyboard is the Shift key); passives add nothing.
    bool toMacro = false;
    if (r.select >= 0 && m_macros && m_data) {
        const std::optional<game::SpellInfo> spell = m_data->spell(entries[static_cast<u32>(r.select)].spellId);
        if (spell && !spell->passive() && !spell->name.empty()) {
            char line[160];
            const std::string_view cast = text::get("SLASH_CAST1");
            if (spell->rank.empty()) {
                std::snprintf(line, sizeof(line), "%.*s %.*s", static_cast<int>(cast.size()), cast.data(),
                              static_cast<int>(spell->name.size()), spell->name.data());
            } else {
                std::snprintf(line, sizeof(line), "%.*s %.*s(%.*s)", static_cast<int>(cast.size()), cast.data(),
                              static_cast<int>(spell->name.size()), spell->name.data(),
                              static_cast<int>(spell->rank.size()), spell->rank.data());
            }
            toMacro = m_macros->addLine(line);
        }
    }
    if (r.select >= 0 && !toMacro) {
        m_bookSelected = r.select;
    }
    if (r.toggleRanks) {
        m_allRanks = !m_allRanks;
        m_bookSelected = -1;
    }
    if (r.place && selectedSpell != 0) {
        if (m_modal == Modal::PickSpell) {
            play.setActionButton(m_editSlot, {net::kActionSpell, selectedSpell});
            trace("action button %u <- spell %u", m_editSlot, selectedSpell);
            m_modal = Modal::None;
        } else {
            // The bar glows: a tap on a slot puts it there (parity pass: the book stays open beside the bar, as
            // the original's while a spell is dragged out of it).
            m_placing = {net::kActionSpell, selectedSpell};
            m_placingPet = 0;
        }
    }
    if (r.cast && selectedSpell != 0) {
        play.castSpell(selectedSpell, play.spellTarget(selectedSpell)); // the book stays open (the original's)
    }
    if (r.book == 1) { // phase 15 (pets): ToggleSpellBook(BOOKTYPE_PET): the window hides and shows the pet's page
        m_bookType = 1;
        m_bookSelected = -1;
        m_uiSound(UiSound::SpellBookClose);
        m_uiSound(UiSound::AbilityOpen);
        ui::scrollTo(ui::widgetId("spellbook.grid"), 0.0f);
    }
    if (r.closed) {
        if (m_modal == Modal::PickSpell) {
            m_modal = Modal::None; // the picker (and the editing) closes; the book's panel stays as it was
        } else {
            close(CombatWindow::Spellbook);
        }
    }
}

void CombatScreens::petSpellbook(GameSession& session, DialogContext& c, std::string_view title) {
    // Phase 15 (pets): SpellBookFrame with BOOKTYPE_PET: the pet's spells (Abilities::petBook, as WoW.exe lists them)
    // with their ranks, passive ones as such, the autocast's overlay and sparkles; a tap selects (its tooltip), "Cast"
    // casts it (CastSpell(id, "pet")), a long press toggles its autocast (the right click: ToggleSpellAutocast), "Put on
    // Bar" lights the pet bar's slots (the spell picked up: PickupSpell(id, "pet")).
    game::Gameplay& play = session.play();
    game::Abilities& abilities = play.abilities();
    const std::span<const u32> petBook = abilities.petBook();
    const Entity* pet = abilities.pet().active() ? session.entities().find(abilities.pet().guid) : nullptr;
    if (m_bookSelected >= static_cast<i32>(petBook.size())) {
        m_bookSelected = -1;
    }
    m_entryViews.clear();
    for (const u32 spellId : petBook) {
        SpellbookEntryView v;
        const std::optional<game::SpellInfo> info = m_data ? m_data->spell(spellId) : std::nullopt;
        const u8 state = abilities.petSpellState(spellId);
        v.passive = state == net::kPetActPassive;
        v.autocastable = state == net::kPetActEnabled || state == net::kPetActDisabled; // GetSpellAutocast
        v.autocast = state == net::kPetActEnabled;
        if (info) {
            v.iconHash = info->iconHash;
            v.name = info->name;
            v.rank = info->rank;
            const game::PetCooldown cd = abilities.petCooldown(spellId, m_nowMs);
            if (cd.durationMs > 0) {
                v.cooldown = static_cast<f32>(cd.durationMs - (m_nowMs - cd.startMs)) / static_cast<f32>(cd.durationMs);
            }
            if (pet) {
                const u32 powerType = std::min<u32>(info->powerType, 4);
                v.usable = pet->value(static_cast<u16>(uf::UNIT_FIELD_POWER1 + powerType)) >= info->powerCost;
            }
        }
        m_entryViews.push_back(v);
    }
    SpellbookView view;
    view.entries = m_entryViews;
    view.selected = m_bookSelected;
    view.books = m_bookNames;
    view.book = 1;
    view.petBook = true;
    view.title = title; // SpellBookTitleText: SpellBookFrame.petTitle
    SpellTooltipView tip;
    const u32 selectedSpell = m_bookSelected >= 0 ? petBook[static_cast<u32>(m_bookSelected)] : 0;
    view.tooltip = tooltip(session, m_bookTip, selectedSpell, 0, tip);
    const SpellbookResult r = spellbookWindow(c, view);
    if (r.select >= 0) {
        m_bookSelected = r.select;
    }
    if (r.autocast >= 0 && static_cast<u32>(r.autocast) < petBook.size()) {
        const u32 spellId = petBook[static_cast<u32>(r.autocast)];
        const u8 state = abilities.petSpellState(spellId);
        if (abilities.setPetSpellAutocast(spellId, state == net::kPetActDisabled)) {
            trace("pet book autocast %u: %d", spellId, state == net::kPetActDisabled ? 1 : 0);
        }
    }
    if (r.place && selectedSpell != 0) {
        m_placingPet = selectedSpell; // PET_BAR_SHOWGRID: the bar's slots light up, a tap puts it there
        m_placing = {};
    }
    if (r.cast && selectedSpell != 0) {
        abilities.castPetSpell(selectedSpell, play.target());
        trace("pet book cast %u at %llx", selectedSpell, static_cast<unsigned long long>(play.target()));
    }
    if (r.book == 0) { // ToggleSpellBook(BOOKTYPE_SPELL)
        m_bookType = 0;
        m_bookSelected = -1;
        m_placingPet = 0;
        m_uiSound(UiSound::AbilityClose);
        m_uiSound(UiSound::SpellBookOpen);
        ui::scrollTo(ui::widgetId("spellbook.grid"), 0.0f);
    }
    if (r.closed) {
        close(CombatWindow::Spellbook);
    }
}

void CombatScreens::trainer(GameSession& session, DialogContext& c) {
    game::Gameplay& play = session.play();
    game::Abilities& abilities = play.abilities();
    const game::TrainerWindow& window = abilities.trainer();
    const Entity* me = session.entities().find(session.playerGuid());
    const u32 level = me ? me->value(uf::UNIT_FIELD_LEVEL) : 0;
    const u32 money = me ? me->value(uf::PLAYER_FIELD_COINAGE) : 0;
    const std::span<const net::TrainerList::Spell> services = window.services();
    // The spell a service teaches (its first effect); the trainer's own spell otherwise.
    const auto taught = [&](u32 spellId) {
        game::SpellDetails d;
        if (m_abilities && m_abilities->spellText().details(spellId, d) &&
            d.effects[0].effect == kEffectLearnSpell && d.effects[0].triggerSpell != 0) {
            return d.effects[0].triggerSpell;
        }
        return spellId;
    };
    m_serviceViews.clear();
    m_serviceIndex.clear();
    for (u32 i = 0; i < services.size(); ++i) {
        const net::TrainerList::Spell& s = services[i];
        const TrainerServiceState state =
            s.state == net::TrainerSpellState::Available     ? TrainerServiceState::Available
            : s.state == net::TrainerSpellState::Unavailable ? TrainerServiceState::Unavailable
                                                             : TrainerServiceState::Known;
        if ((state == TrainerServiceState::Available && !m_showAvailable) ||
            (state == TrainerServiceState::Unavailable && !m_showUnavailable) ||
            (state == TrainerServiceState::Known && !m_showKnown)) {
            continue;
        }
        const u32 spell = taught(s.spellId);
        TrainerServiceView v;
        const std::optional<game::SpellInfo> info = m_data ? m_data->spell(spell) : std::nullopt;
        v.iconHash = info ? info->iconHash : 0;
        v.name = m_abilities ? m_abilities->spellText().name(spell) : std::string_view{};
        v.rank = m_abilities ? m_abilities->spellText().rank(spell) : std::string_view{};
        v.cost = s.cost;
        v.state = state;
        m_serviceViews.push_back(v);
        m_serviceIndex.push_back(i);
    }
    if (m_trainerSelected >= static_cast<i32>(m_serviceViews.size())) {
        m_trainerSelected = -1;
    }
    TrainerView view;
    if (const Entity* npc = session.entities().find(window.trainer())) {
        if (const net::CreatureQueryInfo* info = session.creature(npc->value(uf::OBJECT_FIELD_ENTRY))) {
            view.npcName = info->name;
        }
    }
    view.greeting = window.greeting();
    view.services = m_serviceViews;
    view.selected = m_trainerSelected;
    view.money = money;
    view.showAvailable = m_showAvailable;
    view.showUnavailable = m_showUnavailable;
    view.showKnown = m_showKnown;
    SpellTooltipView tip;
    if (m_trainerSelected >= 0) {
        const net::TrainerList::Spell& s = services[m_serviceIndex[static_cast<u32>(m_trainerSelected)]];
        view.tooltip = tooltip(session, m_trainerTip, taught(s.spellId), 0, tip);
        // What is missing, as the trainer frame lists its requirements ("Requires: Level 12, Frostbolt (Rank
        // 1), Tailoring (50)": REQUIRES_LABEL and TRAINER_REQ_*_RED), then the money and the professions
        // limit.
        std::array<char, 256>& req = m_trainerTip.requirement;
        req[0] = 0;
        usize used = 0;
        if (s.state != net::TrainerSpellState::Known) {
            u32 count = 0;
            const auto next = [&] { // the label before the first requirement, a comma before the others
                if (count++ == 0) {
                    appendText(req.data(), req.size(), used, "REQUIRES_LABEL");
                    appendFormat(req.data(), req.size(), used, " ");
                } else {
                    appendFormat(req.data(), req.size(), used, ", ");
                }
            };
            if (s.requiredLevel > level) {
                next();
                appendText(req.data(), req.size(), used, "TRAINER_REQ_LEVEL_RED", s.requiredLevel);
            }
            for (const u32 required : {s.requiredSpell, s.requiredSpell2}) {
                if (required != 0 && !play.spells().knows(required) && m_abilities) {
                    const std::string_view name = m_abilities->spellText().name(required);
                    const std::string_view rank = m_abilities->spellText().rank(required);
                    char ability[96];
                    if (rank.empty()) {
                        std::snprintf(ability, sizeof(ability), "%.*s", static_cast<int>(name.size()),
                                      name.data());
                    } else {
                        std::snprintf(ability, sizeof(ability), "%.*s (%.*s)", static_cast<int>(name.size()),
                                      name.data(), static_cast<int>(rank.size()), rank.data());
                    }
                    next();
                    appendText(req.data(), req.size(), used, "TRAINER_REQ_ABILITY_RED", ability);
                }
            }
            if (s.requiredSkill != 0 && m_abilities) {
                game::SkillLineInfo line;
                if (m_abilities->skillLine(s.requiredSkill, line)) {
                    next();
                    appendText(req.data(), req.size(), used, "TRAINER_REQ_SKILL_RANK_RED", line.name,
                               s.requiredSkillValue);
                }
            }
            if (s.state == net::TrainerSpellState::Available && money < s.cost) {
                appendFormat(req.data(), req.size(), used, "%s", used > 0 ? "\n" : "");
                appendText(req.data(), req.size(), used, "ERR_NOT_ENOUGH_MONEY");
            }
            if (s.primaryProfessionFirstRank && !s.canLearnPrimaryProfession) {
                appendFormat(req.data(), req.size(), used, "%s", used > 0 ? "\n" : "");
                appendText(req.data(), req.size(), used, "VD_COMBAT_TWO_PROFESSIONS");
            }
        }
        view.tooltip.requirement = req.data();
        view.canLearn = s.state == net::TrainerSpellState::Available && money >= s.cost &&
                        (!s.primaryProfessionFirstRank || s.canLearnPrimaryProfession);
    }
    const TrainerResult r = trainerWindow(c, view);
    if (r.select >= 0) {
        m_trainerSelected = r.select;
    }
    if (r.learn && m_trainerSelected >= 0) {
        const net::TrainerList::Spell& s = services[m_serviceIndex[static_cast<u32>(m_trainerSelected)]];
        if (s.primaryProfessionFirstRank) {
            m_confirmService = s.spellId; // a primary profession: asked first (only two can be known)
            m_modal = Modal::ConfirmProfession;
        } else {
            abilities.buyTrainerSpell(s.spellId);
            trace("trainer: buy %u for %u copper", s.spellId, s.cost);
        }
    }
    if (r.toggleAvailable || r.toggleUnavailable || r.toggleKnown) {
        m_showAvailable ^= r.toggleAvailable;
        m_showUnavailable ^= r.toggleUnavailable;
        m_showKnown ^= r.toggleKnown;
        m_trainerSelected = -1;
    }
    if (r.closed) {
        abilities.closeTrainer();
    }
}

void CombatScreens::talents(GameSession& session, DialogContext& c) {
    game::Gameplay& play = session.play();
    const Entity* me = session.entities().find(session.playerGuid());
    if (!m_abilities || !me) {
        close(CombatWindow::Talents);
        return;
    }
    const game::SpellBook& spells = play.spells();
    const u32 cls = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
    const u32 level = me->value(uf::UNIT_FIELD_LEVEL);
    const u32 freePoints = me->value(uf::PLAYER_CHARACTER_POINTS1);
    const u32 treeCount = std::min<u32>(m_abilities->talentTabs(cls, m_trees), 3);
    m_talentTree = std::min(m_talentTree, treeCount ? treeCount - 1 : 0);
    // Every tree's points; the buttons of the tree shown.
    m_talentViews.clear();
    m_talentInfos.clear();
    for (u32 t = 0; t < treeCount; ++t) {
        TalentTreeView& tree = m_treeViews[t];
        tree = TalentTreeView{};
        tree.name = m_trees[t].name;
        tree.background = m_trees[t].background;
        tree.points = game::treePoints(*m_abilities, m_trees[t].id, spells);
    }
    if (treeCount > 0) {
        const std::span<const game::TalentInfo> list = m_abilities->talents(m_trees[m_talentTree].id);
        for (const game::TalentInfo& t : list) {
            const game::TalentStatus status = game::talentStatus(*m_abilities, t, spells, freePoints);
            TalentButtonView v;
            const std::optional<game::SpellInfo> info = m_data ? m_data->spell(t.ranks[0]) : std::nullopt;
            v.iconHash = info ? info->iconHash : 0;
            v.row = t.row;
            v.column = t.column;
            v.rank = status.rank;
            v.maxRank = status.maxRank;
            v.available = status.available && level >= game::kTalentMinLevel;
            v.prerequisiteMet = status.prerequisiteMet;
            for (u32 i = 0; i < list.size(); ++i) {
                if (t.dependsOn != 0 && list[i].id == t.dependsOn) {
                    v.prerequisite = static_cast<i32>(i);
                }
            }
            m_talentViews.push_back(v);
            m_talentInfos.push_back(&t);
        }
        m_treeViews[m_talentTree].talents = m_talentViews;
    }
    if (m_talentSelected >= static_cast<i32>(m_talentViews.size())) {
        m_talentSelected = -1;
    }
    TalentView view;
    view.trees = {m_treeViews.data(), treeCount};
    view.tree = m_talentTree;
    view.selected = m_talentSelected;
    view.freePoints = freePoints;
    if (level < game::kTalentMinLevel) {
        view.notice = text::format(m_talentNotice, "VD_COMBAT_TALENT_LEVEL", game::kTalentMinLevel);
    }
    SpellTooltipView tip;
    if (m_talentSelected >= 0) {
        const game::TalentInfo& t = *m_talentInfos[static_cast<u32>(m_talentSelected)];
        const TalentButtonView& b = m_talentViews[static_cast<u32>(m_talentSelected)];
        const u32 current = b.rank > 0 ? t.ranks[b.rank - 1] : t.ranks[0];
        const u32 next = b.rank > 0 && b.rank < b.maxRank ? t.ranks[b.rank] : 0;
        view.tooltip = tooltip(session, m_talentTip, current, next, tip);
        view.tooltip.rank = text::format(m_talentRank, "TOOLTIP_TALENT_RANK", b.rank, b.maxRank);
        view.tooltip.cost = view.tooltip.range = view.tooltip.castTime = view.tooltip.cooldown = {};
        // What it still needs (the talent frame's TOOLTIP_TALENT_* lines).
        std::array<char, 256>& req = m_talentTip.requirement;
        req[0] = 0;
        usize used = 0;
        const game::TalentStatus status = game::talentStatus(*m_abilities, t, spells, freePoints);
        if (b.rank < b.maxRank) {
            if (level < game::kTalentMinLevel) {
                appendText(req.data(), req.size(), used, "REQUIRES_LABEL");
                appendFormat(req.data(), req.size(), used, " ");
                appendText(req.data(), req.size(), used, "TRAINER_REQ_LEVEL_RED", game::kTalentMinLevel);
            } else if (!status.rowOpen) {
                appendText(req.data(), req.size(), used, "TOOLTIP_TALENT_TIER_POINTS",
                           t.row * game::kTalentPointsPerRow, m_trees[m_talentTree].name);
            } else if (!status.prerequisiteMet && t.dependsOn != 0) {
                const game::TalentInfo* other = m_abilities->talent(t.dependsOn);
                const std::string_view name =
                    other ? m_abilities->spellText().name(other->ranks[0]) : std::string_view{};
                appendText(req.data(), req.size(), used,
                           t.dependsOnRank == 0 ? "TOOLTIP_TALENT_PREREQ" : "TOOLTIP_TALENT_PREREQ_P1",
                           t.dependsOnRank + 1, name);
            } else if (freePoints == 0) {
                appendText(req.data(), req.size(), used, "VD_COMBAT_NO_TALENT_POINTS");
            }
        }
        view.tooltip.requirement = req.data();
        view.canLearn = b.available;
    }
    const TalentResult r = talentWindow(c, view);
    if (r.tree >= 0 && static_cast<u32>(r.tree) != m_talentTree) {
        m_talentTree = static_cast<u32>(r.tree);
        m_talentSelected = -1;
    }
    if (r.select >= 0) {
        m_talentSelected = r.select;
    }
    if (r.learn && m_talentSelected >= 0) {
        const game::TalentInfo& t = *m_talentInfos[static_cast<u32>(m_talentSelected)];
        play.abilities().learnTalent(t.id, m_talentViews[static_cast<u32>(m_talentSelected)].rank);
        trace("talent %u: learn rank %u", t.id, m_talentViews[static_cast<u32>(m_talentSelected)].rank);
    }
    if (r.closed) {
        close(CombatWindow::Talents);
    }
}

void CombatScreens::actionEdit(GameSession& session, DialogContext& c) {
    game::Gameplay& play = session.play();
    const game::ActionButton button = play.spells().button(m_editSlot);
    ActionEditView view;
    // Page and position as the HUD shows them (the bottom left bar's: page 6).
    view.position = m_editSlot % game::kActionBarButtons + 1;
    view.page = m_editPage;
    SpellTooltipView tip;
    if (button.type == net::kActionSpell && button.action != 0) {
        const std::optional<game::SpellInfo> info = m_data ? m_data->spell(button.action) : std::nullopt;
        view.iconHash = info ? info->iconHash : 0;
        view.name = info ? info->name : std::string_view{};
        view.tooltip = tooltip(session, m_bookTip, button.action, 0, tip);
    } else if (button.type == net::kActionItem) {
        if (const net::ItemQueryInfo* info = session.item(button.action)) {
            view.iconHash = m_data ? m_data->itemIconHash(info->displayId) : 0;
            view.name = info->name;
        }
    } else if (const game::Macro* macro = m_macros ? m_macros->macro(game::macroIdOf(button)) : nullptr) {
        view.iconHash = macro->iconHash; // phase 15 (options)
        view.name = macro->name;
    }
    const ActionEditResult r = actionEditDialog(c, view);
    if (r.pickSpell) {
        m_modal = Modal::PickSpell;
    } else if (r.pickItem) {
        m_modal = Modal::ItemPicker;
    } else if (r.clear) {
        play.setActionButton(m_editSlot, {});
        trace("action button %u cleared", m_editSlot);
        m_modal = Modal::None;
    } else if (r.closed) {
        m_modal = Modal::None;
    }
}

void CombatScreens::pickItem(GameSession& session, DialogContext& c) {
    const Entity* me = session.entities().find(session.playerGuid());
    m_usableItems.clear();
    m_itemViews.clear();
    if (me) {
        // Items with an on-use spell or that start a quest, one row per kind of item.
        game::forEachInventoryItem(session.entities(), *me, [&](const game::InventoryItem& item) {
            for (const game::InventoryItem& seen : m_usableItems) {
                if (seen.entry == item.entry) {
                    return;
                }
            }
            const net::ItemQueryInfo* info = session.item(item.entry);
            if (!info) {
                return; // asked for: listed when known
            }
            bool usable = info->startQuest != 0;
            for (const net::ItemQueryInfo::Spell& spell : info->spells) {
                usable = usable || (spell.id != 0 && spell.trigger == net::kItemSpellTriggerOnUse);
            }
            if (!usable) {
                return;
            }
            m_usableItems.push_back(item);
            ItemView v;
            v.name = info->name;
            v.quality = info->quality;
            v.iconHash = m_data ? m_data->itemIconHash(info->displayId) : 0;
            v.count = game::inventoryCount(session.entities(), *me, item.entry);
            m_itemViews.push_back(v);
        });
    }
    bool closed = false;
    const i32 picked = itemPicker(c, m_itemViews, closed);
    if (picked >= 0) {
        session.play().setActionButton(m_editSlot,
                                       {net::kActionItem, m_usableItems[static_cast<u32>(picked)].entry});
        trace("action button %u <- item %u", m_editSlot, m_usableItems[static_cast<u32>(picked)].entry);
        m_modal = Modal::None;
    } else if (closed) {
        m_modal = Modal::None;
    }
}

// --- HUD pieces --------------------------------------------------------------------------------------------

void CombatScreens::drawHud(GameSession& session, Hud& hud, DialogContext& c, u32 nowMs) {
    VD_PROFILE_ZONE("CombatScreens::drawHud");
    m_nowMs = nowMs;
    m_petFrameBottom = 0.0f;
    m_petFrameRect = {};
    m_totRect = {}; // phase 20 (assists)
    if (!session.inWorld() || !session.entities().find(session.playerGuid())) {
        return;
    }
    targetHud(session, hud, c, nowMs);
    petHud(session, hud, c);
    drawPetPlacing(hud, c); // phase 15 (pets)
    if (!m_tipOverTouch) {
        actionTooltip(session, c); // a held action or stance button (parity pass)
    }
    // Placing a spell: what and how, over the action bar.
    if (!m_placing.empty()) {
        ui::Gui g{c.ui, c.draw};
        const ui::Rect& bar = hud.actionBarRect();
        const f32 h = g.px(40.0f);
        const f32 w = std::min(g.px(360.0f), c.safeArea.w - g.px(16.0f));
        const ui::Rect banner{std::max(c.safeArea.x + g.px(8.0f), bar.right() - w), bar.y - h - g.px(8.0f), w,
                              h};
        ui::tooltipPanel(g, banner);
        const std::string_view name =
            m_abilities ? m_abilities->spellText().name(m_placing.action) : std::string_view{};
        // Phase 15 (options): a macro picked up from the macro window.
        const game::Macro* placingMacro = m_macros ? m_macros->macro(game::macroIdOf(m_placing)) : nullptr;
        char hint[128];
        const std::string_view said =
            text::format(hint, "VD_COMBAT_PLACE_HINT", placingMacro ? std::string_view(placingMacro->name) : name);
        const f32 cancelW = g.px(96.0f);
        ui::label(g, {banner.x + g.px(10.0f), banner.y, banner.w - cancelW - g.px(16.0f), banner.h}, said,
                  ui::gameFont(g.px(13.0f), ui::kWhite));
        if (ui::button(g, ui::widgetId("placing.cancel"),
                       {banner.right() - cancelW - g.px(4.0f), banner.y + g.px(4.0f), cancelW,
                        banner.h - g.px(8.0f)},
                       text::get("CANCEL"))) {
            m_placing = {};
        }
    }
}

void CombatScreens::petHud(GameSession& session, Hud& hud, DialogContext& c) {
    game::Gameplay& play = session.play();
    game::Abilities& abilities = play.abilities();
    const game::PetState& pet = abilities.pet();
    const Entity* unit = pet.active() ? session.entities().find(pet.guid) : nullptr;
    if (!unit) {
        return;
    }
    ui::Gui g{c.ui, c.draw};
    // The frame under the player frame, where the client's PetFrame hangs (80, 60 units in).
    const ui::Rect& player = hud.playerFrameRect();
    const f32 s = player.w / 232.0f;
    const ui::Rect frameRect{player.x + 80.0f * s, player.y + 60.0f * s, 128.0f * s, 64.0f * s};
    m_petFrameBottom = frameRect.bottom();
    m_petFrameRect = frameRect;
    m_petFrameGuid = pet.guid; // phase 20 (assists)
    PetFrameView frame;
    m_petName.assign(abilities.petName());
    if (m_petName.empty()) {
        if (const net::CreatureQueryInfo* info = session.creature(unit->value(uf::OBJECT_FIELD_ENTRY))) {
            m_petName.assign(info->name);
        }
    }
    frame.name = m_petName;
    frame.level = unit->value(uf::UNIT_FIELD_LEVEL);
    const u32 maxHealth = std::max(1u, unit->value(uf::UNIT_FIELD_MAXHEALTH));
    frame.health = static_cast<f32>(unit->value(uf::UNIT_FIELD_HEALTH)) / static_cast<f32>(maxHealth);
    frame.powerType = std::min<u32>(unit->byteValue(uf::UNIT_FIELD_BYTES_0, 3), 4);
    const u32 maxPower = unit->value(static_cast<u16>(uf::UNIT_FIELD_MAXPOWER1 + frame.powerType));
    frame.power =
        maxPower ? static_cast<f32>(unit->value(static_cast<u16>(uf::UNIT_FIELD_POWER1 + frame.powerType))) /
                       static_cast<f32>(maxPower)
                 : 0.0f;
    // Hunter pets: happiness in POWER5 (out of 1 000 000), three moods.
    const u32 maxHappiness = unit->value(uf::UNIT_FIELD_MAXPOWER5);
    if (maxHappiness > 0) {
        const u32 happiness = unit->value(uf::UNIT_FIELD_POWER5);
        frame.happiness = happiness < 333000 ? 1 : happiness < 666000 ? 2 : 3;
    }
    frame.selected = play.target() == pet.guid;
    frame.portrait = m_portraits ? m_portraits->unit(pet.guid) : TextureHandle{};
    const PetFrameResult tapped = petFrame(g, frameRect, frame);
    if (tapped.tapped) {
        session.setTarget(pet.guid);
    }
    if (tapped.held) { // phase 13 (social): the right click, PetFrameDropDown ("PET": app/UnitMenuScreens)
        m_petFrameHeld = true;
    }

    // The bar (PetActionBar_Update): its ten buttons as the server lays them out (Attack, Follow, Stay, four spells,
    // Aggressive, Defensive, Passive by default), each command and reaction checked while in use. Phase 15 (pets): the
    // empty spell slots show only while a spell of the pet's book is being placed (PET_BAR_SHOWGRID), lit where it
    // may go; a finger dragged from a button to another makes them change places (PickupPetAction twice).
    u32 count = 0;
    for (u32 slot = 0; slot < net::kPetActionBarSize && count < m_petSlots.size(); ++slot) {
        const u32 packed = pet.bar[slot];
        const u8 type = net::petButtonType(packed);
        const u32 action = net::petButtonAction(packed);
        PetSlotView v;
        if (type == net::kPetActCommand) {
            v.iconHash = action == net::kPetCommandAttack   ? kPetAttack
                         : action == net::kPetCommandFollow ? kPetFollow
                         : action == net::kPetCommandStay   ? kPetStay
                                                            : 0;
            v.active = action != net::kPetCommandAttack && pet.commandState == action;
        } else if (type == net::kPetActReaction) {
            v.iconHash = action == net::kPetReactAggressive  ? kPetAggressive
                         : action == net::kPetReactDefensive ? kPetDefensive
                                                             : kPetPassive;
            v.active = action == pet.reactState;
        } else if (action != 0) {
            const std::optional<game::SpellInfo> info = m_data ? m_data->spell(action) : std::nullopt;
            v.iconHash = info ? info->iconHash : 0;
            v.autocast = type == net::kPetActEnabled;
            v.autocastable = type == net::kPetActEnabled || type == net::kPetActDisabled;
            const game::PetCooldown cd = abilities.petCooldown(action, m_nowMs);
            if (cd.durationMs > 0) {
                v.cooldown = static_cast<f32>(cd.durationMs - (m_nowMs - cd.startMs)) /
                             static_cast<f32>(cd.durationMs);
            }
            if (info) {
                const u32 powerType = std::min<u32>(info->powerType, 4);
                v.usable =
                    unit->value(static_cast<u16>(uf::UNIT_FIELD_POWER1 + powerType)) >= info->powerCost;
            }
            // Phase 20 (pets): a possessed unit's spells come as ACT_PASSIVE and are cast (HandlePetAction): only
            // SPELL_ATTR_PASSIVE ones cannot be.
            v.usable = v.usable && !(info && info->passive());
            v.target = m_placingPet != 0;
        } else if (m_placingPet == 0) {
            continue; // an empty spell slot: hidden (showgrid 0)
        } else {
            v.target = true;
        }
        if (v.iconHash == 0 && !v.target) {
            continue;
        }
        m_petSlots[count] = v;
        m_petSlotIndex[count] = slot;
        ++count;
    }
    if (count == 0 || !pet.enabled || !m_showPetBar) { // phase 20 (pets): the touch cluster's option
        return;
    }
    // Left of the action cluster, along the bottom, its right end against it; over the stance row when that runs
    // along the bottom too (the cover screen).
    const ui::Rect& cluster = hud.actionBarRect();
    const ui::Rect& stances = hud.stanceBarRect();
    const f32 size = g.px(34.0f), gap = g.px(4.0f);
    const f32 width = static_cast<f32>(count) * size + static_cast<f32>(count - 1) * gap;
    f32 right = cluster.x - g.px(10.0f);
    f32 bottom = c.safeArea.bottom() - g.px(14.0f);
    if (stances.h > 0.0f && stances.bottom() > cluster.y + cluster.h * 0.5f) {
        right = stances.right();
        bottom = stances.y - g.px(8.0f);
    }
    const ui::Rect barRect{std::max(c.safeArea.x + g.px(8.0f), right - width), bottom - size, width, size};
    const PetBarResult r = petBar(g, c.resources, barRect, {m_petSlots.data(), count});
    if (r.tapped >= 0 && m_placingPet != 0) {
        // Phase 15 (pets): the spell being placed goes into a spell slot (command and reaction buttons stay: the server
        // only lets them move, never go).
        const u32 slot = m_petSlotIndex[static_cast<u32>(r.tapped)];
        const u8 type = net::petButtonType(pet.bar[slot]);
        if (type != net::kPetActCommand && type != net::kPetActReaction) {
            abilities.setPetSlot(slot, m_placingPet);
            trace("pet bar slot %u <- spell %u", slot, m_placingPet);
            m_placingPet = 0;
        }
    } else if (r.tapped >= 0) {
        const u32 slot = m_petSlotIndex[static_cast<u32>(r.tapped)];
        abilities.petAction(slot, play.target()); // CastPetAction: commands, reactions, spells
        trace("pet action slot %u (%08x) at %llx", slot, pet.bar[slot], static_cast<unsigned long long>(play.target()));
    }
    if (r.held >= 0) {
        const u32 slot = m_petSlotIndex[static_cast<u32>(r.held)];
        const u8 type = net::petButtonType(pet.bar[slot]);
        if (type == net::kPetActEnabled || type == net::kPetActDisabled) {
            abilities.setPetAutocast(slot, type == net::kPetActDisabled);
            trace("pet autocast slot %u: %d", slot, type == net::kPetActDisabled ? 1 : 0);
        }
    }
    if (r.dragFrom >= 0 && r.dragTo >= 0) {
        const u32 from = m_petSlotIndex[static_cast<u32>(r.dragFrom)], to = m_petSlotIndex[static_cast<u32>(r.dragTo)];
        if (abilities.swapPetSlots(from, to)) {
            trace("pet bar slots %u <-> %u", from, to);
        }
    }
}

void CombatScreens::drawPetPlacing(Hud& hud, DialogContext& c) {
    // Phase 15 (pets): what is being put on the pet bar and how (the pet's spell on the cursor, PET_BAR_SHOWGRID).
    if (m_placingPet == 0) {
        return;
    }
    ui::Gui g{c.ui, c.draw};
    const ui::Rect& bar = hud.actionBarRect();
    const f32 h = g.px(40.0f);
    const f32 w = std::min(g.px(360.0f), c.safeArea.w - g.px(16.0f));
    const ui::Rect banner{std::max(c.safeArea.x + g.px(8.0f), bar.x - w - g.px(10.0f)), bar.y - h - g.px(56.0f), w, h};
    ui::tooltipPanel(g, banner);
    const std::string_view name = m_abilities ? m_abilities->spellText().name(m_placingPet) : std::string_view{};
    char hint[128];
    const std::string_view said = text::format(hint, "VD_PETS_PLACE_HINT", name);
    const f32 cancelW = g.px(96.0f);
    ui::label(g, {banner.x + g.px(10.0f), banner.y, banner.w - cancelW - g.px(16.0f), banner.h}, said,
              ui::gameFont(g.px(13.0f), ui::kWhite));
    if (ui::button(g, ui::widgetId("petPlacing.cancel"),
                   {banner.right() - cancelW - g.px(4.0f), banner.y + g.px(4.0f), cancelW, banner.h - g.px(8.0f)},
                   text::get("CANCEL"))) {
        m_placingPet = 0;
    }
}

void CombatScreens::targetHud(GameSession& session, Hud& hud, DialogContext& c, u32 /*nowMs*/) {
    game::Gameplay& play = session.play();
    const u64 targetGuid = play.target();
    const Entity* target = targetGuid ? session.entities().find(targetGuid) : nullptr;
    const Entity* me = session.entities().find(session.playerGuid());
    if (!target || !me || !game::isUnit(*target)) {
        return;
    }
    ui::Gui g{c.ui, c.draw};
    const ui::Rect& frame = hud.targetFrameRect();
    const f32 s = frame.w / 232.0f;

    // Combo points (ComboFrame): five gems arcing right of the target's portrait.
    const u32 combo = game::comboPoints(*me, targetGuid);
    if (combo > 0) {
        // ComboFrame.xml: a 12 x 16 socket (texels 0..0.375 of ComboPoint), the lit gem 8 x 16
        // (0.375..0.5625) over it, five of them down the frame's right side (a little larger than the
        // client's, for a phone).
        static constexpr f32 kArc[5][2] = {{196, 8}, {204, 20}, {208, 34}, {206, 48}, {199, 60}};
        const f32 k = s * 1.3f;
        for (u32 i = 0; i < 5; ++i) {
            const ui::Rect socket{frame.x + kArc[i][0] * s, frame.y + kArc[i][1] * s, 12.0f * k, 16.0f * k};
            g.draw.sprite(socket, kComboPoint, ui::kWhite, {0.0f, 0.0f, 0.375f, 1.0f});
            if (i < combo) {
                g.draw.sprite({socket.x + 2.0f * k, socket.y, 8.0f * k, 16.0f * k}, kComboPoint, ui::kWhite,
                              {0.375f, 0.0f, 0.5625f, 1.0f});
            }
        }
    }

    // No target cast bar: 1.12's TargetFrame has none.

    // Target of target (TargetofTargetFrame): its name and health, right of the target frame.
    const u64 totGuid = game::guidValue(*target, uf::UNIT_FIELD_TARGET);
    const Entity* tot = totGuid && m_targetOfTargetShown ? session.entities().find(totGuid) : nullptr; // phase 15
    if (tot && game::isUnit(*tot)) {
        const ui::Rect r{frame.right() - 30.0f * s, frame.y + 64.0f * s, 93.0f * s, 34.0f * s};
        m_totRect = r; // phase 20 (assists): a helpful spell may be dragged onto it
        m_totGuid = totGuid;
        const ui::WidgetState state = c.ui.widget(ui::widgetId("target.tot"), r);
        g.draw.solid({r.x + 30.0f * s, r.y + 6.0f * s, 58.0f * s, 14.0f * s}, rgba(0, 0, 0, 160));
        const f32 health =
            std::clamp(static_cast<f32>(tot->value(uf::UNIT_FIELD_HEALTH)) /
                           static_cast<f32>(std::max(1u, tot->value(uf::UNIT_FIELD_MAXHEALTH))),
                       0.0f, 1.0f);
        g.draw.sprite({r.x + 30.0f * s, r.y + 16.0f * s, 58.0f * s * health, 5.0f * s}, kStatusBar,
                      rgba(0, 255, 0), {0.0f, 0.0f, health, 1.0f});
        // Its portrait under the art (TargetofTargetPortrait: 35 units at 6, 6 of the 45-unit high art, drawn
        // here `squash` as high).
        const f32 squash = r.h / (45.0f * s);
        const f32 hole = 35.0f * s * squash;
        g.draw.portrait({r.x + 23.5f * s - hole * 0.5f, r.y + 23.5f * s * squash - hole * 0.5f, hole, hole},
                        m_portraits ? m_portraits->unit(totGuid) : TextureHandle{}, kMonsterPortrait);
        g.draw.sprite(r.inset(0.0f, 0.0f), kTargetOfTarget, ui::kWhite,
                      {0.015625f, 0.0f, 0.7421875f, 0.703125f});
        std::string_view name;
        if (tot->guid == session.playerGuid() || (totGuid >> 48) == 0) {
            const std::string* found = session.playerName(totGuid);
            name = found ? std::string_view(*found) : std::string_view{};
        } else if (const net::CreatureQueryInfo* info =
                       session.creature(tot->value(uf::OBJECT_FIELD_ENTRY))) {
            name = info->name;
        }
        ui::TextStyle style = ui::gameFont(std::max(g.px(9.0f), 9.0f * s * 1.2f), ui::kGold);
        style.shadow = std::max(1.0f, s);
        ui::label(g, {r.x + 32.0f * s, r.y + 2.0f * s, 60.0f * s, 12.0f * s}, name, style);
        if (state.clicked) {
            session.setTarget(totGuid);
        }
    }
}

} // namespace vd
