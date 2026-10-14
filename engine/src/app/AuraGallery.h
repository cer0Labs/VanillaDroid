#pragma once

#include "app/AuraLayout.h"
#include "app/AuraWindows.h"
#include "app/Dialogs.h"
#include "core/Types.h"

#include <array>

namespace vd {

// Development (parity, VD_UI_GALLERY=auras): the aura buttons with sample data, laid out as the HUD lays them
// out (app/HudAuras) and drawn by the same views (app/AuraWindows): the player's BuffFrame left of where the
// minimap goes (two weapon enchantments, ten buffs on two rows, four debuffs in their DebuffTypeColor; some
// about to expire, flashing), a hostile target with a target of target (debuffs first, wrapping at 5) and a
// friendly one (buffs first), the pet frame's buffs, two party frames with their debuffs and one portrait's
// glow. A tap shows an aura's tooltip; a long press cancels a cancelable buff of the player's (it goes away;
// the log says so) and shows the others' tooltips; a long press on a party frame lists the member's auras.
// Sample names and texts are English, as the client's Spell.dbc has them.
class AuraGallery {
public:
    void draw(DialogContext& c, u64 nowMs);

private:
    void build(u64 nowMs);

    bool m_built = false;
    u64 m_startMs = 0;
    std::array<bool, 16> m_cancelled{}; // the player's samples taken away by a long press
    // The tooltip open: which sample list and entry (-1 none), and its button.
    i32 m_tipList = -1;
    i32 m_tipIndex = -1;
    ui::Rect m_tipAnchor;
    ui::Rect m_tipPanel;
    AuraTooltipAnchor m_tipAt = AuraTooltipAnchor::BottomLeft;
    i32 m_listMember = -1; // a party frame's aura list
    ui::Rect m_listFrame;
    ui::Rect m_listPanel;
    std::array<char, 64> m_timeLeft{};
};

} // namespace vd
