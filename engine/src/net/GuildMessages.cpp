#include "net/GuildMessages.h"

namespace vd::net {

// -------------------------------------------------------------------------------------------------------
// Guild management.

void writeGuildSetNote(std::vector<u8>& out, std::string_view member, std::string_view note) {
    ByteWriter(out).cstring(member).cstring(note);
}

void writeGuildRank(std::vector<u8>& out, u32 rankId, u32 rights, std::string_view name) {
    ByteWriter(out).u32v(rankId).u32v(rights).cstring(name);
}

// -------------------------------------------------------------------------------------------------------
// Tabard designer.

void writeSaveGuildEmblem(std::vector<u8>& out, u64 vendor, const GuildEmblemChoice& emblem) {
    ByteWriter w(out);
    w.u64v(vendor);
    w.u32v(static_cast<u32>(emblem.emblemStyle)).u32v(static_cast<u32>(emblem.emblemColor));
    w.u32v(static_cast<u32>(emblem.borderStyle)).u32v(static_cast<u32>(emblem.borderColor));
    w.u32v(static_cast<u32>(emblem.backgroundColor));
}

// -------------------------------------------------------------------------------------------------------
// Guild charters.

bool parsePetitionShowList(std::span<const u8> payload, PetitionShowList& out) {
    ByteReader r(payload);
    out.npc = r.u64v();
    out.count = r.u8v();
    if (r.failed() || out.count > kPetitionShowListMax) {
        out.count = 0;
        return false;
    }
    for (u32 i = 0; i < out.count; ++i) {
        PetitionShowList::Entry& e = out.entries[i];
        e.index = r.u32v();
        e.charterEntry = r.u32v();
        e.charterDisplay = r.u32v();
        e.cost = r.i32v();
        e.flags = r.i32v();
    }
    if (r.failed()) {
        out.count = 0;
    }
    return r.ok();
}

void writePetitionBuy(std::vector<u8>& out, u64 npc, std::string_view name, u32 index) {
    ByteWriter w(out);
    w.u64v(npc).u32v(0).u64v(0).cstring(name);
    for (u32 i = 0; i < 10; ++i) {
        w.u32v(0);
    }
    w.u16v(0).u8v(0).u32v(index).u32v(0);
}

bool parsePetitionShowSignatures(std::span<const u8> payload, PetitionSignatures& out) {
    ByteReader r(payload);
    out.item = r.u64v();
    out.owner = r.u64v();
    out.petition = r.u32v();
    out.count = r.u8v();
    if (r.failed() || out.count > kPetitionSignaturesMax) {
        out.count = 0;
        return false;
    }
    for (u32 i = 0; i < out.count; ++i) {
        out.signers[i] = r.u64v();
        r.u32v(); // Petition::BuildSignatureData's "data << 0" (an int: four bytes)
    }
    if (r.failed()) {
        out.count = 0;
    }
    return r.ok();
}

void writePetitionSign(std::vector<u8>& out, u64 item) {
    ByteWriter(out).u64v(item).u8v(1); // SignPetition() without an argument: 1 (WoW.exe 0x4F46D9)
}

bool parsePetitionSignResults(std::span<const u8> payload, PetitionSignResult& out) {
    ByteReader r(payload);
    out.item = r.u64v();
    out.signer = r.u64v();
    out.result = r.u32v();
    return r.ok();
}

void writePetitionQuery(std::vector<u8>& out, u32 petition, u64 item) {
    ByteWriter(out).u32v(petition).u64v(item);
}

bool parsePetitionQueryResponse(std::span<const u8> payload, PetitionQueryResponse& out) {
    ByteReader r(payload);
    out.petition = r.u32v();
    out.owner = r.u64v();
    out.name = r.cstring();
    out.body = r.cstring();
    out.flags = r.u32v();
    out.minSignatures = r.u32v();
    out.maxSignatures = r.u32v();
    out.deadline = r.u32v();
    out.created = r.u32v();
    out.allowedGuild = r.u32v();
    out.allowedClasses = r.u32v();
    out.allowedRaces = r.u32v();
    out.allowedGender = r.u16v();
    out.allowedMinLevel = r.u32v();
    out.allowedMaxLevel = r.u32v();
    out.choices = r.u32v();
    // Each choice is at least its terminator: a count larger than what is left is malformed.
    if (r.failed() || out.choices > r.remaining()) {
        return false;
    }
    for (u32 i = 0; i < out.choices; ++i) {
        r.cstring();
    }
    out.defaultChoice = r.u32v();
    return r.ok();
}

void writeOfferPetition(std::vector<u8>& out, u64 item, u64 player) {
    ByteWriter(out).u64v(item).u64v(player);
}

void writePetitionRename(std::vector<u8>& out, u64 item, std::string_view name) {
    ByteWriter(out).u64v(item).cstring(name);
}

bool parsePetitionRename(std::span<const u8> payload, PetitionRename& out) {
    ByteReader r(payload);
    out.item = r.u64v();
    out.name = r.cstring();
    return r.ok();
}

} // namespace vd::net
