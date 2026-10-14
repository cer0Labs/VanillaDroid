#pragma once

#include "app/Dialogs.h"
#include "app/PopupStack.h"
#include "app/PopupWindows.h"
#include "app/UiSound.h"
#include "core/Types.h"
#include "game/BindConfirm.h"
#include "game/Inventory.h"
#include "game/ItemActions.h"
#include "ui/TextInput.h"

#include <array>

namespace vd {

class GameSession;
namespace game {
class GameData;
}

// The client's StaticPopups in the game (parity pass, popups): what shows them (UIParent.lua's events, as
// this client's game state tells them), what their buttons do (StaticPopupDialogs' OnAccept / OnCancel /
// OnHide), their texts and countdowns. Up to four at the top centre (app/PopupStack, app/PopupWindows), none
// modal: the stick, the camera and the action bar keep working, and they show over every window instead of
// waiting behind them.
//
// Shown by the game: DEATH (with its auto-release countdown and the self-resurrection button), RESURRECT*,
// RECOVER_CORPSE, XP_LOSS* (the spirit healer, asked twice), PARTY_INVITE, GUILD_INVITE, DUEL_REQUESTED,
// DUEL_OUTOFBOUNDS, TRADE (and ours while the player's own trade request waits), QUEST_ACCEPT,
// CONFIRM_BINDER, CONFIRM_TALENT_WIPE, CONFIRM_SUMMON, CAMP, QUIT. Asked by the windows: ABANDON_QUEST,
// DELETE_ITEM / DELETE_GOOD_ITEM, LOOT_BIND, EQUIP_BIND, AUTOEQUIP_BIND, USE_BIND, ABANDON_PET, and the game
// menu's Logout / Exit Game. Main thread; nothing allocates per frame.
class PopupScreens {
public:
    // `data`: spell names (the self-resurrection button), area names (summons). May be null.
    void init(const game::GameData* data);
    // Where the popups' sounds go (StaticPopup_OnShow / OnHide: igMainMenuOpen / igMainMenuClose;
    // ABANDON_QUEST's igQuestLogAbandonQuest).
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }
    // Where a refused logout's error goes (the UIErrorsFrame: GameScreens' messages).
    using MessageSink = void (*)(void* context, u32 color, bool big, const char* line);
    void setMessageSink(MessageSink sink, void* context) {
        m_message = sink;
        m_messageContext = context;
    }

    // --- Questions the windows ask ---
    // Phase 18 (p3): ABANDON_QUEST, or ABANDON_QUEST_WITH_ITEMS when the quest's items go with it (QuestLogFrame's
    // Abandon button: GetAbandonQuestItems).
    void askAbandonQuest(GameSession& session, u32 questId);
    // Phase 18 (p3): OpenMail_Delete: DELETE_MAIL (a letter with an item: its name), DELETE_MONEY (with money: the money
    // frame); DeleteInboxItem on accept.
    void askDeleteMail(GameSession& session, u32 mailId, std::string_view itemName);
    // Phase 18 (p3): NAME_CHAT (FCF_RenameChatWindow_Popup with the ChatFrame's number, FCF_NewChatWindow with 0); the
    // typed name goes to `namer`.
    void askNameChat(GameSession& session, u32 chatFrame);
    using ChatNamer = void (*)(void* context, u32 chatFrame, std::string_view name);
    void setChatNamer(ChatNamer namer, void* context) {
        m_chatNamer = namer;
        m_chatNamerContext = context;
    }
    void askDeleteMoney(GameSession& session, u32 mailId, u32 copper);
    // Retouches (windows): OpenMailPackageButton's click on a C.O.D. letter: COD_ALERT when the player cannot
    // pay it, else COD_CONFIRMATION with the charge in its money frame (TakeInboxItem on accept).
    void askCod(GameSession& session, u32 mailId, u32 cod);
    // GetAbandonQuestItems (WoW.exe 0x4DFBE0): the quest's required items the player carries that are quest items
    // (bonding 4 or 5) and that no other quest in the log requires, their names joined by ", "; empty: none.
    static std::string_view abandonQuestItems(GameSession& session, u32 questId, std::string& out);
    // DELETE_ITEM, or DELETE_GOOD_ITEM (type DELETE) for rare and better items.
    void askDestroyItem(GameSession& session, game::ItemPos pos);
    // Soulbinding (game/BindConfirm): true when the client asks first (LOOT_BIND, EQUIP_BIND, AUTOEQUIP_BIND,
    // USE_BIND) and the action waits for OKAY; false when there is nothing to ask: do it now.
    bool askBindForAction(GameSession& session, const game::ItemPlace& place, game::ItemAction action);
    bool askBindForDrop(GameSession& session, const game::ItemPlace& from, const game::ItemPlace& to);
    bool askBindForUse(GameSession& session, u32 itemEntry); // the action bar's item
    bool askBindForLoot(GameSession& session, u8 lootSlot, u32 itemEntry);
    void askAbandonPet(GameSession& session);
    // Phase 12: CONFIRM_RESET_INSTANCES (the player's menu: UnitPopup's RESET_INSTANCES).
    void askResetInstances(GameSession& session);
    // Phase 16 (alerts): CONFIRM_LEAVE_QUEUE (MiniMapMeetingStoneFrame's OnClick): game::MeetingStone::leave on
    // accept.
    void askLeaveMeetingStoneQueue(GameSession& session);
    // Phase 17 (queue): CONFIRM_BATTLEFIELD_ENTRY for queue `slot` (BattlefieldFrame_UpdateStatus while it is
    // "confirm": shown again in its own dialog), `mapName` with its instance number; ENTER_BATTLE is
    // AcceptBattlefieldPort(slot, 1), HIDE only hides it. Hidden again when the queue leaves "confirm".
    void askBattlefieldEntry(GameSession& session, u32 slot, std::string_view mapName);
    void hideBattlefieldEntry(u32 slot);
    // CONFIRM_BUY_BANK_SLOT (the bank's Purchase button).
    void askBuyBankSlot(GameSession& session);
    // Phase 13 (social): CONFIRM_GUILD_DISBAND (/gdisband), CONFIRM_GUILD_PROMOTE (GuildSetLeaderByName on
    // accept), CONFIRM_GUILD_LEAVE, RENAME_PET (its edit box; then PETRENAMECONFIRM, then PetRename).
    void askGuildDisband(GameSession& session);
    void askGuildPromote(GameSession& session, std::string_view name);
    void askGuildLeave(GameSession& session);
    void askRenamePet(GameSession& session);
    // Phase 15 (items): CONFIRM_LOOT_ROLL before Need / Greed on a bind-on-pickup item (ConfirmLootRoll on OKAY: the
    // roll goes then); true when asked, false when there is nothing to ask (roll now). CONFIRM_LOOT_DISTRIBUTION
    // before the master looter gives an item of `quality` epic or better (MASTER_LOOT_THREHOLD) to `candidate`; true
    // when asked, false when it goes at once.
    bool askLootRoll(GameSession& session, u64 object, u32 slot, u8 vote, u32 itemEntry);
    bool askLootDistribution(GameSession& session, u8 lootSlot, u32 quality, std::string_view itemName, u64 candidate,
                             std::string_view candidateName);
    // Phase 15 (character): UNLEARN_SKILL (SkillFrame's Unlearn button; AbandonSkill on accept), and hiding it when
    // a skill header collapses while it asks about another line than `selectedSkill` (SkillLabelTemplate's OnClick).
    void askUnlearnSkill(GameSession& session, u32 skill, std::string_view name);
    void hideUnlearnSkill(u32 selectedSkill);
    // Phase 15 (guild): the guild frame's questions (ADD_GUILDMEMBER, REMOVE_GUILDMEMBER, ADD_GUILDRANK,
    // SET_GUILDMOTD with the MOTD, SET_GUILDPLAYERNOTE / SET_GUILDOFFICERNOTE with the member's note) and the
    // charter's RENAME_GUILD; their answers go to game::Social / GuildControl / Petitions.
    void askAddGuildMember(GameSession& session);
    void askRemoveGuildMember(GameSession& session, std::string_view name);
    void askAddGuildRank(GameSession& session);
    void askGuildMotd(GameSession& session, std::string_view motd);
    void askGuildNote(GameSession& session, std::string_view member, std::string_view note, bool officer);
    void askRenameGuild(GameSession& session);
    // Phase 16 (raidframe): ADD_RAIDMEMBER (the raid tab's Add Member); InviteByName on accept.
    void askAddRaidMember(GameSession& session);
    // Retouches (friends frame): ADD_FRIEND and ADD_IGNORE (the friends frame's Add Friend and Ignore Player without a
    // cooperative target); AddFriend / AddIgnore on accept.
    void askAddFriend(GameSession& session);
    void askAddIgnore(GameSession& session);
    // Phase 15 (help): HELP_TICKET (TicketStatusButton), HELP_TICKET_QUEUE_DISABLED; and where "Edit My Ticket" opens
    // the help frame's ticket page.
    void askHelpTicket(GameSession& session);
    void askHelpQueueDisabled(GameSession& session);
    void hideHelpTicket(); // ToggleHelpFrame: StaticPopup_Hide("HELP_TICKET_ABANDON_CONFIRM") and ("HELP_TICKET")
    bool helpTicketShown() const;
    using HelpOpener = void (*)(void* context, GameSession& session);
    void setHelpOpener(HelpOpener opener, void* context) {
        m_helpOpener = opener;
        m_helpContext = context;
    }
    // The game menu's Logout / Exit Game (GameSession::requestLogout); its answer shows CAMP / QUIT, an
    // error, or nothing (logged out at once).
    void askLogout(GameSession& session, bool quit);
    // CAMP or QUIT is up: the game menu's Logout and Exit Game are disabled (GameMenuFrame's OnShow).
    bool loggingOut() const { return m_stack.visible(PopupKind::Camp) || m_stack.visible(PopupKind::Quit); }

    // --- Every frame in the world ---
    // Before the windows: what the game says shows and hides popups; timeouts; what their answers do.
    void update(GameSession& session, u32 nowMs);
    // After every window (StaticPopups are in the DIALOG strata): the stack and its taps.
    void draw(GameSession& session, DialogContext& c, ui::TextInput& text, u32 nowMs);
    // Escape / Back (StaticPopup_EscapePressed): true when a popup was cancelled.
    bool escape(GameSession& session);
    // Out of the world (logged out, disconnected): every popup goes, nothing is answered.
    void leaveWorld();

    const PopupStack& stack() const { return m_stack; }

private:
    void reset();
    Popup* show(GameSession& session, PopupKind kind, u64 data = 0);
    void showDeath(GameSession& session, u32 nowMs);
    void showResurrect(GameSession& session, u32 nowMs);
    void readEvents(GameSession& session, u32 nowMs);
    void readSocial(GameSession& session);
    void watchState(GameSession& session, u32 nowMs);
    void answer(GameSession& session, const PopupOutcome& outcome);
    std::string_view text(GameSession& session, const Popup& p, std::span<char> out, u32 nowMs);
    std::string_view button2Text(GameSession& session, const Popup& p);
    std::string_view playerName(GameSession& session, u64 guid);
    bool nearUnit(GameSession& session, u64 guid, u32 nowMs);
    void message(u32 color, std::string_view line);

    const game::GameData* m_data = nullptr;
    std::string m_abandonItems; // phase 18 (p3): ABANDON_QUEST_WITH_ITEMS' item names
    ChatNamer m_chatNamer = nullptr; // phase 18 (p3): NAME_CHAT's answer
    void* m_chatNamerContext = nullptr;
    UiSoundSink m_uiSound;
    MessageSink m_message = nullptr;
    void* m_messageContext = nullptr;
    bool m_trace = false; // VD_TRACE_GAMEPLAY: every popup shown and answered in the log

    PopupStack m_stack;
    std::array<f32, PopupStack::kSlots> m_heights{}; // StaticPopup1..4's last heights (their places)
    std::array<PopupView, PopupStack::kSlots> m_views{};
    std::array<std::array<char, 400>, PopupStack::kSlots> m_texts{};
    u32 m_eventCursor = 0;
    u32 m_socialCursor = 0;
    u64 m_cursorsPlayer = 0;
    u32 m_lastMs = 0;

    // Edges of the game state the popups follow.
    bool m_wasDeadBody = false; // dead, not released (DEATH)
    u32 m_diedAtMs = 0;         // the release countdown's start (6 minutes, PLAYER_FIELD_BYTE_RELEASE_TIMER)
    u64 m_lastResurrector = 0;
    bool m_corpseInRange = false; // RECOVER_CORPSE (CORPSE_IN_RANGE / CORPSE_OUT_OF_RANGE)
    bool m_corpseInInstance = false; // phase 12: RECOVER_CORPSE_INSTANCE (CORPSE_IN_INSTANCE)
    u64 m_lastSpiritHealer = 0;
    bool m_tradeProposed = false;

    // The action a soulbinding question holds back (only one: they are all exclusive).
    struct PendingBind {
        game::BindAsk ask = game::BindAsk::None;
        game::ItemPlace from, to;
        game::ItemAction action = game::ItemAction::Use;
        u32 itemEntry = 0;
        u8 lootSlot = 0;
        u64 lootGuid = 0;
    };
    PendingBind m_bind;
    u64 m_bindSerial = 0;

    // DELETE_GOOD_ITEM's and RENAME_PET's edit box (the typed confirmation, the pet's name); phase 15: the guild's
    // (SET_GUILDMOTD takes 128 letters).
    std::array<char, 136> m_editText{};
    u32 m_editLength = 0;
    bool m_focusEdit = false; // OnShow: EditBox:SetFocus() (the keyboard opens with it)
    void prefillEdit(std::string_view text); // phase 15 (guild): OnShow's SetText (the MOTD, a note)
    HelpOpener m_helpOpener = nullptr;       // phase 15 (help)
    void* m_helpContext = nullptr;
};

} // namespace vd
