// Phase 18 (extras): the pet's sounds of SMSG_PET_ACTION_SOUND (WoW.exe 0x6040C0 -> 0x623A40: its
// CreatureSoundData attack sound for PET_TALK_ATTACK, its order sound for PET_TALK_SPECIAL_SPELL) through
// app/GameAudio over a real AudioSystem on the null backend and the baked packs (VD_PACKS_DIR: db.vdpk,
// sound.vdpk; skipped without them).
#include "app/GameAudio.h"
#include "audio/AudioSystem.h"
#include "audio/SoundData.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "net/ByteBuffer.h"
#include "net/ExtrasMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x11;
constexpr u64 kImp = 0xF140000000000789ull;
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

} // namespace

TEST_CASE("Extras audio: the pet's attack and order sounds (SMSG_PET_ACTION_SOUND)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok() ||
        !std::filesystem::exists(std::filesystem::path(dir) / "sound.vdpk")) {
        MESSAGE("VD_PACKS_DIR not set or sound.vdpk missing: skipping");
        return;
    }
    audio::AudioSystem system;
    audio::AudioConfig config;
    config.output = audio::AudioOutput::Null;
    REQUIRE(system.init(config, dir).ok());
    audio::SoundData data;
    REQUIRE(data.init(db));
    GameAudio sounds;
    sounds.init(&system, &data);
    REQUIRE(sounds.active());
    game::EntityStore store;
    game::Gameplay play{store};
    play.setSink({[](void*, Opcode, std::span<const u8>) { return true; }, nullptr});
    u32 now = 1000;

    // A demon's display with both sounds (the warlock pets' CreatureSoundData).
    u32 display = 0;
    for (u32 d = 1; d < 15000 && !display; ++d) {
        const audio::CreatureSounds* c = data.creatureSounds(d);
        if (c && c->petAttack && c->petOrder && c->petAttack != c->petOrder) {
            display = d;
        }
    }
    REQUIRE(display != 0);
    const audio::CreatureSounds* pet = data.creatureSounds(display);

    play.enterWorld(kPlayer, 0, true, now);
    const auto apply = [&](const Bytes& update) { REQUIRE(store.applyUpdateObject(update, now)); };
    apply(created(kPlayer, game::ObjectType::Player,
                  {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_BYTES_0, 1}, {uf::UNIT_FIELD_DISPLAYID, 49}},
                  kX, kY, kZ));
    apply(created(kImp, game::ObjectType::Unit,
                  {{uf::UNIT_FIELD_HEALTH, 100},
                   {uf::UNIT_FIELD_DISPLAYID, display},
                   {uf::UNIT_FIELD_SUMMONEDBY, 0x11}},
                  kX + 2.0f, kY, kZ));
    const auto frame = [&] {
        GameAudioFrame f;
        f.nowMs = now;
        f.dt = 1.0f / 60.0f;
        f.listener = glm::dvec3(kX, kY, kZ + 2.0);
        f.world = true;
        f.area = 12;
        f.hours = 12.0f;
        f.play = &play;
        f.entities = &store;
        f.player = kPlayer;
        sounds.update(f);
        system.update();
        now += 16;
    };
    const auto played = [&](u32 entry) {
        for (u32 i = 0; i < 4; ++i) {
            if (sounds.recentSound(i) == entry) {
                return true;
            }
        }
        return false;
    };
    frame(); // the cursors start here
    const auto talk = [&](u32 kind) {
        Bytes body;
        net::ByteWriter(body).u64v(kImp).u32v(kind);
        REQUIRE(play.handle(Opcode::SMSG_PET_ACTION_SOUND, body, now) == game::Gameplay::Handled::Yes);
        frame();
    };
    talk(net::kPetTalkAttack);
    CHECK(played(pet->petAttack));
    CHECK_FALSE(played(pet->petOrder));
    talk(net::kPetTalkSpecialSpell);
    CHECK(played(pet->petOrder));
}
