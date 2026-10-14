#pragma once

#include "core/Types.h"
#include "game/ExchangeContext.h"
#include "net/ExchangeMessages.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::game {

struct ItemPos;

// One message of the mailbox, as the server listed it and as the player's actions changed it since.
struct MailMessage {
    u32 id = 0;
    u8 type = net::kMailNormal;
    u64 sender = 0;      // a player's GUID (net::kMailNormal)
    u32 senderEntry = 0; // an NPC's or object's entry, an auction house's id
    std::string subject;
    u32 itemTextId = 0; // the body: Inventory::letterText()
    u32 stationery = 0;
    net::MailItemInfo item; // entry 0: none (or taken)
    u32 money = 0;          // 0: none (or taken)
    u32 cod = 0;            // what taking the item costs
    u32 flags = 0;          // net::kMailFlag*
    f32 daysLeft = 0.0f;    // when the list arrived
    u32 templateId = 0;     // MailTemplate.dbc: the body of NPC letters

    bool read() const { return (flags & net::kMailFlagRead) != 0; }
    bool hasItem() const { return item.entry != 0; }
    bool fromPlayer() const { return type == net::kMailNormal && sender != 0; }
    // What the client offers (InboxFrame / OpenMailFrame): returning a player's letter that did not come back
    // already; copying a text that has one and was not copied.
    bool canReturn() const { return fromPlayer() && (flags & net::kMailFlagReturned) == 0; }
    bool canCopyText() const {
        return (itemTextId != 0 || templateId != 0) && (flags & net::kMailFlagCopied) == 0;
    }
    // Phase 18 (p3): InboxItemCanDelete (WoW.exe 0x4AFE00): a letter that came back or pays for a COD, and anything not
    // from a player, can be deleted with whatever it holds (DELETE_MAIL / DELETE_MONEY ask first); a player's other
    // letters only when they hold neither money nor an item (else OpenMailFrame offers Return instead).
    bool canDelete() const {
        return (flags & (net::kMailFlagReturned | net::kMailFlagCodPayment)) != 0 || type != net::kMailNormal ||
               (money == 0 && !hasItem());
    }
};

// A message being written.
struct MailDraftView {
    std::string_view to;
    std::string_view subject;
    std::string_view body;
    u64 item = 0; // in the bags
    u32 money = 0;
    u32 cod = 0; // cash on delivery (only with an item; then no money is sent)
};

// The mailbox (phase 8): the list of a mailbox the player stands at (CMSG_GET_MAIL_LIST), reading, taking
// money and items (paying cash on delivery), deleting, returning, copying the text to a letter, sending (one
// item, money or COD), and whether unread mail waits (the minimap's letter: MSG_QUERY_NEXT_MAIL_TIME,
// SMSG_RECEIVED_MAIL). Part of game::Exchange. Storage is reserved once; a list's subjects are copied when it
// arrives (not per frame).
class Mail {
public:
    explicit Mail(ExchangeContext& context);

    // `newLogin`: a new character (asks whether mail waits).
    void enterWorld(bool newLogin);
    void leaveWorld();
    static bool handles(net::Opcode opcode);
    bool handle(net::Opcode opcode, std::span<const u8> payload);

    // --- The mailbox window ---
    // A mailbox object was used: its list is asked for; open() from now on (loaded() once it arrives).
    void open(u64 mailbox);
    bool open() const { return m_mailbox != 0; }
    u64 mailbox() const { return m_mailbox; }
    bool loaded() const { return m_loaded; }
    // The window closes (walked away, or the player closed it): the minimap's letter is asked about again.
    void close();
    void refresh(); // CMSG_GET_MAIL_LIST again
    std::span<const MailMessage> messages() const { return m_messages; }
    const MailMessage* find(u32 id) const;
    // Bumped whenever the list or a message changes (views may cache what they built).
    u32 version() const { return m_version; }

    // --- A message (false when not sent: no mailbox, unknown message, nothing to take...) ---
    bool markRead(u32 id);  // CMSG_MAIL_MARK_AS_READ (opening an unread message)
    bool takeMoney(u32 id); // CMSG_MAIL_TAKE_MONEY
    bool takeItem(u32 id);  // CMSG_MAIL_TAKE_ITEM: pays the COD if any (confirm first)
    bool remove(u32 id);    // CMSG_MAIL_DELETE
    bool returnToSender(u32 id);
    bool copyText(u32 id); // CMSG_MAIL_CREATE_TEXT_ITEM: a letter with the text into the bags
    // The body: the server's text (asked for when missing: empty until it arrives).
    std::string_view body(u32 id);

    // --- Sending ---
    // Why a draft cannot go now (a text for the player), or empty when it can (recipient and subject, sizes,
    // money, the item in the bags).
    std::string_view sendProblem(const MailDraftView& draft) const;
    // CMSG_SEND_MAIL (sendProblem() first); sending() until the server answers (a MailResult event).
    bool send(const MailDraftView& draft);
    bool sending() const { return m_sending; }
    // Postage plus the money sent.
    static u32 cost(const MailDraftView& draft) {
        return net::kMailPostage + (draft.cod == 0 ? draft.money : 0);
    }
    // The item the message being written carries (game/ItemActions: "Adjuntar", a drop on the attachment): an
    // item of the bags while the mailbox is open; 0 none. A successful send or closing the mailbox clears it.
    bool attach(u64 item);
    void detach() { m_attachment = 0; }
    u64 attachment() const { return m_attachment; }
    // Why an item cannot travel by mail (not in the bags, bound, temporary), or empty.
    std::string_view itemProblem(u64 item) const;

    // --- The minimap's letter ---
    bool hasNewMail() const { return m_newMail; }

private:
    MailMessage* findMutable(u32 id);
    bool action(net::Opcode opcode, u32 id);
    void onList(const net::MailList& list);
    void onResult(const net::SendMailResult& r);
    void updateNewMail();

    ExchangeContext& m_c;
    u64 m_mailbox = 0;
    bool m_loaded = false;
    bool m_sending = false;
    bool m_newMail = false;
    u32 m_version = 1;
    u64 m_attachment = 0;
    std::vector<MailMessage> m_messages;
    std::vector<u32> m_bodyAsked;             // item text ids asked for this session
    net::MailList m_list;                     // parsed, reused
    mutable std::array<char, 64> m_problem{}; // sendProblem's formatted texts
};

} // namespace vd::game
