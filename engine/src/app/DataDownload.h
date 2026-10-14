#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "net/HttpClient.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Phase 22 (pairing, ROADMAP 22.4, ADR-0005 §5): the game's data from VanillaDroid Setup on the PC, over the
// local network (tools/setup/PhoneServer.h, protocol 1). On a thread of its own (vd-download), never the
// game's: files.json, then every file the phone lacks or has in another version (its size and SHA-256 against
// what the phone knows: downloads.json, packs/manifest.json, else the file hashed once) into <file>.part,
// resumed with Range / If-Range after a cut, verified with SHA-256 before it is renamed into place;
// packs/manifest.json last. Retries with back-off while the PC does not answer, waits while it is still
// taking its files' digests (503 "preparing"), stops on a key the PC no longer knows (401: pair again). The
// rules (what to fetch, how to resume) are pure functions for the tests.
namespace vd {

// files.json, format 1: what the PC offers.
struct RemoteFile {
    enum class Kind : u8 { Pack, Manifest, Wow };
    Kind kind = Kind::Pack;
    std::string path; // "packs/models.vdpk", "packs/manifest.json", "wow/Data/patch.MPQ"
    u64 size = 0;
    std::string sha256; // 64 hex digits, lowercase
};
struct RemoteList {
    std::string setup;        // the Setup's version
    std::string clientLocale; // the base client's language (empty before a bake of phase 22)
    std::vector<RemoteFile> packs;
    std::optional<RemoteFile> manifest; // packs/manifest.json: fetched last
    std::vector<RemoteFile> wow;        // the WoW's archives (vdsetup serve --wow)
    bool wowOffered = false;
};
Result<RemoteList> parseFilesJson(std::string_view text);

// Where a file of the list goes: "packs/<name>" into `packsDir`, "wow/Data/<name>" into `wowDir`. Empty for a
// name that is not a plain file name (separators, "..", a drive, control characters): the PC's list never
// chooses a place outside those two folders.
std::filesystem::path localPathOf(const RemoteFile& file, const std::filesystem::path& packsDir,
                                  const std::filesystem::path& wowDir);

// A file's modification time as downloads.json keeps it (the file clock's ticks); 0 when it cannot be read.
i64 fileTimeOf(const std::filesystem::path& path);

// downloads.json, in the app's internal storage: the SHA-256 of each file this app received or hashed, with
// the size and time the file had then (a file changed since is hashed again).
class DownloadRecords {
public:
    void load(const std::filesystem::path& file);
    bool save() const;
    // The digest recorded for `path` while the file still has that size and time; empty otherwise.
    std::string_view digestOf(std::string_view path, u64 size, i64 time) const;
    bool known(std::string_view path) const; // recorded once, changed since or not
    void record(std::string_view path, u64 size, i64 time, std::string_view sha256);

private:
    struct Record {
        u64 size = 0;
        i64 time = 0;
        std::string sha256;
    };
    std::filesystem::path m_file;
    std::map<std::string, Record, std::less<>> m_records;
};

// What the device has for one file of the list.
struct LocalFile {
    bool exists = false;
    u64 size = 0;
    std::string sha256; // known (downloads.json, or packs/manifest.json for a pack); empty when not
    u64 partBytes = 0;  // of <file>.part, a download cut short
};
enum class FileNeed : u8 {
    Have,  // there, with the PC's digest
    Hash,  // there with the PC's size, its digest unknown: hash it, then decide again
    Fetch, // missing or another version
};
FileNeed fileNeed(const RemoteFile& remote, const LocalFile& local);

// Resuming: the first byte to ask for, given the bytes of the .part (more than the file: start over, 0; all
// of it: `size`, nothing to ask, only to verify).
u64 resumeOffset(u64 partBytes, u64 size);
enum class ResumeAction : u8 {
    Append,    // 206 from `offset` of a file of this size: the body goes on the end of the .part
    Restart,   // 200 with the whole file (it changed on the PC, or the range was not honoured): the .part
               // starts over
    StartOver, // 416: the .part is not of this file: drop it and ask again without a range
    Fail,      // anything else (401, 404, 503, a range that does not fit...)
};
ResumeAction resumeAction(const net::HttpResponseHead& head, u64 offset, u64 size);

// What the download thread is doing, copied to the game thread every frame (fixed size: no allocation).
struct DownloadStatus {
    enum class Phase : u8 {
        Idle,
        Contacting,  // talking to the PC (info.json, the code, files.json)
        Preparing,   // the PC is taking its files' digests (503 "preparing"): `fraction`
        Checking,    // hashing the phone's files whose digest is unknown: `fraction`
        Paired,      // the code was exchanged for the key (pairedKey())
        Ready,       // the plan of a download: the counts and sizes below
        Downloading, // fileIndex of fileCount, doneBytes of totalBytes
        Waiting,     // the PC stopped answering: the next try in retrySeconds
        Done,        // everything fetched and verified, packs/manifest.json last
        Failed,      // `failure`
        Cancelled,   // stopped by cancel(): the .part files stay for the next time
    };
    enum class Failure : u8 {
        None,
        Unreachable,  // no answer at that address
        NotSetup,     // something answered, but not VanillaDroid Setup (protocol 1)
        WrongCode,    // 403 from /pair
        CodeLocked,   // 429 from /pair: too many wrong codes
        Unauthorized, // 401: the PC has another pairing key now
        NoSpace,      // neededBytes > freeBytes
        NoPacks,      // the PC has no packs to send
        BadList,      // files.json is not one this app reads
        Damaged,      // a file arrived with another SHA-256 twice
        Disk,         // writing to the device failed: `detail`
    };
    u32 job = 0;
    Phase phase = Phase::Idle;
    Failure failure = Failure::None;
    f32 fraction = 0.0f;
    // The plan (from Ready on): what a download fetches, the WoW apart.
    u32 packFiles = 0;
    u64 packBytes = 0; // still to fetch (the .part files already there left out)
    u32 wowFiles = 0;
    u64 wowBytes = 0;
    bool wowOffered = false;
    u64 freeBytes = 0;   // free where the packs go
    u64 neededBytes = 0; // NoSpace: what the download needs
    // Downloading and Waiting.
    u64 totalBytes = 0; // the files being fetched, whole
    u64 doneBytes = 0;  // of them on the device, the resumed parts included
    u32 fileIndex = 0;  // 1-based
    u32 fileCount = 0;
    f32 bytesPerSecond = 0.0f;
    u32 retrySeconds = 0;
    std::array<char, 64> file{};    // the file being fetched
    std::array<char, 160> detail{}; // the last error, for the log and the waiting line
};

class DataDownload {
public:
    struct Places {
        std::filesystem::path packsDir;
        std::filesystem::path wowDir;      // the WoW's archives (<app files>/wow/Data)
        std::filesystem::path recordsFile; // downloads.json, in the internal storage
    };
    // The room kept free beyond the download (the system and the game's own files need some).
    static constexpr u64 kSpareBytes = 256ull << 20;

    DataDownload() = default;
    ~DataDownload();
    DataDownload(const DataDownload&) = delete;
    DataDownload& operator=(const DataDownload&) = delete;

    void start(Places places); // the thread
    void stop();               // cancels and joins (shutdown only: may wait a quarter of a second)

    // Each starts a job on the thread (cancelling the one running, without waiting for it) and returns its
    // number, which status().job carries while it is the current one.
    u32 pair(const net::HttpEndpoint& pc, std::string_view code);
    u32 check(const net::HttpEndpoint& pc, std::string_view key);
    u32 download(const net::HttpEndpoint& pc, std::string_view key, bool includeWow);
    void cancel();

    DownloadStatus status() const;
    std::string pairedKey() const; // after Paired

private:
    struct Job {
        enum class Kind : u8 { Pair, Check, Download } kind = Kind::Check;
        u32 id = 0;
        net::HttpEndpoint pc;
        std::string secret; // the code (Pair) or the key
        bool includeWow = false;
    };
    struct PlanItem {
        RemoteFile remote;
        std::filesystem::path local;
        u64 partBytes = 0;
    };
    struct Plan {
        std::vector<PlanItem> packs; // packs to fetch, the manifest not among them
        std::optional<PlanItem> manifest;
        std::vector<PlanItem> wow;
    };
    enum class Step : u8 { Ok, Again, Retry, Relist, Stop };

    u32 submit(Job job);
    void run();
    void execute(const Job& job);
    void runPair(const Job& job);
    void runList(const Job& job, bool download);
    Step fetchList(const Job& job, RemoteList& list, bool firstContact);
    Step makePlan(const RemoteList& list, Plan& plan);
    Step fetchAll(const Job& job, const RemoteList& list, const Plan& plan, u32& failures);
    Step fetchFile(const Job& job, const PlanItem& item, u64 doneBefore);
    bool finishFile(const PlanItem& item, const std::filesystem::path& part);
    void removeStale(const RemoteList& list);
    bool waitRetry(u32 failures);
    bool sleepFor(u32 ms);
    std::optional<std::string> hashFile(const std::filesystem::path& path, u64 bytes, u64& hashed, u64 total,
                                        bool checking);
    net::HttpGet request(const Job& job, std::string target) const;

    template <typename F> void publish(F&& change);
    void fail(DownloadStatus::Failure failure, std::string_view detail = {});
    void setDetail(std::string_view detail);
    void addProgress(u64 doneBytes, u64 networkBytes);

    Places m_places;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<Job> m_next;
    bool m_quit = false;
    u32 m_lastJob = 0;
    u32 m_currentJob = 0; // the newest job: the one status() describes
    u32 m_running = 0;    // the job the thread runs (set with m_mutex held)
    DownloadStatus m_status;
    std::string m_pairedKey;
    std::atomic<bool> m_cancel{false};

    // The thread's own.
    DownloadRecords m_records;
    std::vector<u8> m_block;
    std::map<std::string, u32> m_damaged; // files that arrived with another digest, this job
    u64 m_networkBytes = 0;
    struct SpeedSample {
        std::chrono::steady_clock::time_point time;
        u64 bytes = 0;
    };
    std::array<SpeedSample, 8> m_speed{};
    u32 m_speedSamples = 0;
    std::chrono::steady_clock::time_point m_lastSample{};
};

} // namespace vd
