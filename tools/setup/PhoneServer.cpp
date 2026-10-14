#include "PhoneServer.h"

#include "BakeRunner.h"
#include "NetInterfaces.h"
#include "core/Clock.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Version.h"
#include "pack/PackManifest.h"

#include <algorithm>
#include <set>

namespace vd::setup {
namespace {

constexpr i64 kFormat = 1;
constexpr const char* kApkPath = "vanilladroid.apk";

HttpResponse jsonResponse(int status, const json::Value& value) {
    HttpResponse response;
    response.status = status;
    response.contentType = "application/json; charset=utf-8";
    response.body = json::write(value, false);
    return response;
}

HttpResponse errorResponse(int status, std::string_view error) {
    json::Value body = json::Value::object();
    body.set("format", json::Value::number(kFormat));
    body.set("error", json::Value::string(std::string(error)));
    return jsonResponse(status, body);
}

// "192.168.1.20:47800" or "192.168.1.20" (the Host header), as host:port; empty for anything else (a name).
std::string hostOf(std::string_view header, std::string_view fallbackPort) {
    const usize colon = header.find(':');
    u32 address = 0;
    if (!parseIpv4(header.substr(0, colon), address)) {
        return {};
    }
    std::string port =
        colon == std::string_view::npos ? std::string(fallbackPort) : std::string(header.substr(colon + 1));
    if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) {
        return {};
    }
    return ipv4Text(address) + ":" + port;
}

} // namespace

PhoneServer::PhoneServer(ServeOptions options, std::function<void(const ServeEvent&)> onEvent)
    : m_options(std::move(options)), m_onEvent(std::move(onEvent)), m_hashes(m_options.hashCache) {
    m_hashes.load();
}

PhoneServer::~PhoneServer() {
    stop();
}

void PhoneServer::emit(ServeEvent event) const {
    if (m_onEvent) {
        m_onEvent(event);
    }
}

std::shared_ptr<const PhoneServer::Catalog> PhoneServer::catalog() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_catalog;
}

bool PhoneServer::ready() const {
    return catalog() != nullptr && !m_preparing.load();
}

void PhoneServer::setHost(std::string host) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_options.host = std::move(host);
}

std::string PhoneServer::filesJson() const {
    const std::shared_ptr<const Catalog> current = catalog();
    return current ? current->json : std::string();
}

Result<u16> PhoneServer::start(u16 firstPort, u32 attempts, bool loopbackOnly) {
    stop();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = false;
        m_prepareWanted = true;
    }
    m_cancel.reset();
    m_preparer = std::thread([this] { prepareLoop(); });
    Result<u16> port = m_server.start(
        firstPort, attempts,
        [this](const HttpRequest& request, u32 client) { return handle(request, client); }, loopbackOnly);
    if (!port) {
        stop();
    }
    return port;
}

void PhoneServer::stop() {
    m_server.stop();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_cancel.cancel();
    m_wake.notify_all();
    if (m_preparer.joinable()) {
        m_preparer.join();
    }
}

void PhoneServer::requestPrepare() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_prepareWanted || m_preparing.load()) {
            return;
        }
        m_prepareWanted = true;
    }
    m_wake.notify_all();
}

void PhoneServer::prepareLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_quit || m_prepareWanted; });
            if (m_quit) {
                return;
            }
            m_prepareWanted = false;
            m_preparing = true;
        }
        m_prepareFraction = 0.0;
        Result<std::shared_ptr<const Catalog>> built = buildCatalog(m_cancel);
        if (built) {
            const u64 total = built.value()->totalBytes;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_catalog = built.value();
            }
            m_preparing = false;
            ServeEvent event;
            event.kind = ServeEvent::Kind::Ready;
            event.bytes = total;
            emit(event);
        } else {
            m_preparing = false;
            if (m_cancel.cancelled()) {
                return;
            }
            ServeEvent event;
            event.kind = ServeEvent::Kind::PrepareFailed;
            event.text = built.error().message;
            emit(event);
        }
    }
}

bool PhoneServer::prepareNow() {
    Result<std::shared_ptr<const Catalog>> built = buildCatalog(m_cancel);
    if (!built) {
        VD_LOG_ERROR("serve: %s", built.error().message.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_catalog = built.value();
    return true;
}

Result<std::shared_ptr<const PhoneServer::Catalog>> PhoneServer::buildCatalog(const CancelToken& cancel) {
    auto catalog = std::make_shared<Catalog>();
    std::vector<Entry>& entries = catalog->entries;
    std::error_code error;
    if (!m_options.apk.empty() && std::filesystem::is_regular_file(m_options.apk, error)) {
        Entry apk;
        apk.path = kApkPath;
        apk.local = m_options.apk;
        apk.kind = Entry::Kind::Apk;
        entries.push_back(std::move(apk));
        catalog->hasApk = true;
    }
    for (const PackFile& file : listPackFiles(m_options.packsDir)) {
        Entry pack;
        pack.path = "packs/" + file.name;
        pack.local = m_options.packsDir / pathFromUtf8(file.name);
        pack.kind = Entry::Kind::Pack;
        entries.push_back(std::move(pack));
    }
    if (!m_options.wowData.empty()) {
        for (const std::string& name : m_options.wowArchives) {
            Entry archive;
            archive.path = "wow/Data/" + name;
            archive.local = m_options.wowData / pathFromUtf8(name);
            archive.kind = Entry::Kind::Wow;
            entries.push_back(std::move(archive));
        }
    }

    std::vector<HashJob> jobs(entries.size());
    for (usize i = 0; i < entries.size(); ++i) {
        jobs[i].file = entries[i].local;
    }
    Operation operation;
    operation.cancel = &cancel;
    operation.progress = [this](const ProgressEvent& progress) {
        if (progress.fraction >= 0) {
            m_prepareFraction = progress.fraction;
            ServeEvent event;
            event.kind = ServeEvent::Kind::Preparing;
            event.fraction = progress.fraction;
            event.text = std::string(progress.text);
            emit(event);
        }
    };
    VD_TRY(hashFiles(jobs, m_hashes, operation));
    for (usize i = 0; i < entries.size(); ++i) {
        entries[i].size = jobs[i].size;
        entries[i].time = jobs[i].time;
        entries[i].sha256 = jobs[i].sha256;
        catalog->totalBytes += jobs[i].size;
    }

    // packs/manifest.json: the recipes, the client, and each pack's digest written in for the phone.
    Result<pack::PackManifest> loaded = pack::PackManifest::load(m_options.packsDir);
    if (loaded && !loaded.value().packs.empty()) {
        pack::PackManifest& manifest = loaded.value();
        bool changed = false;
        for (Entry& entry : entries) {
            if (entry.kind != Entry::Kind::Pack) {
                continue;
            }
            const std::string file = entry.path.substr(6);
            if (const pack::PackManifest::Entry* known = manifest.find(file)) {
                entry.recipe = known->recipe;
                if (known->sha256 != entry.sha256 && known->size == entry.size) {
                    manifest.record(file).sha256 = entry.sha256;
                    changed = true;
                }
            }
        }
        if (changed) {
            if (Result<void> saved = manifest.save(m_options.packsDir); !saved) {
                VD_LOG_WARN("serve: %s", saved.error().message.c_str());
            }
        }
        catalog->clientBuild = manifest.clientBuild;
        catalog->clientLocale = manifest.clientLocale;
        catalog->overlays = manifest.overlays;
        Entry manifestEntry;
        manifestEntry.path = "packs/" + std::string(pack::kManifestFile);
        manifestEntry.local = m_options.packsDir / std::string(pack::kManifestFile);
        manifestEntry.kind = Entry::Kind::Manifest;
        manifestEntry.size = std::filesystem::file_size(manifestEntry.local, error);
        manifestEntry.time = fileTimeOf(manifestEntry.local);
        Result<std::string> digest = sha256FileProgress(manifestEntry.local, {}, &cancel);
        if (!digest) {
            return digest.error();
        }
        manifestEntry.sha256 = std::move(digest.value());
        entries.push_back(std::move(manifestEntry));
    } else if (!loaded) {
        VD_LOG_WARN("serve: %s", loaded.error().message.c_str());
    }
    if (catalog->hasApk) {
        if (Result<ApkInfo> info = readApkInfo(m_options.apk); info) {
            catalog->apk = info.value();
        } else {
            VD_LOG_WARN("serve: %s: %s", utf8FromPath(m_options.apk).c_str(), info.error().message.c_str());
        }
    }

    // files.json
    json::Value root = json::Value::object();
    root.set("format", json::Value::number(kFormat));
    root.set("setup", json::Value::string(kVersion));
    root.set("appId", json::Value::string(m_options.appId));
    json::Value packs = json::Value::array();
    json::Value wow = json::Value::array();
    json::Value manifestItem;
    for (const Entry& entry : entries) {
        json::Value item = json::Value::object();
        item.set("path", json::Value::string(entry.path));
        if (entry.kind == Entry::Kind::Apk) {
            item.set("package", json::Value::string(catalog->apk.package));
            item.set("versionCode", json::Value::number(catalog->apk.versionCode));
            item.set("versionName", json::Value::string(catalog->apk.versionName));
        }
        item.set("size", json::Value::number(static_cast<f64>(entry.size)));
        item.set("sha256", json::Value::string(entry.sha256));
        switch (entry.kind) {
        case Entry::Kind::Apk: root.set("apk", std::move(item)); break;
        case Entry::Kind::Pack:
            item.set("recipe", json::Value::string(entry.recipe));
            packs.push(std::move(item));
            break;
        case Entry::Kind::Manifest: manifestItem = std::move(item); break;
        case Entry::Kind::Wow: wow.push(std::move(item)); break;
        }
    }
    if (catalog->clientBuild != 0) {
        json::Value& client = root.set("client", json::Value::object());
        client.set("build", json::Value::number(catalog->clientBuild));
        client.set("locale", json::Value::string(catalog->clientLocale));
        json::Value& overlays = client.set("overlays", json::Value::array());
        for (const std::string& overlay : catalog->overlays) {
            overlays.push(json::Value::string(overlay));
        }
    }
    root.set("packs", std::move(packs));
    if (manifestItem.isObject()) {
        root.set("manifest", std::move(manifestItem));
    }
    if (!m_options.wowData.empty()) {
        root.set("wow", std::move(wow));
    }
    catalog->json = json::write(root);
    return std::shared_ptr<const Catalog>(std::move(catalog));
}

bool PhoneServer::stale(const Catalog& catalog) const {
    std::error_code error;
    std::set<std::string> listed;
    for (const Entry& entry : catalog.entries) {
        if (entry.kind == Entry::Kind::Pack) {
            listed.insert(entry.path.substr(6));
        }
        const u64 size = std::filesystem::file_size(entry.local, error);
        if (error || size != entry.size || fileTimeOf(entry.local) != entry.time) {
            return true;
        }
    }
    const std::vector<PackFile> files = listPackFiles(m_options.packsDir);
    if (files.size() != listed.size()) {
        return true;
    }
    for (const PackFile& file : files) {
        if (!listed.count(file.name)) {
            return true;
        }
    }
    // A manifest that appeared after the list was made.
    const bool listedManifest = std::any_of(catalog.entries.begin(), catalog.entries.end(),
                                            [](const Entry& e) { return e.kind == Entry::Kind::Manifest; });
    return !listedManifest &&
           std::filesystem::exists(m_options.packsDir / std::string(pack::kManifestFile), error);
}

bool PhoneServer::keyMatches(const HttpRequest& request) const {
    if (m_options.key.empty()) {
        return false;
    }
    std::string key = queryValue(request.query, "k");
    if (key.empty()) {
        key = std::string(request.header("x-vd-key"));
    }
    return constantTimeEquals(toLowerAscii(key), m_options.key);
}

HttpResponse PhoneServer::handle(const HttpRequest& request, u32 client) {
    if (request.method != "GET" && request.method != "HEAD") {
        HttpResponse response = errorResponse(405, "method");
        response.headers.emplace_back("Allow", "GET, HEAD");
        return response;
    }
    if (request.path == "/" || request.path == "/index.html") {
        return page(request);
    }
    if (request.path == "/favicon.ico" || request.path == "/robots.txt") {
        return errorResponse(404, "not found"); // browsers ask: not worth a line in the console
    }
    if (request.path == "/info.json") {
        json::Value info = json::Value::object();
        info.set("format", json::Value::number(kFormat));
        info.set("service", json::Value::string("vanilladroid-setup"));
        info.set("setup", json::Value::string(kVersion));
        info.set("appId", json::Value::string(m_options.appId));
        return jsonResponse(200, info);
    }
    if (request.path == "/pair") {
        if (m_wrongCodes.load() >= kMaxWrongCodes) {
            return errorResponse(429, "locked");
        }
        const std::string code = queryValue(request.query, "code");
        if (m_options.code.empty() || !constantTimeEquals(code, m_options.code)) {
            const u32 wrong = m_wrongCodes.fetch_add(1) + 1;
            ServeEvent event;
            event.kind = wrong >= kMaxWrongCodes ? ServeEvent::Kind::CodeLocked : ServeEvent::Kind::WrongCode;
            event.client = client;
            emit(event);
            return errorResponse(wrong >= kMaxWrongCodes ? 429 : 403, "code");
        }
        ServeEvent event;
        event.kind = ServeEvent::Kind::Paired;
        event.client = client;
        emit(event);
        json::Value body = json::Value::object();
        body.set("format", json::Value::number(kFormat));
        body.set("key", json::Value::string(m_options.key));
        return jsonResponse(200, body);
    }
    const std::shared_ptr<const Catalog> current = catalog();
    const std::string path = request.path.substr(1);
    if (path == kApkPath) {
        // No key: the page's download button works before pairing. While the list is being made, the file as
        // is.
        if (current) {
            for (const Entry& entry : current->entries) {
                if (entry.kind == Entry::Kind::Apk) {
                    return fileResponse(request, entry, client);
                }
            }
        }
        std::error_code error;
        if (!m_options.apk.empty() && std::filesystem::is_regular_file(m_options.apk, error)) {
            Entry apk;
            apk.path = kApkPath;
            apk.local = m_options.apk;
            apk.kind = Entry::Kind::Apk;
            apk.size = std::filesystem::file_size(apk.local, error);
            apk.time = fileTimeOf(apk.local);
            return fileResponse(request, apk, client);
        }
        return errorResponse(404, "not found");
    }
    if (!keyMatches(request)) {
        ServeEvent event;
        event.kind = ServeEvent::Kind::Unauthorized;
        event.client = client;
        event.path = request.path;
        emit(event);
        HttpResponse response = errorResponse(401, "key");
        return response;
    }
    if (!current || m_preparing.load() || stale(*current)) {
        requestPrepare();
        json::Value body = json::Value::object();
        body.set("format", json::Value::number(kFormat));
        body.set("preparing", json::Value::boolean(true));
        body.set("progress", json::Value::number(m_preparing.load() ? m_prepareFraction.load() : 0.0));
        HttpResponse response = jsonResponse(503, body);
        response.headers.emplace_back("Retry-After", "2");
        return response;
    }
    if (path == "files.json") {
        HttpResponse response;
        response.contentType = "application/json; charset=utf-8";
        response.body = current->json;
        return response;
    }
    for (const Entry& entry : current->entries) {
        if (entry.path == path) {
            return fileResponse(request, entry, client);
        }
    }
    return errorResponse(404, "not found");
}

HttpResponse PhoneServer::fileResponse(const HttpRequest& request, const Entry& entry, u32 client) {
    HttpResponse response;
    response.contentType = entry.kind == Entry::Kind::Apk ? "application/vnd.android.package-archive"
                                                          : "application/octet-stream";
    response.headers.emplace_back("Accept-Ranges", "bytes");
    const std::string etag = entry.sha256.empty() ? std::string() : "\"" + entry.sha256 + "\"";
    if (!etag.empty()) {
        response.headers.emplace_back("ETag", etag);
    }
    if (entry.kind == Entry::Kind::Apk) {
        response.headers.emplace_back("Content-Disposition", "attachment; filename=\"VanillaDroid.apk\"");
    }
    ByteRange range;
    RangeStatus status = parseRange(request.header("range"), entry.size, range);
    const std::string_view ifRange = request.header("if-range");
    if (status != RangeStatus::None && !ifRange.empty() && ifRange != etag) {
        status = RangeStatus::None; // the file the phone started is not this one: all of it
    }
    if (status == RangeStatus::Unsatisfiable) {
        response.status = 416;
        response.headers.emplace_back("Content-Range", "bytes */" + std::to_string(entry.size));
        return response;
    }
    response.file = entry.local;
    if (status == RangeStatus::Satisfiable) {
        response.status = 206;
        response.offset = range.first;
        response.length = range.length();
        response.headers.emplace_back("Content-Range", "bytes " + std::to_string(range.first) + "-" +
                                                           std::to_string(range.last) + "/" +
                                                           std::to_string(entry.size));
    } else {
        response.offset = 0;
        response.length = entry.size;
    }
    if (request.method == "GET") {
        ServeEvent event;
        event.kind = ServeEvent::Kind::Download;
        event.client = client;
        event.path = entry.path;
        event.offset = response.offset;
        event.total = entry.size;
        emit(event);
        const bool toTheEnd = response.offset + response.length == entry.size;
        response.onSent = [this, client, toTheEnd, path = entry.path, total = entry.size](u64 sent,
                                                                                          bool complete) {
            if (complete && !toTheEnd) {
                return; // a piece of the file, all sent: not the end of a download
            }
            ServeEvent done;
            done.kind = ServeEvent::Kind::Sent;
            done.client = client;
            done.path = path;
            done.bytes = sent;
            done.total = total;
            done.complete = complete;
            emit(done);
        };
        // Phase 22 (gui): how far it got, a few times a second (the first block at once).
        response.onProgress = [this, client, offset = response.offset, path = entry.path, total = entry.size,
                               last = Clock::time_point()](u64 sent) mutable {
            const Clock::time_point now = Clock::now();
            if (last != Clock::time_point() && secondsBetween(last, now) < 0.25) {
                return;
            }
            last = now;
            ServeEvent progress;
            progress.kind = ServeEvent::Kind::Progress;
            progress.client = client;
            progress.path = path;
            progress.offset = offset;
            progress.bytes = sent;
            progress.total = total;
            emit(progress);
        };
    }
    return response;
}

HttpResponse PhoneServer::page(const HttpRequest& request) const {
    std::string configuredHost;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        configuredHost = m_options.host;
    }
    const usize colon = configuredHost.rfind(':');
    const std::string port = colon == std::string::npos ? std::string() : configuredHost.substr(colon + 1);
    std::string host = hostOf(request.header("host"), port);
    if (host.empty()) {
        host = configuredHost;
    }
    const bool paired = keyMatches(request);
    std::error_code error;
    const bool hasApk = !m_options.apk.empty() && std::filesystem::is_regular_file(m_options.apk, error);
    std::string html;
    html += "<!doctype html>\n<html lang=\"" + htmlEscape(m_options.htmlLang) +
            "\"><head><meta charset=\"utf-8\">";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
    html += "<title>" + htmlEscape(formatText("VD_SETUP_PAGE_TITLE")) + "</title>";
    html +=
        "<style>body{font-family:sans-serif;background:#16120d;color:#f2e6c9;margin:0;padding:24px 16px;"
        "line-height:1.45}main{max-width:560px;margin:0 auto}h1{color:#ffd100;font-size:1.6em;margin:0 0 "
        ".3em}"
        "h2{font-size:1.15em;margin:1.4em 0 .4em}a.button{display:block;text-align:center;background:#8c1d0e;"
        "color:#fff;text-decoration:none;font-weight:bold;padding:14px;border-radius:8px;border:1px solid "
        "#d4a017;margin:.6em "
        "0}p.note{color:#c9b98f;font-size:.92em}code{color:#ffd100}</style></head><body><main>";
    html += "<h1>" + htmlEscape(formatText("VD_SETUP_PAGE_TITLE")) + "</h1>";
    html += "<p>" + htmlEscape(formatText("VD_SETUP_PAGE_INTRO")) + "</p>";
    html += "<h2>" + htmlEscape(formatText("VD_SETUP_PAGE_STEP_INSTALL")) + "</h2>";
    if (hasApk) {
        html += "<a class=\"button\" href=\"/" + std::string(kApkPath) + "\">" +
                htmlEscape(formatText("VD_SETUP_PAGE_DOWNLOAD")) + "</a>";
        html += "<p class=\"note\">" + htmlEscape(formatText("VD_SETUP_PAGE_DOWNLOAD_NOTE")) + "</p>";
    } else {
        html += "<p class=\"note\">" + htmlEscape(formatText("VD_SETUP_PAGE_NO_APK")) + "</p>";
    }
    html += "<h2>" + htmlEscape(formatText("VD_SETUP_PAGE_STEP_PAIR")) + "</h2>";
    if (paired) {
        const std::string link = "vanilladroid://pair?h=" + host + "&k=" + m_options.key;
        html += "<a class=\"button\" href=\"" + htmlEscape(link) + "\">" +
                htmlEscape(formatText("VD_SETUP_PAGE_PAIR")) + "</a>";
        html += "<p class=\"note\">" + htmlEscape(formatText("VD_SETUP_PAGE_PAIR_NOTE")) + "</p>";
    } else {
        html += "<p>" + htmlEscape(formatText("VD_SETUP_PAGE_NO_KEY", {host})) + "</p>";
    }
    html += "<p class=\"note\">" + htmlEscape(formatText("VD_SETUP_PAGE_FOOTER")) + "</p>";
    html += "</main></body></html>\n";
    HttpResponse response;
    response.contentType = "text/html; charset=utf-8";
    response.body = std::move(html);
    return response;
}

} // namespace vd::setup
