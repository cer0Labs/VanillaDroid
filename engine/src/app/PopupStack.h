#pragma once

#include "core/Text.h"
#include "core/Types.h"

#include <array>

namespace vd {

// The client's StaticPopups this client shows (parity pass, popups): StaticPopupDialogs' entries
// (StaticPopup.lua), by name. TradeWaiting is ours: the trade request the player sent, waiting for an answer,
// with a way to cancel it.
enum class PopupKind : u8 {
    Death,               // DEATH: release, or a self-resurrection, with the auto-release countdown
    Resurrect,           // RESURRECT: someone resurrects the player (with resurrection sickness)
    ResurrectNoSickness, // RESURRECT_NO_SICKNESS: ... without sickness, once the corpse delay is over
    ResurrectNoTimer,    // RESURRECT_NO_TIMER: ... without sickness or delay
    RecoverCorpse,       // RECOVER_CORPSE: the ghost by its corpse
    XpLoss,              // XP_LOSS: the spirit healer (with resurrection sickness), asked twice
    XpLossNoSickness,    // XP_LOSS_NO_SICKNESS
    PartyInvite,         // PARTY_INVITE
    GuildInvite,         // GUILD_INVITE
    DuelRequested,       // DUEL_REQUESTED
    DuelOutOfBounds,     // DUEL_OUTOFBOUNDS: no buttons, the forfeit countdown
    Trade,               // TRADE: someone wants to trade
    TradeWaiting,        // ours: the player's own trade request, until answered (VD_EXCHANGE_TRADE_WAITING)
    QuestAccept,         // QUEST_ACCEPT: a quest shared by a group member
    AbandonQuest,        // ABANDON_QUEST
    DeleteItem,          // DELETE_ITEM
    DeleteGoodItem,      // DELETE_GOOD_ITEM: rare and better, typing DELETE_ITEM_CONFIRM_STRING enables Yes
    ConfirmBinder,       // CONFIRM_BINDER: the innkeeper
    ConfirmTalentWipe,   // CONFIRM_TALENT_WIPE: the class trainer, the cost in a money frame
    ConfirmSummon,       // CONFIRM_SUMMON
    Camp,                // CAMP: the logout countdown
    Quit,                // QUIT: the exit countdown
    LootBind,            // LOOT_BIND
    EquipBind,           // EQUIP_BIND
    AutoEquipBind,       // AUTOEQUIP_BIND
    UseBind,             // USE_BIND
    AbandonPet,          // ABANDON_PET
    ConfirmBuyBankSlot,  // CONFIRM_BUY_BANK_SLOT: the next bank bag slot, its price in a money frame
    // Phase 12, instances.
    ConfirmResetInstances, // CONFIRM_RESET_INSTANCES: the player's menu's Reset all instances
    RecoverCorpseInstance, // RECOVER_CORPSE_INSTANCE: the ghost at the entrance of the instance its corpse is in
    InstanceBoot,          // INSTANCE_BOOT: not in this instance's group, the countdown to the bind point
    // Phase 13 (spells): before an enchantment lands (game::EnchantAsk).
    BindEnchant,         // BIND_ENCHANT: enchanting will bind the item
    ReplaceEnchant,      // REPLACE_ENCHANT: "Do you want to replace %s with %s?" (data: old | new << 32)
    TradeReplaceEnchant, // TRADE_REPLACE_ENCHANT: the same on the trader's item
    // Phase 13 (social): the player's menus and slash commands.
    ConfirmGuildDisband, // CONFIRM_GUILD_DISBAND: /gdisband
    ConfirmGuildPromote, // CONFIRM_GUILD_PROMOTE: a guild member's menu, "Promote to Guildmaster" (name)
    ConfirmGuildLeave,   // CONFIRM_GUILD_LEAVE: the player's own name's menu in the guild roster, "Leave Guild"
    RenamePet,           // RENAME_PET: the pet's menu, "Rename" (an edit box, 12 letters)
    PetRenameConfirm,    // PETRENAMECONFIRM: "Name your pet '%s'?" (name)
    // Phase 15 (pets).
    ConfirmPetUnlearn, // CONFIRM_PET_UNLEARN: a pet trainer's untraining (data: the trainer), the cost in a money frame
    // Phase 15 (items): group loot.
    ConfirmLootRoll,         // CONFIRM_LOOT_ROLL: Need / Greed on a bind-on-pickup item (data: the object, data2: slot | vote << 32)
    ConfirmLootDistribution, // CONFIRM_LOOT_DISTRIBUTION: the master looter gives an epic (name: the item, name2: who)
    // Phase 15 (character).
    UnlearnSkill, // UNLEARN_SKILL: SkillFrame's Unlearn button, "Do you want to unlearn %s?" (data: the skill)
    // Phase 15 (guild): the guild frame's, the charter's and the help frame's.
    AddGuildMember,         // ADD_GUILDMEMBER: an edit box (12 letters): GuildInviteByName
    RemoveGuildMember,      // REMOVE_GUILDMEMBER: "remove %s from the guild?" (name): GuildUninviteByName
    AddGuildRank,           // ADD_GUILDRANK: an edit box (15 letters): GuildControlAddRank
    SetGuildMotd,           // SET_GUILDMOTD: a wide edit box (128 letters) with the MOTD: GuildSetMOTD
    SetGuildPlayerNote,     // SET_GUILDPLAYERNOTE: a wide edit box (31 letters) with the note (name: the member)
    SetGuildOfficerNote,    // SET_GUILDOFFICERNOTE: the same for the officer note
    RenameGuild,            // RENAME_GUILD: an edit box (24 letters): RenamePetition
    HelpTicket,             // HELP_TICKET: "Edit My Ticket" (the ticket page) / "Abandon My Ticket"
    HelpTicketAbandonConfirm, // HELP_TICKET_ABANDON_CONFIRM: DeleteGMTicket
    HelpTicketQueueDisabled,  // HELP_TICKET_QUEUE_DISABLED: OKAY alone
    // Phase 16 (raidframe): the raid tab's Add Member without a cooperative target.
    AddRaidMember, // ADD_RAIDMEMBER: an edit box (12 letters): InviteByName
    // Phase 16 (alerts).
    ConfirmLeaveQueue, // CONFIRM_LEAVE_QUEUE: MiniMapMeetingStoneFrame's click; CancelMeetingStoneRequest on accept
    // Phase 17 (honor): AREA_SPIRIT_HEAL (1.12.1's: shown in a spirit guide's range, it queues at once; its
    // only button cancels).
    AreaSpiritHeal,
    // Phase 17 (queue).
    ConfirmBattlefieldEntry, // CONFIRM_BATTLEFIELD_ENTRY: one per queue (data: its slot), name: the map
    // Phase 18 (glue).
    // Phase 18 (create).
    // Phase 18 (p3).
    AbandonQuestWithItems, // ABANDON_QUEST_WITH_ITEMS: the quest's items that go with it (GetAbandonQuestItems)
    DeleteMail,            // DELETE_MAIL: deleting a letter destroys its item (name: the item; data: the mail)
    DeleteMoney,           // DELETE_MONEY: ...or its money (the money frame; data: the mail)
    NameChat,              // NAME_CHAT: a chat window's name (data: its ChatFrame number; 0: a new window)
    // Retouches (windows): OpenMailPackageButton's click on a C.O.D. letter.
    CodAlert,        // COD_ALERT: not enough money to pay it (COD_INSUFFICIENT_MONEY)
    CodConfirmation, // COD_CONFIRMATION: the charge in its money frame (data: the mail)
    // Retouches (friends frame): FriendsFrameAddFriendButton / FriendsFrameIgnorePlayerButton without a cooperative
    // target.
    AddFriend, // ADD_FRIEND: an edit box (12 letters): AddFriend
    AddIgnore, // ADD_IGNORE: an edit box (12 letters): AddIgnore
    Count
};

// A StaticPopupDialogs entry: its texts (keys of core/Text; empty: none) and the rules StaticPopup_Show
// applies.
struct PopupInfo {
    // Flags (the table's fields of the same names).
    static constexpr u16 kWhileDead = 1 << 0;    // shown while dead (else refused, and closed by DEATH)
    static constexpr u16 kExclusive = 1 << 1;    // replaces another exclusive one
    static constexpr u16 kHideOnEscape = 1 << 2; // Escape cancels it
    static constexpr u16 kNotClosableByLogout = 1 << 3; // CAMP and QUIT leave it
    static constexpr u16 kShowAlert = 1 << 4;           // wider, with the DialogAlertIcon
    static constexpr u16 kMoneyFrame = 1 << 5;          // a money frame under the text
    static constexpr u16 kEditBox = 1 << 6;             // an edit box under the text
    static constexpr u16 kCountdown = 1 << 7;    // its text counts the time left down (StaticPopup_OnUpdate)
    static constexpr u16 kKeepOnAccept = 1 << 8; // OnAccept returns 1: stays up (RECOVER_CORPSE)
    static constexpr u16 kMultiple = 1 << 9;     // phase 17 (queue): one per data (StaticPopup_FindVisible)

    text::Key text;
    text::Key delayText; // while the start delay runs (button 1 disabled)
    text::Key againText; // asked again before button 1 acts (XP_LOSS: OnAccept with data)
    text::Key button1;
    text::Key button2;
    u32 timeoutMs = 0; // 0: none
    u16 flags = 0;
    PopupKind cancels = PopupKind::Count; // closes that one when shown (Count: none)

    bool has(u16 flag) const { return (flags & flag) != 0; }
};
const PopupInfo& popupInfo(PopupKind kind);

// One of the four dialogs (StaticPopup1..4): a kind and what it is about. Count: the slot is free.
struct Popup {
    PopupKind kind = PopupKind::Count;
    u64 data = 0; // what it is about (a GUID, a quest, an item's place...): the caller's
    u64 data2 = 0;
    u32 money = 0;               // kMoneyFrame: copper shown under the text
    std::array<char, 64> name{}; // names the text needs that may not stay known (an inviter, a guild)
    std::array<char, 64> name2{};
    i32 timeLeftMs = 0;   // to the timeout (0: none): dialog.timeleft
    i32 startDelayMs = 0; // button 1 waits until then (dialog.startDelay): the corpse delay
    bool again = false;   // the second question of againText is shown
    bool button1Enabled =
        true;                 // the caller's OnUpdate (CONFIRM_SUMMON in combat, DELETE_GOOD_ITEM not typed)
    bool button2Shown = true; // DisplayButton2 (DEATH: only with a self-resurrection)

    bool shown() const { return kind != PopupKind::Count; }
    void setName(std::string_view s) { copy(name, s); }
    void setName2(std::string_view s) { copy(name2, s); }
    std::string_view nameView() const { return name.data(); }
    std::string_view name2View() const { return name2.data(); }

private:
    static void copy(std::array<char, 64>& out, std::string_view s) {
        const usize n = s.size() < out.size() - 1 ? s.size() : out.size() - 1;
        for (usize i = 0; i < n; ++i) {
            out[i] = s[i];
        }
        out[n] = '\0';
    }
};

// How a popup ended (StaticPopup.lua's OnAccept / OnCancel(reason); OnHide follows every one that was shown).
enum class PopupEnd : u8 {
    Accept,   // button 1 (OnAccept)
    Cancel,   // button 2 or Escape (OnCancel "clicked")
    Override, // closed by another popup's rules (OnCancel "override")
    Timeout,  // its time ran out (OnCancel "timeout")
    Refused,  // never shown: the player is dead, or the four slots are taken (OnCancel())
    Hidden,   // hidden by the game (StaticPopup_Hide): OnHide only
};
struct PopupOutcome {
    PopupKind kind = PopupKind::Count;
    PopupEnd end = PopupEnd::Hidden;
    u64 data = 0;
    u64 data2 = 0;
    i32 timeLeftMs = 0; // what was left of its timeout (CAMP's OnHide cancels the logout when > 0)
    bool shown = false; // it was on screen (OnHide runs, its close sound plays)
    bool stays = false; // Accept of a kKeepOnAccept popup: still on screen
    std::array<char, 64> name{}; // phase 13 (social): the popup's name (CONFIRM_GUILD_PROMOTE's, PETRENAMECONFIRM's)
};

// The four StaticPopup dialogs as StaticPopup.lua runs them, without drawing (app/PopupWindows draws them,
// app/PopupScreens drives them from the game): StaticPopup_Show's rules (refused while dead unless whileDead,
// exclusive, cancels, CAMP / QUIT and DEATH closing the others, the same kind replaced in its own slot, the
// first free of four slots, refused when none is free), timeouts and start delays (StaticPopup_OnUpdate), the
// buttons (StaticPopup_OnClick), Escape (StaticPopup_EscapePressed) and StaticPopup_Hide. What happens comes
// out as outcomes, in order, for the caller's OnAccept / OnCancel / OnHide. Not modal: nothing here blocks
// the game. Fixed storage (no allocation).
class PopupStack {
public:
    static constexpr u32 kSlots = 4; // STATICPOPUP_NUMDIALOGS

    // StaticPopup_Show(kind) about `data`. `playerDead`: UnitIsDeadOrGhost("player"). Returns the popup to
    // fill (names, money, its timeleft for the kinds whose OnShow sets it), or nullptr when refused (a
    // Refused outcome).
    Popup* show(PopupKind kind, bool playerDead, u64 data = 0);
    // StaticPopup_Hide: every shown popup of `kind` (and `data`, for the second form). Hidden outcomes.
    void hide(PopupKind kind);
    void hide(PopupKind kind, u64 data);
    Popup* find(PopupKind kind);
    const Popup* find(PopupKind kind) const;
    bool visible(PopupKind kind) const { return find(kind) != nullptr; }
    bool any() const;
    u32 count() const;

    // StaticPopup_OnUpdate, `elapsedMs` since the last call: timeouts (Timeout outcomes) and start delays.
    void update(u32 elapsedMs);
    // StaticPopup_OnClick: button 1 or 2 of slot `index` (a disabled button 1 does nothing).
    void click(u32 index, u32 button);
    // StaticPopup_EscapePressed: every hideOnEscape popup is cancelled. True when one was.
    bool escape();
    // Everything goes at once (a new world): Hidden outcomes.
    void clear();

    // The next outcome, oldest first; false when there is none.
    bool takeOutcome(PopupOutcome& out);

    const std::array<Popup, kSlots>& slots() const { return m_slots; }
    Popup& slot(u32 index) { return m_slots[index]; }
    // Button 1 can be tapped (no start delay, and the caller allows it).
    static bool button1Ready(const Popup& p) { return p.startDelayMs <= 0 && p.button1Enabled; }

private:
    void close(u32 index, PopupEnd end);
    void push(const PopupOutcome& outcome);

    std::array<Popup, kSlots> m_slots{};
    static constexpr u32 kOutcomes = 32;
    std::array<PopupOutcome, kOutcomes> m_outcomes{};
    u32 m_first = 0;
    u32 m_count = 0;
};

} // namespace vd
