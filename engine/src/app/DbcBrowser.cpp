#include "app/DbcBrowser.h"

#include "db/DbcView.h"
#include "pack/PackReader.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vd {
namespace {

constexpr int kVisibleRows = 40;
constexpr u32 kMaxColumns = 24;

// Integers in client tables are small (IDs, flags, counts); large values with a sane exponent are floats.
bool looksLikeFloat(u32 bits) {
    if (bits < 0x01000000u) {
        return false;
    }
    f32 value;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    const f32 magnitude = std::fabs(value);
    return std::isfinite(value) && magnitude > 1e-5f && magnitude < 1e7f;
}

void formatCell(const db::DbcView& table, u32 row, u32 field, int mode, char* out, usize size) {
    const u32 bits = table.getU32(row, field);
    if (mode == 1 || (mode == 0 && field == 0)) { // field 0 is always the record ID
        std::snprintf(out, size, "%u", bits);
    } else if (mode == 2 || (mode == 0 && looksLikeFloat(bits))) {
        std::snprintf(out, size, "%.7g", static_cast<double>(table.getF32(row, field)));
    } else if (mode == 0 && table.looksLikeString(row, field)) {
        const std::string_view text = table.getString(row, field);
        std::snprintf(out, size, "\"%.*s\"", static_cast<int>(std::min<usize>(text.size(), 40)), text.data());
    } else {
        std::snprintf(out, size, "%d", static_cast<i32>(bits));
    }
}

} // namespace

void DbcBrowser::draw(const pack::PackReader& dbPack) {
    const int tables = static_cast<int>(dbPack.entryCount());
    if (tables == 0) {
        return;
    }
    const f32 font = ImGui::GetFontSize();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Anchored by its top edge (the window starts collapsed, so a bottom anchor would be computed from
    // the collapsed height and the expanded window would run off-screen).
    const ImVec2 size(std::min(font * 30.0f, viewport->WorkSize.x * 0.45f), viewport->WorkSize.y * 0.55f - font);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + font, viewport->WorkPos.y + viewport->WorkSize.y - size.y - font),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("DBC tables")) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("##table", &m_table, 0, tables - 1, "%d");
        const pack::PackAsset asset = dbPack.entry(static_cast<u32>(m_table));
        ImGui::TextUnformatted(asset.name.data(), asset.name.data() + asset.name.size());

        Result<db::DbcView> parsed = db::DbcView::parse(asset.data, asset.size);
        if (!parsed) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", parsed.error().message.c_str());
        } else {
            const db::DbcView& table = parsed.value();
            ImGui::Text("%u records x %u fields (%u B/record), %u B of text", table.recordCount(),
                        table.fieldCount(), table.recordSize(), table.stringBlockSize());
            ImGui::RadioButton("auto", &m_mode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("integers", &m_mode, 1);
            ImGui::SameLine();
            ImGui::RadioButton("floats", &m_mode, 2);
            const int rows = static_cast<int>(table.recordCount());
            if (table.hasPackedFields()) {
                // 1-byte fields: show raw bytes per record.
                ImGui::TextUnformatted("1-byte fields:");
                const int lastRow = std::min(rows, kVisibleRows);
                for (int r = 0; r < lastRow; ++r) {
                    char line[256];
                    int length = std::snprintf(line, sizeof(line), "%3d:", r);
                    for (u32 b = 0; b < table.recordSize() && length < 240; ++b) {
                        length += std::snprintf(line + length, sizeof(line) - static_cast<usize>(length), " %3u",
                                                table.getU8(static_cast<u32>(r), b));
                    }
                    ImGui::TextUnformatted(line);
                }
            } else if (rows > 0) {
                m_firstRow = std::clamp(m_firstRow, 0, rows - 1);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderInt("##row", &m_firstRow, 0, rows - 1, "row %d");

                const u32 columns = std::min(table.fieldCount(), kMaxColumns);
                const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_SizingFixedFit;
                if (ImGui::BeginTable("records", static_cast<int>(columns) + 1, flags)) {
                    ImGui::TableSetupScrollFreeze(1, 1);
                    ImGui::TableSetupColumn("#");
                    char label[16];
                    for (u32 c = 0; c < columns; ++c) {
                        std::snprintf(label, sizeof(label), "f%u", c);
                        ImGui::TableSetupColumn(label);
                    }
                    ImGui::TableHeadersRow();
                    const int lastRow = std::min(rows, m_firstRow + kVisibleRows);
                    char cell[64];
                    for (int r = m_firstRow; r < lastRow; ++r) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", r);
                        for (u32 c = 0; c < columns; ++c) {
                            ImGui::TableNextColumn();
                            formatCell(table, static_cast<u32>(r), c, m_mode, cell, sizeof(cell));
                            ImGui::TextUnformatted(cell);
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
    }
    ImGui::End();
}

} // namespace vd
