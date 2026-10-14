#include "game/AccountData.h"

#include "core/Log.h"
#include "core/Profile.h"

namespace vd::game {

using net::Opcode;

void AccountData::reset() {
    m_waiting.fill(false);
    m_timer.fill(false);
}

bool AccountData::local(u32 type, std::string& text, net::Md5& digest) {
    text.clear();
    if (!m_store.read || !m_store.read(m_store.context, type, text)) {
        return false;
    }
    // 0x5AF940: the file's MD5, zeros when there is no file (an empty text here).
    digest = text.empty() ? net::Md5{} : net::md5(text);
    return true;
}

void AccountData::upload(u32 type, std::string_view text) {
    m_body.clear();
    if (net::writeUpdateAccountData(m_body, type, text)) {
        m_sink(Opcode::CMSG_UPDATE_ACCOUNT_DATA, m_body);
    } else {
        VD_LOG_WARN("Account data %u: %zu bytes, too big to keep on the server", type, text.size());
    }
}

bool AccountData::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    (void)nowMs;
    if (opcode == Opcode::SMSG_ACCOUNT_DATA_MD5) {
        Digests server{};
        if (!net::parseAccountDataMd5(payload, server)) {
            return false;
        }
        if (m_store.begin) {
            m_store.begin(m_store.context); // 0x5AF880 read cache.md5 at start; here the account is known now
        }
        for (u32 type = 0; type < net::kAccountDataTypes; ++type) {
            m_waiting[type] = false;
            net::Md5 mine{};
            if (!local(type, m_text, mine)) {
                continue; // not kept by this client
            }
            if (!net::md5IsZero(server[type]) && mine == m_synced[type]) {
                if (mine != server[type]) {
                    m_body.clear();
                    net::writeRequestAccountData(m_body, type);
                    m_sink(Opcode::CMSG_REQUEST_ACCOUNT_DATA, m_body);
                    m_waiting[type] = true;
                    m_timer[type] = false;
                }
                m_synced[type] = server[type];
            } else {
                if (mine != server[type]) {
                    upload(type, m_text);
                }
                m_synced[type] = mine;
            }
        }
        ++m_syncedVersion; // cache.md5 (0x5AF9C0)
        return true;
    }
    if (opcode == Opcode::SMSG_UPDATE_ACCOUNT_DATA) {
        net::AccountDataUpdate update;
        if (!net::parseUpdateAccountData(payload, update)) {
            return false;
        }
        if (update.type >= net::kAccountDataTypes) {
            return true;
        }
        if (!update.text.empty() && m_store.write) {
            m_store.write(m_store.context, update.type, update.text);
            ++m_downloads;
        }
        m_waiting[update.type] = false;
        return true;
    }
    return false;
}

void AccountData::changed(u32 type, u32 nowMs) {
    // 0x5AFED0: nothing while the type waits for the server's text; else the timer starts (again).
    if (type >= net::kAccountDataTypes || m_waiting[type]) {
        return;
    }
    m_timer[type] = true;
    m_dueMs[type] = nowMs + kSaveDelayMs;
}

void AccountData::save(u32 type) {
    m_timer[type] = false;
    net::Md5 mine{};
    if (!local(type, m_text, mine) || mine == m_synced[type]) {
        return;
    }
    upload(type, m_text);
    m_synced[type] = mine;
    ++m_syncedVersion;
}

void AccountData::update(u32 nowMs) {
    VD_PROFILE_ZONE("AccountData::update");
    for (u32 type = 0; type < net::kAccountDataTypes; ++type) {
        if (m_timer[type] && static_cast<i32>(nowMs - m_dueMs[type]) >= 0) {
            save(type);
        }
    }
}

void AccountData::flush() {
    for (u32 type = 0; type < net::kAccountDataTypes; ++type) {
        if (m_timer[type]) {
            save(type);
        }
    }
}

} // namespace vd::game
