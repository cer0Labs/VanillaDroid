// Phase-8 flows through game::Gameplay (game/Abilities): trainers, the talent reset offer, learning talents,
// the pet bar, reputation and the auto-repeat spell. Server packets are written as vmangos-core writes them;
// what the client sends is checked byte for byte.
#include "game/Abilities.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/CombatMessages.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using game::GameEventKind;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x21;
constexpr u64 kTrainer = 0xF130000000000ABCull;
constexpr u64 kWolf = 0xF130000000000456ull;
constexpr u64 kImp = 0xF140000000000042ull;

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    const u32 words = maxIndex / 32 + 1;
    std::vector<u32> mask(words, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(words));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
}

Bytes created(u64 guid, game::ObjectType type, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f);
    writeValues(w, fields);
    return out;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    std::vector<Sent> sent;
    std::vector<u64> createdGuids;
    u32 now = 1000;
    u32 cursor = 0;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kPlayer, 0, true, now);
        apply(
            created(kPlayer, game::ObjectType::Player,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_BYTES_0, 1}, {uf::UNIT_FIELD_LEVEL, 10}}));
    }
    void apply(const Bytes& update) {
        createdGuids.clear();
        game::UpdateStats stats;
        stats.createdGuids = &createdGuids;
        REQUIRE(store.applyUpdateObject(update, now, &stats));
        play.afterUpdate(createdGuids, now);
    }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
    }
    std::vector<Bytes> sentWith(Opcode opcode) const {
        std::vector<Bytes> out;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out.push_back(s.body);
            }
        }
        return out;
    }
    std::vector<game::GameEvent> events() {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            out.push_back(*e);
        }
        return out;
    }
};

const game::GameEvent* findEvent(const std::vector<game::GameEvent>& events, GameEventKind kind) {
    for (const game::GameEvent& e : events) {
        if (e.kind == kind) {
            return &e;
        }
    }
    return nullptr;
}

Bytes guidBody(u64 guid) {
    Bytes out;
    net::writeGuid(out, guid);
    return out;
}

Bytes trainerList(u64 trainer) {
    Bytes body;
    net::ByteWriter w(body);
    w.u64v(trainer).u32v(net::kTrainerTypeClass).u32v(2);
    w.u32v(1173).u8v(0).u32v(100).u32v(0).u32v(0).u8v(6).u32v(0).u32v(0).u32v(133).u32v(0).u32v(0);
    w.u32v(1178).u8v(1).u32v(900).u32v(0).u32v(0).u8v(12).u32v(0).u32v(0).u32v(1173).u32v(0).u32v(0);
    w.cstring("Hello!");
    return body;
}

Bytes petSpells(u64 pet) {
    Bytes body;
    net::ByteWriter w(body);
    w.u64v(pet).u32v(0).u8v(net::kPetReactDefensive).u8v(net::kPetCommandFollow).u8v(0).u8v(0);
    const u32 bar[10] = {
        net::petButton(2, net::kPetActCommand),     net::petButton(1, net::kPetActCommand),
        net::petButton(0, net::kPetActCommand),     net::petButton(3110, net::kPetActEnabled),
        net::petButton(6307, net::kPetActDisabled), net::petButton(0, net::kPetActDisabled),
        net::petButton(0, net::kPetActDisabled),    net::petButton(2, net::kPetActReaction),
        net::petButton(1, net::kPetActReaction),    net::petButton(0, net::kPetActReaction)};
    for (const u32 b : bar) {
        w.u32v(b);
    }
    w.u8v(2).u32v(net::petButton(3110, net::kPetActEnabled)).u32v(net::petButton(6307, net::kPetActDisabled));
    w.u16v(1).u32v(6307).u16v(0).u32v(0).u32v(4000);
    return body;
}

} // namespace

TEST_CASE("Abilities: a trainer from the gossip option to the bought spell") {
    Harness h;
    h.apply(created(kTrainer, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_NPC_FLAGS, game::kNpcFlagTrainer}}));
    // A trainer without a gossip menu: a tap asks for its list at once.
    CHECK(h.play.interact(kTrainer) == game::Interaction::Trainer);
    REQUIRE(h.sentWith(Opcode::CMSG_TRAINER_LIST).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_TRAINER_LIST)[0] == guidBody(kTrainer));

    REQUIRE(h.packet(Opcode::SMSG_TRAINER_LIST, trainerList(kTrainer)) == game::Gameplay::Handled::Yes);
    const game::TrainerWindow& window = h.play.abilities().trainer();
    REQUIRE(window.open());
    CHECK(window.trainer() == kTrainer);
    CHECK(window.greeting() == "Hello!");
    REQUIRE(window.services().size() == 2);
    REQUIRE(h.play.abilities().buyTrainerSpell(1173));
    Bytes buy;
    net::writeTrainerBuySpell(buy, kTrainer, 1173);
    CHECK(h.sentWith(Opcode::CMSG_TRAINER_BUY_SPELL).back() == buy);

    Bytes ok;
    net::ByteWriter(ok).u64v(kTrainer).u32v(1173);
    REQUIRE(h.packet(Opcode::SMSG_TRAINER_BUY_SUCCEEDED, ok) == game::Gameplay::Handled::Yes);
    CHECK(window.services()[0].state == net::TrainerSpellState::Known);
    CHECK(window.lastBought() == 1173);
    std::vector<game::GameEvent> events = h.events();
    REQUIRE(findEvent(events, GameEventKind::TrainerBuySucceeded) != nullptr);
    CHECK(findEvent(events, GameEventKind::TrainerBuySucceeded)->id == 1173);

    Bytes failed;
    net::ByteWriter(failed).u64v(kTrainer).u32v(1178).u32v(net::kTrainFailNotEnoughMoney);
    REQUIRE(h.packet(Opcode::SMSG_TRAINER_BUY_FAILED, failed) == game::Gameplay::Handled::Yes);
    events = h.events();
    REQUIRE(findEvent(events, GameEventKind::TrainerBuyFailed) != nullptr);
    CHECK(findEvent(events, GameEventKind::TrainerBuyFailed)->value[0] ==
          static_cast<i32>(net::kTrainFailNotEnoughMoney));

    // Malformed lists are reported, the window stays as it was; leaving the world closes it.
    CHECK(h.packet(Opcode::SMSG_TRAINER_LIST, Bytes{1, 2, 3}) == game::Gameplay::Handled::Malformed);
    CHECK(window.open());
    h.play.abilities().closeTrainer();
    CHECK_FALSE(window.open());
    CHECK_FALSE(h.play.abilities().buyTrainerSpell(1178));
}

TEST_CASE("Abilities: the talent reset offer and learning talents") {
    Harness h;
    Bytes offer;
    net::ByteWriter(offer).u64v(kTrainer).u32v(10000);
    REQUIRE(h.packet(Opcode::MSG_TALENT_WIPE_CONFIRM, offer) == game::Gameplay::Handled::Yes);
    CHECK(h.play.abilities().talentWipeTrainer() == kTrainer);
    CHECK(h.play.abilities().talentWipeCost() == 10000);
    std::vector<game::GameEvent> events = h.events();
    REQUIRE(findEvent(events, GameEventKind::TalentWipeOffer) != nullptr);
    CHECK(findEvent(events, GameEventKind::TalentWipeOffer)->value[0] == 10000);
    h.play.abilities().answerTalentWipe(true);
    REQUIRE(h.sentWith(Opcode::MSG_TALENT_WIPE_CONFIRM).size() == 1);
    CHECK(h.sentWith(Opcode::MSG_TALENT_WIPE_CONFIRM)[0] == guidBody(kTrainer));
    CHECK(h.play.abilities().talentWipeTrainer() == 0);
    // Declined: nothing sent. "No talents spent" comes as an empty trainer.
    REQUIRE(h.packet(Opcode::MSG_TALENT_WIPE_CONFIRM, offer) == game::Gameplay::Handled::Yes);
    h.play.abilities().answerTalentWipe(false);
    CHECK(h.sentWith(Opcode::MSG_TALENT_WIPE_CONFIRM).size() == 1);

    REQUIRE(h.play.abilities().learnTalent(124, 0));
    Bytes learn;
    net::writeLearnTalent(learn, 124, 0);
    CHECK(h.sentWith(Opcode::CMSG_LEARN_TALENT).back() == learn);
    CHECK_FALSE(h.play.abilities().learnTalent(0, 0));
}

TEST_CASE("Abilities: the pet bar, its commands, autocast, cooldowns and name") {
    Harness h;
    h.apply(
        created(kImp, game::ObjectType::Unit,
                {{uf::UNIT_FIELD_HEALTH, 50}, {uf::UNIT_FIELD_PETNUMBER, 17}, {uf::UNIT_FIELD_LEVEL, 3}}));
    h.apply(created(kWolf, game::ObjectType::Unit, {{uf::UNIT_FIELD_HEALTH, 40}}));
    REQUIRE(h.packet(Opcode::SMSG_PET_SPELLS, petSpells(kImp)) == game::Gameplay::Handled::Yes);
    const game::PetState& pet = h.play.abilities().pet();
    REQUIRE(pet.active());
    CHECK(pet.guid == kImp);
    CHECK(pet.reactState == net::kPetReactDefensive);
    CHECK(pet.slotSpell(3) == 3110);
    CHECK(pet.slotAutocast(3));
    CHECK_FALSE(pet.slotAutocast(4));
    CHECK(pet.slotSpell(0) == 0); // Attack is a command
    std::vector<game::GameEvent> events = h.events();
    REQUIRE(findEvent(events, GameEventKind::PetChanged) != nullptr);
    CHECK(findEvent(events, GameEventKind::PetChanged)->guid == kImp);
    // The cooldown of SMSG_PET_SPELLS counts from its arrival.
    CHECK(h.play.abilities().petCooldownRemaining(6307, h.now + 1000) == 3000);

    // Attack (with the player's target), Stay, Passive, a spell.
    game::Abilities& abilities = h.play.abilities();
    REQUIRE(abilities.petAction(0, kWolf));
    REQUIRE(abilities.petAction(2, kWolf));
    REQUIRE(abilities.petAction(9, kWolf));
    REQUIRE(abilities.petAction(3, kWolf));
    const std::vector<Bytes> actions = h.sentWith(Opcode::CMSG_PET_ACTION);
    REQUIRE(actions.size() == 4);
    Bytes expected;
    net::writePetAction(expected, kImp, net::petButton(net::kPetCommandAttack, net::kPetActCommand), kWolf);
    CHECK(actions[0] == expected);
    expected.clear();
    net::writePetAction(expected, kImp, net::petButton(net::kPetCommandStay, net::kPetActCommand), 0);
    CHECK(actions[1] == expected);
    expected.clear();
    net::writePetAction(expected, kImp, net::petButton(net::kPetReactPassive, net::kPetActReaction), 0);
    CHECK(actions[2] == expected);
    expected.clear();
    net::writePetAction(expected, kImp, net::petButton(3110, net::kPetActEnabled), kWolf);
    CHECK(actions[3] == expected);
    CHECK(pet.commandState == net::kPetCommandStay);
    CHECK(pet.reactState == net::kPetReactPassive);
    CHECK_FALSE(abilities.petAction(5, kWolf)); // an empty spell slot

    // Autocast off for Firebolt; Blood Pact moved onto an empty slot.
    REQUIRE(abilities.setPetAutocast(3, false));
    expected.clear();
    net::writePetSpellAutocast(expected, kImp, 3110, false);
    CHECK(h.sentWith(Opcode::CMSG_PET_SPELL_AUTOCAST).back() == expected);
    CHECK_FALSE(pet.slotAutocast(3));
    CHECK_FALSE(abilities.setPetAutocast(0, true)); // not a spell
    REQUIRE(abilities.setPetSlot(5, 6307));
    expected.clear();
    net::writePetSetAction(expected, kImp, 5, net::petButton(6307, net::kPetActDisabled));
    CHECK(h.sentWith(Opcode::CMSG_PET_SET_ACTION).back() == expected);
    CHECK(pet.slotSpell(5) == 6307);

    // The pet's own casts start its cooldowns from the spell data.
    game::SpellInfo firebolt;
    firebolt.id = 3110;
    firebolt.recoveryMs = 2000;
    struct Table {
        game::SpellInfo spell;
    } table{firebolt};
    game::SpellSource source;
    source.context = &table;
    source.lookup = [](const void* context, u32 id, game::SpellInfo& out) {
        const Table* t = static_cast<const Table*>(context);
        if (id != t->spell.id) {
            return false;
        }
        out = t->spell;
        return true;
    };
    h.play.setSpellSource(source);
    Bytes go;
    net::ByteWriter(go)
        .packedGuid(kImp)
        .packedGuid(kImp)
        .u32v(3110)
        .u16v(0x100)
        .u8v(1)
        .u64v(kWolf)
        .u8v(0)
        .u16v(net::kTargetUnit)
        .packedGuid(kWolf);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, go) == game::Gameplay::Handled::Yes);
    CHECK(abilities.petCooldownRemaining(3110, h.now + 500) == 1500);

    // The name: asked once by pet number, kept when answered.
    CHECK(abilities.petName().empty());
    Bytes query;
    net::writePetNameQuery(query, 17, kImp);
    REQUIRE(h.sentWith(Opcode::CMSG_PET_NAME_QUERY).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_PET_NAME_QUERY)[0] == query);
    CHECK(abilities.petName().empty());
    CHECK(h.sentWith(Opcode::CMSG_PET_NAME_QUERY).size() == 1); // not asked again at once
    Bytes name;
    net::ByteWriter(name).u32v(17).cstring("Zalgaz").u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_PET_NAME_QUERY_RESPONSE, name) == game::Gameplay::Handled::Yes);
    CHECK(abilities.petName() == "Zalgaz");

    // Feedback and refusals become events.
    REQUIRE(h.packet(Opcode::SMSG_PET_ACTION_FEEDBACK, Bytes{net::kPetFeedbackNothingToAttack}) ==
            game::Gameplay::Handled::Yes);
    Bytes failed;
    net::ByteWriter(failed).u32v(3110).u8v(2).u8v(89);
    REQUIRE(h.packet(Opcode::SMSG_PET_CAST_FAILED, failed) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_PET_TAME_FAILURE, Bytes{4}) == game::Gameplay::Handled::Yes);
    events = h.events();
    REQUIRE(findEvent(events, GameEventKind::PetFeedback) != nullptr);
    CHECK(findEvent(events, GameEventKind::PetFeedback)->value[0] == net::kPetFeedbackNothingToAttack);
    REQUIRE(findEvent(events, GameEventKind::PetCastFailed) != nullptr);
    CHECK(findEvent(events, GameEventKind::PetCastFailed)->id == 3110);
    CHECK(findEvent(events, GameEventKind::PetCastFailed)->value[0] == 89);
    REQUIRE(findEvent(events, GameEventKind::PetTameFailed) != nullptr);

    // Dismiss (a warlock's pet) and the bar going away.
    REQUIRE(abilities.dismissPet());
    expected.clear();
    net::writePetAction(expected, kImp, net::petButton(net::kPetCommandDismiss, net::kPetActCommand), 0);
    CHECK(h.sentWith(Opcode::CMSG_PET_ACTION).back() == expected);
    REQUIRE(h.packet(Opcode::SMSG_PET_SPELLS, Bytes(8, 0)) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(pet.active());
    events = h.events();
    REQUIRE(findEvent(events, GameEventKind::PetChanged) != nullptr);
    CHECK(findEvent(events, GameEventKind::PetChanged)->guid == 0);
    CHECK_FALSE(abilities.petAction(0, kWolf));
    CHECK_FALSE(abilities.abandonPet());
}

TEST_CASE("Abilities: reputation standings, visibility and war") {
    Harness h;
    Bytes init;
    net::ByteWriter w(init);
    w.u32v(64);
    for (u32 i = 0; i < 64; ++i) {
        const u8 flags = i == 3    ? (net::kFactionVisible | net::kFactionPeaceForced)
                         : i == 14 ? net::kFactionVisible
                                   : 0;
        w.u8v(flags).u32v(i == 3 ? 1500u : 0u);
    }
    REQUIRE(h.packet(Opcode::SMSG_INITIALIZE_FACTIONS, init) == game::Gameplay::Handled::Yes);
    const game::ReputationList& list = h.play.abilities().reputation();
    REQUIRE(list.initialized());
    CHECK(list.entry(3).standing == 1500);
    CHECK((list.entry(14).flags & net::kFactionVisible) != 0);

    Bytes standing;
    net::ByteWriter(standing).u32v(1).u32v(3).u32v(1750);
    REQUIRE(h.packet(Opcode::SMSG_SET_FACTION_STANDING, standing) == game::Gameplay::Handled::Yes);
    CHECK(list.entry(3).standing == 1750);
    std::vector<game::GameEvent> events = h.events();
    const game::GameEvent* changed = findEvent(events, GameEventKind::ReputationChanged);
    REQUIRE(changed != nullptr);
    CHECK(changed->id == 3);
    CHECK(changed->value[0] == 250);
    CHECK(changed->value[1] == 1750);

    Bytes visible;
    net::writeU32(visible, 20);
    REQUIRE(h.packet(Opcode::SMSG_SET_FACTION_VISIBLE, visible) == game::Gameplay::Handled::Yes);
    CHECK((list.entry(20).flags & net::kFactionVisible) != 0);
    REQUIRE(findEvent(h.events(), GameEventKind::FactionVisible) != nullptr);

    // War with an enemy faction; never with one the server keeps at peace.
    REQUIRE(h.play.abilities().setAtWar(14, true));
    Bytes war;
    net::writeSetFactionAtWar(war, 14, true);
    CHECK(h.sentWith(Opcode::CMSG_SET_FACTION_ATWAR).back() == war);
    CHECK((list.entry(14).flags & net::kFactionAtWar) != 0);
    CHECK_FALSE(h.play.abilities().setAtWar(3, true));
    REQUIRE(h.play.abilities().setAtWar(14, false));
    CHECK((list.entry(14).flags & net::kFactionAtWar) == 0);
    CHECK(h.packet(Opcode::SMSG_SET_FACTION_STANDING, Bytes{1, 0, 0, 0}) ==
          game::Gameplay::Handled::Malformed);
}

TEST_CASE("Abilities: the auto-repeat spell runs until cancelled, refused or the target changes") {
    Harness h;
    game::Abilities& abilities = h.play.abilities();
    abilities.startedAutoRepeat(75, kWolf);
    CHECK(abilities.autoRepeat(kWolf) == 75);
    CHECK(abilities.autoRepeat(kTrainer) == 0); // another target: not running against it
    abilities.cancelAutoRepeat();
    CHECK(h.sentWith(Opcode::CMSG_CANCEL_AUTO_REPEAT_SPELL).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_CANCEL_AUTO_REPEAT_SPELL)[0].empty());
    CHECK(abilities.autoRepeat(kWolf) == 0);
    abilities.cancelAutoRepeat(); // nothing running: nothing sent
    CHECK(h.sentWith(Opcode::CMSG_CANCEL_AUTO_REPEAT_SPELL).size() == 1);

    abilities.startedAutoRepeat(75, kWolf);
    Bytes refused;
    net::ByteWriter(refused).u32v(75).u8v(net::kCastStatusFailed).u8v(net::kCastFailedOutOfRange);
    REQUIRE(h.packet(Opcode::SMSG_CAST_RESULT, refused) == game::Gameplay::Handled::Yes);
    CHECK(abilities.autoRepeat(kWolf) == 0);

    abilities.startedAutoRepeat(5019, kWolf);
    REQUIRE(h.packet(Opcode::SMSG_CANCEL_AUTO_REPEAT, Bytes{}) == game::Gameplay::Handled::Yes);
    CHECK(abilities.autoRepeat(kWolf) == 0);
}

TEST_CASE("Abilities: a new rank takes the old one's action buttons, saved on the server") {
    Harness h;
    std::array<u32, net::kActionButtonCount> buttons{};
    buttons[1] = 133;  // Fireball rank 1 on the main bar
    buttons[30] = 133; // and on another bar
    buttons[2] = 168;
    Bytes body;
    net::ByteWriter w(body);
    for (const u32 b : buttons) {
        w.u32v(b);
    }
    REQUIRE(h.packet(Opcode::SMSG_ACTION_BUTTONS, body) == game::Gameplay::Handled::Yes);
    Bytes superceded;
    net::ByteWriter(superceded).u16v(133).u16v(143);
    REQUIRE(h.packet(Opcode::SMSG_SUPERCEDED_SPELL, superceded) == game::Gameplay::Handled::Yes);
    CHECK(h.play.spells().button(1).action == 143);
    CHECK(h.play.spells().button(30).action == 143);
    CHECK(h.play.spells().button(2).action == 168);
    const std::vector<Bytes> saved = h.sentWith(Opcode::CMSG_SET_ACTION_BUTTON);
    REQUIRE(saved.size() == 2);
    Bytes expected;
    net::writeSetActionButton(expected, 1, 143);
    CHECK(saved[0] == expected);
    expected.clear();
    net::writeSetActionButton(expected, 30, 143);
    CHECK(saved[1] == expected);
}

TEST_CASE("Abilities: a trainer with a gossip menu talks first") {
    Harness h;
    h.apply(created(
        kTrainer, game::ObjectType::Unit,
        {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_NPC_FLAGS, game::kNpcFlagTrainer | game::kNpcFlagGossip}}));
    CHECK(h.play.interact(kTrainer) == game::Interaction::Gossip);
    CHECK(h.sentWith(Opcode::CMSG_TRAINER_LIST).empty());
    // The gossip's "Train me" makes the server send the list: the window opens on it.
    REQUIRE(h.packet(Opcode::SMSG_TRAINER_LIST, trainerList(kTrainer)) == game::Gameplay::Handled::Yes);
    CHECK(h.play.abilities().trainer().open());
    // Leaving the world closes it.
    h.play.leaveWorld();
    CHECK_FALSE(h.play.abilities().trainer().open());
}
