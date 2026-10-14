// Phase 15 (pets): the stable master's packets, a pet trainer's untraining offer, the pet bar's two-slot
// move, and the creature query's model, byte for byte as vmangos-core 4641790 writes and reads them
// (Handlers/NPCHandler.cpp SendStablePet / SendStableResult, Server/Packets/{Npc,Pet,Query}.cpp); truncated
// packets are rejected.
#include "net/ByteBuffer.h"
#include "net/Messages.h"
#include "net/PetMessages.h"

#include <doctest/doctest.h>

#include <ostream>
#include <vector>

namespace {

using namespace vd;
using Bytes = std::vector<u8>;

constexpr u64 kMaster = 0xF13000002AAB0011ull;

// WorldSession::SendStablePet: the master, the count, the bought slots, each pet (the current one flagged 1,
// the stabled ones 1 + their slot), as written there.
Bytes stableList() {
    Bytes body;
    net::ByteWriter w(body);
    w.u64v(kMaster).u8v(3).u8v(2);
    w.u32v(41).u32v(3825).u32v(18).cstring("Shadow").u32v(2).u8v(3); // a cat in stable slot 2, listed first
    w.u32v(40).u32v(1131).u32v(20).cstring("Fang").u32v(3).u8v(1);   // the current wolf
    w.u32v(42).u32v(2959).u32v(12).cstring("Tusk").u32v(1).u8v(2);   // a boar in stable slot 1
    return body;
}

} // namespace

TEST_CASE("Pet messages: MSG_LIST_STABLED_PETS as SendStablePet writes it") {
    net::StabledPetsList m;
    const Bytes body = stableList();
    REQUIRE(net::parseStabledPetsList(body, m));
    CHECK(m.stableMaster == kMaster);
    CHECK(m.slots == 2);
    REQUIRE(m.count == 3);
    CHECK(m.pets[0].petNumber == 41);
    CHECK(m.pets[0].entry == 3825);
    CHECK(m.pets[0].level == 18);
    CHECK(m.pets[0].name == "Shadow");
    CHECK(m.pets[0].loyalty == 2);
    CHECK(m.pets[0].slot == 3);
    CHECK(m.pets[1].name == "Fang");
    CHECK(m.pets[1].slot == net::kStableSlotCurrent);
    CHECK(m.pets[2].petNumber == 42);
    // Every cut of it is malformed; more pets than the stable holds too.
    for (usize n = 0; n < body.size(); ++n) {
        CHECK_FALSE(net::parseStabledPetsList({body.data(), n}, m));
    }
    Bytes tooMany = body;
    tooMany[8] = 4;
    CHECK_FALSE(net::parseStabledPetsList(tooMany, m));
    // No pets at all (a hunter without one).
    Bytes empty;
    net::ByteWriter(empty).u64v(kMaster).u8v(0).u8v(0);
    REQUIRE(net::parseStabledPetsList(empty, m));
    CHECK(m.count == 0);
}

TEST_CASE("Pet messages: SMSG_STABLE_RESULT, SMSG_PET_UNLEARN_CONFIRM and the requests") {
    u8 result = 0;
    REQUIRE(net::parseStableResult(Bytes{net::kStableSuccessBuySlot}, result));
    CHECK(result == 0x0A);
    CHECK_FALSE(net::parseStableResult(Bytes{}, result));

    Bytes confirm;
    net::ByteWriter(confirm).u64v(0xF140000000000456ull).u32v(1250);
    net::PetUnlearnConfirm m;
    REQUIRE(net::parsePetUnlearnConfirm(confirm, m));
    CHECK(m.pet == 0xF140000000000456ull);
    CHECK(m.cost == 1250);
    for (usize n = 0; n < confirm.size(); ++n) {
        CHECK_FALSE(net::parsePetUnlearnConfirm({confirm.data(), n}, m));
    }

    // CMSG_UNSTABLE_PET / CMSG_STABLE_SWAP_PET: npcGuid (u64) then petNumber (u32).
    Bytes out;
    net::writeStablePetNumber(out, kMaster, 42);
    CHECK(out == Bytes{0x11, 0x00, 0xAB, 0x2A, 0x00, 0x00, 0x30, 0xF1, 42, 0, 0, 0});

    // CMSG_PET_SET_ACTION with two actions: 24 bytes (PetSetAction::Read counts them by the size).
    out.clear();
    net::writePetSetActionPair(out, 0xF140000000000456ull, 3, 0x07000002u, 0, 0xC1000C26u);
    REQUIRE(out.size() == 24);
    net::ByteReader r(out);
    CHECK(r.u64v() == 0xF140000000000456ull);
    CHECK(r.u32v() == 3);
    CHECK(r.u32v() == 0x07000002u);
    CHECK(r.u32v() == 0);
    CHECK(r.u32v() == 0xC1000C26u);
}

TEST_CASE("Pet messages: the creature query keeps its model (a stabled pet's)") {
    // CreatureQueryResponse::AppendBodyTo: entry, name, three empty names, subname, type flags, type, family,
    // rank, a reserved 0, the pet spell list, display_id[0], civilian, racial leader.
    Bytes body;
    net::ByteWriter w(body);
    w.u32v(1131).cstring("Timber Wolf").cstring("").cstring("").cstring("").cstring("");
    w.u32v(1).u32v(1).u32v(1).u32v(0).u32v(0).u32v(0).u32v(903).u8v(0).u8v(0);
    const std::optional<net::CreatureQueryInfo> info = net::parseCreatureQueryResponse(body);
    REQUIRE(info);
    CHECK(info->name == "Timber Wolf");
    CHECK(info->family == 1);
    CHECK(info->displayId == 903);
    // An answer that stops after the rank still parses, without a model.
    const std::optional<net::CreatureQueryInfo> shorter =
        net::parseCreatureQueryResponse({body.data(), body.size() - 14});
    REQUIRE(shorter);
    CHECK(shorter->displayId == 0);
}
