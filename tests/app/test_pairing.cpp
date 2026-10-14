// Phase 22 (pairing): the app's side of pairing with VanillaDroid Setup (app/Pairing) and the rules of the
// download (app/DataDownload): the link of the PC's page, the code, pairing.json, when the first-run screen
// opens, files.json, what to fetch and how to resume.
#include "app/DataDownload.h"
#include "app/Pairing.h"
#include "pack/PackManifest.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <ostream> // doctest prints std::string values through operator<<
#include <string>

using namespace vd;

namespace {

const std::string kKey = "00112233445566778899aabbccddeeff";

std::filesystem::path tempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vd_pairing_tests" / name;
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

net::HttpResponseHead head(const std::string& text) {
    Result<net::HttpResponseHead> parsed = net::parseResponseHead(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

} // namespace

TEST_CASE("Pairing: the link of the PC's page") {
    std::optional<PairedPc> pc = parsePairingLink("vanilladroid://pair?h=192.168.1.20:47800&k=" + kKey);
    REQUIRE(pc);
    CHECK(pc->endpoint.host == "192.168.1.20");
    CHECK(pc->endpoint.port == 47800);
    CHECK(pc->key == kKey);
    CHECK_FALSE(pc->pending);
    CHECK(pc->valid());

    // Any case in the scheme, the host and the key (kept lowercase); a slash before the query; the port's
    // default; percent-encoding; other parameters ignored.
    pc = parsePairingLink(
        "VanillaDroid://PAIR/?x=1&k=00112233445566778899AABBCCDDEEFF&h=10.0.0.5%3A47805#top");
    REQUIRE(pc);
    CHECK(pc->endpoint.text() == "10.0.0.5:47805");
    CHECK(pc->key == kKey);
    pc = parsePairingLink("vanilladroid://pair?h=10.0.0.5&k=" + kKey);
    REQUIRE(pc);
    CHECK(pc->endpoint.port == kSetupPort);
    CHECK(parsePairingLink("vanilladroid://pair?h=127.0.0.1:47800&k=" + kKey)); // adb reverse

    // Not ours, or not on the local network: nothing.
    CHECK_FALSE(parsePairingLink("vanilladroid://pair?h=8.8.8.8:47800&k=" + kKey));
    CHECK_FALSE(parsePairingLink("vanilladroid://pair?h=evil.example.com:47800&k=" + kKey));
    CHECK_FALSE(parsePairingLink("vanilladroid://pair?h=192.168.1.20:47800"));
    CHECK_FALSE(parsePairingLink("vanilladroid://pair?h=192.168.1.20:47800&k=0011"));
    CHECK_FALSE(parsePairingLink("vanilladroid://pair?h=192.168.1.20:47800&k=" + std::string(32, 'g')));
    CHECK_FALSE(parsePairingLink("vanilladroid://other?h=192.168.1.20:47800&k=" + kKey));
    CHECK_FALSE(parsePairingLink("http://192.168.1.20:47800/?k=" + kKey));
    CHECK_FALSE(parsePairingLink("vanilladroid://pair"));
    CHECK_FALSE(parsePairingLink("vanilladroid://pair?h=192.168.1.20%ZZ&k=" + kKey));
    CHECK_FALSE(parsePairingLink("vanilladroid://pair?h=192.168.1.20%0A&k=" + kKey));
    CHECK_FALSE(parsePairingLink(""));
}

TEST_CASE("Pairing: the code typed on the phone") {
    CHECK(pairingCode("123456") == "123456");
    CHECK(pairingCode(" 123 456 ") == "123456");
    CHECK(pairingCode("123-456") == "123456");
    CHECK(pairingCode("12345").empty());
    CHECK(pairingCode("1234567").empty());
    CHECK(pairingCode("12345a").empty());
    CHECK(pairingCode("").empty());
    CHECK(isPairingKey(kKey));
    CHECK(isPairingKey("00112233445566778899AABBCCDDEEFF"));
    CHECK_FALSE(isPairingKey(kKey + "0"));
    CHECK_FALSE(isPairingKey(""));
}

TEST_CASE("Pairing: pairing.json") {
    PairedPc pc;
    pc.endpoint.host = "192.168.1.20";
    pc.endpoint.port = 47801;
    pc.key = kKey;
    pc.pending = true;
    pc.includeWow = true;
    const std::optional<PairedPc> read = readPairingFile(writePairingFile(pc));
    REQUIRE(read);
    CHECK(read->endpoint.text() == "192.168.1.20:47801");
    CHECK(read->key == kKey);
    CHECK(read->pending);
    CHECK(read->includeWow);

    CHECK_FALSE(readPairingFile(writeForgottenPairing()));
    CHECK_FALSE(readPairingFile(""));
    CHECK_FALSE(readPairingFile("{"));
    CHECK_FALSE(readPairingFile("{\"format\": 2, \"host\": \"192.168.1.20\", \"port\": 47800, \"key\": \"" +
                                kKey + "\"}"));
    CHECK_FALSE(readPairingFile("{\"format\": 1, \"host\": \"8.8.8.8\", \"port\": 47800, \"key\": \"" + kKey +
                                "\"}"));
    CHECK_FALSE(readPairingFile("{\"format\": 1, \"host\": \"192.168.1.20\", \"port\": 0, \"key\": \"" +
                                kKey + "\"}"));
    CHECK_FALSE(
        readPairingFile("{\"format\": 1, \"host\": \"192.168.1.20\", \"port\": 47800, \"key\": \"00\"}"));

    const std::filesystem::path storage = tempDir("storage");
    CHECK_FALSE(loadPairing(storage));
    writeFile(storage / std::string(kPairingFile), writePairingFile(pc));
    REQUIRE(loadPairing(storage));
    CHECK(loadPairing(storage)->pending);
}

TEST_CASE("Pairing: when the first-run screen opens") {
    const std::filesystem::path root = tempDir("first_run");
    const std::filesystem::path packs = root / "packs";
    const std::filesystem::path storage = root / "storage";
    std::filesystem::create_directories(packs);
    std::filesystem::create_directories(storage);
    CHECK_FALSE(hasGameData(packs));
    CHECK(needsFirstRun(packs, storage));
    writeFile(packs / "app.vdpk", "ours"); // the APK's: not the game's data
    for (const std::string_view pack : kRequiredPacks) {
        writeFile(packs / std::string(pack), "data");
    }
    CHECK_FALSE(hasGameData(packs)); // no map yet
    writeFile(packs / "world_azeroth.vdpk.part", "half");
    CHECK_FALSE(hasGameData(packs)); // half a map is none
    writeFile(packs / "world_azeroth.vdpk", "terrain");
    CHECK(hasGameData(packs));
    CHECK_FALSE(needsFirstRun(packs, storage));
    writeFile(packs / "models.vdpk", ""); // an empty pack is a missing one
    CHECK(needsFirstRun(packs, storage));
    writeFile(packs / "models.vdpk", "models");

    // A download from a paired PC that did not end: the screen again, to go on with it.
    PairedPc pc;
    pc.endpoint.host = "192.168.1.20";
    pc.endpoint.port = 47800;
    pc.key = kKey;
    pc.pending = true;
    writeFile(storage / std::string(kPairingFile), writePairingFile(pc));
    CHECK(needsFirstRun(packs, storage));
    pc.pending = false;
    writeFile(storage / std::string(kPairingFile), writePairingFile(pc));
    CHECK_FALSE(needsFirstRun(packs, storage));

    const std::filesystem::path wow = root / "wow" / "Data";
    CHECK_FALSE(hasWowOnDevice(wow));
    writeFile(wow / "readme.txt", "x");
    CHECK_FALSE(hasWowOnDevice(wow));
    writeFile(wow / "patch.MPQ", "MPQ");
    CHECK(hasWowOnDevice(wow));
}

TEST_CASE("Pairing: the first-run screen's language") {
    CHECK(firstRunLocale("esES", "en-US") == "esES");
    CHECK(firstRunLocale("eses", "") == "esES");
    CHECK(firstRunLocale("", "es-ES") == "esES");
    CHECK(firstRunLocale("", "es-MX") == "esES"); // our Spanish texts
    CHECK(firstRunLocale("", "es") == "esES");
    CHECK(firstRunLocale("", "fr_FR") == "frFR");
    CHECK(firstRunLocale("", "de") == "deDE");
    CHECK(firstRunLocale("", "en-GB") == "enUS");
    CHECK(firstRunLocale("", "ja-JP") == "enUS");
    CHECK(firstRunLocale("", "") == "enUS");
}

TEST_CASE("Download: files.json") {
    const std::string sha(64, 'a');
    const std::string text =
        "{\"format\": 1, \"setup\": \"0.1.0\", \"appId\": \"com.cer0labs.vanilladroid\","
        " \"apk\": {\"path\": \"vanilladroid.apk\", \"size\": 10, \"sha256\": \"" +
        sha +
        "\"},"
        " \"client\": {\"build\": 5875, \"locale\": \"esES\", \"overlays\": []},"
        " \"packs\": [{\"path\": \"packs/db.vdpk\", \"size\": 5000000000, \"sha256\": \"" +
        std::string(64, 'B') +
        "\", \"recipe\": \"r\"}],"
        " \"manifest\": {\"path\": \"packs/manifest.json\", \"size\": 12, \"sha256\": \"" +
        sha +
        "\"},"
        " \"wow\": [{\"path\": \"wow/Data/patch.MPQ\", \"size\": 7, \"sha256\": \"" +
        sha + "\"}]}";
    Result<RemoteList> list = parseFilesJson(text);
    REQUIRE(list.ok());
    CHECK(list.value().setup == "0.1.0");
    CHECK(list.value().clientLocale == "esES");
    REQUIRE(list.value().packs.size() == 1);
    CHECK(list.value().packs[0].size == 5000000000ull);
    CHECK(list.value().packs[0].sha256 == std::string(64, 'b')); // lowercase
    REQUIRE(list.value().manifest);
    CHECK(list.value().manifest->kind == RemoteFile::Kind::Manifest);
    CHECK(list.value().wowOffered);
    REQUIRE(list.value().wow.size() == 1);
    CHECK(list.value().wow[0].kind == RemoteFile::Kind::Wow);

    // Without the WoW and the manifest (a bake before phase 22): both absent.
    list = parseFilesJson("{\"format\": 1, \"packs\": []}");
    REQUIRE(list.ok());
    CHECK_FALSE(list.value().wowOffered);
    CHECK_FALSE(list.value().manifest);

    CHECK_FALSE(parseFilesJson("{\"format\": 2, \"packs\": []}").ok());
    CHECK_FALSE(parseFilesJson("[]").ok());
    CHECK_FALSE(
        parseFilesJson("{\"format\": 1, \"packs\": [{\"path\": \"packs/db.vdpk\", \"size\": 1}]}").ok());
    CHECK_FALSE(parseFilesJson(
                    "{\"format\": 1, \"packs\": [{\"path\": \"packs/db.vdpk\", \"size\": -1, \"sha256\": \"" +
                    sha + "\"}]}")
                    .ok());
    CHECK_FALSE(parseFilesJson("{\"format\": 1, \"packs\": [{\"path\": \"wow/Data/db.vdpk\", \"size\": 1, "
                               "\"sha256\": \"" +
                               sha + "\"}]}")
                    .ok());
}

TEST_CASE("Download: where the PC's files go") {
    const std::filesystem::path packs = "P";
    const std::filesystem::path wow = "W";
    const auto local = [&](RemoteFile::Kind kind, const std::string& path) {
        RemoteFile file;
        file.kind = kind;
        file.path = path;
        return localPathOf(file, packs, wow);
    };
    using Kind = RemoteFile::Kind;
    CHECK(local(Kind::Pack, "packs/models.vdpk") == packs / "models.vdpk");
    CHECK(local(Kind::Pack, "packs/db.esES.vdpk") == packs / "db.esES.vdpk");
    CHECK(local(Kind::Manifest, "packs/manifest.json") == packs / "manifest.json");
    CHECK(local(Kind::Wow, "wow/Data/patch-2.MPQ") == wow / "patch-2.MPQ");
    CHECK(local(Kind::Wow, "wow/Data/terrain.mpq") == wow / "terrain.mpq");
    // Never outside those folders, never something that is not what its kind says.
    CHECK(local(Kind::Pack, "packs/../models.vdpk").empty());
    CHECK(local(Kind::Pack, "packs/sub/models.vdpk").empty());
    CHECK(local(Kind::Pack, "packs/..\\models.vdpk").empty());
    CHECK(local(Kind::Pack, "packs/C:models.vdpk").empty());
    CHECK(local(Kind::Pack, "packs/").empty());
    CHECK(local(Kind::Pack, "packs/app.vdpk").empty()); // comes with the APK
    CHECK(local(Kind::Pack, "packs/notes.txt").empty());
    CHECK(local(Kind::Pack, "packs/models.vdpk.part").empty());
    CHECK(local(Kind::Pack, "packs/manifest.json").empty());
    CHECK(local(Kind::Pack, "wow/Data/models.vdpk").empty());
    CHECK(local(Kind::Manifest, "packs/other.json").empty());
    CHECK(local(Kind::Wow, "wow/Data/WoW.exe").empty());
    CHECK(local(Kind::Wow, "wow/Data/../../x.MPQ").empty());
    CHECK(local(Kind::Wow, "packs/patch.MPQ").empty());
}

TEST_CASE("Download: what to fetch and how to resume") {
    RemoteFile remote;
    remote.path = "packs/models.vdpk";
    remote.size = 100;
    remote.sha256 = std::string(64, 'a');
    LocalFile local;
    CHECK(fileNeed(remote, local) == FileNeed::Fetch); // missing
    local.exists = true;
    local.size = 99;
    CHECK(fileNeed(remote, local) == FileNeed::Fetch); // another size
    local.size = 100;
    CHECK(fileNeed(remote, local) == FileNeed::Hash); // same size, digest unknown
    local.sha256 = remote.sha256;
    CHECK(fileNeed(remote, local) == FileNeed::Have);
    local.sha256 = std::string(64, 'b');
    CHECK(fileNeed(remote, local) == FileNeed::Fetch); // another version of the same size

    CHECK(resumeOffset(0, 100) == 0);
    CHECK(resumeOffset(40, 100) == 40);
    CHECK(resumeOffset(100, 100) == 100); // all there: only the check
    CHECK(resumeOffset(140, 100) == 0);   // not this file: start over

    CHECK(resumeAction(
              head("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 40-99/100\r\nContent-Length: 60"),
              40, 100) == ResumeAction::Append);
    CHECK(resumeAction(head("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 40-99/100"), 40, 100) ==
          ResumeAction::Append);
    CHECK(resumeAction(head("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 41-99/100"), 40, 100) ==
          ResumeAction::Fail); // not where the part ends
    CHECK(resumeAction(head("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 40-99/120"), 40, 100) ==
          ResumeAction::Fail); // another size: not the file listed
    CHECK(resumeAction(head("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 40-79/100"), 40, 100) ==
          ResumeAction::Fail); // not to the end
    CHECK(resumeAction(head("HTTP/1.1 206 Partial Content\r\nContent-Length: 60"), 40, 100) ==
          ResumeAction::Fail);
    // If-Range did not match (the file changed on the PC) or no range was asked: the whole file.
    CHECK(resumeAction(head("HTTP/1.1 200 OK\r\nContent-Length: 100"), 40, 100) == ResumeAction::Restart);
    CHECK(resumeAction(head("HTTP/1.1 200 OK\r\nContent-Length: 100"), 0, 100) == ResumeAction::Restart);
    CHECK(resumeAction(head("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked"), 0, 100) ==
          ResumeAction::Restart);
    CHECK(resumeAction(head("HTTP/1.1 200 OK\r\nContent-Length: 90"), 0, 100) == ResumeAction::Fail);
    CHECK(resumeAction(head("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */100"), 140, 100) ==
          ResumeAction::StartOver);
    CHECK(resumeAction(head("HTTP/1.1 401 Unauthorized"), 0, 100) == ResumeAction::Fail);
    CHECK(resumeAction(head("HTTP/1.1 503 Service Unavailable"), 0, 100) == ResumeAction::Fail);
}

TEST_CASE("Download: downloads.json") {
    const std::filesystem::path dir = tempDir("records");
    const std::filesystem::path file = dir / "downloads.json";
    const std::string digest(64, 'c');
    {
        DownloadRecords records;
        records.load(file); // none yet
        CHECK(records.digestOf("packs/db.vdpk", 10, 5).empty());
        records.record("packs/db.vdpk", 10, 133000000000000001ll, std::string(64, 'C'));
        CHECK(records.save());
    }
    DownloadRecords records;
    records.load(file);
    // The time keeps all its digits (Windows' file clock passes 2^53).
    CHECK(records.digestOf("packs/db.vdpk", 10, 133000000000000001ll) == digest);
    CHECK(records.digestOf("packs/db.vdpk", 10, 133000000000000002ll).empty()); // changed since
    CHECK(records.digestOf("packs/db.vdpk", 11, 133000000000000001ll).empty());
    CHECK(records.digestOf("packs/other.vdpk", 10, 133000000000000001ll).empty());

    writeFile(file, "not json");
    records.load(file); // damaged: starts over
    CHECK(records.digestOf("packs/db.vdpk", 10, 133000000000000001ll).empty());
}
