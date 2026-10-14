#include "HashCache.h"

#include "core/Json.h"
#include "core/Sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace vd::setup {
namespace {

constexpr i64 kFormat = 1;
constexpr usize kBlock = 1u << 20;

} // namespace

i64 fileTimeOf(const std::filesystem::path& file) {
    std::error_code error;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(file, error);
    return error ? 0 : static_cast<i64>(time.time_since_epoch().count());
}

std::string HashCache::keyOf(const std::filesystem::path& file) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(file, error);
    if (error) {
        absolute = file;
    }
    std::string key = utf8FromPath(absolute.lexically_normal());
#if defined(_WIN32)
    key = toLowerAscii(key); // one entry per file on a case-insensitive file system
#endif
    return key;
}

void HashCache::load() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    std::ifstream in(m_file, std::ios::binary);
    if (!in) {
        return;
    }
    std::ostringstream text;
    text << in.rdbuf();
    Result<json::Value> parsed = json::parse(text.str());
    if (!parsed || parsed.value().get("format").asInt() != kFormat) {
        return;
    }
    for (const json::Member& member : parsed.value().get("files").members()) {
        Entry entry;
        entry.size = static_cast<u64>(member.value.get("size").asInt());
        entry.time = std::strtoll(std::string(member.value.get("time").asString()).c_str(), nullptr, 10);
        entry.sha256 = std::string(member.value.get("sha256").asString());
        if (entry.sha256.size() == 64) {
            m_entries.emplace(member.key, std::move(entry));
        }
    }
}

Result<void> HashCache::save() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.empty()) {
        return {};
    }
    json::Value root = json::Value::object();
    root.set("format", json::Value::number(kFormat));
    json::Value& files = root.set("files", json::Value::object());
    for (const auto& [key, entry] : m_entries) {
        json::Value& value = files.set(key, json::Value::object());
        value.set("size", json::Value::number(static_cast<f64>(entry.size)));
        // A string: file times (100 ns ticks on Windows) do not fit a JSON number's 53 bits.
        value.set("time", json::Value::string(std::to_string(entry.time)));
        value.set("sha256", json::Value::string(entry.sha256));
    }
    const std::string text = json::write(root);
    std::error_code error;
    std::filesystem::create_directories(m_file.parent_path(), error);
    std::filesystem::path temp = m_file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            return Error{"Cannot write " + utf8FromPath(temp)};
        }
    }
    std::filesystem::rename(temp, m_file, error);
    if (error) {
        std::filesystem::remove(temp, error);
        return Error{"Cannot replace " + utf8FromPath(m_file)};
    }
    return {};
}

std::string HashCache::find(const std::filesystem::path& file, u64 size, i64 time) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(keyOf(file));
    if (it == m_entries.end() || it->second.size != size || it->second.time != time) {
        return {};
    }
    return it->second.sha256;
}

void HashCache::store(const std::filesystem::path& file, u64 size, i64 time, std::string sha256) {
    std::lock_guard<std::mutex> lock(m_mutex);
    Entry& entry = m_entries[keyOf(file)];
    entry.size = size;
    entry.time = time;
    entry.sha256 = std::move(sha256);
    m_dirty = true;
}

Result<std::string> sha256FileProgress(const std::filesystem::path& file,
                                       const std::function<void(u64)>& onBytes, const CancelToken* cancel) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return Error{"Cannot open " + utf8FromPath(file)};
    }
    Sha256 hasher;
    std::vector<u8> block(kBlock);
    for (;;) {
        if (cancel && cancel->cancelled()) {
            return Error{"cancelled"};
        }
        in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            hasher.update(block.data(), static_cast<usize>(got));
            if (onBytes) {
                onBytes(static_cast<u64>(got));
            }
        }
        if (!in) {
            if (in.eof()) {
                break;
            }
            return Error{"Cannot read " + utf8FromPath(file)};
        }
    }
    const std::array<u8, 32> digest = hasher.finish();
    return Sha256::hex(digest);
}

Result<void> hashFiles(std::span<HashJob> jobs, HashCache& cache, const Operation& operation) {
    std::vector<usize> pending;
    u64 totalBytes = 0;
    for (usize i = 0; i < jobs.size(); ++i) {
        HashJob& job = jobs[i];
        std::error_code error;
        job.size = std::filesystem::file_size(job.file, error);
        if (error) {
            return Error{"Cannot read " + utf8FromPath(job.file)};
        }
        job.time = fileTimeOf(job.file);
        job.sha256 = cache.find(job.file, job.size, job.time);
        if (job.sha256.empty()) {
            pending.push_back(i);
            totalBytes += job.size;
        }
    }
    if (pending.empty()) {
        return {};
    }
    // Largest first, so the threads finish together.
    std::sort(pending.begin(), pending.end(), [&](usize a, usize b) { return jobs[a].size > jobs[b].size; });
    const u32 threads = std::clamp<u32>(std::thread::hardware_concurrency() / 2, 1u,
                                        std::min<u32>(4u, static_cast<u32>(pending.size())));
    std::atomic<usize> next{0};
    std::atomic<u64> doneBytes{0};
    std::atomic<bool> failed{false};
    std::mutex errorMutex;
    std::string firstError;
    const std::string label = formatText("VD_SETUP_HASHING", {formatBytes(totalBytes)});
    auto worker = [&]() {
        for (;;) {
            const usize index = next.fetch_add(1);
            if (index >= pending.size() || failed.load() || operation.cancelled()) {
                return;
            }
            HashJob& job = jobs[pending[index]];
            Result<std::string> digest = sha256FileProgress(
                job.file, [&](u64 bytes) { doneBytes.fetch_add(bytes, std::memory_order_relaxed); },
                operation.cancel);
            if (!digest) {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (firstError.empty()) {
                    firstError = digest.error().message;
                }
                failed = true;
                return;
            }
            // A file rewritten while it was read gets another time: its digest is not kept.
            if (fileTimeOf(job.file) == job.time) {
                cache.store(job.file, job.size, job.time, digest.value());
            }
            job.sha256 = std::move(digest.value());
        }
    };
    std::atomic<u32> finished{0};
    std::vector<std::thread> pool;
    for (u32 i = 0; i < threads; ++i) {
        pool.emplace_back([&]() {
            worker();
            finished.fetch_add(1);
        });
    }
    // Progress from this thread, about four times a second.
    for (u32 tick = 0; finished.load() < threads; ++tick) {
        if (tick % 5 == 0) {
            const u64 done = doneBytes.load(std::memory_order_relaxed);
            operation.report("hash",
                             totalBytes
                                 ? static_cast<f64>(std::min(done, totalBytes)) / static_cast<f64>(totalBytes)
                                 : 0.0,
                             label);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (std::thread& t : pool) {
        t.join();
    }
    static_cast<void>(cache.save()); // a cache that cannot be written only costs time next run
    if (operation.cancelled()) {
        return Error{"cancelled"};
    }
    if (failed) {
        return Error{firstError};
    }
    operation.report("hash", 1.0, label);
    return {};
}

} // namespace vd::setup
