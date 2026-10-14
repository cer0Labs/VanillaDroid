#include "app/DataDownload.h"

#include "core/Json.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Sha256.h"
#include "pack/PackManifest.h"
#include "platform/Thread.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <system_error>

namespace vd {
namespace {

using Phase = DownloadStatus::Phase;
using Failure = DownloadStatus::Failure;

constexpr i64 kFormat = 1;
constexpr usize kBlockBytes = 1u << 20;
constexpr usize kMaxListBytes = 16u << 20;
constexpr usize kMaxSmallBytes = 64u << 10;
constexpr u32 kMaxRetrySeconds = 30;
constexpr u32 kDamagedTries = 2;
constexpr std::string_view kPackPrefix = "packs/";
constexpr std::string_view kWowPrefix = "wow/Data/";
constexpr std::string_view kPartSuffix = ".part";

char lowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = lowerAscii(c);
    }
    return out;
}

bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    for (usize i = 0; i < suffix.size(); ++i) {
        if (lowerAscii(text[text.size() - suffix.size() + i]) != lowerAscii(suffix[i])) {
            return false;
        }
    }
    return true;
}

bool isDigest(std::string_view text) {
    if (text.size() != 64) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// A plain file name: no separators, no "..", no drive, nothing invisible, nothing Windows refuses.
bool plainFileName(std::string_view name) {
    if (name.empty() || name.size() > 128 || name.front() == '.' || name.back() == '.' ||
        name.back() == ' ') {
        return false;
    }
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    return true;
}

std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string utf8Of(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::FILE* openFile(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
    wchar_t wide[4] = {};
    for (usize i = 0; i < 3 && mode[i] != '\0'; ++i) {
        wide[i] = static_cast<wchar_t>(mode[i]);
    }
    std::FILE* file = nullptr;
    _wfopen_s(&file, path.c_str(), wide);
    return file;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

std::filesystem::path partOf(const std::filesystem::path& local) {
    std::filesystem::path part = local;
    part += std::string(kPartSuffix);
    return part;
}

// The size of a regular file; `exists` says whether there is one.
u64 sizeOf(const std::filesystem::path& path, bool& exists) {
    std::error_code error;
    exists = std::filesystem::is_regular_file(path, error);
    if (!exists) {
        return 0;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<u64>(size);
}

template <usize N> void copyText(std::array<char, N>& out, std::string_view text) {
    const usize length = std::min(text.size(), N - 1);
    std::memcpy(out.data(), text.data(), length);
    out[length] = '\0';
}

std::string_view fileNameOf(std::string_view path) {
    const usize slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string percentEncodePath(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' ||
            u == '_' || u == '.' || u == '~' || u == '/') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 15]);
        }
    }
    return out;
}

bool readFile(const RemoteFile::Kind kind, const json::Value& value, RemoteFile& file) {
    file.kind = kind;
    file.path = std::string(value.get("path").asString());
    const f64 size = value.get("size").asNumber(-1.0);
    if (size < 0.0 || size > 9007199254740992.0 || size != static_cast<f64>(static_cast<u64>(size))) {
        return false;
    }
    file.size = static_cast<u64>(size);
    file.sha256 = lowered(value.get("sha256").asString());
    return !file.path.empty() && isDigest(file.sha256);
}

} // namespace

// --- files.json and the rules
// ----------------------------------------------------------------------------------

Result<RemoteList> parseFilesJson(std::string_view text) {
    Result<json::Value> document = json::parse(text);
    if (!document) {
        return Error{"files.json: " + document.error().message};
    }
    const json::Value& root = document.value();
    if (root.get("format").asInt() != kFormat) {
        return Error{"files.json: unknown format"};
    }
    RemoteList list;
    list.setup = std::string(root.get("setup").asString());
    list.clientLocale = std::string(root.get("client").get("locale").asString());
    for (const json::Value& item : root.get("packs").items()) {
        RemoteFile file;
        if (!readFile(RemoteFile::Kind::Pack, item, file) ||
            file.path.compare(0, kPackPrefix.size(), kPackPrefix) != 0) {
            return Error{"files.json: a pack without path, size or digest"};
        }
        list.packs.push_back(std::move(file));
    }
    if (const json::Value* manifest = root.find("manifest"); manifest && manifest->isObject()) {
        RemoteFile file;
        if (!readFile(RemoteFile::Kind::Manifest, *manifest, file)) {
            return Error{"files.json: a manifest without path, size or digest"};
        }
        list.manifest = std::move(file);
    }
    if (const json::Value* wow = root.find("wow"); wow && wow->isArray()) {
        list.wowOffered = true;
        for (const json::Value& item : wow->items()) {
            RemoteFile file;
            if (!readFile(RemoteFile::Kind::Wow, item, file) ||
                file.path.compare(0, kWowPrefix.size(), kWowPrefix) != 0) {
                return Error{"files.json: a WoW archive without path, size or digest"};
            }
            list.wow.push_back(std::move(file));
        }
    }
    return list;
}

std::filesystem::path localPathOf(const RemoteFile& file, const std::filesystem::path& packsDir,
                                  const std::filesystem::path& wowDir) {
    const std::string_view prefix = file.kind == RemoteFile::Kind::Wow ? kWowPrefix : kPackPrefix;
    const std::string_view path = file.path;
    if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
        return {};
    }
    const std::string_view name = path.substr(prefix.size());
    if (!plainFileName(name) || endsWithIgnoringCase(name, kPartSuffix)) {
        return {};
    }
    switch (file.kind) {
    case RemoteFile::Kind::Pack:
        if (!pack::isClientPack(name)) {
            return {}; // app.vdpk comes with the APK; anything else is not a pack
        }
        return packsDir / pathFromUtf8(name);
    case RemoteFile::Kind::Manifest:
        return name == pack::kManifestFile ? packsDir / pathFromUtf8(name) : std::filesystem::path{};
    case RemoteFile::Kind::Wow:
        return endsWithIgnoringCase(name, ".mpq") ? wowDir / pathFromUtf8(name) : std::filesystem::path{};
    }
    return {};
}

i64 fileTimeOf(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<i64>(time.time_since_epoch().count());
}

void DownloadRecords::load(const std::filesystem::path& file) {
    m_file = file;
    m_records.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Result<json::Value> document = json::parse(text);
    if (!document || document.value().get("format").asInt() != kFormat) {
        VD_LOG_WARN("Downloads: %s cannot be read: starting over", utf8Of(file).c_str());
        return;
    }
    for (const json::Member& member : document.value().get("files").members()) {
        Record record;
        record.size = static_cast<u64>(member.value.get("size").asNumber(0.0));
        // The time as text: the file clock's ticks pass 2^53 (Windows counts 100 ns since 1601).
        record.time = std::strtoll(std::string(member.value.get("time").asString()).c_str(), nullptr, 10);
        record.sha256 = lowered(member.value.get("sha256").asString());
        if (isDigest(record.sha256)) {
            m_records[member.key] = std::move(record);
        }
    }
}

bool DownloadRecords::save() const {
    if (m_file.empty()) {
        return false;
    }
    json::Value root = json::Value::object();
    root.set("format", json::Value::number(kFormat));
    json::Value& files = root.set("files", json::Value::object());
    for (const auto& [path, record] : m_records) {
        json::Value& item = files.set(path, json::Value::object());
        item.set("size", json::Value::number(static_cast<f64>(record.size)));
        item.set("time", json::Value::string(std::to_string(record.time)));
        item.set("sha256", json::Value::string(record.sha256));
    }
    const std::string text = json::write(root);
    std::error_code error;
    std::filesystem::create_directories(m_file.parent_path(), error);
    std::filesystem::path temporary = m_file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(temporary, m_file, error);
    return !error;
}

bool DownloadRecords::known(std::string_view path) const {
    return m_records.find(path) != m_records.end();
}

std::string_view DownloadRecords::digestOf(std::string_view path, u64 size, i64 time) const {
    const auto it = m_records.find(path);
    if (it == m_records.end() || it->second.size != size || it->second.time != time) {
        return {};
    }
    return it->second.sha256;
}

void DownloadRecords::record(std::string_view path, u64 size, i64 time, std::string_view sha256) {
    Record& record = m_records[std::string(path)];
    record.size = size;
    record.time = time;
    record.sha256 = lowered(sha256);
}

FileNeed fileNeed(const RemoteFile& remote, const LocalFile& local) {
    if (!local.exists || local.size != remote.size) {
        return FileNeed::Fetch;
    }
    if (local.sha256.empty()) {
        return FileNeed::Hash;
    }
    return local.sha256 == remote.sha256 ? FileNeed::Have : FileNeed::Fetch;
}

u64 resumeOffset(u64 partBytes, u64 size) {
    return partBytes > size ? 0 : partBytes;
}

ResumeAction resumeAction(const net::HttpResponseHead& head, u64 offset, u64 size) {
    switch (head.status) {
    case 200:
        // The whole file: of this size (a chunked body is measured when it ends).
        return head.contentLength < 0 || static_cast<u64>(head.contentLength) == size ? ResumeAction::Restart
                                                                                      : ResumeAction::Fail;
    case 206: {
        net::ContentRange range;
        if (offset > 0 && net::parseContentRange(head.header("content-range"), range) && !range.unsatisfied &&
            range.first == offset && range.total == size && range.last + 1 == size &&
            (head.contentLength < 0 || static_cast<u64>(head.contentLength) == size - offset)) {
            return ResumeAction::Append;
        }
        return ResumeAction::Fail;
    }
    case 416: return ResumeAction::StartOver;
    default: return ResumeAction::Fail;
    }
}

// --- The thread
// --------------------------------------------------------------------------------------------------

DataDownload::~DataDownload() {
    stop();
}

void DataDownload::start(Places places) {
    stop();
    m_places = std::move(places);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = false;
        m_next.reset();
    }
    m_thread = std::thread([this] { run(); });
}

void DataDownload::stop() {
    if (!m_thread.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
        m_next.reset();
        m_cancel = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

u32 DataDownload::submit(Job job) {
    std::lock_guard<std::mutex> lock(m_mutex);
    job.id = ++m_lastJob;
    m_currentJob = job.id;
    m_status = DownloadStatus{};
    m_status.job = job.id;
    m_status.phase = Phase::Contacting;
    if (job.kind == Job::Kind::Pair) {
        m_pairedKey.clear();
    }
    m_next = std::move(job);
    m_cancel = true; // the job running, if any, stops; run() clears the flag for this one
    m_wake.notify_all();
    return m_currentJob;
}

u32 DataDownload::pair(const net::HttpEndpoint& pc, std::string_view code) {
    Job job;
    job.kind = Job::Kind::Pair;
    job.pc = pc;
    job.secret = std::string(code);
    return submit(std::move(job));
}

u32 DataDownload::check(const net::HttpEndpoint& pc, std::string_view key) {
    Job job;
    job.kind = Job::Kind::Check;
    job.pc = pc;
    job.secret = std::string(key);
    return submit(std::move(job));
}

u32 DataDownload::download(const net::HttpEndpoint& pc, std::string_view key, bool includeWow) {
    Job job;
    job.kind = Job::Kind::Download;
    job.pc = pc;
    job.secret = std::string(key);
    job.includeWow = includeWow;
    return submit(std::move(job));
}

void DataDownload::cancel() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_next.reset();
    m_cancel = true;
}

DownloadStatus DataDownload::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

std::string DataDownload::pairedKey() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pairedKey;
}

void DataDownload::run() {
    setCurrentThreadName("vd-download");
    VD_PROFILE_THREAD_NAME("vd-download");
    m_block.resize(kBlockBytes);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_quit || m_next.has_value(); });
            if (m_quit) {
                return;
            }
            job = std::move(*m_next);
            m_next.reset();
            m_cancel = false;
            m_running = job.id;
        }
        execute(job);
    }
}

template <typename F> void DataDownload::publish(F&& change) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_status.job == m_running) {
        change(m_status);
    }
}

void DataDownload::setDetail(std::string_view detail) {
    publish([&](DownloadStatus& s) { copyText(s.detail, detail); });
}

void DataDownload::fail(Failure failure, std::string_view detail) {
    VD_LOG_WARN("Download: job %u failed (%u)%s%.*s", m_running, static_cast<u32>(failure),
                detail.empty() ? "" : ": ", static_cast<int>(detail.size()), detail.data());
    publish([&](DownloadStatus& s) {
        s.phase = Phase::Failed;
        s.failure = failure;
        if (!detail.empty()) {
            copyText(s.detail, detail);
        }
    });
}

bool DataDownload::sleepFor(u32 ms) {
    for (u32 slept = 0; slept < ms; slept += 50) {
        if (m_cancel.load()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !m_cancel.load();
}

net::HttpGet DataDownload::request(const Job& job, std::string target) const {
    net::HttpGet get;
    get.target = std::move(target);
    get.cancel = &m_cancel;
    if (job.kind != Job::Kind::Pair) {
        get.headers.emplace_back("X-VD-Key", job.secret); // not in the URL: it stays out of any log
    }
    return get;
}

void DataDownload::execute(const Job& job) {
    VD_LOG_INFO("Download: job %u (%s) with %s", job.id,
                job.kind == Job::Kind::Pair    ? "pair"
                : job.kind == Job::Kind::Check ? "check"
                                               : "download",
                job.pc.text().c_str());
    switch (job.kind) {
    case Job::Kind::Pair: runPair(job); break;
    case Job::Kind::Check: runList(job, false); break;
    case Job::Kind::Download: runList(job, true); break;
    }
    if (m_cancel.load()) {
        publish([](DownloadStatus& s) {
            if (s.phase != Phase::Failed && s.phase != Phase::Done) {
                s.phase = Phase::Cancelled;
            }
        });
    }
}

void DataDownload::runPair(const Job& job) {
    // Is it VanillaDroid Setup?
    std::string body;
    net::HttpOutcome got = net::httpGetText(job.pc, request(job, "/info.json"), kMaxSmallBytes, body);
    if (got.kind == net::HttpOutcome::Kind::Cancelled) {
        return;
    }
    if (got.kind != net::HttpOutcome::Kind::Complete) {
        fail(got.hasHead ? Failure::NotSetup : Failure::Unreachable, got.error);
        return;
    }
    Result<json::Value> info = json::parse(body);
    if (got.head.status != 200 || !info || info.value().get("format").asInt() != kFormat ||
        info.value().get("service").asString() != "vanilladroid-setup") {
        fail(Failure::NotSetup);
        return;
    }
    // The code for the key.
    got = net::httpGetText(job.pc, request(job, "/pair?code=" + job.secret), kMaxSmallBytes, body);
    if (got.kind == net::HttpOutcome::Kind::Cancelled) {
        return;
    }
    if (got.kind != net::HttpOutcome::Kind::Complete) {
        fail(Failure::Unreachable, got.error);
        return;
    }
    switch (got.head.status) {
    case 200: break;
    case 403: fail(Failure::WrongCode); return;
    case 429: fail(Failure::CodeLocked); return;
    default: fail(Failure::NotSetup); return;
    }
    Result<json::Value> answer = json::parse(body);
    const std::string key = answer ? lowered(answer.value().get("key").asString()) : std::string();
    if (key.size() != 32 || key.find_first_not_of("0123456789abcdef") != std::string::npos) {
        fail(Failure::NotSetup);
        return;
    }
    VD_LOG_INFO("Download: paired with %s", job.pc.text().c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_status.job == m_running) {
        m_pairedKey = key;
        m_status.phase = Phase::Paired;
    }
}

void DataDownload::runList(const Job& job, bool download) {
    m_records.load(m_places.recordsFile);
    m_damaged.clear();
    u32 failures = 0;
    bool contacted = false;
    for (;;) {
        RemoteList list;
        // The first contact fails at once (a wrong address says so); once a download began, the PC may come
        // back.
        const Step listed = fetchList(job, list, download && contacted);
        if (listed == Step::Stop) {
            return;
        }
        if (listed == Step::Retry) {
            if (!waitRetry(failures++)) {
                return;
            }
            continue;
        }
        contacted = true;
        if (list.packs.empty()) {
            fail(Failure::NoPacks);
            return;
        }
        Plan plan;
        if (makePlan(list, plan) != Step::Ok) {
            return;
        }
        if (!download) {
            publish([](DownloadStatus& s) { s.phase = Phase::Ready; });
            return;
        }
        if (fetchAll(job, list, plan, failures) != Step::Relist) {
            return;
        }
        VD_LOG_INFO("Download: the PC's files changed: listing them again");
    }
}

DataDownload::Step DataDownload::fetchList(const Job& job, RemoteList& list, bool retry) {
    std::string body;
    for (;;) {
        const net::HttpOutcome got =
            net::httpGetText(job.pc, request(job, "/files.json"), kMaxListBytes, body);
        if (got.kind == net::HttpOutcome::Kind::Cancelled || m_cancel.load()) {
            return Step::Stop;
        }
        if (got.kind != net::HttpOutcome::Kind::Complete) {
            if (retry) {
                setDetail(got.error);
                return Step::Retry;
            }
            fail(got.hasHead ? Failure::BadList : Failure::Unreachable, got.error);
            return Step::Stop;
        }
        if (got.head.status == 401) {
            fail(Failure::Unauthorized);
            return Step::Stop;
        }
        if (got.head.status == 503) {
            // The PC is still taking its files' digests (the first time: a minute or more).
            Result<json::Value> state = json::parse(body);
            const f32 progress = state ? static_cast<f32>(state.value().get("progress").asNumber(0.0)) : 0.0f;
            publish([&](DownloadStatus& s) {
                s.phase = Phase::Preparing;
                s.fraction = std::clamp(progress, 0.0f, 1.0f);
            });
            const i64 after = std::atoll(std::string(got.head.header("retry-after")).c_str());
            if (!sleepFor(static_cast<u32>(std::clamp<i64>(after, 1, 10)) * 1000)) {
                return Step::Stop;
            }
            continue;
        }
        if (got.head.status != 200) {
            fail(got.head.status == 404 ? Failure::NotSetup : Failure::BadList);
            return Step::Stop;
        }
        Result<RemoteList> parsed = parseFilesJson(body);
        if (!parsed) {
            fail(Failure::BadList, parsed.error().message);
            return Step::Stop;
        }
        list = std::move(parsed.value());
        return Step::Ok;
    }
}

std::optional<std::string> DataDownload::hashFile(const std::filesystem::path& path, u64 bytes, u64& hashed,
                                                  u64 total, bool checking) {
    std::FILE* in = openFile(path, "rb");
    if (!in) {
        return std::nullopt;
    }
    Sha256 sha;
    u64 left = bytes;
    bool ok = true;
    while (left > 0) {
        if (m_cancel.load()) {
            ok = false;
            break;
        }
        const usize want = static_cast<usize>(std::min<u64>(left, m_block.size()));
        const usize read = std::fread(m_block.data(), 1, want, in);
        if (read != want) {
            ok = false;
            break;
        }
        sha.update(std::span<const u8>(m_block.data(), read));
        left -= read;
        hashed += read;
        if (checking && total > 0) {
            const f32 fraction = static_cast<f32>(static_cast<f64>(hashed) / static_cast<f64>(total));
            publish([&](DownloadStatus& s) { s.fraction = fraction; });
        }
    }
    std::fclose(in);
    if (!ok) {
        return std::nullopt;
    }
    return Sha256::hex(sha.finish());
}

DataDownload::Step DataDownload::makePlan(const RemoteList& list, Plan& plan) {
    std::error_code error;
    std::filesystem::create_directories(m_places.packsDir, error);
    // A pack's digest may be in the manifest the last bake or download left (adb push writes it too).
    Result<pack::PackManifest> loaded = pack::PackManifest::load(m_places.packsDir);
    const pack::PackManifest manifest = loaded ? std::move(loaded.value()) : pack::PackManifest{};

    struct Unknown {
        PlanItem item;
        u64 size = 0;
        i64 time = 0;
    };
    std::vector<Unknown> unknown;
    u64 unknownBytes = 0;
    const auto add = [&](PlanItem item) {
        switch (item.remote.kind) {
        case RemoteFile::Kind::Pack: plan.packs.push_back(std::move(item)); break;
        case RemoteFile::Kind::Manifest: plan.manifest = std::move(item); break;
        case RemoteFile::Kind::Wow: plan.wow.push_back(std::move(item)); break;
        }
    };
    std::vector<const RemoteFile*> files;
    for (const RemoteFile& file : list.packs) {
        files.push_back(&file);
    }
    if (list.manifest) {
        files.push_back(&*list.manifest);
    }
    for (const RemoteFile& file : list.wow) {
        files.push_back(&file);
    }
    for (const RemoteFile* file : files) {
        PlanItem item;
        item.remote = *file;
        item.local = localPathOf(*file, m_places.packsDir, m_places.wowDir);
        if (item.local.empty()) {
            fail(Failure::BadList, file->path);
            return Step::Stop;
        }
        LocalFile local;
        local.size = sizeOf(item.local, local.exists);
        bool partExists = false;
        local.partBytes = sizeOf(partOf(item.local), partExists);
        item.partBytes = resumeOffset(local.partBytes, file->size);
        const i64 time = local.exists ? fileTimeOf(item.local) : 0;
        if (local.exists) {
            local.sha256 = std::string(m_records.digestOf(file->path, local.size, time));
            // A pack this app never saw (adb push writes the manifest with each pack's digest): the
            // manifest's word. One it saw and that changed since is hashed again, whatever the manifest says.
            if (local.sha256.empty() && file->kind == RemoteFile::Kind::Pack &&
                !m_records.known(file->path)) {
                if (const pack::PackManifest::Entry* entry = manifest.find(fileNameOf(file->path));
                    entry && entry->size == local.size) {
                    local.sha256 = lowered(entry->sha256);
                }
            }
        }
        switch (fileNeed(*file, local)) {
        case FileNeed::Have: break;
        case FileNeed::Hash:
            unknownBytes += local.size;
            unknown.push_back({std::move(item), local.size, time});
            break;
        case FileNeed::Fetch: add(std::move(item)); break;
        }
    }
    if (!unknown.empty()) {
        // Files the phone has with the PC's size but no digest it knows (copied by hand, an older bake):
        // hashed once, then kept in downloads.json.
        VD_LOG_INFO("Download: hashing %zu files on this device (%llu MB)", unknown.size(),
                    static_cast<unsigned long long>(unknownBytes >> 20));
        publish([](DownloadStatus& s) {
            s.phase = Phase::Checking;
            s.fraction = 0.0f;
        });
        u64 hashed = 0;
        for (Unknown& entry : unknown) {
            const std::optional<std::string> digest =
                hashFile(entry.item.local, entry.size, hashed, unknownBytes, true);
            if (m_cancel.load()) {
                return Step::Stop;
            }
            if (digest) {
                m_records.record(entry.item.remote.path, entry.size, entry.time, *digest);
            }
            if (!digest || *digest != entry.item.remote.sha256) {
                add(std::move(entry.item));
            }
        }
        m_records.save();
    }
    if (!plan.packs.empty() && list.manifest && !plan.manifest) {
        // The manifest goes while packs change (fetchAll): it comes back last, even when it was up to date.
        PlanItem item;
        item.remote = *list.manifest;
        item.local = localPathOf(item.remote, m_places.packsDir, m_places.wowDir);
        bool partExists = false;
        item.partBytes = resumeOffset(sizeOf(partOf(item.local), partExists), item.remote.size);
        plan.manifest = std::move(item);
    }
    u64 packBytes = 0, wowBytes = 0;
    for (const PlanItem& item : plan.packs) {
        packBytes += item.remote.size - item.partBytes;
    }
    if (plan.manifest) {
        packBytes += plan.manifest->remote.size - plan.manifest->partBytes;
    }
    for (const PlanItem& item : plan.wow) {
        wowBytes += item.remote.size - item.partBytes;
    }
    const std::filesystem::space_info space = std::filesystem::space(m_places.packsDir, error);
    const u64 freeBytes = error ? 0 : static_cast<u64>(space.available);
    VD_LOG_INFO(
        "Download: %zu of %zu packs to fetch (%llu MB), %zu of %zu WoW archives (%llu MB), %llu MB free",
        plan.packs.size() + (plan.manifest ? 1 : 0), list.packs.size() + (list.manifest ? 1 : 0),
        static_cast<unsigned long long>(packBytes >> 20), plan.wow.size(), list.wow.size(),
        static_cast<unsigned long long>(wowBytes >> 20), static_cast<unsigned long long>(freeBytes >> 20));
    publish([&](DownloadStatus& s) {
        s.packFiles = static_cast<u32>(plan.packs.size() + (plan.manifest ? 1 : 0));
        s.packBytes = packBytes;
        s.wowFiles = static_cast<u32>(plan.wow.size());
        s.wowBytes = wowBytes;
        s.wowOffered = list.wowOffered && !list.wow.empty();
        s.freeBytes = freeBytes;
    });
    return Step::Ok;
}

bool DataDownload::waitRetry(u32 failures) {
    const u32 seconds = std::min(kMaxRetrySeconds, 2u << std::min(failures, 4u)); // 2, 4, 8, 16, 30...
    m_speedSamples = 0;
    for (u32 left = seconds; left > 0; --left) {
        publish([&](DownloadStatus& s) {
            s.phase = Phase::Waiting;
            s.retrySeconds = left;
            s.bytesPerSecond = 0.0f;
        });
        if (!sleepFor(1000)) {
            return false;
        }
    }
    publish([](DownloadStatus& s) {
        s.phase = Phase::Downloading;
        s.retrySeconds = 0;
    });
    return true;
}

void DataDownload::addProgress(u64 doneBytes, u64 networkBytes) {
    m_networkBytes += networkBytes;
    const auto now = std::chrono::steady_clock::now();
    if (m_speedSamples == 0 || now - m_lastSample >= std::chrono::milliseconds(500)) {
        m_speed[m_speedSamples % m_speed.size()] = {now, m_networkBytes};
        ++m_speedSamples;
        m_lastSample = now;
    }
    // The speed over the samples kept (the last four seconds).
    const SpeedSample& oldest =
        m_speed[m_speedSamples >= m_speed.size() ? m_speedSamples % m_speed.size() : 0];
    const f64 seconds = std::chrono::duration<f64>(now - oldest.time).count();
    const f32 speed =
        seconds > 0.4 ? static_cast<f32>(static_cast<f64>(m_networkBytes - oldest.bytes) / seconds) : 0.0f;
    publish([&](DownloadStatus& s) {
        s.phase = Phase::Downloading;
        s.doneBytes = doneBytes;
        s.retrySeconds = 0;
        if (speed > 0.0f) {
            s.bytesPerSecond = speed;
        }
    });
}

DataDownload::Step DataDownload::fetchAll(const Job& job, const RemoteList& list, const Plan& plan,
                                          u32& failures) {
    std::vector<const PlanItem*> items;
    for (const PlanItem& item : plan.packs) {
        items.push_back(&item);
    }
    if (job.includeWow) {
        for (const PlanItem& item : plan.wow) {
            items.push_back(&item);
        }
    }
    if (plan.manifest) {
        items.push_back(&*plan.manifest); // last: it says the packs are complete
    }
    u64 total = 0, parts = 0, needed = 0;
    for (const PlanItem* item : items) {
        total += item->remote.size;
        parts += item->partBytes;
        needed += item->remote.size - item->partBytes;
    }
    std::error_code error;
    const std::filesystem::space_info space = std::filesystem::space(m_places.packsDir, error);
    const u64 freeBytes = error ? ~0ull : static_cast<u64>(space.available);
    if (needed + kSpareBytes > freeBytes) {
        publish([&](DownloadStatus& s) {
            s.neededBytes = needed + kSpareBytes;
            s.freeBytes = freeBytes;
        });
        fail(Failure::NoSpace);
        return Step::Stop;
    }
    if (!plan.packs.empty()) {
        // The manifest there describes packs about to change: gone until the new one arrives (downloads.json
        // keeps the digests of what is fetched meanwhile).
        std::filesystem::remove(m_places.packsDir / std::string(pack::kManifestFile), error);
    }
    if (job.includeWow && !plan.wow.empty()) {
        std::filesystem::create_directories(m_places.wowDir, error);
    }
    m_networkBytes = 0;
    m_speedSamples = 0;
    publish([&](DownloadStatus& s) {
        s.phase = Phase::Downloading;
        s.totalBytes = total;
        s.doneBytes = parts;
        s.fileCount = static_cast<u32>(items.size());
        s.bytesPerSecond = 0.0f;
    });
    u64 completed = 0;
    for (usize i = 0; i < items.size(); ++i) {
        const PlanItem& item = *items[i];
        parts -= item.partBytes; // this one's part is counted by fetchFile from now on
        publish([&](DownloadStatus& s) {
            s.fileIndex = static_cast<u32>(i + 1);
            copyText(s.file, fileNameOf(item.remote.path));
        });
        for (;;) {
            const u64 before = m_networkBytes;
            const Step step = fetchFile(job, item, completed + parts);
            if (m_networkBytes > before) {
                failures = 0; // bytes flowed: the next cut waits the least again
            }
            if (step == Step::Ok) {
                break;
            }
            if (step == Step::Again) {
                continue;
            }
            if (step == Step::Retry) {
                if (!waitRetry(failures++)) {
                    return Step::Stop;
                }
                continue;
            }
            return step; // Relist, Stop
        }
        completed += item.remote.size;
    }
    removeStale(list);
    VD_LOG_INFO("Download: done, %zu files (%llu MB)", items.size(),
                static_cast<unsigned long long>(total >> 20));
    publish([&](DownloadStatus& s) {
        s.phase = Phase::Done;
        s.doneBytes = total;
    });
    return Step::Ok;
}

DataDownload::Step DataDownload::fetchFile(const Job& job, const PlanItem& item, u64 doneBefore) {
    const RemoteFile& remote = item.remote;
    const std::filesystem::path part = partOf(item.local);
    bool partExists = false;
    const u64 partBytes = sizeOf(part, partExists);
    const u64 offset = resumeOffset(partBytes, remote.size);
    std::error_code error;
    if (partExists && offset == 0) {
        std::filesystem::remove(part, error);
    }
    const auto damaged = [&]() {
        std::filesystem::remove(part, error);
        if (++m_damaged[remote.path] >= kDamagedTries) {
            fail(Failure::Damaged, fileNameOf(remote.path));
            return Step::Stop;
        }
        VD_LOG_WARN("Download: %s arrived with another digest: again", remote.path.c_str());
        return Step::Again;
    };
    Sha256 sha;
    if (offset > 0) {
        // Resuming: the bytes already there go into the digest first.
        std::FILE* in = openFile(part, "rb");
        u64 left = in ? offset : 0;
        while (in && left > 0 && !m_cancel.load()) {
            const usize want = static_cast<usize>(std::min<u64>(left, m_block.size()));
            if (std::fread(m_block.data(), 1, want, in) != want) {
                break;
            }
            sha.update(std::span<const u8>(m_block.data(), want));
            left -= want;
        }
        if (in) {
            std::fclose(in);
        }
        if (m_cancel.load()) {
            return Step::Stop;
        }
        if (!in || left > 0) {
            std::filesystem::remove(part, error);
            return Step::Again;
        }
        if (offset == remote.size) { // all of it was there: only the check was missing
            if (Sha256::hex(sha.finish()) != remote.sha256) {
                return damaged();
            }
            return finishFile(item, part) ? Step::Ok : Step::Stop;
        }
        VD_LOG_INFO("Download: resuming %s at %llu of %llu bytes", remote.path.c_str(),
                    static_cast<unsigned long long>(offset), static_cast<unsigned long long>(remote.size));
    }
    std::FILE* out = openFile(part, offset > 0 ? "ab" : "wb");
    if (!out) {
        fail(Failure::Disk, utf8Of(part));
        return Step::Stop;
    }
    std::setvbuf(out, nullptr, _IOFBF, 1u << 20);
    net::HttpGet get = request(job, "/" + percentEncodePath(remote.path));
    if (offset > 0) {
        get.headers.emplace_back("Range", "bytes=" + std::to_string(offset) + "-");
        get.headers.emplace_back("If-Range",
                                 "\"" + remote.sha256 + "\""); // the ETag: another file sends it whole
    }
    u64 written = offset;
    ResumeAction action = ResumeAction::Fail;
    bool writeFailed = false;
    const net::HttpOutcome got = net::httpGet(
        job.pc, get,
        [&](const net::HttpResponseHead& head) {
            action = resumeAction(head, offset, remote.size);
            if (action == ResumeAction::Restart && written > 0) {
                // The whole file after all: the .part starts over.
                std::fclose(out);
                out = openFile(part, "wb");
                if (!out) {
                    writeFailed = true;
                    return false;
                }
                std::setvbuf(out, nullptr, _IOFBF, 1u << 20);
                sha.finish(); // starts over
                written = 0;
            }
            return action == ResumeAction::Append || action == ResumeAction::Restart;
        },
        [&](std::span<const u8> piece) {
            if (written + piece.size() > remote.size) {
                return false; // longer than the file: not it
            }
            if (std::fwrite(piece.data(), 1, piece.size(), out) != piece.size()) {
                writeFailed = true;
                return false;
            }
            sha.update(piece);
            written += piece.size();
            addProgress(doneBefore + written, piece.size());
            return true;
        });
    const bool closed = out && std::fclose(out) == 0;
    if (writeFailed || !closed) {
        fail(Failure::Disk, utf8Of(part));
        return Step::Stop;
    }
    switch (got.kind) {
    case net::HttpOutcome::Kind::Cancelled: return Step::Stop;
    case net::HttpOutcome::Kind::Unreachable:
    case net::HttpOutcome::Kind::Cut:
    case net::HttpOutcome::Kind::Malformed:
        VD_LOG_WARN("Download: %s cut at %llu of %llu bytes: %s", remote.path.c_str(),
                    static_cast<unsigned long long>(written), static_cast<unsigned long long>(remote.size),
                    got.error.c_str());
        setDetail(got.error);
        return Step::Retry; // the .part stays: resumed from there
    case net::HttpOutcome::Kind::Refused:
        if (action == ResumeAction::StartOver) {
            std::filesystem::remove(part, error);
            return Step::Again;
        }
        if (got.hasHead && got.head.status == 401) {
            fail(Failure::Unauthorized);
            return Step::Stop;
        }
        if (action == ResumeAction::Append || action == ResumeAction::Restart) {
            return damaged(); // more bytes than the file has
        }
        VD_LOG_WARN("Download: %s answered %d: listing the files again", remote.path.c_str(),
                    got.head.status);
        return Step::Relist; // 404, 503 (changed since listed, or preparing again), a range that does not fit
    case net::HttpOutcome::Kind::Complete: break;
    }
    if (written < remote.size) {
        return Step::Retry; // ended early: resumed from there
    }
    if (Sha256::hex(sha.finish()) != remote.sha256) {
        return damaged();
    }
    return finishFile(item, part) ? Step::Ok : Step::Stop;
}

bool DataDownload::finishFile(const PlanItem& item, const std::filesystem::path& part) {
    std::error_code error;
    std::filesystem::rename(part, item.local, error);
    if (error) {
        fail(Failure::Disk, utf8Of(item.local) + ": " + error.message());
        return false;
    }
    m_records.record(item.remote.path, item.remote.size, fileTimeOf(item.local), item.remote.sha256);
    m_records.save();
    VD_LOG_INFO("Download: %s verified (%llu bytes)", item.remote.path.c_str(),
                static_cast<unsigned long long>(item.remote.size));
    return true;
}

void DataDownload::removeStale(const RemoteList& list) {
    // The PC's packs are the set: a client pack it no longer has (another language, a map gone) would be read
    // with the new ones. app.vdpk is not a client pack; .part files left by other versions of a file go too.
    std::set<std::string, std::less<>> wanted;
    for (const RemoteFile& file : list.packs) {
        wanted.insert(std::string(fileNameOf(file.path)));
    }
    std::error_code error;
    for (const std::filesystem::path& dir : {m_places.packsDir, m_places.wowDir}) {
        std::vector<std::filesystem::path> stale;
        for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
            const std::string name = utf8Of(entry.path().filename());
            const bool part = endsWithIgnoringCase(name, kPartSuffix);
            const bool oldPack = dir == m_places.packsDir && pack::isClientPack(name) && !wanted.count(name);
            if ((part || oldPack) && entry.is_regular_file(error)) {
                stale.push_back(entry.path());
            }
        }
        for (const std::filesystem::path& path : stale) {
            VD_LOG_INFO("Download: removing %s (not on the PC)", utf8Of(path.filename()).c_str());
            std::filesystem::remove(path, error);
        }
    }
}

} // namespace vd
