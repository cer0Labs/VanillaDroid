#include "game/Abilities.h"

#include "net/MiscMessages.h"
#include "net/PetMessages.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>

namespace vd::game {
namespace {

using net::Opcode;
namespace uf = net::uf;

constexpr u32 kPetNameRetryMs = 5000; // a name query that got no answer is asked again after this

} // namespace

// -------------------------------------------------------------------------------------------------------
// Trainer window, pet, reputation list.

void TrainerWindow::onList(const net::TrainerList& m) {
    m_trainer = m.trainer;
    m_type = m.trainerType;
    m_greeting.assign(m.greeting);
    m_services.assign(m.spells.begin(), m.spells.end());
    m_lastBought = 0;
}

void TrainerWindow::onBought(u32 spellId) {
    for (net::TrainerList::Spell& s : m_services) {
        if (s.spellId == spellId) {
            s.state = net::TrainerSpellState::Known;
        }
    }
    m_lastBought = spellId;
}

u32 PetState::slotSpell(u32 slot) const {
    if (slot >= bar.size()) {
        return 0;
    }
    const u8 type = net::petButtonType(bar[slot]);
    if (type == net::kPetActCommand || type == net::kPetActReaction) {
        return 0;
    }
    return net::petButtonAction(bar[slot]);
}

bool PetState::slotAutocast(u32 slot) const {
    return slot < bar.size() && net::petButtonType(bar[slot]) == net::kPetActEnabled && slotSpell(slot) != 0;
}

void ReputationList::reset() {
    m_initialized = false;
    m_entries.fill(Entry{});
}

void ReputationList::onInitialize(const net::InitializeFactions& m) {
    m_entries.fill(Entry{});
    for (u32 i = 0; i < m.count && i < m_entries.size(); ++i) {
        m_entries[i].flags = m.factions[i].flags;
        m_entries[i].standing = m.factions[i].standing;
    }
    m_initialized = true;
}

i32 ReputationList::onStanding(u32 listId, i32 standing) {
    if (listId >= m_entries.size()) {
        return 0;
    }
    const i32 change = m_initialized ? standing - m_entries[listId].standing : 0;
    m_entries[listId].standing = standing;
    return change;
}

void ReputationList::onVisible(u32 listId) {
    if (listId < m_entries.size()) {
        m_entries[listId].flags |= net::kFactionVisible;
    }
}

void ReputationList::setAtWar(u32 listId, bool atWar) {
    if (listId >= m_entries.size()) {
        return;
    }
    if (atWar) {
        m_entries[listId].flags |= net::kFactionAtWar;
    } else {
        m_entries[listId].flags &= static_cast<u8>(~net::kFactionAtWar);
    }
}

void ReputationList::setInactive(u32 listId, bool inactive) {
    if (listId >= m_entries.size()) {
        return;
    }
    if (inactive) {
        m_entries[listId].flags |= net::kFactionInactive;
    } else {
        m_entries[listId].flags &= static_cast<u8>(~net::kFactionInactive);
    }
}

// -------------------------------------------------------------------------------------------------------
// Session.

Abilities::Abilities(const EntityStore& entities, Events& events) : m_entities(entities), m_events(events) {
    m_body.reserve(64);
    m_petCooldowns.reserve(16);
    m_petName.reserve(32);
    m_petBook.reserve(32);
}

void Abilities::enterWorld(u64 player, bool sameCharacter, u32 nowMs) {
    m_inWorld = true;
    m_player = player;
    m_nowMs = nowMs;
    m_trainer.close();
    m_wipeTrainer = 0;
    m_autoRepeatSpell = 0;
    m_autoRepeatTarget = 0;
    if (!sameCharacter) {
        m_pet = PetState{};
        m_petBook.clear();
        m_petCooldowns.clear();
        m_petName.clear();
        m_petNameNumber = m_petNameAsked = 0;
        m_reputation.reset();
    }
}

void Abilities::leaveWorld() {
    m_inWorld = false;
    m_trainer.close();
    m_wipeTrainer = 0;
    m_autoRepeatSpell = 0;
    m_petNameAsked = 0; // asked again in the next world
}

bool Abilities::sendBody(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

bool Abilities::sendGuid(Opcode opcode, u64 guid) {
    m_body.clear();
    net::writeGuid(m_body, guid);
    return sendBody(opcode);
}

GameEvent& Abilities::event(GameEventKind kind, u32 id, u64 guid) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = kind;
    e.timeMs = m_nowMs;
    e.id = id;
    e.guid = guid;
    return e;
}

// -------------------------------------------------------------------------------------------------------
// Packets.

bool Abilities::handles(Opcode opcode) {
    switch (opcode) {
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
    case Opcode::SMSG_CANCEL_AUTO_REPEAT: return true;
    default: return false;
    }
}

bool Abilities::handle(Opcode opcode, std::span<const u8> p, u32 nowMs) {
    m_nowMs = nowMs;
    bool ok = true;
    switch (opcode) {
    case Opcode::SMSG_TRAINER_LIST:
        ok = net::parseTrainerList(p, m_trainerList);
        if (ok) {
            m_trainer.onList(m_trainerList);
        }
        break;
    case Opcode::SMSG_TRAINER_BUY_SUCCEEDED: {
        u64 trainer = 0;
        u32 spellId = 0;
        ok = net::parseTrainerBuySucceeded(p, trainer, spellId);
        if (ok) {
            if (trainer == m_trainer.trainer()) {
                m_trainer.onBought(spellId);
            }
            event(GameEventKind::TrainerBuySucceeded, spellId, trainer);
        }
        break;
    }
    case Opcode::SMSG_TRAINER_BUY_FAILED: {
        u64 trainer = 0;
        u32 spellId = 0, reason = 0;
        ok = net::parseTrainerBuyFailed(p, trainer, spellId, reason);
        if (ok) {
            event(GameEventKind::TrainerBuyFailed, spellId, trainer).value[0] = static_cast<i32>(reason);
        }
        break;
    }
    case Opcode::MSG_TALENT_WIPE_CONFIRM: {
        u64 trainer = 0;
        u32 cost = 0;
        ok = net::parseTalentWipeConfirm(p, trainer, cost);
        if (ok) {
            m_wipeTrainer = trainer; // 0: "you have not spent any talent points"
            m_wipeCost = cost;
            event(GameEventKind::TalentWipeOffer, 0, trainer).value[0] = static_cast<i32>(cost);
        }
        break;
    }
    case Opcode::SMSG_PET_SPELLS:
        ok = net::parsePetSpells(p, m_petSpells);
        if (ok) {
            onPetSpells(m_petSpells, nowMs);
        }
        break;
    case Opcode::SMSG_PET_MODE: {
        net::PetMode m;
        ok = net::parsePetMode(p, m);
        if (ok && m.pet == m_pet.guid) {
            m_pet.reactState = m.reactState;
            m_pet.commandState = m.commandState;
            m_pet.enabled = (m.flags & net::kPetDisabledFlag) == 0;
        }
        break;
    }
    case Opcode::SMSG_PET_ACTION_FEEDBACK: {
        u8 message = 0;
        ok = net::parsePetActionFeedback(p, message);
        if (ok) {
            event(GameEventKind::PetFeedback, 0, m_pet.guid).value[0] = message;
        }
        break;
    }
    case Opcode::SMSG_PET_CAST_FAILED: {
        net::PetCastFailed m;
        ok = net::parsePetCastFailed(p, m);
        if (ok) {
            event(GameEventKind::PetCastFailed, m.spellId, m_pet.guid).value[0] = m.reason;
        }
        break;
    }
    case Opcode::SMSG_PET_TAME_FAILURE: {
        u8 reason = 0;
        ok = net::parsePetTameFailure(p, reason);
        if (ok) {
            event(GameEventKind::PetTameFailed).value[0] = reason;
        }
        break;
    }
    case Opcode::SMSG_PET_NAME_QUERY_RESPONSE: {
        net::PetNameQueryResponse m;
        ok = net::parsePetNameQueryResponse(p, m);
        if (ok && m.petNumber == m_petNameAsked) {
            m_petName.assign(m.name);
            m_petNameNumber = m.petNumber;
        }
        break;
    }
    case Opcode::SMSG_PET_BROKEN: // an unhappy hunter pet ran away
        event(GameEventKind::PetFeedback, 0, m_pet.guid).value[0] = 0;
        break;
    case Opcode::SMSG_INITIALIZE_FACTIONS:
        ok = net::parseInitializeFactions(p, m_factions);
        if (ok) {
            m_reputation.onInitialize(m_factions);
        }
        break;
    case Opcode::SMSG_SET_FACTION_STANDING:
        ok = net::parseFactionStanding(p, m_standing);
        if (ok) {
            for (u32 i = 0; i < m_standing.count; ++i) {
                const net::FactionStanding::Entry& s = m_standing.entries[i];
                const i32 change = m_reputation.onStanding(s.listId, s.standing);
                if (change != 0) {
                    GameEvent& e = event(GameEventKind::ReputationChanged, s.listId);
                    e.value[0] = change;
                    e.value[1] = s.standing;
                }
            }
        }
        break;
    case Opcode::SMSG_SET_FACTION_VISIBLE: {
        u32 listId = 0;
        ok = net::parseU32(p, listId);
        if (ok) {
            m_reputation.onVisible(listId);
            event(GameEventKind::FactionVisible, listId);
        }
        break;
    }
    case Opcode::SMSG_SET_FACTION_ATWAR: { // phase 13: a reputation creature attacked us, or that war ended
        net::FactionAtWar m;
        ok = net::parseFactionAtWar(p, m);
        if (ok) {
            m_reputation.setAtWar(m.listId, m.atWar);
        }
        break;
    }
    case Opcode::SMSG_CANCEL_AUTO_REPEAT:
        m_autoRepeatSpell = 0;
        m_autoRepeatTarget = 0;
        break;
    default: return true;
    }
    return ok;
}

void Abilities::onPetSpells(const net::PetSpells& m, u32 nowMs) {
    const u64 before = m_pet.guid;
    if (m.pet == 0) {
        m_pet = PetState{};
        m_petBook.clear();
        m_petCooldowns.clear();
    } else {
        m_pet.guid = m.pet;
        m_pet.durationMs = m.durationMs;
        m_pet.sinceMs = nowMs;
        m_pet.reactState = m.reactState;
        m_pet.commandState = m.commandState;
        m_pet.enabled = (m.flags & net::kPetDisabledFlag) == 0;
        m_pet.bar = m.bar;
        m_pet.spells.assign(m.spells.begin(), m.spells.end());
        if (before != m.pet) {
            m_petCooldowns.clear();
        }
        for (const net::PetSpells::Cooldown& c : m.cooldowns) {
            const u32 category = c.categoryCooldownMs & 0x07FFFFFFu; // bit 27: permanent
            startPetCooldown(c.spellId, std::max(c.cooldownMs, category), nowMs);
        }
        buildPetBook(); // phase 15 (pets)
    }
    if (before != m_pet.guid) {
        event(GameEventKind::PetChanged, 0, m_pet.guid);
    }
}

void Abilities::buildPetBook() {
    // WoW.exe 0x4BD990 -> 0x4B2F90: each spell of the packet (its low 16 bits) unless Spell.dbc hides it
    // (attributes 0x80), then 0x4B2FD0's sort (0x4B30C0: the player's spell tab first, the same for every pet
    // spell; the name; the rank).
    m_petBook.clear();
    for (const u32 packed : m_pet.spells) {
        const u32 spellId = net::petButtonAction(packed) & 0xFFFFu;
        SpellInfo info;
        if (spellId == 0 || (m_source.get(spellId, info) && (info.attributes & 0x80) != 0)) {
            continue;
        }
        if (std::find(m_petBook.begin(), m_petBook.end(), spellId) == m_petBook.end()) {
            m_petBook.push_back(spellId);
        }
    }
    const auto rankOf = [](std::string_view rank) {
        u32 value = 0;
        for (const char c : rank) {
            if (c >= '0' && c <= '9') {
                value = value * 10 + static_cast<u32>(c - '0');
            }
        }
        return value;
    };
    std::sort(m_petBook.begin(), m_petBook.end(), [&](u32 a, u32 b) {
        SpellInfo ia, ib;
        const bool ka = m_source.get(a, ia), kb = m_source.get(b, ib);
        if (!ka || !kb) {
            return ka != kb ? ka : a < b; // unknown spells (no data) last
        }
        if (ia.name != ib.name) {
            return ia.name < ib.name;
        }
        return rankOf(ia.rank) < rankOf(ib.rank);
    });
}

u8 Abilities::petSpellState(u32 spellId) const {
    for (const u32 packed : m_pet.spells) {
        if ((net::petButtonAction(packed) & 0xFFFFu) == spellId) {
            return net::petButtonType(packed);
        }
    }
    return 0;
}

bool Abilities::setPetSpellAutocast(u32 spellId, bool enabled) {
    const u8 state = petSpellState(spellId);
    if (!m_pet.active() || (state != net::kPetActEnabled && state != net::kPetActDisabled)) {
        return false; // GetSpellAutocast: only what the book allows (passive spells cannot)
    }
    m_body.clear();
    net::writePetSpellAutocast(m_body, m_pet.guid, spellId, enabled);
    if (!sendBody(Opcode::CMSG_PET_SPELL_AUTOCAST)) {
        return false;
    }
    // The server keeps the book and the bar in step silently (Pet::ToggleAutocast,
    // CharmInfo::SetSpellAutocast).
    const u8 type = enabled ? net::kPetActEnabled : net::kPetActDisabled;
    for (u32& packed : m_pet.spells) {
        if ((net::petButtonAction(packed) & 0xFFFFu) == spellId) {
            packed = net::petButton(net::petButtonAction(packed), type);
        }
    }
    for (u32& button : m_pet.bar) {
        const u8 t = net::petButtonType(button);
        if ((t == net::kPetActEnabled || t == net::kPetActDisabled) &&
            net::petButtonAction(button) == spellId) {
            button = net::petButton(spellId, type);
        }
    }
    return true;
}

bool Abilities::castPetSpell(u32 spellId, u64 target) {
    const u8 state = petSpellState(spellId);
    if (!m_pet.active() || state == 0 || state == net::kPetActPassive) {
        return false;
    }
    m_body.clear();
    net::writePetAction(m_body, m_pet.guid, net::petButton(spellId, state), target);
    return sendBody(Opcode::CMSG_PET_ACTION);
}

bool Abilities::swapPetSlots(u32 from, u32 to) {
    if (!m_pet.active() || !m_pet.enabled || from >= m_pet.bar.size() || to >= m_pet.bar.size() ||
        from == to) {
        return false;
    }
    // HandlePetSetAction: each position gets the button that goes there; a command or reaction button may
    // only move this way (a pair whose other half holds its old place).
    const u32 a = m_pet.bar[from], b = m_pet.bar[to];
    m_body.clear();
    net::writePetSetActionPair(m_body, m_pet.guid, from, b, to, a);
    if (!sendBody(Opcode::CMSG_PET_SET_ACTION)) {
        return false;
    }
    m_pet.bar[from] = b;
    m_pet.bar[to] = a;
    return true;
}

void Abilities::startPetCooldown(u32 spellId, u32 durationMs, u32 nowMs) {
    std::erase_if(m_petCooldowns, [&](const PetCooldown& c) {
        return c.spellId == spellId || nowMs - c.startMs >= c.durationMs;
    });
    if (durationMs > 0) {
        m_petCooldowns.push_back({spellId, nowMs, durationMs});
    }
}

void Abilities::onSpellGo(const net::SpellGo& m, u32 nowMs) {
    const u64 caster = m.unitCaster ? m.unitCaster : m.caster;
    if (caster == 0 || caster != m_pet.guid) {
        return;
    }
    SpellInfo info;
    if (m_source.get(m.spellId, info)) {
        const u32 recovery = std::max(info.recoveryMs, info.category ? info.categoryRecoveryMs : 0u);
        if (recovery > 0) {
            startPetCooldown(m.spellId, recovery, nowMs);
        }
    }
}

void Abilities::onCastResult(const net::CastResult& m) {
    if (m.status == net::kCastStatusFailed && m.spellId == m_autoRepeatSpell) {
        m_autoRepeatSpell = 0; // refused: not running
        m_autoRepeatTarget = 0;
    }
}

// -------------------------------------------------------------------------------------------------------
// Requests.

void Abilities::requestTrainerList(u64 npc) {
    sendGuid(Opcode::CMSG_TRAINER_LIST, npc);
}

bool Abilities::buyTrainerSpell(u32 spellId) {
    if (!m_trainer.open()) {
        return false;
    }
    m_body.clear();
    net::writeTrainerBuySpell(m_body, m_trainer.trainer(), spellId);
    return sendBody(Opcode::CMSG_TRAINER_BUY_SPELL);
}

void Abilities::answerTalentWipe(bool accept) {
    if (accept && m_wipeTrainer != 0) {
        sendGuid(Opcode::MSG_TALENT_WIPE_CONFIRM, m_wipeTrainer);
    }
    m_wipeTrainer = 0;
}

bool Abilities::learnTalent(u32 talentId, u32 rank) {
    if (talentId == 0) {
        return false;
    }
    m_body.clear();
    net::writeLearnTalent(m_body, talentId, rank);
    return sendBody(Opcode::CMSG_LEARN_TALENT);
}

std::string_view Abilities::petName() {
    const Entity* pet = m_pet.guid ? m_entities.find(m_pet.guid) : nullptr;
    if (!pet) {
        return {};
    }
    const u32 number = pet->value(uf::UNIT_FIELD_PETNUMBER);
    if (number == 0) {
        return {}; // charmed creatures have no pet name: the caller shows the creature's
    }
    if (number == m_petNameNumber) {
        return m_petName;
    }
    if (number != m_petNameAsked || m_nowMs - m_petNameAskedMs > kPetNameRetryMs) {
        m_body.clear();
        net::writePetNameQuery(m_body, number, m_pet.guid);
        if (sendBody(Opcode::CMSG_PET_NAME_QUERY)) {
            m_petNameAsked = number;
            m_petNameAskedMs = m_nowMs;
        }
    }
    return {};
}

bool Abilities::petAction(u32 slot, u64 target) {
    if (!m_pet.active() || slot >= m_pet.bar.size()) {
        return false;
    }
    const u32 button = m_pet.bar[slot];
    const u8 type = net::petButtonType(button);
    const u32 action = net::petButtonAction(button);
    if (type == net::kPetActCommand) {
        return petCommand(action, target);
    }
    if (type == net::kPetActReaction) {
        return petReact(action);
    }
    if (action == 0) {
        return false;
    }
    m_body.clear();
    net::writePetAction(m_body, m_pet.guid, button, target);
    return sendBody(Opcode::CMSG_PET_ACTION);
}

bool Abilities::petCommand(u32 command, u64 target) {
    if (!m_pet.active()) {
        return false;
    }
    m_body.clear();
    net::writePetAction(m_body, m_pet.guid, net::petButton(command, net::kPetActCommand),
                        command == net::kPetCommandAttack ? target : 0);
    if (!sendBody(Opcode::CMSG_PET_ACTION)) {
        return false;
    }
    if (command != net::kPetCommandAttack && command != net::kPetCommandDismiss) {
        m_pet.commandState = static_cast<u8>(command); // the server keeps it silently
    }
    return true;
}

bool Abilities::petReact(u32 react) {
    if (!m_pet.active()) {
        return false;
    }
    m_body.clear();
    net::writePetAction(m_body, m_pet.guid, net::petButton(react, net::kPetActReaction), 0);
    if (!sendBody(Opcode::CMSG_PET_ACTION)) {
        return false;
    }
    m_pet.reactState = static_cast<u8>(react);
    return true;
}

bool Abilities::setPetAutocast(u32 slot, bool enabled) {
    const u32 spellId = m_pet.slotSpell(slot);
    if (spellId == 0) {
        return false;
    }
    m_body.clear();
    net::writePetSpellAutocast(m_body, m_pet.guid, spellId, enabled);
    if (!sendBody(Opcode::CMSG_PET_SPELL_AUTOCAST)) {
        return false;
    }
    m_pet.bar[slot] = net::petButton(spellId, enabled ? net::kPetActEnabled : net::kPetActDisabled);
    for (u32& packed : m_pet.spells) {
        if (net::petButtonAction(packed) == spellId) {
            packed = net::petButton(spellId, enabled ? net::kPetActEnabled : net::kPetActDisabled);
        }
    }
    return true;
}

bool Abilities::setPetSlot(u32 slot, u32 spellId) {
    if (!m_pet.active() || slot >= m_pet.bar.size()) {
        return false;
    }
    u8 type = net::kPetActDisabled;
    for (const u32 packed : m_pet.spells) {
        if (net::petButtonAction(packed) == spellId) {
            type = net::petButtonType(packed);
        }
    }
    const u32 button = net::petButton(spellId, type);
    m_body.clear();
    net::writePetSetAction(m_body, m_pet.guid, slot, button);
    if (!sendBody(Opcode::CMSG_PET_SET_ACTION)) {
        return false;
    }
    m_pet.bar[slot] = button;
    return true;
}

bool Abilities::dismissPet() {
    return petCommand(net::kPetCommandDismiss, 0);
}

bool Abilities::abandonPet() {
    return m_pet.active() && sendGuid(Opcode::CMSG_PET_ABANDON, m_pet.guid);
}

PetCooldown Abilities::petCooldown(u32 spellId, u32 nowMs) const {
    for (const PetCooldown& c : m_petCooldowns) {
        if (c.spellId == spellId && nowMs - c.startMs < c.durationMs) {
            return c;
        }
    }
    return PetCooldown{};
}

u32 Abilities::petCooldownRemaining(u32 spellId, u32 nowMs) const {
    const PetCooldown c = petCooldown(spellId, nowMs);
    return c.durationMs ? c.durationMs - (nowMs - c.startMs) : 0;
}

bool Abilities::setAtWar(u32 listId, bool atWar) {
    const ReputationList::Entry& e = m_reputation.entry(listId);
    if ((e.flags & net::kFactionPeaceForced) && atWar) {
        return false; // the server refuses wars with the player's own side
    }
    m_body.clear();
    net::writeSetFactionAtWar(m_body, listId, atWar);
    if (!sendBody(Opcode::CMSG_SET_FACTION_ATWAR)) {
        return false;
    }
    m_reputation.setAtWar(listId, atWar);
    return true;
}

void Abilities::startedAutoRepeat(u32 spellId, u64 target) {
    m_autoRepeatSpell = spellId;
    m_autoRepeatTarget = target;
}

void Abilities::cancelAutoRepeat() {
    if (m_autoRepeatSpell == 0) {
        return;
    }
    m_body.clear();
    sendBody(Opcode::CMSG_CANCEL_AUTO_REPEAT_SPELL);
    m_autoRepeatSpell = 0;
    m_autoRepeatTarget = 0;
}

} // namespace vd::game
