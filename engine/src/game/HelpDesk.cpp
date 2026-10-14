#include "game/HelpDesk.h"

#include "game/GameData.h"
#include "net/GameMessages.h"

#include <algorithm>

namespace vd::game {

using net::Opcode;

namespace {

constexpr u32 kCheckIntervalMs = 600 * 1000; // GMTICKET_CHECK_INTERVAL
constexpr u32 kSpellEffectStuck = 84;        // SPELL_EFFECT_STUCK
constexpr f32 kSecondsPerDay = 24.0f * 60.0f * 60.0f;

} // namespace

HelpDesk::HelpDesk() {
    m_body.reserve(600);
    m_ticket.text.reserve(64);
}

void HelpDesk::enterWorld(u32 nowMs) {
    m_inWorld = true;
    m_nowMs = nowMs;
    m_ticket = GmTicketState{};
    m_surveyOffered = false;
    m_queueActive = true;
    m_queueAlert = false;
    ++m_version;
    requestTicket(); // TicketStatusFrame_OnEvent: PLAYER_ENTERING_WORLD -> GetGMTicket()
}

void HelpDesk::leaveWorld() {
    m_inWorld = false;
}

void HelpDesk::update(u32 nowMs) {
    m_nowMs = nowMs;
    if (m_inWorld && m_ticket.open && static_cast<i32>(nowMs - m_refreshAtMs) >= 0) {
        requestTicket();
    }
}

bool HelpDesk::sendBody(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

bool HelpDesk::send(Opcode opcode) {
    m_body.clear();
    return sendBody(opcode);
}

GuildNotice& HelpDesk::notice(const char* key) {
    GuildNotice& n = m_notices.push();
    n = GuildNotice{};
    n.key = key;
    n.timeMs = m_nowMs;
    return n;
}

bool HelpDesk::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::SMSG_GMTICKET_CREATE:
    case Opcode::SMSG_GMTICKET_UPDATETEXT:
    case Opcode::SMSG_GMTICKET_GETTICKET:
    case Opcode::SMSG_GMTICKET_DELETETICKET:
    case Opcode::SMSG_GMTICKET_SYSTEMSTATUS:
    case Opcode::SMSG_GM_TICKET_STATUS_UPDATE: return true;
    default: return false;
    }
}

void HelpDesk::noTicket() {
    // UPDATE_TICKET with no ticket: the status frame hides, the ticket page offers a new one.
    m_ticket = GmTicketState{};
    m_ticket.known = true;
    m_ticket.receivedMs = m_nowMs;
    ++m_version;
}

bool HelpDesk::handle(Opcode opcode, std::span<const u8> p, u32 nowMs) {
    m_nowMs = nowMs;
    switch (opcode) {
    case Opcode::SMSG_GMTICKET_CREATE:
    case Opcode::SMSG_GMTICKET_UPDATETEXT: {
        u32 response = 0;
        if (!net::parseU32(p, response)) {
            return false;
        }
        // WoW.exe 0x5E4432: 1 already exists, 3 create error, 5 update error; 2 and 4 ask for the ticket
        // again.
        switch (response) {
        case net::kTicketAlreadyExists: notice("ERR_TICKET_ALREADY_EXISTS"); break;
        case net::kTicketCreated:
        case net::kTicketUpdated: requestTicket(); break;
        case net::kTicketCreateError: notice("ERR_TICKET_CREATE_ERROR"); break;
        case net::kTicketUpdateError: notice("ERR_TICKET_UPDATE_ERROR"); break;
        default: break;
        }
        return true;
    }
    case Opcode::SMSG_GMTICKET_GETTICKET: {
        net::GmTicket m;
        if (!net::parseGmTicket(p, m)) {
            return false;
        }
        if (m.status != net::kTicketStatusHasText) {
            if (m.status == 0) {
                notice("ERR_TICKET_DB_ERROR");
            }
            noTicket();
            return true;
        }
        m_ticket.known = true;
        m_ticket.open = m.category != 0;
        m_ticket.category = m.category;
        m_ticket.text.assign(m.text);
        m_ticket.ticketAge = m.ticketAge > 0.0f ? m.ticketAge : 0.0f; // WoW.exe clamps the age at zero
        m_ticket.oldestTicket = m.oldestTicket;
        m_ticket.updateAge = m.updateAge;
        m_ticket.escalation = m.escalation;
        m_ticket.openedByGm = m.openedByGm;
        m_ticket.receivedMs = nowMs;
        m_refreshAtMs = nowMs + kCheckIntervalMs;
        ++m_version;
        return true;
    }
    case Opcode::SMSG_GMTICKET_DELETETICKET: {
        u32 response = 0;
        if (!net::parseU32(p, response)) {
            return false;
        }
        if (response == net::kTicketDeleted) {
            noTicket();
        } else {
            notice("ERR_TICKET_DB_ERROR");
        }
        return true;
    }
    case Opcode::SMSG_GMTICKET_SYSTEMSTATUS: {
        u32 status = 0;
        if (!net::parseU32(p, status)) {
            return false;
        }
        // HelpFrame_OnEvent: 1 active; anything else not, and -1 (never from vMaNGOS) says so at once.
        m_queueActive = status == net::kTicketQueueEnabled;
        m_queueAlert = static_cast<i32>(status) == -1;
        ++m_version;
        return true;
    }
    case Opcode::SMSG_GM_TICKET_STATUS_UPDATE: {
        u32 status = 0;
        if (!net::parseU32(p, status)) {
            return false;
        }
        if (status == net::kTicketStatusUpdated) {
            requestTicket();
        } else if (status == net::kTicketStatusClosed) {
            noTicket();
        } else if (status == net::kTicketStatusSurvey) {
            m_surveyOffered = true; // GMSURVEY_DISPLAY
            m_ticket.open = false;  // HelpFrameOpenTicket.hasTicket = nil
            ++m_version;
        }
        return true;
    }
    default: return false;
    }
}

bool HelpDesk::requestTicket() {
    m_refreshAtMs = m_nowMs + kCheckIntervalMs;
    return send(Opcode::CMSG_GMTICKET_GETTICKET);
}

bool HelpDesk::requestStatus() {
    return send(Opcode::CMSG_GMTICKET_SYSTEMSTATUS);
}

bool HelpDesk::takeQueueDisabledAlert() {
    const bool alert = m_queueAlert;
    m_queueAlert = false;
    return alert;
}

bool HelpDesk::create(u8 category, std::string_view text, u32 map, f32 x, f32 y, f32 z) {
    if (text.empty()) {
        notice("ERR_TICKET_NO_TEXT"); // WoW.exe 0x5EF80C
        return false;
    }
    net::GmTicketCreate t;
    t.category = category;
    t.map = map;
    t.x = x;
    t.y = y;
    t.z = z;
    t.text = text.substr(0, net::kTicketTextMax);
    m_body.clear();
    net::writeGmTicketCreate(m_body, t);
    return sendBody(Opcode::CMSG_GMTICKET_CREATE);
}

bool HelpDesk::updateText(u8 category, std::string_view text) {
    if (text.empty()) {
        notice("ERR_TICKET_NO_TEXT");
        return false;
    }
    m_body.clear();
    net::writeGmTicketUpdateText(m_body, category, text.substr(0, net::kTicketTextMax));
    return sendBody(Opcode::CMSG_GMTICKET_UPDATETEXT);
}

bool HelpDesk::remove() {
    return send(Opcode::CMSG_GMTICKET_DELETETICKET);
}

TicketWait HelpDesk::waitText(const GmTicketState& t, u32 nowMs) {
    // HelpFrameOpenTicket_OnEvent's status text, and TicketStatus_OnUpdate's countdown of the wait.
    TicketWait out;
    if (!t.open) {
        return out;
    }
    if (t.openedByGm == 1) {
        out.key = t.escalation == 2 ? "GM_TICKET_ESCALATED" : "GM_TICKET_SERVICE_SOON";
        return out;
    }
    f32 wait = (t.oldestTicket - t.ticketAge) * kSecondsPerDay;
    wait = std::max(wait, 0.0f);
    if (t.oldestTicket < 0.0f || t.updateAge < 0.0f || t.updateAge > 0.042f) {
        out.key = "GM_TICKET_UNAVAILABLE";
    } else if (wait > 7200.0f) {
        out.key = "GM_TICKET_HIGH_VOLUME";
    } else if (wait > 300.0f) {
        out.key = "GM_TICKET_WAIT_TIME";
        const f32 elapsed = static_cast<f32>(nowMs - t.receivedMs) / 1000.0f;
        out.seconds = static_cast<u32>(std::max(wait - elapsed, 0.0f));
    } else {
        out.key = "GM_TICKET_SERVICE_SOON";
    }
    return out;
}

bool HelpDesk::submitSurvey(u32 survey, std::span<const net::GmSurveyAnswer> answers,
                            std::string_view comment) {
    m_body.clear();
    net::writeGmSurveySubmit(m_body, survey, answers, comment);
    if (!sendBody(Opcode::CMSG_GMSURVEY_SUBMIT)) {
        return false;
    }
    m_surveyOffered = false; // TicketStatusFrame.hasGMSurvey = nil
    ++m_version;
    return true;
}

bool HelpDesk::report(bool suggestion, std::string_view details, std::string_view text) {
    if (text.empty()) {
        return false;
    }
    m_body.clear();
    net::writeBug(m_body, suggestion, details, text);
    return sendBody(Opcode::CMSG_BUG);
}

u32 HelpDesk::stuckSpell(std::span<const u32> known, const SpellSource& source) {
    SpellInfo info;
    for (const u32 spell : known) {
        if (source.get(spell, info) && info.effects[0] == kSpellEffectStuck) {
            return spell;
        }
    }
    return 0;
}

} // namespace vd::game
