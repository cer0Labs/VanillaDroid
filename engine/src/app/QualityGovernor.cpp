#include "app/QualityGovernor.h"

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

// Phase 10 presets. High is what every phase was measured with (60 fps on the Mi 11X); Ultra sees further for the
// Fold5's headroom; Medium and Low trade the world's resolution and reach for battery and heat.
constexpr QualitySettings kPresets[kQualityPresets] = {
    {0.70f, 400.0f, 0.6f, 0.4f},  // Low
    {0.85f, 550.0f, 0.8f, 0.7f},  // Medium
    {1.00f, 700.0f, 1.0f, 1.0f},  // High
    {1.00f, 1000.0f, 1.3f, 1.0f}, // Ultra
};

// What each thermal level (0..4) leaves of the player's settings: the render scale's ceiling, the distances and the
// particles. Every level trims all three a little: on the Fold5 the GPU's cost is the geometry (draws, triangles,
// binning), not the pixels (half the resolution left its clocks where they were), while on a GPU short of fill rate
// the resolution is what helps.
constexpr f32 kLevelScale[5] = {1.0f, 0.9f, 0.8f, 0.7f, 0.6f};
constexpr f32 kLevelDistance[5] = {1.0f, 0.9f, 0.8f, 0.7f, 0.6f};
constexpr f32 kLevelEffects[5] = {1.0f, 0.85f, 0.7f, 0.5f, 0.35f};
// And each load level (0..3), against frames missed with the CPU as the bottleneck.
constexpr f32 kLoadDistance[QualityGovernor::kMaxLoadLevel + 1] = {1.0f, 0.85f, 0.7f, 0.55f};
constexpr f32 kLoadEffects[QualityGovernor::kMaxLoadLevel + 1] = {1.0f, 0.75f, 0.5f, 0.35f};

constexpr i32 kStatusCritical = 4; // ATHERMAL_STATUS_CRITICAL: the platform is about to shut things down
constexpr f32 kWindowSeconds = 1.0f;
constexpr f32 kProbeBlameSeconds = 3.0f;    // misses this soon after a step up are its fault
constexpr f32 kProbeSuccessSeconds = 30.0f; // a step up that held this long resets the wait between tries

} // namespace

QualitySettings qualityPreset(QualityPreset preset) {
    const u32 index = static_cast<u32>(preset);
    return index < kQualityPresets ? kPresets[index] : kPresets[static_cast<u32>(QualityPreset::High)];
}

QualityPreset matchingPreset(const QualitySettings& s) {
    for (u32 i = 0; i < kQualityPresets; ++i) {
        const QualitySettings& p = kPresets[i];
        if (std::fabs(s.renderScale - p.renderScale) < 0.005f && std::fabs(s.viewDistance - p.viewDistance) < 1.0f &&
            std::fabs(s.environmentDetail - p.environmentDetail) < 0.005f &&
            std::fabs(s.spellDetail - p.spellDetail) < 0.005f) {
            return static_cast<QualityPreset>(i);
        }
    }
    return QualityPreset::Custom;
}

const char* qualityPresetKey(QualityPreset preset) {
    switch (preset) {
    case QualityPreset::Low: return "LOW"; // the client's own words where it has them
    case QualityPreset::Medium: return "VD_QUALITY_MEDIUM";
    case QualityPreset::High: return "HIGH";
    case QualityPreset::Ultra: return "VD_QUALITY_ULTRA";
    case QualityPreset::Custom: break;
    }
    return "VD_QUALITY_CUSTOM";
}

void QualityGovernor::configure(const Config& config) {
    const bool changed = config.dynamic != m_config.dynamic || config.baseScale != m_config.baseScale ||
                         config.minScale != m_config.minScale;
    m_config = config;
    m_config.baseScale = std::clamp(m_config.baseScale, m_config.minScale, 1.0f);
    if (changed) {
        // A new ceiling: the load controller starts from it (the thermal level is the device's, it stays).
        m_dynamicScale = m_config.baseScale;
        m_loadLevel = 0;
        m_probeDelay = kProbeSeconds;
        m_sinceProbe = -1.0f;
        m_calmSeconds = 0.0f;
    }
    publish();
}

void QualityGovernor::reset() {
    m_dynamicScale = m_config.baseScale;
    m_loadLevel = 0;
    m_probeDelay = kProbeSeconds;
    m_sinceProbe = -1.0f;
    m_calmSeconds = 0.0f;
    m_windowSeconds = 0.0f;
    m_windowFrames = 0;
    m_windowMisses = 0;
    m_windowGpu = 0.0;
    m_windowCpu = 0.0;
    publish();
}

u32 QualityGovernor::thermalTarget(const GovernorInput& input) const {
    u32 level = 0;
    if (!std::isnan(input.thermalHeadroom)) {
        // A level already held lasts until the headroom falls clearly below where it began.
        for (u32 i = 0; i < 4; ++i) {
            const f32 threshold = kLevelHeadroom[i] - (m_thermalLevel > i ? kLevelExitMargin : 0.0f);
            if (input.thermalHeadroom >= threshold) {
                level = i + 1;
            }
        }
    }
    // The system's own verdict counts as well (the only one where the headroom is unknown): light, moderate,
    // severe, critical and beyond.
    if (input.thermalStatus > 0) {
        level = std::max(level, static_cast<u32>(std::min(input.thermalStatus, 4)));
    }
    return level;
}

void QualityGovernor::updateThermal(const GovernorInput& input) {
    const u32 target = thermalTarget(input);
    if (target > m_thermalLevel) {
        m_levelTimer = std::max(m_levelTimer, 0.0f) + input.dt;
        if (m_levelTimer >= kLevelUpSeconds) {
            ++m_thermalLevel;
            ++m_levelChanges;
            m_levelTimer = 0.0f;
        }
    } else if (target < m_thermalLevel) {
        m_levelTimer = std::min(m_levelTimer, 0.0f) - input.dt;
        if (-m_levelTimer >= kLevelDownSeconds) {
            --m_thermalLevel;
            ++m_levelChanges;
            m_levelTimer = 0.0f;
        }
    } else {
        m_levelTimer = 0.0f;
    }
}

void QualityGovernor::update(const GovernorInput& input) {
    updateThermal(input);
    m_critical = input.thermalStatus >= kStatusCritical;
    if (!input.measuring || input.dt <= 0.0f) {
        // Loading: the frames say nothing about the steady cost, and the window starts over afterwards.
        m_windowSeconds = 0.0f;
        m_windowFrames = 0;
        m_windowMisses = 0;
        m_windowGpu = 0.0;
        m_windowCpu = 0.0;
        m_calmSeconds = 0.0f;
        publish();
        return;
    }
    m_windowSeconds += input.dt;
    ++m_windowFrames;
    m_windowMisses += input.frameMs > input.targetMs * kMissFactor ? 1u : 0u;
    m_windowGpu += input.gpuMs;
    m_windowCpu += input.cpuMs;
    m_windowTarget = input.targetMs;
    if (m_windowSeconds >= kWindowSeconds * 0.999f) { // whole seconds of frames, despite the rounding
        closeWindow();
    }
    publish();
}

void QualityGovernor::closeWindow() {
    const f32 seconds = m_windowSeconds;
    const u32 allowed = std::max(2u, static_cast<u32>(std::ceil(static_cast<f32>(m_windowFrames) * kMissRatio)));
    const bool missing = m_config.dynamic && m_windowFrames > 0 && m_windowMisses >= allowed;
    const f32 ceiling = std::max(m_config.minScale, m_config.baseScale * kLevelScale[m_thermalLevel]);
    if (m_sinceProbe >= 0.0f) {
        m_sinceProbe += seconds;
    }
    if (missing) {
        m_calmSeconds = 0.0f;
        if (m_sinceProbe >= 0.0f && m_sinceProbe <= kProbeBlameSeconds + seconds) {
            m_probeDelay = std::min(m_probeDelay * 2.0f, kMaxProbeSeconds); // that step up was one too many
        }
        m_sinceProbe = -1.0f;
        // Only a GPU or a CPU that really is near the frame's end is to blame: misses with both far from it come from
        // outside (the system's interface over the game, a touch on the notifications) and lowering the quality
        // would not help.
        const f32 gpu = static_cast<f32>(m_windowGpu / m_windowFrames);
        const f32 cpu = static_cast<f32>(m_windowCpu / m_windowFrames);
        const f32 busy = kBusyShare * m_windowTarget;
        const f32 scale = std::min(m_dynamicScale, ceiling);
        const bool canScale = scale > m_config.minScale + 0.001f;
        const bool canLoad = m_loadLevel < kMaxLoadLevel;
        if (gpu >= busy && gpu >= cpu && canScale) {
            m_dynamicScale = std::max(m_config.minScale, scale - kScaleStepDown);
            ++m_scaleChanges;
        } else if ((cpu >= busy || gpu >= busy) && canLoad) {
            ++m_loadLevel;
        } else if ((cpu >= busy || gpu >= busy) && canScale) {
            m_dynamicScale = std::max(m_config.minScale, scale - kScaleStepDown);
            ++m_scaleChanges;
        }
    } else if (m_config.dynamic) {
        m_calmSeconds += seconds;
        if (m_sinceProbe >= kProbeSuccessSeconds) {
            m_probeDelay = kProbeSeconds; // it held: the next try comes as soon as the first
            m_sinceProbe = -1.0f;
        }
        if (m_calmSeconds >= m_probeDelay) {
            // A step back up: the resolution first, then the CPU's work.
            if (m_dynamicScale < ceiling - 0.001f) {
                m_dynamicScale = std::min(ceiling, std::min(m_dynamicScale, ceiling) + kScaleStepUp);
                ++m_scaleChanges;
                m_sinceProbe = 0.0f;
                m_calmSeconds = 0.0f;
            } else if (m_loadLevel > 0) {
                --m_loadLevel;
                m_sinceProbe = 0.0f;
                m_calmSeconds = 0.0f;
            }
        }
    }
    m_windowSeconds = 0.0f;
    m_windowFrames = 0;
    m_windowMisses = 0;
    m_windowGpu = 0.0;
    m_windowCpu = 0.0;
}

void QualityGovernor::publish() {
    m_output.thermalLevel = m_thermalLevel;
    m_output.loadLevel = m_loadLevel;
    if (!m_config.dynamic) {
        m_output.renderScale = m_config.baseScale;
        m_output.distanceScale = 1.0f;
        m_output.effectScale = 1.0f;
        m_output.frameInterval = 1;
        return;
    }
    const f32 ceiling = std::max(m_config.minScale, m_config.baseScale * kLevelScale[m_thermalLevel]);
    m_output.renderScale = std::round(std::min(ceiling, m_dynamicScale) * 100.0f) / 100.0f;
    m_output.distanceScale = kLevelDistance[m_thermalLevel] * kLoadDistance[m_loadLevel];
    m_output.effectScale = kLevelEffects[m_thermalLevel] * kLoadEffects[m_loadLevel];
    m_output.frameInterval = m_critical ? 2u : 1u;
}

} // namespace vd
