#pragma once

#include "app/GameAudioScene.h"
#include "app/UiSound.h"
#include "audio/AudioSystem.h"
#include "audio/GroundSounds.h"
#include "audio/SoundData.h"
#include "audio/WorldSoundTables.h"
#include "core/Types.h"

#include <glm/vec3.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace vd {

namespace game {
class EntityStore;
class Gameplay;
class Reactions;
struct CombatEvent;
struct Entity;
struct GameEvent;
} // namespace game

namespace physics {
class CollisionWorld;
}


// What the game tells the audio each frame.
struct GameAudioFrame {
    u32 nowMs = 0;
    f32 dt = 0.0f;
    glm::dvec3 listener{0.0};               // the camera
    glm::vec3 listenerRight{0.0f, -1.0f, 0.0f};
    bool world = false;                     // a map is shown (online in the world, or a development view)
    bool login = false;                     // the login / character screen is up
    bool loading = false;                   // loading screen: no world sounds yet
    std::string_view map;                   // Map.dbc directory of the terrain shown (its footstep surface table)
    u32 area = 0;                           // AreaTable id under the character (or the camera); 0 = not loaded yet
    f32 hours = 12.0f;                      // time of day, 0..24
    // Online in the world: the game state (null offline).
    game::Gameplay* play = nullptr;
    const game::EntityStore* entities = nullptr;
    u64 player = 0;
    const game::Reactions* reactions = nullptr;
    // The player's character (online, or offline with --walk / F), for its footsteps.
    bool character = false;
    glm::dvec3 feet{0.0};
    f32 speed = 0.0f;  // horizontal, yards per second
    bool onGround = false;
    bool swimming = false;
    bool wading = false;   // liquid over the feet, not swimming: splashing steps
    bool landed = false;   // came down from a jump or a fall this frame
    u32 display = 0;       // its CreatureDisplayInfo (0 offline: a human's footsteps)
    bool onObject = false; // standing on a building or an object, not on the terrain: stone steps
    u32 terrainSound = 0;  // TerrainType SoundID under the feet; 0 = from the map's ground table (GroundSounds)
    // Parity pass: the character's body (CharacterController::events()) and the liquids around the listener.
    bool jumped = false;      // took off this frame (MSG_MOVE_JUMP)
    u32 airMs = 0;            // with `landed` or `startedSwim`: how long the flight that ended lasted
    bool startedSwim = false; // fell or walked into deep water this frame
    const physics::CollisionWorld* collision = nullptr; // the map's liquids (water loops, the camera under water)
    // Phase 11: the weather's loop (SMSG_WEATHER's SoundEntries id, 0 none) and its share of its volume (the
    // weather's fade; quieter indoors, silent under water: world/WeatherEffect).
    u32 weatherSound = 0;
    f32 weatherVolume = 0.0f;
    // Phase 14 (sound): the models' sounds and the buildings' rooms. `listener` is where the original hears from:
    // the character's head facing where it faces (WoW.exe 0x483118), the camera without a character; `camera` is the
    // camera (under water or not).
    glm::dvec3 camera{0.0};
    // The actors' animation events (footsteps, fidgets, wings, emotes...), the units they show and the ground
    // (app/GameAudioScene.h); null: no actors' sounds.
    const AudioScene* scene = nullptr;
    u32 playerActor = ~0u; // the player's character (its own footsteps come from its events)
    // The room of a building the character stands in (WMOAreaTable, world/WmoAreas): its music (ZoneMusic), intro
    // and ambience (SoundAmbience) replace the area's where set; `interiorKey` changes with the room (0: outdoors).
    u64 interiorKey = 0;
    u32 interiorMusic = 0;
    u32 interiorIntro = 0;
    u32 interiorAmbience = 0;
};

// The game's sounds (phase 9), read from the game state like GameScreens reads it for the HUD, with its own event
// cursors: zone music and ambience by area and time of day (ZoneMusic with its silences, intros, SMSG_PLAY_MUSIC),
// interface sounds (the UI's UiSoundSink, NPC and loot windows, targets, level up, quests, vocal errors), combat
// (weapon swings and impacts, wounds, deaths, aggro), spells (precast and channel loops, cast and impact sounds,
// kits, SMSG_PLAY_SOUND / _OBJECT_SOUND), NPC greetings and farewells, text emote voices and the player's footsteps.
// Game thread; nothing allocates per frame (fixed tables of loops, units and recent sounds).
class GameAudio {
public:
    void init(audio::AudioSystem* audio, const audio::SoundData* data);
    // Phase 14: the doodads' sound events and the terrain's emitters (worldsound.vdpk in `packsDir`).
    void initWorldSounds(const std::filesystem::path& packsDir);
    bool active() const { return m_audio && m_audio->active() && m_data && m_data->ready(); }

    void update(const GameAudioFrame& frame);
    // Phase 15 (options): SoundOptionsFrame's switches the mixer's volumes do not cover (the game sets them each frame):
    // EnableMusic (no zone, intro or login music starts; what plays stops), EnableAmbience (no ambience loop),
    // EnableErrorSpeech (the character's complaints), EmoteSounds (the text emotes' voices and the models' emote
    // sounds), SoundZoneMusicNoDelay (no silence between a zone's tracks).
    struct Options {
        bool music = true;
        bool ambience = true;
        bool errorSpeech = true;
        bool emoteSounds = true;
        bool musicLooping = false;
    };
    void setOptions(const Options& options) { m_options = options; }

    // The UI's sounds.
    void playUi(UiSound sound);
    UiSoundSink uiSink() { return {&GameAudio::uiCallback, this, &GameAudio::itemCallback}; }
    // Parity pass: an item picked up (true) or put down (ItemGroupSounds of its display, once its template is known).
    void playItem(u32 itemEntry, bool pickup);

    // A SoundEntries row: 2D (interface, music) or at a point; voice handle for loops. 0 or unknown: nothing.
    // Phase 14: `minDistance` / `maxDistance` (> 0) replace the row's (the terrain emitters' own). `file` >= 0: that
    // one of the row's files (the NPCs' pissed lines in order), else one at random.
    audio::VoiceHandle playSound(u32 soundEntry, audio::Category category, u8 priority,
                                 const glm::dvec3* at = nullptr, bool loop = false, f32 fadeIn = 0.0f,
                                 f32 volume = 1.0f, f32 minDistance = 0.0f, f32 maxDistance = 0.0f, i32 file = -1);

    // Development: the "Sound" panel (ImGui, app/GameAudioPanel.cpp) and the trace (VD_TRACE_AUDIO=1: every sound
    // played, in the log).
    void drawPanel();
    // The SoundEntries id of the sound started `back` sounds ago (0 = the last one); 0 if none. Tests, the panel.
    u32 recentSound(u32 back) const;
    u32 musicEntry() const { return m_musicEntry; } // SoundEntries of the music playing (0: none, or the login theme)
    bool musicPlaying() const { return m_audio && m_audio->playing(m_music); }
    u32 ambienceEntry() const { return m_ambienceEntry; }
    bool underwater() const { return m_underwater; }
    u32 waterEntry() const { return m_waterEntry; } // the liquid loop near the listener (SoundWaterType), 0 none
    u32 weatherEntry() const { return m_weatherEntry; } // phase 11: the weather's loop playing (0 none)
    f32 weatherVolume() const { return m_weatherVolume; }
    // Phase 14: the loops of doodads, terrain emitters and creatures playing; footsteps of others started.
    struct ModelSoundStats {
        u32 doodads = 0;     // doodad sounds near the listener (loops and timed ones)
        u32 loops = 0;       // their loops playing (doodads, emitters, creatures, objects' $DSL)
        u32 events = 0;      // animation events heard (since init)
        u32 footsteps = 0;   // footsteps played (since init): the player's and others'
        f32 updateMs = 0.0f; // the last frame's model sounds
        f64 sumMs = 0.0;     // since the last stats line
        u32 frames = 0;
    };
    const ModelSoundStats& modelSoundStats() const { return m_modelStats; }
    struct Stats {
        u32 played = 0; // sounds started
        f32 updateMs = 0.0f;
        f32 worstMs = 0.0f;
        // Logged every kStatsSeconds: the main thread's cost (GameAudio + AudioSystem updates) and the mixer's.
        f64 sumMs = 0.0;
        u32 frames = 0;
        u32 windowStartMs = 0;
    };
    static constexpr u32 kStatsSeconds = 10;
    const Stats& stats() const { return m_stats; }

private:
    static void uiCallback(void* self, UiSound sound) { static_cast<GameAudio*>(self)->playUi(sound); }
    static void itemCallback(void* self, u32 entry, bool pickup) { static_cast<GameAudio*>(self)->playItem(entry, pickup); }

    // Music: zone sets with silences between tracks, intros on entering a city, the server's music.
    enum class MusicKind : u8 { None, Zone, Intro, Server, Login };
    void updateMusic(const GameAudioFrame& f, bool night);
    void startMusic(u32 soundEntry, MusicKind kind, f32 fade);
    void startMusicFile(u64 pathHash, MusicKind kind, f32 fade);
    void updateAmbience(const GameAudioFrame& f, bool night);
    void weatherSounds(const GameAudioFrame& f); // phase 11 (app/GameAudioWeather.cpp)

    // Online: the game's rings and windows (f.play and f.entities set).
    void readEvents(const GameAudioFrame& f);
    void readFx(const GameAudioFrame& f);
    void readCombat(const GameAudioFrame& f);
    void watchWindows(const GameAudioFrame& f);
    void watchDeaths(const GameAudioFrame& f);
    // Ours (NPC voices, WoW.exe): the unit whose window is open (the interaction NPC, 0xB4E2D0: gossip, quests, vendor,
    // trainer, taxi, bank, auction house, stable, guild registrar, tabard; 0 none), its greeting or farewell
    // (0x60C3B0), a click's voice (CGUnit::PlayClickVoice 0x60C270), and one line at a time per unit (CGUnit +0xB1C).
    u64 interactNpc(const game::Gameplay& play) const;
    void npcGreeting(const GameAudioFrame& f, u64 guid, bool greeting);
    void clickVoice(const GameAudioFrame& f, u64 guid);
    bool npcSpeaking(u64 guid) const;
    bool npcSay(const GameAudioFrame& f, const game::Entity& unit, u32 soundEntry, i32 file);
    void updateLoops(const GameAudioFrame& f);
    void updateMissiles(const GameAudioFrame& f);
    void footsteps(const GameAudioFrame& f);

    const audio::CreatureSounds* soundsOf(const game::Entity& unit) const;
    bool positionOf(const GameAudioFrame& f, u64 guid, glm::dvec3& out) const;
    // The weapon a unit swings: ItemSubClass and whether it is wooden (impacts and parries).
    void weaponOf(const GameAudioFrame& f, const game::Entity& unit, bool offHand, u32& subclass, bool& wooden) const;
    // A sound a unit makes at most every `intervalMs` (wounds, attack grunts, aggro): false while it is too soon.
    bool unitCooldown(u64 guid, u32 kind, u32 nowMs, u32 intervalMs);
    void watchUnit(u64 guid, u32 nowMs);
    void vocalError(const GameAudioFrame& f, u32 vocal);
    u32 random();
    void trace(const char* what, u32 soundEntry);

    // Parity pass (app/GameAudioWorld.cpp): every other sound the 1.12 client plays.
    struct Body {
        u32 race = 1; // ChrRaces (offline: a human male)
        u32 sex = 0;
        u32 display = 0;
        u32 mount = 0; // the mount's display while mounted
        bool ghost = false;
    };
    Body bodyOf(const GameAudioFrame& f) const;
    void initParity();
    void resetParity(const GameAudioFrame& f); // a new world: what is around is known from now on, silently
    void bodySounds(const GameAudioFrame& f); // jumps, landings, splashes
    void waterSounds(const GameAudioFrame& f);                  // the camera under water, liquid loops nearby
    void environmentDamage(const GameAudioFrame& f, const game::CombatEvent& e); // falls, lava, drowning
    void meleeSwing(const GameAudioFrame& f, const game::CombatEvent& e);        // swooshes, impacts, wounds
    void parityEvent(const GameAudioFrame& f, const game::GameEvent& e); // items, exploration, taxi, errors
    void socialSounds(const GameAudioFrame& f);                          // invitations and duel requests
    void itemSounds(const GameAudioFrame& f);                   // equipped items, loot, the finger's drags
    void objectSounds(const GameAudioFrame& f);                 // doors, chests, campfires (GameObjectDisplayInfo)
    void sheatheSounds(const GameAudioFrame& f);                // the player drawing and putting away its weapon
    void creatureFidgets(const GameAudioFrame& f);              // idle creatures' stand and fidget sounds
    void deathThud(const GameAudioFrame& f, const game::Entity& unit, const glm::dvec3& at);
    void playLater(u32 soundEntry, u32 delayMs, const glm::dvec3& at, u8 priority);
    void delayedSounds(const GameAudioFrame& f);
    bool playedWithin(u32 soundEntry, u32 ms) const; // the same sound started that recently (drag + equip)
    u32 vocalPowerType(const GameAudioFrame& f) const;

    // Phase 14 (app/GameAudioModels.cpp): the models' sounds.
    void modelSounds(const GameAudioFrame& f);
    void doodadSounds(const GameAudioFrame& f);   // $DSL / $DSO of the doodads near the listener
    void emitterSounds(const GameAudioFrame& f);  // the terrain's emitters (MCSE)
    void creatureLoops(const GameAudioFrame& f);  // CreatureSoundData loops of the units near
    void reviewLoops(const GameAudioFrame& f);    // which of all those loops play (the loudest few)
    void actorEvents(const GameAudioFrame& f);    // footsteps, fidgets, wings, emotes, objects' sounds
    void eventFootstep(const GameAudioFrame& f, u32 actor, const glm::dvec3& at, const audio::CreatureSounds* sounds);
    void actorLoop(const GameAudioFrame& f, u32 actor, u32 index, u32 sound, const glm::dvec3& at, bool stop);
    const audio::CreatureSounds* actorSounds(const GameAudioFrame& f, u32 actor, u64* guid = nullptr);
    void mapActors(const GameAudioFrame& f); // this frame's actor -> unit table (online)
    u32 groundSoundAt(const GameAudioFrame& f, const glm::dvec3& at, bool* water) const;
    void resetModelSounds();
    bool eventFootsteps(const GameAudioFrame& f) const; // the player's steps come from its animation

    audio::AudioSystem* m_audio = nullptr;
    const audio::SoundData* m_data = nullptr;
    std::array<const audio::SoundEntry*, static_cast<u32>(UiSound::Count)> m_ui{};
    u64 m_loginMusic = 0; // the login screen's theme, by path (GlueXML plays it by name)
    bool m_trace = false;
    u32 m_random = 0x9E3779B9u;
    bool m_wasPaused = false;
    bool m_wasWorld = false;
    Stats m_stats;

    // Music and ambience.
    MusicKind m_musicKind = MusicKind::None;
    Options m_options; // phase 15 (options)
    audio::VoiceHandle m_music;
    u32 m_musicEntry = 0;
    u32 m_zoneSet = 0;
    u32 m_area = 0;
    bool m_night = false;
    u32 m_silenceUntilMs = 0;
    u32 m_introArea = 0; // the intro of the area last entered (played once per visit)
    struct IntroPlayed {
        u32 intro = 0;
        u32 atMs = 0;
    };
    std::array<IntroPlayed, 8> m_introsPlayed{};
    audio::VoiceHandle m_ambience;
    u32 m_ambienceEntry = 0;
    // Phase 11: the weather's loop and the volume it was last given.
    audio::VoiceHandle m_weather;
    u32 m_weatherEntry = 0;
    f32 m_weatherVolume = 0.0f;

    // Online state followed across frames.
    u64 m_cursorsPlayer = 0;
    u32 m_eventCursor = 0;
    u32 m_fxCursor = 0;
    u32 m_combatCursor = 0;
    u32 m_chatCursor = 0;
    u64 m_target = 0;
    u8 m_dialogKind = 0;
    u64 m_dialogNpc = 0;
    bool m_lootOpen = false;
    // Ours (NPC voices): the interaction NPC, the last unit clicked and its clicks (0xC4D908 / 0xC4D970: one for all
    // units), and the voice lines the NPCs are saying.
    u64 m_interactNpc = 0;
    u64 m_clickGuid = 0;
    u32 m_clickCount = 0;
    struct NpcVoice {
        u64 guid = 0;
        audio::VoiceHandle voice;
    };
    std::array<NpcVoice, 4> m_npcVoices{};
    u32 m_serverWindows = 0; // vendor, bank, book, trade, auction house, mailbox, taxi map open last frame
    u32 m_vocalAtMs = 0;
    u32 m_tellAtMs = 0;

    // Loops attached to a unit: precasts and channels.
    struct Loop {
        u64 guid = 0;
        u32 spell = 0;
        bool channel = false;
        audio::VoiceHandle voice;
    };
    std::array<Loop, 16> m_loops{};
    // Spell missiles in flight: their loop goes from the caster to the target, the impact sounds on arrival.
    struct Missile {
        u64 target = 0;
        glm::dvec3 from{0.0};
        u32 startMs = 0;
        u32 arriveMs = 0;
        u32 impact = 0; // SoundEntries
        u8 priority = 0;
        audio::VoiceHandle loop;
    };
    std::array<Missile, 16> m_missiles{};
    // Units heard recently (wounds, grunts, aggro: rate limits) and units in combat (their deaths).
    struct UnitMemory {
        u64 guid = 0;
        std::array<u32, 4> lastMs{}; // per kind: wound, grunt, aggro, death watch
        bool alive = true;
    };
    std::array<UnitMemory, 32> m_units{};

    // Footsteps.
    audio::GroundSounds m_ground;
    f32 m_stepDistance = 0.0f;
    bool m_leftFoot = false;

    // Parity pass: the sounds the client's code names (SoundEntries / ZoneMusic rows "(DONOTRENAME)").
    std::array<u32, 2> m_missSound{}; // "(DONOTRENAME)Combat Miss 1H" / "2H"
    u32 m_absorbSound = 0;            // "(DONOTRENAME)AbsorbGetHit"
    u32 m_fallSound = 0;              // "DamageFalling" (EnvironmentalDamage's fall kit has none)
    u32 m_underwaterSound = 0;        // "Underwater (DONOTRENAME)": the ambience under water
    u32 m_ghostSound = 0;             // "Ghost (DONOTRENAME)": the ghost's ambience
    u32 m_ghostMusic = 0;             // ZoneMusic "GhostMusic (DONOTRENAME)"
    Body m_body; // this frame's
    bool m_ghost = false;
    bool m_underwater = false;
    // The liquid loop near the listener (SoundWaterType), sampled every kWaterSampleMs.
    audio::VoiceHandle m_water;
    u32 m_waterEntry = 0;
    u32 m_waterSampleMs = 0;
    glm::dvec3 m_waterAt{0.0};
    // Sounds a moment later (a body hitting the ground after its death cry).
    struct Delayed {
        u32 entry = 0;
        u32 atMs = 0;
        glm::dvec3 position{0.0};
        u8 priority = 0;
    };
    std::array<Delayed, 8> m_delayed{};
    // Item sounds waiting for their template (display): from the UI, loot and equipment.
    struct PendingItem {
        u32 entry = 0;
        u32 sinceMs = 0;
        bool pickup = false;
    };
    std::array<PendingItem, 8> m_items{};
    std::array<u32, 19> m_equipped{}; // the player's equipment, by slot, last frame
    bool m_equippedKnown = false;
    u32 m_equipQuietUntilMs = 0;
    u32 m_inventoryVersion = 0;
    // Game objects around the listener: their state (doors and chests opening) and loops (campfires).
    struct ObjectMemory {
        u64 guid = 0;
        u32 state = 0;
        bool seen = false;
        audio::VoiceHandle loop;
        u32 loopEntry = 0; // phase 14: what `loop` plays (an animated object's own $DSL of it is not doubled)
    };
    std::array<ObjectMemory, 24> m_objects{};
    u32 m_objectScanMs = 0;
    // Phase 13 (living world objects): the server's last SMSG_PLAY_OBJECT_SOUND, so a custom animation's own sound
    // is not played twice (the fishing bobber's bite comes with both).
    u64 m_lastObjectSoundGuid = 0;
    u32 m_lastObjectSound = 0;
    u32 m_lastObjectSoundMs = 0;
    u32 m_fidgetAtMs = 0; // the next idle creature sound
    u32 m_sheath = 0xFF; // the player's sheath state last frame (0xFF unknown)
    bool m_invitePending = false;
    bool m_duelPending = false;

    // Phase 14: the models' sounds (app/GameAudioModels.cpp).
    audio::WorldSoundTables m_worldSounds;
    bool m_modelSoundsOn = true;  // VD_MODEL_SOUNDS=0: no doodad, object or creature sounds of models
    bool m_emittersOn = true;     // VD_EMITTERS=0
    bool m_otherStepsOn = true;   // VD_OTHER_FOOTSTEPS=0: only the player's own footsteps
    u64 m_interiorKey = 0;        // the room whose music plays (GameAudioFrame::interiorKey)
    u64 m_modelSoundsMap = 0;     // the map whose emitters play (its name's hash)
    ModelSoundStats m_modelStats;
    // A loop of a model (a doodad's, a terrain emitter's, a unit's): what it plays and its voice when it does.
    struct LoopVoice {
        u32 entry = 0;
        audio::VoiceHandle voice;
    };
    // The map's doodad sounds near the listener (indices into WorldSoundTables::doodadSounds()), looked for a few times
    // a second, and the loops among them.
    std::array<u32, 128> m_doodadNear{};
    u32 m_doodadNearCount = 0;
    u32 m_doodadQueryMs = 0;
    glm::dvec3 m_doodadQueryAt{1e30};
    u32 m_doodadGeneration = ~0u;
    struct DoodadLoop {
        u32 record = ~0u;
        LoopVoice loop;
    };
    std::array<DoodadLoop, 48> m_doodadLoops{};
    u32 m_lastCycleMs = 0; // the animation clock of the last frame (timed doodad events)
    // The terrain's emitters: when each sounds next and how many sounds its group has left.
    struct EmitterState {
        u32 nextMs = 0;
        u16 left = 0;
        LoopVoice loop; // a looping row's
    };
    std::array<EmitterState, 64> m_emitters{};
    // Units' CreatureSoundData loops.
    struct CreatureLoop {
        u64 guid = 0;
        glm::dvec3 at{0.0};
        LoopVoice loop;
    };
    std::array<CreatureLoop, 12> m_creatureLoops{};
    u32 m_creatureScanMs = 0;
    // reviewLoops(): every loop that could play and how loud it would be.
    struct LoopCandidate {
        LoopVoice* loop = nullptr;
        glm::dvec3 at{0.0};
        f32 audibility = 0.0f;
        f32 maxDistance = 0.0f;
        f32 minDistance = 0.0f;
        bool playing = false;
    };
    std::array<LoopCandidate, 320> m_loopCandidates{};
    u32 m_loopReviewMs = 0;
    // Actors' own loops ($DSL of transports, elevators and animated objects): one per actor, as the client keeps.
    struct ActorLoop {
        u32 actor = ~0u;
        u32 index = 0; // the event (its point follows the actor)
        u32 entry = 0;
        audio::VoiceHandle voice;
    };
    std::array<ActorLoop, 16> m_actorLoops{};
    // This frame's actor -> unit table (sorted by actor), built when an event needs it.
    struct ActorUnit {
        u32 actor = 0;
        u64 guid = 0;
    };
    std::array<ActorUnit, 512> m_actorUnits{};
    u32 m_actorUnitCount = 0;
    u32 m_actorUnitsMs = 0;
    // Other units' footsteps playing (a few at a time).
    std::array<audio::VoiceHandle, 8> m_otherSteps{};

    // Development: the last sounds started.
    struct Recent {
        u32 entry = 0;
        u32 atMs = 0;
        std::array<char, 40> what{};
    };
    std::array<Recent, 12> m_recent{};
    u32 m_recentNext = 0;
    u32 m_nowMs = 0;
};

} // namespace vd
