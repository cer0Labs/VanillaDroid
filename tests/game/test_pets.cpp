// Phase 15 (pets) flows through game::Gameplay: the stable (game/PetStable, WoW.exe's ClickStablePet rules),
// a pet trainer's untraining offer, the pet's spell book and bar (game/Abilities), party members' pets
// (game/Social), the portraits' UNIT_COMBATs and CombatFeedback.lua (game/CombatFeedback), and the pet tables
// of db.vdpk (game/PetData, VD_PACKS_DIR; skipped without it). Server packets are written as vmangos-core
// writes them; what the client sends is checked byte for byte.
#include "core/Hash.h"
#include "game/Combat.h"
#include "game/CombatFeedback.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/PetData.h"
#include "game/PetStable.h"
#include "net/ByteBuffer.h"
#include "net/CombatMessages.h"
#include "net/GameMessages.h"
#include "net/PetMessages.h"
#include "net/SocialMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
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
constexpr u64 kMaster = 0xF13000002AAB0011ull;
constexpr u64 kWolf = 0xF140000000000456ull;
constexpr u64 kTrainer = 0xF1300000033E0022ull;

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
        apply(created(
            kPlayer, game::ObjectType::Player,
            {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_BYTES_0, 3u << 8}, {uf::UNIT_FIELD_LEVEL, 20}}));
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

Bytes petNumberBody(u32 number) {
    Bytes out;
    net::writeStablePetNumber(out, kMaster, number);
    return out;
}

// SendStablePet with `slots` bought: the current wolf (40) and the stabled cat (41, its slot 2) and boar (42,
// slot 3), the cat listed before the current pet.
Bytes stableList(u8 slots, bool boar = true) {
    Bytes body;
    net::ByteWriter w(body);
    w.u64v(kMaster).u8v(boar ? 3 : 2).u8v(slots);
    w.u32v(41).u32v(3825).u32v(18).cstring("Shadow").u32v(2).u8v(2);
    w.u32v(40).u32v(1131).u32v(20).cstring("Fang").u32v(3).u8v(1);
    if (boar) {
        w.u32v(42).u32v(2959).u32v(12).cstring("Tusk").u32v(1).u8v(3);
    }
    return body;
}

} // namespace

TEST_CASE("Pet stable: the list, the selection and WoW.exe's moves") {
    Harness h;
    REQUIRE(h.packet(Opcode::MSG_LIST_STABLED_PETS, stableList(2)) == game::Gameplay::Handled::Yes);
    const auto events = h.events();
    REQUIRE(findEvent(events, GameEventKind::StableOpened));
    game::PetStable& stable = h.play.stable();
    REQUIRE(stable.open());
    CHECK(stable.stableMaster() == kMaster);
    CHECK(stable.slots() == 2);
    // The current pet goes to the first record whatever its place in the packet; the stabled ones keep their
    // order.
    REQUIRE(stable.pet(0));
    CHECK(stable.pet(0)->nameView() == "Fang");
    REQUIRE(stable.pet(1));
    CHECK(stable.pet(1)->nameView() == "Shadow");
    REQUIRE(stable.pet(2));
    CHECK(stable.pet(2)->nameView() == "Tusk");
    CHECK(stable.stabledCount() == 2);

    // Nothing selected after a list; PetStable_Update picks the pet out, or the first with a pet.
    CHECK(stable.selected() == -1);
    CHECK(stable.autoSelect(false) == 0);
    CHECK(stable.click(2, false));
    CHECK(stable.selected() == 2);

    // The current pet dropped on an occupied stable slot: swapped with it (CMSG_STABLE_SWAP_PET with that
    // pet).
    stable.pickup(0);
    CHECK(stable.picked() == 0);
    stable.click(1, true);
    CHECK(stable.picked() == -1);
    REQUIRE(h.sentWith(Opcode::CMSG_STABLE_SWAP_PET).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_STABLE_SWAP_PET)[0] == petNumberBody(41));
    // A stabled pet dropped on the current slot: swapped with the pet out, unstabled without one.
    stable.pickup(2);
    stable.click(0, true);
    REQUIRE(h.sentWith(Opcode::CMSG_STABLE_SWAP_PET).size() == 2);
    CHECK(h.sentWith(Opcode::CMSG_STABLE_SWAP_PET)[1] == petNumberBody(42));
    stable.pickup(1);
    stable.click(0, false);
    REQUIRE(h.sentWith(Opcode::CMSG_UNSTABLE_PET).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_UNSTABLE_PET)[0] == petNumberBody(41));
    // Between two stable slots nothing moves; put back on itself, nothing either.
    stable.pickup(1);
    stable.click(2, true);
    stable.pickup(1);
    stable.click(1, true);
    CHECK(h.sentWith(Opcode::CMSG_STABLE_SWAP_PET).size() == 2);
    CHECK(h.sentWith(Opcode::CMSG_UNSTABLE_PET).size() == 1);
    // An empty slot holds nothing to pick up.
    REQUIRE(h.packet(Opcode::MSG_LIST_STABLED_PETS, stableList(2, false)) == game::Gameplay::Handled::Yes);
    stable.pickup(2);
    CHECK(stable.picked() == -1);
    // The current pet onto an empty bought slot: stabled (CMSG_STABLE_PET with the master).
    stable.pickup(0);
    stable.click(2, true);
    REQUIRE(h.sentWith(Opcode::CMSG_STABLE_PET).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_STABLE_PET)[0] == guidBody(kMaster));
}

TEST_CASE("Pet stable: the results, buying a slot and closing") {
    Harness h;
    REQUIRE(h.packet(Opcode::MSG_LIST_STABLED_PETS, stableList(1, false)) == game::Gameplay::Handled::Yes);
    h.events();
    game::PetStable& stable = h.play.stable();
    // A slot not bought is no place for the current pet.
    stable.pickup(0);
    stable.click(2, true);
    CHECK(h.sentWith(Opcode::CMSG_STABLE_PET).empty());

    // BuyStableSlot: only with the money for it (the price is StableSlotPrices.dbc's, the caller's).
    CHECK_FALSE(stable.buySlot(50000, 49999));
    CHECK(stable.buySlot(50000, 50000));
    REQUIRE(h.sentWith(Opcode::CMSG_BUY_STABLE_SLOT).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_BUY_STABLE_SLOT)[0] == guidBody(kMaster));

    // The result handler: not enough money is said, a success asks for the list again (a slot counts at
    // once), the other failures say nothing.
    REQUIRE(h.packet(Opcode::SMSG_STABLE_RESULT, Bytes{net::kStableErrMoney}) ==
            game::Gameplay::Handled::Yes);
    const auto failed = h.events();
    REQUIRE(findEvent(failed, GameEventKind::StableFailed));
    REQUIRE(h.packet(Opcode::SMSG_STABLE_RESULT, Bytes{net::kStableErrStable}) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.events().empty());
    CHECK(h.sentWith(Opcode::MSG_LIST_STABLED_PETS).empty());
    REQUIRE(h.packet(Opcode::SMSG_STABLE_RESULT, Bytes{net::kStableSuccessBuySlot}) ==
            game::Gameplay::Handled::Yes);
    CHECK(stable.slots() == 2);
    REQUIRE(h.packet(Opcode::SMSG_STABLE_RESULT, Bytes{net::kStableSuccessStable}) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_STABLE_RESULT, Bytes{net::kStableSuccessUnstable}) ==
            game::Gameplay::Handled::Yes);
    const auto lists = h.sentWith(Opcode::MSG_LIST_STABLED_PETS);
    REQUIRE(lists.size() == 3);
    CHECK(lists[0] == guidBody(kMaster));
    CHECK(h.packet(Opcode::SMSG_STABLE_RESULT, Bytes{}) == game::Gameplay::Handled::Malformed);

    // Both slots bought: no more to buy. ClosePetStables: PET_STABLE_CLOSED, nothing more is asked.
    CHECK_FALSE(stable.buySlot(50000, 100000));
    stable.close();
    CHECK_FALSE(stable.open());
    REQUIRE(findEvent(h.events(), GameEventKind::StableClosed));
    REQUIRE(h.packet(Opcode::SMSG_STABLE_RESULT, Bytes{net::kStableSuccessStable}) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.sentWith(Opcode::MSG_LIST_STABLED_PETS).size() == 3);
}

TEST_CASE("Pet trainer: the untraining offer and its answer") {
    Harness h;
    // A pet trainer's gossip: its "untrain" option chosen (the server closes the gossip, then asks).
    Bytes gossip;
    net::ByteWriter(gossip).u64v(kTrainer).u32v(4783).u32v(1).u32v(0).u8v(3).u8v(0).cstring(
        "I wish to untrain my pet.");
    gossip.insert(gossip.end(), {0, 0, 0, 0});
    REQUIRE(h.packet(Opcode::SMSG_GOSSIP_MESSAGE, gossip) == game::Gameplay::Handled::Yes);
    REQUIRE(h.play.dialog().open());
    h.play.selectGossipOption(0);
    REQUIRE(h.packet(Opcode::SMSG_GOSSIP_COMPLETE, Bytes{}) == game::Gameplay::Handled::Yes);
    Bytes confirm;
    net::ByteWriter(confirm).u64v(kWolf).u32v(1000);
    REQUIRE(h.packet(Opcode::SMSG_PET_UNLEARN_CONFIRM, confirm) == game::Gameplay::Handled::Yes);
    const auto events = h.events();
    const game::GameEvent* offer = findEvent(events, GameEventKind::PetUnlearnOffer);
    REQUIRE(offer);
    CHECK(offer->guid == kWolf);
    CHECK(offer->value[0] == 1000);
    const game::PetStable& stable = h.play.stable();
    CHECK(stable.unlearnPet() == kWolf);
    CHECK(stable.unlearnCost() == 1000);
    CHECK(stable.unlearnTrainer() == kTrainer); // CheckPetUntrainerDist's NPC
    // ConfirmPetUnlearn: CMSG_PET_UNLEARN with the pet; the offer is gone.
    h.play.stable().answerUnlearn(true);
    REQUIRE(h.sentWith(Opcode::CMSG_PET_UNLEARN).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_PET_UNLEARN)[0] == guidBody(kWolf));
    CHECK(stable.unlearnPet() == 0);
    // Cancelled: nothing sent.
    REQUIRE(h.packet(Opcode::SMSG_PET_UNLEARN_CONFIRM, confirm) == game::Gameplay::Handled::Yes);
    h.play.stable().answerUnlearn(false);
    CHECK(h.sentWith(Opcode::CMSG_PET_UNLEARN).size() == 1);
}

namespace {

// The wolf's spells (Spell.dbc names, ranks and attributes as the book needs them).
struct SpellTable {
    std::vector<game::SpellInfo> spells;
};

game::SpellInfo spell(u32 id, const char* name, const char* rank, u32 attributes = 0) {
    game::SpellInfo s;
    s.id = id;
    s.name = name;
    s.rank = rank;
    s.attributes = attributes;
    return s;
}

Bytes wolfSpells() {
    Bytes body;
    net::ByteWriter w(body);
    w.u64v(kWolf).u32v(0).u8v(net::kPetReactDefensive).u8v(net::kPetCommandFollow).u8v(0).u8v(0);
    const u32 bar[10] = {
        net::petButton(2, net::kPetActCommand),      net::petButton(1, net::kPetActCommand),
        net::petButton(0, net::kPetActCommand),      net::petButton(17253, net::kPetActEnabled),
        net::petButton(14921, net::kPetActDisabled), net::petButton(0, net::kPetActDisabled),
        net::petButton(0, net::kPetActDisabled),     net::petButton(2, net::kPetActReaction),
        net::petButton(1, net::kPetActReaction),     net::petButton(0, net::kPetActReaction)};
    for (const u32 b : bar) {
        w.u32v(b);
    }
    // Growl 3, Bite 2, a hidden family passive, Great Stamina, Bite 1, Growl 2 (vMaNGOS' map order).
    w.u8v(6)
        .u32v(net::petButton(14921, net::kPetActDisabled))
        .u32v(net::petButton(17253, net::kPetActEnabled))
        .u32v(net::petButton(8875, net::kPetActPassive))
        .u32v(net::petButton(4187, net::kPetActPassive))
        .u32v(net::petButton(17254, net::kPetActDisabled))
        .u32v(net::petButton(14920, net::kPetActDisabled));
    w.u16v(0);
    return body;
}

} // namespace

TEST_CASE("Pet book: WoW.exe's list, autocast from the book, the bar's moves and casts") {
    Harness h;
    static SpellTable table{{spell(14921, "Growl", "Rank 3"), spell(14920, "Growl", "Rank 2"),
                             spell(17253, "Bite", "Rank 2"), spell(17254, "Bite", "Rank 1"),
                             spell(8875, "Tamed Pet Passive (DND)", "", 0xC0),
                             spell(4187, "Great Stamina", "Rank 1", 0x40)}};
    game::SpellSource source;
    source.context = &table;
    source.lookup = [](const void* context, u32 id, game::SpellInfo& out) {
        for (const game::SpellInfo& s : static_cast<const SpellTable*>(context)->spells) {
            if (s.id == id) {
                out = s;
                return true;
            }
        }
        return false;
    };
    h.play.setSpellSource(source);
    h.apply(
        created(kWolf, game::ObjectType::Unit,
                {{uf::UNIT_FIELD_HEALTH, 300}, {uf::UNIT_FIELD_PETNUMBER, 40}, {uf::UNIT_FIELD_LEVEL, 20}}));
    REQUIRE(h.packet(Opcode::SMSG_PET_SPELLS, wolfSpells()) == game::Gameplay::Handled::Yes);
    game::Abilities& abilities = h.play.abilities();
    // By name then rank, the hidden passive left out (Spell.dbc attributes 0x80).
    const std::span<const u32> book = abilities.petBook();
    REQUIRE(book.size() == 5);
    CHECK(book[0] == 17254);
    CHECK(book[1] == 17253);
    CHECK(book[2] == 4187);
    CHECK(book[3] == 14920);
    CHECK(book[4] == 14921);
    CHECK(abilities.petSpellState(4187) == net::kPetActPassive);
    CHECK(abilities.petSpellState(17253) == net::kPetActEnabled);

    // ToggleSpellAutocast: Growl 3 on (the bar's Growl follows); a passive spell cannot.
    REQUIRE(abilities.setPetSpellAutocast(14921, true));
    Bytes expected;
    net::writePetSpellAutocast(expected, kWolf, 14921, true);
    REQUIRE(h.sentWith(Opcode::CMSG_PET_SPELL_AUTOCAST).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_PET_SPELL_AUTOCAST)[0] == expected);
    CHECK(abilities.petSpellState(14921) == net::kPetActEnabled);
    CHECK(abilities.pet().slotAutocast(4));
    CHECK_FALSE(abilities.setPetSpellAutocast(4187, true));
    CHECK(h.sentWith(Opcode::CMSG_PET_SPELL_AUTOCAST).size() == 1);

    // Dragging Attack (slot 0) onto Bite (slot 3): both positions with the button that goes there.
    const u32 attack = abilities.pet().bar[0], bite = abilities.pet().bar[3];
    REQUIRE(abilities.swapPetSlots(0, 3));
    Bytes pair;
    net::writePetSetActionPair(pair, kWolf, 0, bite, 3, attack);
    REQUIRE(h.sentWith(Opcode::CMSG_PET_SET_ACTION).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_PET_SET_ACTION)[0] == pair);
    CHECK(abilities.pet().bar[0] == bite);
    CHECK(abilities.pet().bar[3] == attack);
    CHECK_FALSE(abilities.swapPetSlots(2, 2));

    // A spell of the book onto an empty spell slot, and cast from the book (CMSG_PET_ACTION with the spell).
    REQUIRE(abilities.setPetSlot(5, 17254));
    CHECK(abilities.pet().bar[5] == net::petButton(17254, net::kPetActDisabled));
    REQUIRE(abilities.castPetSpell(17253, 0xF130000000001234ull));
    Bytes action;
    net::writePetAction(action, kWolf, net::petButton(17253, net::kPetActEnabled), 0xF130000000001234ull);
    REQUIRE(h.sentWith(Opcode::CMSG_PET_ACTION).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_PET_ACTION)[0] == action);
    CHECK_FALSE(abilities.castPetSpell(4187, 0)); // passive

    // The pet gone: no book.
    REQUIRE(h.packet(Opcode::SMSG_PET_SPELLS, Bytes(8, 0)) == game::Gameplay::Handled::Yes);
    CHECK(abilities.petBook().empty());
}

namespace {

game::CombatEvent hit(game::CombatEvent::Kind kind, u64 victim, i32 amount) {
    game::CombatEvent e;
    e.kind = kind;
    e.attacker = 0xF130000000000777ull;
    e.victim = victim;
    e.amount = amount;
    return e;
}

} // namespace

TEST_CASE("Combat feedback: the UNIT_COMBATs WoW.exe fires and CombatFeedback.lua's indicator") {
    using E = game::UnitCombatEvent;
    using F = game::UnitCombatFlag;
    using K = game::CombatEvent::Kind;
    game::SpellLogFilter hidden;
    hidden.hidden = [](const void*, u32 spellId) { return spellId == 999; };
    std::array<game::UnitCombat, 2> out{};

    // A melee crit (the HitInfo's bits), a fully absorbed swing (WOUND with ABSORB), a dodge.
    game::CombatEvent swing = hit(K::Damage, kPlayer, 61);
    swing.melee = true;
    swing.hitInfo = net::kHitInfoCritical;
    REQUIRE(game::unitCombats(swing, hidden, out) == 1);
    CHECK(out[0].unit == kPlayer);
    CHECK(out[0].event == E::Wound);
    CHECK(out[0].flag == F::Critical);
    CHECK(out[0].amount == 61);
    swing = hit(K::Absorb, kPlayer, 0);
    swing.melee = true;
    swing.absorbed = 40;
    swing.hitInfo = net::kHitInfoAbsorb;
    REQUIRE(game::unitCombats(swing, hidden, out) == 1);
    CHECK(out[0].event == E::Wound);
    CHECK(out[0].flag == F::Absorb);
    swing = hit(K::Dodge, kPlayer, 0);
    swing.melee = true;
    REQUIRE(game::unitCombats(swing, hidden, out) == 1);
    CHECK(out[0].event == E::Dodge);

    // Spell damage: crit / absorb / block / resist bits; periodic without crit; a hidden spell nothing.
    game::CombatEvent spell = hit(K::Damage, kWolf, 38);
    spell.spellId = 133;
    spell.school = 2;
    spell.flags = game::CombatEvent::kCrit;
    REQUIRE(game::unitCombats(spell, hidden, out) == 1);
    CHECK(out[0].flag == F::Critical);
    CHECK(out[0].type == 2);
    spell.flags = game::CombatEvent::kCrit | game::CombatEvent::kPeriodic;
    REQUIRE(game::unitCombats(spell, hidden, out) == 1);
    CHECK(out[0].flag == F::None);
    spell = hit(K::Resist, kWolf, 0);
    spell.spellId = 133;
    spell.resisted = 50;
    REQUIRE(game::unitCombats(spell, hidden, out) == 1);
    CHECK(out[0].event == E::Wound);
    CHECK(out[0].flag == F::Resist);
    spell.spellId = 999;
    CHECK(game::unitCombats(spell, hidden, out) == 0);

    // Heals: SPELLHEALLOG's crit; a hidden periodic heal nothing (a hidden direct heal still fires).
    game::CombatEvent heal = hit(K::Heal, kPlayer, 210);
    heal.spellId = 999;
    heal.flags = game::CombatEvent::kCrit;
    REQUIRE(game::unitCombats(heal, hidden, out) == 1);
    CHECK(out[0].event == E::Heal);
    CHECK(out[0].flag == F::Critical);
    heal.flags = game::CombatEvent::kPeriodic;
    CHECK(game::unitCombats(heal, hidden, out) == 0);

    // Misses: the miss's name, a reflect on the caster too; SPELLORDAMAGE_IMMUNE, energize and drains
    // nothing.
    game::CombatEvent miss = hit(K::Reflect, kPlayer, 0);
    miss.spellId = 133;
    REQUIRE(game::unitCombats(miss, hidden, out) == 2);
    CHECK(out[0].event == E::Reflect);
    CHECK(out[1].unit == miss.attacker);
    miss.kind = K::Absorb; // a SPELLLOGMISS absorb (nothing absorbed): ABSORB, not WOUND
    REQUIRE(game::unitCombats(miss, hidden, out) == 1);
    CHECK(out[0].event == E::Absorb);
    miss.kind = K::Immune;
    miss.noFeedback = true;
    CHECK(game::unitCombats(miss, hidden, out) == 0);
    CHECK(game::unitCombats(hit(K::Energize, kPlayer, 20), hidden, out) == 0);

    // The environment: lava is fire; a damage shield strikes whoever struck its bearer.
    game::CombatEvent lava = hit(K::Damage, kPlayer, 150);
    lava.attacker = 0;
    lava.spellId = 3;
    lava.flags = game::CombatEvent::kEnvironment;
    REQUIRE(game::unitCombats(lava, hidden, out) == 1);
    CHECK(out[0].type == 2);
    game::CombatEvent thorns = hit(K::Damage, kWolf, 3);
    thorns.flags = game::CombatEvent::kDamageShield;
    thorns.school = 3;
    REQUIRE(game::unitCombats(thorns, hidden, out) == 1);
    CHECK(out[0].unit == kWolf);

    // CombatFeedback_OnCombatEvent / OnUpdate: 30 high, crit 1.5, glancing 0.75, magic yellow, heals green;
    // IMMUNE 0.5; fade in 0.2 s, hold 0.7 s, fade out 0.3 s.
    game::CombatFeedback fb;
    fb.onCombatEvent({kPlayer, E::Wound, F::Critical, 61, 0}, 1000);
    game::FeedbackView v = fb.view(1000);
    CHECK(v.shown);
    CHECK(v.amount == 61);
    CHECK(v.height == doctest::Approx(45.0f));
    CHECK(v.alpha == doctest::Approx(0.0f));
    CHECK(fb.view(1100).alpha == doctest::Approx(0.5f));
    CHECK(fb.view(1500).alpha == doctest::Approx(1.0f));
    CHECK(fb.view(1050 + 900).alpha == doctest::Approx(0.8333f).epsilon(0.01));
    CHECK_FALSE(fb.view(2200).shown);
    fb.onCombatEvent({kPlayer, E::Wound, F::Glancing, 9, 4}, 3000);
    v = fb.view(3300);
    CHECK(v.height == doctest::Approx(22.5f));
    CHECK(v.color == game::FeedbackColor::Yellow);
    fb.onCombatEvent({kPlayer, E::Wound, F::None, 0, 0}, 4000);
    CHECK(fb.view(4300).word == E::Miss);
    fb.onCombatEvent({kPlayer, E::Wound, F::Block, 0, 0}, 5000);
    v = fb.view(5300);
    CHECK(v.word == E::Block);
    CHECK(v.height == doctest::Approx(22.5f));
    fb.onCombatEvent({kPlayer, E::Immune, F::None, 0, 0}, 6000);
    CHECK(fb.view(6300).height == doctest::Approx(15.0f));
    fb.onCombatEvent({kPlayer, E::Heal, F::None, 120, 0}, 7000);
    CHECK(fb.view(7300).color == game::FeedbackColor::Green);
}

TEST_CASE("Party pets: the stats' pet fields, in and out of sight, and its auras") {
    Harness h;
    constexpr u64 kMember = 0x45;
    constexpr u64 kMemberPet = 0xF140000000000999ull;
    // SMSG_GROUP_LIST with the member (and the player as leader).
    Bytes list;
    net::ByteWriter w(list);
    w.u8v(0).u8v(0).u32v(1).cstring("Aeryn").u64v(kMember).u8v(1).u8v(0);
    w.u64v(kPlayer).u8v(0).u64v(0).u8v(0).u8v(0);
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, list) == game::Gameplay::Handled::Yes);
    // Its stats while out of sight: online, health, then the pet (guid, name, model, health, a debuff in slot
    // 32).
    Bytes stats;
    net::ByteWriter s(stats);
    const u32 mask = net::kGroupUpdateStatus | net::kGroupUpdateCurHp | net::kGroupUpdateMaxHp |
                     net::kGroupUpdatePetGuid | net::kGroupUpdatePetName | net::kGroupUpdatePetModel |
                     net::kGroupUpdatePetCurHp | net::kGroupUpdatePetMaxHp |
                     net::kGroupUpdatePetAurasNegative;
    s.packedGuid(kMember).u32v(mask).u8v(net::kMemberOnline).u16v(300).u16v(400);
    s.u64v(kMemberPet).cstring("Wolfie").u16v(903).u16v(150).u16v(300).u16v(1).u16v(1714);
    REQUIRE(h.packet(Opcode::SMSG_PARTY_MEMBER_STATS_FULL, stats) == game::Gameplay::Handled::Yes);
    std::array<game::PartyMemberView, 4> views{};
    REQUIRE(h.play.social().partyMembers(views) == 1);
    CHECK(views[0].petGuid == kMemberPet);
    CHECK(views[0].petName == "Wolfie");
    CHECK(views[0].petHealth == 150);
    CHECK(views[0].petMaxHealth == 300);
    CHECK_FALSE(views[0].petInSight);
    std::array<game::AuraInfo, 48> auras{};
    REQUIRE(h.play.auras(kMemberPet, auras) == 1);
    CHECK(auras[0].spellId == 1714);
    CHECK(auras[0].slot == 32);

    // In sight: the member's UNIT_FIELD_SUMMON names the pet, whose own fields give its health.
    h.apply(created(kMember, game::ObjectType::Player,
                    {{uf::UNIT_FIELD_HEALTH, 350},
                     {uf::UNIT_FIELD_MAXHEALTH, 400},
                     {uf::UNIT_FIELD_SUMMON, static_cast<u32>(kMemberPet)},
                     {static_cast<u16>(uf::UNIT_FIELD_SUMMON + 1), static_cast<u32>(kMemberPet >> 32)}}));
    h.apply(created(kMemberPet, game::ObjectType::Unit,
                    {{uf::UNIT_FIELD_HEALTH, 280}, {uf::UNIT_FIELD_MAXHEALTH, 300}}));
    REQUIRE(h.play.social().partyMembers(views) == 1);
    CHECK(views[0].petGuid == kMemberPet);
    CHECK(views[0].petInSight);
    CHECK(views[0].petHealth == 280);

    // The pet dismissed: the member's summon field cleared, and no frame.
    Bytes gone;
    net::ByteWriter g(gone);
    g.u32v(1).u8v(0).u8v(0).packedGuid(kMember);
    writeValues(g, {{uf::UNIT_FIELD_SUMMON, 0}, {static_cast<u16>(uf::UNIT_FIELD_SUMMON + 1), 0}});
    h.apply(gone);
    REQUIRE(h.play.social().partyMembers(views) == 1);
    CHECK(views[0].petGuid == 0);
}

TEST_CASE("Pet data: families, diets, loyalty, stable prices and happiness from db.vdpk") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    static pack::PackReader db;
    static game::PetData data;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok() || !data.init(db)) {
        MESSAGE("VD_PACKS_DIR not set or db.vdpk missing: skipping");
        return;
    }
    CHECK(data.familyName(1) == "Wolf");
    CHECK(data.familyFoodMask(1) == 1);
    CHECK(data.familyIconHash(1) == assetPathHash("Interface\\Icons\\Ability_Hunter_Pet_Wolf.blp"));
    char foods[80];
    CHECK(data.dietList(data.familyFoodMask(2), foods) == "Meat, Fish"); // a cat
    CHECK(data.dietList(0, foods).empty());
    CHECK(data.loyaltyName(3) == "(Loyalty Level 3) Submissive");
    CHECK(data.stableSlotPrice(1) == 500);
    CHECK(data.stableSlotPrice(2) == 50000);
    CHECK(data.stableSlotPrice(3) == 0);
    CHECK(data.happiness(0).level == 1);
    CHECK(data.happiness(500000).level == 2);
    CHECK(data.happiness(700000).level == 3);
    CHECK(data.happiness(700000).damagePercent == doctest::Approx(125.0f));
    CHECK(data.petNameToken(9) == "DEMON");
    CHECK(data.petNameToken(3) == "PET");
    CHECK(data.classFileName(3) == "HUNTER");
}
