// Phase-8 packets (net/CombatMessages): trainers, talents, pets and reputation, byte for byte as vmangos-core
// writes (server -> client) and reads (client -> server) them; truncated and malformed bodies are rejected.
#include "net/ByteBuffer.h"
#include "net/CombatMessages.h"

#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <vector>

namespace {

using namespace vd;
using Bytes = std::vector<u8>;

// SendTrainerSpellHelper for one spell.
void trainerSpell(net::ByteWriter& w, u32 spell, u8 state, u32 cost, u32 profFirst, u8 level, u32 skill,
                  u32 skillValue, u32 req1, u32 req2) {
    w.u32v(spell).u8v(state).u32v(cost).u32v(profFirst).u32v(profFirst).u8v(level).u32v(skill).u32v(
        skillValue);
    w.u32v(req1).u32v(req2).u32v(0);
}

Bytes cut(Bytes b, usize drop) {
    b.resize(b.size() - drop);
    return b;
}

} // namespace

TEST_CASE("Trainer list, buy results and the talent wipe offer") {
    Bytes body;
    net::ByteWriter w(body);
    w.u64v(0xF130000000000ABCull).u32v(net::kTrainerTypeClass).u32v(3);
    trainerSpell(w, 1173, 0, 100, 0, 6, 0, 0, 133, 0);   // Fireball rank 2: available
    trainerSpell(w, 1178, 1, 900, 0, 12, 0, 0, 1173, 0); // rank 3: level too low
    trainerSpell(w, 2138, 2, 0, 0, 1, 0, 0, 0, 0);       // known
    w.cstring("Hello, mage!");
    net::TrainerList list;
    REQUIRE(net::parseTrainerList(body, list));
    CHECK(list.trainer == 0xF130000000000ABCull);
    CHECK(list.trainerType == net::kTrainerTypeClass);
    REQUIRE(list.spells.size() == 3);
    CHECK(list.spells[0].spellId == 1173);
    CHECK(list.spells[0].state == net::TrainerSpellState::Available);
    CHECK(list.spells[0].cost == 100);
    CHECK(list.spells[0].requiredLevel == 6);
    CHECK(list.spells[0].requiredSpell == 133);
    CHECK(list.spells[1].state == net::TrainerSpellState::Unavailable);
    CHECK(list.spells[1].requiredLevel == 12);
    CHECK(list.spells[2].state == net::TrainerSpellState::Known);
    CHECK(list.greeting == "Hello, mage!");
    CHECK_FALSE(net::parseTrainerList(cut(body, 1), list)); // greeting without its NUL
    CHECK_FALSE(net::parseTrainerList(cut(body, 20), list));
    // A count larger than the body, and an unknown state.
    Bytes lying = body;
    lying[12] = 200;
    CHECK_FALSE(net::parseTrainerList(lying, list));
    Bytes badState = body;
    badState[16 + 4] = 7;
    CHECK_FALSE(net::parseTrainerList(badState, list));
    // A profession trainer with no spell for the player, and its empty greeting.
    Bytes empty;
    net::ByteWriter(empty).u64v(5).u32v(net::kTrainerTypeTradeskills).u32v(0).cstring("");
    REQUIRE(net::parseTrainerList(empty, list));
    CHECK(list.spells.empty());
    CHECK(list.trainerType == net::kTrainerTypeTradeskills);

    Bytes ok;
    net::ByteWriter(ok).u64v(0xF130000000000ABCull).u32v(1173);
    u64 trainer = 0;
    u32 spell = 0, reason = 9;
    REQUIRE(net::parseTrainerBuySucceeded(ok, trainer, spell));
    CHECK(trainer == 0xF130000000000ABCull);
    CHECK(spell == 1173);
    CHECK_FALSE(net::parseTrainerBuySucceeded(cut(ok, 1), trainer, spell));
    Bytes failed;
    net::ByteWriter(failed).u64v(0xF130000000000ABCull).u32v(1178).u32v(net::kTrainFailNotEnoughMoney);
    REQUIRE(net::parseTrainerBuyFailed(failed, trainer, spell, reason));
    CHECK(spell == 1178);
    CHECK(reason == net::kTrainFailNotEnoughMoney);
    CHECK_FALSE(net::parseTrainerBuyFailed(cut(failed, 2), trainer, spell, reason));

    Bytes wipe;
    net::ByteWriter(wipe).u64v(0xF130000000000ABCull).u32v(10000);
    u32 cost = 0;
    REQUIRE(net::parseTalentWipeConfirm(wipe, trainer, cost));
    CHECK(cost == 10000);
    CHECK_FALSE(net::parseTalentWipeConfirm(cut(wipe, 4), trainer, cost));

    // Client bodies: HandleTrainerBuySpellOpcode reads guid + spell; HandleLearnTalentOpcode two u32.
    Bytes out;
    net::writeTrainerBuySpell(out, 0xF130000000000ABCull, 1173);
    CHECK(out == Bytes{0xBC, 0x0A, 0, 0, 0, 0, 0x30, 0xF1, 0x95, 0x04, 0, 0});
    out.clear();
    net::writeLearnTalent(out, 1141, 2);
    CHECK(out == Bytes{0x75, 0x04, 0, 0, 2, 0, 0, 0});
}

TEST_CASE("Pet spells: pet, charm and removal forms, with cooldowns") {
    // PetSpellInitialize: an imp with Firebolt on autocast, Blood Pact off.
    Bytes body;
    net::ByteWriter w(body);
    w.u64v(0xF140000000000042ull)
        .u32v(0)
        .u8v(net::kPetReactDefensive)
        .u8v(net::kPetCommandFollow)
        .u8v(0)
        .u8v(0);
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
    w.u16v(1).u32v(3110).u16v(0).u32v(1500).u32v(0); // WritePetSpellsCooldown
    net::PetSpells pet;
    REQUIRE(net::parsePetSpells(body, pet));
    CHECK(pet.pet == 0xF140000000000042ull);
    CHECK(pet.reactState == net::kPetReactDefensive);
    CHECK(pet.commandState == net::kPetCommandFollow);
    CHECK(pet.flags == 0);
    CHECK(pet.bar[0] == 0x07000002u);
    CHECK(pet.bar[3] == (3110u | 0xC1000000u));
    CHECK(net::petButtonType(pet.bar[9]) == net::kPetActReaction);
    REQUIRE(pet.spells.size() == 2);
    CHECK(net::petButtonAction(pet.spells[1]) == 6307);
    REQUIRE(pet.cooldowns.size() == 1);
    CHECK(pet.cooldowns[0].spellId == 3110);
    CHECK(pet.cooldowns[0].cooldownMs == 1500);
    CHECK_FALSE(net::parsePetSpells(cut(body, 1), pet));
    CHECK_FALSE(net::parsePetSpells(cut(body, 30), pet));
    Bytes tooManySpells = body;
    tooManySpells[8 + 8 + 40] = 50; // the spell count
    CHECK_FALSE(net::parsePetSpells(tooManySpells, pet));

    // CharmSpellInitialize: a duration, a disabled bar flag, no spells, no cooldowns.
    Bytes charm;
    net::ByteWriter c(charm);
    c.u64v(0xF130000000000099ull)
        .u32v(60000)
        .u8v(net::kPetReactAggressive)
        .u8v(net::kPetCommandAttack)
        .u8v(0)
        .u8v(net::kPetDisabledFlag);
    for (u32 i = 0; i < 10; ++i) {
        c.u32v(0);
    }
    c.u8v(0).u16v(0);
    REQUIRE(net::parsePetSpells(charm, pet));
    CHECK(pet.durationMs == 60000);
    CHECK(pet.flags == net::kPetDisabledFlag);
    CHECK(pet.spells.empty());
    CHECK(pet.cooldowns.empty());

    // RemovePetActionBar: an empty GUID alone.
    Bytes removed(8, 0);
    REQUIRE(net::parsePetSpells(removed, pet));
    CHECK(pet.pet == 0);
    CHECK(pet.spells.empty());
    removed.push_back(1);
    CHECK_FALSE(net::parsePetSpells(removed, pet)); // nothing may follow it
    CHECK_FALSE(net::parsePetSpells(Bytes(7, 0), pet));
}

TEST_CASE("Pet mode, feedback, cast failures, tame failures and names") {
    Bytes mode;
    net::ByteWriter(mode)
        .u64v(0xF140000000000042ull)
        .u8v(net::kPetReactPassive)
        .u8v(net::kPetCommandStay)
        .u8v(0)
        .u8v(8);
    net::PetMode m;
    REQUIRE(net::parsePetMode(mode, m));
    CHECK(m.pet == 0xF140000000000042ull);
    CHECK(m.reactState == net::kPetReactPassive);
    CHECK(m.commandState == net::kPetCommandStay);
    CHECK(m.flags == net::kPetDisabledFlag);
    CHECK_FALSE(net::parsePetMode(cut(mode, 1), m));

    u8 value = 0;
    REQUIRE(net::parsePetActionFeedback(Bytes{net::kPetFeedbackNothingToAttack}, value));
    CHECK(value == net::kPetFeedbackNothingToAttack);
    CHECK_FALSE(net::parsePetActionFeedback(Bytes{}, value));
    REQUIRE(net::parsePetTameFailure(Bytes{4}, value));
    CHECK(value == 4);
    CHECK_FALSE(net::parsePetTameFailure(Bytes{}, value));

    Bytes failed;
    net::ByteWriter(failed).u32v(3110).u8v(2).u8v(89);
    net::PetCastFailed f;
    REQUIRE(net::parsePetCastFailed(failed, f));
    CHECK(f.spellId == 3110);
    CHECK(f.status == 2);
    CHECK(f.reason == 89); // OUT_OF_RANGE
    CHECK_FALSE(net::parsePetCastFailed(cut(failed, 1), f));

    Bytes name;
    net::ByteWriter(name).u32v(17).cstring("Zalgaz").u32v(1690000000u);
    net::PetNameQueryResponse n;
    REQUIRE(net::parsePetNameQueryResponse(name, n));
    CHECK(n.petNumber == 17);
    CHECK(n.name == "Zalgaz");
    CHECK(n.timestamp == 1690000000u);
    CHECK_FALSE(net::parsePetNameQueryResponse(cut(name, 1), n));

    // Client bodies as PetHandler reads them.
    Bytes out;
    net::writePetAction(out, 0xF140000000000042ull,
                        net::petButton(net::kPetCommandAttack, net::kPetActCommand), 0xF130000000000123ull);
    CHECK(out == Bytes{0x42, 0, 0, 0, 0, 0, 0x40, 0xF1, 2, 0, 0, 7, 0x23, 0x01, 0, 0, 0, 0, 0x30, 0xF1});
    out.clear();
    net::writePetSetAction(out, 0xF140000000000042ull, 5, net::petButton(6307, net::kPetActDisabled));
    CHECK(out == Bytes{0x42, 0, 0, 0, 0, 0, 0x40, 0xF1, 5, 0, 0, 0, 0xA3, 0x18, 0, 0x81});
    out.clear();
    net::writePetSpellAutocast(out, 0xF140000000000042ull, 3110, false);
    CHECK(out == Bytes{0x42, 0, 0, 0, 0, 0, 0x40, 0xF1, 0x26, 0x0C, 0, 0, 0});
    out.clear();
    net::writePetNameQuery(out, 17, 0xF140000000000042ull);
    CHECK(out == Bytes{17, 0, 0, 0, 0x42, 0, 0, 0, 0, 0, 0x40, 0xF1});
}

TEST_CASE("Reputation: the initial list, standings, visibility and at war") {
    Bytes init;
    net::ByteWriter w(init);
    w.u32v(64);
    for (u32 i = 0; i < 64; ++i) {
        w.u8v(i == 3 ? (net::kFactionVisible | net::kFactionPeaceForced) : 0).u32v(i == 3 ? 1500u : 0u);
    }
    net::InitializeFactions factions;
    REQUIRE(net::parseInitializeFactions(init, factions));
    CHECK(factions.count == 64);
    CHECK(factions.factions[3].flags == (net::kFactionVisible | net::kFactionPeaceForced));
    CHECK(factions.factions[3].standing == 1500);
    CHECK_FALSE(net::parseInitializeFactions(cut(init, 1), factions));
    Bytes extra = init;
    extra.push_back(0);
    CHECK_FALSE(net::parseInitializeFactions(extra, factions));
    Bytes tooMany = init;
    tooMany[0] = 65;
    CHECK_FALSE(net::parseInitializeFactions(tooMany, factions));

    Bytes standing;
    net::ByteWriter(standing)
        .u32v(2)
        .u32v(3)
        .u32v(static_cast<u32>(1750))
        .u32v(14)
        .u32v(static_cast<u32>(-250));
    net::FactionStanding s;
    REQUIRE(net::parseFactionStanding(standing, s));
    REQUIRE(s.count == 2);
    CHECK(s.entries[0].listId == 3);
    CHECK(s.entries[0].standing == 1750);
    CHECK(s.entries[1].standing == -250);
    CHECK_FALSE(net::parseFactionStanding(cut(standing, 3), s));

    Bytes out;
    net::writeSetFactionAtWar(out, 14, true);
    CHECK(out == Bytes{14, 0, 0, 0, 1});
}
