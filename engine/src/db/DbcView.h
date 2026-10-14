#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <string_view>

namespace vd::db {

// Zero-copy view over a client DBC table ('WDBC': fixed-size records of 4-byte fields + string block),
// typically pointing straight into a memory-mapped pack. The bytes must outlive the view.
// Layout-agnostic: per-table field indices (5875) live with the code that consumes each table.
class DbcView {
public:
    static Result<DbcView> parse(const u8* data, usize size);

    u32 recordCount() const { return m_recordCount; }
    u32 fieldCount() const { return m_fieldCount; }
    u32 recordSize() const { return m_recordSize; }
    u32 stringBlockSize() const { return m_stringBlockSize; }

    // A few tables (CharBaseInfo, CharStartOutfit) pack 1-byte fields: recordSize < fieldCount * 4.
    // Read those with getU8 and a byte offset; the 4-byte accessors only work on regular tables.
    bool hasPackedFields() const { return m_recordSize < m_fieldCount * 4u; }
    u8 getU8(u32 record, u32 byteOffset) const;

    u32 getU32(u32 record, u32 field) const;
    i32 getI32(u32 record, u32 field) const { return static_cast<i32>(getU32(record, field)); }
    f32 getF32(u32 record, u32 field) const;
    // The field holds an offset into the string block; "" when out of range.
    std::string_view getString(u32 record, u32 field) const;
    // True if the field value is a valid offset to a non-empty string (heuristic for tools).
    bool looksLikeString(u32 record, u32 field) const;

    // Linear search on field 0 (the ID in every client table). Returns recordCount() if absent.
    u32 findById(u32 id) const;

private:
    const u8* m_data = nullptr;
    u32 m_recordCount = 0;
    u32 m_fieldCount = 0;
    u32 m_recordSize = 0;
    u32 m_stringBlockSize = 0;
};

} // namespace vd::db
