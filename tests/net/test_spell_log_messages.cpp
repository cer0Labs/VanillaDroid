// Phase 13 (combat): the movement control packets (net/ControlMessages) and the combat log packets the client did
// not read before (net/SpellLogMessages), byte for byte as vmangos-core 4641790 writes (server -> client) and reads
// (client -> server) them; every truncation and the malformed bodies rejected.
#include "net/ByteBuffer.h"
#include "net/ControlMessages.h"
#include "net/SpellLogMessages.h"

#include <doctest/doctest.h>

#include <cstring>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace vd;
using Bytes = std::vector<u8>;

constexpr u64 kMe = 0x21;
constexpr u64 kWolf = 0xF130000000000456ull;
constexpr u64 kImp = 0xF140000000000042ull;

// Every prefix shorter than the whole payload must be rejected by `parse`.
template <typename F> void checkTruncations(const Bytes& payload, F&& parse) {
    for (usize n = 0; n < payload.size(); ++n) {
        CHECK_FALSE(parse(std::span<const u8>(payload.data(), n)));
    }
}

net::MovementInfo standing(f32 x, f32 y, f32 z, f32 o, u32 time) {
    net::MovementInfo info;
    info.time = time;
    info.position = {x, y, z, o};
    return info;
}

} // namespace

TEST_CASE("Client control update, time skipped and the mover's packets") {
    // ClientControlUpdate::AppendBodyTo: packed mover GUID + u8 allowMove.
    Bytes body;
    net::ByteWriter(body).packedGuid(kWolf).u8v(1);
    net::ClientControlUpdate control;
    REQUIRE(net::parseClientControlUpdate(body, control));
    CHECK(control.guid == kWolf);
    CHECK(control.allowMove);
    checkTruncations(body, [&](std::span<const u8> b) { return net::parseClientControlUpdate(b, control); });
    body.clear();
    net::ByteWriter(body).packedGuid(kMe).u8v(0);
    REQUIRE(net::parseClientControlUpdate(body, control));
    CHECK(control.guid == kMe);
    CHECK_FALSE(control.allowMove);

    // MSG_MOVE_TIME_SKIPPED relayed by HandleMoveTimeSkippedOpcode: packed GUID + u32 lag.
    body.clear();
    net::ByteWriter(body).packedGuid(0x42).u32v(1750);
    net::MoveTimeSkipped skipped;
    REQUIRE(net::parseMoveTimeSkipped(body, skipped));
    CHECK(skipped.guid == 0x42);
    CHECK(skipped.lagMs == 1750);
    checkTruncations(body, [&](std::span<const u8> b) { return net::parseMoveTimeSkipped(b, skipped); });

    // Client -> server, as MoveTimeSkipped / MoveNotActiveMover / SetActiveMover / FarSight::ReadFromWorldPacket read
    // them: ObjectGuid is a full u64 there.
    Bytes out;
    net::writeMoveTimeSkipped(out, kMe, 1750);
    Bytes expected;
    net::ByteWriter(expected).u64v(kMe).u32v(1750);
    CHECK(out == expected);

    out.clear();
    const net::MovementInfo info = standing(-8913.5f, -137.25f, 81.5f, 1.5f, 123456);
    net::writeMoveNotActiveMover(out, kMe, info);
    expected.clear();
    net::ByteWriter e(expected);
    e.u64v(kMe);
    net::writeMovementInfo(e, info);
    CHECK(out == expected);
    REQUIRE(out.size() == 8 + 4 + 4 + 16 + 4); // guid, flags, time, x y z o, fall time
    net::ByteReader r(std::span<const u8>(out).subspan(8));
    const net::MovementInfo back = net::readMovementInfo(r);
    CHECK(back.position.x == info.position.x);
    CHECK(back.time == 123456);

    out.clear();
    net::writeSetActiveMover(out, kWolf);
    expected.clear();
    net::ByteWriter(expected).u64v(kWolf);
    CHECK(out == expected);

    out.clear();
    net::writeFarSight(out, true);
    CHECK(out == Bytes{1});
    out.clear();
    net::writeFarSight(out, false);
    CHECK(out == Bytes{0});
}

TEST_CASE("Spell log execute: each effect's entries as SpellLogExecute writes them") {
    // Drain Mana (POWER_DRAIN with a multiplier) on the wolf, then a summoned imp (a GUID only), then a created item.
    Bytes body;
    net::ByteWriter w(body);
    w.packedGuid(kMe).u32v(5138).u32v(3);
    w.u32v(net::kEffectPowerDrain).u32v(1).u64v(kWolf).u32v(110).u32v(0).f32v(1.0f);
    w.u32v(net::kEffectSummonPet).u32v(1).u64v(kImp);
    w.u32v(net::kEffectCreateItem).u32v(1).u32v(5512);
    net::SpellLogExecute log;
    REQUIRE(net::parseSpellLogExecute(body, log));
    CHECK(log.caster == kMe);
    CHECK(log.spellId == 5138);
    REQUIRE(log.effectCount == 3);
    REQUIRE(log.entries.size() == 3);
    CHECK(log.effects[0].effect == net::kEffectPowerDrain);
    CHECK(log.entries[0].target == kWolf);
    CHECK(log.entries[0].value == 110);
    CHECK(log.entries[0].value2 == 0);
    CHECK(log.entries[0].multiplier == 1.0f);
    CHECK(log.effects[1].first == 1);
    CHECK(log.entries[1].target == kImp);
    CHECK(log.effects[2].effect == net::kEffectCreateItem);
    CHECK(log.entries[2].target == 0);
    CHECK(log.entries[2].value == 5512);
    checkTruncations(body, [&](std::span<const u8> b) { return net::parseSpellLogExecute(b, log); });

    // Extra attacks, an interrupt (Pummel), durability damage of every item (-1, -1), heals (only written for
    // builds <= 1.9.4: the layout is still read), energize, and an effect the server never logs (no bytes).
    body.clear();
    net::ByteWriter v(body);
    v.packedGuid(kWolf).u32v(6603).u32v(3);
    v.u32v(net::kEffectAddExtraAttacks).u32v(1).u64v(kMe).u32v(2);
    v.u32v(net::kEffectInterruptCast).u32v(2).u64v(kMe).u32v(133).u64v(kImp).u32v(3110);
    v.u32v(net::kEffectDurabilityDamage).u32v(1).u64v(kMe).u32v(0xFFFFFFFFu).u32v(0xFFFFFFFFu);
    REQUIRE(net::parseSpellLogExecute(body, log));
    REQUIRE(log.entries.size() == 4);
    CHECK(log.entries[0].value == 2);
    CHECK(log.entries[1].value == 133);
    CHECK(log.entries[2].target == kImp);
    CHECK(log.entries[2].value == 3110);
    CHECK(log.entries[3].value == 0xFFFFFFFFu);
    CHECK(log.entries[3].value2 == 0xFFFFFFFFu);

    body.clear();
    net::ByteWriter h(body);
    h.packedGuid(kMe).u32v(2054).u32v(3);
    h.u32v(net::kEffectHeal).u32v(1).u64v(kMe).u32v(52).u8v(1);
    h.u32v(net::kEffectEnergize).u32v(1).u64v(kMe).u32v(15).u32v(1);
    h.u32v(net::kEffectLearnSpell).u32v(2);
    REQUIRE(net::parseSpellLogExecute(body, log));
    REQUIRE(log.entries.size() == 4);
    CHECK(log.entries[0].value == 52);
    CHECK(log.entries[0].critical);
    CHECK(log.entries[1].value == 15);
    CHECK(log.entries[1].value2 == 1);
    CHECK(log.entries[2].target == 0);

    // Malformed: four effects, or more entries than the cap.
    Bytes tooMany;
    net::ByteWriter(tooMany).packedGuid(kMe).u32v(1).u32v(4);
    CHECK_FALSE(net::parseSpellLogExecute(tooMany, log));
    Bytes huge;
    net::ByteWriter(huge).packedGuid(kMe).u32v(1).u32v(1).u32v(net::kEffectCreateItem).u32v(100000);
    CHECK_FALSE(net::parseSpellLogExecute(huge, log));
}

TEST_CASE("Damage shields, instant kills, dispels, enchantments and chain targets") {
    // SpellDamageShield: full bearer, full attacker, damage, school (Thorns: nature 3).
    Bytes body;
    net::ByteWriter(body).u64v(kMe).u64v(kWolf).u32v(9).u32v(3);
    net::SpellDamageShield shield;
    REQUIRE(net::parseSpellDamageShield(body, shield));
    CHECK(shield.bearer == kMe);
    CHECK(shield.attacker == kWolf);
    CHECK(shield.damage == 9);
    CHECK(shield.school == 3);
    checkTruncations(body, [&](std::span<const u8> b) { return net::parseSpellDamageShield(b, shield); });

    // EffectInstaKill: full victim + spell.
    body.clear();
    net::ByteWriter(body).u64v(kWolf).u32v(5);
    net::SpellInstaKillLog kill;
    REQUIRE(net::parseSpellInstaKillLog(body, kill));
    CHECK(kill.victim == kWolf);
    CHECK(kill.spellId == 5);
    checkTruncations(body, [&](std::span<const u8> b) { return net::parseSpellInstaKillLog(b, kill); });

    // SMSG_SPELLDISPELLOG (>= 1.12.1): packed victim, packed caster, count, spells.
    body.clear();
    net::ByteWriter(body).packedGuid(kWolf).packedGuid(kMe).u32v(2).u32v(1243).u32v(1459);
    net::SpellDispelLog dispel;
    REQUIRE(net::parseSpellDispelLog(body, dispel));
    CHECK(dispel.victim == kWolf);
    CHECK(dispel.caster == kMe);
    REQUIRE(dispel.count == 2);
    CHECK(dispel.spells[1] == 1459);
    checkTruncations(body, [&](std::span<const u8> b) { return net::parseSpellDispelLog(b, dispel); });
    Bytes lying;
    net::ByteWriter(lying).packedGuid(kWolf).packedGuid(kMe).u32v(1000);
    CHECK_FALSE(net::parseSpellDispelLog(lying, dispel));

    // SMSG_DISPEL_FAILED: full caster, full victim, then the spells to the end (no count).
    body.clear();
    net::ByteWriter(body).u64v(kMe).u64v(kWolf).u32v(1243).u32v(1459);
    net::DispelFailed failed;
    REQUIRE(net::parseDispelFailed(body, failed));
    CHECK(failed.caster == kMe);
    CHECK(failed.victim == kWolf);
    REQUIRE(failed.count == 2);
    CHECK(failed.spells[0] == 1243);
    CHECK_FALSE(net::parseDispelFailed(std::span<const u8>(body.data(), body.size() - 1), failed)); // half a spell
    CHECK_FALSE(net::parseDispelFailed(std::span<const u8>(body.data(), 16), failed));              // no spell
    CHECK_FALSE(net::parseDispelFailed(std::span<const u8>(body.data(), 9), failed));

    // EnchantmentLog::AppendBodyTo: caster (0: faded), owner, item, enchantment, u8 affiliation.
    body.clear();
    net::ByteWriter(body).u64v(kWolf).u64v(kMe).u32v(2488).u32v(250).u8v(1);
    net::EnchantmentLog enchant;
    REQUIRE(net::parseEnchantmentLog(body, enchant));
    CHECK(enchant.caster == kWolf);
    CHECK(enchant.owner == kMe);
    CHECK(enchant.itemEntry == 2488);
    CHECK(enchant.enchantId == 250);
    CHECK(enchant.showAffiliation);
    checkTruncations(body, [&](std::span<const u8> b) { return net::parseEnchantmentLog(b, enchant); });

    // SpellUpdateChainTargets: full caster, spell, count, full GUIDs.
    body.clear();
    net::ByteWriter(body).u64v(kMe).u32v(421).u32v(3).u64v(kWolf).u64v(kImp).u64v(0x55);
    net::SpellChainTargets chain;
    REQUIRE(net::parseSpellChainTargets(body, chain));
    CHECK(chain.caster == kMe);
    CHECK(chain.spellId == 421);
    REQUIRE(chain.count == 3);
    CHECK(chain.targets[2] == 0x55);
    checkTruncations(body, [&](std::span<const u8> b) { return net::parseSpellChainTargets(b, chain); });
}
