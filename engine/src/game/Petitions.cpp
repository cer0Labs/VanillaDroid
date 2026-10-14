#include "game/Petitions.h"

#include "game/Inventory.h"
#include "game/Social.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>

namespace vd::game {

using net::Opcode;
namespace uf = net::uf;

Petitions::Petitions(const EntityStore& entities, const Social& social, const Inventory& inventory)
    : m_entities(entities), m_social(social), m_inventory(inventory) {
    m_body.reserve(128);
    m_petition.name.reserve(32);
}

void Petitions::enterWorld(u64 player, u32 nowMs) {
    m_inWorld = true;
    m_player = player;
    m_nowMs = nowMs;
    m_registrar = GuildRegistrar{};
    m_petition = GuildPetition{};
    ++m_version;
}

void Petitions::leaveWorld() {
    m_inWorld = false;
    m_registrar = GuildRegistrar{};
    m_petition = GuildPetition{};
    ++m_version;
}

bool Petitions::sendBody(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

GuildNotice& Petitions::notice(const char* key, u64 guid) {
    GuildNotice& n = m_notices.push();
    n = GuildNotice{};
    n.key = key;
    n.guid = guid;
    n.timeMs = m_nowMs;
    return n;
}

bool Petitions::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::SMSG_PETITION_SHOWLIST:
    case Opcode::SMSG_PETITION_SHOW_SIGNATURES:
    case Opcode::SMSG_PETITION_SIGN_RESULTS:
    case Opcode::MSG_PETITION_DECLINE:
    case Opcode::SMSG_TURN_IN_PETITION_RESULTS:
    case Opcode::SMSG_PETITION_QUERY_RESPONSE:
    case Opcode::MSG_PETITION_RENAME: return true;
    default: return false;
    }
}

bool Petitions::handle(Opcode opcode, std::span<const u8> p, u32 nowMs) {
    m_nowMs = nowMs;
    switch (opcode) {
    case Opcode::SMSG_PETITION_SHOWLIST: {
        net::PetitionShowList m;
        if (!net::parsePetitionShowList(p, m)) {
            return false;
        }
        // WoW.exe 0x5EEB80: the frame opens only for the first entry with flag 1 (vMaNGOS: "must be &1 to
        // show it").
        if (m.count > 0 && (m.entries[0].flags & 1) != 0) {
            m_registrar.open = true;
            m_registrar.npc = m.npc;
            m_registrar.cost = static_cast<u32>(std::max(0, m.entries[0].cost));
            m_registrar.index = m.entries[0].index;
            ++m_version;
        }
        return true;
    }
    case Opcode::SMSG_PETITION_SHOW_SIGNATURES:
        if (!net::parsePetitionShowSignatures(p, m_signatures)) {
            return false;
        }
        onShowSignatures(m_signatures);
        return true;
    case Opcode::SMSG_PETITION_QUERY_RESPONSE: {
        net::PetitionQueryResponse m;
        if (!net::parsePetitionQueryResponse(p, m)) {
            return false;
        }
        if (m_petition.open && m.petition == m_petition.petition) {
            m_petition.name.assign(m.name);
            m_petition.charter = (m.flags & 1) != 0;
            m_petition.maxSignatures = std::min<u32>(m.maxSignatures, net::kPetitionSignaturesMax);
            ++m_version;
        }
        return true;
    }
    case Opcode::SMSG_PETITION_SIGN_RESULTS: {
        net::PetitionSignResult m;
        if (!net::parsePetitionSignResults(p, m)) {
            return false;
        }
        if (m.signer != m_player) {
            // WoW.exe 0x4F41C0: someone signed the owner's charter. Only while that charter is shown: the
            // signer is added (once) with ERR_PETITION_SIGNED_S, and the window updates (PETITION_SHOW).
            if (m_petition.open && m.item == m_petition.item) {
                const bool known =
                    std::find(m_petition.signers.begin(), m_petition.signers.begin() + m_petition.signatures,
                              m.signer) != m_petition.signers.begin() + m_petition.signatures;
                if (!known) {
                    addSigner(m.signer);
                    notice("ERR_PETITION_SIGNED_S", m.signer);
                }
            }
            return true;
        }
        switch (m.result) {
        case net::kPetitionSignOk:
            notice("ERR_PETITION_SIGNED");
            m_petition.signedIt = true;
            closePetition(); // PETITION_CLOSED
            break;
        case net::kPetitionAlreadySigned: notice("ERR_PETITION_ALREADY_SIGNED"); break;
        case net::kPetitionAlreadyInGuild: notice("ERR_PETITION_IN_GUILD"); break;
        case net::kPetitionCantSignOwn: notice("ERR_PETITION_CREATOR"); break;
        case net::kPetitionNotServer: notice("ERR_PETITION_NOT_SAME_SERVER"); break;
        default: notice("VD_GUILD_PETITION_ERROR"); break; // WoW.exe's hard-coded "Petition error"
        }
        return true;
    }
    case Opcode::MSG_PETITION_DECLINE: {
        u64 who = 0;
        if (!net::parseGuid(p, who)) {
            return false;
        }
        notice("ERR_PETITION_DECLINED_S", who); // WoW.exe 0x5EF0E0
        return true;
    }
    case Opcode::SMSG_TURN_IN_PETITION_RESULTS: {
        u32 result = 0;
        if (!net::parseU32(p, result)) {
            return false;
        }
        // WoW.exe 0x5EF150: done closes the registrar; nothing else is said (the guild's own events follow).
        if (result == net::kPetitionSignOk) {
            closeRegistrar();
        } else if (result == net::kPetitionAlreadyInGuild) {
            notice("ERR_PETITION_IN_GUILD");
        } else if (result == net::kPetitionNeedMore) {
            notice("ERR_PETITION_NOT_ENOUGH_SIGNATURES");
        } else {
            notice("VD_GUILD_PETITION_ERROR");
        }
        return true;
    }
    case Opcode::MSG_PETITION_RENAME: {
        net::PetitionRename m;
        if (!net::parsePetitionRename(p, m)) {
            return false;
        }
        if (m_petition.open && m.item == m_petition.item) {
            m_petition.name.assign(m.name);
            ++m_version;
        }
        return true;
    }
    default: return false;
    }
}

void Petitions::onShowSignatures(const net::PetitionSignatures& m) {
    const bool same = m_petition.open && m_petition.item == m.item;
    if (!same) {
        m_petition = GuildPetition{};
        m_petition.name.clear();
    }
    m_petition.open = true;
    m_petition.item = m.item;
    m_petition.owner = m.owner;
    m_petition.petition = m.petition;
    m_petition.signatures = 0;
    for (u32 i = 0; i < m.count; ++i) {
        addSigner(m.signers[i]);
    }
    // GetPetitionInfo needs the charter's name: asked for every time it opens (it may have been renamed).
    m_body.clear();
    net::writePetitionQuery(m_body, m.petition, m.item);
    sendBody(Opcode::CMSG_PETITION_QUERY);
    ++m_version;
}

void Petitions::addSigner(u64 guid) {
    if (m_petition.signatures < m_petition.signers.size()) {
        m_petition.signers[m_petition.signatures++] = guid;
        ++m_version;
    }
}

// --- The registrar
// ---------------------------------------------------------------------------------------------

const char* Petitions::guildNameError(std::string_view name) {
    if (name.empty()) {
        return "ERR_GUILD_ENTER_NAME";
    }
    if (name.front() == ' ' || name.back() == ' ') {
        return "ERR_GUILD_NAME_INVALID_SPACE";
    }
    u32 letters = 0;
    for (usize i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c == ' ') {
            if (i + 1 < name.size() && name[i + 1] == ' ') {
                return "ERR_GUILD_NAME_NAME_CONSECUTIVE_SPACES";
            }
            continue;
        }
        if (c >= 0x80) { // UTF-8: a lead byte counts one letter, continuation bytes none
            letters += (c & 0xC0) != 0x80 ? 1u : 0u;
            continue;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
            return "ERR_GUILD_NAME_INVALID";
        }
        ++letters;
    }
    if (letters < 2) {
        return "ERR_GUILD_NAME_TOO_SHORT";
    }
    if (letters > net::kGuildNameMax) {
        return "ERR_GUILD_NAME_INVALID";
    }
    return nullptr;
}

bool Petitions::buyCharter(std::string_view name, u32 nowMs) {
    m_nowMs = nowMs;
    if (!m_registrar.open || name.empty()) {
        return false;
    }
    // WoW.exe 0x4F5260 / 0x4F5050: the name first, then the guild, then the money.
    if (const char* error = guildNameError(name)) {
        notice(error);
        return false;
    }
    if (m_social.guild().id != 0) {
        notice("ERR_ALREADY_IN_GUILD");
        return false;
    }
    const Entity* me = m_entities.find(m_player);
    if (me && me->value(uf::PLAYER_FIELD_COINAGE) < m_registrar.cost) {
        notice("ERR_NOT_ENOUGH_MONEY");
        return false;
    }
    m_body.clear();
    net::writePetitionBuy(m_body, m_registrar.npc, name, m_registrar.index);
    return sendBody(Opcode::CMSG_PETITION_BUY);
}

u64 Petitions::carriedCharter() const {
    for (i32 container = kContainerBackpack; container <= kContainerLastBag; ++container) {
        const u32 size = m_inventory.containerSize(container);
        for (u32 i = 0; i < size; ++i) {
            const ItemInstance* item = m_inventory.containerItem(container, i);
            if (item && item->guid != 0 && item->entry == net::kGuildCharterEntry) {
                return item->guid;
            }
        }
    }
    return 0;
}

bool Petitions::turnInCharter() {
    const u64 charter = carriedCharter();
    if (charter == 0) {
        notice("ERR_NO_GUILD_CHARTER");
        return false;
    }
    m_body.clear();
    net::writeGuid(m_body, charter);
    return sendBody(Opcode::CMSG_TURN_IN_PETITION);
}

void Petitions::closeRegistrar() {
    if (m_registrar.open) {
        m_registrar = GuildRegistrar{};
        ++m_version;
    }
}

// --- The charter
// -----------------------------------------------------------------------------------------------

bool Petitions::showCharter(u64 item) {
    if (item == 0) {
        return false;
    }
    m_body.clear();
    net::writeGuid(m_body, item);
    return sendBody(Opcode::CMSG_PETITION_SHOW_SIGNATURES);
}

bool Petitions::canSign() const {
    const GuildPetition& p = m_petition;
    if (!p.open) {
        return false;
    }
    if (p.charter && (m_social.guild().id != 0 || p.signatures >= p.maxSignatures)) {
        return false;
    }
    if (p.owner == m_player) {
        return false;
    }
    for (u32 i = 0; i < p.signatures; ++i) {
        if (p.signers[i] == m_player) {
            return false;
        }
    }
    return true;
}

bool Petitions::sign() {
    if (!m_petition.open) {
        return false;
    }
    m_body.clear();
    net::writePetitionSign(m_body, m_petition.item);
    if (!sendBody(Opcode::CMSG_PETITION_SIGN)) {
        return false;
    }
    m_petition.signedIt = true; // WoW.exe 0x4F475D: no decline when it closes
    return true;
}

bool Petitions::offer(u64 target, bool friendly, u32 nowMs) {
    m_nowMs = nowMs;
    if (!m_petition.open || target == 0) {
        return false;
    }
    const Entity* other = m_entities.find(target);
    if (!other) {
        notice("ERR_OUT_OF_RANGE");
        return false;
    }
    if (other->type != ObjectType::Player) {
        return false;
    }
    if (target == m_player) {
        notice("ERR_PETITION_CREATOR");
        return false;
    }
    if (!friendly) {
        notice("ERR_GUILD_NOT_ALLIED");
        return false;
    }
    if (other->value(uf::PLAYER_GUILDID) != 0) {
        notice("ERR_ALREADY_IN_GUILD_S", target);
        return false;
    }
    m_body.clear();
    net::writeOfferPetition(m_body, m_petition.item, target);
    if (!sendBody(Opcode::CMSG_OFFER_PETITION)) {
        return false;
    }
    notice("ERR_PETITION_OFFERED_S", target);
    return true;
}

bool Petitions::rename(std::string_view name, u32 nowMs) {
    m_nowMs = nowMs;
    if (!m_petition.open) {
        return false;
    }
    if (const char* error = guildNameError(name)) {
        notice(error);
        return false;
    }
    m_body.clear();
    net::writePetitionRename(m_body, m_petition.item, name);
    return sendBody(Opcode::MSG_PETITION_RENAME);
}

void Petitions::closePetition() {
    if (!m_petition.open) {
        return;
    }
    if (!m_petition.signedIt && m_petition.owner != m_player) {
        m_body.clear();
        net::writeGuid(m_body, m_petition.item);
        sendBody(Opcode::MSG_PETITION_DECLINE);
    }
    m_petition = GuildPetition{};
    ++m_version;
}

} // namespace vd::game
