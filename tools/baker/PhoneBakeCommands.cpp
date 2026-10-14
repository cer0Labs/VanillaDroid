// Phase 22 (phonebake, ROADMAP 22.7, ADR-0005 §8): the phone bakes each step into a staging folder next to
// packs/; `vd_baker install` then moves the step's packs into the packs folder and merges their recipes into
// packs/manifest.json (pack/PackInstall.h), as the PC's bake leaves it.
#include "BakerCommon.h"

#include "core/Log.h"
#include "pack/PackInstall.h"

namespace vd::baker {

int commandInstall(const Options& options) {
    const std::string staging = option(options, "staging");
    const std::string packs = option(options, "packs");
    if (staging.empty() || packs.empty()) {
        VD_LOG_ERROR("install needs --staging <dir> and --packs <dir>");
        return 1;
    }
    Result<pack::InstallResult> installed = pack::installStagedPacks(staging, packs);
    if (!installed) {
        VD_LOG_ERROR("%s", installed.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("install: %u packs, %.1f MB -> %s", installed.value().packs,
                static_cast<f64>(installed.value().bytes) / 1e6, packs.c_str());
    return 0;
}

} // namespace vd::baker
