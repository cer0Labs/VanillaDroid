#pragma once

#include "core/Types.h"
#include "game/TouchLayout.h"

#include <array>
#include <span>

namespace vd::game {

class GameData;
struct DeathState;
struct PetState;
struct SpellDetails;
struct SpellInfo;
class SpellText;

// Phase 20 (pets, docs/MOBILE_CONTROLS.md §6.8 and §5.2 rows 1-3): the rules of the touch cluster's pet
// button, of the layers that take the cluster over (another unit controlled, no control, death) and of Feed
// Pet's memory. Plain functions over the client's tables and what the server said; app/TouchPetScreens keeps
// the state and draws. The rules are vMaNGOS' (Spell::CheckCast / CheckCasterAuras, Unit::EffectSummonPet,
// HandlePetAction): the server decides, the client shows what it would decide.

// --- Pet actions in the cluster's slots (§6.8: Spell Lock or Seduction anywhere) ---------------------------

// A TouchActionKind::PetAction's id: below kTouchPetSlots, a slot of the pet bar (whatever the pet has there:
// the foundation's); a command or a reaction with these bits (the packing of net::petButton: CommandStates /
// ReactStates); else a pet's spell, of any rank (the pet out casts its own rank of it; nothing while another
// pet is out). Saved as the number ("pet 19647": Spell Lock).
constexpr u32 kTouchPetSlots = 10;             // net::kPetActionBarSize
constexpr u32 kTouchPetCommand = 0x07000000u;  // ACT_COMMAND << 24
constexpr u32 kTouchPetReaction = 0x06000000u; // ACT_REACTION << 24
enum class TouchPetKind : u8 { Slot, Spell, Command, Reaction };
struct TouchPetAction {
    TouchPetKind kind = TouchPetKind::Slot;
    u32 value = 0; // the slot, the spell, the command or the reaction
};
TouchPetAction decodeTouchPetAction(u32 id);
constexpr u32 touchPetCommand(u32 command) {
    return kTouchPetCommand | command;
}
constexpr u32 touchPetReaction(u32 react) {
    return kTouchPetReaction | react;
}

// The slot of the pet's bar that does `id` now (-1: none there): the slot itself, the command's or the
// reaction's button, the spell's button or, for a spell, the button of the same spell at another rank (same
// name: `text` may be null, then the very id only).
i32 touchPetSlot(const PetState& pet, u32 id, const SpellText* text);
// A spell of the pet's book (Abilities::petBook) of the same name as `spellId` (castPetSpell when no button
// holds it), 0 none.
u32 touchPetBookSpell(std::span<const u32> book, u32 spellId, const SpellText* text);

// --- The player's spells for pets --------------------------------------------------------------------------

// What a known spell does for the pet button and its panel, from its effects: Call Pet (SUMMON_PET without a
// creature: the hunter's pet from the database), Revive Pet (SUMMON_DEAD_PET), a demon (SUMMON_PET with a
// creature: Summon Imp...), Feed Pet (FEED_PET), Dismiss Pet (DISMISS_PET), or a spell aimed at the pet
// (TARGET_UNIT_CASTER_PET: Mend Pet, Health Funnel, Eyes of the Beast, Bestial Wrath, Dark Pact).
enum class PetSpellRole : u8 { None, CallPet, RevivePet, SummonDemon, FeedPet, DismissPet, OnPet };
PetSpellRole petSpellRole(const SpellInfo& spell);

// The player's pet spells by role (scanned when the known spells change). One spell per name (its highest
// rank, by level); the demons by the level they are learned at.
struct PetSpellBook {
    static constexpr u32 kMaxDemons = 8;
    static constexpr u32 kMaxOnPet = 6;
    u32 callPet = 0;
    u32 revivePet = 0;
    u32 feedPet = 0;
    u32 dismissPet = 0;
    std::array<u32, kMaxDemons> demons{};
    u32 demonCount = 0;
    std::array<u32, kMaxOnPet> onPet{};
    u32 onPetCount = 0;
    bool any() const { return callPet != 0 || revivePet != 0 || demonCount != 0; }
    bool knowsDemon(u32 spellId) const;
};
void scanPetSpells(std::span<const u32> known, const GameData& data, PetSpellBook& out);

// --- The pet button (§6.8) ---------------------------------------------------------------------------------

enum class PetButtonMode : u8 {
    Hidden,      // nothing to do: no pet, no spell to call one
    CallPet,     // the hunter's pet is away: Call Pet
    RevivePet,   // ...dead (lying there, or away dead): Revive Pet
    SummonDemon, // the warlock without a demon: the last one summoned (a hold: the fan of those known)
    Command,     // a pet is out: Attack the target, or Follow when it attacks it already (a hold: the panel)
};
struct PetButtonState {
    bool petOut = false;    // a pet or a charmed unit (Abilities::pet)
    bool petDead = false;   // ...lying dead
    bool knownDead = false; // no pet out and it died (seen dying, or the server's PETTAME_DEAD)
    const PetSpellBook* spells = nullptr;
    u32 lastDemon = 0; // TouchLayout::lastDemon
    u64 target = 0;    // the player's target when the pet may attack it (alive, attackable), else 0
    u64 petTarget = 0; // what the pet attacks now (its UNIT_FIELD_TARGET)
};
struct PetButtonChoice {
    PetButtonMode mode = PetButtonMode::Hidden;
    u32 spellId = 0;    // CallPet, RevivePet, SummonDemon: what a tap casts
    u32 command = 0;    // Command: net::kPetCommandAttack or net::kPetCommandFollow
    u64 target = 0;     // ...Attack's
    bool fan = false;   // a hold opens the demons' fan
    bool panel = false; // a hold opens the pet's panel
};
PetButtonChoice choosePetButton(const PetButtonState& s);
// The demon the button summons: the last one when it is still known, else the one learned last.
u32 chooseDemon(const PetSpellBook& spells, u32 lastDemon);

// --- Another unit controlled (§5.2 row 1: Mind Control, Eyes of the Beast, Eye of Kilrogg) -----------------

// The pet bar's slots the cluster's skill slots show while the player moves another unit (its
// SMSG_PET_SPELLS: a possessed unit's bar is the Attack command and its spells, sent as ACT_PASSIVE but cast
// like any other): Main the Attack command; the bar's spells on A, C, D, E, B, F, G, H in their order (the
// other commands and the reactions are of no use: the player moves it). -1: nothing there. `passive(spellId)`
// says which spells cannot be cast at all (SPELL_ATTR_PASSIVE).
using SpellTest = bool (*)(const void* context, u32 spellId);
std::array<i32, kTouchSkills> controlledSlots(const PetState& pet, SpellTest passive, const void* context);

// --- No control (§5.2 row 2) -------------------------------------------------------------------------------

// What keeps the player from acting, as vMaNGOS' Spell::CheckCasterAuras sees an aura: its stun, fear and
// confuse effects (SPELL_AURA_MOD_STUN / _MOD_FEAR / _MOD_CONFUSE), each with its mechanics (the spell's
// Mechanic and the effect's own), and the aura's school and dispel type.
struct ControlAura {
    static constexpr u8 kStun = 1, kFear = 2, kConfuse = 3;
    struct Effect {
        u8 kind = 0;
        u32 mechanics = 0; // bit (mechanic - 1)
    };
    std::array<Effect, 3> effects{};
    u32 count = 0;
    u32 schoolMask = 0; // 1 << School
    u32 dispel = 0;     // SpellDispelType (0 none)
};
// False when the aura controls nothing.
bool controlAura(const SpellDetails& aura, ControlAura& out);
// The immunities a spell grants that let it be cast through a control (SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT
// with SPELL_AURA_MECHANIC_IMMUNITY / _MECHANIC_IMMUNITY_MASK / _SCHOOL_IMMUNITY / _DISPEL_IMMUNITY: Will of
// the Forsaken, Berserker Rage, Blink, Divine Shield, Ice Block, a druid's forms against Polymorph...).
struct SpellImmunity {
    u32 mechanics = 0; // bit (mechanic - 1)
    u32 schools = 0;
    u32 dispels = 0; // bit dispel type
    bool any() const { return mechanics != 0 || schools != 0 || dispels != 0; }
};
SpellImmunity spellImmunity(const SpellDetails& spell);
// Whether a spell with `immunity` breaks `controls` (the player's): it can be cast through each of their
// effects (Spell::CheckCasterAuras) and takes them away (RemoveAurasAtMechanicImmunity, a school's or a
// dispel type's immunity): each is covered by its aura's school or dispel type, or by its mechanics. False
// without controls.
bool breaksControl(const SpellImmunity& immunity, std::span<const ControlAura> controls);

// --- Dead or a ghost (§5.2 row 3) --------------------------------------------------------------------------

// What the cluster offers instead of the skills, the same requests the popups send (DEATH, RECOVER_CORPSE,
// RESURRECT): Main releases the spirit (the body lying there) or brings the ghost back to its corpse when it
// is near it (RetrieveCorpse, once the corpse delay is over); the reactive place uses the soulstone or
// Reincarnation (PLAYER_SELF_RES_SPELL) while the body lies there; ring A accepts a resurrection offered.
enum class DeathAction : u8 { None, Release, Reclaim, SelfRes, AcceptRes };
struct DeathButtons {
    bool active = false; // dead or a ghost: the skills are hidden
    DeathAction main = DeathAction::None;
    u32 mainWaitMs = 0; // until it can be used (the corpse delay)
    DeathAction reactive = DeathAction::None;
    u32 selfResSpell = 0;
    DeathAction ringA = DeathAction::None;
    u32 ringAWaitMs = 0;
};
// `corpseInRange`: the ghost is within CORPSE_RECLAIM_RADIUS of its corpse on its map; `reclaimDelayMs`: what
// is left of the corpse delay (Gameplay::reclaimDelayRemaining).
DeathButtons deathButtons(const DeathState& death, bool corpseInRange, u32 reclaimDelayMs);

// --- Feed Pet (§6.8) ---------------------------------------------------------------------------------------

// The server's answer to a food (FEED_PET on it): accepted (the last one first), refused (left out from then
// on).
void rememberPetFood(TouchPetFood& memory, u32 family, u32 entry, bool accepted);
bool petFoodRefused(const TouchPetFood& memory, u32 family, u32 entry);
// Its place among the accepted ones (0: the last), -1 when it never was.
i32 petFoodAccepted(const TouchPetFood& memory, u32 family, u32 entry);

// A food candidate carried.
struct PetFoodItem {
    u32 entry = 0;
    u32 count = 0;
    u32 itemLevel = 0; // what the pet's happiness gains depends on (Pet::GetCurrentFoodBenefitLevel)
    bool food = false; // eaten by players too (an on-use Food spell); else a trade good (raw meat, raw fish)
};
// The candidates in the order the list shows them, the refused left out (returns how many remain at the
// front): those accepted before (the last first), then the foods, then the trade goods, the highest item
// level first.
u32 sortPetFood(const TouchPetFood& memory, u32 family, std::span<PetFoodItem> items);

} // namespace vd::game
