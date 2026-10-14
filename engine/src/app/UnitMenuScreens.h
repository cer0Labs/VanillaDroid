#pragma once

#include "app/Dialogs.h"
#include "app/InventoryWindows.h"
#include "app/UnitMenuWindows.h"
#include "core/Types.h"
#include "game/UnitPopup.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace vd {

class CharacterScreens;
class GameSession;
class Hud;
class InventoryScreens;
class PlayerController;
struct HudFrame;
class PopupScreens;
class SocialScreens;
namespace game {
class GameData;
class ItemData;
class Reactions;
} // namespace game

// Phase 13 (social): the player's menus (UnitPopup.lua) and what their buttons and the matching slash commands do
// in the client's C code: FollowUnit / FollowByName (the character runs after a player, AutoFollowStatus),
// TargetByName, AssistUnit, InspectUnit (the InspectFrame: the player's equipment from its visible item fields),
// InitiateTrade, StartDuelUnit, SetRaidTarget. Fills game::UnitPopupState from the session, draws the menu
// (app/UnitMenuWindows' DropDownList), and acts on the button picked (UnitPopup_OnClick) through game::Social,
// game::PlayerCommands, the popups (app/PopupScreens) and the chat (app/SocialScreens). Main thread; fixed storage.
//
// Touch: a long press on a unit frame is its right click (PlayerFrame, TargetFrame, PartyMemberFrame, PetFrame;
// the social window's rows; a name in the chat); the list hangs where the client anchors it.
class UnitMenuScreens {
public:
    // Where the menu was opened (ToggleDropDownMenu's anchors).
    enum class Frame : u8 { Player, Target, Party, Pet, Cursor };

    void init(const game::GameData* data);
    void setPopups(PopupScreens* popups) { m_popups = popups; }
    void setSocial(SocialScreens* social) { m_social = social; }
    void setInventory(InventoryScreens* inventory) { m_inventory = inventory; }
    // Phase 15 (character): the InspectFrame's model and honor tab (null: the slots alone).
    void setCharacterSheet(CharacterScreens* sheet) { m_sheet = sheet; }
    void setAssistAttack(bool on) { m_assistAttack = on; } // phase 15 (options): ASSIST_ATTACK
    // Where the UIErrorsFrame's lines go (GameScreens' messages).
    using MessageSink = void (*)(void* context, u32 color, bool big, const char* line);
    void setMessageSink(MessageSink sink, void* context) {
        m_message = sink;
        m_messageContext = context;
    }

    // --- The menus ---
    // A unit frame's menu: PlayerFrameDropDown ("SELF"), TargetFrameDropDown (whatever the target is),
    // PartyMemberFrameNDropDown ("PARTY"), PetFrameDropDown ("PET"). `frame` is the frame's rectangle.
    void openUnitMenu(GameSession& session, u64 guid, Frame from, const ui::Rect& frame);
    // FriendsDropDown ("FRIEND", by name) at the finger (`x`, `y`): the friends, who and guild lists, a name in
    // the chat. `guildTab`: opened from the guild roster (GuildFrame:IsVisible(): its promote / leave buttons).
    void openFriendMenu(GameSession& session, std::string_view name, f32 x, f32 y, bool guildTab = false);
    // RaidFrameDropDown ("RAID") for a raid member at the finger.
    void openRaidMenu(GameSession& session, u64 guid, std::string_view name, f32 x, f32 y);
    bool wantsModal() const { return m_menu.open; }
    void closeMenu() { m_menu.open = false; } // phase 18 (extras): CloseMenus (Escape)
    void drawModal(GameSession& session, DialogContext& c);
    // The HUD's long presses (the unit frames' right clicks): the player frame, the target frame and the pet
    // frame (`petFrame`: where CombatScreens drew it, empty without a pet).
    void actOnHud(GameSession& session, const Hud& hud, bool petFrameHeld, const ui::Rect& petFrame);
    // A new character in the world (or none): the menu, the follow and the inspect frame end.
    void reset();
    // PET_PAPERDOLL asked for the pet's sheet (ToggleCharacter("PetPaperDollFrame")): read once.
    bool takeOpenCharacter() { return std::exchange(m_openCharacter, false); }

    // --- The C functions behind the buttons and the slash commands ---
    // FollowUnit: false with the client's error (ERR_INVALID_FOLLOW_TARGET, ERR_PLAYER_DEAD,
    // ERR_TOOBUSYTOFOLLOW, ERR_AUTOFOLLOW_TOO_FAR).
    bool followUnit(GameSession& session, u64 guid);
    // TargetByName / AssistByName / FollowByName: the nearest unit so named in sight (`exact`: the whole name,
    // else a name that starts with it; any case). False with ERR_UNIT_NOT_FOUND / ERR_GENERIC_NO_TARGET.
    bool targetByName(GameSession& session, std::string_view name, bool exact);
    bool followByName(GameSession& session, std::string_view name, bool exact);
    bool assistByName(GameSession& session, std::string_view name);
    // AssistUnit: the unit's own target becomes the player's.
    bool assistUnit(GameSession& session, u64 guid);
    // InspectUnit (Blizzard_InspectUI's InspectFrame_Show): CanInspect's checks and errors, NotifyInspect, the
    // frame. `fromTarget`: the unit is "target" (a new target is inspected in turn, no target closes it).
    bool inspectUnit(GameSession& session, u64 guid, bool fromTarget);
    // InitiateTrade (CMSG_INITIATE_TRADE, or why not).
    bool initiateTrade(GameSession& session, u64 guid);
    // Who the player follows (0: nobody).
    u64 following() const { return m_follow.guid; }

    // --- Every frame ---
    // Before the HUD (GameScreens::prepareHud): the follow walks the character (hud.player), ending when the
    // leader is gone or too far, when the player moves by itself or dies; the game's events of phase 13 (the
    // time played, a refused pet name, the PvP wish) become their lines.
    void prepareHud(GameSession& session, HudFrame& hud, u32 nowMs);
    // AutoFollowStatus over the world (not modal).
    void drawFollowStatus(DialogContext& c, u32 nowMs);

    // --- The InspectFrame (a panel: UIPanelWindows' area "left") ---
    bool inspectOpen(const GameSession& session) const;
    void closeInspect() { m_inspect = 0; }
    void drawInspect(GameSession& session, DialogContext& c, u32 nowMs);

    // Development: a menu with sample data (VD_UI_GALLERY=unitmenu*; app/UiGallery).
    // `x`, `y`: where level 1 hangs.
    static void galleryMenu(DialogContext& c, u32 page, f32 x, f32 y);

private:
    struct OpenMenu {
        bool open = false;
        game::UnitPopupMenu menu = game::UnitPopupMenu::Self;
        u64 guid = 0;           // the unit (0: FRIEND's, by name)
        bool unitIsTarget = false; // the target frame's ("target": the raid target icons' first level checks it)
        std::array<char, 64> name{};
        u8 raidRank = 0;
        bool guildTab = false;
        f32 x = 0.0f, y = 0.0f; // level 1's top left
        i32 nestedRow = -1;     // level 2 open from this row of level 1 (-1: none)
        game::UnitPopupButton nested = game::UnitPopupButton::Cancel;
    };

    void open(GameSession& session, game::UnitPopupMenu menu, u64 guid, std::string_view name, f32 x, f32 y,
              bool guildTab = false);
    void updateFollow(GameSession& session, PlayerController* player, u32 nowMs);
    void readEvents(GameSession& session);
    void updateInspect(GameSession& session);
    game::UnitPopupState state(GameSession& session) const;
    void act(GameSession& session, game::UnitPopupButton button);
    void message(u32 color, std::string_view line);
    void error(std::string_view key);
    std::string_view nameOf(GameSession& session, u64 guid) const;
    u64 findByName(GameSession& session, std::string_view name, bool exact) const;
    void stopFollow(u32 nowMs, bool show);
    std::string_view itemText(game::UnitPopupButton button, const game::UnitPopupState& s,
                              std::span<char> buffer) const;
    void fillItems(std::span<const game::UnitPopupItem> items, const game::UnitPopupState& s, bool level2);

    const game::GameData* m_data = nullptr;
    const game::ItemData* m_items = nullptr;
    const game::Reactions* m_reactions = nullptr; // the HUD's (FactionTemplate.dbc), from prepareHud
    PopupScreens* m_popups = nullptr;
    SocialScreens* m_social = nullptr;
    InventoryScreens* m_inventory = nullptr;
    MessageSink m_message = nullptr;
    void* m_messageContext = nullptr;
    bool m_trace = false;
    bool m_openCharacter = false;
    bool m_assistAttack = false; // phase 15 (options)

    OpenMenu m_menu;
    std::array<game::UnitPopupItem, game::kUnitPopupMaxItems> m_level1{};
    std::array<game::UnitPopupItem, game::kUnitPopupMaxItems> m_level2{};
    u32 m_level1Count = 0, m_level2Count = 0;
    std::array<DropDownItemView, game::kUnitPopupMaxItems + 1> m_views{}; // + the title
    std::array<DropDownItemView, game::kUnitPopupMaxItems> m_subViews{};
    std::array<std::array<char, 64>, game::kUnitPopupMaxItems + 1> m_texts{};
    std::array<std::array<char, 64>, game::kUnitPopupMaxItems> m_subTexts{};

    // FollowUnit: whom, its name for AutoFollowStatus, and the status line fading after it ended.
    struct Follow {
        u64 guid = 0;
        bool moving = false; // running after it (hysteresis between kFollowNear and kFollowResume)
        std::array<char, 96> status{};
        u32 endedMs = 0; // AUTOFOLLOW_END: the line fades for AUTOFOLLOW_STATUS_FADETIME (0: following or none)
        bool shown = false;
    };
    Follow m_follow;
    u32 m_nowMs = 0;
    u32 m_eventCursor = 0;
    u64 m_cursorsPlayer = 0;

    // The InspectFrame: whom, whether it is "target", and its views.
    u64 m_inspect = 0;
    bool m_inspectTarget = false;
    std::array<ItemSlotView, 20> m_inspectSlots{};
    std::array<u32, 20> m_inspectEntries{};
    std::array<char, 64> m_inspectTitle{};
    std::array<char, 128> m_inspectSubtitle{};
    ItemDrag m_inspectDrag; // never carries anything: the inspected player's items stay where they are
    CharacterScreens* m_sheet = nullptr; // phase 15 (character)
};

} // namespace vd
