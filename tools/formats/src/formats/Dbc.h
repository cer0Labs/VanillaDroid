#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "db/DbcView.h"

#include <vector>

namespace vd::formats {

// Owning DBC table for tools (bytes read from an MPQ). All accessors come from db::DbcView, the
// zero-copy reader the runtime uses on baked packs.
class DbcFile {
public:
    static Result<DbcFile> parse(std::vector<u8> bytes);

    DbcFile(DbcFile&&) = default;
    DbcFile& operator=(DbcFile&&) = default;
    DbcFile(const DbcFile&) = delete;
    DbcFile& operator=(const DbcFile&) = delete;

    const db::DbcView& view() const { return m_view; }
    const db::DbcView* operator->() const { return &m_view; }

private:
    DbcFile() = default;

    std::vector<u8> m_bytes; // moving the vector keeps its buffer, so m_view stays valid
    db::DbcView m_view;
};

} // namespace vd::formats
