#include "GuiKit.h"

#include "HttpProtocol.h"
#include "core/Text.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace vd::setup::gui {
namespace {

constexpr ImVec4 kPrimary{0.549f, 0.114f, 0.055f, 1.0f};
constexpr ImVec4 kPrimaryHovered{0.659f, 0.149f, 0.102f, 1.0f};
constexpr ImVec4 kPrimaryActive{0.431f, 0.086f, 0.043f, 1.0f};
constexpr ImVec4 kPrimaryBorder{0.831f, 0.627f, 0.090f, 1.0f};

ImVec4 rgb(int r, int g, int b, f32 a = 1.0f) {
    return ImVec4(static_cast<f32>(r) / 255.0f, static_cast<f32>(g) / 255.0f, static_cast<f32>(b) / 255.0f,
                  a);
}

} // namespace

void applyStyle(f32 scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.WindowPadding = ImVec2(18, 16);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.CellPadding = ImVec2(8, 5);
    style.ScrollbarSize = 13;
    style.GrabMinSize = 12;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.WindowRounding = 6;
    style.ChildRounding = 6;
    style.FrameRounding = 4;
    style.PopupRounding = 6;
    style.ScrollbarRounding = 6;
    style.GrabRounding = 4;
    style.TabRounding = 4;
    style.SeparatorTextBorderSize = 2;

    ImVec4* colours = style.Colors;
    colours[ImGuiCol_Text] = kText;
    colours[ImGuiCol_TextDisabled] = rgb(143, 129, 104);
    colours[ImGuiCol_WindowBg] = kBackground;
    colours[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    colours[ImGuiCol_PopupBg] = rgb(34, 27, 19);
    colours[ImGuiCol_Border] = rgb(70, 56, 37);
    colours[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    colours[ImGuiCol_FrameBg] = rgb(42, 33, 23);
    colours[ImGuiCol_FrameBgHovered] = rgb(58, 46, 31);
    colours[ImGuiCol_FrameBgActive] = rgb(74, 58, 38);
    colours[ImGuiCol_TitleBg] = kSidebar;
    colours[ImGuiCol_TitleBgActive] = kSidebar;
    colours[ImGuiCol_TitleBgCollapsed] = kSidebar;
    colours[ImGuiCol_MenuBarBg] = kSidebar;
    colours[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    colours[ImGuiCol_ScrollbarGrab] = rgb(70, 56, 37);
    colours[ImGuiCol_ScrollbarGrabHovered] = rgb(90, 72, 48);
    colours[ImGuiCol_ScrollbarGrabActive] = rgb(110, 88, 58);
    colours[ImGuiCol_CheckMark] = kGold;
    colours[ImGuiCol_SliderGrab] = rgb(212, 160, 23);
    colours[ImGuiCol_SliderGrabActive] = kGold;
    colours[ImGuiCol_Button] = rgb(58, 46, 31);
    colours[ImGuiCol_ButtonHovered] = rgb(77, 61, 40);
    colours[ImGuiCol_ButtonActive] = rgb(94, 74, 48);
    colours[ImGuiCol_Header] = rgb(58, 46, 31);
    colours[ImGuiCol_HeaderHovered] = rgb(77, 61, 40);
    colours[ImGuiCol_HeaderActive] = rgb(94, 74, 48);
    colours[ImGuiCol_Separator] = rgb(70, 56, 37);
    colours[ImGuiCol_SeparatorHovered] = rgb(90, 72, 48);
    colours[ImGuiCol_SeparatorActive] = kGold;
    colours[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    colours[ImGuiCol_ResizeGripHovered] = ImVec4(0, 0, 0, 0);
    colours[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
    colours[ImGuiCol_Tab] = rgb(42, 33, 23);
    colours[ImGuiCol_TabHovered] = rgb(77, 61, 40);
    colours[ImGuiCol_TabSelected] = rgb(58, 46, 31);
    colours[ImGuiCol_PlotHistogram] = rgb(212, 160, 23);
    colours[ImGuiCol_PlotHistogramHovered] = kGold;
    colours[ImGuiCol_TableHeaderBg] = rgb(42, 33, 23);
    colours[ImGuiCol_TableBorderStrong] = rgb(70, 56, 37);
    colours[ImGuiCol_TableBorderLight] = rgb(52, 42, 28);
    colours[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    colours[ImGuiCol_TableRowBgAlt] = rgb(255, 240, 200, 0.03f);
    colours[ImGuiCol_TextSelectedBg] = rgb(255, 209, 0, 0.30f);
    colours[ImGuiCol_TextLink] = kGold;
    colours[ImGuiCol_NavCursor] = kGold;
    colours[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.55f);

    style.ScaleAllSizes(scale);
    style.FontSizeBase = kFontSize;
    style.FontScaleDpi = scale;
}

std::string_view tr(std::string_view key) {
    return text::get(text::Key(key));
}

std::string trf(std::string_view key, const std::vector<std::string>& args) {
    return formatText(key, args);
}

std::string label(std::string_view key, std::string_view id) {
    std::string out(tr(key));
    out += "##";
    out += id;
    return out;
}

void text(std::string_view value) {
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

void textWrapped(std::string_view value) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
    ImGui::PopTextWrapPos();
}

void textColored(const ImVec4& colour, std::string_view value, bool wrapped) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    if (wrapped) {
        textWrapped(value);
    } else {
        text(value);
    }
    ImGui::PopStyleColor();
}

void textMuted(std::string_view value) {
    textColored(kMuted, value);
}

void title(std::string_view value) {
    ImGui::PushFont(nullptr, kFontSize * 1.6f);
    textColored(kGold, value);
    ImGui::PopFont();
    ImGui::Spacing();
}

void heading(std::string_view value) {
    gap(0.4f);
    ImGui::PushFont(nullptr, kFontSize * 1.15f);
    textColored(kGold, value);
    ImGui::PopFont();
}

void messageText(Message::Level level, std::string_view value) {
    const Status status = level == Message::Level::Error     ? Status::Error
                          : level == Message::Level::Warning ? Status::Warning
                                                             : Status::Ok;
    statusDot(status);
    ImGui::SameLine();
    textColored(level == Message::Level::Info ? kText : statusColour(status), value);
}

void message(const Message& line) {
    messageText(line.level, format(line));
}

ImVec4 statusColour(Status status) {
    switch (status) {
    case Status::Ok: return kOk;
    case Status::Warning: return kWarning;
    case Status::Error: return kError;
    case Status::Busy: return kGold;
    case Status::None: break;
    }
    return ImVec4(0.45f, 0.40f, 0.32f, 1.0f);
}

void statusDot(Status status) {
    const f32 size = ImGui::GetTextLineHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 centre(at.x + size * 0.5f, at.y + size * 0.5f);
    const f32 radius = size * 0.28f;
    const ImU32 colour = ImGui::GetColorU32(statusColour(status));
    if (status == Status::Busy) {
        const f32 start = static_cast<f32>(ImGui::GetTime()) * 6.0f;
        draw->PathArcTo(centre, radius, start, start + 4.2f, 16);
        draw->PathStroke(colour, size * 0.12f);
    } else {
        draw->AddCircleFilled(centre, radius, colour, 16);
    }
}

bool button(const std::string& labelText, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(labelText.c_str());
    ImGui::EndDisabled();
    return pressed && enabled;
}

bool primaryButton(const std::string& labelText, bool enabled) {
    ImGui::PushStyleColor(ImGuiCol_Button, kPrimary);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kPrimaryHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kPrimaryActive);
    ImGui::PushStyleColor(ImGuiCol_Border, kPrimaryBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, ImGui::GetStyle().FontScaleDpi));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x * 1.6f,
                                                           ImGui::GetStyle().FramePadding.y * 1.3f));
    const bool pressed = button(labelText, enabled);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return pressed;
}

void progressBar(f64 fraction, std::string_view overlay, f32 width) {
    const std::string text(overlay);
    if (fraction >= 0) {
        ImGui::ProgressBar(static_cast<f32>(std::min(fraction, 1.0)), ImVec2(width, 0), text.c_str());
    } else {
        // ImGui's indeterminate bar: a negative fraction times the time.
        ImGui::ProgressBar(-1.0f * static_cast<f32>(ImGui::GetTime()), ImVec2(width, 0), text.c_str());
    }
}

void spinner() {
    statusDot(Status::Busy);
}

void qrCode(const QrMatrix& qr, f32 side) {
    constexpr int kQuiet = 4;
    const int modules = qr.size + 2 * kQuiet;
    if (qr.size <= 0) {
        return;
    }
    // Whole pixels per module: a sharp code scans better.
    const f32 module = std::max(2.0f, std::floor(side / static_cast<f32>(modules)));
    const f32 size = module * static_cast<f32>(modules);
    ImVec2 at = ImGui::GetCursorScreenPos();
    at.x = std::floor(at.x);
    at.y = std::floor(at.y);
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size), IM_COL32(255, 255, 255, 255));
    for (int y = 0; y < qr.size; ++y) {
        for (int x = 0; x < qr.size; ++x) {
            if (!qr.dark(x, y)) {
                continue;
            }
            const ImVec2 corner(at.x + (static_cast<f32>(x + kQuiet)) * module,
                                at.y + (static_cast<f32>(y + kQuiet)) * module);
            draw->AddRectFilled(corner, ImVec2(corner.x + module, corner.y + module), IM_COL32(0, 0, 0, 255));
        }
    }
}

void selectableText(const std::string& value, std::string_view id) {
    std::string buffer = value;
    const std::string widget = "##" + std::string(id);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0.25f));
    ImGui::SetNextItemWidth(
        std::min(ImGui::GetContentRegionAvail().x,
                 ImGui::CalcTextSize(value.c_str()).x + ImGui::GetStyle().FramePadding.x * 2 + 4));
    ImGui::InputText(widget.c_str(), buffer.data(), buffer.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();
}

void openPath(const std::string& utf8Path) {
#if defined(_WIN32)
    // ShellExecute opens a plain path: Explorer for a folder, the file's program for a file.
    SDL_OpenURL(utf8Path.c_str());
#else
    const std::string url = "file://" + percentEncodePath(utf8Path);
    SDL_OpenURL(url.c_str());
#endif
}

void gap(f32 lines) {
    ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * lines));
}

} // namespace vd::setup::gui
