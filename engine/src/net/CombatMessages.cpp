#include "net/CombatMessages.h"

namespace vd::net {

// -------------------------------------------------------------------------------------------------------
// Trainers.

bool parseTrainerList(std::span<const u8> payload, TrainerList& out) {
    ByteReader r(payload);
    out.spells.clear();
    out.greeting = {};
    out.trainer = r.u64v();
    out.trainerType = r.u32v();
    const u32 count = r.u32v();
    // SendTrainerSpellHelper: 38 bytes a spell.
    if (r.failed() || count > TrainerList::kMaxSpells || usize{count} * 38 > r.remaining()) {
        return false;
    }
    out.spells.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        TrainerList::Spell s;
        s.spellId = r.u32v();
        const u8 state = r.u8v();
        if (state > static_cast<u8>(TrainerSpellState::Known)) {
            return false;
        }
        s.state = static_cast<TrainerSpellState>(state);
        s.cost = r.u32v();
        s.canLearnPrimaryProfession = r.u32v() != 0;
        s.primaryProfessionFirstRank = r.u32v() != 0;
        s.requiredLevel = r.u8v();
        s.requiredSkill = r.u32v();
        s.requiredSkillValue = r.u32v();
        s.requiredSpell = r.u32v();
        s.requiredSpell2 = r.u32v();
        r.u32v(); // always 0
        out.spells.push_back(s);
    }
    out.greeting = r.cstring();
    return r.ok();
}

bool parseTrainerBuySucceeded(std::span<const u8> payload, u64& trainer, u32& spellId) {
    ByteReader r(payload);
    trainer = r.u64v();
    spellId = r.u32v();
    return r.ok();
}

bool parseTrainerBuyFailed(std::span<const u8> payload, u64& trainer, u32& spellId, u32& reason) {
    ByteReader r(payload);
    trainer = r.u64v();
    spellId = r.u32v();
    reason = r.u32v();
    return r.ok();
}

bool parseTalentWipeConfirm(std::span<const u8> payload, u64& trainer, u32& cost) {
    ByteReader r(payload);
    trainer = r.u64v();
    cost = r.u32v();
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Pets.

bool parsePetSpells(std::span<const u8> payload, PetSpells& out) {
    ByteReader r(payload);
    out.spells.clear();
    out.cooldowns.clear();
    out.durationMs = 0;
    out.reactState = out.commandState = out.flags = 0;
    out.bar = {};
    out.pet = r.u64v();
    if (r.failed()) {
        return false;
    }
    if (out.pet == 0) {
        return r.atEnd(); // RemovePetActionBar: the GUID alone
    }
    out.durationMs = r.u32v();
    out.reactState = r.u8v();
    out.commandState = r.u8v();
    r.u8v(); // always 0
    out.flags = r.u8v();
    for (u32& button : out.bar) {
        button = r.u32v();
    }
    const u8 spellCount = r.u8v();
    if (r.failed() || usize{spellCount} * 4 > r.remaining()) {
        return false;
    }
    out.spells.reserve(spellCount);
    for (u32 i = 0; i < spellCount; ++i) {
        out.spells.push_back(r.u32v());
    }
    // WritePetSpellsCooldown: u16 count, then 14 bytes each.
    const u16 cooldownCount = r.u16v();
    if (r.failed() || usize{cooldownCount} * 14 > r.remaining()) {
        return false;
    }
    out.cooldowns.reserve(cooldownCount);
    for (u32 i = 0; i < cooldownCount; ++i) {
        PetSpells::Cooldown c;
        c.spellId = r.u32v();
        c.category = r.u16v();
        c.cooldownMs = r.u32v();
        c.categoryCooldownMs = r.u32v();
        out.cooldowns.push_back(c);
    }
    return r.ok();
}

bool parsePetMode(std::span<const u8> payload, PetMode& out) {
    ByteReader r(payload);
    out.pet = r.u64v();
    out.reactState = r.u8v();
    out.commandState = r.u8v();
    r.u8v(); // always 0
    out.flags = r.u8v();
    return r.ok();
}

bool parsePetActionFeedback(std::span<const u8> payload, u8& message) {
    ByteReader r(payload);
    message = r.u8v();
    return r.ok();
}

bool parsePetTameFailure(std::span<const u8> payload, u8& reason) {
    ByteReader r(payload);
    reason = r.u8v();
    return r.ok();
}

bool parsePetCastFailed(std::span<const u8> payload, PetCastFailed& out) {
    ByteReader r(payload);
    out.spellId = r.u32v();
    out.status = r.u8v();
    out.reason = r.u8v();
    return r.ok();
}

bool parsePetNameQueryResponse(std::span<const u8> payload, PetNameQueryResponse& out) {
    ByteReader r(payload);
    out.petNumber = r.u32v();
    out.name = r.cstring();
    out.timestamp = r.u32v();
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Reputation.

bool parseInitializeFactions(std::span<const u8> payload, InitializeFactions& out) {
    ByteReader r(payload);
    out.count = 0;
    const u32 count = r.u32v();
    if (r.failed() || count > kFactionListSize || usize{count} * 5 != r.remaining()) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        out.factions[i].flags = r.u8v();
        out.factions[i].standing = r.i32v();
    }
    for (u32 i = count; i < kFactionListSize; ++i) {
        out.factions[i] = {};
    }
    out.count = count;
    return r.ok();
}

bool parseFactionStanding(std::span<const u8> payload, FactionStanding& out) {
    ByteReader r(payload);
    out.count = 0;
    const u32 count = r.u32v();
    if (r.failed() || count > FactionStanding::kMaxEntries || usize{count} * 8 != r.remaining()) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        out.entries[i].listId = r.u32v();
        out.entries[i].standing = r.i32v();
    }
    out.count = count;
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Client -> server.

void writeTrainerBuySpell(std::vector<u8>& out, u64 trainer, u32 spellId) {
    ByteWriter(out).u64v(trainer).u32v(spellId);
}

void writeLearnTalent(std::vector<u8>& out, u32 talentId, u32 rank) {
    ByteWriter(out).u32v(talentId).u32v(rank);
}

void writePetAction(std::vector<u8>& out, u64 pet, u32 button, u64 target) {
    ByteWriter(out).u64v(pet).u32v(button).u64v(target);
}

void writePetSetAction(std::vector<u8>& out, u64 pet, u32 position, u32 button) {
    ByteWriter(out).u64v(pet).u32v(position).u32v(button);
}

void writePetSpellAutocast(std::vector<u8>& out, u64 pet, u32 spellId, bool enabled) {
    ByteWriter(out).u64v(pet).u32v(spellId).u8v(enabled ? 1 : 0);
}

void writePetNameQuery(std::vector<u8>& out, u32 petNumber, u64 pet) {
    ByteWriter(out).u32v(petNumber).u64v(pet);
}

void writeSetFactionAtWar(std::vector<u8>& out, u32 listId, bool atWar) {
    ByteWriter(out).u32v(listId).u8v(atWar ? 1 : 0);
}

} // namespace vd::net
