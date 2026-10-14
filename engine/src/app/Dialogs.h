#pragma once

#include "app/ScreenEdges.h"
#include "core/Types.h"
#include "ui/Context.h"
#include "ui/DrawList.h"
#include "ui/TextInput.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

class ChatLinkInsert;
class UiResources;

// The touch UI's windows (phase 7): NPC conversations, quests, the quest log and tracker, loot, chat, the
// game menu, settings and confirmations, drawn with ui/Widgets over the client's art. Pure views: each
// function draws a view-model the caller fills from the game state and returns what the player did this
// frame; none touches the network or the game. Modal dialogs cover the screen with a blocker (no finger
// reaches the world or the HUD under them), are centered in the safe area and scroll their body when it does
// not fit. Call them after the HUD, in the same Context and DrawList, every frame they are open.
//
// Text the server sends (NPC and quest text: enUS client data) is drawn as is; "$B" breaks lines and |c..|r
// color codes apply. The interface's own words come from core/Text by key (the client's GlobalStrings /
// GlueStrings, or ours in locale/*/common.lang): English by default, translated by the configured locale.

// Screen shapes the layouts are made for.
enum class ScreenProfile : u8 {
    Wide,   // 20:9 phones and the Fold5's 23:9 outer screen: short, wide dialogs in two columns
    Square, // the Fold5's inner screen (~6:5): taller dialogs in one column
};

// Parity pass (panels): where a window smaller than its panel's place sits in it.
enum class PanelAlign : u8 { TopLeft, BottomRight, Center };

struct DialogContext {
    ui::Context& ui;
    ui::DrawList& draw;
    UiResources& resources;
    ScreenProfile profile;
    ui::Rect safeArea;   // logical pixels
    // Phase 20 (look): beyond the safe area where no display cutout reaches (the chat at the bottom left). Unknown
    // (edges.known false): the safe area.
    ScreenEdges edges;
    u32 playerLevel = 0; // colors quest levels (0: unknown, all gold)
    // Parity pass (panels): not empty, the window is a panel in this place (app/UiPanels: the original's left /
    // center slots, the bags' corner) instead of a modal dialog: no blocker (the stick, the camera and the HUD keep
    // working around it), a tap outside does not close it, its size is at most the place's.
    ui::Rect panel;
    PanelAlign panelAlign = PanelAlign::TopLeft;
    // Ours: nothing under the window shows through (the windows over the unit frames on the wide screens: the
    // original's MerchantFrame, QuestFrame... are opaque art).
    bool panelOpaque = false;
    // Phase 13: while the chat's edit box is open, a tap on an item, a recipe or a quest inserts its link there
    // instead of acting (app/ChatLinkInsert: the desktop's Shift + click). Null outside the game.
    ChatLinkInsert* chatLink = nullptr;
};

struct ItemView {
    u64 iconHash =
        0; // assetPathHash of the icon texture in textures.vdpk ("Interface\\Icons\\INV_Misc_Bag_07.blp")
    std::string_view name;
    u32 count = 1;
    u32 quality = 1; // 0 poor .. 5 legendary
    u32 entry = 0;   // the item (a long press on its cell shows its tooltip: kit::takeLongPressedItem); 0 = none
    bool usable = true; // false: a quest reward the player cannot use, tinted red (kit::UnusableTint)
};

// --- NPC conversation (SMSG_GOSSIP_MESSAGE): its text, options and quests ----------------------------------
// Quest windows: GossipFrame and QuestFrame with their 1.12 art (app/QuestFrameLayout): the NPC's portrait in the ring,
// its name in the title bar, the parchment with its scroll bar, the buttons in their slots; the rewards two to a row
// (QuestFrameItems_Update).

// Who the window is with (QuestFrame_SetPortrait / GossipFrameUpdate): its name and, for a unit, its portrait
// (SetPortraitTexture: app/Portraits, invalid while it is drawn); an object (UnitExists("npc") false) shows the quest
// log's book.
struct NpcPortraitView {
    TextureHandle portrait;
    bool unit = false;
};

struct GossipOptionView {
    std::string_view text;
    u32 icon = 0; // vMaNGOS GossipOptionIcon (0 chat, 1 vendor, 2 taxi, 3 trainer, ...)
};
struct GossipQuestView {
    std::string_view title;
    u32 level = 0;
    bool active = false;      // already taken (the "?" icon; "!" otherwise)
    bool completable = false; // ready to turn in (a golden "?", gray when not)
};
struct GossipView {
    std::string_view npcName;
    std::string_view text;
    std::span<const GossipOptionView> options;
    std::span<const GossipQuestView> quests;
    NpcPortraitView npc;
};
struct GossipResult {
    i32 option = -1; // index into options tapped this frame
    i32 quest = -1;  // index into quests tapped this frame
    bool closed = false;
    bool scrolled = false; // a scroll bar's button scrolled (PlaySound("UChatScrollButton"))
};
GossipResult gossipDialog(DialogContext& c, const GossipView& v);

// --- Quests (SMSG_QUESTGIVER_QUEST_DETAILS / _REQUEST_ITEMS / _OFFER_REWARD) -------------------------------

struct QuestDetailsView {
    std::string_view npcName, title, description, objectives;
    std::span<const ItemView> rewards; // all of them are given
    std::span<const ItemView> choices; // one of them is chosen at the end
    u32 money = 0;                     // copper
    u32 xp = 0;
    NpcPortraitView npc;
};
struct QuestDetailsResult {
    bool accept = false;
    bool decline = false; // declined or closed
    bool scrolled = false;
};
QuestDetailsResult questDetailsDialog(DialogContext& c, const QuestDetailsView& v);

struct QuestProgressView {
    std::string_view npcName, title, text;
    std::span<const ItemView> required;
    u32 money = 0; // copper required
    bool completable = false;
    NpcPortraitView npc;
};
struct QuestProgressResult {
    bool proceed = false; // CONTINUE (only when completable)
    bool closed = false;
    bool scrolled = false;
};
QuestProgressResult questProgressDialog(DialogContext& c, const QuestProgressView& v);

struct QuestRewardView {
    std::string_view npcName, title, text;
    std::span<const ItemView> rewards;
    std::span<const ItemView> choices;
    u32 money = 0;
    u32 xp = 0;
    NpcPortraitView npc;
};
struct QuestRewardResult {
    i32 choice = -1;       // the chosen reward when completing (index into choices; -1 without choices)
    bool complete = false; // COMPLETE_QUEST: only once a choice is selected, when there are choices
    bool closed = false;
    bool scrolled = false;
};
// `selectedChoice`: the reward the player has picked so far (-1 none), kept by the caller between frames.
QuestRewardResult questRewardDialog(DialogContext& c, const QuestRewardView& v, i32& selectedChoice);

// --- Quest objectives (phase 13: the quest log, QuestWatchFrame and QuestTimerFrame are app/QuestWindows) -------

struct ObjectiveView {
    std::string_view text; // "Kobold Vermin", "Tough Wolf Meat", or the quest's own text for it
    u32 have = 0;
    u32 need = 0;      // 0: a text-only objective
    bool kill = false; // `text` is a creature to kill: "Kobold Vermin slain: 3/10" (QUEST_MONSTERS_KILLED)
};
// A quest of the samples (gallery pages, the panel demo).
struct QuestLogEntryView {
    std::string_view title, description, objectivesText;
    u32 level = 0;
    bool complete = false;
    bool failed = false;
    std::span<const ObjectiveView> objectives;
};

// --- Loot (SMSG_LOOT_RESPONSE) -----------------------------------------------------------------------------

struct LootItemView {
    ItemView item;
    u32 slot = 0; // the server's loot slot
    // Phase 8, group loot: why it cannot be taken now ("Being rolled for", the master looter...), shown
    // dimmed over the row; empty: it can.
    std::string_view note;
};
struct LootView {
    std::string_view source; // what is looted (the title)
    std::span<const LootItemView> items;
    u32 money = 0;
    u32 page = 0; // LootFrame's page (four slots a page, three when there are more)
};
struct LootResult {
    i32 take = -1; // index into items tapped this frame (items[take].slot is the loot slot)
    ui::Rect takeRect; // phase 15 (items): its button (the master looter's menu hangs from it)
    bool takeMoney = false;
    i32 page = -1; // the page asked for (-1: no change)
    bool closed = false;
};
LootResult lootWindow(DialogContext& c, const LootView& v);

// --- Chat --------------------------------------------------------------------------------------------------

struct ChatLineView {
    std::string_view text;  // may hold |cAARRGGBB..|r codes
    u32 color = 0xFFFFFFFF; // RGBA8 (ui::rgba), the line's base color
};
struct ChatView {
    std::span<const ChatLineView> lines; // oldest first (the newest 256 are shown)
    std::string_view input;
    bool editing = false;     // the input has the keyboard (caret shown)
    std::string_view channel; // "Say", "Yell", "Party"... (CHAT_MSG_SAY when empty)
    bool expanded = false;    // expanded: background, scrolling and the input row; collapsed: the last lines
};
struct ChatResult {
    bool tappedInput = false;
    bool send = false;
    bool cycleChannel = false;
    bool toggleExpanded = false;
};
// Not modal. Newest line at the bottom; the expanded view scrolls (and follows new lines while at the end).
ChatResult chatWindow(DialogContext& c, const ui::Rect& area, const ChatView& v);

// --- Menu, settings, confirmation --------------------------------------------------------------------------

// The game menu's entries (phase 8 adds the character's windows; each is wired in GameScreens).
enum class MenuChoice : u8 {
    None,
    Resume,
    Character, // equipment and stats
    Bags,
    Spellbook,
    Talents,
    QuestLog,
    Social, // group, friends, guild, who
    WorldMap,
    Settings, // VIDEOOPTIONS_MENU (OptionsFrame)
    Logout,
    Skills,     // phase 8: weapon skills, professions, languages
    Reputation, // phase 8: factions
    Quit,       // parity (popups): EXIT_GAME
    // Phase 15 (options): GameMenuFrame's other buttons.
    SoundOptions, // SOUNDOPTIONS_MENU (SoundOptionsFrame)
    UiOptions,    // UIOPTIONS_MENU (UIOptionsFrame)
    Macros,       // MACROS (ShowMacroFrame)
};
// `logoutEnabled`: Logout and Exit Game can be pressed (GameMenuFrame disables them while CAMP or QUIT is
// up).
MenuChoice gameMenu(DialogContext& c, bool logoutEnabled = true);

struct SettingsView {
    f32 uiScale = 1.0f;
    f32 cameraSpeed = 1.0f;
    bool invertCamera = false;
    bool showFps = true;
    f32 viewDistance = 700.0f; // yards
    // Phase 9: sound volumes (0..1), the master and one per category. Phase 14: music and ambience start where 1.12's
    // CVars do (MusicVolume "0.4", AmbienceVolume "0.6"; MasterVolume and SoundVolume "1.0").
    f32 masterVolume = 1.0f;
    f32 musicVolume = 0.4f;
    f32 ambienceVolume = 0.6f;
    f32 effectsVolume = 1.0f;
    f32 voicesVolume = 1.0f;
    f32 interfaceVolume = 1.0f;
    // Parity pass: the minimap's zoom levels outdoors and indoors (CVars minimapZoom / minimapInsideZoom, 0..5).
    u32 minimapZoom = 3;
    u32 minimapInsideZoom = 3;
    // Parity (world feedback): chat bubbles (CVars ChatBubbles "1" / ChatBubblesParty: phase 15, "0" in WoW.exe) and
    // the name plates the client toggles with V and Shift-V (off by default).
    bool chatBubbles = true;
    bool partyChatBubbles = false;
    bool enemyPlates = false;
    bool friendPlates = false;
    // Phase 10: video options. The preset last chosen (QualityPreset; Custom once a knob moves on its own) and its
    // knobs (viewDistance above is the fourth), the dynamic quality (ADPF: temperature and missed frames lower the
    // resolution, the distances and the effects for a while) and the 30 fps limit (battery saver).
    u32 graphicsQuality = 2; // High
    f32 renderScale = 1.0f;
    f32 environmentDetail = 1.0f;
    f32 spellDetail = 1.0f;
    bool dynamicQuality = true;
    bool limitFps = false;
    // Phase 11: the client's Weather Intensity (WEATHER_DETAIL, the CVar weatherDensity 0..3), set by the presets.
    u32 weatherDensity = 3;
    // Phase 11: the client's full-screen effects (OptionsFrame FULL_SCREEN_GLOW / DEATH_EFFECT, CVars ffxGlow /
    // ffxDeath, both on by default): the glow that softens the light, and the ghost's grey-blue world.
    bool fullScreenGlow = true;
    bool deathEffect = true;
    // Phase 11 (f11/interiors): "Terrain Highlights" (CVar specular): specular highlights on terrain and water.
    bool terrainHighlights = true;
    // Phase 14 (grass): the terrain's grass, flowers, bushes and pebbles, 0 none, 1 low, 2 as 1.12 (its frillDensity 16,
    // which no 1.12 option changes), 3 double. Set by the presets.
    u32 groundClutter = 2;
    // Phase 18 (world): OptionsFrame's ANISOTROPIC slider (CVar anisotropic: 1, 2, 4, 8, 16) and its MULTISAMPLE
    // drop-down (1, 2, 4, 8 samples), both off (1) but on Ultra; capped by what the GPU offers (the max* below, set by
    // the game, not saved).
    u32 anisotropy = 1;
    u32 multisample = 1;
    u32 maxAnisotropy = 16;
    u32 maxMultisample = 4;
    // Phase 13 (quests): "Automatic Quest Tracking" (UIOptionsFrame AUTO_QUEST_WATCH, "1" by default): an objective
    // that progresses watches its quest for five minutes.
    bool autoQuestWatch = true;

    // Phase 15 (options): the rest of UIOptionsFrame and SoundOptionsFrame, each with 1.12's default (its CVar's in
    // WoW.exe, or UIOptionsFrame_Init's value of its saved variable), but four deliberately otherwise (auto self cast,
    // NPC names and buff durations on, the camera never following; SettingsFile's version 2 moves the files written
    // before to them). The extra action bars' toggles, the helm and the cloak are the server's (PLAYER_FIELD_BYTES,
    // PLAYER_FLAGS: app/OptionsScreens), not saved here.
    // Controls.
    bool deselectOnClick = true; // GAMEFIELD_DESELECT_TEXT unchecked: a tap on the ground clears the target
    bool assistAttack = false;   // ASSIST_ATTACK: /assist starts attacking
    bool autoClearAfk = true;    // CLEAR_AFK: moving or talking ends /afk
    bool blockTrades = false;    // BLOCK_TRADES
    bool autoSelfCast = true;    // AUTO_SELF_CAST_TEXT: a helpful spell without a friendly target goes on the player
    // Ours (1.12 only has Shift + click): the loot window opens and everything the player may take goes to the bags.
    bool autoLoot = false;
    // Ours: an item's tooltip shows the comparison tooltips at once (off: its "Compare" button does).
    bool alwaysCompare = false;
    // Display.
    bool statusBarText = false;      // STATUS_BAR_TEXT: the bars' numbers always (else while touched)
    bool showPlayerNames = true;     // SHOW_PLAYER_NAMES (UnitNamePlayer)
    bool showGuildNames = true;      // SHOW_GUILD_NAMES (UnitNamePlayerGuild)
    bool showPlayerTitles = true;    // SHOW_PLAYER_TITLES (UnitNamePlayerPVPTitle)
    bool showNpcNames = true;        // SHOW_NPC_NAMES (UnitNameNPC)
    bool showOwnName = false;        // SHOW_OWN_NAME (UnitNameOwn)
    bool showPartyBackground = false; // SHOW_PARTY_BACKGROUND_TEXT
    bool showBuffDurations = true;   // SHOW_BUFF_DURATION_TEXT
    bool hideOutdoorWorldState = false; // HIDE_OUTDOOR_WORLD_STATE_TEXT (the zone objectives: F17)
    bool showNewbieTips = true;         // SHOW_NEWBIE_TIPS_TEXT: the gold help under the micro buttons' names
    bool showGameTips = true;           // SHOW_TIPOFTHEDAY_TEXT (showGameTips): a GameTips.dbc line over the loading bar
    // Phase 20 (retouches): the gameTip CVar, GameTips.dbc's row of the next loading screen's tip (not an option:
    // Okay, Cancel and Defaults leave it).
    u32 gameTip = 0;
    // Camera.
    u32 cameraFollowStyle = 0;           // CAMERA_FOLLOWING_STYLE (cameraSmoothStyle): 0 never, 1 smart, 2 always
    bool cameraTerrainTilt = false;      // FOLLOW_TERRAIN
    bool cameraBobbing = false;          // HEAD_BOB
    bool cameraWaterCollision = true;    // WATER_COLLISION
    f32 cameraFollowSpeed = 180.0f;      // AUTO_FOLLOW_SPEED (cameraYawSmoothSpeed), degrees a second
    f32 cameraDistanceMaxFactor = 1.0f;  // MAX_FOLLOW_DIST (cameraDistanceMaxFactor x cameraDistanceMax 15 yd)
    // Action bars.
    bool lockActionBars = false;      // LOCK_ACTIONBAR_TEXT
    bool alwaysShowMultiBars = false; // ALWAYS_SHOW_MULTIBARS_TEXT: the extra bars' empty buttons too
    // Phase 20 (docs/MOBILE_CONTROLS.md): the touch cluster instead of the 1.12 bars (off: the classic bars), the whole
    // HUD mirrored for the left hand (the cluster on the left, the stick on the right).
    bool touchControls = true;
    bool touchPetBar = false; // phase 20 (pets): the 1.12 pet bar under the cluster too (the classic bars: always)
    bool touchLeftHanded = false;
    // phase 20 (assists): settings here
    // Phase 20 (assists, docs/MOBILE_CONTROLS.md §8): the touch cluster's assists, §8's defaults (app/TouchAssists).
    bool touchAutoTarget = true;     // a hostile spell without a hostile target targets the Target button's unit
    bool touchStartAttack = true;    // Heroic Strike, Raptor Strike, the seals start the auto attack
    bool touchRepeatHeld = true;     // a held button fires again whenever it can
    bool touchStanceAssist = true;   // warrior: a spell of another stance takes that stance first
    bool touchFormAssist = false;    // druid: a caster spell in an animal form leaves the form first
    bool touchDragToFrame = true;    // a helpful spell dragged onto a unit frame lands on that unit
    bool touchAimGround = true;      // a ground spell dragged out of its button aims around the player
    bool touchSpellQueue = false;    // a press in the last 200 ms of the global cooldown waits for its end
    bool touchHaptics = true;        // vibration
    bool touchSiteTemplates = false; // the template set for a dungeon, raid, battleground or the world on entering it
    // Chat.
    bool guildMemberAlert = false; // GUILDMEMBER_ALERT (guildMemberNotify)
    bool showLootSpam = true;      // SHOW_LOOT_SPAM: every roll of a group loot (else the winner only)
    // Raid and party.
    bool hidePartyInterface = false;    // HIDE_PARTY_INTERFACE_TEXT: no party frames in a raid
    bool showPartyPets = true;          // SHOW_PARTY_PETS_TEXT (PartyMemberPetFrame: app f15/pets reads it)
    // Phase 16 (pullouts): the raid pullouts' filters start off as RaidFrame_OnLoad sets them ("0"); the window's
    // Defaults button checks them (UIOptionsFrameCheckButtons' "1": OptionsScreens::interfaceDefaults).
    bool showDispellableDebuffs = false; // SHOW_DISPELLABLE_DEBUFFS_TEXT
    bool showCastableBuffs = false;      // SHOW_CASTABLE_BUFFS_TEXT
    bool showTargetOfTarget = false;    // SHOW_TARGET_OF_TARGET_TEXT
    u32 targetOfTargetState = 5;        // SHOW_TARGET_OF_TARGET_STATE: 1 raid, 2 party, 3 solo, 4 raid or party, 5 always
    // Combat text (Blizzard_CombatText, and the damage over the units).
    bool showCombatText = false;      // SHOW_COMBAT_TEXT_TEXT
    u32 combatTextFloatMode = 1;      // COMBAT_TEXT_FLOAT_MODE: 1 up, 2 down, 3 arc
    bool combatTextLowHealthMana = true;
    bool combatTextAuras = true;
    bool combatTextAuraFade = false;
    bool combatTextCombatState = true;
    bool combatTextDodgeParryMiss = false;
    bool combatTextResistances = false;
    bool combatTextReputation = false;
    bool combatTextReactives = false;
    bool combatTextFriendlyNames = false;
    bool combatTextComboPoints = false;
    bool combatTextMana = false;
    bool combatTextHonorGained = true;
    bool showDamage = true;           // SHOW_DAMAGE_TEXT (CombatDamage): the damage over the units
    bool logPeriodicEffects = true;   // LOG_PERIODIC_EFFECTS (CombatLogPeriodicSpells): the periodic damage too
    bool showPetDamage = true;        // SHOW_PET_MELEE_DAMAGE (PetMeleeDamage and PetSpellDamage)
    // Sound options (SoundOptionsFrame's switches; the volumes are above).
    bool soundEffects = true;     // ENABLE_ALL_SOUND (MasterSoundEffects): every sound but the music
    bool ambienceSounds = true;   // ENABLE_AMBIENCE
    bool errorSpeech = true;      // ENABLE_ERROR_SPEECH
    bool musicEnabled = true;     // ENABLE_MUSIC
    bool soundAtCharacter = true; // ENABLE_SOUND_AT_CHARACTER (SoundListenerAtCharacter)
    bool emoteSounds = true;      // ENABLE_EMOTE_SOUNDS
    bool musicLooping = false;    // ENABLE_MUSIC_LOOPING (SoundZoneMusicNoDelay): no silence between tracks
    // Phase 17 (scores): SHOW_BATTLEFIELD_MINIMAP (WorldStateAlwaysUpFrame_OnLoad's "0", saved) and
    // Blizzard_BattlefieldMinimap's BattlefieldMinimapOptions (BattlefieldMinimapDefaults: opacity 0.7, locked,
    // showPlayers; per character in 1.12, the device's here).
    bool showBattlefieldMinimap = false;
    f32 battlefieldMinimapOpacity = 0.7f;
    bool battlefieldMinimapLocked = true;
    bool battlefieldMinimapShowPlayers = true;

    bool operator==(const SettingsView&) const = default;
};
// OptionsFrame (video) over the whole screen (kit::beginFullScreen). Edits `v` in place; returns true when closed.
bool settingsPanel(DialogContext& c, SettingsView& v);

struct ConfirmResult {
    bool yes = false;
    bool no = false;
};
// `no` empty: a single button.
ConfirmResult confirmDialog(DialogContext& c, std::string_view text, std::string_view yes,
                            std::string_view no);

// --- Form fields ----------------------------------------------------------------------------------------

// Text of a form field between frames (fixed capacity: nothing allocates).
template <u32 N>
struct FormText {
    std::array<char, N> bytes{};
    u32 length = 0;

    std::string_view view() const { return {bytes.data(), length}; }
    bool empty() const { return length == 0; }
    void set(std::string_view s) {
        wipe();
        length = static_cast<u32>(s.size() < N ? s.size() : N);
        for (u32 i = 0; i < length; ++i) {
            bytes[i] = s[i];
        }
    }
    void wipe() { // zeroed, not just forgotten (the password lives in one)
        volatile char* p = bytes.data();
        for (u32 i = 0; i < N; ++i) {
            p[i] = 0;
        }
        length = 0;
    }
};

} // namespace vd
