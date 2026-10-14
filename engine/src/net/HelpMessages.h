#pragma once

#include "core/Types.h"
#include "net/ByteBuffer.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

// Phase 15 (help): the GM ticket, bug report and GM survey packets, as vmangos-core 4641790 reads and writes
// them for build 5875 (Server/Packets/GmTicket.cpp, Handlers/GMTicketHandler.cpp, GMTicketMgr.h,
// Server/Packets/Misc.cpp Bug) and as WoW.exe 1.12.1 sends and reads them (NewGMTicket 0x5EF740,
// UpdateGMTicket 0x5EFAC0, the ticket answers 0x5E4432..0x5E466F, SMSG_GM_TICKET_STATUS_UPDATE 0x5E78F0,
// ReportBug 0x5AC3xx).
//
// Same rules as net/SocialMessages. Bodies covered elsewhere: CMSG_GMTICKET_GETTICKET,
// CMSG_GMTICKET_DELETETICKET and CMSG_GMTICKET_SYSTEMSTATUS (empty); SMSG_GMTICKET_CREATE,
// SMSG_GMTICKET_UPDATETEXT, SMSG_GMTICKET_DELETETICKET, SMSG_GMTICKET_SYSTEMSTATUS and
// SMSG_GM_TICKET_STATUS_UPDATE (one u32: parseU32()).
namespace vd::net {

// TicketType (SharedDefines.h): GMTicketCategory.dbc's ids, what the help frame's categories send.
constexpr u8 kTicketStuck = 1;
constexpr u8 kTicketHarassment = 2;
constexpr u8 kTicketCategories = 10; // GMTICKET_MAX - 1: the server drops anything above
constexpr u32 kTicketTextMax = 500;  // HelpFrameOpenTicketText letters (the client reads at most 2000 back)

// CMSG_GMTICKET_CREATE (GmTicketCreate::ReadFromWorldPacket): category, map, position, the text, then the
// "Reserved for future use" string WoW.exe writes. (Its harassment tickets append the chat log after it;
// vMaNGOS stops reading before, and this client sends none.)
struct GmTicketCreate {
    u8 category = 0;
    u32 map = 0;
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    std::string_view text;
};
void writeGmTicketCreate(std::vector<u8>& out, const GmTicketCreate& ticket);
// CMSG_GMTICKET_UPDATETEXT (GmTicketUpdateText::ReadFromWorldPacket): the category, then the new text.
void writeGmTicketUpdateText(std::vector<u8>& out, u8 category, std::string_view text);

// GMTicketResponse: SMSG_GMTICKET_CREATE / _UPDATETEXT / _DELETETICKET.
constexpr u32 kTicketAlreadyExists = 1; // ERR_TICKET_ALREADY_EXISTS
constexpr u32 kTicketCreated = 2;       // the client asks for its ticket again (CMSG_GMTICKET_GETTICKET)
constexpr u32 kTicketCreateError = 3;   // ERR_TICKET_CREATE_ERROR
constexpr u32 kTicketUpdated = 4;       // the client asks for its ticket again
constexpr u32 kTicketUpdateError = 5;   // ERR_TICKET_UPDATE_ERROR
constexpr u32 kTicketDeleted = 9;       // UPDATE_TICKET with no ticket; anything else: ERR_TICKET_DB_ERROR

// SMSG_GMTICKET_GETTICKET (GmTicketGetTicket::AppendBodyTo): a status; with GMTICKET_STATUS_HASTEXT the
// ticket.
constexpr u32 kTicketStatusHasText = 0x06; // GMTICKET_STATUS_HASTEXT
constexpr u32 kTicketStatusDefault =
    0x0A; // GMTICKET_STATUS_DEFAULT: no ticket (0: ERR_TICKET_DB_ERROR, then none)
struct GmTicket {
    u32 status = 0;
    std::string_view text;
    u8 category = 0;
    f32 ticketAge = 0.0f;    // days since the ticket was last changed
    f32 oldestTicket = 0.0f; // days: the age of the oldest open ticket
    f32 updateAge = 0.0f;    // days since the queue's data last changed (WoW.exe's arg5)
    u8 escalation = 0;       // 0 not assigned, 1 assigned, 2 in the escalation queue
    u8 openedByGm = 0;       // 1: a GM has read it
};
bool parseGmTicket(std::span<const u8> payload, GmTicket& out);

// SMSG_GMTICKET_SYSTEMSTATUS: GMTicketSystemStatus (0 disabled, 1 enabled) — WoW.exe's UPDATE_GM_STATUS
// argument.
constexpr u32 kTicketQueueDisabled = 0;
constexpr u32 kTicketQueueEnabled = 1;
// SMSG_GM_TICKET_STATUS_UPDATE (WoW.exe 0x5E78F0; vMaNGOS 4641790 never sends it): 1 the ticket changed (ask
// for it again), 2 it was closed (UPDATE_TICKET with none), 3 a GM survey is offered (GMSURVEY_DISPLAY).
constexpr u32 kTicketStatusUpdated = 1;
constexpr u32 kTicketStatusClosed = 2;
constexpr u32 kTicketStatusSurvey = 3;

// CMSG_BUG (Bug::ReadFromWorldPacket; WoW.exe's ReportBug / ReportSuggestion): u32 1 for a suggestion, then
// two strings each led by its length with the terminator: the player's details, then what the player wrote.
void writeBug(std::vector<u8>& out, bool suggestion, std::string_view details, std::string_view text);

// CMSG_GMSURVEY_SUBMIT (GMSurveySubmit::ReadFromWorldPacket): the survey, then per question its id, the rank
// (0 "not applicable", 1..5) and a comment; a question id 0 ends the list early (at most 10); then the
// comment.
struct GmSurveyAnswer {
    u32 question = 0; // GMSurveyQuestions.dbc
    u8 rank = 0;
    std::string_view comment;
};
constexpr u32 kSurveyQuestionsMax = 10; // MAX_SURVEY_QUESTIONS
void writeGmSurveySubmit(std::vector<u8>& out, u32 survey, std::span<const GmSurveyAnswer> answers,
                         std::string_view comment);

} // namespace vd::net
