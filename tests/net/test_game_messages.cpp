// Gameplay packets (phase 7): every parser against payloads written field by field as vmangos-core 4641790
// writes them (build 5875), including every truncation of each (parsers must refuse, never read past the
// end), and every builder against the exact bytes the server reads.
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/Messages.h"

#include <doctest/doctest.h>

#include <cstring>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace {

using namespace vd;
using namespace vd::net;

using Bytes = std::vector<u8>;

// Every strict prefix of a valid packet must be refused.
void checkTruncations(const Bytes& full, const std::function<bool(std::span<const u8>)>& parse) {
    for (usize length = 0; length < full.size(); ++length) {
        INFO("truncated to " << length << " of " << full.size() << " bytes");
        CHECK_FALSE(parse(std::span<const u8>(full.data(), length)));
    }
}

Bytes bytes(std::initializer_list<int> list) {
    Bytes out;
    for (int b : list) {
        out.push_back(static_cast<u8>(b));
    }
    return out;
}

// ByteBuffer::appendPackGUID of the server.
constexpr u64 kPlayer = 0x0000000000000011ull;   // packs to 01 11
constexpr u64 kCreature = 0xF130000123000456ull; // packs to db 56 04 23 01 30 f1 (mask 0b11011011)
constexpr u64 kObject = 0xF110000000000777ull;

} // namespace

TEST_CASE("Packed GUIDs as the server writes them") {
    Bytes out;
    ByteWriter(out).packedGuid(kCreature).packedGuid(0).packedGuid(kPlayer);
    CHECK(out == bytes({0xDB, 0x56, 0x04, 0x23, 0x01, 0x30, 0xF1, 0x00, 0x01, 0x11}));
    ByteReader r(out);
    CHECK(r.packedGuid() == kCreature);
    CHECK(r.packedGuid() == 0);
    CHECK(r.packedGuid() == kPlayer);
    CHECK(r.atEnd());
}

TEST_CASE("Melee: SMSG_ATTACKSTART / ATTACKSTOP / ATTACKERSTATEUPDATE") {
    // Combat.cpp AttackStart: two full GUIDs.
    Bytes start;
    ByteWriter(start).u64v(kPlayer).u64v(kCreature);
    AttackStart as;
    REQUIRE(parseAttackStart(start, as));
    CHECK(as.attacker == kPlayer);
    CHECK(as.victim == kCreature);
    checkTruncations(start, [](auto p) {
        AttackStart m;
        return parseAttackStart(p, m);
    });

    // Combat.cpp AttackStop: packed attacker, packed victim (may be empty), u32 isDead.
    Bytes stop;
    ByteWriter(stop).packedGuid(kPlayer).packedGuid(0).u32v(1);
    AttackStop st;
    REQUIRE(parseAttackStop(stop, st));
    CHECK(st.attacker == kPlayer);
    CHECK(st.victim == 0);
    CHECK(st.dead);
    checkTruncations(stop, [](auto p) {
        AttackStop m;
        return parseAttackStop(p, m);
    });

    // Combat.cpp MeleeAttackingStateUpdate (build > 1.5.1: resist per sub damage, no meleeSpellDamage).
    Bytes update;
    ByteWriter w(update);
    w.u32v(kHitInfoAffectsVictim | kHitInfoCritical).packedGuid(kPlayer).packedGuid(kCreature).u32v(42);
    w.u8v(2);
    w.u32v(0).f32v(40.0f).u32v(40).u32v(3).u32v(0); // physical: 40, 3 absorbed
    w.u32v(2).f32v(5.0f).u32v(5).u32v(0).u32v(2);   // fire: 5, 2 resisted
    w.u32v(1).u32v(0).u32v(0).u32v(7);              // victim state NORMAL, attacker state, spell, blocked
    AttackerStateUpdate asu;
    REQUIRE(parseAttackerStateUpdate(update, asu));
    CHECK(asu.hitInfo == (kHitInfoAffectsVictim | kHitInfoCritical));
    CHECK(asu.attacker == kPlayer);
    CHECK(asu.victim == kCreature);
    CHECK(asu.totalDamage == 42);
    REQUIRE(asu.subDamageCount == 2);
    CHECK(asu.subDamages[0].absorbed == 3);
    CHECK(asu.subDamages[1].school == 2);
    CHECK(asu.subDamages[1].resisted == 2);
    CHECK(asu.victimState == VictimState::Normal);
    CHECK(asu.meleeSpellId == 0);
    CHECK(asu.blocked == 7);
    checkTruncations(update, [](auto p) {
        AttackerStateUpdate m;
        return parseAttackerStateUpdate(p, m);
    });

    // More sub damages than the struct keeps: all read, five kept.
    Bytes many;
    ByteWriter m(many);
    m.u32v(0).packedGuid(1).packedGuid(2).u32v(60).u8v(6);
    for (u32 i = 0; i < 6; ++i) {
        m.u32v(i).f32v(10.0f).u32v(10).u32v(0).u32v(0);
    }
    m.u32v(1).u32v(0).u32v(0).u32v(0);
    REQUIRE(parseAttackerStateUpdate(many, asu));
    CHECK(asu.subDamageCount == AttackerStateUpdate::kMaxSubDamages);
}

TEST_CASE("Combat log: spell damage, heal, energize, periodic, misses, immune, environment, kills") {
    // Spell.cpp SpellNonMeleeDamageLog.
    Bytes damage;
    ByteWriter(damage)
        .packedGuid(kCreature)
        .packedGuid(kPlayer)
        .u32v(133)
        .u32v(31)
        .u8v(2)
        .u32v(4)
        .u32v(5)
        .u8v(0)
        .u8v(0)
        .u32v(0)
        .u32v(kSpellHitCrit)
        .u8v(0);
    SpellDamageLog sd;
    REQUIRE(parseSpellDamageLog(damage, sd));
    CHECK(sd.target == kCreature);
    CHECK(sd.attacker == kPlayer);
    CHECK(sd.spellId == 133);
    CHECK(sd.damage == 31);
    CHECK(sd.school == 2);
    CHECK(sd.absorbed == 4);
    CHECK(sd.resisted == 5);
    CHECK_FALSE(sd.periodic);
    CHECK(sd.hitType == kSpellHitCrit);
    checkTruncations(damage, [](auto p) {
        SpellDamageLog m;
        return parseSpellDamageLog(p, m);
    });

    // Spell.cpp SpellHealLog / SpellEnergizeLog.
    Bytes heal;
    ByteWriter(heal).packedGuid(kPlayer).packedGuid(kPlayer).u32v(2050).u32v(120).u8v(1);
    SpellHealLog sh;
    REQUIRE(parseSpellHealLog(heal, sh));
    CHECK(sh.amount == 120);
    CHECK(sh.critical);
    checkTruncations(heal, [](auto p) {
        SpellHealLog m;
        return parseSpellHealLog(p, m);
    });
    Bytes energize;
    ByteWriter(energize).packedGuid(kPlayer).packedGuid(kPlayer).u32v(2687).u32v(1).u32v(20);
    SpellEnergizeLog se;
    REQUIRE(parseSpellEnergizeLog(energize, se));
    CHECK(se.powerType == 1);
    CHECK(se.amount == 20);
    checkTruncations(energize, [](auto p) {
        SpellEnergizeLog m;
        return parseSpellEnergizeLog(p, m);
    });

    // Unit::SendPeriodicAuraLog: damage, heal, energize and mana leech forms.
    Bytes dot;
    ByteWriter(dot)
        .packedGuid(kCreature)
        .packedGuid(kPlayer)
        .u32v(172)
        .u32v(1)
        .u32v(kAuraPeriodicDamage)
        .u32v(9)
        .u32v(5)
        .u32v(1)
        .u32v(2);
    PeriodicAuraLog pa;
    REQUIRE(parsePeriodicAuraLog(dot, pa));
    CHECK(pa.auraType == kAuraPeriodicDamage);
    CHECK(pa.amount == 9);
    CHECK(pa.school == 5);
    CHECK(pa.absorbed == 1);
    CHECK(pa.resisted == 2);
    checkTruncations(dot, [](auto p) {
        PeriodicAuraLog m;
        return parsePeriodicAuraLog(p, m);
    });
    Bytes hot;
    ByteWriter(hot)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(774)
        .u32v(1)
        .u32v(kAuraPeriodicHeal)
        .u32v(12);
    REQUIRE(parsePeriodicAuraLog(hot, pa));
    CHECK(pa.amount == 12);
    Bytes leech;
    ByteWriter(leech)
        .packedGuid(kCreature)
        .packedGuid(kPlayer)
        .u32v(5138)
        .u32v(1)
        .u32v(kAuraPeriodicManaLeech)
        .u32v(0)
        .u32v(30)
        .f32v(1.5f);
    REQUIRE(parsePeriodicAuraLog(leech, pa));
    CHECK(pa.powerType == 0);
    CHECK(pa.amount == 30);
    CHECK(pa.multiplier == 1.5f);
    checkTruncations(leech, [](auto p) {
        PeriodicAuraLog m;
        return parsePeriodicAuraLog(p, m);
    });
    Bytes unknownAura;
    ByteWriter(unknownAura).packedGuid(1).packedGuid(1).u32v(1).u32v(1).u32v(99).u32v(0);
    CHECK_FALSE(parsePeriodicAuraLog(unknownAura, pa)); // the server never sends another type

    // Spell.cpp SpellLogMiss: spell, full caster GUID, extended flag, count, (full GUID, miss) pairs.
    Bytes miss;
    ByteWriter(miss).u32v(116).u64v(kPlayer).u8v(0).u32v(2).u64v(kCreature).u8v(2).u64v(kObject).u8v(7);
    SpellLogMiss lm;
    REQUIRE(parseSpellLogMiss(miss, lm));
    CHECK(lm.caster == kPlayer);
    REQUIRE(lm.count == 2);
    CHECK(lm.entries[0].target == kCreature);
    CHECK(lm.entries[0].miss == SpellMiss::Resist);
    CHECK(lm.entries[1].miss == SpellMiss::Immune);
    checkTruncations(miss, [](auto p) {
        SpellLogMiss m;
        return parseSpellLogMiss(p, m);
    });
    Bytes huge;
    ByteWriter(huge).u32v(1).u64v(1).u8v(0).u32v(0x7FFFFFFF);
    CHECK_FALSE(parseSpellLogMiss(huge, lm)); // a count the payload cannot hold

    // Spell.cpp SpellOrDamageImmune / ProcResist; Combat.cpp EnvironmentalDamageLog / PartyKillLog.
    Bytes immune;
    ByteWriter(immune).u64v(kPlayer).u64v(kCreature).u32v(118).u8v(0);
    SpellImmuneLog im;
    REQUIRE(parseSpellImmuneLog(immune, im));
    CHECK(im.target == kCreature);
    CHECK(im.spellId == 118);
    checkTruncations(immune, [](auto p) {
        SpellImmuneLog m;
        return parseSpellImmuneLog(p, m);
    });
    Bytes env;
    ByteWriter(env).u64v(kPlayer).u8v(2).u32v(57).u32v(0).u32v(0);
    EnvironmentalDamageLog ed;
    REQUIRE(parseEnvironmentalDamageLog(env, ed));
    CHECK(ed.type == 2);
    CHECK(ed.damage == 57);
    checkTruncations(env, [](auto p) {
        EnvironmentalDamageLog m;
        return parseEnvironmentalDamageLog(p, m);
    });
    Bytes kill;
    ByteWriter(kill).u64v(kPlayer).u64v(kCreature);
    PartyKillLog pk;
    REQUIRE(parsePartyKillLog(kill, pk));
    CHECK(pk.killer == kPlayer);
    CHECK(pk.victim == kCreature);
    checkTruncations(kill, [](auto p) {
        PartyKillLog m;
        return parsePartyKillLog(p, m);
    });
}

TEST_CASE("Spellbook: SMSG_INITIAL_SPELLS, LEARNED / REMOVED / SUPERCEDED, ACTION_BUTTONS") {
    // Spell.cpp InitialSpells: u8 0, u16 count, (u16 id, i16 0)..., u16 count, (u16, u16, u16, i32, i32)...
    Bytes initial;
    ByteWriter w(initial);
    w.u8v(0).u16v(3).u16v(78).u16v(0).u16v(6603).u16v(0).u16v(2457).u16v(0);
    w.u16v(2);
    w.u16v(100).u16v(0).u16v(0).u32v(4000).u32v(0);              // Charge: 4 s left
    w.u16v(2687).u16v(0).u16v(12).u32v(1).u32v(0x80000000u | 0); // permanent (on event)
    InitialSpells is;
    REQUIRE(parseInitialSpells(initial, is));
    CHECK(is.spells == std::vector<u16>{78, 6603, 2457});
    REQUIRE(is.cooldowns.size() == 2);
    CHECK(is.cooldowns[0].spellId == 100);
    CHECK(is.cooldowns[0].recoveryMs == 4000);
    CHECK(is.cooldowns[1].category == 12);
    CHECK((static_cast<u32>(is.cooldowns[1].categoryRecoveryMs) & 0x80000000u) != 0);
    checkTruncations(initial, [](auto p) {
        InitialSpells m;
        return parseInitialSpells(p, m);
    });

    u32 id = 0, other = 0;
    REQUIRE(parseLearnedSpell(bytes({0x4E, 0x00, 0x00, 0x00}), id)); // u16 78, i16 0
    CHECK(id == 78);
    CHECK_FALSE(parseLearnedSpell(bytes({0x4E, 0x00, 0x00}), id));
    REQUIRE(parseRemovedSpell(bytes({0x4E, 0x00}), id)); // only the u16
    CHECK(id == 78);
    CHECK_FALSE(parseRemovedSpell(bytes({0x4E}), id));
    REQUIRE(parseSupercededSpell(bytes({0x4E, 0x00, 0x8C, 0x01}), id, other));
    CHECK(id == 78);
    CHECK(other == 396);

    // MasterPlayer.cpp: exactly 120 packed u32, no count.
    Bytes buttons;
    ByteWriter b(buttons);
    for (u32 i = 0; i < kActionButtonCount; ++i) {
        b.u32v(i == 0 ? 78u : i == 11 ? (0x80u << 24 | 159u) : 0u);
    }
    std::array<u32, kActionButtonCount> parsed{};
    REQUIRE(parseActionButtons(buttons, parsed));
    CHECK(parsed[0] == 78);
    CHECK((parsed[11] >> 24) == kActionItem);
    CHECK((parsed[11] & 0xFFFFFF) == 159);
    checkTruncations(buttons, [](auto p) {
        std::array<u32, kActionButtonCount> m{};
        return parseActionButtons(p, m);
    });
}

TEST_CASE("Casting: SMSG_SPELL_START / SPELL_GO / SPELL_FAILED_OTHER / CAST_RESULT / cooldowns / channels") {
    // Spell.cpp SpellStart: packed caster, packed unit caster, spell, u16 flags, u32 cast time, targets
    // (unit).
    Bytes start;
    ByteWriter(start)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(133)
        .u16v(0x0002)
        .u32v(1500)
        .u16v(kTargetUnit)
        .packedGuid(kCreature);
    SpellStart ss;
    REQUIRE(parseSpellStart(start, ss));
    CHECK(ss.unitCaster == kPlayer);
    CHECK(ss.spellId == 133);
    CHECK(ss.castTimeMs == 1500);
    CHECK(ss.targets.mask == kTargetUnit);
    CHECK(ss.targets.unit == kCreature);
    checkTruncations(start, [](auto p) {
        SpellStart m;
        return parseSpellStart(p, m);
    });
    // Ranged: ammo fields follow the targets.
    Bytes ranged;
    ByteWriter(ranged)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(75)
        .u16v(0x0022)
        .u32v(0)
        .u16v(kTargetUnit)
        .packedGuid(kCreature)
        .u32v(5996)
        .u32v(24);
    REQUIRE(parseSpellStart(ranged, ss));
    checkTruncations(ranged, [](auto p) {
        SpellStart m;
        return parseSpellStart(p, m);
    });

    // Spell.cpp SpellGo: hit and miss lists of FULL GUIDs; a reflect carries one more byte.
    Bytes go;
    ByteWriter(go)
        .packedGuid(kPlayer)
        .packedGuid(kPlayer)
        .u32v(133)
        .u16v(0x0100)
        .u8v(1)
        .u64v(kCreature)
        .u8v(2)
        .u64v(kObject)
        .u8v(2)
        .u64v(0x33)
        .u8v(11)
        .u8v(1)
        .u16v(kTargetUnit)
        .packedGuid(kCreature);
    SpellGo sg;
    REQUIRE(parseSpellGo(go, sg));
    CHECK(sg.hits == std::vector<u64>{kCreature});
    REQUIRE(sg.misses.size() == 2);
    CHECK(sg.misses[0].miss == SpellMiss::Resist);
    CHECK(sg.misses[1].miss == SpellMiss::Reflect);
    CHECK(sg.targets.unit == kCreature);
    checkTruncations(go, [](auto p) {
        SpellGo m;
        return parseSpellGo(p, m);
    });

    // SpellCastTargets written by the server: one GUID by priority (unit > game object > corpse), then
    // item...
    Bytes targets;
    ByteWriter(targets)
        .u16v(kTargetGameObject | kTargetItem | kTargetDestLocation | kTargetString)
        .packedGuid(kObject)
        .packedGuid(0x55)
        .f32v(1.0f)
        .f32v(2.0f)
        .f32v(3.0f)
        .cstring("x");
    ByteReader tr(targets);
    SpellCastTargets t;
    readSpellCastTargets(tr, t);
    REQUIRE(tr.ok());
    CHECK(tr.atEnd());
    CHECK(t.gameObject == kObject);
    CHECK(t.item == 0x55);
    CHECK(t.destination[2] == 3.0f);
    CHECK(t.text == "x");

    Bytes failed;
    ByteWriter(failed).u64v(kPlayer).u32v(133);
    SpellFailedOther sf;
    REQUIRE(parseSpellFailedOther(failed, sf));
    CHECK(sf.caster == kPlayer);
    checkTruncations(failed, [](auto p) {
        SpellFailedOther m;
        return parseSpellFailedOther(p, m);
    });

    // Spell.cpp CastResult: OK = spell + 0; failure = spell + 2 + reason [+ arguments].
    CastResult cr;
    REQUIRE(parseCastResult(bytes({0x85, 0, 0, 0, 0}), cr));
    CHECK(cr.status == kCastStatusOk);
    REQUIRE(parseCastResult(bytes({0x85, 0, 0, 0, 2, 89}), cr));
    CHECK(cr.status == kCastStatusFailed);
    CHECK(cr.reason == kCastFailedOutOfRange);
    CHECK(cr.argCount == 0);
    REQUIRE(
        parseCastResult(bytes({0x85, 0, 0, 0, 2, 94, 3, 0, 0, 0}), cr)); // REQUIRES_SPELL_FOCUS + focus id
    CHECK(cr.argCount == 1);
    CHECK(cr.args[0] == 3);
    REQUIRE(parseCastResult(bytes({0x85, 0, 0, 0, 2, 25, 2, 0, 0, 0, 0x10, 0, 0, 0}),
                            cr)); // class + subclass mask
    CHECK(cr.argCount == 2);
    CHECK(cr.args[1] == 0x10);
    CHECK_FALSE(parseCastResult(bytes({0x85, 0, 0, 0, 2}), cr)); // failure without its reason
    CHECK_FALSE(parseCastResult(bytes({0x85, 0, 0, 0}), cr));
    CHECK(std::string(spellCastResultName(kCastFailedOutOfRange)) == "OUT_OF_RANGE");
    CHECK(std::string(spellCastResultName(kCastFailedUnitNotInfront)) == "UNIT_NOT_INFRONT");
    CHECK(std::string(spellCastResultName(145)) == "UNKNOWN");

    // Player.cpp SMSG_SPELL_COOLDOWN: full GUID then (spell, ms) pairs to the end, no count, no flags.
    Bytes cooldown;
    ByteWriter(cooldown).u64v(kPlayer).u32v(116).u32v(4000).u32v(133).u32v(4000);
    SpellCooldown sc;
    REQUIRE(parseSpellCooldown(cooldown, sc));
    CHECK(sc.caster == kPlayer);
    REQUIRE(sc.entries.size() == 2);
    CHECK(sc.entries[1].spellId == 133);
    Bytes odd(cooldown.begin(), cooldown.end() - 3);
    CHECK_FALSE(parseSpellCooldown(odd, sc)); // a broken pair
    CHECK_FALSE(parseSpellCooldown(bytes({1, 2, 3}), sc));

    u32 spellId = 0;
    u64 guid = 0;
    Bytes event;
    ByteWriter(event).u32v(2687).u64v(kPlayer);
    REQUIRE(parseCooldownEvent(event, spellId, guid));
    CHECK(spellId == 2687);
    CHECK(guid == kPlayer);
    checkTruncations(event, [](auto p) {
        u32 a = 0;
        u64 b = 0;
        return parseCooldownEvent(p, a, b);
    });

    Bytes delayed;
    ByteWriter(delayed).u64v(kPlayer).u32v(500);
    SpellDelayed sd;
    REQUIRE(parseSpellDelayed(delayed, sd));
    CHECK(sd.delayMs == 500);
    ChannelStart cs;
    REQUIRE(parseChannelStart(bytes({0xA0, 0x13, 0, 0, 0x88, 0x13, 0, 0}), cs));
    CHECK(cs.spellId == 5024);
    CHECK(cs.durationMs == 5000);
    u32 remaining = 0;
    REQUIRE(parseChannelUpdate(bytes({0, 0, 0, 0}), remaining));
    CHECK(remaining == 0);
    u8 slot = 0;
    REQUIRE(parseUpdateAuraDuration(bytes({33, 0x30, 0x75, 0, 0}), slot, remaining));
    CHECK(slot == 33);
    CHECK(remaining == 30000);
    CHECK_FALSE(parseUpdateAuraDuration(bytes({33, 0x30, 0x75, 0}), slot, remaining));
}

TEST_CASE("Gossip: SMSG_GOSSIP_MESSAGE, SMSG_NPC_TEXT_UPDATE, SMSG_QUESTGIVER_STATUS / QUEST_LIST") {
    // GossipDef.cpp SendGossipMenu (build > 1.5.1: u8 icon, u8 coded).
    Bytes gossip;
    ByteWriter w(gossip);
    w.u64v(kCreature).u32v(2).u32v(2);
    w.u32v(0).u8v(0).u8v(0).cstring("Tell me about the kobolds.");
    w.u32v(1).u8v(3).u8v(1).cstring("Train me.");
    w.u32v(1).u32v(783).u32v(5).u32v(1).cstring("A Threat Within");
    GossipMessage gm;
    REQUIRE(parseGossipMessage(gossip, gm));
    CHECK(gm.source == kCreature);
    CHECK(gm.textId == 2);
    REQUIRE(gm.optionCount == 2);
    CHECK(gm.options[1].icon == 3);
    CHECK(gm.options[1].coded);
    CHECK(gm.options[1].text == "Train me.");
    REQUIRE(gm.questCount == 1);
    CHECK(gm.quests[0].questId == 783);
    CHECK(gm.quests[0].icon == 5);
    CHECK(gm.quests[0].title == "A Threat Within");
    checkTruncations(gossip, [](auto p) {
        GossipMessage m;
        return parseGossipMessage(p, m);
    });
    Bytes tooMany;
    ByteWriter(tooMany).u64v(1).u32v(1).u32v(GossipMessage::kMaxItems + 1);
    CHECK_FALSE(parseGossipMessage(tooMany, gm));

    // Query.cpp NpcTextUpdate: id, then 8 x (f32, male, female, language, 3 x (delay, emote)).
    Bytes text;
    ByteWriter t(text);
    t.u32v(2);
    for (u32 i = 0; i < 8; ++i) {
        t.f32v(i == 0 ? 1.0f : 0.0f).cstring(i == 0 ? "Hello, $N." : "").cstring(i == 0 ? "" : "").u32v(7);
        t.u32v(0).u32v(1).u32v(0).u32v(0).u32v(0).u32v(0);
    }
    NpcTextUpdate nt;
    REQUIRE(parseNpcTextUpdate(text, nt));
    CHECK(nt.textId == 2);
    CHECK(nt.options[0].maleText == "Hello, $N.");
    CHECK(nt.options[0].femaleText.empty());
    CHECK(nt.options[0].emotes[0] == 1);
    CHECK(nt.options[7].language == 7);
    checkTruncations(text, [](auto p) {
        NpcTextUpdate m;
        return parseNpcTextUpdate(p, m);
    });

    u64 guid = 0;
    u32 status = 0;
    Bytes st;
    ByteWriter(st).u64v(kCreature).u32v(5); // the status is a u32
    REQUIRE(parseQuestGiverStatus(st, guid, status));
    CHECK(status == 5);
    checkTruncations(st, [](auto p) {
        u64 a = 0;
        u32 b = 0;
        return parseQuestGiverStatus(p, a, b);
    });

    // Quest.cpp QuestGiverQuestList: guid, greeting, delay, emote, u8 count, (id, icon, level, title)...
    Bytes list;
    ByteWriter(list)
        .u64v(kCreature)
        .cstring("Greetings, $c.")
        .u32v(0)
        .u32v(1)
        .u8v(2)
        .u32v(33)
        .u32v(5)
        .u32v(2)
        .cstring("Wolves Across the Border")
        .u32v(7)
        .u32v(3)
        .u32v(1)
        .cstring("Kobold Camp Cleanup");
    QuestGiverQuestList ql;
    REQUIRE(parseQuestGiverQuestList(list, ql));
    CHECK(ql.greeting == "Greetings, $c.");
    REQUIRE(ql.questCount == 2);
    CHECK(ql.quests[1].questId == 7);
    CHECK(ql.quests[1].icon == 3);
    checkTruncations(list, [](auto p) {
        QuestGiverQuestList m;
        return parseQuestGiverQuestList(p, m);
    });
}

TEST_CASE("Quests: details, request items, offer reward, complete, failures, query response, updates") {
    // Quest.cpp QuestGiverQuestDetails (rewards shown).
    Bytes details;
    ByteWriter w(details);
    w.u64v(kCreature)
        .u32v(783)
        .cstring("A Threat Within")
        .cstring("Details $N")
        .cstring("Speak with Marshal")
        .u32v(1);
    w.u32v(2).u32v(1).u32v(1).u32v(100).u32v(2).u32v(1).u32v(200); // two choices
    w.u32v(1).u32v(3).u32v(5).u32v(300);                           // one reward
    w.u32v(75).u32v(0).u32v(4);                                    // money, spell, emote count
    for (u32 i = 0; i < 4; ++i) {
        w.u32v(i + 1).u32v(i * 100); // emote, delay
    }
    QuestDetails qd;
    REQUIRE(parseQuestDetails(details, qd));
    CHECK(qd.questId == 783);
    CHECK(qd.details == "Details $N");
    CHECK(qd.objectives == "Speak with Marshal");
    CHECK(qd.choiceCount == 2);
    CHECK(qd.choices[1].displayId == 200);
    CHECK(qd.rewardCount == 1);
    CHECK(qd.rewards[0].count == 5);
    CHECK(qd.money == 75);
    CHECK(qd.emotes[3] == 4);
    CHECK(qd.emoteDelays[3] == 300);
    checkTruncations(details, [](auto p) {
        QuestDetails m;
        return parseQuestDetails(p, m);
    });
    // Hidden rewards: three bare zeros instead of the lists and the money.
    Bytes hidden;
    ByteWriter(hidden)
        .u64v(kCreature)
        .u32v(1)
        .cstring("T")
        .cstring("D")
        .cstring("O")
        .u32v(1)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(4)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0);
    REQUIRE(parseQuestDetails(hidden, qd));
    CHECK(qd.choiceCount == 0);
    CHECK(qd.rewardCount == 0);
    Bytes sevenChoices;
    ByteWriter(sevenChoices).u64v(1).u32v(1).cstring("").cstring("").cstring("").u32v(1).u32v(7);
    CHECK_FALSE(parseQuestDetails(sevenChoices, qd));

    // Quest.cpp QuestGiverRequestItems.
    Bytes request;
    ByteWriter(request)
        .u64v(kCreature)
        .u32v(33)
        .cstring("Wolves")
        .cstring("Have the meat?")
        .u32v(0)
        .u32v(6)
        .u32v(0)
        .u32v(0)
        .u32v(1)
        .u32v(750)
        .u32v(8)
        .u32v(7300)
        .u32v(2)
        .u32v(3)
        .u32v(4)
        .u32v(8);
    QuestRequestItems ri;
    REQUIRE(parseQuestRequestItems(request, ri));
    CHECK(ri.questId == 33);
    CHECK(ri.text == "Have the meat?");
    REQUIRE(ri.itemCount == 1);
    CHECK(ri.items[0].itemId == 750);
    CHECK(ri.items[0].count == 8);
    CHECK(ri.completable);
    checkTruncations(request, [](auto p) {
        QuestRequestItems m;
        return parseQuestRequestItems(p, m);
    });
    Bytes incomplete(request);
    std::memset(incomplete.data() + incomplete.size() - 12, 0, 4); // second completion flag: 0
    REQUIRE(parseQuestRequestItems(incomplete, ri));
    CHECK_FALSE(ri.completable);

    // Quest.cpp QuestGiverOfferReward: emotes are (delay, emote) and counted.
    Bytes offer;
    ByteWriter(offer)
        .u64v(kCreature)
        .u32v(33)
        .cstring("Wolves")
        .cstring("Well done.")
        .u32v(1)
        .u32v(1)
        .u32v(0)
        .u32v(2)
        .u32v(1)
        .u32v(2080)
        .u32v(1)
        .u32v(9000)
        .u32v(0)
        .u32v(40)
        .u32v(0)
        .u32v(0);
    QuestOfferReward orw;
    REQUIRE(parseQuestOfferReward(offer, orw));
    CHECK(orw.emoteCount == 1);
    CHECK(orw.emotes[0] == 2);
    CHECK(orw.choiceCount == 1);
    CHECK(orw.choices[0].itemId == 2080);
    CHECK(orw.rewardCount == 0);
    CHECK(orw.money == 40);
    checkTruncations(offer, [](auto p) {
        QuestOfferReward m;
        return parseQuestOfferReward(p, m);
    });

    // Player.cpp SendQuestReward: quest, 3, xp, money, count, (item, count)...
    Bytes complete;
    ByteWriter(complete).u32v(33).u32v(3).u32v(250).u32v(40).u32v(1).u32v(2080).u32v(1);
    QuestComplete qc;
    REQUIRE(parseQuestComplete(complete, qc));
    CHECK(qc.xp == 250);
    CHECK(qc.money == 40);
    CHECK(qc.itemCount == 1);
    checkTruncations(complete, [](auto p) {
        QuestComplete m;
        return parseQuestComplete(p, m);
    });

    u32 questId = 0, reason = 0;
    REQUIRE(parseQuestGiverQuestFailed(bytes({33, 0, 0, 0, 4, 0, 0, 0}), questId, reason));
    CHECK(reason == 4);
    REQUIRE(parseQuestGiverQuestInvalid(bytes({13, 0, 0, 0}), reason));
    CHECK(reason == 13);
    CHECK_FALSE(parseQuestGiverQuestInvalid(bytes({13, 0, 0}), reason));

    // Quest.cpp QuestQueryResponse: title, objectives, details, end text; game objects as entry | 0x80000000.
    Bytes query;
    ByteWriter q(query);
    q.u32v(7)
        .u32v(2)
        .u32v(2)
        .u32v(9)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(0)
        .u32v(15)
        .u32v(35)
        .u32v(60)
        .u32v(0)
        .u32v(0)
        .u32v(0);
    for (u32 i = 0; i < 4; ++i) {
        q.u32v(i == 0 ? 2080 : 0).u32v(i == 0 ? 1 : 0);
    }
    for (u32 i = 0; i < 6; ++i) {
        q.u32v(0).u32v(0);
    }
    q.u32v(0).f32v(0.0f).f32v(0.0f).u32v(0);
    q.cstring("Kobold Camp Cleanup").cstring("Kill 10 Kobold Vermin").cstring("Details").cstring("");
    q.u32v(6).u32v(10).u32v(0).u32v(0);
    q.u32v(0x80000000u | 1561).u32v(1).u32v(0).u32v(0);
    q.u32v(0).u32v(0).u32v(750).u32v(8);
    q.u32v(0).u32v(0).u32v(0).u32v(0);
    q.cstring("").cstring("Pile inspected").cstring("").cstring("");
    QuestQueryResponse qq;
    REQUIRE(parseQuestQueryResponse(query, qq));
    CHECK(qq.questId == 7);
    CHECK(qq.nextQuestInChain == 15);
    CHECK(qq.money == 35);
    CHECK(qq.rewards[0].itemId == 2080);
    CHECK(qq.title == "Kobold Camp Cleanup");
    CHECK(qq.objectives == "Kill 10 Kobold Vermin");
    CHECK(qq.details == "Details");
    CHECK(qq.objectiveList[0].creatureOrGo == 6);
    CHECK(qq.objectiveList[1].creatureOrGo == (0x80000000u | 1561));
    CHECK(qq.objectiveList[1].text == "Pile inspected");
    CHECK(qq.objectiveList[2].itemId == 750);
    checkTruncations(query, [](auto p) {
        QuestQueryResponse m;
        return parseQuestQueryResponse(p, m);
    });

    QuestUpdateAddKill ak;
    Bytes addKill;
    ByteWriter(addKill).u32v(7).u32v(6).u32v(3).u32v(10).u64v(kCreature);
    REQUIRE(parseQuestUpdateAddKill(addKill, ak));
    CHECK(ak.count == 3);
    CHECK(ak.required == 10);
    CHECK(ak.source == kCreature);
    checkTruncations(addKill, [](auto p) {
        QuestUpdateAddKill m;
        return parseQuestUpdateAddKill(p, m);
    });
    u32 item = 0, added = 0;
    REQUIRE(parseQuestUpdateAddItem(bytes({0xEE, 2, 0, 0, 1, 0, 0, 0}), item, added));
    CHECK(item == 750);
    CHECK(added == 1);
    REQUIRE(parseQuestId(bytes({7, 0, 0, 0}), questId));
    CHECK(questId == 7);

    QuestConfirmAccept ca;
    Bytes confirm;
    ByteWriter(confirm).u32v(7).cstring("Kobold Camp Cleanup").u64v(kPlayer);
    REQUIRE(parseQuestConfirmAccept(confirm, ca));
    CHECK(ca.title == "Kobold Camp Cleanup");
    CHECK(ca.sharer == kPlayer);
    checkTruncations(confirm, [](auto p) {
        QuestConfirmAccept m;
        return parseQuestConfirmAccept(p, m);
    });
}

TEST_CASE("Loot and items: LOOT_RESPONSE (both forms), removed, money, push result, inventory failure") {
    // Player::SendLoot + LootView: guid, type, gold, count, (slot, item, count, display, 0, random prop, slot
    // type).
    Bytes loot;
    ByteWriter(loot)
        .u64v(kCreature)
        .u8v(kLootTypeCorpse)
        .u32v(23)
        .u8v(2)
        .u8v(0)
        .u32v(750)
        .u32v(1)
        .u32v(7300)
        .u32v(0)
        .u32v(0)
        .u8v(0)
        .u8v(3)
        .u32v(2589)
        .u32v(2)
        .u32v(1234)
        .u32v(0)
        .u32v(0)
        .u8v(0);
    LootResponse lr;
    REQUIRE(parseLootResponse(loot, lr));
    CHECK(lr.guid == kCreature);
    CHECK(lr.lootType == kLootTypeCorpse);
    CHECK(lr.gold == 23);
    REQUIRE(lr.items.size() == 2);
    CHECK(lr.items[1].slot == 3); // not contiguous: echo it as is
    CHECK(lr.items[1].count == 2);
    checkTruncations(loot, [](auto p) {
        LootResponse m;
        return parseLootResponse(p, m);
    });
    // Player::SendLootError: guid, type 0, error.
    Bytes error;
    ByteWriter(error).u64v(kCreature).u8v(0).u8v(4);
    REQUIRE(parseLootResponse(error, lr));
    CHECK(lr.lootType == 0);
    CHECK(lr.error == 4);
    CHECK(std::string(lootErrorName(lr.error)) == "TOO_FAR");
    checkTruncations(error, [](auto p) {
        LootResponse m;
        return parseLootResponse(p, m);
    });

    u64 guid = 0;
    Bytes release;
    ByteWriter(release).u64v(kCreature).u8v(1);
    REQUIRE(parseLootReleaseResponse(release, guid));
    CHECK(guid == kCreature);
    checkTruncations(release, [](auto p) {
        u64 g = 0;
        return parseLootReleaseResponse(p, g);
    });
    u8 slot = 0;
    REQUIRE(parseLootRemoved(bytes({3}), slot));
    CHECK(slot == 3);
    CHECK_FALSE(parseLootRemoved({}, slot));

    // Item.cpp ItemPushResult (build > 1.10.2: item slot and count).
    Bytes push;
    ByteWriter(push).u64v(kPlayer).u32v(0).u32v(0).u32v(1).u8v(255).u32v(24).u32v(750).u32v(0).u32v(0).u32v(
        1);
    ItemPushResult ip;
    REQUIRE(parseItemPushResult(push, ip));
    CHECK(ip.player == kPlayer);
    CHECK_FALSE(ip.fromNpc);
    CHECK(ip.bag == 255);
    CHECK(ip.bagSlot == 24);
    CHECK(ip.itemId == 750);
    CHECK(ip.count == 1);
    checkTruncations(push, [](auto p) {
        ItemPushResult m;
        return parseItemPushResult(p, m);
    });

    // Item.cpp InventoryChangeFailure: 1 byte for OK; the level only for CANT_EQUIP_LEVEL_I.
    InventoryChangeFailure f;
    REQUIRE(parseInventoryChangeFailure(bytes({0}), f));
    CHECK(f.result == 0);
    Bytes full;
    ByteWriter(full).u8v(50).u64v(0x77).u64v(0).u8v(0);
    REQUIRE(parseInventoryChangeFailure(full, f));
    CHECK(f.result == 50);
    CHECK(f.item1 == 0x77);
    CHECK(std::string(inventoryResultName(f.result)) == "INVENTORY_FULL");
    checkTruncations(full, [](auto p) {
        InventoryChangeFailure m;
        return parseInventoryChangeFailure(p, m);
    });
    Bytes level;
    ByteWriter(level).u8v(1).u32v(10).u64v(0x77).u64v(0).u8v(0);
    REQUIRE(parseInventoryChangeFailure(level, f));
    CHECK(f.requiredLevel == 10);
    checkTruncations(level, [](auto p) {
        InventoryChangeFailure m;
        return parseInventoryChangeFailure(p, m);
    });

    // Query.cpp GameObjectQueryResponse (1.12.1: icon string and 24 data words, no size).
    Bytes go;
    ByteWriter g(go);
    g.u32v(1561)
        .u32v(3)
        .u32v(259)
        .cstring("Pile of Kobold Candles")
        .cstring("")
        .cstring("")
        .cstring("")
        .cstring("");
    for (u32 i = 0; i < 24; ++i) {
        g.u32v(i == 1 ? 1561u : i == 8 ? 7u : 0u);
    }
    GameObjectQueryInfo gi;
    REQUIRE(parseGameObjectQueryResponse(go, gi));
    CHECK(gi.found);
    CHECK(gi.type == 3);
    CHECK(gi.name == "Pile of Kobold Candles");
    CHECK(gi.data[8] == 7);
    checkTruncations(go, [](auto p) {
        GameObjectQueryInfo m;
        return parseGameObjectQueryResponse(p, m);
    });
    REQUIRE(parseGameObjectQueryResponse(bytes({0x19, 0x06, 0, 0x80}), gi));
    CHECK_FALSE(gi.found);
    CHECK(gi.entry == 1561);

    // The item query response now also gives the on-use spells and the quest an item starts.
    Bytes itemQuery;
    ByteWriter iq(itemQuery);
    iq.u32v(1307).u32v(12).u32v(0).cstring("Gold Pickup Schedule").cstring("").cstring("").cstring("");
    iq.u32v(3029).u32v(1).u32v(0).u32v(0).u32v(0).u32v(0);
    iq.u32v(0).u32v(0).u32v(5).u32v(4); // allowable class / race, item level, required level
    for (u32 i = 0; i < 8; ++i) {
        iq.u32v(0);
    }
    iq.u32v(1).u32v(0); // stackable, container slots
    for (u32 i = 0; i < 10; ++i) {
        iq.u32v(0).u32v(0);
    }
    for (u32 i = 0; i < 5; ++i) {
        iq.f32v(0.0f).f32v(0.0f).u32v(0);
    }
    for (u32 i = 0; i < 7; ++i) {
        iq.u32v(0);
    }
    iq.u32v(0).u32v(0).f32v(0.0f);
    for (u32 i = 0; i < 5; ++i) {
        iq.u32v(i == 1 ? 8593u : 0u).u32v(0).u32v(0).u32v(0xFFFFFFFFu).u32v(0).u32v(0xFFFFFFFFu);
    }
    iq.u32v(4).cstring("").u32v(0).u32v(0).u32v(0).u32v(123).u32v(0).u32v(0).u32v(0);
    for (u32 i = 0; i < 7; ++i) {
        iq.u32v(0); // random property, block, set, durability, area, map, bag family
    }
    const std::optional<ItemQueryInfo> itemInfo = parseItemQueryResponse(itemQuery);
    REQUIRE(itemInfo.has_value());
    CHECK(itemInfo->requiredLevel == 4);
    CHECK(itemInfo->stackable == 1);
    CHECK(itemInfo->spells[1].id == 8593);
    CHECK(itemInfo->spells[1].trigger == kItemSpellTriggerOnUse);
    CHECK(itemInfo->bonding == 4);
    CHECK(itemInfo->startQuest == 123);
}

TEST_CASE("Experience and death packets") {
    // Misc.cpp LogXpGain: kill (base + group bonus) and non-kill forms.
    Bytes kill;
    ByteWriter(kill).u64v(kCreature).u32v(90).u8v(0).u32v(45).f32v(1.0f);
    XpGain xp;
    REQUIRE(parseXpGain(kill, xp));
    CHECK(xp.kill);
    CHECK(xp.totalXp == 90);
    CHECK(xp.baseXp == 45);
    checkTruncations(kill, [](auto p) {
        XpGain m;
        return parseXpGain(p, m);
    });
    Bytes quest;
    ByteWriter(quest).u64v(0).u32v(250).u8v(1);
    REQUIRE(parseXpGain(quest, xp));
    CHECK_FALSE(xp.kill);
    CHECK(xp.baseXp == 250);

    Bytes levelUp;
    ByteWriter l(levelUp);
    l.u32v(5).u32v(17);
    for (u32 i = 0; i < 10; ++i) {
        l.u32v(i == 0 ? 22 : i == 5 ? 1 : 0);
    }
    LevelUpInfo lu;
    REQUIRE(parseLevelUpInfo(levelUp, lu));
    CHECK(lu.level == 5);
    CHECK(lu.powerGains[0] == 22);
    CHECK(lu.statGains[0] == 1);
    checkTruncations(levelUp, [](auto p) {
        LevelUpInfo m;
        return parseLevelUpInfo(p, m);
    });

    // QueryHandler.cpp HandleCorpseQueryOpcode.
    Bytes corpse;
    ByteWriter(corpse).u8v(1).u32v(0).f32v(-9000.0f).f32v(-100.0f).f32v(80.0f).u32v(0);
    CorpseQuery cq;
    REQUIRE(parseCorpseQuery(corpse, cq));
    CHECK(cq.found);
    CHECK(cq.x == -9000.0f);
    checkTruncations(corpse, [](auto p) {
        CorpseQuery m;
        return parseCorpseQuery(p, m);
    });
    REQUIRE(parseCorpseQuery(bytes({0}), cq));
    CHECK_FALSE(cq.found);

    // Spell.cpp ResurrectRequest: guid, u32 length with the NUL, the name, sickness, delayed.
    Bytes rez;
    ByteWriter(rez).u64v(kPlayer).u32v(1).u8v(0).u8v(1).u8v(0);
    ResurrectRequest rr;
    REQUIRE(parseResurrectRequest(rez, rr));
    CHECK(rr.name.empty());
    CHECK(rr.sickness);
    checkTruncations(rez, [](auto p) {
        ResurrectRequest m;
        return parseResurrectRequest(p, m);
    });
    Bytes named;
    ByteWriter(named).u64v(kCreature).u32v(15).cstring("Spirit Healer!").u8v(0).u8v(1);
    REQUIRE(parseResurrectRequest(named, rr));
    CHECK(rr.name == "Spirit Healer!");
    Bytes badLength;
    ByteWriter(badLength).u64v(kCreature).u32v(3).bytes(bytes({'a', 'b', 'c'})).u8v(0).u8v(1); // no NUL
                                                                                               // inside
    CHECK_FALSE(parseResurrectRequest(badLength, rr));

    u32 delay = 0;
    REQUIRE(parseU32(bytes({0x30, 0x75, 0, 0}), delay));
    CHECK(delay == 30000);
    u64 guid = 0;
    Bytes confirm;
    ByteWriter(confirm).u64v(kCreature);
    REQUIRE(parseGuid(confirm, guid));
    CHECK(guid == kCreature);
    u8 stand = 0;
    REQUIRE(parseStandStateUpdate(bytes({1}), stand));
    CHECK(stand == 1);
}

TEST_CASE("Chat: every SMSG_MESSAGECHAT layout, notifications, text emotes") {
    ChatMessage m;
    // SAY / PARTY / YELL: the sender twice.
    Bytes say;
    ByteWriter(say).u8v(0x00).u32v(7).u64v(kPlayer).u64v(kPlayer).u32v(6).cstring("hello").u8v(0);
    REQUIRE(parseChatMessage(say, m));
    CHECK(m.type == ChatType::Say);
    CHECK(m.language == 7);
    CHECK(m.sender == kPlayer);
    CHECK(m.text == "hello");
    checkTruncations(say, [](auto p) {
        ChatMessage c;
        return parseChatMessage(p, c);
    });
    // MONSTER_SAY: sender, name with length, target.
    Bytes monster;
    ByteWriter(monster)
        .u8v(0x0B)
        .u32v(0)
        .u64v(kCreature)
        .u32v(7)
        .cstring("Hogger")
        .u64v(0)
        .u32v(10)
        .cstring("Grrrr...!")
        .u8v(0);
    REQUIRE(parseChatMessage(monster, m));
    CHECK(m.sender == kCreature);
    CHECK(m.senderName == "Hogger");
    CHECK(m.text == "Grrrr...!");
    checkTruncations(monster, [](auto p) {
        ChatMessage c;
        return parseChatMessage(p, c);
    });
    // MONSTER_EMOTE: name and target, no sender GUID.
    Bytes emote;
    ByteWriter(emote)
        .u8v(0x0D)
        .u32v(0)
        .u32v(7)
        .cstring("Hogger")
        .u64v(kPlayer)
        .u32v(19)
        .cstring("%s becomes enraged")
        .u8v(0);
    REQUIRE(parseChatMessage(emote, m));
    CHECK(m.sender == 0);
    CHECK(m.target == kPlayer);
    CHECK(m.senderName == "Hogger");
    checkTruncations(emote, [](auto p) {
        ChatMessage c;
        return parseChatMessage(p, c);
    });
    // CHANNEL: name, rank, sender.
    Bytes channel;
    ByteWriter(channel)
        .u8v(0x0E)
        .u32v(7)
        .cstring("General - Elwynn Forest")
        .u32v(0)
        .u64v(kPlayer)
        .u32v(3)
        .cstring("hi")
        .u8v(0);
    REQUIRE(parseChatMessage(channel, m));
    CHECK(m.channel == "General - Elwynn Forest");
    CHECK(m.sender == kPlayer);
    checkTruncations(channel, [](auto p) {
        ChatMessage c;
        return parseChatMessage(p, c);
    });
    // SYSTEM / WHISPER (default layout): one sender GUID; a GM tag.
    Bytes system;
    ByteWriter(system).u8v(0x0A).u32v(0).u64v(0).u32v(11).cstring("Hello, GM!").u8v(3);
    REQUIRE(parseChatMessage(system, m));
    CHECK(m.type == ChatType::System);
    CHECK(m.text == "Hello, GM!");
    CHECK(m.tag == 3);
    checkTruncations(system, [](auto p) {
        ChatMessage c;
        return parseChatMessage(p, c);
    });
    Bytes badLength;
    ByteWriter(badLength).u8v(0x0A).u32v(0).u64v(0).u32v(0).u8v(0); // a zero length has no room for the NUL
    CHECK_FALSE(parseChatMessage(badLength, m));

    std::string_view text;
    REQUIRE(parseCString(bytes({'N', 'o', 0}), text));
    CHECK(text == "No");
    CHECK_FALSE(parseCString(bytes({'N', 'o'}), text));
    Bytes area;
    ByteWriter(area).u32v(9).cstring("Too high");
    REQUIRE(parseAreaTriggerMessage(area, text));
    CHECK(text == "Too high");
    checkTruncations(area, [](auto p) {
        std::string_view t;
        return parseAreaTriggerMessage(p, t);
    });

    Bytes textEmote;
    ByteWriter(textEmote).u64v(kPlayer).u32v(101).u32v(0).u32v(1).u8v(0);
    TextEmote te;
    REQUIRE(parseTextEmote(textEmote, te));
    CHECK(te.textEmote == 101);
    CHECK(te.targetName.empty());
    checkTruncations(textEmote, [](auto p) {
        TextEmote t;
        return parseTextEmote(p, t);
    });
}

TEST_CASE("Client packets: byte-exact bodies") {
    Bytes out;
    // CMSG_CAST_SPELL: spell, then SpellCastTargets as the server reads them.
    SpellCastTargets self;
    writeCastSpell(out, 133, self);
    CHECK(out == bytes({0x85, 0, 0, 0, 0, 0})); // self: mask 0 and nothing else
    out.clear();
    SpellCastTargets unit;
    unit.mask = kTargetUnit;
    unit.unit = kCreature;
    writeCastSpell(out, 133, unit);
    CHECK(out == bytes({0x85, 0, 0, 0, 0x02, 0x00, 0xDB, 0x56, 0x04, 0x23, 0x01, 0x30, 0xF1}));
    out.clear();
    SpellCastTargets ground;
    ground.mask = kTargetDestLocation;
    ground.destination = {1.0f, 2.0f, 3.0f};
    writeCastSpell(out, 10, ground);
    CHECK(out == bytes({10, 0, 0, 0, 0x40, 0, 0, 0, 0x80, 0x3F, 0, 0, 0, 0x40, 0, 0, 0x40, 0x40}));
    out.clear();
    SpellCastTargets object;
    object.mask = kTargetGameObject;
    object.gameObject = 0x0102;
    writeCastSpell(out, 1, object);
    CHECK(out == bytes({1, 0, 0, 0, 0x00, 0x08, 0x03, 0x02, 0x01}));

    // CMSG_USE_ITEM: bag, slot, spell index, targets.
    out.clear();
    writeUseItem(out, 255, 23, 1, self);
    CHECK(out == bytes({255, 23, 1, 0, 0}));

    // CMSG_SET_ACTION_BUTTON: u8 button + u32 packed (action | type << 24).
    out.clear();
    writeSetActionButton(out, 11, (u32{kActionItem} << 24) | 159u);
    CHECK(out == bytes({11, 159, 0, 0, 0x80}));

    // Full GUID bodies (CMSG_ATTACKSWING, CMSG_GOSSIP_HELLO, CMSG_LOOT...).
    out.clear();
    writeGuid(out, kCreature);
    CHECK(out == bytes({0x56, 0x04, 0x00, 0x23, 0x01, 0x00, 0x30, 0xF1}));

    // CMSG_GOSSIP_SELECT_OPTION: guid, u32 option, the code only for coded options.
    out.clear();
    writeGossipSelectOption(out, 0x11, 2);
    CHECK(out == bytes({0x11, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0}));
    out.clear();
    writeGossipSelectOption(out, 0x11, 2, "ab");
    CHECK(out == bytes({0x11, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 'a', 'b', 0}));

    // CMSG_NPC_TEXT_QUERY: u32 text id then the GUID.
    out.clear();
    writeNpcTextQuery(out, 2, 0x11);
    CHECK(out == bytes({2, 0, 0, 0, 0x11, 0, 0, 0, 0, 0, 0, 0}));

    // CMSG_QUESTGIVER_*_QUEST: guid, quest; CHOOSE_REWARD adds the index.
    out.clear();
    writeQuestGiverQuest(out, 0x11, 783);
    CHECK(out == bytes({0x11, 0, 0, 0, 0, 0, 0, 0, 0x0F, 0x03, 0, 0}));
    out.clear();
    writeQuestGiverChooseReward(out, 0x11, 783, 1);
    CHECK(out == bytes({0x11, 0, 0, 0, 0, 0, 0, 0, 0x0F, 0x03, 0, 0, 1, 0, 0, 0}));

    // CMSG_QUESTLOG_REMOVE_QUEST / CMSG_AUTOSTORE_LOOT_ITEM: one byte. CMSG_QUEST_QUERY / CANCEL_CAST: one
    // u32.
    out.clear();
    writeU8(out, 19);
    CHECK(out == bytes({19}));
    out.clear();
    writeU32(out, 783);
    CHECK(out == bytes({0x0F, 0x03, 0, 0}));

    // CMSG_RESURRECT_RESPONSE: guid + u8.
    out.clear();
    writeResurrectResponse(out, 0x11, true);
    CHECK(out == bytes({0x11, 0, 0, 0, 0, 0, 0, 0, 1}));

    // CMSG_MESSAGECHAT: type, language, [target for whisper / channel], text.
    out.clear();
    writeMessageChat(out, ChatType::Say, Language::Common, "ignored", "hi");
    CHECK(out == bytes({0, 0, 0, 0, 7, 0, 0, 0, 'h', 'i', 0}));
    out.clear();
    writeMessageChat(out, ChatType::Whisper, Language::Orcish, "Bob", "yo");
    CHECK(out == bytes({6, 0, 0, 0, 1, 0, 0, 0, 'B', 'o', 'b', 0, 'y', 'o', 0}));
    out.clear();
    writeMessageChat(out, ChatType::Channel, Language::Common, "General", "x");
    CHECK(out == bytes({14, 0, 0, 0, 7, 0, 0, 0, 'G', 'e', 'n', 'e', 'r', 'a', 'l', 0, 'x', 0}));

    // CMSG_GAMEOBJECT_QUERY / ITEM_QUERY_SINGLE / CREATURE_QUERY: entry + GUID.
    out.clear();
    writeEntryQuery(out, 1561, 0x22);
    CHECK(out == bytes({0x19, 0x06, 0, 0, 0x22, 0, 0, 0, 0, 0, 0, 0}));
}
