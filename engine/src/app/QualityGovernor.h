#pragma once

#include "core/Types.h"

#include <limits>

namespace vd {

// --- Quality profiles (phase 10) ------------------------------------------------------------------------------------

// The Video Options' "graphics quality": each preset sets the four knobs below; moving one of them on its own makes
// the choice Custom.
enum class QualityPreset : u32 { Low, Medium, High, Ultra, Custom };
constexpr u32 kQualityPresets = 4; // Custom is not one

struct QualitySettings {
    f32 renderScale = 1.0f;       // 0.5..1: the world's share of the screen's resolution (the UI keeps all of it)
    f32 viewDistance = 700.0f;    // yards (FARCLIP)
    f32 environmentDetail = 1.0f; // 0.5..1.5: doodad draw distances times this (ENVIRONMENT_DETAIL)
    f32 spellDetail = 1.0f;       // 0.25..1: particle emission times this (SPELL_DETAIL)
};

QualitySettings qualityPreset(QualityPreset preset);
// The preset whose values these are (within a slider step), or Custom.
QualityPreset matchingPreset(const QualitySettings& settings);
// The text key of a preset's name.
const char* qualityPresetKey(QualityPreset preset);

// --- Dynamic quality (phase 10) -------------------------------------------------------------------------------------

// What the governor reads each frame. Thermal values come from Android's ADPF thermal API (platform/DeviceMonitor):
// headroom is AThermal_getThermalHeadroom's forecast (1.0 = severe throttling), status AThermal_getCurrentThermalStatus
// (0 none .. 6 shutdown); NaN / -1 where the device gives none.
struct GovernorInput {
    f32 dt = 0.0f;        // seconds since the last frame
    f32 frameMs = 0.0f;   // present to present
    f32 cpuMs = 0.0f;     // the game thread's work, without the waits on the GPU and the display
    f32 gpuMs = 0.0f;     // the GPU's, last completed frame
    f32 targetMs = 16.667f; // the frame the player asked for (33.3 with the 30 fps limit)
    f32 thermalHeadroom = std::numeric_limits<f32>::quiet_NaN();
    i32 thermalStatus = -1;
    bool measuring = true; // false while loading (a loading screen, a teleport): frames then say nothing
};

// What the frame uses: the render scale as is, the rest as factors over the player's settings.
struct GovernorOutput {
    f32 renderScale = 1.0f;
    f32 distanceScale = 1.0f; // view and doodad distances
    f32 effectScale = 1.0f;   // particle emission
    u32 frameInterval = 1;    // 2 = 30 fps: only at a critical temperature
    u32 thermalLevel = 0;     // 0 (cool) .. 4
    u32 loadLevel = 0;        // 0 .. 3: steps taken against frames missed with the CPU as the bottleneck
};

// Keeps 60 fps for a whole session on a phone that heats up (ADPF: the temperature and the missed frames decide):
//  - Temperature: the closer the thermal headroom gets to throttling (1.0), the lower the ceiling of the render
//    scale, the distances and the effects, before the system starts cutting clocks. Up a level after 2 s above its
//    threshold, down one after 30 s clearly below it.
//  - Missed frames (> 5 % of a second's frames over 1.25 x the target): with the GPU as the bottleneck (busy 75 % of
//    the frame or more) the render scale drops 0.1; with the CPU, the distances and the effects drop a step; with
//    both well below, the misses came from outside the game and nothing changes. Without misses for a while it tries a step
//    back up; a step that brings misses back makes it wait twice as long before the next try (up to 2 min).
// Never above the player's settings. Off (`dynamic` false): the settings as they are.
class QualityGovernor {
public:
    struct Config {
        bool dynamic = true;
        f32 baseScale = 1.0f; // the player's render scale: the ceiling
        f32 minScale = 0.5f;
    };

    // Thermal headroom where each level (1..4) begins, and how far below it a level ends.
    static constexpr f32 kLevelHeadroom[4] = {0.75f, 0.85f, 0.95f, 1.05f};
    static constexpr f32 kLevelExitMargin = 0.07f;
    static constexpr f32 kLevelUpSeconds = 2.0f;
    static constexpr f32 kLevelDownSeconds = 30.0f;
    static constexpr f32 kMissFactor = 1.25f; // a frame this many targets long missed its vsync
    static constexpr f32 kMissRatio = 0.05f;
    static constexpr f32 kBusyShare = 0.75f; // of the frame: a GPU or CPU this busy is the bottleneck
    static constexpr f32 kScaleStepDown = 0.1f;
    static constexpr f32 kScaleStepUp = 0.05f;
    static constexpr f32 kProbeSeconds = 10.0f;
    static constexpr f32 kMaxProbeSeconds = 120.0f;
    static constexpr u32 kMaxLoadLevel = 3;

    void configure(const Config& config);
    void update(const GovernorInput& input);
    const GovernorOutput& output() const { return m_output; }
    void reset(); // back to the settings (a new map, the governor switched on)

    // For the logs and the soak benchmark: changes made since the start.
    u32 scaleChanges() const { return m_scaleChanges; }
    u32 levelChanges() const { return m_levelChanges; }

private:
    u32 thermalTarget(const GovernorInput& input) const;
    void updateThermal(const GovernorInput& input);
    void closeWindow();
    void publish();

    Config m_config;
    GovernorOutput m_output;
    f32 m_dynamicScale = 1.0f; // the load controller's scale, under the thermal ceiling
    u32 m_thermalLevel = 0;
    u32 m_loadLevel = 0;
    f32 m_levelTimer = 0.0f; // seconds the thermal target has stood above / below the level
    // The current one-second window.
    f32 m_windowSeconds = 0.0f;
    u32 m_windowFrames = 0;
    u32 m_windowMisses = 0;
    f64 m_windowGpu = 0.0;
    f64 m_windowCpu = 0.0;
    f32 m_windowTarget = 16.667f;
    // Probing back up.
    f32 m_calmSeconds = 0.0f;  // since the last window with misses (or the last step up)
    f32 m_probeDelay = kProbeSeconds;
    f32 m_sinceProbe = -1.0f;  // seconds since the last step up still on trial; < 0: none
    bool m_critical = false;   // the thermal status is critical or worse
    u32 m_scaleChanges = 0;
    u32 m_levelChanges = 0;
};

} // namespace vd
