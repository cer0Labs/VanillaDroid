#include "formats/Dbc.h"
#include "formats/MpqArchiveSet.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <cstring>

namespace {

// Builds a tiny WDBC in memory: 2 records x 2 fields (id, name offset).
std::vector<vd::u8> makeDbc() {
    const char strings[] = "\0Azeroth\0Kalimdor"; // offsets 1 and 9
    std::vector<vd::u8> bytes(20 + 2 * 8 + sizeof(strings));
    auto put = [&](std::size_t at, vd::u32 value) { std::memcpy(bytes.data() + at, &value, 4); };
    std::memcpy(bytes.data(), "WDBC", 4);
    put(4, 2);
    put(8, 2);
    put(12, 8);
    put(16, sizeof(strings));
    put(20, 0);
    put(24, 1);
    put(28, 1);
    put(32, 9);
    std::memcpy(bytes.data() + 36, strings, sizeof(strings));
    return bytes;
}

} // namespace

TEST_CASE("DbcFile parses records and strings") {
    vd::Result<vd::formats::DbcFile> dbc = vd::formats::DbcFile::parse(makeDbc());
    REQUIRE(dbc.ok());
    CHECK(dbc.value()->recordCount() == 2);
    CHECK(dbc.value()->getString(0, 1) == "Azeroth");
    CHECK(dbc.value()->getString(1, 1) == "Kalimdor");
    CHECK(dbc.value()->findById(1) == 1);
    CHECK(dbc.value()->findById(7) == 2);
}

TEST_CASE("DbcFile rejects bad input") {
    std::vector<vd::u8> truncated = makeDbc();
    truncated.pop_back();
    CHECK_FALSE(vd::formats::DbcFile::parse(truncated).ok());
    CHECK_FALSE(vd::formats::DbcFile::parse(std::vector<vd::u8>(8, 0)).ok());
}

TEST_CASE("Every client DBC parses, including tables with 1-byte fields") {
    VD_REQUIRE_MPQ(mpq);
    const std::vector<std::string> tables = mpq->list("DBFilesClient\\*.dbc");
    CHECK(tables.size() == 158);
    for (const std::string& name : tables) {
        CAPTURE(name);
        vd::Result<std::vector<vd::u8>> bytes = mpq->read(name);
        REQUIRE(bytes.ok());
        CHECK(vd::formats::DbcFile::parse(std::move(bytes.value())).ok());
    }
    // CharBaseInfo: race (u8), class (u8) per record.
    vd::Result<std::vector<vd::u8>> bytes = mpq->read("DBFilesClient\\CharBaseInfo.dbc");
    REQUIRE(bytes.ok());
    vd::Result<vd::formats::DbcFile> baseInfo = vd::formats::DbcFile::parse(std::move(bytes.value()));
    REQUIRE(baseInfo.ok());
    CHECK(baseInfo.value()->hasPackedFields());
    CHECK(baseInfo.value()->recordSize() == 2);
    CHECK(baseInfo.value()->getU8(0, 0) >= 1); // race ids start at 1 (Human)
}

TEST_CASE("Map.dbc from the client lists the continents") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<std::vector<vd::u8>> bytes = mpq->read("DBFilesClient\\Map.dbc");
    REQUIRE(bytes.ok());
    vd::Result<vd::formats::DbcFile> map = vd::formats::DbcFile::parse(std::move(bytes.value()));
    REQUIRE(map.ok());
    // Map.dbc (5875): field 0 = ID, field 1 = directory name.
    const vd::u32 azeroth = map.value()->findById(0);
    const vd::u32 kalimdor = map.value()->findById(1);
    REQUIRE(azeroth < map.value()->recordCount());
    REQUIRE(kalimdor < map.value()->recordCount());
    CHECK(map.value()->getString(azeroth, 1) == "Azeroth");
    CHECK(map.value()->getString(kalimdor, 1) == "Kalimdor");
}
