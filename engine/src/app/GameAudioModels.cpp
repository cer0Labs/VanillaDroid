// GameAudio, phase 14: the sounds of the models, as WoW.exe 1.12.1 plays them. The doodads' sound events ($DSL
// loops, $DSO one-shots: braziers, campfires, bellows, windmills, waterfalls) baked into worldsound.vdpk; the
// terrain's emitters (MCSE); the units' CreatureSoundData loops (elementals, golems, machines); and the animation
// events of every actor near the listener (world/Actors::events): footsteps ($FSD) of players, creatures and
// mounts on the ground under them, fidgets ($FD1-$FD4, $FDX), custom attacks ($AH0-$AH3), wings ($WNG, $WGG), emote
// sounds ($CSD, $ESD) and the objects' own loops and one-shots ($DSL, $DSO, $DSE: elevators, the tram, ships).
// The rules are audio/ModelSoundRules; this file follows the world.
#include "app/GameAudio.h"

#include "app/GameAudioInternal.h"
#include "audio/ModelSoundRules.h"
#include "audio/Spatial.h"
#include "core/Clock.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/Gameplay.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "physics/CollisionWorld.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdlib>

namespace vd {
namespace {

using namespace gameaudio;
using audio::Category;
namespace uf = net::uf;

constexpr f64 kDoodadRange = 60.0;  // doodads further away are not tracked (their sounds reach 10-45 yd)
constexpr f64 kDoodadRequery = 8.0; // the listener moved this far: the doodads near are looked for again
constexpr u32 kCreatureScanMs = 250;
constexpr u32 kActorMapMs = 250; // the actors' units are looked up again this often
constexpr f64 kCreatureRange = 50.0;
constexpr f64 kOtherStepRange = 30.0; // others' footsteps further away are not played (-20 dB at 30 yd)
constexpr u32 kLoopReviewMs = 250;    // the loops to play are chosen again this often
constexpr f32 kLoopFade = 0.5f;
// The models' sounds give way to everything else when the voices run out.
constexpr u8 kPriorityModel = 90;
constexpr u32 kHumanFootsteps = 7; // FootstepTerrainLookup id of most characters
constexpr u32 kTerrainDirt = 1;    // TerrainType.SoundID: dirt, the fallback
constexpr u32 kTerrainStone = 3;   // and stone: buildings and objects

bool switchedOff(const char* name) {
    const char* value = VD_DEV_ENV(name);
    return value && value[0] == '0';
}

u32 between(u32 random, u32 lo, u32 hi) {
    return hi > lo ? lo + random % (hi - lo + 1) : lo;
}

} // namespace

void GameAudio::initWorldSounds(const std::filesystem::path& packsDir) {
    m_modelSoundsOn = !switchedOff("VD_MODEL_SOUNDS");
    m_emittersOn = !switchedOff("VD_EMITTERS");
    m_otherStepsOn = !switchedOff("VD_OTHER_FOOTSTEPS");
    if (active()) {
        m_worldSounds.init(packsDir);
    }
}

bool GameAudio::eventFootsteps(const GameAudioFrame& f) const {
    return m_modelSoundsOn && f.scene && f.playerActor != ~0u;
}

void GameAudio::resetModelSounds() {
    for (DoodadLoop& d : m_doodadLoops) {
        m_audio->stop(d.loop.voice, kLoopFade);
        d = {};
    }
    m_doodadNearCount = 0;
    m_doodadQueryMs = 0;
    for (EmitterState& e : m_emitters) {
        m_audio->stop(e.loop.voice, kLoopFade);
        e = {};
    }
    for (CreatureLoop& c : m_creatureLoops) {
        m_audio->stop(c.loop.voice, kLoopFade);
        c = {};
    }
    for (ActorLoop& a : m_actorLoops) {
        m_audio->stop(a.voice, kLoopFade);
        a = {};
    }
    m_modelStats.doodads = 0;
    m_modelStats.loops = 0;
}

void GameAudio::modelSounds(const GameAudioFrame& f) {
    VD_PROFILE_ZONE("GameAudio::modelSounds");
    const Clock::time_point begin = Clock::now();
    m_worldSounds.setMap(f.world ? f.map : std::string_view{});
    m_worldSounds.update();
    if (const u64 map = f.world ? assetPathHash(f.map) : 0; map != m_modelSoundsMap) {
        m_modelSoundsMap = map; // another map: its emitters are others
        for (EmitterState& e : m_emitters) {
            m_audio->stop(e.loop.voice, kLoopFade);
            e = {};
        }
    }
    if (!f.world || f.loading || f.login) {
        if (m_modelStats.doodads != 0 || m_modelStats.loops != 0) {
            resetModelSounds();
        }
        m_lastCycleMs = f.nowMs;
        return;
    }
    // Where a slow frame went (logged past 2 ms): doodads, emitters, units' loops, the loops' review, events.
    std::array<f32, 5> parts{};
    Clock::time_point mark = begin;
    const auto lap = [&](u32 part) {
        const Clock::time_point now = Clock::now();
        parts[part] = static_cast<f32>(secondsBetween(mark, now) * 1000.0);
        mark = now;
    };
    doodadSounds(f);
    lap(0);
    emitterSounds(f);
    lap(1);
    creatureLoops(f);
    lap(2);
    reviewLoops(f);
    lap(3);
    actorEvents(f);
    lap(4);
    m_lastCycleMs = f.nowMs;
    m_modelStats.updateMs = static_cast<f32>(secondsBetween(begin, Clock::now()) * 1000.0);
    m_modelStats.sumMs += m_modelStats.updateMs;
    ++m_modelStats.frames;
    if (m_modelStats.updateMs > 2.0f) {
        VD_LOG_WARN("Model sounds hitch %.2f ms: doodads %.2f, emitters %.2f, creatures %.2f, loops %.2f, events %.2f",
                    m_modelStats.updateMs, parts[0], parts[1], parts[2], parts[3], parts[4]);
    }
}

// --- Doodads ---------------------------------------------------------------------------------------------------------

void GameAudio::doodadSounds(const GameAudioFrame& f) {
    if (!m_modelSoundsOn) {
        return;
    }
    // The map's baked doodad sounds near the listener (pack::DoodadSound, the doodads the map places with their
    // events' world points): looked for a few times a second, or at once when the listener jumped or the table changed.
    const std::span<const pack::DoodadSound> records = m_worldSounds.doodadSounds();
    const glm::dvec3 moved = f.listener - m_doodadQueryAt;
    if (m_worldSounds.generation() != m_doodadGeneration || reached(f.nowMs, m_doodadQueryMs) ||
        glm::dot(moved, moved) > kDoodadRequery * kDoodadRequery) {
        if (m_worldSounds.generation() != m_doodadGeneration) {
            m_doodadGeneration = m_worldSounds.generation();
            for (DoodadLoop& d : m_doodadLoops) {
                m_audio->stop(d.loop.voice, kLoopFade); // another table: its records are others
                d = {};
            }
        }
        m_doodadQueryMs = f.nowMs + kLoopReviewMs;
        m_doodadQueryAt = f.listener;
        m_doodadNearCount =
            m_worldSounds.doodadSoundsNear(f.listener.x, f.listener.y, static_cast<f32>(kDoodadRange), m_doodadNear);
        // The loops near join the tracked ones (their voices start in reviewLoops()); those no longer near go.
        for (DoodadLoop& d : m_doodadLoops) {
            if (d.record == ~0u) {
                continue;
            }
            bool near = false;
            for (u32 i = 0; i < m_doodadNearCount && !near; ++i) {
                near = m_doodadNear[i] == d.record;
            }
            if (!near) {
                m_audio->stop(d.loop.voice, kLoopFade);
                d = {};
            }
        }
        for (u32 i = 0; i < m_doodadNearCount; ++i) {
            const pack::DoodadSound& r = records[m_doodadNear[i]];
            if (r.kind != pack::kModelSoundLoop) {
                continue;
            }
            DoodadLoop* free = nullptr;
            bool tracked = false;
            for (DoodadLoop& d : m_doodadLoops) {
                tracked = tracked || d.record == m_doodadNear[i];
                if (!free && d.record == ~0u) {
                    free = &d;
                }
            }
            if (!tracked && free) {
                *free = {};
                free->record = m_doodadNear[i];
                free->loop.entry = r.sound;
            }
        }
    }
    m_modelStats.doodads = m_doodadNearCount;
    // Their timed one-shots (the bellows breathing in and out, a windmill's creak), on the doodads' animation clock.
    for (u32 i = 0; i < m_doodadNearCount; ++i) {
        const pack::DoodadSound& r = records[m_doodadNear[i]];
        if (r.kind != pack::kModelSoundTimed || r.cycleMs == 0) {
            continue;
        }
        if (audio::crossed(audio::cycleTime(m_lastCycleMs, r.cycleMs), audio::cycleTime(f.nowMs, r.cycleMs), r.timeMs,
                           r.cycleMs)) {
            const glm::dvec3 at(r.position[0], r.position[1], r.position[2]);
            const audio::SoundEntry* entry = m_data->entry(r.sound);
            if (entry && glm::length(at - f.listener) < static_cast<f64>(entry->maxDistance)) {
                playSound(r.sound, Category::Ambience, kPriorityModel, &at);
            }
        }
    }
}

// --- Terrain emitters (MCSE) ----------------------------------------------------------------------------------------

void GameAudio::emitterSounds(const GameAudioFrame& f) {
    if (!m_emittersOn) {
        return;
    }
    const std::span<const pack::SoundEmitter> emitters = m_worldSounds.emitters();
    const u32 minute = static_cast<u32>(f.hours * 60.0f) % 1440;
    for (u32 i = 0; i < emitters.size() && i < m_emitters.size(); ++i) {
        const pack::SoundEmitter& e = emitters[i];
        EmitterState& state = m_emitters[i];
        // Far ones (most of the map's) cost a distance: no table is looked at.
        const glm::dvec3 at(e.position[0], e.position[1], e.position[2]);
        const f64 bound = std::max({static_cast<f64>(e.cutoffDistance), static_cast<f64>(e.maxDistance), 60.0});
        if (!reached(f.nowMs, state.nextMs) || glm::dot(at - f.listener, at - f.listener) > bound * bound) {
            continue;
        }
        const audio::SoundEntry* entry = m_data->entry(e.sound);
        if (!entry || entry->fileCount == 0 || (entry->flags & audio::SoundEntry::kFlagLoop) != 0) {
            continue; // loops are chosen with the others (reviewLoops)
        }
        // Heard between its two times of the day (both 0: always), within its reach.
        const bool always = e.startMinute == 0 && e.endMinute == 0;
        const bool window = always || (e.startMinute <= e.endMinute ? minute >= e.startMinute && minute < e.endMinute
                                                                    : minute >= e.startMinute || minute < e.endMinute);
        const f32 reach = e.cutoffDistance > 0.0f ? e.cutoffDistance : entry->maxDistance;
        if (!window || glm::length(at - f.listener) > static_cast<f64>(reach)) {
            continue;
        }
        // Groups of instancesMin..Max sounds (each loopCountMin..Max times) gapMin..Max ms apart, then silenceMin..Max.
        if (state.left == 0) {
            const u32 instances =
                std::max(1u, between(random(), e.instancesMin, std::max(e.instancesMin, e.instancesMax)));
            const u32 loops = std::max(1u, between(random(), e.loopCountMin, std::max(e.loopCountMin, e.loopCountMax)));
            state.left = static_cast<u16>(std::min(instances * loops, 64u));
        }
        playSound(e.sound, Category::Ambience, kPriorityModel, &at, false, 0.0f, 1.0f, e.minDistance,
                  e.cutoffDistance > 0.0f ? e.cutoffDistance : e.maxDistance);
        --state.left;
        state.nextMs = f.nowMs + (state.left > 0 ? std::max(250u, between(random(), e.gapMinMs, e.gapMaxMs))
                                                 : std::max(1000u, between(random(), e.silenceMinMs, e.silenceMaxMs)));
    }
}

// --- Units' loops ----------------------------------------------------------------------------------------------------

void GameAudio::creatureLoops(const GameAudioFrame& f) {
    if (!m_modelSoundsOn || !f.entities) {
        for (CreatureLoop& c : m_creatureLoops) {
            m_audio->stop(c.loop.voice, kLoopFade);
            c = {};
        }
        return;
    }
    // The units with a loop (CreatureSoundData column 23: FireElementalLoop, GolemLoop...) near: who they are is looked
    // at a few times a second; where they are, every frame.
    if (reached(f.nowMs, m_creatureScanMs)) {
        m_creatureScanMs = f.nowMs + kCreatureScanMs;
        for (CreatureLoop& c : m_creatureLoops) {
            const game::Entity* unit = c.guid != 0 ? f.entities->find(c.guid) : nullptr;
            glm::dvec3 at;
            if (c.guid != 0 && (!unit || game::isDead(*unit) || !positionOf(f, c.guid, at) ||
                                glm::length(at - f.listener) > kCreatureRange * 1.2)) {
                m_audio->stop(c.loop.voice, kLoopFade);
                c = {};
            }
        }
        f.entities->forEach([&](const game::Entity& e) {
            if (e.type != game::ObjectType::Unit || !e.hasPosition || game::isDead(e)) {
                return;
            }
            const audio::CreatureSounds* sounds = soundsOf(e);
            if (!sounds || sounds->loop == 0 ||
                glm::length(glm::dvec3(e.positionAt(f.nowMs)) - f.listener) > kCreatureRange) {
                return;
            }
            CreatureLoop* free = nullptr;
            for (CreatureLoop& c : m_creatureLoops) {
                if (c.guid == e.guid) {
                    return;
                }
                if (!free && c.guid == 0) {
                    free = &c;
                }
            }
            if (free) {
                *free = {};
                free->guid = e.guid;
                free->loop.entry = sounds->loop;
            }
        });
    }
    for (CreatureLoop& c : m_creatureLoops) {
        glm::dvec3 at;
        if (c.guid != 0 && positionOf(f, c.guid, at)) {
            c.at = at;
            m_audio->setPosition(c.loop.voice, at);
        }
    }
}

// --- Which loops play ------------------------------------------------------------------------------------------------

void GameAudio::reviewLoops(const GameAudioFrame& f) {
    if (!reached(f.nowMs, m_loopReviewMs)) {
        return;
    }
    m_loopReviewMs = f.nowMs + kLoopReviewMs;
    VD_PROFILE_ZONE("GameAudio::reviewLoops");
    // Every loop that could play (doodads', looping emitters', units'), with how loud it would be; the loudest
    // kMaxModelLoops play. One within its cutoff may start; one playing stops past the cutoff plus the hysteresis.
    u32 count = 0;
    const auto consider = [&](LoopVoice& loop, const glm::dvec3& at, f32 maxDistance, f32 minDistance) {
        const audio::SoundEntry* entry = loop.entry != 0 ? m_data->entry(loop.entry) : nullptr;
        if (!entry || count >= m_loopCandidates.size()) {
            m_audio->stop(loop.voice, kLoopFade);
            return;
        }
        const f32 reach = maxDistance > 0.0f ? maxDistance : std::max(entry->maxDistance, entry->minDistance + 1.0f);
        const f32 near = minDistance > 0.0f ? minDistance : std::max(entry->minDistance, 1.0f);
        const f32 distance = static_cast<f32>(glm::length(at - f.listener));
        const bool playing = m_audio->playing(loop.voice);
        if (distance > reach * (playing ? audio::kLoopHysteresis : 1.0f)) {
            m_audio->stop(loop.voice, kLoopFade);
            return;
        }
        m_loopCandidates[count++] = {
            &loop, at,   entry->volume * audio::distanceGain(std::min(distance, reach * 0.99f), near, reach),
            reach, near, playing};
    };
    if (m_modelSoundsOn) {
        const std::span<const pack::DoodadSound> records = m_worldSounds.doodadSounds();
        for (DoodadLoop& d : m_doodadLoops) {
            if (d.record != ~0u && d.record < records.size()) {
                const pack::DoodadSound& r = records[d.record];
                consider(d.loop, glm::dvec3(r.position[0], r.position[1], r.position[2]), 0.0f, 0.0f);
            }
        }
        for (CreatureLoop& c : m_creatureLoops) {
            if (c.guid != 0) {
                consider(c.loop, c.at, 0.0f, 0.0f);
            }
        }
    }
    if (m_emittersOn) {
        const std::span<const pack::SoundEmitter> emitters = m_worldSounds.emitters();
        for (u32 i = 0; i < emitters.size() && i < m_emitters.size(); ++i) {
            const pack::SoundEmitter& e = emitters[i];
            const audio::SoundEntry* entry = m_data->entry(e.sound);
            if (entry && (entry->flags & audio::SoundEntry::kFlagLoop) != 0) {
                m_emitters[i].loop.entry = e.sound;
                consider(m_emitters[i].loop, glm::dvec3(e.position[0], e.position[1], e.position[2]),
                         e.cutoffDistance > 0.0f ? e.cutoffDistance : e.maxDistance, e.minDistance);
            }
        }
    }
    // The loudest play.
    const u32 keep = std::min<u32>(count, audio::kMaxModelLoops);
    std::partial_sort(m_loopCandidates.begin(), m_loopCandidates.begin() + keep, m_loopCandidates.begin() + count,
                      [](const LoopCandidate& a, const LoopCandidate& b) { return a.audibility > b.audibility; });
    u32 playing = 0;
    for (u32 i = 0; i < count; ++i) {
        LoopCandidate& c = m_loopCandidates[i];
        if (i >= keep) {
            m_audio->stop(c.loop->voice, kLoopFade);
            continue;
        }
        if (!c.playing) {
            c.loop->voice = playSound(c.loop->entry, Category::Ambience, kPriorityModel, &c.at, true, kLoopFade, 1.0f,
                                      c.minDistance, c.maxDistance);
        }
        playing += m_audio->playing(c.loop->voice) ? 1 : 0;
    }
    m_modelStats.loops = playing;
}

// --- Animation events ------------------------------------------------------------------------------------------------

void GameAudio::mapActors(const GameAudioFrame& f) {
    // Built when an event needs it, at most a few times a second (actorSounds() checks an entry still holds).
    if (!reached(f.nowMs, m_actorUnitsMs)) {
        return;
    }
    m_actorUnitsMs = f.nowMs + kActorMapMs;
    m_actorUnitCount = 0;
    if (!f.scene || !f.entities) {
        return;
    }
    f.entities->forEach([&](const game::Entity& e) {
        if (!game::isUnit(e) || m_actorUnitCount >= m_actorUnits.size()) {
            return;
        }
        if (const u32 actor = f.scene->unitActor(e.guid); actor != ~0u) {
            m_actorUnits[m_actorUnitCount++] = {actor, e.guid};
        }
    });
    std::sort(m_actorUnits.begin(), m_actorUnits.begin() + m_actorUnitCount,
              [](const ActorUnit& a, const ActorUnit& b) { return a.actor < b.actor; });
}

const audio::CreatureSounds* GameAudio::actorSounds(const GameAudioFrame& f, u32 actor, u64* guid) {
    // A unit's display (its own sounds or its model's); else (a mount, a synthetic crowd) the actor's model's.
    mapActors(f);
    const ActorUnit* begin = m_actorUnits.data();
    const ActorUnit* end = begin + m_actorUnitCount;
    const ActorUnit* it =
        std::lower_bound(begin, end, actor, [](const ActorUnit& a, u32 key) { return a.actor < key; });
    if (it != end && it->actor == actor && f.scene->unitActor(it->guid) == actor) {
        if (guid) {
            *guid = it->guid;
        }
        if (const game::Entity* unit = f.entities ? f.entities->find(it->guid) : nullptr) {
            if (const audio::CreatureSounds* sounds = soundsOf(*unit)) {
                return sounds;
            }
        }
    }
    const u64 model = f.scene->actorModel(actor);
    return model != 0 ? m_data->modelSounds(model) : nullptr;
}

u32 GameAudio::groundSoundAt(const GameAudioFrame& f, const glm::dvec3& at, bool* water) const {
    // The ground under a foot: the terrain's surface (GroundSounds), stone on a building or an object; splashing where
    // a liquid covers it.
    u32 terrain = 0;
    const std::optional<f32> height = f.scene ? f.scene->groundHeight(at.x, at.y) : std::nullopt;
    if (height && at.z > static_cast<f64>(*height) + 0.5) {
        terrain = kTerrainStone;
    } else {
        terrain = m_ground.soundAt(at.x, at.y);
    }
    if (water) {
        *water = false;
        if (f.collision) {
            const std::optional<physics::LiquidSample> liquid =
                f.collision->liquidAt(at.x, at.y, at.z + 2.0, at.z - 0.5);
            *water = liquid && static_cast<f64>(liquid->level) > at.z + 0.05;
        }
    }
    return terrain != 0 ? terrain : kTerrainDirt;
}

void GameAudio::eventFootstep(const GameAudioFrame& f, u32 actor, const glm::dvec3& at,
                              const audio::CreatureSounds* sounds) {
    // The player's character and its mount: its own footsteps, always; others' within a few yards, a few at a time.
    const bool own = actor == f.playerActor || (f.playerActor != ~0u && f.scene->actorParent(f.playerActor) == actor);
    if (!own && (!m_otherStepsOn || glm::length(at - f.listener) > kOtherStepRange)) {
        return;
    }
    u32 footstep = sounds ? sounds->footstep : 0;
    if (footstep == 0 && own) {
        footstep = kHumanFootsteps;
    }
    if (footstep == 0) {
        return;
    }
    bool water = false;
    const u32 terrain = groundSoundAt(f, at, &water);
    const u32 entry = m_data->footstep(footstep, terrain, water);
    if (own) {
        playSound(entry, Category::Effects, kPriorityPlayer, &at);
        ++m_modelStats.footsteps;
        return;
    }
    audio::VoiceHandle* slot = nullptr;
    u32 busy = 0;
    for (audio::VoiceHandle& h : m_otherSteps) {
        if (m_audio->playing(h)) {
            ++busy;
        } else if (!slot) {
            slot = &h;
        }
    }
    if (busy >= audio::kMaxOtherFootsteps || !slot) {
        return;
    }
    *slot = playSound(entry, Category::Effects, kPriorityModel, &at);
    m_modelStats.footsteps += *slot ? 1 : 0;
}

void GameAudio::actorLoop(const GameAudioFrame& f, u32 actor, u32 index, u32 sound, const glm::dvec3& at, bool stop) {
    // One loop per model, as the client keeps it (0x6951E0): $DSL starts it or replaces another, $DSE stops it.
    ActorLoop* slot = nullptr;
    ActorLoop* free = nullptr;
    for (ActorLoop& a : m_actorLoops) {
        if (a.actor == actor) {
            slot = &a;
            break;
        }
        if (!free && (a.actor == ~0u || !m_audio->playing(a.voice))) {
            free = &a;
        }
    }
    if (stop) {
        if (slot) {
            m_audio->stop(slot->voice, kLoopFade);
            *slot = {};
        }
        return;
    }
    if (slot && slot->entry == sound && m_audio->playing(slot->voice)) {
        return; // the same loop goes on
    }
    // A server object's GameObjectDisplayInfo loop may already play it (objectSounds()).
    for (const ObjectMemory& o : m_objects) {
        if (o.loopEntry == sound && m_audio->playing(o.loop)) {
            return;
        }
    }
    if (!slot) {
        slot = free;
    }
    if (!slot) {
        return;
    }
    m_audio->stop(slot->voice, kLoopFade);
    const audio::SoundEntry* entry = m_data->entry(sound);
    const bool loop = entry && (entry->flags & audio::SoundEntry::kFlagLoop) != 0;
    const u8 priority = actor == f.playerActor ? kPriorityPlayer : kPriorityWorld;
    *slot = {actor, index, sound, playSound(sound, Category::Effects, priority, &at, loop, loop ? kLoopFade : 0.0f)};
}

void GameAudio::actorEvents(const GameAudioFrame& f) {
    if (!f.scene) {
        return;
    }
    const AudioScene& scene = *f.scene;
    // The objects' loops follow them, and end with them.
    for (ActorLoop& a : m_actorLoops) {
        if (a.actor == ~0u) {
            continue;
        }
        if (scene.actorModel(a.actor) == 0 || !m_audio->playing(a.voice)) {
            m_audio->stop(a.voice, kLoopFade);
            a = {};
        } else if (const std::optional<glm::dvec3> at = scene.eventPosition(a.actor, a.index)) {
            m_audio->setPosition(a.voice, *at);
        }
    }
    const u32 count = scene.eventCount();
    for (u32 i = 0; i < count; ++i) {
        const AudioScene::Event hit = scene.event(i);
        const audio::ModelEvent event = audio::classifyModelEventInfo(hit.id);
        if (event.kind == audio::ModelEventKind::None) {
            continue;
        }
        const bool own = hit.actor == f.playerActor;
        const u8 priority = own ? kPriorityPlayer : kPriorityWorld;
        switch (event.kind) {
        case audio::ModelEventKind::Footstep:
            if (m_modelSoundsOn) {
                eventFootstep(f, hit.actor, hit.position, actorSounds(f, hit.actor));
            }
            break;
        case audio::ModelEventKind::Fidget:
        case audio::ModelEventKind::Stand:
        case audio::ModelEventKind::CustomAttack:
        case audio::ModelEventKind::WingFlap:
        case audio::ModelEventKind::WingGlide: {
            if (!m_modelSoundsOn || (event.kind == audio::ModelEventKind::Stand && own)) {
                break; // $FDX: never the player's own character (0x6236B0)
            }
            if (const audio::CreatureSounds* sounds = actorSounds(f, hit.actor)) {
                const bool voice =
                    event.kind == audio::ModelEventKind::Fidget || event.kind == audio::ModelEventKind::Stand;
                playSound(audio::creatureEventSound(*sounds, event), voice ? Category::Voices : Category::Effects,
                          priority, &hit.position);
            }
            break;
        }
        case audio::ModelEventKind::EmoteSound:
            if (m_modelSoundsOn && m_options.emoteSounds) { // phase 15 (options): EmoteSounds
                playSound(hit.data, Category::Voices, own ? kPriorityPlayer : kPriorityVoice, &hit.position);
            }
            break;
        case audio::ModelEventKind::EmoteState: {
            // Emotes.dbc's sound of the unit's emote state (UNIT_NPC_EMOTESTATE: mining, chopping wood).
            u64 guid = 0;
            actorSounds(f, hit.actor, &guid);
            const game::Entity* unit = guid != 0 && f.entities ? f.entities->find(guid) : nullptr;
            if (m_modelSoundsOn && unit) {
                playSound(m_data->emoteStateSound(unit->value(uf::UNIT_NPC_EMOTESTATE)), Category::Effects, priority,
                          &hit.position);
            }
            break;
        }
        case audio::ModelEventKind::DoodadOneShot:
        case audio::ModelEventKind::Sound:
            if (m_modelSoundsOn) {
                playSound(hit.data, Category::Effects, priority, &hit.position);
            }
            break;
        case audio::ModelEventKind::DoodadLoop:
        case audio::ModelEventKind::DoodadStop:
            if (m_modelSoundsOn) {
                actorLoop(f, hit.actor, hit.index, hit.data, hit.position,
                          event.kind == audio::ModelEventKind::DoodadStop);
            }
            break;
        default: break;
        }
        ++m_modelStats.events;
    }
}

} // namespace vd
