#include "app/GameAudio.h"

#include <imgui.h>

#include <cstdio>

namespace vd {

// --- Development panel -------------------------------------------------------------------------------------------

void GameAudio::drawPanel() {
    if (!m_audio) {
        return;
    }
    const f32 font = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(font * 40.0f, font * 11.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sound")) {
        ImGui::End();
        return;
    }
    if (!m_audio->active()) {
        ImGui::TextUnformatted("Audio off (--audio null|wav:<file>|device)");
        ImGui::End();
        return;
    }
    const audio::AudioStats& s = m_audio->stats();
    ImGui::Text("voices %u/%u (mixer %u) | %u Hz | audio thread load %.1f%%", s.voices,
                audio::Mixer::kMaxVoices, s.mixerVoices, m_audio->outputRate(), s.audioLoad * 100.0f);
    ImGui::Text("started %u, stolen %u, dropped %u, out of range %u, missing %u", s.started, s.stolen,
                s.dropped, s.culled, s.missing);
    ImGui::Text("main thread: GameAudio %.3f ms (worst %.3f), AudioSystem %.3f ms", m_stats.updateMs,
                m_stats.worstMs, s.updateMs);
    static constexpr const char* kKinds[] = {"-", "zone", "intro", "server", "login"};
    ImGui::Text("music: %s, SoundEntries %u, set %u | ambience %u | area %u%s", kKinds[static_cast<u32>(m_musicKind)],
                m_musicEntry, m_zoneSet, m_ambienceEntry, m_area, m_night ? " (night)" : "");
    if (m_musicKind == MusicKind::None && m_zoneSet != 0) {
        ImGui::Text("silence: %.0f s more", static_cast<f64>(static_cast<i32>(m_silenceUntilMs - m_nowMs)) / 1000.0);
    }
    // Phase 14: the models' sounds, the room's music and ambience, the output stage.
    ImGui::Text("models: %u doodads, %u loops, %u events, %u footsteps | %.3f ms | room %s", m_modelStats.doodads,
                m_modelStats.loops, m_modelStats.events, m_modelStats.footsteps, m_modelStats.updateMs,
                m_interiorKey != 0 ? "yes" : "no");
    ImGui::Text("output: gain x%.1f, limiter %.1f dB", m_audio->outputGain(), s.limiterDb);
    if (ImGui::Button("Next track")) {
        m_audio->stop(m_music, 1.0f);
        m_silenceUntilMs = m_nowMs + 1000;
    }
    ImGui::SameLine();
    if (ImGui::Button("Level up (test)")) {
        playUi(UiSound::LevelUp);
    }
    ImGui::Separator();
    for (u32 i = 0; i < m_recent.size(); ++i) {
        const Recent& r = m_recent[(m_recentNext + m_recent.size() - 1 - i) % m_recent.size()];
        if (r.atMs == 0 && r.entry == 0) {
            continue;
        }
        ImGui::Text("%6.1f s  %s (%u)", static_cast<f64>(static_cast<i32>(m_nowMs - r.atMs)) / 1000.0, r.what.data(),
                    r.entry);
    }
    ImGui::End();
}

} // namespace vd
