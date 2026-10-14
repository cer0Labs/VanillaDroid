// Phase 15 (guild, help): the guild management, charter, tabard designer, GM ticket, bug report and GM survey
// packets against the exact bytes vmangos-core 4641790 reads and writes (Server/Packets/Guild.cpp
// GuildSetPublicNote / GuildRank / SaveGuildEmblem; Petition.cpp PetitionBuy "guid, u32, u64, name, ten u32,
// u16, u8, u32 index, u32", PetitionShowList, PetitionSign "guid, skipped u8", PetitionQueryResponse,
// PetitionRename; Guild/GuildMgr.cpp BuildSignatureData "guid, then data << 0 (four bytes)"; GmTicket.cpp
// GmTicketCreate "type, map, x, y, z, text, reserved", GmTicketGetTicket "status, then the ticket with
// HASTEXT", GMSurveySubmit "id per question, 0 ends"; Misc.cpp Bug "suggestion, length, content, length,
// type"), every truncation rejected.
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/GuildMessages.h"
#include "net/HelpMessages.h"
#include "net/SocialMessages.h"

#include <doctest/doctest.h>

#include <ostream>
#include <vector>

namespace {

using namespace vd;
using namespace vd::net;

using Bytes = std::vector<u8>;

constexpr u64 kRegistrar = 0xF1300000000013B2ull;
constexpr u64 kCharter = 0x4000000000000A21ull;
constexpr u64 kMe = 0x11;
constexpr u64 kAeryn = 0x21;
constexpr u64 kBrakk = 0x22;

template <typename T, typename Parse> void rejectsTruncation(const Bytes& whole, T& out, Parse parse) {
    for (usize length = 0; length < whole.size(); ++length) {
        CHECK_FALSE(parse(std::span<const u8>(whole.data(), length), out));
    }
}

} // namespace

TEST_CASE("Guild messages: notes, ranks and the information text") {
    Bytes out;
    writeGuildSetNote(out, "Aeryn", "Tanque");
    Bytes expected;
    ByteWriter(expected).cstring("Aeryn").cstring("Tanque");
    CHECK(out == expected);

    out.clear();
    writeGuildRank(out, 2, 0x40 | 0x1 | 0x2 | 0x2000, "Veterano");
    expected.clear();
    ByteWriter(expected).u32v(2).u32v(0x2043).cstring("Veterano");
    CHECK(out == expected);

    out.clear();
    writeCString(out, "Raid los viernes"); // CMSG_GUILD_INFO_TEXT, CMSG_GUILD_ADD_RANK
    CHECK(out == Bytes{'R', 'a', 'i', 'd', ' ', 'l', 'o', 's', ' ', 'v', 'i', 'e', 'r', 'n', 'e', 's', 0});
}

TEST_CASE("Guild messages: the tabard designer") {
    Bytes out;
    GuildEmblemChoice e;
    e.emblemStyle = 169;
    e.emblemColor = 16;
    e.borderStyle = 5;
    e.borderColor = 3;
    e.backgroundColor = 50;
    writeSaveGuildEmblem(out, kRegistrar, e);
    Bytes expected;
    ByteWriter(expected).u64v(kRegistrar).u32v(169).u32v(16).u32v(5).u32v(3).u32v(50);
    CHECK(out == expected);

    // MSG_TABARDVENDOR_ACTIVATE: the vendor's GUID; MSG_SAVE_GUILD_EMBLEM: a u32.
    Bytes activate;
    writeGuid(activate, kRegistrar);
    u64 vendor = 0;
    REQUIRE(parseGuid(activate, vendor));
    CHECK(vendor == kRegistrar);
    rejectsTruncation(activate, vendor, parseGuid);
    u32 result = 9;
    Bytes saved{4, 0, 0, 0};
    REQUIRE(parseU32(saved, result));
    CHECK(result == kEmblemNotEnoughMoney);
}

TEST_CASE("Guild messages: the registrar's list and buying a charter") {
    Bytes list;
    ByteWriter(list).u64v(kRegistrar).u8v(1).u32v(1).u32v(kGuildCharterEntry).u32v(16161).u32v(1000).u32v(1);
    PetitionShowList m;
    REQUIRE(parsePetitionShowList(list, m));
    CHECK(m.npc == kRegistrar);
    REQUIRE(m.count == 1);
    CHECK(m.entries[0].index == 1);
    CHECK(m.entries[0].charterEntry == kGuildCharterEntry);
    CHECK(m.entries[0].charterDisplay == 16161);
    CHECK(m.entries[0].cost == 1000);
    CHECK(m.entries[0].flags == 1);
    rejectsTruncation(list, m, parsePetitionShowList);
    Bytes tooMany;
    ByteWriter(tooMany).u64v(kRegistrar).u8v(11);
    CHECK_FALSE(parsePetitionShowList(tooMany, m));

    Bytes out;
    writePetitionBuy(out, kRegistrar, "Los Errantes", 1);
    Bytes expected;
    ByteWriter w(expected);
    w.u64v(kRegistrar).u32v(0).u64v(0).cstring("Los Errantes");
    for (int i = 0; i < 10; ++i) {
        w.u32v(0);
    }
    w.u16v(0).u8v(0).u32v(1).u32v(0);
    CHECK(out == expected);
    // vMaNGOS reads exactly that: 8 + 4 + 8 + name + 40 + 2 + 1 + 4 + 4 bytes.
    CHECK(out.size() == 8 + 4 + 8 + 13 + 40 + 2 + 1 + 4 + 4);
}

TEST_CASE("Guild messages: a charter's signatures, signing, the query, offering and renaming") {
    Bytes shown;
    ByteWriter(shown).u64v(kCharter).u64v(kMe).u32v(77).u8v(2).u64v(kAeryn).u32v(0).u64v(kBrakk).u32v(0);
    PetitionSignatures s;
    REQUIRE(parsePetitionShowSignatures(shown, s));
    CHECK(s.item == kCharter);
    CHECK(s.owner == kMe);
    CHECK(s.petition == 77);
    REQUIRE(s.count == 2);
    CHECK(s.signers[0] == kAeryn);
    CHECK(s.signers[1] == kBrakk);
    rejectsTruncation(shown, s, parsePetitionShowSignatures);
    Bytes tooMany;
    ByteWriter(tooMany).u64v(kCharter).u64v(kMe).u32v(77).u8v(10);
    CHECK_FALSE(parsePetitionShowSignatures(tooMany, s));

    Bytes out;
    writePetitionSign(out, kCharter);
    Bytes expected;
    ByteWriter(expected).u64v(kCharter).u8v(1);
    CHECK(out == expected);

    Bytes result;
    ByteWriter(result).u64v(kCharter).u64v(kAeryn).u32v(kPetitionAlreadySigned);
    PetitionSignResult r;
    REQUIRE(parsePetitionSignResults(result, r));
    CHECK(r.item == kCharter);
    CHECK(r.signer == kAeryn);
    CHECK(r.result == kPetitionAlreadySigned);
    rejectsTruncation(result, r, parsePetitionSignResults);

    out.clear();
    writePetitionQuery(out, 77, kCharter);
    expected.clear();
    ByteWriter(expected).u32v(77).u64v(kCharter);
    CHECK(out == expected);

    Bytes response;
    ByteWriter q(response);
    q.u32v(77).u64v(kMe).cstring("Los Errantes").cstring("").u32v(1).u32v(9).u32v(9);
    q.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u16v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    PetitionQueryResponse p;
    REQUIRE(parsePetitionQueryResponse(response, p));
    CHECK(p.petition == 77);
    CHECK(p.owner == kMe);
    CHECK(p.name == "Los Errantes");
    CHECK(p.flags == 1);
    CHECK(p.minSignatures == 9);
    CHECK(p.maxSignatures == 9);
    rejectsTruncation(response, p, parsePetitionQueryResponse);

    out.clear();
    writeOfferPetition(out, kCharter, kAeryn);
    expected.clear();
    ByteWriter(expected).u64v(kCharter).u64v(kAeryn);
    CHECK(out == expected);

    out.clear();
    writePetitionRename(out, kCharter, "Los Otros");
    expected.clear();
    ByteWriter(expected).u64v(kCharter).cstring("Los Otros");
    CHECK(out == expected);
    PetitionRename n;
    REQUIRE(parsePetitionRename(expected, n));
    CHECK(n.item == kCharter);
    CHECK(n.name == "Los Otros");
    rejectsTruncation(expected, n, parsePetitionRename);
}

TEST_CASE("Help messages: tickets, their answers, bugs and the survey") {
    Bytes out;
    GmTicketCreate t;
    t.category = kTicketStuck;
    t.map = 1;
    t.x = 1.5f;
    t.y = -2.0f;
    t.z = 3.25f;
    t.text = "Atascado en una roca";
    writeGmTicketCreate(out, t);
    Bytes expected;
    ByteWriter(expected)
        .u8v(1)
        .u32v(1)
        .f32v(1.5f)
        .f32v(-2.0f)
        .f32v(3.25f)
        .cstring("Atascado en una roca")
        .cstring("Reserved for future use");
    CHECK(out == expected);

    out.clear();
    writeGmTicketUpdateText(out, 4, "Otro texto");
    expected.clear();
    ByteWriter(expected).u8v(4).cstring("Otro texto");
    CHECK(out == expected);

    // SMSG_GMTICKET_GETTICKET: no ticket, then one.
    GmTicket g;
    Bytes none{0x0A, 0, 0, 0};
    REQUIRE(parseGmTicket(none, g));
    CHECK(g.status == kTicketStatusDefault);
    CHECK(g.text.empty());
    Bytes ticket;
    ByteWriter(ticket)
        .u32v(kTicketStatusHasText)
        .cstring("Ayuda")
        .u8v(3)
        .f32v(0.5f)
        .f32v(1.0f)
        .f32v(0.01f)
        .u8v(2)
        .u8v(1);
    REQUIRE(parseGmTicket(ticket, g));
    CHECK(g.text == "Ayuda");
    CHECK(g.category == 3);
    CHECK(g.ticketAge == doctest::Approx(0.5f));
    CHECK(g.oldestTicket == doctest::Approx(1.0f));
    CHECK(g.updateAge == doctest::Approx(0.01f));
    CHECK(g.escalation == 2);
    CHECK(g.openedByGm == 1);
    rejectsTruncation(ticket, g, parseGmTicket);

    out.clear();
    writeBug(out, true, "Arthel Human Mage", "Mas sillas");
    expected.clear();
    ByteWriter(expected).u32v(1).u32v(18).cstring("Arthel Human Mage").u32v(11).cstring("Mas sillas");
    CHECK(out == expected);

    // The survey: three answers then the 0 that ends the list, then the comment.
    out.clear();
    const GmSurveyAnswer answers[] = {{28, 5, ""}, {29, 4, ""}, {30, 0, ""}};
    writeGmSurveySubmit(out, 6, answers, "Gracias");
    expected.clear();
    ByteWriter(expected)
        .u32v(6)
        .u32v(28)
        .u8v(5)
        .cstring("")
        .u32v(29)
        .u8v(4)
        .cstring("")
        .u32v(30)
        .u8v(0)
        .cstring("")
        .u32v(0)
        .cstring("Gracias");
    CHECK(out == expected);
    // Ten answers: no terminator (GMSurveySubmit::ReadFromWorldPacket reads ten at most).
    out.clear();
    std::vector<GmSurveyAnswer> ten(10, GmSurveyAnswer{31, 3, ""});
    writeGmSurveySubmit(out, 6, ten, "");
    CHECK(out.size() == 4 + 10 * (4 + 1 + 1) + 1);
}
