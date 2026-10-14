#include "game/Gameplay.h"

#include "core/Log.h"

#include "game/ActionBar.h"
#include "game/GameObjectAnim.h"
#include "game/ItemDefs.h"
#include "game/UnitInfo.h"
#include "net/QuestMessages.h"
#include "net/SpellMessages.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace vd::game {
namespace {

using net::Opcode;
namespace uf = net::uf;

constexpr u32 kStatusRefreshDelayMs = 300; // quest events come in bursts: one status round after them
constexpr u32 kStatusPruneMs = 10000;      // statuses of objects gone are forgotten this often
constexpr u32 kDuplicateEventMs = 5000;    // a packet and a field change telling the same thing
constexpr u32 kMoneyWaitMs = 5000;         // how long the looted money's coinage change is waited for
constexpr u8 kCastFailedDontReport = 23;   // SPELL_FAILED_DONT_REPORT
// Phase 13 (spells): the reasons the client gives itself (SpellCastResult, net::spellCastResultName).
constexpr u8 kCastFailedNoTarget = 9;     // SPELL_FAILED_BAD_IMPLICIT_TARGETS: "No target"
constexpr u8 kCastFailedBadTargets = 10;  // SPELL_FAILED_BAD_TARGETS: "Invalid target"
constexpr u8 kCastFailedOutOfRange = 89;  // SPELL_FAILED_OUT_OF_RANGE
constexpr u8 kCastFailedTooClose = 118;   // SPELL_FAILED_TOO_CLOSE

// Player GUIDs have a zero high part (HIGHGUID_PLAYER); creatures 0xF130, pets 0xF140...
bool isPlayerGuid(u64 guid) {
    return guid != 0 && (guid >> 48) == 0;
}

bool due(u32 nowMs, u32 atMs) {
    return static_cast<i32>(nowMs - atMs) >= 0;
}

} // namespace

Gameplay::Gameplay(const EntityStore& entities) : m_entities(entities) {
    m_body.reserve(256);
    m_questGiverStatus.reserve(256);
    // Phase 13 (professions): recipes are cast as any spell without a target.
    m_tradeSkills.setCaster({[](void* context, u32 spellId) {
                                 return static_cast<Gameplay*>(context)->castSpell(spellId, 0);
                             },
                             this});
    m_spells.setMods(&m_spellMods); // phase 13 (spells)
    m_commands.setReadyCheck(&m_readyCheck); // phase 16 (alerts): DoReadyCheck runs the ready check's rules
}

void Gameplay::setSink(PacketSink sink) {
    m_sink = sink;
    m_queries.setSink(sink);
    m_abilities.setSink(sink);
    m_character.setSink(sink); // phase 15 (character)
    m_social.setSink(sink);
    m_commands.setSink(sink); // phase 13 (social)
    m_readyCheck.setSink(sink); // phase 16 (alerts)
    m_meetingStone.setSink(sink);
    m_battlegrounds.setSink(sink); // phase 17 (lead)
    // Phase 17 (queue).
    m_battlefieldQueueing.setSink(sink);
    // Phase 17 (scores).
    m_battlefieldScore.setSink(sink);
    // Phase 17 (honor).
    m_areaSpiritHealer.setSink(sink);
    // Phase 18 (glue).
    // Phase 18 (create).
    // Phase 18 (p3).
    m_tutorials.setSink(sink);
    m_accountData.setSink(sink);
    m_guildControl.setSink(sink); // phase 15 (guild)
    m_petitions.setSink(sink);
    m_tabards.setSink(sink);
    m_help.setSink(sink); // phase 15 (help)
    m_inventory.setSink(sink);
    m_travel.setSink(sink);
    m_exchange.setSink(sink);
    m_minimap.setSink(sink);
    m_logout.setSink(sink); // parity (popups)
    m_summon.setSink(sink);
    m_control.setSink(sink); // phase 13 (combat)
    m_stable.setSink(sink);  // phase 15 (pets)
}

void Gameplay::setData(const GameData* data, const Reactions* reactions) {
    m_reactions = reactions;
    m_gameData = data; // phase 13 (spells): enchantments, the class's spell family
    setSpellSource(data ? spellSource(*data) : SpellSource{});
}

void Gameplay::setSpellSource(SpellSource source) {
    m_spellSource = source;
    m_spells.setSource(source);
    m_abilities.setSpellSource(source);
}

// -------------------------------------------------------------------------------------------------------
// Session.

void Gameplay::enterWorld(u64 player, u32 map, bool newLogin, u32 nowMs) {
    const bool sameCharacter = !newLogin && player == m_player;
    m_nowMs = nowMs;
    m_inWorld = true;
    m_queries.setOnline(true);
    m_player = player;
    m_map = map;
    m_target = 0;
    m_standing.player = player; // phase 13
    m_standing.reputation = &m_abilities.reputation();
    if (!sameCharacter) {
        m_standing.forced.clear(); // the server sends the new character's own
    }
    m_melee.stop();
    m_sheathKnown = false; // read again from the player's fields
    m_spells.clearCast();
    m_dialog.close();
    m_loot.close();
    m_questGiverStatus.clear();
    m_statusRefreshPending = false;
    m_statusPruneAt = nowMs + kStatusPruneMs;
    m_moneyRequested = false;
    m_expectMoney = false;
    m_mirrorTimers.reset(); // MirrorTimer.lua: PLAYER_ENTERING_WORLD hides the bars
    m_abilities.enterWorld(player, sameCharacter, nowMs);
    m_character.enterWorld(player, !sameCharacter); // phase 15 (character)
    m_social.enterWorld(player, !sameCharacter, nowMs);
    m_commands.enterWorld(player, !sameCharacter, nowMs); // phase 13 (social)
    m_readyCheck.enterWorld(player, !sameCharacter, nowMs); // phase 16 (alerts)
    m_meetingStone.enterWorld(player, nowMs);               // PLAYER_ENTERING_WORLD asks for the queue
    m_battlegrounds.enterWorld(player, map, !sameCharacter, nowMs); // phase 17 (lead): CMSG_BATTLEFIELD_STATUS
    // The old world's states go with it (0x491222 -> 0x4C55B0), also on a far teleport: until the new zone's INIT
    // GetNumWorldStateUI is 0 (BattlefieldMinimap_OnEvent's PLAYER_ENTERING_WORLD hides the battle map then).
    m_worldStates.leaveWorld();
    // Phase 17 (queue).
    m_battlefieldQueueing.enterWorld(player, !sameCharacter, nowMs); // the list forgotten (0x4A9DB0)
    // Phase 17 (scores).
    m_battlefieldScore.enterWorld(player, nowMs); // 0x4A9DB0: the score, columns and positions start over
    // Phase 17 (honor).
    m_areaSpiritHealer.enterWorld(player, nowMs);
    m_honor.enterWorld(player);
    // Phase 18 (glue).
    // Phase 18 (create).
    // Phase 18 (p3).
    m_tutorials.enterWorld(newLogin, nowMs); // PLAYER_ENTERING_WORLD: Welcome, Questgivers
    // Phase 18 (extras).
    if (newLogin) {
        m_chatLanguage = 0; // the edit box speaks the player's own language again
    }
    m_guildControl.enterWorld(player, !sameCharacter); // phase 15 (guild)
    m_petitions.enterWorld(player, nowMs);
    m_tabards.enterWorld(player, nowMs);
    m_help.enterWorld(nowMs); // phase 15 (help): PLAYER_ENTERING_WORLD asks for the ticket
    m_inventory.enterWorld(player, newLogin, nowMs);
    m_travel.enterWorld(player, map, !sameCharacter, nowMs);
    m_exchange.enterWorld(player, !sameCharacter, nowMs);
    m_minimap.enterWorld(player);
    m_logout.enterWorld(); // parity (popups)
    m_summon.enterWorld();
    m_weather.enterWorld(); // phase 11: the new zone's weather comes right after
    m_tradeSkills.enterWorld(player, sameCharacter); // phase 13
    m_stable.enterWorld(player, nowMs);              // phase 15 (pets)
    m_control.enterWorld(player, nowMs); // phase 13 (combat): the player moves itself again
    if (!sameCharacter) {
        m_spells.reset();
        m_auraDurations.reset();
        m_enchantTimers.reset();
        m_questLog.reset();
        m_journal.reset(); // phase 13 (quests)
        m_death = DeathState{};
        m_standState = 0;
        m_coinage = 0;
    } else if (m_death.ghost) {
        queryCorpse(); // where the corpse is seen from this map
    }
}

void Gameplay::leaveWorld() {
    m_inWorld = false;
    m_standing.player = 0; // phase 13
    m_social.leaveWorld();
    m_commands.leaveWorld(); // phase 13 (social)
    m_readyCheck.leaveWorld(); // phase 16 (alerts)
    m_meetingStone.leaveWorld();
    m_battlegrounds.leaveWorld(); // phase 17 (lead)
    m_worldStates.leaveWorld();
    // Phase 17 (queue).
    m_battlefieldQueueing.leaveWorld();
    // Phase 17 (scores).
    m_worldStateUi.leaveWorld();
    m_battlefieldScore.leaveWorld();
    // Phase 17 (honor).
    m_areaSpiritHealer.leaveWorld();
    m_honor.leaveWorld();
    // Phase 18 (glue).
    // Phase 18 (create).
    // Phase 18 (p3).
    m_tutorials.leaveWorld();
    m_guildControl.leaveWorld(); // phase 15 (guild)
    m_petitions.leaveWorld();
    m_tabards.leaveWorld();
    m_help.leaveWorld(); // phase 15 (help)
    m_queries.setOnline(false);
    m_queries.forgetRequests(); // answers that never came: ask again next time
    m_target = 0;
    m_melee.stop();
    m_sheathKnown = false; // read again from the player's fields
    m_spells.clearCast();
    m_dialog.close();
    m_loot.close();
    m_questGiverStatus.clear();
    m_statusRefreshPending = false;
    m_abilities.leaveWorld();
    m_character.leaveWorld(); // phase 15 (character)
    m_inventory.leaveWorld();
    m_travel.leaveWorld();
    m_exchange.leaveWorld();
    m_minimap.leaveWorld();
    m_logout.leaveWorld(); // parity (popups)
    m_summon.leaveWorld();
    m_weather.leaveWorld();
    m_tradeSkills.leaveWorld(); // phase 13
    m_stable.leaveWorld();      // phase 15 (pets)
    // Phase 13 (spells): the next login sends its modifiers again, before SMSG_LOGIN_VERIFY_WORLD.
    m_targeting.stop();
    m_spellMods.reset();
    m_spellMods.setClassFamily(0);
    m_control.leaveWorld(); // phase 13 (combat)
}

void Gameplay::update(u32 nowMs) {
    m_nowMs = nowMs;
    if (!m_inWorld) {
        return;
    }
    if (const Entity* player = me()) {
        const u8 field = player->byteValue(uf::UNIT_FIELD_BYTES_2, 0);
        if (!m_sheathKnown || (field != m_sheathField && field != m_sheathAsked)) {
            m_sheath = field; // on entering the world, or the server's own change
            m_sheathAsked = field;
            m_sheathAskedAt = nowMs - 1000;
            m_sheathKnown = true;
        }
        m_sheathField = field;
    }
    m_social.update(nowMs);
    m_commands.update(nowMs); // phase 13 (social): raid target icons, /pvp
    m_readyCheck.update(nowMs); // phase 16 (alerts): CheckReadyCheckTime, ReadyCheckFrame's timer
    // Phase 17 (queue).
    m_battlefieldQueueing.update(nowMs); // walking away from the list, the queues' sounds, closing messages
    // Phase 17 (scores).
    m_worldStateUi.update(nowMs);     // the WorldStateUI list (a new INIT, the LocalDefense channel)
    m_battlefieldScore.update(nowMs); // the score's names, the columns of an active queue
    // Phase 17 (honor).
    m_areaSpiritHealer.update(nowMs, m_reactions); // a ghost's spirit guide (0x4923B0)
    // Phase 18 (glue).
    // Phase 18 (create).
    // Phase 18 (p3).
    m_tutorials.update(*this, nowMs); // what triggers the tutorials
    m_accountData.update(nowMs);      // the account data's 30 s save timers
    m_help.update(nowMs); // phase 15 (help): the open ticket asked for again every 10 minutes
    m_inventory.refresh(nowMs);
    if (m_target != 0 && !m_entities.find(m_target)) {
        m_target = 0; // gone (out of range, despawned)
    }
    if (m_melee.attacking()) { // the victim died or left: done (the server's SMSG_ATTACKSTOP may still be on its way)
        const Entity* victim = m_entities.find(m_melee.target());
        if (!victim || isDead(*victim)) {
            const u64 guid = m_melee.target();
            m_melee.stop();
            event(GameEventKind::AttackStopped, 0, guid);
        }
    }
    if (m_statusRefreshPending && due(nowMs, m_statusRefreshAt)) {
        m_statusRefreshPending = false;
        refreshQuestGiverStatuses();
    }
    if (due(nowMs, m_statusPruneAt)) {
        m_statusPruneAt = nowMs + kStatusPruneMs;
        std::erase_if(m_questGiverStatus,
                      [&](const auto& entry) { return m_entities.find(entry.first) == nullptr; });
    }
    m_travel.update(nowMs);
    m_exchange.update(nowMs);
    m_tradeSkills.update(nowMs); // phase 13: after the inventory's refresh
    if (m_targeting.active()) { // phase 13 (spells): dead, or the item used up: nothing waits any more
        const Entity* player = me();
        const ItemInstance* item = m_targeting.fromItem() ? m_inventory.at(m_targeting.itemPos()) : nullptr;
        if (!player || isDead(*player) || (m_targeting.fromItem() && (!item || item->guid != m_targeting.itemGuid()))) {
            stopTargeting();
        }
    }
    m_journal.update(nowMs); // phase 13 (quests): timed watches run out
    m_control.update(nowMs); // phase 13 (combat): the far sight's view point and the mover
}

void Gameplay::afterUpdate(std::span<const u64> created, u32 nowMs) {
    m_nowMs = nowMs;
    if (!m_inWorld) {
        return;
    }
    m_inventory.markDirty();
    for (const u64 guid : created) {
        const Entity* e = m_entities.find(guid);
        if (e && e->guid != m_player && isQuestGiver(*e)) {
            queryQuestGiverStatus(guid);
        }
    }
    m_travel.afterUpdate(created);
    const Entity* player = me();
    if (!player) {
        return;
    }
    // Phase 13 (spells): the modifiers apply to the spells of the player's class family.
    const u32 cls = player->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
    const u32 family = m_gameData ? m_gameData->classSpellFamily(cls) : 0;
    m_spellMods.setClassFamily(family != 0 ? family : classSpellFamily(cls));
    const bool baseline = !m_questLog.initialized();
    const bool questsChanged = m_questLog.refresh(*player, [&](GameEventKind kind, u32 questId) {
        const bool alsoByPacket =
            kind == GameEventKind::QuestObjectivesDone || kind == GameEventKind::QuestFailed;
        if (!alsoByPacket || !recentEvent(kind, questId, kDuplicateEventMs)) {
            event(kind, questId);
        }
    });
    if (questsChanged) {
        for (const QuestLogEntry& q : m_questLog.slots()) {
            if (q.questId != 0) {
                m_queries.quest(q.questId); // the log shows titles and objectives: have them ready
            }
        }
        if (!baseline) { // at login every quest giver is asked as it appears anyway
            scheduleStatusRefresh(nowMs);
        }
    }
    m_auraDurations.onPlayerFields(*player, nowMs);
    updateDeath(*player);
    const u32 coinage = player->value(uf::PLAYER_FIELD_COINAGE);
    if (m_expectMoney && nowMs - m_expectMoneyMs > kMoneyWaitMs) {
        m_expectMoney = false; // never came: a later coinage change is something else
    }
    if (m_expectMoney && coinage > m_coinage) {
        event(GameEventKind::MoneyLooted).value[0] = static_cast<i32>(coinage - m_coinage); // solo: no notify
        m_expectMoney = false;
    }
    m_coinage = coinage;
}

void Gameplay::updateDeath(const Entity& player) {
    const bool ghost = isGhost(player);
    const bool dead = ghost || isDead(player);
    if (dead && !m_death.dead) {
        event(GameEventKind::PlayerDied);
    }
    if (ghost && !m_death.ghost) {
        event(GameEventKind::PlayerReleased);
        queryCorpse();
    }
    if (!dead && m_death.dead) {
        m_death = DeathState{}; // alive again: corpse, offers and delays are over
        event(GameEventKind::PlayerAlive);
    }
    m_death.dead = dead;
    m_death.ghost = ghost;
    m_death.releaseTimer =
        dead && !ghost && (player.byteValue(uf::PLAYER_FIELD_BYTES, 0) & kPlayerBytesReleaseTimer);
    m_death.selfResSpell = dead && !ghost ? player.value(uf::PLAYER_SELF_RES_SPELL) : 0; // parity (popups)
}

// -------------------------------------------------------------------------------------------------------
// Sending.

bool Gameplay::sendBody(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

bool Gameplay::send(Opcode opcode) {
    m_body.clear();
    return sendBody(opcode);
}

bool Gameplay::send(Opcode opcode, u64 guid) {
    m_body.clear();
    net::writeGuid(m_body, guid);
    return sendBody(opcode);
}

GameEvent& Gameplay::event(GameEventKind kind, u32 id, u64 guid) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = kind;
    e.timeMs = m_nowMs;
    e.id = id;
    e.guid = guid;
    return e;
}

ExecuteLogSpell Gameplay::executeLogSpell(u32 spellId) const {
    // UnitCombatLog_C 0x62ACE0: no generic line for spells hidden from the log (attributes 0x180), without a name, or
    // teaching a spell or opening a lock; "perform" for abilities (attribute 0x10).
    ExecuteLogSpell out;
    SpellInfo info;
    if (!m_spellSource.get(spellId, info)) {
        return out; // unknown here: the line names it "Unknown"
    }
    out.perform = (info.attributes & 0x10u) != 0;
    out.hidden = (info.attributes & 0x180u) != 0 || info.name.empty();
    out.quiet = out.hidden;
    for (const u32 effect : info.effects) {
        out.quiet = out.quiet || effect == net::kEffectLearnSpell || effect == net::kEffectOpenLock ||
                    effect == net::kEffectOpenLockItem;
    }
    return out;
}

FxEvent& Gameplay::fx(FxEventKind kind, u64 guid, u32 id, u64 other) {
    FxEvent& e = m_fx.push();
    e = FxEvent{};
    e.kind = kind;
    e.timeMs = m_nowMs;
    e.guid = guid;
    e.id = id;
    e.other = other;
    return e;
}

void Gameplay::fxTargets(FxEvent& e, const net::SpellCastTargets& targets) {
    if (targets.mask & net::kTargetUnit) {
        e.other = targets.unit;
    }
    if (targets.mask & net::kTargetDestLocation) {
        e.hasPosition = true;
        e.position = targets.destination;
    }
}

bool Gameplay::recentEvent(GameEventKind kind, u32 id, u32 withinMs) const {
    u32 cursor = m_events.end() - std::min<u32>(m_events.size(), 16);
    while (const GameEvent* e = m_events.next(cursor)) {
        if (e->kind == kind && e->id == id && m_nowMs - e->timeMs <= withinMs) {
            return true;
        }
    }
    return false;
}

// -------------------------------------------------------------------------------------------------------
// Packets.

Gameplay::Handled Gameplay::handle(Opcode opcode, std::span<const u8> p, u32 nowMs) {
    m_nowMs = nowMs;
    if (const Social::Handled social = m_social.handle(opcode, p, nowMs); social != Social::Handled::No) {
        return social == Social::Handled::Yes ? Handled::Yes : Handled::Malformed; // phase 8: game/Social
    }
    if (PlayerCommands::handles(opcode)) { // phase 13 (social): raid target icons, /played, inspect, pet names
        return m_commands.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    if (ReadyCheck::handles(opcode)) { // phase 16 (alerts): MSG_RAID_READY_CHECK
        return m_readyCheck.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    if (MeetingStone::handles(opcode)) { // phase 16 (alerts): the meeting stones' queue
        return m_meetingStone.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    if (Battlegrounds::handles(opcode)) { // phase 17 (lead): the battleground queues
        return m_battlegrounds.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    if (WorldStates::handles(opcode)) { // phase 17 (lead): the world states
        return m_worldStates.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    // Phase 17 (queue).
    if (BattlefieldQueueing::handles(opcode)) { // the battlemaster's list, a group's join
        return m_battlefieldQueueing.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    // Phase 17 (scores).
    if (BattlefieldScore::handles(opcode)) { // the score, the team's positions, players joining and leaving
        return m_battlefieldScore.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    // Phase 17 (honor).
    if (AreaSpiritHealer::handles(opcode)) { // SMSG_AREA_SPIRIT_HEALER_TIME
        return m_areaSpiritHealer.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    if (Honor::handles(opcode)) { // honor, the insignia, the defense channels
        return m_honor.handle(opcode, p, nowMs, m_reactions) ? Handled::Yes : Handled::Malformed;
    }
    // Phase 18 (glue).
    // Phase 18 (create).
    // Phase 18 (p3).
    if (Tutorials::handles(opcode)) { // SMSG_TUTORIAL_FLAGS
        return m_tutorials.handle(opcode, p) ? Handled::Yes : Handled::Malformed;
    }
    if (AccountData::handles(opcode)) { // SMSG_ACCOUNT_DATA_MD5, SMSG_UPDATE_ACCOUNT_DATA
        return m_accountData.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    // Phase 18 (extras).
    if (handlesExtras(opcode)) { // the mount's special animation, the pet's sounds, renames, item names, rest
        return handleExtras(opcode, p, nowMs);
    }
    if (CharacterSheet::handles(opcode)) { // phase 15 (character): the inspected player's honor
        return m_character.handle(opcode, p) ? Handled::Yes : Handled::Malformed;
    }
    if (Petitions::handles(opcode)) { // phase 15 (guild): the registrar, charters and their signatures
        return m_petitions.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    if (TabardDesigner::handles(opcode)) { // phase 15 (guild): the tabard designer
        return m_tabards.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    if (HelpDesk::handles(opcode)) { // phase 15 (help): GM tickets, the GM survey
        return m_help.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    if (Exchange::handles(opcode)) { // phase 8: group loot rolls, mail, auction house, trade
        if (opcode == Opcode::MSG_AUCTION_HELLO) {
            m_dialog.close(); // the auction house replaces the gossip menu that led there
        }
        if (!m_exchange.handle(opcode, p, nowMs)) {
            return Handled::Malformed;
        }
        const GameEvent* passed = opcode == Opcode::SMSG_LOOT_ALL_PASSED ? m_events.last() : nullptr;
        if (passed && passed->kind == GameEventKind::LootAllPassed) {
            m_loot.onRollEnded(passed->guid, static_cast<u8>(passed->value[2])); // nobody wanted it: free
        }
        return Handled::Yes;
    }
    if (Minimap::handles(opcode)) { // parity pass: the group's minimap pings
        return m_minimap.handle(opcode, p, nowMs) ? Handled::Yes : Handled::Malformed;
    }
    bool ok = true;
    switch (opcode) {
    // Static data queries.
    case Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE: {
        std::optional<net::ItemQueryInfo> info = net::parseItemQueryResponse(p);
        ok = info.has_value();
        if (ok) {
            m_queries.onItem(std::move(*info));
        }
        break;
    }
    case Opcode::SMSG_NAME_QUERY_RESPONSE: {
        std::optional<net::NameQueryInfo> info = net::parseNameQueryResponse(p);
        ok = info.has_value();
        if (ok) {
            m_queries.onName(std::move(*info));
        }
        break;
    }
    case Opcode::SMSG_CREATURE_QUERY_RESPONSE: {
        std::optional<net::CreatureQueryInfo> info = net::parseCreatureQueryResponse(p);
        ok = info.has_value();
        if (ok) {
            m_queries.onCreature(std::move(*info));
        }
        break;
    }
    case Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE:
        ok = net::parseGameObjectQueryResponse(p, m_gameObjectQuery);
        if (ok) {
            m_queries.onGameObject(m_gameObjectQuery);
        }
        break;
    case Opcode::SMSG_NPC_TEXT_UPDATE:
        ok = net::parseNpcTextUpdate(p, m_npcTextUpdate);
        if (ok) {
            m_queries.onNpcText(m_npcTextUpdate);
        }
        break;
    case Opcode::SMSG_QUEST_QUERY_RESPONSE:
        ok = net::parseQuestQueryResponse(p, m_questQuery);
        if (ok) {
            m_queries.onQuest(m_questQuery);
        }
        break;

    // Phase 13 (combat): fear, confusion, charms and possessions (game/MoverControl).
    case Opcode::SMSG_CLIENT_CONTROL_UPDATE: ok = m_control.onControlUpdate(p, nowMs); break;
    case Opcode::SMSG_CLEAR_FAR_SIGHT_IMMEDIATE: m_control.clearFarSightImmediate(nowMs); break; // phase 18 (motion)

    // Melee.
    case Opcode::SMSG_ATTACKSTART: {
        net::AttackStart m;
        ok = net::parseAttackStart(p, m);
        if (ok) {
            fx(FxEventKind::AttackStart, m.attacker, 0, m.victim);
            if (m_melee.onAttackStart(m, m_player)) {
                event(GameEventKind::AttackStarted, 0, m.victim);
                setSheath(kSheathMelee); // the server started it (Charge, a melee spell): the weapons come out too
            }
        }
        break;
    }
    case Opcode::SMSG_ATTACKSTOP: {
        net::AttackStop m;
        ok = net::parseAttackStop(p, m);
        if (ok) {
            fx(FxEventKind::AttackStop, m.attacker, 0, m.victim).value[0] = m.dead ? 1 : 0;
            if (m_melee.onAttackStop(m, m_player)) {
                event(GameEventKind::AttackStopped, 0, m.victim);
            }
        }
        break;
    }
    case Opcode::SMSG_CANCEL_COMBAT:
        if (m_melee.attacking()) {
            const u64 victim = m_melee.target();
            m_melee.stop();
            event(GameEventKind::AttackStopped, 0, victim);
        }
        break;
    case Opcode::SMSG_ATTACKSWING_NOTINRANGE:
        event(GameEventKind::SwingNotInRange, 0, m_melee.target());
        break;
    case Opcode::SMSG_ATTACKSWING_BADFACING: event(GameEventKind::SwingBadFacing, 0, m_melee.target()); break;
    case Opcode::SMSG_ATTACKSWING_NOTSTANDING:
        event(GameEventKind::SwingNotStanding, 0, m_melee.target());
        break;
    case Opcode::SMSG_ATTACKSWING_DEADTARGET:
        event(GameEventKind::SwingDeadTarget, 0, m_melee.target());
        break;
    case Opcode::SMSG_ATTACKSWING_CANT_ATTACK:
        event(GameEventKind::SwingCantAttack, 0, m_melee.target());
        break;

    // Combat log.
    case Opcode::SMSG_ATTACKERSTATEUPDATE:
        ok = net::parseAttackerStateUpdate(p, m_attackerState);
        if (ok) {
            m_combat.onAttackerStateUpdate(m_attackerState, nowMs);
        }
        break;
    case Opcode::SMSG_SPELLNONMELEEDAMAGELOG:
        ok = net::parseSpellDamageLog(p, m_spellDamage);
        if (ok) {
            m_combat.onSpellDamage(m_spellDamage, nowMs);
        }
        break;
    case Opcode::SMSG_SPELLHEALLOG: {
        net::SpellHealLog m;
        ok = net::parseSpellHealLog(p, m);
        if (ok) {
            m_combat.onSpellHeal(m, nowMs);
        }
        break;
    }
    case Opcode::SMSG_SPELLENERGIZELOG: {
        net::SpellEnergizeLog m;
        ok = net::parseSpellEnergizeLog(p, m);
        if (ok) {
            m_combat.onSpellEnergize(m, nowMs);
        }
        break;
    }
    case Opcode::SMSG_PERIODICAURALOG:
        ok = net::parsePeriodicAuraLog(p, m_periodic);
        if (ok) {
            m_combat.onPeriodicAura(m_periodic, nowMs);
        }
        break;
    case Opcode::SMSG_SPELLLOGMISS:
        ok = net::parseSpellLogMiss(p, m_logMiss);
        if (ok) {
            m_combat.onSpellLogMiss(m_logMiss, nowMs);
        }
        break;
    case Opcode::SMSG_SPELLORDAMAGE_IMMUNE:
    case Opcode::SMSG_PROCRESIST: {
        net::SpellImmuneLog m;
        ok = net::parseSpellImmuneLog(p, m);
        if (ok && opcode == Opcode::SMSG_PROCRESIST) {
            m_combat.onProcResist(m, nowMs);
        } else if (ok) {
            m_combat.onImmune(m, nowMs);
        }
        break;
    }
    case Opcode::SMSG_ENVIRONMENTALDAMAGELOG: {
        net::EnvironmentalDamageLog m;
        ok = net::parseEnvironmentalDamageLog(p, m);
        if (ok) {
            m_combat.onEnvironmental(m, nowMs);
        }
        break;
    }
    // Phase 13 (combat): the rest of WoW.exe's UnitCombatLog_C packets.
    case Opcode::SMSG_SPELLLOGEXECUTE:
        ok = net::parseSpellLogExecute(p, m_logExecute);
        if (ok) {
            ExecuteLogSpells spells;
            spells.context = this;
            spells.of = [](const void* self, u32 id) { return static_cast<const Gameplay*>(self)->executeLogSpell(id); };
            m_combat.onSpellLogExecute(m_logExecute, spells, nowMs);
        }
        break;
    case Opcode::SMSG_SPELLDAMAGESHIELD: {
        net::SpellDamageShield m;
        ok = net::parseSpellDamageShield(p, m);
        if (ok) {
            m_combat.onDamageShield(m, nowMs);
        }
        break;
    }
    case Opcode::SMSG_SPELLDISPELLOG: {
        net::SpellDispelLog m;
        ok = net::parseSpellDispelLog(p, m);
        if (ok) {
            m_combat.onDispelLog(m, nowMs);
        }
        break;
    }
    case Opcode::SMSG_DISPEL_FAILED: {
        net::DispelFailed m;
        ok = net::parseDispelFailed(p, m);
        if (ok) {
            m_combat.onDispelFailed(m, nowMs);
        }
        break;
    }
    case Opcode::SMSG_SPELLINSTAKILLLOG: {
        net::SpellInstaKillLog m;
        ok = net::parseSpellInstaKillLog(p, m);
        if (ok) {
            m_combat.onInstaKill(m, nowMs);
        }
        break;
    }
    case Opcode::SMSG_ENCHANTMENTLOG: {
        net::EnchantmentLog m;
        ok = net::parseEnchantmentLog(p, m);
        if (ok) {
            m_combat.onEnchantmentLog(m, nowMs);
        }
        break;
    }
    case Opcode::SMSG_FEIGN_DEATH_RESISTED: event(GameEventKind::FeignDeathResisted); break;
    case Opcode::SMSG_PARTYKILLLOG: {
        net::PartyKillLog m;
        ok = net::parsePartyKillLog(p, m);
        if (ok) {
            event(GameEventKind::PartyKill, 0, m.victim).other = m.killer;
        }
        break;
    }

    // Spellbook, action bar, casts, cooldowns, auras.
    case Opcode::SMSG_INITIAL_SPELLS:
        ok = net::parseInitialSpells(p, m_initialSpells);
        if (ok) {
            m_spells.onInitialSpells(m_initialSpells, nowMs);
        }
        break;
    case Opcode::SMSG_LEARNED_SPELL: {
        u32 spellId = 0;
        ok = net::parseLearnedSpell(p, spellId);
        if (ok) {
            m_spells.onLearned(spellId);
            event(GameEventKind::SpellLearned, spellId);
        }
        break;
    }
    case Opcode::SMSG_REMOVED_SPELL: {
        u32 spellId = 0;
        ok = net::parseRemovedSpell(p, spellId);
        if (ok) {
            m_spells.onRemoved(spellId);
            event(GameEventKind::SpellRemoved, spellId);
        }
        break;
    }
    case Opcode::SMSG_SUPERCEDED_SPELL: {
        u32 oldId = 0, newId = 0;
        ok = net::parseSupercededSpell(p, oldId, newId);
        if (ok) {
            // The new rank takes the old one's buttons; the server keeps the bars as the client tells it (phase 8).
            std::array<bool, net::kActionButtonCount> had{};
            for (u32 slot = 0; slot < net::kActionButtonCount; ++slot) {
                const ActionButton b = m_spells.button(slot);
                had[slot] = oldId != 0 && b.type == net::kActionSpell && b.action == oldId;
            }
            m_spells.onSuperceded(oldId, newId);
            for (u32 slot = 0; slot < net::kActionButtonCount; ++slot) {
                if (had[slot]) {
                    setActionButton(slot, {net::kActionSpell, newId});
                }
            }
        }
        break;
    }
    case Opcode::SMSG_ACTION_BUTTONS: {
        std::array<u32, net::kActionButtonCount> buttons{};
        ok = net::parseActionButtons(p, buttons);
        if (ok) {
            m_spells.onActionButtons(buttons);
        }
        break;
    }
    case Opcode::SMSG_SPELL_START:
        ok = net::parseSpellStart(p, m_spellStart);
        if (ok) {
            m_spells.onSpellStart(m_spellStart, m_player, nowMs);
            FxEvent& e = fx(FxEventKind::SpellStart, m_spellStart.unitCaster, m_spellStart.spellId);
            fxTargets(e, m_spellStart.targets);
            e.value[0] = static_cast<i32>(m_spellStart.castTimeMs);
        }
        break;
    case Opcode::SMSG_SPELL_GO:
        ok = net::parseSpellGo(p, m_spellGo);
        if (ok) {
            ItemSpellCooldown itemCooldown; // parity: an item's cast takes the item's cooldown (potions, Hearthstone)
            const bool fromItem = m_inventory.itemCastCooldown(m_spellGo, itemCooldown);
            m_spells.onSpellGo(m_spellGo, m_player, nowMs, fromItem ? &itemCooldown : nullptr);
            m_abilities.onSpellGo(m_spellGo, nowMs); // pet cooldowns
            m_tradeSkills.onSpellGo(m_spellGo, m_player); // phase 13: "Create All" goes on
            m_combat.onSpellGoMisses(m_spellGo, nowMs);
            FxEvent& e = fx(FxEventKind::SpellGo, m_spellGo.unitCaster, m_spellGo.spellId,
                            m_spellGo.hits.empty() ? 0 : m_spellGo.hits.front());
            fxTargets(e, m_spellGo.targets);
            e.value = {static_cast<i32>(m_spellGo.hits.size()), static_cast<i32>(m_spellGo.misses.size())};
            for (const u64 hit : m_spellGo.hits) {
                fx(FxEventKind::SpellHit, m_spellGo.unitCaster, m_spellGo.spellId, hit);
            }
        }
        break;
    case Opcode::SMSG_SPELL_FAILED_OTHER: {
        net::SpellFailedOther m;
        ok = net::parseSpellFailedOther(p, m);
        if (ok) {
            m_spells.onSpellFailedOther(m, m_player);
            fx(FxEventKind::SpellInterrupted, m.caster, m.spellId);
        }
        break;
    }
    case Opcode::SMSG_CAST_RESULT: {
        net::CastResult m;
        ok = net::parseCastResult(p, m);
        if (ok) {
            m_spells.onCastResult(m);
            m_abilities.onCastResult(m); // a refused auto-repeat spell
            if (m.status == net::kCastStatusFailed) {
                m_tradeSkills.onCastFailed(m.spellId); // phase 13: a refused recipe ends its repeat
            }
            if (m.status == net::kCastStatusFailed && m.reason != kCastFailedDontReport) {
                GameEvent& e = event(GameEventKind::CastFailed, m.spellId);
                e.value = {m.reason, static_cast<i32>(m.args[0]), static_cast<i32>(m.args[1])};
            }
        }
        break;
    }
    // Phase 13 (spells): the talents' and auras' spell modifiers.
    case Opcode::SMSG_SET_FLAT_SPELL_MODIFIER:
    case Opcode::SMSG_SET_PCT_SPELL_MODIFIER:
        ok = m_spellMods.handle(opcode == Opcode::SMSG_SET_FLAT_SPELL_MODIFIER, p);
        break;
    case Opcode::SMSG_SPELL_COOLDOWN:
        ok = net::parseSpellCooldown(p, m_spellCooldown);
        if (ok) {
            m_spells.onSpellCooldown(m_spellCooldown, m_player, nowMs);
        }
        break;
    case Opcode::SMSG_COOLDOWN_EVENT:
    case Opcode::SMSG_CLEAR_COOLDOWN: {
        u32 spellId = 0;
        u64 guid = 0;
        ok = net::parseCooldownEvent(p, spellId, guid);
        if (ok && guid == m_player) {
            if (opcode == Opcode::SMSG_COOLDOWN_EVENT) {
                m_spells.onCooldownEvent(spellId, nowMs);
            } else {
                m_spells.onClearCooldown(spellId);
            }
        }
        break;
    }
    case Opcode::SMSG_SPELL_DELAYED: {
        net::SpellDelayed m;
        ok = net::parseSpellDelayed(p, m);
        if (ok) {
            m_spells.onSpellDelayed(m, m_player);
        }
        break;
    }
    case Opcode::MSG_CHANNEL_START: {
        net::ChannelStart m;
        ok = net::parseChannelStart(p, m);
        if (ok) {
            m_spells.onChannelStart(m, m_player, nowMs);
            fx(FxEventKind::ChannelStart, m_player, m.spellId).value[0] = static_cast<i32>(m.durationMs);
        }
        break;
    }
    case Opcode::MSG_CHANNEL_UPDATE: {
        u32 remaining = 0;
        ok = net::parseChannelUpdate(p, remaining);
        if (ok) {
            const u32 channel = m_spells.cast().channel ? m_spells.cast().spellId : 0;
            m_spells.onChannelUpdate(remaining, nowMs);
            if (remaining == 0 && channel != 0) {
                fx(FxEventKind::ChannelEnd, m_player, channel);
            }
        }
        break;
    }
    case Opcode::SMSG_UPDATE_AURA_DURATION: {
        u8 slot = 0;
        u32 duration = 0;
        ok = net::parseUpdateAuraDuration(p, slot, duration);
        if (ok) {
            m_auraDurations.onDuration(slot, duration, nowMs);
        }
        break;
    }

    // Gossip and quest givers.
    case Opcode::SMSG_GOSSIP_MESSAGE:
        ok = net::parseGossipMessage(p, m_gossip);
        if (ok) {
            m_dialog.onGossipMessage(m_gossip);
            m_queries.npcText(m_gossip.textId, m_gossip.source);
        }
        break;
    case Opcode::SMSG_GOSSIP_COMPLETE: m_dialog.close(); break;
    case Opcode::SMSG_QUESTGIVER_STATUS: {
        u64 guid = 0;
        u32 status = 0;
        ok = net::parseQuestGiverStatus(p, guid, status);
        if (ok) {
            m_questGiverStatus[guid] = static_cast<net::QuestGiverStatus>(std::min<u32>(status, 0xFF));
        }
        break;
    }
    case Opcode::SMSG_QUESTGIVER_QUEST_LIST:
        ok = net::parseQuestGiverQuestList(p, m_questList);
        if (ok) {
            m_dialog.onQuestList(m_questList);
        }
        break;
    case Opcode::SMSG_QUESTGIVER_QUEST_DETAILS:
        ok = net::parseQuestDetails(p, m_questDetails);
        if (ok) {
            m_dialog.onQuestDetails(m_questDetails);
            queryItems({m_questDetails.choices.data(), m_questDetails.choiceCount});
            queryItems({m_questDetails.rewards.data(), m_questDetails.rewardCount});
        }
        break;
    case Opcode::SMSG_QUESTGIVER_REQUEST_ITEMS:
        ok = net::parseQuestRequestItems(p, m_requestItems);
        if (ok) {
            m_dialog.onRequestItems(m_requestItems);
            queryItems({m_requestItems.items.data(), m_requestItems.itemCount});
        }
        break;
    case Opcode::SMSG_QUESTGIVER_OFFER_REWARD:
        ok = net::parseQuestOfferReward(p, m_offerReward);
        if (ok) {
            m_dialog.onOfferReward(m_offerReward);
            queryItems({m_offerReward.choices.data(), m_offerReward.choiceCount});
            queryItems({m_offerReward.rewards.data(), m_offerReward.rewardCount});
        }
        break;
    case Opcode::SMSG_QUESTGIVER_QUEST_COMPLETE: {
        net::QuestComplete& m = m_questComplete; // kept: its items name the rewards (lastQuestComplete())
        ok = net::parseQuestComplete(p, m);
        if (ok) {
            GameEvent& e = event(GameEventKind::QuestRewarded, m.questId);
            e.value[0] = static_cast<i32>(m.xp);
            e.value[1] = m.money;
            m_dialog.close(); // a follow-up quest's details may come right after
            scheduleStatusRefresh(nowMs);
        }
        break;
    }
    case Opcode::SMSG_QUESTGIVER_QUEST_INVALID: {
        u32 reason = 0;
        ok = net::parseQuestGiverQuestInvalid(p, reason);
        if (ok) {
            event(GameEventKind::QuestInvalid).value[0] = static_cast<i32>(reason);
        }
        break;
    }
    case Opcode::SMSG_QUESTGIVER_QUEST_FAILED: {
        u32 questId = 0, reason = 0;
        ok = net::parseQuestGiverQuestFailed(p, questId, reason);
        if (ok) {
            event(GameEventKind::QuestGiverFailed, questId).value[0] = static_cast<i32>(reason);
        }
        break;
    }
    case Opcode::SMSG_QUESTLOG_FULL: event(GameEventKind::QuestLogFull); break;
    case Opcode::SMSG_QUESTUPDATE_ADD_KILL: {
        net::QuestUpdateAddKill m;
        ok = net::parseQuestUpdateAddKill(p, m);
        if (ok) {
            GameEvent& e = event(GameEventKind::QuestObjectiveProgress, m.questId, m.source);
            e.value = {static_cast<i32>(m.entry), static_cast<i32>(m.count), static_cast<i32>(m.required)};
            questWatchUpdate(m.questId); // phase 13 (quests)
        }
        break;
    }
    case Opcode::SMSG_QUESTUPDATE_ADD_ITEM: {
        u32 itemId = 0, added = 0;
        ok = net::parseQuestUpdateAddItem(p, itemId, added);
        if (ok) {
            // WoW.exe 0x5E5AD0 / 0x5DD060: the first quest of the log asking for the item (not failed, already
            // known) counts it, as the bags hold it plus what was added: vMaNGOS sends this before the item itself
            // (ItemAddedQuestCheck, then StoreItem; the item's fields come with the next object update). With the
            // bags already holding enough there is no line.
            const Entity* player = me();
            const u32 have = player ? inventoryCount(m_entities, *player, itemId) : 0;
            GameEvent& e = event(GameEventKind::QuestItemProgress, itemId);
            e.value[0] = static_cast<i32>(added);
            for (const QuestLogEntry& q : m_questLog.slots()) {
                const QuestInfo* info = q.questId != 0 && !q.failed() ? m_queries.findQuest(q.questId) : nullptr;
                u32 need = 0;
                for (u32 i = 0; info && i < net::kQuestObjectiveCount && need == 0; ++i) {
                    need = info->objectiveList[i].itemId == itemId ? info->objectiveList[i].itemCount : 0;
                }
                if (need > 0) {
                    e.value[1] = static_cast<i32>(std::min(have + added, need));
                    e.value[2] = have < need ? static_cast<i32>(need) : 0;
                    break;
                }
            }
            m_queries.item(itemId);
            // Phase 13 (quests): QUEST_WATCH_UPDATE for every quest of the log that asks for the item.
            for (const QuestLogEntry& q : m_questLog.slots()) {
                const QuestInfo* info = q.questId != 0 ? m_queries.findQuest(q.questId) : nullptr;
                for (u32 i = 0; info && i < net::kQuestObjectiveCount; ++i) {
                    if (info->objectiveList[i].itemId == itemId && info->objectiveList[i].itemCount > 0) {
                        questWatchUpdate(q.questId);
                        break;
                    }
                }
            }
        }
        break;
    }
    case Opcode::SMSG_QUESTUPDATE_COMPLETE:
    case Opcode::SMSG_QUESTUPDATE_FAILED:
    case Opcode::SMSG_QUESTUPDATE_FAILEDTIMER: {
        u32 questId = 0;
        ok = net::parseQuestId(p, questId);
        if (ok) {
            event(opcode == Opcode::SMSG_QUESTUPDATE_COMPLETE ? GameEventKind::QuestObjectivesDone
                                                              : GameEventKind::QuestFailed,
                  questId);
        }
        break;
    }
    case Opcode::SMSG_QUEST_CONFIRM_ACCEPT: {
        net::QuestConfirmAccept m;
        ok = net::parseQuestConfirmAccept(p, m);
        if (ok) {
            event(GameEventKind::QuestShared, m.questId, m.sharer);
        }
        break;
    }
    case Opcode::MSG_QUEST_PUSH_RESULT: { // phase 13 (quests): how sharing went with a party member
        net::QuestPushResult m;
        ok = net::parseQuestPushResult(p, m);
        if (ok) {
            event(GameEventKind::QuestPushResult, 0, m.guid).value[0] = m.result;
            m_queries.playerName(m.guid); // the line names the member
        }
        break;
    }

    // Loot and items.
    case Opcode::SMSG_LOOT_RESPONSE:
        ok = net::parseLootResponse(p, m_lootResponse);
        if (ok) {
            if (m_loot.onResponse(m_lootResponse)) {
                for (const net::LootResponse::Item& item : m_lootResponse.items) {
                    m_queries.item(item.itemId); // names and icons for the window
                }
            } else {
                event(GameEventKind::LootError, 0, m_lootResponse.guid).value[0] = m_lootResponse.error;
            }
        }
        break;
    case Opcode::SMSG_LOOT_RELEASE_RESPONSE: {
        u64 guid = 0;
        ok = net::parseLootReleaseResponse(p, guid);
        if (ok) {
            m_loot.onReleased(guid);
            m_moneyRequested = false;
        }
        break;
    }
    case Opcode::SMSG_LOOT_REMOVED: {
        u8 slot = 0;
        ok = net::parseLootRemoved(p, slot);
        if (ok) {
            m_loot.onRemoved(slot);
        }
        break;
    }
    case Opcode::SMSG_LOOT_CLEAR_MONEY:
        m_loot.onMoneyCleared();
        if (m_moneyRequested) {
            m_moneyRequested = false;
            m_expectMoney = true; // the share arrives as a notify (groups) or a coinage change (solo)
            m_expectMoneyMs = nowMs;
        }
        break;
    case Opcode::SMSG_LOOT_MONEY_NOTIFY: {
        u32 amount = 0;
        ok = net::parseU32(p, amount);
        if (ok) {
            m_expectMoney = false;
            GameEvent& e = event(GameEventKind::MoneyLooted);
            e.value[0] = static_cast<i32>(amount);
            e.value[1] = 1; // the group's share
        }
        break;
    }
    case Opcode::SMSG_ITEM_PUSH_RESULT: {
        net::ItemPushResult m;
        ok = net::parseItemPushResult(p, m);
        if (ok && m.player == m_player) { // group loot is broadcast
            GameEvent& e = event(GameEventKind::ItemReceived, m.itemId);
            e.value = {static_cast<i32>(m.count), m.fromNpc ? 1 : 0, m.created ? 1 : 0};
            e.other = m.showInChat ? 0 : 1;
            m_queries.item(m.itemId);
        } else if (ok && m.showInChat) { // a group member's loot ("Aeryn receives loot: ...")
            GameEvent& e = event(GameEventKind::MemberItemReceived, m.itemId, m.player);
            e.value = {static_cast<i32>(m.count), 0, m.created ? 1 : 0};
            m_queries.item(m.itemId);
        }
        break;
    }
    case Opcode::SMSG_INVENTORY_CHANGE_FAILURE: {
        net::InventoryChangeFailure m;
        ok = net::parseInventoryChangeFailure(p, m);
        if (ok && m.result != 0) {
            GameEvent& e = event(GameEventKind::InventoryError);
            e.value[0] = m.result;
            e.value[1] = static_cast<i32>(m.requiredLevel);
        }
        break;
    }

    // Vendors, bank, books, item cooldowns and proficiencies (phase 8): game/Inventory.
    case Opcode::SMSG_LIST_INVENTORY:
    case Opcode::SMSG_SHOW_BANK:
        m_dialog.close(); // the vendor or bank replaces the gossip menu that led there
        [[fallthrough]];
    case Opcode::SMSG_BUY_ITEM:
    case Opcode::SMSG_BUY_FAILED:
    case Opcode::SMSG_SELL_ITEM:
    case Opcode::SMSG_BUY_BANK_SLOT_RESULT:
    case Opcode::SMSG_READ_ITEM_OK:
    case Opcode::SMSG_READ_ITEM_FAILED:
    case Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE:
    case Opcode::SMSG_ITEM_TEXT_QUERY_RESPONSE:
    case Opcode::SMSG_DURABILITY_DAMAGE_DEATH:
    case Opcode::SMSG_ITEM_COOLDOWN:
    case Opcode::SMSG_SET_PROFICIENCY:
    case Opcode::SMSG_OPEN_CONTAINER: ok = m_inventory.handle(opcode, p, nowMs); break;
    case Opcode::SMSG_ITEM_TIME_UPDATE: ok = m_inventory.handle(opcode, p, nowMs); break; // phase 13
    case Opcode::SMSG_ITEM_ENCHANT_TIME_UPDATE: // parity: BuffFrame's TempEnchant timers
        ok = m_enchantTimers.handle(p, nowMs) && m_inventory.handle(opcode, p, nowMs);
        break;

    // Experience.
    case Opcode::SMSG_LOG_XPGAIN: {
        net::XpGain m;
        ok = net::parseXpGain(p, m);
        if (ok) {
            // WoW.exe 0x637EC0: the player as victim is experience lost; a kill names its victim only when
            // the client knows it as a unit (else, and for quests and exploration, the UNNAMED lines).
            const Entity* victim = m.kill && m.victim != 0 ? m_entities.find(m.victim) : nullptr;
            const bool unit =
                victim && (victim->type == ObjectType::Unit || victim->type == ObjectType::Player);
            const bool loss = m.victim != 0 && m.victim == m_player;
            GameEvent& e = event(GameEventKind::XpGained, loss ? 1 : 0, unit && !loss ? m.victim : 0);
            e.value[0] = static_cast<i32>(m.totalXp);
            e.value[1] = static_cast<i32>(m.baseXp);
            e.value[2] = std::bit_cast<i32>(m.groupBonus);
        }
        break;
    }
    case Opcode::SMSG_LEVELUP_INFO:
        ok = net::parseLevelUpInfo(p, m_levelUp);
        if (ok) {
            // Sent before the level field changes (Player::GiveLevel): the level is still the one before.
            const Entity* player = me();
            GameEvent& e = event(GameEventKind::LevelUp, player ? player->value(uf::UNIT_FIELD_LEVEL) : 0);
            e.value = {static_cast<i32>(m_levelUp.level), static_cast<i32>(m_levelUp.healthGain),
                       static_cast<i32>(m_levelUp.powerGains[0])};
            scheduleStatusRefresh(nowMs); // quests of the new level become available
        }
        break;

    // Death.
    case Opcode::SMSG_CORPSE_RECLAIM_DELAY: {
        u32 delay = 0;
        ok = net::parseU32(p, delay);
        if (ok) {
            m_death.reclaimAtMs = nowMs + delay;
        }
        break;
    }
    case Opcode::MSG_CORPSE_QUERY: {
        net::CorpseQuery m;
        ok = net::parseCorpseQuery(p, m);
        if (ok) {
            m_death.corpseKnown = m.found;
            m_death.corpseMap = m.mapId;
            m_death.corpseX = m.x;
            m_death.corpseY = m.y;
            m_death.corpseZ = m.z;
            m_death.corpseRealMap = m.corpseMapId;
            if (m.found) {
                event(GameEventKind::CorpseLocated);
            }
        }
        break;
    }
    case Opcode::SMSG_RESURRECT_REQUEST: {
        net::ResurrectRequest m;
        ok = net::parseResurrectRequest(p, m);
        if (ok) {
            m_death.resurrector = m.resurrector;
            m_death.resurrectorName.assign(m.name);
            m_death.resurrectSickness = m.sickness;
            m_death.resurrectDelayed = m.delayed; // parity (popups)
            if (isPlayerGuid(m.resurrector)) {
                m_queries.playerName(m.resurrector);
            }
            event(GameEventKind::ResurrectRequest, 0, m.resurrector);
        }
        break;
    }
    case Opcode::SMSG_SPIRIT_HEALER_CONFIRM: {
        u64 guid = 0;
        ok = net::parseGuid(p, guid);
        if (ok) {
            m_death.spiritHealer = guid;
            event(GameEventKind::SpiritHealerConfirm, 0, guid);
        }
        break;
    }
    case Opcode::SMSG_STANDSTATE_UPDATE: ok = net::parseStandStateUpdate(p, m_standState); break;

    // Chat.
    case Opcode::SMSG_MESSAGECHAT:
        ok = net::parseChatMessage(p, m_chatMessage);
        if (ok) {
            addChatMessage(m_chatMessage, nowMs); // phase 18 (extras): B4, the languages
            if (m_chatMessage.senderName.empty() && isPlayerGuid(m_chatMessage.sender)) {
                m_queries.playerName(m_chatMessage.sender); // players' names never travel with their lines
            }
        }
        break;
    case Opcode::SMSG_NOTIFICATION:
    case Opcode::SMSG_CHAT_PLAYER_NOT_FOUND: {
        std::string_view text;
        ok = net::parseCString(p, text);
        if (ok) {
            m_chat.addNotice(opcode == Opcode::SMSG_NOTIFICATION ? ChatSource::Notification
                                                                 : ChatSource::PlayerNotFound,
                             text, nowMs);
        }
        break;
    }
    case Opcode::SMSG_AREA_TRIGGER_MESSAGE: {
        std::string_view text;
        ok = net::parseAreaTriggerMessage(p, text);
        if (ok) {
            m_chat.addNotice(ChatSource::AreaTrigger, text, nowMs);
        }
        break;
    }
    case Opcode::SMSG_CHAT_WRONG_FACTION: m_chat.addNotice(ChatSource::WrongFaction, {}, nowMs); break;
    case Opcode::SMSG_CHAT_RESTRICTED: m_chat.addNotice(ChatSource::Restricted, {}, nowMs); break;
    // Phase 13 (lead): the shutdown / restart countdown. WoW.exe words it with ServerMessages.dbc's row of the type
    // ("[SERVER] Shutdown in %s", the text as its argument; "[%d]: %s" for a type it does not know) and adds it to
    // the chat as a system line (CHAT_MSG_SYSTEM), nowhere else.
    case Opcode::SMSG_SERVER_MESSAGE:
        ok = net::parseServerMessage(p, m_serverMessage);
        if (ok) {
            m_chat.addNotice(ChatSource::ServerMessage, serverMessageText(m_serverMessage), nowMs);
        }
        break;
    // Mounting failures (Player::Mount / Unmount): the client's ERR_MOUNT_* / ERR_DISMOUNT_* error line.
    case Opcode::SMSG_MOUNTRESULT:
    case Opcode::SMSG_DISMOUNTRESULT: {
        u32 result = 0;
        ok = net::parseMountResult(p, result);
        const bool dismount = opcode == Opcode::SMSG_DISMOUNTRESULT;
        if (ok && result != (dismount ? net::kDismountResultOk : net::kMountResultOk)) {
            GameEvent& e = event(GameEventKind::MountFailed);
            e.value[0] = static_cast<i32>(result);
            e.value[1] = dismount ? 1 : 0;
        }
        break;
    }
    // A creature noticed the player or attacks (its alert and aggro sounds); the others are not used by 1.12.
    case Opcode::SMSG_AI_REACTION: {
        net::AiReaction m;
        ok = net::parseAiReaction(p, m);
        if (ok && (m.reaction == net::kAiReactionAlert || m.reaction == net::kAiReactionHostile)) {
            fx(FxEventKind::AiReaction, m.guid, m.reaction);
        }
        break;
    }
    // Forced reactions (SPELL_AURA_FORCE_REACTION): game::Reactions reads them through standing().
    case Opcode::SMSG_SET_FORCED_REACTIONS:
        ok = net::parseForcedReactions(p, m_forcedReactions);
        if (ok) {
            m_standing.forced.clear();
            for (const net::ForcedReaction& f : m_forcedReactions) {
                m_standing.forced.push_back({f.faction, f.rank});
            }
        }
        break;
    // A new character's intro (Player::SendCinematicStart) or a camera object's view: the server moves the player's
    // view point along the cinematic's path, so nothing around the player is sent until CMSG_COMPLETE_CINEMATIC.
    // Phase 14: played by the client (app/Cinematics), which completes it.
    case Opcode::SMSG_TRIGGER_CINEMATIC:
        ok = p.size() >= 4;
        if (ok) {
            u32 sequence = 0;
            std::memcpy(&sequence, p.data(), 4);
            m_cinematic = sequence;
            if (skipCinematics || sequence == 0) {
                completeCinematic();
            }
        }
        break;
    case Opcode::SMSG_TEXT_EMOTE: {
        net::TextEmote m;
        ok = net::parseTextEmote(p, m);
        if (ok) {
            m_chat.addTextEmote(m, nowMs);
            if (isPlayerGuid(m.guid)) {
                m_queries.playerName(m.guid);
            }
            fx(FxEventKind::TextEmote, m.guid, m.textEmote).value[0] = static_cast<i32>(m.emoteNum);
        }
        break;
    }

    // Visuals and sounds (phase 9: only recorded here, see FxEventKind).
    case Opcode::SMSG_PLAY_SPELL_VISUAL:
    case Opcode::SMSG_PLAY_SPELL_IMPACT: {
        u64 guid = 0;
        u32 kit = 0;
        ok = net::parseSpellVisualKit(p, guid, kit);
        if (ok) {
            fx(opcode == Opcode::SMSG_PLAY_SPELL_VISUAL ? FxEventKind::SpellVisual : FxEventKind::SpellImpact, guid,
               kit);
        }
        break;
    }
    case Opcode::SMSG_SPELL_UPDATE_CHAIN_TARGETS: { // phase 13 (combat): the jumps, for the beams of F14.4
        net::SpellChainTargets m;
        ok = net::parseSpellChainTargets(p, m);
        for (u32 i = 0; ok && i < m.count; ++i) {
            FxEvent& e = fx(FxEventKind::ChainTarget, m.caster, m.spellId, m.targets[i]);
            e.value = {static_cast<i32>(i), static_cast<i32>(m.count)};
        }
        break;
    }
    case Opcode::SMSG_PLAY_SOUND:
    case Opcode::SMSG_PLAY_MUSIC: {
        u32 sound = 0;
        ok = net::parseSoundId(p, sound);
        if (ok) {
            fx(opcode == Opcode::SMSG_PLAY_SOUND ? FxEventKind::Sound : FxEventKind::Music, 0, sound);
        }
        break;
    }
    case Opcode::SMSG_PLAY_OBJECT_SOUND:
    case Opcode::SMSG_EMOTE: {
        u32 id = 0;
        u64 guid = 0;
        ok = net::parseIdAndGuid(p, id, guid);
        if (ok) {
            fx(opcode == Opcode::SMSG_EMOTE ? FxEventKind::Emote : FxEventKind::ObjectSound, guid, id);
        }
        break;
    }

    // Phase 13 (quests): living world objects, fishing and readable objects.
    case Opcode::SMSG_GAMEOBJECT_CUSTOM_ANIM: {
        net::GameObjectCustomAnim m;
        ok = net::parseGameObjectCustomAnim(p, m);
        if (ok) {
            fx(FxEventKind::ObjectCustomAnim, m.guid, m.anim);
        }
        break;
    }
    case Opcode::SMSG_GAMEOBJECT_SPAWN_ANIM:
    case Opcode::SMSG_GAMEOBJECT_DESPAWN_ANIM:
    case Opcode::SMSG_GAMEOBJECT_RESET_STATE: {
        u64 guid = 0;
        ok = net::parseGuid(p, guid);
        if (ok) {
            fx(opcode == Opcode::SMSG_GAMEOBJECT_SPAWN_ANIM     ? FxEventKind::ObjectSpawnAnim
               : opcode == Opcode::SMSG_GAMEOBJECT_DESPAWN_ANIM ? FxEventKind::ObjectDespawnAnim
                                                                : FxEventKind::ObjectResetState,
               guid);
        }
        break;
    }
    case Opcode::SMSG_GAMEOBJECT_PAGETEXT: { // a goober with a page (GameObject::Use): the client reads it
        u64 guid = 0;
        ok = net::parseGuid(p, guid);
        if (ok) {
            readObject(guid);
        }
        break;
    }
    case Opcode::SMSG_FISH_NOT_HOOKED: event(GameEventKind::FishNotHooked); break;
    case Opcode::SMSG_FISH_ESCAPED: event(GameEventKind::FishEscaped); break;

    // Phase 8: trainers, talents, pets, reputation, auto repeat (game/Abilities).
    case Opcode::SMSG_TRAINER_LIST:
    case Opcode::SMSG_TRAINER_BUY_SUCCEEDED:
    case Opcode::SMSG_TRAINER_BUY_FAILED:
    case Opcode::MSG_TALENT_WIPE_CONFIRM:
    case Opcode::SMSG_PET_SPELLS:
    case Opcode::SMSG_PET_MODE:
    case Opcode::SMSG_PET_ACTION_FEEDBACK:
    case Opcode::SMSG_PET_CAST_FAILED:
    case Opcode::SMSG_PET_TAME_FAILURE:
    case Opcode::SMSG_PET_NAME_QUERY_RESPONSE:
    case Opcode::SMSG_PET_BROKEN:
    case Opcode::SMSG_INITIALIZE_FACTIONS:
    case Opcode::SMSG_SET_FACTION_STANDING:
    case Opcode::SMSG_SET_FACTION_VISIBLE:
    case Opcode::SMSG_SET_FACTION_ATWAR: // phase 13
    case Opcode::SMSG_CANCEL_AUTO_REPEAT: ok = m_abilities.handle(opcode, p, nowMs); break;
    // Phase 15 (pets): the stable master's list and results, a pet trainer's untraining offer (game/PetStable).
    case Opcode::MSG_LIST_STABLED_PETS:
        m_dialog.close(); // the stable replaces the gossip menu that led there
        [[fallthrough]];
    case Opcode::SMSG_STABLE_RESULT:
    case Opcode::SMSG_PET_UNLEARN_CONFIRM: ok = m_stable.handle(opcode, p, nowMs); break;
    // Travel (phase 8): exploration, flight masters, innkeepers, far teleports.
    case Opcode::SMSG_EXPLORATION_EXPERIENCE:
    case Opcode::SMSG_SHOWTAXINODES:
    case Opcode::SMSG_TAXINODE_STATUS:
    case Opcode::SMSG_ACTIVATETAXIREPLY:
    case Opcode::SMSG_NEW_TAXI_PATH:
    case Opcode::SMSG_BINDER_CONFIRM:
    case Opcode::SMSG_BINDPOINTUPDATE:
    case Opcode::SMSG_PLAYERBOUND:
    case Opcode::SMSG_TRANSFER_PENDING:
    case Opcode::SMSG_TRANSFER_ABORTED:
    // Phase 12: instance saves, resets, raid info and the instance boot.
    case Opcode::SMSG_INSTANCE_SAVE_CREATED:
    case Opcode::SMSG_UPDATE_INSTANCE_OWNERSHIP:
    case Opcode::SMSG_UPDATE_LAST_INSTANCE:
    case Opcode::SMSG_RAID_INSTANCE_MESSAGE:
    case Opcode::SMSG_INSTANCE_RESET:
    case Opcode::SMSG_INSTANCE_RESET_FAILED:
    case Opcode::SMSG_RAID_INSTANCE_INFO:
    case Opcode::SMSG_RAID_GROUP_ONLY: ok = m_travel.handle(opcode, p, nowMs); break;
    // Parity: breath, fatigue and feign death bars (game/MirrorTimers).
    case Opcode::SMSG_START_MIRROR_TIMER:
    case Opcode::SMSG_PAUSE_MIRROR_TIMER:
    case Opcode::SMSG_STOP_MIRROR_TIMER: ok = m_mirrorTimers.handle(opcode, p, nowMs); break;
    // Parity (popups): logging out, summons.
    case Opcode::SMSG_LOGOUT_RESPONSE:
    case Opcode::SMSG_LOGOUT_CANCEL_ACK: ok = m_logout.handle(opcode, p, nowMs); break;
    case Opcode::SMSG_SUMMON_REQUEST: ok = m_summon.handle(p, nowMs); break;
    case Opcode::SMSG_WEATHER: ok = m_weather.handle(p); break; // phase 11

    default: return Handled::No;
    }
    return ok ? Handled::Yes : Handled::Malformed;
}

// -------------------------------------------------------------------------------------------------------
// Targeting and melee.

void Gameplay::setTarget(u64 guid) {
    if (guid == m_target) {
        return;
    }
    m_target = guid;
    send(Opcode::CMSG_SET_SELECTION, guid);
}

bool Gameplay::startAttack(u64 guid) {
    if (!m_inWorld || guid == 0 || guid == m_player) {
        return false;
    }
    setTarget(guid);
    if (!send(Opcode::CMSG_ATTACKSWING, guid)) {
        return false;
    }
    m_melee.requested(guid);
    setSheath(kSheathMelee);
    return true;
}

void Gameplay::stopAttack() {
    send(Opcode::CMSG_ATTACKSTOP);
    m_melee.stop();
}

void Gameplay::nextCinematicCamera() {
    if (m_cinematic != 0) {
        send(Opcode::CMSG_NEXT_CINEMATIC_CAMERA);
    }
}

void Gameplay::completeCinematic() {
    send(Opcode::CMSG_COMPLETE_CINEMATIC);
    m_cinematic = 0;
}

// -------------------------------------------------------------------------------------------------------
// Spells.

bool Gameplay::cast(u32 spellId, const net::SpellCastTargets& targets) {
    if (!m_inWorld || spellId == 0) {
        return false;
    }
    m_body.clear();
    net::writeCastSpell(m_body, spellId, targets);
    if (!sendBody(Opcode::CMSG_CAST_SPELL)) {
        return false;
    }
    m_spells.onCastRequested(spellId, m_nowMs);
    return true;
}

bool Gameplay::castSpell(u32 spellId, u64 target) {
    // Phase 13 (professions): a profession's spell opens its window instead (Spell_C_CastSpell sends nothing).
    if (m_inWorld && m_tradeSkills.openBySpell(spellId)) {
        return true;
    }
    // Phase 13 (spells): a spell aimed at a place or an item waits for it (the client's targeting cursor); any other
    // cast replaces what was waiting (CURRENT_SPELL_CAST_CHANGED).
    SpellInfo info;
    if (m_inWorld && m_spellSource.get(spellId, info)) {
        if (const SpellAim aim = spellAim(info); aim != SpellAim::None) {
            startTargeting(info, aim, nullptr, 0);
            return true;
        }
    }
    // Phase 15 (options): without AUTO_SELF_CAST, a helpful spell (or one for any unit) with no unit to land on is not
    // cast (spellTarget gives 0 then): "No target", or "Invalid target" with a target that does not fit. 1.12 waits
    // for a unit with its targeting cursor; on touch that wait is not kept.
    if (m_inWorld && !autoSelfCast && target == 0 && info.id == spellId &&
        (info.targetKind() == SpellTargetKind::Friend || info.targetKind() == SpellTargetKind::AnyUnit)) {
        GameEvent& e = event(GameEventKind::CastFailed, spellId);
        e.value = {static_cast<i32>(m_target == 0 ? kCastFailedNoTarget : kCastFailedBadTargets), 0, 0};
        return false;
    }
    if (m_targeting.active()) {
        stopTargeting();
    }
    net::SpellCastTargets t;
    if (target != 0 && target != m_player) {
        t.mask = net::kTargetUnit;
        t.unit = target;
    }
    return cast(spellId, t);
}

bool Gameplay::castSpellOnObject(u32 spellId, u64 gameObject) {
    net::SpellCastTargets t;
    t.mask = net::kTargetGameObject;
    t.gameObject = gameObject;
    return cast(spellId, t);
}

bool Gameplay::castSpellAt(u32 spellId, f32 x, f32 y, f32 z) {
    net::SpellCastTargets t;
    t.mask = net::kTargetDestLocation;
    t.destination = {x, y, z};
    return cast(spellId, t);
}

u64 Gameplay::spellTarget(u32 spellId) const {
    SpellInfo info;
    if (!m_spellSource.get(spellId, info)) {
        return m_target; // unknown: the server decides
    }
    const Entity* player = me();
    const Entity* target = m_target ? m_entities.find(m_target) : nullptr;
    // Helpful spells land on a friendly target, else on the player (the client's auto self cast): the action
    // bars' range uses the same rule (game/ActionBar).
    const bool friendly = target && player &&
                          (target == player || reactionTo(*player, *target, m_reactions) == Reaction::Friendly);
    // Phase 15 (options): AUTO_SELF_CAST off, nothing fitting is 0 (castSpell refuses it then).
    if (!autoSelfCast && ((info.targetKind() == SpellTargetKind::Friend && !friendly) ||
                          (info.targetKind() == SpellTargetKind::AnyUnit && m_target == 0))) {
        return 0;
    }
    return spellLandsOn(info.targetKind(), m_player, m_target, friendly);
}

void Gameplay::cancelCast() {
    const CastBar& c = m_spells.cast();
    if (!c.active()) {
        return;
    }
    m_body.clear();
    net::writeU32(m_body, c.spellId);
    sendBody(c.channel ? Opcode::CMSG_CANCEL_CHANNELLING : Opcode::CMSG_CANCEL_CAST);
    m_spells.clearCast();
    m_tradeSkills.onCastCancelled(); // phase 13: the player stops a repeated recipe
}

void Gameplay::cancelAura(u32 spellId) {
    m_body.clear();
    net::writeU32(m_body, spellId);
    sendBody(Opcode::CMSG_CANCEL_AURA);
    m_areaSpiritHealer.onAuraCancelled(spellId); // phase 17 (honor): Waiting to Resurrect: AREA_SPIRIT_HEAL
}

bool Gameplay::useAction(u32 slot) {
    const ActionButton b = m_spells.button(slot);
    if (b.empty()) {
        return false;
    }
    if (b.type == net::kActionSpell) {
        return castSpell(b.action, spellTarget(b.action));
    }
    if (b.type == net::kActionItem) {
        return useItem(b.action);
    }
    return false; // macros: not supported
}

void Gameplay::setActionButton(u32 slot, ActionButton button) {
    if (slot >= net::kActionButtonCount) {
        return;
    }
    m_spells.setButton(slot, button);
    m_body.clear();
    net::writeSetActionButton(m_body, static_cast<u8>(slot), button.packed());
    sendBody(Opcode::CMSG_SET_ACTION_BUTTON);
}

bool Gameplay::useItem(u32 itemEntry, u64 target) {
    const Entity* player = me();
    InventoryItem item;
    if (!m_inWorld || !player) {
        return false;
    }
    if (findInventoryItem(m_entities, *player, itemEntry, item)) {
        return useItemAt({item.bag, item.slot}, target);
    }
    // Parity pass: a worn item too (an on-use trinket on the action bar).
    m_inventory.refresh(m_nowMs, true);
    for (u8 slot = 0; slot < kEquipmentSlotEnd; ++slot) {
        const ItemInstance* worn = m_inventory.equipped(slot);
        if (worn && worn->entry == itemEntry) {
            return useItemAt({kInventoryBackpack, slot}, target);
        }
    }
    return false;
}

bool Gameplay::useItemAt(ItemPos pos, u64 target) {
    m_inventory.refresh(m_nowMs, true);
    const ItemInstance* stack = m_inventory.at(pos);
    if (!m_inWorld || !stack || stack->entry == 0) {
        return false;
    }
    InventoryItem item;
    item.guid = stack->guid;
    item.bag = pos.bag;
    item.slot = pos.slot;
    const net::ItemQueryInfo* info = m_queries.item(stack->entry);
    if (!info) {
        return false; // asked for: its spells are known on the answer
    }
    for (u32 i = 0; i < info->spells.size(); ++i) {
        const net::ItemQueryInfo::Spell& spell = info->spells[i];
        if (spell.id == 0 || spell.trigger != net::kItemSpellTriggerOnUse) {
            continue;
        }
        // Phase 13 (spells): a bomb waits for its place, a poison or a sharpening stone for the weapon.
        if (SpellInfo aimed; target == 0 && m_spellSource.get(spell.id, aimed)) {
            if (const SpellAim aim = spellAim(aimed); aim != SpellAim::None) {
                item.entry = stack->entry;
                startTargeting(aimed, aim, &item, static_cast<u8>(i));
                return true;
            }
        }
        if (m_targeting.active()) {
            stopTargeting();
        }
        const u64 to = target ? target : spellTarget(spell.id);
        net::SpellCastTargets t;
        if (to != 0 && to != m_player) {
            t.mask = net::kTargetUnit;
            t.unit = to;
        }
        m_body.clear();
        net::writeUseItem(m_body, item.bag, item.slot, static_cast<u8>(i), t);
        return sendBody(Opcode::CMSG_USE_ITEM);
    }
    if (info->startQuest != 0) { // an item that starts a quest: its details window
        m_body.clear();
        net::writeQuestGiverQuest(m_body, item.guid, info->startQuest);
        return sendBody(Opcode::CMSG_QUESTGIVER_QUERY_QUEST);
    }
    if (info->pageText != 0 || stack->itemTextId != 0) { // books and letters: the reading window
        return m_inventory.read(pos);
    }
    if ((info->flags & kItemFlagLootable) || info->lockId != 0) { // clams, lockboxes: the loot window
        return m_inventory.open(pos);
    }
    if (info->flags & kItemFlagCharter) { // phase 15 (guild): a charter shows its signatures (PetitionFrame)
        return m_petitions.showCharter(item.guid);
    }
    return false;
}

// -------------------------------------------------------------------------------------------------------
// Phase 13 (spells): spells aimed at a place or an item.

void Gameplay::startTargeting(const SpellInfo& spell, SpellAim aim, const InventoryItem* item, u8 itemSpellIndex) {
    if (m_targeting.active()) {
        event(GameEventKind::SpellTargetingStopped, m_targeting.spellId());
    }
    if (item) {
        m_targeting.beginFromItem(spell, aim, {item->bag, item->slot}, item->guid, item->entry, itemSpellIndex);
    } else {
        m_targeting.begin(spell, aim);
    }
    event(GameEventKind::SpellTargetingStarted, spell.id).value[0] = static_cast<i32>(aim);
}

void Gameplay::stopTargeting() {
    if (!m_targeting.active()) {
        return;
    }
    const u32 spellId = m_targeting.spellId();
    m_targeting.stop();
    event(GameEventKind::SpellTargetingStopped, spellId);
}

Gameplay::TargetResult Gameplay::refuseTarget(u32 reason) {
    GameEvent& e = event(GameEventKind::CastFailed, m_targeting.spellId());
    e.value = {static_cast<i32>(reason), 0, 0};
    return TargetResult::Refused;
}

bool Gameplay::sendTargeting(const net::SpellCastTargets& targets) {
    bool sent = false;
    if (m_targeting.fromItem()) {
        const ItemPos pos = m_targeting.itemPos();
        m_body.clear();
        net::writeUseItem(m_body, pos.bag, pos.slot, m_targeting.itemSpellIndex(), targets);
        sent = sendBody(Opcode::CMSG_USE_ITEM);
    } else {
        sent = cast(m_targeting.spellId(), targets);
    }
    stopTargeting();
    return sent;
}

Gameplay::TargetResult Gameplay::targetLocation(f32 x, f32 y, f32 z) {
    const Entity* player = me();
    if (!m_inWorld || !player || !m_targeting.active() || m_targeting.aim() != SpellAim::Location) {
        return TargetResult::NotSent;
    }
    // Spell::CheckRange for a destination (game::placeRange).
    const u32 boundsBits = player->value(uf::UNIT_FIELD_BOUNDINGRADIUS);
    f32 bounds;
    std::memcpy(&bounds, &boundsBits, sizeof(bounds));
    switch (placeRange(m_targeting.spell(), &m_spellMods, player->positionAt(m_nowMs), bounds, {x, y, z})) {
    case PlaceRange::OutOfRange: return refuseTarget(kCastFailedOutOfRange);
    case PlaceRange::TooClose: return refuseTarget(kCastFailedTooClose);
    default: break;
    }
    net::SpellCastTargets t;
    t.mask = net::kTargetDestLocation;
    t.destination = {x, y, z};
    return sendTargeting(t) ? TargetResult::Sent : TargetResult::NotSent;
}

Gameplay::TargetResult Gameplay::targetItem(ItemPos pos) {
    if (!m_inWorld || !m_targeting.active() ||
        (m_targeting.aim() != SpellAim::Item && m_targeting.aim() != SpellAim::Locked)) {
        return TargetResult::NotSent;
    }
    m_inventory.refresh(m_nowMs, true);
    const ItemInstance* item = m_inventory.at(pos);
    const net::ItemQueryInfo* info = item && item->entry ? m_queries.item(item->entry) : nullptr;
    if (!item || !info) {
        return TargetResult::NotSent; // asked for: tapped again once known
    }
    const SpellInfo& spell = m_targeting.spell();
    if (itemFitsSpell(spell, *info, item) == ItemFit::Refused) {
        return refuseTarget(kCastFailedBadTargets);
    }
    const EnchantQuestion q = enchantQuestion(spell, *item, *info, false, m_gameData);
    if (q.ask != EnchantAsk::None) {
        m_targeting.ask(q, item->guid, false);
        GameEvent& e = event(GameEventKind::EnchantQuestion, spell.id, item->guid);
        e.value = {static_cast<i32>(q.ask), static_cast<i32>(q.oldEnchant), static_cast<i32>(q.newEnchant)};
        return TargetResult::Asked;
    }
    net::SpellCastTargets t;
    t.mask = net::kTargetItem;
    t.item = item->guid;
    return sendTargeting(t) ? TargetResult::Sent : TargetResult::NotSent;
}

Gameplay::TargetResult Gameplay::targetTradeItem() {
    const Trade& trade = m_exchange.trade();
    if (!m_inWorld || !m_targeting.active() || m_targeting.aim() != SpellAim::Item || !trade.open() ||
        trade.theirs().slots[net::kTradeSlotNonTraded].empty()) {
        return TargetResult::NotSent;
    }
    const SpellInfo& spell = m_targeting.spell();
    const EnchantQuestion q =
        tradeEnchantQuestion(spell, trade.theirs().slots[net::kTradeSlotNonTraded].enchantment, m_gameData);
    if (q.ask != EnchantAsk::None) {
        m_targeting.ask(q, 0, false);
        GameEvent& e = event(GameEventKind::EnchantQuestion, spell.id);
        e.value = {static_cast<i32>(q.ask), static_cast<i32>(q.oldEnchant), static_cast<i32>(q.newEnchant)};
        return TargetResult::Asked;
    }
    net::SpellCastTargets t;
    t.mask = net::kTargetTradeItem;
    t.item = net::kTradeItemTargetSlot;
    return sendTargeting(t) ? TargetResult::Sent : TargetResult::NotSent;
}

Gameplay::TargetResult Gameplay::targetObject(u64 gameObject) {
    const Entity* go = m_entities.find(gameObject);
    if (!m_inWorld || !m_targeting.active() || m_targeting.aim() != SpellAim::Locked || !go ||
        go->type != ObjectType::GameObject) {
        return TargetResult::NotSent;
    }
    net::SpellCastTargets t;
    t.mask = net::kTargetGameObject;
    t.gameObject = gameObject;
    return sendTargeting(t) ? TargetResult::Sent : TargetResult::NotSent;
}

void Gameplay::confirmEnchant() {
    const EnchantQuestion asked = m_targeting.question();
    const u64 itemGuid = m_targeting.questionItem();
    if (!m_inWorld || !m_targeting.active() || asked.ask == EnchantAsk::None) {
        return;
    }
    m_targeting.clearQuestion();
    if (asked.ask == EnchantAsk::TradeReplace) { // ReplaceTradeEnchant: cast on the trade slot
        net::SpellCastTargets t;
        t.mask = net::kTargetTradeItem;
        t.item = net::kTradeItemTargetSlot;
        sendTargeting(t);
        return;
    }
    const ItemInstance* item = m_inventory.find(itemGuid);
    const net::ItemQueryInfo* info = item && item->entry ? m_queries.item(item->entry) : nullptr;
    if (!item || !info) {
        return; // gone meanwhile: still waiting for another
    }
    if (asked.ask == EnchantAsk::Bind) { // BindEnchant: the same check again, the binding accepted
        const EnchantQuestion next = enchantQuestion(m_targeting.spell(), *item, *info, true, m_gameData);
        if (next.ask != EnchantAsk::None) {
            m_targeting.ask(next, item->guid, true);
            GameEvent& e = event(GameEventKind::EnchantQuestion, m_targeting.spellId(), item->guid);
            e.value = {static_cast<i32>(next.ask), static_cast<i32>(next.oldEnchant), static_cast<i32>(next.newEnchant)};
            return;
        }
    }
    net::SpellCastTargets t; // ReplaceEnchant, or nothing more to ask
    t.mask = net::kTargetItem;
    t.item = item->guid;
    sendTargeting(t);
}

void Gameplay::declineEnchant() {
    m_targeting.clearQuestion();
}

bool Gameplay::castSpellOnItem(u32 spellId, ItemPos pos) {
    m_inventory.refresh(m_nowMs, true);
    const ItemInstance* item = m_inventory.at(pos);
    if (!item || item->guid == 0) {
        return false;
    }
    stopTargeting();
    net::SpellCastTargets t;
    t.mask = net::kTargetItem;
    t.item = item->guid;
    return cast(spellId, t);
}

bool Gameplay::castSpellOnTradeItem(u32 spellId) {
    stopTargeting();
    net::SpellCastTargets t;
    t.mask = net::kTargetTradeItem;
    t.item = net::kTradeItemTargetSlot;
    return cast(spellId, t);
}

u32 Gameplay::auras(u64 guid, std::span<AuraInfo> out) const {
    const Entity* e = m_entities.find(guid);
    if (!e) {
        return m_social.memberAuras(guid, out); // parity: a party member out of sight
    }
    const u32 count = readAuras(*e, out);
    if (guid == m_player) {
        m_auraDurations.apply(out.first(count), m_nowMs);
    }
    return count;
}

// -------------------------------------------------------------------------------------------------------
// NPCs and quests.

Interaction Gameplay::interact(u64 guid) {
    const Entity* player = me();
    const Entity* e = m_entities.find(guid);
    if (!m_inWorld || !player || !e || guid == m_player) {
        return Interaction::None;
    }
    if (e->type == ObjectType::GameObject) {
        if (e->value(uf::GAMEOBJECT_FLAGS) & kGameObjectFlagNoInteract) {
            return Interaction::None;
        }
        if (gameObjectType(*e) == kGameObjectMailbox) { // phase 8: the client opens the mailbox itself
            m_exchange.mail().open(guid);
            return Interaction::Mailbox;
        }
        if (gameObjectType(*e) == kGameObjectText) { // phase 13: the server has nothing to do with plaques
            return readObject(guid) ? Interaction::ReadObject : Interaction::None;
        }
        if (gameObjectType(*e) == kGameObjectMeetingStone) { // phase 16 (alerts): its own checks and JOIN
            return m_meetingStone.use(guid, m_nowMs) ? Interaction::UseObject : Interaction::None;
        }
        send(Opcode::CMSG_GAMEOBJ_USE, guid);
        return Interaction::UseObject;
    }
    if (e->type == ObjectType::Corpse) {
        if (const Interaction insignia = removeInsignia(*e); insignia != Interaction::None) {
            return insignia; // phase 17 (honor): an enemy's corpse in a battleground
        }
        if (isLootable(*e)) {
            send(Opcode::CMSG_LOOT, guid);
            return Interaction::Loot;
        }
        return Interaction::None;
    }
    if (!isUnit(*e)) {
        return Interaction::None;
    }
    setTarget(guid);
    if (isDead(*e)) {
        if (const Interaction insignia = removeInsignia(*e); insignia != Interaction::None) {
            return insignia; // phase 17 (honor): an enemy player's body before it was released
        }
        if (isLootable(*e)) {
            send(Opcode::CMSG_LOOT, guid);
            return Interaction::Loot;
        }
        return Interaction::None;
    }
    const u32 npc = npcFlags(*e);
    if ((npc & kNpcFlagSpiritGuide) && !(npc & kNpcFlagGossip)) {
        return interactSpiritGuide(*e); // phase 17 (honor): no gossip menu (0x5DF950)
    }
    if (npc & (kNpcFlagSpiritHealer | kNpcFlagSpiritGuide)) {
        send(Opcode::CMSG_GOSSIP_HELLO, guid);
        return Interaction::SpiritHealer;
    }
    const bool attackable = canAttack(*player, *e, m_reactions);
    if (attackable && reactionTo(*player, *e, m_reactions) == Reaction::Hostile) {
        return startAttack(guid) ? Interaction::Attack : Interaction::None;
    }
    if (npc & kNpcFlagGossip) {
        send(Opcode::CMSG_GOSSIP_HELLO, guid);
        return Interaction::Gossip;
    }
    // Phase 8: vendors and bankers without a gossip menu open their window at once. A vendor that also gives
    // quests answers CMSG_GOSSIP_HELLO with both (Player::SendPreparedGossip): that way the quests stay reachable.
    if ((npc & kNpcFlagVendor) && (npc & kNpcFlagQuestGiver)) {
        send(Opcode::CMSG_GOSSIP_HELLO, guid);
        return Interaction::Gossip;
    }
    if (npc & kNpcFlagVendor) {
        m_inventory.requestVendor(guid);
        return Interaction::Vendor;
    }
    if (npc & kNpcFlagBanker) {
        m_inventory.requestBank(guid);
        return Interaction::Bank;
    }
    if (npc & kNpcFlagAuctioneer) { // phase 8: without a gossip menu, straight to the auction house
        m_exchange.auction().requestOpen(guid);
        return Interaction::Auctioneer;
    }
    if (npc & kNpcFlagQuestGiver) {
        send(Opcode::CMSG_QUESTGIVER_HELLO, guid);
        return Interaction::QuestGiver;
    }
    if (npc & kNpcFlagTrainer) { // no gossip menu: straight to its list
        m_abilities.requestTrainerList(guid);
        return Interaction::Trainer;
    }
    if (attackable && npc == 0) { // a neutral creature
        return startAttack(guid) ? Interaction::Attack : Interaction::None;
    }
    if (npc & kNpcFlagFlightMaster) { // without a gossip menu: straight to its map
        m_travel.openTaxiMap(guid);
        return Interaction::FlightMaster;
    }
    if (npc & kNpcFlagBattlemaster) { // phase 17 (queue): without a gossip menu (0x5F0130 -> 0x5E01A0)
        return m_battlefieldQueueing.hello(guid) ? Interaction::Battlemaster : Interaction::None;
    }
    return Interaction::None;
}

void Gameplay::selectGossipOption(u32 index, std::string_view code) {
    if (m_dialog.kind() != DialogKind::Gossip) {
        return;
    }
    m_stable.noteGossipNpc(m_dialog.npc()); // phase 15 (pets): the untrainer CONFIRM_PET_UNLEARN stays near
    m_body.clear();
    net::writeGossipSelectOption(m_body, m_dialog.npc(), index, code);
    sendBody(Opcode::CMSG_GOSSIP_SELECT_OPTION);
}

void Gameplay::selectQuest(u32 questId) {
    if (!m_dialog.open()) {
        return;
    }
    net::QuestGiverStatus icon = net::QuestGiverStatus::None;
    for (const QuestMenuEntry& q : m_dialog.quests()) {
        if (q.questId == questId) {
            icon = q.icon;
        }
    }
    const bool turnIn = turnsIn(icon) || m_questLog.find(questId) != nullptr;
    m_body.clear();
    net::writeQuestGiverQuest(m_body, m_dialog.npc(), questId);
    sendBody(turnIn ? Opcode::CMSG_QUESTGIVER_COMPLETE_QUEST : Opcode::CMSG_QUESTGIVER_QUERY_QUEST);
}

void Gameplay::acceptQuest() {
    if (m_dialog.kind() != DialogKind::QuestDetails) {
        return;
    }
    m_body.clear();
    net::writeQuestGiverQuest(m_body, m_dialog.npc(), m_dialog.quest().questId);
    sendBody(Opcode::CMSG_QUESTGIVER_ACCEPT_QUEST); // the server closes the window (SMSG_GOSSIP_COMPLETE)
}

void Gameplay::completeQuest() {
    if (m_dialog.kind() != DialogKind::QuestRequestItems || !m_dialog.quest().completable) {
        return;
    }
    m_body.clear();
    net::writeQuestGiverQuest(m_body, m_dialog.npc(), m_dialog.quest().questId);
    sendBody(Opcode::CMSG_QUESTGIVER_REQUEST_REWARD);
}

void Gameplay::chooseQuestReward(u32 choice) {
    if (m_dialog.kind() != DialogKind::QuestOfferReward) {
        return;
    }
    const QuestDialog& q = m_dialog.quest();
    m_body.clear();
    net::writeQuestGiverChooseReward(m_body, m_dialog.npc(), q.questId,
                                     q.choiceCount ? std::min(choice, q.choiceCount - 1) : 0);
    sendBody(Opcode::CMSG_QUESTGIVER_CHOOSE_REWARD);
}

void Gameplay::closeDialog() {
    // Phase 13 (quests): a quest a party member shared, declined (DeclineQuest): the sharer hears it and the server
    // forgets the share (else this player stays "busy" for the next one). The rest needs no packet.
    if (m_dialog.kind() == DialogKind::QuestDetails && isPlayerGuid(m_dialog.npc())) {
        m_body.clear();
        net::writeQuestPushResult(m_body, m_dialog.npc(), net::kQuestPushDeclined);
        sendBody(Opcode::MSG_QUEST_PUSH_RESULT);
    }
    m_dialog.close();
}

void Gameplay::confirmSharedQuest(u32 questId) {
    m_body.clear();
    net::writeU32(m_body, questId);
    sendBody(Opcode::CMSG_QUEST_CONFIRM_ACCEPT);
}

net::QuestGiverStatus Gameplay::questGiverStatus(u64 guid) const {
    const auto it = m_questGiverStatus.find(guid);
    return it == m_questGiverStatus.end() ? net::QuestGiverStatus::None : it->second;
}

const NpcText* Gameplay::npcText(u32 textId) {
    return m_queries.npcText(textId, m_dialog.npc());
}

void Gameplay::queryQuestGiverStatus(u64 guid) {
    send(Opcode::CMSG_QUESTGIVER_STATUS_QUERY, guid);
}

void Gameplay::refreshQuestGiverStatuses() {
    // The server never pushes status changes: ask every quest giver in sight again.
    m_entities.forEach([&](const Entity& e) {
        if (e.guid != m_player && isQuestGiver(e)) {
            queryQuestGiverStatus(e.guid);
        }
    });
}

void Gameplay::scheduleStatusRefresh(u32 nowMs) {
    m_statusRefreshPending = true;
    m_statusRefreshAt = nowMs + kStatusRefreshDelayMs;
}

void Gameplay::queryItems(std::span<const net::QuestItem> items) {
    for (const net::QuestItem& item : items) {
        m_queries.item(item.itemId);
    }
}

u32 Gameplay::questObjectives(u32 questId, std::span<QuestObjectiveView> out) {
    const QuestInfo* q = m_queries.quest(questId);
    if (!q) {
        return 0;
    }
    const QuestLogEntry* entry = m_questLog.find(questId);
    const Entity* player = me();
    u32 count = 0;
    for (u32 i = 0; i < net::kQuestObjectiveCount && count < out.size(); ++i) {
        const QuestInfo::Objective& o = q->objectiveList[i];
        if (o.creatureOrGo == 0 || o.creatureOrGoCount == 0) {
            continue;
        }
        QuestObjectiveView& v = out[count++];
        v = QuestObjectiveView{};
        const bool object = (o.creatureOrGo & kQuestObjectiveGameObject) != 0;
        v.kind = object ? QuestObjectiveView::Kind::GameObject : QuestObjectiveView::Kind::Creature;
        v.entry = o.creatureOrGo & ~kQuestObjectiveGameObject;
        v.need = o.creatureOrGoCount;
        v.have = entry ? std::min<u32>(entry->counters[i], v.need) : 0;
        if (entry && entry->complete()) {
            v.have = v.need;
        }
        v.text = o.text;
        if (object) {
            if (const GameObjectInfo* go = m_queries.gameObject(v.entry)) {
                v.name = go->name;
            }
        } else if (const net::CreatureQueryInfo* c = m_queries.creature(v.entry)) {
            v.name = c->name;
        }
    }
    for (u32 i = 0; i < net::kQuestObjectiveCount && count < out.size(); ++i) {
        const QuestInfo::Objective& o = q->objectiveList[i];
        if (o.itemId == 0 || o.itemCount == 0) {
            continue;
        }
        QuestObjectiveView& v = out[count++];
        v = QuestObjectiveView{};
        v.kind = QuestObjectiveView::Kind::Item;
        v.entry = o.itemId;
        v.need = o.itemCount;
        v.have = player ? std::min(inventoryCount(m_entities, *player, o.itemId), v.need) : 0;
        if (const net::ItemQueryInfo* item = m_queries.item(o.itemId)) {
            v.name = item->name;
        }
    }
    return count;
}

void Gameplay::abandonQuest(u32 questId) {
    const i32 slot = m_questLog.slotOf(questId);
    if (slot < 0) {
        return;
    }
    m_body.clear();
    net::writeU8(m_body, static_cast<u8>(slot));
    sendBody(Opcode::CMSG_QUESTLOG_REMOVE_QUEST);
}

// -------------------------------------------------------------------------------------------------------
// Phase 13 (quests): sharing, watches, readable objects.

bool Gameplay::canShareQuest(u32 questId) const {
    const QuestInfo* info = m_queries.findQuest(questId);
    // Phase 16 (pullouts): QuestLogFrame's GetNumPartyMembers() > 0: in a raid, someone else in the player's subgroup.
    return m_questLog.find(questId) != nullptr && info && questSharable(*info) && m_social.group().active &&
           m_social.partyMemberCount() > 0;
}

bool Gameplay::shareQuest(u32 questId) {
    if (!canShareQuest(questId)) {
        return false;
    }
    m_body.clear();
    net::writePushQuestToParty(m_body, questId);
    return sendBody(Opcode::CMSG_PUSHQUESTTOPARTY);
}

void Gameplay::questWatchUpdate(u32 questId) {
    if (m_questLog.find(questId) == nullptr) {
        return;
    }
    event(GameEventKind::QuestWatchUpdate, questId);
    m_journal.autoWatch(questId, m_nowMs); // nothing with AUTO_QUEST_WATCH off
}

bool Gameplay::readObject(u64 guid) {
    const Entity* e = m_entities.find(guid);
    if (!e || e->type != ObjectType::GameObject) {
        return false;
    }
    const GameObjectInfo* info = m_queries.gameObject(e->value(uf::OBJECT_FIELD_ENTRY), guid);
    if (!info || !info->found) {
        return false; // its template is on its way (asked for now): a tap again reads it
    }
    // GameObjectDefines.h: TEXT data0 page, data2 material; GOOBER data7 page, data9 material.
    const bool text = info->type == kGameObjectText;
    const u32 page = text ? info->data[0] : info->type == kGameObjectGoober ? info->data[7] : 0;
    if (page == 0) {
        return false;
    }
    m_inventory.openPages(guid, page, text ? info->data[2] : info->data[9], info->name);
    return true;
}

// -------------------------------------------------------------------------------------------------------
// Loot.

void Gameplay::lootItem(u8 slot) {
    if (!m_loot.open()) {
        return;
    }
    m_body.clear();
    net::writeU8(m_body, slot);
    sendBody(Opcode::CMSG_AUTOSTORE_LOOT_ITEM);
}

void Gameplay::lootMoney() {
    if (!m_loot.open() || m_loot.gold() == 0) {
        return;
    }
    if (send(Opcode::CMSG_LOOT_MONEY)) {
        m_moneyRequested = true;
    }
}

void Gameplay::lootAll() {
    lootMoney();
    for (const LootSlot& item : m_loot.items()) {
        if (item.lootable()) {
            lootItem(item.slot);
        }
    }
}

void Gameplay::closeLoot() {
    if (!m_loot.open()) {
        return;
    }
    send(Opcode::CMSG_LOOT_RELEASE, m_loot.guid());
    m_loot.close(); // SMSG_LOOT_RELEASE_RESPONSE confirms
    m_exchange.rolls().clearMasterCandidates(); // they were this loot's
}

// -------------------------------------------------------------------------------------------------------
// Chat.

std::string_view Gameplay::serverMessageText(const net::ServerMessage& m) {
    // WoW.exe's SMSG_SERVER_MESSAGE handler: sprintf(ServerMessages[type], text) when the text is not empty, the
    // row as is when it is, "[%d]: %s" for a type the table does not have. Only the row's first "%s" takes the
    // text here (the rows have one at most; the server's text is never used as a format).
    m_serverText.clear();
    const std::string_view format = m_gameData ? m_gameData->serverMessage(m.type) : std::string_view{};
    if (format.empty()) {
        m_serverText.append("[").append(std::to_string(m.type)).append("]: ").append(m.text);
        return m_serverText;
    }
    const usize at = m.text.empty() ? std::string_view::npos : format.find("%s");
    if (at == std::string_view::npos) {
        m_serverText.assign(format);
        return m_serverText;
    }
    m_serverText.assign(format.substr(0, at)).append(m.text).append(format.substr(at + 2));
    return m_serverText;
}

u32 Gameplay::playerLanguage() const {
    // The race's own language (ChrRaces): humans, dwarves, night elves and gnomes speak Common, the Horde
    // Orcish.
    const Entity* player = me();
    const u8 race = player ? player->byteValue(uf::UNIT_FIELD_BYTES_0, 0) : 1;
    const bool alliance = race == 1 || race == 3 || race == 4 || race == 7;
    return static_cast<u32>(alliance ? net::Language::Common : net::Language::Orcish);
}

bool Gameplay::sendChat(net::ChatType type, std::string_view text, std::string_view target) {
    if (!m_inWorld || text.empty()) {
        return false;
    }
    if ((type == net::ChatType::Whisper || type == net::ChatType::Channel) && target.empty()) {
        return false;
    }
    // The server drops LANG_UNIVERSAL for everything but AFK / DND: even emotes go in the player's language.
    m_body.clear();
    // Phase 18 (extras): in the language the chat speaks (ChatMenu's Language submenu), the player's own by default.
    net::writeMessageChat(m_body, type, static_cast<net::Language>(speakingLanguage()), target, text);
    return sendBody(Opcode::CMSG_MESSAGECHAT);
}

std::string_view Gameplay::senderName(const ChatLine& line) {
    if (!line.senderName.empty()) {
        return line.senderName;
    }
    if (line.sender == 0) {
        return {};
    }
    if (isPlayerGuid(line.sender)) {
        const std::string* name = m_queries.playerName(line.sender);
        return name ? std::string_view(*name) : std::string_view{};
    }
    if (const Entity* e = m_entities.find(line.sender)) {
        if (const net::CreatureQueryInfo* c = m_queries.creature(e->value(uf::OBJECT_FIELD_ENTRY))) {
            return c->name;
        }
    }
    return {};
}

// -------------------------------------------------------------------------------------------------------
// Death.

void Gameplay::releaseSpirit() {
    if (m_death.dead && !m_death.ghost) {
        send(Opcode::CMSG_REPOP_REQUEST);
    }
}

u32 Gameplay::reclaimDelayRemaining(u32 nowMs) const {
    if (m_death.reclaimAtMs == 0 || due(nowMs, m_death.reclaimAtMs)) {
        return 0;
    }
    return m_death.reclaimAtMs - nowMs;
}

bool Gameplay::canReclaimCorpse(u32 nowMs) const {
    const Entity* player = me();
    // Phase 12: a corpse inside an instance is reported at its entrance; it is recovered by entering it.
    if (!m_death.ghost || !m_death.corpseKnown || !player || !player->hasPosition ||
        static_cast<u32>(m_death.corpseMap) != m_map || m_death.corpseRealMap != m_map ||
        reclaimDelayRemaining(nowMs) != 0) {
        return false;
    }
    const glm::vec3 p = player->positionAt(nowMs);
    const f32 dx = p.x - m_death.corpseX, dy = p.y - m_death.corpseY, dz = p.z - m_death.corpseZ;
    return dx * dx + dy * dy + dz * dz <= kCorpseReclaimRadius * kCorpseReclaimRadius;
}

void Gameplay::reclaimCorpse() {
    if (!m_death.ghost) {
        return;
    }
    // The server finds the corpse itself; the GUID is read and ignored. Send the right one when it is in
    // sight.
    u64 corpse = 0;
    m_entities.forEach([&](const Entity& e) {
        if (e.type == ObjectType::Corpse && guidValue(e, uf::CORPSE_FIELD_OWNER) == m_player) {
            corpse = e.guid;
        }
    });
    send(Opcode::CMSG_RECLAIM_CORPSE, corpse);
}

void Gameplay::answerResurrect(bool accept) {
    if (m_death.resurrector == 0) {
        return;
    }
    m_body.clear();
    net::writeResurrectResponse(m_body, m_death.resurrector, accept);
    sendBody(Opcode::CMSG_RESURRECT_RESPONSE);
    m_death.resurrector = 0;
    m_death.resurrectorName.clear();
}

void Gameplay::acceptSpiritHealer() {
    if (m_death.spiritHealer == 0) {
        return;
    }
    send(Opcode::CMSG_SPIRIT_HEALER_ACTIVATE, m_death.spiritHealer);
    m_death.spiritHealer = 0;
}

void Gameplay::selfResurrect() {
    if (m_death.dead && !m_death.ghost && m_death.selfResSpell != 0) {
        send(Opcode::CMSG_SELF_RES); // the server casts it and clears the field
    }
}

void Gameplay::queryCorpse() {
    send(Opcode::MSG_CORPSE_QUERY);
}

void Gameplay::setSheath(u8 state) {
    const Entity* player = me();
    if (!m_inWorld || !player) {
        return;
    }
    m_sheath = state;
    m_sheathKnown = true;
    const u8 field = player->byteValue(uf::UNIT_FIELD_BYTES_2, 0);
    if (state == m_sheathAsked && (field == state || m_nowMs - m_sheathAskedAt < 1000)) {
        return; // the server has it, or was asked a moment ago and its field change is on its way
    }
    m_body.clear();
    net::writeU32(m_body, state);
    if (sendBody(Opcode::CMSG_SETSHEATHED)) {
        m_sheathAsked = state;
        m_sheathAskedAt = m_nowMs;
    }
}

u8 Gameplay::sheath() const {
    if (m_sheathKnown) {
        return m_sheath;
    }
    const Entity* player = me();
    return player ? player->byteValue(uf::UNIT_FIELD_BYTES_2, 0) : 0;
}

void Gameplay::setStandState(u8 state) {
    m_body.clear();
    net::writeU32(m_body, state);
    sendBody(Opcode::CMSG_STANDSTATECHANGE);
}

} // namespace vd::game
