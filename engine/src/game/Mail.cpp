#include "game/Mail.h"

#include "core/Text.h"
#include "game/Inventory.h"
#include "net/GameMessages.h"

#include <algorithm>

namespace vd::game {

using net::Opcode;

Mail::Mail(ExchangeContext& context) : m_c(context) {
    m_messages.reserve(net::kMaxMailMessages);
    m_list.messages.reserve(net::kMaxMailMessages);
    m_bodyAsked.reserve(64);
}

void Mail::enterWorld(bool newLogin) {
    m_mailbox = 0;
    m_loaded = false;
    m_sending = false;
    m_attachment = 0;
    if (newLogin) {
        m_messages.clear();
        m_bodyAsked.clear();
        m_newMail = false;
        ++m_version;
        m_c.sendEmpty(Opcode::MSG_QUERY_NEXT_MAIL_TIME); // what the client asks at login: does mail wait?
    }
}

void Mail::leaveWorld() {
    m_mailbox = 0;
    m_loaded = false;
    m_sending = false;
    m_attachment = 0;
}

bool Mail::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::SMSG_MAIL_LIST_RESULT:
    case Opcode::SMSG_SEND_MAIL_RESULT:
    case Opcode::MSG_QUERY_NEXT_MAIL_TIME:
    case Opcode::SMSG_RECEIVED_MAIL: return true;
    default: return false;
    }
}

bool Mail::handle(Opcode opcode, std::span<const u8> p) {
    switch (opcode) {
    case Opcode::SMSG_MAIL_LIST_RESULT:
        if (!net::parseMailList(p, m_list)) {
            return false;
        }
        onList(m_list);
        return true;
    case Opcode::SMSG_SEND_MAIL_RESULT: {
        net::SendMailResult r;
        if (!net::parseSendMailResult(p, r)) {
            return false;
        }
        onResult(r);
        return true;
    }
    case Opcode::MSG_QUERY_NEXT_MAIL_TIME: {
        f32 seconds = 0.0f;
        if (!net::parseNextMailTime(p, seconds)) {
            return false;
        }
        m_newMail = seconds >= 0.0f; // 0: unread mail waits; minus a day: none
        return true;
    }
    case Opcode::SMSG_RECEIVED_MAIL: {
        u32 delay = 0;
        if (!net::parseU32(p, delay)) {
            return false;
        }
        m_newMail = true;
        m_c.event(GameEventKind::NewMail);
        if (open()) {
            refresh(); // the new letter shows in the open mailbox
        }
        return true;
    }
    default: return false;
    }
}

// -------------------------------------------------------------------------------------------------------
// The mailbox.

void Mail::open(u64 mailbox) {
    if (mailbox == 0) {
        return;
    }
    m_mailbox = mailbox;
    m_loaded = false;
    refresh();
}

void Mail::close() {
    if (m_mailbox == 0) {
        return;
    }
    m_mailbox = 0;
    m_loaded = false;
    m_attachment = 0;
    m_c.sendEmpty(Opcode::MSG_QUERY_NEXT_MAIL_TIME); // the minimap's letter follows what is still unread
}

void Mail::refresh() {
    if (m_mailbox == 0) {
        return;
    }
    m_c.body.clear();
    net::writeGuid(m_c.body, m_mailbox);
    m_c.send(Opcode::CMSG_GET_MAIL_LIST);
}

const MailMessage* Mail::find(u32 id) const {
    for (const MailMessage& m : m_messages) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

MailMessage* Mail::findMutable(u32 id) {
    for (MailMessage& m : m_messages) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

void Mail::onList(const net::MailList& list) {
    m_messages.clear();
    for (const net::MailEntry& from : list.messages) {
        MailMessage& m = m_messages.emplace_back();
        m.id = from.id;
        m.type = from.type;
        m.sender = from.sender;
        m.senderEntry = from.senderEntry;
        m.subject.assign(from.subject);
        m.itemTextId = from.itemTextId;
        m.stationery = from.stationery;
        m.item = from.item;
        m.money = from.money;
        m.cod = from.cod;
        m.flags = from.flags;
        m.daysLeft = from.daysLeft;
        m.templateId = from.templateId;
        // Names and items the list shows.
        if (m.type == net::kMailNormal && m.sender != 0) {
            m_c.queries.playerName(m.sender);
        } else if (m.type == net::kMailCreature && m.senderEntry != 0) {
            m_c.queries.creature(m.senderEntry);
        } else if (m.type == net::kMailGameObject && m.senderEntry != 0) {
            m_c.queries.gameObject(m.senderEntry);
        }
        if (m.item.entry != 0) {
            m_c.queries.item(m.item.entry);
        }
    }
    m_loaded = m_mailbox != 0;
    ++m_version;
    updateNewMail();
    m_c.event(GameEventKind::MailListed, static_cast<u32>(m_messages.size()));
}

void Mail::onResult(const net::SendMailResult& r) {
    GameEvent& e = m_c.event(GameEventKind::MailResult, r.mailId);
    e.value = {static_cast<i32>(r.action), static_cast<i32>(r.error), static_cast<i32>(r.equipError)};
    if (r.action == net::kMailActionSend) {
        m_sending = false;
        if (r.error == net::kMailOk) {
            m_attachment = 0; // it went with the message
        }
    }
    if (r.error != net::kMailOk) {
        return;
    }
    MailMessage* m = findMutable(r.mailId);
    switch (r.action) {
    case net::kMailActionMoneyTaken:
        if (m) {
            m->money = 0;
        }
        break;
    case net::kMailActionItemTaken:
        if (m) {
            m->item = net::MailItemInfo{};
            m->cod = 0; // paid
        }
        break;
    case net::kMailActionMadePermanent:
        if (m) {
            m->flags |= net::kMailFlagCopied;
        }
        break;
    case net::kMailActionDeleted:
    case net::kMailActionReturned:
        std::erase_if(m_messages, [&](const MailMessage& message) { return message.id == r.mailId; });
        break;
    default: break;
    }
    ++m_version;
    updateNewMail();
}

void Mail::updateNewMail() {
    if (!m_loaded) {
        return;
    }
    m_newMail =
        std::any_of(m_messages.begin(), m_messages.end(), [](const MailMessage& m) { return !m.read(); });
}

// -------------------------------------------------------------------------------------------------------
// A message.

bool Mail::action(Opcode opcode, u32 id) {
    if (m_mailbox == 0) {
        return false;
    }
    m_c.body.clear();
    net::writeMailAction(m_c.body, m_mailbox, id);
    return m_c.send(opcode);
}

bool Mail::markRead(u32 id) {
    MailMessage* m = findMutable(id);
    if (!m || m->read() || !action(Opcode::CMSG_MAIL_MARK_AS_READ, id)) {
        return false;
    }
    m->flags |= net::kMailFlagRead; // the server does not answer
    ++m_version;
    updateNewMail();
    return true;
}

bool Mail::takeMoney(u32 id) {
    const MailMessage* m = find(id);
    return m && m->money != 0 && action(Opcode::CMSG_MAIL_TAKE_MONEY, id);
}

bool Mail::takeItem(u32 id) {
    const MailMessage* m = find(id);
    return m && m->hasItem() && action(Opcode::CMSG_MAIL_TAKE_ITEM, id);
}

bool Mail::remove(u32 id) {
    const MailMessage* m = find(id);
    return m && m->canDelete() && m->cod == 0 && action(Opcode::CMSG_MAIL_DELETE, id);
}

bool Mail::returnToSender(u32 id) {
    const MailMessage* m = find(id);
    return m && m->canReturn() && action(Opcode::CMSG_MAIL_RETURN_TO_SENDER, id);
}

bool Mail::copyText(u32 id) {
    const MailMessage* m = find(id);
    if (!m || !m->canCopyText() || m_mailbox == 0) {
        return false;
    }
    m_c.body.clear();
    net::writeMailCreateTextItem(m_c.body, m_mailbox, id, m->templateId);
    return m_c.send(Opcode::CMSG_MAIL_CREATE_TEXT_ITEM);
}

std::string_view Mail::body(u32 id) {
    const MailMessage* m = find(id);
    if (!m || m->itemTextId == 0) {
        return {};
    }
    const std::string_view text = m_c.inventory.letterText(m->itemTextId);
    if (text.empty() &&
        std::find(m_bodyAsked.begin(), m_bodyAsked.end(), m->itemTextId) == m_bodyAsked.end()) {
        if (m_c.inventory.requestLetter(m->itemTextId)) {
            m_bodyAsked.push_back(m->itemTextId);
        }
    }
    return text;
}

// -------------------------------------------------------------------------------------------------------
// Sending.

std::string_view Mail::sendProblem(const MailDraftView& d) const {
    if (m_mailbox == 0) {
        return text::get("VD_EXCHANGE_NO_MAILBOX");
    }
    if (m_sending) {
        return text::get("VD_EXCHANGE_MAIL_SENDING");
    }
    if (d.to.empty()) {
        return text::get("VD_EXCHANGE_MAIL_NO_RECIPIENT");
    }
    if (d.subject.empty()) {
        return text::get("VD_EXCHANGE_MAIL_NO_SUBJECT");
    }
    if (d.subject.size() > net::kMailMaxSubject) {
        return text::get("VD_EXCHANGE_MAIL_SUBJECT_TOO_LONG");
    }
    if (d.body.size() > net::kMailMaxBody) {
        return text::get("VD_EXCHANGE_MAIL_BODY_TOO_LONG");
    }
    if (d.cod != 0 && d.item == 0) {
        return text::get("VD_EXCHANGE_MAIL_COD_NEEDS_ITEM");
    }
    if (d.cod > net::kMailMaxCod) {
        return text::format(m_problem, "MAIL_COD_ERROR", static_cast<int>(net::kMailMaxCod / 10000)); // gold
    }
    if (static_cast<u64>(net::kMailPostage) + (d.cod == 0 ? d.money : 0) > m_c.inventory.money()) {
        return text::get("ERR_NOT_ENOUGH_MONEY");
    }
    return d.item != 0 ? itemProblem(d.item) : std::string_view{};
}

std::string_view Mail::itemProblem(u64 guid) const {
    const ItemInstance* item = m_c.inventory.find(guid);
    const i32 container = item ? m_c.inventory.containerOf(item->pos) : 99;
    if (!item || container < kContainerBackpack || container > kContainerLastBag) {
        return text::get("VD_EXCHANGE_MAIL_NOT_IN_BAGS");
    }
    if (item->soulbound()) {
        return text::get("ERR_MAIL_BOUND_ITEM");
    }
    const net::ItemQueryInfo* info = m_c.queries.findItem(item->entry);
    if (item->durationS != 0 || (info && (info->flags & kItemFlagConjured))) {
        return text::get("ERR_MAIL_CONJURED_ITEM");
    }
    return {};
}

bool Mail::attach(u64 item) {
    if (m_mailbox == 0 || item == 0 || !itemProblem(item).empty()) {
        return false;
    }
    m_attachment = item;
    return true;
}

bool Mail::send(const MailDraftView& d) {
    if (!sendProblem(d).empty()) {
        return false;
    }
    net::MailDraft draft;
    draft.to = d.to;
    draft.subject = d.subject;
    draft.body = d.body;
    draft.item = d.item;
    draft.cod = d.item != 0 ? d.cod : 0;
    draft.money = draft.cod == 0 ? d.money : 0; // money or cash on delivery, as the client's two choices
    m_c.body.clear();
    net::writeSendMail(m_c.body, m_mailbox, draft);
    if (!m_c.send(Opcode::CMSG_SEND_MAIL)) {
        return false;
    }
    m_sending = true;
    return true;
}

} // namespace vd::game
