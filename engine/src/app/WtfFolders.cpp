#include "app/WtfFolders.h"

namespace vd {

std::string wtfFolderName(std::string_view name, bool upper) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                         c == '>' || c == '|' || static_cast<u8>(c) < 0x20;
        out.push_back(bad ? '_' : upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    }
    return out;
}

bool renameCharacterFolders(const std::filesystem::path& folder, std::string_view account,
                            std::string_view realm, std::string_view from, std::string_view to) {
    if (folder.empty() || account.empty() || from.empty() || to.empty() || from == to) {
        return false;
    }
    const std::filesystem::path accountFolder = folder / "WTF" / "Account" / wtfFolderName(account, true);
    const std::filesystem::path realmFolder = accountFolder / wtfFolderName(realm, false);
    bool moved = false;
    for (const std::filesystem::path& parent : {realmFolder, accountFolder}) {
        std::error_code error;
        const std::filesystem::path old = parent / wtfFolderName(from, false);
        const std::filesystem::path renamed = parent / wtfFolderName(to, false);
        if (std::filesystem::is_directory(old, error) && !std::filesystem::exists(renamed, error)) {
            std::filesystem::rename(old, renamed, error);
            moved = moved || !error;
        }
    }
    return moved;
}

} // namespace vd
