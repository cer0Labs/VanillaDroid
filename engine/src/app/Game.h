#pragma once

#include "app/Benchmark.h"
#include "app/CharacterPreview.h"
#include "app/GlueStage.h"
#include "app/Cinematics.h"
#include "app/ChatBubbles.h"
#include "app/Crowd.h"
#include "app/CrowdFight.h"
#include "app/DbcBrowser.h"
#include "app/DebugHud.h"
#include "app/FarSightView.h" // phase 18 (motion)
#include "app/FirstRunScreens.h" // phase 22 (pairing)
#include "app/FlyCamera.h"
#include "app/GameAudio.h"
#include "app/GameSession.h"
#include "app/GameScreens.h"
#include "app/Hud.h"
#include "app/ServerObjects.h"
#include "app/SpellEffects.h"
#include "app/PlayerController.h"
#include "app/ModelViews.h"
#include "app/MotionDemo.h" // phase 18 (motion)
#include "app/Portraits.h"
#include "app/QualityGovernor.h"
#include "app/SelectionCircle.h"
#include "app/TextureBrowser.h"
#include "app/Transports.h"
#include "app/UiGallery.h"
#include "app/UiResources.h"
#include "app/QuestMarks.h"
#include "app/Units.h"
#include "app/WalkTest.h"
#include "app/MapTravelTest.h" // phase 18 (world)
#include "app/WeatherControl.h"
#include "audio/AudioSystem.h"
#include "audio/SoundData.h"
#include "core/Clock.h"
#include "core/Types.h"
#include "game/CharCreate.h"
#include "game/SocialData.h"
#include "game/ItemData.h"
#include "game/PetData.h"
#include "game/TradeSkillData.h"
#include "game/ExchangeData.h"
#include "game/Languages.h" // phase 18 (extras)
#include "game/MinimapData.h"
#include "game/SpellKitFx.h"
#include "game/SpellVisualData.h"
#include "game/UnitFade.h"
#include "jobs/JobSystem.h"
#include "pack/PackReader.h"
#include "physics/CollisionWorld.h"
#include "platform/DeviceMonitor.h"
#include "platform/Input.h"
#include "platform/NativeSurface.h"
#include "platform/PerformanceHint.h"
#include "platform/PlatformInfo.h"
#include "render/Renderer.h"
#include "ui/TextInput.h"
#include "world/Actors.h"
#include "world/Camera.h"
#include "world/CameraShakes.h"
#include "world/Effects.h"
#include "world/GroundEffects.h" // phase 14 (grass)
#include "world/Lighting.h"
#include "world/Sky.h"
#include "world/SpellBeams.h"
#include "world/Terrain.h"
#include "world/WmoAreas.h"
#include "world/UnitBodyEffects.h"
#include "world/WorldObjects.h"

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd {

// Platform-independent entry point. Platform layers (GameActivity, Win32) only translate OS
// lifecycle events into these calls; all game logic lives behind this interface.
class Game {
public:
    bool init(const PlatformInfo& platform);
    void shutdown();

    // The surface can come and go many times (fold/unfold, background). The Vulkan device is
    // created once in init(); only the swapchain follows the surface.
    void onSurfaceCreated(const NativeSurface& surface);
    // Size and/or orientation may have changed (fold/unfold, rotation, window resize).
    void onSurfaceResized(u32 width, u32 height);
    void onSurfaceDestroyed();
    void onLowMemory();
    // Phase 10: Android's onTrimMemory with its level (ComponentCallbacks2.TRIM_MEMORY_*): the further from the
    // foreground, the more of the streamed world is let go (now, even without frames); onResume gives it back.
    void onTrimMemory(i32 level);
    // The app goes to the background and comes back (Android onPause / onResume): the sound stops meanwhile.
    void onPause();
    void onResume();
    // Android audio focus: 1 owned, < 1 ducked under another app, 0 lost (muted).
    void onAudioFocus(f32 gain);

    // Input. Positions in logical pixels (as the player sees the screen).
    // Mouse primary button / pointer: debug UI.
    void onPointer(f32 x, f32 y, bool down, bool touch);
    void onKey(Key key, bool down);
    void onMouseLook(f32 dx, f32 dy); // desktop: drag with the secondary button
    void onMouseWheel(f32 steps);
    void onTouch(TouchPhase phase, i32 id, f32 x, f32 y);
    void onFocusLost();
    // Phase 14: Escape (desktop) or Back (Android) while no text is edited: true when the game used it (a cinematic
    // stopped); false lets the platform do its own (the desktop closes the window).
    bool onEscape();
    // Phase 18 (extras): Enter and the arrows while no text is edited (desktop; Android: a physical keyboard): the glue
    // screens' OnKeyDown, in the world OPENCHAT. True when the game used it. Escape: also ToggleGameMenu in the world.
    bool onEnter();
    bool onArrow(bool next);

    // Text entry (phase 7): the one field being edited (chat, login), which widgets begin/end. While it is active the
    // platform shows its keyboard and sends it the typing; desktop keys stop moving the character.
    ui::TextInput& textInput() { return m_textInput; }
    const ui::TextInput& textInput() const { return m_textInput; }
    bool wantsKeyboard() const { return m_textInput.active(); }
    void onTextChar(u32 codepoint);               // desktop: a typed character (WM_CHAR)
    void onTextKey(TextKey key);                  // Backspace / Enter (IME action) / Escape (Android: Back)
    void onTextInputState(std::string_view utf8); // Android: the whole field as the soft keyboard holds it
    // Screen (phase 7). Insets in logical pixels as the player sees the screen (after the swapchain pre-rotation):
    // system bars, display cutout. The soft keyboard's height is kept apart: it covers the bottom while typing.
    void onSafeArea(f32 left, f32 top, f32 right, f32 bottom);
    // Phase 20 (look): where the display cutout is (its rectangles, window coordinates; at most
    // ScreenEdges::kMaxCutouts) and the insets that are not its (system bars, curved edges): the chat and the
    // experience bar use the screen beyond the safe area where no cutout reaches (app/ScreenEdges). Not called: the
    // safe area everywhere.
    void onDisplayCutouts(const f32 otherInsets[4], std::span<const ui::Rect> cutouts);
    void onKeyboardInset(f32 bottom);
    // Physical pixels per dp (Android densityDpi / 160): changes when the Fold5 folds or unfolds.
    void onContentScale(f32 dpScale);
    // Phase 20 (edit): the screen's real pixels per millimetre (PlatformInfo::pixelsPerMm), again after a fold; 0: unknown.
    void onPixelsPerMm(f32 pixelsPerMm) { m_pixelsPerMm = pixelsPerMm; }
    ui::Rect safeArea() const; // the screen minus the insets, logical pixels
    ScreenEdges screenEdges() const; // phase 20 (look): the screen beyond it where no cutout reaches
    // The safe area minus the soft keyboard while it is up: where dialogs with a text field go.
    ui::Rect uiArea() const;
    f32 keyboardInset() const { return m_keyboardInset; } // 0 while the soft keyboard is hidden
    f32 contentScale() const { return m_contentScale; }
    // Game controller (phase 7): the platform's state of the first pad, once per frame (connected = false: none).
    void onGamepad(const GamepadState& state);
    const GamepadState& gamepadState() const { return m_gamepad; }

    bool hasSurface() const { return m_surface.window != nullptr; }

    // Development: place the free camera (map directory as in Map.dbc; height = yards above ground).
    // `z`: absolute height (under a mountain, inside a WMO); otherwise `heightAboveGround` over the terrain.
    void setView(const std::string& map, f64 x, f64 y, f32 yawDegrees, f32 pitchDegrees, f32 heightAboveGround,
                 std::optional<f64> z = std::nullopt);
    void setTimeOfDay(f32 hours) { m_timeOfDay = hours * 120.0f; }
    bool startBenchmark(std::string_view id);
    // A benchmark launched with exitAfterBenchmark has finished, or the player chose Exit Game (parity:
    // popups): the platform should quit.
    bool wantsExit() const { return m_exitRequested; }

    // One full frame: input -> network -> simulation -> render -> present.
    // Blocks in present (FIFO / Swappy), which is what paces the loop at 60 fps.
    void tick();

    // Phase 22 (pairing): a vanilladroid://pair link the platform received (the app opened by it, or brought back
    // while running): the first-run screen pairs with that PC.
    void onPairingLink(std::string_view uri);
    // Phase 22 (pairing): the data arrived (or the player plays with what there is): the platform starts the game
    // over, in place (shutdown, a new Game, init: the packs are opened there).
    bool wantsRestart() const { return m_firstRunMode && m_firstRun.wantsRestart(); }
    // Phase 22 (phonebake, ROADMAP 22.7): baking the packs on the phone from the WoW copied to it (its own
    // thread and, on Android, a service of its own process). The first-run screen starts a first bake; after
    // an update, the update flow starts the steps whose recipes changed (PhoneBake::start(jobsForPacks(...)),
    // status()).
    PhoneBake& phoneBake() { return m_phoneBake; }

private:
    // Phase 22 (pairing): without the game's data (or with a download left half way) the game is the first-run
    // screen alone (app/FirstRunScreens, app/GameFirstRun.cpp): app.vdpk, the renderer and the touch UI.
    bool initFirstRun(const PlatformInfo& platform);
    void tickFirstRun();
    FirstRunScreens m_firstRun;
    bool m_firstRunMode = false;
    PhoneBake m_phoneBake; // phase 22 (phonebake)

    void updateFrameStats(Clock::time_point now);
    void openPacks();
    void findWorldMaps();
    struct WorldMap {
        u32 id; // Map.dbc
        std::string directory;
    };
    const WorldMap* findWorldMap(std::string_view directory) const;
    const WorldMap* findWorldMap(u32 id) const;
    void enterWorld(const WorldEntry& entry);
    bool openMap(const std::string& directory);
    void teleport(const std::string& directory, f64 x, f64 y);
    void drawWorldPanel();
    void drawActorsPanel();
    void drawLoadingScreen(f32 width, f32 height);
    // Collision radius and height of a creature display (CreatureDisplayInfo -> CreatureModelData), for the player.
    bool displayShape(u32 displayId, f32& radius, f32& height) const;
    // F: the free camera takes over (online the character stays where it is), or back to the character; offline
    // this places a character of its own on the ground under the camera.
    void toggleCharacter();
    static std::optional<f32> groundBelow(const void* game, f32 x, f32 y, f32 z);
    // Phase 18 (world): WorldObjects' TerrainQuery (the collision's terrain, holes left out).
    static std::optional<f32> terrainBelow(const void* game, f64 x, f64 y, f64 top);
    // A point on the ground `distance` yards in front of the camera.
    glm::dvec3 groundInFront(f64 distance) const;
    SceneLighting sceneLighting(f32& drawDistance) const;
    // Phase 11: what selects Light.dbc's parameter sets this frame (weather, under water, the ghost's world).
    void updateLightConditions(f32 dt);
    // Phase 11: rain, snow or sand around the camera in this frame's light (params.weather).
    void updateWeather(f32 dt, FrameParams& params);

    JobSystem m_jobs;
    Renderer m_renderer;
    DebugHud m_hud;
    pack::PackReader m_dbPack;
    pack::PackReader m_texturePack;
    TextureBrowser m_textureBrowser;
    DbcBrowser m_dbcBrowser;
    NativeSurface m_surface;
    std::filesystem::path m_packsDir;
    // Phase 21 (languages): the player's language (client.cfg's locale, VD_LOCALE): the interface's texts, the language
    // overlays of db / textures / sound (db.esES.vdpk...) and what the server is told.
    std::string m_locale = "enUS";
    f32 m_contentScale = 1.0f;
    f32 m_uiScale = 1.0f; // the touch UI's size on top of the density
    f32 m_pixelsPerMm = 0.0f; // phase 20 (edit): the screen's real ones (0: from the density)

    // World exploration (phase 2): free camera over streamed terrain.
    Terrain m_terrain;
    Camera m_camera;
    FlyCamera m_fly;
    Benchmark m_benchmark;
    PerformanceHint m_performanceHint; // opened on the first tick, from the game thread
    bool m_performanceHintTried = false;
    bool m_exitAfterBenchmark = false;
    bool m_exitRequested = false;
    std::vector<TerrainDraw> m_terrainDraws;
    std::vector<HorizonDraw> m_horizonDraws;
    WorldObjects m_objects;
    std::vector<ModelDraw> m_modelDraws;
    std::vector<ModelInstanceData> m_modelInstances;
    std::vector<u16> m_modelGroups;
    // Phase 14 (grass): the terrain's detail doodads (grass.vdpk) and what they draw this frame.
    GroundEffects m_grass;
    std::vector<GrassDraw> m_grassDraws;
    std::vector<GrassInstance> m_grassInstances;
    f32 m_horizonDistance = 2500.0f;
    std::vector<WorldMap> m_worldMaps; // maps with a world pack on disk
    WorldLighting m_lighting;
    Sky m_sky; // phase 11: sun, moons, stars, clouds, skyboxes and glare
    LightConditions m_lightConditions; // phase 11: updateLightConditions()
    WeatherControl m_weather;          // phase 11: SMSG_WEATHER (or VD_WEATHER) shown around the camera
    // Phase 11 (variants): the auras that make units translucent or tint them (ghosts, stealth, invisibility), and
    // VD_GHOST=1: the ghost's world offline (its light and grading, and the offline character as a ghost).
    game::UnitFades m_unitFades;
    bool m_devGhost = false;
    bool m_fullScreenGlow = true; // the options FULL_SCREEN_GLOW and DEATH_EFFECT (render/ScreenFx.h)
    bool m_deathEffect = true;
    i32 m_devGlow = -1;  // development: VD_FFX_GLOW=0|1 / VD_FFX_DEATH=0|1 override the options (-1: they rule)
    i32 m_devDeath = -1;
    void updateLocalFade(); // the offline character's look (VD_GHOST)
    // Phase 11 (f11/interiors): MFOG of the building the camera is in (kept while the frame's fog leaves it), and how far
    // the frame's fog has gone towards it.
    std::optional<WorldObjects::FogSample> m_interiorFog;
    f32 m_interiorFogBlend = 0.0f;
    bool m_terrainHighlights = true; // phase 11 (f11/interiors): TERRAIN_HIGHLIGHTS
    Actors m_actors;         // phase 5: characters and creatures
    ActorFrame m_actorFrame; // this frame's actor draw lists
    Crowd m_crowd;           // synthetic actors (debug panel, crowd benchmark)
    Units m_units;           // the server's creatures and players as actors
    QuestMarks m_questMarks; // ours: TalkToMe*.m2 over the quest givers and flight masters
    ServerObjects m_serverObjects; // phase 7: the server's game objects (chests, quest objects...) as models
    Transports m_transports;       // phase 8: elevators, ships and zeppelins (moving models and collision)
    MotionDemo m_motionDemo;       // phase 18 (motion): synthetic server units offline (VD_MOTION_DEMO)
    FarSightView m_farSight;       // phase 18 (motion): the camera through a far sight object
    Effects m_effects;                          // phase 9: particles and ribbons of doodads, actors and spells
    std::vector<ParticleDraw> m_particleDraws;  // this frame's
    // Phase 14 (bodyfx): the units' blob shadows, footprints, ripples, sprays, breath and blood; the shadows and
    // footprints go to the frame's ground decals (FrameParams::groundDecals), drawn before the translucent world.
    UnitBodyEffects m_bodyFx;
    std::vector<ParticleDraw> m_decalDraws;
    static TextureHandle bodyTexture(const void* game, u64 pathHash);
    std::vector<UvTransform> m_uvTransforms;    // this frame's texture animations (models and actors)
    struct OrbitEffect {                        // development (--spawn orbit:<model>): a moving spell effect
        EffectHandle effect;
        glm::dvec3 center{0.0};
        f32 angle = 0.0f;
    };
    std::vector<OrbitEffect> m_orbitEffects;
    i32 m_spawnDisplay = 4;  // debug panel: CreatureDisplayInfo id to place
    u32 m_crowdSeed = 1;
    bool m_hideDebugUi = false;
    std::string m_pendingSpawn; // PlatformInfo::spawn, placed once the ground under the camera is known
    void spawnFromSpec(std::string_view spec);
    GameSession m_session; // phase 4: the server (network thread inside)
    physics::CollisionWorld m_collision; // phase 6: what the map collides with, streamed around the player
    PlayerController m_player;           // phase 6: the player's character and its movement packets
    bool m_freeCamera = true;            // the fly camera, not the character, has the input
    bool m_pendingWalk = false;          // PlatformInfo::walk: a character once the ground there is loaded
    WalkTest m_walkTest;                 // phase 6 acceptance: the controller walks NPC patrol routes
    std::string m_commands;              // PlatformInfo::commands still to say
    std::string m_onlineWalk;            // PlatformInfo::walkRoute, once the commands are said
    f32 m_commandDelay = 0.0f;
    bool startWalk(const WalkRoute& route);
    u32 m_playerDisplay = 0;             // display the collision shape was taken from
    f32 m_timeOfDay = 1320.0f; // half-minutes (11:00); the server will drive it from phase 4
    f32 m_timeSpeed = 0.0f;    // game half-minutes per real second (0 = frozen)
    f32 m_viewDistance = 700.0f;
    bool m_snapToGround = false;
    f32 m_snapHeight = 25.0f; // yards above ground after a teleport
    i32 m_uiTouchId = -1; // touch currently driving the debug UI
    UiResources m_uiResources; // phase 7: what the touch UI draws with
    Hud m_gameHud;             // phase 7: the in-game touch interface
    GameScreens m_screens;     // phase 7: the game state behind the HUD and the windows, and the login screen
    game::GameData m_gameData; // phase 7: spells and item icons (Spell.dbc...)
    game::AbilityData m_abilityData; // phase 8: spell texts, skill lines, talents, factions
    game::SocialData m_socialData;     // phase 8: zones, chat channels, text emotes
    game::CharCreateData m_charCreate; // phase 8: races, classes and looks of new characters
    CharacterPreview m_preview;        // phase 8: the character being created, in front of the camera
    // Phase 18: the glue screens' 3D scene (the Dark Portal, the races' places) instead of the world while the login,
    // character list or creation screen asks for it; m_glueShown: drawn this frame (the world's updates wait).
    GlueStage m_glueStage;
    bool m_glueShown = false;
    // Development: VD_GLUE_PREVIEW=mainmenu|human|orc|dwarf|nightelf|scourge|tauren[:race[:gender]]: that scene alone,
    // without the interface (a character of its race standing in the race scenes).
    GlueSceneRequest m_gluePreview;
    void collectGlue(FrameParams& params, f32 dt, u32 nowMs);
    game::ItemData m_itemData; // phase 8: repair costs, bank slots, sets, item spell texts
    game::AreaData m_areaData;         // phase 8: zones, area triggers, maps, loading screens
    game::WorldMapData m_worldMapData; // phase 8: the world map
    game::TaxiData m_taxiData;         // phase 8: flight paths
    game::LanguageData m_languageData; // phase 18 (extras): B4, LanguageWords and the languages' names and spells
    game::ExchangeData m_exchangeData; // phase 8: auction houses, mail templates, item class names
    game::TradeSkillData m_tradeSkillData; // phase 13: the profession windows' recipes, headers, foci
    game::PetData m_petData; // phase 15 (pets): families, loyalty, diets, stable slot prices
    game::MinimapData m_minimapData;   // parity pass: locks and tracking spells (the minimap's tracking)
    // Phase 8: a world entry on another map waits one frame, so the loading screen is on screen first. Phase 18 (world):
    // opening the map no longer blocks (it took ~0.85 s on the Mi 11X, ~1 s on the desktop's CPU ASTC): it goes on
    // under the animated loading screen (Terrain's map loader, the collision's indexer).
    std::optional<WorldEntry> m_pendingWorld;
    // Phase 18 (world): VD_WORLD_TRAVEL, far teleports offline measured until the ground is there (app/MapTravelTest).
    MapTravelTest m_travelTest;
    void updateTravelTest(f32 dt);
    bool m_groupsCompared = false; // VD_WORLD_COMPARE_GROUPS, once
    bool m_roomsProbed = false;    // VD_WORLD_PROBE_ROOMS, once
    void probeRooms(f32 radius);
    // Development (VD_WORLD_CHECK_ROOMS=<yd>, once): a character on the floors of every true interior around, whether
    // the minimap goes indoors and the room is the one of the floor it stands on, now and by the rules before.
    bool m_roomsChecked = false;
    void checkRooms(f32 radius);
    // Phase 14 (sound): the room of a building a character with its feet at `feet` is in (WMOAreaTable): the group the
    // world finds under it (WorldObjects::standingAt(), the minimap's indoors too), or the floors baked in
    // worldsound.vdpk when the buildings have no BSP.
    const WmoAreaHit& roomAt(const glm::dvec3& feet);
    f32 m_loadingProgress = 0.0f;      // phase 8: the loading screen's bar (never goes back while it shows)
    bool m_loadingShown = false;
    // Entering a world (the login, a transfer) keeps the loading screen up until then, at least kLoadingMinimumMs, so
    // the world it opens on has streamed in (without it the screen could vanish within a frame).
    u32 m_loadingUntilMs = 0;
    void updateTravel(u32 nowMs);      // phase 8: where the player is, for zones and area triggers
    UiGallery m_uiGallery;     // development: VD_UI_GALLERY draws the dialogs instead of the HUD
    void uiTouch(TouchPhase phase, i32 id, f32 x, f32 y); // a finger for the HUD (or the gallery)
    game::Reactions m_reactions;              // phase 7: how units look to the player
    std::vector<Units::UnitView> m_unitViews; // this frame's drawn units (tapping, names)
    bool m_mouseDown = false;  // desktop: the left button as a finger of the touch UI
    bool m_mouseOnHud = false;
    bool m_hudDemo = false;    // VD_HUD_DEMO: sample HUD contents (development)
    // Phase 13 (development): VD_FAKE_CONTROL=lost: how long the offline character loses control, and when.
    u32 m_fakeControlMs = 0;
    u32 m_fakeControlAfterMs = 0;
    u32 m_fakeControlAtMs = 0;
    bool m_fakeControlDone = false;
    // Phase 7 platform input: text entry, safe area, game controller.
    ui::TextInput m_textInput;
    bool m_typing = false; // text input was active last frame (held movement keys are released when it starts)
    std::array<f32, 4> m_safeInsets{}; // left, top, right, bottom
    // Phase 20 (look): onDisplayCutouts.
    std::array<f32, 4> m_otherInsets{};
    std::array<ui::Rect, ScreenEdges::kMaxCutouts> m_cutouts{};
    u32 m_cutoutCount = 0;
    bool m_cutoutsKnown = false;
    f32 m_keyboardInset = 0.0f;
    f32 m_fakeKeyboard = 0.0f; // development: PlatformInfo::fakeKeyboard
    GamepadState m_gamepad;
    // Phase 9: sound.
    audio::AudioSystem m_audio;
    audio::SoundData m_soundData;
    GameAudio m_gameAudio;
    void updateAudio(u32 nowMs, f32 dt);
    // Phase 14 (sound): the room of a building the character stands in (WMOAreaTable): its music and ambience, its
    // name on the minimap. VD_INTERIOR_AUDIO=0: the area's music and ambience indoors too (to compare).
    WmoAreas m_wmoAreas;
    GameAudioScene m_audioScene; // the world as GameAudio sees it (doodads, actors' events, units, ground)
    bool m_interiorAudio = true;
    bool m_listenerAtCamera = false; // VD_LISTENER=camera: heard from the camera (before phase 14)
    bool m_traceRooms = false;       // VD_TRACE_AUDIO: each room change in the log
    u64 m_tracedRoom = ~0ull;
    // Phase 14: cinematics (the server's SMSG_TRIGGER_CINEMATIC; offline VD_CINEMATIC=<CinematicSequences id>).
    Cinematics m_cinematics;
    audio::VoiceHandle m_cinematicVoice;
    u32 m_cinematicPlayed = 0; // the sequence started last (0: none), so one trigger plays once
    u32 m_devCinematic = 0;
    bool m_traceEvents = false; // development (VD_TRACE_EVENTS=1): every M2 event the actors cross, in the log
    f32 m_cinematicSkipShown = 0.0f; // seconds the skip button stays after a tap
    void updateCinematic(f32 dt);
    void drawCinematic(f32 width, f32 height, f32 dt);
    // Phase 9: spell visuals and combat animations.
    game::SpellVisualData m_spellVisualData;
    SpellEffects m_spellEffects; // the server's spells on its units
    CrowdFight m_fight;          // offline: the group fight (route "fight", --spawn fight:<n>)
    // Phase 14 (spellfx, app/GameSpellFx.cpp): the kits' procs and shakes, the camera shakes (applied to the
    // drawn view only), and the beams and fishing lines written after the frame's particles (returns the
    // quads in all).
    game::SpellKitFx m_spellKitFx;
    CameraShakes m_cameraShakes;
    BeamWriter m_beamWriter;
    bool m_spellBeamsOn = true; // VD_BEAMS=0
    bool m_kitTintsOn = true;   // VD_KIT_FLASH=0
    // Development (VD_FISHING_DEMO=1, offline): a character fishing in front of the camera, with its bobber.
    bool m_fishingDemo = false;
    u32 m_fishingDemoActor = ~0u;
    glm::dvec3 m_fishingDemoBobber{0.0};
    void updateFishingDemo();
    void initSpellFx();
    void addEventShakes(u32 nowMs);
    glm::mat4 cameraShake(u32 nowMs);
    u32 addSpellBeams(u32 quads, f32 viewportHeight);
    static TextureHandle spellBeamTexture(void* game, u64 hash);
    void logSpellFx(); // the stats line, with the others every few seconds
    f64 m_spellFxMsSum = 0.0; // beams, lines and shakes, this stats window
    f64 m_spellFxMsMax = 0.0;
    u32 m_spellFxFrames = 0;
    u32 m_beamQuadsMax = 0;
    // Parity: the unit frames' portraits, drawn after the world's actors into the renderer's portrait slots.
    Portraits m_portraits;
    std::vector<ActorDraw> m_portraitActors;              // this frame's
    std::vector<PortraitDraw> m_portraitDraws;            // idem
    ModelViews m_modelViews;                              // phase 15: the interface's 3D model frames
    std::vector<PortraitDraw> m_modelViewDraws;           // this frame's
    static u32 portraitActor(const void* game, u64 guid); // a unit's actor (the offline character for guid 0)
    // Parity (world feedback): the selection circle under the target, written after the frame's particles
    // (the first `quads`) into the same pass; returns the quads in all. VD_SELECTION_DEMO: three circles
    // (friendly, neutral, hostile) on the ground in front of the camera, where --spawn character / display
    // put their actors. And the chat bubbles over the speakers (read from the session's chat, drawn by the
    // HUD); the game objects drawn (tapping them uses them) and the short flash under the one just tapped.
    u32 addSelectionCircles(u32 quads, u32 nowMs);
    static std::optional<f32> floorBetween(const void* game, f32 x, f32 y, f32 top, f32 bottom);
    SelectionCircle m_selection;
    u64 m_selectionTarget = 0;
    bool m_selectionDemo = false;
    ChatBubbles m_chatBubbles;
    std::vector<WorldObjectView> m_objectViews;
    SelectionCircle m_tapFlash;
    u64 m_tapFlashGuid = 0;
    u32 m_tapFlashMs = 0;
    f32 m_tapFlashRadius = 0.0f;
    // Phase 13 (spells): the circle of a spell waiting for a place (app/SpellTargetingScreens), and where a line
    // from the camera meets the ground (the map's collision near the player, the terrain beyond), for the HUD.
    SelectionCircle m_spellCircle;
    mutable physics::TriangleSet m_pickSet{2048};
    static std::optional<glm::dvec3> groundAlongRay(const void* game, const glm::dvec3& from,
                                                    const glm::vec3& direction, f32 maxDistance);
    // floorBetween without the doodads (bushes, crates): where the spell's circle lies.
    static std::optional<f32> floorWithoutDoodads(const void* game, f32 x, f32 y, f32 top, f32 bottom);

    // Phase 10: the device (temperature, battery, memory) on a thread of its own, the dynamic quality over the
    // player's video options, and the settings saved to disk a moment after they change.
    void updateQuality(f32 dt);
    void saveSettings();
    // Phase 18 (world): anisotropic filtering and multisampling: the GPU's limits into the options, the development
    // overrides, and the renderer told once a slider stands still (it rebuilds samplers or passes).
    void initGpuOptions(SettingsView& settings);
    void updateGpuOptions(SettingsView& settings, f32 dt);
    void logDevice();
    DeviceMonitor m_monitor;
    DeviceReading m_device;
    QualityGovernor m_governor;
    f32 m_fixedRenderScale = 0.0f; // PlatformInfo::renderScale: no governor
    f32 m_effectDistance = 100.0f; // yards of particles at full quality (the debug panel's)
    u32 m_frameInterval = 1;       // 2 = 30 fps
    f32 m_drawDistance = 0.0f;     // last frame's: the view distance cut at the fog's end
    bool m_settingsPersist = false;
    std::filesystem::path m_settingsPath;
    SettingsView m_savedSettings;   // as last written
    SettingsView m_pendingSettings; // as last frame
    f32 m_settingsCalm = 0.0f;      // seconds the settings have stood still unsaved
    u32 m_gpuAnisotropy = 1;        // phase 18 (world): as last told to the renderer
    u32 m_gpuMultisample = 1;
    u32 m_gpuPendingAnisotropy = 1; // as last frame's options
    u32 m_gpuPendingMultisample = 1;
    f32 m_gpuOptionsCalm = 0.0f;    // seconds the options have stood still unapplied
    f32 m_gpuCycleSeconds = 0.0f;   // VD_WORLD_GPU_CYCLE (development)
    f32 m_gpuCycleClock = 0.0f;
    u32 m_gpuCycleStep = 0;
    u64 m_trimmedBudget = 0;        // the terrain budget onTrimMemory left (0: none)
    // Phase 10 (soak benchmark): the battery when the measuring began and what the quality did since.
    struct Soak {
        bool started = false;
        DeviceReading start;
        Clock::time_point startTime{};
        f32 minScale = 1.0f;
        f64 scaleSum = 0.0;
        u64 frames = 0;
        u32 maxThermalLevel = 0;
        f32 maxHeadroom = 0.0f;
        f64 wattSum = 0.0; // one per minute
        u32 wattSamples = 0;
        std::string report; // the SOAK lines so far, also written to packs/bench_soak.txt (scripts/soak.ps1)
    } m_soak;
    void soakLine(const char* line);
    void logSoakMinute(const BenchmarkMinute& minute);
    void logSoakSummary();

    Clock::time_point m_startTime{};
    Clock::time_point m_lastFrame{};
    Clock::time_point m_lastTickStart{};
    Clock::time_point m_statsWindowStart{};
    u32 m_statsFrames = 0;
    f64 m_statsWorstFrame = 0.0;
    f64 m_statsGpuMs = 0.0;
    // Main thread sections over the stats window (ms): object uploads, culling + draw lists, recording +
    // submit (without the waits on the GPU/display).
    enum Section : u32 {
        kSectionTerrain,
        kSectionObjects,
        kSectionActors,
        kSectionCollect,
        kSectionRecord,
        kSectionPlayer, // collision streaming + character controller + movement packets
        kSectionNetwork, // the session's packets (entities, orders)
        kSectionUi,      // the touch UI: its streaming, input and draw list
        kSectionEffects, // phase 9: particles (placing, simulation, quads) and texture animations
        kSectionSpells,  // phase 9: spell visuals and combat animations (events, scheduling; not the particles)
        kSectionCount
    };
    std::array<f64, kSectionCount> m_sectionSum{};
    std::array<f32, kSectionCount> m_sectionMax{};
    std::array<f32, kSectionCount> m_sectionFrame{}; // this frame, for the hitch log
    void addSection(Section section, f64 ms);
};

} // namespace vd
