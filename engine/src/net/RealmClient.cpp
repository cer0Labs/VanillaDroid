#include "net/RealmClient.h"

#include "core/Locale.h"
#include "core/Log.h"
#include "net/ByteBuffer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>

namespace vd::net {
namespace {

constexpr u8 kCmdLogonChallenge = 0x00;
constexpr u8 kCmdLogonProof = 0x01;
constexpr usize kMaxAccountLength = 16; // AUTH_LOGON_MAX_NAME

// A 4CC as the client sends it: the text reversed, zero-padded ("x86" -> '6','8','x',0).
void writeReversed(ByteWriter& w, std::string_view text) {
    u8 bytes[4] = {};
    for (usize i = 0; i < text.size() && i < 4; ++i) {
        bytes[text.size() - 1 - i] = static_cast<u8>(text[i]);
    }
    w.bytes(bytes);
}

std::array<u8, 32> randomPrivateKey() {
    std::random_device device; // OS entropy (BCryptGenRandom / getrandom)
    std::array<u8, 32> key{};
    for (usize i = 0; i < key.size(); i += 4) {
        const u32 word = device();
        std::memcpy(key.data() + i, &word, 4);
    }
    key[31] |= 0x01; // never zero
    return key;
}

Error realmdError(const char* stage, u8 code) {
    return Error{std::string(stage) + ": " + realmdResultText(code)};
}

} // namespace

const char* realmdResultKey(u8 code) {
    switch (code) {
    case 0x03: return "LOGIN_BANNED";
    case 0x04:
    case 0x05: return "LOGIN_UNKNOWN_ACCOUNT";
    case 0x06: return "LOGIN_ALREADYONLINE";
    case 0x07: return "LOGIN_NOTIME";
    case 0x08: return "LOGIN_DBBUSY";
    case 0x09: return "LOGIN_BADVERSION";
    case 0x0C: return "LOGIN_SUSPENDED";
    case 0x0F: return "LOGIN_PARENTALCONTROL";
    default: return "LOGIN_FAILED"; // "Unable to connect", as the client words the rest
    }
}

const char* realmdResultText(u8 code) {
    switch (code) {
    case 0x00: return "success";
    case 0x03: return "account banned";
    case 0x04:
    case 0x05: return "unknown account or wrong password";
    case 0x06: return "account already online";
    case 0x08: return "server busy (too many failed logins from this address? wait a minute)";
    case 0x09: return "game version not accepted";
    case 0x0C: return "account suspended";
    case 0x0D: return "no access";
    default: return "login refused";
    }
}

Result<RealmSession> authenticateRealmd(Socket& socket, std::string_view account, const Sha1Digest& credentials,
                                        const char** textKey, std::string_view locale) {
    const char* unused = nullptr;
    const char*& key = textKey ? *textKey : unused;
    key = "LOGIN_FAILED"; // the protocol's own failures: "Unable to connect"
    std::string accountUpper(account);
    std::transform(accountUpper.begin(), accountUpper.end(), accountUpper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (accountUpper.empty() || accountUpper.size() > kMaxAccountLength) {
        key = "LOGIN_UNKNOWN_ACCOUNT";
        return Error{"Account name must have 1 to 16 characters"};
    }

    // 1. AUTH_LOGON_CHALLENGE: cmd, error, size, then the 30-byte body + account.
    std::vector<u8> out;
    ByteWriter w(out);
    w.u8v(kCmdLogonChallenge).u8v(0x03).u16v(static_cast<u16>(30 + accountUpper.size()));
    w.bytes(asBytes(std::string_view("WoW\0", 4)));
    w.u8v(1).u8v(12).u8v(1).u16v(kClientBuild);
    writeReversed(w, "x86"); // the world server only accepts Win/x86 accounts (WorldSocket.cpp)
    writeReversed(w, "Win");
    const std::string_view code = canonicalLocale(locale); // phase 21: the server's LocaleConstant by this name
    writeReversed(w, code.empty() ? std::string_view("enUS") : code);
    w.u32v(0); // timezone bias
    w.u32v(0); // client IP (unused by the server)
    w.u8v(static_cast<u8>(accountUpper.size()));
    w.bytes(asBytes(accountUpper));
    VD_TRY(socket.sendAll(out));

    // 2. Challenge answer.
    u8 head[3];
    VD_TRY(socket.receiveExact(head, sizeof(head), kNetTimeoutMs));
    if (head[0] != kCmdLogonChallenge) {
        return Error{"Unexpected realmd answer to the challenge"};
    }
    if (head[2] != 0) {
        key = realmdResultKey(head[2]);
        return realmdError("Login", head[2]);
    }
    SrpChallenge challenge;
    VD_TRY(socket.receiveExact(challenge.B.data(), 32, kNetTimeoutMs));
    u8 length = 0;
    VD_TRY(socket.receiveExact(&length, 1, kNetTimeoutMs));
    if (length != 1) {
        return Error{"Unexpected SRP6 generator size"};
    }
    VD_TRY(socket.receiveExact(&challenge.g, 1, kNetTimeoutMs));
    VD_TRY(socket.receiveExact(&length, 1, kNetTimeoutMs));
    if (length != 32) {
        return Error{"Unexpected SRP6 prime size"};
    }
    VD_TRY(socket.receiveExact(challenge.N.data(), 32, kNetTimeoutMs));
    VD_TRY(socket.receiveExact(challenge.salt.data(), 32, kNetTimeoutMs));
    u8 versionChallenge[16];
    VD_TRY(socket.receiveExact(versionChallenge, sizeof(versionChallenge), kNetTimeoutMs));
    u8 securityFlags = 0; // sent from build 5428 on
    VD_TRY(socket.receiveExact(&securityFlags, 1, kNetTimeoutMs));
    if (securityFlags != 0) {
        return Error{"The account asks for a PIN: not supported"};
    }

    // 3. AUTH_LOGON_PROOF.
    const std::array<u8, 32> privateKey = randomPrivateKey();
    Result<SrpProof> proof = srpClientProof(accountUpper, credentials, challenge, privateKey);
    if (!proof) {
        return proof.error();
    }
    out.clear();
    w.u8v(kCmdLogonProof).bytes(proof.value().A).bytes(proof.value().M1);
    const u8 versionHash[20] = {}; // only checked with StrictVersionCheck
    w.bytes(versionHash).u8v(0).u8v(0); // number of keys, security flags
    VD_TRY(socket.sendAll(out));

    u8 proofHead[2];
    VD_TRY(socket.receiveExact(proofHead, sizeof(proofHead), kNetTimeoutMs));
    if (proofHead[0] != kCmdLogonProof) {
        return Error{"Unexpected realmd answer to the proof"};
    }
    if (proofHead[1] != 0) {
        key = realmdResultKey(proofHead[1]);
        return realmdError("Login", proofHead[1]);
    }
    u8 proofBody[24]; // M2[20] + survey id
    VD_TRY(socket.receiveExact(proofBody, sizeof(proofBody), kNetTimeoutMs));
    if (!std::equal(proof.value().M2.begin(), proof.value().M2.end(), proofBody)) {
        key = "LOGIN_BAD_SERVER_PROOF";
        return Error{"The server's proof does not match: not the expected realmd"};
    }

    RealmSession session;
    session.sessionKey = proof.value().K;
    session.sessionKeySize = proof.value().keySize;
    return session;
}

Result<void> fetchRealmList(Socket& socket, std::vector<u8>& body) {
    // 4. REALM_LIST (net/RealmListMessages).
    std::vector<u8> out;
    writeRealmListRequest(out);
    VD_TRY(socket.sendAll(out));
    u8 listHead[3];
    VD_TRY(socket.receiveExact(listHead, sizeof(listHead), kNetTimeoutMs));
    if (listHead[0] != kCmdRealmList) {
        return Error{"Unexpected realmd answer to the realm list"};
    }
    const u16 size = static_cast<u16>(listHead[1] | (listHead[2] << 8));
    body.resize(size);
    VD_TRY(socket.receiveExact(body.data(), body.size(), kNetTimeoutMs));
    return {};
}

Result<RealmSession> loginToRealmd(Socket& socket, std::string_view account, const Sha1Digest& credentials,
                                   const char** textKey, std::string_view locale) {
    Result<RealmSession> session = authenticateRealmd(socket, account, credentials, textKey, locale);
    if (!session) {
        return session;
    }
    std::vector<u8> body;
    VD_TRY(fetchRealmList(socket, body));
    std::optional<std::vector<RealmInfo>> realms = parseRealmList(body);
    if (!realms) {
        return Error{"Malformed realm list"};
    }
    session.value().realms = std::move(*realms);
    return session;
}

} // namespace vd::net
