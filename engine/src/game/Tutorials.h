#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/AccountDataMessages.h"
#include "net/Opcodes.gen.h"

#include <array>
#include <span>

namespace vd::game {

class Gameplay;

// Phase 18 (p3): the 50 tutorials of 1.12.1 by WoW.exe's 0-based id (TUTORIAL_TITLE<id + 1> / TUTORIAL<id + 1> of
// GlobalStrings). The comment says what triggers each one (TriggerTutorial 0x4B5390: its 51 call sites; addresses in
// Tutorials.cpp). SpellBook (14, "Spells and Abilities Book") has no call site in 1.12.1: it never shows.
enum class Tutorial : u8 {
    Questgivers,            // entering the world, after Welcome
    Movement,               // 90 s in the world without moving
    Cameras,                // level 4 reached
    Targeting,              // 10 s after the first move
    CombatMode,             // a unit the player can attack targeted
    Spells,                 // the player's own auto attack starts
    Looting,                // a unit becomes lootable (UNIT_DYNFLAG_LOOTABLE)
    Backpack,               // an item received
    UsingItems,             // an item received with an on-use spell
    Bags,                   // a container received
    Food,                   // in combat under half health
    Drink,                  // in combat under half mana (mana users)
    Talents,                // level 10 reached
    Trainers,               // level 4 reached
    SpellBook,              // never (no call site)
    Reputation,             // a faction listed (SMSG_SET_FACTION_VISIBLE) from level 10
    ReplyingToTells,        // a whisper received
    Grouping,               // a friendly player targeted from level 5
    Players,                // another player targeted (the first time)
    Vendors,                // a vendor opened
    QuestLog,               // a quest accepted
    Friends,                // level 7 reached
    Chatting,               // level 3 reached
    EquippableItems,        // an item received that goes in an equipment or bag slot
    Death,                  // released (a ghost)
    Rested,                 // rested (PLAYER_BYTES_2 rest state 1)
    Fatigue,                // the fatigue bar starts
    Swimming,               // swimming starts
    Breath,                 // the breath bar starts
    Resting,                // resting (PLAYER_FLAGS_RESTING)
    Hearthstones,           // the Hearthstone (6948) received
    Pvp,                    // PvP flagged (UNIT_FLAG_PVP)
    Jumping,                // level 8 reached
    QuestCompletion,        // every objective of a quest met
    Travel,                 // a flight master's map opened, or a flight master targeted
    DamagedItems,           // an equipped item low on durability
    BrokenItems,            // an equipped item broken
    Professions,            // level 7 reached
    Groups,                 // level 5 reached
    TheSpellbook,           // a spell learned (LEARNED_SPELL_IN_TAB)
    EliteQuests,            // an elite quest (QuestInfo type 1) accepted
    Welcome,                // entering the world
    UnavailableQuestGivers, // a quest giver with the grey "!" near
    RangedWeapons,          // a bow, gun or crossbow received (not hunters)
    Ammunition,             // a cast refused for want of ammunition (SPELL_FAILED_NEED_AMMO)
    RaidGroups,             // in a raid group
    MeetingStones,          // queued at a meeting stone
    BattlegroundQueue,      // a battleground queue joined
    PortToBattleground,     // a battleground ready to enter
    Keyrings,               // a key received
    Count
};
constexpr u32 kTutorialCount = static_cast<u32>(Tutorial::Count); // TriggerTutorial's FlagTutorial check: < 50

// The tutorials as WoW.exe 1.12.1 keeps them (Tutorial.cpp, read-only disassembly 0x4B5280..0x4B5A60) against vMaNGOS'
// account-wide flags (CharacterHandler.cpp HandleTutorial*, WorldSession.cpp SendTutorialsData):
//  - SMSG_TUTORIAL_FLAGS (0x4B5700, every login before the world) fills two bit sets with the server's words: the
//    "triggered" set (a tutorial shows once per session) and the "flagged" set (seen for good).
//  - TriggerTutorial(id, delay) (0x4B5390): nothing before the flags came, or when the id is triggered already; else it
//    becomes triggered and, after `delay` ms (a timer) or at once, the TutorialPopup sound plays and TUTORIAL_TRIGGER
//    fires with id + 1 (the TutorialTriggered event here: TutorialFrame_NewTutorial queues its alert button).
//  - FlagTutorial(n) (0x4B59B0 -> 0x4B54C0, TutorialFrame_Update when its button is clicked): n - 1 in 0..49 not flagged
//    yet: its pending timers go, it becomes triggered and flagged and CMSG_TUTORIAL_FLAG(id) goes out.
//  - ClearTutorials (0x4B55C0: the frame closed with "Display Tips" unchecked, or the option turned off): timers gone,
//    every bit of both sets set, CMSG_TUTORIAL_CLEAR. ResetTutorials (0x4B5660: "Reset Tutorials", the option turned
//    on): every bit cleared, CMSG_TUTORIAL_RESET. TutorialsEnabled (0x4B5960): not every byte of the flagged set 0xFF.
// What triggers each tutorial is checked here from the game's state and events (update()); the movement input comes
// from the session (playerMoved). Main thread; nothing allocates after construction.
class Tutorials {
public:
    using Events = EventRing<GameEvent, 64>;

    Tutorials(const EntityStore& entities, Events& events) : m_entities(entities), m_events(events) {}

    void setSink(PacketSink sink) { m_sink = sink; }
    static bool handles(net::Opcode opcode) { return opcode == net::Opcode::SMSG_TUTORIAL_FLAGS; }
    bool handle(net::Opcode opcode, std::span<const u8> payload);
    // PLAYER_ENTERING_WORLD (every world entry, 0x490A41): Welcome, then Questgivers. A new login starts the idle clock
    // of Movement over (WorldFrame 0x482FE8: 90 s after the input control started).
    void enterWorld(bool newLogin, u32 nowMs);
    void leaveWorld();
    // Once a frame: the timers, the events and the state the triggers watch (game/Tutorials.cpp).
    void update(Gameplay& play, u32 nowMs);
    // The player's movement keys (InputControl 0x514A83: forward, backward or a strafe started): Movement is flagged and
    // Targeting triggered 10 s later.
    void playerMoved(u32 nowMs);

    // The Lua API of TutorialFrame / UIOptionsFrame.
    void trigger(Tutorial tutorial, u32 nowMs, u32 delayMs = 0); // TriggerTutorial (C only)
    void flag(Tutorial tutorial);                                // FlagTutorial(id + 1)
    void clear();                                                // ClearTutorials
    void reset();                                                // ResetTutorials
    bool enabled() const;                                        // TutorialsEnabled
    bool known() const { return m_known; }                       // SMSG_TUTORIAL_FLAGS came
    // Bumped by every SMSG_TUTORIAL_FLAGS (a login): the TutorialTriggered events carry it in value[0].
    u32 session() const { return m_session; }
    bool triggered(Tutorial tutorial) const { return test(m_triggered, static_cast<u32>(tutorial)); }
    bool flagged(Tutorial tutorial) const { return test(m_flagged, static_cast<u32>(tutorial)); }
    const net::TutorialWords& flags() const { return m_flagged; }

    // Development (VD_P3_TUTORIAL): the flags as if the server sent them.
    void setFlags(const net::TutorialWords& words);

private:
    static bool test(const net::TutorialWords& words, u32 id) { return (words[id / 32] & (1u << (id % 32))) != 0; }
    static void set(net::TutorialWords& words, u32 id) { words[id / 32] |= 1u << (id % 32); }
    void fire(u32 id, u32 nowMs);
    void readEvents(Gameplay& play, u32 nowMs);
    void watchTarget(Gameplay& play, u32 nowMs);
    void watchPlayer(Gameplay& play, u32 nowMs);
    void scanUnits(Gameplay& play, u32 nowMs);
    bool itemReceived(Gameplay& play, u32 entry, u32 nowMs); // false while its template is not known

    const EntityStore& m_entities;
    Events& m_events;
    PacketSink m_sink;
    bool m_known = false;
    u32 m_session = 0;
    net::TutorialWords m_triggered{};
    net::TutorialWords m_flagged{};
    // TUTORIALTIMER: a trigger waiting for its delay (only Targeting has one in 1.12.1).
    struct Timer {
        u32 id = kTutorialCount; // kTutorialCount: free
        u32 dueMs = 0;
    };
    std::array<Timer, 4> m_timers{};

    bool m_inWorld = false;
    u32 m_idleSinceMs = 0; // when the idle clock of Movement started
    u32 m_eventCursor = 0;
    u32 m_chatCursor = 0;
    bool m_resetChat = true; // a new login: the chat is read from its end
    u64 m_lastTarget = 0;
    u32 m_nextScanMs = 0;
    u32 m_nextPollMs = 0;
    // The player's state at the last poll: what changed since (the triggers fire on the change).
    u32 m_restState = 0;
    u32 m_playerFlags = 0;
    u32 m_unitFlags = 0;
    bool m_swimming = false;
    bool m_fatigue = false, m_breath = false;
    // Items received whose template was not known yet (asked for): checked again later.
    std::array<u32, 8> m_pendingItems{};
};

} // namespace vd::game
