#include "app/TouchSmart.h"

#include "app/CombatScreens.h"
#include "app/Dialogs.h"
#include "app/GameSession.h"
#include "app/Hud.h"
#include "app/TouchBarScreens.h"
#include "app/TouchBarView.h"
#include "app/UiResources.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/ActionBar.h"
#include "game/Auras.h"
#include "game/ItemDefs.h"
#include "game/Progress.h"
#include "game/SpellLines.h"
#include "game/UnitInfo.h"
#include "net/Messages.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vd {

// Phase 20 (smart): the consumables, the mount and the reactive button (docs/MOBILE_CONTROLS.md §6.1-6.3),
// the context button's new subjects (§6.4) and the next enemy's rule (§6.5).

namespace uf = net::uf;
using game::Entity;
using game::TouchAction;
using game::TouchActionKind;
using game::TouchConsumable;
using game::touchIndex;
using game::TouchPlace;

namespace {

// Spell.dbc numbers (vMaNGOS SpellDefines.h / SharedDefines.h).
constexpr u32 kEffectResurrect = 18;     // SPELL_EFFECT_RESURRECT
constexpr u32 kEffectResurrectNew = 113; // SPELL_EFFECT_RESURRECT_NEW: Resurrection, Redemption, Rebirth...
constexpr u32 kEffectOpenLock = 33;      // SPELL_EFFECT_OPEN_LOCK (misc: the LockType)
constexpr u32 kEffectSkinning = 95;      // SPELL_EFFECT_SKINNING
constexpr u32 kEffectApplyAura = 6;
constexpr u32 kAuraMechanicImmunity = 77;          // SPELL_AURA_MECHANIC_IMMUNITY (misc: the mechanic)
constexpr i32 kMechanicBandage = 16;               // MECHANIC_BANDAGE: Recently Bandaged
constexpr u32 kAuraInterruptDamage = 0x2;          // AURA_INTERRUPT_DAMAGE_CANCELS
constexpr u32 kLockHerbalism = 2, kLockMining = 3; // LockType.dbc
constexpr u32 kGameObjectFlagInUse = 0x01;         // GO_FLAG_IN_USE: someone is gathering it
constexpr u32 kGameObjectStateReady = 1;           // GO_STATE_READY
constexpr f32 kGatherSearchYards = 12.0f;          // as far as the context button looks for objects
constexpr u32 kGatherScanMs = 250;
constexpr u32 kBagRetryMs = 500;       // the bags scanned again while item queries are on their way
constexpr u32 kSkillsReadMs = 1000;    // the skill fields read again (an item's required skill)
constexpr u32 kReactiveSoundMs = 2000; // igBonusBarOpen at most this often
constexpr u32 kGcdSlackMs = 100; // a cooldown no longer than the global one (and this) is the global one
// CombatScreens::touchView caches of the reactive button's fan (TouchBarScreens keeps 0..27).
constexpr u32 kCacheReactiveFan = 112; // after the drawer's (TouchEditScreens: 40..111)

constexpr u64 kIconHealth = assetPathHash("Interface\\Icons\\INV_Potion_54.blp");
constexpr u64 kIconMana = assetPathHash("Interface\\Icons\\INV_Potion_76.blp");
constexpr u64 kIconRage = assetPathHash("Interface\\Icons\\INV_Potion_24.blp");
constexpr u64 kIconEnergy = assetPathHash("Interface\\Icons\\INV_Drink_Milk_02.blp");
constexpr u64 kIconBandage = assetPathHash("Interface\\Icons\\INV_Misc_Bandage_15.blp");
constexpr u64 kIconFood = assetPathHash("Interface\\Icons\\INV_Misc_Food_15.blp");
constexpr u64 kIconDrink = assetPathHash("Interface\\Icons\\INV_Drink_07.blp");
constexpr u64 kCheck = assetPathHash("Interface\\Buttons\\UI-CheckBox-Check.blp");
constexpr u64 kHighlight = assetPathHash("Interface\\QuestFrame\\UI-QuestTitleHighlight.blp");
constexpr u64 kIconMount = assetPathHash("Interface\\Icons\\Ability_Mount_RidingHorse.blp");

// The order the button offers the travel spells in: class mounts, then Travel Form (outdoors), Cat Form (what
// is left indoors), Ghost Wolf, Aspect of the Cheetah, Aquatic Form (swimming).
u32 travelOrder(const TouchAction& action, bool mount) {
    if (mount) {
        return 0;
    }
    switch (action.id) {
    case game::kSpellTravelForm: return 1;
    case game::kSpellCatForm: return 2;
    case game::kSpellGhostWolf: return 3;
    case game::kSpellAspectOfTheCheetah: return 4;
    default: return 5;
    }
}

// The consumable a slot falls back to when an item fixed there is let go and fits no kind (its default,
// §6.1).
TouchConsumable slotKind(u32 slot) {
    return slot == touchIndex(TouchPlace::Consumable3)   ? TouchConsumable::Bandage
           : slot == touchIndex(TouchPlace::Consumable2) ? TouchConsumable::Mana
                                                         : TouchConsumable::Health;
}

} // namespace

void TouchSmart::init(const game::GameData* data, const game::AbilityData* abilities) {
    m_data = data;
    m_abilities = abilities;
    const char* demo = VD_DEV_ENV("VD_TOUCH_SMART_DEMO");
    m_demo = demo && *demo && *demo != '0';
    const char* trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY");
    m_trace = trace && *trace && *trace != '0';
}

void TouchSmart::enterWorld() {
    m_player = 0;
    m_refreshed = false;
    m_spellsVersion = 0; // SpellBook::knownVersion() starts at 1: scanned again
    m_bagVersion = 0xFFFFFFFFu;
    m_bagPending = false;
    m_consumableCount = m_bagMountCount = m_spellTravelCount = m_travelCount = 0;
    m_reactiveCount = m_reactiveNowCount = m_resurrectCount = 0;
    m_reactiveGlow = 0;
    m_reactiveShown = false;
    m_skinning = {};
    m_gatherLocks = 0;
    m_gatherNode = 0;
    m_subjectSpell = 0;
    m_skillCount = 0;
    m_choiceFrame = {};
    m_travelFrame = 0xFFFFFFFFu;
    m_listPlace = -1;
}

// --- The player
// -------------------------------------------------------------------------------------------------

void TouchSmart::refresh(GameSession& session, const HudFrame& hud, u32 nowMs) {
    if (m_refreshed && nowMs == m_nowMs) {
        return; // GameScreens and TouchBarScreens both ask in a frame
    }
    VD_PROFILE_ZONE("TouchSmart::refresh");
    m_refreshed = true;
    m_nowMs = nowMs;
    ++m_frame;
    m_session = &session;
    m_reactions = hud.reactions;
    m_indoors = hud.indoors;
    const Entity* me = session.entities().find(session.playerGuid());
    if (!me) {
        m_player = 0;
        return;
    }
    m_player = me->guid;
    m_level = me->value(uf::UNIT_FIELD_LEVEL);
    m_maxHealth = me->value(uf::UNIT_FIELD_MAXHEALTH);
    m_maxMana = me->value(uf::UNIT_FIELD_MAXPOWER1);
    m_inCombat = game::inCombat(*me);
    m_swimming = (me->moveFlags & net::kMoveFlagSwimming) != 0;
    m_form = me->byteValue(uf::UNIT_FIELD_BYTES_1, 2);
    if (m_skillCount == 0 || nowMs - m_skillsReadMs >= kSkillsReadMs) {
        m_skillsReadMs = nowMs;
        std::array<game::SkillValue, game::kMaxSkills> skills;
        const u32 count = game::readSkills(*me, skills);
        m_skillCount = 0;
        for (u32 i = 0; i < count && m_skillCount < m_skills.size(); ++i) {
            const i32 rank = static_cast<i32>(skills[i].value) + skills[i].bonus;
            m_skills[m_skillCount++] = {skills[i].id, static_cast<u32>(std::max(0, rank))};
        }
    }
    const game::SpellBook& spells = session.play().spells();
    if (m_abilities && m_data && spells.knownVersion() != m_spellsVersion) {
        scanSpells(session);
    }
    // The player's auras the buttons depend on: the mount's (dismount), Recently Bandaged, an aspect in use.
    m_mountAura = 0;
    m_bandaged = false;
    m_bandagedLeftMs = m_bandagedTotalMs = 0;
    const bool mounted = me->value(uf::UNIT_FIELD_MOUNTDISPLAYID) != 0;
    for (u32 slot = 0; slot < game::kMaxAuras; ++slot) {
        const u32 spellId = me->value(static_cast<u16>(uf::UNIT_FIELD_AURA + slot));
        if (spellId == 0) {
            continue;
        }
        const u8 flags = spellFlags(spellId);
        if (mounted && slot < game::kMaxPositiveAuras && (flags & kFlagMount) != 0) {
            m_mountAura = spellId;
        }
        if (slot >= game::kMaxPositiveAuras && (flags & kFlagBandaged) != 0) {
            m_bandaged = true;
            std::array<game::AuraInfo, game::kMaxAuras> auras;
            const u32 count = session.play().auras(me->guid, auras);
            for (u32 i = 0; i < count; ++i) {
                if (auras[i].spellId == spellId && auras[i].timed()) {
                    m_bandagedLeftMs = auras[i].remainingMs;
                    m_bandagedTotalMs = auras[i].durationMs;
                }
            }
        }
    }
    for (u32 i = 0; i < m_spellTravelCount; ++i) { // Aspect of the Cheetah in use: a tap cancels it
        game::TravelOption& o = m_spellTravel[i];
        o.active = false;
        for (u32 slot = 0; slot < game::kMaxPositiveAuras && o.form == 0 && !o.mount; ++slot) {
            o.active = o.active || me->value(static_cast<u16>(uf::UNIT_FIELD_AURA + slot)) == o.action.id;
        }
    }
}

u8 TouchSmart::spellFlags(u32 spellId) {
    const u32 size = static_cast<u32>(m_spellFacts.size());
    u32 index = (spellId * 2654435761u) % size;
    SpellFact* free = nullptr;
    for (u32 probe = 0; probe < 16; ++probe, index = (index + 1) % size) {
        SpellFact& f = m_spellFacts[index];
        if (f.spellId == spellId) {
            return f.flags;
        }
        if (f.spellId == 0) {
            free = &f;
            break;
        }
    }
    u8 flags = kFlagKnown;
    game::SpellDetails d;
    if (m_abilities && m_abilities->spellText().details(spellId, d)) {
        if ((d.auraInterruptFlags & kAuraInterruptDamage) != 0) {
            flags |= kFlagBreaks;
        }
        for (const game::SpellEffectData& e : d.effects) {
            if (e.effect == kEffectApplyAura && e.aura == kAuraMechanicImmunity &&
                e.miscValue == kMechanicBandage) {
                flags |= kFlagBandaged;
            }
        }
        if (game::isMountSpell(d)) {
            flags |= kFlagMount;
        }
    }
    if (free) {
        *free = {spellId, flags};
    }
    return flags;
}

u32 TouchSmart::skillRank(u32 skill) const {
    for (u32 i = 0; i < m_skillCount; ++i) {
        if (m_skills[i].id == skill) {
            return m_skills[i].rank;
        }
    }
    return 0;
}

bool TouchSmart::itemUsable(const Needs& needs) const {
    // Player::CanUseItem: the item's required level, its required skill at its rank.
    return m_level >= needs.level && (needs.skill == 0 || skillRank(needs.skill) >= needs.skillRank);
}

// --- What the player has
// ----------------------------------------------------------------------------------------

void TouchSmart::update(GameSession& session, const HudFrame& hud, const game::TouchAction& favorite,
                        u32 nowMs) {
    refresh(session, hud, nowMs);
    m_favorite = favorite;
    game::Inventory& inventory = session.play().inventory();
    if (m_player != 0 &&
        (inventory.version() != m_bagVersion || (m_bagPending && nowMs - m_bagScanMs >= kBagRetryMs))) {
        m_bagVersion = inventory.version();
        m_bagScanMs = nowMs;
        scanBags(session);
        composeTravel();
    }
}

void TouchSmart::scanBags(GameSession& session) {
    VD_PROFILE_ZONE("TouchSmart::scanBags");
    m_consumableCount = 0;
    m_bagMountCount = 0;
    m_bagPending = false;
    const Entity* me = session.entities().find(session.playerGuid());
    if (!me || !m_data || !m_abilities) {
        return;
    }
    // The kinds of items carried (backpack and bags), once each.
    std::array<u32, 96> entries;
    u32 count = 0;
    game::forEachInventoryItem(session.entities(), *me, [&](const game::InventoryItem& item) {
        if (item.entry == 0 || count == entries.size() ||
            std::find(entries.begin(), entries.begin() + count, item.entry) != entries.begin() + count) {
            return;
        }
        entries[count++] = item.entry;
    });
    const game::Inventory& inventory = session.play().inventory();
    for (u32 i = 0; i < count; ++i) {
        const net::ItemQueryInfo* info =
            session.item(entries[i]); // asked for when unknown: scanned again then
        if (!info) {
            m_bagPending = true;
            continue;
        }
        const Needs needs{info->requiredLevel, info->requiredSkill, info->requiredSkillRank};
        for (const net::ItemQueryInfo::Spell& use : info->spells) {
            if (use.id == 0 || use.trigger != net::kItemSpellTriggerOnUse) {
                continue;
            }
            const std::optional<game::SpellInfo> spell = m_data->spell(use.id);
            game::SpellDetails details;
            if (!spell || !m_abilities->spellText().details(use.id, details)) {
                continue;
            }
            // §6.1: what it restores, by its on-use spell.
            const game::ConsumableUse restores = game::classifyConsumable(*spell, details, use.category);
            if (restores.any()) {
                if (m_consumableCount < m_consumables.size()) {
                    game::ConsumableItem& c = m_consumables[m_consumableCount];
                    c = {};
                    c.entry = info->entry;
                    c.count = inventory.count(info->entry);
                    c.use = restores;
                    c.conjured = (info->flags & game::kItemFlagConjured) != 0;
                    m_consumableNeeds[m_consumableCount++] = needs;
                }
                break;
            }
            // §6.2: a mount (SPELL_AURA_MOUNTED) and its speed.
            if (game::isMountSpell(details)) {
                if (m_bagMountCount < m_bagMounts.size()) {
                    m_bagMounts[m_bagMountCount] =
                        game::travelOptionOf(details, TouchAction::item(info->entry));
                    m_bagMountNeeds[m_bagMountCount++] = needs;
                }
                break;
            }
        }
    }
    m_choiceFrame = {}; // chosen again
}

void TouchSmart::scanSpells(GameSession& session) {
    VD_PROFILE_ZONE("TouchSmart::scanSpells");
    const game::SpellBook& spells = session.play().spells();
    m_spellsVersion = spells.knownVersion();
    m_reactiveCount = 0;
    m_spellTravelCount = 0;
    m_resurrectCount = 0;
    m_skinning = {};
    m_gatherLocks = 0;
    const game::SpellText& text = m_abilities->spellText();
    for (const u32 id : spells.known()) {
        game::SpellDetails d;
        if (!text.details(id, d) || d.passive()) {
            continue;
        }
        KnownSpell s;
        s.spellId = id;
        s.nameHash = stringHash(d.name);
        s.rank = game::SpellLines::rankNumber(d.rank);
        s.cooldownMs = std::max(d.recoveryMs, d.categoryRecoveryMs);
        s.notInCombat = (d.attributes & game::kSpellAttrNotInCombat) != 0;
        const std::optional<game::SpellInfo> info = m_data->spell(id);
        s.icon = info ? info->iconHash : 0;
        // §6.3: what depends on an aura state of the caster or the target (Revenge, Execute, Riposte,
        // Mongoose Bite, Counterattack, Hammer of Wrath), and the warrior's Overpower (a combo point the
        // dodge gives): game/ReactiveSpells.
        if (info && game::isReactiveSpell(d)) {
            // The highest rank of a line (the spells of one name, as the spell book groups them).
            u32 at = m_reactiveCount;
            bool lower = false;
            for (u32 i = 0; i < m_reactiveCount; ++i) {
                if (m_reactive[i].spell.nameHash == s.nameHash) {
                    at = i;
                    lower = m_reactive[i].spell.rank >= s.rank;
                    break;
                }
            }
            if (!lower && at < m_reactive.size()) {
                m_reactive[at] = {s, *info, d};
                m_reactiveCount = std::max(m_reactiveCount, at + 1);
            }
        }
        bool mount = false;
        for (const game::SpellEffectData& e : d.effects) {
            if (e.effect == kEffectResurrect || e.effect == kEffectResurrectNew) { // §6.4
                bool replaced = false;
                for (u32 i = 0; i < m_resurrectCount && !replaced; ++i) {
                    if (m_resurrect[i].nameHash == s.nameHash) {
                        replaced = true;
                        if (m_resurrect[i].rank < s.rank) {
                            m_resurrect[i] = s;
                        }
                    }
                }
                if (!replaced && m_resurrectCount < m_resurrect.size()) {
                    m_resurrect[m_resurrectCount++] = s;
                }
            } else if (e.effect == kEffectSkinning) {
                m_skinning = s;
            } else if (e.effect == kEffectOpenLock && (e.miscValue == static_cast<i32>(kLockHerbalism) ||
                                                       e.miscValue == static_cast<i32>(kLockMining))) {
                m_gatherLocks |= 1u << e.miscValue;
            } else if (e.effect == kEffectApplyAura && e.aura == game::kAuraMounted) {
                mount = true; // §6.2: a class mount (Summon Felsteed, Warhorse...)
            }
        }
        const bool travelForm = id == game::kSpellTravelForm || id == game::kSpellAquaticForm ||
                                id == game::kSpellCatForm || id == game::kSpellGhostWolf ||
                                id == game::kSpellAspectOfTheCheetah;
        if ((mount || travelForm) && m_spellTravelCount < m_spellTravel.size()) {
            m_spellTravel[m_spellTravelCount++] = game::travelOptionOf(d, TouchAction::spell(id));
        }
    }
    std::stable_sort(m_spellTravel.begin(), m_spellTravel.begin() + m_spellTravelCount,
                     [](const game::TravelOption& a, const game::TravelOption& b) {
                         return travelOrder(a.action, a.mount) < travelOrder(b.action, b.mount);
                     });
    composeTravel();
}

void TouchSmart::composeTravel() {
    m_travelCount = 0;
    for (u32 i = 0; i < m_bagMountCount && m_travelCount < kMaxTravel; ++i) {
        m_travelNeeds[m_travelCount] = m_bagMountNeeds[i];
        m_travel[m_travelCount++] = m_bagMounts[i];
    }
    for (u32 i = 0; i < m_spellTravelCount && m_travelCount < kMaxTravel; ++i) {
        m_travelNeeds[m_travelCount] = {};
        m_travel[m_travelCount++] = m_spellTravel[i];
    }
    m_travelFrame = 0xFFFFFFFFu;
}

const game::ConsumableItem* TouchSmart::carried(u32 entry) const {
    for (u32 i = 0; i < m_consumableCount; ++i) {
        if (m_consumables[i].entry == entry) {
            return &m_consumables[i];
        }
    }
    return nullptr;
}

// --- §6.1 Consumables
// -------------------------------------------------------------------------------------------

u32 TouchSmart::consumableItem(GameSession& session, TouchConsumable kind, u32 nowMs) {
    const u32 k = static_cast<u32>(kind);
    if (k >= m_choice.size() || m_player == 0) {
        return 0;
    }
    if (m_choiceFrame[k] == m_frame) {
        return m_choice[k];
    }
    VD_PROFILE_ZONE("TouchSmart::consumable");
    bool fresh = false;
    for (const u32 frame : m_choiceFrame) {
        fresh = fresh || frame == m_frame;
    }
    if (!fresh) { // once a frame: what the level and skills allow, what is off cooldown
        const game::SpellBook& spells = session.play().spells();
        for (u32 i = 0; i < m_consumableCount; ++i) {
            game::ConsumableItem& c = m_consumables[i];
            c.usable = itemUsable(m_consumableNeeds[i]);
            const net::ItemQueryInfo* info = session.item(c.entry);
            c.ready = !info || !spells.itemCooldown(*info, nowMs).active(nowMs);
        }
    }
    m_choiceFrame[k] = m_frame;
    const i32 best = game::chooseConsumable(kind, m_inCombat, {m_consumables.data(), m_consumableCount},
                                            m_maxHealth, m_maxMana);
    const u32 previous = m_choice[k];
    m_choice[k] = best >= 0 ? m_consumables[static_cast<u32>(best)].entry : 0;
    if (m_trace && m_choice[k] != previous) {
        VD_LOG_INFO("Touch smart: consumable %u (%s) -> item %u", k,
                    m_inCombat ? "in combat" : "out of combat", m_choice[k]);
    }
    return m_choice[k];
}

u64 TouchSmart::consumableIcon(TouchConsumable kind) const {
    // What the button would use now: out of combat Health and Mana eat and drink, Rage and Energy bandage.
    switch (kind) {
    case TouchConsumable::Health: return m_inCombat ? kIconHealth : kIconFood;
    case TouchConsumable::Mana: return m_inCombat ? kIconMana : kIconDrink;
    case TouchConsumable::Rage: return kIconRage;
    case TouchConsumable::Energy: return kIconEnergy;
    case TouchConsumable::Bandage: return kIconBandage;
    case TouchConsumable::Food: return kIconFood;
    case TouchConsumable::Drink: return kIconDrink;
    default: return kIconHealth;
    }
}

// --- §6.2 Mount / travel
// ----------------------------------------------------------------------------------------

void TouchSmart::chooseTravel(GameSession& session) {
    if (m_travelFrame == m_frame) {
        return;
    }
    m_travelFrame = m_frame;
    m_travelChoice = {};
    const Entity* me = m_player ? session.entities().find(m_player) : nullptr;
    if (!me || game::isDead(*me) || game::isGhost(*me) || session.play().travel().flying()) {
        return; // dead, a ghost, on a taxi: nothing to ride
    }
    for (u32 i = 0; i < m_travelCount; ++i) {
        m_travel[i].usable = itemUsable(m_travelNeeds[i]);
        if (m_travel[i].action.kind == TouchActionKind::Spell) {
            for (u32 s = 0; s < m_spellTravelCount; ++s) {
                m_travel[i].active = m_spellTravel[s].action == m_travel[i].action ? m_spellTravel[s].active
                                                                                   : m_travel[i].active;
            }
        }
    }
    game::TravelState state;
    state.mounted = me->value(uf::UNIT_FIELD_MOUNTDISPLAYID) != 0;
    if (state.mounted && m_mountAura == 0) {
        return; // a mount the client did not give (a quest's, a vehicle): not ours to cancel
    }
    state.swimming = m_swimming;
    state.indoors = m_indoors;
    state.inCombat = m_inCombat;
    state.form = m_form;
    state.auraSpell = m_mountAura;
    m_travelChoice = game::chooseTravel(state, {m_travel.data(), m_travelCount}, m_favorite);
}

TouchAction TouchSmart::mountAction(GameSession& session, u32 nowMs) {
    (void)nowMs;
    chooseTravel(session);
    return m_travelChoice.action;
}

void TouchSmart::useMount(GameSession& session, CombatScreens& combat, u32 nowMs) {
    (void)nowMs;
    chooseTravel(session);
    const TouchAction& a = m_travelChoice.action;
    if (a.empty()) {
        return;
    }
    if (m_trace) {
        VD_LOG_INFO("Touch smart: travel %s %s %u (%s)", m_travelChoice.active ? "leave" : "use",
                    a.kind == TouchActionKind::Item ? "item" : "spell", a.id,
                    m_travelChoice.usable ? "usable" : "faint");
    }
    if (m_travelChoice.active && a.kind == TouchActionKind::Spell) {
        // Off the mount, out of the form, the aspect gone (CMSG_CANCEL_AURA, as BuffFrame does).
        session.play().cancelAura(a.id);
        return;
    }
    combat.useButton(session, {a.kind == TouchActionKind::Item ? net::kActionItem : net::kActionSpell, a.id});
}

// --- §6.3 Reactive
// ----------------------------------------------------------------------------------------------

u32 TouchSmart::reactiveSpell(GameSession& session, std::span<const u32> visible, u32 nowMs) {
    m_reactiveNowCount = 0;
    m_reactiveGlow = 0;
    const Entity* me = m_player ? session.entities().find(m_player) : nullptr;
    if (!me || m_reactiveCount == 0 || !m_abilities) {
        return 0;
    }
    VD_PROFILE_ZONE("TouchSmart::reactive");
    game::Gameplay& play = session.play();
    const u64 targetGuid = play.target();
    const Entity* target = targetGuid ? session.entities().find(targetGuid) : nullptr;
    const u32 combo = game::comboPoints(*me, targetGuid);
    const game::SpellBook& spells = play.spells();
    const u32 gcdLeft = spells.globalCooldown(nowMs).remaining(nowMs);
    const game::SpellText& text = m_abilities->spellText();
    std::array<u64, game::kTouchSlots> names{};
    for (u32 s = 0; s < visible.size() && s < names.size(); ++s) {
        names[s] = visible[s] != 0 ? stringHash(text.name(visible[s])) : 0;
    }
    for (u32 i = 0; i < m_reactiveCount; ++i) {
        const Reactive& r = m_reactive[i];
        if (r.info.targetKind() == game::SpellTargetKind::Enemy &&
            (!target || !game::canAttack(*me, *target, m_reactions))) {
            continue;
        }
        // IsUsableAction: its aura state, Overpower's combo point, the form; short of power still counts
        // (blue).
        // Phase 20 (assists): the stance assist takes its stance first (Overpower in Berserker Stance shows).
        const bool reachable =
            m_formReach && m_formReach(m_formReachContext, r.details, me->byteValue(uf::UNIT_FIELD_BYTES_1, 2));
        const game::ActionUsable usable = game::spellUsable(r.info, &r.details, *me, target, combo,
                                                            reachable ? nullptr : m_abilities, &play.spellMods());
        if (!usable.usable && !usable.noMana) {
            continue;
        }
        const game::CooldownView cooldown = spells.cooldown(r.info, r.info.category, nowMs);
        if (!game::reactiveReady(usable, cooldown.active(nowMs) ? cooldown.remaining(nowMs) : 0u, gcdLeft)) {
            continue; // its own cooldown (not the global one)
        }
        bool shown = false;
        for (u32 s = 0; s < visible.size() && s < names.size(); ++s) {
            if (visible[s] != 0 && (visible[s] == r.spell.spellId || names[s] == r.spell.nameHash)) {
                m_reactiveGlow |= static_cast<u16>(1u << s); // it glows there instead
                shown = true;
            }
        }
        if (!shown) { // the longest cooldown first (Overpower before Execute)
            game::insertReactive(m_reactiveNow, m_reactiveNowCount, {r.spell.spellId, r.spell.cooldownMs});
        }
    }
    const u32 first = m_reactiveNowCount > 0 ? m_reactiveNow[0].spellId : 0;
    if (m_trace && first != m_tracedReactive) {
        VD_LOG_INFO("Touch smart: reactive %u (%u usable, glow mask %x)", first, m_reactiveNowCount,
                    m_reactiveGlow);
    }
    m_tracedReactive = first;
    return first;
}

// --- The cluster's views
// ----------------------------------------------------------------------------------------

void TouchSmart::finishView(CombatScreens& combat, std::span<const TouchAction> resolved, TouchBarView& view,
                            u32 nowMs) {
    VD_PROFILE_ZONE("TouchSmart::finishView");
    // The consumables: on release (a hold lists them); a bandage greyed while Recently Bandaged lasts.
    for (const TouchPlace p : {TouchPlace::Consumable1, TouchPlace::Consumable2, TouchPlace::Consumable3}) {
        const u32 slot = touchIndex(p);
        TouchButtonView& b = view.places[slot];
        const TouchAction& a = slot < resolved.size() ? resolved[slot] : TouchAction{};
        if (!b.shown || (a.kind != TouchActionKind::Consumable && a.kind != TouchActionKind::Item)) {
            continue;
        }
        b.onPress = false;
        const u32 entry =
            a.kind == TouchActionKind::Item
                ? a.id
                : (a.id < m_choice.size() && m_choiceFrame[a.id] == m_frame ? m_choice[a.id] : 0u);
        const game::ConsumableItem* item = entry != 0 ? carried(entry) : nullptr;
        if (item && (item->use.flags & game::kConsumeBandage) != 0 && m_bandaged) {
            b.slot.usable = false;
            b.slot.noMana = false;
            if (m_bandagedLeftMs > 0) {
                b.slot.cooldownLeft = static_cast<f32>(m_bandagedLeftMs) / 1000.0f;
                b.slot.cooldownTotal =
                    static_cast<f32>(std::max(m_bandagedTotalMs, m_bandagedLeftMs)) / 1000.0f;
            }
        }
    }
    // The mount: on release (a hold chooses the favourite); lit while mounted or in the form (a tap leaves
    // it), faint where it cannot be used.
    if (TouchButtonView& b = view.places[touchIndex(TouchPlace::Mount)]; b.shown) {
        b.onPress = false;
        b.repeat = false;
        if (m_travelChoice.active) {
            b.slot.usable = true;
            b.slot.noMana = false;
            b.slot.checked = true;
        } else if (!m_travelChoice.usable) {
            b.slot.usable = false;
        }
    }
    // The reactive abilities: they glow where they are; the button is a fan of them when there are several.
    for (u32 s = 0; s < game::kTouchSlots; ++s) {
        if (reactiveGlows(s) && view.places[s].shown) {
            view.places[s].glow = true;
        }
    }
    if (TouchButtonView& b = view.places[touchIndex(TouchPlace::Reactive)];
        b.shown && m_reactiveNowCount > 1) {
        b.group = true;
        b.onPress = false;
        b.repeat = false;
    }
    // igBonusBarOpen when one comes (the bonus bar's sound in 1.12: something new to press).
    const bool any = m_reactiveNowCount > 0 || m_reactiveGlow != 0;
    if (any && !m_reactiveShown && nowMs - m_reactiveSoundMs >= kReactiveSoundMs) {
        m_reactiveSoundMs = nowMs;
        combat.uiSoundSink()(UiSound::BonusBarOpen);
    }
    m_reactiveShown = any;
}

void TouchSmart::fillFan(GameSession& session, CombatScreens& combat, i32 fanPlace, TouchBarView& view,
                         u32 nowMs) {
    if (fanPlace != static_cast<i32>(touchIndex(TouchPlace::Reactive)) || m_reactiveNowCount == 0) {
        return;
    }
    view.fanPlace = fanPlace;
    view.fanCurrent = 0;
    view.fanCount = std::min<u32>(m_reactiveNowCount, kTouchFanItems);
    for (u32 i = 0; i < view.fanCount; ++i) {
        view.fan[i] = combat.touchView(session, {net::kActionSpell, m_reactiveNow[i].spellId},
                                       kCacheReactiveFan + i, nowMs);
    }
}

bool TouchSmart::actOnHud(GameSession& session, TouchBarScreens& bar, const TouchBarOutput& out,
                          CombatScreens& combat, u64& aim) {
    bool took = false;
    if (out.held >= 0) {
        took = openList(session, bar, out.held, out.heldRect);
    }
    if (out.fanPlace == static_cast<i32>(touchIndex(TouchPlace::Reactive)) && out.fanChosen >= 0 &&
        static_cast<u32>(out.fanChosen) < m_reactiveNowCount) {
        aim = combat.useButton(session,
                               {net::kActionSpell, m_reactiveNow[static_cast<u32>(out.fanChosen)].spellId});
    }
    return took;
}

// --- The hold's list
// --------------------------------------------------------------------------------------------

bool TouchSmart::openList(GameSession& session, TouchBarScreens& bar, i32 place, const ui::Rect& anchor) {
    if (place < 0 || !bar.haveLayout()) {
        return false;
    }
    const u32 p = static_cast<u32>(place);
    m_rowChecked = 0;
    if (p >= touchIndex(TouchPlace::Consumable1) && p <= touchIndex(TouchPlace::Consumable3)) {
        const TouchAction a = game::resolveTouchAction(bar.layout().current(), bar.context(), p);
        if (a.kind == TouchActionKind::Consumable) {
            m_listKind = static_cast<TouchConsumable>(a.id);
        } else if (a.kind == TouchActionKind::Item) {
            const game::ConsumableItem* item = carried(a.id);
            m_listKind = item ? game::consumableKindOf(item->use, slotKind(p)) : slotKind(p);
        } else {
            return false; // a spell or a macro put there: its tooltip
        }
    } else if (p == touchIndex(TouchPlace::Mount)) {
        u32 mounts = 0;
        for (u32 i = 0; i < m_travelCount; ++i) {
            mounts += m_travel[i].mount ? 1u : 0u;
        }
        if (mounts == 0) {
            return false; // nothing to choose among
        }
    } else {
        return false;
    }
    m_listPlace = place;
    m_listAnchor = anchor;
    listRows(session, bar);
    return true;
}

void TouchSmart::listRows(GameSession& session, TouchBarScreens& bar) {
    m_rowCount = 0;
    m_rows[m_rowCount++] = {}; // "Automatic"
    const u32 p = static_cast<u32>(m_listPlace);
    const auto iconOf = [&](u32 entry, u32& quality) -> u64 {
        const net::ItemQueryInfo* info = session.item(entry);
        quality = info ? info->quality : 1u;
        return info && m_data ? m_data->itemIconHash(info->displayId) : 0;
    };
    if (p == touchIndex(TouchPlace::Mount)) {
        for (u32 i = 0; i < m_travelCount && m_rowCount < m_rows.size(); ++i) {
            const game::TravelOption& o = m_travel[i];
            if (!o.mount) {
                continue;
            }
            ListRow& row = m_rows[m_rowCount];
            row.action = o.action;
            if (o.action.kind == TouchActionKind::Item) {
                row.icon = iconOf(o.action.id, row.quality);
            } else {
                const std::optional<game::SpellInfo> info =
                    m_data ? m_data->spell(o.action.id) : std::nullopt;
                row.icon = info ? info->iconHash : 0;
                row.quality = 1;
            }
            if (bar.layout().mount == o.action) {
                m_rowChecked = static_cast<i32>(m_rowCount);
            }
            ++m_rowCount;
        }
        return;
    }
    // The consumables that fit the button, what restores most first.
    std::array<u32, 32> order;
    u32 count = 0;
    for (u32 i = 0; i < m_consumableCount; ++i) {
        if (game::consumableFits(m_listKind, m_consumables[i].use)) {
            order[count++] = i;
        }
    }
    std::sort(order.begin(), order.begin() + count, [&](u32 a, u32 b) {
        const game::ConsumableUse& x = m_consumables[a].use;
        const game::ConsumableUse& y = m_consumables[b].use;
        return x.health + x.power > y.health + y.power;
    });
    const TouchAction now = game::resolveTouchAction(bar.layout().current(), bar.context(), p);
    for (u32 i = 0; i < count && m_rowCount < m_rows.size(); ++i) {
        ListRow& row = m_rows[m_rowCount];
        row.action = TouchAction::item(m_consumables[order[i]].entry, game::kTouchManual);
        row.icon = iconOf(row.action.id, row.quality);
        if (now.kind == TouchActionKind::Item && now.id == row.action.id) {
            m_rowChecked = static_cast<i32>(m_rowCount);
        }
        ++m_rowCount;
    }
}

bool TouchSmart::escape() {
    const bool open = m_listPlace >= 0;
    m_listPlace = -1;
    return open;
}

i32 TouchSmart::listView(DialogContext& c, std::span<const ListView> rows, i32 checked, bool& closed) {
    ui::Gui g{c.ui, c.draw};
    // A small list over the held button (§6.1 "Mantener sobre el botón"): a tap on a row picks it, outside
    // closes it.
    closed = ui::modalBlocker(g, ui::widgetId("touchSmart.list"), 0.0f);
    const f32 rowH = std::max(g.px(38.0f), g.unit(22.0f));
    const f32 icon = rowH * 0.78f;
    const f32 pad = g.px(8.0f);
    const f32 check = rowH * 0.6f;
    const ui::TextStyle style = ui::gameFont(std::max(g.px(13.0f), rowH * 0.36f), ui::kWhite);
    ui::TextStyle countStyle = ui::numberFont(style.size, ui::kWhite);
    countStyle.align = ui::Align::Right;
    f32 textW = 0.0f;
    for (const ListView& row : rows) {
        textW = std::max(textW, c.draw.measure(row.name, style.font, style.size));
    }
    const f32 countW = c.draw.measure("999", countStyle.font, countStyle.size);
    const f32 width = std::min(c.safeArea.w - 2.0f * pad, check + icon + textW + countW + 5.0f * pad);
    const f32 height = rowH * static_cast<f32>(rows.size()) + 2.0f * pad;
    ui::Rect r{m_listAnchor.right() - width, m_listAnchor.y - height - pad, width, height};
    if (r.y < c.safeArea.y) { // no room above the button: beside it
        r.y = std::min(m_listAnchor.y, c.safeArea.bottom() - height);
        r.x = m_listAnchor.x - width - pad;
    }
    r.x = std::clamp(r.x, c.safeArea.x, std::max(c.safeArea.x, c.safeArea.right() - width));
    r.y = std::clamp(r.y, c.safeArea.y, std::max(c.safeArea.y, c.safeArea.bottom() - height));
    // Opaque: the cluster's buttons are under it (the tooltip's own background lets them show through).
    c.draw.solid(r.inset(g.unit(4.0f)), ui::rgba(12, 12, 28, 255));
    ui::tooltipPanel(g, r);
    i32 picked = -1;
    for (u32 i = 0; i < rows.size(); ++i) {
        const ListView& row = rows[i];
        const ui::Rect line{r.x + pad * 0.5f, r.y + pad + rowH * static_cast<f32>(i), r.w - pad, rowH};
        const ui::WidgetState state = ui::hit(g, ui::widgetId("touchSmart.row", i), line);
        if (ui::held(state, line)) {
            c.draw.spriteAdditive(line, kHighlight, ui::rgba(255, 255, 255, 200));
        }
        f32 x = line.x + pad * 0.5f;
        if (static_cast<i32>(i) == checked) {
            c.draw.sprite({x, line.centerY() - check * 0.5f, check, check}, kCheck);
        }
        x += check;
        if (const std::optional<u32> slot = row.icon ? c.resources.icon(row.icon) : std::nullopt) {
            c.draw.icon({x, line.centerY() - icon * 0.5f, icon, icon}, *slot);
        }
        x += icon + pad;
        ui::TextStyle nameStyle = style;
        nameStyle.color = row.color;
        ui::label(g, {x, line.y, line.right() - x - countW - pad, line.h}, row.name, nameStyle);
        if (row.count > 0) {
            char number[12];
            std::snprintf(number, sizeof(number), "%u", row.count);
            c.draw.text(line.right() - pad, ui::centeredBaseline(c.draw, countStyle, line.centerY()), number,
                        countStyle);
        }
        if (ui::tapped(state)) {
            picked = static_cast<i32>(i);
        }
    }
    return picked;
}

void TouchSmart::draw(GameSession& session, TouchBarScreens& bar, DialogContext& c, u32 nowMs) {
    (void)nowMs;
    if (m_listPlace < 0) {
        return;
    }
    if (!bar.haveLayout() || !session.inWorld()) {
        m_listPlace = -1;
        return;
    }
    VD_PROFILE_ZONE("TouchSmart::list");
    const bool mount = m_listPlace == static_cast<i32>(touchIndex(TouchPlace::Mount));
    const game::Inventory& inventory = session.play().inventory();
    std::array<ListView, std::tuple_size_v<decltype(m_rows)>> views;
    for (u32 i = 0; i < m_rowCount; ++i) {
        const ListRow& row = m_rows[i];
        ListView& v = views[i];
        v = {};
        if (row.action.empty()) {
            v.icon = mount ? kIconMount : consumableIcon(m_listKind);
            v.name = text::get("VD_TOUCH_SMART_AUTOMATIC");
            v.color = ui::kGold;
        } else if (row.action.kind == TouchActionKind::Item) {
            const net::ItemQueryInfo* info = session.item(row.action.id);
            v.icon = row.icon;
            v.name = info ? std::string_view(info->name) : std::string_view("...");
            v.color = ui::qualityColor(row.quality);
            v.count = mount ? 0u : inventory.count(row.action.id);
        } else {
            v.icon = row.icon;
            v.name = m_abilities ? m_abilities->spellText().name(row.action.id) : std::string_view{};
            v.color = ui::kWhite;
        }
    }
    bool closed = false;
    const i32 picked = listView(c, {views.data(), m_rowCount}, m_rowChecked, closed);
    if (picked >= 0) {
        const ListRow& row = m_rows[static_cast<u32>(picked)];
        const u32 p = static_cast<u32>(m_listPlace);
        if (mount) {
            bar.layout().mount = row.action; // empty: the fastest again
        } else {
            // The slot of the layer it shows (its own when the layer has one, else the base's).
            game::TouchTemplate& t = bar.layout().current();
            const u32 layer = static_cast<u32>(bar.context());
            TouchAction& own = t.layers[layer][p];
            TouchAction& slot = layer != 0 && !own.empty() ? own : t.layers[0][p];
            slot = row.action.empty() ? TouchAction::consumable(m_listKind) : row.action;
        }
        bar.changed();
        m_choiceFrame = {};
        m_travelFrame = 0xFFFFFFFFu;
        m_listPlace = -1;
    } else if (closed) {
        m_listPlace = -1;
    }
}

// --- §6.4 The context button's subjects, §6.5 the next enemy
// ---------------------------------------------------

void TouchSmart::prepareHud(GameSession& session, HudFrame& hud, u32 nowMs) {
    refresh(session, hud, nowMs);
    hud.breakableControl = [](const void* context, u64 guid) {
        TouchSmart* self = const_cast<TouchSmart*>(static_cast<const TouchSmart*>(context));
        return self->m_session && self->breakableControl(*self->m_session, guid);
    };
    hud.breakableContext = this;
}

bool TouchSmart::breakableControl(GameSession& session, u64 guid) {
    const Entity* e = session.entities().find(guid);
    for (u32 slot = game::kMaxPositiveAuras; e && slot < game::kMaxAuras; ++slot) {
        const u32 spellId = e->value(static_cast<u16>(uf::UNIT_FIELD_AURA + slot));
        if (spellId != 0 && (spellFlags(spellId) & kFlagBreaks) != 0) {
            return true;
        }
    }
    return false;
}

TouchSmart::Subject TouchSmart::subject(GameSession& session, u32 nowMs) {
    Subject out;
    out.action = HudContextAction::None;
    m_subjectSpell = 0;
    game::Gameplay& play = session.play();
    const Entity* me = m_player ? session.entities().find(m_player) : nullptr;
    const u64 targetGuid = play.target();
    const Entity* target =
        targetGuid && targetGuid != m_player ? session.entities().find(targetGuid) : nullptr;
    if (!me || game::isDead(*me) || !target || !game::isUnit(*target) || !game::isDead(*target) || !m_data) {
        return out;
    }
    const KnownSpell* spell = nullptr;
    HudContextAction action = HudContextAction::None;
    if (target->type == game::ObjectType::Player) {
        // A dead friendly player: the class's resurrection; in combat only what can be cast there (Rebirth).
        if (game::reactionTo(*me, *target, m_reactions) == game::Reaction::Hostile ||
            game::canAttack(*me, *target, m_reactions)) {
            return out;
        }
        const game::SpellBook& spells = play.spells();
        const u32 gcdLeft = spells.globalCooldown(nowMs).remaining(nowMs);
        for (u32 i = 0; i < m_resurrectCount && !spell; ++i) {
            const KnownSpell& s = m_resurrect[i];
            const game::CooldownView cooldown = spells.cooldown(s.spellId, nowMs);
            if ((m_inCombat && s.notInCombat) ||
                (cooldown.active(nowMs) && cooldown.remaining(nowMs) > gcdLeft + kGcdSlackMs)) {
                continue;
            }
            spell = &s;
        }
        action = HudContextAction::Resurrect;
    } else if (game::isSkinnable(*target) && !game::isLootable(*target) && m_skinning.spellId != 0) {
        spell = &m_skinning; // the client's right click on a looted skinnable corpse
        action = HudContextAction::Skin;
    }
    if (!spell) {
        return out;
    }
    const std::optional<game::SpellInfo> info = m_data->spell(spell->spellId);
    out.action = action;
    out.guid = targetGuid;
    out.icon = spell->icon;
    out.spellId = spell->spellId;
    out.inRange =
        !info || game::spellRange(*info, *me, target, nowMs, &play.spellMods()) != game::ActionRange::Out;
    m_subjectSpell = spell->spellId;
    return out;
}

void TouchSmart::useSubject(GameSession& session, u64 guid) {
    if (m_subjectSpell != 0 && guid != 0) {
        session.play().castSpell(m_subjectSpell, guid);
        if (m_trace) {
            VD_LOG_INFO("Touch smart: context spell %u on %016llx", m_subjectSpell,
                        static_cast<unsigned long long>(guid));
        }
    }
}

u64 TouchSmart::gatherNode(GameSession& session, u32 nowMs) {
    if (m_gatherLocks == 0 || !m_data || m_player == 0) {
        return 0;
    }
    const game::EntityStore& entities = session.entities();
    if (nowMs - m_gatherScanMs < kGatherScanMs) {
        return m_gatherNode != 0 && entities.find(m_gatherNode) ? m_gatherNode : 0;
    }
    VD_PROFILE_ZONE("TouchSmart::gatherNode");
    m_gatherScanMs = nowMs;
    m_gatherNode = 0;
    const Entity* me = entities.find(m_player);
    if (!me || game::isDead(*me)) {
        return 0;
    }
    // Herbs and veins are chests whose lock a gathering spell opens (Herb Gathering: LockType 2, Mining: 3);
    // the server only lights quest chests (GO_DYNFLAG_LO_ACTIVATE), so these are found by their lock.
    f32 best = kGatherSearchYards;
    entities.forEach([&](const Entity& e) {
        if (e.type != game::ObjectType::GameObject || game::gameObjectType(e) != game::kGameObjectChest ||
            (e.value(uf::GAMEOBJECT_FLAGS) & (game::kGameObjectFlagNoInteract | kGameObjectFlagInUse)) != 0 ||
            e.value(uf::GAMEOBJECT_STATE) != kGameObjectStateReady) {
            return;
        }
        const f32 d = game::distanceAt(*me, e, nowMs);
        if (d >= best) {
            return;
        }
        const game::GameObjectInfo* info = session.gameObject(e.value(uf::OBJECT_FIELD_ENTRY));
        const u32 lock = info ? m_data->lockOpenType(info->data[0]) : 0;
        if (lock < 32 && (m_gatherLocks & (1u << lock)) != 0) {
            best = d;
            m_gatherNode = e.guid;
        }
    });
    return m_gatherNode;
}

// --- VD_TOUCH_SMART_DEMO
// ----------------------------------------------------------------------------------------

void TouchSmart::prepareOffline(HudFrame& hud, TouchBarView& view, i32 fanPlace, u32 nowMs) {
    if (!m_demo || !view.geometry) {
        return;
    }
    const auto spellIcon = [&](u32 spellId) -> u64 {
        const std::optional<game::SpellInfo> info = m_data ? m_data->spell(spellId) : std::nullopt;
        return info ? info->iconHash : 0;
    };
    // In combat: a healthstone, a mana potion, a bandage while Recently Bandaged lasts (its 60 s, looping).
    struct Sample {
        TouchPlace place;
        const char* icon;
        u32 count;
    };
    static constexpr Sample kSamples[] = {
        {TouchPlace::Consumable1, "Interface\\Icons\\INV_Stone_04.blp", 1},
        {TouchPlace::Consumable2, "Interface\\Icons\\INV_Potion_76.blp", 4},
        {TouchPlace::Consumable3, "Interface\\Icons\\INV_Misc_Bandage_12.blp", 12}};
    for (const Sample& s : kSamples) {
        TouchButtonView& b = view.places[touchIndex(s.place)];
        b = {};
        b.shown = true;
        b.slot.iconHash = assetPathHash(s.icon);
        b.slot.count = s.count;
    }
    TouchButtonView& bandage = view.places[touchIndex(TouchPlace::Consumable3)];
    bandage.slot.usable = false;
    bandage.slot.cooldownTotal = 60.0f;
    bandage.slot.cooldownLeft = 60.0f - static_cast<f32>(nowMs % 60000) / 1000.0f;
    // The mount: Swift Mistsaber, ridden every other 4 s (lit: a tap gets off).
    TouchButtonView& mount = view.places[touchIndex(TouchPlace::Mount)];
    mount = {};
    mount.shown = true;
    mount.slot.iconHash = spellIcon(23219);
    mount.slot.checked = (nowMs / 4000) % 2 == 1;
    // Overpower and Execute usable at once: the button is a fan of them; Revenge glowing in its place
    // (RingC).
    TouchButtonView& reactive = view.places[touchIndex(TouchPlace::Reactive)];
    reactive = {};
    reactive.shown = true;
    reactive.glow = true;
    reactive.group = true;
    reactive.slot.iconHash = spellIcon(7384);
    view.places[touchIndex(TouchPlace::RingC)].glow = true;
    view.places[touchIndex(TouchPlace::RingC)].slot.iconHash = spellIcon(6572);
    if (fanPlace == static_cast<i32>(touchIndex(TouchPlace::Reactive))) {
        view.fanPlace = fanPlace;
        view.fanCount = 2;
        view.fanCurrent = 0;
        view.fan[0] = HudActionSlot{spellIcon(7384)};
        view.fan[1] = HudActionSlot{spellIcon(5308)};
    }
    // §6.4: a dead friendly player targeted: Resurrection.
    hud.context = HudContextAction::Resurrect;
    hud.contextActionIcon = spellIcon(2006);
    hud.targetFanDemo = true;
}

void TouchSmart::actOffline(const TouchBarOutput& out) {
    if (!m_demo || out.held < 0) {
        return;
    }
    const u32 p = static_cast<u32>(out.held);
    if ((p >= touchIndex(TouchPlace::Consumable1) && p <= touchIndex(TouchPlace::Consumable3)) ||
        p == touchIndex(TouchPlace::Mount)) {
        m_listPlace = out.held;
        m_listAnchor = out.heldRect;
        m_listKind = p == touchIndex(TouchPlace::Consumable3)   ? TouchConsumable::Bandage
                     : p == touchIndex(TouchPlace::Consumable2) ? TouchConsumable::Mana
                                                                : TouchConsumable::Health;
    }
}

void TouchSmart::drawOffline(DialogContext& c) {
    if (!m_demo || m_listPlace < 0) {
        return;
    }
    // Sample rows (item names are the game's data, as a server would send them).
    const bool mount = m_listPlace == static_cast<i32>(touchIndex(TouchPlace::Mount));
    const std::string_view automatic = text::get("VD_TOUCH_SMART_AUTOMATIC");
    const ListView mounts[] = {
        {kIconMount, automatic, ui::kGold, 0},
        {assetPathHash("Interface\\Icons\\Ability_Mount_WhiteTiger.blp"), "Reins of the Swift Mistsaber",
         ui::qualityColor(4), 0},
        {assetPathHash("Interface\\Icons\\Ability_Mount_BlackPanther.blp"), "Reins of the Striped Nightsaber",
         ui::qualityColor(3), 0},
        {assetPathHash("Interface\\Icons\\Spell_Nature_Swiftness.blp"), "Summon Felsteed", ui::kWhite, 0}};
    const ListView health[] = {
        {consumableIcon(TouchConsumable::Health), automatic, ui::kGold, 0},
        {assetPathHash("Interface\\Icons\\INV_Stone_04.blp"), "Major Healthstone", ui::qualityColor(1), 1},
        {assetPathHash("Interface\\Icons\\INV_Potion_54.blp"), "Major Healing Potion", ui::qualityColor(1),
         5},
        {assetPathHash("Interface\\Icons\\INV_Potion_53.blp"), "Superior Healing Potion", ui::qualityColor(1),
         3},
        {assetPathHash("Interface\\Icons\\INV_Misc_Food_19.blp"), "Smoked Desert Dumplings",
         ui::qualityColor(1), 8}};
    bool closed = false;
    const i32 picked = mount ? listView(c, mounts, 1, closed) : listView(c, health, 0, closed);
    if (picked >= 0 || closed) {
        m_listPlace = -1;
    }
}

} // namespace vd
