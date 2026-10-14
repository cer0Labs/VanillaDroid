#include "audio/AudioSystem.h"
#include "audio/GroundSounds.h"
#include "audio/Mixer.h"
#include "audio/SoundData.h"
#include "pack/PackReader.h"
#include "pack/SoundFormat.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

using namespace vd;
using namespace vd::audio;

namespace {

bool openDb(pack::PackReader& db) {
    const char* dir = std::getenv("VD_PACKS_DIR");
    return dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok();
}

} // namespace

// The field numbers of the sound tables (SoundData.cpp), checked on rows whose meaning is known.
TEST_CASE("Sound tables: real rows of the 1.12.1 client (VD_PACKS_DIR)") {
    pack::PackReader db;
    if (!openDb(db)) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    SoundData data;
    REQUIRE(data.init(db));

    // SoundEntries: name, files under DirectoryBase, 3D range, volume.
    const SoundEntry* forest = data.entry(2523);
    REQUIRE(forest);
    CHECK(forest->name == "Zone-Forest Day");
    CHECK(forest->type == 28); // music
    CHECK(forest->fileCount == 3);
    CHECK(forest->files[0] == assetPathHash("Sound\\Music\\ZoneMusic\\Forest\\DayForest01.mp3"));
    CHECK(forest->minDistance == doctest::Approx(8.0f));
    CHECK(forest->maxDistance == doctest::Approx(45.0f));
    const SoundEntry* stormwind = data.entry(2532);
    REQUIRE(stormwind);
    CHECK(stormwind->fileCount == 8);
    CHECK(stormwind->volume == doctest::Approx(0.69f));
    // Picking is weighted by Freq and stays within the files.
    for (u32 r = 0; r < 50; ++r) {
        const u64 file = stormwind->pick(r * 2654435761u);
        CHECK(std::find(stormwind->files.begin(), stormwind->files.begin() + 8, file) !=
              stormwind->files.begin() + 8);
    }
    const SoundEntry* precast = data.entry(20);
    REQUIRE(precast);
    CHECK(precast->name == "Frost Precast");
    CHECK(precast->files[0] == assetPathHash("Sound\\Spells\\PreCastFrostMagicLow.wav"));
    // Interface sounds by the names FrameXML plays them with, any case.
    const SoundEntry* enterWorld = data.entryByName("gsTitleEnterWorld");
    REQUIRE(enterWorld);
    CHECK(enterWorld->id == 777);
    CHECK(data.entryByName("GSTITLEENTERWORLD") == enterWorld);
    CHECK(data.entryByName("igMainMenuOpen"));
    CHECK(data.entryByName("igQuestListOpen"));
    CHECK(data.entryByName("LEVELUPSOUND"));
    CHECK_FALSE(data.entryByName("NoSuchSound"));

    // Zones: Elwynn Forest (12) has the forest music and ambience; Goldshire (87) takes them from Elwynn;
    // Stormwind City (1519) and Orgrimmar (1637) have their city music and intro.
    const AreaSound elwynn = data.areaSound(12);
    CHECK(elwynn.music == 1);
    CHECK(elwynn.ambience[0] == 4183); // ForestNormalDay
    CHECK(elwynn.ambience[1] == 4184); // ForestNormalNight
    const ZoneMusicSet* forestSet = data.zoneMusic(elwynn.music);
    REQUIRE(forestSet);
    CHECK(forestSet->sound[0] == 2523);
    CHECK(forestSet->silenceMinMs[0] == 180000);
    CHECK(forestSet->silenceMaxMs[0] == 300000);
    const AreaSound goldshire = data.areaSound(87);
    CHECK(goldshire.music != 0);
    CHECK(goldshire.ambience[0] != 0);
    const AreaSound city = data.areaSound(1519);
    CHECK(city.music == 13);
    CHECK(city.intro == 61);
    const ZoneIntro* intro = data.zoneIntro(61);
    REQUIRE(intro);
    CHECK(intro->sound == 2541); // "Moment-ValleyofHeros Intro"
    CHECK(data.areaSound(1637).intro == 62);
    CHECK(data.zoneIntro(62)->sound == 3402);
    CHECK(data.areaSound(0).music == 0);

    // Creatures: the human male character's model sounds (CreatureModelData 49 -> CreatureSoundData 49).
    const CreatureSounds* human = data.creatureSounds(49);
    REQUIRE(human);
    CHECK(human->id == 49);
    CHECK(human->exertion == 2941); // HumanMalePlayerAttack
    CHECK(human->footstep == 7);
    CHECK(data.footstepId(49) == 7);
    CHECK(data.footstepId(54) == 8); // dwarf female
    const CreatureSounds* basilisk = nullptr;
    for (u32 display = 1; display < 12000 && !basilisk; ++display) {
        const CreatureSounds* c = data.creatureSounds(display);
        if (c && c->id == 1) {
            basilisk = c;
        }
    }
    REQUIRE(basilisk);
    CHECK(basilisk->exertion == 565); // BasiliskAttack
    CHECK(basilisk->injury == 567);
    CHECK(basilisk->injuryCritical == 568);
    CHECK(basilisk->death == 569);
    CHECK(basilisk->aggro == 570);
    CHECK(basilisk->customAttack[1] == 7374); // BiteMedium (column 19: its $AH1)

    // Combat: one-hand axe on flesh, critical; swings; parry and shield columns shared by every weapon.
    CHECK(data.weaponImpact(0, 0, false) == 171);
    CHECK(data.weaponImpact(0, 0, true) == 172);
    CHECK(data.weaponImpact(0, 2, false) == 175);
    CHECK(data.weaponImpact(4, 0, false) == 969); // metal mace (the wooden mace row, 941, is not used yet)
    CHECK(data.weaponImpact(SoundData::kUnarmed, 0, false) == 1014);
    CHECK(data.weaponImpact(99, 0, false) == 1014); // unknown weapon: unarmed
    CHECK(data.weaponSwing(0, false) == 233);
    CHECK(data.weaponSwing(2, true) == 238);
    CHECK(SoundData::swingType(15) == 0);
    CHECK(SoundData::swingType(7) == 1);
    CHECK(SoundData::swingType(8) == 2);

    // Footsteps: the characters' footstep id 7 on each terrain sound (TerrainType.SoundID), splash in water.
    CHECK(data.footstep(7, 0, false) == 560);
    CHECK(data.footstep(7, 2, false) == 564);
    CHECK(data.footstep(7, 2, true) == 1063);
    CHECK(data.footstep(8, 3, false) == 653); // small characters on stone
    CHECK(data.footstep(99, 0, false) == 0);

    // Spells: Frostbolt (116) -> SpellVisual 13 -> precast kit 194 (Frost Precast), cast 202, impact 4991.
    const SoundData::SpellSounds frostbolt = data.spellSounds(116);
    CHECK(frostbolt.precast == 20);
    CHECK(frostbolt.cast == 2559);
    CHECK(frostbolt.impact == 7);
    CHECK(frostbolt.missile == 3013); // MissileIceLoop
    CHECK(frostbolt.speed == doctest::Approx(28.0f)); // yards per second
    CHECK(data.spellSounds(585).speed == 0.0f);       // Smite: no missile
    CHECK(data.kitSound(194) == 20);
    CHECK(data.kitSound(0) == 0);
    CHECK(data.spellSounds(999999).cast == 0);

    // Voices.
    CHECK(data.npcSounds(1) == nullptr);
    CHECK(data.emoteSound(14, 1, 1) == 7951);
    CHECK(data.emoteSound(14, 1, 0) == 7952);
}

TEST_CASE("Sound pack: every SoundEntries file the client has is baked (VD_PACKS_DIR)") {
    pack::PackReader db;
    if (!openDb(db)) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    pack::PackReader sounds;
    if (!sounds.open(std::filesystem::path(std::getenv("VD_PACKS_DIR")) / "sound.vdpk").ok()) {
        MESSAGE("sound.vdpk missing (vd_baker bake --only sound): skipping");
        return;
    }
    SoundData data;
    REQUIRE(data.init(db));
    u32 files = 0, missing = 0, mp3 = 0;
    for (u32 id = 1; id < 10000; ++id) {
        const SoundEntry* e = data.entry(id);
        if (!e) {
            continue;
        }
        for (u32 f = 0; f < e->fileCount; ++f) {
            ++files;
            const std::optional<pack::PackAsset> asset = sounds.find(e->files[f]);
            SoundRef sound;
            if (!asset || !parseSound(asset->data, asset->size, sound)) {
                ++missing;
                continue;
            }
            mp3 += sound.codec == pack::SoundCodec::Mp3 ? 1 : 0;
        }
    }
    MESSAGE(files << " file references, " << missing << " not baked, " << mp3 << " to MP3s");
    // 24 references name files the client does not have, two rows are broken (see bakeSound).
    CHECK(files > 8900);
    CHECK(missing < 30);
    CHECK(mp3 > 150);
}

TEST_CASE("Ground sounds: the footstep surface of the terrain (VD_PACKS_DIR)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader sounds;
    if (!dir || !sounds.open(std::filesystem::path(dir) / "sound.vdpk").ok() || !sounds.find("ground/azeroth")) {
        MESSAGE("sound.vdpk with ground tables missing: skipping");
        return;
    }
    GroundSounds ground;
    CHECK(ground.soundAt(-9464.0, 62.0) == 0); // nothing until the table is read
    ground.setMap(sounds, "Azeroth");
    for (u32 wait = 0; wait < 500 && !ground.ready(); ++wait) {
        ground.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(ground.ready());
    // The surface most found around a place (cells are 8 yards: roads and patches show up in single points).
    const auto dominant = [&](f64 x, f64 y) {
        std::array<u32, 10> count{};
        for (i32 i = -6; i <= 6; ++i) {
            for (i32 j = -6; j <= 6; ++j) {
                const u8 sound = ground.soundAt(x + i * 10.0, y + j * 10.0);
                CHECK(sound <= 9);
                ++count[sound];
            }
        }
        return static_cast<u32>(std::max_element(count.begin(), count.end()) - count.begin());
    };
    CHECK(ground.soundAt(-9464.0, 62.0) == 3); // Goldshire's crossroads: cobblestone
    CHECK(dominant(-9300.0, -300.0) == 6);     // Elwynn's meadows: grass
    CHECK(dominant(-5601.0, -482.0) == 4);     // Dun Morogh: snow
    CHECK(ground.soundAt(1.0e6, 0.0) == 0);      // off the map
    // Another map, then none.
    ground.setMap(sounds, "kalimdor");
    CHECK_FALSE(ground.ready());
    for (u32 wait = 0; wait < 500 && !ground.ready(); ++wait) {
        ground.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(ground.ready());
    CHECK(dominant(-7177.0, -3785.0) == 8); // Tanaris: sand
    ground.setMap(sounds, "");
    CHECK_FALSE(ground.ready());
    // Malformed tables are refused.
    std::vector<u8> bad(sizeof(pack::GroundSoundsHeader), 0);
    CHECK_FALSE(GroundSounds::validate(bad.data(), static_cast<u32>(bad.size())));
    CHECK_FALSE(GroundSounds::validate(nullptr, 0));
}

TEST_CASE("Mixer cost: every voice busy with the client's sounds (VD_PACKS_DIR)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader sounds;
    if (!dir || !sounds.open(std::filesystem::path(dir) / "sound.vdpk").ok()) {
        MESSAGE("sound.vdpk missing: skipping");
        return;
    }
    // 46 effects (IMA ADPCM, mostly 22 kHz mono) looping and two music tracks (MP3), mixed at 48 kHz.
    Mixer mixer;
    mixer.init(48000);
    u32 voices = 0, mp3 = 0;
    for (u32 i = 0; i < sounds.entryCount() && voices < Mixer::kMaxVoices; ++i) {
        const pack::PackAsset asset = sounds.entry(i);
        SoundRef sound;
        if (asset.type != pack::AssetType::Sound || !parseSound(asset.data, asset.size, sound)) {
            continue;
        }
        const bool isMp3 = sound.codec == pack::SoundCodec::Mp3;
        if ((isMp3 && mp3 >= 2) || (!isMp3 && voices - mp3 >= Mixer::kMaxVoices - 2)) {
            continue;
        }
        mp3 += isMp3 ? 1 : 0;
        mixer.setGains(voices, 0.02f, 0.02f);
        REQUIRE(mixer.start({sound, voices, voices + 1, Category::Effects, true, 0.0f}));
        ++voices;
    }
    REQUIRE(voices == Mixer::kMaxVoices);
    std::vector<f32> out(2 * 480);
    const auto begin = std::chrono::steady_clock::now();
    for (u32 period = 0; period < 1000; ++period) { // 10 s of audio in 10 ms periods
        mixer.render(out.data(), 480);
    }
    const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - begin).count();
    MESSAGE("48 voices (" << mp3 << " MP3): " << seconds * 1000.0 / 10.0 << " ms per second of audio ("
                          << seconds * 10.0 << "% of a core)");
    CHECK(mixer.activeVoices() == Mixer::kMaxVoices);
    CHECK(seconds < 5.0); // far below real time even on a slow machine
}
