#include "net/PetMessages.h"

namespace vd::net {

bool parseStabledPetsList(std::span<const u8> payload, StabledPetsList& out) {
    ByteReader r(payload);
    out.count = 0;
    out.stableMaster = r.u64v();
    const u8 count = r.u8v();
    out.slots = r.u8v();
    if (r.failed() || count > StabledPetsList::kMaxPets) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        StabledPetsList::Pet& p = out.pets[i];
        p.petNumber = r.u32v();
        p.entry = r.u32v();
        p.level = r.u32v();
        p.name = r.cstring();
        p.loyalty = r.u32v();
        p.slot = r.u8v();
    }
    if (!r.ok()) {
        return false;
    }
    out.count = count;
    return true;
}

bool parseStableResult(std::span<const u8> payload, u8& result) {
    ByteReader r(payload);
    result = r.u8v();
    return r.ok();
}

bool parsePetUnlearnConfirm(std::span<const u8> payload, PetUnlearnConfirm& out) {
    ByteReader r(payload);
    out.pet = r.u64v();
    out.cost = r.u32v();
    return r.ok();
}

void writeStablePetNumber(std::vector<u8>& out, u64 stableMaster, u32 petNumber) {
    ByteWriter(out).u64v(stableMaster).u32v(petNumber);
}

void writePetSetActionPair(std::vector<u8>& out, u64 pet, u32 position0, u32 button0, u32 position1,
                           u32 button1) {
    ByteWriter(out).u64v(pet).u32v(position0).u32v(button0).u32v(position1).u32v(button1);
}

} // namespace vd::net
