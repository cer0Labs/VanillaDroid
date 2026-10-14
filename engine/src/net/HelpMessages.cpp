#include "net/HelpMessages.h"

namespace vd::net {

namespace {

constexpr std::string_view kReserved = "Reserved for future use"; // WoW.exe 0x860708

// A string led by its length with the terminator (CMSG_BUG).
void sizedString(ByteWriter& w, std::string_view text) {
    w.u32v(static_cast<u32>(text.size() + 1)).cstring(text);
}

} // namespace

void writeGmTicketCreate(std::vector<u8>& out, const GmTicketCreate& ticket) {
    ByteWriter w(out);
    w.u8v(ticket.category).u32v(ticket.map).f32v(ticket.x).f32v(ticket.y).f32v(ticket.z);
    w.cstring(ticket.text).cstring(kReserved);
}

void writeGmTicketUpdateText(std::vector<u8>& out, u8 category, std::string_view text) {
    ByteWriter(out).u8v(category).cstring(text);
}

bool parseGmTicket(std::span<const u8> payload, GmTicket& out) {
    ByteReader r(payload);
    out = GmTicket{};
    out.status = r.u32v();
    if (r.ok() && out.status == kTicketStatusHasText) {
        out.text = r.cstring();
        out.category = r.u8v();
        out.ticketAge = r.f32v();
        out.oldestTicket = r.f32v();
        out.updateAge = r.f32v();
        out.escalation = r.u8v();
        out.openedByGm = r.u8v();
    }
    return r.ok();
}

void writeBug(std::vector<u8>& out, bool suggestion, std::string_view details, std::string_view text) {
    ByteWriter w(out);
    w.u32v(suggestion ? 1u : 0u);
    sizedString(w, details);
    sizedString(w, text);
}

void writeGmSurveySubmit(std::vector<u8>& out, u32 survey, std::span<const GmSurveyAnswer> answers,
                         std::string_view comment) {
    ByteWriter w(out);
    w.u32v(survey);
    u32 written = 0;
    for (const GmSurveyAnswer& a : answers) {
        if (written == kSurveyQuestionsMax || a.question == 0) {
            break;
        }
        w.u32v(a.question).u8v(a.rank).cstring(a.comment);
        ++written;
    }
    if (written < kSurveyQuestionsMax) {
        w.u32v(0); // the end of the list (GMSurveySubmit::ReadFromWorldPacket stops at a zero id)
    }
    w.cstring(comment);
}

} // namespace vd::net
