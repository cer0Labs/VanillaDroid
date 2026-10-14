// Phase 18 (glue): the realm list and the rename against the exact bytes vmangos-core 4641790 writes and
// reads (src/realmd/AuthSocket.cpp _HandleRealmList "ReadSkip(4)" and LoadRealmlistAndWriteIntoBuffer for
// builds before 6299: "uint32 0, uint8 count, then per realm uint32 icon, uint8 realmflags, name, address,
// float population, uint8 characters, uint8 category, uint8 0, then uint16 2"; Server/Packets/Character.cpp
// CharRename "guid, newname" and CharRenameResponse "result [, guid, name]"), including every truncation; and
// the realm server's address as a player writes it (1.12's realmlist).
#include "net/ByteBuffer.h"
#include "net/CharRenameMessages.h"
#include "net/NetClient.h"
#include "net/RealmListMessages.h"

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

namespace {

using namespace vd;
using namespace vd::net;

using Bytes = std::vector<u8>;

struct SampleRealm {
    u32 icon;
    u8 flags;
    const char* name;
    const char* address;
    f32 population;
    u8 characters;
    u8 category;
};

// The body realmd sends after the command byte and the size (std::map order: by name).
Bytes realmListBody(std::initializer_list<SampleRealm> realms) {
    Bytes out;
    ByteWriter w(out);
    w.u32v(0).u8v(static_cast<u8>(realms.size()));
    for (const SampleRealm& r : realms) {
        w.u32v(r.icon).u8v(r.flags).cstring(r.name).cstring(r.address).f32v(r.population);
        w.u8v(r.characters).u8v(r.category).u8v(0);
    }
    w.u16v(2);
    return out;
}

} // namespace

TEST_CASE("Realm list: the request") {
    Bytes out;
    writeRealmListRequest(out);
    CHECK(out == Bytes{0x10, 0, 0, 0, 0});
}

TEST_CASE("Realm list: vMaNGOS' answer for a 1.12 client") {
    const Bytes body = realmListBody({
        {1, 0x00, "Dev Realm", "192.168.1.20:8085", 0.5f, 3, 1},
        {6, kRealmFlagOffline, "Zz Test (1,12,1)", "127.0.0.1:8086", 0.0f, 0, 1},
    });
    const std::optional<std::vector<RealmInfo>> realms = parseRealmList(body);
    REQUIRE(realms);
    REQUIRE(realms->size() == 2);
    const RealmInfo& dev = (*realms)[0];
    CHECK(dev.icon == 1);
    CHECK(dev.flags == 0);
    CHECK(dev.name == "Dev Realm");
    CHECK(dev.address == "192.168.1.20:8085");
    CHECK(dev.population == doctest::Approx(0.5f));
    CHECK(dev.characters == 3);
    CHECK(dev.category == 1);
    CHECK_FALSE(dev.offline());
    CHECK((*realms)[1].offline());
    CHECK((*realms)[1].icon == 6);
    // Truncated anywhere (the trailing u16 included): refused.
    for (usize size = 0; size < body.size(); ++size) {
        CHECK_FALSE(parseRealmList(std::span<const u8>(body.data(), size)));
    }
    // No realms at all is a valid list.
    const Bytes empty = realmListBody({});
    const std::optional<std::vector<RealmInfo>> none = parseRealmList(empty);
    REQUIRE(none);
    CHECK(none->empty());
}

TEST_CASE("Realm list: the realm the session takes by itself") {
    const std::vector<RealmInfo> two = *parseRealmList(realmListBody({
        {1, 0, "Alpha", "a:1", 1.0f, 0, 1},
        {1, kRealmFlagOffline, "Beta", "b:1", 1.0f, 2, 1},
    }));
    // The last realm, when listed and up.
    CHECK(pickRealm(two, "Alpha") == 0);
    // Down, unknown or none saved, with two realms: the player chooses.
    CHECK(pickRealm(two, "Beta") == -1);
    CHECK(pickRealm(two, "Gamma") == -1);
    CHECK(pickRealm(two, "") == -1);
    // A single realm up: that one.
    const std::vector<RealmInfo> one = *parseRealmList(realmListBody({{1, 0, "Solo", "s:1", 0.0f, 0, 1}}));
    CHECK(pickRealm(one, "") == 0);
    CHECK(pickRealm(one, "Gone") == 0);
    const std::vector<RealmInfo> down =
        *parseRealmList(realmListBody({{1, kRealmFlagOffline, "Solo", "s:1", 0, 0, 1}}));
    CHECK(pickRealm(down, "") == -1);
    CHECK(pickRealm({}, "") == -1);
}

TEST_CASE("Rename: CMSG_CHAR_RENAME") {
    Bytes out;
    writeCharRename(out, 0x0000000000000042ull, "Newname");
    Bytes expected;
    ByteWriter(expected).u64v(0x42).cstring("Newname");
    CHECK(out == expected);
    CHECK(out.size() == 8 + 8);
}

TEST_CASE("Rename: SMSG_CHAR_RENAME") {
    // Success: the result, the character and its new name.
    Bytes ok;
    ByteWriter(ok).u8v(0).u64v(0x42).cstring("Newname");
    const std::optional<CharRenameResult> done = parseCharRename(ok);
    REQUIRE(done);
    CHECK(done->result == 0);
    CHECK(done->guid == 0x42);
    CHECK(done->name == "Newname");
    for (usize size = 0; size < ok.size(); ++size) {
        CHECK_FALSE(parseCharRename(std::span<const u8>(ok.data(), size)));
    }
    // A refusal is the result alone (CHAR_NAME_TOO_SHORT, CHAR_CREATE_ERROR...).
    const Bytes refused{70};
    const std::optional<CharRenameResult> no = parseCharRename(refused);
    REQUIRE(no);
    CHECK(no->result == 70);
    CHECK(no->guid == 0);
    CHECK(no->name.empty());
    CHECK_FALSE(parseCharRename({}));
}

TEST_CASE("Realm server address: host, host:port, the default port") {
    std::string host = "unchanged";
    u16 port = 1;
    CHECK(parseRealmdAddress("logon.example.org", host, port) == AddressError::None);
    CHECK(host == "logon.example.org");
    CHECK(port == kRealmdPort);
    CHECK(parseRealmdAddress(" 192.168.1.20:3725\t", host, port) == AddressError::None);
    CHECK(host == "192.168.1.20");
    CHECK(port == 3725);
    CHECK(parseRealmdAddress("localhost:65535", host, port) == AddressError::None);
    CHECK(port == 65535);
    // Refused: what is wrong, and the outputs untouched.
    host = "kept";
    port = 7;
    for (const char* bad : {"host:", "host:0", "host:65536", "host:99999999999", "host:3a", "host: 3724"}) {
        INFO(bad);
        CHECK(parseRealmdAddress(bad, host, port) == AddressError::BadPort);
    }
    for (const char* noHost : {"", "   ", ":3724", ":", ":x"}) {
        INFO(noHost);
        CHECK(parseRealmdAddress(noHost, host, port) == AddressError::NoHost);
    }
    CHECK(host == "kept");
    CHECK(port == 7);
}
