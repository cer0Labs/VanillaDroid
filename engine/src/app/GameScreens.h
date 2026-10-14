#pragma once

#include "app/AuraScreens.h"
#include "app/BattlefieldScreens.h"
#include "app/CharacterScreens.h"
#include "app/ChatLinkInsert.h"
#include "app/CombatScreens.h"
#include "app/Dialogs.h"
#include "app/DressUpScreens.h"
#include "app/ExchangeScreens.h"
#include "app/GlueScene.h" // phase 18 (lead)
#include "app/GlueScreens.h" // phase 18 (glue)
#include "app/GuildScreens.h"
#include "app/HelpScreens.h"
#include "app/Hud.h"
#include "app/UiSound.h"
#include "app/SocialScreens.h"
#include "app/InventoryScreens.h"
#include "app/MacroScreens.h"
#include "app/MicroMenu.h"
#include "app/MinimapScreens.h"
#include "app/ObjectTooltip.h"
#include "app/MessageHud.h"
#include "app/OptionsScreens.h"
#include "app/CombatTextScreens.h"
#include "app/PopupGallery.h"
#include "app/PetScreens.h"
#include "app/PopupScreens.h"
#include "app/PvpScreens.h" // phase 17 (honor)
#include "app/TradeSkillScreens.h"
#include "app/SpellTargetingScreens.h"
#include "app/QuestScreens.h"
#include "app/RaidAlertScreens.h"
#include "app/RaidPullouts.h"
#include "app/WorldStateScreens.h"
#include "app/TouchBarScreens.h" // phase 20
#include "app/TravelScreens.h"
#include "app/UiPanels.h"
#include "app/UnitMenuScreens.h"
#include "app/UnitScreens.h"
#include "app/TutorialScreens.h" // phase 18 (p3)
#include "app/AccountDataFiles.h" // phase 18 (p3)
#include "core/Text.h"
#include "core/Types.h"
#include "game/Gameplay.h"
#include "game/MirrorTimers.h"
#include "game/Notices.h"
#include "ui/MessageFrame.h"
#include "ui/TextInput.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace vd {

class GameSession;
class PanelDemo;
class ModelViews;
class Portraits;
class UiResources;

// The game's side of the touch UI (phase 7): what the HUD and the windows show, read from the session's
// game::Gameplay every frame, and what the player's taps do there. The HUD (app/Hud) and the windows
// (app/Dialogs) only draw view-models; this fills them and applies their results. Also the login screen while
// there is no world. Main thread; the view storage is reserved once (no allocation per frame once warmed up).
class GameScreens {
public:
    // `abilities`: the phase-8 tables of the spell book, trainers, talents... (may be null).
    void init(const game::GameData* data, UiResources* resources, const game::AbilityData* abilities = nullptr);

    // Whether the login screen replaces the game this frame: not in the world and not dismissed (VD_LOGIN_OFFLINE,
    // or a development mode that shows the world without a server).
    bool wantsLogin(const GameSession& session) const { return !m_offline && !inWorld(session); }
    void setOffline(bool offline) { m_offline = offline; }
    // Phase 18 (lead): the 3D scene the glue screens want behind them this frame (drawLogin fills it; None in the
    // world).
    const GlueSceneRequest& glueScene() const { return m_glue; }
    GlueScreens& glue() { return m_glueScreens; } // phase 18 (glue): AccountLogin, RealmList, CharacterSelect
    // Phase 18 (extras): the hardware keys (desktop Escape, Enter and the arrows; Android's Back and a physical
    // keyboard) while no field is edited (app/GameScreensKeys.cpp): before the world, the glue screens' OnKeyDown; in
    // the world, the bindings TOGGLEGAMEMENU (ToggleGameMenu's order) and OPENCHAT. True when the key was used.
    bool escapePressed(GameSession& session);
    bool enterPressed(GameSession& session);
    bool arrowPressed(GameSession& session, bool next); // the character list's previous / next character
    // The game menu's settings (the game applies them).
    SettingsView& settings() { return m_settings; }
    // Phase 20 (look): the screen beyond the safe area this frame (every DialogContext carries it).
    void setScreenEdges(const ScreenEdges& edges) { m_edges = edges; }
    // Phase 9: where the sounds of the windows the player opens and closes go (app/GameAudio).
    void setUiSoundSink(UiSoundSink sink) {
        m_uiSound = sink;
        m_minimap.setUiSoundSink(sink);
        m_inventory.setUiSoundSink(sink); // parity pass: the carried item's sounds
        m_combat.setUiSoundSink(sink);    // parity pass: the spellbook's page turn
        m_popups.setUiSoundSink(sink);    // parity (popups): StaticPopup_OnShow / OnHide
        m_pets.setUiSoundSink(sink);      // phase 15 (pets): the stable, the model frames' rotate buttons
        m_dressUp.setUiSoundSink(sink);   // phase 15 (items): the dressing rooms
        m_character.setUiSoundSink(sink); // phase 15 (character): the tabs' check boxes, the model's rotation
        m_guild.setUiSoundSink(sink);     // phase 15 (guild): the guild frame's check boxes, arrows, the model
        m_macros.setUiSoundSink(sink);    // phase 15 (options)
        m_options.setUiSoundSink(sink);
        m_raidAlerts.setUiSoundSink(sink); // phase 16 (alerts): ReadyCheckFrame's sound
        m_battlefield.setUiSoundSink(sink); // phase 17 (queue): PVPENTERQUEUE, PVPTHROUGHQUEUE
        m_worldStateScreens.setUiSoundSink(sink); // phase 17 (scores): the score's tabs
        m_glueScreens.setUiSoundSink(sink);       // phase 18 (glue)
        m_tutorialScreens.setUiSoundSink(sink);   // phase 18 (p3): TutorialPopup, the frame's close
        m_social.setUiSoundSink(sink); // phase 18 (p3): the chat menus' check marks, the who list's headers
    }
    // Parity: the unit frames' portraits (app/Portraits) for the party, pet and target-of-target frames.
    void setPortraits(Portraits* portraits) {
        m_social.setPortraits(portraits);
        m_combat.setPortraits(portraits);
        m_pets.setPortraits(portraits); // phase 15 (pets): the pet's look for its page and the stable
        m_raidAlerts.setPortraits(portraits); // phase 16 (alerts): ReadyCheckPortrait
        m_portraits = portraits;              // retouches: the quest frames' QuestFramePortrait
        m_character.setPortraits(portraits);  // retouches (character frame): CharacterFramePortrait
    }
    // Phase 15: the interface's 3D model frames (app/ModelViews) for the windows that show one.
    void setModelViews(ModelViews* views) {
        m_modelViews = views;
        m_pets.setModelViews(views);    // phase 15 (pets)
        m_dressUp.setModelViews(views); // phase 15 (items): the dressing rooms
        m_guild.setModelViews(views);   // phase 15 (guild): the tabard designer's model
    }
    // Phase 15 (items): the tables that turn a character's look into a model (the dressing rooms' outfits).
    void setAppearance(const game::AppearanceDb* appearance) { m_dressUp.setAppearance(appearance); }
    DressUpScreens& dressUp() { return m_dressUp; }
    ModelViews* modelViews() const { return m_modelViews; }
    // Phase 8: character creation, groups, chat channels, friends, guild, duels (app/SocialScreens).
    SocialScreens& social() { return m_social; }
    // Phase 8: bags, character, vendor, bank, books and item tooltips (InventoryScreens::showItemTooltip for
    // the loot and quest windows, addCharacterTab for the character window's other tabs).
    InventoryScreens& inventory() { return m_inventory; }
    CombatScreens& combat() { return m_combat; } // phase 13
    // Phase 15 (character): the paper doll's model and numbers, reputation, skills, honor, the watch bar, inspect.
    CharacterScreens& character() { return m_character; }
    // Phase 8: zones, the world map, flight masters, innkeepers and the loading screen.
    TravelScreens& travel() { return m_travel; }
    // Phase 17 (scores): the world states' lines and capture bars, the score, the battle map.
    WorldStateScreens& worldStates() { return m_worldStateScreens; }
    // Phase 8: group loot rolls, mail, the auction house and trade.
    ExchangeScreens& exchange() { return m_exchange; }
    // Parity pass: the minimap cluster (zone bar, zoom, blips, pings, tracking, mail, the clock).
    MinimapScreens& minimap() { return m_minimap; }
    // Parity (popups): the StaticPopups (invitations, death, logout, soulbinding... app/PopupScreens).
    PopupScreens& popups() { return m_popups; }
    // Phase 13 (professions): the TradeSkillFrame and the CraftFrame.
    TradeSkillScreens& tradeSkills() { return m_tradeSkills; }
    // Phase 13 (spells): a spell waiting for a place or an item (the ground circle the game draws).
    SpellTargetingScreens& targeting() { return m_targeting; }
    // Phase 13 (quests): the quest log, the watch frame and the quest timers.
    QuestScreens& quests() { return m_quests; }
    // Phase 15 (pets): the stable, the Pet tab, the raid group number and the portraits' hit indicators.
    PetScreens& pets() { return m_pets; }
    // Phase 15: the guild windows (the guild tab, the registrar, the charter, the tabard designer) and the help windows.
    GuildScreens& guild() { return m_guild; }
    HelpScreens& help() { return m_help; }
    // Phase 15 (options): the macros (Blizzard_MacroUI, the action buttons holding them) and UIOptionsFrame /
    // SoundOptionsFrame.
    MacroScreens& macros() { return m_macros; }
    TouchBarScreens& touch() { return m_touch; } // phase 20: the touch cluster (its files' storage)
    // Phase 15 (options): the character moves this frame (CLEAR_AFK).
    void setPlayerMoving(bool moving) { m_playerMoving = moving; }
    OptionsScreens& options() { return m_options; }

    GameScreens(); // out of line: the panel demo's type is only known there
    ~GameScreens();
    // Before the HUD draws: the action bar, the context button, the cast bar and the auras.
    void prepareHud(GameSession& session, HudFrame& hud, u32 nowMs);
    // The login screen (instead of the HUD). `area`: the safe area without the soft keyboard.
    void drawLogin(GameSession& session, Hud& hud, ui::TextInput& text, const ui::Rect& area);
    // After the HUD drew: its buttons act, then the windows (NPC, quests, loot, log, chat, menu, death...),
    // the quest tracker, the messages and the floating combat text.
    void drawGame(GameSession& session, Hud& hud, ui::TextInput& text, const ui::Rect& area, u32 nowMs);

private:

    static bool inWorld(const GameSession& session);
    ScreenProfile profile(const ui::Rect& screen) const;
    DialogContext context(Hud& hud, const ui::Rect& area);

    void actOnHud(GameSession& session, const HudOutput& out);
    void readEvents(GameSession& session, Hud& hud, u32 nowMs);
    void readCombat(GameSession& session, Hud& hud, u32 nowMs);
    // Parity: the client's own lines (game/Notices) where 1.12 shows them: chat, combat log, the middle.
    void readNotices(GameSession& session, u32 nowMs);
    // A line of the UIErrorsFrame: the text of `key` (core/Text) with `args`, or a text already worded.
    template <typename... Args> void message(u32 color, bool big, text::Key key, const Args&... args) {
        text::format(nextMessage(color, big), key, args...);
    }
    void messageText(u32 color, bool big, std::string_view line);
    std::span<char> nextMessage(u32 color, bool big); // the next line's buffer (emptied)
    void drawMessages(Hud& hud, const ui::Rect& area, u64 nowMs, f32 below = 0.0f,
                      const ui::Rect& avoid = {});

    void npcWindow(GameSession& session, DialogContext& c);
    void lootWindow(GameSession& session, DialogContext& c);
    void autoLoot(GameSession& session); // the Auto Loot option's next take
    void deathHint(GameSession& session, DialogContext& c, u32 nowMs); // the ghost's way back to its corpse
    QuestScreens::Frame questFrame(GameSession& session, u32 nowMs); // phase 13 (quests)
    // Item as the windows show it (name and icon when the server told them; asks otherwise).
    ItemView item(GameSession& session, u32 entry, u32 count);
    std::string_view objectName(GameSession& session, u64 guid);
    // Keeps `text` expanded ($N, $C, $B...) for the player in one of the reusable buffers.
    std::string_view expand(GameSession& session, std::string_view text, u32 buffer);
    // The nearest game object the player could use now (chests, quest objects), 0 if none.
    u64 nearbyObject(GameSession& session);
    // The nearest corpse the player may loot within reach (as the server measures it), 0 if none.
    u64 nearbyLoot(GameSession& session);

    const game::GameData* m_data = nullptr;
    UiResources* m_resources = nullptr;
    bool m_trace = false; // VD_TRACE_GAMEPLAY: every game and combat event in the log (development)
    u8 m_tracedSheath = 0xFF; // the player's sheath state the trace saw last
    bool m_offline = false;

    // Login screen.
    GlueSceneRequest m_glue; // phase 18 (lead)

    // HUD contents (the action bar: m_combat).
    AuraScreens m_auras; // parity: buffs, debuffs, weapon enchantments, their tooltips (app/AuraScreens)
    HudContextAction m_context = HudContextAction::None;
    bool m_contextInRange = true;
    u64 m_contextGuid = 0;
    std::string m_castName;
    u32 m_lastCastSpell = 0;
    u64 m_castEndMs = 0; // the flash after a cast ended
    u64 m_lastTarget = 0; // a new target while auto attacking moves the attack to it (or stops it)
    u64 m_faceGuid = 0;  // turn the player towards it until m_faceUntilMs (an attack or spell just asked for)
    u32 m_faceUntilMs = 0;
    u32 m_nowMs = 0;
    bool m_castFailed = false;

    // Windows.
    i32 m_rewardChoice = -1;
    u32 m_rewardQuest = 0;
    u32 m_dialogQuest = 0;        // the quest of the last quest window, and its title (for the messages that
    std::string m_dialogQuestTitle; // come before the quest's template does)
    SettingsView m_settings;
    std::vector<ItemView> m_items;       // reserved: views of the open window
    std::vector<ItemView> m_choiceItems; // reserved
    std::vector<LootItemView> m_lootItems;
    u64 m_lootGuid = 0; // the loot shown, and its page (parity: LootFrame)
    u32 m_lootPage = 0;
    // Auto Loot (ours): the next thing of the loot just opened to take (0 the money, 1 + the items' index; done at the
    // end), when, and whether a bind on pickup already asked.
    u32 m_autoLootNext = 0;
    u32 m_autoLootAtMs = 0;
    bool m_autoLootAsked = false;
    std::vector<GossipOptionView> m_gossipOptions;
    std::vector<GossipQuestView> m_gossipQuests;
    std::array<std::string, 4> m_expanded; // expanded texts of the open window
    QuestScreens m_quests; // phase 13: the quest log, QuestWatchFrame, QuestTimerFrame

    // Chat, party frames, invitations, the social window (phase 8).
    SocialScreens m_social;

    // Event cursors (the session's rings; reset when a new world starts).
    u32 m_eventCursor = 0;
    u32 m_combatCursor = 0;
    u64 m_cursorsPlayer = 0;

    // The UIErrorsFrame (UIErrorsFrame.xml: three lines, newest on top, 5 s then a 3 s fade).
    ui::MessageFrame m_messages;
    // Parity: loot, money, experience, reputation, learned spells, level-ups, skill-ups, quests.
    game::NoticeFeed m_notices;
    u32 m_noticeCursor = 0;
    std::array<MirrorTimerBarView, game::MirrorTimers::kFrames> m_mirrorBars{}; // breath, fatigue, feign

    // Phase 8: action bar pages, spell book, trainers, talents, pet, skills, reputation, combat log.
    CombatScreens m_combat;
    InventoryScreens m_inventory; // phase 8
    CharacterScreens m_character; // phase 15 (character)
    TravelScreens m_travel; // phase 8
    ExchangeScreens m_exchange; // phase 8
    MinimapScreens m_minimap;   // parity pass
    UnitScreens m_units;        // parity pass: unit frames, the unit tooltip, the experience bar
    ChatLinkInsert m_chatLink;  // phase 13: the chat's edit box, for the windows' link taps (DialogContext)
    ScreenEdges m_edges;        // phase 20 (look): DialogContext::edges
    TradeSkillScreens m_tradeSkills; // phase 13 (professions)
    UnitMenuScreens m_unitMenus; // phase 13 (social): the players' menus, follow, inspect, raid target icons
    PetScreens m_pets;           // phase 15 (pets)
    RaidPullouts m_raidPullouts; // phase 16 (raids): the pullout frames out of the raid tab
    RaidAlertScreens m_raidAlerts; // phase 16 (alerts): RaidWarningFrame, RaidBossEmoteFrame, ReadyCheckFrame
    // Phase 17 (lead stub, f17/scores implements it): WorldStateAlwaysUpFrame, WorldStateScoreFrame, BattlefieldMinimap.
    WorldStateScreens m_worldStateScreens;
    // Phase 17 (queue).
    BattlefieldScreens m_battlefield; // BattlefieldFrame, MiniMapBattlefieldFrame, CONFIRM_BATTLEFIELD_ENTRY
    // Phase 17 (honor).
    PvpScreens m_pvp; // "HK: <rank>" over the player; VD_PVP_DEMO over the offline HUD
    // Phase 18 (glue).
    GlueScreens m_glueScreens; // the glue screens before the world but CharacterCreate (app/GlueScreens)
    // Phase 18 (create).
    // Phase 18 (p3).
    TutorialScreens m_tutorialScreens; // TutorialFrame and its alert buttons (game::Tutorials)
    bool m_tutorialDemo = false;       // VD_P3_TUTORIAL: alerts and the frame without a server
    AccountDataFiles m_accountFiles;   // the account data's files (macros, chat windows, cache.md5)
    // Phase 18 (extras).
    std::string m_gossipHeard; // the gossip's text as the player reads it (another language)
    bool closeMenus(GameSession& session);      // ToggleGameMenu's CloseMenus
    bool closeAllWindows(GameSession& session); // and CloseAllWindows
    static f32 hudBottom(Hud& hud, const DialogContext& c);
    ObjectTooltip m_objectTooltip; // phase 16 (alerts): a game object held still (a meeting stone's levels)
    GuildScreens m_guild;        // phase 15 (guild)
    HelpScreens m_help;          // phase 15 (help)
    PopupScreens m_popups;      // parity (popups): the StaticPopups at the top centre, none modal
    DressUpScreens m_dressUp;   // phase 15 (items): DressUpFrame and the auction house's
    PopupGallery m_popupDemo;   // VD_POPUP_DEMO=1|2 (development): sample popups over the offline HUD
    u32 m_popupDemoSet = 0;
    // Phase 13 (spells): spells aimed at a place or an item; the windows its item choice opened (closed after).
    SpellTargetingScreens m_targeting;
    bool m_targetingOpenedBags = false;
    bool m_targetingOpenedCharacter = false;
    void followTargeting(GameSession& session);

    // Parity pass (panels): the original's panel windows (UIPanelWindows) over every module's windows, placed by
    // app/UiPanels; the micro menu and the bag bar that open them (app/MicroMenu). The game menu, the options and
    // the quest log are this class's own: their slot is their state.
    void syncPanels(GameSession& session);
    bool panelWanted(GameSession& session, UiPanel panel) const; // its module has it open
    void closePanel(GameSession& session, UiPanel panel);        // the rules hid it: its module closes it
    void closeHidden(GameSession& session);
    // The player asked for it (the micro menu): ShowUIPanel(frame, force), or HideUIPanel when shown.
    void togglePanel(GameSession& session, UiPanel panel);
    void toggleBags(GameSession& session); // phase 15 (items): OpenAllBags
    void bagButton(GameSession& session, i32 button, bool shift); // phase 15 (items): the bag bar
    void syncBags(GameSession& session); // phase 15 (items): the bags' place follows the ContainerFrames
    UiPanelPlaces panelPlaces(const Hud& hud, const DialogContext& c) const;
    void drawPanels(GameSession& session, DialogContext& c, ui::TextInput& input, const UiPanelPlaces& places,
                    u32 nowMs);
    void drawPanel(GameSession& session, DialogContext& c, ui::TextInput& input, UiPanel panel, u32 nowMs);
    // The option windows (video, sound, interface) over the whole screen, after everything else: UIOptionsFrame's
    // FULLSCREEN strata, over the popups' DIALOG one.
    void drawFullScreenPanel(GameSession& session, DialogContext& c, ui::TextInput& input, u32 nowMs);
    void questLog(GameSession& session, DialogContext& c);
    void microMenu(GameSession& session, DialogContext& c);
    UiPanelSlots m_panels;
    MicroMenuState m_microState;
    MicroMenuLayout m_microLayout;
    std::array<ui::Rect, 6> m_bagRects{}; // phase 15 (items): the bag bar's buttons this frame (drops on them)
    TextureHandle m_playerPortrait; // the micro menu's character button (prepareHud)
    bool m_wasDead = false;          // PLAYER_DEAD closes the windows (CloseAllWindows)
    bool m_rightPlaceUsed = false;   // last frame: a window on the right (the bottom left action bar gives way)
    ui::Rect m_questWatchRect;       // last frame: the watched quests over the chat (a tap there is not the world's)
    // QuestWatchFrame's place, a deliberate change from 1.12 (which keeps it on the right, under the minimap): over the
    // collapsed chat (this frame's) at its left edge and at most its width, from under the unit frames (the party's
    // when they are over it) down to the chat; x, y, w and the bottom of the rectangle are questWatchFrame's left,
    // top, width and bottom (empty: no chat drawn).
    ui::Rect questWatchPlace(const DialogContext& c, const ui::Rect& area) const;
    bool m_framesCovered = false;    // last frame: a window over the unit frames (UiPanelPlaces::overFrames)
    UiPanel m_fullScreen = UiPanel::None; // this frame: the option window drawPanels left to drawFullScreenPanel
    std::unique_ptr<PanelDemo> m_demo; // VD_HUD_DEMO: the panels with sample data, without a server
    UiPanel m_demoOpen = UiPanel::None; // VD_HUD_DEMO=merchant|gossip|character|spellbook...: a window to open at once

    // Phase 9: the windows open last frame (a bit per kind), for their open / close sounds.
    void windowSounds();
    UiSoundSink m_uiSound;
    Portraits* m_portraits = nullptr; // retouches (quest windows): the NPC's portrait in GossipFrame and QuestFrame
    ModelViews* m_modelViews = nullptr; // phase 15
    MacroScreens m_macros;              // phase 15 (options)
    TouchBarScreens m_touch;            // phase 20 (docs/MOBILE_CONTROLS.md)
    OptionsScreens m_options;
    CombatTextScreens m_combatText;
    bool m_playerMoving = false;
    bool m_afkClearSent = false;
    // Phase 15 (options): the game modules follow the options each frame (auto self cast, block trades, the action
    // bars' lock, the target of target, the party frames, the loot rolls' lines).
    void applyOptions(GameSession& session);
    u32 m_soundWindows = 0;
    u8 m_soundToggles = 0xFF; // parity pass: the settings' check boxes last frame (0xFF: not yet)
};

} // namespace vd
