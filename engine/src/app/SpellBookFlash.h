#pragma once

#include "core/Types.h"

namespace vd {

// Phase 18 (extras): the spell book's new-spell flash as SpellBookFrame.lua (1.12) runs it.
// LEARNED_SPELL_IN_TAB (WoW.exe 0x4B2B8D: a spell learned in play, not a hidden one nor a recipe, with its
// tab's index) shows that tab's Flash texture (Interface\Buttons\CheckButtonGlow, ADD) unless the pet's page
// was the last one shown, and sets flashTabs; SpellBookFrame_OnShow then flashes the frame that holds the
// textures (UIFrameFlash(frame, 0.5, 0.5, 30): half a second in, half a second out, for 30 s; a frame already
// flashing is left as it is); a tab clicked hides its own texture; SpellBookFrame_OnHide stops the flash and
// hides every texture. A spell learned while the book is open shows its texture in a frame that only flashes
// while its 30 s last. The micro button does not flash in 1.12 (MainMenuBarMicroButtons.lua pulses only the
// talents' one). Main thread; nothing allocates.
class SpellBookFlash {
public:
    static constexpr u32 kMaxTabs = 8; // MAX_SKILLLINE_TABS
    static constexpr u32 kFadeMs = 500;
    static constexpr u32 kDurationMs = 30000;

    // LEARNED_SPELL_IN_TAB: `tab` 0-based; ignored while the pet's page (BOOKTYPE_PET) is the book's.
    void learned(u32 tab, bool petPage);
    void shown(u64 nowMs); // SpellBookFrame_OnShow
    void hidden();         // SpellBookFrame_OnHide
    void clicked(u32 tab); // SpellBookSkillLineTab_OnClick
    // The tab's glow now: 0 when its texture is hidden or the frame is not flashing.
    f32 alpha(u32 tab, u64 nowMs) const;
    bool pending(u32 tab) const { return tab < kMaxTabs && (m_textures & (1u << tab)) != 0; }

private:
    u32 m_textures = 0;       // SpellBookSkillLineTab<n>Flash shown (bit n - 1)
    bool m_flashTabs = false; // SpellBookFrame.flashTabs (never cleared again, as in the original)
    bool m_flashing = false;  // SpellBookTabFlashFrame in FLASHFRAMES
    u64 m_flashStart = 0;
};

} // namespace vd
