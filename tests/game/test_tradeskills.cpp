// Phase 13 (professions): the TradeSkillFrame and the CraftFrame as WoW.exe 1.12.1 runs them
// (game/TradeSkills): the recipe colours, the tables of db.vdpk (VD_PACKS_DIR; skipped without it) and the
// windows' flows through game::Gameplay: a profession's spell opens its window without being sent, the list
// waits for the created items' templates, headers, filters, "Create All" re-casting after each SMSG_SPELL_GO,
// Enchanting and Beast Training. Server packets are written as vmangos-core writes them; what the client
// sends is checked byte for byte.
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/TradeSkillData.h"
#include "game/TradeSkills.h"
#include "net/ByteBuffer.h"
#include "net/CombatMessages.h"
#include "net/GameMessages.h"
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
using game::RecipeColor;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x21;
constexpr u64 kPet = 0xF140000000000077ull;
constexpr u32 kWolfEntry = 299;

// Spells (Spell.dbc 1.12.1).
constexpr u32 kBlacksmithing = 2018, kRoughSharpeningStone = 2660, kCopperBracers = 2663,
              kRoughWeightstone = 3115, kCoarseWeightstone = 3116;
constexpr u32 kFirstAid = 3273, kLinenBandage = 3275;
constexpr u32 kEnchanting = 7411, kEnchantBracerHealth = 7418, kRunedCopperRod = 7421;
constexpr u32 kBeastTraining = 5149, kTeachBite1 = 17254, kTeachBite2 = 17262, kBite1 = 17253;
// Items: what the recipes make and use.
constexpr u32 kRoughStone = 2835, kLinenCloth = 2589, kCopperBar = 2840;
constexpr u32 kSharpeningStoneItem = 2862, kBracersItem = 2853, kWeightstoneItem = 3239,
              kCoarseWeightstoneItem = 3240;

// The tables, loaded once for every test case of this file (nullptr without the packs).
const game::TradeSkillData* tradeData() {
    static pack::PackReader db;
    static game::TradeSkillData data;
    static int state = 0; // 0 not tried, 1 ready, 2 missing
    if (state == 0) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        state = dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok() && data.init(db) ? 1 : 2;
    }
    return state == 1 ? &data : nullptr;
}

#define REQUIRE_TRADE_DATA(var)                                                                              \
    const game::TradeSkillData* var = tradeData();                                                           \
    if (!var) {                                                                                              \
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");                                          \
        return;                                                                                              \
    }

u32 low(u64 guid) {
    return static_cast<u32>(guid);
}
u32 high(u64 guid) {
    return static_cast<u32>(guid >> 32);
}

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

Bytes itemObject(u64 guid, u32 entry, u32 count) {
    return created(guid, game::ObjectType::Item,
                   {{uf::OBJECT_FIELD_ENTRY, entry},
                    {uf::ITEM_FIELD_OWNER, low(kPlayer)},
                    {uf::ITEM_FIELD_STACK_COUNT, count}});
}

// HandleItemQuerySingleOpcode's layout (enough of a template for the windows: class, subclass, slot, level).
Bytes itemQuery(u32 entry, const char* name, u32 itemClass, u32 subclass, u32 inventoryType, u32 itemLevel) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(entry).u32v(itemClass).u32v(subclass).cstring(name).cstring("").cstring("").cstring("");
    w.u32v(1).u32v(1).u32v(0).u32v(0).u32v(0).u32v(inventoryType); // display, quality, flags, prices, slot
    w.u32v(0x7FFF).u32v(0x1FF).u32v(itemLevel).u32v(0);            // classes, races, level, required level
    for (u32 i = 0; i < 8; ++i) {
        w.u32v(0); // required skill, rank, spell, honor, city rank, reputation faction and rank, max count
    }
    w.u32v(20).u32v(0); // stackable, container slots
    for (u32 i = 0; i < 10; ++i) {
        w.u32v(0).u32v(0);
    }
    for (u32 i = 0; i < 5; ++i) {
        w.f32v(0.0f).f32v(0.0f).u32v(0);
    }
    w.u32v(0);
    for (u32 i = 0; i < 6; ++i) {
        w.u32v(0);
    }
    w.u32v(0).u32v(0).f32v(0.0f);
    for (u32 i = 0; i < 5; ++i) {
        w.u32v(0).u32v(0).u32v(0).u32v(0xFFFFFFFFu).u32v(0).u32v(0xFFFFFFFFu);
    }
    w.u32v(0).cstring("").u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    return out;
}

// SMSG_SPELL_GO as Spell::SendSpellGo writes it (cast flags CAST_FLAG_UNKNOWN9), no targets hit.
Bytes spellGo(u64 caster, u32 spellId, u16 castFlags = 0x100) {
    Bytes out;
    net::ByteWriter(out)
        .packedGuid(caster)
        .packedGuid(caster)
        .u32v(spellId)
        .u16v(castFlags)
        .u8v(0)
        .u8v(0)
        .u16v(0);
    return out;
}

Bytes castFailed(u32 spellId, u8 reason) {
    Bytes out;
    net::ByteWriter(out).u32v(spellId).u8v(net::kCastStatusFailed).u8v(reason);
    return out;
}

// CMSG_CAST_SPELL of a spell without a target (TARGET_FLAG_SELF).
Bytes castSelf(u32 spellId) {
    Bytes out;
    net::ByteWriter(out).u32v(spellId).u16v(0);
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

    explicit Harness(const game::TradeSkillData* data) {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.tradeSkills().setData(data);
        play.enterWorld(kPlayer, 0, true, now);
        // Rough Stone x5, Linen Cloth x3 and Copper Bar x3 in the backpack.
        apply(itemObject(0x4001, kRoughStone, 5));
        apply(itemObject(0x4002, kLinenCloth, 3));
        apply(itemObject(0x4003, kCopperBar, 3));
        // A human warrior: Blacksmithing 36 / 75, First Aid 1 / 75, Enchanting 50 / 75.
        Fields f = {{uf::UNIT_FIELD_HEALTH, 100},
                    {uf::UNIT_FIELD_BYTES_0, 1u | (1u << 8)},
                    {uf::UNIT_FIELD_LEVEL, 10}};
        const auto skill = [&](u32 slot, u32 line, u32 value, u32 max) {
            f.push_back({static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + slot * 3), line});
            f.push_back({static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + slot * 3 + 1), value | (max << 16)});
        };
        skill(0, 164, 36, 75);
        skill(1, 129, 1, 75);
        skill(2, 333, 50, 75);
        const u64 items[] = {0x4001, 0x4002, 0x4003};
        for (u32 i = 0; i < 3; ++i) {
            f.push_back({static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + (23 + i) * 2), low(items[i])});
            f.push_back(
                {static_cast<u16>(uf::PLAYER_FIELD_INV_SLOT_HEAD + (23 + i) * 2 + 1), high(items[i])});
        }
        apply(created(kPlayer, game::ObjectType::Player, f));
        Bytes spells;
        net::ByteWriter w(spells);
        const u32 known[] = {
            kBlacksmithing, kRoughSharpeningStone, kCopperBracers, kRoughWeightstone,    kCoarseWeightstone,
            kFirstAid,      kLinenBandage,         kEnchanting,    kEnchantBracerHealth, kRunedCopperRod,
            kBeastTraining, kTeachBite1,           kTeachBite2};
        w.u8v(0).u16v(static_cast<u16>(std::size(known)));
        for (const u32 id : known) {
            w.u16v(static_cast<u16>(id)).u16v(0);
        }
        w.u16v(0);
        REQUIRE(packet(Opcode::SMSG_INITIAL_SPELLS, spells) == game::Gameplay::Handled::Yes);
        update();
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
    void update() {
        now += 16;
        play.update(now);
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
    // The created items' templates, as the server answers the queries the list sent.
    void answerItems() {
        REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                       itemQuery(kSharpeningStoneItem, "Rough Sharpening Stone", 0, 0, 0, 11)) ==
                game::Gameplay::Handled::Yes);
        REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                       itemQuery(kWeightstoneItem, "Rough Weightstone", 0, 0, 0, 12)) ==
                game::Gameplay::Handled::Yes);
        REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                       itemQuery(kCoarseWeightstoneItem, "Coarse Weightstone", 0, 0, 0, 21)) ==
                game::Gameplay::Handled::Yes);
        REQUIRE(packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE,
                       itemQuery(kBracersItem, "Copper Bracers", 4, 3, 9, 7)) ==
                game::Gameplay::Handled::Yes);
        update();
    }
    // The list as names, headers in brackets.
    std::vector<std::string> rows() const {
        std::vector<std::string> out;
        const game::TradeSkills& t = play.tradeSkills();
        for (const game::TradeSkillRow& row : t.rows()) {
            out.push_back(row.header ? "[" + std::string(t.headers()[row.index].name) + "]"
                                     : std::string(t.recipes()[row.index].name));
        }
        return out;
    }
    const game::TradeRecipe* recipe(u32 spellId) const {
        for (const game::TradeRecipe& r : play.tradeSkills().recipes()) {
            if (r.spellId == spellId) {
                return &r;
            }
        }
        return nullptr;
    }
    i32 rowOf(u32 spellId) const {
        const game::TradeSkills& t = play.tradeSkills();
        for (u32 i = 0; i < t.rows().size(); ++i) {
            if (!t.rows()[i].header && t.recipes()[t.rows()[i].index].spellId == spellId) {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }
};

} // namespace

TEST_CASE("Trade skills: the client's difficulty colours and inventory slot bits") {
    // Rough Sharpening Stone: yellow from 15, grey from 55, green from halfway (35).
    CHECK(game::recipeColor(1, 15, 55) == RecipeColor::Optimal);
    CHECK(game::recipeColor(14, 15, 55) == RecipeColor::Optimal);
    CHECK(game::recipeColor(15, 15, 55) == RecipeColor::Medium);
    CHECK(game::recipeColor(34, 15, 55) == RecipeColor::Medium);
    CHECK(game::recipeColor(35, 15, 55) == RecipeColor::Easy);
    CHECK(game::recipeColor(54, 15, 55) == RecipeColor::Easy);
    CHECK(game::recipeColor(55, 15, 55) == RecipeColor::Trivial);
    // No yellow rank: 25 below grey.
    CHECK(game::recipeColor(74, 0, 100) == RecipeColor::Optimal);
    CHECK(game::recipeColor(75, 0, 100) == RecipeColor::Medium);
    CHECK(game::recipeColor(88, 0, 100) == RecipeColor::Easy);
    // Slots: bracers on the wrists (bit 8), one ring, one trinket, the first bag; the rest "not equippable".
    CHECK(game::tradeSlotMask(9) == (1u << 8));
    CHECK(game::tradeSlotMask(11) == (1u << 10));
    CHECK(game::tradeSlotMask(12) == (1u << 12));
    CHECK(game::tradeSlotMask(13) == ((1u << 15) | (1u << 16))); // one-handed: either hand
    CHECK(game::tradeSlotMask(18) == (1u << 19));
    CHECK(game::tradeSlotMask(0) == (1u << 23));
    CHECK(std::string(game::kTradeSlotKeys[23]) == "NONEQUIPSLOT");
}

TEST_CASE("Trade skill data: recipes, ranks, headers, foci and pet families from db.vdpk") {
    REQUIRE_TRADE_DATA(data);
    game::TradeSpellInfo s;
    REQUIRE(data->spell(kBlacksmithing, s));
    CHECK(s.opensWindow());
    CHECK(s.misc[0] == 0);
    REQUIRE(data->spell(kEnchanting, s));
    CHECK(s.opensWindow());
    CHECK(s.misc[0] == static_cast<i32>(game::kCraftEnchanting));
    REQUIRE(data->spell(kBeastTraining, s));
    CHECK(s.misc[0] == static_cast<i32>(game::kCraftBeastTraining));
    // Copper Bracers: two copper bars at an anvil with a blacksmith hammer; one made.
    REQUIRE(data->spell(kCopperBracers, s));
    CHECK(s.tradeSpell());
    CHECK(s.reagents[0] == kCopperBar);
    CHECK(s.reagentCounts[0] == 2);
    CHECK(s.reagents[1] == 0);
    CHECK(s.spellFocus == 1);
    CHECK(data->focusName(s.spellFocus) == "Anvil");
    CHECK(s.tools[0] == 5956);
    CHECK(s.createdItem == kBracersItem);
    CHECK(s.madeMin() == 1);
    CHECK(s.madeMax() == 1);
    // Coarse Dynamite makes one to three; Large Copper Bomb two to four.
    REQUIRE(data->spell(3931, s));
    CHECK(s.madeMin() == 1);
    CHECK(s.madeMax() == 3);
    REQUIRE(data->spell(3937, s));
    CHECK(s.madeMin() == 2);
    CHECK(s.madeMax() == 4);
    // An Enchanting craft: castUI 3, an item target, the rod as its tool.
    REQUIRE(data->spell(kEnchantBracerHealth, s));
    CHECK_FALSE(s.tradeSpell());
    CHECK(s.castUi == game::kCraftEnchanting);
    CHECK(s.needsItemTarget());
    CHECK(s.tools[0] == 6218);
    CHECK(s.reagents[0] == 10940);
    // The skill line rows and their difficulty ranks.
    game::SkillAbility a;
    REQUIRE(data->ability(kRoughSharpeningStone, 1, 1, a));
    CHECK(a.skillLine == 164);
    CHECK(a.trivialLow == 15);
    CHECK(a.trivialHigh == 55);
    REQUIRE(data->ability(kBlacksmithing, 1, 1, a));
    CHECK(a.skillLine == 164);
    REQUIRE(data->ability(kEnchantBracerHealth, 1, 1, a));
    CHECK(a.skillLine == 333);
    CHECK(a.trivialLow == 70);
    CHECK(data->skillLineName(164) == "Blacksmithing");
    // Headers: the long subclass name, else the short one.
    CHECK(data->subclassName(2, 0) == "One-Handed Axes");
    CHECK(data->subclassName(4, 3) == "Mail");
    CHECK(data->subclassName(7, 0) == "Trade Goods");
    CHECK(data->subclassName(0, 0) == "Consumable");
    // Beast Training: the wolf's lines and Bite (Rank 2)'s cost there.
    const std::array<u32, 2> wolf = data->familySkillLines(1);
    CHECK(wolf[0] == 208);
    REQUIRE(data->abilityInLine(208, 17255, a));
    CHECK(a.trainingPoints == 4);
    CHECK(a.supercededBy != 0);
}

TEST_CASE("Trade skills: Blacksmithing opens without a cast, waits for its items, sorts and filters") {
    REQUIRE_TRADE_DATA(data);
    Harness h(data);
    game::TradeSkills& t = h.play.tradeSkills();
    // The profession's spell opens the window: nothing is sent (Spell_C_CastSpell).
    REQUIRE(h.play.castSpell(kBlacksmithing, 0));
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL).empty());
    REQUIRE(t.tradeSkillOpen());
    CHECK(t.skillLine() == 164);
    CHECK(t.skillLineName() == "Blacksmithing");
    CHECK(t.recipeCount() == 4);
    // The created items' templates are asked for; the list waits for them.
    CHECK(t.pending());
    CHECK(t.rows().empty());
    CHECK(h.sentWith(Opcode::CMSG_ITEM_QUERY_SINGLE).size() >= 4);
    h.answerItems();
    REQUIRE_FALSE(t.pending());
    CHECK(t.skillRank() == 36);
    CHECK(t.skillMax() == 75);
    // Headers by item class, then name; recipes by colour, item level (highest first), name.
    CHECK(h.rows() == std::vector<std::string>{"[Consumable]", "Coarse Weightstone", "Rough Weightstone",
                                               "Rough Sharpening Stone", "[Mail]", "Copper Bracers"});
    CHECK(h.recipe(kCoarseWeightstone)->color == RecipeColor::Optimal); // 65 / 80
    CHECK(h.recipe(kRoughWeightstone)->color == RecipeColor::Easy);     // 15 / 55 at 36
    CHECK(h.recipe(kCopperBracers)->color == RecipeColor::Medium);      // 20 / 60: green from 40
    // What the bags allow: 5 rough stones, 3 linen cloths, 3 copper bars (2 a bracer), no coarse stone.
    CHECK(h.recipe(kRoughSharpeningStone)->available == 5);
    CHECK(h.recipe(kRoughWeightstone)->available == 3);
    CHECK(h.recipe(kCopperBracers)->available == 1);
    CHECK(h.recipe(kCoarseWeightstone)->available == 0);
    CHECK(t.slotsPresent() == ((1u << 8) | (1u << 23)));
    // The first recipe is selected (GetFirstTradeSkill).
    CHECK(t.selected() == kCoarseWeightstone);

    // TradeSkillInvSlotDropDown: only the wrists; the selection moves to what is still shown.
    t.setSlotFilter(8);
    CHECK(h.rows() == std::vector<std::string>{"[Mail]", "Copper Bracers"});
    CHECK(t.selected() == kCopperBracers);
    t.setSlotFilter(-1);
    // TradeSkillSubClassDropDown: one subclass.
    t.setSubclassFilter(0);
    CHECK(h.rows() == std::vector<std::string>{"[Consumable]", "Coarse Weightstone", "Rough Weightstone",
                                               "Rough Sharpening Stone"});
    t.setSubclassFilter(-1);
    // A header folds and unfolds; "All" folds them all.
    t.selectRow(0);
    CHECK(h.rows() == std::vector<std::string>{"[Consumable]", "[Mail]", "Copper Bracers"});
    CHECK_FALSE(t.allCollapsed());
    t.setAllCollapsed(true);
    CHECK(h.rows() == std::vector<std::string>{"[Consumable]", "[Mail]"});
    CHECK(t.allCollapsed());
    t.setAllCollapsed(false);
    CHECK(h.rows().size() == 6);

    // The same spell again closes it (TRADE_SKILL_CLOSE); reopened, the folds and the selection stay.
    t.selectRow(static_cast<u32>(h.rowOf(kRoughSharpeningStone)));
    t.selectRow(0);
    REQUIRE(h.play.castSpell(kBlacksmithing, 0));
    CHECK_FALSE(t.tradeSkillOpen());
    REQUIRE(h.play.castSpell(kBlacksmithing, 0));
    REQUIRE(t.tradeSkillOpen());
    CHECK(h.rows() == std::vector<std::string>{"[Consumable]", "[Mail]", "Copper Bracers"});
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL).empty());
}

TEST_CASE(
    "Trade skills: Create All repeats after each SMSG_SPELL_GO until the count runs out or a cast fails") {
    REQUIRE_TRADE_DATA(data);
    Harness h(data);
    game::TradeSkills& t = h.play.tradeSkills();
    REQUIRE(h.play.castSpell(kBlacksmithing, 0));
    h.answerItems();
    t.selectRow(static_cast<u32>(h.rowOf(kRoughSharpeningStone)));
    REQUIRE(t.selected() == kRoughSharpeningStone);
    CHECK(t.repeatCount() == 1);
    // Ten asked for, five allowed by the stones: one cast now, the rest after each of the player's casts.
    REQUIRE(t.create(10));
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL) == std::vector<Bytes>{castSelf(kRoughSharpeningStone)});
    CHECK(t.repeatCount() == 5);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, spellGo(kPlayer, kRoughSharpeningStone)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL).size() == 2);
    CHECK(t.repeatCount() == 4);
    // Another unit's cast, or cast flags 0x41, change nothing.
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, spellGo(0xF130000000000001ull, kRoughSharpeningStone)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, spellGo(kPlayer, kRoughSharpeningStone, 0x41)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL).size() == 2);
    // A refused cast ends the repeat (the player moved, a reagent is gone...).
    REQUIRE(
        h.packet(Opcode::SMSG_CAST_RESULT, castFailed(kRoughSharpeningStone, net::kCastFailedOutOfRange)) ==
        game::Gameplay::Handled::Yes);
    CHECK(t.repeatCount() == 1);
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, spellGo(kPlayer, kRoughSharpeningStone)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL).size() == 2);

    // Three: two more after the first; then nothing.
    REQUIRE(t.create(3));
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, spellGo(kPlayer, kRoughSharpeningStone)) ==
                game::Gameplay::Handled::Yes);
    }
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL).size() == 5);
    // Another spell of the player's, or closing the window, ends a repeat.
    REQUIRE(t.create(4));
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, spellGo(kPlayer, 6603)) == game::Gameplay::Handled::Yes);
    CHECK(t.repeatCount() == 1);
    REQUIRE(t.create(4));
    CHECK(t.repeatCount() == 4);
    t.closeTradeSkill();
    REQUIRE(h.packet(Opcode::SMSG_SPELL_GO, spellGo(kPlayer, kRoughSharpeningStone)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL).size() == 7);
}

TEST_CASE("Trade skills: First Aid has its own window; recipes are never in the spell book") {
    REQUIRE_TRADE_DATA(data);
    Harness h(data);
    game::TradeSkills& t = h.play.tradeSkills();
    REQUIRE(h.play.castSpell(kBlacksmithing, 0));
    REQUIRE(h.play.castSpell(kFirstAid, 0)); // another profession replaces the list
    CHECK(t.skillLine() == 129);
    CHECK(t.recipeCount() == 1);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(1251, "Linen Bandage", 0, 0, 0, 5)) ==
            game::Gameplay::Handled::Yes);
    h.update();
    CHECK(h.rows() == std::vector<std::string>{"[Consumable]", "Linen Bandage"});
    CHECK(t.recipes()[0].color == RecipeColor::Optimal); // 1 against 30 / 60
    CHECK(t.recipes()[0].available == 3);
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL).empty());
}

TEST_CASE("Crafts: Enchanting and Beast Training with a pet and its training points") {
    REQUIRE_TRADE_DATA(data);
    Harness h(data);
    game::TradeSkills& t = h.play.tradeSkills();
    REQUIRE(h.play.castSpell(kEnchanting, 0));
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL).empty());
    REQUIRE(t.craftOpen());
    CHECK(t.craftList() == game::kCraftEnchanting);
    CHECK(t.craftOpener() == kEnchanting);
    CHECK_FALSE(t.craftHeaders()); // one skill line: no headers
    REQUIRE(t.crafts().size() == 2);
    CHECK(t.crafts()[0].spellId == kEnchantBracerHealth); // orange (70 / 110 at 50) before grey (5 / 10)
    CHECK(t.crafts()[0].color == RecipeColor::Optimal);
    CHECK(t.crafts()[1].spellId == kRunedCopperRod);
    CHECK(t.crafts()[1].color == RecipeColor::Trivial);
    CHECK(t.selectedCraft() == kEnchantBracerHealth);
    CHECK(t.craftSkillLine() == 333);
    CHECK(t.craftSkillRank() == 50);
    REQUIRE(h.play.castSpell(kEnchanting, 0)); // again: CRAFT_CLOSE
    CHECK_FALSE(t.craftOpen());

    // Beast Training without a pet: the abilities the hunter knows, by name and rank.
    REQUIRE(h.play.castSpell(kBeastTraining, 0));
    REQUIRE(t.craftList() == game::kCraftBeastTraining);
    REQUIRE(t.crafts().size() == 2);
    CHECK(t.crafts()[0].spellId == kTeachBite1);
    CHECK(t.crafts()[0].color == RecipeColor::None);
    CHECK(t.crafts()[1].spellId == kTeachBite2);
    u32 points = 0, level = 0;
    CHECK_FALSE(t.craftPetCost(kTeachBite2, points, level));
    CHECK(t.petTrainingPoints().total == 0);

    // A wolf (20 training points, 5 spent) that knows Bite (Rank 1): that craft is "used" and goes last.
    h.apply(created(kPet, game::ObjectType::Unit,
                    {{uf::OBJECT_FIELD_ENTRY, kWolfEntry},
                     {uf::UNIT_FIELD_HEALTH, 100},
                     {uf::UNIT_FIELD_LEVEL, 10},
                     {uf::UNIT_TRAINING_POINTS, (20u << 16) | 5u}}));
    Bytes pet;
    net::ByteWriter w(pet);
    w.u64v(kPet).u32v(0).u8v(net::kPetReactDefensive).u8v(net::kPetCommandFollow).u8v(0).u8v(0);
    for (u32 i = 0; i < 10; ++i) {
        w.u32v(0);
    }
    w.u8v(1).u32v(net::petButton(kBite1, net::kPetActEnabled));
    w.u16v(0);
    REQUIRE(h.packet(Opcode::SMSG_PET_SPELLS, pet) == game::Gameplay::Handled::Yes);
    Bytes wolf;
    net::ByteWriter(wolf)
        .u32v(kWolfEntry)
        .cstring("Wolf")
        .cstring("")
        .cstring("")
        .cstring("")
        .cstring("")
        .u32v(0)
        .u32v(1)
        .u32v(1)
        .u32v(0);
    REQUIRE(h.packet(Opcode::SMSG_CREATURE_QUERY_RESPONSE, wolf) == game::Gameplay::Handled::Yes);
    h.update();
    REQUIRE(t.crafts().size() == 2);
    CHECK(t.crafts()[0].spellId == kTeachBite2);
    CHECK(t.crafts()[0].color == RecipeColor::None);
    CHECK(t.crafts()[1].spellId == kTeachBite1);
    CHECK(t.crafts()[1].color == RecipeColor::Used);
    REQUIRE(t.craftPetCost(kTeachBite2, points, level));
    CHECK(points == 4);
    CHECK(level == 8);
    CHECK(t.petTrainingPoints().total == 20);
    CHECK(t.petTrainingPoints().available() == 15);
    CHECK(h.sentWith(Opcode::CMSG_CAST_SPELL).empty());
}
