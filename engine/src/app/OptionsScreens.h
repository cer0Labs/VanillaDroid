#pragma once

#include "app/Dialogs.h"
#include "app/Hud.h"
#include "app/OptionsWindows.h"
#include "app/UiSound.h"
#include "core/Types.h"

#include <array>
#include <string_view>

namespace vd {

class GameSession;
class TutorialScreens;

// Phase 15 (options): the game's side of UIOptionsFrame and SoundOptionsFrame (app/OptionsWindows): their rows built
// from the settings with 1.12's sections, dependencies (UIOptionsFrame_UpdateDependencies) and tooltips, and what the
// windows' buttons do. The interface options edit a copy until Okay (UIOptionsFrame_Save; Cancel forgets it,
// Defaults sets UIOptionsFrame_SetDefaults' values); the sound options act at once and Cancel puts back what they were
// (SoundOptionsFrame_Cancel). The helm, the cloak and the extra action bars go to the server on Okay
// (game::Gameplay::showHelm / setActionBarToggles). prepareHud hands the options the HUD and the modules follow.
// GameScreens owns it. Main thread; nothing allocates.
class OptionsScreens {
public:
    enum class Window : u8 { Interface, Sound };

    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }
    // Opening a window (UIOptionsFrame_Load / SoundOptionsFrame_Load): what it starts from.
    void open(Window window, GameSession* session, const SettingsView& settings);
    // Draws it in the place of `c`; false once it closed (Okay or Cancel).
    bool draw(Window window, GameSession* session, DialogContext& c, SettingsView& settings);
    void setTab(u32 tab) { m_tab = tab; } // the gallery: the Advanced tab
    // Phase 18 (p3): the Help section's SHOW_TUTORIALS and Reset Tutorials act on TutorialFrame's buttons too.
    void setTutorials(TutorialScreens* tutorials) { m_tutorialScreens = tutorials; }

    // The extra action bars shown (SHOW_MULTI_ACTIONBAR_1..4, bits 0..3): the server's for the character in the world
    // (PLAYER_FIELD_BYTES), a local choice without one (the offline HUD: the bottom left bar).
    u8 multiBars(GameSession* session) const;
    // What the HUD shows by the options (names, bars' text, buff durations, extra bars...).
    void prepareHud(GameSession* session, const SettingsView& settings, HudFrame& hud) const;
    // TargetofTarget_Update's rule: SHOW_TARGET_OF_TARGET and its state (1 raid, 2 party, 3 solo, 4 raid or party,
    // 5 always) against the group the player is in.
    static bool targetOfTargetShown(const SettingsView& settings, bool inParty, bool inRaid);
    // UIOptionsFrame_SetDefaults / SoundOptionsFrame_SetDefaults on the options of one window (the rest stays).
    static void interfaceDefaults(SettingsView& settings);
    static void soundDefaults(SettingsView& settings);
    // The interface options of `from` into `to` (Okay).
    static void copyInterfaceOptions(const SettingsView& from, SettingsView& to);
    static void copySoundOptions(const SettingsView& from, SettingsView& to);

private:
    // What a row edits.
    enum class Binding : u8 { None, Bool, InvertedBool, Float, Choice, Helm, Cloak, Bar1, Bar2, Bar3, Bar4, Reset,
                              Tutorials }; // phase 18 (p3): SHOW_TUTORIALS (Reset: RESET_TUTORIALS)
    struct RowBinding {
        Binding kind = Binding::None;
        bool* flag = nullptr;
        f32* value = nullptr;
        u32* choice = nullptr;
        const u32* values = nullptr; // a choice's setting values, in the order shown
    };
    static constexpr u32 kMaxRows = 80;

    void buildInterface(SettingsView& s, u8 playerClass);
    void buildSound(SettingsView& s);
    void addSection(std::string_view label);
    OptionRow& addCheck(std::string_view label, std::string_view tooltip, bool* flag, bool inverted = false);
    OptionRow& addServerCheck(std::string_view label, std::string_view tooltip, Binding kind, bool checked);
    OptionRow& addSlider(std::string_view label, std::string_view tooltip, f32* value, f32 min, f32 max, f32 step,
                         std::string_view shown);
    OptionRow& addChoice(std::string_view label, std::string_view tooltip, u32* choice,
                         std::span<const std::string_view> labels, const u32* values);
    void apply(const OptionsWindowResult& r);
    OptionRow& addButton(std::string_view label, Binding kind); // phase 18 (p3)
    void resetTutorials(GameSession* session);                  // phase 18 (p3)

    UiSoundSink m_uiSound;
    SettingsView m_pending;      // the interface options being edited
    SettingsView m_soundInitial; // the sound options when the window opened
    bool m_helm = true, m_cloak = true;
    std::array<bool, 4> m_bars{};
    bool m_tutorials = true;                     // phase 18 (p3): SHOW_TUTORIALS (TutorialsEnabled)
    TutorialScreens* m_tutorialScreens = nullptr; // phase 18 (p3)
    u8 m_localBars = 1; // without a server: the bottom left bar, as the offline HUD always showed it
    bool m_localBarsSet = false; // changed in the options (VD_MULTIBARS no longer applies)
    u32 m_tab = 0;
    i32 m_tooltipRow = -1;
    i32 m_openChoice = -1;
    std::array<OptionRow, kMaxRows> m_rows{};
    std::array<RowBinding, kMaxRows> m_bindings{};
    u32 m_rowCount = 0;
    std::array<std::array<char, 24>, 8> m_shown{}; // the sliders' values as text
    u32 m_shownCount = 0;
};

} // namespace vd
