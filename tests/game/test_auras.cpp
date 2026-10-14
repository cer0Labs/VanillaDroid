// Parity (auras): which buffs the player may cancel (CancelPlayerBuff, as
// WorldSession::HandleCancelAuraOpcode decides), the weapons' temporary enchantments and their timers
// (SMSG_ITEM_ENCHANT_TIME_UPDATE), a party member's auras from SMSG_PARTY_MEMBER_STATS(_FULL) while out of
// sight, driven through game::Gameplay; packets as vmangos-core writes them. Then the dispel types of
// Spell.dbc against the baked db.vdpk (VD_PACKS_DIR; skipped without it).
#include "game/AbilityData.h"
#include "game/Auras.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/SpellText.h"
#include "game/WeaponEnchants.h"
#include "net/ByteBuffer.h"
#include "net/SocialMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
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
constexpr u64 kAeryn = 0x21;
constexpr u64 kSword = 0x4000000000000101ull;
constexpr u64 kDagger = 0x4000000000000102ull;

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

// SMSG_UPDATE_OBJECT with one create block (UPDATEFLAG_HAS_POSITION for units; none for items).
Bytes created(u64 guid, game::ObjectType type, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    const bool unit = type == game::ObjectType::Player || type == game::ObjectType::Unit;
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(unit ? 0x40 : 0x00);
    if (unit) {
        w.f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f);
    }
    writeValues(w, fields);
    return out;
}

Bytes values(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

Fields playerFields(Fields extra = {}) {
    Fields f = {{uf::UNIT_FIELD_HEALTH, 100},
                {uf::UNIT_FIELD_MAXHEALTH, 100},
                {uf::UNIT_FIELD_BYTES_0, 1},
                {uf::UNIT_FIELD_FACTIONTEMPLATE, 1},
                {uf::UNIT_FIELD_LEVEL, 20}};
    f.insert(f.end(), extra.begin(), extra.end());
    return f;
}

// Gameplay over its own entity store; what it sends is captured.
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    std::vector<std::pair<Opcode, Bytes>> sent;
    std::vector<u64> createdGuids;
    u32 now = 1000;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kPlayer, 0, true, now);
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
};

// SMSG_ITEM_ENCHANT_TIME_UPDATE (WorldPackets::Item::ItemEnchantTimeUpdate, builds > 1.10.2): the item, the
// slot, the seconds left, the player.
Bytes enchantTime(u64 item, u32 slot, u32 seconds) {
    Bytes out;
    net::ByteWriter(out).u64v(item).u32v(slot).u32v(seconds).u64v(kPlayer);
    return out;
}

// SMSG_GROUP_LIST with the player leading `member`.
Bytes groupWith(u64 member) {
    Bytes out;
    net::ByteWriter w(out);
    w.u8v(0).u8v(0).u32v(1).cstring("Aeryn").u64v(member).u8v(net::kMemberOnline).u8v(0);
    w.u64v(kPlayer).u8v(3).u64v(0).u8v(2).u8v(0);
    return out;
}

// SMSG_PARTY_MEMBER_STATS(_FULL) carrying only auras (WorldSession::BuildPartyMemberStatsPacket): positive
// slots with a u32 mask, negative slots 32..47 with a u16 mask, a u16 spell id per set bit.
Bytes auraStats(u64 member, std::vector<std::pair<u32, u16>> slots) {
    u32 positive = 0;
    u16 negative = 0;
    for (const auto& [slot, spell] : slots) {
        if (slot < 32) {
            positive |= 1u << slot;
        } else {
            negative = static_cast<u16>(negative | (1u << (slot - 32)));
        }
    }
    std::sort(slots.begin(), slots.end());
    Bytes out;
    net::ByteWriter w(out);
    w.packedGuid(member).u32v(net::kGroupUpdateAuras | net::kGroupUpdateAurasNegative);
    w.u32v(positive);
    for (const auto& [slot, spell] : slots) {
        if (slot < 32) {
            w.u16v(spell);
        }
    }
    w.u16v(negative);
    for (const auto& [slot, spell] : slots) {
        if (slot >= 32) {
            w.u16v(spell);
        }
    }
    return out;
}

const game::AbilityData* abilityData() {
    static pack::PackReader db;
    static game::AbilityData data;
    static int state = 0; // 0 not tried, 1 ready, 2 missing
    if (state == 0) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        state = dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok() && data.init(db) ? 1 : 2;
    }
    return state == 1 ? &data : nullptr;
}

} // namespace

TEST_CASE("Auras: which buffs CancelPlayerBuff may take away") {
    game::AuraInfo buff;
    buff.spellId = 1243;
    buff.slot = 3;
    buff.flags = game::AuraInfo::kCancelable | 0x8;
    game::SpellDetails spell;
    spell.id = 1243;
    CHECK(game::canCancelAura(buff, spell, false));

    // The server's flag (AFLAG_CANCELABLE) and a positive slot are both needed.
    game::AuraInfo flagless = buff;
    flagless.flags = 0x8;
    CHECK_FALSE(game::canCancelAura(flagless, spell, true));
    game::AuraInfo debuff = buff;
    debuff.slot = 32;
    CHECK_FALSE(game::canCancelAura(debuff, spell, true));

    // Passive, SPELL_ATTR_NO_AURA_CANCEL, SPELL_ATTR_DO_NOT_DISPLAY.
    for (const u32 attribute : {0x40u, 0x80000000u, 0x80u}) {
        game::SpellDetails s = spell;
        s.attributes = attribute;
        CHECK_FALSE(game::canCancelAura(buff, s, true));
    }
    // SPELL_ATTR_EX_NO_AURA_ICON: only with an active icon.
    game::SpellDetails noIcon = spell;
    noIcon.attributesEx = 0x10000000;
    CHECK_FALSE(game::canCancelAura(buff, noIcon, true));
    noIcon.activeIcon = 92;
    CHECK(game::canCancelAura(buff, noIcon, true));

    // An area aura (SPELL_EFFECT_APPLY_AREA_AURA_PARTY: a paladin's aura): only the caster's own goes.
    game::SpellDetails area = spell;
    area.effects[0].effect = 35;
    CHECK_FALSE(game::canCancelAura(buff, area, false));
    CHECK(game::canCancelAura(buff, area, true));
}

TEST_CASE("Auras: weapon enchantment timers count down from SMSG_ITEM_ENCHANT_TIME_UPDATE") {
    game::EnchantTimers timers;
    CHECK_FALSE(timers.remainingMs(kSword, 1, 0).has_value());
    timers.onTimeUpdate(kSword, 1, 1800, 1000);
    REQUIRE(timers.remainingMs(kSword, 1, 1000).has_value());
    CHECK(*timers.remainingMs(kSword, 1, 1000) == 1800000);
    CHECK(*timers.remainingMs(kSword, 1, 61000) == 1740000);
    CHECK(*timers.remainingMs(kSword, 1, 2000000) ==
          0); // past its end: nothing left (until the field clears)
    CHECK_FALSE(timers.remainingMs(kSword, 0, 1000).has_value()); // another slot
    // A refresh replaces it; 0 seconds removes it.
    timers.onTimeUpdate(kSword, 1, 600, 5000);
    CHECK(*timers.remainingMs(kSword, 1, 5000) == 600000);
    timers.onTimeUpdate(kSword, 1, 0, 6000);
    CHECK_FALSE(timers.remainingMs(kSword, 1, 6000).has_value());
    // Full: the oldest goes.
    for (u32 i = 0; i < game::EnchantTimers::kCapacity + 1; ++i) {
        timers.onTimeUpdate(0x4000000000001000ull + i, 1, 100 + i, 7000);
    }
    CHECK_FALSE(timers.remainingMs(0x4000000000001000ull, 1, 7000).has_value());
    CHECK(*timers.remainingMs(0x4000000000001000ull + game::EnchantTimers::kCapacity, 1, 7000) ==
          (100 + game::EnchantTimers::kCapacity) * 1000);

    // The packet, byte for byte; truncated ones are refused.
    game::EnchantTimers parsed;
    const Bytes body = enchantTime(kDagger, 1, 25);
    REQUIRE(body.size() == 24);
    CHECK(parsed.handle(body, 100));
    CHECK(*parsed.remainingMs(kDagger, 1, 100) == 25000);
    CHECK_FALSE(parsed.handle(std::span<const u8>(body.data(), 15), 100));
    CHECK_FALSE(parsed.handle({}, 100));
}

TEST_CASE("Auras: the weapons' temporary enchantments, off hand first (TempEnchant1, 2)") {
    Harness h;
    // A sword in the main hand with Instant Poison (temporary slot: id, duration, charges), a dagger in the
    // off hand with a sharpening stone; a permanent enchantment alone shows nothing.
    h.apply(created(kSword, game::ObjectType::Item,
                    {{uf::OBJECT_FIELD_ENTRY, 2024},
                     {uf::ITEM_FIELD_ENCHANTMENT + 0, 803}, // permanent: Fiery Weapon
                     {uf::ITEM_FIELD_ENCHANTMENT + 3, 323}, // temporary: Instant Poison
                     {uf::ITEM_FIELD_ENCHANTMENT + 4, 1800000},
                     {uf::ITEM_FIELD_ENCHANTMENT + 5, 40}}));
    h.apply(created(kDagger, game::ObjectType::Item,
                    {{uf::OBJECT_FIELD_ENTRY, 2092}, {uf::ITEM_FIELD_ENCHANTMENT + 3, 40}}));
    h.apply(created(kPlayer, game::ObjectType::Player,
                    playerFields({{uf::PLAYER_FIELD_INV_SLOT_HEAD + 30, static_cast<u32>(kSword)},
                                  {uf::PLAYER_FIELD_INV_SLOT_HEAD + 31, static_cast<u32>(kSword >> 32)},
                                  {uf::PLAYER_FIELD_INV_SLOT_HEAD + 32, static_cast<u32>(kDagger)},
                                  {uf::PLAYER_FIELD_INV_SLOT_HEAD + 33, static_cast<u32>(kDagger >> 32)}})));
    const game::Entity* me = h.store.find(kPlayer);
    REQUIRE(me);
    std::array<game::WeaponEnchant, 2> out{};
    REQUIRE(game::weaponEnchants(h.store, *me, h.play.enchantTimers(), h.now, out) == 2);
    CHECK(out[0].equipSlot == game::kEquipOffHand);
    CHECK(out[0].item == kDagger);
    CHECK(out[0].enchantId == 40);
    CHECK(out[0].remainingMs == 0); // nothing said yet
    CHECK(out[1].equipSlot == game::kEquipMainHand);
    CHECK(out[1].itemEntry == 2024);
    CHECK(out[1].charges == 40);
    CHECK(out[1].remainingMs == 1800000); // the item's field until the server says better

    // The server's timers win and count down.
    REQUIRE(h.packet(Opcode::SMSG_ITEM_ENCHANT_TIME_UPDATE, enchantTime(kSword, 1, 1500)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_ENCHANT_TIME_UPDATE, enchantTime(kDagger, 1, 30)) ==
            game::Gameplay::Handled::Yes);
    h.now += 10000;
    REQUIRE(game::weaponEnchants(h.store, *me, h.play.enchantTimers(), h.now, out) == 2);
    CHECK(out[0].remainingMs == 20000);
    CHECK(out[1].remainingMs == 1490000);
    CHECK(h.packet(Opcode::SMSG_ITEM_ENCHANT_TIME_UPDATE, Bytes(10, 0)) ==
          game::Gameplay::Handled::Malformed);

    // The poison wears off (the field clears): only the off hand's remains. Room for one: the first.
    h.apply(values(kSword, {{uf::ITEM_FIELD_ENCHANTMENT + 3, 0}}));
    REQUIRE(game::weaponEnchants(h.store, *me, h.play.enchantTimers(), h.now, out) == 1);
    CHECK(out[0].item == kDagger);
    CHECK(game::weaponEnchants(h.store, *me, h.play.enchantTimers(), h.now, std::span(out).first(0)) == 0);
}

TEST_CASE("Auras: a party member's auras from its stats while out of sight") {
    Harness h;
    h.apply(created(kPlayer, game::ObjectType::Player, playerFields()));
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupWith(kAeryn)) == game::Gameplay::Handled::Yes);
    std::array<game::AuraInfo, game::kMaxAuras> auras{};
    CHECK(h.play.auras(kAeryn, auras) == 0);
    CHECK(h.play.auras(0x99, auras) == 0); // nobody

    // The full stats name every occupied slot: Fortitude (slot 0), Arcane Intellect (slot 4), Corruption
    // (slot 33).
    REQUIRE(h.packet(Opcode::SMSG_PARTY_MEMBER_STATS_FULL,
                     auraStats(kAeryn, {{0, 1243}, {4, 1459}, {33, 172}})) == game::Gameplay::Handled::Yes);
    REQUIRE(h.play.auras(kAeryn, auras) == 3);
    CHECK(auras[0].spellId == 1243);
    CHECK(auras[0].positive());
    CHECK(auras[1].slot == 4);
    CHECK(auras[2].spellId == 172);
    CHECK(auras[2].slot == 33);
    CHECK_FALSE(auras[2].positive());
    CHECK(auras[2].stacks == 1);
    CHECK_FALSE(auras[2].timed());

    // Changes name only the slots that changed (0: emptied).
    REQUIRE(h.packet(Opcode::SMSG_PARTY_MEMBER_STATS, auraStats(kAeryn, {{4, 0}, {40, 980}})) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.play.auras(kAeryn, auras) == 3);
    CHECK(auras[0].spellId == 1243);
    CHECK(auras[1].spellId == 172);
    CHECK(auras[2].spellId == 980);
    CHECK(auras[2].slot == 40);
    // A new full set replaces everything.
    REQUIRE(h.packet(Opcode::SMSG_PARTY_MEMBER_STATS_FULL, auraStats(kAeryn, {{1, 6673}})) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.play.auras(kAeryn, auras) == 1);
    CHECK(auras[0].spellId == 6673);
    CHECK(h.play.auras(kAeryn, std::span(auras).first(0)) == 0);

    // In sight its update fields tell (stacks included); when it leaves sight, what it had then stays.
    h.apply(created(kAeryn, game::ObjectType::Player,
                    playerFields({{uf::UNIT_FIELD_AURA + 2, 774},
                                  {uf::UNIT_FIELD_AURA + 35, 2818},
                                  {uf::UNIT_FIELD_AURAAPPLICATIONS + 8, 2u << 24}})));
    REQUIRE(h.play.auras(kAeryn, auras) == 2);
    CHECK(auras[1].stacks == 3);
    h.play.update(h.now);
    Bytes destroy;
    net::ByteWriter(destroy).u64v(kAeryn);
    REQUIRE(h.store.applyDestroy(destroy));
    REQUIRE(h.play.auras(kAeryn, auras) == 2);
    CHECK(auras[0].spellId == 774);
    CHECK(auras[1].spellId == 2818);
    CHECK(auras[1].slot == 35);
}

TEST_CASE("Auras: dispel types of Spell.dbc and their names") {
    const game::AbilityData* data = abilityData();
    if (!data) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    const game::SpellText& text = data->spellText();
    CHECK(text.dispelType(172) == 1);  // Corruption: Magic
    CHECK(text.dispelType(980) == 2);  // Curse of Agony: Curse
    CHECK(text.dispelType(2818) == 4); // Deadly Poison: Poison
    CHECK(text.dispelType(772) == 0);  // Rend: none
    CHECK(text.dispelType(1243) == 1); // Power Word: Fortitude: Magic
    CHECK(text.dispelType(0xFFFFFF) == 0);
    CHECK(text.dispelName(1) == "Magic");
    CHECK(text.dispelName(2) == "Curse");
    CHECK(text.dispelName(3) == "Disease");
    CHECK(text.dispelName(4) == "Poison");
    CHECK(text.dispelName(0).empty());
    game::SpellDetails details;
    REQUIRE(text.details(172, details));
    CHECK(details.dispel == 1);
    // Fortitude: a buff the player may drop.
    REQUIRE(text.details(1243, details));
    game::AuraInfo fortitude;
    fortitude.spellId = 1243;
    fortitude.flags = game::AuraInfo::kCancelable;
    CHECK(game::canCancelAura(fortitude, details, false));
    // Devotion Aura (rank 1): an area aura, the caster's own only.
    REQUIRE(text.details(465, details));
    CHECK_FALSE(game::canCancelAura(fortitude, details, false));
    CHECK(game::canCancelAura(fortitude, details, true));
}
