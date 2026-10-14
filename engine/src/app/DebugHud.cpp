#include "app/DebugHud.h"

#include "render/Renderer.h"

#include <imgui.h>

#include <algorithm>

namespace vd {
namespace {

constexpr f32 kSmoothing = 0.05f;        // exponential moving average weight (~20 frames)
constexpr f32 kGraphMaxMs = 1000.0f / 30.0f; // graph top = 30 fps; the 60 fps line sits in the middle

f32 smooth(f32 average, f32 sample) {
    return average == 0.0f ? sample : average + (sample - average) * kSmoothing;
}

} // namespace

void DebugHud::addFrame(f32 frameMs, f32 cpuMs, f32 gpuMs) {
    m_frameMs[m_head] = frameMs;
    m_head = (m_head + 1) % kHistory;
    m_avgFrameMs = smooth(m_avgFrameMs, frameMs);
    m_avgCpuMs = smooth(m_avgCpuMs, cpuMs);
    m_avgGpuMs = smooth(m_avgGpuMs, gpuMs);
}

void DebugHud::draw(const RenderStats& render, u32 workerCount, u32 dbTables, u32 textures) const {
    const f32 fontSize = ImGui::GetFontSize();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Top center: the touch UI has the unit frames on the left and the minimap on the right.
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + fontSize),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##debug_hud", nullptr, flags)) {
        const f32 worstMs = *std::max_element(m_frameMs.begin(), m_frameMs.end());
        const f32 fps = m_avgFrameMs > 0.0f ? 1000.0f / m_avgFrameMs : 0.0f;
        ImGui::Text("%.1f fps   frame %.2f ms   worst %.2f ms", fps, m_avgFrameMs, worstMs);
        ImGui::Text("CPU %.2f ms   GPU %.2f ms", m_avgCpuMs, m_avgGpuMs);
        ImGui::Text("%ux%u   rot %u   workers %u", render.width, render.height, render.rotationDegrees,
                    workerCount);
        ImGui::Text("draws %u   tris %.1fk   particles %u", render.drawCalls,
                    static_cast<f32>(render.triangles) / 1000.0f, render.particleQuads);
        if (dbTables == 0 && textures == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "packs: not loaded");
        } else {
            ImGui::Text("packs: %u tables, %u textures", dbTables, textures);
        }
        ImGui::PlotLines("##frame_times", m_frameMs.data(), static_cast<int>(kHistory), static_cast<int>(m_head),
                         nullptr, 0.0f, kGraphMaxMs, ImVec2(fontSize * 18.0f, fontSize * 3.0f));
    }
    ImGui::End();
}

} // namespace vd
