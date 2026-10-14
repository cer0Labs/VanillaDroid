#pragma once

#include "ApkInfo.h"
#include "HashCache.h"
#include "HttpServer.h"
#include "SetupCommon.h"
#include "core/Result.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Phase 22 (ADR-0005 §5): what the phone downloads from the PC, over HTTP on the local network. The protocol
// the app (22.4) speaks, version 1:
//
//   GET /                  no key. A page in the Setup's language: "Download VanillaDroid"
//   (/vanilladroid.apk) and,
//                          when ?k=<key> is right, "Link" (vanilladroid://pair?h=<ip>:<port>&k=<key>).
//   GET /info.json         no key. {"format":1,"service":"vanilladroid-setup","setup":"0.1.0","appId":"..."}:
//   the app
//                          checks an address typed by hand before asking for the code.
//   GET /vanilladroid.apk  no key (our own program, published anyway). 404 when the Setup has no APK.
//   GET /pair?code=NNNNNN  no key. The 6-digit code shown in the console, exchanged for the key:
//                          {"format":1,"key":"<32 hex>"}. 403 for a wrong code; after 10 wrong ones the code
//                          stops working until `serve` starts again (429). The code is random per session: 6
//                          digits cannot carry a 128-bit key, so they only unlock it, once, for whoever is in
//                          front of the PC.
//   GET /files.json        key. What the phone may fetch (below); 503
//   {"format":1,"preparing":true,"progress":0.4}
//                          with Retry-After while the digests are taken (the first time: a minute or more for
//                          10 GB).
//   GET /<path>            key. Each file files.json lists, nothing else (no path is ever mapped to the
//   disk).
//                          "Range: bytes=N-" (and N-M, -N) -> 206 with Content-Range, for resuming; If-Range
//                          with the ETag (the SHA-256 in quotes) falls back to the whole file when it
//                          changed; 416 past the end. 503 when the file changed since it was listed (the list
//                          is taken again).
//
// The key: ?k=<key> or the header X-VD-Key; 401 without it. HEAD works wherever GET does. files.json:
//
//   {"format": 1, "setup": "0.1.0", "appId": "com.cer0labs.vanilladroid",
//    "apk": {"path": "vanilladroid.apk", "package": "...", "versionCode": 1000, "versionName": "0.1.0",
//            "size": N, "sha256": "..."},                                      (absent without an APK)
//    "client": {"build": 5875, "locale": "enUS", "overlays": ["esES"]},       (from packs/manifest.json)
//    "packs": [{"path": "packs/models.vdpk", "size": N, "sha256": "...", "recipe": "..."}, ...],
//    "manifest": {"path": "packs/manifest.json", "size": N, "sha256": "..."}, (absent before any bake of
//    phase 22) "wow": [{"path": "wow/Data/patch.MPQ", "size": N, "sha256": "..."}, ...]} (only with serve
//    --wow)
//
// The phone downloads each file it lacks (or whose digest differs) to <name>.part, resumes with Range, checks
// the SHA-256 and renames it; manifest.json last. packs/manifest.json carries each pack's sha256 too (written
// here), so the phone knows what it has without reading 4.5 GB again.
namespace vd::setup {

struct ServeOptions {
    std::filesystem::path packsDir;
    std::filesystem::path apk;     // empty: no APK
    std::filesystem::path wowData; // the official archives of this Data folder are offered when not empty
    std::vector<std::string> wowArchives;
    std::string key;
    std::string code;
    std::string appId;
    std::string host;     // "192.168.1.20:47800" for the links when the request names no usable host
    std::string htmlLang; // "en", "es"
    std::filesystem::path hashCache; // <data folder>/hashes.json
};

struct ServeEvent {
    enum class Kind : u8 {
        Preparing,     // fraction: digests being taken
        Ready,         // files.json can be served: bytes = what it lists
        PrepareFailed, // text: why
        Download,      // a file transfer began: path, offset (resumed from), total
        Sent,          // it ended: bytes sent, complete
        Paired,        // the code was exchanged for the key
        WrongCode,
        CodeLocked,   // too many wrong codes
        Unauthorized, // a request without the key: path
        // Phase 22 (gui): a file transfer goes on, about four times a second: path, offset (resumed from), bytes
        // (sent since), total. The window's progress bars; the console ignores it.
        Progress,
    };
    Kind kind = Kind::Ready;
    u32 client = 0;
    std::string path;
    u64 offset = 0;
    u64 bytes = 0;
    u64 total = 0;
    bool complete = false;
    f64 fraction = 0.0;
    std::string text;
};

class PhoneServer {
public:
    static constexpr u32 kMaxWrongCodes = 10;

    PhoneServer(ServeOptions options, std::function<void(const ServeEvent&)> onEvent);
    ~PhoneServer();
    PhoneServer(const PhoneServer&) = delete;
    PhoneServer& operator=(const PhoneServer&) = delete;

    // Starts taking the digests (in the background) and listening from `firstPort`. Returns the port.
    Result<u16> start(u16 firstPort, u32 attempts = 20, bool loopbackOnly = false);
    void stop();

    // The routes, without sockets (tests).
    HttpResponse handle(const HttpRequest& request, u32 client);
    // Takes the digests now, on this thread (tests); false when it failed.
    bool prepareNow();
    bool ready() const;
    std::string filesJson() const;
    void setHost(std::string host);

private:
    struct Entry {
        std::string path; // "packs/models.vdpk"
        std::filesystem::path local;
        u64 size = 0;
        i64 time = 0;
        std::string sha256;
        std::string recipe;
        enum class Kind : u8 { Apk, Pack, Manifest, Wow } kind = Kind::Pack;
    };
    struct Catalog {
        std::vector<Entry> entries;
        bool hasApk = false;
        ApkInfo apk;
        u32 clientBuild = 0;
        std::string clientLocale;
        std::vector<std::string> overlays;
        std::string json;
        u64 totalBytes = 0;
    };

    void prepareLoop();
    Result<std::shared_ptr<const Catalog>> buildCatalog(const CancelToken& cancel);
    bool stale(const Catalog& catalog) const;
    void requestPrepare();
    bool keyMatches(const HttpRequest& request) const;
    HttpResponse page(const HttpRequest& request) const;
    HttpResponse fileResponse(const HttpRequest& request, const Entry& entry, u32 client);
    void emit(ServeEvent event) const;
    std::shared_ptr<const Catalog> catalog() const;

    ServeOptions m_options;
    std::function<void(const ServeEvent&)> m_onEvent;
    HttpServer m_server;
    HashCache m_hashes;

    mutable std::mutex m_mutex;
    std::shared_ptr<const Catalog> m_catalog; // null until the first preparation ends
    bool m_prepareWanted = false;
    bool m_quit = false;
    std::condition_variable m_wake;
    std::thread m_preparer;
    CancelToken m_cancel;
    std::atomic<f64> m_prepareFraction{0.0};
    std::atomic<bool> m_preparing{false};
    std::atomic<u32> m_wrongCodes{0};
};

} // namespace vd::setup
