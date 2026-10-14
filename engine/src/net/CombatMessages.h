#pragma once

#include "core/Types.h"
#include "net/ByteBuffer.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

// Phase-8 packets of the character's abilities: trainers, talents, pets and reputation, as vmangos-core
// 4641790 writes and reads them for build 5875 (Handlers/NPCHandler.cpp, SkillHandler.cpp, PetHandler.cpp,
// Objects/Player.cpp PetSpellInitialize, ReputationMgr.cpp, Server/Packets/{Npc,Skill,Pet,Misc}.cpp). Same
// rules as net/GameMessages: parsers fill a caller-owned struct and return false on a malformed packet
// without reading past its end; reused structs keep their vectors' capacity; strings are views into the
// payload.
namespace vd::net {

// -------------------------------------------------------------------------------------------------------
// Trainers (server -> client).

// TrainerSpellState as sent (Objects/Player.h; GREEN_DISABLED is sent as GREEN).
enum class TrainerSpellState : u8 { Available = 0, Unavailable = 1, Known = 2 };
// TrainerType (Objects/CreatureDefines.h).
constexpr u32 kTrainerTypeClass = 0;
constexpr u32 kTrainerTypeMounts = 1;
constexpr u32 kTrainerTypeTradeskills = 2;
constexpr u32 kTrainerTypePets = 3;
// TrainingFailureReason (SharedDefines.h).
constexpr u32 kTrainFailUnavailable = 0;
constexpr u32 kTrainFailNotEnoughMoney = 1;
constexpr u32 kTrainFailNotEnoughSkill = 2;

struct TrainerList { // SMSG_TRAINER_LIST (WorldSession::SendTrainerList)
    struct Spell {
        u32 spellId = 0; // the trainer's spell: its first effect teaches the real one (EffectTriggerSpell)
        TrainerSpellState state = TrainerSpellState::Unavailable;
        u32 cost = 0; // copper, reputation discount applied
        bool primaryProfessionFirstRank = false;
        bool canLearnPrimaryProfession = false; // a free primary profession slot (the "learn" button works)
        u8 requiredLevel = 0;
        u32 requiredSkill = 0;
        u32 requiredSkillValue = 0;
        u32 requiredSpell = 0; // previous rank, or the additional spell the chain needs
        u32 requiredSpell2 = 0;
    };
    static constexpr u32 kMaxSpells = 512; // more is malformed
    u64 trainer = 0;
    u32 trainerType = kTrainerTypeClass;
    std::vector<Spell> spells;
    std::string_view greeting;
};
bool parseTrainerList(std::span<const u8> payload, TrainerList& out);
// SMSG_TRAINER_BUY_SUCCEEDED: trainer + spell (the trainer's spell id the client asked for).
bool parseTrainerBuySucceeded(std::span<const u8> payload, u64& trainer, u32& spellId);
// SMSG_TRAINER_BUY_FAILED: trainer + spell + kTrainFail* reason.
bool parseTrainerBuyFailed(std::span<const u8> payload, u64& trainer, u32& spellId, u32& reason);
// MSG_TALENT_WIPE_CONFIRM (server -> client): the trainer (0: "no talents to reset") and the cost in copper.
bool parseTalentWipeConfirm(std::span<const u8> payload, u64& trainer, u32& cost);

// -------------------------------------------------------------------------------------------------------
// Pets (server -> client).

// ActiveStates of a pet action button (Objects/UnitDefines.h): the high byte of its packed value.
constexpr u8 kPetActPassive = 0x01;
constexpr u8 kPetActDisabled = 0x81; // a castable spell, autocast off
constexpr u8 kPetActEnabled = 0xC1;  // autocast on
constexpr u8 kPetActCommand = 0x07;
constexpr u8 kPetActReaction = 0x06;
// CommandStates / ReactStates: the action of a command / reaction button.
constexpr u32 kPetCommandStay = 0;
constexpr u32 kPetCommandFollow = 1;
constexpr u32 kPetCommandAttack = 2;
constexpr u32 kPetCommandDismiss = 3;
constexpr u32 kPetReactPassive = 0;
constexpr u32 kPetReactDefensive = 1;
constexpr u32 kPetReactAggressive = 2;
constexpr u32 kPetActionBarSize = 10; // MAX_UNIT_ACTION_BAR_INDEX
constexpr u8 kPetDisabledFlag = 0x08; // the action bar is disabled (Pet::IsEnabled false)

constexpr u32 petButton(u32 action, u8 type) {
    return (action & 0x00FFFFFFu) | (static_cast<u32>(type) << 24);
}
constexpr u32 petButtonAction(u32 packed) {
    return packed & 0x00FFFFFFu;
}
constexpr u8 petButtonType(u32 packed) {
    return static_cast<u8>(packed >> 24);
}

struct PetSpells { // SMSG_PET_SPELLS (PetSpellInitialize / CharmSpellInitialize / PossessSpellInitialize)
    struct Cooldown {
        u32 spellId = 0;
        u16 category = 0;
        u32 cooldownMs = 0; // remaining own cooldown
        u32 categoryCooldownMs =
            0; // remaining category cooldown (bit 27 set: permanent, as the server writes it)
    };
    u64 pet = 0;        // 0: the pet is gone (RemovePetActionBar): nothing else follows
    u32 durationMs = 0; // charms and possessions: what is left of the effect (0: a pet)
    u8 reactState = 0;
    u8 commandState = 0;
    u8 flags = 0; // kPetDisabledFlag
    std::array<u32, kPetActionBarSize> bar{};
    std::vector<u32> spells; // the pet's spell book: packed buttons (spell | state << 24)
    std::vector<Cooldown> cooldowns;
};
bool parsePetSpells(std::span<const u8> payload, PetSpells& out);

struct PetMode { // SMSG_PET_MODE
    u64 pet = 0;
    u8 reactState = 0;
    u8 commandState = 0;
    u8 flags = 0; // kPetDisabledFlag
};
bool parsePetMode(std::span<const u8> payload, PetMode& out);

// PetActionFeedback (Objects/Pet.h).
constexpr u8 kPetFeedbackDead = 1;
constexpr u8 kPetFeedbackNothingToAttack = 2;
constexpr u8 kPetFeedbackCantAttackTarget = 3;
constexpr u8 kPetFeedbackNoPath = 4;
// SMSG_PET_ACTION_FEEDBACK: one kPetFeedback* byte.
bool parsePetActionFeedback(std::span<const u8> payload, u8& message);
// SMSG_PET_TAME_FAILURE: one PetTameFailureReason byte (SharedDefines.h).
bool parsePetTameFailure(std::span<const u8> payload, u8& reason);

struct PetCastFailed { // SMSG_PET_CAST_FAILED
    u32 spellId = 0;
    u8 status = 0; // SPELL_RESULT_STATUS_FAIL (2)
    u8 reason = 0; // SpellCastResult (net::spellCastResultName)
};
bool parsePetCastFailed(std::span<const u8> payload, PetCastFailed& out);

struct PetNameQueryResponse { // SMSG_PET_NAME_QUERY_RESPONSE
    u32 petNumber = 0;
    std::string_view name;
    u32 timestamp = 0;
};
bool parsePetNameQueryResponse(std::span<const u8> payload, PetNameQueryResponse& out);

// -------------------------------------------------------------------------------------------------------
// Reputation (server -> client).

constexpr u32 kFactionListSize = 64; // InitializeFactions::MAX_FACTION_COUNT
// FactionFlags (ReputationMgr.h).
constexpr u8 kFactionVisible = 0x01;
constexpr u8 kFactionAtWar = 0x02;
constexpr u8 kFactionHidden = 0x04;
constexpr u8 kFactionInvisibleForced = 0x08;
constexpr u8 kFactionPeaceForced = 0x10;
constexpr u8 kFactionInactive = 0x20;

struct InitializeFactions { // SMSG_INITIALIZE_FACTIONS: always 64 entries, by reputation list id
    struct Entry {
        u8 flags = 0;
        i32 standing = 0; // without the base reputation of Faction.dbc
    };
    u32 count = 0; // stored: at most kFactionListSize
    std::array<Entry, kFactionListSize> factions{};
};
bool parseInitializeFactions(std::span<const u8> payload, InitializeFactions& out);

struct FactionStanding { // SMSG_SET_FACTION_STANDING
    struct Entry {
        u32 listId = 0;
        i32 standing = 0;
    };
    static constexpr u32 kMaxEntries = 64;
    u32 count = 0;
    std::array<Entry, kMaxEntries> entries{};
};
bool parseFactionStanding(std::span<const u8> payload, FactionStanding& out);
// SMSG_SET_FACTION_VISIBLE: the reputation list id (net::parseU32).

// -------------------------------------------------------------------------------------------------------
// Client -> server bodies. A single full GUID (net::writeGuid): CMSG_TRAINER_LIST, MSG_TALENT_WIPE_CONFIRM,
// CMSG_PET_ABANDON, CMSG_PET_STOP_ATTACK. Empty: CMSG_CANCEL_AUTO_REPEAT_SPELL.

// CMSG_TRAINER_BUY_SPELL: trainer + the trainer's spell id.
void writeTrainerBuySpell(std::vector<u8>& out, u64 trainer, u32 spellId);
// CMSG_LEARN_TALENT: Talent.dbc id + the rank wanted (0-based).
void writeLearnTalent(std::vector<u8>& out, u32 talentId, u32 rank);
// CMSG_PET_ACTION: pet + packed button (petButton) + target (0: none).
void writePetAction(std::vector<u8>& out, u64 pet, u32 button, u64 target);
// CMSG_PET_SET_ACTION with one slot (the server ignores single moves of command / reaction buttons).
void writePetSetAction(std::vector<u8>& out, u64 pet, u32 position, u32 button);
// CMSG_PET_SPELL_AUTOCAST: pet + spell + on/off.
void writePetSpellAutocast(std::vector<u8>& out, u64 pet, u32 spellId, bool enabled);
// CMSG_PET_NAME_QUERY: UNIT_FIELD_PETNUMBER + the pet.
void writePetNameQuery(std::vector<u8>& out, u32 petNumber, u64 pet);
// CMSG_SET_FACTION_ATWAR: reputation list id + 1 at war / 0 at peace.
void writeSetFactionAtWar(std::vector<u8>& out, u32 listId, bool atWar);

} // namespace vd::net
