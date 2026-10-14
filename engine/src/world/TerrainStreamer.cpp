#include "world/TerrainStreamer.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "platform/Thread.h"

#include <cstring>

namespace vd {

void TerrainStreamer::start(const std::array<u8*, kSlots>& staging, usize capacity) {
    if (m_thread.joinable()) {
        return;
    }
    m_stop = false;
    m_staging = staging;
    m_capacity = capacity;
    for (std::vector<u8>& heap : m_heap) {
        heap.resize(capacity);
    }
    m_freeSlots.clear();
    for (u32 slot = 0; slot < kSlots; ++slot) {
        m_freeSlots.push_back(kSlots - 1 - slot);
    }
    m_thread = std::thread([this] { run(); });
}

void TerrainStreamer::stop() {
    if (!m_thread.joinable()) {
        return;
    }
    m_stop = true;
    m_wake.release();
    m_thread.join();
    // Drop whatever was queued or finished: the caller is closing the map.
    Request request;
    while (m_requests.pop(request)) {
    }
    Done done;
    while (m_done.pop(done)) {
    }
    m_freeSlots.clear();
}

bool TerrainStreamer::request(u16 tx, u16 ty, const pack::PackAsset& asset) {
    if (m_freeSlots.empty()) {
        return false;
    }
    const u32 slot = m_freeSlots.back();
    if (!m_requests.push({tx, ty, slot, asset})) {
        return false;
    }
    m_freeSlots.pop_back();
    m_wake.release();
    return true;
}

bool TerrainStreamer::poll(Done& out) {
    return m_done.pop(out);
}

void TerrainStreamer::release(u32 slot) {
    m_freeSlots.push_back(slot);
}

void TerrainStreamer::run() {
    setCurrentThreadName("vd-io");
    for (;;) {
        m_wake.acquire();
        if (m_stop.load()) {
            return;
        }
        Request request;
        while (!m_stop.load() && m_requests.pop(request)) {
            VD_PROFILE_ZONE("TerrainStreamer::load");
            const Clock::time_point start = Clock::now();
            u8* heap = m_heap[request.slot].data();
            const Result<void> read = pack::PackReader::read(request.asset, heap, m_capacity);
            if (read) {
                std::memcpy(m_staging[request.slot], heap, request.asset.rawSize); // sequential writes only
            } else {
                VD_LOG_ERROR("Terrain tile %u_%u: %s", request.tx, request.ty, read.error().message.c_str());
            }
            m_sizes[request.slot] = request.asset.rawSize;
            Done done;
            done.tx = request.tx;
            done.ty = request.ty;
            done.slot = request.slot;
            done.ok = read.ok();
            done.ioMs = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
            m_done.push(done); // capacity 8 > kSlots: never full
        }
    }
}

} // namespace vd
