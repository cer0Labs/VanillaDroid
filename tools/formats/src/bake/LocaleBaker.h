#pragma once

#include "core/Locale.h"
#include "core/Result.h"
#include "core/Types.h"
#include "db/DbcView.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::formats {
class MpqArchiveSet;
}

namespace vd::bake {

// Phase 21 (languages): clients in other languages. A 1.12 DBC text is 8 string fields, one per language in the order
// of core/Locale (kClientLocales), then a flags field; a client fills only the column of its own language (the enUS
// client the first one, the esES client the seventh) and reads that one. The runtime always reads the first column
// (the code names the enUS field), so the baker moves the texts of another language there.
constexpr u32 kLocaleColumns = 8;   // string fields of a text
constexpr u32 kLocaleTextFields = 9; // + its flags

// The column of a language code ("esES", any case), -1 when unknown.
inline int localeColumn(std::string_view code) {
    return localeIndex(code);
}

// The first field of each text of `table` whose language column `column` is the one in use: 8 fields of which only
// `column` is ever set, each the start of a string. Finds the 66 texts of the 5875 tables in the esES client exactly
// (checked against a field-by-field comparison with the enUS client). Packed tables have none.
std::vector<u32> findLocaleTexts(const db::DbcView& table, u32 column);

// The language column a table's texts use: the one findLocaleTexts finds the most texts in, -1 when it has none.
int detectLocaleColumn(const db::DbcView& table);

// The language of a client's texts ("enUS", "esES"...: which of the 8 columns of a DBC text it fills, voted by three
// tables a client always translates), empty when none is found. Phase 22: in vd_formats (it was the baker's) so
// VanillaDroid Setup checks the player's client the same way.
std::string detectClientLocale(const formats::MpqArchiveSet& mpq);

// A copy of `dbc` with the texts of `column` in the first column of each text (the same string block: the offsets
// are copied). `texts` receives how many texts moved. Column 0 returns the table as it is.
Result<std::vector<u8>> localizeDbc(std::span<const u8> dbc, u32 column, u32* texts = nullptr);

} // namespace vd::bake
