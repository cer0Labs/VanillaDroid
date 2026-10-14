#pragma once

#include "formats/MpqArchiveSet.h"

#include "TestData.h"

#include <memory>

namespace vd::test {

// Opening the 14 client archives costs seconds (StormLib loads their listfiles), so all tests share
// one read-only instance. nullptr when the client is not available.
inline const formats::MpqArchiveSet* sharedMpq() {
    static const std::unique_ptr<formats::MpqArchiveSet> mpq = []() -> std::unique_ptr<formats::MpqArchiveSet> {
        const auto dataDir = wowDataDir();
        if (!dataDir) {
            return nullptr;
        }
        auto set = std::make_unique<formats::MpqArchiveSet>();
        return set->open(*dataDir).ok() ? std::move(set) : nullptr;
    }();
    return mpq.get();
}

} // namespace vd::test

#define VD_REQUIRE_MPQ(var)                                                                              \
    const ::vd::formats::MpqArchiveSet* var = ::vd::test::sharedMpq();                                   \
    if (!var) {                                                                                          \
        MESSAGE("VD_WOW_DIR not set or missing: skipping test that needs the original client");         \
        return;                                                                                          \
    }
