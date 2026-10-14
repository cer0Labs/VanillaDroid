// The game's sounds (phase 9): app/GameAudio over a real AudioSystem on the null backend (nothing reaches the
// speakers) and the baked packs (VD_PACKS_DIR: db.vdpk, sound.vdpk), driven by game::Gameplay with server packets
// written as vmangos-core writes them.
#include "app/GameAudio.h"
#include "audio/AudioSystem.h"
#include "audio/SoundData.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"
#include "physics/CollisionWorld.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <thread>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x11;
constexpr u64 kNpc = 0xF130000000000123ull;
constexpr u64 kWolf = 0xF130000000000456ull;
constexpr f32 kX = -9464.0f, kY = 62.0f, kZ = 56.0f; // Goldshire

Bytes created(u64 guid, game::ObjectType type, Fields fields, f32 x, f32 y, f32 z) {
    std::sort(fields.begin(), fields.end());
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(x).f32v(y).f32v(z).f32v(0.0f);
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    std::vector<u32> mask(maxIndex / 32 + 1, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(mask.size()));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
    return out;
}

struct Fixture {
    pack::PackReader db;
    audio::AudioSystem system;
    audio::SoundData data;
    GameAudio sounds;
    game::EntityStore store;
    game::Gameplay play{store};
    u32 now = 1000;
    bool ready = false;

    Fixture() {
        const char* dir = std::getenv("VD_PACKS_DIR");
        if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok() ||
            !std::filesystem::exists(std::filesystem::path(dir) / "sound.vdpk")) {
            return;
        }
        audio::AudioConfig config;
        config.output = audio::AudioOutput::Null;
        REQUIRE(system.init(config, dir).ok());
        REQUIRE(data.init(db));
        sounds.init(&system, &data);
        ready = sounds.active();
        play.setSink({[](void*, Opcode, std::span<const u8>) { return true; }, nullptr});
    }

    GameAudioFrame frame(u32 area, f32 hours, bool online = false) {
        GameAudioFrame f;
        f.nowMs = now;
        f.dt = 1.0f / 60.0f;
        f.listener = glm::dvec3(kX, kY, kZ + 2.0);
        f.world = true;
        f.area = area;
        f.hours = hours;
        if (online) {
            f.play = &play;
            f.entities = &store;
            f.player = kPlayer;
        }
        return f;
    }
    void update(const GameAudioFrame& f) {
        sounds.update(f);
        system.update();
        now += 16;
    }
    // The sounds started since `count` updates ago, newest first (up to 12).
    bool played(u32 soundEntry, u32 within = 12) const {
        for (u32 i = 0; i < within; ++i) {
            if (sounds.recentSound(i) == soundEntry) {
                return true;
            }
        }
        return false;
    }
    void apply(const Bytes& update) {
        std::vector<u64> created;
        game::UpdateStats stats;
        stats.createdGuids = &created;
        REQUIRE(store.applyUpdateObject(update, now, &stats));
        play.afterUpdate(created, now);
    }
};

// A values update (UPDATETYPE_VALUES) of one object.
Bytes values(u64 guid, Fields fields) {
    std::sort(fields.begin(), fields.end());
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    std::vector<u32> mask(maxIndex / 32 + 1, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(mask.size()));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
    return out;
}

// SMSG_ITEM_QUERY_SINGLE_RESPONSE (HandleItemQuerySingleOpcode's layout) of a template with the class, display and
// material the item sounds need.
Bytes itemQuery(u32 entry, u32 itemClass, u32 subclass, u32 displayId, i32 material) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(entry).u32v(itemClass).u32v(subclass).cstring("Item").cstring("").cstring("").cstring("");
    w.u32v(displayId).u32v(1).u32v(0).u32v(0).u32v(0).u32v(13);
    w.u32v(0xFFFFFFFFu).u32v(0xFFFFFFFFu).u32v(5).u32v(0);
    for (u32 i = 0; i < 8; ++i) {
        w.u32v(0);
    }
    w.u32v(1).u32v(0);
    for (u32 i = 0; i < 10; ++i) {
        w.u32v(0).u32v(0);
    }
    for (u32 i = 0; i < 5; ++i) {
        w.f32v(0.0f).f32v(0.0f).u32v(0);
    }
    for (u32 i = 0; i < 7; ++i) {
        w.u32v(0);
    }
    w.u32v(0).u32v(0).f32v(0.0f);
    for (u32 i = 0; i < 5; ++i) {
        w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    }
    w.u32v(0).cstring("").u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(static_cast<u32>(material)).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    return out;
}

#define VD_REQUIRE_SOUND(f)                                                                                  \
    Fixture f;                                                                                               \
    if (!f.ready) {                                                                                          \
        MESSAGE("VD_PACKS_DIR not set or sound.vdpk missing: skipping");                                     \
        return;                                                                                              \
    }

} // namespace

TEST_CASE("Game audio: zone music, ambience and city intros by area and time of day") {
    VD_REQUIRE_SOUND(f);
    // Elwynn Forest (12) at noon: the forest's day music and ambience.
    f.update(f.frame(12, 12.0f));
    CHECK(f.sounds.musicEntry() == 2523); // Zone-Forest Day
    CHECK(f.sounds.musicPlaying());
    CHECK(f.sounds.ambienceEntry() == 4183); // ForestNormalDay
    // Night falls: the ambience changes, the track playing goes on.
    f.update(f.frame(12, 23.0f));
    CHECK(f.sounds.ambienceEntry() == 4184); // ForestNormalNight
    CHECK(f.sounds.musicEntry() == 2523);
    // Goldshire (87, a subzone) keeps Elwynn's sounds.
    f.update(f.frame(87, 23.0f));
    CHECK(f.sounds.ambienceEntry() == 4184);
    CHECK(f.sounds.musicEntry() == 2523);
    // Stormwind City (1519): its intro (ZoneIntroMusicTable 61) and the city's ambience.
    f.update(f.frame(1519, 12.0f));
    CHECK(f.sounds.musicEntry() == 2541); // Moment-ValleyofHeros Intro
    CHECK(f.sounds.ambienceEntry() == 4176);
    // Out and back in at once: the intro does not play twice within its minimum delay.
    f.update(f.frame(12, 12.0f));
    f.update(f.frame(1519, 12.0f));
    CHECK(f.sounds.musicEntry() == 2541); // the first one goes on
    CHECK_FALSE(f.played(2541, 1));
    // Unknown area (tile not loaded yet): nothing changes.
    f.update(f.frame(0, 12.0f));
    CHECK(f.sounds.ambienceEntry() == 4176);
    // The login screen: the main theme replaces it all; the world's ambience stops.
    GameAudioFrame login = f.frame(0, 12.0f);
    login.login = true;
    login.world = false;
    f.update(login);
    CHECK(f.sounds.musicEntry() == 0);
    CHECK(f.sounds.musicPlaying());
    CHECK(f.sounds.ambienceEntry() == 0);
    // Background: everything stops; back in front, music and ambience start again at once.
    f.update(f.frame(12, 12.0f));
    CHECK(f.sounds.musicEntry() == 2523);
    f.system.setPaused(true);
    f.update(f.frame(12, 12.0f));
    CHECK_FALSE(f.sounds.musicPlaying());
    f.system.setPaused(false);
    f.update(f.frame(12, 12.0f));
    CHECK(f.sounds.musicEntry() == 2523);
    CHECK(f.sounds.ambienceEntry() == 4183);
}

TEST_CASE("Game audio: interface sounds, NPC voices, targets, combat and spells from the game state") {
    VD_REQUIRE_SOUND(f);
    // A display with NPC greetings, and one with creature sounds (wounds, death).
    u32 npcDisplay = 0, wolfDisplay = 0;
    for (u32 d = 1; d < 15000 && (!npcDisplay || !wolfDisplay); ++d) {
        const audio::NpcSounds* npc = f.data.npcSounds(d);
        if (!npcDisplay && npc && npc->greeting && npc->farewell && npc->pissed) {
            npcDisplay = d;
        }
        const audio::CreatureSounds* c = f.data.creatureSounds(d);
        if (!wolfDisplay && c && c->injury && c->injuryCritical && c->death && c->aggro) {
            wolfDisplay = d;
        }
    }
    REQUIRE(npcDisplay != 0);
    REQUIRE(wolfDisplay != 0);
    f.play.enterWorld(kPlayer, 0, true, f.now);
    f.apply(created(kPlayer, game::ObjectType::Player,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_BYTES_0, 1 /* human male */},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, 1}, {uf::UNIT_FIELD_LEVEL, 3}, {uf::UNIT_FIELD_DISPLAYID, 49}},
                    kX, kY, kZ));
    f.apply(created(kNpc, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_DISPLAYID, npcDisplay},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, 12}},
                    kX + 3.0f, kY, kZ));
    f.apply(created(kWolf, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_DISPLAYID, wolfDisplay},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, 32}},
                    kX, kY + 4.0f, kZ));
    f.update(f.frame(12, 12.0f, true)); // the cursors start here

    // The UI's own sounds.
    f.sounds.playUi(UiSound::GameMenuOpen);
    CHECK(f.sounds.recentSound(0) == 850); // igMainMenuOpen
    f.sounds.playUi(UiSound::QuestLogClose);
    CHECK(f.sounds.recentSound(0) == 845);

    // Level up.
    Bytes level;
    net::ByteWriter l(level);
    l.u32v(4).u32v(17).u32v(22);
    for (u32 i = 0; i < 9; ++i) {
        l.u32v(1);
    }
    REQUIRE(f.play.handle(Opcode::SMSG_LEVELUP_INFO, level, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.sounds.recentSound(0) == 124); // LEVELUPSOUND

    // An NPC talks: the gossip window opens with its sound and the NPC's greeting; closing it, its farewell. WoW.exe
    // says one line at a time per unit (CGUnit +0xB1C): closed while it still greets, no farewell. A stopped voice is
    // over for the null output a second later (AudioSystem: fade + 1 s).
    const auto silence = [&] {
        f.system.stopAll(0.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        f.update(f.frame(12, 12.0f, true));
    };
    Bytes gossip;
    net::ByteWriter(gossip).u64v(kNpc).u32v(2).u32v(1).u32v(0).u8v(0).u8v(0).cstring("Hello").u32v(0);
    REQUIRE(f.play.handle(Opcode::SMSG_GOSSIP_MESSAGE, gossip, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    const audio::NpcSounds* npcSounds = f.data.npcSounds(npcDisplay);
    CHECK(f.played(875, 2)); // igQuestListOpen
    CHECK(f.played(npcSounds->greeting, 2));
    f.play.closeDialog();
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(876, 2)); // igQuestListClose
    CHECK_FALSE(f.played(npcSounds->farewell, 2));
    silence();
    REQUIRE(f.play.handle(Opcode::SMSG_GOSSIP_MESSAGE, gossip, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(npcSounds->greeting, 2));
    silence();
    f.play.closeDialog();
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(npcSounds->farewell, 2));

    // Clicks on it (CGUnit::PlayClickVoice): five greetings, then its pissed lines; not while it still talks.
    silence();
    f.play.unitClicked(kNpc);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(npcSounds->greeting, 2));
    const u32 played = f.sounds.stats().played;
    f.play.unitClicked(kNpc);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.sounds.stats().played == played); // still talking: that click does not count
    for (u32 i = 1; i < 6; ++i) {
        silence();
        f.play.unitClicked(kNpc);
        f.update(f.frame(12, 12.0f, true));
        CHECK(f.played(i < 5 ? npcSounds->greeting : npcSounds->pissed, 2));
    }
    silence();

    // Targeting the wolf (hostile or neutral: one of the select sounds), then nothing.
    f.play.setTarget(kWolf);
    f.update(f.frame(12, 12.0f, true));
    CHECK((f.played(873, 1) || f.played(871, 1) || f.played(867, 1)));
    f.play.setTarget(0);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.sounds.recentSound(0) == 684); // INTERFACESOUND_LOSTTARGETUNIT

    // A critical punch on the wolf: the unarmed critical impact on its body and its critical wound cry.
    const audio::CreatureSounds* wolf = f.data.creatureSounds(wolfDisplay);
    Bytes swing;
    net::ByteWriter(swing)
        .u32v(net::kHitInfoAffectsVictim | net::kHitInfoCritical)
        .packedGuid(kPlayer)
        .packedGuid(kWolf)
        .u32v(24)
        .u8v(1)
        .u32v(0)
        .f32v(24.0f)
        .u32v(24)
        .u32v(0)
        .u32v(0)
        .u32v(1)
        .u32v(0)
        .u32v(0)
        .u32v(0);
    REQUIRE(f.play.handle(Opcode::SMSG_ATTACKERSTATEUPDATE, swing, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(f.data.weaponImpact(audio::SoundData::kUnarmed, std::min<u32>(wolf->impactType, 9), true), 3));
    CHECK(f.played(wolf->injuryCritical, 3));
    // It dies: its death cry.
    REQUIRE(f.store.applyUpdateObject(
        [] {
            Bytes out;
            net::ByteWriter w(out);
            w.u32v(1).u8v(0).u8v(0).packedGuid(kWolf).u8v(1).u32v(1u << uf::UNIT_FIELD_HEALTH).u32v(0);
            return out;
        }(),
        f.now));
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.sounds.recentSound(0) == wolf->death);

    // Frostbolt: its precast loop while casting, then the cast and the impact on the wolf.
    Bytes start;
    net::ByteWriter(start)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(116)
        .u16v(2)
        .u32v(1500)
        .u16v(net::kTargetUnit)
        .packedGuid(kWolf);
    REQUIRE(f.play.handle(Opcode::SMSG_SPELL_START, start, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.sounds.recentSound(0) == 20); // Frost Precast (a loop)
    Bytes go;
    net::ByteWriter(go)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(116)
        .u16v(0x100)
        .u8v(1)
        .u64v(kWolf)
        .u8v(0)
        .u16v(net::kTargetUnit)
        .packedGuid(kWolf);
    REQUIRE(f.play.handle(Opcode::SMSG_SPELL_GO, go, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(2559, 2)); // cast
    CHECK(f.played(3013, 2)); // the missile's loop (MissileIceLoop), flying 4 yards at 28 yards a second
    CHECK_FALSE(f.played(7, 2));
    for (u32 i = 0; i < 12; ++i) {
        f.update(f.frame(12, 12.0f, true));
    }
    CHECK(f.sounds.recentSound(0) == 7); // impact, on arrival
    // The server's sounds: SMSG_PLAY_SOUND.
    Bytes soundBody;
    net::ByteWriter(soundBody).u32v(124);
    REQUIRE(f.play.handle(Opcode::SMSG_PLAY_SOUND, soundBody, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.sounds.recentSound(0) == 124);
}

TEST_CASE("Game audio: footsteps by stride and ground, splashes, landing") {
    VD_REQUIRE_SOUND(f);
    GameAudioFrame walk = f.frame(12, 12.0f);
    walk.character = true;
    walk.feet = glm::dvec3(kX, kY, kZ);
    walk.onGround = true;
    walk.speed = 7.0f;
    walk.display = 49;     // human male: footstep id 7
    walk.terrainSound = 6; // grass
    f.update(walk);
    const u32 before = f.sounds.stats().played;
    // Two seconds running at 7 yd/s: about three steps a second.
    for (u32 i = 0; i < 120; ++i) {
        f.update(walk);
    }
    const u32 steps = f.sounds.stats().played - before;
    CHECK(steps >= 5);
    CHECK(steps <= 8);
    CHECK(f.sounds.recentSound(0) == f.data.footstep(7, 6, false));
    // Wading: splashes; standing still: nothing; landing: one step.
    walk.wading = true;
    for (u32 i = 0; i < 30; ++i) {
        f.update(walk);
    }
    CHECK(f.sounds.recentSound(0) == f.data.footstep(7, 6, true));
    walk.speed = 0.0f;
    walk.wading = false;
    const u32 still = f.sounds.stats().played;
    for (u32 i = 0; i < 60; ++i) {
        f.update(walk);
    }
    CHECK(f.sounds.stats().played == still);
    walk.landed = true;
    f.update(walk);
    CHECK(f.sounds.stats().played == still + 1);
}

// --- Parity pass ---------------------------------------------------------------------------------------------------

TEST_CASE("Game audio (parity): jumps, landings, splashes and mounted steps") {
    VD_REQUIRE_SOUND(f);
    GameAudioFrame walk = f.frame(12, 12.0f);
    walk.character = true;
    walk.feet = glm::dvec3(kX, kY, kZ);
    walk.onGround = true;
    walk.display = 49;     // human male (offline: race 1, sex 0)
    walk.terrainSound = 6; // grass
    f.update(walk);
    // Taking off: nothing (1.12 has no jump sound).
    walk.jumped = true;
    walk.onGround = false;
    f.update(walk);
    CHECK_FALSE(f.played(3283, 1)); // HumanmaleMainJump
    walk.jumped = false;
    // Landing after a jump's 0.8 s: only a step on the grass, no grunt.
    walk.landed = true;
    walk.onGround = true;
    walk.airMs = 820;
    f.update(walk);
    CHECK(f.played(f.data.footstep(7, 6, false), 2));
    CHECK_FALSE(f.played(3284, 2)); // HumanmaleMainLand
    walk.landed = false;
    // Into the water: the race's splash.
    walk.startedSwim = true;
    f.update(walk);
    CHECK(f.played(1096, 1)); // CharacterSplashSoundMedium
    walk.startedSwim = false;
    f.update(walk);
}

TEST_CASE("Game audio (parity): falls, loot, equipment, objects, swings, ghosts and vocal errors online") {
    VD_REQUIRE_SOUND(f);
    constexpr u64 kTorch = 0xF110000000000777ull;
    constexpr u64 kChest = 0xF110000000000778ull;
    constexpr u32 kAxe = 750;
    f.play.enterWorld(kPlayer, 0, true, f.now);
    f.apply(created(kPlayer, game::ObjectType::Player,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_BYTES_0, 1 /* human male, mana */},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, 1}, {uf::UNIT_FIELD_LEVEL, 3}, {uf::UNIT_FIELD_DISPLAYID, 49}},
                    kX, kY, kZ));
    f.apply(created(kWolf, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_DISPLAYID, 161 /* Wolf.mdx */},
                     {uf::UNIT_FIELD_FACTIONTEMPLATE, 32}},
                    kX, kY + 4.0f, kZ));
    // A torch (G_Torch01: TorchLoop) and a chest (Chest01), closed.
    f.apply(created(kTorch, game::ObjectType::GameObject,
                    {{uf::GAMEOBJECT_DISPLAYID, 325}, {uf::GAMEOBJECT_STATE, 1}, {uf::GAMEOBJECT_TYPE_ID, 5}},
                    kX + 6.0f, kY, kZ));
    f.apply(created(kChest, game::ObjectType::GameObject,
                    {{uf::GAMEOBJECT_DISPLAYID, 10}, {uf::GAMEOBJECT_STATE, 1}, {uf::GAMEOBJECT_TYPE_ID, 3}},
                    kX - 6.0f, kY, kZ));
    f.update(f.frame(12, 12.0f, true)); // the cursors start here; the objects are seen
    CHECK(f.played(4694, 4));            // TorchLoop
    // The chest opens: its sound; then it closes.
    f.apply(values(kChest, {{uf::GAMEOBJECT_STATE, 0}}));
    for (u32 i = 0; i < 20; ++i) {
        f.update(f.frame(12, 12.0f, true));
    }
    CHECK(f.played(1277)); // ChestOpen
    f.apply(values(kChest, {{uf::GAMEOBJECT_STATE, 1}}));
    for (u32 i = 0; i < 20; ++i) {
        f.update(f.frame(12, 12.0f, true));
    }
    CHECK(f.played(1278)); // ChestClose
    // Phase 13: the fishing bobber bites (SMSG_GAMEOBJECT_CUSTOM_ANIM 0): its display's Custom0 sound, Fishing Hooked.
    constexpr u64 kBobber = 0xF110000000000779ull;
    f.apply(created(kBobber, game::ObjectType::GameObject,
                    {{uf::GAMEOBJECT_DISPLAYID, 668}, {uf::GAMEOBJECT_STATE, 1}, {uf::GAMEOBJECT_TYPE_ID, 17}},
                    kX + 3.0f, kY, kZ));
    f.update(f.frame(12, 12.0f, true));
    Bytes hooked;
    net::ByteWriter(hooked).u64v(kBobber).u32v(0);
    REQUIRE(f.play.handle(Opcode::SMSG_GAMEOBJECT_CUSTOM_ANIM, hooked, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(3355, 2)); // Fishing Hooked
    // The wolf stands idle near: within 20 s one of its stand / fidget sounds (WolfFidget2); without the actors'
    // $FD events (no Actors here) GameAudio picks idle creatures at random.
    const audio::CreatureSounds* wolf = f.data.creatureSounds(161);
    REQUIRE(wolf);
    bool fidgeted = false;
    for (u32 i = 0; i < 1300 && !fidgeted; ++i) {
        f.update(f.frame(12, 12.0f, true));
        for (const u32 id : wolf->fidget) {
            fidgeted = fidgeted || (id != 0 && f.sounds.recentSound(0) == id);
        }
        fidgeted = fidgeted || (wolf->stand != 0 && f.sounds.recentSound(0) == wolf->stand);
    }
    CHECK(fidgeted);

    // A fall that hurts: DamageFalling and the character's wound.
    Bytes fall;
    net::ByteWriter(fall).u64v(kPlayer).u8v(2).u32v(37).u32v(0).u32v(0);
    REQUIRE(f.play.handle(Opcode::SMSG_ENVIRONMENTALDAMAGELOG, fall, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(3542, 2)); // DamageFalling
    CHECK(f.played(2942, 2)); // HumanMalePlayerWound
    // Drowning: its kit's sound.
    Bytes drown;
    net::ByteWriter(drown).u64v(kPlayer).u8v(1).u32v(12).u32v(0).u32v(0);
    REQUIRE(f.play.handle(Opcode::SMSG_ENVIRONMENTALDAMAGELOG, drown, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(5736, 2)); // Drowning

    // Loot: the axe's template (Axe_1H_Flint_A_01: small metal) and the item pushed into the bags.
    REQUIRE(f.play.handle(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(kAxe, 2, 0, 453, 1), f.now) ==
            game::Gameplay::Handled::Yes);
    Bytes push;
    net::ByteWriter(push).u64v(kPlayer).u32v(0).u32v(0).u32v(1).u8v(255).u32v(23).u32v(kAxe).u32v(0).u32v(0).u32v(1);
    REQUIRE(f.play.handle(Opcode::SMSG_ITEM_PUSH_RESULT, push, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(1191, 2)); // PickUpMetalSmall
    // The finger puts it down (the bags window's drag: UiSoundSink::itemSound).
    for (u32 i = 0; i < 20; ++i) {
        f.update(f.frame(12, 12.0f, true));
    }
    f.sounds.uiSink().itemSound(kAxe, false);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(1213, 1)); // PutDownSmallMetal

    // The axe drawn (UNIT_FIELD_BYTES_2's sheath state 1, its main hand): a metal weapon out, then away.
    f.apply(values(kPlayer, {{uf::PLAYER_VISIBLE_ITEM_16_0, kAxe}}));
    f.update(f.frame(12, 12.0f, true));
    f.apply(values(kPlayer, {{uf::UNIT_FIELD_BYTES_2, 1}}));
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(700, 1)); // SHEATHINGMETALWEAPONUNSHEATHE
    f.apply(values(kPlayer, {{uf::UNIT_FIELD_BYTES_2, 0}}));
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(698, 1)); // SHEATHINGMETALWEAPONSHEATHE

    // Melee: a miss whooshes (the client's "Combat Miss"); a hit swooshes and hits.
    const auto melee = [&](u32 hitInfo, u32 damage, u32 victimState) {
        Bytes swing;
        net::ByteWriter(swing)
            .u32v(hitInfo)
            .packedGuid(kWolf)
            .packedGuid(kPlayer)
            .u32v(damage)
            .u8v(1)
            .u32v(0)
            .f32v(static_cast<f32>(damage))
            .u32v(damage)
            .u32v(0)
            .u32v(0)
            .u32v(victimState)
            .u32v(0)
            .u32v(0)
            .u32v(0);
        REQUIRE(f.play.handle(Opcode::SMSG_ATTACKERSTATEUPDATE, swing, f.now) == game::Gameplay::Handled::Yes);
        f.update(f.frame(12, 12.0f, true));
    };
    melee(net::kHitInfoMiss, 0, 0);
    CHECK(f.played(7080, 2)); // (DONOTRENAME)Combat Miss 1H (the wolf bites: unarmed)
    for (u32 i = 0; i < 200; ++i) {
        f.update(f.frame(12, 12.0f, true)); // past the wound's cooldown
    }
    melee(net::kHitInfoAffectsVictim, 7, 1);
    CHECK(f.played(f.data.weaponSwing(0, false), 4)); // LightWeaponNormal
    CHECK(f.played(f.data.weaponImpact(audio::SoundData::kUnarmed, 0, false), 4));
    CHECK(f.played(2942, 4)); // the human's wound

    // The character complains (VocalUISounds): no mana for a cast.
    Bytes noMana;
    net::ByteWriter(noMana).u32v(133).u8v(net::kCastStatusFailed).u8v(net::kCastFailedNoPower);
    REQUIRE(f.play.handle(Opcode::SMSG_CAST_RESULT, noMana, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(1885, 1)); // HumanMale_CantCastNoMana

    // A new area explored, a flight path learned, a party invitation.
    Bytes explored;
    net::ByteWriter(explored).u32v(87).u32v(55);
    REQUIRE(f.play.handle(Opcode::SMSG_EXPLORATION_EXPERIENCE, explored, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(4140, 2)); // HumanExploration
    REQUIRE(f.play.handle(Opcode::SMSG_NEW_TAXI_PATH, Bytes{}, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(1519, 1)); // TaxiNodeDiscovered
    Bytes inviter;
    net::ByteWriter(inviter).cstring("Aeryn");
    REQUIRE(f.play.handle(Opcode::SMSG_GROUP_INVITE, inviter, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.played(880, 1)); // igPlayerInvite

    // The player hit the wolf a moment ago (watched): it dies, its cry, then its body on the ground (a medium body
    // on unknown ground: dirt).
    Bytes bite;
    net::ByteWriter(bite)
        .u32v(net::kHitInfoAffectsVictim)
        .packedGuid(kPlayer)
        .packedGuid(kWolf)
        .u32v(5)
        .u8v(1)
        .u32v(0)
        .f32v(5.0f)
        .u32v(5)
        .u32v(0)
        .u32v(0)
        .u32v(1)
        .u32v(0)
        .u32v(0)
        .u32v(0);
    REQUIRE(f.play.handle(Opcode::SMSG_ATTACKERSTATEUPDATE, bite, f.now) == game::Gameplay::Handled::Yes);
    f.update(f.frame(12, 12.0f, true));
    REQUIRE(f.store.applyUpdateObject(values(kWolf, {{uf::UNIT_FIELD_HEALTH, 0}}), f.now));
    f.update(f.frame(12, 12.0f, true));
    const u32 thud = f.data.deathThud(161, 1, false);
    CHECK(thud == 912); // DeathThudMediumDirt
    CHECK_FALSE(f.played(thud, 2));
    for (u32 i = 0; i < 70; ++i) { // 16 ms a frame: past the 0.9 s
        f.update(f.frame(12, 12.0f, true));
    }
    CHECK(f.played(thud));

    // Released as a ghost: the ghost's ambience and music everywhere.
    f.apply(values(kPlayer, {{uf::PLAYER_FLAGS, game::kPlayerFlagGhost}}));
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.sounds.ambienceEntry() == 4160); // Ghost (DONOTRENAME)
    CHECK((f.sounds.musicEntry() == 3820 || f.sounds.musicEntry() == 3821)); // GhostMusic's day / night
    // Resurrected: Elwynn's again.
    f.apply(values(kPlayer, {{uf::PLAYER_FLAGS, 0}}));
    f.update(f.frame(12, 12.0f, true));
    CHECK(f.sounds.ambienceEntry() == 4183); // ForestNormalDay

    // Mounted (a horse's display): the hooves, not the feet.
    f.apply(values(kPlayer, {{uf::UNIT_FIELD_MOUNTDISPLAYID, 65}}));
    GameAudioFrame ride = f.frame(12, 12.0f, true);
    ride.character = true;
    ride.feet = glm::dvec3(kX, kY, kZ);
    ride.onGround = true;
    ride.speed = 11.2f;
    ride.display = 49;
    ride.terrainSound = 1; // dirt
    for (u32 i = 0; i < 30; ++i) {
        ride.nowMs = f.now;
        f.update(ride);
    }
    CHECK(f.played(f.data.footstep(f.data.footstepId(65), 1, false), 3));
    CHECK(f.data.footstep(f.data.footstepId(65), 1, false) == 1137); // FootstepHorseDirt
}

TEST_CASE("Game audio (parity): the camera under water, the lake's loop (VD_PACKS_DIR)") {
    VD_REQUIRE_SOUND(f);
    const char* dir = std::getenv("VD_PACKS_DIR");
    physics::CollisionWorld world;
    if (!world.openMap(dir, "Azeroth").ok()) {
        MESSAGE("collision_azeroth.vdpk missing: skipping");
        return;
    }
    // Some water near Goldshire: Elwynn's lakes and streams (scanned on the loaded collision tiles).
    const glm::dvec3 around(kX, kY, kZ);
    for (u32 i = 0; i < 500 && !world.ready(around.x, around.y, 400.0f); ++i) {
        world.update(around, 600.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(world.ready(around.x, around.y, 400.0f));
    std::optional<glm::dvec3> water;
    for (f64 dy = -400.0; dy <= 400.0 && !water; dy += 8.0) {
        for (f64 dx = -400.0; dx <= 400.0 && !water; dx += 8.0) {
            if (const std::optional<physics::LiquidSample> l =
                    world.liquidAt(kX + dx, kY + dy, kZ + 200.0, kZ - 200.0);
                l && l->kind == 0) {
                water = glm::dvec3(kX + dx, kY + dy, static_cast<f64>(l->level));
            }
        }
    }
    REQUIRE(water);
    // Two yards above the surface: the water's loop (RiverStill), the zone's ambience.
    GameAudioFrame above = f.frame(12, 12.0f);
    above.collision = &world;
    above.listener = *water + glm::dvec3(0.0, 0.0, 2.0);
    f.update(above);
    CHECK_FALSE(f.sounds.underwater());
    CHECK(f.sounds.waterEntry() == 1111); // RiverStill
    CHECK(f.sounds.ambienceEntry() == 4183);
    // The camera dives: the client's underwater ambience instead.
    GameAudioFrame below = above;
    below.nowMs = f.now;
    below.listener = *water - glm::dvec3(0.0, 0.0, 1.5);
    f.update(below);
    CHECK(f.sounds.underwater());
    CHECK(f.sounds.ambienceEntry() == 4209); // Underwater (DONOTRENAME)
    // Far from any water (300 yards up): the loop fades out.
    GameAudioFrame high = above;
    high.listener = *water + glm::dvec3(0.0, 0.0, 300.0);
    for (u32 i = 0; i < 20; ++i) {
        high.nowMs = f.now;
        f.update(high);
    }
    CHECK(f.sounds.waterEntry() == 0);
}
