#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"

#include <span>
#include <string_view>

namespace vd {

// Phase 15 (options): the original's option windows as pure views like app/Dialogs: UIOptionsFrame (its Basic and
// Advanced tabs, each a stack of OptionFrameBoxTemplate sections with their check buttons, sliders and drop-downs)
// and SoundOptionsFrame, drawn from a list of rows the glue builds (app/OptionsScreens) with Defaults at the bottom
// left and Okay / Cancel at the bottom right. Both over the whole screen with UIOptionsFrame's art
// (kit::beginFullScreen: SoundOptionsFrame was a dialog in 1.12); two columns when the parchment is 600 dp wide or more
// (whole sections go to the shorter column), one otherwise. Touch: a tap is the click; a long press on a row shows its
// OPTION_TOOLTIP_* (the original's hover) until another row is touched or the tooltip is tapped; a drop-down opens its
// choices under itself.

struct OptionRow {
    enum class Kind : u8 {
        Section, // OptionFrameBoxTemplate's title: a new box
        Check,   // UIOptionsCheckButtonTemplate
        Slider,  // OptionsSliderTemplate
        Choice,  // UIDropDownMenuTemplate
        Button,  // GameMenuButtonTemplate (Reset Tutorials)
    };
    Kind kind = Kind::Check;
    std::string_view label;
    std::string_view tooltip; // OPTION_TOOLTIP_* (empty: none)
    bool enabled = true;      // OptionsFrame_DisableCheckBox / _DisableSlider / _DisableDropDown: grey, untouched
    bool indent = false;      // depends on the row above (SHOW_GUILD_NAMES under SHOW_PLAYER_NAMES)
    bool checked = false;     // Check
    f32 value = 0.0f, min = 0.0f, max = 1.0f, step = 0.0f; // Slider
    std::string_view shown;                                // Slider: its value as text
    std::span<const std::string_view> choices;             // Choice
    u32 choice = 0;                                        // into choices
};

struct OptionsWindowView {
    std::string_view name;  // widget ids and the panel's name ("uioptions", "soundoptions")
    std::string_view title; // UIOPTIONS_MENU, SOUNDOPTIONS_MENU
    std::span<const std::string_view> tabs; // BASIC_LABEL / ADVANCED_OPTIONS; empty: none
    u32 tab = 0;
    std::span<const OptionRow> rows;
    i32 tooltipRow = -1; // the row whose tooltip shows
    i32 openChoice = -1; // the drop-down whose choices show
    bool defaults = true;
};

struct OptionsWindowResult {
    i32 tab = -1;
    i32 toggled = -1; // a check tapped
    i32 slider = -1;  // a slider moved, to `value`
    f32 value = 0.0f;
    i32 choiceOpened = -1; // a drop-down tapped (it opens, or closes when open)
    i32 choice = -1;       // a choice picked in the open drop-down: its index
    i32 button = -1;       // a button tapped
    i32 held = -1;         // a row held: its tooltip
    bool tooltipDismissed = false;
    bool okay = false, cancel = false, defaults = false;
};

OptionsWindowResult optionsWindow(DialogContext& c, const OptionsWindowView& v);

} // namespace vd
