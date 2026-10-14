#include "formats/Dbc.h"

namespace vd::formats {

Result<DbcFile> DbcFile::parse(std::vector<u8> bytes) {
    Result<db::DbcView> view = db::DbcView::parse(bytes.data(), bytes.size());
    if (!view) {
        return view.error();
    }
    DbcFile file;
    file.m_bytes = std::move(bytes);
    file.m_view = view.value();
    return file;
}

} // namespace vd::formats
