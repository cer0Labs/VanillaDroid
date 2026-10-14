#include "net/SpellMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

std::optional<SpellModifierUpdate> parseSpellModifier(std::span<const u8> payload) {
    ByteReader r(payload);
    SpellModifierUpdate m;
    m.bit = r.u8v();
    m.op = r.u8v();
    m.value = r.i32v();
    if (!r.ok() || m.bit >= 64) {
        return std::nullopt;
    }
    return m;
}

} // namespace vd::net
