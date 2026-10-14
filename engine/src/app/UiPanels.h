#pragma once

#include "core/Types.h"
#include "ui/DrawList.h"

#include <array>
#include <span>

namespace vd {

// Parity pass (panels): the original interface's panel windows as UIParent.lua manages them (UIPanelWindows,
// ShowUIPanel, HideUIPanel, CloseWindows, CloseAllWindows): which window takes the left slot and which the
// center one, which pushes which aside, which replace or close the others; the bags beside them
// (ContainerFrame); and where those slots go on a touch screen, clear of the movement stick and the action
// buttons (uiPanelPlaces). Pure logic, no drawing, no game state (unit tested): app/GameScreens keeps one
// UiPanelSlots, tells it what the modules open and close, and draws each module's window in the slot it says.

// The windows the original registers in UIPanelWindows (UIParent.lua, and the load-on-demand addons' own
// entries: Blizzard_TalentUI, Blizzard_TrainerUI, Blizzard_AuctionUI), the ones this client has.
enum class UiPanel : u8 {
    None,
    GameMenu,  // GameMenuFrame
    Options,   // OptionsFrame (the settings)
    Character, // CharacterFrame, with its Reputation and Skills tabs
    ItemText,  // ItemTextFrame (a book, a letter)
    Spellbook, // SpellBookFrame
    Loot,      // LootFrame
    Taxi,      // TaxiFrame
    Quest,     // QuestFrame / GossipFrame: an NPC's conversation and quest pages
    QuestLog,  // QuestLogFrame
    Merchant,  // MerchantFrame
    Trade,     // TradeFrame
    Bank,      // BankFrame
    Friends,   // FriendsFrame (the social window)
    WorldMap,  // WorldMapFrame
    Mail,      // MailFrame
    Talents,   // TalentFrame
    Trainer,   // ClassTrainerFrame
    Auction,   // AuctionFrame
    // Phase 13 (professions): Blizzard_TradeSkillUI's and Blizzard_CraftUI's own entries.
    TradeSkill, // TradeSkillFrame
    Craft,      // CraftFrame (Enchanting, Beast Training)
    Inspect,   // phase 13 (social): InspectFrame (Blizzard_InspectUI)
    PetStable, // phase 15 (pets): PetStableFrame
    DressUp,   // phase 15 (items): DressUpFrame
    // Phase 15 (guild, help).
    GuildRegistrar, // GuildRegistrarFrame
    Petition,       // PetitionFrame
    Tabard,         // TabardFrame
    Help,           // HelpFrame
    GmSurvey,       // GMSurveyFrame (Blizzard_GMSurveyUI's entry)
    Suggest,        // SuggestFrame (no UIPanelWindows entry: shown as a center window here)
    // Phase 15 (options): GameMenuFrame's other windows and Blizzard_MacroUI's own entry.
    SoundOptions, // SoundOptionsFrame
    UIOptions,    // UIOptionsFrame
    Macro,        // MacroFrame
    Battlefield, // phase 17 (queue): BattlefieldFrame (the battlemaster's list)
    WorldStateScore, // phase 17 (scores): WorldStateScoreFrame (drawn over both slots: as wide as two windows)
    Count
};

// UIPanelWindows' areas: "left" (the left slot, pushed to the center by a lower one), "center" (alone,
// closing the rest), "doublewide" (both slots) and "full" (the whole screen; here the slots together).
enum class UiPanelArea : u8 { Left, Center, Doublewide, Full };

struct UiPanelInfo {
    UiPanelArea area = UiPanelArea::Left;
    u8 pushable = 0;        // with two open, the higher one goes to the center
    bool whileDead = false; // may open while the player is dead (ShowUIPanel: NotWhileDeadError otherwise)
};
UiPanelInfo uiPanelInfo(UiPanel panel);
// Whether the window has the player's bags inside it (ours: vendor, bank, mailbox, trade, auction house): the
// bags do not open beside it, and it closes them.
bool uiPanelHostsBags(UiPanel panel);

// What ShowUIPanel decided: the window shows, it was refused (a full screen or center window is up, or the
// player is dead), or the player is dead and it does not open then (NotWhileDeadError: ERR_PLAYER_DEAD).
enum class UiPanelShow : u8 { Shown, Refused, Dead };

// UIParent's left, center, doublewide and full-screen frames, plus whether the bags are open. The bags are
// not a UIPanel in the original (ContainerFrames stack on their own at the bottom right); here they share the
// right side with the center slot: the last one opened stays. With one place (the cover screen) the center
// slot does not exist: a window that would go there, or the bags, replace the one shown.
class UiPanelSlots {
public:
    // How many windows fit side by side (1 or 2). Fewer than before closes what no longer fits.
    void setCapacity(u32 places);
    u32 capacity() const { return m_capacity; }

    // ShowUIPanel(frame, force). Without `force`, a full-screen or center window keeps others from opening.
    UiPanelShow show(UiPanel panel, bool dead, bool force);
    // HideUIPanel(frame): the center window moves back to the left when the left one closes.
    void hide(UiPanel panel);
    // The bags (ContainerFrame): false when a window that has the bags inside it is up (nothing opens).
    bool openBags();
    void closeBags();
    // CloseAllWindows(ignoreCenter): every window and the bags (but a center window when `ignoreCenter`).
    void closeAll(bool ignoreCenter = false);

    bool shown(UiPanel panel) const;
    bool bagsOpen() const { return m_bags; }
    UiPanel left() const { return m_left; }
    UiPanel center() const { return m_center; }
    UiPanel doublewide() const { return m_doublewide; }
    UiPanel full() const { return m_full; }
    bool any() const {
        return m_left != UiPanel::None || m_center != UiPanel::None || m_doublewide != UiPanel::None ||
               m_full != UiPanel::None || m_bags;
    }
    // The windows the last calls hid, for the caller to close in their modules (ShowUIPanel's frame:Hide() on
    // the windows it replaces). Read once: the list empties.
    std::span<const UiPanel> takeHidden();

private:
    void setLeft(UiPanel panel);
    void setCenter(UiPanel panel);
    void moveToCenter();
    void moveToLeft();
    void hidden(UiPanel panel);
    void closeWindows(bool ignoreCenter);

    UiPanel m_left = UiPanel::None;
    UiPanel m_center = UiPanel::None;
    UiPanel m_doublewide = UiPanel::None;
    UiPanel m_full = UiPanel::None;
    bool m_bags = false;
    u32 m_capacity = 2;
    std::array<UiPanel, 16> m_hidden{};
    u32 m_hiddenCount = 0;
    std::array<UiPanel, 16> m_taken{};
};

// --- Where the slots go ------------------------------------------------------------------------------------

// What the screen leaves to the windows, in logical pixels.
struct UiPanelSpace {
    ui::Rect safe;           // the safe area (without the soft keyboard)
    f32 dp = 1.0f;           // logical pixels per dp
    f32 unit = 1.0f;         // logical pixels per unit of the original's 768-high interface
    bool wide = false;       // ScreenProfile::Wide: the 20:9 phones and the cover screen held sideways
    f32 framesBottom = 0.0f; // under the player and target frames: the windows start there
    f32 leftTop = 0.0f;      // two places: the left one starts there instead (under the target's auras)
    f32 rightTop = 0.0f;     // two places: the right one starts there (under the minimap cluster)
    ui::Rect cluster;        // the action buttons' cluster (never covered)
    ui::Rect stance;         // the stance bar (never covered; empty: none)
};

// The rectangles the windows go in (logical pixels). The movement stick floats where the left thumb lands: on the
// inner screen they leave it the lower third at the left (as the chat does, app/ChatFrame); the wide screens, too
// short for that, give the windows everything from the left and top edges instead (over the stick's column and the
// unit frames: no walking with a window open there). The action buttons and the stance bar stay clear; the bottom left
// action bar (inner screen) gives way to a window on the right.
struct UiPanelPlaces {
    ui::Rect left;     // the left slot (the only one when `right` is empty)
    // Retouches (quest log): the left slot of the wide screens as wide as it gets, shorter over the stance bar
    // instead of narrower beside it (the quest log's side-by-side arrangement wants the width); else `left`.
    ui::Rect leftWide;
    ui::Rect right;    // the center slot and the bags (empty: one window at a time)
    ui::Rect both;     // the slots together: doublewide and full-screen windows
    ui::Rect stickPad; // where the stick floats with no window over it
    // The wide screens: the place covers the unit frames' band (the windows there are opaque, the frames hide).
    bool overFrames = false;
};
UiPanelPlaces uiPanelPlaces(const UiPanelSpace& space);

// The screen shape a window lays itself out for in `place` (the windows' wide layouts in short places).
bool uiPanelWide(const ui::Rect& place);
// Whether two rectangles share some area.
bool uiPanelOverlaps(const ui::Rect& a, const ui::Rect& b);

} // namespace vd
