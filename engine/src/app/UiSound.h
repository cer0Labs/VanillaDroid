#pragma once

#include "core/Types.h"

namespace vd {

// Interface sounds (phase 9): the SoundEntries the original FrameXML plays with PlaySound("...") when its frames
// open, close and act. The windows say what happened through a UiSoundSink (GameScreens for the windows the player
// opens); app/GameAudio plays it, and follows by itself what the server opens (NPC, loot, vendor, bank, trade,
// auction house, mailbox, taxi map), targets, quests, level up and whispers.
enum class UiSound : u8 {
    GameMenuOpen,   // igMainMenuOpen: the game menu; also popups, bank, books, taxi map, the social window
    GameMenuClose,  // igMainMenuQuit: the game menu closed
    PanelClose,     // igMainMenuClose: popups, bank, books, taxi map, the social window closed
    GameMenuOption, // igMainMenuOption: a game menu button opens another window
    Logout,         // igMainMenuLogout
    QuestLogOpen,   // igQuestLogOpen: the quest log, the world map
    QuestLogClose,  // igQuestLogClose
    QuestAbandon,   // igQuestLogAbandonQuest
    NpcOpen,        // igQuestListOpen: gossip and quest windows of an NPC
    NpcClose,       // igQuestListClose
    QuestSelect,    // igQuestListSelect: a quest of the list
    QuestComplete,  // igQuestListComplete: reward taken
    QuestDecline,   // igQuestCancel
    QuestAdded,     // QUESTADDED: a quest accepted
    QuestFailed,    // igQuestFailed
    LootEmpty,      // LOOTWINDOWOPENEMPTY
    LootCoins,      // LOOTWINDOWCOINSOUND
    FishingReel,    // FISHING REEL IN
    TargetEnemy,    // igCreatureAggroSelect
    TargetFriend,   // igCharacterNPCSelect
    TargetNeutral,  // igCreatureNeutralSelect
    TargetLost,     // INTERFACESOUND_LOSTTARGETUNIT
    CheckBoxOn,     // igMainMenuOptionCheckBoxOn
    CheckBoxOff,    // igMainMenuOptionCheckBoxOff
    CharacterOpen,  // igCharacterInfoOpen: the character sheet (skills, reputation), vendor, trade, mailbox
    CharacterClose, // igCharacterInfoClose
    CharacterTab,   // igCharacterInfoTab
    BagOpen,        // igBackPackOpen
    BagClose,       // igBackPackClose
    SpellBookOpen,  // igSpellBookOpen
    SpellBookClose, // igSpellBookClose
    PageTurn,       // igAbiliityPageTurn (sic)
    TalentsOpen,    // TalentScreenOpen
    TalentsClose,   // TalentScreenClose
    AuctionOpen,    // AuctionWindowOpen
    AuctionClose,   // AuctionWindowClose
    Repair,         // ITEM_REPAIR
    Whisper,        // TellMessage: a whisper came in
    MapPing,        // MapPing
    ChatScroll,     // UChatScrollButton
    LevelUp,        // LEVELUPSOUND
    EnterWorld,     // GLUEENTERWORLDBUTTON (the character screen's enter button)
    MinimapZoomIn,  // igMiniMapZoomIn (parity pass: Minimap.lua)
    MinimapZoomOut, // igMiniMapZoomOut
    MinimapOpen,    // igMiniMapOpen: MinimapToggleButton shows the map
    MinimapClose,   // igMiniMapClose: ...hides it
    // Parity pass (audio).
    PlayerInvite,       // igPlayerInvite: StaticPopup's PARTY_INVITE and DUEL_REQUESTED
    TaxiNodeDiscovered, // TaxiNodeDiscovered: SMSG_NEW_TAXI_PATH (the client's code)
    RaidWarning,        // RaidWarning: RaidWarningFrame
    ItemGrab,           // INTERFACESOUND_CURSORGRABOBJECT: an item without group sounds picked up
    ItemPlace,          // INTERFACESOUND_CURSORDROPOBJECT: ...put down
    // Phase 15.
    InventoryRotate, // igInventoryRotateCharacter: a model frame's rotate button (Model_RotateLeft / Right)
    // Phase 15 (pets).
    AbilityOpen,  // igAbilityOpen: the spell book shows the pet's page (SpellBookFrame_Update, BOOKTYPE_PET)
    AbilityClose, // igAbilityClose
    // Phase 15 (items).
    KeyRingOpen,   // KeyRingOpen: the keyring's ContainerFrame shown
    KeyRingClose,  // KeyRingClose
    TitleOptionOk, // gsTitleOptionOK: the dressing rooms' Reset
    BagMenuButton, // BAGMENUBUTTONPRESS: a bag slot button opens or closes its bag
    // Phase 16 (alerts).
    ReadyCheck, // ReadyCheck: ShowReadyCheck (Blizzard_RaidUI), ReadyCheckFrame shown to a raid member
    // Phase 17 (queue): BattlefieldFrame_UpdateStatus.
    PvpEnterQueue,   // PVPENTERQUEUE: a battleground queue joined
    PvpThroughQueue, // PVPTHROUGHQUEUE: its battle is ready (CONFIRM_BATTLEFIELD_ENTRY)
    // Phase 18 (create): CharacterCreate.lua.
    CharCreateClass,  // gsCharacterCreationClass: a race, class or gender button
    CharCreateLook,   // gsCharacterCreationLook: a customization arrow, Randomize, the random name
    CharCreateCreate, // gsCharacterCreationCreateChar: Accept (CharacterCreate_Okay)
    CharCreateCancel, // gsCharacterCreationCancel: Back (CharacterCreate_Back)
    // Phase 18 (glue): the glue screens' PlaySound (AccountLogin.lua, RealmList.lua, CharacterSelect.xml / .lua).
    GlueLogin,             // gsLogin: AccountLogin_Login
    GlueQuit,              // gsTitleQuit: AccountLogin_Exit
    GlueOptionExit,        // gsTitleOptionExit: CharacterDeleteButton2 (Cancel)
    GlueRealmSelect,       // gsLoginChangeRealmSelect: CharacterSelect_ChangeRealm
    GlueRealmOk,           // gsLoginChangeRealmOK: RealmList_OnOk
    GlueRealmCancel,       // gsLoginChangeRealmCancel: RealmList_OnCancel
    GlueEnterWorld,        // gsCharacterSelectionEnterWorld: CharacterSelect_EnterWorld
    GlueCharacterExit,     // gsCharacterSelectionExit: CharacterSelect_Exit (Back)
    GlueDeleteCharacter,   // gsCharacterSelectionDelCharacter: CharacterSelect_Delete
    GlueCreateCharacter,   // gsCharacterSelectionCreateNew: CharacterSelect_SelectCharacter(createIndex)
    // Phase 18 (p3).
    TutorialPopup, // TutorialPopup: TriggerTutorial (WoW.exe 0x4B5497): a tutorial's alert button appears
    // Phase 20 (smart).
    BonusBarOpen, // igBonusBarOpen: BonusActionBarFrame shown; the touch cluster's reactive ability appearing
    Count
};

// Where the UI reports its sounds: a plain function and its context (no allocation, no virtual). Empty: silent.
struct UiSoundSink {
    void (*play)(void* context, UiSound sound) = nullptr;
    void* context = nullptr;
    // Parity pass: an item the finger picks up (true) or puts down, by its entry (its ItemGroupSounds).
    void (*item)(void* context, u32 itemEntry, bool pickup) = nullptr;

    void operator()(UiSound sound) const {
        if (play) {
            play(context, sound);
        }
    }
    void itemSound(u32 itemEntry, bool pickup) const {
        if (item) {
            item(context, itemEntry, pickup);
        }
    }
};

} // namespace vd
