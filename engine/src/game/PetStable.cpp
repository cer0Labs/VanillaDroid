#include "game/PetStable.h"

#include "net/GameMessages.h"

#include <algorithm>
#include <cstring>

namespace vd::game {
namespace {

using net::Opcode;

void copyName(std::array<char, 64>& out, std::string_view name) {
    const usize n = std::min(name.size(), out.size() - 1);
    std::memcpy(out.data(), name.data(), n);
    out[n] = '\0';
}

} // namespace

void PetStable::enterWorld(u64 player, u32 nowMs) {
    m_inWorld = true;
    m_player = player;
    m_nowMs = nowMs;
    m_body.reserve(32);
    m_master = 0;
    m_picked = -1;
    m_unlearnPet = 0;
}

void PetStable::leaveWorld() {
    m_inWorld = false;
    m_master = 0;
    m_picked = -1;
    m_unlearnPet = 0;
}

bool PetStable::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::MSG_LIST_STABLED_PETS:
    case Opcode::SMSG_STABLE_RESULT:
    case Opcode::SMSG_PET_UNLEARN_CONFIRM: return true;
    default: return false;
    }
}

bool PetStable::handle(Opcode opcode, std::span<const u8> p, u32 nowMs) {
    m_nowMs = nowMs;
    switch (opcode) {
    case Opcode::MSG_LIST_STABLED_PETS:
        if (!net::parseStabledPetsList(p, m_list)) {
            return false;
        }
        onList(m_list);
        return true;
    case Opcode::SMSG_STABLE_RESULT: {
        u8 result = 0;
        if (!net::parseStableResult(p, result)) {
            return false;
        }
        onResult(result);
        return true;
    }
    case Opcode::SMSG_PET_UNLEARN_CONFIRM: {
        net::PetUnlearnConfirm m;
        if (!net::parsePetUnlearnConfirm(p, m)) {
            return false;
        }
        // UIParent: CONFIRM_PET_UNLEARN, the cost in the popup's money frame.
        m_unlearnPet = m.pet;
        m_unlearnCost = m.cost;
        m_unlearnTrainer = m_gossipNpc;
        event(GameEventKind::PetUnlearnOffer, 0, m.pet).value[0] = static_cast<i32>(m.cost);
        return true;
    }
    default: return true;
    }
}

void PetStable::onList(const net::StabledPetsList& m) {
    // WoW.exe 0x4CAB10: the stable master (a new one, or again: the selection is forgotten either way), the
    // bought slots, then each pet: the one flagged current goes to the first record, the others follow in
    // their order.
    m_master = m.stableMaster;
    m_slots = m.slots;
    m_selected = 0;
    m_current = StabledPet{};
    m_stabledCount = 0;
    for (u32 i = 0; i < m.count; ++i) {
        const net::StabledPetsList::Pet& in = m.pets[i];
        StabledPet* out = nullptr;
        if ((in.slot & 0x02) == 0) { // not a stable slot (2, 3): the current pet
            out = &m_current;
        } else if (m_stabledCount < m_stabled.size()) {
            out = &m_stabled[m_stabledCount++];
        } else {
            continue;
        }
        out->petNumber = in.petNumber;
        out->entry = in.entry;
        out->level = in.level;
        out->loyalty = in.loyalty;
        copyName(out->name, in.name);
    }
    for (u32 i = m_stabledCount; i < m_stabled.size(); ++i) {
        m_stabled[i] = StabledPet{};
    }
    if (m_picked > static_cast<i32>(m_stabledCount)) {
        m_picked = -1;
    }
    event(GameEventKind::StableOpened, 0, m_master); // PET_STABLE_SHOW
}

void PetStable::onResult(u8 result) {
    // WoW.exe 0x4CACB0: not enough money is said; a success asks for the list again (a bought slot counts at
    // once); the other failures say nothing.
    switch (result) {
    case net::kStableErrMoney: event(GameEventKind::StableFailed).value[0] = result; break;
    case net::kStableSuccessBuySlot: ++m_slots; [[fallthrough]];
    case net::kStableSuccessStable:
    case net::kStableSuccessUnstable:
        if (m_master != 0) {
            sendGuid(Opcode::MSG_LIST_STABLED_PETS, m_master);
        }
        break;
    case 0x0B: event(GameEventKind::StableUpdated); break; // PET_STABLE_UPDATE (vMaNGOS never sends it)
    default: break;
    }
}

const StabledPet* PetStable::pet(u32 index) const {
    const StabledPet* p = index == 0 ? &m_current : index <= m_stabledCount ? &m_stabled[index - 1] : nullptr;
    return p && p->petNumber != 0 ? p : nullptr;
}

i32 PetStable::selected() const {
    if (m_selected == ~0u) {
        return 0;
    }
    if (m_selected == 0) {
        return -1;
    }
    for (u32 i = 0; i < m_stabledCount; ++i) {
        if (m_stabled[i].petNumber == m_selected) {
            return static_cast<i32>(i) + 1;
        }
    }
    return -1;
}

i32 PetStable::autoSelect(bool petOut) {
    if (selected() != -1) {
        return selected();
    }
    if (petOut) {
        click(0, petOut);
    } else {
        for (u32 i = 0; i <= kStabledSlots; ++i) {
            if (pet(i)) {
                click(static_cast<i32>(i), petOut);
                break;
            }
        }
    }
    return selected();
}

bool PetStable::click(i32 index, bool petOut) {
    if (index < 0 || index > static_cast<i32>(kStabledSlots)) {
        return false;
    }
    if (m_picked < 0) {
        // Nothing on the cursor: select it (the current pet, a stabled one, or nothing for an empty slot).
        m_selected = index == 0                                  ? ~0u
                     : static_cast<u32>(index) <= m_stabledCount ? m_stabled[index - 1].petNumber
                                                                 : 0u;
        return true;
    }
    const i32 from = m_picked;
    m_picked = -1;
    if (from == index) {
        return true; // put back where it was
    }
    if (from == 0) {
        // The current pet onto a stable slot: swapped with the pet there, or stabled into an empty bought
        // slot.
        const u32 slot = static_cast<u32>(index) - 1;
        if (slot < m_stabledCount && m_stabled[slot].petNumber != 0) {
            sendPetNumber(Opcode::CMSG_STABLE_SWAP_PET, m_stabled[slot].petNumber);
        } else if (slot < m_slots) {
            sendGuid(Opcode::CMSG_STABLE_PET, m_master);
        }
    } else if (index == 0 && static_cast<u32>(from) <= m_stabledCount) {
        // A stabled pet onto the current slot: swapped with the pet out, or unstabled.
        const u32 number = m_stabled[from - 1].petNumber;
        if (number != 0) {
            sendPetNumber(petOut ? Opcode::CMSG_STABLE_SWAP_PET : Opcode::CMSG_UNSTABLE_PET, number);
        }
    }
    return true; // between two stable slots: nothing moves
}

void PetStable::pickup(i32 index) {
    if (index >= 0 && pet(static_cast<u32>(index))) {
        m_picked = index;
    }
}

bool PetStable::buySlot(u32 price, u32 money) {
    if (m_master == 0 || m_slots >= kStabledSlots || price == 0 || money < price) {
        return false;
    }
    return sendGuid(Opcode::CMSG_BUY_STABLE_SLOT, m_master);
}

void PetStable::close() {
    if (m_master == 0) {
        return;
    }
    m_master = 0;
    m_picked = -1;
    event(GameEventKind::StableClosed); // PET_STABLE_CLOSED
}

void PetStable::answerUnlearn(bool accept) {
    if (accept && m_unlearnPet != 0) {
        sendGuid(Opcode::CMSG_PET_UNLEARN, m_unlearnPet);
    }
    m_unlearnPet = 0;
}

bool PetStable::sendGuid(Opcode opcode, u64 guid) {
    m_body.clear();
    net::writeGuid(m_body, guid);
    return m_inWorld && m_sink(opcode, m_body);
}

bool PetStable::sendPetNumber(Opcode opcode, u32 petNumber) {
    m_body.clear();
    net::writeStablePetNumber(m_body, m_master, petNumber);
    return m_inWorld && m_sink(opcode, m_body);
}

GameEvent& PetStable::event(GameEventKind kind, u32 id, u64 guid) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = kind;
    e.timeMs = m_nowMs;
    e.id = id;
    e.guid = guid;
    return e;
}

} // namespace vd::game
