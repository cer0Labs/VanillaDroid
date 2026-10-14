// Phase 14 (spellfx): the glue of the kits' procs and shakes (game/SpellKitFx) in the frame: the camera
// shakes applied to the rendered view, and the beams of channels and chains and the fishing lines written
// after the particles into the same pass. Part of Game (see Game.h); the rest of the frame is in Game.cpp.
#include "app/Game.h"

#include "app/Crowd.h"
#include "core/DevEnv.h"
#include "core/Profile.h"

#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vd {
namespace {

bool switchedOff(const char* name) {
    const char* value = VD_DEV_ENV(name);
    return value && std::strcmp(value, "0") == 0;
}

} // namespace

void Game::initSpellFx() {
    m_spellKitFx.init(m_dbPack);
    m_beamWriter.init();
    m_particleDraws.reserve(m_particleDraws.capacity() + 256); // the beams' runs
    // Development switches for before/after captures.
    m_cameraShakes.enabled = !switchedOff("VD_CAMERA_SHAKE");
    m_spellBeamsOn = !switchedOff("VD_BEAMS");
    m_kitTintsOn = !switchedOff("VD_KIT_FLASH");
    m_effects.particles().settings().meshes = !switchedOff("VD_MODEL_PARTICLES"); // emitters of models
    const char* fishing = VD_DEV_ENV("VD_FISHING_DEMO");
    m_fishingDemo = fishing && std::strcmp(fishing, "0") != 0;
    m_spellEffects.setKitFx(&m_spellKitFx, &m_cameraShakes);
    m_fight.setKitFx(&m_spellKitFx, &m_cameraShakes, &m_unitFades);
}

void Game::addEventShakes(u32 nowMs) {
    // The models' events the last frame crossed: a $SHK starts its SpellEffectCameraShakes (0x5D58C0), and
    // the footsteps and death thud ($DTH, 0x625C30) of big creatures (a kodo, a giant: their
    // CreatureModelData) shake.
    static constexpr u32 kShake = pack::actorEventId("$SHK"), kDeathThud = pack::actorEventId("$DTH");
    for (const Actors::EventHit& e : m_actors.events()) {
        if (e.id == kShake) {
            m_cameraShakes.addEffect(m_spellKitFx, e.data, e.position, nowMs);
            continue;
        }
        if (e.id != kDeathThud && !footstepEvent(e.id)) {
            continue;
        }
        const game::ActorLook* look = m_actors.look(e.actor);
        const u32 id = !look                ? 0
                       : e.id == kDeathThud ? m_spellKitFx.deathShake(look->model)
                                            : m_spellKitFx.footstepShake(look->model);
        if (const game::CameraShake* shake = m_spellKitFx.cameraShake(id)) {
            m_cameraShakes.add(*shake, e.position, nowMs);
        }
    }
}

glm::mat4 Game::cameraShake(u32 nowMs) {
    VD_PROFILE_ZONE("Game::cameraShake");
    const Clock::time_point start = Clock::now();
    addEventShakes(nowMs);
    // The camera follows the player: its shakes go along the character's facing (the free camera's offline).
    const f32 yaw = m_player.active() ? m_player.character().facing() : m_camera.yaw;
    glm::vec3 offset = m_cameraShakes.sample(nowMs, m_camera.position, yaw);
    if (m_cinematics.active()) {
        offset = glm::vec3(0.0f); // the cinematic's camera is the client's own flight: not shaken
    }
    m_spellFxMsSum += secondsBetween(start, Clock::now()) * 1000.0;
    // Moving the eye by `offset` moves every camera-relative position by -offset: the gameplay camera stays.
    return glm::translate(glm::mat4(1.0f), -offset);
}

void Game::updateFishingDemo() {
    if (m_fishingDemoActor != ~0u || !m_actors.isOpen() || !m_terrain.isOpen() ||
        !m_terrain.heightAt(m_camera.position.x, m_camera.position.y)) {
        return;
    }
    // A character 10 yards ahead and 4 to the left, facing right across the view with a fishing pole in its
    // hands (FishingLoop), and the bobber (GameObjectDisplayInfo 668) 9 yards ahead of it: the line from the
    // side.
    const game::AppearanceDb& db = m_actors.appearance();
    u32 pole = 0;
    for (u32 display : db.weaponDisplays()) {
        std::string model(db.itemModel(display));
        for (char& c : model) {
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
        if (model.find("fishingpole") != std::string::npos) {
            pole = display;
            break;
        }
    }
    u32 random = 1234;
    game::CharacterLook look = Crowd::randomCharacter(db, random);
    look.items[game::kSlotMainHand] = pole;
    look.inventoryTypes[game::kSlotMainHand] = 17; // two-hand
    const f32 facing = m_camera.yaw - 1.5707963f;
    const glm::dvec3 across(std::cos(facing), std::sin(facing), 0.0);
    const auto ground = [&](glm::dvec3 at) {
        at.z = m_terrain.heightAt(at.x, at.y).value_or(static_cast<f32>(m_camera.position.z - 2.0));
        return at;
    };
    const glm::dvec3 feet = ground(groundInFront(10.0) - across * 4.0);
    m_fishingDemoActor = m_actors.create();
    m_actors.setLook(m_fishingDemoActor, db.character(look, false));
    m_actors.setPlacement(m_fishingDemoActor, feet, facing, 1.0f);
    m_actors.play(m_fishingDemoActor, 134, true, 1.0f); // FishingLoop
    m_fishingDemoBobber = ground(feet + across * 9.0) + glm::dvec3(0.0, 0.0, 0.2);
    const u32 nowMs = static_cast<u32>(secondsBetween(m_startTime, Clock::now()) * 1000.0);
    m_serverObjects.spawnDemo(668, m_fishingDemoBobber - glm::dvec3(0.0, 0.0, 0.2), facing, nowMs);
    VD_LOG_INFO("VD_FISHING_DEMO: a character fishing with pole display %u", pole);
}

TextureHandle Game::spellBeamTexture(void* game, u64 hash) {
    return static_cast<Game*>(game)->m_uiResources.texture(hash);
}

u32 Game::addSpellBeams(u32 quads, f32 viewportHeight) {
    VD_PROFILE_ZONE("Game::addSpellBeams");
    const Clock::time_point start = Clock::now();
    for (SpellVisuals* visuals : {&m_spellEffects.visuals(), &m_fight.visuals()}) {
        visuals->beamsEnabled = m_spellBeamsOn;
        visuals->tintsEnabled = m_kitTintsOn;
        visuals->shakesEnabled = m_cameraShakes.enabled;
    }
    if (!m_uiResources.ready()) {
        return quads;
    }
    const f32 pixelsPerYard = viewportHeight / (2.0f * std::tan(m_camera.fovY * 0.5f));
    m_beamWriter.begin(m_camera.position, m_camera.forward(), pixelsPerYard, viewportHeight);
    if (m_spellBeamsOn) {
        m_beamWriter.addBeams(m_spellEffects.visuals().beams(), &Game::spellBeamTexture, this);
        m_beamWriter.addBeams(m_fight.visuals().beams(), &Game::spellBeamTexture, this);
        if (m_fishingDemo && !m_session.inWorld()) {
            updateFishingDemo();
            const std::optional<glm::dvec3> tip = m_actors.itemTip(m_fishingDemoActor, game::kSlotMainHand);
            if (tip) {
                m_beamWriter.addLine(*tip, m_fishingDemoBobber);
            }
        }
        // Fishing (WoW.exe 0x61F780): from the pole's tip to the middle of the bobber.
        for (const SpellEffects::FishingLine& line : m_spellEffects.fishingLines()) {
            const std::optional<glm::dvec3> tip = m_actors.itemTip(line.actor, game::kSlotMainHand);
            for (const WorldObjectView& view : m_objectViews) {
                if (tip && view.guid == line.bobber) {
                    m_beamWriter.addLine(*tip, view.center);
                }
            }
        }
    }
    quads = m_beamWriter.finish(m_renderer.particleVertices(), quads, m_particleDraws);
    const f64 ms = secondsBetween(start, Clock::now()) * 1000.0;
    m_spellFxMsSum += ms;
    m_spellFxMsMax = std::max(m_spellFxMsMax, ms);
    m_beamQuadsMax = std::max(m_beamQuadsMax, m_beamWriter.quads());
    ++m_spellFxFrames;
    return quads;
}

void Game::logSpellFx() {
    const SpellVisuals::Stats& spells = m_spellEffects.visuals().stats();
    const SpellVisuals::Stats& fight = m_fight.visuals().stats();
    VD_LOG_INFO("spellfx: cpu %.3f/%.3f ms avg/max (beams, lines, shakes) | %u beams in %u quads (max %u), "
                "%u + %u tints, %u shakes now (%u + %u started)",
                m_spellFxFrames ? m_spellFxMsSum / m_spellFxFrames : 0.0, m_spellFxMsMax,
                m_beamWriter.beams(), m_beamWriter.quads(), m_beamQuadsMax, spells.tints, fight.tints,
                m_cameraShakes.active(), spells.shakes, fight.shakes);
    m_spellFxMsSum = 0.0;
    m_spellFxMsMax = 0.0;
    m_spellFxFrames = 0;
    m_beamQuadsMax = 0;
}

} // namespace vd
