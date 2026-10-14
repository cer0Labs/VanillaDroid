#pragma once

#include "core/Types.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Phase 18 (p3): what the server keeps of an account besides its characters, as vmangos-core 4641790 reads and writes
// it for build 5875 (a 1.9+ client: the "new" account data types).
// - The tutorials seen (Server/WorldSession.cpp SendTutorialsData, Handlers/CharacterHandler.cpp
//   HandleTutorialFlagOpcode / Clear / Reset, Server/Packets/Misc TutorialFlag): eight u32 words per account, bit
//   `id % 32` of word `id / 32` set once tutorial `id` was flagged.
// - The account data (AccountData.h NewAccountData, WorldSession.cpp SendAccountDataTimes / SetAccountData,
//   Handlers/MiscHandler.cpp HandleUpdateAccountData / HandleRequestAccountData, Packets/Misc UpdateAccountData /
//   RequestAccountData / UpdateAccountDataResponse): eight texts, the global ones per account and the others per
//   character, exchanged zlib-compressed and compared by their MD5.
// Same rules as net/GameMessages: parsers fill a caller-owned value and return false on a malformed packet, never
// reading past its end; builders append the body of a client packet.
namespace vd::net {

// --- Tutorials ---------------------------------------------------------------------------------------------------

constexpr u32 kTutorialWords = 8; // ACCOUNT_TUTORIALS_COUNT
constexpr u32 kTutorialBits = kTutorialWords * 32;
using TutorialWords = std::array<u32, kTutorialWords>;

// Server SMSG_TUTORIAL_FLAGS (sent before SMSG_LOGIN_VERIFY_WORLD on every login): the eight words. WoW.exe's handler
// (0x4B5700) copies whatever size comes; vMaNGOS always sends 32 bytes: shorter is malformed here.
bool parseTutorialFlags(std::span<const u8> payload, TutorialWords& out);
// Client CMSG_TUTORIAL_FLAG: u32 the tutorial's id (0-based: TUTORIAL<id + 1> of GlobalStrings). The server sets its
// bit (ids past 255 are ignored). CMSG_TUTORIAL_CLEAR (every bit set) and CMSG_TUTORIAL_RESET (every bit clear) have
// no body.
void writeTutorialFlag(std::vector<u8>& out, u32 id);

// --- Account data ------------------------------------------------------------------------------------------------

// NewAccountData::AccountDataType: the files WoW.exe keeps in sync (its table 0x85DCF0: global flag, file names).
enum class AccountDataType : u8 {
    GlobalConfig = 0,      // WTF\Account\<ACCOUNT>\config-cache.wtf (1.12.1 writes no such file)
    CharacterConfig = 1,   // ...\<realm>\<character>\config-cache.wtf
    GlobalBindings = 2,    // bindings-cache.wtf (the key bindings)
    CharacterBindings = 3, // the character's bindings-cache.wtf
    GlobalMacros = 4,      // macros-cache.txt: the General Macros
    CharacterMacros = 5,   // the character's macros-cache.txt: its Specific Macros
    CharacterLayout = 6,   // layout-cache.txt (the frames moved by hand)
    CharacterChat = 7,     // chat-cache.txt (the chat windows)
};
constexpr u32 kAccountDataTypes = 8;          // NUM_ACCOUNT_DATA_TYPES
constexpr u32 kAccountDataGlobalMask = 0x15;  // GLOBAL_CACHE_MASK: 0, 2, 4 per account; the others per character
constexpr u32 kAccountDataMaxSize = 0xFFFF;   // HandleUpdateAccountData drops anything bigger
inline bool accountDataIsGlobal(u32 type) { return type < kAccountDataTypes && ((1u << type) & kAccountDataGlobalMask); }

using Md5 = std::array<u8, 16>;
// MD5 (RFC 1321) of `data`. vMaNGOS sends all zeros (MD5::CreateEmpty), not the digest of nothing, for an empty text.
Md5 md5(std::span<const u8> data);
inline Md5 md5(std::string_view text) { return md5({reinterpret_cast<const u8*>(text.data()), text.size()}); }
bool md5IsZero(const Md5& digest);

// Server SMSG_ACCOUNT_DATA_MD5 (HandlePlayerLogin, right after the character's data is loaded): the eight digests in
// type order, 128 bytes.
bool parseAccountDataMd5(std::span<const u8> payload, std::array<Md5, kAccountDataTypes>& out);
// Client CMSG_REQUEST_ACCOUNT_DATA: u32 the type. Only while in the world (STATUS_LOGGEDIN).
void writeRequestAccountData(std::vector<u8>& out, u32 type);
// Client CMSG_UPDATE_ACCOUNT_DATA: u32 type, u32 the text's size, the text deflated with zlib (WoW.exe 0x5AFAE0:
// 0x6605A0 compresses the file). An empty text sends size 0 and nothing else: the server erases it. False when the
// text is too big for the server or zlib failed (nothing appended then).
bool writeUpdateAccountData(std::vector<u8>& out, u32 type, std::string_view text);

// Server SMSG_UPDATE_ACCOUNT_DATA (the answer to a request): u32 the type as asked, u32 the inflated size, then the zlib
// stream (nothing after a 0 size: no data kept).
struct AccountDataUpdate {
    u32 type = 0;
    std::string text; // inflated
};
bool parseUpdateAccountData(std::span<const u8> payload, AccountDataUpdate& out);

} // namespace vd::net
