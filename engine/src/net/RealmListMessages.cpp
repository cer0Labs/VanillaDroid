#include "net/RealmListMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {

void writeRealmListRequest(std::vector<u8>& out) {
    ByteWriter(out).u8v(kCmdRealmList).u32v(0);
}

std::optional<std::vector<RealmInfo>> parseRealmList(std::span<const u8> body) {
    ByteReader r(body);
    r.u32v(); // unused
    const u8 count = r.u8v();
    std::vector<RealmInfo> realms;
    realms.reserve(count);
    for (u32 i = 0; i < count && r.ok(); ++i) {
        RealmInfo realm;
        realm.icon = r.u32v();
        realm.flags = r.u8v();
        realm.name = std::string(r.cstring());
        realm.address = std::string(r.cstring());
        realm.population = r.f32v();
        realm.characters = r.u8v();
        realm.category = r.u8v();
        r.u8v(); // 0 (the realm's id in later builds)
        realms.push_back(std::move(realm));
    }
    r.u16v(); // 2 (unused)
    if (!r.ok()) {
        return std::nullopt;
    }
    return realms;
}

i32 pickRealm(std::span<const RealmInfo> realms, std::string_view preferred) {
    if (!preferred.empty()) {
        for (usize i = 0; i < realms.size(); ++i) {
            if (realms[i].name == preferred) {
                return realms[i].offline() ? -1 : static_cast<i32>(i);
            }
        }
    }
    if (realms.size() == 1 && !realms[0].offline()) {
        return 0;
    }
    return -1;
}

} // namespace vd::net
