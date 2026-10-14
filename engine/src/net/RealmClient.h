#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "net/Crypto.h"
#include "net/RealmListMessages.h"
#include "net/Socket.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace vd::net {

constexpr u16 kClientBuild = 5875; // 1.12.1
constexpr u32 kNetTimeoutMs = 10000;

struct RealmSession {
    std::array<u8, 40> sessionKey{}; // K, little-endian
    u32 sessionKeySize = 40;         // minimal length, as the world server uses it
    std::vector<RealmInfo> realms;
};

// Logs in to realmd on a connected socket (AUTH_LOGON_CHALLENGE, SRP6 proof, checking the server's M2)
// and fetches the realm list. `credentials` = srpCredentials(account, password). Protocol as
// src/realmd/AuthSocket.cpp (build 5875 branches) in vmangos-core. On a failure, `textKey` (when given) gets the
// core/Text key of what the player is told (the client's LOGIN_* texts; the error itself is technical, for the log).
// Phase 21: `locale` is the language the challenge tells the server (one of core/Locale's; anything else goes as
// enUS).
Result<RealmSession> loginToRealmd(Socket& socket, std::string_view account, const Sha1Digest& credentials,
                                   const char** textKey = nullptr, std::string_view locale = "enUS");
// Phase 18 (glue): the login alone (no realm list: `realms` stays empty), and the realm list asked for on a connection
// already logged in (again whenever RealmList refreshes it; realmd closes connections that ask more often than once a
// second, MinRealmListDelay): its answer's body for parseRealmList.
Result<RealmSession> authenticateRealmd(Socket& socket, std::string_view account, const Sha1Digest& credentials,
                                        const char** textKey = nullptr, std::string_view locale = "enUS");
Result<void> fetchRealmList(Socket& socket, std::vector<u8>& body);

// For an AuthResult code of the challenge/proof answers (src/realmd/AuthCodes.h): a description for the log, and the
// key of the client's text for it (GlueStrings LOGIN_*, as the original login screen shows them).
const char* realmdResultText(u8 code);
const char* realmdResultKey(u8 code);

} // namespace vd::net
