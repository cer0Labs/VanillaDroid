#include "app/TouchPetScreens.h"

#include "app/CombatScreens.h"
#include "app/GameSession.h"
#include "app/Hud.h"
#include "app/Portraits.h"
#include "app/TouchBarScreens.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/Auras.h"
#include "game/Consumables.h"
#include "game/GameData.h"
#include "game/PetData.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {

namespace uf = net::uf;
using game::Entity;
using game::TouchAction;
using game::TouchActionKind;
using game::touchIndex;
using game::TouchPlace;

namespace {

// CombatScreens::touchView's caches of this module (kTouchPetViews on): the pet button, the demons (its fan
// and the panel's row), the panel's spells on the pet, Feed Pet.
constexpr u32 kCachePet = CombatScreens::kTouchPetViews;
constexpr u32 kCacheDemons = kCachePet + 1;
constexpr u32 kCacheOnPet = kCacheDemons + game::PetSpellBook::kMaxDemons;
constexpr u32 kCacheFeed = kCacheOnPet + game::PetSpellBook::kMaxOnPet;
static_assert(kCacheFeed + 1 <= CombatScreens::kTouchViews);

// UNIT_FIELD_FLAGS (UnitDefines.h): what keeps the player from acting.
constexpr u32 kUnitFlagStunned = 0x00040000;
constexpr u32 kUnitFlagConfused = 0x00400000;
constexpr u32 kUnitFlagFleeing = 0x00800000;
constexpr u32 kUnitFlagPossessed = 0x01000000;
constexpr u32 kUnitFlagInCombat = 0x00080000;
// PetTameFailureReason (SharedDefines.h): Call Pet's answers.
constexpr i32 kPetTameNoPetAvailable = 7;
constexpr i32 kPetTameDead = 10;
constexpr i32 kPetTameNotDead = 11;
// SpellCastResult (build 5875, net::spellCastResultName): Feed Pet's refusals.
constexpr i32 kSpellFailedFoodLowLevel = 31;
constexpr i32 kSpellFailedWrongPetFood = 125;
constexpr u32 kFeedWaitMs = 4000; // a food not taken nor refused by then is forgotten
constexpr u32 kReleaseGraceMs =
    1500; // a tap on Release Spirit this soon after dying is the fight's, not a choice
constexpr u32 kItemClassTradeGoods = 7;

// Icons of the client's (textures.vdpk): Release Spirit is the Ghost aura's (spell 8326), Resurrect the
// priest's Resurrection (2006); Mind Control's for an Exit without a channel.
constexpr u64 kIconRelease = assetPathHash("Interface\\Icons\\Ability_Vanish.blp");
constexpr u64 kIconResurrect = assetPathHash("Interface\\Icons\\Spell_Holy_Resurrection.blp");
constexpr u64 kIconExit = assetPathHash("Interface\\Icons\\Spell_Shadow_ShadowWordDominate.blp");
constexpr u64 kIconDismiss = assetPathHash("Interface\\Icons\\Spell_Nature_SpiritWolf.blp");
constexpr u64 kIconCallPet = assetPathHash("Interface\\Icons\\Ability_Hunter_BeastCall.blp");
constexpr u64 kHappiness = assetPathHash("Interface\\PetPaperDollFrame\\UI-PetHappiness.blp");

constexpr u32 kPetCommands[] = {net::kPetCommandAttack, net::kPetCommandFollow, net::kPetCommandStay};
constexpr u32 kPetReactions[] = {net::kPetReactAggressive, net::kPetReactDefensive, net::kPetReactPassive};
constexpr const char* kCommandText[] = {"PET_ACTION_WAIT", "PET_ACTION_FOLLOW",
                                        "PET_ACTION_ATTACK"}; // by command
constexpr const char* kReactionText[] = {"PET_MODE_PASSIVE", "PET_MODE_DEFENSIVE", "PET_MODE_AGGRESSIVE"};

u64 mix(u64 h, u64 v) {
    return (h ^ v) * 1099511628211ull;
}

// The face of UI-PetHappiness (PetFrame_SetHappiness: happy, content, unhappy).
ui::TexCoords happinessFace(u32 level) {
    const f32 u0 = level >= 3 ? 0.0f : level == 2 ? 0.1875f : 0.375f;
    return {u0, 0.0f, u0 + 0.1875f, 0.359375f};
}

} // namespace

void TouchPetScreens::init(const game::GameData* data, const game::AbilityData* abilities) {
    m_data = data;
    m_abilities = abilities;
    const char* trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY");
    m_trace = trace && *trace && *trace != '0';
    m_iconRelease = kIconRelease;
    m_iconResurrect = kIconResurrect;
    m_iconExit = kIconExit;
    m_iconDismiss = kIconDismiss;
    m_iconCallPet = kIconCallPet;
    if (const char* demo = VD_DEV_ENV("VD_TOUCH_PETS_DEMO"); demo && *demo) {
        // pet|panel|feed|callpet|revive|demons|controlled|feared|dead|ghost
        const std::string_view d(demo);
        m_demo = d == "pet"          ? Demo::Pet
                 : d == "panel"      ? Demo::Panel
                 : d == "feed"       ? Demo::Feed
                 : d == "callpet"    ? Demo::CallPet
                 : d == "revive"     ? Demo::Revive
                 : d == "demons"     ? Demo::Demons
                 : d == "controlled" ? Demo::Controlled
                 : d == "feared"     ? Demo::Feared
                 : d == "dead"       ? Demo::Dead
                 : d == "ghost"      ? Demo::Ghost
                                     : Demo::None;
        m_demoPanel = m_demo == Demo::Panel;
        m_demoFood = m_demo == Demo::Feed;
        const char* cls = VD_DEV_ENV("VD_TOUCH_DEMO");
        m_demoWarlock = cls && std::strncmp(cls, "warlock", 7) == 0; // a felhunter's panel, with the demons
    }
}

void TouchPetScreens::enterWorld(GameSession& session) {
    m_eventCursor = session.play().events().end();
    m_spells = {};
    m_spellsVersion = 0xFFFFFFFFu;
    m_petGuid = 0;
    m_petWasDead = false;
    m_knownDead = false;
    m_panelOpen = false;
    m_foodOpen = false;
    m_feedEntry = 0;
    m_controlledKey = 0;
    m_controlKey = 0;
    m_controlCount = 0;
    m_wasDead = false;
    m_immunities = {};
}

// --- Once a frame ------------------------------------------------------------------------------------------

void TouchPetScreens::update(GameSession& session, TouchBarScreens& bar, u32 nowMs) {
    VD_PROFILE_ZONE("TouchPetScreens::update");
    game::Gameplay& play = session.play();
    if (m_data && m_spellsVersion != play.spells().knownVersion()) {
        m_spellsVersion = play.spells().knownVersion();
        game::scanPetSpells(play.spells().known(), *m_data, m_spells);
    }
    readEvents(session, bar, nowMs);
    watchPet(session, bar, nowMs);
}

void TouchPetScreens::readEvents(GameSession& session, TouchBarScreens& bar, u32 nowMs) {
    const game::Gameplay& play = session.play();
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        switch (e->kind) {
        case game::GameEventKind::PetTameFailed:
            // Unit::EffectSummonPet / Spell::CheckCast's answers to Call Pet and Revive Pet: the pet in the
            // database is dead, alive, or there is none.
            if (e->value[0] == kPetTameDead) {
                m_knownDead = true;
            } else if (e->value[0] == kPetTameNotDead || e->value[0] == kPetTameNoPetAvailable) {
                m_knownDead = false;
            }
            break;
        case game::GameEventKind::CastFailed:
            // Feed Pet's refusals: the food is not of the family's diet, or too low for the pet's level.
            if (m_feedEntry != 0 && e->id == m_spells.feedPet) {
                if (e->value[0] == kSpellFailedWrongPetFood || e->value[0] == kSpellFailedFoodLowLevel) {
                    game::rememberPetFood(bar.layout().petFood, m_feedFamily, m_feedEntry, false);
                    bar.changed();
                    m_foodKey = 0; // the list without it
                    if (m_trace) {
                        VD_LOG_INFO("Touch pets: food %u refused by family %u (%d)", m_feedEntry,
                                    m_feedFamily, e->value[0]);
                    }
                }
                m_feedEntry = 0;
            }
            break;
        default: break;
        }
    }
    // A food is taken when its stack goes down (the server destroys one on Feed Pet's effect).
    if (m_feedEntry != 0) {
        if (play.inventory().count(m_feedEntry) < m_feedCount) {
            game::rememberPetFood(bar.layout().petFood, m_feedFamily, m_feedEntry, true);
            bar.changed();
            m_foodKey = 0;
            if (m_trace) {
                VD_LOG_INFO("Touch pets: food %u accepted by family %u", m_feedEntry, m_feedFamily);
            }
            m_feedEntry = 0;
        } else if (nowMs - m_feedMs > kFeedWaitMs) {
            m_feedEntry = 0;
        }
    }
}

void TouchPetScreens::watchPet(GameSession& session, TouchBarScreens& bar, u32 nowMs) {
    (void)nowMs;
    const game::PetState& pet = session.play().abilities().pet();
    const Entity* unit = pet.active() ? session.entities().find(pet.guid) : nullptr;
    if (unit) {
        const bool dead = game::isDead(*unit);
        m_knownDead = dead;
        m_petWasDead = dead;
        // The warlock's demon summoned last (from the book, the drawer, the pet button): the one the button
        // summons.
        const u32 created = unit->value(uf::UNIT_CREATED_BY_SPELL);
        if (m_spells.knowsDemon(created) && bar.layout().lastDemon != created) {
            bar.layout().lastDemon = created;
            bar.changed();
        }
    } else if (m_petGuid != 0 && !pet.active()) {
        // The pet went away: dead when it lay dead (its corpse despawned, the hunter went too far: it stays
        // dead in the database, Pet::Update), alive otherwise (dismissed, stabled).
        m_knownDead = m_petWasDead;
        m_panelOpen = false;
        m_foodOpen = false;
    }
    m_petGuid = pet.active() ? pet.guid : 0;
}

u32 TouchPetScreens::petFamily(GameSession& session) const {
    const game::PetState& pet = session.play().abilities().pet();
    const Entity* unit = pet.active() ? session.entities().find(pet.guid) : nullptr;
    const net::CreatureQueryInfo* info =
        unit ? session.creature(unit->value(uf::OBJECT_FIELD_ENTRY)) : nullptr;
    return info ? info->family : 0;
}

game::PetButtonChoice TouchPetScreens::choose(GameSession& session, const game::TouchLayout& layout) const {
    game::Gameplay& play = session.play();
    const game::PetState& pet = play.abilities().pet();
    const Entity* unit = pet.active() ? session.entities().find(pet.guid) : nullptr;
    const Entity* me = session.entities().find(session.playerGuid());
    game::PetButtonState s;
    s.petOut = unit != nullptr;
    s.petDead = unit && game::isDead(*unit);
    s.knownDead = !unit && m_knownDead;
    s.spells = &m_spells;
    s.lastDemon = layout.lastDemon;
    if (const Entity* target = play.target() != 0 ? session.entities().find(play.target()) : nullptr;
        target && me && target->guid != pet.guid && game::isUnit(*target) && !game::isDead(*target) &&
        game::canAttack(*me, *target, play.reactions())) {
        s.target = target->guid;
    }
    s.petTarget = unit ? game::guidValue(*unit, uf::UNIT_FIELD_TARGET) : 0;
    return game::choosePetButton(s);
}

// --- The pet button ----------------------------------------------------------------------------------------

void TouchPetScreens::fillPetButton(GameSession& session, CombatScreens& combat, TouchBarScreens& bar,
                                    TouchBarView& view, u32 nowMs) {
    VD_PROFILE_ZONE("TouchPetScreens::fillPetButton");
    m_takeover = Takeover::None; // takeOver() says, later in the frame
    m_death = {};
    m_choice = choose(session, bar.layout());
    TouchButtonView& b = view.places[touchIndex(TouchPlace::Pet)];
    b = {};
    switch (m_choice.mode) {
    case game::PetButtonMode::Hidden: return;
    case game::PetButtonMode::CallPet:
    case game::PetButtonMode::RevivePet:
    case game::PetButtonMode::SummonDemon:
        b.shown = true;
        b.slot = combat.touchView(session, {net::kActionSpell, m_choice.spellId}, kCachePet, nowMs);
        b.group = m_choice.fan; // a hold: the demons' fan
        return;
    case game::PetButtonMode::Command: break;
    }
    // A pet out: its portrait (else its family's icon, a demon's summoning spell's), its mood (hunter pets),
    // lit while it attacks.
    const game::PetState& pet = session.play().abilities().pet();
    const Entity* unit = session.entities().find(pet.guid);
    if (!unit) {
        return;
    }
    b.shown = true;
    b.breaksControl =
        true; // the pet obeys while its master is feared or stunned (HandlePetAction asks nothing of it)
    if (Portraits* portraits = combat.portraits()) {
        b.portrait = portraits->unit(pet.guid);
    }
    const game::PetData* pets = combat.petData();
    const u32 family = petFamily(session);
    if (pets && family != 0 && pets->familyFoodMask(family) != 0) {
        b.slot.iconHash = pets->familyIconHash(family); // a beast (the demons' families have a cat's icon)
    }
    if (b.slot.iconHash == 0) {
        const u32 created = unit->value(uf::UNIT_CREATED_BY_SPELL);
        const std::optional<game::SpellInfo> info = m_data && created ? m_data->spell(created) : std::nullopt;
        b.slot.iconHash = info ? info->iconHash : m_iconCallPet;
    }
    if (unit->value(uf::UNIT_FIELD_MAXPOWER5) > 0 && pets) { // a hunter pet's happiness (GetPetHappiness)
        if (const u32 level = pets->happiness(unit->value(uf::UNIT_FIELD_POWER5)).level; level > 0) {
            b.cornerIcon = kHappiness;
            b.cornerUv = happinessFace(level);
        }
    }
    b.slot.checked = game::guidValue(*unit, uf::UNIT_FIELD_TARGET) != 0 &&
                     (unit->value(uf::UNIT_FIELD_FLAGS) & kUnitFlagInCombat) != 0;
    b.slot.usable = pet.enabled;
}

void TouchPetScreens::fillFan(GameSession& session, CombatScreens& combat, i32 fanPlace, TouchBarView& view,
                              u32 nowMs) {
    if (fanPlace != static_cast<i32>(touchIndex(TouchPlace::Pet)) ||
        m_choice.mode != game::PetButtonMode::SummonDemon) {
        return;
    }
    view.fanPlace = fanPlace;
    view.fanCount = 0;
    view.fanCurrent = 0;
    for (u32 i = 0; i < m_spells.demonCount && i < kTouchFanItems; ++i) {
        view.fan[i] =
            combat.touchView(session, {net::kActionSpell, m_spells.demons[i]}, kCacheDemons + i, nowMs);
        view.fanCurrent = m_spells.demons[i] == m_choice.spellId ? i : view.fanCurrent;
        view.fanCount = i + 1;
    }
}

// --- §5.2 rows 1-3 -----------------------------------------------------------------------------------------

bool TouchPetScreens::takeOver(GameSession& session, CombatScreens& combat, TouchBarScreens& bar,
                               std::span<TouchAction> resolved, TouchBarView& view, u32 nowMs) {
    VD_PROFILE_ZONE("TouchPetScreens::takeOver");
    game::Gameplay& play = session.play();
    const Entity* me = session.entities().find(session.playerGuid());
    if (!me) {
        return false;
    }
    // Row 1: another unit moves under the player's hand (its SMSG_PET_SPELLS is the pet's: Abilities::pet).
    const u64 mover = play.control().wantedMover();
    if (mover != 0 && mover != session.playerGuid()) {
        m_takeover = Takeover::Controlled;
        return takeOverControlled(session, combat, resolved, view, nowMs);
    }
    m_controlledKey = 0;
    // Row 3: dead or a ghost.
    if (takeOverDead(session, resolved, view, nowMs)) {
        m_takeover = Takeover::Dead;
        return true;
    }
    // Row 2: no control (feared, confused, possessed: PLAYER_CONTROL_LOST; stunned: the client keeps control
    // but casting fails).
    const u32 flags = me->value(uf::UNIT_FIELD_FLAGS);
    m_charmed = (flags & kUnitFlagPossessed) != 0 || game::guidValue(*me, uf::UNIT_FIELD_CHARMEDBY) != 0;
    if (!play.control().playerHasControl() || m_charmed ||
        (flags & (kUnitFlagStunned | kUnitFlagConfused | kUnitFlagFleeing)) != 0) {
        m_takeover = Takeover::NoControl;
        takeOverNoControl(session, bar, resolved, view);
    } else {
        view.dimmed = false;
    }
    return false;
}

bool TouchPetScreens::takeOverControlled(GameSession& session, CombatScreens& combat,
                                         std::span<TouchAction> resolved, TouchBarView& view, u32 nowMs) {
    game::Gameplay& play = session.play();
    const game::PetState& pet = play.abilities().pet();
    const u64 mover = play.control().wantedMover();
    // The unit's bar, its slots placed when it changes (a possession's bar comes with the possession).
    u64 key = mix(1469598103934665603ull, pet.guid == mover ? pet.guid : 0);
    for (const u32 packed : pet.bar) {
        key = mix(key, packed);
    }
    if (key != m_controlledKey) {
        m_controlledKey = key;
        m_controlled.fill(-1);
        if (pet.guid == mover) {
            m_controlled = game::controlledSlots(
                pet,
                [](const void* context, u32 spellId) {
                    const auto* data = static_cast<const game::GameData*>(context);
                    const std::optional<game::SpellInfo> info = data ? data->spell(spellId) : std::nullopt;
                    return info && info->passive();
                },
                m_data);
        }
    }
    for (u32 s = 0; s < game::kTouchSlots; ++s) {
        TouchButtonView& b = view.places[s];
        b = {};
        resolved[s] = {};
        if (s >= game::kTouchSkills || m_controlled[s] < 0) {
            continue;
        }
        const u32 slot = static_cast<u32>(m_controlled[s]);
        resolved[s] = {TouchActionKind::PetAction, 0, 0, slot};
        b.slot = combat.petActionView(session, slot, nowMs);
        b.shown = b.slot.iconHash != 0;
        // The Attack and the spells for an enemy fire as the finger goes down, as the player's own (§4).
        const u32 action = net::petButtonAction(pet.bar[slot]);
        const bool command = net::petButtonType(pet.bar[slot]) == net::kPetActCommand;
        const std::optional<game::SpellInfo> info = !command && m_data ? m_data->spell(action) : std::nullopt;
        b.onPress = command || (info && info->targetKind() == game::SpellTargetKind::Enemy);
    }
    for (const TouchPlace p : {TouchPlace::Mount, TouchPlace::Pet}) {
        view.places[touchIndex(p)] = {};
    }
    // The Exit: the channel that holds the unit (Mind Control, Eyes of the Beast, Eye of Kilrogg).
    const game::CastBar& cast = play.spells().cast();
    m_exitSpell = cast.active() && cast.channel ? cast.spellId : 0;
    TouchButtonView& exit = view.places[touchIndex(TouchPlace::Reactive)];
    exit = {};
    exit.shown = true;
    const std::optional<game::SpellInfo> info =
        m_data && m_exitSpell ? m_data->spell(m_exitSpell) : std::nullopt;
    exit.slot.iconHash = info ? info->iconHash : m_iconExit;
    exit.slot.label = text::get("EXIT");
    view.classBarCount = 0;
    view.comboPoints = 0;
    view.dimmed = false;
    view.fanPlace = -1;
    return true;
}

bool TouchPetScreens::takeOverDead(GameSession& session, std::span<TouchAction> resolved, TouchBarView& view,
                                   u32 nowMs) {
    game::Gameplay& play = session.play();
    const game::DeathState& death = play.death();
    // The ghost by its corpse (as the popups' CORPSE_IN_RANGE: within CORPSE_RECLAIM_RADIUS on its map).
    bool inRange = false;
    if (const Entity* me = session.entities().find(session.playerGuid());
        me && death.ghost && death.corpseKnown && me->hasPosition &&
        static_cast<u32>(death.corpseMap) == session.map() && death.corpseRealMap == session.map()) {
        const glm::vec3 at = me->positionAt(nowMs);
        const f32 dx = at.x - death.corpseX, dy = at.y - death.corpseY, dz = at.z - death.corpseZ;
        inRange = dx * dx + dy * dy + dz * dz <= game::kCorpseReclaimRadius * game::kCorpseReclaimRadius;
    }
    const u32 delay = play.reclaimDelayRemaining(nowMs);
    m_death = game::deathButtons(death, inRange, delay);
    const bool bodyDown = m_death.active && !death.ghost;
    if (bodyDown && !m_wasDead) {
        m_deathSinceMs = nowMs;
    }
    m_wasDead = bodyDown;
    if (!m_death.active) {
        m_reclaimTotalMs = 0;
        return false;
    }
    m_panelOpen = false;
    m_foodOpen = false;
    for (u32 s = 0; s < game::kTouchSlots; ++s) {
        view.places[s] = {};
        resolved[s] = {};
    }
    for (const TouchPlace p : {TouchPlace::Mount, TouchPlace::Pet, TouchPlace::Reactive}) {
        view.places[touchIndex(p)] = {};
    }
    // A wait shown as a cooldown: the corpse delay, the first moments of Release.
    m_reclaimTotalMs = delay > 0 ? std::max(m_reclaimTotalMs, delay) : 0;
    const auto wait = [&](HudActionSlot& slot, u32 leftMs, u32 totalMs) {
        if (leftMs > 0 && totalMs > 0) {
            slot.cooldownLeft = static_cast<f32>(leftMs) / 1000.0f;
            slot.cooldownTotal = static_cast<f32>(totalMs) / 1000.0f;
            slot.usable = false;
        }
    };
    const u32 grace =
        bodyDown && nowMs - m_deathSinceMs < kReleaseGraceMs ? kReleaseGraceMs - (nowMs - m_deathSinceMs) : 0;
    if (m_death.main != game::DeathAction::None) {
        TouchButtonView& b = view.places[touchIndex(TouchPlace::Main)];
        b.shown = true;
        b.slot.iconHash = m_death.main == game::DeathAction::Release ? m_iconRelease : m_iconResurrect;
        if (m_death.main == game::DeathAction::Release) {
            wait(b.slot, grace, kReleaseGraceMs);
        } else {
            wait(b.slot, m_death.mainWaitMs, m_reclaimTotalMs);
            b.glow = m_death.mainWaitMs == 0;
        }
    }
    if (m_death.reactive == game::DeathAction::SelfRes) {
        TouchButtonView& b = view.places[touchIndex(TouchPlace::Reactive)];
        b.shown = true;
        const std::optional<game::SpellInfo> info =
            m_data ? m_data->spell(m_death.selfResSpell) : std::nullopt;
        b.slot.iconHash = info ? info->iconHash : m_iconResurrect;
        wait(b.slot, grace, kReleaseGraceMs);
        b.glow = grace == 0;
    }
    if (m_death.ringA == game::DeathAction::AcceptRes) {
        TouchButtonView& b = view.places[touchIndex(TouchPlace::RingA)];
        b.shown = true;
        b.slot.iconHash = m_iconResurrect;
        b.slot.label = text::get("ACCEPT");
        wait(b.slot, m_death.ringAWaitMs, std::max(m_reclaimTotalMs, m_death.ringAWaitMs));
        b.glow = m_death.ringAWaitMs == 0;
    }
    view.classBarCount = 0;
    view.comboPoints = 0;
    view.dimmed = false;
    view.fanPlace = -1;
    return true;
}

const game::SpellImmunity& TouchPetScreens::immunityOf(u32 spellId) {
    // Looked up once per spell (open addressing; a full table only costs lookups).
    const u32 n = static_cast<u32>(m_immunities.size());
    for (u32 i = 0, at = (spellId * 2654435761u) % n; i < n; ++i, at = (at + 1) % n) {
        Immunity& e = m_immunities[at];
        if (e.spellId == spellId) {
            return e.immunity;
        }
        if (e.spellId == 0) {
            e.spellId = spellId;
            game::SpellDetails details;
            e.immunity = m_abilities && m_abilities->spellText().details(spellId, details)
                             ? game::spellImmunity(details)
                             : game::SpellImmunity{};
            return e.immunity;
        }
    }
    static const game::SpellImmunity kNone;
    return kNone;
}

void TouchPetScreens::takeOverNoControl(GameSession& session, TouchBarScreens& bar,
                                        std::span<const TouchAction> resolved, TouchBarView& view) {
    view.dimmed = true;
    const Entity* me = session.entities().find(session.playerGuid());
    if (!me) {
        return;
    }
    // The player's control auras (its debuffs), read again when they change.
    u64 key = 1469598103934665603ull;
    for (u32 slot = game::kMaxPositiveAuras; slot < game::kMaxAuras; ++slot) {
        key = mix(key, me->value(static_cast<u16>(uf::UNIT_FIELD_AURA + slot)));
    }
    if (key != m_controlKey) {
        m_controlKey = key;
        m_controlCount = 0;
        for (u32 slot = game::kMaxPositiveAuras; slot < game::kMaxAuras && m_controlCount < m_controls.size();
             ++slot) {
            const u32 spellId = me->value(static_cast<u16>(uf::UNIT_FIELD_AURA + slot));
            game::SpellDetails details;
            if (spellId != 0 && m_abilities && m_abilities->spellText().details(spellId, details) &&
                game::controlAura(details, m_controls[m_controlCount])) {
                ++m_controlCount;
            }
        }
    }
    if (m_charmed) {
        return; // charmed or possessed by another: the client acts for nobody
    }
    // What breaks the control stays usable (Will of the Forsaken against a fear, Berserker Rage, Blink
    // against a stun, Divine Shield, a trinket's spell...): the spell's immunities cover every control aura
    // (Spell::CheckCasterAuras).
    const std::span<const game::ControlAura> controls{m_controls.data(), m_controlCount};
    for (u32 s = 0; s < game::kTouchSlots && s < resolved.size(); ++s) {
        TouchButtonView& b = view.places[s];
        if (!b.shown) {
            continue;
        }
        u32 spell = 0;
        TouchAction a = resolved[s];
        if (a.kind == TouchActionKind::Group && a.id < game::kTouchGroups) { // the group's current action
            const game::TouchGroup& g = bar.layout().current().groups[a.id];
            a = g.count > 0 ? g.actions[std::min<u32>(g.current, g.count - 1u)] : TouchAction{};
        }
        if (a.kind == TouchActionKind::PetAction) {
            b.breaksControl =
                true; // the pet obeys its feared master (HandlePetAction asks nothing of the owner)
            continue;
        }
        if (a.kind == TouchActionKind::Spell) {
            spell = a.id; // the line's first rank: the immunities do not change with it
        } else if (a.kind ==
                   TouchActionKind::Item) { // an item put in the slot: its on-use spell (a trinket's)
            if (const net::ItemQueryInfo* info = session.item(a.id)) {
                for (const net::ItemQueryInfo::Spell& use : info->spells) {
                    spell = spell == 0 && use.trigger == net::kItemSpellTriggerOnUse ? use.id : spell;
                }
            }
        }
        b.breaksControl = spell != 0 && game::breaksControl(immunityOf(spell), controls);
    }
}

// --- After the HUD -----------------------------------------------------------------------------------------

bool TouchPetScreens::actOnHud(GameSession& session, TouchBarScreens& bar, const TouchBarOutput& out,
                               CombatScreens& combat, u32 nowMs, u64& aim) {
    game::Gameplay& play = session.play();
    game::Abilities& abilities = play.abilities();
    const i32 petPlace = static_cast<i32>(touchIndex(TouchPlace::Pet));
    // The death's buttons, the Exit.
    if (m_takeover == Takeover::Dead && out.pressed >= 0) {
        const auto inGrace = [&] { return nowMs - m_deathSinceMs < kReleaseGraceMs; };
        if (out.pressed == static_cast<i32>(touchIndex(TouchPlace::Main))) {
            if (m_death.main == game::DeathAction::Release && !inGrace()) {
                play.releaseSpirit(); // RepopMe
            } else if (m_death.main == game::DeathAction::Reclaim && play.canReclaimCorpse(nowMs)) {
                play.reclaimCorpse(); // RetrieveCorpse
            }
        } else if (out.pressed == static_cast<i32>(touchIndex(TouchPlace::Reactive)) &&
                   m_death.reactive == game::DeathAction::SelfRes && !inGrace()) {
            play.selfResurrect(); // UseSoulstone
        } else if (out.pressed == static_cast<i32>(touchIndex(TouchPlace::RingA)) &&
                   m_death.ringA == game::DeathAction::AcceptRes && m_death.ringAWaitMs == 0) {
            play.answerResurrect(true); // AcceptResurrect
        }
        return false;
    }
    if (m_takeover == Takeover::Controlled &&
        out.pressed == static_cast<i32>(touchIndex(TouchPlace::Reactive))) {
        // Leave the unit: the channel holding it cancelled as its aura (CMSG_CANCEL_AURA:
        // HandleCancelAuraOpcode lets a possession go while the player moves another unit, and interrupts the
        // channel; CMSG_CANCEL_CHANNELLING is ignored when the unit is a player).
        if (m_exitSpell != 0) {
            play.cancelAura(m_exitSpell);
        } else {
            play.cancelCast();
        }
        if (m_trace) {
            VD_LOG_INFO("Touch pets: exit (%u)", m_exitSpell);
        }
        return false;
    }
    // The pet button.
    if (out.fanPlace == petPlace && out.fanChosen >= 0 &&
        static_cast<u32>(out.fanChosen) < m_spells.demonCount) {
        const u32 demon = m_spells.demons[static_cast<u32>(out.fanChosen)];
        aim = combat.useButton(session, {net::kActionSpell, demon});
        bar.layout().lastDemon = demon;
        bar.changed();
        return false;
    }
    if (out.pressed == petPlace) {
        switch (m_choice.mode) {
        case game::PetButtonMode::CallPet:
        case game::PetButtonMode::RevivePet:
        case game::PetButtonMode::SummonDemon:
            aim = combat.useButton(session, {net::kActionSpell, m_choice.spellId});
            if (m_choice.mode == game::PetButtonMode::SummonDemon) {
                bar.layout().lastDemon = m_choice.spellId;
                bar.changed();
            }
            break;
        case game::PetButtonMode::Command:
            abilities.petCommand(m_choice.command, m_choice.target); // PetAttack / PetFollow
            break;
        default: break;
        }
        if (m_trace) {
            VD_LOG_INFO("Touch pets: button (mode %u, spell %u, command %u)", static_cast<u32>(m_choice.mode),
                        m_choice.spellId, m_choice.command);
        }
        return false;
    }
    if (out.held == petPlace && m_choice.mode == game::PetButtonMode::Command) {
        m_panelOpen = true;
        m_foodOpen = false;
        m_petRect = out.heldRect;
        return true;
    }
    return out.held == petPlace;
}

// --- The panel ---------------------------------------------------------------------------------------------

void TouchPetScreens::buildPanel(GameSession& session, CombatScreens& combat, u32 nowMs) {
    const game::PetState& pet = session.play().abilities().pet();
    const game::SpellText* text = m_abilities ? &m_abilities->spellText() : nullptr;
    m_panelCount = 0;
    const auto add = [&](PanelEntry entry, const HudActionSlot& slot, std::string_view label, u8 group,
                         bool autocastable = false) {
        if (m_panelCount >= kMaxPanel || slot.iconHash == 0) {
            return;
        }
        m_panelEntries[m_panelCount] = entry;
        m_panel[m_panelCount] = {slot, label, group, autocastable};
        ++m_panelCount;
    };
    // Its commands and reactions (whatever their places on its bar), its spells as its bar orders them, the
    // owner's spells on it, Feed Pet, the dismiss / abandon confirmations, the demons.
    for (const u32 command : kPetCommands) {
        add({PanelKind::Command, command},
            combat.petActionView(session, game::touchPetCommand(command), nowMs),
            text::get(kCommandText[command]), 0);
    }
    for (const u32 react : kPetReactions) { // beside the commands when the row has room
        add({PanelKind::Reaction, react}, combat.petActionView(session, game::touchPetReaction(react), nowMs),
            text::get(kReactionText[react]), 4);
    }
    for (u32 slot = 0; slot < pet.bar.size(); ++slot) {
        const u8 type = net::petButtonType(pet.bar[slot]);
        const u32 spell = net::petButtonAction(pet.bar[slot]);
        if (type == net::kPetActCommand || type == net::kPetActReaction || spell == 0) {
            continue;
        }
        add({PanelKind::PetSlot, slot}, combat.petActionView(session, slot, nowMs),
            text ? text->name(spell) : "", 1, type == net::kPetActEnabled || type == net::kPetActDisabled);
    }
    for (u32 i = 0; i < m_spells.onPetCount; ++i) {
        const u32 spell = m_spells.onPet[i];
        add({PanelKind::OwnerSpell, spell},
            combat.touchView(session, {net::kActionSpell, spell}, kCacheOnPet + i, nowMs),
            text ? text->name(spell) : "", 2);
    }
    const Entity* unit = pet.active() ? session.entities().find(pet.guid) : nullptr;
    const bool hunterPet = unit && unit->value(uf::UNIT_FIELD_MAXPOWER5) > 0; // it eats
    if (m_spells.feedPet != 0 && hunterPet) {
        add({PanelKind::Feed, m_spells.feedPet},
            combat.touchView(session, {net::kActionSpell, m_spells.feedPet}, kCacheFeed, nowMs),
            text ? text->name(m_spells.feedPet) : "", 2);
    }
    {
        HudActionSlot dismiss;
        const std::optional<game::SpellInfo> info =
            m_data && m_spells.dismissPet ? m_data->spell(m_spells.dismissPet) : std::nullopt;
        dismiss.iconHash = info ? info->iconHash : m_iconDismiss;
        add({PanelKind::Dismiss, 0}, dismiss, text::get("PET_DISMISS"), 2);
    }
    for (u32 i = 0; i < m_spells.demonCount; ++i) {
        add({PanelKind::Demon, m_spells.demons[i]},
            combat.touchView(session, {net::kActionSpell, m_spells.demons[i]}, kCacheDemons + i, nowMs), {},
            3);
    }
}

void TouchPetScreens::usePanel(GameSession& session, TouchBarScreens& bar, CombatScreens& combat, u32 index,
                               bool held, u64& aim) {
    game::Gameplay& play = session.play();
    game::Abilities& abilities = play.abilities();
    const PanelEntry& e = m_panelEntries[index];
    if (held) { // a pet spell's autocast (ToggleSpellAutocast); nothing else is held
        if (e.kind == PanelKind::PetSlot && m_panel[index].autocastable) {
            const u8 type = net::petButtonType(abilities.pet().bar[e.value]);
            abilities.setPetAutocast(e.value, type == net::kPetActDisabled);
        }
        return;
    }
    bool close = true;
    switch (e.kind) {
    case PanelKind::Command: abilities.petCommand(e.value, play.target()); break;
    case PanelKind::Reaction:
        abilities.petReact(e.value);
        close = false; // its choice shows at once
        break;
    case PanelKind::PetSlot: abilities.petAction(e.value, play.target()); break;
    case PanelKind::OwnerSpell: aim = combat.useButton(session, {net::kActionSpell, e.value}); break;
    case PanelKind::Feed:
        m_foodOpen = true;
        m_foodKey = 0;
        break;
    case PanelKind::Dismiss: combat.askPetMenu(); break; // Dismiss, and a hunter's Abandon (ABANDON_PET)
    case PanelKind::Demon:
        aim = combat.useButton(session, {net::kActionSpell, e.value});
        bar.layout().lastDemon = e.value;
        bar.changed();
        break;
    }
    if (m_trace) {
        VD_LOG_INFO("Touch pets: panel %u (%u)", static_cast<u32>(e.kind), e.value);
    }
    m_panelOpen = !close;
}

void TouchPetScreens::buildFood(GameSession& session, const game::TouchLayout& layout) {
    // Rebuilt when the bags or the item answers change (and once a food was taken or refused).
    const game::Inventory& inventory = session.play().inventory();
    const u32 key = static_cast<u32>(mix(inventory.version(), session.queryVersion())) | 1u; // 0: rebuild
    if (key == m_foodKey) {
        return;
    }
    m_foodKey = key;
    m_foodCount = 0;
    const Entity* me = session.entities().find(session.playerGuid());
    if (!me) {
        return;
    }
    const u32 family = petFamily(session);
    // The candidates: what the family ate before, the foods (an on-use Food spell), the trade goods (raw meat
    // and fish have no use of their own); the server decides (Pet::HaveInDiet: the food type the item query
    // leaves out).
    game::forEachInventoryItem(session.entities(), *me, [&](const game::InventoryItem& item) {
        if (item.entry == 0 || m_foodCount >= m_foods.size()) {
            return;
        }
        for (u32 i = 0; i < m_foodCount; ++i) {
            if (m_foods[i].entry == item.entry) {
                return;
            }
        }
        const net::ItemQueryInfo* info = session.item(item.entry);
        if (!info) {
            return; // asked for: listed once known (queryVersion)
        }
        bool food = false;
        for (const net::ItemQueryInfo::Spell& use : info->spells) {
            if (use.id == 0 || use.trigger != net::kItemSpellTriggerOnUse || !m_data || !m_abilities) {
                continue;
            }
            const std::optional<game::SpellInfo> spell = m_data->spell(use.id);
            game::SpellDetails details;
            if (spell && m_abilities->spellText().details(use.id, details)) {
                food = food || (game::classifyConsumable(*spell, details, use.category).flags &
                                game::kConsumeFood) != 0;
            }
        }
        const bool eaten = game::petFoodAccepted(layout.petFood, family, item.entry) >= 0;
        if (food || eaten || info->itemClass == kItemClassTradeGoods) {
            m_foods[m_foodCount++] = {item.entry, inventory.count(item.entry), info->itemLevel, food};
        }
    });
    m_foodCount = game::sortPetFood(layout.petFood, family, {m_foods.data(), m_foodCount});
    for (u32 i = 0; i < m_foodCount; ++i) {
        const net::ItemQueryInfo* info = session.item(m_foods[i].entry);
        TouchPetFoodRow& row = m_foodRows[i];
        row = {};
        row.icon = info && m_data ? m_data->itemIconHash(info->displayId) : 0;
        row.name = info ? std::string_view(info->name) : std::string_view{};
        row.count = m_foods[i].count;
        row.quality = info ? info->quality : 1;
        row.accepted = game::petFoodAccepted(layout.petFood, family, m_foods[i].entry) >= 0;
    }
}

void TouchPetScreens::feed(GameSession& session, u32 row, u32 nowMs) {
    const Entity* me = session.entities().find(session.playerGuid());
    game::InventoryItem item;
    if (!me || row >= m_foodCount || m_spells.feedPet == 0 ||
        !game::findInventoryItem(session.entities(), *me, m_foods[row].entry, item)) {
        return;
    }
    // Feed Pet on the food (CMSG_CAST_SPELL with the item target, as the client's cursor sends it); its
    // answer is watched: a stack going down, or SPELL_FAILED_WRONG_PET_FOOD / _FOOD_LOWLEVEL.
    if (session.play().castSpellOnItem(m_spells.feedPet, {item.bag, item.slot})) {
        m_feedEntry = item.entry;
        m_feedCount = session.play().inventory().count(item.entry);
        m_feedFamily = petFamily(session);
        m_feedMs = nowMs;
        if (m_trace) {
            VD_LOG_INFO("Touch pets: feeding %u to family %u", item.entry, m_feedFamily);
        }
    }
}

void TouchPetScreens::draw(GameSession& session, TouchBarScreens& bar, CombatScreens& combat,
                           DialogContext& c, u32 nowMs) {
    if (!m_panelOpen && !m_foodOpen) {
        return;
    }
    const game::PetState& pet = session.play().abilities().pet();
    const Entity* unit = pet.active() ? session.entities().find(pet.guid) : nullptr;
    if (!unit || !session.inWorld() || !bar.enabled() || m_takeover == Takeover::Controlled ||
        m_takeover == Takeover::Dead) {
        m_panelOpen = m_foodOpen = false; // the pet gone, the classic bars, another unit controlled, death
        return;
    }
    VD_PROFILE_ZONE("TouchPetScreens::draw");
    if (m_foodOpen) {
        buildFood(session, bar.layout());
        TouchPetFoodView v;
        v.title = m_abilities ? m_abilities->spellText().name(m_spells.feedPet) : std::string_view{};
        v.empty = text::get("VD_TOUCH_PETS_NO_FOOD");
        v.rows = {m_foodRows.data(), m_foodCount};
        v.anchor = m_petRect;
        v.mirrored = bar.mirrored();
        const TouchPetFoodResult r = touchPetFood(c, v);
        if (r.tapped >= 0) {
            feed(session, static_cast<u32>(r.tapped), nowMs);
            m_foodOpen = false;
            m_panelOpen = false;
        } else if (r.closed) {
            m_foodOpen = false;
            m_panelOpen = false;
        }
        return;
    }
    buildPanel(session, combat, nowMs);
    TouchPetPanelView v;
    v.title = session.play().abilities().petName();
    const net::CreatureQueryInfo* info = session.creature(unit->value(uf::OBJECT_FIELD_ENTRY));
    if (v.title.empty() && info) {
        v.title = info->name; // a charmed creature has no pet name
    }
    const game::PetData* pets = combat.petData();
    const std::string_view family = pets && info ? pets->familyName(info->family) : std::string_view{};
    v.subtitle =
        family.empty()
            ? text::format(m_subtitle, "UNIT_LEVEL_TEMPLATE", unit->value(uf::UNIT_FIELD_LEVEL))
            : text::format(m_subtitle, "FRIENDS_LEVEL_TEMPLATE", unit->value(uf::UNIT_FIELD_LEVEL), family);
    if (unit->value(uf::UNIT_FIELD_MAXPOWER5) > 0 && pets) {
        v.happiness = pets->happiness(unit->value(uf::UNIT_FIELD_POWER5)).level;
    }
    v.buttons = {m_panel.data(), m_panelCount};
    v.anchor = m_petRect;
    v.buttonSize = m_petRect.w;
    v.mirrored = bar.mirrored();
    const TouchPetPanelResult r = touchPetPanel(c, v);
    u64 aim = 0;
    if (r.held >= 0) {
        usePanel(session, bar, combat, static_cast<u32>(r.held), true, aim);
    } else if (r.tapped >= 0) {
        usePanel(session, bar, combat, static_cast<u32>(r.tapped), false, aim);
    } else if (r.closed) {
        m_panelOpen = false;
    }
}

bool TouchPetScreens::escape() {
    const bool open = m_panelOpen || m_foodOpen;
    m_panelOpen = false;
    m_foodOpen = false;
    return open;
}

bool TouchPetScreens::firesOnPress(GameSession& session, u32 id) const {
    const game::PetState& pet = session.play().abilities().pet();
    const game::TouchPetAction a = game::decodeTouchPetAction(id);
    if (a.kind == game::TouchPetKind::Command) {
        return a.value == net::kPetCommandAttack;
    }
    const i32 at = game::touchPetSlot(pet, id, m_abilities ? &m_abilities->spellText() : nullptr);
    u32 spell = a.kind == game::TouchPetKind::Spell ? a.value : 0;
    if (at >= 0) {
        const u32 packed = pet.bar[static_cast<u32>(at)];
        if (net::petButtonType(packed) == net::kPetActCommand) {
            return net::petButtonAction(packed) == net::kPetCommandAttack;
        }
        spell = net::petButtonType(packed) == net::kPetActReaction ? 0 : net::petButtonAction(packed);
    }
    const std::optional<game::SpellInfo> info = m_data && spell ? m_data->spell(spell) : std::nullopt;
    return info && info->targetKind() == game::SpellTargetKind::Enemy;
}

// --- The edit mode's Pet tab -------------------------------------------------------------------------------

u32 TouchPetScreens::pickerEntries(GameSession& session, CombatScreens& combat,
                                   std::span<PickerEntry> out) const {
    const game::Abilities& abilities = session.play().abilities();
    const game::PetState& pet = abilities.pet();
    const game::SpellText* text = m_abilities ? &m_abilities->spellText() : nullptr;
    u32 n = 0;
    if (!pet.active()) {
        return n;
    }
    const auto add = [&](u32 id, std::string_view name) {
        if (n >= out.size()) {
            return;
        }
        const HudActionSlot view = combat.petActionView(session, id, session.lastUpdateMs());
        if (view.iconHash != 0) {
            out[n++] = {{TouchActionKind::PetAction, 0, 0, id}, view.iconHash, name};
        }
    };
    // Its spells (by spell: Spell Lock stays Spell Lock whatever the felhunter's bar), then its commands and
    // reactions.
    for (const u32 spell : abilities.petBook()) {
        if (abilities.petSpellState(spell) != net::kPetActPassive) {
            add(spell, text ? text->name(spell) : std::string_view{});
        }
    }
    for (const u32 command : kPetCommands) {
        add(game::touchPetCommand(command), text::get(kCommandText[command]));
    }
    for (const u32 react : kPetReactions) {
        add(game::touchPetReaction(react), text::get(kReactionText[react]));
    }
    return n;
}

// --- VD_TOUCH_PETS_DEMO ------------------------------------------------------------------------------------

void TouchPetScreens::prepareOffline(TouchBarView& view, i32 fanPlace, u32 nowMs) {
    if (m_demo == Demo::None) {
        return;
    }
    const auto spellIcon = [&](u32 spell) -> u64 {
        const std::optional<game::SpellInfo> info = m_data ? m_data->spell(spell) : std::nullopt;
        return info ? info->iconHash : 0;
    };
    TouchButtonView& pet = view.places[touchIndex(TouchPlace::Pet)];
    pet = {};
    pet.shown = true;
    switch (m_demo) {
    case Demo::Pet:
    case Demo::Panel:
    case Demo::Feed:
    case Demo::Feared:
        // A wolf, content, attacking; Spell Lock as a pet action on G.
        pet.slot.iconHash = assetPathHash("Interface\\Icons\\Ability_Hunter_Pet_Wolf.blp");
        pet.cornerIcon = kHappiness;
        pet.cornerUv = happinessFace(2);
        pet.slot.checked = true;
        pet.breaksControl = true;
        break;
    case Demo::CallPet: pet.slot.iconHash = spellIcon(883); break; // Call Pet
    case Demo::Revive: pet.slot.iconHash = spellIcon(982); break;  // Revive Pet
    case Demo::Demons:
        pet.slot.iconHash = spellIcon(691); // Summon Felhunter (the last one), a fan of the four
        pet.group = true;
        if (fanPlace == static_cast<i32>(touchIndex(TouchPlace::Pet))) {
            static constexpr u32 kDemons[] = {688, 697, 712, 691};
            view.fanPlace = fanPlace;
            view.fanCount = 4;
            view.fanCurrent = 3;
            for (u32 i = 0; i < 4; ++i) {
                view.fan[i] = HudActionSlot{spellIcon(kDemons[i])};
            }
        }
        break;
    case Demo::Controlled: {
        // A possessed creature's bar (Mind Control): its Attack on Main, two spells, the Exit.
        static constexpr u32 kSpells[] = {6603, 11977, 3391}; // Attack, Rend, Thrash
        for (u32 s = 0; s < game::kTouchSlots; ++s) {
            view.places[s] = {};
        }
        const TouchPlace places[] = {TouchPlace::Main, TouchPlace::RingA, TouchPlace::RingC};
        for (u32 i = 0; i < 3; ++i) {
            TouchButtonView& b = view.places[touchIndex(places[i])];
            b.shown = true;
            b.slot.iconHash =
                i == 0 ? assetPathHash("Interface\\Icons\\Ability_GhoulFrenzy.blp") : spellIcon(kSpells[i]);
        }
        view.places[touchIndex(TouchPlace::RingA)].slot.cooldownLeft = 4.0f;
        view.places[touchIndex(TouchPlace::RingA)].slot.cooldownTotal = 6.0f;
        pet = {};
        view.places[touchIndex(TouchPlace::Mount)] = {};
        TouchButtonView& exit = view.places[touchIndex(TouchPlace::Reactive)];
        exit = {};
        exit.shown = true;
        exit.slot.iconHash = spellIcon(605); // Mind Control
        exit.slot.label = text::get("EXIT");
        view.classBarCount = 0;
        view.comboPoints = 0;
        break;
    }
    case Demo::Dead:
    case Demo::Ghost: {
        for (u32 s = 0; s < game::kTouchSlots; ++s) {
            view.places[s] = {};
        }
        pet = {};
        for (const TouchPlace p : {TouchPlace::Mount, TouchPlace::Reactive}) {
            view.places[touchIndex(p)] = {};
        }
        TouchButtonView& main = view.places[touchIndex(TouchPlace::Main)];
        main.shown = true;
        if (m_demo == Demo::Dead) { // Release Spirit, and a soulstone
            main.slot.iconHash = m_iconRelease;
            TouchButtonView& self = view.places[touchIndex(TouchPlace::Reactive)];
            self.shown = true;
            self.glow = true;
            self.slot.iconHash = spellIcon(20707); // Soulstone Resurrection
        } else { // Resurrect at the corpse, its delay running; a resurrection offered
            main.slot.iconHash = m_iconResurrect;
            const f32 left = 30.0f - static_cast<f32>(nowMs % 30000) / 1000.0f;
            main.slot.cooldownLeft = left;
            main.slot.cooldownTotal = 30.0f;
            main.slot.usable = false;
            TouchButtonView& accept = view.places[touchIndex(TouchPlace::RingA)];
            accept.shown = true;
            accept.glow = true;
            accept.slot.iconHash = m_iconResurrect;
            accept.slot.label = text::get("ACCEPT");
        }
        view.classBarCount = 0;
        view.comboPoints = 0;
        break;
    }
    default: break;
    }
    if (m_demo == Demo::Pet || m_demo == Demo::Feared) {
        TouchButtonView& g = view.places[touchIndex(TouchPlace::ColumnG)];
        g.shown = true;
        g.slot = HudActionSlot{spellIcon(19647)}; // Spell Lock (a pet action), its cooldown in seconds
        g.slot.cooldownLeft = 18.0f - static_cast<f32>(nowMs % 18000) / 1000.0f;
        g.slot.cooldownTotal = 24.0f;
    }
    if (m_demo == Demo::Feared) {
        // Feared: everything faint but Will of the Forsaken (H) and the pet.
        view.dimmed = true;
        TouchButtonView& h = view.places[touchIndex(TouchPlace::ColumnH)];
        h.shown = true;
        h.slot = HudActionSlot{spellIcon(7744)};
        h.breaksControl = true;
    }
    if (m_demoPanel || m_demoFood) {
        // The pet button's rect as the HUD draws it is not known offline: the windows open beside the
        // cluster's top-left corner instead.
        m_panelOpen = m_demoPanel;
        m_foodOpen = m_demoFood;
    }
}

void TouchPetScreens::actOffline(const TouchBarOutput& out) {
    if (m_demo == Demo::None) {
        return;
    }
    if (out.held == static_cast<i32>(touchIndex(TouchPlace::Pet))) {
        m_petRect = out.heldRect;
        m_demoPanel = true;
    }
}

void TouchPetScreens::drawOffline(DialogContext& c) {
    if (m_demo == Demo::None || (!m_demoPanel && !m_demoFood)) {
        return;
    }
    const auto spellIcon = [&](u32 spell) -> u64 {
        const std::optional<game::SpellInfo> info = m_data ? m_data->spell(spell) : std::nullopt;
        return info ? info->iconHash : 0;
    };
    const game::SpellText* text = m_abilities ? &m_abilities->spellText() : nullptr;
    const auto name = [&](u32 spell) { return text ? text->name(spell) : std::string_view{}; };
    if (m_petRect.w <= 0.0f) { // not held yet: beside where the square profile's pet button is
        const f32 d = c.ui.px(48.0f);
        m_petRect = {c.safeArea.right() - c.ui.px(330.0f), c.safeArea.bottom() - c.ui.px(330.0f), d, d};
    }
    if (m_demoFood) {
        static constexpr const char* kNames[] = {"Haunch of Meat", "Mutton Chop", "Tough Jerky",
                                                 "Stringy Wolf Meat"};
        static constexpr u64 kIcons[] = {assetPathHash("Interface\\Icons\\INV_Misc_Food_14.blp"),
                                         assetPathHash("Interface\\Icons\\INV_Misc_Food_48.blp"),
                                         assetPathHash("Interface\\Icons\\INV_Misc_Food_16.blp"),
                                         assetPathHash("Interface\\Icons\\INV_Misc_Food_14.blp")};
        std::array<TouchPetFoodRow, 4> rows;
        for (u32 i = 0; i < rows.size(); ++i) {
            rows[i] = {kIcons[i], kNames[i], 20u - i * 4u, 1, i == 0};
        }
        TouchPetFoodView v;
        v.title = name(6991);
        v.empty = ::vd::text::get("VD_TOUCH_PETS_NO_FOOD");
        v.rows = rows;
        v.anchor = m_petRect;
        if (const TouchPetFoodResult r = touchPetFood(c, v); r.tapped >= 0 || r.closed) {
            m_demoFood = false;
        }
        return;
    }
    // A wolf's panel: Attack, Follow (in use), Stay, Aggressive, Defensive (in use), Passive; Bite (cooling
    // down), Claw, Growl (autocast), Dash; Mend Pet, Eyes of the Beast, Feed Pet, Dismiss. A felhunter's (the
    // warlock): Devour Magic (autocast), Tainted Blood, Spell Lock (cooling down), Paranoia; Health Funnel, Dismiss;
    // the four demons.
    std::array<TouchPetPanelButton, 18> buttons;
    u32 n = 0;
    static constexpr u64 kCommandIcons[] = {assetPathHash("Interface\\Icons\\Ability_GhoulFrenzy.blp"),
                                            assetPathHash("Interface\\Icons\\Ability_Tracking.blp"),
                                            assetPathHash("Interface\\Icons\\Spell_Nature_TimeStop.blp"),
                                            assetPathHash("Interface\\Icons\\Ability_Racial_BloodRage.blp"),
                                            assetPathHash("Interface\\Icons\\Ability_Defend.blp"),
                                            assetPathHash("Interface\\Icons\\Ability_Seal.blp")};
    static constexpr const char* kLabels[] = {"PET_ACTION_ATTACK",  "PET_ACTION_FOLLOW",
                                              "PET_ACTION_WAIT",    "PET_MODE_AGGRESSIVE",
                                              "PET_MODE_DEFENSIVE", "PET_MODE_PASSIVE"};
    for (u32 i = 0; i < 6; ++i) {
        TouchPetPanelButton& b = buttons[n++];
        b.slot = HudActionSlot{kCommandIcons[i]};
        b.slot.checked = i == 1 || i == 4;
        b.label = ::vd::text::get(kLabels[i]);
        b.group = i < 3 ? 0 : 4;
    }
    static constexpr u32 kPetSpells[] = {17253, 16827, 2649, 23099};  // Bite, Claw, Growl, Dash
    static constexpr u32 kDemonSpells[] = {19505, 19478, 19244, 19480}; // the felhunter's
    for (u32 i = 0; i < 4; ++i) {
        TouchPetPanelButton& b = buttons[n++];
        const u32 spell = m_demoWarlock ? kDemonSpells[i] : kPetSpells[i];
        b.slot = HudActionSlot{spellIcon(spell)};
        b.slot.checked = i == (m_demoWarlock ? 0u : 2u);
        b.label = name(spell);
        b.group = 1;
        b.autocastable = true;
        if (i == (m_demoWarlock ? 2u : 0u)) {
            b.slot.cooldownLeft = 6.0f;
            b.slot.cooldownTotal = 10.0f;
        }
    }
    static constexpr u32 kOwner[] = {136, 1002, 6991,
                                     2641}; // Mend Pet, Eyes of the Beast, Feed Pet, Dismiss Pet
    static constexpr u32 kWarlockOwner[] = {755, 2641}; // Health Funnel, Dismiss
    static constexpr u32 kDemons[] = {688, 697, 712, 691};
    for (u32 i = 0; i < (m_demoWarlock ? 2u : 4u); ++i) {
        TouchPetPanelButton& b = buttons[n++];
        const u32 spell = m_demoWarlock ? kWarlockOwner[i] : kOwner[i];
        b.slot = HudActionSlot{spellIcon(spell)};
        b.label = spell == 2641 ? ::vd::text::get("PET_DISMISS") : name(spell);
        b.group = 2;
    }
    for (u32 i = 0; i < (m_demoWarlock ? 4u : 0u); ++i) {
        TouchPetPanelButton& b = buttons[n++];
        b.slot = HudActionSlot{spellIcon(kDemons[i])};
        b.group = 3;
    }
    TouchPetPanelView v;
    v.title = m_demoWarlock ? "Grimrak" : "Grimfang";
    v.subtitle = ::vd::text::format(m_subtitle, "FRIENDS_LEVEL_TEMPLATE", 40,
                                    std::string_view(m_demoWarlock ? "Felhunter" : "Wolf"));
    v.happiness = m_demoWarlock ? 0 : 2;
    v.buttons = {buttons.data(), n};
    v.anchor = m_petRect;
    v.buttonSize = m_petRect.w;
    const TouchPetPanelResult r = touchPetPanel(c, v);
    if (r.tapped == 12 && !m_demoWarlock) { // Feed Pet: the list
        m_demoFood = true;
        m_demoPanel = false;
    } else if (r.tapped >= 0 || r.closed) {
        m_demoPanel = r.tapped >= 3 && r.tapped < 6; // a reaction keeps it open
    }
}

} // namespace vd
