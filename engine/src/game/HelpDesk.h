#pragma once

#include "core/Types.h"
#include "game/GameEvents.h"
#include "game/GuildNotice.h"
#include "game/Spells.h"
#include "net/HelpMessages.h"
#include "net/Opcodes.gen.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::game {

// The player's GM ticket as UPDATE_TICKET tells it to HelpFrame.lua and TicketStatusFrame (WoW.exe 0x5E44E5:
// the arguments of SMSG_GMTICKET_GETTICKET).
struct GmTicketState {
    bool known = false; // an answer came (GetGMTicket)
    bool open = false;  // the player has a ticket (UPDATE_TICKET's arg1 ~= 0)
    u8 category = 0;    // GMTicketCategory.dbc id (arg1)
    std::string text;   // arg2
    f32 ticketAge = 0.0f, oldestTicket = 0.0f, updateAge = 0.0f; // days (arg3, arg4, arg5)
    u8 escalation = 0;                                           // arg6
    u8 openedByGm = 0;                                           // arg7
    u32 receivedMs = 0; // when it came: the wait counts down from then (TicketStatus_OnUpdate's ticketTimer)
};

// TicketStatusFrame's second line (HelpFrameOpenTicket_OnEvent): the client string's key and, for
// GM_TICKET_WAIT_TIME, the seconds left (SecondsToTime).
struct TicketWait {
    const char* key = nullptr;
    u32 seconds = 0;
};

// Phase 15 (help): the GM help desk of 1.12 (HelpFrame.lua, TicketStatusFrame, Blizzard_GMSurveyUI and the C
// side behind them): the player's ticket (GetGMTicket at login and every 10 minutes while one is open,
// NewGMTicket, UpdateGMTicket, DeleteGMTicket) with WoW.exe's answers (0x5E4432..: ERR_TICKET_* lines, the
// ticket asked for again after a change), whether GM help is available (GetGMStatus: UPDATE_GM_STATUS), the
// GM survey the server may offer (SMSG_GM_TICKET_STATUS_UPDATE 3: GMSURVEY_DISPLAY; GMSurveySubmit), "Stuck"
// (Stuck(): the player's SPELL_EFFECT_STUCK spell, WoW.exe 0x489370) and the suggestion frame's reports
// (ReportBug / ReportSuggestion: CMSG_BUG). Packets as vmangos-core 4641790 handles them (net/HelpMessages).
// Main thread.
class HelpDesk {
public:
    HelpDesk();

    void setSink(PacketSink sink) { m_sink = sink; }
    // PLAYER_ENTERING_WORLD: TicketStatusFrame asks for the ticket.
    void enterWorld(u32 nowMs);
    void leaveWorld();
    // TicketStatus_OnUpdate: every GMTICKET_CHECK_INTERVAL (600 s) while a ticket is open, GetGMTicket again.
    void update(u32 nowMs);
    static bool handles(net::Opcode opcode);
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs); // false when malformed
    const GuildNotices& notices() const { return m_notices; }
    u32 version() const {
        return m_version;
    } // bumps with every change of the ticket, the queue or the survey

    // --- The ticket ---
    const GmTicketState& ticket() const { return m_ticket; }
    bool requestTicket(); // GetGMTicket: CMSG_GMTICKET_GETTICKET
    bool requestStatus(); // GetGMStatus: CMSG_GMTICKET_SYSTEMSTATUS (HelpFrame_OnShow)
    // PETITION_QUEUE_ACTIVE: until the server says otherwise it is (HelpFrame.lua starts with 1).
    bool queueActive() const { return m_queueActive; }
    // UPDATE_GM_STATUS with -1 shows HELP_TICKET_QUEUE_DISABLED (vMaNGOS sends 0 or 1): read once.
    bool takeQueueDisabledAlert();
    // NewGMTicket(category, text): empty text is ERR_TICKET_NO_TEXT; `map` / `x, y, z`: where the player
    // stands.
    bool create(u8 category, std::string_view text, u32 map, f32 x, f32 y, f32 z);
    bool updateText(u8 category, std::string_view text); // UpdateGMTicket
    bool remove();                                       // DeleteGMTicket
    // The status frame's second line now (`nowMs`: the wait counts down).
    static TicketWait waitText(const GmTicketState& ticket, u32 nowMs);

    // --- The GM survey ---
    bool surveyOffered() const { return m_surveyOffered; } // TicketStatusFrame.hasGMSurvey
    // GMSurveyAnswerSubmit for each question, GMSurveyCommentSubmit, GMSurveySubmit: CMSG_GMSURVEY_SUBMIT.
    bool submitSurvey(u32 survey, std::span<const net::GmSurveyAnswer> answers, std::string_view comment);
    void dropSurvey() {
        m_surveyOffered = false;
        ++m_version;
    }

    // --- Bugs and suggestions ---
    // ReportBug(text) / ReportSuggestion(text): CMSG_BUG with `details` (the player's name, race, class,
    // level and place, as SuggestFramePlayerInfo shows them) before the text.
    bool report(bool suggestion, std::string_view details, std::string_view text);

    // Stuck(): the spell the player knows with SPELL_EFFECT_STUCK first (WoW.exe 0x4B261D keeps it as spells
    // are learned; 7355 "Stuck" for every character), 0 when none.
    static u32 stuckSpell(std::span<const u32> known, const SpellSource& source);

private:
    bool send(net::Opcode opcode);
    bool sendBody(net::Opcode opcode);
    GuildNotice& notice(const char* key);
    void noTicket();

    PacketSink m_sink;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u32 m_nowMs = 0;
    u32 m_version = 0;
    GmTicketState m_ticket;
    bool m_queueActive = true;
    bool m_queueAlert = false;
    bool m_surveyOffered = false;
    u32 m_refreshAtMs = 0;
    GuildNotices m_notices;
};

} // namespace vd::game
