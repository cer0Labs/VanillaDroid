#pragma once

#include "app/AppUpdates.h"
#include "app/Dialogs.h"
#include "app/GlueKeys.h"

#include <array>

namespace vd {

// Phase 22 (updates): the notices of app/AppUpdates over the glue screens (app/GlueScreens asks wanted() each
// frame and draws them over everything else of its own): a newer release (Update / What's new / Later), each
// step of updating (Android's permission, the download, its check, Android's confirmation, what went wrong),
// and once per start the game data baked for another version with what to do about it. Main thread; no
// allocations.
class UpdateScreens {
public:
    // Something to show this frame (the screens under it should not react). Takes AppUpdates' state.
    bool wanted();
    // Draws it (after wanted()); true when a button was tapped (the caller plays the glue's sound).
    bool draw(DialogContext& c, GlueKey key);

private:
    enum class Notice : u8 { None, Update, Data };
    Notice notice() const;

    AppUpdates::State m_state;
    bool m_dataSeen = false; // the data notice was answered this run
    std::array<char, 256> m_text{};
    std::array<char, 512> m_detail{};
    std::array<char, 64> m_progress{};
    std::array<char, 64> m_title{};
};

} // namespace vd
