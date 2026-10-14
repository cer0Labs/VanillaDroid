#include "net/CharRenameMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

void writeCharRename(std::vector<u8>& out, u64 guid, std::string_view name) {
    ByteWriter(out).u64v(guid).cstring(name);
}

std::optional<CharRenameResult> parseCharRename(std::span<const u8> payload) {
    ByteReader r(payload);
    CharRenameResult m;
    m.result = r.u8v();
    if (r.ok() && m.result == 0) { // RESPONSE_SUCCESS
        m.guid = r.u64v();
        m.name = std::string(r.cstring());
    }
    if (!r.ok()) {
        return std::nullopt;
    }
    return m;
}

} // namespace vd::net
