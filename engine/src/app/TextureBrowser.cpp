#include "app/TextureBrowser.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "pack/PackReader.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vd {
namespace {

const char* formatName(u16 format) {
    switch (static_cast<pack::TextureFormat>(format)) {
    case pack::TextureFormat::Rgba8: return "RGBA8";
    case pack::TextureFormat::Astc4x4: return "ASTC 4x4";
    case pack::TextureFormat::Astc5x5: return "ASTC 5x5";
    case pack::TextureFormat::Astc6x6: return "ASTC 6x6";
    case pack::TextureFormat::Astc8x8: return "ASTC 8x8";
    case pack::TextureFormat::R8: return "R8";
    }
    return "?";
}

} // namespace

void TextureBrowser::load(Renderer& renderer, const pack::PackReader& pack, int index) {
    if (m_texture.valid()) {
        renderer.destroyTexture(m_texture);
        m_texture = {};
    }
    m_loadedIndex = index;
    m_error.clear();
    const pack::PackAsset asset = pack.entry(static_cast<u32>(index));
    m_name.assign(asset.name.data(), asset.name.size());
    if (asset.type != pack::AssetType::Texture || asset.size < sizeof(pack::TextureHeader)) {
        m_error = "Not a texture";
        return;
    }
    pack::TextureHeader header;
    std::memcpy(&header, asset.data, sizeof(header));
    m_format = header.format;
    m_flags = header.flags;
    Result<TextureHandle> texture = renderer.createTexture(asset.data, asset.size, &m_info);
    if (!texture) {
        m_error = texture.error().message;
        return;
    }
    m_texture = texture.value();
}

void TextureBrowser::runBenchmark(Renderer& renderer, const pack::PackReader& pack) {
    constexpr u32 kBenchmarkSamples = 200;
    const u32 count = pack.entryCount();
    const u32 samples = std::min(kBenchmarkSamples, count);
    m_benchmark.clear();
    for (const char* pass : {"cold", "warm"}) {
        std::vector<f32> times;
        times.reserve(samples);
        u64 bytes = 0;
        for (u32 i = 0; i < samples; ++i) {
            const pack::PackAsset asset = pack.entry(static_cast<u32>(u64{i} * count / samples));
            if (asset.type != pack::AssetType::Texture) {
                continue;
            }
            const Clock::time_point start = Clock::now();
            Result<TextureHandle> texture = renderer.createTexture(asset.data, asset.size, nullptr, false);
            const f32 ms = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0);
            if (texture) {
                times.push_back(ms);
                bytes += asset.size;
                renderer.destroyTexture(texture.value());
            }
        }
        if (times.empty()) {
            continue;
        }
        std::sort(times.begin(), times.end());
        f64 total = 0.0;
        for (f32 t : times) {
            total += t;
        }
        auto percentile = [&](f64 p) { return times[std::min(times.size() - 1, static_cast<usize>(p * times.size()))]; };
        char line[192];
        std::snprintf(line, sizeof(line), "%s: n=%zu mean %.2f ms  p50 %.2f  p95 %.2f  max %.2f  (%.1f MB)\n", pass,
                      times.size(), total / static_cast<f64>(times.size()), percentile(0.5), percentile(0.95),
                      times.back(), static_cast<f64>(bytes) / 1e6);
        m_benchmark += line;
        VD_LOG_INFO("Texture load benchmark %s", line);
    }
}

void TextureBrowser::draw(Renderer& renderer, const pack::PackReader& pack) {
    const int count = static_cast<int>(pack.entryCount());
    if (count == 0) {
        return;
    }
    const f32 font = ImGui::GetFontSize();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - font, viewport->WorkPos.y + font),
                            ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(font * 22.0f, font * 28.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Pack textures")) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("##index", &m_index, 0, count - 1, "%d");
        const struct {
            const char* label;
            int step;
        } buttons[] = {{"<<", -100}, {"<", -1}, {">", 1}, {">>", 100}};
        for (int i = 0; i < 4; ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }
            if (ImGui::Button(buttons[i].label, ImVec2(font * 3.0f, font * 2.0f))) {
                m_index = std::clamp(m_index + buttons[i].step, 0, count - 1);
            }
        }
        if (m_index != m_loadedIndex) {
            load(renderer, pack, m_index);
        }
        if (ImGui::Button("Load benchmark (200)", ImVec2(-1.0f, font * 2.0f))) {
            runBenchmark(renderer, pack);
        }
        if (!m_benchmark.empty()) {
            ImGui::TextUnformatted(m_benchmark.c_str());
        }

        ImGui::TextWrapped("%s", m_name.c_str());
        if (!m_error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_error.c_str());
        } else {
            ImGui::Text("%ux%u  %u mips  %s%s%s", m_info.width, m_info.height, m_info.mipCount, formatName(m_format),
                        (m_flags & pack::kTextureHasAlpha) ? "  alpha" : "",
                        (m_flags & pack::kTextureAlphaCutout) ? " (cutout)" : "");
            ImGui::Text("%.0f KB on the GPU, load %.2f ms (%s)", static_cast<double>(m_info.gpuBytes) / 1024.0,
                        m_info.uploadMs, m_info.nativeAstc ? "native ASTC" : "decoded on the CPU");
            if (const u64 id = renderer.debugUiTextureId(m_texture); id != 0 && m_info.width > 0) {
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                const f32 scale = std::min(avail.x / static_cast<f32>(m_info.width),
                                           avail.y / static_cast<f32>(m_info.height));
                ImGui::Image(static_cast<ImTextureID>(id),
                             ImVec2(static_cast<f32>(m_info.width) * scale, static_cast<f32>(m_info.height) * scale));
            }
        }
    }
    ImGui::End();
}

void TextureBrowser::release(Renderer& renderer) {
    if (m_texture.valid()) {
        renderer.destroyTexture(m_texture);
        m_texture = {};
    }
    m_loadedIndex = -1;
}

} // namespace vd
