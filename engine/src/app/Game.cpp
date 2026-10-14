#include "app/Game.h"

#include "app/AppUpdates.h" // phase 22 (updates)
#include "app/HudDemo.h"
#include "app/SettingsFile.h"
#include "core/Config.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Locale.h"
#include "core/Log.h"
#include "core/Text.h"
#include "core/Profile.h"
#include "core/Version.h"
#include "game/CharacterNames.h"
#include "game/SocialText.h"
#include "game/UnitInfo.h"
#include "db/DbcView.h"
#include "net/Crypto.h"
#include "net/UpdateFields.gen.h"
#include "pack/AppPack.h" // phase 22 (apppack)
#include "app/Pairing.h"  // phase 22 (pairing)
#include "pack/CollisionFormat.h"
#include "pack/ModelFormat.h"
#include "pack/PackManifest.h"
#include "pack/TerrainFormat.h"
#include "platform/MappedFile.h"
#include "render/ScreenFx.h"

#include <imgui.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>

namespace vd {
namespace {

constexpr f64 kStatsWindowSeconds = 5.0;
constexpr f32 kHitchMs = 25.0f; // frames above this are logged with their breakdown
constexpr i64 kFrameTargetNanos = 16'666'667; // 60 fps: the game thread's work target for ADPF
constexpr f32 kCameraHeightAboveGround = 25.0f; // after a teleport
constexpr f32 kCollisionRadius = 96.0f;         // yards of collision kept loaded around the player
constexpr u32 kLoadingMinimumMs = 2000;         // the loading screen after entering a world, at least

// CreatureDisplayInfo.dbc (5875): 1 model (CreatureModelData), 4 scale. CreatureModelData.dbc: 14 collision width,
// 15 collision height (vMaNGOS DBCStructure.h).
constexpr u32 kDisplayModelField = 1;
constexpr u32 kDisplayScaleField = 4;
constexpr u32 kModelCollisionWidthField = 14;
constexpr u32 kModelCollisionHeightField = 15;

// Map.dbc (5875): field 1 is the map directory ("Azeroth").
constexpr u32 kMapDirectoryField = 1;

struct Destination {
    const char* name;
    const char* map;
    f64 x;
    f64 y;
};
// Benchmark areas of docs/PERFORMANCE.md and the phase 2 acceptance routes.
constexpr Destination kDestinations[] = {
    {"Goldshire", "Azeroth", -9464.0, 62.0},
    {"Stormwind", "Azeroth", -8913.0, 554.0},
    {"Sentinel Hill (Westfall)", "Azeroth", -10628.0, 1037.0},
    {"Darkshire (Duskwood)", "Azeroth", -10560.0, -1180.0},
    {"Grom'gol (Stranglethorn Vale)", "Azeroth", -12356.0, 208.0},
    {"Kharanos (Dun Morogh)", "Azeroth", -5601.0, -482.0},
    {"Brill (Tirisfal Glades)", "Azeroth", 2260.0, 288.0},
    {"Orgrimmar", "Kalimdor", 1629.0, -4373.0},
    {"Valley of Trials (Durotar)", "Kalimdor", -618.0, -4251.0},
    {"The Crossroads (The Barrens)", "Kalimdor", -441.0, -2596.0},
    {"Thunder Bluff", "Kalimdor", -1277.0, 124.0},
    {"Darnassus", "Kalimdor", 9947.0, 2482.0},
};

// Light.dbc fog distances are tuned for the original ~500 yard view: scale them with the chosen view
// distance so foggy zones stay proportionally foggier than clear ones.
constexpr f32 kReferenceFogEnd = 500.0f;

// Phase 10: WorldObjects' doodad distances at an environment detail of 100 %, how long the settings stand still
// before they are saved, and the terrain kept in the background (ComponentCallbacks2.TRIM_MEMORY_BACKGROUND and up).
constexpr f32 kDoodadBaseDistance = 80.0f;
constexpr f32 kDoodadDistancePerRadius = 30.0f;
constexpr f32 kSettingsSaveSeconds = 1.5f;
constexpr i32 kTrimBackground = 40;

// Phase 22 (distribution, ADR-0005 §3): the packs a bake left with another recipe than this version's (an older
// baker made them: VanillaDroid Setup bakes them again), from the manifest.json the bakes write. Packs baked before
// phase 22 have no manifest: nothing to compare.
void logOutdatedPacks(const std::filesystem::path& packsDir) {
    const Result<pack::PackManifest> manifest = pack::PackManifest::load(packsDir);
    if (!manifest) {
        VD_LOG_WARN("Packs: %s", manifest.error().message.c_str());
        return;
    }
    if (manifest.value().packs.empty()) {
        return;
    }
    const std::vector<std::string> outdated = pack::outdatedPacks(manifest.value(), pack::releaseRecipes());
    for (const std::string& file : outdated) {
        VD_LOG_WARN("Packs: %s is missing or was baked for another version (bake it again: %s)", file.c_str(),
                    pack::bakeStepOf(file).step.c_str());
    }
    VD_LOG_INFO("Packs baked by %s for a %s client, %zu outdated (VanillaDroid %s)",
                manifest.value().bakedBy.c_str(), manifest.value().clientLocale.c_str(), outdated.size(), kVersion);
}

} // namespace

bool Game::init(const PlatformInfo& platform) {
    VD_LOG_INFO("VanillaDroid engine init (content scale %.2f)", platform.contentScale);
    m_contentScale = platform.contentScale;
    m_uiScale = platform.uiScale > 0.1f ? platform.uiScale : 1.0f;
    m_pixelsPerMm = platform.pixelsPerMm; // phase 20 (edit)
    m_hudDemo = VD_DEV_ENV("VD_HUD_DEMO") != nullptr;
    // Phase 13 (development): VD_FAKE_CONTROL=lost[:seconds[:after]]: the offline character loses control for that long
    // (8 s), `after` seconds (2) after it stands, as a fear would take it.
    if (const char* fake = VD_DEV_ENV("VD_FAKE_CONTROL"); fake && std::strncmp(fake, "lost", 4) == 0) {
        f32 seconds = 8.0f, after = 2.0f;
        std::sscanf(fake + 4, ":%f:%f", &seconds, &after);
        m_fakeControlMs = static_cast<u32>(std::max(0.0f, seconds) * 1000.0f);
        m_fakeControlAfterMs = static_cast<u32>(std::max(0.0f, after) * 1000.0f);
    }
    m_selectionDemo = VD_DEV_ENV("VD_SELECTION_DEMO") != nullptr; // parity: the selection circles, offline
    if (const char* speed = VD_DEV_ENV("VD_TIME_SPEED")) { // phase 11: game minutes per second (0 = frozen)
        m_timeSpeed = 2.0f * static_cast<f32>(std::atof(speed));
    }
    m_packsDir = platform.packsDir;
    {
        // Phase 21 (languages): VD_LOCALE overrides client.cfg (development: try a language without editing the
        // configuration). A known code is spelled as the client does ("eses" -> "esES").
        const Config config = platform.configPath.empty() ? Config{} : Config::load(platform.configPath);
        const char* localeOverride = VD_DEV_ENV("VD_LOCALE");
        m_locale = localeOverride ? std::string(localeOverride) : config.get("locale", "enUS");
        if (const std::string_view canonical = canonicalLocale(m_locale); !canonical.empty()) {
            m_locale = std::string(canonical);
        }
    }
    // Phase 22 (phonebake): the bake on the phone, followed on its own thread (the first-run screen, the
    // updates).
    m_phoneBake.init(PhoneBake::configFor(platform));
    // Phase 22 (pairing): without the game's data (or with a download left half way) the first-run screen instead,
    // with app.vdpk alone (app/GameFirstRun.cpp).
    if (needsFirstRun(m_packsDir, platform.cacheDir) || VD_DEV_ENV("VD_PAIRING_DEMO")) {
        return initFirstRun(platform);
    }
    m_jobs.init();
    m_monitor.start(platform); // phase 10: temperature, battery and memory, from now on
    if (Result<void> result = m_renderer.init(platform); !result) {
        VD_LOG_ERROR("Renderer init failed: %s", result.error().message.c_str());
        return false;
    }
    openPacks();
    logOutdatedPacks(m_packsDir); // phase 22
    // Phase 22 (phonebake): "Prepare now" of the notice of game data baked for another version bakes those steps
    // on the phone, with the WoW copied to it (app/PhoneBake; before start(): the notice offers it then).
    appUpdates().setPhoneBaker([this](const std::vector<pack::StepPlan>& steps) {
        std::vector<PhoneBakeJob> jobs;
        for (const pack::StepPlan& step : steps) {
            if (isPhoneBakeStep(step.step)) { // not app.vdpk, nor a second client's overlays
                jobs.push_back({step.step, step.maps});
            }
        }
        if (!m_phoneBake.start(std::move(jobs), PhoneBakeKind::Update)) {
            VD_LOG_WARN("Updates: the phone cannot bake now (one bake runs, or nothing it can bake)");
        }
    });
    // Phase 22 (updates): a newer release and game data baked for another version, on the updates thread.
    appUpdates().start(platform, m_packsDir, platform.cacheDir);
    findWorldMaps();
    if (Result<void> lighting = m_lighting.init(m_dbPack); !lighting) {
        VD_LOG_WARN("Lighting: %s (fixed light)", lighting.error().message.c_str());
    }
    if (Result<void> sky = m_sky.open(m_packsDir, m_texturePack, m_lighting, m_renderer); !sky) { // phase 11
        VD_LOG_WARN("Sky: %s", sky.error().message.c_str());
    }
    m_terrainDraws.reserve(Terrain::kMaxLoadedTiles);
    m_horizonDraws.reserve(1024); // HorizonRenderer::kMaxInstances
    m_modelDraws.reserve(WorldObjects::kMaxVisible);
    m_modelInstances.reserve(WorldObjects::kMaxVisible);
    m_modelGroups.reserve(8192);
    m_effects.init(&m_actors);
    m_effects.enabled = VD_DEV_ENV("VD_FX_OFF") == nullptr;
    const fx::ParticleSystem::Settings& particleSettings = m_effects.particles().settings();
    m_particleDraws.reserve(particleSettings.maxEmitters + particleSettings.maxRibbons +
                            4); // + selection circles
    m_objectViews.reserve(1024);
    m_uvTransforms.reserve(16384);
    if (m_texturePack.entryCount() > 0) {
        m_weather.init(m_texturePack, m_renderer, platform.weather); // phase 11
        if (Result<void> objects = m_objects.open(m_packsDir, m_texturePack, m_renderer); objects) {
            m_terrain.setListener(&m_objects);
            // Phase 18 (world): the terrain under a point (its holes left out) for the 1.12 camera group rule, and
            // VD_WORLD_PORTALS=0 for the phase 3 rule (boxes and portal spheres), to compare.
            m_objects.setTerrainQuery(&Game::terrainBelow, this);
            if (const char* portals = VD_DEV_ENV("VD_WORLD_PORTALS"); portals && portals[0] == '0') {
                m_objects.portalCulling = false;
            }
        } else {
            VD_LOG_WARN("World objects disabled: %s", objects.error().message.c_str());
        }
        if (Result<void> actors = m_actors.open(m_packsDir, m_dbPack, m_texturePack, m_renderer); !actors) {
            VD_LOG_WARN("Actors disabled: %s", actors.error().message.c_str());
        }
        // Phase 14: the terrain's detail doodads (grass.vdpk; without it, none). VD_GRASS=0 turns them off.
        if (Result<void> grass = m_grass.open(m_packsDir, m_texturePack, m_renderer); !grass) {
            VD_LOG_WARN("Grass disabled (vd_baker bake --only grass): %s", grass.error().message.c_str());
        }
        m_grassDraws.reserve(1024);
        m_grassInstances.reserve(GroundEffects::kMaxInstances);
        // Phase 11: the light of interiors for doodads and actors (VD_INTERIORS=0: the outdoor light, to compare).
        const char* interiors = VD_DEV_ENV("VD_INTERIORS");
        m_objects.interiorLighting = interiors == nullptr || std::strcmp(interiors, "0") != 0;
        // Phase 14: animated doodads and their fade with the distance (VD_DOODAD_ANIM=0 / VD_DOODAD_FADE=0: as before).
        const char* doodadAnim = VD_DEV_ENV("VD_DOODAD_ANIM");
        const char* doodadFade = VD_DEV_ENV("VD_DOODAD_FADE");
        m_objects.animateDoodads = doodadAnim == nullptr || std::strcmp(doodadAnim, "0") != 0;
        m_objects.fadeDoodads = doodadFade == nullptr || std::strcmp(doodadFade, "0") != 0;
        m_objects.traceDoodads = VD_DEV_ENV("VD_TRACE_DOODADS") != nullptr;
        if (doodadFade && std::strchr(doodadFade, '.')) { // VD_DOODAD_FADE=0.5: every doodad that faded (captures)
            m_objects.doodadFadeCeiling = std::clamp(std::strtof(doodadFade, nullptr), 0.0f, 1.0f);
        }
        m_actors.lightSource = &m_objects;
        m_units.init(m_dbPack);
        // Phase 18 (motion): the liquid over a unit's feet (the creatures swim by the client's rule).
        m_units.setLiquidQuery(
            [](const void* game, f32 x, f32 y, f32 z) -> std::optional<f32> {
                const std::optional<physics::LiquidSample> liquid =
                    static_cast<const Game*>(game)->m_collision.liquidAt(x, y, z + 50.0, z - 2.0);
                return liquid ? std::optional<f32>(liquid->level) : std::nullopt;
            },
            this);
        m_motionDemo.init();
        m_farSight.init();
        m_serverObjects.init(m_dbPack);
        m_serverObjects.setActors(&m_actors); // phase 13: doors, chests and the bobber animate
        m_serverObjects.setCollision(&m_collision, m_packsDir); // phase 13: closed doors block
    }
    // Phase 9: spell visuals and combat animations.
    m_spellVisualData.init(m_dbPack);
    m_units.setVisualData(&m_spellVisualData);
    m_unitFades.init(m_dbPack); // phase 11: ghosts, stealth, invisibility
    m_units.setFades(&m_unitFades);
    // Phase 14 (bodyfx): shadows, footprints, ripples, sprays, breath and blood of the units' bodies.
    if (m_bodyFx.init(m_dbPack, m_packsDir)) {
        m_units.setBodyEffects(&m_bodyFx, &m_effects);
        m_fight.setBodyEffects(&m_bodyFx);
    }
    m_decalDraws.reserve(16);
    if (const char* ghost = VD_DEV_ENV("VD_GHOST"); platform.ghost || (ghost && std::strcmp(ghost, "0") != 0)) {
        m_devGhost = true; // development: the ghost's world offline (--ghost, the intent's "ghost")
    }
    if (const char* glow = VD_DEV_ENV("VD_FFX_GLOW")) {
        m_devGlow = std::strcmp(glow, "0") != 0 ? 1 : 0;
    }
    if (const char* death = VD_DEV_ENV("VD_FFX_DEATH")) {
        m_devDeath = std::strcmp(death, "0") != 0 ? 1 : 0;
    }
    m_spellEffects.init(&m_spellVisualData, &m_effects, &m_actors, &m_units);
    initSpellFx();                           // phase 14: beams, tints, flashes, camera shakes
    m_cinematics.open(m_packsDir, m_dbPack); // phase 14
    m_glueStage.open(m_packsDir);            // phase 18: the glue screens' scenes
    if (const char* preview = VD_DEV_ENV("VD_GLUE_PREVIEW")) {
        static constexpr const char* kNames[] = {"", "mainmenu", "human", "orc", "dwarf", "nightelf", "scourge", "tauren"};
        static constexpr u8 kRaces[] = {0, 0, 1, 2, 3, 4, 5, 6}; // the scene's own race
        const std::string_view spec(preview);
        const std::string_view name = spec.substr(0, spec.find(':'));
        for (u32 i = 1; i < std::size(kNames); ++i) {
            if (name == kNames[i]) {
                m_gluePreview.background = static_cast<GlueBackground>(i);
                m_gluePreview.hasCharacter = kRaces[i] != 0;
                m_gluePreview.character.race = kRaces[i];
            }
        }
        // ...[:race[:sex[:hairStyle:hairColor:facialHair]]] (the hair: checking a look, e.g. a bald gnome's brows).
        unsigned race = 0, gender = 0, hairStyle = 0, hairColor = 0, facialHair = 0;
        if (std::sscanf(preview + name.size(), ":%u:%u:%u:%u:%u", &race, &gender, &hairStyle, &hairColor,
                        &facialHair) >= 1 &&
            race > 0) {
            m_gluePreview.hasCharacter = true;
            m_gluePreview.character.race = static_cast<u8>(race);
        }
        m_gluePreview.character.gender = static_cast<u8>(gender);
        m_gluePreview.character.hairStyle = static_cast<u8>(hairStyle);
        m_gluePreview.character.hairColor = static_cast<u8>(hairColor);
        m_gluePreview.character.facialHair = static_cast<u8>(facialHair);
    }
    if (const char* trace = VD_DEV_ENV("VD_TRACE_EVENTS")) {
        m_traceEvents = std::strcmp(trace, "0") != 0;
    }
    if (const char* cinematic = VD_DEV_ENV("VD_CINEMATIC")) {
        m_devCinematic = static_cast<u32>(std::strtoul(cinematic, nullptr, 10)); // development: one offline
    }
    m_reactions.init(m_dbPack);
    m_gameData.init(m_dbPack);
    game::loadCharacterNames(m_dbPack); // race and class names of the interface (ChrRaces, ChrClasses)
    m_abilityData.init(m_dbPack);
    m_session.setGameData(&m_gameData, &m_reactions);
    m_reactions.setPlayerStanding(&m_session.play().standing()); // phase 13: forced reactions, at war
    m_socialData.init(m_dbPack);
    game::setChannelNames(&m_socialData); // phase 21: the zone channels shown in the player's language
    m_charCreate.init(m_dbPack);
    m_itemData.init(m_dbPack);
    m_session.play().inventory().setData(&m_itemData);
    m_areaData.init(m_dbPack);
    m_worldMapData.init(m_dbPack);
    m_taxiData.init(m_dbPack);
    m_session.play().travel().setData(&m_areaData, &m_taxiData);
    m_exchangeData.init(m_dbPack);
    m_session.play().exchange().setData(&m_exchangeData);
    m_tradeSkillData.init(m_dbPack); // phase 13 (professions)
    m_session.play().tradeSkills().setData(&m_tradeSkillData);
    m_languageData.init(m_dbPack); // phase 18 (extras): B4, the other languages garbled as 1.12 does
    m_session.play().setLanguageData(&m_languageData);
    m_petData.init(m_dbPack); // phase 15 (pets)
    m_minimapData.init(m_dbPack);
    m_transports.init(m_dbPack, &m_taxiData, m_packsDir);
    game::setTransportResolver(&Transports::placementAtCallback, &m_transports); // phase 18 (motion): passengers
    m_player.setTransports({&Transports::transportOfObjectCallback, &Transports::placementCallback, &m_transports,
                            &Transports::solidCallback});
    // The touch UI (phase 7). Phase 22 (apppack): ui.vdpk (the client's art and texts) and app.vdpk (ours: copied from
    // the APK on Android, next to the packs on the desktop); VD_UI_GALLERY=apponly leaves ui.vdpk out, as on a phone
    // without data yet.
    const std::filesystem::path appPack =
        platform.appPack.empty() ? m_packsDir / pack::kAppPackFile : std::filesystem::path(platform.appPack);
    const char* galleryPage = VD_DEV_ENV("VD_UI_GALLERY");
    const std::filesystem::path uiPack =
        galleryPage && std::strcmp(galleryPage, "apponly") == 0 ? std::filesystem::path{} : m_packsDir / "ui.vdpk";
    if (m_uiResources.init(m_renderer, m_texturePack.entryCount() > 0 ? &m_texturePack : nullptr, uiPack,
                           appPack)) {
        m_gameHud.init(&m_uiResources, m_renderer.uiQuadCapacity());
        m_uiGallery.init(&m_uiResources, m_renderer.uiQuadCapacity());
        m_uiGallery.setTravelData(&m_areaData, &m_worldMapData, &m_taxiData);
        m_screens.init(&m_gameData, &m_uiResources, &m_abilityData);
        m_screens.social().init(&m_gameData, &m_socialData, &m_charCreate, &m_uiResources);
        m_screens.inventory().init(&m_gameData, &m_itemData, &m_uiResources);
        m_screens.combat().setItemData(&m_itemData); // phase 13: enchantment names in the combat log
        m_screens.combat().setPetData(&m_petData);   // phase 15 (pets): the pet's page title
        m_screens.pets().setPetData(&m_petData);
        m_screens.travel().init(&m_areaData, &m_worldMapData, &m_taxiData, &m_uiResources);
        // Phase 17 (scores): WorldStateUI.dbc, the battlegrounds' maps; the gallery's pages use the same.
        m_screens.worldStates().init(m_dbPack, &m_areaData, &m_worldMapData, &m_uiResources);
        m_uiGallery.setWorldStates(&m_screens.worldStates());
        m_uiGallery.setLanguageData(&m_languageData); // phase 18 (extras): the "languages" page
        m_screens.exchange().init(&m_gameData, &m_exchangeData, &m_uiResources);
        m_screens.minimap().init(&m_gameData, &m_minimapData, &m_uiResources); // phase 15: indoor maps
        m_screens.quests().init(m_dbPack, &m_gameData, &m_areaData); // phase 13: headers, rewards, spells
        m_screens.help().init(&m_gameData, &m_dbPack); // phase 15 (help): ticket categories, the GM survey
        m_screens.setPortraits(&m_portraits); // parity: the party, pet and target-of-target frames
        m_uiGallery.setPortraits(&m_actors, &m_portraits);
        m_screens.setModelViews(&m_modelViews); // phase 15: the character sheet, dressing room, pet, inspect...
        m_screens.setAppearance(&m_actors.appearance()); // phase 15 (items): the dressing rooms' outfits
        // Phase 15 (character): SkillRaceClassInfo / ChrClasses, and the player's and the inspected unit's models.
        m_screens.character().init(m_dbPack, &m_gameData, &m_abilityData, &m_itemData);
        m_screens.character().setModelSources(&m_modelViews, &m_actors, &Game::portraitActor, this);
        m_uiGallery.setModelViews(&m_actors, &m_modelViews);
    }
    m_portraits.setActorQuery(&Game::portraitActor, this);
    m_portraitActors.reserve(64);
    m_portraitDraws.reserve(kMaxPortraitRenders);
    m_modelViewDraws.reserve(kModelViewSlots);
    // Sound (phase 9). The desktop stays silent unless asked (--audio): the PC is a development machine.
    audio::AudioConfig audioConfig;
#if defined(__ANDROID__)
    const std::string audioOutput = platform.audio.empty() ? "device" : platform.audio;
#else
    const std::string audioOutput = platform.audio.empty() ? "off" : platform.audio;
#endif
    if (!audio::parseAudioOutput(audioOutput, audioConfig.output, audioConfig.wavPath)) {
        VD_LOG_WARN("Unknown audio output '%s' (off | null | device | wav:<file>): off", audioOutput.c_str());
    }
    audioConfig.locale = m_locale; // phase 21: sound.<locale>.vdpk, the voices recorded in that language
    if (Result<void> sound = m_audio.init(audioConfig, m_packsDir); !sound) {
        VD_LOG_WARN("Audio: %s", sound.error().message.c_str());
    }
    if (m_audio.active()) {
        m_soundData.init(m_dbPack);
    }
    m_gameAudio.init(&m_audio, &m_soundData);
    m_screens.setUiSoundSink(m_gameAudio.uiSink());
    // Phase 14 (sound): the doodads' sound events and the terrain's emitters; the buildings' rooms (also without
    // audio: their names).
    m_gameAudio.initWorldSounds(m_packsDir);
    m_wmoAreas.init(m_dbPack, m_packsDir);
    if (const char* interior = VD_DEV_ENV("VD_INTERIOR_AUDIO"); interior && interior[0] == '0') {
        m_interiorAudio = false;
    }
    if (const char* listener = VD_DEV_ENV("VD_LISTENER"); listener && std::string_view(listener) == "camera") {
        m_listenerAtCamera = true;
    }
    m_traceRooms = VD_DEV_ENV("VD_TRACE_AUDIO") != nullptr;
    m_actorFrame.draws.reserve(Actors::kMaxVisible * 8);
    m_actorFrame.instances.reserve(Actors::kMaxVisible * 8);
    m_actorFrame.geosets.reserve(Actors::kMaxVisible * 64);
    m_actorFrame.colors.reserve(Actors::kMaxVisible * 32);
    m_actorFrame.uvTransforms.reserve(Actors::kMaxVisible * 8);
    m_camera.fovY = 1.0f;
    if (findWorldMap("Azeroth")) {
        teleport("Azeroth", kDestinations[0].x, kDestinations[0].y);
    } else if (!m_worldMaps.empty()) {
        openMap(m_worldMaps.front().directory);
    }
    {
        // Phase 10: the saved settings (benchmarks keep the defaults, so runs compare), then the overrides of the
        // command line / launch intent.
        SettingsView& settings = m_screens.settings();
        settings.showFps = false;
        m_settingsPersist = platform.benchmark.empty() && !platform.cacheDir.empty();
        if (m_settingsPersist) {
            m_settingsPath = std::filesystem::path(platform.cacheDir) / "settings.cfg";
            if (std::ifstream file(m_settingsPath, std::ios::binary); file) {
                const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                parseSettings(text, settings);
                VD_LOG_INFO("Settings read from %s", m_settingsPath.string().c_str());
            }
        }
        static constexpr const char* kPresetNames[kQualityPresets] = {"low", "medium", "high", "ultra"};
        for (u32 preset = 0; preset < kQualityPresets; ++preset) {
            if (platform.quality == kPresetNames[preset]) {
                applyQualityPreset(settings, preset);
            }
        }
        if (!platform.dynamicQuality.empty()) {
            settings.dynamicQuality = platform.dynamicQuality != "off" && platform.dynamicQuality != "0";
        }
        m_fixedRenderScale = platform.renderScale;
        initGpuOptions(settings); // phase 18 (world)
        // Phase 15 (options): the macros' files next to the settings (1.12's WTF\Account\... under the same folder),
        // written by the monitor thread like the settings.
        if (!platform.cacheDir.empty()) {
            m_screens.macros().setStorage(
                platform.cacheDir,
                [](void* self, const std::filesystem::path& path, std::string text) {
                    static_cast<Game*>(self)->m_monitor.writeFile(path, std::move(text));
                },
                this);
            // Phase 20: the touch layout (WTF\Account\<ACCOUNT>\<realm>\<character>\touch-layout.txt).
            m_screens.touch().setStorage(
                platform.cacheDir,
                [](void* self, const std::filesystem::path& path, std::string text) {
                    static_cast<Game*>(self)->m_monitor.writeFile(path, std::move(text));
                },
                this);
            // Phase 18 (glue): WTF\Config.wtf (the account name kept, the last realm and character, the server's address).
            m_screens.glue().setStorage(
                platform.cacheDir,
                [](void* self, const std::filesystem::path& path, std::string text) {
                    static_cast<Game*>(self)->m_monitor.writeFile(path, std::move(text));
                },
                this);
        }
        m_savedSettings = settings;
        m_pendingSettings = settings;
        m_viewDistance = settings.viewDistance;
        VD_LOG_INFO("Video: %s, world resolution %.0f%%%s, view %.0f yd, environment %.0f%%, spells %.0f%%, dynamic "
                    "quality %s%s",
                    settings.graphicsQuality < kQualityPresets ? kPresetNames[settings.graphicsQuality] : "custom",
                    (m_fixedRenderScale > 0.0f ? m_fixedRenderScale : settings.renderScale) * 100.0f,
                    m_fixedRenderScale > 0.0f ? " (fixed)" : "", settings.viewDistance,
                    settings.environmentDetail * 100.0f, settings.spellDetail * 100.0f,
                    settings.dynamicQuality && m_fixedRenderScale <= 0.0f ? "on" : "off",
                    settings.limitFps ? ", 30 fps" : "");
    }
    m_benchmark.setMinutes(platform.benchMinutes);
    m_exitAfterBenchmark = platform.exitAfterBenchmark;
    m_pendingSpawn = platform.spawn;
    m_hideDebugUi = platform.hideDebugUi;
    m_fakeKeyboard = platform.fakeKeyboard;
#if defined(__ANDROID__)
    // The touch UI takes the screen; three fingers on the world bring the development panels back.
    m_hideDebugUi = m_hideDebugUi || m_gameHud.ready();
#endif
    m_pendingWalk = platform.walk;
    m_travelTest.init(); // phase 18 (world): VD_WORLD_TRAVEL (with --walk)
    // Development modes that show the world without a server skip the login screen.
    m_screens.setOffline(!platform.benchmark.empty() || platform.walk || !platform.spawn.empty() ||
                         m_fishingDemo); // phase 14: VD_FISHING_DEMO
    m_commands = platform.commands;
    m_onlineWalk = platform.walkRoute;
    m_player.traceMovement = platform.traceMovement;

    // Server session: client.cfg, overridden by the command line / launch intent. No automatic login while
    // benchmarking (those measure rendering alone).
    const Config config = platform.configPath.empty() ? Config{} : Config::load(platform.configPath);
    // The interface's texts in the configured language (core/Text; English when the pack lacks it).
    text::load(uiPack, appPack, m_locale); // phase 22 (apppack): the clients' tables and ours
    net::LoginParams login;
    login.locale = m_locale; // phase 21: the server answers in it where it can
    // The realm server (1.12's realmlist), in one order for the login screen and this automatic login
    // (GlueScreens::serverAddress, app/GlueModel glueRealmList): the launch's --realmd / intent extra, the
    // player's own (WTF\Config.wtf, realmlist.wtf), client.cfg's realm.address; none: the login screen asks for it.
    GlueScreens& glue = m_screens.glue();
    glue.setServerSources(platform.realmd, config.get("realm.address"));
    const std::string_view server = glue.serverAddress();
    if (net::parseRealmdAddress(server, login.realmdHost, login.realmdPort) != net::AddressError::None) {
        login.realmdHost.clear();
    }
    login.account = !platform.account.empty() ? platform.account : config.get("account");
    login.realmName = config.get("realm.name");
    if (!platform.password.empty() && !login.account.empty()) {
        login.credentials = net::srpCredentials(login.account, platform.password);
    }
    m_session.configure(login, !platform.character.empty() ? platform.character : config.get("character"));
    if (m_session.hasCredentials() && platform.benchmark.empty()) {
        if (login.realmdHost.empty()) {
            VD_LOG_WARN("No realm server (--realmd, realm.address or the login screen): no automatic login");
        } else {
            VD_LOG_INFO("Logging in to %s:%u as %s", login.realmdHost.c_str(), login.realmdPort,
                        login.account.c_str());
            m_session.login();
        }
    }

    if (!platform.benchmark.empty() && !startBenchmark(platform.benchmark)) {
        VD_LOG_ERROR("Unknown or unavailable benchmark '%s'", platform.benchmark.c_str());
        m_exitRequested = m_exitAfterBenchmark;
    }
    m_startTime = Clock::now();
    return true;
}

void Game::openPacks() {
    if (m_packsDir.empty()) {
        VD_LOG_WARN("No packs directory configured");
        return;
    }
    struct Pack {
        pack::PackReader* reader;
        const char* file;
    };
    for (const Pack& p : {Pack{&m_dbPack, "db.vdpk"}, Pack{&m_texturePack, "textures.vdpk"}}) {
        if (Result<void> opened = p.reader->open(m_packsDir / p.file); !opened) {
            VD_LOG_WARN("Pack not loaded: %s", opened.error().message.c_str());
            continue;
        }
        VD_LOG_INFO("Pack %s: %u assets", p.file, p.reader->entryCount());
        // Phase 21 (languages): what a client in the player's language changes (its DBC texts, the world map tiles
        // with painted names), when it was baked (vd_baker bake --only locale).
        if (p.reader->locale() != m_locale) {
            if (Result<void> overlay = p.reader->openOverlay(m_locale); !overlay) {
                VD_LOG_WARN("Language overlay not loaded: %s", overlay.error().message.c_str());
            } else if (p.reader->hasOverlay()) {
                VD_LOG_INFO("Pack %s: %u assets in %s on top", p.file, p.reader->overlay()->entryCount(),
                            m_locale.c_str());
            }
        }
    }
}

void Game::findWorldMaps() {
    const std::optional<pack::PackAsset> mapDbc = m_dbPack.find("DBFilesClient\\Map.dbc");
    if (!mapDbc) {
        return;
    }
    Result<db::DbcView> maps = db::DbcView::parse(mapDbc->data, mapDbc->size);
    if (!maps) {
        return;
    }
    for (u32 record = 0; record < maps.value().recordCount(); ++record) {
        const std::string directory(maps.value().getString(record, kMapDirectoryField));
        std::error_code error;
        if (!directory.empty() && std::filesystem::exists(m_packsDir / pack::worldPackFileName(directory), error)) {
            m_worldMaps.push_back({maps.value().getU32(record, 0), directory});
        }
    }
    VD_LOG_INFO("World packs: %zu maps", m_worldMaps.size());
}

const Game::WorldMap* Game::findWorldMap(std::string_view directory) const {
    for (const WorldMap& map : m_worldMaps) {
        if (map.directory == directory) {
            return &map;
        }
    }
    return nullptr;
}

const Game::WorldMap* Game::findWorldMap(u32 id) const {
    for (const WorldMap& map : m_worldMaps) {
        if (map.id == id) {
            return &map;
        }
    }
    return nullptr;
}

void Game::enterWorld(const WorldEntry& entry) {
    const WorldMap* map = findWorldMap(entry.map);
    if (!map) {
        // An instance or a map without terrain (no world pack yet): the character waits where the server put it,
        // under the loading screen, and nothing is simulated or sent (the old map's collision must not answer).
        VD_LOG_WARN("The player is on map %u, which has no world pack", entry.map);
        m_collision.closeMap();
        m_player.leave(&m_actors);
        m_player.enterWorld(entry.position, entry.orientation);
        m_freeCamera = false;
        return;
    }
    // Phase 8: a far teleport on a ship or zeppelin brings the player's offset on it, not a world position.
    const u32 transportEntry = m_session.play().travel().transferTransport();
    glm::dvec3 position = entry.position;
    if (transportEntry != 0) {
        game::TransportPlacement placement;
        if (m_transports.placementOfEntry(transportEntry, m_session.lastUpdateMs(), placement) &&
            placement.map == entry.map) {
            position = glm::dvec3(game::transportToWorld(placement, glm::vec3(entry.position)));
        }
    }
    teleport(map->directory, position.x, position.y);
    m_snapToGround = false;
    m_camera.yaw = entry.orientation; // server orientation: radians from +X (north) towards +Y, like the camera
    m_camera.pitch = -0.3f;
    m_camera.position = position + glm::dvec3(0.0, 0.0, 2.0);
    // The character is the server's player from now on (its shape once its display is known).
    m_player.leave(&m_actors);
    m_player.enterWorld(position, entry.orientation);
    if (transportEntry != 0) {
        m_player.boardOnArrival(Transports::moTransportGuid(transportEntry), glm::vec3(entry.position),
                                entry.orientation);
    } else if (entry.transport != 0) { // phase 18: logged in on board (at its world place until it is placed)
        m_player.boardOnArrival(entry.transport, entry.transportOffset, entry.transportFacing);
    }
    m_playerDisplay = 0;
    m_freeCamera = false;
    if (const std::optional<f32> time = m_session.serverTimeOfDay()) {
        m_timeOfDay = *time;
        m_timeSpeed = 2.0f / 60.0f; // the server's clock runs at real speed: one game minute per minute
    }
}

bool Game::openMap(const std::string& directory) {
    if (m_terrain.isOpen() && m_terrain.mapDirectory() == directory) {
        return true;
    }
    if (const WorldMap* map = findWorldMap(directory)) {
        m_lighting.setMap(map->id);
    }
    VD_PROFILE_ZONE("Game::openMap");
    const Clock::time_point start = Clock::now();
    // Phase 18 (world): nothing here waits. The terrain's pack, tables and texture arrays are read on the map loader's
    // thread and the collision index on one of its own; the map before leaves over the next frames. The loading screen
    // keeps animating meanwhile (the character waits for the collision under it).
    if (Result<void> opened = m_terrain.openMap(m_packsDir, directory, m_renderer); !opened) {
        VD_LOG_ERROR("Map %s: %s", directory.c_str(), opened.error().message.c_str());
        return false;
    }
    m_grass.setMap(directory); // phase 14
    m_bodyFx.setMap(directory); // phase 14 (bodyfx): its ground table; the marks of the last map go
    m_collision.beginOpenMap(m_packsDir, directory);
    VD_LOG_INFO("Map %s: opening (game thread %.2f ms)", directory.c_str(), secondsBetween(start, Clock::now()) * 1000.0);
    return true;
}

void Game::updateTravelTest(f32 dt) {
    if (!m_travelTest.active()) {
        return;
    }
    const glm::dvec3 at = m_player.active() ? m_player.character().position() : m_camera.position;
    const bool settled = m_terrain.ready() && m_terrain.heightAt(at.x, at.y).has_value() &&
                         (!m_player.active() || !m_player.waiting());
    const MapTravelTest::Stop* stop = m_travelTest.update(dt, settled);
    if (!stop || !findWorldMap(stop->map)) {
        return;
    }
    VD_LOG_INFO("VD_WORLD_TRAVEL: to %s (%.1f, %.1f, %.1f)", stop->map.c_str(), stop->position.x, stop->position.y,
                stop->position.z);
    const bool walking = m_player.active() && !m_player.online();
    teleport(stop->map, stop->position.x, stop->position.y);
    m_snapToGround = false;
    m_camera.position = stop->position + glm::dvec3(0.0, 0.0, 3.0);
    if (walking) {
        m_player.spawnLocal(m_actors, stop->position, m_camera.yaw); // waits for the collision there: the loading screen
        m_freeCamera = false;
    }
}

void Game::teleport(const std::string& directory, f64 x, f64 y) {
    if (!openMap(directory)) {
        return;
    }
    m_camera.position = glm::dvec3(x, y, 100.0);
    m_snapToGround = true; // once the tile under the camera is loaded
    m_snapHeight = kCameraHeightAboveGround;
}

bool Game::startWalk(const WalkRoute& route) {
    if (!findWorldMap(route.map) || route.points.size() < 2) {
        return false;
    }
    const WalkPoint& a = route.points[0];
    const WalkPoint& b = route.points[1];
    teleport(route.map, a.x, a.y);
    m_snapToGround = false;
    m_camera.position = glm::dvec3(a.x, a.y, a.z + 3.0);
    const f32 facing = std::atan2(b.y - a.y, b.x - a.x);
    m_player.spawnLocal(m_actors, glm::dvec3(a.x, a.y, a.z), facing);
    m_freeCamera = false;
    m_walkTest.start(route);
    return true;
}

bool Game::startBenchmark(std::string_view id) {
    if (const WalkRoute* walk = findWalkRoute(id)) {
        return startWalk(*walk);
    }
    const BenchmarkRoute* route = findBenchmarkRoute(id);
    if (!route || !findWorldMap(route->map)) {
        return false;
    }
    teleport(route->map, route->waypoints[0].x, route->waypoints[0].y);
    m_camera.fovY = 1.0f;
    if (route->crowd > 0 && m_actors.isOpen()) {
        m_crowd.clear(m_actors);
        const f32 floor = route->waypoints[0].floor;
        m_crowd.spawnCharacters(m_actors, glm::dvec3(route->lookX, route->lookY, floor), route->crowd, 12345, 0.0f,
                                floor);
        // Phase 11 (measuring translucent units): VD_FADE_CROWD=<spell id> dresses the crowd in that aura's look.
        if (const char* spell = VD_DEV_ENV("VD_FADE_CROWD")) {
            if (const game::UnitFade* fade = m_unitFades.ofSpell(static_cast<u32>(std::strtoul(spell, nullptr, 10)))) {
                m_crowd.setFade(m_actors, glm::vec4(fade->tint, fade->alpha));
            }
        }
    }
    m_fight.clear();
    if (route->fight > 0 && m_actors.isOpen()) {
        // Phase 9: the group fight around the look-at point.
        m_crowd.clear(m_actors);
        m_fight.start(m_actors, m_effects, m_spellVisualData,
                      glm::dvec3(route->lookX, route->lookY, route->waypoints[0].floor), route->fight, 12345,
                      route->waypoints[0].floor);
    }
    if (route->effects > 0 && m_actors.isOpen()) {
        // Phase 9: looping spell effects and fire elementals on two rings around the look-at point.
        static constexpr const char* kSpellModels[] = {
            "Spells\\Fireball_Missile_High.m2",    "Spells\\FireNova_State.m2",
            "Spells\\Flamestrike_Area.m2",         "Spells\\Holy_Missile_Med.m2",
            "Spells\\RetributionAuraRed_Base.m2",  "Spells\\GooBolt_Missile_Low.m2",
            "Spells\\Fire_Cast_Hand.m2",           "Spells\\ArcaneSpirit_Impact_Base.m2",
            "Spells\\WisdomAura_Impact_Base.m2",   "Spells\\Rag_FireNova_Area.m2"};
        m_crowd.clear(m_actors);
        const f32 floor = route->waypoints[0].floor;
        for (u32 i = 0; i < route->effects; ++i) {
            const f64 angle = static_cast<f64>(i) * 6.2831853 / static_cast<f64>(route->effects);
            const f64 radius = (i % 2) ? 14.0 : 8.0;
            const glm::dvec3 at(route->lookX + std::cos(angle) * radius, route->lookY + std::sin(angle) * radius,
                                floor);
            if (i % 4 == 3) {
                m_crowd.spawnCreature(m_actors, at, static_cast<f32>(angle), 1204); // a fire elemental
            } else {
                m_effects.spawnAt(Effects::effectModelHash(kSpellModels[i % std::size(kSpellModels)]),
                                  at + glm::dvec3(0.0, 0.0, 1.5), static_cast<f32>(angle));
            }
        }
    }
    m_benchmark.start(*route);
    return true;
}

void Game::setView(const std::string& map, f64 x, f64 y, f32 yawDegrees, f32 pitchDegrees, f32 heightAboveGround,
                   std::optional<f64> z) {
    teleport(map, x, y);
    m_camera.yaw = yawDegrees / 57.29578f;
    m_camera.pitch = pitchDegrees / 57.29578f;
    m_snapHeight = heightAboveGround;
    if (z) {
        m_camera.position.z = *z;
        m_snapToGround = false;
    }
}

void Game::onPointer(f32 x, f32 y, bool down, bool touch) {
    m_renderer.debugUiPointer(x, y, down, touch);
    if (touch) {
        return; // fingers come through onTouch
    }
    // Desktop: the mouse's left button is a finger for the touch UI, unless it goes down on a debug window.
    constexpr i32 kMouseFinger = 1000;
    if (down && !m_mouseDown) {
        m_mouseDown = true;
        m_mouseOnHud = !m_renderer.debugUiHitTest(x, y);
        if (m_mouseOnHud) {
            uiTouch(TouchPhase::Down, kMouseFinger, x, y);
        }
    } else if (down && m_mouseOnHud) {
        uiTouch(TouchPhase::Move, kMouseFinger, x, y);
    } else if (!down && m_mouseDown) {
        if (m_mouseOnHud) {
            uiTouch(TouchPhase::Up, kMouseFinger, x, y);
        }
        m_mouseDown = false;
        m_mouseOnHud = false;
    }
}

bool Game::onEscape() {
    if (m_firstRunMode) { // phase 22 (pairing): the page before
        return m_firstRun.escape(m_textInput);
    }
    if (m_cinematics.active()) {
        m_cinematics.stop(); // CinematicFrame's OnKeyDown: Escape -> StopCinematic()
        return true;
    }
    // Phase 18 (extras): the glue screens' OnKeyDown, ToggleGameMenu in the world (the galleries keep closing).
    return !m_uiGallery.active() && m_screens.escapePressed(m_session);
}

bool Game::onEnter() {
    return !m_cinematics.active() && !m_uiGallery.active() && m_screens.enterPressed(m_session); // phase 18 (extras)
}

bool Game::onArrow(bool next) {
    return !m_cinematics.active() && !m_uiGallery.active() && m_screens.arrowPressed(m_session, next);
}

void Game::onKey(Key key, bool down) {
    if (m_cinematics.active()) {
        m_player.releaseAll(); // the interface and the controls are away while it plays
        return;
    }
    if (down && m_textInput.active()) {
        return; // typing: these keys are characters (onTextChar), not movement; releases still go through
    }
    if (key == Key::Mode) {
        if (down) {
            toggleCharacter();
        }
        return;
    }
    if (m_player.active() && !m_freeCamera) {
        m_player.onKey(key, down);
    } else {
        m_fly.onKey(key, down);
    }
}

void Game::onMouseLook(f32 dx, f32 dy) {
    if (m_player.active() && !m_freeCamera) {
        m_player.onLook(dx, dy);
    } else {
        m_fly.onLook(dx, dy);
    }
}

void Game::onMouseWheel(f32 steps) {
    if (m_renderer.debugUiWantsMouse()) {
        m_renderer.debugUiWheel(steps);
        return;
    }
    if (m_player.active() && !m_freeCamera) {
        m_player.onWheel(steps);
        return;
    }
    m_fly.speed = std::clamp(m_fly.speed * std::pow(1.25f, steps), 5.0f, 2000.0f);
}

void Game::toggleCharacter() {
    m_fly.releaseAll();
    m_player.releaseAll();
    if (m_player.active() && !m_freeCamera) {
        m_freeCamera = true; // online the character stays in the world; offline it is removed
        if (!m_player.online()) {
            m_player.leave(&m_actors);
        }
        return;
    }
    if (m_player.active()) {
        m_freeCamera = false;
        return;
    }
    // Offline: on the ground under the camera (or right below it inside a building).
    const glm::dvec3 at = m_camera.position;
    const std::optional<f64> ground = m_collision.groundHeight(at.x, at.y, at.z, at.z - 500.0);
    const f64 z = ground ? *ground : m_terrain.heightAt(at.x, at.y).value_or(static_cast<f32>(at.z));
    m_player.spawnLocal(m_actors, glm::dvec3(at.x, at.y, z), m_camera.yaw);
    m_freeCamera = false;
}

std::optional<f32> Game::terrainBelow(const void* game, f64 x, f64 y, f64 top) {
    const std::optional<f64> height = static_cast<const Game*>(game)->m_collision.terrainHeight(x, y, top, top - 1760.0);
    return height ? std::optional<f32>(static_cast<f32>(*height)) : std::nullopt;
}

std::optional<f32> Game::groundBelow(const void* game, f32 x, f32 y, f32 z) {
    const std::optional<f64> ground =
        static_cast<const Game*>(game)->m_collision.groundHeight(x, y, z + 1.0, z - 4.0);
    return ground ? std::optional<f32>(static_cast<f32>(*ground)) : std::nullopt;
}

bool Game::displayShape(u32 displayId, f32& radius, f32& height) const {
    const std::optional<pack::PackAsset> displays = m_dbPack.find("DBFilesClient\\CreatureDisplayInfo.dbc");
    const std::optional<pack::PackAsset> models = m_dbPack.find("DBFilesClient\\CreatureModelData.dbc");
    if (!displays || !models) {
        return false;
    }
    Result<db::DbcView> displayTable = db::DbcView::parse(displays->data, displays->size);
    Result<db::DbcView> modelTable = db::DbcView::parse(models->data, models->size);
    if (!displayTable || !modelTable || modelTable.value().fieldCount() <= kModelCollisionHeightField) {
        return false;
    }
    for (u32 r = 0; r < displayTable.value().recordCount(); ++r) {
        if (displayTable.value().getU32(r, 0) != displayId) {
            continue;
        }
        const u32 model = displayTable.value().getU32(r, kDisplayModelField);
        const f32 scale = displayTable.value().getF32(r, kDisplayScaleField);
        for (u32 m = 0; m < modelTable.value().recordCount(); ++m) {
            if (modelTable.value().getU32(m, 0) == model) {
                const f32 s = scale > 0.0f ? scale : 1.0f;
                radius = modelTable.value().getF32(m, kModelCollisionWidthField) * 0.5f * s;
                height = modelTable.value().getF32(m, kModelCollisionHeightField) * s;
                return radius > 0.0f && height > 0.0f;
            }
        }
    }
    return false;
}

void Game::onTouch(TouchPhase phase, i32 id, f32 x, f32 y) {
    // A finger that lands on a debug window drives the UI until lifted; the rest move the camera.
    if (phase == TouchPhase::Down && m_uiTouchId < 0 && m_renderer.debugUiHitTest(x, y)) {
        m_uiTouchId = id;
        m_renderer.debugUiPointer(x, y, true, true);
        return;
    }
    if (id == m_uiTouchId) {
        const bool down = phase == TouchPhase::Down || phase == TouchPhase::Move;
        m_renderer.debugUiPointer(x, y, down, true);
        if (!down) {
            m_uiTouchId = -1;
        }
        return;
    }
    if (m_gameHud.ready()) {
        uiTouch(phase, id, x, y); // the HUD's widgets, else its stick and camera
    } else {
        m_fly.onTouch(phase, id, x, y, static_cast<f32>(m_renderer.stats().width));
    }
}

void Game::uiTouch(TouchPhase phase, i32 id, f32 x, f32 y) {
    if (m_uiGallery.active()) {
        m_uiGallery.touchEvent(phase, id, x, y);
    } else {
        m_gameHud.touchEvent(phase, id, x, y);
    }
}

void Game::onFocusLost() {
    m_fly.releaseAll();
    m_player.releaseAll();
}

// --- Phase 7 platform input: text entry, screen insets and density, game controller ---

void Game::onTextChar(u32 codepoint) {
    m_textInput.typeChar(codepoint);
}

void Game::onTextKey(TextKey key) {
    switch (key) {
    case TextKey::Backspace: m_textInput.backspace(); break;
    case TextKey::Enter: m_textInput.submit(); break;
    case TextKey::Escape: m_textInput.cancel(); break;
    }
}

void Game::onTextInputState(std::string_view utf8) {
    m_textInput.setText(utf8);
}

void Game::onSafeArea(f32 left, f32 top, f32 right, f32 bottom) {
    const std::array<f32, 4> insets{std::max(0.0f, left), std::max(0.0f, top), std::max(0.0f, right),
                                    std::max(0.0f, bottom)};
    if (insets != m_safeInsets) {
        VD_LOG_INFO("Safe area insets: left %.0f, top %.0f, right %.0f, bottom %.0f", insets[0], insets[1], insets[2],
                    insets[3]);
        m_safeInsets = insets;
    }
}

void Game::onDisplayCutouts(const f32 otherInsets[4], std::span<const ui::Rect> cutouts) {
    std::array<f32, 4> insets{};
    for (u32 i = 0; i < 4; ++i) {
        insets[i] = std::max(0.0f, otherInsets[i]);
    }
    const u32 count = static_cast<u32>(std::min<usize>(cutouts.size(), m_cutouts.size()));
    bool changed = !m_cutoutsKnown || insets != m_otherInsets || count != m_cutoutCount;
    for (u32 i = 0; i < count && !changed; ++i) {
        const ui::Rect& a = m_cutouts[i];
        const ui::Rect& b = cutouts[i];
        changed = a.x != b.x || a.y != b.y || a.w != b.w || a.h != b.h;
    }
    if (!changed) {
        return;
    }
    m_otherInsets = insets;
    m_cutoutCount = count;
    for (u32 i = 0; i < count; ++i) {
        m_cutouts[i] = cutouts[i];
        VD_LOG_INFO("Display cutout %u: %.0f, %.0f, %.0f x %.0f", i, cutouts[i].x, cutouts[i].y, cutouts[i].w,
                    cutouts[i].h);
    }
    m_cutoutsKnown = true;
}

ScreenEdges Game::screenEdges() const {
    ScreenEdges edges;
    const RenderStats& render = m_renderer.stats();
    edges.screen = {0.0f, 0.0f, static_cast<f32>(render.width), static_cast<f32>(render.height)};
    edges.safe = safeArea();
    edges.known = m_cutoutsKnown;
    for (u32 i = 0; i < 4; ++i) {
        edges.insets[i] = m_otherInsets[i];
    }
    edges.cutouts = m_cutouts;
    edges.cutoutCount = m_cutoutCount;
    return edges;
}

ui::Rect Game::uiArea() const {
    ui::Rect area = safeArea();
    f32 keyboard = m_keyboardInset;
    if (m_fakeKeyboard > 0.0f && m_textInput.active()) {
        keyboard = std::max(keyboard, static_cast<f32>(m_renderer.stats().height) * m_fakeKeyboard);
    }
    const f32 covered = keyboard - (static_cast<f32>(m_renderer.stats().height) - area.bottom());
    if (covered > 0.0f) {
        area.h = std::max(0.0f, area.h - covered);
    }
    return area;
}

void Game::onKeyboardInset(f32 bottom) {
    m_keyboardInset = std::max(0.0f, bottom);
}

void Game::onContentScale(f32 dpScale) {
    if (dpScale > 0.0f && dpScale != m_contentScale) {
        VD_LOG_INFO("Content scale %.2f -> %.2f", m_contentScale, dpScale);
        m_contentScale = dpScale;
    }
}

ui::Rect Game::safeArea() const {
    const RenderStats& render = m_renderer.stats();
    const f32 width = static_cast<f32>(render.width);
    const f32 height = static_cast<f32>(render.height);
    return {m_safeInsets[0], m_safeInsets[1], std::max(0.0f, width - m_safeInsets[0] - m_safeInsets[2]),
            std::max(0.0f, height - m_safeInsets[1] - m_safeInsets[3])};
}

void Game::onGamepad(const GamepadState& state) {
    if (state.connected != m_gamepad.connected) {
        VD_LOG_INFO("Game controller %s", state.connected ? "connected" : "disconnected");
    }
    m_gamepad = state.connected ? state : GamepadState{};
}

void Game::shutdown() {
    m_firstRun.shutdown(); // phase 22 (pairing): the download thread, before the monitor writes its last files
    m_phoneBake.shutdown(); // phase 22 (phonebake): its thread (Android's service goes on)
    appUpdates().stop(); // phase 22 (updates): before the texts its thread reads go
    appUpdates().setPhoneBaker(nullptr); // phase 22 (phonebake): it called this Game
    saveSettings();
    m_monitor.stop(); // writes what saveSettings() left pending
    m_audio.shutdown();
    m_wmoAreas.shutdown(); // phase 14
    m_cinematics.close();
    text::unload();
    game::unloadCharacterNames();
    m_player.leave(&m_actors);
    m_collision.closeMap();
    game::setTransportResolver(nullptr, nullptr); // phase 18 (motion)
    m_transports.shutdown(m_objects, m_collision);
    m_serverObjects.shutdown(m_objects); // phase 13: doors' collision, its IO thread
    m_terrain.shutdown(m_renderer); // unloads tiles: their objects go with them
    m_spellEffects.clear();
    m_fight.clear();
    m_effects.shutdown();
    m_grass.close(m_renderer); // phase 14
    m_objects.close(m_renderer);
    m_crowd.clear(m_actors);
    m_questMarks.clear(m_actors);
    m_units.clear(m_actors);
    m_modelViews.reset(m_actors); // phase 15
    m_actors.close(m_renderer);
    m_textureBrowser.release(m_renderer);
    m_uiResources.shutdown(m_renderer);
    m_renderer.shutdown();
    m_jobs.shutdown();
    VD_LOG_INFO("VanillaDroid engine shutdown");
}

void Game::onSurfaceCreated(const NativeSurface& surface) {
    m_surface = surface;
    VD_LOG_INFO("Surface created: %ux%u", surface.width, surface.height);
    if (Result<void> result = m_renderer.attachSurface(surface); !result) {
        VD_LOG_ERROR("Could not attach surface: %s", result.error().message.c_str());
    }

    // Coming back from background must not count as one huge frame.
    m_lastFrame = Clock::now();
    m_lastTickStart = m_lastFrame;
    m_statsWindowStart = m_lastFrame;
    m_statsFrames = 0;
    m_statsWorstFrame = 0.0;
    m_statsGpuMs = 0.0;
}

void Game::onSurfaceResized(u32 width, u32 height) {
    if (!hasSurface()) {
        return;
    }
    if (width != m_surface.width || height != m_surface.height) {
        VD_LOG_INFO("Surface resized: %ux%u", width, height);
    }
    m_surface.width = width;
    m_surface.height = height;
    // Always forwarded: a 180-degree rotation keeps the size but changes the transform.
    m_renderer.onSurfaceChanged(width, height);
}

void Game::onSurfaceDestroyed() {
    if (!hasSurface()) {
        return;
    }
    m_renderer.detachSurface();
    m_surface = {};
    m_fly.releaseAll();
    m_uiTouchId = -1;
    VD_LOG_INFO("Surface destroyed");
}

void Game::onLowMemory() {
    // A low-memory warning without its level (the platform layer gives onTrimMemory's when it has it).
    onTrimMemory(10); // TRIM_MEMORY_RUNNING_LOW
}

void Game::onTrimMemory(i32 level) {
    // ComponentCallbacks2.TRIM_MEMORY_*: 5 / 10 / 15 the foreground under pressure (running moderate / low / critical,
    // delivered until Android 13), 20 the interface went out of sight, 40 / 60 / 80 the process is in the background
    // list, closer and closer to being killed.
    const u64 residentBefore = DeviceMonitor::residentBytesNow();
    u32 effectModels = 0;
    if (level >= 15) {
        effectModels = m_actors.trimIdleModels(); // spell effects nothing shows: streamed again on the next cast
    }
    if (level >= 10 && level < 20) {
        // Playing under pressure: the world stops growing (fewer terrain tiles, so fewer buildings and doodads).
        const u64 budget = level >= 15 ? Terrain::kDefaultGpuBudget / 2 : Terrain::kDefaultGpuBudget * 3 / 4;
        m_terrain.setGpuBudget(std::min(budget, m_terrain.gpuBudget()));
        m_trimmedBudget = m_terrain.gpuBudget();
    }
    u64 mapped = 0;
    if (level >= kTrimBackground) {
        // In the background list: the packs' pages go now (clean: read from the file again when touched). The GPU
        // memory stays on purpose: most of it is the streaming reserve (8 blocks of 48 MB that never return to the
        // driver, since a new block costs 10-17 ms on the Fold5 and up to 62 ms on the Adreno 650), so unloading the
        // world would free nothing and cost its reload on the way back.
        if (!hasSurface()) {
            m_renderer.releaseRetired();
        }
        mapped = MappedFile::releaseResidentPages();
    }
    VD_LOG_WARN("onTrimMemory(%d): %u effect models freed%s, terrain budget %llu MB | resident %.0f -> %.0f MB "
                "(packs: %.0f MB mapped) | GPU %.0f MB",
                level, effectModels, mapped > 0 ? ", the packs' pages released" : "",
                static_cast<unsigned long long>(m_terrain.gpuBudget() >> 20),
                static_cast<f64>(residentBefore) / 1048576.0,
                static_cast<f64>(DeviceMonitor::residentBytesNow()) / 1048576.0, static_cast<f64>(mapped) / 1048576.0,
                static_cast<f64>(m_renderer.gpuMemoryBytes()) / 1048576.0);
}

void Game::onPause() {
    m_audio.setPaused(true);
    m_player.releaseAll(); // phase 13: nothing held through the pause (its time is reported as skipped on resume)
    saveSettings(); // the process may not come back
}

void Game::onResume() {
    m_audio.setPaused(false);
    if (m_trimmedBudget != 0 || m_terrain.gpuBudget() < Terrain::kDefaultGpuBudget) {
        m_terrain.setGpuBudget(Terrain::kDefaultGpuBudget);
        m_trimmedBudget = 0;
        VD_LOG_INFO("Back in the foreground: terrain budget -> %llu MB",
                    static_cast<unsigned long long>(Terrain::kDefaultGpuBudget >> 20));
    }
}

void Game::saveSettings() {
    const SettingsView& settings = m_screens.settings();
    if (!m_settingsPersist || settings == m_savedSettings) {
        return;
    }
    m_savedSettings = settings;
    m_settingsCalm = 0.0f;
    m_monitor.writeFile(m_settingsPath, formatSettings(settings));
    VD_LOG_INFO("Settings saved");
}

void Game::updateQuality(f32 dt) {
    VD_PROFILE_ZONE("Game::updateQuality");
    m_monitor.latest(m_device);
    SettingsView& settings = m_screens.settings();
    // Saved once they stand still a moment (a slider being dragged changes them every frame).
    if (!(settings == m_pendingSettings)) {
        m_pendingSettings = settings;
        m_settingsCalm = 0.0f;
    } else if (m_settingsPersist && !(settings == m_savedSettings)) {
        m_settingsCalm += dt;
        if (m_settingsCalm >= kSettingsSaveSeconds) {
            saveSettings();
        }
    }
    // The governor's ceiling is the player's resolution; a fixed one (development) turns it off.
    QualityGovernor::Config config;
    config.dynamic = settings.dynamicQuality && m_fixedRenderScale <= 0.0f;
    config.baseScale = m_fixedRenderScale > 0.0f ? m_fixedRenderScale : settings.renderScale;
    m_governor.configure(config);
    const GovernorOutput& q = m_governor.output();
    m_renderer.setRenderScale(q.renderScale);
    m_viewDistance = settings.viewDistance * q.distanceScale;
    m_objects.doodadBaseDistance = kDoodadBaseDistance * settings.environmentDetail * q.distanceScale;
    m_objects.doodadDistancePerRadius = kDoodadDistancePerRadius * settings.environmentDetail * q.distanceScale;
    m_objects.doodadDistanceScale = settings.environmentDetail * q.distanceScale; // phase 14: 1.12's ranges
    fx::ParticleSystem::Settings& particles = m_effects.particles().settings();
    particles.density = settings.spellDetail * q.effectScale;
    particles.maxDistance = m_effectDistance * q.distanceScale;
    // Phase 14 (bodyfx): the shadows reach as far as the doodads' detail, the marks follow the spell detail.
    m_bodyFx.setQuality(settings.environmentDetail * q.distanceScale, settings.spellDetail * q.effectScale);
    m_objects.effectDistance = particles.maxDistance;
    // Phase 14: the ground clutter (the client's frillDensity) and its fade, nearer when the device runs hot.
    m_grass.setFrillDensity(GroundEffects::frillDensityForLevel(settings.groundClutter));
    m_grass.distanceScale = q.distanceScale;
    updateGpuOptions(settings, dt); // phase 18 (world)
    const u32 interval = settings.limitFps || q.frameInterval > 1 ? 2u : 1u;
    if (interval != m_frameInterval) {
        m_frameInterval = interval;
        m_renderer.setFrameInterval(interval);
        m_performanceHint.updateTarget(kFrameTargetNanos * interval);
        m_performanceHint.setPreferPowerEfficiency(settings.limitFps);
    }
}

// Phase 18 (world) ------------------------------------------------------------------------------------------------

void Game::initGpuOptions(SettingsView& settings) {
    settings.maxAnisotropy = std::max(1u, m_renderer.maxAnisotropy());
    settings.maxMultisample = std::max(1u, m_renderer.maxMultisample());
    // Development: VD_WORLD_ANISO=<1..16> and VD_WORLD_MSAA=<1|2|4|8> over the saved options (benchmarks: -EnvSwitches).
    if (const char* aniso = VD_DEV_ENV("VD_WORLD_ANISO"); aniso && aniso[0] != '\0') {
        settings.anisotropy = static_cast<u32>(std::max(1, std::atoi(aniso)));
        settings.graphicsQuality = currentQualityPreset(settings);
    }
    if (const char* msaa = VD_DEV_ENV("VD_WORLD_MSAA"); msaa && msaa[0] != '\0') {
        settings.multisample = static_cast<u32>(std::max(1, std::atoi(msaa)));
        settings.graphicsQuality = currentQualityPreset(settings);
    }
    if (const char* coverage = VD_DEV_ENV("VD_WORLD_A2C"); coverage && coverage[0] == '0') {
        m_renderer.setAlphaToCoverage(false); // multisampling with the plain alpha test, to compare
    }
    settings.anisotropy = std::min(settings.anisotropy, settings.maxAnisotropy);
    settings.multisample = std::min(settings.multisample, settings.maxMultisample);
    m_gpuAnisotropy = m_gpuPendingAnisotropy = settings.anisotropy;
    m_gpuMultisample = m_gpuPendingMultisample = settings.multisample;
    if (const char* cycle = VD_DEV_ENV("VD_WORLD_GPU_CYCLE")) {
        m_gpuCycleSeconds = static_cast<f32>(std::atof(cycle));
    }
    m_renderer.setAnisotropy(m_gpuAnisotropy); // applied at the start of the first frame, before anything is drawn
    m_renderer.setMultisample(m_gpuMultisample);
    VD_LOG_INFO("Video: anisotropic filtering x%u (GPU up to x%u), multisampling x%u (up to x%u)", m_gpuAnisotropy,
                settings.maxAnisotropy, m_gpuMultisample, settings.maxMultisample);
}

void Game::updateGpuOptions(SettingsView& settings, f32 dt) {
    // Development (VD_WORLD_GPU_CYCLE=<seconds>): the options go round 1 / 16x + 4 samples / 4x + 2 samples, to see
    // them rebuilt during play (validation layer, Tracy).
    if (m_gpuCycleSeconds > 0.0f) {
        m_gpuCycleClock += dt;
        if (m_gpuCycleClock >= m_gpuCycleSeconds) {
            constexpr u32 kAnisotropy[] = {1, 16, 4};
            constexpr u32 kSamples[] = {1, 4, 2};
            m_gpuCycleClock = 0.0f;
            m_gpuCycleStep = (m_gpuCycleStep + 1) % 3;
            settings.anisotropy = std::min(kAnisotropy[m_gpuCycleStep], settings.maxAnisotropy);
            settings.multisample = std::min(kSamples[m_gpuCycleStep], settings.maxMultisample);
        }
    }
    // New samplers, or new passes and pipelines (from the pipeline cache): the GPU idles for it, so it waits until the
    // slider has stood still half a second, as OptionsFrame's changes wait for "Okay".
    constexpr f32 kCalmSeconds = 0.5f;
    if (settings.anisotropy != m_gpuPendingAnisotropy || settings.multisample != m_gpuPendingMultisample) {
        m_gpuPendingAnisotropy = settings.anisotropy; // still moving
        m_gpuPendingMultisample = settings.multisample;
        m_gpuOptionsCalm = 0.0f;
        return;
    }
    if (settings.anisotropy == m_gpuAnisotropy && settings.multisample == m_gpuMultisample) {
        return;
    }
    m_gpuOptionsCalm += dt;
    if (m_gpuOptionsCalm < kCalmSeconds) {
        return;
    }
    m_gpuAnisotropy = settings.anisotropy;
    m_gpuMultisample = settings.multisample;
    m_renderer.setAnisotropy(m_gpuAnisotropy);
    m_renderer.setMultisample(m_gpuMultisample);
}

// ---------------------------------------------------------------------------------------------------------------------

void Game::logSoakMinute(const BenchmarkMinute& minute) {
    const GovernorOutput& q = m_governor.output();
    const RenderStats& render = m_renderer.stats();
    const DeviceReading& d = m_device;
    const f64 watts = d.voltageMv > 0 && d.currentMicroA != 0
                          ? std::fabs(static_cast<f64>(d.currentMicroA)) * 1e-6 * static_cast<f64>(d.voltageMv) * 1e-3
                          : 0.0;
    if (watts > 0.0 && !d.pluggedIn) {
        m_soak.wattSum += watts;
        ++m_soak.wattSamples;
    }
    char line[512];
    std::snprintf(line, sizeof(line),
                "SOAK minute %u: %.1f fps | p50 %.2f p99 %.2f max %.2f ms | >20 ms %u, >33 ms %u | GPU %.2f CPU %.2f "
                "ms | scale %.2f distance %.2f effects %.2f T%u L%u | headroom %.2f status %d | battery %d%% %+.0f mA "
                "%.2f W %.1f C%s | GPU memory %.0f MB",
                minute.index, static_cast<f64>(minute.fps), static_cast<f64>(minute.p50Ms),
                static_cast<f64>(minute.p99Ms), static_cast<f64>(minute.maxMs), minute.over20, minute.over33,
                static_cast<f64>(minute.gpuMs), static_cast<f64>(minute.cpuMs), static_cast<f64>(render.renderScale),
                static_cast<f64>(q.distanceScale), static_cast<f64>(q.effectScale), q.thermalLevel, q.loadLevel,
                static_cast<f64>(d.thermalHeadroom), d.thermalStatus, d.batteryPercent,
                static_cast<f64>(d.currentMicroA) / 1000.0, watts, static_cast<f64>(d.batteryCelsius),
                d.pluggedIn ? " (charging)" : "", static_cast<f64>(m_renderer.gpuMemoryBytes()) / 1048576.0);
    soakLine(line);
}

void Game::soakLine(const char* line) {
    // Once a minute: the log, and the whole report so far to a file the script fetches at the end (no adb traffic
    // during the run, which would cost battery).
    VD_LOG_INFO("%s", line);
    m_soak.report += line;
    m_soak.report += '\n';
    if (!m_packsDir.empty()) {
        m_monitor.writeFile(m_packsDir / "bench_soak.txt", m_soak.report);
    }
}

void Game::logSoakSummary() {
    if (!m_soak.started) {
        return;
    }
    const f64 minutes = secondsBetween(m_soak.startTime, Clock::now()) / 60.0;
    const f64 hours = std::max(1e-6, minutes / 60.0);
    const DeviceReading& a = m_soak.start;
    const DeviceReading& b = m_device;
    char battery[192] = "battery: unknown";
    if (a.pluggedIn || b.pluggedIn) {
        std::snprintf(battery, sizeof(battery), "battery: on a charger, not measured");
    } else if (a.chargeMicroAh > 0 && b.chargeMicroAh > 0) {
        const f64 used = static_cast<f64>(a.chargeMicroAh - b.chargeMicroAh) / 1000.0; // mAh
        std::snprintf(battery, sizeof(battery),
                      "battery %d%% -> %d%% (%.1f %%/h), %.0f mAh used = %.0f mAh/h, %.2f W average", a.batteryPercent,
                      b.batteryPercent, static_cast<f64>(a.batteryPercent - b.batteryPercent) / hours, used,
                      used / hours, m_soak.wattSamples > 0 ? m_soak.wattSum / m_soak.wattSamples : 0.0);
    }
    char line[512];
    std::snprintf(line, sizeof(line),
                "SOAK done: %.1f min | scale min %.2f avg %.2f | thermal level max %u, headroom max %.2f, battery "
                "%.1f -> %.1f C | %s",
                minutes, static_cast<f64>(m_soak.minScale),
                m_soak.frames > 0 ? m_soak.scaleSum / static_cast<f64>(m_soak.frames) : 1.0, m_soak.maxThermalLevel,
                static_cast<f64>(m_soak.maxHeadroom), static_cast<f64>(a.batteryCelsius),
                static_cast<f64>(b.batteryCelsius), battery);
    soakLine(line);
    if (!m_benchmark.report().empty()) {
        soakLine(("BENCH " + m_benchmark.report()).c_str());
    }
    m_soak.started = false;
}

void Game::logDevice() {
    const GovernorOutput& q = m_governor.output();
    const RenderStats& render = m_renderer.stats();
    const DeviceReading& d = m_device;
    const f64 watts = d.voltageMv > 0 && d.currentMicroA != 0
                          ? std::fabs(static_cast<f64>(d.currentMicroA)) * 1e-6 * static_cast<f64>(d.voltageMv) * 1e-3
                          : 0.0;
    VD_LOG_INFO("quality: scale %.2f (%ux%u), view %.0f yd (drawn %.0f), distance x%.2f, effects x%.2f, thermal level %u, "
                "load level %u, %u fps "
                "| thermal headroom %.2f status %d | battery %d%% %+.0f mA %.2f W %.1f C%s | memory: GPU %.0f MB, "
                "RSS %.0f MB, available %.0f MB",
                static_cast<f64>(render.renderScale), render.sceneWidth, render.sceneHeight,
                static_cast<f64>(m_viewDistance), static_cast<f64>(m_drawDistance), static_cast<f64>(q.distanceScale),
                static_cast<f64>(q.effectScale), q.thermalLevel, q.loadLevel, 60u / std::max(1u, m_frameInterval), static_cast<f64>(d.thermalHeadroom), d.thermalStatus,
                d.batteryPercent, static_cast<f64>(d.currentMicroA) / 1000.0, watts,
                static_cast<f64>(d.batteryCelsius), d.pluggedIn ? " (charging)" : "",
                static_cast<f64>(m_renderer.gpuMemoryBytes()) / 1048576.0,
                static_cast<f64>(d.residentBytes) / 1048576.0, static_cast<f64>(d.availableBytes) / 1048576.0);
}

void Game::onAudioFocus(f32 gain) {
    m_audio.setFocusGain(gain);
}

void Game::updateAudio(u32 nowMs, f32 dt) {
    if (!m_audio.active()) {
        return;
    }
    const SettingsView& settings = m_screens.settings();
    // Phase 15 (options): "Enable All Sound" (MasterSoundEffects) silences every sound but the music; "Enable Music",
    // "Enable Ambience", error speech, emote sounds and "Loop Music" are GameAudio's own switches.
    const f32 effects = settings.soundEffects ? 1.0f : 0.0f;
    m_audio.setMasterVolume(settings.masterVolume);
    m_audio.setCategoryVolume(audio::Category::Music, settings.musicEnabled ? settings.musicVolume : 0.0f);
    m_audio.setCategoryVolume(audio::Category::Ambience, settings.ambienceSounds ? settings.ambienceVolume * effects : 0.0f);
    m_audio.setCategoryVolume(audio::Category::Effects, settings.effectsVolume * effects);
    m_audio.setCategoryVolume(audio::Category::Voices, settings.voicesVolume * effects);
    m_audio.setCategoryVolume(audio::Category::Interface, settings.interfaceVolume * effects);
    m_gameAudio.setOptions({settings.musicEnabled, settings.ambienceSounds && settings.soundEffects,
                            settings.errorSpeech && settings.soundEffects, settings.emoteSounds, settings.musicLooping});
    // Heard from the camera; the zone, the footsteps and the ground from the character when there is one.
    GameAudioFrame frame;
    frame.nowMs = nowMs;
    frame.dt = dt;
    frame.listener = m_camera.position;
    frame.listenerRight = m_camera.right();
    frame.login = !m_uiGallery.active() && !m_hudDemo && m_screens.wantsLogin(m_session);
    frame.world = m_terrain.isOpen() && !frame.login;
    frame.loading = m_loadingShown;
    const glm::dvec3 focus = m_player.active() ? m_player.character().position() : m_camera.position;
    frame.area = m_terrain.isOpen() ? m_terrain.areaAt(focus.x, focus.y) : 0;
    frame.map = m_terrain.isOpen() ? std::string_view(m_terrain.mapDirectory()) : std::string_view{};
    frame.hours = m_timeOfDay / 120.0f;
    if (m_session.inWorld()) {
        frame.play = &m_session.play();
        frame.entities = &m_session.entities();
        frame.player = m_session.playerGuid();
    }
    frame.reactions = &m_reactions;
    frame.collision = m_collision.isOpen() ? &m_collision : nullptr; // parity pass: water sounds
    frame.weatherSound = m_weather.sound(); // phase 11
    frame.weatherVolume = m_weather.soundVolume();
    // Phase 14 (sound): heard from the character's head, facing where it faces (WoW.exe 0x483118), the camera
    // without one; the models' sounds; the room of a building the character stands in.
    frame.camera = m_camera.position;
    // Phase 15 (options): "Enable Sound at Character" (SoundListenerAtCharacter, on by default) or the camera.
    if (m_player.active() && !m_listenerAtCamera && settings.soundAtCharacter) {
        const physics::CharacterController& character = m_player.character();
        frame.listener = character.position() + glm::dvec3(0.0, 0.0, character.height);
        frame.listenerRight = glm::vec3(std::sin(character.facing()), -std::cos(character.facing()), 0.0f);
    }
    m_audioScene.terrain = m_terrain.isOpen() ? &m_terrain : nullptr;
    m_audioScene.actors = m_actors.isOpen() ? &m_actors : nullptr;
    m_audioScene.units = &m_units;
    frame.scene = &m_audioScene;
    frame.playerActor = m_session.inWorld() ? m_units.actorOf(m_session.playerGuid()) : m_player.localActor();
    if (m_terrain.isOpen()) {
        const WmoAreaHit& room = roomAt(focus);
        if (room.valid() && m_interiorAudio) {
            frame.interiorKey = room.key();
            frame.interiorMusic = room.zoneMusic;
            frame.interiorIntro = room.intro;
            frame.interiorAmbience = room.ambience;
            if (room.areaId != 0 && frame.area != 0) {
                frame.area = room.areaId;
            }
        }
    }
    if (m_player.active()) {
        const physics::CharacterController& character = m_player.character();
        frame.character = true;
        frame.feet = character.position();
        frame.speed = character.speed();
        frame.onGround = character.mode() == physics::MotionMode::Ground;
        frame.swimming = character.mode() == physics::MotionMode::Swim;
        const f32 liquid = character.liquidLevel();
        frame.wading = !frame.swimming && !std::isnan(liquid) && liquid > static_cast<f32>(frame.feet.z) + 0.05f;
        frame.landed = character.events().landed;
        frame.jumped = character.events().jumped; // parity pass: jumps, landings and splashes
        frame.startedSwim = character.events().startedSwim;
        frame.airMs = character.endedFallTimeMs();
        frame.display = m_playerDisplay;
        const std::optional<f32> ground = m_terrain.heightAt(frame.feet.x, frame.feet.y);
        frame.onObject = ground && frame.feet.z > static_cast<f64>(*ground) + 0.5;
    }
    m_gameAudio.update(frame);
    m_audio.update();
}

void Game::drawWorldPanel() {
    const f32 font = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(font, font * 11.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("World")) {
        ImGui::End();
        return;
    }
    if (m_worldMaps.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "No world packs (vd_baker bake --only world)");
    }
    const std::string current = m_terrain.isOpen() ? m_terrain.mapDirectory() : std::string("-");
    if (ImGui::BeginCombo("Map", current.c_str())) {
        for (const WorldMap& map : m_worldMaps) {
            if (ImGui::Selectable(map.directory.c_str(), map.directory == current)) {
                openMap(map.directory);
            }
        }
        ImGui::EndCombo();
    }
    f32 hours = m_timeOfDay / 120.0f;
    char clock[16];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", static_cast<int>(hours) % 24,
                  static_cast<int>((hours - std::floor(hours)) * 60.0f));
    if (ImGui::SliderFloat("Time", &hours, 0.0f, 24.0f, clock)) {
        m_timeOfDay = hours * 120.0f;
    }
    bool running = m_timeSpeed > 0.0f;
    if (ImGui::Checkbox("Fast time (1 h every 10 s)", &running)) {
        m_timeSpeed = running ? 12.0f : 0.0f;
    }
    u32 tx = 0;
    u32 ty = 0;
    Terrain::tileAt(m_camera.position.x, m_camera.position.y, tx, ty);
    ImGui::Text("X %.1f  Y %.1f  Z %.1f   tile %u_%u", m_camera.position.x, m_camera.position.y,
                m_camera.position.z, tx, ty);
    if (const std::optional<f32> ground = m_terrain.heightAt(m_camera.position.x, m_camera.position.y)) {
        ImGui::Text("ground %.1f  (height %.1f)  area %u", *ground, m_camera.position.z - *ground,
                    m_terrain.areaAt(m_camera.position.x, m_camera.position.y));
    }
    ImGui::SliderFloat("Speed", &m_fly.speed, 5.0f, 2000.0f, "%.0f yd/s", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Distance", &m_screens.settings().viewDistance, 200.0f, 1600.0f, "%.0f yd");
    ImGui::SliderFloat("Horizon", &m_horizonDistance, 0.0f, 5000.0f, "%.0f yd");
    std::array<f32, pack::kLodLevels - 1>& lod = m_terrain.lodDistances();
    ImGui::SliderFloat3("LOD (yd)", lod.data(), 0.0f, 1600.0f, "%.0f");
    f32 fovDegrees = m_camera.fovY * 57.29578f;
    if (ImGui::SliderFloat("FOV", &fovDegrees, 40.0f, 100.0f, "%.0f")) {
        m_camera.fovY = fovDegrees / 57.29578f;
    }

    const TerrainStats& terrain = m_terrain.stats();
    ImGui::Text("tiles: %u loaded, %u visible, %u in IO, %u waiting for the GPU | horizon %zu", terrain.loadedTiles,
                terrain.visibleTiles, terrain.inFlight, terrain.waitingUpload, m_horizonDraws.size());
    const ObjectStats& objects = m_objects.stats();
    ImGui::Text("objects: %u instances, %u visible in %u models | %u models, %u textures, %u pending",
                objects.instances, objects.visibleInstances, objects.modelsDrawn, objects.modelsLoaded,
                objects.texturesLoaded, objects.pendingAssets);
    ImGui::Text("WMO groups: %u of %u (%u interiors culled by portals)", objects.visibleGroups,
                objects.totalGroups, objects.portalCulledGroups);
    const GrassStats& grass = m_grass.stats(); // phase 14
    ImGui::Text("grass: frillDensity %u, %u of %u chunks, %u doodads in %u draws | +%u placed, %.2f ms | %u tiles, "
                "%u reading",
                grass.frillDensity, grass.visibleChunks, grass.cachedChunks, grass.instances, grass.draws,
                grass.generated, static_cast<f64>(grass.collectMs), grass.tiles, grass.pending);
    ImGui::Text("models GPU %.0f MB | upload %.2f ms (worst %.2f)",
                static_cast<f64>(m_renderer.modelGpuBytes()) / (1024.0 * 1024.0), objects.lastUploadMs,
                objects.worstUploadMs);
    ImGui::Text("terrain GPU %.0f MB | IO %.1f ms | upload %.2f ms (worst %.2f)",
                static_cast<f64>(m_renderer.terrainGpuBytes()) / (1024.0 * 1024.0), terrain.lastIoMs,
                terrain.lastUploadMs, terrain.worstUploadMs);
    if (ImGui::CollapsingHeader("Effects (phase 9)")) {
        const fx::ParticleStats& fx = m_effects.particles().stats();
        ImGui::Text("%u instances (%u doodads) | %u emitters + %u ribbons run, %u culled", fx.instances,
                    m_effects.doodadInstances(), fx.emitters, fx.ribbons, fx.culled);
        ImGui::Text("%u particles, %u quads, %u draws | %.2f ms, rate x%.2f | %u starved, %u clipped", fx.particles,
                    fx.quads, fx.draws, fx.updateMs, fx.rateScale, fx.starved, fx.clipped);
        fx::ParticleSystem::Settings& settings = m_effects.particles().settings();
        ImGui::SliderFloat("Effect distance", &m_effectDistance, 20.0f, 250.0f, "%.0f yd");
        ImGui::SliderFloat("Full rate up to", &settings.fullRateDistance, 5.0f, 100.0f, "%.0f yd");
        ImGui::SliderFloat("Lowest rate", &settings.minRateScale, 0.05f, 1.0f, "%.2f");
        ImGui::SliderFloat("CPU budget", &settings.budgetMs, 0.2f, 4.0f, "%.1f ms");
        ImGui::Checkbox("Workers", &settings.workers);
        ImGui::SameLine();
        ImGui::Checkbox("Enabled", &m_effects.enabled);
        // Spell visuals and combat animations.
        const SpellVisuals::Stats& spells = m_spellEffects.visuals().stats();
        const Units::CombatStats& combat = m_units.combatStats();
        ImGui::Text("spells: %u effects | %u casts/channels, %u missiles, %u/%u states, %u areas | %u kits, %u skipped",
                    spells.effects, spells.holds, spells.missiles, spells.shownAuras, spells.auras, spells.areas,
                    spells.kits, spells.skipped);
        ImGui::Text("combat: %u swings, %u reactions, %u emotes | %.3f ms", combat.swings, combat.reactions,
                    combat.emotes, static_cast<f64>(spells.updateMs + combat.readMs));
        if (m_fight.active()) {
            const SpellVisuals::Stats& fight = m_fight.visuals().stats();
            ImGui::Text("fight: %u fighters, %u swings, %u casts, %u deaths | %u casts/channels, %u missiles, "
                        "%u/%u states, %u areas | %.3f + %.3f ms",
                        m_fight.size(), m_fight.stats().swings, m_fight.stats().casts, m_fight.stats().deaths,
                        fight.holds, fight.missiles, fight.shownAuras, fight.auras, fight.areas,
                        static_cast<f64>(m_fight.stats().updateMs), static_cast<f64>(fight.updateMs));
        }
    }

    if (ImGui::CollapsingHeader("Benchmarks")) {
        if (m_benchmark.running()) {
            ImGui::Text("%s: %s %.0f%%", m_benchmark.route()->name, m_benchmark.warmingUp() ? "warming up" : "flying",
                        m_benchmark.progress() * 100.0f);
            if (ImGui::Button("Stop")) {
                m_benchmark.stop();
            }
        } else {
            for (const BenchmarkRoute& route : benchmarkRoutes()) {
                if (findWorldMap(route.map) && ImGui::Button(route.name)) {
                    startBenchmark(route.id);
                }
            }
            ImGui::TextUnformatted("On foot (phase 6):");
            for (const WalkRoute& route : walkRoutes()) {
                if (findWorldMap(route.map) && ImGui::Button(route.name)) {
                    startWalk(route);
                }
            }
            if (m_walkTest.running()) {
                ImGui::Text("%s: %.0f%%", m_walkTest.route()->name, m_walkTest.progress() * 100.0f);
            }
            if (!m_walkTest.report().empty()) {
                ImGui::TextWrapped("%s", m_walkTest.report().c_str());
            }
        }
        if (!m_benchmark.report().empty()) {
            ImGui::TextWrapped("%s", m_benchmark.report().c_str());
        }
    }
    if (ImGui::CollapsingHeader("Go to")) {
        for (const Destination& destination : kDestinations) {
            if (!findWorldMap(destination.map)) {
                continue;
            }
            if (ImGui::Button(destination.name)) {
                teleport(destination.map, destination.x, destination.y);
            }
        }
    }
    ImGui::End();
}

glm::dvec3 Game::groundInFront(f64 distance) const {
    const glm::vec3 forward = m_camera.forward();
    glm::dvec3 point = m_camera.position + glm::dvec3(forward.x, forward.y, 0.0) * distance;
    point.z = m_terrain.heightAt(point.x, point.y).value_or(static_cast<f32>(m_camera.position.z - 2.0));
    return point;
}

std::optional<f32> Game::floorBetween(const void* game, f32 x, f32 y, f32 top, f32 bottom) {
    const Game& self = *static_cast<const Game*>(game);
    if (self.m_collision.isOpen()) {
        if (const std::optional<f64> floor = self.m_collision.groundHeight(x, y, top, bottom)) {
            return static_cast<f32>(*floor);
        }
    }
    // No collision loaded there (or nothing in it): the terrain, when it lies in the window.
    const std::optional<f32> terrain = self.m_terrain.heightAt(x, y);
    return terrain && *terrain <= top && *terrain >= bottom ? terrain : std::nullopt;
}

std::optional<glm::dvec3> Game::groundAlongRay(const void* game, const glm::dvec3& from, const glm::vec3& direction,
                                               f32 maxDistance) {
    VD_PROFILE_ZONE("Game::groundAlongRay");
    const Game& self = *static_cast<const Game*>(game);
    // In short pieces: each gathers few triangles. The collision has the buildings and the terrain near the player
    // (not the trees: a place is on the ground under them); the terrain's heights go farther.
    constexpr f32 kPiece = 6.0f;
    const glm::dvec3 dir(direction);
    for (f32 t0 = 0.0f; t0 < maxDistance; t0 += kPiece) {
        const f32 t1 = std::min(maxDistance, t0 + kPiece);
        const glm::dvec3 a = from + dir * static_cast<f64>(t0);
        const glm::dvec3 b = from + dir * static_cast<f64>(t1);
        if (self.m_collision.isOpen()) {
            self.m_pickSet.reset(a);
            self.m_collision.gather(glm::min(a, b) - glm::dvec3(0.5), glm::max(a, b) + glm::dvec3(0.5), self.m_pickSet);
            physics::Hit hit;
            if (physics::raycast(self.m_pickSet, glm::vec3(0.0f), glm::vec3(b - a), hit, pack::kCollisionDoodad)) {
                return a + (b - a) * static_cast<f64>(hit.t);
            }
        }
        const std::optional<f32> ha = self.m_terrain.heightAt(a.x, a.y);
        const std::optional<f32> hb = self.m_terrain.heightAt(b.x, b.y);
        if (ha && hb && a.z >= *ha && b.z < *hb) {
            // Under the terrain within this piece: where it crosses, by halving.
            f64 lo = 0.0, hi = 1.0;
            for (u32 i = 0; i < 10; ++i) {
                const f64 mid = (lo + hi) * 0.5;
                const glm::dvec3 p = a + (b - a) * mid;
                const std::optional<f32> h = self.m_terrain.heightAt(p.x, p.y);
                (h && p.z < *h ? hi : lo) = mid;
            }
            glm::dvec3 p = a + (b - a) * hi;
            if (const std::optional<f32> h = self.m_terrain.heightAt(p.x, p.y)) {
                p.z = *h;
            }
            return p;
        }
    }
    return std::nullopt;
}

std::optional<f32> Game::floorWithoutDoodads(const void* game, f32 x, f32 y, f32 top, f32 bottom) {
    const Game& self = *static_cast<const Game*>(game);
    if (self.m_collision.isOpen() && top > bottom) {
        self.m_pickSet.reset(glm::dvec3(x, y, top));
        self.m_collision.gather(glm::dvec3(x - 0.01, y - 0.01, bottom), glm::dvec3(x + 0.01, y + 0.01, top),
                                self.m_pickSet);
        physics::Hit hit;
        if (physics::raycast(self.m_pickSet, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, bottom - top), hit,
                             pack::kCollisionDoodad)) {
            return top + hit.t * (bottom - top);
        }
    }
    const std::optional<f32> terrain = self.m_terrain.heightAt(x, y);
    return terrain && *terrain <= top && *terrain >= bottom ? terrain : std::nullopt;
}

u32 Game::addSelectionCircles(u32 quads, u32 nowMs) {
    // The circle's texture is asked for every frame in a world, so it stays loaded between targets.
    const TextureHandle texture =
        m_uiResources.ready() ? m_uiResources.texture(SelectionCircle::kTexture) : TextureHandle{};
    const std::span<ParticleVertex> vertices = m_renderer.particleVertices();
    if (!texture.valid() || vertices.size() < u64{quads} * 4) {
        return quads;
    }
    const auto add = [&](SelectionCircle& circle, const glm::dvec3& feet, f32 radius, u32 color,
                         SelectionCircle::GroundQuery ground = &Game::floorBetween, TextureHandle image = {}) {
        const u32 written = circle.build(feet, radius, color, m_camera.position, m_camera.forward(),
                                         vertices.subspan(u64{quads} * 4), ground, this);
        if (written > 0) {
            ParticleDraw draw;
            draw.texture = image.valid() ? image : texture;
            draw.blend = static_cast<u8>(pack::ModelBlend::Alpha);
            draw.flags = kParticleDrawUnfogged;
            draw.firstQuad = quads;
            draw.quadCount = written;
            m_particleDraws.push_back(draw);
            quads += written;
        }
    };
    if (m_selectionDemo && !m_session.inWorld()) {
        // Development: where --spawn character:<n> (4 yards ahead) and display:<n> (8 yards) stand, and a
        // hostile one beside the second, sampled again every frame.
        const glm::vec3 forward = m_camera.forward();
        const glm::dvec3 right = glm::normalize(glm::dvec3(forward.y, -forward.x, 0.0));
        const glm::dvec3 spots[3] = {groundInFront(4.0), groundInFront(8.0),
                                     groundInFront(8.0) + right * 4.0};
        const game::Reaction reactions[3] = {game::Reaction::Friendly, game::Reaction::Neutral,
                                             game::Reaction::Hostile};
        const f32 bounding[3] = {0.306f, 0.75f, 0.5f};
        // The neutral one sized by the model spawned last (display:<n> after character:<n>), as WoW.exe sizes it.
        const u32 last = m_crowd.lastActor();
        const game::ActorLook* lastLook = last != ~0u ? m_actors.look(last) : nullptr;
        const f32 lastHalf = lastLook ? m_actors.selectionHalfSize(last) * lastLook->scale : 0.0f;
        for (u32 i = 0; i < 3; ++i) {
            SelectionCircle circle;
            add(circle, spots[i],
                i == 1 && lastHalf > 0.0f ? SelectionCircle::ringOfHalfSize(lastHalf)
                                          : SelectionCircle::ringRadius(bounding[i]),
                SelectionCircle::color(reactions[i]));
        }
        return quads;
    }
    // The game object just tapped: a short golden flash at its foot, fading out.
    constexpr u32 kTapFlashMs = 600;
    if (const game::Entity* go = m_tapFlashGuid != 0 && nowMs - m_tapFlashMs < kTapFlashMs
                                     ? m_session.entities().find(m_tapFlashGuid)
                                     : nullptr) {
        const f32 fade = 1.0f - static_cast<f32>(nowMs - m_tapFlashMs) / static_cast<f32>(kTapFlashMs);
        add(m_tapFlash, glm::dvec3(go->positionAt(nowMs)), std::clamp(m_tapFlashRadius * 0.7f, 0.5f, 4.0f),
            ui::withAlpha(ui::rgba(255, 209, 0), fade));
    }
    // Phase 13 (spells): the shadow of a spell waiting for a place, the client's own textures (WoW.exe 0x48383B):
    // in range the runes of Spell-Shadow-Acceptable to the spell's radius (its edge is the texture's), else the
    // small red Spell-Shadow-Unacceptable (1.39 yd each way), untinted, along the world's axes.
    if (SpellTargetingScreens::Circle spell; m_screens.targeting().circle(spell)) {
        constexpr u64 kAcceptable = assetPathHash("Interface\\SpellShadow\\Spell-Shadow-Acceptable.blp");
        constexpr u64 kUnacceptable = assetPathHash("Interface\\SpellShadow\\Spell-Shadow-Unacceptable.blp");
        constexpr f32 kUnacceptableHalf = 1.388889f; // 0x3FB1C71C
        const TextureHandle shadow = m_uiResources.texture(spell.acceptable ? kAcceptable : kUnacceptable);
        const f32 half = spell.acceptable ? spell.radius : kUnacceptableHalf;
        m_spellCircle.setCells(spell.acceptable ? SelectionCircle::kMaxCells : 2); // wide: a finer grid on the ground
        m_spellCircle.setWorldAligned(true);
        if (shadow.valid()) {
            add(m_spellCircle, spell.center, half * SelectionCircle::kRingFraction, ui::rgba(255, 255, 255),
                &Game::floorWithoutDoodads, shadow);
        }
    }
    // The target (a unit, the player too) where its model stands, by how it looks to the player.
    const u64 target = m_session.inWorld() ? m_session.target() : 0;
    const game::Entity* unit = target ? m_session.entities().find(target) : nullptr;
    const game::Entity* me = m_session.entities().find(m_session.playerGuid());
    glm::dvec3 feet;
    f32 yaw = 0.0f;
    if (!unit || !me || !game::isUnit(*unit) || !m_units.placement(target, feet, yaw)) {
        return quads;
    }
    if (target != m_selectionTarget) {
        m_selectionTarget = target;
        m_selection.reset();
    }
    // Sized by its model as WoW.exe does; by the server's bounding radius only while the model loads.
    f32 ring = SelectionCircle::ringOfHalfSize(m_units.selectionHalfSize(target, m_actors));
    if (!(ring > 0.0f)) {
        f32 bounding = 0.0f;
        const u32 bits = unit->value(net::uf::UNIT_FIELD_BOUNDINGRADIUS);
        std::memcpy(&bounding, &bits, sizeof(bounding));
        ring = SelectionCircle::ringRadius(bounding);
    }
    add(m_selection, feet, ring, SelectionCircle::color(game::reactionTo(*me, *unit, &m_reactions)));
    return quads;
}

TextureHandle Game::bodyTexture(const void* game, u64 pathHash) {
    Game& self = *const_cast<Game*>(static_cast<const Game*>(game));
    return self.m_uiResources.ready() ? self.m_uiResources.texture(pathHash) : TextureHandle{};
}

u32 Game::portraitActor(const void* game, u64 guid) {
    const Game& self = *static_cast<const Game*>(game);
    if (guid == 0 || (!self.m_session.inWorld() && guid == self.m_session.playerGuid())) {
        return self.m_player.localActor();
    }
    return self.m_units.actorOf(guid);
}

void Game::spawnFromSpec(std::string_view spec) {
    while (!spec.empty()) {
        const usize comma = spec.find(',');
        const std::string_view item = spec.substr(0, comma);
        spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
        // kind:value[:degrees], the angle turning the actor away from facing the camera.
        const usize colon = item.find(':');
        const std::string_view kind = item.substr(0, colon);
        const std::string rest = colon == std::string_view::npos ? std::string() : std::string(item.substr(colon + 1));
        char* end = nullptr;
        const u32 value = rest.empty() ? 1u : static_cast<u32>(std::strtoul(rest.c_str(), &end, 10));
        const f32 turn = end && *end == ':' ? static_cast<f32>(std::atof(end + 1)) / 57.29578f : 0.0f;
        const f32 facing = m_camera.yaw + 3.14159f + turn;
        if (kind == "crowd") {
            m_crowd.spawnCharacters(m_actors, groundInFront(20.0), value, m_crowdSeed++);
        } else if (kind == "fight") {
            // Phase 9: a group fight with spells in front of the camera (fight:<fighters>).
            m_fight.start(m_actors, m_effects, m_spellVisualData, groundInFront(24.0), value, m_crowdSeed++);
        } else if (kind == "cast") {
            // Phase 9 (development): a caster casting spell <value> at a fighter, both in front of the camera.
            const glm::vec3 right = m_camera.right();
            m_fight.startDuel(m_actors, m_effects, m_spellVisualData, groundInFront(16.0),
                              glm::normalize(glm::dvec3(right.x, right.y, 0.0)), value);
        } else if (kind == "display") {
            m_crowd.spawnCreature(m_actors, groundInFront(8.0), facing, value);
        } else if (kind == "character") {
            m_crowd.spawnCharacters(m_actors, groundInFront(4.0), 1, value, facing);
        } else if (kind == "ghost" || kind == "stealth" || kind == "aura") {
            // Phase 11 (development): a character wearing an aura's look (game/UnitFade): ghost:<seed> 2 yards to the
            // left of where character:<n> stands (the Ghost aura, 8326), stealth:<seed> 2 yards to its right
            // (Stealth, 1784), aura:<spell id> 3 yards behind it.
            const glm::vec3 forward = m_camera.forward();
            const glm::dvec3 right = glm::normalize(glm::dvec3(forward.y, -forward.x, 0.0));
            glm::dvec3 at = groundInFront(4.0) + right * (kind == "ghost" ? -2.0 : kind == "stealth" ? 2.0 : 0.0);
            if (kind == "aura") {
                at = groundInFront(7.0);
            }
            const u32 spell = kind == "ghost" ? 8326u : kind == "stealth" ? 1784u : value;
            m_crowd.spawnCharacters(m_actors, at, 1, kind == "aura" ? spell : value, facing);
            if (const game::UnitFade* fade = m_unitFades.ofSpell(spell); fade && m_crowd.lastActor() != ~0u) {
                m_actors.setFade(m_crowd.lastActor(), glm::vec4(fade->tint, fade->alpha));
            } else {
                VD_LOG_WARN("Spell %u changes nothing in how its unit looks", spell);
            }
        } else if (kind == "mounted") {
            m_crowd.spawnRider(m_actors, groundInFront(6.0), facing, value, m_crowdSeed++);
        } else if (kind == "bleed") {
            // Phase 14 (bodyfx, development): a creature display (or a random character with no value) 5 yards
            // ahead, facing the camera, that bleeds every 0.6 s: hit in front, from behind, small and large.
            const glm::dvec3 at = groundInFront(5.0);
            if (rest.empty()) {
                m_crowd.spawnCharacters(m_actors, at, 1, m_crowdSeed++, facing);
            } else {
                m_crowd.spawnCreature(m_actors, at, facing, value);
            }
            m_bodyFx.demoBleed(m_crowd.lastActor(), at, facing);
        } else if (kind == "wake") {
            // Phase 14 (bodyfx, development): a swimmer's wake going back and forth over the water 8 yards ahead.
            m_bodyFx.demoWake(groundInFront(8.0), m_camera.yaw + 1.5708f);
        } else if (kind == "poof") {
            // Phase 14 (bodyfx, development): the poof of mounting (HARDCODED Mount Poof), 6 yards ahead, every 2 s.
            m_bodyFx.demoPoof(groundInFront(6.0), facing);
        } else if (kind == "archers") {
            // Phase 13 (development): archers:<race> (0: any): a row with a bow, a gun, a crossbow, a wand and a
            // thrown weapon drawn (sheath state 2), and one more with its bow put away, 8 yards ahead.
            const glm::vec3 forward = m_camera.forward();
            const glm::dvec3 right = glm::normalize(glm::dvec3(forward.y, -forward.x, 0.0));
            for (u32 i = 0; i < 6; ++i) {
                const glm::dvec3 at = groundInFront(8.0) + right * ((static_cast<f64>(i) - 2.5) * 1.5);
                m_crowd.spawnArcher(m_actors, at, facing, i % 5, m_crowdSeed++, static_cast<u8>(rest.empty() ? 0 : value),
                                    i == 5);
            }
        } else if (kind == "effect") {
            // Phase 9 (development): a spell effect model (actors.vdpk) looping on the ground in front of the camera.
            m_effects.spawnAt(Effects::effectModelHash(rest), groundInFront(6.0), facing);
        } else if (kind == "orbit") {
            // Phase 9 (development): the same, circling 3 yards around that point 2 yards up (ribbons, trails).
            const glm::dvec3 center = groundInFront(8.0) + glm::dvec3(0.0, 0.0, 2.0);
            const EffectHandle effect = m_effects.spawnAt(Effects::effectModelHash(rest), center, 0.0f);
            if (effect.valid()) {
                m_orbitEffects.push_back({effect, center, 0.0f});
            }
        } else if (kind == "gameobject") {
            // Phase 13 (development): a game object display (GameObjectDisplayInfo) whose model animates, 12 yards
            // ahead, going through closed, open, Custom0, closed, destroyed and rebuilt every 3 s (app/ServerObjects).
            m_serverObjects.spawnDemo(value, groundInFront(12.0), facing,
                                      static_cast<u32>(secondsBetween(m_startTime, Clock::now()) * 1000.0));
        } else if (kind == "doodad") {
            // Phase 9 (development): a doodad (models.vdpk) on the ground in front of the camera, facing it. Phase 14:
            // "doodad:<path>@<degrees>" turns it that much (a billboard seen from its side).
            const glm::dvec3 at = groundInFront(6.0);
            const usize rotated = rest.find('@');
            const f32 yaw =
                facing + (rotated != std::string::npos ? std::strtof(rest.c_str() + rotated + 1, nullptr) / 57.29578f
                                                       : 0.0f);
            const f32 c = std::cos(yaw), s = std::sin(yaw);
            const f32 transform[12] = {c, -s, 0.0f, static_cast<f32>(at.x), s, c, 0.0f, static_cast<f32>(at.y),
                                       0.0f, 0.0f, 1.0f, static_cast<f32>(at.z)};
            m_objects.addDynamic(assetPathHash(normalizeAssetPath(rest.substr(0, rotated))), transform);
        }
    }
}

void Game::drawActorsPanel() {
    const f32 font = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(font * 26.0f, font * 11.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Actors")) {
        ImGui::End();
        return;
    }
    if (!m_actors.isOpen()) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "No actors.vdpk (vd_baker bake --only actors)");
        ImGui::End();
        return;
    }
    const ActorStats& stats = m_actors.stats();
    ImGui::Text("actors %u, visible %u, sampled %u | bones %u | draws %u | events %u", stats.actors, stats.visible,
                stats.sampled, stats.bones, stats.draws, stats.events);
    ImGui::Text("models %u, textures %u, composed skins %u, pending %u | %.2f ms", stats.modelsLoaded,
                stats.texturesLoaded, stats.composedSkins, stats.pendingAssets, stats.animateMs);
    const Portraits::Stats& portraits = m_portraits.stats();
    ImGui::Text("portraits: %u of %u slots, %u drawn | last: %.3f ms CPU, %.3f ms GPU", portraits.slotsInUse,
                kPortraitSlots, portraits.renders, portraits.lastRenderMs, m_renderer.stats().portraitGpuMs);
    const ModelViews::Stats& views = m_modelViews.stats(); // phase 15
    ImGui::Text("model frames: %u shown, %u kept | last: %.3f ms CPU, %.3f ms GPU", views.shown, views.actors,
                views.lastRenderMs, m_renderer.stats().modelViewGpuMs);
    ImGui::InputInt("Display", &m_spawnDisplay);
    ImGui::SameLine();
    if (ImGui::Button("Creature")) {
        const glm::dvec3 at = groundInFront(8.0);
        m_crowd.spawnCreature(m_actors, at, m_camera.yaw + 3.14159f, static_cast<u32>(std::max(0, m_spawnDisplay)));
    }
    if (ImGui::Button("Character")) {
        m_crowd.spawnCharacters(m_actors, groundInFront(4.0), 1, m_crowdSeed++, m_camera.yaw + 3.14159f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Crowd (40)")) {
        m_crowd.spawnCharacters(m_actors, groundInFront(20.0), 40, m_crowdSeed++);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        m_crowd.clear(m_actors);
    }
    static constexpr struct {
        const char* name;
        u16 id;
    } kAnimations[] = {{"Own", 0xFFFF}, {"Stand", 0},  {"Walk", 4},          {"Run", 5},    {"Dead", 6},
                       {"Dance", 69},   {"Sit", 97},   {"Attack 1H", 17},    {"Spell", 32}, {"Mounted", 91}};
    static i32 selected = 0;
    if (ImGui::Combo("Animation", &selected, [](void*, i32 i) { return kAnimations[i].name; }, nullptr,
                     static_cast<i32>(std::size(kAnimations)))) {
        m_crowd.setAnimation(m_actors, kAnimations[selected].id);
    }
    ImGui::SliderFloat("Distance", &m_actors.drawDistance, 30.0f, 300.0f, "%.0f yd");
    ImGui::SliderFloat("Full rate", &m_actors.fullRateDistance, 0.0f, 100.0f, "%.0f yd");
    ImGui::SliderFloat("Half rate", &m_actors.halfRateDistance, 0.0f, 200.0f, "%.0f yd");
    ImGui::End();
}

void Game::drawLoadingScreen(f32 width, f32 height) {
    // Provisional (the real one, with LoadingScreens.dbc art, comes with the UI): the world behind is hidden until
    // the character stands on loaded ground.
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(width, height), IM_COL32(8, 8, 12, 255));
    char shown[128];
    text::format(shown, m_session.transferring() ? "VD_TRAVEL_LOADING_TRANSFER" : "VD_TRAVEL_LOADING_WORLD");
    const ImVec2 size = ImGui::CalcTextSize(shown);
    draw->AddText(ImVec2((width - size.x) * 0.5f, (height - size.y) * 0.5f), IM_COL32(230, 210, 150, 255), shown);
}

SceneLighting Game::sceneLighting(f32& drawDistance) const {
    SceneLighting scene;
    drawDistance = m_viewDistance;
    if (!m_lighting.ready()) {
        scene.fogEnd = m_viewDistance;
        scene.fogStart = m_viewDistance * 0.4f;
        return scene;
    }
    const LightingState light = m_lighting.evaluate(m_camera.position, m_timeOfDay, m_lightConditions);
    scene.lightDirection = light.lightDirection;
    scene.directColor = light.direct;
    scene.ambientColor = light.ambient;
    scene.fogColor = light.fog;
    scene.fogEnd = std::clamp(light.fogEnd * m_viewDistance / kReferenceFogEnd, 150.0f, m_viewDistance);
    if (m_lightConditions.underwater || m_lightConditions.dead) {
        // Phase 11: under water and in the ghost's world the client's fog is short and absolute (Light.dbc: 167 and
        // 180 yards around Goldshire, 28 in murky water): the view distance only clips it.
        scene.fogEnd = std::min(light.fogEnd, m_viewDistance);
    }
    scene.fogStart = scene.fogEnd * light.fogStartScale;
    scene.sky = light.sky;
    scene.riverClose = light.riverClose;
    scene.riverFar = light.riverFar;
    scene.oceanClose = light.oceanClose;
    scene.oceanFar = light.oceanFar;
    scene.waterAlphas =
        glm::vec4(light.riverShallowAlpha, light.riverDeepAlpha, light.oceanShallowAlpha, light.oceanDeepAlpha);
    scene.shadowColor = light.shadow;
    scene.sunColor = light.sun;
    scene.sunHalo = light.sunHalo;
    scene.cloudEmissive = light.cloudEmissive;
    scene.cloudLayer1 = light.cloudLayer1;
    scene.cloudLayer2 = light.cloudLayer2;
    scene.celestialGlow = light.celestialGlow;
    scene.cloudDensity = light.cloudDensity;
    scene.glow = light.glow;
    scene.skybox = light.skybox;
    scene.highlightSky = light.highlightSky;
    scene.dayFraction = m_timeOfDay / WorldLighting::kDayLength;
    scene.storm = m_lightConditions.storm;
    scene.underwater = m_lightConditions.underwater;
    scene.dead = m_lightConditions.dead;
    // Phase 11 (f11/interiors): windows glow at night; the specular highlights option; inside a WMO interior its own
    // fog (MFOG) takes over from the outdoor one, for everything the frame draws (WoW.exe 0x6cf054).
    scene.nightGlow = nightGlowFactor(m_timeOfDay);
    scene.terrainHighlights = m_terrainHighlights;
    if (m_interiorFog && m_interiorFogBlend > 0.0f) {
        const f32 weight = m_interiorFogBlend;
        const f32 end = std::min(m_interiorFog->end, m_viewDistance);
        scene.fogColor = glm::mix(scene.fogColor, m_interiorFog->color, weight);
        scene.fogEnd = glm::mix(scene.fogEnd, end, weight);
        scene.fogStart = glm::mix(scene.fogStart, end * m_interiorFog->startScale, weight);
    }
    if (m_lightConditions.underwater) {
        // Phase 11: under water nothing but the water's murk lies past the fog (its sky bands are bluer than its fog,
        // and would show as a band where the fogged ground ends).
        scene.sky.fill(scene.fogColor);
    }
    const ScreenFxParams screenFx =
        screenFxParams(light.glow, m_lightConditions.dead, m_fullScreenGlow, m_deathEffect); // phase 11
    scene.screenGlow = screenFx.glow;
    scene.screenDeath = screenFx.death;
    drawDistance = scene.fogEnd; // nothing past the fog end is visible
    return scene;
}

// Phase 14: the cinematic the server started (or VD_CINEMATIC offline) flies the camera; the server hears about each
// camera after the first and about the end, also when the player stops it.
void Game::updateCinematic(f32 dt) {
    const bool online = m_session.inWorld();
    const u32 wanted = online ? m_session.play().cinematic() : m_devCinematic;
    if (wanted == 0) {
        m_cinematicPlayed = 0;
    } else if (!m_cinematics.active() && wanted != m_cinematicPlayed) {
        m_cinematicPlayed = wanted;
        m_cinematicSkipShown = 0.0f;
        if (!m_cinematics.start(wanted)) {
            if (online) {
                m_session.play().completeCinematic(); // nothing to show: ended at once, as before phase 14
            }
            m_devCinematic = 0;
        }
    }
    if (!m_cinematics.active()) {
        return;
    }
    const CinematicView& view = m_cinematics.view();
    const bool ready = m_terrain.isOpen() && m_terrain.heightAt(view.eye.x, view.eye.y).has_value();
    const Cinematics::Step step = m_cinematics.update(dt, ready);
    if (step.voice != 0) {
        m_audio.stop(m_cinematicVoice, 0.3f);
        m_cinematicVoice = m_gameAudio.playSound(step.voice, audio::Category::Voices, 255);
    }
    if (step.nextCamera && online) {
        m_session.play().nextCinematicCamera();
    }
    if (step.finished) {
        m_audio.stop(m_cinematicVoice, 0.5f);
        m_cinematicVoice = {};
        if (online) {
            m_session.play().completeCinematic();
        }
        m_devCinematic = 0;
        return;
    }
    const RenderStats& render = m_renderer.stats();
    const f32 aspect = render.height > 0 ? static_cast<f32>(render.width) / static_cast<f32>(render.height) : 1.0f;
    m_cinematics.apply(m_camera, aspect);
    m_snapToGround = false;
}

// CinematicFrame: black bars over the picture's top and bottom on wide screens; touch adaptation of its Escape: a tap
// shows a skip button for a few seconds.
void Game::drawCinematic(f32 width, f32 height, f32 dt) {
    ui::Gui g{m_gameHud.context(), m_gameHud.drawList()};
    const f32 bar = Cinematics::barHeight(width, height);
    if (bar > 0.0f) {
        g.draw.solid({0.0f, 0.0f, width, bar}, 0xFF000000u);
        g.draw.solid({0.0f, height - bar, width, bar}, 0xFF000000u);
    }
    // Any tap on the picture brings the button back.
    if (g.ui.widget(ui::widgetId("cinematic.picture"), {0.0f, 0.0f, width, height}).clicked) {
        m_cinematicSkipShown = 4.0f;
    }
    m_cinematicSkipShown = std::max(0.0f, m_cinematicSkipShown - dt);
    if (m_cinematicSkipShown > 0.0f) {
        const f32 w = g.px(120.0f);
        const f32 h = g.px(36.0f);
        const ui::Rect area = safeArea(); // the screen minus the insets
        const f32 bottom = std::min(area.y + area.h, height - bar);
        const ui::Rect r{area.x + area.w - w - g.px(16.0f), bottom - h - g.px(16.0f), w, h};
        if (ui::button(g, ui::widgetId("cinematic.skip"), r, text::get("VD_CINEMATIC_SKIP"))) {
            m_cinematics.stop();
        }
    }
}

void Game::updateLightConditions(f32 dt) {
    // The camera under a liquid's surface sees the underwater sets (like the audio's underwater ambience), a ghost
    // the death sets.
    m_lightConditions.underwater = false;
    if (m_collision.isOpen()) {
        const glm::dvec3& eye = m_camera.position;
        const std::optional<physics::LiquidSample> above = m_collision.liquidAt(eye.x, eye.y, eye.z + 50.0, eye.z);
        m_lightConditions.underwater = above && static_cast<f64>(above->level) > eye.z;
    }
    m_lightConditions.dead = (m_session.inWorld() && m_session.play().death().ghost) || m_devGhost;
    // The weather darkens the sky, the fog and the light towards the storm sets as it comes in (last frame's: the
    // weather takes this frame's light).
    m_lightConditions.storm = m_weather.storm();
    m_fullScreenGlow = m_devGlow >= 0 ? m_devGlow != 0 : m_screens.settings().fullScreenGlow;
    m_deathEffect = m_devDeath >= 0 ? m_devDeath != 0 : m_screens.settings().deathEffect;
    // Phase 11 (f11/interiors): the fog of the WMO interior the camera is in, and the specular option. 1.12 (WoW.exe
    // 0x6cefcb) moves the frame's fog towards it a quarter of the way a second, and back out the same way with the last
    // one it had; a loading screen leaves nothing to watch, so the fog is already there behind it.
    constexpr f32 kInteriorFogRate = 0.25f; // 0x8115b0, per second
    const std::optional<WorldObjects::FogSample> fog = m_objects.fogAt(m_camera.position, m_lightConditions.underwater);
    if (fog) {
        m_interiorFog = fog;
    }
    const f32 target = fog ? 1.0f : 0.0f;
    m_interiorFogBlend = m_loadingShown ? target
                                        : std::clamp(m_interiorFogBlend + (fog ? dt : -dt) * kInteriorFogRate, 0.0f, 1.0f);
    m_terrainHighlights = m_screens.settings().terrainHighlights;
}

void Game::updateLocalFade() {
    // Development (VD_GHOST): the offline character wears the Ghost aura's look, as its own ghost does online.
    constexpr u32 kGhostSpell = 8326;
    const u32 local = m_player.localActor();
    const game::UnitFade* ghost = m_devGhost ? m_unitFades.ofSpell(kGhostSpell) : nullptr;
    if (local != ~0u && ghost) {
        m_actors.setFade(local, glm::vec4(ghost->tint, ghost->alpha));
    }
}

void Game::updateWeather(f32 dt, FrameParams& params) {
    const glm::dvec3 focus = m_player.active() ? m_player.character().position() : m_camera.position;
    WeatherControl::Inputs in;
    in.dt = dt;
    in.camera = &m_camera;
    in.screenHeight = static_cast<f32>(m_renderer.stats().height);
    in.map = m_terrain.mapDirectory();
    in.indoors = m_objects.isOpen() && m_objects.insideInterior(focus); // the minimap's test, from the same point
    in.underwater = m_lightConditions.underwater;
    in.density = m_screens.settings().weatherDensity;
    in.effectScale = m_governor.output().effectScale;
    in.light = &params.lighting;
    in.server = m_session.inWorld() ? &m_session.play().weather() : nullptr;
    in.collision = m_collision.isOpen() ? &m_collision : nullptr;
    in.terrain = &m_terrain;
    m_weather.update(in);
    params.weather = m_weather.uniforms();
}

void Game::updateTravel(u32 nowMs) {
    // Phase 8: the zone under the player (the zone text, the minimap's name) and the area triggers it walks into.
    // Offline the free camera stands in for it (triggers are only logged then).
    if (!m_terrain.isOpen()) {
        return;
    }
    const WorldMap* shown = findWorldMap(m_terrain.mapDirectory());
    const u32 map = m_session.inWorld() ? m_session.map() : shown ? shown->id : 0;
    const glm::dvec3 feet = m_player.active() ? m_player.character().position() : m_camera.position;
    game::Travel& travel = m_session.play().travel();
    const bool canTrigger = m_session.inWorld() && m_player.active() && m_player.online() &&
                            m_player.triggersAllowed(nowMs) && !travel.flying();
    // Phase 14 (sound): the room of a building under the feet (WMOAreaTable): its area and its name (the subzone).
    m_wmoAreas.setMap(m_terrain.mapDirectory());
    m_wmoAreas.update();
    const WmoAreaHit& room = roomAt(feet);
    travel.setIndoorArea(room.valid() ? room.areaId : 0, room.valid() ? room.name : std::string_view{});
    if (m_traceRooms && room.key() != m_tracedRoom) {
        m_tracedRoom = room.key();
        VD_LOG_INFO("WMO area: %s (WMO %d, name set %d, group %d, flags 0x%x) area %u, music %u, ambience %u, intro %u",
                    room.valid() ? std::string(room.name).c_str() : "(outdoors)", room.wmoId, room.nameSet,
                    room.groupId, room.groupFlags, room.areaId, room.zoneMusic, room.ambience, room.intro);
    }
    travel.updatePosition(map, glm::vec3(feet), m_terrain.areaAt(feet.x, feet.y), canTrigger, nowMs);
}

void Game::tick() {
    if (m_firstRunMode) { // phase 22 (pairing)
        tickFirstRun();
        return;
    }
    if (!m_performanceHintTried) {
        m_performanceHintTried = true;
        const bool hint = m_performanceHint.startForThreads(m_jobs.threadIds(), kFrameTargetNanos);
#if defined(__ANDROID__)
        // Without a session that boosts the workers, their bursts land on idle little cores (see Actors).
        m_actors.sampleOnWorkers = hint;
        m_effects.particles().settings().workers = hint;
#else
        static_cast<void>(hint);
#endif
    }
    {
        VD_PROFILE_ZONE("Game::tick");
        const Clock::time_point now = Clock::now();
        // Tick start to tick start (m_lastFrame is taken after present, right before this tick).
        const f32 dt = std::min(0.1f, static_cast<f32>(secondsBetween(m_lastTickStart, now)));
        m_lastTickStart = now;
        updateQuality(dt); // phase 10: this frame's resolution, distances and effects
        // Text entry: this frame's submit / cancel; when typing starts, the movement keys held until then are let go.
        m_textInput.newFrame();
        if (m_textInput.active() && !m_typing) {
            m_fly.releaseAll();
            m_player.releaseAll();
        }
        m_typing = m_textInput.active();

        if (m_benchmark.running()) {
            const std::optional<f32> ground = m_terrain.heightAt(m_camera.position.x, m_camera.position.y);
            const bool looping = m_benchmark.looping();
            if (!m_benchmark.update(m_camera, dt, ground ? &*ground : nullptr)) {
                if (looping) {
                    if (BenchmarkMinute minute; m_benchmark.takeMinute(minute)) {
                        logSoakMinute(minute); // the last, shorter minute before the summary
                    }
                    logSoakSummary();
                }
                m_exitRequested = m_exitRequested || m_exitAfterBenchmark;
            }
            m_snapToGround = false;
        } else if (!m_player.active() || m_freeCamera) {
            m_fly.update(m_camera, dt, m_contentScale);
        }
        const u32 nowMs = static_cast<u32>(secondsBetween(m_startTime, now) * 1000.0);
        if (m_pendingWorld) { // shown under the loading screen last frame: now the map opens
            const WorldEntry entry = *m_pendingWorld;
            m_pendingWorld.reset();
            enterWorld(entry);
        }
        const Clock::time_point networkStart = Clock::now();
        if (const std::optional<WorldEntry> entered = m_session.update(nowMs)) {
            m_loadingUntilMs = nowMs + kLoadingMinimumMs;
            const WorldMap* next = findWorldMap(entered->map);
            if (next && m_terrain.isOpen() && m_terrain.mapDirectory() != next->directory && m_gameHud.ready()) {
                // Another map: the character waits at its new place (nothing simulated or sent) while the loading
                // screen is drawn once before the map opens.
                m_player.leave(&m_actors);
                m_player.enterWorld(entered->position, entered->orientation);
                m_freeCamera = false;
                m_pendingWorld = entered;
            } else {
                enterWorld(*entered);
            }
        }
        addSection(kSectionNetwork, secondsBetween(networkStart, Clock::now()) * 1000.0);
        updateTravelTest(dt); // phase 18 (world): VD_WORLD_TRAVEL
        if (m_session.quitRequested()) {
            m_exitRequested = true; // parity (popups): Exit Game, once logged out
        }
        if (m_player.active() && m_player.online() && !m_session.inWorld()) {
            m_player.leave(&m_actors); // disconnected: the free camera stays where it was
            m_freeCamera = true;
        }
        // The player's collision shape from its model, once its display is known (and when it changes). Phase 13: the
        // model of the unit it moves (a Mind Controlled one) while it does.
        if (m_player.online()) {
            const u64 unit = m_player.characterUnit() != 0 ? m_player.characterUnit() : m_session.playerGuid();
            if (const game::Entity* me = m_session.entities().find(unit)) {
                const u32 display = me->value(net::uf::UNIT_FIELD_DISPLAYID);
                f32 radius = 0.0f, height = 0.0f;
                if (display != 0 && display != m_playerDisplay && displayShape(display, radius, height)) {
                    m_player.setShape(radius, height);
                    m_playerDisplay = display;
                }
            }
        }
        const Clock::time_point playerStart = Clock::now();
        if (m_collision.isOpen()) {
            const glm::dvec3 focus = m_player.active() ? m_player.character().position() : m_camera.position;
            m_collision.update(focus, kCollisionRadius);
        }
        if (m_walkTest.running() && !m_walkTest.update(m_player, dt) && m_exitAfterBenchmark) {
            m_exitRequested = true;
        }
        if (m_fakeControlMs != 0 && m_player.active() && !m_player.online() && !m_player.waiting()) {
            // Development (VD_FAKE_CONTROL): the loss comes once, a while after the character stands.
            if (m_fakeControlAtMs == 0) {
                m_fakeControlAtMs = nowMs + m_fakeControlAfterMs;
            } else if (static_cast<i32>(nowMs - m_fakeControlAtMs) >= 0 && !m_fakeControlDone) {
                m_fakeControlDone = true;
                m_player.fakeControlLoss(nowMs, m_fakeControlMs);
                VD_LOG_INFO("VD_FAKE_CONTROL: control lost for %u ms", m_fakeControlMs);
            }
        }
        if (m_player.active()) {
            m_player.setGamepadInput(m_gamepad);
            if (m_freeCamera || m_loadingShown) { // under the loading screen keys and pads move nothing either
                m_player.releaseAll();
            }
            // Phase 15 (options): the camera options (benchmarks keep the camera they were measured with).
            if (!m_benchmark.running()) {
                const SettingsView& options = m_screens.settings();
                PlayerController::CameraOptions camera;
                camera.followStyle = options.cameraFollowStyle;
                camera.followSpeed = options.cameraFollowSpeed;
                camera.maxDistance = 15.0f * options.cameraDistanceMaxFactor; // cameraDistanceMax "15.0"
                camera.terrainTilt = options.cameraTerrainTilt;
                camera.bobbing = options.cameraBobbing;
                camera.waterCollision = options.cameraWaterCollision;
                m_player.setCameraOptions(camera);
            } else {
                m_player.setCameraOptions({});
            }
            m_player.update(m_player.online() ? &m_session : nullptr, m_player.online() ? nullptr : &m_actors,
                            m_collision, nowMs, dt, m_contentScale);
            if (!m_freeCamera && !m_benchmark.running()) {
                m_player.updateCamera(m_collision, m_camera, dt);
                m_snapToGround = false;
                // Phase 18 (motion): through a far sight object (Far Sight, Eagle Eye), maybe far from the player.
                const physics::CharacterController& character = m_player.character();
                m_farSight.update(m_session, m_player.online(),
                                  character.position() + glm::dvec3(0.0, 0.0, character.height * 0.9), m_camera, nowMs,
                                  [](const void* game, f64 x, f64 y) {
                                      return static_cast<const Game*>(game)->m_terrain.heightAt(x, y);
                                  },
                                  this);
            }
        }
        addSection(kSectionPlayer, secondsBetween(playerStart, Clock::now()) * 1000.0);
        updateCinematic(dt); // phase 14: a cinematic takes the camera
        updateTravel(nowMs);
        if (m_motionDemo.active()) { // phase 18 (motion): synthetic server units offline (VD_MOTION_DEMO)
            const glm::dvec3 center = m_player.active() ? m_player.character().position() : m_camera.position;
            m_motionDemo.update(m_session, m_units, m_transports, glm::vec3(center), nowMs,
                                [](const void* game, f32 x, f32 y, f32 top, f32 bottom) -> std::optional<f32> {
                                    const std::optional<f64> z =
                                        static_cast<const Game*>(game)->m_collision.groundHeight(x, y, top, bottom);
                                    return z ? std::optional<f32>(static_cast<f32>(*z)) : std::nullopt;
                                },
                                this);
        }
        m_units.update(m_session, m_actors, nowMs, dt, &Game::groundBelow, this);
        m_questMarks.update(m_session, m_units, m_actors, [](const void* hud, u64 guid) {
            return static_cast<const Hud*>(hud)->nameShown(guid); // last frame's names
        }, &m_gameHud);
        updateLocalFade(); // phase 11 (VD_GHOST)
        {
            // Phase 14: the character the camera follows, translucent with the camera inside it, gone in first person.
            const bool following = m_player.active() && !m_freeCamera && !m_cinematics.active() && !m_benchmark.running();
            u32 actor = m_player.localActor();
            if (m_player.online()) {
                const u64 unit = m_player.characterUnit() != 0 ? m_player.characterUnit() : m_session.playerGuid();
                actor = m_units.actorOf(unit);
            }
            if (actor != ~0u) {
                m_actors.setCameraFade(actor, following ? m_player.cameraFade() : 1.0f);
            }
            if (m_player.localActor() != ~0u) {
                m_actors.setBody(m_player.localActor(), true); // phase 14 (bodyfx): the offline character's shadow
            }
        }
        {
            // Phase 9: the spells the server tells about, over the units just placed.
            const Clock::time_point spellsStart = Clock::now();
            m_spellEffects.update(m_session, nowMs, dt, m_camera.position);
            m_fight.update(nowMs, dt, m_camera.position, m_terrain.isOpen() ? &m_terrain : nullptr);
            addSection(kSectionSpells, secondsBetween(spellsStart, Clock::now()) * 1000.0 +
                                           m_units.combatStats().readMs);
        }
        m_serverObjects.update(m_session, m_objects, nowMs);
        m_transports.update(m_session, m_objects, m_collision, nowMs);
        // Scripted chat lines: one every 8 s once the character stands in the world; then the walking route.
        if (m_player.active() && !m_player.waiting() && !m_session.transferring() &&
            (!m_commands.empty() || !m_onlineWalk.empty())) {
            m_commandDelay += dt;
            if (m_commandDelay >= 8.0f) {
                m_commandDelay = 0.0f;
                if (!m_commands.empty()) {
                    const usize end = m_commands.find(';');
                    m_session.say(std::string_view(m_commands).substr(0, end));
                    m_commands.erase(0, end == std::string::npos ? std::string::npos : end + 1);
                } else {
                    if (const WalkRoute* route = findWalkRoute(m_onlineWalk)) {
                        m_walkTest.start(*route, true);
                    } else {
                        VD_LOG_ERROR("Unknown walking route %s", m_onlineWalk.c_str());
                    }
                    m_onlineWalk.clear();
                }
            }
        }
        m_timeOfDay = std::fmod(m_timeOfDay + m_timeSpeed * dt, WorldLighting::kDayLength);
        if (m_glueShown) {
            // Phase 18: the glue scene's models stream in; the world waits, but the map opened at start finishes
            // opening (without it the renderer draws no scene: a phone faster than the map showed the test triangle).
            m_terrain.updateOpening(m_renderer);
            m_actors.update(m_renderer);
        } else if (m_terrain.isOpen()) {
            const Clock::time_point terrainStart = Clock::now();
            m_terrain.update(m_camera.position, m_viewDistance, m_renderer);
            addSection(kSectionTerrain, secondsBetween(terrainStart, Clock::now()) * 1000.0);
            const Clock::time_point objectsStart = Clock::now();
            m_objects.animationMs = nowMs; // phase 14: the doodads' clock
            m_objects.update(m_renderer);
            m_grass.update(m_renderer); // phase 14
            addSection(kSectionObjects, secondsBetween(objectsStart, Clock::now()) * 1000.0);
            m_actors.update(m_renderer);
            m_crowd.update(m_actors, dt, &m_terrain);
            if (m_snapToGround) {
                if (const std::optional<f32> ground = m_terrain.heightAt(m_camera.position.x, m_camera.position.y)) {
                    m_camera.position.z = *ground + m_snapHeight;
                    m_snapToGround = false;
                }
            }
            if (!m_pendingSpawn.empty() && !m_snapToGround && m_actors.isOpen() &&
                m_terrain.heightAt(m_camera.position.x, m_camera.position.y)) {
                spawnFromSpec(m_pendingSpawn);
                m_pendingSpawn.clear();
            }
            if (m_pendingWalk && !m_snapToGround && m_collision.isOpen() &&
                m_collision.ready(m_camera.position.x, m_camera.position.y, 2.0f)) {
                m_pendingWalk = false;
                if (!m_player.active()) {
                    toggleCharacter();
                }
            }
        }

#if defined(VD_DIST)
        constexpr bool debugUi = false; // phase 22: no debug overlay (its panels compile away)
#else
        const bool debugUi = m_renderer.beginDebugUi(dt);
#endif
        if (debugUi) {
            VD_PROFILE_ZONE("DebugHud");
            // The touch HUD owns the top of the screen: the overlay goes with the panels (the log keeps the numbers);
            // gallery captures show the dialogs alone.
            if (!m_hideDebugUi || (!m_gameHud.ready() && !m_uiGallery.active())) {
                m_hud.draw(m_renderer.stats(), m_jobs.workerCount(), m_dbPack.entryCount(),
                           m_texturePack.entryCount());
            }
            if (!m_hideDebugUi) {
                drawWorldPanel();
                drawActorsPanel();
                m_session.drawPanel();
                m_gameAudio.drawPanel();
                m_weather.drawPanel(); // phase 11
                if (m_player.active()) {
                    const f32 font = ImGui::GetFontSize();
                    ImGui::SetNextWindowPos(ImVec2(font, font * 30.0f), ImGuiCond_FirstUseEver);
                    if (ImGui::Begin("Character")) {
                        m_player.drawPanel();
                        const physics::CollisionStats& collision = m_collision.stats();
                        ImGui::Text("collision: %u tiles (%.1f MB), %u loading | F: free camera",
                                    collision.residentTiles,
                                    static_cast<f64>(collision.residentBytes) / 1e6, collision.loadingTiles);
                    }
                    ImGui::End();
                }
                m_textureBrowser.draw(m_renderer, m_texturePack);
                m_dbcBrowser.draw(m_dbPack);
            }
        }

        FrameParams params;
        params.timeSeconds = static_cast<f32>(secondsBetween(m_startTime, now));
        {
            // The touch UI over whatever is drawn (phase 7).
            const Clock::time_point uiStart = Clock::now();
            m_uiResources.update(m_renderer);
            const RenderStats& render = m_renderer.stats();
            HudFrame hud;
            hud.width = static_cast<f32>(render.width);
            hud.height = static_cast<f32>(render.height);
            SettingsView& settings = m_screens.settings();
            hud.dp = m_contentScale * m_uiScale * std::clamp(settings.uiScale, 0.6f, 1.6f);
            // Phase 20: a millimetre (the touch cluster keeps its places in mm): 160 dp an inch, without the player's
            // interface scale (the cluster has its own). Phase 20 (edit, §3.4): the screen's real millimetres when the
            // platform knows them (Android's xdpi: the density is a bucket).
            hud.mm = m_pixelsPerMm > 0.0f ? m_pixelsPerMm : m_contentScale * m_uiScale * (160.0f / 25.4f);
            hud.enemyPlates = settings.enemyPlates; // parity: V / Shift-V
            hud.friendPlates = settings.friendPlates;
            hud.cameraSpeed = settings.cameraSpeed;
            hud.invertCamera = settings.invertCamera;
            hud.fps = settings.showFps ? m_hud.averageFps() : 0.0f;
            hud.safeArea = safeArea();
            hud.edges = screenEdges(); // phase 20 (look)
            m_screens.setScreenEdges(hud.edges);
            hud.nowMs = nowMs;
            hud.dt = dt;
            hud.session = &m_session;
            hud.player = m_player.active() ? &m_player : nullptr;
            hud.freeCamera = (m_freeCamera || !m_player.active()) ? &m_fly : nullptr;
            const f32 aspect = hud.height > 0.0f ? hud.width / hud.height : 1.0f;
            hud.viewProj = m_camera.projection(aspect) * m_camera.view();
            hud.camera = m_camera.position;
            m_units.collectViews(m_actors, m_unitViews);
            hud.units = m_unitViews;
            m_serverObjects.collectViews(m_session, m_objects, m_objectViews); // parity: tapping game objects
            hud.objects = m_objectViews;
            hud.groundPick = &Game::groundAlongRay; // phase 13 (spells): the circle under the finger
            hud.groundContext = this;
            hud.reactions = &m_reactions;
            hud.map = m_terrain.mapDirectory();
            hud.position = m_player.active() ? m_player.character().position() : m_camera.position;
            hud.facing = m_player.active() ? m_player.character().facing() : m_camera.yaw;
            // Phase 13: UIParent.isOutOfControl (PLAYER_CONTROL_LOST is ignored on a taxi).
            hud.outOfControl = m_session.inWorld()
                                   ? !m_session.play().control().playerHasControl() && !m_session.play().travel().flying()
                                   : m_player.active() && m_player.fakeControlLost();
            // Parity: the player's and the target's portraits (the offline character's without a server).
            m_portraits.beginFrame(m_actors, m_renderer, m_terrain.isOpen()); // drawn with the world
            m_modelViews.beginFrame(m_actors, m_renderer, m_terrain.isOpen()); // phase 15: the model frames, too
            hud.playerPortrait = m_portraits.unit(m_session.inWorld() ? m_session.playerGuid() : 0);
            hud.targetPortrait =
                m_session.target() != 0 ? m_portraits.unit(m_session.target()) : TextureHandle{};
            // Parity pass: the minimap's indoor zoom and the game clock (its day / night dial).
            hud.indoors = m_objects.isOpen() && m_objects.insideInterior(hud.position);
            if (hud.indoors) { // phase 15: the indoor minimap's building
                hud.interiorWmo = m_objects.interiorPlace().wmo;
                hud.interiorTransform = m_objects.interiorPlace().transform;
                hud.interiorScale = m_objects.interiorPlace().scale;
            }
            hud.gameTime = m_timeOfDay;
            if (m_uiGallery.active()) { // development: the dialogs with sample data instead of the HUD
                m_uiGallery.update(hud.width, hud.height, hud.dp, uiArea(), nowMs, m_textInput);
                params.uiVertices = m_uiGallery.vertices();
                params.uiCommands = m_uiGallery.commands();
            } else if (m_gluePreview.background != GlueBackground::None) {
                m_gameHud.begin(hud); // development (VD_GLUE_PREVIEW): the glue scene alone
                m_gameHud.end();
                params.uiVertices = m_gameHud.vertices();
                params.uiCommands = m_gameHud.commands();
            } else {
                // Before the world: the login screen. In it: the HUD, then the windows over it.
                const bool login = m_screens.wantsLogin(m_session) && !m_hudDemo;
                if (!login) {
                    m_screens.setPlayerMoving(m_player.online() && m_player.moving()); // phase 15 (options)
                    m_screens.prepareHud(m_session, hud, nowMs);
                    // Parity: chat bubbles over the speakers (ChatBubbles / ChatBubblesParty).
                    if (m_session.inWorld()) {
                        m_chatBubbles.update(m_session.play(), m_session.playerGuid(), nowMs,
                                             {settings.chatBubbles, settings.partyChatBubbles});
                        hud.bubbles = &m_chatBubbles;
                    }
                }
                // Phase 8: the zone channels follow the area under the player; the character being created stands
                // in front of the camera (lowered to eye height, as the original screen frames it).
                m_screens.social().setArea(m_terrain.isOpen() ? m_terrain.areaAt(hud.position.x, hud.position.y) : 0);
                if (const game::CharacterLook* look =
                        login && !m_glueStage.available() ? m_screens.social().previewLook() : nullptr) {
                    if (const std::optional<f32> ground = m_terrain.heightAt(m_camera.position.x, m_camera.position.y)) {
                        m_camera.position.z = *ground + CharacterPreview::cameraHeight(look->race);
                        m_camera.pitch = -0.08f;
                    }
                    m_preview.update(m_actors, look, groundInFront(CharacterPreview::cameraDistance(look->race)),
                                     m_camera.yaw + 3.14159f + m_screens.social().previewTurn());
                } else if (m_preview.active()) {
                    m_preview.clear(m_actors);
                }
                if (m_hudDemo) {
                    // Development (VD_HUD_DEMO): sample buttons, cast bar and auras to look at the layout offline.
                    // Parity pass: twelve buttons, the bottom left bar and the stance bar in every state (app/HudDemo).
                    const HudDemo& demo = hudDemo();
                    static const HudAura demoAuras[5] = {
                        {assetPathHash("Interface\\Icons\\INV_Sword_04.blp"), 1700.0f, 0, false, 0, true},
                        {assetPathHash("Interface\\Icons\\Ability_Warrior_BattleShout.blp"), 95.0f},
                        {assetPathHash("Interface\\Icons\\Spell_Holy_WordFortitude.blp"), 1500.0f},
                        {assetPathHash("Interface\\Icons\\Spell_Frost_FrostNova.blp"), 25.0f, 0, true, 1},
                        {assetPathHash("Interface\\Icons\\Ability_Gouge.blp"), 12.0f, 2, true}};
                    hud.actions = demo.actions;
                    hud.extraActions = demo.extraActions;
                    for (u32 b = 0; b < 3; ++b) {
                        hud.moreBars[b] = demo.moreBars[b]; // phase 15 (options)
                    }
                    hud.stances = demo.stances;
                    hud.playerAuras = demoAuras;
                    hud.context = HudContextAction::StopAttack;
                    hud.contextName = "Kobold Vermin"; // parity: the context button's subject and cursor
                    hud.contextIcon = assetPathHash("Interface\\Cursor\\Attack.blp");
                    hud.actionPage = 0; // the page button (ActionButton.lua's six pages)
                    hud.actionPages = 6;
                    hud.cast = {true, "Heroic Strike", 0.6f};
                }
                m_gameHud.begin(hud);
                // Phase 8: a far teleport, or the ground not loaded yet under the character: the loading screen.
                // Phase 18 (world): and the map's terrain textures, which may come a moment after the ground stands.
                // And at least kLoadingMinimumMs after entering a world.
                const bool waiting = m_session.transferring() || (m_player.active() && m_player.waiting()) ||
                                     (m_player.active() && m_terrain.isOpen() && !m_terrain.ready());
                const bool loading = !login && (waiting || static_cast<i32>(m_loadingUntilMs - nowMs) > 0);
                if (m_cinematics.active()) {
                    m_loadingShown = false;
                    drawCinematic(hud.width, hud.height, dt); // phase 14: CinematicFrame hides the interface
                } else if (loading) {
                    if (!m_loadingShown) {
                        m_loadingShown = true;
                        m_loadingProgress = 0.0f;
                    }
                    // Far teleport sent, then the map open, its terrain under the character, its collision.
                    const TerrainStats& terrain = m_terrain.stats();
                    const glm::dvec3 feet = m_player.active() ? m_player.character().position() : m_camera.position;
                    const f32 tiles = static_cast<f32>(terrain.loadedTiles) /
                                      static_cast<f32>(std::max(1u, terrain.loadedTiles + terrain.inFlight + terrain.waitingUpload));
                    f32 progress = 0.1f;
                    if (!m_session.transferring()) { // phase 18 (world): the map opening off the game thread too
                        progress = 0.3f + 0.15f * m_terrain.openProgress() + 0.3f * tiles +
                                   (m_terrain.heightAt(feet.x, feet.y) ? 0.15f : 0.0f);
                    }
                    progress = waiting ? std::min(progress, 0.95f) : 1.0f; // only the minimum time left: full
                    m_loadingProgress = std::max(m_loadingProgress, progress);
                    const game::Travel& travel = m_session.play().travel();
                    u32 map = m_session.inWorld() ? m_session.map() : 0;
                    if (m_session.transferring() && travel.transferPending()) {
                        map = travel.transferMap();
                    } else if (!m_session.inWorld()) {
                        const WorldMap* shown = findWorldMap(m_terrain.mapDirectory());
                        map = shown ? shown->id : 0;
                    }
                    DialogContext c{m_gameHud.context(), m_gameHud.drawList(), m_uiResources,
                                    hud.width >= hud.height * 1.6f ? ScreenProfile::Wide : ScreenProfile::Square,
                                    uiArea()};
                    const bool unsupported = m_session.inWorld() && !m_session.transferring() && !m_pendingWorld &&
                                             !findWorldMap(m_session.map());
                    // Phase 20 (retouches): as the 1.12, nothing under the bar unless the map cannot be shown.
                    m_screens.travel().drawLoading(c, map, m_loadingProgress,
                                                   unsupported ? text::get("VD_TRAVEL_LOADING_NO_PACK")
                                                               : std::string_view{});
                } else if (login) {
                    m_loadingShown = false;
                    m_screens.drawLogin(m_session, m_gameHud, m_textInput, uiArea());
                } else {
                    m_loadingShown = false;
                    m_gameHud.drawGame(hud);
                    m_screens.drawGame(m_session, m_gameHud, m_textInput, uiArea(), nowMs);
                    if (const u64 tapped = m_gameHud.output().objectTapped;
                        tapped != 0) { // parity: its flash
                        m_tapFlashGuid = tapped;
                        m_tapFlashMs = nowMs;
                        m_tapFlash.reset();
                        for (const WorldObjectView& view : m_objectViews) {
                            m_tapFlashRadius = view.guid == tapped ? view.radius : m_tapFlashRadius;
                        }
                    }
                }
                m_gameHud.end();
                if (m_gameHud.takeDebugToggle()) {
                    m_hideDebugUi = !m_hideDebugUi;
                }
                params.uiVertices = m_gameHud.vertices();
                params.uiCommands = m_gameHud.commands();
            }
            addSection(kSectionUi, secondsBetween(uiStart, Clock::now()) * 1000.0);
            // Phase 18: what the glue screens asked for this frame (none in the world, the gallery or offline).
            const bool glue = !m_uiGallery.active() && m_screens.wantsLogin(m_session) && !m_hudDemo &&
                              m_screens.glueScene().background != GlueBackground::None && m_glueStage.available();
            const GlueSceneRequest& request = m_gluePreview.background != GlueBackground::None ? m_gluePreview
                                              : glue                                            ? m_screens.glueScene()
                                                                                                : GlueSceneRequest{};
            m_glueStage.update(request, m_actors, m_camera, hud.height > 0.0f ? hud.width / hud.height : 1.0f, dt);
            m_glueShown = m_glueStage.active();
        }
        updateAudio(nowMs, dt); // phase 9: music, ambience and the sounds of what happened this frame
        if (m_glueShown) {
            collectGlue(params, dt, nowMs); // phase 18: the glue scene instead of the world
        } else if (m_terrain.isOpen()) {
            const RenderStats& render = m_renderer.stats();
            const f32 aspect = render.height > 0 ? static_cast<f32>(render.width) / static_cast<f32>(render.height)
                                                 : 1.0f;
            f32 drawDistance = m_viewDistance;
            updateLightConditions(dt);
            m_sky.overrideConditions(m_lightConditions); // phase 11: development switches (VD_STORM)
            params.lighting = sceneLighting(drawDistance);
            m_sky.update(dt, m_timeOfDay / WorldLighting::kDayLength, m_session.serverDay().value_or(0),
                         m_camera, params.lighting, &m_objects, &m_collision, drawDistance);
            params.sky = m_sky.frame();
            m_drawDistance = drawDistance;
            updateWeather(dt, params); // phase 11
            m_camera.farPlane = std::max(drawDistance, m_horizonDistance) + pack::kTileSize * 0.5f;
            params.viewProj = m_camera.projection(aspect) * m_camera.view() * cameraShake(nowMs); // phase 14
            const Frustum frustum = Frustum::fromViewProj(params.viewProj);
            const Clock::time_point collectStart = Clock::now();
            // Skeletons sample on the workers while the terrain and objects are culled here.
            const Clock::time_point actorsStart = Clock::now();
            for (OrbitEffect& orbit : m_orbitEffects) {
                orbit.angle += dt * 1.5f;
                const glm::dvec3 at =
                    orbit.center + glm::dvec3(std::cos(orbit.angle), std::sin(orbit.angle), 0.0) * 3.0;
                m_effects.move(orbit.effect, at, orbit.angle + 1.5708f);
            }
            m_effects.updateSpells(dt);
            m_actors.beginCollect(m_camera.position, frustum, m_camera.forward(), dt, nowMs, m_jobs, m_renderer,
                                  m_actorFrame);
            f64 actorsMs = secondsBetween(actorsStart, Clock::now()) * 1000.0;
            // Phase 18 (world): the camera's group the 1.12 way and the walk through its building's portals; inside
            // with no portal leading out, nothing of the world outside is drawn (terrain, grass, other buildings).
            m_objects.beginView(m_camera.position, params.viewProj);
            const bool outdoors = m_objects.outdoorsVisible();
            if (outdoors) {
                m_terrain.collectDraws(m_camera.position, frustum, drawDistance, m_terrainDraws);
                m_terrain.collectHorizon(m_camera.position, frustum, m_horizonDistance, m_horizonDraws);
            } else {
                m_terrainDraws.clear();
                m_horizonDraws.clear();
            }
            m_objects.collect(m_camera.position, frustum, drawDistance, m_modelDraws, m_modelInstances, m_modelGroups);
            params.models = m_modelDraws;
            params.modelInstances = m_modelInstances;
            params.modelGroups = m_modelGroups;
            // Phase 14: the ground clutter of the chunks in reach, placed by WoW.exe's generator as they come near.
            if (outdoors) {
                m_grass.collect(m_camera.position, m_camera.forward(), frustum, m_terrain, m_grassDraws,
                                m_grassInstances);
            } else {
                m_grassDraws.clear();
                m_grassInstances.clear();
            }
            params.grass = m_grassDraws;
            params.grassInstances = m_grassInstances;
            params.grassFadeStart = m_grass.fadeStart();
            params.grassFadeEnd = m_grass.fadeEnd();
            params.doodadBoneCount = m_objects.doodadBoneCount(); // phase 14
            const Clock::time_point finishStart = Clock::now();
            m_actors.finishCollect(m_jobs, m_actorFrame);
            if (m_traceEvents) { // development: VD_TRACE_EVENTS=1
                for (const Actors::EventHit& e : m_actors.events()) {
                    VD_LOG_INFO("Event %c%c%c%c (data %u) on actor %u at (%.2f, %.2f, %.2f)", static_cast<char>(e.id),
                                static_cast<char>(e.id >> 8), static_cast<char>(e.id >> 16), static_cast<char>(e.id >> 24),
                                e.data, e.actor, e.position.x, e.position.y, e.position.z);
                }
            }
            // Parity: the unit frames' new portraits, posed after the world's actors (their bones go after).
            m_portraitActors.clear();
            m_portraitDraws.clear();
            m_portraits.render(m_actors, m_renderer, m_actorFrame, m_portraitActors, m_portraitDraws);
            m_modelViewDraws.clear(); // phase 15: the model frames shown, posed the same way (their actors after)
            m_modelViews.render(m_actors, m_renderer, m_actorFrame, m_portraitActors, m_modelViewDraws);
            params.portraits = m_portraitDraws;
            params.portraitActors = m_portraitActors;
            params.modelViews = m_modelViewDraws;
            actorsMs += secondsBetween(finishStart, Clock::now()) * 1000.0;
            addSection(kSectionActors, actorsMs);
            // Phase 9: texture animations, and the particles of whatever was placed (doodads here, actors above).
            const Clock::time_point effectsStart = Clock::now();
            m_uvTransforms.clear();
            if (m_effects.enabled) {
                m_objects.collectUvTransforms(nowMs, m_modelDraws, m_uvTransforms);
            }
            m_objects.collectSelfLights(nowMs, m_modelDraws); // phase 11: braziers' and campfires' own fire
            m_effects.placeDoodads(m_objects, nowMs);
            {
                // Phase 14 (bodyfx): the bodies drawn this frame, their steps and breaths (the effects they spawn
                // show from the next frame).
                UnitBodyEffects::Frame body;
                body.actors = &m_actors;
                body.effects = &m_effects;
                body.terrain = &m_terrain;
                body.collision = &m_collision;
                body.areas = &m_areaData;
                body.camera = m_camera.position;
                body.playerActor = m_player.active() && !m_player.online()
                                       ? m_player.localActor()
                                       : m_units.actorOf(m_session.inWorld() ? m_session.playerGuid() : 0);
                body.dt = dt;
                m_bodyFx.update(body);
            }
            params.particleQuads = m_effects.update(dt, m_camera, frustum, static_cast<f32>(render.height), m_jobs,
                                                    m_renderer, m_particleDraws);
            m_decalDraws.clear();
            params.particleQuads = m_bodyFx.writeQuads(params.particleQuads, m_camera.position,
                                                       m_renderer.particleVertices(), &Game::bodyTexture, this,
                                                       m_decalDraws, m_particleDraws); // phase 14 (bodyfx)
            params.groundDecals = m_decalDraws;
            params.particleQuads =
                addSelectionCircles(params.particleQuads, nowMs); // parity: in the particle pass
            params.particleQuads =
                addSpellBeams(params.particleQuads, static_cast<f32>(render.height)); // phase 14: beams
            params.particles = m_particleDraws;
            params.uvTransforms = m_uvTransforms;
            const f64 effectsMs = secondsBetween(effectsStart, Clock::now()) * 1000.0;
            addSection(kSectionEffects, effectsMs);
            actorsMs += effectsMs; // not world culling (kSectionCollect)
            params.actors = m_actorFrame.draws;
            params.actorInstances = m_actorFrame.instances;
            params.actorGeosets = m_actorFrame.geosets;
            params.actorColors = m_actorFrame.colors;
            params.actorUvTransforms = m_actorFrame.uvTransforms;
            params.boneCount = m_actorFrame.boneCount;
            params.terrain = m_terrainDraws;
            params.horizon = m_horizonDraws;
            addSection(kSectionCollect, secondsBetween(collectStart, Clock::now()) * 1000.0 - actorsMs); // world only
            if (debugUi && !m_gameHud.ready() && (m_session.transferring() || (m_player.active() && m_player.waiting()))) {
                drawLoadingScreen(static_cast<f32>(render.width), static_cast<f32>(render.height));
            }
            if (debugUi && (!m_hideDebugUi || !m_gameHud.ready())) { // wire boxes: a development view
                m_session.drawEntities(params.viewProj, m_camera.position, static_cast<f32>(render.width),
                                       static_cast<f32>(render.height), nowMs, &Units::hasModelCallback, &m_units);
                if (!m_gameHud.ready()) {
                    m_units.drawNamePlates(m_session, m_actors, params.viewProj, m_camera.position,
                                           static_cast<f32>(render.width), static_cast<f32>(render.height));
                }
            }
        }
        const Clock::time_point renderStart = Clock::now();
        if (!m_renderer.renderFrame(params)) {
            // Nothing presented (swapchain being rebuilt, zero-sized window): don't spin the CPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        addSection(kSectionRecord, std::max(0.0, secondsBetween(renderStart, Clock::now()) * 1000.0 -
                                                     static_cast<f64>(m_renderer.stats().blockedMs)));
        updateFrameStats(Clock::now());
    }
    VD_PROFILE_FRAME();
}

// Phase 18: the glue scene (app/GlueStage) as the frame's world: its actors (the background model and the character)
// with their particles, under the scene's light and fog.
void Game::collectGlue(FrameParams& params, f32 dt, u32 nowMs) {
    VD_PROFILE_ZONE("Game::collectGlue");
    const RenderStats& render = m_renderer.stats();
    const f32 aspect = render.height > 0 ? static_cast<f32>(render.width) / static_cast<f32>(render.height) : 1.0f;
    params.lighting = m_glueStage.lighting();
    params.sky.dome = true; // black where the scene's own sky does not reach (GlueParent's background)
    params.viewProj = m_camera.projection(aspect) * m_camera.view();
    const Frustum frustum = Frustum::fromViewProj(params.viewProj);
    const Clock::time_point actorsStart = Clock::now();
    m_actors.beginCollect(m_camera.position, frustum, m_camera.forward(), dt, nowMs, m_jobs, m_renderer, m_actorFrame);
    m_actors.finishCollect(m_jobs, m_actorFrame);
    addSection(kSectionActors, secondsBetween(actorsStart, Clock::now()) * 1000.0);
    const Clock::time_point effectsStart = Clock::now();
    m_uvTransforms.clear();
    params.particleQuads = m_effects.update(dt, m_camera, frustum, static_cast<f32>(render.height), m_jobs, m_renderer,
                                            m_particleDraws);
    params.particles = m_particleDraws;
    params.uvTransforms = m_uvTransforms;
    addSection(kSectionEffects, secondsBetween(effectsStart, Clock::now()) * 1000.0);
    params.actors = m_actorFrame.draws;
    params.actorInstances = m_actorFrame.instances;
    params.actorGeosets = m_actorFrame.geosets;
    params.actorColors = m_actorFrame.colors;
    params.actorUvTransforms = m_actorFrame.uvTransforms;
    params.boneCount = m_actorFrame.boneCount;
}

void Game::addSection(Section section, f64 ms) {
    m_sectionSum[section] += ms;
    m_sectionMax[section] = std::max(m_sectionMax[section], static_cast<f32>(ms));
    m_sectionFrame[section] = static_cast<f32>(ms);
}

void Game::updateFrameStats(Clock::time_point now) {
    const f64 frameSeconds = secondsBetween(m_lastFrame, now);
    m_lastFrame = now;
    ++m_statsFrames;
    const RenderStats& render = m_renderer.stats();
    m_statsGpuMs += render.gpuFrameMs;
    const f32 frameMs = static_cast<f32>(frameSeconds * 1000.0);
    const f32 workMs = std::max(0.0f, frameMs - render.blockedMs);
    m_performanceHint.reportActual(static_cast<i64>(static_cast<f64>(workMs) * 1.0e6),
                                   static_cast<i64>(static_cast<f64>(render.gpuFrameMs) * 1.0e6));
    {
        // Phase 10: the dynamic quality learns from this frame (not while loading: those frames say nothing).
        GovernorInput input;
        input.dt = static_cast<f32>(frameSeconds);
        input.frameMs = frameMs;
        input.cpuMs = workMs;
        input.gpuMs = render.gpuFrameMs;
        input.targetMs = 1000.0f / 60.0f * static_cast<f32>(m_frameInterval);
        input.thermalHeadroom = m_device.thermalHeadroom;
        input.thermalStatus = m_device.thermalStatus;
        input.measuring = m_terrain.isOpen() && !m_loadingShown && !m_pendingWorld &&
                          !(m_benchmark.running() && m_benchmark.warmingUp()) &&
                          secondsBetween(m_startTime, now) > 5.0;
        m_governor.update(input);
    }
    m_hud.addFrame(frameMs, workMs, render.gpuFrameMs);
    if (m_benchmark.running() && m_benchmark.looping() && !m_benchmark.warmingUp()) {
        // Phase 10 (soak): the battery at the start, and what the dynamic quality did along the way.
        if (!m_soak.started) {
            m_soak = {};
            m_soak.report.reserve(64 * 512);
            m_soak.started = true;
            m_soak.start = m_device;
            m_soak.startTime = now;
        }
        m_soak.minScale = std::min(m_soak.minScale, render.renderScale);
        m_soak.scaleSum += render.renderScale;
        ++m_soak.frames;
        m_soak.maxThermalLevel = std::max(m_soak.maxThermalLevel, m_governor.output().thermalLevel);
        if (!std::isnan(m_device.thermalHeadroom)) {
            m_soak.maxHeadroom = std::max(m_soak.maxHeadroom, m_device.thermalHeadroom);
        }
    }
    m_benchmark.record({frameMs, std::max(0.0f, frameMs - render.blockedMs), render.gpuFrameMs, render.drawCalls,
                        render.triangles});
    m_walkTest.record(frameMs, workMs, render.gpuFrameMs, m_sectionFrame[kSectionPlayer]);
    m_travelTest.record(frameMs); // phase 18 (world)
    if (BenchmarkMinute minute; m_benchmark.takeMinute(minute)) {
        logSoakMinute(minute);
    }
    if (frameSeconds > m_statsWorstFrame) {
        m_statsWorstFrame = frameSeconds;
    }
    // A frame that missed a vsync by far: what the main thread did, so single hitches can be traced.
    if (frameMs > kHitchMs) {
        VD_LOG_WARN("Hitch %.1f ms: terrain %.2f, objects %.2f, actors %.2f, collect %.2f, record %.2f, player %.2f, "
                    "network %.2f, blocked %.2f, GPU %.2f | %u draws, %u tris",
                    frameMs, m_sectionFrame[kSectionTerrain], m_sectionFrame[kSectionObjects],
                    m_sectionFrame[kSectionActors], m_sectionFrame[kSectionCollect], m_sectionFrame[kSectionRecord],
                    m_sectionFrame[kSectionPlayer], m_sectionFrame[kSectionNetwork], render.blockedMs,
                    render.gpuFrameMs, render.drawCalls, render.triangles);
    }
    m_sectionFrame = {};

    const f64 windowSeconds = secondsBetween(m_statsWindowStart, now);
    if (windowSeconds >= kStatsWindowSeconds) {
        const TerrainStats& terrain = m_terrain.stats();
        const ObjectStats& objects = m_objects.stats();
        char session[160];
        m_session.describe(session, sizeof(session));
        const ActorStats& actors = m_actors.stats();
        VD_LOG_INFO("%.1f fps avg | worst frame %.2f ms | GPU %.2f ms avg | %ux%u | %u draws, %u tris "
                    "(WMO %u/%uk, M2 %u/%uk, actors %u/%uk) | "
                    "%u tiles (%u streamed, upload worst %.2f ms, IO %.1f ms) | objects %u/%u in %u models, "
                    "%u pending, upload worst %.2f ms | actors %u/%u visible, %u sampled, %u bones, %u pending, "
                    "sampling %.2f ms | "
                    "cpu ms avg/max: terrain %.2f/%.2f, objects %.2f/%.2f, actors %.2f/%.2f, "
                    "collect %.2f/%.2f, record %.2f/%.2f, player %.2f/%.2f, ui %.2f/%.2f (%u quads, %u draws) | net: %s",
                    m_statsFrames / windowSeconds, m_statsWorstFrame * 1000.0, m_statsGpuMs / m_statsFrames,
                    render.width, render.height, render.drawCalls, render.triangles, render.wmoDrawCalls,
                    render.wmoTriangles / 1000, render.m2DrawCalls, render.m2Triangles / 1000, render.actorDrawCalls,
                    render.actorTriangles / 1000, terrain.loadedTiles,
                    terrain.tilesStreamed, terrain.worstUploadMs, terrain.lastIoMs, objects.visibleInstances,
                    objects.instances, objects.modelsDrawn, objects.pendingAssets, objects.worstUploadMs,
                    actors.visible, actors.actors, actors.sampled, actors.bones, actors.pendingAssets, actors.sampleMs,
                    m_sectionSum[kSectionTerrain] / m_statsFrames, m_sectionMax[kSectionTerrain],
                    m_sectionSum[kSectionObjects] / m_statsFrames, m_sectionMax[kSectionObjects],
                    m_sectionSum[kSectionActors] / m_statsFrames, m_sectionMax[kSectionActors],
                    m_sectionSum[kSectionCollect] / m_statsFrames, m_sectionMax[kSectionCollect],
                    m_sectionSum[kSectionRecord] / m_statsFrames, m_sectionMax[kSectionRecord],
                    m_sectionSum[kSectionPlayer] / m_statsFrames, m_sectionMax[kSectionPlayer],
                    m_sectionSum[kSectionUi] / m_statsFrames, m_sectionMax[kSectionUi], render.uiQuads,
                    render.uiDrawCalls, session);
        logDevice(); // phase 10: quality, temperature, battery and memory
        const fx::ParticleStats& fx = m_effects.particles().stats();
        if (VD_DEV_ENV("VD_TRACE_FX")) {
            // Development: the effect doodads in view, to aim the camera at them.
            for (u32 instance : m_objects.visibleEffects()) {
                const f32* m = m_objects.instanceTransform(instance);
                const std::string_view name = m_objects.instanceModelName(instance);
                VD_LOG_INFO("fx doodad %.1f,%.1f,%.1f (%.0f yd) %.*s", m[3], m[7], m[11],
                            glm::length(glm::dvec3(m[3], m[7], m[11]) - m_camera.position),
                            static_cast<int>(name.size()), name.data());
            }
        }
        VD_LOG_INFO("fx: cpu %.2f/%.2f ms avg/max (last update %.3f: prepare %.3f, simulate %.3f) | %u instances "
                    "(%u doodads), %u emitters + %u ribbons run, %u culled, %u particles, %u quads in %u draws, "
                    "rate x%.2f, %u starved, %u clipped",
                    m_sectionSum[kSectionEffects] / m_statsFrames, m_sectionMax[kSectionEffects], fx.updateMs,
                    fx.prepareMs, fx.simulateMs, fx.instances,
                    m_effects.doodadInstances(), fx.emitters, fx.ribbons, fx.culled, fx.particles, render.particleQuads,
                    render.particleDrawCalls, fx.rateScale, fx.starved, fx.clipped);
        const SpellVisuals::Stats& spells = m_spellEffects.visuals().stats();
        const Units::CombatStats& combat = m_units.combatStats();
        VD_LOG_INFO("spells: cpu %.3f/%.3f ms avg/max | %u effects, %u casts/channels, %u missiles, %u/%u states, "
                    "%u areas, %u kits, %u skipped | combat: %u swings, %u reactions, %u emotes",
                    m_sectionSum[kSectionSpells] / m_statsFrames, m_sectionMax[kSectionSpells], spells.effects,
                    spells.holds, spells.missiles, spells.shownAuras, spells.auras, spells.areas, spells.kits,
                    spells.skipped, combat.swings, combat.reactions, combat.emotes);
        if (m_fight.active()) {
            const SpellVisuals::Stats& fight = m_fight.visuals().stats();
            VD_LOG_INFO("fight: %u fighters, %u swings, %u casts, %u deaths | %u casts/channels, %u missiles, %u/%u "
                        "states, %u areas, %u kits, %u skipped | last frame %.3f ms (fight) + %.3f ms (visuals)",
                        m_fight.size(), m_fight.stats().swings, m_fight.stats().casts, m_fight.stats().deaths,
                        fight.holds, fight.missiles, fight.shownAuras, fight.auras, fight.areas, fight.kits,
                        fight.skipped, static_cast<f64>(m_fight.stats().updateMs), static_cast<f64>(fight.updateMs));
        }
        logSpellFx(); // phase 14
        if (VD_DEV_ENV("VD_WORLD_TRACE_PORTALS")) { // phase 18 (world): where the camera is, what its portals show
            const ObjectStats& o = m_objects.stats();
            VD_LOG_INFO("portals: camera %s (group %u), outdoors %s, %u groups reached, %u portals tested, %u interior "
                        "groups culled, %u/%u groups drawn | locate + walk %.3f ms",
                        o.cameraInside ? "inside" : "outside", o.cameraInside ? o.cameraGroup : 0u,
                        o.outdoorsVisible ? "seen" : "hidden", o.portalWalkGroups, o.portalsTested,
                        o.portalCulledGroups, o.visibleGroups, o.totalGroups, static_cast<f64>(o.locateMs));
            if (VD_DEV_ENV("VD_WORLD_TRACE_PORTALS")[0] == '2') {
                m_objects.traceCulledDoodads(m_camera.position, 60.0f);
            }
        }
        if (const char* compare = VD_DEV_ENV("VD_WORLD_COMPARE_GROUPS"); compare && !m_groupsCompared &&
            secondsBetween(m_startTime, now) > 15.0) { // development: once the buildings around have loaded
            m_groupsCompared = true;
            m_objects.compareGroupRules(m_camera.position, static_cast<f32>(std::atof(compare)), 40);
        }
        if (const char* probe = VD_DEV_ENV("VD_WORLD_PROBE_ROOMS"); probe && !m_roomsProbed &&
            secondsBetween(m_startTime, now) > 15.0) {
            m_roomsProbed = true;
            probeRooms(static_cast<f32>(std::atof(probe)));
        }
        if (const char* check = VD_DEV_ENV("VD_WORLD_CHECK_ROOMS"); check && !m_roomsChecked &&
            secondsBetween(m_startTime, now) > 25.0) {
            m_roomsChecked = true;
            checkRooms(static_cast<f32>(std::atof(check)));
        }
        m_sectionSum = {};
        m_sectionMax = {};
        m_terrain.resetWorst();
        m_objects.resetWorst();
        m_actors.resetWorst();
        m_statsWindowStart = now;
        m_statsFrames = 0;
        m_statsWorstFrame = 0.0;
        m_statsGpuMs = 0.0;
    }
}

const WmoAreaHit& Game::roomAt(const glm::dvec3& feet) {
    static const WmoAreaHit kOutside{};
    if (m_objects.isOpen()) {
        if (const WorldObjects::Standing& standing = m_objects.standingAt(feet); standing.bsp) {
            return standing.placed
                       ? m_wmoAreas.inGroup(standing.uniqueId, standing.location.group, standing.groupCount)
                       : kOutside;
        }
    }
    return m_wmoAreas.at(feet, m_terrain.heightAt(feet.x, feet.y));
}

void Game::checkRooms(f32 radius) {
    const glm::dvec3 center = m_player.active() ? m_player.character().position() : m_camera.position;
    std::vector<WorldObjects::FloorSample> samples;
    m_objects.floorSamples(center, radius, 3, samples);
    struct Tally {
        u32 samples = 0, minimap = 0, minimapBefore = 0, rooms = 0, room = 0, roomBefore = 0, reported = 0, covered = 0;
    };
    std::map<std::string, Tally> byModel;
    Tally total;
    for (const WorldObjects::FloorSample& s : samples) {
        std::string_view name = m_objects.instanceModelName(s.instance);
        name = name.substr(name.rfind('/') == std::string_view::npos ? 0 : name.rfind('/') + 1);
        Tally& t = byModel[std::string(name)];
        ++t.samples;
        const WorldObjects::Standing now = m_objects.standingNow(s.feet);
        const WorldObjects::Location before =
            m_objects.locate(s.feet, terrainBelow(this, s.feet.x, s.feet.y, s.feet.z));
        t.minimap += now.interior ? 1u : 0u;
        t.minimapBefore += m_objects.trueInterior(before) ? 1u : 0u;
        // WMOAreaTable's room for the floor it stands on (a copy: inGroup() keeps one answer).
        const WmoAreaHit expected = m_wmoAreas.inGroup(s.uniqueId, s.group, s.groupCount);
        if (expected.valid()) {
            ++t.rooms;
            const WmoAreaHit got = now.placed ? m_wmoAreas.inGroup(now.uniqueId, now.location.group, now.groupCount)
                                              : WmoAreaHit{};
            t.room += got.valid() && got.name == expected.name ? 1u : 0u;
            const WmoAreaHit baked = m_wmoAreas.locate(s.feet, m_terrain.heightAt(s.feet.x, s.feet.y));
            t.roomBefore += baked.valid() && baked.name == expected.name ? 1u : 0u;
        }
        if (!now.interior) {
            // Another walkable surface just over it (a walkway, a doubled floor, a crate): no place to stand.
            const std::optional<f64> over =
                m_collision.groundHeight(s.feet.x, s.feet.y, s.feet.z + 2.0, s.feet.z + 0.1);
            t.covered += over ? 1u : 0u;
            if (t.reported++ < 10) {
                const u32 flags = m_objects.groupFlags(now.location);
                // Where a character there would stand: the highest walkable surface under a yard over the point.
                const std::optional<f64> stand =
                    m_collision.groundHeight(s.feet.x, s.feet.y, s.feet.z + 1.0, s.feet.z - 1.0);
                VD_LOG_INFO("rooms check: %.*s group %u at %.2f,%.2f,%.2f: %s %d (flags 0x%x), stands %+.3f yd%s",
                            static_cast<int>(name.size()), name.data(), s.group, s.feet.x, s.feet.y, s.feet.z,
                            now.location.inside() ? "found group" : "outdoors",
                            now.location.inside() ? static_cast<i32>(now.location.group) : -1, flags,
                            stand ? *stand - s.feet.z : -99.0, over ? " | covered by a surface over it" : "");
            }
        }
    }
    for (const auto& [name, t] : byModel) {
        VD_LOG_INFO("rooms check: %-44s %5u floors: minimap %5u (before %5u, misses covered %u) | %5u with a room: "
                    "right %5u (before %5u)",
                    name.c_str(), t.samples, t.minimap, t.minimapBefore, t.covered, t.rooms, t.room, t.roomBefore);
        total.samples += t.samples;
        total.minimap += t.minimap;
        total.minimapBefore += t.minimapBefore;
        total.rooms += t.rooms;
        total.room += t.room;
        total.roomBefore += t.roomBefore;
    }
    VD_LOG_INFO("rooms check around %.0f,%.0f,%.0f (%.0f yd): %u floors of %zu building models: minimap indoors %u "
                "(before %u) | %u with a room: right %u (before %u)",
                center.x, center.y, center.z, static_cast<f64>(radius), total.samples, byModel.size(), total.minimap,
                total.minimapBefore, total.rooms, total.room, total.roomBefore);
}

void Game::probeRooms(f32 radius) {
    // Development: a map of the floors around the player, one character per yard (north up, west left): the minimap's
    // indoors and the room (WMOAreaTable) as a character standing there gets them, and the room by the floors baked in
    // worldsound.vdpk (the rule without the BSP).
    const glm::dvec3 center = m_player.active() ? m_player.character().position() : m_camera.position;
    const i32 r = static_cast<i32>(radius);
    VD_LOG_INFO("rooms around %.2f,%.2f,%.2f: [minimap | room | room by the baked floors] I interior, g in a "
                "building, . outdoors, R room, r room without a name, t the terrain says outdoors",
                center.x, center.y, center.z);
    std::string a, b, c;
    for (i32 ix = r; ix >= -r; --ix) {
        a.clear();
        b.clear();
        c.clear();
        for (i32 iy = r; iy >= -r; --iy) {
            const f64 x = center.x + ix, y = center.y + iy;
            const std::optional<f64> ground = m_collision.groundHeight(x, y, center.z + 2.0, center.z - 3.0);
            if (!ground) {
                a += ' ';
                b += ' ';
                c += ' ';
                continue;
            }
            const glm::dvec3 feet(x, y, *ground);
            const WorldObjects::Standing& standing = m_objects.standingAt(feet);
            a += standing.interior ? 'I' : standing.location.inside() ? 'g' : '.';
            const WmoAreaHit& room = roomAt(feet);
            b += room.valid() ? (room.name.empty() ? 'r' : 'R') : '.';
            const WmoAreaHit baked = m_wmoAreas.locate(feet, m_terrain.heightAt(x, y));
            c += baked.valid() ? (baked.name.empty() ? 'r' : 'R')
                               : m_wmoAreas.locate(feet, std::nullopt).valid() ? 't' : '.';
        }
        VD_LOG_INFO("rooms %+4d %s | %s | %s", ix, a.c_str(), b.c_str(), c.c_str());
    }
    const WmoAreaHit& room = roomAt(center);
    VD_LOG_INFO("rooms at the player: '%.*s' (WMO %d, group %d, area %u)", static_cast<int>(room.name.size()),
                room.name.data(), room.wmoId, room.groupId, room.areaId);
}

} // namespace vd
