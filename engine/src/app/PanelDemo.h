#pragma once

#include "app/CharacterGallery.h"
#include "app/CombatGallery.h"
#include "app/Dialogs.h"
#include "app/GuildGallery.h"
#include "app/InventoryGallery.h"
#include "app/ItemsGallery.h"
#include "app/MicroMenu.h"
#include "app/QuestGallery.h"
#include "app/UiPanels.h"
#include "core/Types.h"

namespace vd {

class TravelScreens;

// Development (VD_HUD_DEMO, no server): the panel windows with the galleries' sample data over the running
// HUD, so their places can be checked with the character moving (--walk): the micro menu opens them through
// the same UIPanelWindows rules (app/UiPanels) as in the game. A level-12 warrior with three talent points
// (the talents button pulses); bags, character, spell book, talents, quest log, social window and world map.
// What the player does in them goes to the log ("UI gallery: ...").
class PanelDemo {
public:
    PanelDemo();
    MicroMenuView microView(bool bagsOpen, const UiPanelSlots& panels) const;
    // `panel` in the place of `c` (UiPanel::None: the bags).
    void draw(UiPanel panel, DialogContext& c, TravelScreens& travel);
    // Phase 15 (items): the sample ContainerFrames (the bag bar's buttons; whether any is open; CloseAllWindows).
    void bagButton(i32 button, bool shift) { m_items.bagButton(button, shift); }
    bool bagsOpen() const { return m_items.bagsOpen(); }
    // The sample watched quests in `place` (app/GameScreens' place over the chat).
    void drawQuestWatch(DialogContext& c, const ui::Rect& place) {
        m_quests.drawWatchAt(c, static_cast<u32>(c.ui.nowMs()), place);
    }
    void closeBags() { m_items.closeBags(); }

private:
    InventoryGallery m_inventory;
    CharacterGallery m_character; // phase 15 (character)
    CombatGallery m_spellbook;
    CombatGallery m_talents;
    QuestGallery m_quests; // phase 13: the quest log with sample quests
    ItemsGallery m_items;  // phase 15 (items): the bags as ContainerFrames
    GuildGallery m_help;   // phase 15 (help): HelpFrame with an open ticket
};

} // namespace vd
