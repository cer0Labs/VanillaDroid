#include "game/TouchPet.h"

#include "game/Abilities.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "game/SpellText.h"
#include "net/CombatMessages.h"

#include <algorithm>

namespace vd::game {
namespace {

// vMaNGOS' SpellDefines.h / SpellAuraDefines.h (build 5875).
constexpr u32 kEffectSummonPet = 56; // SPELL_EFFECT_SUMMON_PET (misc: the creature; 0: the hunter's own pet)
constexpr u32 kEffectFeedPet = 101;  // SPELL_EFFECT_FEED_PET
constexpr u32 kEffectDismissPet = 102;            // SPELL_EFFECT_DISMISS_PET
constexpr u32 kEffectSummonDeadPet = 109;         // SPELL_EFFECT_SUMMON_DEAD_PET
constexpr u32 kTargetCasterPet = 5;               // TARGET_UNIT_CASTER_PET
constexpr u32 kAttrPassive = 0x40;                // SPELL_ATTR_PASSIVE
constexpr u32 kAttrHidden = 0x80;                 // SPELL_ATTR_DO_NOT_DISPLAY
constexpr u32 kAttrExImmunityPurges = 0x00008000; // SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT
constexpr u32 kAuraModConfuse = 5;
constexpr u32 kAuraModFear = 7;
constexpr u32 kAuraModStun = 12;
constexpr u32 kAuraSchoolImmunity = 39;
constexpr u32 kAuraDispelImmunity = 41;
constexpr u32 kAuraMechanicImmunity = 77;
constexpr u32 kAuraMechanicImmunityMask = 147;
constexpr u32 kDispelAll = 7;                                                 // DISPEL_ALL
constexpr u32 kDispelAllMask = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4); // magic, curse, disease, poison

constexpr u32 mechanicBit(u32 mechanic) {
    return mechanic != 0 && mechanic <= 32 ? 1u << (mechanic - 1) : 0u;
}

bool isCommand(u32 packed) {
    return net::petButtonType(packed) == net::kPetActCommand;
}
bool isReaction(u32 packed) {
    return net::petButtonType(packed) == net::kPetActReaction;
}
// A spell button (not a command, a reaction or an empty spell slot).
u32 buttonSpell(u32 packed) {
    return isCommand(packed) || isReaction(packed) ? 0u : net::petButtonAction(packed);
}

bool sameSpell(u32 a, u32 b, const SpellText* text) {
    if (a == b) {
        return true;
    }
    if (!text || a == 0 || b == 0) {
        return false;
    }
    const std::string_view na = text->name(a);
    return !na.empty() && na == text->name(b);
}

TouchPetFood::Family* foodFamily(TouchPetFood& memory, u32 family) {
    // The family to the front (the last used first); a new one takes the last place.
    auto& f = memory.families;
    u32 at = 0;
    while (at < f.size() && f[at].family != family && f[at].family != 0) {
        ++at;
    }
    if (at == f.size()) {
        at = static_cast<u32>(f.size()) - 1;
        f[at] = {};
    }
    TouchPetFood::Family moved = f[at];
    for (u32 i = at; i > 0; --i) {
        f[i] = f[i - 1];
    }
    f[0] = moved;
    f[0].family = family;
    return &f[0];
}

const TouchPetFood::Family* findFamily(const TouchPetFood& memory, u32 family) {
    for (const TouchPetFood::Family& f : memory.families) {
        if (family != 0 && f.family == family) {
            return &f;
        }
    }
    return nullptr;
}

// `entry` first in `list`, the others shifted down (the last falls off); taken out of it first when there.
template <usize N> void pushFront(std::array<u32, N>& list, u32 entry) {
    auto it = std::find(list.begin(), list.end(), entry);
    if (it == list.end()) {
        it = list.end() - 1;
    }
    std::move_backward(list.begin(), it, it + 1);
    list[0] = entry;
}
template <usize N> void remove(std::array<u32, N>& list, u32 entry) {
    auto it = std::find(list.begin(), list.end(), entry);
    if (it != list.end()) {
        std::move(it + 1, list.end(), it);
        list.back() = 0;
    }
}

} // namespace

// --- Pet actions -------------------------------------------------------------------------------------------

TouchPetAction decodeTouchPetAction(u32 id) {
    if (id < kTouchPetSlots) {
        return {TouchPetKind::Slot, id};
    }
    if ((id & 0xFF000000u) == kTouchPetCommand) {
        return {TouchPetKind::Command, id & 0x00FFFFFFu};
    }
    if ((id & 0xFF000000u) == kTouchPetReaction) {
        return {TouchPetKind::Reaction, id & 0x00FFFFFFu};
    }
    return {TouchPetKind::Spell, id};
}

i32 touchPetSlot(const PetState& pet, u32 id, const SpellText* text) {
    if (!pet.active()) {
        return -1;
    }
    const TouchPetAction a = decodeTouchPetAction(id);
    if (a.kind == TouchPetKind::Slot) {
        const u32 packed = pet.bar[a.value];
        return isCommand(packed) || isReaction(packed) || net::petButtonAction(packed) != 0
                   ? static_cast<i32>(a.value)
                   : -1;
    }
    for (u32 slot = 0; slot < pet.bar.size(); ++slot) {
        const u32 packed = pet.bar[slot];
        const u32 action = net::petButtonAction(packed);
        if ((a.kind == TouchPetKind::Command && isCommand(packed) && action == a.value) ||
            (a.kind == TouchPetKind::Reaction && isReaction(packed) && action == a.value) ||
            (a.kind == TouchPetKind::Spell && buttonSpell(packed) == a.value)) {
            return static_cast<i32>(slot);
        }
    }
    if (a.kind == TouchPetKind::Spell) { // another rank of it
        for (u32 slot = 0; slot < pet.bar.size(); ++slot) {
            if (sameSpell(buttonSpell(pet.bar[slot]), a.value, text)) {
                return static_cast<i32>(slot);
            }
        }
    }
    return -1;
}

u32 touchPetBookSpell(std::span<const u32> book, u32 spellId, const SpellText* text) {
    for (const u32 spell : book) {
        if (spell == spellId) {
            return spell;
        }
    }
    for (const u32 spell : book) {
        if (sameSpell(spell, spellId, text)) {
            return spell;
        }
    }
    return 0;
}

// --- The player's pet spells -------------------------------------------------------------------------------

PetSpellRole petSpellRole(const SpellInfo& spell) {
    for (u32 i = 0; i < 3; ++i) {
        switch (spell.effects[i]) {
        case kEffectSummonPet:
            return spell.effectMisc[i] != 0 ? PetSpellRole::SummonDemon : PetSpellRole::CallPet;
        case kEffectSummonDeadPet: return PetSpellRole::RevivePet;
        case kEffectFeedPet: return PetSpellRole::FeedPet;
        case kEffectDismissPet: return PetSpellRole::DismissPet;
        default: break;
        }
    }
    if ((spell.attributes & (kAttrPassive | kAttrHidden)) == 0 && spell.effects[0] != 0 &&
        spell.implicitTargetA[0] == kTargetCasterPet) {
        return PetSpellRole::OnPet;
    }
    return PetSpellRole::None;
}

bool PetSpellBook::knowsDemon(u32 spellId) const {
    return spellId != 0 &&
           std::find(demons.begin(), demons.begin() + demonCount, spellId) != demons.begin() + demonCount;
}

void scanPetSpells(std::span<const u32> known, const GameData& data, PetSpellBook& out) {
    out = {};
    std::array<u32, PetSpellBook::kMaxDemons> demonLevels{};
    std::array<u32, PetSpellBook::kMaxOnPet> onPetLevels{};
    std::array<std::string_view, PetSpellBook::kMaxOnPet> onPetNames{};
    for (const u32 id : known) {
        const std::optional<SpellInfo> info = data.spell(id);
        if (!info) {
            continue;
        }
        switch (petSpellRole(*info)) {
        case PetSpellRole::CallPet: out.callPet = id; break;
        case PetSpellRole::RevivePet: out.revivePet = id; break;
        case PetSpellRole::FeedPet: out.feedPet = id; break;
        case PetSpellRole::DismissPet: out.dismissPet = id; break;
        case PetSpellRole::SummonDemon:
            if (out.demonCount < out.demons.size() && !out.knowsDemon(id)) {
                // By the level it is learned at (Imp, Voidwalker, Succubus, Felhunter).
                u32 at = out.demonCount++;
                while (at > 0 && demonLevels[at - 1] > info->baseLevel) {
                    out.demons[at] = out.demons[at - 1];
                    demonLevels[at] = demonLevels[at - 1];
                    --at;
                }
                out.demons[at] = id;
                demonLevels[at] = info->baseLevel;
            }
            break;
        case PetSpellRole::OnPet: {
            // One per name: its highest rank.
            u32 at = 0;
            while (at < out.onPetCount && onPetNames[at] != info->name) {
                ++at;
            }
            if (at == out.onPetCount) {
                if (out.onPetCount == out.onPet.size()) {
                    break;
                }
                ++out.onPetCount;
            } else if (onPetLevels[at] > info->baseLevel) {
                break;
            }
            out.onPet[at] = id;
            onPetLevels[at] = info->baseLevel;
            onPetNames[at] = info->name;
            break;
        }
        default: break;
        }
    }
}

// --- The pet button ----------------------------------------------------------------------------------------

u32 chooseDemon(const PetSpellBook& spells, u32 lastDemon) {
    if (spells.knowsDemon(lastDemon)) {
        return lastDemon;
    }
    return spells.demonCount > 0 ? spells.demons[spells.demonCount - 1] : 0u;
}

PetButtonChoice choosePetButton(const PetButtonState& s) {
    PetButtonChoice c;
    const PetSpellBook empty;
    const PetSpellBook& spells = s.spells ? *s.spells : empty;
    if (s.petOut && !s.petDead) {
        // §6.8: attack the target; when the pet attacks it already, call it back (Follow).
        c.mode = PetButtonMode::Command;
        c.panel = true;
        if (s.target != 0 && s.petTarget != s.target) {
            c.command = net::kPetCommandAttack;
            c.target = s.target;
        } else {
            c.command = net::kPetCommandFollow;
        }
        return c;
    }
    // Revive Pet brings back a pet that lies dead (Unit::EffectSummonPet refuses Call Pet: PETTAME_DEAD) or
    // that died and went away (its corpse despawned, the hunter moved away: it stays dead in the database).
    if ((s.petDead || s.knownDead) && spells.revivePet != 0) {
        c.mode = PetButtonMode::RevivePet;
        c.spellId = spells.revivePet;
        return c;
    }
    if (s.petOut) {
        return c; // a dead pet the player cannot revive (a demon's corpse goes away at once)
    }
    if (spells.callPet != 0) {
        c.mode = PetButtonMode::CallPet;
        c.spellId = spells.callPet;
        return c;
    }
    if (const u32 demon = chooseDemon(spells, s.lastDemon); demon != 0) {
        c.mode = PetButtonMode::SummonDemon;
        c.spellId = demon;
        c.fan = spells.demonCount > 1;
    }
    return c;
}

// --- Another unit controlled -------------------------------------------------------------------------------

std::array<i32, kTouchSkills> controlledSlots(const PetState& pet, SpellTest passive, const void* context) {
    std::array<i32, kTouchSkills> out;
    out.fill(-1);
    if (!pet.active()) {
        return out;
    }
    // §7.1's roles: Main the most pressed (the Attack), then A, C, D, E, B, F, G, H.
    static constexpr TouchPlace kOrder[] = {TouchPlace::RingA,   TouchPlace::RingC,  TouchPlace::RingD,
                                            TouchPlace::RingE,   TouchPlace::RingB,  TouchPlace::ColumnF,
                                            TouchPlace::ColumnG, TouchPlace::ColumnH};
    u32 n = 0;
    for (u32 slot = 0; slot < pet.bar.size(); ++slot) {
        const u32 packed = pet.bar[slot];
        if (isCommand(packed)) {
            if (net::petButtonAction(packed) == net::kPetCommandAttack && out[0] < 0) {
                out[0] = static_cast<i32>(slot);
            }
            continue;
        }
        const u32 spell = buttonSpell(packed);
        if (spell == 0 || (passive && passive(context, spell)) || n >= std::size(kOrder)) {
            continue;
        }
        out[touchIndex(kOrder[n++])] = static_cast<i32>(slot);
    }
    return out;
}

// --- No control --------------------------------------------------------------------------------------------

bool controlAura(const SpellDetails& aura, ControlAura& out) {
    out = {};
    out.schoolMask = aura.school < 32 ? 1u << aura.school : 0u;
    out.dispel = aura.dispel;
    for (const SpellEffectData& e : aura.effects) {
        const u8 kind = e.aura == kAuraModStun      ? ControlAura::kStun
                        : e.aura == kAuraModFear    ? ControlAura::kFear
                        : e.aura == kAuraModConfuse ? ControlAura::kConfuse
                                                    : 0;
        if (kind != 0) {
            out.effects[out.count++] = {kind, mechanicBit(aura.mechanic) | mechanicBit(e.mechanic)};
        }
    }
    return out.count > 0;
}

SpellImmunity spellImmunity(const SpellDetails& spell) {
    SpellImmunity out;
    if ((spell.attributesEx & kAttrExImmunityPurges) == 0) {
        return out;
    }
    for (const SpellEffectData& e : spell.effects) {
        const u32 misc = static_cast<u32>(e.miscValue);
        switch (e.aura) {
        case kAuraMechanicImmunity: out.mechanics |= mechanicBit(misc); break;
        case kAuraMechanicImmunityMask: out.mechanics |= misc; break;
        case kAuraSchoolImmunity: out.schools |= misc; break;
        case kAuraDispelImmunity:
            out.dispels |= misc == kDispelAll ? kDispelAllMask : (misc < 32 ? 1u << misc : 0u);
            break;
        default: break;
        }
    }
    return out;
}

bool breaksControl(const SpellImmunity& immunity, std::span<const ControlAura> controls) {
    if (controls.empty() || !immunity.any()) {
        return false;
    }
    // Spell::CheckCasterAuras lets the spell through the control effects its immunities skip: the aura's
    // school or dispel type, the effect's mechanics (it also lets through an effect of another mechanic when
    // the spell is immune to what the effect does, a fear for Will of the Forsaken under Death Coil's horror:
    // cast, but the horror stays, as RemoveAurasAtMechanicImmunity only takes the mechanic's auras away; that
    // one is not shown as breaking it).
    for (const ControlAura& c : controls) {
        if ((c.schoolMask & immunity.schools) != 0 ||
            ((c.dispel < 32 ? 1u << c.dispel : 0u) & immunity.dispels) != 0) {
            continue;
        }
        for (u32 i = 0; i < c.count; ++i) {
            if ((c.effects[i].mechanics & immunity.mechanics) == 0) {
                return false;
            }
        }
    }
    return true;
}

// --- Dead or a ghost ---------------------------------------------------------------------------------------

DeathButtons deathButtons(const DeathState& death, bool corpseInRange, u32 reclaimDelayMs) {
    DeathButtons b;
    if (!death.dead && !death.ghost) {
        return b;
    }
    b.active = true;
    if (!death.ghost) {
        b.main = DeathAction::Release; // RepopMe
        if (death.selfResSpell != 0) {
            b.reactive = DeathAction::SelfRes; // UseSoulstone
            b.selfResSpell = death.selfResSpell;
        }
    } else if (corpseInRange) {
        b.main = DeathAction::Reclaim; // RetrieveCorpse
        b.mainWaitMs = reclaimDelayMs;
    }
    if (death.resurrector != 0) {
        b.ringA =
            DeathAction::AcceptRes; // AcceptResurrect, after the corpse delay when the offer waits for it
        b.ringAWaitMs = death.resurrectDelayed ? reclaimDelayMs : 0;
    }
    return b;
}

// --- Feed Pet ----------------------------------------------------------------------------------------------

void rememberPetFood(TouchPetFood& memory, u32 family, u32 entry, bool accepted) {
    if (family == 0 || entry == 0) {
        return;
    }
    TouchPetFood::Family& f = *foodFamily(memory, family);
    if (accepted) {
        remove(f.refused, entry);
        pushFront(f.accepted, entry);
    } else {
        remove(f.accepted, entry);
        pushFront(f.refused, entry);
    }
}

bool petFoodRefused(const TouchPetFood& memory, u32 family, u32 entry) {
    const TouchPetFood::Family* f = findFamily(memory, family);
    return f && entry != 0 && std::find(f->refused.begin(), f->refused.end(), entry) != f->refused.end();
}

i32 petFoodAccepted(const TouchPetFood& memory, u32 family, u32 entry) {
    const TouchPetFood::Family* f = findFamily(memory, family);
    if (!f || entry == 0) {
        return -1;
    }
    const auto it = std::find(f->accepted.begin(), f->accepted.end(), entry);
    return it == f->accepted.end() ? -1 : static_cast<i32>(it - f->accepted.begin());
}

u32 sortPetFood(const TouchPetFood& memory, u32 family, std::span<PetFoodItem> items) {
    const auto kept = std::partition(items.begin(), items.end(), [&](const PetFoodItem& item) {
        return item.entry != 0 && !petFoodRefused(memory, family, item.entry);
    });
    std::sort(items.begin(), kept, [&](const PetFoodItem& a, const PetFoodItem& b) {
        const i32 ra = petFoodAccepted(memory, family, a.entry),
                  rb = petFoodAccepted(memory, family, b.entry);
        if ((ra >= 0) != (rb >= 0)) {
            return ra >= 0;
        }
        if (ra >= 0 && ra != rb) {
            return ra < rb;
        }
        if (a.food != b.food) {
            return a.food;
        }
        if (a.itemLevel != b.itemLevel) {
            return a.itemLevel > b.itemLevel;
        }
        return a.entry < b.entry;
    });
    return static_cast<u32>(kept - items.begin());
}

} // namespace vd::game
