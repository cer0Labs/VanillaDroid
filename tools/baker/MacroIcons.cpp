// Phase 15 (options): the macro icons of ui.vdpk ("ui/macroicons", pack/UiFormat.h), as WoW.exe 1.12.1 lists them for
// GetNumMacroIcons / GetMacroIconInfo (0x4F0090, read from its disassembly): every file of Interface\Icons in the
// archives (the callback 0x4F04F0 takes *.blp and *.tga, not directories), its name without the extension, sorted with
// stricmp and without repeats (the same name in two archives, or as .blp and .tga, is one icon). The client also adds
// the Ability_* and Spell_* files of an Interface\Icons folder on disk; the 1.12.1 install has none.

#include "BakerCommon.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "pack/UiFormat.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace vd::baker {

std::vector<u8> bakeMacroIcons(const formats::MpqArchiveSet& mpq, usize& count) {
    const std::string directory = "Interface\\Icons\\";
    const auto lower = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    struct Icon {
        std::string key; // lowercase: the sort and the repeats
        std::string name;
    };
    std::vector<Icon> icons;
    for (const std::string& file : mpq.list((directory + "*").c_str())) {
        if (file.size() <= directory.size() || file.find('\\', directory.size()) != std::string::npos) {
            continue; // a file of a subdirectory
        }
        const std::string name = file.substr(directory.size());
        const usize dot = name.rfind('.');
        const std::string extension = dot == std::string::npos ? std::string{} : lower(name.substr(dot));
        if (extension != ".blp" && extension != ".tga") {
            continue;
        }
        icons.push_back({lower(name.substr(0, dot)), name.substr(0, dot)});
    }
    std::stable_sort(icons.begin(), icons.end(), [](const Icon& a, const Icon& b) { return a.key < b.key; });
    icons.erase(std::unique(icons.begin(), icons.end(), [](const Icon& a, const Icon& b) { return a.key == b.key; }),
                icons.end());
    count = icons.size();
    if (icons.empty()) {
        VD_LOG_ERROR("macro icons: no file in %s", directory.c_str());
    }

    std::vector<pack::UiMacroIcon> entries;
    std::string names;
    for (const Icon& icon : icons) {
        pack::UiMacroIcon entry{};
        entry.textureHash = assetPathHash(directory + icon.name + ".blp");
        entry.nameOffset = static_cast<u32>(names.size());
        entry.nameLength = static_cast<u32>(icon.name.size());
        entries.push_back(entry);
        names += icon.name;
        names += '\0';
    }
    pack::UiMacroIconsHeader header{};
    header.count = static_cast<u32>(entries.size());
    header.namesOffset = static_cast<u32>(sizeof(header) + entries.size() * sizeof(pack::UiMacroIcon));
    header.namesSize = static_cast<u32>(names.size());
    std::vector<u8> blob(header.namesOffset + names.size());
    std::memcpy(blob.data(), &header, sizeof(header));
    if (!entries.empty()) {
        std::memcpy(blob.data() + sizeof(header), entries.data(), entries.size() * sizeof(pack::UiMacroIcon));
    }
    if (!names.empty()) {
        std::memcpy(blob.data() + header.namesOffset, names.data(), names.size());
    }
    return blob;
}

} // namespace vd::baker
