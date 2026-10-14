// The action bars (parity pass): which slot each button shows (ActionButton_GetPagedID: six pages of twelve, the
// form's bonus bar on the first page, the bottom left bar), when a button is usable, short of power (blue) or out of
// range, the stance bar's forms from the real Spell.dbc, the item cooldowns started by an item's cast (the item's
// category and recovery, as the server records them) and the on-next-swing spell waiting for the swing.
#include "TestData.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/ActionBar.h"
#include "game/Entities.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x11;
constexpr u64 kFriend = 0x12;
constexpr u64 kWolf = 0xF130000000000456ull;
constexpr u64 kPotionItem = 0x4000000000000888ull;
constexpr u64 kOtherPotionItem = 0x4000000000000889ull;
constexpr u64 kHearthstoneItem = 0x400000000000088Aull;
constexpr u64 kTrinketItem = 0x400000000000088Bull;

u32 low(u64 guid) {
    return static_cast<u32>(guid);
}
u32 high(u64 guid) {
    return static_cast<u32>(guid >> 32);
}
u32 floatBits(f32 value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Values block (Object::BuildValuesUpdate): mask words, mask, then the values in index order.
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

// SMSG_UPDATE_OBJECT with one create block (UPDATEFLAG_HAS_POSITION).
Bytes created(u64 guid, game::ObjectType type, const Fields& fields, f32 x = 0.0f, f32 y = 0.0f, f32 z = 0.0f) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(x).f32v(y).f32v(z).f32v(0.0f);
    writeValues(w, fields);
    return out;
}

// SMSG_UPDATE_OBJECT with one values block.
Bytes values(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

// A player slot (PLAYER_FIELD_INV_SLOT_HEAD + 2 * slot) holding `guid`.
void slot(Fields& f, u8 slotIndex, u64 guid) {
    f.push_back({static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slotIndex * 2), low(guid)});
    f.push_back({static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + slotIndex * 2 + 1), high(guid)});
}

Bytes itemObject(u64 guid, u32 entry, u32 count) {
    return created(guid, game::ObjectType::Item,
                   {{uf::OBJECT_FIELD_ENTRY, entry}, {uf::ITEM_FIELD_OWNER, low(kPlayer)}, {uf::ITEM_FIELD_STACK_COUNT, count}});
}

// SMSG_SPELL_GO as Spell::SendSpellGo writes it: the cast item (or the caster) and the caster, no targets hit.
Bytes spellGo(u64 caster, u64 unitCaster, u32 spellId) {
    Bytes out;
    net::ByteWriter(out).packedGuid(caster).packedGuid(unitCaster).u32v(spellId).u16v(0x100).u8v(0).u8v(0).u16v(0);
    return out;
}

// Gameplay over its own entity store; what it sends is dropped.
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    std::vector<u64> createdGuids;
    u32 now = 1000;

    Harness() {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void*, Opcode, std::span<const u8>) { return true; };
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
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) { return play.handle(opcode, body, now); }
};

// A table of spells for the timing rules.
struct SpellTable {
    std::vector<game::SpellInfo> spells;
    game::SpellSource source() const {
        game::SpellSource s;
        s.context = this;
        s.lookup = [](const void* context, u32 id, game::SpellInfo& out) {
            for (const game::SpellInfo& info : static_cast<const SpellTable*>(context)->spells) {
                if (info.id == id) {
                    out = info;
                    return true;
                }
            }
            return false;
        };
        return s;
    }
};

game::SpellInfo spell(u32 id, u32 recoveryMs, u32 gcdMs, u32 category = 0, u32 categoryMs = 0, u32 attributes = 0) {
    game::SpellInfo info;
    info.id = id;
    info.recoveryMs = recoveryMs;
    info.gcdCategory = gcdMs ? 133 : 0;
    info.gcdMs = gcdMs;
    info.category = category;
    info.categoryRecoveryMs = categoryMs;
    info.attributes = attributes;
    return info;
}

// An item template with one on-use spell (the item query's cooldown values: < 0 the spell's own).
net::ItemQueryInfo usableItem(u32 entry, u32 spellId, i32 cooldownMs, u32 category, i32 categoryCooldownMs,
                              u32 stackable = 20) {
    net::ItemQueryInfo i;
    i.entry = entry;
    i.found = true;
    i.name = "Item";
    i.stackable = stackable;
    i.spells[0].id = spellId;
    i.spells[0].trigger = net::kItemSpellTriggerOnUse;
    i.spells[0].charges = -1;
    i.spells[0].cooldownMs = cooldownMs;
    i.spells[0].category = category;
    i.spells[0].categoryCooldownMs = categoryCooldownMs;
    return i;
}

// A unit entity for the state rules (no store needed).
game::Entity unit(u64 guid, Fields fields, glm::vec3 position = {}) {
    game::Entity e;
    e.guid = guid;
    e.type = (guid >> 48) == 0 ? game::ObjectType::Player : game::ObjectType::Unit;
    e.values.assign(uf::PLAYER_END, 0);
    for (const auto& [index, value] : fields) {
        e.values[index] = value;
    }
    e.hasPosition = true;
    e.position = position;
    return e;
}

// The client tables (VD_PACKS_DIR), loaded once; nullptr without the packs.
const game::AbilityData* abilityData() {
    static pack::PackReader db;
    static game::AbilityData data;
    static int state = 0;
    if (state == 0) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        state = dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok() && data.init(db) ? 1 : 2;
    }
    return state == 1 ? &data : nullptr;
}

std::vector<u32> formSpells(std::vector<u32> known) {
    const game::AbilityData* data = abilityData();
    std::sort(known.begin(), known.end()); // the spell book keeps them sorted by id
    std::array<game::ShapeshiftForm, game::kMaxShapeshiftForms> forms{};
    const u32 count = game::shapeshiftForms(known, data->spellText(), forms);
    std::vector<u32> out;
    for (u32 i = 0; i < count; ++i) {
        out.push_back(forms[i].spellId);
    }
    return out;
}

} // namespace

TEST_CASE("Action bars: twelve buttons a page, six pages, the form's bonus bar and the bottom left bar") {
    // ActionButton_GetPagedID: button + (page - 1) * 12.
    CHECK(game::mainBarSlot(0, 0, 0) == 0);
    CHECK(game::mainBarSlot(0, 11, 0) == 11);
    CHECK(game::mainBarSlot(1, 0, 0) == 12);
    CHECK(game::mainBarSlot(5, 11, 0) == 71);
    CHECK(game::mainBarSlot(6, 0, 0) == 0); // the page wraps (ActionBar_PageUp past the sixth)
    // The bonus bar replaces the first page only: (NUM_ACTIONBAR_PAGES + offset - 1) * 12.
    CHECK(game::mainBarSlot(0, 0, 1) == 72);  // battle stance, cat form, stealth
    CHECK(game::mainBarSlot(0, 11, 2) == 95); // defensive stance
    CHECK(game::mainBarSlot(0, 5, 3) == 101); // bear form, berserker stance
    CHECK(game::mainBarSlot(1, 5, 3) == 17);  // other pages stay
    // MultiBarBottomLeft shows page 6.
    CHECK(game::multiBarSlot(game::kBottomLeftActionBarPage, 0) == 60);
    CHECK(game::multiBarSlot(game::kBottomLeftActionBarPage, 11) == 71);
    CHECK(game::multiBarSlot(5, 0) == 48); // BottomRight
    // The bonus bars of 1.12's forms, without the table.
    CHECK(game::bonusBarOffset(0x11, nullptr) == 1);
    CHECK(game::bonusBarOffset(0x12, nullptr) == 2);
    CHECK(game::bonusBarOffset(0x13, nullptr) == 3);
    CHECK(game::bonusBarOffset(0x01, nullptr) == 1);
    CHECK(game::bonusBarOffset(0x05, nullptr) == 3);
    CHECK(game::bonusBarOffset(0x1E, nullptr) == 1);
    CHECK(game::bonusBarOffset(0x03, nullptr) == 0); // travel form keeps the main bar
    if (const game::AbilityData* data = abilityData()) {
        for (const u32 form : {0x01u, 0x03u, 0x05u, 0x08u, 0x11u, 0x12u, 0x13u, 0x1Eu, 0x1Fu}) {
            INFO(form);
            CHECK(game::bonusBarOffset(form, data) == game::bonusBarOffset(form, nullptr));
        }
    }
}

TEST_CASE("Action bars: usable, short of power (blue), unusable (grey)") {
    game::SpellInfo heroicStrike = spell(78, 0, 0);
    heroicStrike.powerType = 1; // rage, stored x10
    heroicStrike.powerCost = 150;
    const game::Entity warrior =
        unit(kPlayer, {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_POWER2, 200}, {uf::UNIT_FIELD_LEVEL, 10}});
    game::ActionUsable u = game::spellUsable(heroicStrike, nullptr, warrior, nullptr, 0, nullptr);
    CHECK(u.usable);
    CHECK_FALSE(u.noMana);
    const game::Entity angry =
        unit(kPlayer, {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_POWER2, 100}, {uf::UNIT_FIELD_LEVEL, 10}});
    u = game::spellUsable(heroicStrike, nullptr, angry, nullptr, 0, nullptr);
    CHECK_FALSE(u.usable);
    CHECK(u.noMana); // blue: only the rage is short

    // Mana with a share of the base mana (ManaCostPercentage of UNIT_FIELD_BASE_MANA).
    game::SpellInfo heal = spell(2050, 0, 1500);
    heal.powerCost = 10;
    heal.powerCostPercent = 20;
    const game::Entity priest = unit(kPlayer, {{uf::UNIT_FIELD_HEALTH, 100},
                                               {uf::UNIT_FIELD_POWER1, 60},
                                               {uf::UNIT_FIELD_BASE_MANA, 300}});
    CHECK(game::spellPowerCost(heal, priest) == 70);
    CHECK(game::spellUsable(heal, nullptr, priest, nullptr, 0, nullptr).noMana);
    const game::Entity rich = unit(kPlayer, {{uf::UNIT_FIELD_HEALTH, 100},
                                             {uf::UNIT_FIELD_POWER1, 70},
                                             {uf::UNIT_FIELD_BASE_MANA, 300}});
    CHECK(game::spellUsable(heal, nullptr, rich, nullptr, 0, nullptr).usable);

    // Reactive abilities: the aura state the caster needs (Revenge after a block: AURA_STATE_DEFENSE, 1).
    game::SpellDetails revenge;
    revenge.casterAuraState = 1;
    u = game::spellUsable(heroicStrike, &revenge, warrior, nullptr, 0, nullptr);
    CHECK_FALSE(u.usable);
    CHECK_FALSE(u.noMana); // grey, even with the rage short too
    u = game::spellUsable(heroicStrike, &revenge, angry, nullptr, 0, nullptr);
    CHECK_FALSE(u.usable);
    CHECK_FALSE(u.noMana);
    const game::Entity blocked = unit(kPlayer, {{uf::UNIT_FIELD_HEALTH, 100},
                                                {uf::UNIT_FIELD_POWER2, 200},
                                                {uf::UNIT_FIELD_AURASTATE, 1}});
    CHECK(game::spellUsable(heroicStrike, &revenge, blocked, nullptr, 0, nullptr).usable);
    // The target's aura state (Execute under 20 %: AURA_STATE_HEALTHLESS_20_PERCENT, 2).
    game::SpellDetails execute;
    execute.targetAuraState = 2;
    const game::Entity healthy = unit(kWolf, {{uf::UNIT_FIELD_HEALTH, 100}});
    const game::Entity dying = unit(kWolf, {{uf::UNIT_FIELD_HEALTH, 10}, {uf::UNIT_FIELD_AURASTATE, 2}});
    CHECK_FALSE(game::spellUsable(heroicStrike, &execute, warrior, &healthy, 0, nullptr).usable);
    CHECK_FALSE(game::spellUsable(heroicStrike, &execute, warrior, nullptr, 0, nullptr).usable);
    CHECK(game::spellUsable(heroicStrike, &execute, warrior, &dying, 0, nullptr).usable);
    // Finishers need combo points.
    game::SpellDetails eviscerate;
    eviscerate.attributesEx = 0x00100000;
    CHECK_FALSE(game::spellUsable(heroicStrike, &eviscerate, warrior, &healthy, 0, nullptr).usable);
    CHECK(game::spellUsable(heroicStrike, &eviscerate, warrior, &healthy, 3, nullptr).usable);
    // Nothing is usable dead.
    const game::Entity dead = unit(kPlayer, {{uf::UNIT_FIELD_HEALTH, 0}, {uf::UNIT_FIELD_POWER2, 1000}});
    u = game::spellUsable(heroicStrike, nullptr, dead, nullptr, 0, nullptr);
    CHECK_FALSE(u.usable);
    CHECK_FALSE(u.noMana);
}

TEST_CASE("Action bars: range for melee, ranged, helpful spells, and the hunter's dead zone") {
    const f32 reach = 1.5f;
    const game::Entity me = unit(kPlayer, {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_COMBATREACH, floatBits(reach)}});
    std::vector<game::Entity> units; // kept alive for the pointers
    units.reserve(16);
    const auto at = [&](f32 x, f32 z = 0.0f) {
        units.push_back(unit(kWolf, {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_COMBATREACH, floatBits(reach)}},
                             {x, 0.0f, z}));
        return &units.back();
    };
    // Melee abilities (5 yd "combat range"): both reaches + 1 + 1.333, on the ground plane (5.33 + 1 = 5.33..).
    game::SpellInfo strike = spell(78, 0, 0);
    strike.maxRange = 5.0f;
    CHECK(game::spellRange(strike, me, nullptr, 0) == game::ActionRange::None);
    CHECK(game::spellRange(strike, me, &me, 0) == game::ActionRange::None);
    CHECK(game::spellRange(strike, me, at(5.0f), 0) == game::ActionRange::In);
    CHECK(game::spellRange(strike, me, at(5.3f, 10.0f), 0) == game::ActionRange::In); // height is ignored
    CHECK(game::spellRange(strike, me, at(5.4f), 0) == game::ActionRange::Out);
    // Ranged spells: the distance less both reaches, 1.25 yd of give (30 + 3 + 1.25).
    game::SpellInfo bolt = spell(686, 0, 1500);
    bolt.maxRange = 30.0f;
    CHECK(game::spellRange(bolt, me, at(34.0f), 0) == game::ActionRange::In);
    CHECK(game::spellRange(bolt, me, at(34.2f), 0) == game::ActionRange::In);
    CHECK(game::spellRange(bolt, me, at(34.3f), 0) == game::ActionRange::Out);
    CHECK(game::spellRange(bolt, me, at(20.0f, 20.0f), 0) == game::ActionRange::In);
    CHECK(game::spellRange(bolt, me, at(30.0f, 20.0f), 0) == game::ActionRange::Out); // 36 yd in 3D
    // The minimum range (Auto Shot 8-35 yd): too close is out of range too.
    game::SpellInfo autoShot = spell(75, 0, 0);
    autoShot.minRange = 8.0f;
    autoShot.maxRange = 35.0f;
    CHECK(game::spellRange(autoShot, me, at(10.0f), 0) == game::ActionRange::Out);
    CHECK(game::spellRange(autoShot, me, at(12.0f), 0) == game::ActionRange::In);
    // A spell without range (cast on the player).
    CHECK(game::spellRange(spell(2457, 0, 0), me, at(3.0f), 0) == game::ActionRange::None);
}

TEST_CASE("Action bars: a helpful spell and a bandage measured to a friend") {
    // The HUD measures to where the action lands (Gameplay::spellTarget): a friendly target here.
    game::SpellInfo heal = spell(2050, 0, 1500);
    heal.maxRange = 40.0f;
    game::SpellInfo bandage = spell(746, 0, 0); // First Aid: 15 yd
    bandage.maxRange = 15.0f;
    const game::Entity me = unit(kPlayer, {{uf::UNIT_FIELD_HEALTH, 100}});
    const game::Entity mate = unit(kFriend, {{uf::UNIT_FIELD_HEALTH, 100}}, {30.0f, 0.0f, 0.0f});
    CHECK(game::spellRange(heal, me, &mate, 0) == game::ActionRange::In);
    CHECK(game::spellRange(bandage, me, &mate, 0) == game::ActionRange::Out);
    CHECK(game::spellRange(bandage, me, nullptr, 0) == game::ActionRange::None); // on the player
}

TEST_CASE("Action bars: the stance bar's forms from Spell.dbc, in the spell book's order") {
    if (!abilityData()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    // A warrior: Battle (level 1), Defensive (10), Berserker Stance (30); other spells are not forms.
    CHECK(formSpells({2458, 71, 2457, 78, 6603}) == std::vector<u32>{2457, 71, 2458});
    // A druid: Bear (10), Aquatic (16), Cat (20), Travel Form (30), Moonkin Form (talent, 40); Dire Bear Form keeps
    // Bear Form's place.
    CHECK(formSpells({768, 783, 1066, 5487, 24858}) == std::vector<u32>{5487, 1066, 768, 783, 24858});
    CHECK(formSpells({768, 783, 1066, 9634, 24858}) == std::vector<u32>{9634, 1066, 768, 783, 24858});
    CHECK(formSpells({5487, 9634, 768}) == std::vector<u32>{9634, 768});
    // A rogue: one Stealth button, its highest rank; a priest's Shadowform, a shaman's Ghost Wolf.
    CHECK(formSpells({1784, 1785, 1786}) == std::vector<u32>{1786});
    CHECK(formSpells({15473}) == std::vector<u32>{15473});
    CHECK(formSpells({2645}) == std::vector<u32>{2645});
    // Aspects and paladin auras are not on the 1.12 bar.
    CHECK(formSpells({13163, 5118, 465}).empty());

    // A tap: casts; on the form in use, leaves it, except a warrior's stance.
    const game::SpellText& text = abilityData()->spellText();
    game::SpellDetails battle, cat, stealth;
    REQUIRE(text.details(2457, battle));
    REQUIRE(text.details(768, cat));
    REQUIRE(text.details(1784, stealth));
    CHECK(game::formTap(battle, false) == game::FormTap::Cast);
    CHECK(game::formTap(battle, true) == game::FormTap::Nothing);
    CHECK(game::formTap(cat, true) == game::FormTap::Cancel);
    CHECK(game::formTap(stealth, true) == game::FormTap::Cancel);
    CHECK(game::formTap(cat, false) == game::FormTap::Cast);
}

TEST_CASE("Action bars: an item's cast starts the item's cooldown (potions share their category)") {
    Harness h;
    SpellTable table;
    constexpr u32 kHealingPotion = 439, kManaPotion = 437, kHearthstone = 8690, kTrinketSpell = 23133;
    constexpr u32 kPotionEntry = 118, kManaPotionEntry = 2455, kHearthstoneEntry = 6948, kTrinketEntry = 18638;
    // Spell.dbc's own values: the potions' 10 s category 4, the Hearthstone's hour in category 89.
    table.spells = {spell(kHealingPotion, 0, 0, 4, 10000), spell(kManaPotion, 0, 0, 4, 10000),
                    spell(kHearthstone, 0, 1500, 89, 3600000), spell(kTrinketSpell, 0, 0)};
    h.play.setSpellSource(table.source());
    Fields player = {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_LEVEL, 10}};
    slot(player, 23, kPotionItem);
    slot(player, 24, kOtherPotionItem);
    slot(player, 25, kHearthstoneItem);
    slot(player, 13, kTrinketItem); // trinket 1 (equipment slot 13)
    h.apply(created(kPlayer, game::ObjectType::Player, player));
    h.apply(itemObject(kPotionItem, kPotionEntry, 3));
    h.apply(itemObject(kOtherPotionItem, kManaPotionEntry, 1));
    h.apply(itemObject(kHearthstoneItem, kHearthstoneEntry, 1));
    h.apply(itemObject(kTrinketItem, kTrinketEntry, 1));
    // The item queries: potions in the database's category 4 with two minutes (spellcooldown -1: the spell's own,
    // none); the Hearthstone without database values (the spell's: sent as the server's HandleItemQuerySingle does).
    h.play.queries().onItem(usableItem(kPotionEntry, kHealingPotion, -1, 4, 120000));
    h.play.queries().onItem(usableItem(kManaPotionEntry, kManaPotion, -1, 4, 120000));
    h.play.queries().onItem(usableItem(kHearthstoneEntry, kHearthstone, 0, 89, 3600000, 1));
    h.play.queries().onItem(usableItem(kTrinketEntry, kTrinketSpell, 180000, 0, -1, 1));
    h.play.update(h.now);
    const game::SpellBook& spells = h.play.spells();
    const game::QueryCache& queries = h.play.queries();

    // The healing potion goes off from its item (SMSG_SPELL_GO's caster is the item): two minutes for both potions.
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, spellGo(kPotionItem, kPlayer, kHealingPotion)) ==
            game::Gameplay::Handled::Yes);
    const game::CooldownView potion = spells.itemCooldown(*queries.findItem(kPotionEntry), h.now + 1000);
    CHECK(potion.durationMs == 120000);
    CHECK(potion.remaining(h.now + 1000) == 119000);
    CHECK(spells.itemCooldown(*queries.findItem(kManaPotionEntry), h.now + 1000).remaining(h.now + 1000) == 119000);
    CHECK(spells.itemCooldown(*queries.findItem(kHearthstoneEntry), h.now + 1000).remaining(h.now + 1000) == 0);
    CHECK(spells.itemCooldown(*queries.findItem(kPotionEntry), h.now + 121000).remaining(h.now + 121000) == 0);

    // The Hearthstone: its query carries the spell's own hour (category 89).
    h.now += 200000;
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, spellGo(kHearthstoneItem, kPlayer, kHearthstone)) ==
            game::Gameplay::Handled::Yes);
    CHECK(spells.itemCooldown(*queries.findItem(kHearthstoneEntry), h.now + 60000).remaining(h.now + 60000) ==
          3540000);
    // An equipped trinket: its own three minutes; it is worn (the green border) and usable from the bar.
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, spellGo(kTrinketItem, kPlayer, kTrinketSpell)) ==
            game::Gameplay::Handled::Yes);
    CHECK(spells.itemCooldown(*queries.findItem(kTrinketEntry), h.now + 1000).remaining(h.now + 1000) == 179000);
    CHECK(h.play.inventory().isEquipped(kTrinketEntry));
    CHECK_FALSE(h.play.inventory().isEquipped(kPotionEntry));

    // A cast of another player's item, or not from an item, takes no item cooldown; an unknown item neither.
    game::ItemSpellCooldown cd;
    net::SpellGo go;
    REQUIRE(net::parseSpellGo(spellGo(kPlayer, kPlayer, kHealingPotion), go));
    CHECK_FALSE(h.play.inventory().itemCastCooldown(go, cd));
    REQUIRE(net::parseSpellGo(spellGo(kPotionItem, kFriend, kHealingPotion), go));
    CHECK_FALSE(h.play.inventory().itemCastCooldown(go, cd));
    REQUIRE(net::parseSpellGo(spellGo(0x4000000000000999ull, kPlayer, kHealingPotion), go));
    CHECK_FALSE(h.play.inventory().itemCastCooldown(go, cd));
    REQUIRE(net::parseSpellGo(spellGo(kPotionItem, kPlayer, kHealingPotion), go));
    REQUIRE(h.play.inventory().itemCastCooldown(go, cd));
    CHECK(cd.category == 4);
    CHECK(cd.recoveryMs == -1);
    CHECK(cd.categoryRecoveryMs == 120000);
}

TEST_CASE("Action bars: an on-next-swing spell is the current action until it goes off or fails") {
    Harness h;
    SpellTable table;
    constexpr u32 kHeroicStrike = 78, kRend = 772;
    table.spells = {spell(kHeroicStrike, 0, 0, 0, 0, 0x00000400), spell(kRend, 0, 1500)};
    h.play.setSpellSource(table.source());
    h.apply(created(kPlayer, game::ObjectType::Player, {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_LEVEL, 10}}));
    h.apply(created(kWolf, game::ObjectType::Unit, {{uf::UNIT_FIELD_HEALTH, 10}}));
    h.play.update(h.now);
    const game::SpellBook& spells = h.play.spells();
    CHECK(spells.queuedSwing(h.now) == 0);
    REQUIRE(h.play.castSpell(kHeroicStrike, kWolf));
    CHECK(spells.queuedSwing(h.now + 100) == kHeroicStrike);
    REQUIRE(h.play.castSpell(kRend, kWolf)); // other spells leave it queued
    CHECK(spells.queuedSwing(h.now + 100) == kHeroicStrike);
    // The swing comes: SMSG_SPELL_GO.
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, spellGo(kPlayer, kPlayer, kHeroicStrike)) == game::Gameplay::Handled::Yes);
    CHECK(spells.queuedSwing(h.now + 200) == 0);
    // Refused (no rage): SMSG_CAST_RESULT failed.
    REQUIRE(h.play.castSpell(kHeroicStrike, kWolf));
    Bytes refused;
    net::ByteWriter(refused).u32v(kHeroicStrike).u8v(2).u8v(net::kCastFailedOutOfRange);
    REQUIRE(h.packet(Opcode::SMSG_CAST_RESULT, refused) == game::Gameplay::Handled::Yes);
    CHECK(spells.queuedSwing(h.now + 200) == 0);
    // Never swung: forgotten after a while.
    REQUIRE(h.play.castSpell(kHeroicStrike, kWolf));
    CHECK(spells.queuedSwing(h.now + game::SpellBook::kQueuedSwingMs - 1) == kHeroicStrike);
    CHECK(spells.queuedSwing(h.now + game::SpellBook::kQueuedSwingMs) == 0);
}

TEST_CASE("Action bars: their texts are in ui.vdpk, in English and Spanish") {
    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    constexpr const char* kKeys[] = {"VD_ACTIONBAR_EDIT", "SHOW_MULTIBAR1_TEXT", "ITEM_COOLDOWN_TIME_SEC",
                                     "ITEM_COOLDOWN_TIME_MIN", "ITEM_COOLDOWN_TIME_HOURS_P1", "VD_COMBAT_ACTION_TITLE",
                                     "MINUTE_ONELETTER_ABBR"};
    char buffer[128];
    for (const char* key : kKeys) {
        INFO(key);
        CHECK(text::has(key));
    }
    CHECK(text::get("VD_ACTIONBAR_EDIT") == "Edit");
    CHECK(text::get("SHOW_MULTIBAR1_TEXT").starts_with("Show Bottom Left "));
    CHECK(text::format(buffer, "ITEM_COOLDOWN_TIME_SEC", 5) == "Cooldown remaining: 5 sec");
    CHECK(text::format(buffer, "VD_COMBAT_ACTION_TITLE", 12, 6) == "Button 12 of Page 6");
    REQUIRE(text::load(std::filesystem::path(std::getenv("VD_PACKS_DIR")) / "ui.vdpk", "esES"));
    CHECK(text::get("VD_ACTIONBAR_EDIT") == "Editar");
    const std::string_view cooldown = text::format(buffer, "ITEM_COOLDOWN_TIME_MIN", 3);
    CHECK(cooldown.starts_with(test::esESClientTexts() ? "Tiempo de regeneración" : "Tiempo de reutilización"));
    CHECK(cooldown.ends_with(" restante: 3 min"));
    text::unload();
}

