#pragma once

#include "core/Types.h"

#include <array>
#include <string_view>

namespace vd {

// Phase 21 (languages): the client languages of 1.12, in the order of the 8 string columns of a DBC text and of the
// server's LocaleConstant (vMaNGOS: LOCALE_enUS = 0 ... LOCALE_esMX = 7). A client fills and reads only its own
// column; the baker moves the texts of another language to the first one, which the runtime reads.
inline constexpr std::array<std::string_view, 8> kClientLocales = {"enUS", "koKR", "frFR", "deDE",
                                                                   "zhCN", "zhTW", "esES", "esMX"};

// The column of a language code ("esES", any case), -1 when unknown.
constexpr int localeIndex(std::string_view code) {
    for (usize i = 0; i < kClientLocales.size(); ++i) {
        const std::string_view name = kClientLocales[i];
        bool same = name.size() == code.size();
        for (usize c = 0; same && c < name.size(); ++c) {
            const char a = name[c] >= 'A' && name[c] <= 'Z' ? static_cast<char>(name[c] - 'A' + 'a') : name[c];
            const char b = code[c] >= 'A' && code[c] <= 'Z' ? static_cast<char>(code[c] - 'A' + 'a') : code[c];
            same = a == b;
        }
        if (same) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The canonical spelling of a language code ("eses" -> "esES"), empty when unknown.
constexpr std::string_view canonicalLocale(std::string_view code) {
    const int index = localeIndex(code);
    return index >= 0 ? kClientLocales[static_cast<usize>(index)] : std::string_view{};
}

} // namespace vd
