#pragma once

#include "core/Types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::net {

// Phase 18 (glue): renaming a character the server marked for it (CHARACTER_FLAG_RENAME in SMSG_CHAR_ENUM's
// flags, set by `.character rename` or by a name that no longer passes the rules: Player::LoadFromDB), as
// vmangos-core 4641790 reads and writes it (Server/Packets/Character.cpp CharRename::ReadFromWorldPacket
// "guid, newname"; CharRenameResponse::AppendBodyTo "result", and on RESPONSE_SUCCESS "renamedCharacterGuid,
// newName"; Handlers/CharacterHandler.cpp HandleCharRenameOpcode: CHAR_NAME_* when the name breaks a rule,
// CHAR_CREATE_ERROR when the character is not the account's, not marked or the name is taken).

constexpr u32 kCharacterFlagRename = 0x4000; // CHARACTER_FLAG_RENAME: the character list asks for a new name

// CMSG_CHAR_RENAME: the character and its new name.
void writeCharRename(std::vector<u8>& out, u64 guid, std::string_view name);

// SMSG_CHAR_RENAME.
struct CharRenameResult {
    u8 result = 0;    // a ResponseCode: RESPONSE_SUCCESS (0), CHAR_NAME_*, CHAR_CREATE_ERROR...
    u64 guid = 0;     // on success: the character renamed
    std::string name; // ...and its new name
};
std::optional<CharRenameResult> parseCharRename(std::span<const u8> payload);

} // namespace vd::net
