#include "game/Tutorials.h"

#include "core/Profile.h"
#include "game/Gameplay.h"
#include "game/ItemDefs.h"
#include "game/UnitInfo.h"
#include "net/Messages.h"
#include "net/UpdateFields.gen.h"

#include <glm/geometric.hpp>

namespace vd::game {
namespace {

using net::Opcode;
namespace uf = net::uf;

// WoW.exe's call sites of TriggerTutorial (0x4B5390), by tutorial:
//   Questgivers, Welcome     0x490A41 PLAYER_ENTERING_WORLD
//   Movement                 0x482FE8 WorldFrame: 90 s since the input control started, unless flagged (by moving)
//   Targeting                0x514A83 InputControl: a movement key (FlagTutorial(Movement), Targeting in 10 s)
//   Chatting.. Talents       0x5E4077 SMSG_LEVELUP_INFO: level >= 3, 4 (Trainers, Cameras), 5, 7 (Friends,
//                            Professions), 8, 10
//   Players, Grouping,       0x493706 the target changed: another player (once); else a friendly player from level 5,
//   CombatMode, Travel       else a unit the player can attack, else a flight master (UNIT_NPC_FLAGS 0x08)
//   Spells                   0x629B10 UnitCombat: the player's own auto attack starts
//   Looting                  0x6003A0 UNIT_DYNAMIC_FLAGS gains UNIT_DYNFLAG_LOOTABLE
//   Backpack.. Keyrings      0x491730 SMSG_ITEM_PUSH_RESULT for the player: Backpack always; UsingItems (an on-use spell);
//                            EquippableItems (its inventory type has a slot, 0x809200); Bags (class 1); Keyrings (into
//                            the keyring: here class 13, the keys that go there); Hearthstones (6948); RangedWeapons
//                            (INVTYPE_RANGED, not a hunter)
//   Food, Drink              0x5ECAC0 in combat: health, or mana for mana users, under half
//   Reputation               0x4D5710 SMSG_SET_FACTION_VISIBLE from level 10
//   ReplyingToTells          0x49A870 a CHAT_MSG_WHISPER line
//   Vendors                  0x4FACF0 MERCHANT_SHOW
//   QuestLog, EliteQuests    0x501130 / 0x5DDCF0 a quest accepted; elite: its QuestInfo type is 1
//   Death                    0x5DE9C0 PLAYER_FLAGS_GHOST set
//   Rested                   0x5DE4E0 UPDATE_EXHAUSTION to rest state 1 (the 0x80AF50 table)
//   Fatigue, Breath          0x5E7990 MIRROR_TIMER_START of type 0 / 1
//   Swimming                 0x60DF70 the active mover starts swimming
//   Resting                  0x5EE8D0 PLAYER_FLAGS_RESTING set
//   Pvp                      0x5FF4E0 UNIT_FLAG_PVP set on the player
//   QuestCompletion          0x4DF270 every objective of a quest met
//   Travel                   0x4DB710 TAXIMAP_OPENED
//   DamagedItems, Broken     0x4C7EE0 the durability frame's alert status 3 / 4
//   TheSpellbook             0x4B25B0 LEARNED_SPELL_IN_TAB
//   UnavailableQuestGivers   0x5EFE60 a quest giver near with DIALOG_STATUS_UNAVAILABLE
//   Ammunition               0x6E1910 SMSG_CAST_RESULT with SPELL_FAILED_NEED_AMMO (47)
//   RaidGroups               0x4BA5F0 a raid roster with members
//   MeetingStones            0x4CA1C0 SMSG_MEETINGSTONE_SETQUEUE status 1
//   BattlegroundQueue, Port  0x4AA850 SMSG_BATTLEFIELD_STATUS 1 (queued) / 2 (ready)

constexpr u32 kMovementIdleMs = 90000;     // 0x15F90
constexpr u32 kTargetingDelayMs = 10000;   // 0x2710
constexpr u32 kPollMs = 250;               // the player's fields (ours: WoW.exe reacts to each field change)
constexpr u32 kScanMs = 500;               // the units around (lootable corpses, grey quest givers)
constexpr f32 kQuestGiverRange = 30.0f;    // ours: WoW.exe compares with a runtime distance (0xC4C28C)
constexpr u32 kHearthstone = 6948;         // 0x1B24
constexpr u32 kClassHunter = 3;
constexpr u8 kSpellFailedNeedAmmo = 47;    // SPELL_FAILED_NEED_AMMO
constexpr u32 kMirrorFatigue = 0, kMirrorBreath = 1;
constexpr u32 kQuestTypeElite = 1;

// The inventory types with an equipment or bag slot (WoW.exe's table 0x809200, bits 0..22): all but none, ammunition
// and quivers.
bool equippable(u32 inventoryType) {
    return inventoryType != kInvTypeNonEquip && inventoryType != kInvTypeAmmo && inventoryType != 27 &&
           inventoryType <= 28;
}

u32 tutorialId(Tutorial t) {
    return static_cast<u32>(t);
}

} // namespace

bool Tutorials::handle(Opcode opcode, std::span<const u8> payload) {
    if (opcode != Opcode::SMSG_TUTORIAL_FLAGS) {
        return false;
    }
    net::TutorialWords words{};
    if (!net::parseTutorialFlags(payload, words)) {
        return false;
    }
    setFlags(words);
    return true;
}

void Tutorials::setFlags(const net::TutorialWords& words) {
    // 0x4B5700: both sets take the server's words; a session starts over.
    m_triggered = words;
    m_flagged = words;
    m_known = true;
    ++m_session;
    for (Timer& t : m_timers) {
        t.id = kTutorialCount;
    }
}

void Tutorials::enterWorld(bool newLogin, u32 nowMs) {
    m_inWorld = true;
    m_eventCursor = m_events.end();
    if (newLogin) {
        m_idleSinceMs = nowMs;
        m_lastTarget = 0;
        m_restState = 0;
        m_playerFlags = 0;
        m_unitFlags = 0;
        m_swimming = false;
        m_fatigue = m_breath = false;
        m_pendingItems.fill(0);
        m_resetChat = true;
    }
    trigger(Tutorial::Welcome, nowMs); // 0x490A41: Welcome first, then Questgivers (the queue's order)
    trigger(Tutorial::Questgivers, nowMs);
}

void Tutorials::leaveWorld() {
    m_inWorld = false;
}

void Tutorials::trigger(Tutorial tutorial, u32 nowMs, u32 delayMs) {
    const u32 id = tutorialId(tutorial);
    if (!m_known || id >= kTutorialCount || test(m_triggered, id)) {
        return;
    }
    set(m_triggered, id);
    if (delayMs == 0) {
        fire(id, nowMs);
        return;
    }
    for (Timer& t : m_timers) {
        if (t.id == kTutorialCount) {
            t.id = id;
            t.dueMs = nowMs + delayMs;
            return;
        }
    }
    fire(id, nowMs); // no timer left: at once
}

void Tutorials::fire(u32 id, u32 nowMs) {
    // The TutorialPopup sound and TUTORIAL_TRIGGER(id + 1).
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = GameEventKind::TutorialTriggered;
    e.timeMs = nowMs;
    e.id = id;
    e.value[0] = static_cast<i32>(m_session);
}

void Tutorials::flag(Tutorial tutorial) {
    const u32 id = tutorialId(tutorial);
    if (!m_known || id >= kTutorialCount || test(m_flagged, id)) {
        return;
    }
    for (Timer& t : m_timers) {
        if (t.id == id) {
            t.id = kTutorialCount;
        }
    }
    set(m_triggered, id);
    set(m_flagged, id);
    std::vector<u8> body;
    net::writeTutorialFlag(body, id);
    m_sink(Opcode::CMSG_TUTORIAL_FLAG, body);
}

void Tutorials::clear() {
    for (Timer& t : m_timers) {
        t.id = kTutorialCount;
    }
    m_triggered.fill(0xFFFFFFFFu);
    m_flagged.fill(0xFFFFFFFFu);
    m_sink(Opcode::CMSG_TUTORIAL_CLEAR, {});
}

void Tutorials::reset() {
    m_triggered.fill(0);
    m_flagged.fill(0);
    m_sink(Opcode::CMSG_TUTORIAL_RESET, {});
}

bool Tutorials::enabled() const {
    for (const u32 word : m_flagged) {
        if (word != 0xFFFFFFFFu) {
            return true;
        }
    }
    return false;
}

void Tutorials::playerMoved(u32 nowMs) {
    if (!m_known) {
        return;
    }
    m_idleSinceMs = nowMs;
    flag(Tutorial::Movement);
    trigger(Tutorial::Targeting, nowMs, kTargetingDelayMs);
}

void Tutorials::update(Gameplay& play, u32 nowMs) {
    VD_PROFILE_ZONE("Tutorials::update");
    if (!m_known || !m_inWorld) {
        return;
    }
    for (Timer& t : m_timers) {
        if (t.id != kTutorialCount && static_cast<i32>(nowMs - t.dueMs) >= 0) {
            const u32 id = t.id;
            t.id = kTutorialCount;
            fire(id, nowMs);
        }
    }
    if (!test(m_triggered, tutorialId(Tutorial::Movement)) && nowMs - m_idleSinceMs >= kMovementIdleMs) {
        trigger(Tutorial::Movement, nowMs);
    }
    readEvents(play, nowMs);
    watchTarget(play, nowMs);
    if (static_cast<i32>(nowMs - m_nextPollMs) >= 0) {
        m_nextPollMs = nowMs + kPollMs;
        watchPlayer(play, nowMs);
    }
    if (static_cast<i32>(nowMs - m_nextScanMs) >= 0) {
        m_nextScanMs = nowMs + kScanMs;
        scanUnits(play, nowMs);
    }
}

bool Tutorials::itemReceived(Gameplay& play, u32 entry, u32 nowMs) {
    const net::ItemQueryInfo* item = play.queries().item(entry);
    if (!item) {
        return false;
    }
    if (!item->found) {
        return true;
    }
    for (const net::ItemQueryInfo::Spell& spell : item->spells) {
        if (spell.id != 0 && spell.trigger == net::kItemSpellTriggerOnUse) {
            trigger(Tutorial::UsingItems, nowMs);
            break;
        }
    }
    if (equippable(item->inventoryType)) {
        trigger(Tutorial::EquippableItems, nowMs);
    }
    if (item->itemClass == kItemClassContainer) {
        trigger(Tutorial::Bags, nowMs);
    }
    if (item->itemClass == kItemClassKey) {
        trigger(Tutorial::Keyrings, nowMs);
    }
    if (entry == kHearthstone) {
        trigger(Tutorial::Hearthstones, nowMs);
    }
    if (item->inventoryType == kInvTypeRanged) {
        const Entity* me = m_entities.find(play.player());
        if (me && me->byteValue(uf::UNIT_FIELD_BYTES_0, 1) != kClassHunter) {
            trigger(Tutorial::RangedWeapons, nowMs);
        }
    }
    return true;
}

void Tutorials::readEvents(Gameplay& play, u32 nowMs) {
    if (m_resetChat) {
        m_resetChat = false;
        m_chatCursor = play.chat().end();
    }
    // Items whose template was asked for earlier.
    for (u32& pending : m_pendingItems) {
        if (pending != 0 && itemReceived(play, pending, nowMs)) {
            pending = 0;
        }
    }
    while (const GameEvent* e = m_events.next(m_eventCursor)) {
        switch (e->kind) {
        case GameEventKind::LevelUp: {
            const i32 level = e->value[0];
            if (level >= 3) {
                trigger(Tutorial::Chatting, nowMs);
            }
            if (level >= 4) {
                trigger(Tutorial::Trainers, nowMs);
                trigger(Tutorial::Cameras, nowMs);
            }
            if (level >= 5) {
                trigger(Tutorial::Groups, nowMs);
            }
            if (level >= 7) {
                trigger(Tutorial::Friends, nowMs);
                trigger(Tutorial::Professions, nowMs);
            }
            if (level >= 8) {
                trigger(Tutorial::Jumping, nowMs);
            }
            if (level >= 10) {
                trigger(Tutorial::Talents, nowMs);
            }
            break;
        }
        case GameEventKind::ItemReceived:
            trigger(Tutorial::Backpack, nowMs);
            if (!itemReceived(play, e->id, nowMs)) {
                for (u32& pending : m_pendingItems) {
                    if (pending == 0) {
                        pending = e->id;
                        break;
                    }
                }
            }
            break;
        case GameEventKind::AttackStarted: trigger(Tutorial::Spells, nowMs); break;
        case GameEventKind::FactionVisible: {
            const Entity* me = m_entities.find(play.player());
            if (me && me->value(uf::UNIT_FIELD_LEVEL) >= 10) {
                trigger(Tutorial::Reputation, nowMs);
            }
            break;
        }
        case GameEventKind::VendorOpened: trigger(Tutorial::Vendors, nowMs); break;
        case GameEventKind::QuestAccepted: {
            trigger(Tutorial::QuestLog, nowMs);
            if (const QuestInfo* quest = play.quest(e->id); quest && quest->type == kQuestTypeElite) {
                trigger(Tutorial::EliteQuests, nowMs);
            }
            break;
        }
        case GameEventKind::QuestObjectivesDone: trigger(Tutorial::QuestCompletion, nowMs); break;
        case GameEventKind::PlayerReleased: trigger(Tutorial::Death, nowMs); break;
        case GameEventKind::TaxiMapOpened: trigger(Tutorial::Travel, nowMs); break;
        case GameEventKind::SpellLearned: trigger(Tutorial::TheSpellbook, nowMs); break;
        case GameEventKind::CastFailed:
            if (e->value[0] == kSpellFailedNeedAmmo) {
                trigger(Tutorial::Ammunition, nowMs);
            }
            break;
        case GameEventKind::MeetingStoneChanged:
            if (e->value[0] == 1 && e->id != 0) { // queued (SETQUEUE status 1)
                trigger(Tutorial::MeetingStones, nowMs);
            }
            break;
        case GameEventKind::BattlefieldStatus:
            if (e->value[0] == static_cast<i32>(BattlefieldState::Queued)) {
                trigger(Tutorial::BattlegroundQueue, nowMs);
            } else if (e->value[0] == static_cast<i32>(BattlefieldState::Confirm)) {
                trigger(Tutorial::PortToBattleground, nowMs);
            }
            break;
        default: break;
        }
    }
    while (const ChatLine* line = play.chat().next(m_chatCursor)) {
        if (line->type == net::ChatType::Whisper && line->source == ChatSource::Message) {
            trigger(Tutorial::ReplyingToTells, nowMs);
        }
    }
}

void Tutorials::watchTarget(Gameplay& play, u32 nowMs) {
    // 0x493706: what the new target is decides one tutorial.
    const u64 target = play.target();
    if (target == m_lastTarget) {
        return;
    }
    m_lastTarget = target;
    const Entity* me = m_entities.find(play.player());
    const Entity* unit = target != 0 ? m_entities.find(target) : nullptr;
    if (!me || !unit || target == play.player() ||
        (unit->type != ObjectType::Unit && unit->type != ObjectType::Player)) {
        return;
    }
    if (unit->type == ObjectType::Player && !triggered(Tutorial::Players)) {
        trigger(Tutorial::Players, nowMs);
        return;
    }
    if (unit->type == ObjectType::Player && reactionTo(*me, *unit, play.reactions()) == Reaction::Friendly &&
        me->value(uf::UNIT_FIELD_LEVEL) >= 5) {
        trigger(Tutorial::Grouping, nowMs);
    } else if (canAttack(*me, *unit, play.reactions())) {
        trigger(Tutorial::CombatMode, nowMs);
    } else if ((unit->value(uf::UNIT_NPC_FLAGS) & kNpcFlagFlightMaster) != 0) {
        trigger(Tutorial::Travel, nowMs);
    }
}

void Tutorials::watchPlayer(Gameplay& play, u32 nowMs) {
    const Entity* me = m_entities.find(play.player());
    if (!me || me->values.size() <= uf::PLAYER_BYTES_2) {
        return;
    }
    const u32 playerFlags = me->value(uf::PLAYER_FLAGS);
    const u32 gained = playerFlags & ~m_playerFlags;
    m_playerFlags = playerFlags;
    if (gained & kPlayerFlagGhost) {
        trigger(Tutorial::Death, nowMs);
    }
    if (gained & kPlayerFlagResting) {
        trigger(Tutorial::Resting, nowMs);
    }
    const u32 rest = me->byteValue(uf::PLAYER_BYTES_2, 3);
    if (rest != m_restState && rest == kRestStateRested) {
        trigger(Tutorial::Rested, nowMs);
    }
    m_restState = rest;
    const u32 unitFlags = me->value(uf::UNIT_FIELD_FLAGS);
    if ((unitFlags & ~m_unitFlags) & kUnitFlagPvp) {
        trigger(Tutorial::Pvp, nowMs);
    }
    m_unitFlags = unitFlags;
    const bool swimming = (me->moveFlags & net::kMoveFlagSwimming) != 0;
    if (swimming && !m_swimming) {
        trigger(Tutorial::Swimming, nowMs);
    }
    m_swimming = swimming;
    const MirrorTimer* fatigue = play.mirrorTimers().find(kMirrorFatigue);
    const MirrorTimer* breath = play.mirrorTimers().find(kMirrorBreath);
    const bool fatigueOn = fatigue && fatigue->active, breathOn = breath && breath->active;
    if (fatigueOn && !m_fatigue) {
        trigger(Tutorial::Fatigue, nowMs);
    }
    if (breathOn && !m_breath) {
        trigger(Tutorial::Breath, nowMs);
    }
    m_fatigue = fatigueOn;
    m_breath = breathOn;
    // 0x5ECAC0: in combat, under half health; a mana user under half mana.
    if ((unitFlags & kUnitFlagInCombat) != 0) {
        const u32 health = me->value(uf::UNIT_FIELD_HEALTH), maxHealth = me->value(uf::UNIT_FIELD_MAXHEALTH);
        if (maxHealth > 0 && health * 2 < maxHealth) {
            trigger(Tutorial::Food, nowMs);
        }
        const u32 mana = me->value(uf::UNIT_FIELD_POWER1), maxMana = me->value(uf::UNIT_FIELD_MAXPOWER1);
        if (me->byteValue(uf::UNIT_FIELD_BYTES_0, 3) == 0 && maxMana > 0 && mana * 2 < maxMana) {
            trigger(Tutorial::Drink, nowMs);
        }
    }
    // The durability frame's alerts (GetInventoryAlertStatus 4 broken, 3 damaged).
    const DurabilityAlerts& alerts = play.inventory().durability();
    for (const DurabilityAlert part : alerts.parts) {
        if (part == DurabilityAlert::Broken) {
            trigger(Tutorial::BrokenItems, nowMs);
        } else if (part == DurabilityAlert::Low) {
            trigger(Tutorial::DamagedItems, nowMs);
        }
    }
    if (play.social().group().raid && !play.social().group().members.empty()) {
        trigger(Tutorial::RaidGroups, nowMs);
    }
}

void Tutorials::scanUnits(Gameplay& play, u32 nowMs) {
    const bool looting = !triggered(Tutorial::Looting);
    const bool questGivers = !triggered(Tutorial::UnavailableQuestGivers);
    if (!looting && !questGivers) {
        return;
    }
    const Entity* me = m_entities.find(play.player());
    if (!me) {
        return;
    }
    bool lootable = false, unavailable = false;
    m_entities.forEach([&](const Entity& e) {
        if (e.type != ObjectType::Unit) {
            return;
        }
        if (looting && (e.value(uf::UNIT_DYNAMIC_FLAGS) & kDynFlagLootable) != 0) {
            lootable = true;
        }
        if (questGivers && play.questGiverStatus(e.guid) == net::QuestGiverStatus::Unavailable &&
            glm::distance(e.position, me->position) <= kQuestGiverRange) {
            unavailable = true;
        }
    });
    if (lootable) {
        trigger(Tutorial::Looting, nowMs);
    }
    if (unavailable) {
        trigger(Tutorial::UnavailableQuestGivers, nowMs);
    }
}

} // namespace vd::game
