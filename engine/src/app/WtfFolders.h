#pragma once

#include "core/Types.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace vd {

// Phase 18 (extras): the names of 1.12's WTF folders under the app's storage folder (desktop: packs/.cache;
// Android: the app's files folder), shared by the macros and the account data's files (app/MacroScreens,
// app/AccountDataFiles): WTF\Account\<ACCOUNT>\ and ...\<realm>\<character>\.

// A folder name of WTF: the account in capitals, as the client writes it; characters a path cannot hold
// become '_'.
std::string wtfFolderName(std::string_view name, bool upper);

// A character renamed takes its folders along (WoW.exe 0x4725F0, on the rename's success at the character
// list): WTF\Account\<ACCOUNT>\<realm>\<old> and the older WTF\Account\<ACCOUNT>\<old> become <new> (0x42A820
// on each). A missing folder or a name already taken leaves that one as it is. True when one was moved.
bool renameCharacterFolders(const std::filesystem::path& folder, std::string_view account,
                            std::string_view realm, std::string_view from, std::string_view to);

} // namespace vd
