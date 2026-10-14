// Phase 22 (pairing): the app's download (app/DataDownload) against VanillaDroid Setup's own server
// (tools/setup/PhoneServer) on the loopback: the code for the key, the list (also while the PC prepares it),
// the files verified by their digests, what changed, what the PC no longer has, a .part resumed, a damaged
// .part, a key the PC does not know, and a connection cut in the middle of a file.
#include "HttpProtocol.h"
#include "HttpServer.h"
#include "PhoneServer.h"
#include "app/DataDownload.h"
#include "app/Pairing.h"
#include "core/Json.h"
#include "core/Sha256.h"
#include "net/Socket.h"
#include "pack/PackManifest.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <ostream> // doctest prints std::string values through operator<<
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr vd::u16 kFirstPort = 47910; // away from a VanillaDroid Setup that may be running (47800...)
const std::string kKey = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";
using Phase = vd::DownloadStatus::Phase;
using Failure = vd::DownloadStatus::Failure;

std::filesystem::path tempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vd_pairing_download" / name;
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

// Bytes that differ with the seed, every one of them.
std::string content(vd::usize size, vd::u32 seed) {
    std::string out(size, '\0');
    vd::u32 state = seed * 2654435761u + 1;
    for (char& c : out) {
        state = state * 1664525u + 1013904223u;
        c = static_cast<char>(state >> 24);
    }
    return out;
}

void writeFile(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string sha256Of(const std::string& bytes) {
    return vd::sha256Hex(std::span(reinterpret_cast<const vd::u8*>(bytes.data()), bytes.size()));
}

// Waits for the job to end (or to pair); its last status.
vd::DownloadStatus waitFor(const vd::DataDownload& download, vd::u32 job, bool* sawWaiting = nullptr,
                           bool* sawPreparing = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        const vd::DownloadStatus status = download.status();
        if (status.job == job) {
            if (sawWaiting && status.phase == Phase::Waiting) {
                *sawWaiting = true;
            }
            if (sawPreparing && status.phase == Phase::Preparing) {
                *sawPreparing = true;
            }
            if (status.phase == Phase::Ready || status.phase == Phase::Done ||
                status.phase == Phase::Failed || status.phase == Phase::Paired ||
                status.phase == Phase::Cancelled) {
                return status;
            }
        }
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("the download did not end");
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// tools/setup's HttpServer::stop() waits for its accept() to return, and closing the listening socket does
// not make a blocking accept() return on Windows: a connection made meanwhile does (reported to the lead; the
// fix belongs in HttpServer). Until then the tests stop it this way.
template <typename Server> void stopServer(Server& server, vd::u16 port) {
    std::atomic<bool> stopped{false};
    std::thread stopper([&] {
        server.stop();
        stopped = true;
    });
    while (!stopped) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        vd::net::Socket poke;
        static_cast<void>(poke.connect("127.0.0.1", port, 200));
    }
    stopper.join();
}

struct Events {
    std::mutex mutex;
    std::vector<std::pair<std::string, vd::u64>> downloads; // path, offset

    vd::u64 offsetOf(const std::string& path, vd::usize nth = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [p, offset] : downloads) {
            if (p == path && nth-- == 0) {
                return offset;
            }
        }
        return ~0ull;
    }
    vd::usize count(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex);
        vd::usize n = 0;
        for (const auto& entry : downloads) {
            n += entry.first == path ? 1 : 0;
        }
        return n;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        downloads.clear();
    }
};

} // namespace

TEST_CASE("Pairing download: from VanillaDroid Setup's server, on the loopback") {
    const std::filesystem::path root = tempDir("setup");
    const std::filesystem::path pcPacks = root / "pc" / "packs";
    const std::filesystem::path phonePacks = root / "phone" / "packs";
    const std::filesystem::path phoneWow = root / "phone" / "wow" / "Data";
    const std::filesystem::path storage = root / "phone" / "internal";
    struct Pack {
        const char* name;
        vd::usize size;
    };
    const Pack packs[] = {{"actors.vdpk", 300000},         {"db.vdpk", 200000}, {"models.vdpk", 400000},
                          {"textures.vdpk", 3u << 20},     {"ui.vdpk", 100000}, {"sound.vdpk", 50000},
                          {"world_azeroth.vdpk", 1u << 20}};
    vd::pack::PackManifest manifest;
    manifest.bakedBy = "0.1.0";
    manifest.clientBuild = 5875;
    manifest.clientLocale = "enUS";
    vd::u32 seed = 1;
    for (const Pack& pack : packs) {
        writeFile(pcPacks / pack.name, content(pack.size, seed++));
        vd::pack::PackManifest::Entry& entry = manifest.record(pack.name);
        entry.recipe = "r";
        entry.size = pack.size;
    }
    REQUIRE(manifest.save(pcPacks).ok());

    vd::setup::ServeOptions options;
    options.packsDir = pcPacks;
    options.key = kKey;
    options.code = "123456";
    options.appId = "com.cer0labs.vanilladroid";
    options.htmlLang = "en";
    options.hashCache = root / "pc" / "hashes.json";
    Events events;
    vd::setup::PhoneServer server(options, [&](const vd::setup::ServeEvent& event) {
        if (event.kind == vd::setup::ServeEvent::Kind::Download) {
            std::lock_guard<std::mutex> lock(events.mutex);
            events.downloads.emplace_back(event.path, event.offset);
        }
    });
    const vd::Result<vd::u16> port = server.start(kFirstPort, 40, true);
    REQUIRE(port.ok());
    vd::net::HttpEndpoint pc;
    pc.host = "127.0.0.1";
    pc.port = port.value();

    vd::DataDownload download;
    download.start({phonePacks, phoneWow, storage / "downloads.json"});

    // The code: a wrong one, then the right one for the key.
    vd::DownloadStatus status = waitFor(download, download.pair(pc, "000000"));
    CHECK(status.phase == Phase::Failed);
    CHECK(status.failure == Failure::WrongCode);
    status = waitFor(download, download.pair(pc, "123456"));
    REQUIRE(status.phase == Phase::Paired);
    CHECK(download.pairedKey() == kKey);

    // A key the PC does not know.
    status = waitFor(download, download.check(pc, std::string(32, '0')));
    CHECK(status.phase == Phase::Failed);
    CHECK(status.failure == Failure::Unauthorized);

    // The list (the PC may still be taking its digests: the 503 "preparing" is waited out) and the plan.
    vd::u64 total = 0;
    for (const Pack& pack : packs) {
        total += pack.size;
    }
    status = waitFor(download, download.check(pc, kKey));
    REQUIRE(status.phase == Phase::Ready);
    CHECK(status.packFiles == std::size(packs) + 1); // and the manifest
    CHECK(status.packBytes > total);
    CHECK(status.freeBytes > 0);
    CHECK_FALSE(status.wowOffered);

    // A pack of another bake on the phone, which the PC does not have: it goes once the download ends; ours
    // stays.
    writeFile(phonePacks / "collision_azeroth.vdpk", "old");
    writeFile(phonePacks / "app.vdpk", "ours");
    writeFile(phonePacks / "db.vdpk.part",
              "x"); // a part of other bytes: its digest tells, then the file whole

    status = waitFor(download, download.download(pc, kKey, false));
    REQUIRE(status.phase == Phase::Done);
    CHECK(status.doneBytes == status.totalBytes);
    for (const Pack& pack : packs) {
        CHECK(readFile(phonePacks / pack.name) == readFile(pcPacks / pack.name));
    }
    CHECK(readFile(phonePacks / "manifest.json") == readFile(pcPacks / "manifest.json"));
    CHECK_FALSE(std::filesystem::exists(phonePacks / "collision_azeroth.vdpk"));
    CHECK_FALSE(std::filesystem::exists(phonePacks / "db.vdpk.part"));
    CHECK(readFile(phonePacks / "app.vdpk") == "ours");
    CHECK(std::filesystem::exists(storage / "downloads.json"));
    CHECK(vd::hasGameData(phonePacks));

    // Nothing changed: nothing to fetch.
    status = waitFor(download, download.check(pc, kKey));
    REQUIRE(status.phase == Phase::Ready);
    CHECK(status.packFiles == 0);
    CHECK(status.packBytes == 0);

    // A pack baked again on the PC (same size, other bytes): only it (and the manifest, whose digests change)
    // again.
    writeFile(pcPacks / "world_azeroth.vdpk", content(1u << 20, 99));
    events.clear();
    status = waitFor(download, download.download(pc, kKey, false));
    REQUIRE(status.phase == Phase::Done);
    CHECK(readFile(phonePacks / "world_azeroth.vdpk") == readFile(pcPacks / "world_azeroth.vdpk"));
    CHECK(events.count("packs/world_azeroth.vdpk") == 1);
    CHECK(events.count("packs/textures.vdpk") == 0);

    // A download cut short: the .part's bytes are not asked again.
    const std::string textures = readFile(pcPacks / "textures.vdpk");
    std::filesystem::remove(phonePacks / "textures.vdpk");
    writeFile(phonePacks / "textures.vdpk.part", textures.substr(0, 1u << 20));
    events.clear();
    status = waitFor(download, download.download(pc, kKey, false));
    REQUIRE(status.phase == Phase::Done);
    CHECK(events.offsetOf("packs/textures.vdpk") == (1u << 20));
    CHECK(readFile(phonePacks / "textures.vdpk") == textures);

    // A .part of other bytes: the digest says so at the end, and the file comes again whole.
    std::filesystem::remove(phonePacks / "textures.vdpk");
    writeFile(phonePacks / "textures.vdpk.part", content(1u << 20, 7));
    events.clear();
    status = waitFor(download, download.download(pc, kKey, false));
    REQUIRE(status.phase == Phase::Done);
    CHECK(events.offsetOf("packs/textures.vdpk", 0) == (1u << 20));
    CHECK(events.offsetOf("packs/textures.vdpk", 1) == 0);
    CHECK(readFile(phonePacks / "textures.vdpk") == textures);

    // A file changed since it was downloaded (by hand, whatever the manifest says): hashed, then fetched
    // again, the manifest with it.
    writeFile(phonePacks / "ui.vdpk", content(100000, 1234));
    status = waitFor(download, download.check(pc, kKey));
    REQUIRE(status.phase == Phase::Ready);
    CHECK(status.packFiles == 2);

    // Packs that came without this app (adb push): the manifest's digests are taken as they are, nothing is
    // fetched.
    std::filesystem::remove(storage / "downloads.json");
    writeFile(phonePacks / "ui.vdpk", readFile(pcPacks / "ui.vdpk"));
    status = waitFor(download, download.check(pc, kKey));
    REQUIRE(status.phase == Phase::Ready);
    CHECK(status.packFiles == 0);

    download.stop();
    stopServer(server, pc.port);
}

TEST_CASE("Pairing download: a connection cut in the middle of a file resumes where it stopped") {
    const std::filesystem::path root = tempDir("cut");
    const std::filesystem::path phonePacks = root / "phone" / "packs";
    const std::string bytes = content(2u << 20, 5);
    const std::filesystem::path full = root / "pc" / "db.vdpk";
    const std::filesystem::path half = root / "pc" / "db.half";
    writeFile(full, bytes);
    writeFile(half, bytes.substr(0, 1u << 20));
    const std::string sha = sha256Of(bytes);

    std::atomic<int> fileRequests{0};
    std::atomic<vd::u64> resumedAt{~0ull};
    vd::setup::HttpServer server;
    const vd::Result<vd::u16> port = server.start(
        kFirstPort + 50, 40,
        [&](const vd::setup::HttpRequest& request, vd::u32) {
            vd::setup::HttpResponse response;
            if (request.header("x-vd-key") != kKey) {
                response.status = 401;
                return response;
            }
            if (request.path == "/files.json") {
                response.contentType = "application/json";
                response.body = "{\"format\": 1, \"setup\": \"0.1.0\", \"packs\": [{\"path\": "
                                "\"packs/db.vdpk\", \"size\": " +
                                std::to_string(bytes.size()) + ", \"sha256\": \"" + sha + "\"}]}";
                return response;
            }
            if (request.path != "/packs/db.vdpk") {
                response.status = 404;
                return response;
            }
            if (fileRequests.fetch_add(1) == 0) {
                // The first time the connection drops after half of it: the file announced whole, half of it
                // sent.
                response.file = half;
                response.length = bytes.size();
                return response;
            }
            vd::setup::ByteRange range;
            if (vd::setup::parseRange(request.header("range"), bytes.size(), range) ==
                    vd::setup::RangeStatus::Satisfiable &&
                request.header("if-range") == "\"" + sha + "\"") {
                resumedAt = range.first;
                response.status = 206;
                response.headers.emplace_back("Content-Range", "bytes " + std::to_string(range.first) + "-" +
                                                                   std::to_string(range.last) + "/" +
                                                                   std::to_string(bytes.size()));
                response.file = full;
                response.offset = range.first;
                response.length = range.length();
                return response;
            }
            response.file = full;
            response.length = bytes.size();
            return response;
        },
        true);
    REQUIRE(port.ok());
    vd::net::HttpEndpoint pc;
    pc.host = "127.0.0.1";
    pc.port = port.value();

    vd::DataDownload download;
    download.start({phonePacks, root / "phone" / "wow" / "Data", root / "phone" / "downloads.json"});
    bool waited = false;
    const vd::DownloadStatus status = waitFor(download, download.download(pc, kKey, false), &waited);
    REQUIRE(status.phase == Phase::Done);
    CHECK(waited); // it told the player it would try again
    CHECK(fileRequests.load() == 2);
    CHECK(resumedAt.load() == (1u << 20));
    CHECK(readFile(phonePacks / "db.vdpk") == bytes);
    CHECK_FALSE(std::filesystem::exists(phonePacks / "db.vdpk.part"));

    // Cancelled in the wait: it stops at once, the job's status says so.
    fileRequests = 0;
    std::filesystem::remove(phonePacks / "db.vdpk");
    const vd::u32 job = download.download(pc, kKey, false);
    for (int i = 0; i < 2000 && download.status().phase != Phase::Waiting; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(download.status().phase == Phase::Waiting);
    download.cancel();
    const vd::DownloadStatus cancelled = waitFor(download, job);
    CHECK(cancelled.phase == Phase::Cancelled);
    CHECK(std::filesystem::exists(phonePacks / "db.vdpk.part")); // kept for the next time

    download.stop();
    stopServer(server, pc.port);
}
