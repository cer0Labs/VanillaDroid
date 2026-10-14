#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "game/Spells.h"
#include "net/CombatMessages.h"
#include "net/GameMessages.h"
#include "net/Opcodes.gen.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::game {

// The open trainer window (SMSG_TRAINER_LIST until closed or out of reach).
class TrainerWindow {
public:
    bool open() const { return m_trainer != 0; }
    u64 trainer() const { return m_trainer; }
    u32 type() const { return m_type; } // net::kTrainerType*
    std::string_view greeting() const { return m_greeting; }
    std::span<const net::TrainerList::Spell> services() const { return m_services; }
    // The service bought last (SMSG_TRAINER_BUY_SUCCEEDED marks it known), 0 none.
    u32 lastBought() const { return m_lastBought; }

    void onList(const net::TrainerList& m);
    void onBought(u32 spellId);
    void close() {
        m_trainer = 0;
        m_services.clear();
    }

private:
    u64 m_trainer = 0;
    u32 m_type = 0;
    u32 m_lastBought = 0;
    std::string m_greeting;
    std::vector<net::TrainerList::Spell> m_services;
};

// The player's pet or charmed unit as SMSG_PET_SPELLS describes it: its action bar of 10 buttons (Attack,
// Follow, Stay, four spells, Aggressive, Defensive, Passive by default), its spell book with autocast, its
// states.
struct PetState {
    u64 guid = 0;       // 0: none
    u32 durationMs = 0; // charms: how long the control lasts (0: a pet)
    u32 sinceMs = 0;    // when SMSG_PET_SPELLS came
    u8 reactState = net::kPetReactDefensive;
    u8 commandState = net::kPetCommandFollow;
    bool enabled = true;                           // false: its bar is disabled (kPetDisabledFlag)
    std::array<u32, net::kPetActionBarSize> bar{}; // packed (net::petButton)
    std::vector<u32> spells;                       // packed: spell | state << 24

    bool active() const { return guid != 0; }
    // The spell of a bar slot (0 for commands, reactions and empty slots).
    u32 slotSpell(u32 slot) const;
    bool slotAutocast(u32 slot) const;
};

// When a pet spell can be cast again (cooldowns come with SMSG_PET_SPELLS and start on the pet's
// SMSG_SPELL_GO).
struct PetCooldown {
    u32 spellId = 0;
    u32 startMs = 0;
    u32 durationMs = 0;
};

// The reputation list (SMSG_INITIALIZE_FACTIONS, _SET_FACTION_STANDING, _SET_FACTION_VISIBLE): flags and
// standing (without the base reputation of Faction.dbc) by reputation list id.
class ReputationList {
public:
    struct Entry {
        u8 flags = 0; // net::kFaction*
        i32 standing = 0;
    };
    bool initialized() const { return m_initialized; }
    const Entry& entry(u32 listId) const { return m_entries[listId < m_entries.size() ? listId : 0]; }
    u32 size() const { return static_cast<u32>(m_entries.size()); }

    void reset();
    void onInitialize(const net::InitializeFactions& m);
    // Returns the change of standing (0 if none or unknown).
    i32 onStanding(u32 listId, i32 standing);
    void onVisible(u32 listId);
    void setAtWar(u32 listId, bool atWar);
    void setInactive(u32 listId, bool inactive); // phase 15: SetFactionInactive changes it here (the server is silent)

private:
    bool m_initialized = false;
    std::array<Entry, net::kFactionListSize> m_entries{};
};

// What the character can do and learn beyond its spell book (phase 8): trainers, talent learning and resets,
// the pet, reputation and the auto-repeat spell (Auto Shot, Shoot). Owned by Gameplay, which hands it its
// packets; its requests go through the same PacketSink and its news to the same event ring. Main thread;
// nothing allocates per frame once warmed up.
class Abilities {
public:
    using Events = EventRing<GameEvent, 64>;

    Abilities(const EntityStore& entities, Events& events);

    void setSink(PacketSink sink) { m_sink = sink; }
    void setSpellSource(SpellSource source) { m_source = source; }
    void enterWorld(u64 player, bool sameCharacter, u32 nowMs);
    void leaveWorld();

    // A packet of this module (see Gameplay::handle): false when malformed.
    static bool handles(net::Opcode opcode);
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    // Gameplay forwards these packets too (they belong to the spell book first).
    void onSpellGo(const net::SpellGo& m, u32 nowMs);
    void onCastResult(const net::CastResult& m);

    // --- Trainers ---
    const TrainerWindow& trainer() const { return m_trainer; }
    void requestTrainerList(u64 npc);  // CMSG_TRAINER_LIST (trainers without a gossip menu)
    bool buyTrainerSpell(u32 spellId); // CMSG_TRAINER_BUY_SPELL at the open trainer
    void closeTrainer() { m_trainer.close(); }
    // A trainer's talent reset offer (MSG_TALENT_WIPE_CONFIRM): the trainer, 0 none.
    u64 talentWipeTrainer() const { return m_wipeTrainer; }
    u32 talentWipeCost() const { return m_wipeCost; }
    void answerTalentWipe(bool accept);

    // --- Talents ---
    // CMSG_LEARN_TALENT (rank 0-based). The server answers with the learned spell and the new free points.
    bool learnTalent(u32 talentId, u32 rank);

    // --- Pet ---
    const PetState& pet() const { return m_pet; }
    // The pet's name: asked for (CMSG_PET_NAME_QUERY) when missing, empty until the answer.
    std::string_view petName();
    // A tap on a bar slot: commands (`target` for Attack), reactions, spells at `target` (0: none).
    bool petAction(u32 slot, u64 target);
    bool petCommand(u32 command, u64 target);
    bool petReact(u32 react);
    // Autocast on / off of the spell in a bar slot (CMSG_PET_SPELL_AUTOCAST).
    bool setPetAutocast(u32 slot, bool enabled);
    // Puts a pet spell (from its spell book, 0 to empty it) on a spell slot of the bar (CMSG_PET_SET_ACTION).
    bool setPetSlot(u32 slot, u32 spellId);
    // Phase 15 (pets): the pet's spell book (SpellBookFrame's BOOKTYPE_PET), as WoW.exe lists it from
    // SMSG_PET_SPELLS (0x4BD990: every spell but those hidden from the client, Spell.dbc attributes 0x80,
    // sorted by name then rank).
    std::span<const u32> petBook() const { return m_petBook; }
    // A pet spell's state in its book (net::kPetActPassive / Disabled / Enabled; 0: not known).
    u8 petSpellState(u32 spellId) const;
    // ToggleSpellAutocast(id, "pet"): autocast on / off of a spell of the book (CMSG_PET_SPELL_AUTOCAST); the
    // bar's buttons of that spell follow.
    bool setPetSpellAutocast(u32 spellId, bool enabled);
    // Phase 15 (pets): PickupPetAction on one slot, then on another: the two buttons change places
    // (CMSG_PET_SET_ACTION with both, which is how the server lets command and reaction buttons move).
    bool swapPetSlots(u32 from, u32 to);
    // CastSpell(id, "pet") from the pet's page: CMSG_PET_ACTION with the spell at `target` (0: none).
    bool castPetSpell(u32 spellId, u64 target);
    // Warlocks' and charmed pets go away (COMMAND_DISMISS); hunters cast Dismiss Pet (the caller does).
    bool dismissPet();
    // Hunters: the pet leaves for good (CMSG_PET_ABANDON).
    bool abandonPet();
    // Remaining cooldown of a pet spell.
    u32 petCooldownRemaining(u32 spellId, u32 nowMs) const;
    PetCooldown petCooldown(u32 spellId, u32 nowMs) const;

    // --- Reputation ---
    const ReputationList& reputation() const { return m_reputation; }
    ReputationList& reputation() { return m_reputation; } // phase 15 (game/CharacterSheet: inactive factions)
    bool setAtWar(u32 listId, bool atWar); // CMSG_SET_FACTION_ATWAR (also changed here: the server is silent)

    // --- Auto repeat ---
    // The auto-repeat spell running against `target` (0: none). The HUD asks with the current target: a
    // change of target (or its death) ends it, as the client's StopAutoRepeat.
    u32 autoRepeat(u64 target) const {
        return target != 0 && target == m_autoRepeatTarget ? m_autoRepeatSpell : 0;
    }
    void startedAutoRepeat(u32 spellId, u64 target); // the caller cast it
    void cancelAutoRepeat();                         // CMSG_CANCEL_AUTO_REPEAT_SPELL

private:
    bool sendBody(net::Opcode opcode);
    bool sendGuid(net::Opcode opcode, u64 guid);
    GameEvent& event(GameEventKind kind, u32 id = 0, u64 guid = 0);
    void onPetSpells(const net::PetSpells& m, u32 nowMs);
    void startPetCooldown(u32 spellId, u32 durationMs, u32 nowMs);
    void buildPetBook(); // phase 15 (pets)

    const EntityStore& m_entities;
    Events& m_events;
    PacketSink m_sink;
    SpellSource m_source;
    bool m_inWorld = false;
    u64 m_player = 0;
    u32 m_nowMs = 0;
    std::vector<u8> m_body;

    TrainerWindow m_trainer;
    u64 m_wipeTrainer = 0;
    u32 m_wipeCost = 0;
    PetState m_pet;
    std::vector<u32> m_petBook; // phase 15 (pets): spell ids in the book's order
    std::vector<PetCooldown> m_petCooldowns;
    std::string m_petName;
    u32 m_petNameNumber = 0; // UNIT_FIELD_PETNUMBER the name belongs to
    u32 m_petNameAsked = 0;  // the pet number asked for
    u32 m_petNameAskedMs = 0;
    ReputationList m_reputation;
    u32 m_autoRepeatSpell = 0;
    u64 m_autoRepeatTarget = 0;

    // Parsed messages, reused.
    net::TrainerList m_trainerList;
    net::PetSpells m_petSpells;
    net::InitializeFactions m_factions;
    net::FactionStanding m_standing;
};

} // namespace vd::game
