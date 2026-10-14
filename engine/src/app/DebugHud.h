#pragma once

#include "core/Types.h"

#include <array>

namespace vd {

struct RenderStats;

// Development overlay: frame pacing, CPU/GPU cost and render info, drawn with Dear ImGui.
// draw() must be called between Renderer::beginDebugUi() and Renderer::renderFrame().
class DebugHud {
public:
    void addFrame(f32 frameMs, f32 cpuMs, f32 gpuMs);
    void draw(const RenderStats& render, u32 workerCount, u32 dbTables, u32 textures) const;
    // Frames per second over the recent frames (the touch HUD's counter).
    f32 averageFps() const { return m_avgFrameMs > 0.0f ? 1000.0f / m_avgFrameMs : 0.0f; }

private:
    static constexpr u32 kHistory = 120; // 2 s at 60 fps

    std::array<f32, kHistory> m_frameMs{};
    u32 m_head = 0; // index of the oldest sample
    f32 m_avgFrameMs = 0.0f;
    f32 m_avgCpuMs = 0.0f;
    f32 m_avgGpuMs = 0.0f;
};

} // namespace vd
