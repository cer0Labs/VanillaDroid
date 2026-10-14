#include "pack/AsyncReader.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "platform/Thread.h"

#include <new>

namespace vd::pack {

AsyncReader::~AsyncReader() {
    stop();
}

void AsyncReader::start(const char* threadName) {
    if (m_thread.joinable()) {
        return;
    }
    m_threadName = threadName;
    m_stop = false;
    m_thread = std::thread([this] { run(); });
}

void AsyncReader::stop() {
    if (!m_thread.joinable()) {
        return;
    }
    m_stop = true;
    m_wake.release();
    m_thread.join();
    Request request;
    while (m_requests.pop(request)) {
    }
    Done done;
    while (m_done.pop(done)) {
        delete[] done.data;
    }
    m_pending = 0;
}

bool AsyncReader::request(u64 tag, const PackAsset& asset, Validate validate) {
    if (m_pending >= 63 || !m_requests.push({tag, asset, validate})) {
        return false;
    }
    ++m_pending;
    m_wake.release();
    return true;
}

bool AsyncReader::poll(Done& out) {
    if (!m_done.pop(out)) {
        return false;
    }
    --m_pending;
    return true;
}

void AsyncReader::run() {
    setCurrentThreadName(m_threadName);
    for (;;) {
        m_wake.acquire();
        if (m_stop.load()) {
            return;
        }
        Request request;
        while (!m_stop.load() && m_requests.pop(request)) {
            VD_PROFILE_ZONE("AsyncReader::read");
            Done done;
            done.tag = request.tag;
            done.data = new (std::nothrow) u8[request.asset.rawSize];
            if (done.data) {
                const Result<void> read = PackReader::read(request.asset, done.data, request.asset.rawSize);
                if (read && (!request.validate || request.validate(done.data, request.asset.rawSize))) {
                    done.size = request.asset.rawSize;
                } else if (read) {
                    VD_LOG_ERROR("Pack asset %.*s failed validation", static_cast<int>(request.asset.name.size()),
                                 request.asset.name.data());
                    delete[] done.data;
                    done.data = nullptr;
                } else {
                    VD_LOG_ERROR("Pack read (%.*s): %s", static_cast<int>(request.asset.name.size()),
                                 request.asset.name.data(), read.error().message.c_str());
                    delete[] done.data;
                    done.data = nullptr;
                }
            }
            m_done.push(done); // never full: at most 63 in flight
        }
    }
}

} // namespace vd::pack
