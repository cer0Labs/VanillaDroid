// Phase 16 (pullouts): the player's party inside a raid ("party1".."party4": the others of its subgroup, as
// WoW.exe's SMSG_GROUP_LIST handler fills them), UnitInParty, the whole group for the world map; the
// pullouts' aura filters (UnitBuff / UnitDebuff with SHOW_CASTABLE_BUFFS / SHOW_DISPELLABLE_DEBUFFS) on
// synthetic spells and, with VD_PACKS_DIR, on the client's own.
#include "game/AbilityData.h"
#include "game/Entities.h"
#include "game/GameData.h"
#include "game/Queries.h"
#include "game/RaidAuras.h"
#include "game/Social.h"
#include "game/SpellText.h"
#include "net/ByteBuffer.h"
#include "net/Messages.h"
#include "net/SocialMessages.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <ostream>
#include <tuple>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;

using Bytes = std::vector<u8>;

constexpr u64 kMe = 0x11;

struct Harness {
    game::EntityStore store;
    game::QueryCache queries;
    game::Social social{store, queries};
    u32 now = 1000;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void*, Opcode, std::span<const u8>) { return true; };
        social.setSink(sink);
        queries.setSink(sink);
        queries.setOnline(true);
        social.enterWorld(kMe, true, now);
    }
    void list(u8 groupType, u64 leader, u8 ownFlags, std::vector<std::tuple<const char*, u64, u8>> members) {
        Bytes out;
        net::ByteWriter w(out);
        w.u8v(groupType).u8v(ownFlags).u32v(static_cast<u32>(members.size()));
        for (const auto& [name, guid, flags] : members) {
            w.cstring(name).u64v(guid).u8v(net::kMemberOnline).u8v(flags);
        }
        w.u64v(leader).u8v(3).u64v(0).u8v(2).u8v(0);
        REQUIRE(social.handle(Opcode::SMSG_GROUP_LIST, out, now) == game::Social::Handled::Yes);
    }
    std::vector<std::string> party() const {
        std::array<game::PartyMemberView, 40> out;
        const u32 n = social.partyMembers(out);
        std::vector<std::string> names;
        for (u32 i = 0; i < n; ++i) {
            names.emplace_back(out[i].name);
        }
        return names;
    }
};

using Names = std::vector<std::string>;

} // namespace

TEST_CASE("Party in a raid: the others of the player's subgroup in the list's order, four at most") {
    Harness h;
    // The player in subgroup 1 (Group 2); the list's order is the server's (join order), the assistant bit
    // apart.
    h.list(net::kGroupTypeRaid, 0x21, 1,
           {{"Aeryn", 0x21, 0},
            {"Brakk", 0x22, 1},
            {"Cyra", 0x23, 1 | 0x80},
            {"Dorn", 0x24, 2},
            {"Elsa", 0x25, 1},
            {"Fenn", 0x26, 1},
            {"Gil", 0x27, 1}});
    CHECK(h.party() == Names{"Brakk", "Cyra", "Elsa", "Fenn"}); // the fifth of the subgroup gets no slot
    CHECK(h.social.partyMemberCount() == 4);
    CHECK(h.social.inPartyWith(0x22));
    CHECK(h.social.inPartyWith(0x23));
    CHECK_FALSE(h.social.inPartyWith(0x21)); // the leader, in Group 1
    CHECK_FALSE(h.social.inPartyWith(0x27)); // no fifth slot
    CHECK_FALSE(h.social.inPartyWith(0));
    CHECK(h.social.inGroupWith(0x21)); // still in the raid
    std::array<game::PartyMemberView, 40> all;
    CHECK(h.social.groupMembers(all) == 7);
    CHECK(all[3].name == "Dorn");
    // The raid's leader is not in the player's party: no crown on its party frames (GetPartyLeaderIndex).
    std::array<game::PartyMemberView, 4> party;
    REQUIRE(h.social.partyMembers(party) == 4);
    for (const game::PartyMemberView& m : party) {
        CHECK_FALSE(m.leader);
    }

    // Moved to Group 3: Dorn is its party now.
    h.list(net::kGroupTypeRaid, 0x24, 2,
           {{"Aeryn", 0x21, 0}, {"Brakk", 0x22, 1}, {"Cyra", 0x23, 1}, {"Dorn", 0x24, 2}, {"Elsa", 0x25, 1}});
    CHECK(h.party() == Names{"Dorn"});
    REQUIRE(h.social.partyMembers(party) == 1);
    CHECK(party[0].leader); // the leader in the player's own subgroup has its crown
    CHECK(h.social.inPartyWith(0x24));
    CHECK_FALSE(h.social.inPartyWith(0x22));

    // Alone in Group 8: no party frames, nobody to share a quest with (GetNumPartyMembers() == 0).
    h.list(net::kGroupTypeRaid, 0x21, 7, {{"Aeryn", 0x21, 0}, {"Brakk", 0x22, 1}});
    CHECK(h.party().empty());
    CHECK(h.social.partyMemberCount() == 0);
    CHECK(h.social.groupMembers(all) == 2);
}

TEST_CASE("Party in a party: everybody, in the list's order") {
    Harness h;
    h.list(0, kMe, 0, {{"Aeryn", 0x21, 0}, {"Brakk", 0x22, 0}, {"Cyra", 0x23, 0}});
    CHECK(h.party() == Names{"Aeryn", "Brakk", "Cyra"});
    CHECK(h.social.partyMemberCount() == 3);
    CHECK(h.social.inPartyWith(0x23));
    // Out of the group: nobody.
    h.list(0, 0, 0, {});
    CHECK(h.party().empty());
    CHECK_FALSE(h.social.inPartyWith(0x23));
}

TEST_CASE("Raid aura filters: what UnitBuff and UnitDebuff list and let through") {
    game::RaidAuraSpell shown;
    shown.effects = {6, 0, 0};      // SPELL_EFFECT_APPLY_AURA
    shown.effectAuras = {29, 0, 0}; // SPELL_AURA_MOD_STAT
    shown.implicitTargetA = {21, 0, 0};
    CHECK(game::raidAuraListed(shown));
    game::RaidAuraSpell hidden = shown;
    hidden.attributes = game::kSpellAttrDoNotDisplay;
    CHECK_FALSE(game::raidAuraListed(hidden));
    hidden = shown;
    hidden.attributesEx = game::kSpellAttrExNoAuraIcon;
    CHECK_FALSE(game::raidAuraListed(hidden));
    hidden = shown;
    hidden.effectAuras[1] = 44; // tracking
    CHECK_FALSE(game::raidAuraListed(hidden));

    // Castable: known, active, cast on others, no area aura.
    CHECK(game::raidCastableBuff(shown, true));
    CHECK_FALSE(game::raidCastableBuff(shown, false));
    game::RaidAuraSpell other = shown;
    other.attributes = game::kSpellAttrPassive;
    CHECK_FALSE(game::raidCastableBuff(other, true));
    other = shown;
    other.attributesEx3 = game::kSpellAttrEx3HideInRaidFilter;
    CHECK_FALSE(game::raidCastableBuff(other, true));
    other = shown;
    other.implicitTargetA[0] = game::kTargetUnitCaster;
    CHECK_FALSE(game::raidCastableBuff(other, true));
    other = shown;
    other.effects[1] = 35; // SPELL_EFFECT_APPLY_AREA_AURA_PARTY
    CHECK_FALSE(game::raidCastableBuff(other, true));

    // Dispel types: SPELL_EFFECT_DISPEL's misc value unless it aims at an enemy.
    game::RaidAuraSpell cleanse;
    cleanse.effects = {game::kSpellEffectDispel, game::kSpellEffectDispel, game::kSpellEffectDispel};
    cleanse.implicitTargetA = {21, 21, game::kTargetUnitEnemy};
    cleanse.effectMisc = {1, 4, 3};
    const u32 types = game::raidDispelTypes(cleanse);
    CHECK(types == ((1u << 1) | (1u << 4)));
    CHECK(game::raidDispellable(types, 1));
    CHECK(game::raidDispellable(types, 4));
    CHECK_FALSE(game::raidDispellable(types, 3));
    CHECK_FALSE(game::raidDispellable(types, 0)); // a debuff without a type never passes
}

TEST_CASE("Raid aura filters on the client's spells") {
    static pack::PackReader db;
    static game::GameData data;
    static game::AbilityData abilities;
    static int state = 0;
    if (state == 0) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        state =
            dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok() && data.init(db) && abilities.init(db)
                ? 1
                : 2;
    }
    if (state != 1) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    const auto spell = [&](u32 id) {
        game::RaidAuraSpell s;
        const std::optional<game::SpellInfo> info = data.spell(id);
        REQUIRE(info);
        s.attributes = info->attributes;
        s.attributesEx3 = info->attributesEx3;
        for (u32 i = 0; i < 3; ++i) {
            s.effects[i] = info->effects[i];
            s.effectAuras[i] = info->effectAura[i];
            s.implicitTargetA[i] = info->implicitTargetA[i];
            s.effectMisc[i] = info->effectMisc[i];
        }
        game::SpellDetails details;
        REQUIRE(abilities.spellText().details(id, details));
        s.attributesEx = details.attributesEx;
        return s;
    };
    // What each class's dispels teach (SpellDispelType: 1 magic, 2 curse, 3 disease, 4 poison).
    CHECK(game::raidDispelTypes(spell(475)) == 1u << 2);  // Remove Lesser Curse (mage)
    CHECK(game::raidDispelTypes(spell(2782)) == 1u << 2); // Remove Curse (druid)
    CHECK(game::raidDispelTypes(spell(528)) == 1u << 3);  // Cure Disease (priest)
    CHECK(game::raidDispelTypes(spell(4987)) == ((1u << 1) | (1u << 3) | (1u << 4))); // Cleanse (paladin)
    CHECK(game::raidDispelTypes(spell(370)) == 0);               // Purge: the enemy's buffs
    CHECK((game::raidDispelTypes(spell(527)) & (1u << 1)) != 0); // Dispel Magic (priest): friends' magic too
    // Buffs a caster can put on others, and the ones the filter leaves out.
    CHECK(game::raidCastableBuff(spell(1243), true));       // Power Word: Fortitude
    CHECK(game::raidCastableBuff(spell(1459), true));       // Arcane Intellect
    CHECK(game::raidCastableBuff(spell(6673), true)); // Battle Shout: the party around, cast (no area aura)
    CHECK_FALSE(game::raidCastableBuff(spell(465), true));  // Devotion Aura
    CHECK_FALSE(game::raidCastableBuff(spell(588), true));  // Inner Fire: on the caster
    // Tracking is never listed.
    CHECK_FALSE(game::raidAuraListed(spell(5225))); // Track Humanoids
    CHECK(game::raidAuraListed(spell(1243)));
}

TEST_CASE("Raid roster per frame: 40 members cost microseconds") {
    // The pullouts rebuild their lists every frame from Social::raidMembers: a raid of 40 out of sight (stats
    // only).
    Harness h;
    std::vector<std::tuple<const char*, u64, u8>> members;
    static const char* kNames[] = {"Aa", "Bb", "Cc", "Dd", "Ee", "Ff", "Gg", "Hh", "Ii", "Jj",
                                   "Kk", "Ll", "Mm", "Nn", "Oo", "Pp", "Qq", "Rr", "Ss", "Tt",
                                   "Uu", "Vv", "Ww", "Xx", "Yy", "Zz", "Ab", "Ac", "Ad", "Ae",
                                   "Af", "Ag", "Ah", "Ai", "Aj", "Ak", "Al", "Am", "An"};
    for (u32 i = 0; i < 39; ++i) {
        members.emplace_back(kNames[i], 0x100 + i, static_cast<u8>((i + 1) / 5));
    }
    h.list(net::kGroupTypeRaid, 0x100, 0, members);
    std::array<game::PartyMemberView, 40> roster;
    const auto start = std::chrono::steady_clock::now();
    u32 total = 0;
    for (u32 i = 0; i < 1000; ++i) {
        total += h.social.raidMembers(roster);
        total += h.social.partyMembers(std::span(roster).first(4));
    }
    const f64 us =
        std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - start).count() / 1000.0;
    MESSAGE("raidMembers(40) + partyMembers: " << us << " us per frame");
    CHECK(total == 1000 * (40 + 4));
    CHECK(us < 500.0);
}
