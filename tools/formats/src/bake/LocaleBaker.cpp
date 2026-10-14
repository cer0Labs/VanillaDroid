#include "bake/LocaleBaker.h"

#include "formats/MpqArchiveSet.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace vd::bake {
namespace {

constexpr usize kDbcHeaderSize = 20;

// The field holds 0 or the start of a string (an empty one too).
bool stringStart(const db::DbcView& table, u32 record, u32 field) {
    const u32 offset = table.getU32(record, field);
    if (offset == 0) {
        return true;
    }
    if (offset >= table.stringBlockSize()) {
        return false;
    }
    return table.getString(record, field).data()[-1] == '\0'; // offset > 0: the byte before is in the block
}

bool isText(const db::DbcView& table, u32 first, u32 column) {
    bool used = false;
    for (u32 r = 0; r < table.recordCount(); ++r) {
        for (u32 k = 0; k < kLocaleColumns; ++k) {
            const u32 value = table.getU32(r, first + k);
            if (k != column) {
                if (value != 0) {
                    return false;
                }
            } else if (!stringStart(table, r, first + k)) {
                return false;
            } else {
                used |= value != 0;
            }
        }
    }
    return used;
}

} // namespace

std::vector<u32> findLocaleTexts(const db::DbcView& table, u32 column) {
    std::vector<u32> texts;
    if (column >= kLocaleColumns || table.hasPackedFields() || table.recordCount() == 0) {
        return texts;
    }
    u32 field = 0;
    while (field + kLocaleColumns < table.fieldCount()) { // the flags field follows the 8 strings
        if (isText(table, field, column)) {
            texts.push_back(field);
            field += kLocaleTextFields;
        } else {
            ++field;
        }
    }
    return texts;
}

int detectLocaleColumn(const db::DbcView& table) {
    int best = -1;
    usize bestCount = 0;
    for (u32 column = 0; column < kLocaleColumns; ++column) {
        const usize count = findLocaleTexts(table, column).size();
        if (count > bestCount) {
            best = static_cast<int>(column);
            bestCount = count;
        }
    }
    return best;
}

Result<std::vector<u8>> localizeDbc(std::span<const u8> dbc, u32 column, u32* texts) {
    Result<db::DbcView> parsed = db::DbcView::parse(dbc.data(), dbc.size());
    if (!parsed) {
        return parsed.error();
    }
    std::vector<u8> out(dbc.begin(), dbc.end());
    if (texts) {
        *texts = 0;
    }
    if (column == 0) {
        return out;
    }
    const db::DbcView& table = parsed.value();
    const std::vector<u32> found = findLocaleTexts(table, column);
    for (u32 r = 0; r < table.recordCount(); ++r) {
        for (const u32 first : found) {
            const u32 offset = table.getU32(r, first + column);
            std::memcpy(out.data() + kDbcHeaderSize + usize{r} * table.recordSize() + usize{first} * 4, &offset,
                        sizeof(offset));
        }
    }
    if (texts) {
        *texts = static_cast<u32>(found.size());
    }
    return out;
}

std::string detectClientLocale(const formats::MpqArchiveSet& mpq) {
    // Tables whose texts decide the client's language (votes: a client always translates these).
    static constexpr const char* kProbeTables[] = {"DBFilesClient\\ChrRaces.dbc", "DBFilesClient\\AreaTable.dbc",
                                                   "DBFilesClient\\ChrClasses.dbc"};
    std::array<u32, kLocaleColumns> votes{};
    for (const char* table : kProbeTables) {
        Result<std::vector<u8>> bytes = mpq.read(table);
        if (!bytes) {
            continue;
        }
        Result<db::DbcView> view = db::DbcView::parse(bytes.value().data(), bytes.value().size());
        if (!view) {
            continue;
        }
        if (const int column = detectLocaleColumn(view.value()); column >= 0) {
            ++votes[static_cast<usize>(column)];
        }
    }
    const auto best = std::max_element(votes.begin(), votes.end());
    return *best > 0 ? std::string(kClientLocales[static_cast<usize>(best - votes.begin())]) : std::string{};
}

} // namespace vd::bake
