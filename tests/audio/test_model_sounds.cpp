// Phase 14 (sound): the models' sounds. The rules of WoW.exe's animation event handlers (audio/ModelSoundRules), the
// tables they read (audio/SoundData), and app/GameAudio playing doodads, emitters, the actors' events and the rooms
// of buildings over a fake world (AudioScene) on the null backend (nothing reaches the speakers), with the baked
// packs (VD_PACKS_DIR: db.vdpk, sound.vdpk, worldsound.vdpk).
#include "app/GameAudio.h"
#include "app/GameAudioScene.h"
#include "audio/AudioSystem.h"
#include "audio/ModelSoundRules.h"
#include "audio/SoundData.h"
#include "audio/Spatial.h"
#include "audio/WorldSoundTables.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

using namespace vd;

TEST_CASE("Model sounds: WoW.exe's event ids") {
    using audio::classifyModelEventInfo;
    using audio::ModelEventKind;
    CHECK(classifyModelEventInfo(audio::modelEventId("$FSD")).kind == ModelEventKind::Footstep);
    CHECK(classifyModelEventInfo(audio::modelEventId("$DSL")).kind == ModelEventKind::DoodadLoop);
    CHECK(classifyModelEventInfo(audio::modelEventId("$DSO")).kind == ModelEventKind::DoodadOneShot);
    CHECK(classifyModelEventInfo(audio::modelEventId("$DSE")).kind == ModelEventKind::DoodadStop);
    CHECK(classifyModelEventInfo(audio::modelEventId("$SND")).kind == ModelEventKind::Sound);
    CHECK(classifyModelEventInfo(audio::modelEventId("$CSD")).kind == ModelEventKind::EmoteSound);
    CHECK(classifyModelEventInfo(audio::modelEventId("$ESD")).kind == ModelEventKind::EmoteState);
    CHECK(classifyModelEventInfo(audio::modelEventId("$WNG")).kind == ModelEventKind::WingFlap);
    CHECK(classifyModelEventInfo(audio::modelEventId("$WGG")).kind == ModelEventKind::WingGlide);
    CHECK(classifyModelEventInfo(audio::modelEventId("$FDX")).kind == ModelEventKind::Stand);
    // $FD1..$FD4: fidgets 0..3; $FD5..$FD9 and $FD0 play nothing in 1.12 (0x6230A0 answers below 4 only).
    for (u32 n = 1; n <= 4; ++n) {
        const char id[5] = {'$', 'F', 'D', static_cast<char>('0' + n), 0};
        const audio::ModelEvent e = classifyModelEventInfo(audio::modelEventId(id));
        CHECK(e.kind == ModelEventKind::Fidget);
        CHECK(e.index == n - 1);
    }
    CHECK(classifyModelEventInfo(audio::modelEventId("$FD5")).kind == ModelEventKind::None);
    CHECK(classifyModelEventInfo(audio::modelEventId("$FD0")).kind == ModelEventKind::None);
    // $AH0..$AH3: the custom attacks.
    CHECK(classifyModelEventInfo(audio::modelEventId("$AH0")).index == 0);
    CHECK(classifyModelEventInfo(audio::modelEventId("$AH3")).index == 3);
    CHECK(classifyModelEventInfo(audio::modelEventId("$AH3")).kind == ModelEventKind::CustomAttack);
    // The feet's own events (footprints, sprays), breath, combat timing: no sound here.
    for (const char* id : {"$RL0", "$FL0", "$BTH", "$CSS", "$HIT", "$DTH", "$SHK"}) {
        char text[5] = {};
        for (u32 i = 0; i < 4; ++i) {
            text[i] = id[i];
        }
        CHECK(classifyModelEventInfo(audio::modelEventId(text)).kind == ModelEventKind::None);
    }
    // The same id the actors' pack stores ('$' in the low byte).
    CHECK(audio::modelEventId("$FSD") == 0x44534624u);

    audio::CreatureSounds wolf;
    wolf.fidget = {1018, 0, 0, 0};
    wolf.customAttack = {3176, 3176, 3176, 3176};
    wolf.stand = 7;
    wolf.wingFlap = 8;
    CHECK(audio::creatureEventSound(wolf, {ModelEventKind::Fidget, 0}) == 1018);
    CHECK(audio::creatureEventSound(wolf, {ModelEventKind::Fidget, 1}) == 0);
    CHECK(audio::creatureEventSound(wolf, {ModelEventKind::CustomAttack, 2}) == 3176);
    CHECK(audio::creatureEventSound(wolf, {ModelEventKind::Stand, 0}) == 7);
    CHECK(audio::creatureEventSound(wolf, {ModelEventKind::WingFlap, 0}) == 8);
    CHECK(audio::creatureEventSound(wolf, {ModelEventKind::Footstep, 0}) == 0);
}

TEST_CASE("Model sounds: the doodads' animation clock") {
    // A frame from 900 to 1200 ms of a 2200 ms cycle crosses an event at 1100, not at 0 nor at 1200 exactly twice.
    CHECK(audio::crossed(900, 1200, 1100, 2200));
    CHECK_FALSE(audio::crossed(900, 1200, 0, 2200));
    CHECK(audio::crossed(900, 1200, 1200, 2200));
    CHECK_FALSE(audio::crossed(1200, 1500, 1200, 2200));
    // Wrapping: 2100 -> 100 crosses 0 and 50.
    CHECK(audio::crossed(2100, 100, 0, 2200));
    CHECK(audio::crossed(2100, 100, 50, 2200));
    CHECK_FALSE(audio::crossed(2100, 100, 1100, 2200));
    CHECK_FALSE(audio::crossed(5, 5, 5, 2200)); // no time passed
    CHECK(audio::cycleTime(4500, 2200) == 100);
    // The 1.12 rolloff: a footstep (min 4) 7 yards from a listener at the head is at a quarter of its volume.
    CHECK(audio::distanceGain(7.0f, 4.0f, 45.0f) == doctest::Approx(0.25f));
}

TEST_CASE("Model sounds: the doodads' sounds baked per map, around Goldshire (VD_PACKS_DIR)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    audio::WorldSoundTables tables;
    if (!dir || !tables.init(dir)) {
        MESSAGE("VD_PACKS_DIR without worldsound.vdpk: skipping");
        return;
    }
    tables.setMap("Azeroth");
    const Clock::time_point start = Clock::now();
    while (tables.doodadSounds().empty() && secondsBetween(start, Clock::now()) < 5.0) {
        tables.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(tables.doodadSounds().empty());
    // The two bellows of Goldshire's smithy (the WMO's doodad set): out at 0 ms and in at 1100 ms of their 2 s cycle,
    // up on the model; and four loops (FlyBuzzLoop: flies) south-west of the town within 250 yards, nothing more.
    std::array<u32, 64> near{};
    u32 count = tables.doodadSoundsNear(-9440.0, 118.0, 20.0f, near);
    REQUIRE(count == 4);
    for (u32 i = 0; i < count; ++i) {
        const pack::DoodadSound& r = tables.doodadSounds()[near[i]];
        CHECK(r.kind == pack::kModelSoundTimed);
        CHECK(r.cycleMs == 2000);
        CHECK((r.sound == 8673 ? r.timeMs == 0u : r.sound == 8672 && r.timeMs == 1100u));
        CHECK(r.position[2] > 60.0f);
    }
    count = tables.doodadSoundsNear(-9460.0, 94.0, 250.0f, near);
    u32 loops = 0;
    for (u32 i = 0; i < count; ++i) {
        loops += tables.doodadSounds()[near[i]].kind == pack::kModelSoundLoop ? 1 : 0;
    }
    CHECK(count == 8);
    CHECK(loops == 4);
    // The query is a circle: a radius that leaves the bellows 3 yards out finds nothing.
    CHECK(tables.doodadSoundsNear(-9433.61, 109.0, 2.5f, near) == 0);
    CHECK(tables.doodadSoundsNear(-9433.61, 109.0, 3.5f, near) == 2);
}

namespace {

// A world of a few actors (the doodads' sounds come from the baked tables).
struct FakeScene final : AudioScene {
    std::vector<Event> events;
    std::vector<std::pair<u32, u64>> actorModels; // actor -> model hash
    u32 parent = ~0u;
    f32 ground = 0.0f;

    u32 eventCount() const override { return static_cast<u32>(events.size()); }
    Event event(u32 i) const override { return events[i]; }
    u64 actorModel(u32 actor) const override {
        for (const auto& [a, model] : actorModels) {
            if (a == actor) {
                return model;
            }
        }
        return 0;
    }
    u32 actorParent(u32) const override { return parent; }
    std::optional<glm::dvec3> eventPosition(u32, u32) const override { return std::nullopt; }
    u32 unitActor(u64) const override { return ~0u; }
    std::optional<f32> groundHeight(f64, f64) const override { return ground; }
};

constexpr f64 kX = -9460.0, kY = 94.0, kZ = 56.5; // Goldshire's smithy

struct Fixture {
    pack::PackReader db;
    audio::AudioSystem system;
    audio::SoundData data;
    GameAudio sounds;
    FakeScene scene;
    u32 now = 1000;
    bool ready = false;

    Fixture() {
        const char* dir = std::getenv("VD_PACKS_DIR");
        if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok() ||
            !std::filesystem::exists(std::filesystem::path(dir) / "sound.vdpk") ||
            !std::filesystem::exists(std::filesystem::path(dir) / pack::kWorldSoundPack)) {
            return;
        }
        audio::AudioConfig config;
        config.output = audio::AudioOutput::Null;
        REQUIRE(system.init(config, dir).ok());
        REQUIRE(data.init(db));
        sounds.init(&system, &data);
        sounds.initWorldSounds(dir);
        ready = sounds.active();
    }
    GameAudioFrame frame(const glm::dvec3& listener) {
        GameAudioFrame f;
        f.nowMs = now;
        f.dt = 1.0f / 60.0f;
        f.listener = listener;
        f.camera = listener;
        f.world = true;
        f.map = "Azeroth";
        f.area = 87; // Goldshire
        f.hours = 12.0f;
        f.scene = &scene;
        return f;
    }
    void update(const GameAudioFrame& f) {
        sounds.update(f);
        system.update();
        now += 16;
    }
    bool played(u32 soundEntry, u32 within = 12) const {
        for (u32 i = 0; i < within; ++i) {
            if (sounds.recentSound(i) == soundEntry) {
                return true;
            }
        }
        return false;
    }
    // Updates until the doodads' table has been read (its IO thread), at most two seconds.
    void waitForTables(const glm::dvec3& listener) {
        const Clock::time_point start = Clock::now();
        while (secondsBetween(start, Clock::now()) < 2.0) {
            update(frame(listener));
            if (sounds.modelSoundStats().doodads > 0) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

} // namespace

TEST_CASE("Model sounds: doodads, actors' events and rooms through GameAudio (VD_PACKS_DIR)") {
    Fixture f;
    if (!f.ready) {
        MESSAGE("VD_PACKS_DIR without db.vdpk, sound.vdpk and worldsound.vdpk: skipping");
        return;
    }
    const audio::SoundData& data = f.data;
    // The tables: a creature's sounds by its model, Emotes' state sounds, SoundAmbience.
    const audio::CreatureSounds* wolf = data.modelSounds(assetPathHash("Creature\\Wolf\\Wolf.m2"));
    REQUIRE(wolf);
    CHECK(wolf->id == 43);
    CHECK(wolf->fidget[0] == 1018); // WolfFidget2
    const audio::CreatureSounds* human = data.modelSounds(assetPathHash("Character\\Human\\Male\\HumanMale.m2"));
    REQUIRE(human);
    CHECK(human->footstep == 7);
    CHECK(data.emoteStateSound(233) == 3782); // STATE_WORK_NOSHEATHE_MINING: Mining
    CHECK(data.emoteStateSound(234) == 3202); // chopping wood
    CHECK(data.emoteStateSound(28) == 0);     // STATE_WORK: none
    CHECK(data.ambience(170)[0] == 4394);     // Tavern
    CHECK(data.ambience(54)[0] == 4218);      // BlackSmith

    // Goldshire's smithy (its WMO's doodad set): the bellows breathe on their cycle beside the listener.
    const glm::dvec3 bellows(-9436.0, 115.0, 61.0);
    f.waitForTables(bellows);
    REQUIRE(f.sounds.modelSoundStats().doodads > 0);
    bool breathedIn = false, breathedOut = false;
    for (u32 i = 0; i < 300 && !(breathedIn && breathedOut); ++i) {
        f.update(f.frame(bellows));
        breathedOut = breathedOut || f.played(8673, 2);
        breathedIn = breathedIn || f.played(8672, 2);
    }
    CHECK(breathedOut);
    CHECK(breathedIn);
    // The flies south-west of the town (FlyBuzzLoop) start beside them, and stop 200 yards away.
    const glm::dvec3 water(-9406.0, -115.0, 59.0);
    for (u32 i = 0; i < 40 && f.sounds.modelSoundStats().loops == 0; ++i) {
        f.update(f.frame(water));
    }
    CHECK(f.sounds.modelSoundStats().loops > 0);
    CHECK(f.played(1575, 40));
    const glm::dvec3 away(-9406.0 + 200.0, -115.0, 59.0);
    for (u32 i = 0; i < 40; ++i) {
        f.update(f.frame(away));
    }
    CHECK(f.sounds.modelSoundStats().loops == 0);
    CHECK(f.sounds.modelSoundStats().doodads == 0);
    const glm::dvec3 here(kX, kY, kZ + 2.0);

    // A footstep of the player's character (its model: a human's) on dirt.
    constexpr u32 kPlayer = 7, kWolf = 8, kMount = 9;
    f.scene.actorModels = {{kPlayer, assetPathHash("Character\\Human\\Male\\HumanMale.m2")},
                           {kWolf, assetPathHash("Creature\\Wolf\\Wolf.m2")},
                           {kMount, assetPathHash("Creature\\RidingHorse\\RidingHorse.m2")}};
    f.scene.ground = static_cast<f32>(kZ);
    GameAudioFrame step = f.frame(here);
    step.playerActor = kPlayer;
    f.scene.events = {{kPlayer, audio::modelEventId("$FSD"), 0, 0, glm::dvec3(kX, kY, kZ)}};
    const u32 footsteps = f.sounds.modelSoundStats().footsteps;
    f.update(step);
    CHECK(f.sounds.modelSoundStats().footsteps == footsteps + 1);
    // The wolf's fidget ($FD1 on its idle variation), its custom attack ($AH0), a clap ($CSD's data word).
    f.scene.events = {{kWolf, audio::modelEventId("$FD1"), 0, 0, glm::dvec3(kX + 5.0, kY, kZ)}};
    f.update(step);
    CHECK(f.played(1018, 4));
    f.scene.events = {{kWolf, audio::modelEventId("$AH0"), 0, 0, glm::dvec3(kX + 5.0, kY, kZ)}};
    f.update(step);
    CHECK(f.played(3176, 4));
    f.scene.events = {{kPlayer, audio::modelEventId("$CSD"), 6576, 0, glm::dvec3(kX, kY, kZ + 1.0)}};
    f.update(step);
    CHECK(f.played(6576, 4)); // ClapSounds
    // Others' footsteps: a few at a time, none far away; VD_OTHER_FOOTSTEPS' rule is the budget.
    f.scene.events.clear();
    for (u32 i = 0; i < 10; ++i) {
        f.scene.events.push_back({kWolf, audio::modelEventId("$FSD"), 0, 0, glm::dvec3(kX + 3.0, kY + i, kZ)});
    }
    const u32 before = f.sounds.modelSoundStats().footsteps;
    f.update(step);
    CHECK(f.sounds.modelSoundStats().footsteps - before == audio::kMaxOtherFootsteps);
    f.scene.events = {{kWolf, audio::modelEventId("$FSD"), 0, 0, glm::dvec3(kX + 200.0, kY, kZ)}};
    for (u32 i = 0; i < 40; ++i) {
        f.update(step); // the six end
    }
    const u32 far = f.sounds.modelSoundStats().footsteps;
    f.update(step);
    CHECK(f.sounds.modelSoundStats().footsteps == far);
    // Mounted: the mount's hooves are the player's own (FootstepHorse*: the horse's footstep id 10).
    f.scene.parent = kMount;
    f.scene.events = {{kMount, audio::modelEventId("$FSD"), 0, 0, glm::dvec3(kX, kY, kZ)}};
    const u32 hooves = f.sounds.modelSoundStats().footsteps;
    f.update(step);
    CHECK(f.sounds.modelSoundStats().footsteps == hooves + 1);
    bool horse = false;
    for (u32 i = 0; i < 4; ++i) {
        const audio::SoundEntry* e = data.entry(f.sounds.recentSound(i));
        horse = horse || (e && e->name.starts_with("FootstepHorse"));
    }
    CHECK(horse);
    f.scene.parent = ~0u;
    f.scene.events.clear();

    // A building's room: the tavern's music and ambience replace Goldshire's.
    GameAudioFrame inn = f.frame(here);
    inn.interiorKey = 71414;
    inn.interiorMusic = 156;
    inn.interiorAmbience = 170;
    for (u32 i = 0; i < 10; ++i) {
        f.update(inn);
    }
    CHECK(f.sounds.ambienceEntry() == 4394); // Tavern
    CHECK(f.sounds.musicEntry() == 4516);    // Zone-TavernAlliance
}
