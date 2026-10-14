#pragma once

#include "core/Types.h"
#include "game/GameEvents.h"
#include "net/AccountDataMessages.h"
#include "net/Opcodes.gen.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::game {

// What this client keeps of each account data type (the app's files, app/AccountDataFiles).
struct AccountDataStore {
    // The type's text now (what WoW.exe reads from the file; empty: no file). False: this client keeps nothing of that
    // type (key bindings, frame layouts, config-cache.wtf): it is never asked for nor sent.
    bool (*read)(void* context, u32 type, std::string& out) = nullptr;
    // A text the server sent replaces the local one (its file, and what was read from it).
    void (*write)(void* context, u32 type, std::string_view text) = nullptr;
    // SMSG_ACCOUNT_DATA_MD5 came: the synced digests of this account are wanted now (setSynced, cache.md5).
    void (*begin)(void* context) = nullptr;
    void* context = nullptr;
};

// Phase 18 (p3): the account data as WoW.exe 1.12.1 keeps it in step with the server (AccountData.cpp, read-only
// disassembly 0x5AF6B0..0x5AFF96; vMaNGOS: WorldSession SendAccountDataTimes / SetAccountData, MiscHandler
// HandleUpdateAccountData / HandleRequestAccountData). Three digests per type: the local file's ("local"), the one the
// last sync left (WTF\Account\<ACCOUNT>\cache.md5, "synced") and the server's (SMSG_ACCOUNT_DATA_MD5).
//  - SMSG_ACCOUNT_DATA_MD5 (0x5AF8E0 -> 0x5AFA10, at every character login), per type: the server has a text and the
//    local file did not change since the last sync (local == synced) -> when they differ the server's is asked for
//    (CMSG_REQUEST_ACCOUNT_DATA, 0x5AFBE0: the type waits for it) and synced = the server's. Otherwise (the server has
//    nothing, or the file changed here) -> when they differ the local file goes up (CMSG_UPDATE_ACCOUNT_DATA, 0x5AFAE0)
//    and synced = local. So a change made on this device wins over the server's copy; an untouched file takes the
//    server's (another device's change). cache.md5 is written after (0x5AF9C0).
//  - SMSG_UPDATE_ACCOUNT_DATA (0x5AFC60): a text written over the file (nothing when its size is 0), the type no longer
//    waits and what wanted to load it loads it (0x5AFE50's deferred reader).
//  - A file saved (0x5AFED0: chat-cache.txt and layout-cache.txt when the character leaves the world, the macros and
//    the bindings when they changed; nothing while the type waits) starts a 30 s timer (0x5AFF40, SaveTimerHandler);
//    then its digest is compared with the synced one and, when it differs, the file goes up (0x5AFD70). Disconnecting
//    (0x5AB049 -> 0x5AFD30) runs every waiting timer at once.
// Main thread. Packets and sent texts allocate (a few at login and on rare saves); nothing per frame.
class AccountData {
public:
    using Digests = std::array<net::Md5, net::kAccountDataTypes>;
    static constexpr u32 kSaveDelayMs = 30000; // 0x7530

    void setSink(PacketSink sink) { m_sink = sink; }
    void setStore(AccountDataStore store) { m_store = store; }
    // cache.md5 (the app reads and writes the file; syncedVersion() is bumped whenever it should be written again).
    void setSynced(const Digests& digests) { m_synced = digests; }
    const Digests& synced() const { return m_synced; }
    u32 syncedVersion() const { return m_syncedVersion; }

    static bool handles(net::Opcode opcode) {
        return opcode == net::Opcode::SMSG_ACCOUNT_DATA_MD5 || opcode == net::Opcode::SMSG_UPDATE_ACCOUNT_DATA;
    }
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    // A type's file was saved here: it goes up kSaveDelayMs later when its digest is not the synced one.
    void changed(u32 type, u32 nowMs);
    // Once a frame: the save timers.
    void update(u32 nowMs);
    // Leaving the world / disconnecting: every waiting save goes up now.
    void flush();
    // The type waits for the server's text (asked for at login): do not read nor write its file yet.
    bool waiting(u32 type) const { return type < net::kAccountDataTypes && m_waiting[type]; }
    // Bumped by every text the server sent (written through the store).
    u32 downloads() const { return m_downloads; }
    // A new session (another account or character): nothing waits, no timer runs.
    void reset();

private:
    // The local text of a type through the store (false: not kept here).
    bool local(u32 type, std::string& text, net::Md5& digest);
    void upload(u32 type, std::string_view text);
    void save(u32 type); // SaveTimerHandler

    PacketSink m_sink;
    AccountDataStore m_store;
    Digests m_synced{};
    u32 m_syncedVersion = 0;
    std::array<bool, net::kAccountDataTypes> m_waiting{};
    std::array<bool, net::kAccountDataTypes> m_timer{};
    std::array<u32, net::kAccountDataTypes> m_dueMs{};
    u32 m_downloads = 0;
    std::string m_text; // scratch
    std::vector<u8> m_body;
};

} // namespace vd::game
