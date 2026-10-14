// Phase 22 (setup): the local server's HTTP and the protocol the phone speaks (PhoneServer.h), without
// sockets.
#include "HttpProtocol.h"
#include "HttpServer.h"
#include "PhoneServer.h"
#include "SetupText.h"
#include "core/Json.h"
#include "core/Sha256.h"
#include "core/Text.h"
#include "pack/PackManifest.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <ostream> // doctest prints std::string values through operator<<
#include <string>
#include <vector>

using namespace vd::setup;

namespace {

std::filesystem::path tempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vd_setup_tests" / name;
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

HttpRequest get(const std::string& target, const std::vector<std::string>& headers = {}) {
    std::string head = "GET " + target + " HTTP/1.1\r\nHost: 192.168.1.20:47800\r\n";
    for (const std::string& header : headers) {
        head += header + "\r\n";
    }
    const vd::Result<HttpRequest> request = parseRequestHead(head);
    REQUIRE(request.ok());
    return request.value();
}

std::string headerOf(const HttpResponse& response, std::string_view name) {
    for (const auto& [key, value] : response.headers) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

std::string sha256Of(const std::string& text) {
    return vd::sha256Hex(std::span(reinterpret_cast<const vd::u8*>(text.data()), text.size()));
}

} // namespace

TEST_CASE("Setup HTTP: request heads") {
    const vd::Result<HttpRequest> request = parseRequestHead(
        "GET /packs/models%2Evdpk?k=AB%20c&x=1 HTTP/1.1\r\nHost: pc:47800\r\nRange:  bytes=10- \r\n"
        "X-VD-Key: abc\r\n");
    REQUIRE(request.ok());
    CHECK(request.value().method == "GET");
    CHECK(request.value().path == "/packs/models.vdpk");
    CHECK(request.value().query == "k=AB%20c&x=1");
    CHECK(request.value().header("range") == "bytes=10-");
    CHECK(request.value().header("x-vd-key") == "abc");
    CHECK(request.value().keepAlive);
    CHECK(queryValue(request.value().query, "k") == "AB c");
    CHECK(queryValue(request.value().query, "x") == "1");
    CHECK(queryValue(request.value().query, "y").empty());
    CHECK(queryValue("code=12+34", "code") == "12 34");

    CHECK_FALSE(parseRequestHead("GET /x HTTP/1.1\r\nConnection: close\r\n").value().keepAlive);
    CHECK_FALSE(parseRequestHead("GET /x HTTP/1.0\r\n").value().keepAlive);
    CHECK(parseRequestHead("GET /x HTTP/1.0\r\nConnection: keep-alive\r\n").value().keepAlive);
    CHECK_FALSE(parseRequestHead("GET x HTTP/1.1\r\n").ok());
    CHECK_FALSE(parseRequestHead("GET /x HTTP/2\r\n").ok());
    CHECK_FALSE(parseRequestHead("GARBAGE\r\n").ok());
    CHECK_FALSE(parseRequestHead("GET /a%2 HTTP/1.1\r\n").ok());
    CHECK_FALSE(parseRequestHead("GET /a%00b HTTP/1.1\r\n").ok());
    CHECK_FALSE(parseRequestHead("GET /x HTTP/1.1\r\nno colon\r\n").ok());

    CHECK(percentEncodePath("wow/Data/patch-2.MPQ") == "wow/Data/patch-2.MPQ");
    CHECK(percentEncodePath("a b&c") == "a%20b%26c");
    CHECK(htmlEscape("<a href=\"x\">&'") == "&lt;a href=&quot;x&quot;&gt;&amp;&#39;");
    CHECK(constantTimeEquals("abc", "abc"));
    CHECK_FALSE(constantTimeEquals("abc", "abd"));
    CHECK_FALSE(constantTimeEquals("abc", "ab"));
}

TEST_CASE("Setup HTTP: byte ranges for resuming") {
    ByteRange range;
    CHECK(parseRange("", 100, range) == RangeStatus::None);
    CHECK(parseRange("bytes=10-", 100, range) == RangeStatus::Satisfiable);
    CHECK(range.first == 10);
    CHECK(range.last == 99);
    CHECK(range.length() == 90);
    CHECK(parseRange("bytes=10-19", 100, range) == RangeStatus::Satisfiable);
    CHECK(range.length() == 10);
    CHECK(parseRange("bytes=90-500", 100, range) == RangeStatus::Satisfiable);
    CHECK(range.last == 99);
    CHECK(parseRange("bytes=-30", 100, range) == RangeStatus::Satisfiable);
    CHECK(range.first == 70);
    CHECK(parseRange("bytes=-300", 100, range) == RangeStatus::Satisfiable);
    CHECK(range.first == 0);
    CHECK(parseRange("bytes=100-", 100, range) == RangeStatus::Unsatisfiable);
    CHECK(parseRange("bytes=0-", 0, range) == RangeStatus::Unsatisfiable);
    CHECK(parseRange("bytes=-0", 100, range) == RangeStatus::Unsatisfiable);
    CHECK(parseRange("bytes=0-1,5-6", 100, range) == RangeStatus::None);
    CHECK(parseRange("bytes=20-10", 100, range) == RangeStatus::None);
    CHECK(parseRange("items=0-1", 100, range) == RangeStatus::None);
    CHECK(parseRange("bytes=x-", 100, range) == RangeStatus::None);
    // Beyond 4 GB (the WoW's patch.MPQ is 2 GB; a pack could pass 4 GB one day).
    CHECK(parseRange("bytes=5000000000-", 6000000000ull, range) == RangeStatus::Satisfiable);
    CHECK(range.first == 5000000000ull);

    HttpResponse response;
    response.status = 206;
    response.contentType = "application/octet-stream";
    response.headers.emplace_back("Content-Range", "bytes 10-99/100");
    const std::string head = responseHead(response, 90, true);
    CHECK(head.rfind("HTTP/1.1 206 Partial Content\r\n", 0) == 0);
    CHECK(head.find("Content-Length: 90\r\n") != std::string::npos);
    CHECK(head.find("Connection: keep-alive\r\n") != std::string::npos);
    CHECK(head.find("Content-Range: bytes 10-99/100\r\n") != std::string::npos);
    CHECK(head.size() >= 4);
    CHECK(head.substr(head.size() - 4) == "\r\n\r\n");
}

TEST_CASE("Setup HTTP: local network addresses") {
    CHECK(isLocalNetworkAddress(0x7F000001));       // 127.0.0.1
    CHECK(isLocalNetworkAddress(0xC0A80114));       // 192.168.1.20
    CHECK(isLocalNetworkAddress(0x0A000001));       // 10.0.0.1
    CHECK(isLocalNetworkAddress(0xAC100001));       // 172.16.0.1
    CHECK_FALSE(isLocalNetworkAddress(0xAC200001)); // 172.32.0.1
    CHECK(isLocalNetworkAddress(0x64400001));       // 100.64.0.1 (Tailscale)
    CHECK_FALSE(isLocalNetworkAddress(0x08080808)); // 8.8.8.8
    CHECK(ipv4Text(0xC0A80114) == "192.168.1.20");
}

TEST_CASE("Setup HTTP: the phone's protocol (files.json, keys, ranges, the code)") {
    loadSetupTexts("enUS");
    const std::filesystem::path dir = tempDir("serve");
    const std::filesystem::path packs = dir / "packs";
    std::filesystem::create_directories(packs);
    const std::string models(300000, 'm');
    const std::string db = "database bytes";
    writeFile(packs / "models.vdpk", models);
    writeFile(packs / "db.vdpk", db);
    writeFile(packs / "app.vdpk", "ours"); // in the APK: never served
    writeFile(packs / "notes.txt", "not a pack");
    writeFile(dir / "setup.json", "{\"pairingKey\": \"secret\"}");
    writeFile(dir / "VanillaDroid.apk", "not really an apk");
    vd::pack::PackManifest manifest;
    manifest.bakedBy = "0.1.0";
    manifest.clientBuild = 5875;
    manifest.clientLocale = "enUS";
    manifest.record("models.vdpk").size = models.size();
    manifest.record("models.vdpk").recipe = "r1";
    manifest.record("db.vdpk").size = 999; // not this file's size: no digest written for it
    REQUIRE(manifest.save(packs).ok());

    ServeOptions options;
    options.packsDir = packs;
    options.apk = dir / "VanillaDroid.apk";
    options.key = "0123456789abcdef0123456789abcdef";
    options.code = "042917";
    options.appId = "com.cer0labs.vanilladroid";
    options.host = "192.168.1.20:47800";
    options.htmlLang = "en";
    options.hashCache = dir / "hashes.json";
    std::vector<ServeEvent> events;
    PhoneServer server(options, [&](const ServeEvent& event) { events.push_back(event); });

    // Before the list exists: 503 with progress.
    const std::string key = "?k=" + options.key;
    HttpResponse response = server.handle(get("/files.json" + key), 0x7F000001);
    CHECK(response.status == 503);
    CHECK(headerOf(response, "Retry-After") == "2");

    REQUIRE(server.prepareNow());
    response = server.handle(get("/files.json"), 0x7F000001);
    CHECK(response.status == 401);
    response = server.handle(get("/files.json?k=wrong"), 0x7F000001);
    CHECK(response.status == 401);
    response = server.handle(get("/files.json" + key), 0x7F000001);
    REQUIRE(response.status == 200);
    response = server.handle(get("/files.json", {"X-VD-Key: " + options.key}), 0x7F000001);
    REQUIRE(response.status == 200);
    const vd::Result<vd::json::Value> parsed = vd::json::parse(response.body);
    REQUIRE(parsed.ok());
    const vd::json::Value& files = parsed.value();
    CHECK(files.get("format").asInt() == 1);
    CHECK(files.get("appId").asString() == "com.cer0labs.vanilladroid");
    CHECK(files.get("apk").get("path").asString() == "vanilladroid.apk");
    CHECK(files.get("apk").get("sha256").asString() == sha256Of("not really an apk"));
    CHECK(files.get("client").get("locale").asString() == "enUS");
    REQUIRE(files.get("packs").size() == 2); // db and models: not app.vdpk, not notes.txt
    CHECK(files.get("packs").items()[0].get("path").asString() == "packs/db.vdpk");
    CHECK(files.get("packs").items()[1].get("path").asString() == "packs/models.vdpk");
    CHECK(files.get("packs").items()[1].get("size").asInt() == static_cast<vd::i64>(models.size()));
    CHECK(files.get("packs").items()[1].get("sha256").asString() == sha256Of(models));
    CHECK(files.get("packs").items()[1].get("recipe").asString() == "r1");
    CHECK(files.get("manifest").get("path").asString() == "packs/manifest.json");
    CHECK(files.find("wow") == nullptr);
    // The digest went into the manifest (the phone keeps it), only where the size agrees.
    const vd::Result<vd::pack::PackManifest> written = vd::pack::PackManifest::load(packs);
    REQUIRE(written.ok());
    CHECK(written.value().find("models.vdpk")->sha256 == sha256Of(models));
    CHECK(written.value().find("db.vdpk")->sha256.empty());
    const vd::Result<std::string> manifestDigest = vd::sha256File(packs / "manifest.json");
    CHECK(files.get("manifest").get("sha256").asString() == manifestDigest.value());

    // A whole pack, a resumed one, a stale If-Range, past the end.
    response = server.handle(get("/packs/models.vdpk" + key), 0xC0A80132);
    CHECK(response.status == 200);
    CHECK(response.file == packs / "models.vdpk");
    CHECK(response.offset == 0);
    CHECK(response.length == models.size());
    CHECK(headerOf(response, "ETag") == "\"" + sha256Of(models) + "\"");
    CHECK(headerOf(response, "Accept-Ranges") == "bytes");
    response = server.handle(get("/packs/models.vdpk" + key, {"Range: bytes=1000-"}), 0xC0A80132);
    CHECK(response.status == 206);
    CHECK(response.offset == 1000);
    CHECK(response.length == models.size() - 1000);
    CHECK(headerOf(response, "Content-Range") == "bytes 1000-299999/300000");
    response = server.handle(get("/packs/models.vdpk" + key, {"Range: bytes=1000-", "If-Range: \"old\""}),
                             0xC0A80132);
    CHECK(response.status == 200);
    response = server.handle(
        get("/packs/models.vdpk" + key, {"Range: bytes=1000-", "If-Range: \"" + sha256Of(models) + "\""}),
        0xC0A80132);
    CHECK(response.status == 206);
    response = server.handle(get("/packs/models.vdpk" + key, {"Range: bytes=300000-"}), 0xC0A80132);
    CHECK(response.status == 416);
    CHECK(headerOf(response, "Content-Range") == "bytes */300000");

    // Only what files.json lists.
    for (const char* path :
         {"/packs/app.vdpk", "/packs/notes.txt", "/packs/../setup.json", "/packs/..%2Fsetup.json",
          "/setup.json", "/packs/MODELS.VDPK", "/packs/", "/packs//models.vdpk"}) {
        CHECK_MESSAGE(server.handle(get(std::string(path) + key), 0x7F000001).status == 404, path);
    }
    CHECK(server.handle(get("/packs/../setup.json"), 0x7F000001).status == 401); // the key first

    // The APK and the page need no key; the pairing link only with it.
    response = server.handle(get("/vanilladroid.apk"), 0xC0A80132);
    CHECK(response.status == 200);
    CHECK(response.contentType == "application/vnd.android.package-archive");
    response = server.handle(get("/"), 0xC0A80132);
    CHECK(response.status == 200);
    CHECK(response.body.find("vanilladroid.apk") != std::string::npos);
    CHECK(response.body.find("vanilladroid://pair") == std::string::npos);
    CHECK(response.body.find(options.key) == std::string::npos);
    response = server.handle(get("/" + key), 0xC0A80132);
    CHECK(response.body.find("vanilladroid://pair?h=192.168.1.20:47800&amp;k=" + options.key) !=
          std::string::npos);
    response = server.handle(get("/info.json"), 0xC0A80132);
    CHECK(response.status == 200);
    CHECK(vd::json::parse(response.body).value().get("service").asString() == "vanilladroid-setup");

    // HEAD answers the same without a transfer event.
    events.clear();
    HttpRequest head = get("/packs/db.vdpk" + key);
    head.method = "HEAD";
    CHECK(server.handle(head, 0x7F000001).status == 200);
    CHECK(events.empty());
    HttpRequest post = get("/files.json" + key);
    post.method = "POST";
    CHECK(server.handle(post, 0x7F000001).status == 405);

    // A pack that changes after the list was made: 503 until the list is taken again.
    writeFile(packs / "db.vdpk", "database bytes, longer now");
    CHECK(server.handle(get("/packs/db.vdpk" + key), 0x7F000001).status == 503);
    REQUIRE(server.prepareNow());
    CHECK(server.handle(get("/packs/db.vdpk" + key), 0x7F000001).status == 200);
    // A new pack appears.
    writeFile(packs / "sound.vdpk", "sounds");
    CHECK(server.handle(get("/files.json" + key), 0x7F000001).status == 503);
    REQUIRE(server.prepareNow());
    CHECK(vd::json::parse(server.handle(get("/files.json" + key), 0x7F000001).body)
              .value()
              .get("packs")
              .size() == 3);

    // The code: exchanged for the key; ten wrong ones lock it.
    response = server.handle(get("/pair?code=042917"), 0xC0A80132);
    CHECK(response.status == 200);
    CHECK(vd::json::parse(response.body).value().get("key").asString() == options.key);
    for (vd::u32 i = 0; i < PhoneServer::kMaxWrongCodes - 1; ++i) {
        CHECK(server.handle(get("/pair?code=000000"), 0xC0A80132).status == 403);
    }
    CHECK(server.handle(get("/pair?code=000000"), 0xC0A80132).status == 429);
    CHECK(server.handle(get("/pair?code=042917"), 0xC0A80132).status == 429);
    vd::text::unload();
}
