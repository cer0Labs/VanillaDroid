// Phase 13 (combat): the combat log packets the client did not read before, through game::Gameplay into the combat
// log ring, and their lines with the client's texts (worded as the enUS GlobalStrings.lua has them), in the forms
// and with the arguments WoW.exe's UnitCombatLog_C gives them.
#include "TextTable.h"
#include "core/Text.h"
#include "game/Combat.h"
#include "game/CombatText.h"
#include "game/Entities.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "net/ByteBuffer.h"
#include "net/SpellLogMessages.h"

#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
using Bytes = std::vector<u8>;
using K = game::CombatEvent::Kind;

constexpr u64 kMe = 0x21;
constexpr u64 kWolf = 0xF130000000000456ull;
constexpr u64 kKobold = 0xF130000000000789ull;
constexpr u64 kImp = 0xF140000000000042ull;
constexpr u64 kChest = 0xF110000000000099ull;

struct EnglishTexts {
    std::vector<u8> blob = test::textTable({
        {"SPELLPOWERDRAINSELFOTHER", "Your %s: -%d %s on %s."},
        {"SPELLPOWERLEECHSELFOTHER", "Your %s: -%d %s on %s, +%d %s."},
        {"SPELLPOWERLEECHOTHERSELF", "%s's %s: -%d %s on you, %s +%d %s."},
        {"SPELLEXTRAATTACKSSELF", "+%d extra swings (%s)."},
        {"SPELLEXTRAATTACKSSELF_SINGULAR", "+%d extra swing (%s)."},
        {"SPELLEXTRAATTACKSOTHER", "%s: +%d swings (%s)."},
        {"TRADESKILL_LOG_FIRSTPERSON", "You create %s."},
        {"TRADESKILL_LOG_THIRDPERSON", "%s creates %s."},
        {"FEEDPET_LOG_FIRSTPERSON", "Your pet eats the %s."},
        {"SPELLINTERRUPTOTHERSELF", "%s interrupts your %s."},
        {"OPEN_LOCK_SELF", "You perform %s on %s."},
        {"SPELLDURABILITYDAMAGEOTHERSELF", "%s's %s: your %s worn."},
        {"SPELLDURABILITYDAMAGEALLOTHERSELF", "%s's %s: all worn."},
        {"SPELLDISMISSPETSELF", "Your %s is dismissed."},
        {"SPELLTERSE_SELF", "You cast %s."},
        {"SPELLTERSEPERFORM_OTHER", "%s performs %s."},
        {"SIMPLECASTSELFOTHER", "You cast %s on %s."},
        {"SIMPLEPERFORMOTHERSELF", "%s performs %s on you."},
        {"DAMAGESHIELDSELFOTHER", "Reflected %d %s at %s."},
        {"DAMAGESHIELDOTHERSELF", "%s reflected %d %s."},
        {"AURADISPELOTHER", "%s's %s is removed."},
        {"AURADISPELSELF", "Your %s is removed."},
        {"DISPELFAILEDSELFOTHER", "Dispel failed: %s's %s."},
        {"INSTAKILLOTHER", "%s is killed by %s."},
        {"INSTAKILLSELF", "You are killed by %s."},
        {"ITEMENCHANTMENTADDOTHERSELF", "%s casts %s on your %s."},
        {"ITEMENCHANTMENTADDSELFSELF", "You cast %s on your %s."},
        {"ITEMENCHANTMENTREMOVESELF", "%s gone from your %s."},
        {"UNKNOWNOBJECT", "Unknown"},
        {"UNKNOWN", "Unknown"},
        {"SPELL_SCHOOL3_CAP", "Nature"},
        {"MANA", "Mana"},
        {"RAGE", "Rage"},
    });
    EnglishTexts() { text::setTable(blob); }
    ~EnglishTexts() { text::unload(); }
};

game::CombatNames names() {
    game::CombatNames n;
    n.player = kMe;
    n.pet = kImp;
    n.unitName = [](const void*, u64 guid) -> std::string_view {
        switch (guid) {
        case kMe: return "Arthel";
        case kWolf: return "Young Wolf";
        case kKobold: return "Kobold Geomancer";
        case kImp: return "Zalgaz";
        case kChest: return "Battered Chest";
        default: return {};
        }
    };
    n.spellName = [](const void*, u32 spell) -> std::string_view {
        switch (spell) {
        case 5138: return "Drain Mana";
        case 15601: return "Hand of Justice";
        case 2550: return "Cooking";
        case 6552: return "Pummel";
        case 133: return "Fireball";
        case 1804: return "Pick Lock";
        case 467: return "Thorns";
        case 1243: return "Power Word: Fortitude";
        case 5: return "Death Touch";
        case 688: return "Summon Imp";
        case 1715: return "Hamstring";
        case 8023: return "Rockbiter Weapon";
        default: return {};
        }
    };
    n.itemName = [](const void*, u32 entry) -> std::string_view {
        switch (entry) {
        case 2681: return "Roasted Boar Meat";
        case 2488: return "Gladius";
        case 117: return "Tough Jerky";
        default: return {};
        }
    };
    n.enchantName = [](const void*, u32 id) -> std::string_view { return id == 29 ? "Rockbiter 3" : ""; };
    return n;
}

std::string line(const game::CombatEvent& e, game::CombatLineKind* kind = nullptr) {
    char out[200];
    game::formatCombatEvent(out, sizeof(out), e, names(), kind);
    return out;
}

// Gameplay over its own store; the spells it knows come from a small table.
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    u32 cursor = 0;
    u32 fxCursor = 0;

    Harness() {
        game::SpellSource source;
        source.lookup = [](const void*, u32 id, game::SpellInfo& out) {
            out = game::SpellInfo{};
            out.id = id;
            switch (id) {
            case 688: out.name = "Summon Imp"; out.effects[0] = 56; return true;
            case 1715: out.name = "Hamstring"; out.attributes = 0x10; return true; // SPELL_ATTR_ABILITY
            case 1804: out.name = "Pick Lock"; out.effects[0] = net::kEffectOpenLock; return true;
            case 9999: out.name = "Hidden"; out.attributes = 0x100; return true;
            default: return false;
            }
        };
        play.setSpellSource(source);
        play.enterWorld(kMe, 0, true, 1000);
        cursor = play.combatLog().end();
        fxCursor = play.fx().end();
    }
    void packet(Opcode opcode, const Bytes& body) {
        REQUIRE(play.handle(opcode, body, 2000) == game::Gameplay::Handled::Yes);
    }
    std::vector<game::CombatEvent> events() {
        std::vector<game::CombatEvent> out;
        while (const game::CombatEvent* e = play.combatLog().next(cursor)) {
            out.push_back(*e);
        }
        return out;
    }
};

} // namespace

TEST_CASE("Spell log execute: drains, leeches, extra attacks, items made, interrupts, durability, generic casts") {
    EnglishTexts texts;
    Harness h;
    Bytes b;
    net::ByteWriter w(b);
    // Drain Mana on the kobold (a leech: the caster gains as much), a rage drain shown in whole points.
    w.packedGuid(kMe).u32v(5138).u32v(1);
    w.u32v(net::kEffectPowerDrain).u32v(1).u64v(kKobold).u32v(110).u32v(0).f32v(1.0f);
    h.packet(Opcode::SMSG_SPELLLOGEXECUTE, b);
    std::vector<game::CombatEvent> e = h.events();
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == K::Drain);
    CHECK(e[0].gain == 110);
    CHECK(line(e[0]) == "Your Drain Mana: -110 Mana on Kobold Geomancer, +110 Mana.");
    game::CombatEvent rage = e[0];
    rage.gain = 0;
    rage.school = 1;
    rage.amount = 15;
    CHECK(line(rage) == "Your Drain Mana: -15 Rage on Kobold Geomancer.");

    b.clear();
    net::ByteWriter v(b);
    v.packedGuid(kMe).u32v(15601).u32v(1).u32v(net::kEffectAddExtraAttacks).u32v(1).u64v(kMe).u32v(1);
    h.packet(Opcode::SMSG_SPELLLOGEXECUTE, b);
    e = h.events();
    REQUIRE(e.size() == 1);
    CHECK(line(e[0]) == "+1 extra swing (Hand of Justice).");
    e[0].amount = 2;
    CHECK(line(e[0]) == "+2 extra swings (Hand of Justice).");

    // Cooking: the item made; the kobold makes one too.
    b.clear();
    net::ByteWriter c(b);
    c.packedGuid(kMe).u32v(2550).u32v(1).u32v(net::kEffectCreateItem).u32v(1).u32v(2681);
    h.packet(Opcode::SMSG_SPELLLOGEXECUTE, b);
    e = h.events();
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == K::CreateItem);
    CHECK(line(e[0]) == "You create Roasted Boar Meat.");
    e[0].attacker = kKobold;
    CHECK(line(e[0]) == "Kobold Geomancer creates Roasted Boar Meat.");

    // The wolf's Pummel interrupts the player's Fireball: the line names the spell interrupted.
    b.clear();
    net::ByteWriter p(b);
    p.packedGuid(kWolf).u32v(6552).u32v(1).u32v(net::kEffectInterruptCast).u32v(1).u64v(kMe).u32v(133);
    h.packet(Opcode::SMSG_SPELLLOGEXECUTE, b);
    e = h.events();
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == K::Interrupt);
    CHECK(line(e[0]) == "Young Wolf interrupts your Fireball.");

    // Pick Lock on a chest: OPEN_LOCK's own line (the generic one is skipped for lock spells).
    b.clear();
    net::ByteWriter l(b);
    l.packedGuid(kMe).u32v(1804).u32v(1).u32v(net::kEffectOpenLock).u32v(1).u64v(kChest);
    h.packet(Opcode::SMSG_SPELLLOGEXECUTE, b);
    e = h.events();
    REQUIRE(e.size() == 1);
    CHECK(line(e[0]) == "You perform Pick Lock on Battered Chest.");

    // Durability damage: one item, then every item (-1, -1).
    b.clear();
    net::ByteWriter d(b);
    d.packedGuid(kWolf).u32v(1243).u32v(1).u32v(net::kEffectDurabilityDamage).u32v(2);
    d.u64v(kMe).u32v(2488).u32v(0xFFFFFFFFu).u64v(kMe).u32v(0xFFFFFFFFu).u32v(0xFFFFFFFFu);
    h.packet(Opcode::SMSG_SPELLLOGEXECUTE, b);
    e = h.events();
    REQUIRE(e.size() == 2);
    CHECK(line(e[0]) == "Young Wolf's Power Word: Fortitude: your Gladius worn.");
    CHECK(line(e[1]) == "Young Wolf's Power Word: Fortitude: all worn.");

    // Summon Imp: no target is read for it (the default of UnitCombatLog_C): "You cast Summon Imp."
    b.clear();
    net::ByteWriter s(b);
    s.packedGuid(kMe).u32v(688).u32v(1).u32v(net::kEffectSummonPet).u32v(1).u64v(kImp);
    h.packet(Opcode::SMSG_SPELLLOGEXECUTE, b);
    e = h.events();
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == K::SpellCast);
    CHECK(line(e[0]) == "You cast Summon Imp.");
    // An ability performed on a unit (SPELL_ATTR_ABILITY), by another: SIMPLEPERFORM.
    b.clear();
    net::ByteWriter t(b);
    t.packedGuid(kWolf).u32v(1715).u32v(1).u32v(net::kEffectThreat).u32v(1).u64v(kMe);
    h.packet(Opcode::SMSG_SPELLLOGEXECUTE, b);
    e = h.events();
    REQUIRE(e.size() == 1);
    CHECK(line(e[0]) == "Young Wolf performs Hamstring on you.");
    // A spell hidden from the log writes nothing; the unknown dismissal names the pet.
    b.clear();
    net::ByteWriter x(b);
    x.packedGuid(kMe).u32v(9999).u32v(2).u32v(net::kEffectThreat).u32v(1).u64v(kWolf);
    x.u32v(net::kEffectDismissPet).u32v(1).u64v(kImp);
    h.packet(Opcode::SMSG_SPELLLOGEXECUTE, b);
    e = h.events();
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == K::DismissPet);
    CHECK(line(e[0]) == "Your Zalgaz is dismissed.");
    // The log-only lines move nothing: no reaction, no floating text.
    CHECK(e[0].logOnly());
    CHECK_FALSE(e[0].swing());
}

TEST_CASE("Damage shields, dispels, instant kills, enchantments, feign death and chain targets") {
    EnglishTexts texts;
    Harness h;
    Bytes b;
    net::ByteWriter(b).u64v(kMe).u64v(kWolf).u32v(3).u32v(3); // the player's Thorns hurts the wolf
    h.packet(Opcode::SMSG_SPELLDAMAGESHIELD, b);
    std::vector<game::CombatEvent> e = h.events();
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == K::Damage);
    CHECK_FALSE(e[0].swing()); // a damage shield is not a swing: no swing animation, no weapon sound
    game::CombatLineKind kind{};
    CHECK(line(e[0], &kind) == "Reflected 3 Nature at Young Wolf.");
    CHECK(kind == game::CombatLineKind::Outgoing);
    b.clear();
    net::ByteWriter(b).u64v(kWolf).u64v(kMe).u32v(4).u32v(3);
    h.packet(Opcode::SMSG_SPELLDAMAGESHIELD, b);
    CHECK(line(h.events().at(0)) == "Young Wolf reflected 4 Nature.");

    b.clear();
    net::ByteWriter(b).packedGuid(kWolf).packedGuid(kMe).u32v(1).u32v(1243);
    h.packet(Opcode::SMSG_SPELLDISPELLOG, b);
    CHECK(line(h.events().at(0)) == "Young Wolf's Power Word: Fortitude is removed.");
    b.clear();
    net::ByteWriter(b).packedGuid(kMe).packedGuid(kWolf).u32v(1).u32v(1243);
    h.packet(Opcode::SMSG_SPELLDISPELLOG, b);
    CHECK(line(h.events().at(0)) == "Your Power Word: Fortitude is removed.");
    b.clear();
    net::ByteWriter(b).u64v(kMe).u64v(kWolf).u32v(1243);
    h.packet(Opcode::SMSG_DISPEL_FAILED, b);
    CHECK(line(h.events().at(0)) == "Dispel failed: Young Wolf's Power Word: Fortitude.");

    b.clear();
    net::ByteWriter(b).u64v(kWolf).u32v(5);
    h.packet(Opcode::SMSG_SPELLINSTAKILLLOG, b);
    CHECK(line(h.events().at(0)) == "Young Wolf is killed by Death Touch.");
    b.clear();
    net::ByteWriter(b).u64v(kMe).u32v(5);
    h.packet(Opcode::SMSG_SPELLINSTAKILLLOG, b);
    CHECK(line(h.events().at(0)) == "You are killed by Death Touch.");

    // An enchantment someone else put on the player's weapon, then it fades (the owner's copy).
    b.clear();
    net::ByteWriter(b).u64v(kWolf).u64v(kMe).u32v(2488).u32v(29).u8v(0);
    h.packet(Opcode::SMSG_ENCHANTMENTLOG, b);
    CHECK(line(h.events().at(0)) == "Young Wolf casts Rockbiter 3 on your Gladius.");
    b.clear();
    net::ByteWriter(b).u64v(kMe).u64v(kMe).u32v(2488).u32v(29).u8v(0);
    h.packet(Opcode::SMSG_ENCHANTMENTLOG, b);
    CHECK(line(h.events().at(0)) == "You cast Rockbiter 3 on your Gladius.");
    b.clear();
    net::ByteWriter(b).u64v(0).u64v(kMe).u32v(2488).u32v(29).u8v(0);
    h.packet(Opcode::SMSG_ENCHANTMENTLOG, b);
    CHECK(line(h.events().at(0)) == "Rockbiter 3 gone from your Gladius.");

    // Feign Death resisted: the UIErrorsFrame's text (an event; the body is empty).
    u32 events = h.play.events().end();
    REQUIRE(h.play.handle(Opcode::SMSG_FEIGN_DEATH_RESISTED, {}, 2000) == game::Gameplay::Handled::Yes);
    const game::GameEvent* resisted = h.play.events().next(events);
    REQUIRE(resisted);
    CHECK(resisted->kind == game::GameEventKind::FeignDeathResisted);

    // Chain Lightning's jumps, in order, for the beams.
    b.clear();
    net::ByteWriter(b).u64v(kMe).u32v(421).u32v(2).u64v(kWolf).u64v(kKobold);
    h.packet(Opcode::SMSG_SPELL_UPDATE_CHAIN_TARGETS, b);
    const game::FxEvent* first = h.play.fx().next(h.fxCursor);
    const game::FxEvent* second = h.play.fx().next(h.fxCursor);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->kind == game::FxEventKind::ChainTarget);
    CHECK(first->guid == kMe);
    CHECK(first->other == kWolf);
    CHECK(first->id == 421);
    CHECK(second->other == kKobold);
    CHECK(second->value[0] == 1);
    CHECK(second->value[1] == 2);
    // Malformed bodies are reported.
    CHECK(h.play.handle(Opcode::SMSG_SPELLDAMAGESHIELD, Bytes(5), 2000) == game::Gameplay::Handled::Malformed);
}
