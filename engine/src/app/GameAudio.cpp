#include "app/GameAudio.h"

#include "app/GameAudioInternal.h"
#include "audio/SoundRules.h"
#include "core/Clock.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/Gameplay.h"
#include "game/UnitInfo.h"
#include "net/ExtrasMessages.h" // phase 18 (extras)
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace vd {
namespace {

using namespace gameaudio;
using audio::Category;
namespace uf = net::uf;

// FrameXML's PlaySound names, in UiSound order.
constexpr const char* kUiSoundNames[] = {
    "igMainMenuOpen",         "igMainMenuQuit",          "igMainMenuClose",
    "igMainMenuOption",       "igMainMenuLogout",        "igQuestLogOpen",
    "igQuestLogClose",        "igQuestLogAbandonQuest",  "igQuestListOpen",
    "igQuestListClose",       "igQuestListSelect",       "igQuestListComplete",
    "igQuestCancel",          "QUESTADDED",              "igQuestFailed",
    "LOOTWINDOWOPENEMPTY",    "LOOTWINDOWCOINSOUND",     "FISHING REEL IN",
    "igCreatureAggroSelect",  "igCharacterNPCSelect",    "igCreatureNeutralSelect",
    "INTERFACESOUND_LOSTTARGETUNIT", "igMainMenuOptionCheckBoxOn", "igMainMenuOptionCheckBoxOff",
    "igCharacterInfoOpen",    "igCharacterInfoClose",    "igCharacterInfoTab",
    "igBackPackOpen",         "igBackPackClose",         "igSpellBookOpen",
    "igSpellBookClose",       "igAbiliityPageTurn",      "TalentScreenOpen",
    "TalentScreenClose",      "AuctionWindowOpen",       "AuctionWindowClose",
    "ITEM_REPAIR",            "TellMessage",             "MapPing",
    "UChatScrollButton",      "LEVELUPSOUND",            "GLUEENTERWORLDBUTTON",
    "igMiniMapZoomIn",        "igMiniMapZoomOut",        "igMiniMapOpen",
    "igMiniMapClose",         "igPlayerInvite",          "TaxiNodeDiscovered",
    "RaidWarning",            "INTERFACESOUND_CURSORGRABOBJECT", "INTERFACESOUND_CURSORDROPOBJECT",
    "igInventoryRotateCharacter",
    "igAbilityOpen",          "igAbilityClose", // phase 15 (pets)
    "KeyRingOpen",            "KeyRingClose",            "gsTitleOptionOK",
    "BAGMENUBUTTONPRESS", // phase 15 (items)
    "ReadyCheck", // phase 16 (alerts)
    "PVPENTERQUEUE",          "PVPTHROUGHQUEUE", // phase 17 (queue)
    "gsCharacterCreationClass", "gsCharacterCreationLook", "gsCharacterCreationCreateChar",
    "gsCharacterCreationCancel", // phase 18 (create)
    // Phase 18 (glue).
    "gsLogin",                "gsTitleQuit",             "gsTitleOptionExit",
    "gsLoginChangeRealmSelect", "gsLoginChangeRealmOK",  "gsLoginChangeRealmCancel",
    "gsCharacterSelectionEnterWorld", "gsCharacterSelectionExit", "gsCharacterSelectionDelCharacter",
    "gsCharacterSelectionCreateNew",
    "TutorialPopup", // phase 18 (p3)
    "igBonusBarOpen"}; // phase 20 (smart)
static_assert(std::size(kUiSoundNames) == static_cast<usize>(UiSound::Count), "one name per UiSound");

// The login screen's music: GlueXML plays it by file name, no SoundEntries row names it (baked explicitly).
constexpr const char* kLoginMusic = "Sound\\Music\\GlueScreenMusic\\wow_main_theme.mp3";

constexpr f32 kMusicFade = 3.0f; // cross-fade when the zone changes
constexpr f32 kAmbienceFade = 2.0f;
constexpr u32 kDefaultSilenceMs = 60000;

constexpr u32 kHumanFootsteps = 7; // FootstepTerrainLookup id of most characters (offline: no display)
constexpr u32 kTerrainDirt = 1;    // TerrainType.SoundID of dirt (the fallback)
constexpr u32 kTerrainStone = 3;   // and of stone: buildings, bridges, city streets
// Ours (NPC voices): an NPC window that closed with its NPC this near was closed by hand or by the server (they close
// themselves past 10 yards): its farewell.
constexpr f32 kNpcFarewellReach = 9.5f;

bool isNight(f32 hours) {
    return hours < 6.0f || hours >= 21.0f;
}

} // namespace

void GameAudio::init(audio::AudioSystem* audio, const audio::SoundData* data) {
    m_audio = audio;
    m_data = data;
    m_trace = VD_DEV_ENV("VD_TRACE_AUDIO") != nullptr;
    if (!data || !data->ready()) {
        return;
    }
    for (u32 i = 0; i < static_cast<u32>(UiSound::Count); ++i) {
        m_ui[i] = data->entryByName(kUiSoundNames[i]);
        if (!m_ui[i]) {
            VD_LOG_WARN("Audio: no SoundEntries row named %s", kUiSoundNames[i]);
        }
    }
    m_loginMusic = assetPathHash(kLoginMusic);
    m_random ^= static_cast<u32>(Clock::now().time_since_epoch().count());
    initParity();
}

u32 GameAudio::random() {
    // xorshift32: varied picks without the C library's state.
    m_random ^= m_random << 13;
    m_random ^= m_random >> 17;
    m_random ^= m_random << 5;
    return m_random;
}

void GameAudio::trace(const char* what, u32 soundEntry) {
    Recent& r = m_recent[m_recentNext++ % m_recent.size()];
    r.entry = soundEntry;
    r.atMs = m_nowMs;
    std::snprintf(r.what.data(), r.what.size(), "%s", what);
    if (m_trace) {
        VD_LOG_INFO("Audio: %s (%u)", what, soundEntry);
    }
}

audio::VoiceHandle GameAudio::playSound(u32 soundEntry, Category category, u8 priority, const glm::dvec3* at, bool loop,
                                   f32 fadeIn, f32 volume, f32 minDistance, f32 maxDistance, i32 file) {
    const audio::SoundEntry* e = soundEntry != 0 && m_data ? m_data->entry(soundEntry) : nullptr;
    if (!e || e->fileCount == 0 || !m_audio) {
        return {};
    }
    audio::SoundRef sound = m_audio->find(file >= 0 ? e->files[std::min(static_cast<u32>(file), e->fileCount - 1)]
                                                    : e->pick(random()));
    // A file the client lacks (GhostMusic01 and 02 of the ghost's three, a few more): another of the row's.
    for (u32 i = 0; !sound.valid() && i < e->fileCount; ++i) {
        sound = m_audio->find(e->files[i]);
    }
    if (!sound.valid()) {
        return {};
    }
    audio::PlayParams params;
    params.category = category;
    params.volume = e->volume * volume;
    params.loop = loop;
    params.fadeInSeconds = fadeIn;
    params.priority = priority;
    if (at) {
        params.positional = true;
        params.position = *at;
        params.minDistance = std::max(minDistance > 0.0f ? minDistance : e->minDistance, 1.0f);
        params.maxDistance = std::max(maxDistance > 0.0f ? maxDistance : e->maxDistance, params.minDistance + 1.0f);
    }
    const audio::VoiceHandle voice = m_audio->play(sound, params);
    if (voice) {
        ++m_stats.played;
        char what[40];
        std::snprintf(what, sizeof(what), "%.*s", static_cast<int>(std::min<usize>(e->name.size(), 39)),
                      e->name.data());
        trace(what, soundEntry);
    }
    return voice;
}

void GameAudio::playUi(UiSound sound) {
    if (!active() || sound >= UiSound::Count) {
        return;
    }
    if (const audio::SoundEntry* e = m_ui[static_cast<u32>(sound)]) {
        playSound(e->id, Category::Interface, kPriorityUi);
    }
}

void GameAudio::update(const GameAudioFrame& f) {
    if (!active()) {
        return;
    }
    VD_PROFILE_ZONE("GameAudio::update");
    const Clock::time_point begin = Clock::now();
    m_nowMs = f.nowMs;
    m_audio->setListener(f.listener, f.listenerRight);
    if (m_audio->paused()) {
        m_wasPaused = true; // in the background nothing plays
        return;
    }
    if (m_wasPaused) {
        // Back from the background: the voices were dropped; music and ambience start again at once.
        m_wasPaused = false;
        m_music = {};
        m_musicKind = MusicKind::None;
        m_silenceUntilMs = f.nowMs;
        m_ambience = {};
        m_ambienceEntry = 0;
        m_weather = {};
        m_weatherEntry = 0;
        m_loops = {};
        m_missiles = {};
        m_water = {};
        m_waterEntry = 0;
        for (ObjectMemory& o : m_objects) {
            o.loop = {};
        }
        resetModelSounds(); // phase 14
        m_interiorKey = ~0ull;
    }
    // Where a slow frame went (the log line below): ground, music, ambience, events, footsteps.
    std::array<f32, 5> parts{};
    Clock::time_point mark = begin;
    const auto lap = [&](u32 part) {
        const Clock::time_point now = Clock::now();
        parts[part] = static_cast<f32>(secondsBetween(mark, now) * 1000.0);
        mark = now;
    };
    m_ground.setMap(m_audio->pack(), f.world ? f.map : std::string_view{});
    m_ground.update();
    m_body = bodyOf(f);
    waterSounds(f); // before the ambience: under water it changes
    lap(0);
    const bool night = isNight(f.hours);
    updateMusic(f, night);
    lap(1);
    updateAmbience(f, night);
    weatherSounds(f); // phase 11
    lap(2);

    if (f.play && f.entities && f.play->inWorld()) {
        const game::Gameplay& play = *f.play;
        if (m_cursorsPlayer != f.player) {
            // A new world: the rings are read from now on.
            m_cursorsPlayer = f.player;
            m_eventCursor = play.events().end();
            m_fxCursor = play.fx().end();
            m_combatCursor = play.combatLog().end();
            m_chatCursor = play.chat().end();
            m_serverWindows = 0;
            m_target = play.target();
            m_dialogKind = static_cast<u8>(play.dialog().kind());
            m_dialogNpc = play.dialog().npc();
            m_lootOpen = play.loot().open();
            m_interactNpc = interactNpc(play);
            m_clickGuid = 0;
            m_clickCount = 0;
            m_npcVoices = {};
            for (Loop& loop : m_loops) {
                m_audio->stop(loop.voice);
                loop = {};
            }
            m_units = {};
            resetParity(f);
        }
        readEvents(f);
        readFx(f);
        readCombat(f);
        watchWindows(f);
        watchDeaths(f);
        updateLoops(f);
        updateMissiles(f);
        itemSounds(f);
        objectSounds(f);
        sheatheSounds(f);
        if (!f.scene || !m_modelSoundsOn) {
            creatureFidgets(f); // phase 14: with the actors' events, their $FD1-$FD4 and $FDX play the fidgets
        }
        socialSounds(f);
    } else if (m_cursorsPlayer != 0) {
        m_cursorsPlayer = 0;
        for (Loop& loop : m_loops) {
            m_audio->stop(loop.voice);
            loop = {};
        }
        for (Missile& missile : m_missiles) {
            m_audio->stop(missile.loop);
            missile = {};
        }
        for (ObjectMemory& o : m_objects) {
            m_audio->stop(o.loop, 0.5f);
            o = {};
        }
        m_items = {};
    }
    delayedSounds(f);
    lap(3);
    bodySounds(f);
    footsteps(f);
    modelSounds(f); // phase 14: doodads, terrain emitters, creature loops, the actors' animation events
    lap(4);

    m_stats.updateMs = static_cast<f32>(secondsBetween(begin, Clock::now()) * 1000.0);
    if (m_stats.updateMs > 3.0f) {
        VD_LOG_WARN("Audio hitch %.2f ms: ground %.2f, music %.2f, ambience %.2f, events %.2f, footsteps %.2f",
                    m_stats.updateMs, parts[0], parts[1], parts[2], parts[3], parts[4]);
    }
    m_stats.worstMs = std::max(m_stats.worstMs, m_stats.updateMs);
    m_stats.sumMs += m_stats.updateMs + m_audio->stats().updateMs;
    ++m_stats.frames;
    if (reached(f.nowMs, m_stats.windowStartMs + kStatsSeconds * 1000)) {
        const audio::AudioStats& s = m_audio->stats();
        VD_LOG_INFO("Audio: %u voices, mixer load %.2f%%, limiter %.1f dB | main thread %.3f ms avg (GameAudio worst %.3f) | %u played, "
                    "%u stolen, %u dropped, %u culled, %u missing | music %u, ambience %u, area %u | models %.3f ms avg: "
                    "%u doodads, %u loops, %u events, %u footsteps",
                    s.voices, s.audioLoad * 100.0f, s.limiterDb, m_stats.frames ? m_stats.sumMs / m_stats.frames : 0.0,
                    m_stats.worstMs, m_stats.played, s.stolen, s.dropped, s.culled, s.missing, m_musicEntry,
                    m_ambienceEntry, m_area,
                    m_modelStats.frames ? m_modelStats.sumMs / m_modelStats.frames : 0.0, m_modelStats.doodads,
                    m_modelStats.loops, m_modelStats.events, m_modelStats.footsteps);
        m_modelStats.sumMs = 0.0;
        m_modelStats.frames = 0;
        m_stats.sumMs = 0.0;
        m_stats.frames = 0;
        m_stats.worstMs = 0.0f;
        m_stats.windowStartMs = f.nowMs;
    }
}

// --- Music and ambience -------------------------------------------------------------------------------------------

void GameAudio::startMusic(u32 soundEntry, MusicKind kind, f32 fade) {
    if (!m_options.music) {
        return; // phase 15 (options): EnableMusic off (the server's SMSG_PLAY_MUSIC too)
    }
    const bool playing = m_audio->playing(m_music);
    m_audio->stop(m_music, fade);
    m_music = playSound(soundEntry, Category::Music, kPriorityMusic, nullptr, false, playing ? fade : 0.5f);
    m_musicKind = m_music ? kind : MusicKind::None;
    m_musicEntry = m_music ? soundEntry : 0;
}

void GameAudio::startMusicFile(u64 pathHash, MusicKind kind, f32 fade) {
    const audio::SoundRef sound = m_audio->find(pathHash);
    m_audio->stop(m_music, fade);
    m_music = {};
    if (sound.valid()) {
        audio::PlayParams params;
        params.category = Category::Music;
        params.priority = kPriorityMusic;
        params.loop = true;
        params.fadeInSeconds = fade;
        m_music = m_audio->play(sound, params);
        if (m_music) {
            trace("wow_main_theme", 0);
        }
    }
    m_musicKind = kind; // even when missing: not asked again every frame
    m_musicEntry = 0;
}

void GameAudio::updateMusic(const GameAudioFrame& f, bool night) {
    if (f.login) {
        // The login and character screens: the main theme, looping (phase 15: unless EnableMusic is off).
        if (m_musicKind != MusicKind::Login && m_options.music) {
            startMusicFile(m_loginMusic, MusicKind::Login, 1.0f);
        } else if (m_musicKind == MusicKind::Login && !m_options.music) {
            m_audio->stop(m_music, 1.0f);
            m_music = {};
            m_musicKind = MusicKind::None;
        }
        m_zoneSet = 0;
        m_area = 0;
        m_introArea = 0;
        m_ghost = false;
        return;
    }
    if (m_musicKind == MusicKind::Login) {
        m_audio->stop(m_music, 2.0f);
        m_music = {};
        m_musicKind = MusicKind::None;
        m_silenceUntilMs = f.nowMs; // into the world: its music at once
    }
    if (!f.world) {
        if (m_music) {
            m_audio->stop(m_music, 2.0f);
            m_music = {};
            m_musicKind = MusicKind::None;
        }
        m_zoneSet = 0;
        m_area = 0;
        return;
    }
    if (f.loading) {
        return; // whatever plays goes on; the zone is known once the ground is
    }
    if (!m_options.music) { // phase 15 (options): EnableMusic off: nothing plays, the zone is found again when back on
        if (m_music) {
            m_audio->stop(m_music, kMusicFade);
            m_music = {};
        }
        m_musicKind = MusicKind::None;
        m_musicEntry = 0;
        m_zoneSet = 0;
        m_area = 0;
        m_silenceUntilMs = f.nowMs;
        return;
    }
    if (m_musicKind != MusicKind::None && !m_audio->playing(m_music)) {
        // A track ended: the zone's silence before the next one (ZoneMusic's interval, day or night). Phase 15
        // (options): none with "Loop Music" (SoundZoneMusicNoDelay).
        const audio::ZoneMusicSet* set = m_data->zoneMusic(m_zoneSet);
        const u32 n = night ? 1 : 0;
        const u32 lo = m_options.musicLooping ? 0u : set ? set->silenceMinMs[n] : kDefaultSilenceMs;
        const u32 hi = m_options.musicLooping ? 0u : set ? std::max(lo, set->silenceMaxMs[n]) : lo;
        m_silenceUntilMs = f.nowMs + lo + (hi > lo ? random() % (hi - lo) : 0);
        m_music = {};
        m_musicKind = MusicKind::None;
        m_musicEntry = 0;
    }
    const bool ghost = m_body.ghost && m_ghostMusic != 0;
    if (f.area != 0 && (f.area != m_area || ghost != m_ghost || f.interiorKey != m_interiorKey)) {
        m_area = f.area;
        m_ghost = ghost;
        m_interiorKey = f.interiorKey;
        audio::AreaSound area = m_data->areaSound(f.area);
        // Phase 14: a building's room (WMOAreaTable) plays its own music and intro (a tavern's, Ironforge's).
        if (f.interiorMusic != 0) {
            area.music = f.interiorMusic;
        }
        if (f.interiorIntro != 0) {
            area.intro = f.interiorIntro;
        }
        if (ghost) {
            // A ghost hears its own music everywhere (ZoneMusic "GhostMusic (DONOTRENAME)"), no city intros.
            area.music = m_ghostMusic;
            area.intro = 0;
        }
        // Entering a city: its intro, at most every MinDelayMinutes.
        bool intro = false;
        if (area.intro != 0 && area.intro != m_introArea) {
            if (const audio::ZoneIntro* z = m_data->zoneIntro(area.intro)) {
                IntroPlayed* slot = &m_introsPlayed[0];
                for (IntroPlayed& p : m_introsPlayed) {
                    if (p.intro == area.intro) {
                        slot = &p;
                        break;
                    }
                    if (p.atMs < slot->atMs) {
                        slot = &p;
                    }
                }
                const bool due = slot->intro != area.intro || slot->atMs == 0 ||
                                 reached(f.nowMs, slot->atMs + z->minDelayMinutes * 60000u);
                if (due && m_musicKind != MusicKind::Server) {
                    startMusic(z->sound, MusicKind::Intro, kMusicFade);
                    *slot = {area.intro, std::max(1u, f.nowMs)};
                    intro = true;
                }
            }
        }
        m_introArea = area.intro;
        if (area.music != m_zoneSet) {
            m_zoneSet = area.music;
            // Another zone: its music right away (the last zone's fades out), unless an intro or the server's music
            // is playing.
            if (!intro && (m_musicKind == MusicKind::Zone || m_musicKind == MusicKind::None)) {
                if (m_zoneSet != 0) {
                    m_silenceUntilMs = f.nowMs;
                    m_musicKind = MusicKind::None; // started below
                    m_audio->stop(m_music, kMusicFade);
                    m_music = {};
                } else if (m_music) {
                    m_audio->stop(m_music, kMusicFade);
                    m_music = {};
                    m_musicKind = MusicKind::None;
                }
            }
        }
    }
    m_night = night;
    if (m_musicKind == MusicKind::None && m_zoneSet != 0 && reached(f.nowMs, m_silenceUntilMs)) {
        const audio::ZoneMusicSet* set = m_data->zoneMusic(m_zoneSet);
        const u32 entry = set ? set->sound[night ? 1 : 0] : 0;
        startMusic(entry != 0 ? entry : (set ? set->sound[0] : 0), MusicKind::Zone, kMusicFade);
        if (!m_music) {
            m_silenceUntilMs = f.nowMs + kDefaultSilenceMs; // nothing to play here: try again later
        }
    }
}

void GameAudio::updateAmbience(const GameAudioFrame& f, bool night) {
    if (!f.world || f.login || !m_options.ambience) { // phase 15 (options): EnableAmbience
        if (m_ambience) {
            m_audio->stop(m_ambience, 1.0f);
            m_ambience = {};
        }
        m_ambienceEntry = 0;
        return;
    }
    if (f.loading || f.area == 0) {
        return;
    }
    audio::AreaSound area = m_data->areaSound(f.area);
    // Phase 14: a building's room (WMOAreaTable) has its own ambience (the tavern's, the smithy's hammering).
    if (const std::array<u32, 2> room = f.interiorAmbience != 0 ? m_data->ambience(f.interiorAmbience)
                                                                : std::array<u32, 2>{};
        room[0] != 0 || room[1] != 0) {
        area.ambience = room;
    }
    u32 entry = area.ambience[night ? 1 : 0] != 0 ? area.ambience[night ? 1 : 0] : area.ambience[0];
    // Parity pass: the client's own ambiences for a ghost and for the camera under water.
    if (m_body.ghost && m_ghostSound != 0) {
        entry = m_ghostSound;
    }
    if (m_underwater && m_underwaterSound != 0) {
        entry = m_underwaterSound;
    }
    if (entry == m_ambienceEntry) {
        return;
    }
    // Another area or the night came: cross-fade the loops.
    m_audio->stop(m_ambience, kAmbienceFade);
    m_ambience = entry != 0 ? playSound(entry, Category::Ambience, kPriorityAmbience, nullptr, true, kAmbienceFade)
                            : audio::VoiceHandle{};
    m_ambienceEntry = entry;
}

// --- Online: events, spells, combat, windows ---------------------------------------------------------------------

bool GameAudio::positionOf(const GameAudioFrame& f, u64 guid, glm::dvec3& out) const {
    const game::Entity* e = guid != 0 ? f.entities->find(guid) : nullptr;
    if (!e || !e->hasPosition) {
        return false;
    }
    out = glm::dvec3(e->positionAt(f.nowMs));
    return true;
}

const audio::CreatureSounds* GameAudio::soundsOf(const game::Entity& unit) const {
    return game::isUnit(unit) ? m_data->creatureSounds(unit.value(uf::UNIT_FIELD_DISPLAYID)) : nullptr;
}

void GameAudio::weaponOf(const GameAudioFrame& f, const game::Entity& unit, bool offHand, u32& subclass,
                         bool& wooden) const {
    subclass = audio::SoundData::kUnarmed;
    wooden = false;
    if (unit.type == game::ObjectType::Player) {
        // The visible item of the hand (its template says the weapon class), asked for when unknown.
        const u32 entry = unit.value(offHand ? uf::PLAYER_VISIBLE_ITEM_17_0 : uf::PLAYER_VISIBLE_ITEM_16_0);
        if (entry != 0) {
            if (const net::ItemQueryInfo* item = f.play->queries().item(entry);
                item && item->itemClass == kItemClassWeapon) {
                subclass = item->subclass;
            }
        }
        return;
    }
    // Creatures: UNIT_VIRTUAL_ITEM_INFO, two fields per hand: class, subclass, material, inventory type.
    const u16 info = static_cast<u16>(uf::UNIT_VIRTUAL_ITEM_INFO + (offHand ? 2 : 0));
    if (unit.byteValue(info, 0) == kItemClassWeapon) {
        subclass = unit.byteValue(info, 1);
        wooden = unit.byteValue(info, 2) == kItemMaterialWood;
    }
}

bool GameAudio::unitCooldown(u64 guid, u32 kind, u32 nowMs, u32 intervalMs) {
    UnitMemory* slot = nullptr;
    UnitMemory* oldest = &m_units[0];
    for (UnitMemory& u : m_units) {
        if (u.guid == guid) {
            slot = &u;
            break;
        }
        const u32 last = *std::max_element(u.lastMs.begin(), u.lastMs.end());
        const u32 oldestLast = *std::max_element(oldest->lastMs.begin(), oldest->lastMs.end());
        if (u.guid == 0 || (oldest->guid != 0 && static_cast<i32>(last - oldestLast) < 0)) {
            oldest = &u;
        }
    }
    if (!slot) {
        slot = oldest;
        *slot = {};
        slot->guid = guid;
    }
    u32& last = slot->lastMs[kind];
    if (last != 0 && !reached(nowMs, last + intervalMs)) {
        return false;
    }
    last = std::max(1u, nowMs);
    return true;
}

void GameAudio::watchUnit(u64 guid, u32 nowMs) {
    // Watched for its death from now on (its cry, its body falling).
    unitCooldown(guid, kWatch, nowMs, 0);
    for (UnitMemory& u : m_units) {
        if (u.guid == guid) {
            u.alive = true;
        }
    }
}

void GameAudio::vocalError(const GameAudioFrame& f, u32 vocal) {
    if (!m_options.errorSpeech) {
        return; // phase 15 (options): EnableErrorSpeech
    }
    if (vocal == audio::vocal::kNone || (m_vocalAtMs != 0 && !reached(f.nowMs, m_vocalAtMs + 1500))) {
        return; // the character does not complain more than this
    }
    const game::Entity* me = f.entities->find(f.player);
    if (!me) {
        return;
    }
    const u32 race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
    const u32 gender = me->byteValue(uf::UNIT_FIELD_BYTES_0, 2);
    if (const u32 sound = m_data->vocalError(vocal, race, gender); sound != 0 && playSound(sound, Category::Voices, kPriorityUi)) {
        m_vocalAtMs = std::max(1u, f.nowMs);
    }
}

void GameAudio::readEvents(const GameAudioFrame& f) {
    const game::Gameplay& play = *f.play;
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        switch (e->kind) {
        case game::GameEventKind::LevelUp: playUi(UiSound::LevelUp); break;
        case game::GameEventKind::MoneyLooted: playUi(UiSound::LootCoins); break;
        case game::GameEventKind::QuestAccepted: playUi(UiSound::QuestAdded); break;
        case game::GameEventKind::QuestRewarded: playUi(UiSound::QuestComplete); break;
        case game::GameEventKind::QuestFailed: playUi(UiSound::QuestFailed); break;
        case game::GameEventKind::CastFailed:
            // The character says why (VocalUISounds), like the original client does for these reasons.
            vocalError(f, audio::vocalForCastFailure(net::spellCastResultName(static_cast<u8>(e->value[0])),
                                                     vocalPowerType(f)));
            break;
        case game::GameEventKind::SwingNotInRange: vocalError(f, audio::vocal::kTargetTooFar); break;
        case game::GameEventKind::InventoryError:
            vocalError(f, audio::vocalForInventoryError(net::inventoryResultName(static_cast<u8>(e->value[0]))));
            break;
        default: parityEvent(f, *e); break;
        }
    }
}

void GameAudio::readFx(const GameAudioFrame& f) {
    const game::Gameplay& play = *f.play;
    const u64 player = f.player;
    while (const game::FxEvent* e = play.fx().next(m_fxCursor)) {
        glm::dvec3 at;
        const u8 priority = e->guid == player || e->other == player ? kPriorityPlayer : kPriorityWorld;
        switch (e->kind) {
        case game::FxEventKind::SpellStart: {
            // A cast with a cast time: its precast loop while it lasts.
            const u32 precast = e->value[0] > 0 ? m_data->spellSounds(e->id).precast : 0;
            if (precast != 0 && positionOf(f, e->guid, at)) {
                Loop* free = nullptr;
                for (Loop& loop : m_loops) {
                    if (loop.guid == e->guid && !loop.channel) {
                        m_audio->stop(loop.voice); // a unit casts one spell at a time
                        loop = {};
                    }
                    if (!free && !m_audio->playing(loop.voice)) {
                        free = &loop;
                    }
                }
                if (free) {
                    *free = {e->guid, e->id, false, playSound(precast, Category::Effects, priority, &at, true)};
                }
            }
            break;
        }
        case game::FxEventKind::SpellGo:
        case game::FxEventKind::SpellInterrupted:
            for (Loop& loop : m_loops) {
                if (loop.guid == e->guid && !loop.channel) {
                    m_audio->stop(loop.voice, 0.1f);
                    loop = {};
                }
            }
            if (e->kind == game::FxEventKind::SpellGo && positionOf(f, e->guid, at)) {
                playSound(m_data->spellSounds(e->id).cast, Category::Effects, priority, &at);
            }
            break;
        case game::FxEventKind::SpellHit: {
            // A missile (Frostbolt, Fireball: Spell.dbc speed) flies first; the impact sounds when it arrives.
            const audio::SoundData::SpellSounds sounds = m_data->spellSounds(e->id);
            glm::dvec3 from;
            if (!positionOf(f, e->other, at)) {
                break;
            }
            const f64 distance = positionOf(f, e->guid, from) ? glm::length(at - from) : 0.0;
            Missile* slot = nullptr;
            for (Missile& m : m_missiles) {
                if (m.target == 0) {
                    slot = &m;
                    break;
                }
            }
            if (sounds.speed > 0.0f && distance > 1.0 && slot) {
                *slot = {e->other, from, f.nowMs,
                         f.nowMs + static_cast<u32>(distance / static_cast<f64>(sounds.speed) * 1000.0), sounds.impact,
                         priority, playSound(sounds.missile, Category::Effects, priority, &from, true)};
                if (slot->arriveMs == f.nowMs) {
                    ++slot->arriveMs;
                }
            } else {
                playSound(sounds.impact, Category::Effects, priority, &at);
            }
            break;
        }
        case game::FxEventKind::ChannelStart:
            if (const u32 channel = m_data->spellSounds(e->id).channel;
                channel != 0 && positionOf(f, e->guid, at)) {
                for (Loop& loop : m_loops) {
                    if (!m_audio->playing(loop.voice)) {
                        loop = {e->guid, e->id, true, playSound(channel, Category::Effects, kPriorityPlayer, &at, true)};
                        break;
                    }
                }
            }
            break;
        case game::FxEventKind::ChannelEnd:
            for (Loop& loop : m_loops) {
                if (loop.guid == e->guid && loop.channel) {
                    m_audio->stop(loop.voice, 0.2f);
                    loop = {};
                }
            }
            break;
        case game::FxEventKind::SpellVisual:
        case game::FxEventKind::SpellImpact:
            if (positionOf(f, e->guid, at)) {
                playSound(m_data->kitSound(e->id), Category::Effects, priority, &at);
            }
            break;
        case game::FxEventKind::Sound: playSound(e->id, Category::Effects, kPriorityUi); break;
        case game::FxEventKind::Music: startMusic(e->id, MusicKind::Server, 1.0f); break;
        case game::FxEventKind::ObjectSound:
            if (positionOf(f, e->guid, at)) {
                playSound(e->id, Category::Effects, kPriorityWorld, &at);
            }
            m_lastObjectSoundGuid = e->guid; // phase 13: a custom animation right after does not repeat it
            m_lastObjectSound = e->id;
            m_lastObjectSoundMs = f.nowMs;
            break;
        case game::FxEventKind::ObjectCustomAnim: {
            // Phase 13: the object's GameObjectDisplayInfo sound of Custom0..3 (the bobber's "Fishing Hooked"),
            // unless the server just played that very sound on it.
            const game::Entity* object = f.entities->find(e->guid);
            const audio::ObjectSounds* sounds =
                object && e->id < 4 ? m_data->objectSounds(object->value(uf::GAMEOBJECT_DISPLAYID)) : nullptr;
            const u32 sound = sounds ? sounds->custom[e->id] : 0;
            const bool played = m_lastObjectSoundGuid == e->guid && m_lastObjectSound == sound &&
                                f.nowMs - m_lastObjectSoundMs < 1000;
            if (sound != 0 && !played && positionOf(f, e->guid, at)) {
                playSound(sound, Category::Effects, kPriorityWorld, &at);
            }
            break;
        }
        case game::FxEventKind::TextEmote: {
            const game::Entity* unit = f.entities->find(e->guid);
            if (m_options.emoteSounds && unit && unit->type == game::ObjectType::Player && positionOf(f, e->guid, at)) {
                const u32 race = unit->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
                const u32 gender = unit->byteValue(uf::UNIT_FIELD_BYTES_0, 2);
                playSound(m_data->emoteSound(e->id, race, gender), Category::Voices, kPriorityVoice, &at);
            }
            break;
        }
        case game::FxEventKind::AiReaction: {
            // Phase 13: SMSG_AI_REACTION, what WoW.exe plays for it: a creature that noticed the player cries its
            // alert sound, one that attacks its aggro sound (CreatureSoundData). The server repeats the latter on
            // every Unit::Attack: once in a while per creature.
            const game::Entity* unit = f.entities->find(e->guid);
            if (unit && unit->type == game::ObjectType::Unit && unitCooldown(e->guid, kAggro, f.nowMs, 15000) &&
                positionOf(f, e->guid, at)) {
                if (const audio::CreatureSounds* sounds = soundsOf(*unit)) {
                    playSound(e->id == net::kAiReactionAlert ? sounds->alert : sounds->aggro, Category::Voices,
                              priority, &at);
                }
            }
            break;
        }
        case game::FxEventKind::PetTalk: {
            // Phase 18 (extras): SMSG_PET_ACTION_SOUND, what WoW.exe plays for it (0x6040C0 -> 0x623A40): the pet's
            // CreatureSoundData attack sound when told to attack, its order sound for a special spell.
            const game::Entity* unit = f.entities->find(e->guid);
            if (unit && positionOf(f, e->guid, at)) {
                if (const audio::CreatureSounds* sounds = soundsOf(*unit)) {
                    playSound(e->id == net::kPetTalkAttack ? sounds->petAttack : sounds->petOrder, Category::Voices,
                              priority, &at);
                }
            }
            break;
        }
        default: break;
        }
    }
}

void GameAudio::readCombat(const GameAudioFrame& f) {
    while (const game::CombatEvent* e = f.play->combatLog().next(m_combatCursor)) {
        if ((e->flags & game::CombatEvent::kEnvironment) != 0) {
            environmentDamage(f, *e); // falls, lava, drowning (parity pass)
        } else if (e->swing()) { // phase 13: a damage shield's hit is no swing
            meleeSwing(f, *e); // spells sound through their visuals
        }
    }
}

void GameAudio::meleeSwing(const GameAudioFrame& f, const game::CombatEvent& e) {
    const game::Entity* attacker = f.entities->find(e.attacker);
    const game::Entity* victim = f.entities->find(e.victim);
    glm::dvec3 victimAt, attackerAt;
    if (!attacker || !victim || !positionOf(f, e.victim, victimAt) || !positionOf(f, e.attacker, attackerAt)) {
        return;
    }
    const u64 player = f.player;
    const u8 priority = e.attacker == player || e.victim == player ? kPriorityPlayer : kPriorityWorld;
    const bool critical = (e.flags & game::CombatEvent::kCrit) != 0;
    u32 subclass = 0;
    bool wooden = false;
    weaponOf(f, *attacker, (e.flags & game::CombatEvent::kOffHand) != 0, subclass, wooden);
    const u32 swing = audio::SoundData::swingType(subclass);
    if (e.kind == game::CombatEvent::Kind::Miss) {
        // A miss: the client's own whoosh by the weapon's hands ("(DONOTRENAME)Combat Miss 1H / 2H").
        playSound(m_missSound[swing == 2 ? 1 : 0], Category::Effects, priority, &attackerAt);
        return;
    }
    // Every other swing whooshes (the attack animations' $CSS event: WeaponSwingSounds2 by weight, its critical row
    // on a critical hit), then sounds on what it met.
    playSound(m_data->weaponSwing(swing, critical), Category::Effects, priority, &attackerAt);
    switch (e.kind) {
    case game::CombatEvent::Kind::Damage: {
        // The weapon on the victim's body: its impact column (a creature's type, a player's body armor).
        const audio::CreatureSounds* hurt = soundsOf(*victim);
        u32 armor = 0;
        if (victim->type == game::ObjectType::Player) {
            if (const u32 chest = victim->value(uf::PLAYER_VISIBLE_ITEM_5_0); chest != 0) {
                if (const net::ItemQueryInfo* item = f.play->queries().item(chest);
                    item && item->itemClass == kItemClassArmor) {
                    armor = item->subclass;
                }
            }
        }
        const u32 column = audio::SoundData::impactColumn(hurt ? hurt->impactType : 0, armor);
        playSound(m_data->weaponImpact(subclass, column, critical), Category::Effects, priority, &victimAt);
        if (e.blocked > 0) {
            playSound(m_data->weaponImpact(subclass, 3, false), Category::Effects, priority, &victimAt);
        }
        const bool crushing = (e.flags & game::CombatEvent::kCrushing) != 0;
        if (hurt && unitCooldown(e.victim, kWound, f.nowMs, critical || crushing ? 400 : 2500)) {
            const u32 wound = crushing && hurt->injuryCrushingBlow ? hurt->injuryCrushingBlow
                              : critical && hurt->injuryCritical ? hurt->injuryCritical
                                                                 : hurt->injury;
            playSound(wound, Category::Voices, priority, &victimAt);
        }
        const audio::CreatureSounds* swinging = soundsOf(*attacker);
        if (swinging && (critical || random() % 3 == 0) && unitCooldown(e.attacker, kGrunt, f.nowMs, 2500)) {
            playSound(critical && swinging->exertionCritical ? swinging->exertionCritical : swinging->exertion,
                      Category::Voices, priority, &attackerAt);
        }
        watchUnit(e.victim, f.nowMs);
        break;
    }
    case game::CombatEvent::Kind::Parry:
        playSound(m_data->weaponImpact(subclass, wooden ? 6 : 5, false), Category::Effects, priority, &victimAt);
        break;
    case game::CombatEvent::Kind::Block:
        playSound(m_data->weaponImpact(subclass, 3, false), Category::Effects, priority, &victimAt);
        break;
    case game::CombatEvent::Kind::Absorb:
        playSound(m_absorbSound, Category::Effects, priority, &victimAt);
        break;
    default: break; // a dodge: only the swoosh
    }
}

void GameAudio::watchDeaths(const GameAudioFrame& f) {
    for (UnitMemory& u : m_units) {
        if (u.guid == 0 || !u.alive || u.lastMs[kWatch] == 0 || reached(f.nowMs, u.lastMs[kWatch] + kWatchMs)) {
            continue;
        }
        const game::Entity* unit = f.entities->find(u.guid);
        if (!unit) {
            u.alive = false;
            continue;
        }
        if (game::isDead(*unit)) {
            u.alive = false;
            glm::dvec3 at;
            const audio::CreatureSounds* sounds = soundsOf(*unit);
            if (sounds && positionOf(f, u.guid, at)) {
                playSound(sounds->death, Category::Voices,
                      u.guid == f.player ? kPriorityPlayer : kPriorityWorld, &at);
            }
            if (positionOf(f, u.guid, at)) {
                deathThud(f, *unit, at); // parity pass: the body hits the ground
            }
        }
    }
}

// --- NPC voices ------------------------------------------------------------------------------------------------------

u64 GameAudio::interactNpc(const game::Gameplay& play) const {
    // The windows that call SetInteract (0x4930D0) on their server packet; the mailbox, books and trade do too, but on
    // an object or a player (no voice).
    const game::NpcDialog& dialog = play.dialog();
    const game::TrainerWindow& trainer = play.abilities().trainer();
    const game::AuctionHouse& auction = play.exchange().auction();
    const u64 open[] = {dialog.open() ? dialog.npc() : 0,
                        play.inventory().vendor().npc,
                        trainer.open() ? trainer.trainer() : 0,
                        play.travel().taxiMap().open ? play.travel().taxiMap().flightMaster : 0,
                        play.inventory().banker(),
                        auction.open() ? auction.auctioneer() : 0,
                        play.stable().stableMaster(),
                        play.petitions().registrar().open ? play.petitions().registrar().npc : 0,
                        play.tabards().open() ? play.tabards().vendor() : 0};
    for (const u64 guid : open) {
        if (guid != 0) {
            return guid;
        }
    }
    return 0;
}

bool GameAudio::npcSpeaking(u64 guid) const {
    for (const NpcVoice& v : m_npcVoices) {
        if (v.guid == guid && m_audio && m_audio->playing(v.voice)) {
            return true;
        }
    }
    return false;
}

bool GameAudio::npcSay(const GameAudioFrame& f, const game::Entity& unit, u32 soundEntry, i32 file) {
    // At the unit when the line starts (not followed), not at all beyond the row's cutoff (0x45CDF0).
    const audio::SoundEntry* e = soundEntry != 0 ? m_data->entry(soundEntry) : nullptr;
    glm::dvec3 at;
    if (!e || !positionOf(f, unit.guid, at) || glm::length(at - f.listener) > static_cast<f64>(e->maxDistance)) {
        return false;
    }
    const audio::VoiceHandle voice = playSound(soundEntry, Category::Voices, kPriorityVoice, &at, false, 0.0f, 1.0f,
                                               0.0f, 0.0f, file);
    if (!voice) {
        return false;
    }
    NpcVoice* slot = &m_npcVoices[0];
    for (NpcVoice& v : m_npcVoices) {
        if (v.guid == unit.guid || !m_audio->playing(v.voice)) {
            slot = &v;
            break;
        }
    }
    *slot = {unit.guid, voice};
    return true;
}

void GameAudio::npcGreeting(const GameAudioFrame& f, u64 guid, bool greeting) {
    // 0x60C3B0: a creature (not a player) alive, the player neither dead nor a ghost, the creature silent; no reaction
    // check. It also forgets the clicks.
    m_clickGuid = 0;
    const game::Entity* unit = f.entities->find(guid);
    const game::DeathState& death = f.play->death();
    if (!unit || unit->type != game::ObjectType::Unit || game::isDead(*unit) || death.dead || death.ghost ||
        npcSpeaking(guid)) {
        return;
    }
    if (const audio::NpcSounds* sounds = m_data->npcSounds(unit->value(uf::UNIT_FIELD_DISPLAYID))) {
        npcSay(f, *unit, greeting ? sounds->greeting : sounds->farewell, -1);
    }
}

void GameAudio::clickVoice(const GameAudioFrame& f, u64 guid) {
    // CGUnit::PlayClickVoice (0x60C270): a creature alive and silent, neither it nor the player hostile to the other,
    // the player alive. Five greetings, then the pissed row's files in order, then a greeting and all over again; the
    // count is the last unit's (another unit starts it again) and only grows when the line plays.
    const game::Entity* unit = f.entities->find(guid);
    const game::Entity* me = f.entities->find(f.player);
    const game::DeathState& death = f.play->death();
    if (!unit || !me || unit->type != game::ObjectType::Unit || game::isDead(*unit) || npcSpeaking(guid) ||
        game::reactionTo(*me, *unit, f.reactions) == game::Reaction::Hostile ||
        game::reactionTo(*unit, *me, f.reactions) == game::Reaction::Hostile || death.dead || death.ghost) {
        return;
    }
    if (guid != m_clickGuid) {
        m_clickCount = 0;
    }
    m_clickGuid = guid;
    const audio::NpcSounds* sounds = m_data->npcSounds(unit->value(uf::UNIT_FIELD_DISPLAYID));
    if (!sounds || sounds->greeting == 0) {
        return;
    }
    constexpr u32 kGreetings = 5;
    const audio::SoundEntry* pissed = sounds->pissed != 0 ? m_data->entry(sounds->pissed) : nullptr;
    const u32 pissedFiles = pissed ? pissed->fileCount : 0;
    u32 entry = sounds->greeting;
    i32 file = -1;
    u32 next = m_clickCount + 1;
    if (m_clickCount >= kGreetings) {
        if (m_clickCount - kGreetings < pissedFiles) {
            entry = sounds->pissed;
            file = static_cast<i32>(m_clickCount - kGreetings);
        } else {
            next = 0;
        }
    }
    if (npcSay(f, *unit, entry, file)) {
        m_clickCount = next;
    }
}

void GameAudio::watchWindows(const GameAudioFrame& f) {
    const game::Gameplay& play = *f.play;
    // Ours (NPC voices): SetInteract (0x4930D0) greets the NPC of a window that opens unless its window was already up
    // (gossip -> vendor: silent), and the previous NPC goes without a word; ClearInteract (0x4931C0) says farewell
    // when the last window closes by the player's hand or the server's, not when the NPC walked out of reach or went
    // away (0x493230, 0x605860).
    if (const u64 npc = interactNpc(play); npc != m_interactNpc) {
        if (npc != 0) {
            npcGreeting(f, npc, true);
        } else if (const game::Entity* old = f.entities->find(m_interactNpc)) {
            const game::Entity* me = f.entities->find(f.player);
            f32 radius = 0.0f;
            const u32 bits = old->value(uf::UNIT_FIELD_BOUNDINGRADIUS);
            std::memcpy(&radius, &bits, sizeof(radius));
            // The windows close themselves past 10 yards between the player and the NPC's bounding sphere.
            if (me && game::distanceAt(*me, *old, f.nowMs) - radius < kNpcFarewellReach) {
                npcGreeting(f, m_interactNpc, false);
            }
        }
        m_interactNpc = npc;
    }
    // NPC windows: open / close as the original frames.
    const game::NpcDialog& dialog = play.dialog();
    const u8 kind = static_cast<u8>(dialog.kind());
    if (kind != m_dialogKind || (dialog.open() && dialog.npc() != m_dialogNpc)) {
        const bool wasOpen = m_dialogKind != static_cast<u8>(game::DialogKind::None);
        if (!wasOpen && dialog.open()) {
            playUi(UiSound::NpcOpen);
        } else if (wasOpen && !dialog.open()) {
            playUi(UiSound::NpcClose);
        } else if (dialog.kind() == game::DialogKind::QuestDetails ||
                   dialog.kind() == game::DialogKind::QuestRequestItems ||
                   dialog.kind() == game::DialogKind::QuestOfferReward) {
            playUi(UiSound::QuestSelect);
        }
        m_dialogKind = kind;
        m_dialogNpc = dialog.npc();
    }
    // Loot: an empty corpse's sound, the fishing reel.
    const game::LootWindow& loot = play.loot();
    if (loot.open() && !m_lootOpen) {
        if (loot.lootType() == net::kLootTypeFishing) {
            playUi(UiSound::FishingReel);
        } else if (loot.empty()) {
            playUi(UiSound::LootEmpty);
        }
    }
    m_lootOpen = loot.open();
    // The windows the server opens, with their frames' sounds (MerchantFrame, BankFrame, ItemTextFrame, TradeFrame,
    // the auction house, MailFrame, TaxiFrame).
    struct WindowSound {
        UiSound open;
        UiSound close;
    };
    static constexpr WindowSound kWindows[] = {
        {UiSound::CharacterOpen, UiSound::CharacterClose}, // vendor
        {UiSound::GameMenuOpen, UiSound::PanelClose},      // bank
        {UiSound::GameMenuOpen, UiSound::PanelClose},      // book or letter
        {UiSound::CharacterOpen, UiSound::CharacterClose}, // trade
        {UiSound::AuctionOpen, UiSound::AuctionClose},     // auction house
        {UiSound::BagOpen, UiSound::CharacterClose},       // mailbox (MAIL_SHOW opens the backpack; OnHide)
        {UiSound::GameMenuOpen, UiSound::PanelClose},      // taxi map
        {UiSound::CharacterOpen, UiSound::CharacterClose}, // trainer (ClassTrainerFrame, parity pass)
    };
    const game::Inventory& inventory = play.inventory();
    const game::Exchange& exchange = play.exchange();
    const bool shown[] = {inventory.vendor().open(),
                          inventory.banker() != 0,
                          inventory.reading().open(),
                          exchange.trade().phase() == game::TradePhase::Open,
                          exchange.auction().open(),
                          exchange.mail().open(),
                          play.travel().taxiMap().open,
                          play.abilities().trainer().open()};
    static_assert(std::size(kWindows) == std::size(shown), "one sound pair per window");
    u32 windows = 0;
    for (u32 i = 0; i < std::size(shown); ++i) {
        windows |= shown[i] ? 1u << i : 0u;
        if ((windows ^ m_serverWindows) & (1u << i)) {
            playUi(shown[i] ? kWindows[i].open : kWindows[i].close);
        }
    }
    m_serverWindows = windows;
    // Whispers (ChatFrame: TellMessage, at most every few seconds).
    while (const game::ChatLine* line = play.chat().next(m_chatCursor)) {
        if (line->type == net::ChatType::Whisper && line->sender != f.player &&
            (m_tellAtMs == 0 || reached(f.nowMs, m_tellAtMs + 5000))) {
            playUi(UiSound::Whisper);
            m_tellAtMs = std::max(1u, f.nowMs);
        } else if (line->type == net::ChatType::RaidWarning) {
            playUi(UiSound::RaidWarning); // RaidWarningFrame (parity pass)
        }
    }
    // Targets (TargetFrame: by how the target looks to the player).
    const u64 target = play.target();
    if (target != m_target) {
        if (target == 0) {
            playUi(UiSound::TargetLost);
        } else {
            const game::Entity* me = f.entities->find(f.player);
            const game::Entity* unit = f.entities->find(target);
            const game::Reaction reaction = me && unit ? game::reactionTo(*me, *unit, f.reactions)
                                                       : game::Reaction::Neutral;
            playUi(reaction == game::Reaction::Hostile    ? UiSound::TargetEnemy
                   : reaction == game::Reaction::Friendly ? UiSound::TargetFriend
                                                          : UiSound::TargetNeutral);
        }
        m_target = target;
    }
}

void GameAudio::updateMissiles(const GameAudioFrame& f) {
    for (Missile& m : m_missiles) {
        if (m.target == 0) {
            continue;
        }
        glm::dvec3 to;
        const bool known = positionOf(f, m.target, to);
        if (reached(f.nowMs, m.arriveMs) || !known) {
            m_audio->stop(m.loop, 0.05f);
            if (known) {
                playSound(m.impact, Category::Effects, m.priority, &to);
            }
            m = {};
            continue;
        }
        const f64 t = static_cast<f64>(f.nowMs - m.startMs) / static_cast<f64>(m.arriveMs - m.startMs);
        m_audio->setPosition(m.loop, m.from + (to - m.from) * t);
    }
}

void GameAudio::updateLoops(const GameAudioFrame& f) {
    // Precasts and channels follow their caster; a loop whose caster is gone stops.
    for (Loop& loop : m_loops) {
        if (loop.guid == 0) {
            continue;
        }
        glm::dvec3 at;
        if (!m_audio->playing(loop.voice)) {
            loop = {};
        } else if (positionOf(f, loop.guid, at)) {
            m_audio->setPosition(loop.voice, at);
        } else {
            m_audio->stop(loop.voice, 0.1f);
            loop = {};
        }
    }
}

// --- Footsteps ---------------------------------------------------------------------------------------------------

void GameAudio::footsteps(const GameAudioFrame& f) {
    if (!f.world || f.loading || !f.character || eventFootsteps(f)) {
        m_stepDistance = 0.0f; // phase 14: with its actor's events, its feet sound when they touch the ground
        return;
    }
    // Mounted: the mount's hooves (parity pass), else the character's feet.
    const bool mounted = m_body.mount != 0 && m_data->footstepId(m_body.mount) != 0;
    u32 footstep = mounted ? m_data->footstepId(m_body.mount) : (f.display != 0 ? m_data->footstepId(f.display) : 0);
    if (footstep == 0) {
        footstep = kHumanFootsteps;
    }
    u32 terrain = f.terrainSound;
    if (terrain == 0) {
        terrain = f.onObject ? kTerrainStone : m_ground.soundAt(f.feet.x, f.feet.y);
    }
    if (terrain == 0) {
        terrain = kTerrainDirt;
    }
    const auto step = [&] {
        playSound(m_data->footstep(footstep, terrain, f.wading), Category::Effects, kPriorityPlayer, &f.feet, false, 0.0f,
              m_leftFoot ? 1.0f : 0.9f);
        m_leftFoot = !m_leftFoot;
    };
    if (f.landed) {
        step();
    }
    if (!f.onGround || f.swimming || f.speed < 0.5f) {
        m_stepDistance = 0.0f;
        return;
    }
    // About two steps per animation cycle: 1.4 yd walking (2.5 yd/s), 2.3 yd running (7 yd/s); a mount's four hooves.
    const f32 stride = audio::strideLength(f.speed, mounted);
    if (m_stepDistance <= 0.0f) {
        m_stepDistance = stride * 0.6f; // the first step comes soon after starting to move
    }
    m_stepDistance += f.speed * f.dt;
    if (m_stepDistance >= stride) {
        m_stepDistance = std::fmod(m_stepDistance, stride);
        step();
    }
}

u32 GameAudio::recentSound(u32 back) const {
    if (back >= m_recent.size()) {
        return 0;
    }
    return m_recent[(m_recentNext + m_recent.size() - 1 - back) % m_recent.size()].entry;
}

} // namespace vd
