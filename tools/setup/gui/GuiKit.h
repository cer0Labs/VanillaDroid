#pragma once

#include "QrText.h"
#include "SetupCommon.h"
#include "core/Types.h"

#include <imgui.h>

#include <string>
#include <string_view>
#include <vector>

// Phase 22 (gui): VanillaDroid Setup's look over Dear ImGui (the colours of the phone's page: dark brown,
// parchment text, gold and the red of the buttons) and the few widgets its pages share. Every text comes from
// core/Text keys.
namespace vd::setup::gui {

inline constexpr ImVec4 kText{0.949f, 0.902f, 0.788f, 1.0f};
inline constexpr ImVec4 kMuted{0.788f, 0.725f, 0.561f, 1.0f};
inline constexpr ImVec4 kGold{1.0f, 0.820f, 0.0f, 1.0f};
inline constexpr ImVec4 kOk{0.486f, 0.765f, 0.357f, 1.0f};
inline constexpr ImVec4 kWarning{0.910f, 0.639f, 0.239f, 1.0f};
inline constexpr ImVec4 kError{0.878f, 0.314f, 0.235f, 1.0f};
inline constexpr ImVec4 kSidebar{0.071f, 0.055f, 0.035f, 1.0f};
inline constexpr ImVec4 kBackground{0.106f, 0.086f, 0.063f, 1.0f};
inline constexpr ImVec4 kPanel{0.133f, 0.106f, 0.075f, 1.0f};

// The base size of the text (before the display's scale).
inline constexpr f32 kFontSize = 16.0f;

// The style for a display scale (1 on a 96-dpi screen). Called again when the window moves to another scale.
void applyStyle(f32 scale);

// --- Texts ---
std::string_view tr(std::string_view key);
std::string trf(std::string_view key, const std::vector<std::string>& args);
// "<text>##<id>": a widget's label whose id does not change with the language.
std::string label(std::string_view key, std::string_view id);

void text(std::string_view value);
void textWrapped(std::string_view value);
void textColored(const ImVec4& colour, std::string_view value, bool wrapped = true);
void textMuted(std::string_view value);
void title(std::string_view value);   // a page's title
void heading(std::string_view value); // a section of a page
// A line the player should read, coloured and marked by its level.
void message(const Message& line);
void messageText(Message::Level level, std::string_view value);

// --- Widgets ---
enum class Status : u8 { None, Ok, Warning, Error, Busy };
ImVec4 statusColour(Status status);
// A dot of the status' colour at the cursor, as tall as a line of text (a turning arc while busy).
void statusDot(Status status);
bool button(const std::string& labelText, bool enabled = true);
bool primaryButton(const std::string& labelText, bool enabled = true);
// A progress bar; a negative fraction moves back and forth (unknown).
void progressBar(f64 fraction, std::string_view overlay, f32 width = -1.0f);
void spinner();
// The QR code as squares (black on white with its quiet zone), `side` pixels wide at most.
void qrCode(const QrMatrix& qr, f32 side);
// A text that can be selected and copied (paths, addresses).
void selectableText(const std::string& value, std::string_view id);
// Opens a folder or file in the system's file manager / its program.
void openPath(const std::string& utf8Path);
// Space between sections.
void gap(f32 lines = 0.6f);

} // namespace vd::setup::gui
