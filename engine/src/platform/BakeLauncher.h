#pragma once

#include "platform/PlatformInfo.h"

#include <filesystem>
#include <memory>

namespace vd {

// Phase 22 (phonebake, ROADMAP 22.7): what runs a bake plan (<bakeDir>/plan.json, app/PhoneBakePlan.h)
// outside the game. Android: the foreground service BakeService in the process :baker (MainActivity.startBake
// / cancelBake, by JNI); the desktop: PhoneBakeRunner on a thread of its own, running vd_baker.exe the same
// way (to try the screens with --packs <an empty folder>). Called from one thread only (PhoneBake's vd-bake):
// never the game's.
class BakeLauncher {
public:
    virtual ~BakeLauncher() = default;
    // Runs the plan, or goes on with it from its last finished step; false when the request could not be
    // made.
    virtual bool start(const std::filesystem::path& bakeDir) = 0;
    // Stops it (what is finished stays); its status becomes "cancelled".
    virtual void cancel(const std::filesystem::path& bakeDir) = 0;
    // The calling thread is about to end (Android: it leaves the Java VM).
    virtual void threadEnds() {}
};

// Android: the service; elsewhere null.
std::unique_ptr<BakeLauncher> makeAndroidBakeLauncher(const PlatformInfo& platform);

} // namespace vd
