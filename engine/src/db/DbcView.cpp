#include "db/DbcView.h"

#include "core/Assert.h"

#include <cstring>
#include <string>

namespace vd::db {
namespace {

constexpr usize kHeaderSize = 20; // magic + 4 x u32

u32 readU32(const u8* p) {
    u32 value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

} // namespace

Result<DbcView> DbcView::parse(const u8* data, usize size) {
    if (!data || size < kHeaderSize || std::memcmp(data, "WDBC", 4) != 0) {
        return Error{"Not a WDBC file"};
    }
    DbcView view;
    view.m_data = data;
    view.m_recordCount = readU32(data + 4);
    view.m_fieldCount = readU32(data + 8);
    view.m_recordSize = readU32(data + 12);
    view.m_stringBlockSize = readU32(data + 16);
    if (view.m_recordSize < view.m_fieldCount) {
        return Error{"DBC record size smaller than its field count"};
    }
    const u64 expected = kHeaderSize + u64{view.m_recordCount} * view.m_recordSize + view.m_stringBlockSize;
    if (expected != size) {
        return Error{"DBC size mismatch: header says " + std::to_string(expected) + " bytes, data has " +
                     std::to_string(size)};
    }
    return view;
}

u8 DbcView::getU8(u32 record, u32 byteOffset) const {
    VD_ASSERT(record < m_recordCount && byteOffset < m_recordSize);
    return m_data[kHeaderSize + usize{record} * m_recordSize + byteOffset];
}

u32 DbcView::getU32(u32 record, u32 field) const {
    VD_ASSERT(record < m_recordCount && field < m_fieldCount && usize{field} * 4 + 4 <= m_recordSize);
    return readU32(m_data + kHeaderSize + usize{record} * m_recordSize + usize{field} * 4);
}

f32 DbcView::getF32(u32 record, u32 field) const {
    const u32 bits = getU32(record, field);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string_view DbcView::getString(u32 record, u32 field) const {
    const u32 offset = getU32(record, field);
    if (offset >= m_stringBlockSize) {
        return {};
    }
    const char* strings = reinterpret_cast<const char*>(m_data + kHeaderSize + usize{m_recordCount} * m_recordSize);
    return std::string_view(strings + offset, ::strnlen(strings + offset, m_stringBlockSize - offset));
}

bool DbcView::looksLikeString(u32 record, u32 field) const {
    const u32 offset = getU32(record, field);
    if (offset == 0 || offset >= m_stringBlockSize) {
        return false;
    }
    // Must start right after a terminator, i.e. at the beginning of a string.
    const char* strings = reinterpret_cast<const char*>(m_data + kHeaderSize + usize{m_recordCount} * m_recordSize);
    return strings[offset - 1] == '\0' && strings[offset] != '\0';
}

u32 DbcView::findById(u32 id) const {
    for (u32 i = 0; i < m_recordCount; ++i) {
        if (getU32(i, 0) == id) {
            return i;
        }
    }
    return m_recordCount;
}

} // namespace vd::db
