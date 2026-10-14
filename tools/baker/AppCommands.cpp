// app.vdpk (phase 22, ADR-0005 §2): what is ours, baked without the WoW client (no MPQ is opened): our texts
// (locale/), our art (art/touch) and our font (art/fonts). See pack/AppPack.h and
// tools/formats/src/bake/AppPackBaker.

#include "BakerCommon.h"

#include "bake/AppPackBaker.h"
#include "core/Clock.h"
#include "core/Log.h"

namespace vd::baker {

int bakeApp(const std::filesystem::path& outPath, const std::filesystem::path& localeDir,
            const std::filesystem::path& artDir) {
    const Clock::time_point start = Clock::now();
    Result<bake::AppPackStats> baked = bake::bakeAppPack(outPath, localeDir, artDir, kBakerVersion);
    if (!baked) {
        VD_LOG_ERROR("app: %s", baked.error().message.c_str());
        return 1;
    }
    const bake::AppPackStats& stats = baked.value();
    VD_LOG_INFO(
        "app: texts in %zu languages (%zu), %zu sprites in %zu atlas pages, %zu glyphs in %zu font layers, "
        "%.1f KB, %u failed, %.1f s -> %s",
        stats.languages, stats.texts, stats.sprites, stats.atlasPages, stats.glyphs, stats.fontLayers,
        static_cast<double>(stats.bytes) / 1e3, stats.failures, secondsBetween(start, Clock::now()),
        outPath.string().c_str());
    return stats.failures == 0 ? 0 : 1;
}

} // namespace vd::baker
