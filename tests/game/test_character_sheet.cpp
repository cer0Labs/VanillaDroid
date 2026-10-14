// Phase 15 (character): the character window's game side. The reputation list as WoW.exe builds it
// (game/FactionList: headers by name, FACTION_OTHER and FACTION_INACTIVE, collapsing, the client's base
// reputation), the skill list (game/SkillList: headers by SkillLineCategory's sort index, learned first,
// names, collapsing and the rebuild that expands them, GetSkillLineInfo's numbers and abandon rule), the
// honor numbers from the byte-packed fields and the rank names (game/CharacterSheet), the sheet's values and
// tooltips (game/PaperDoll, PaperDollFrame.lua's formats), and game::CharacterSheet's requests through
// game::Gameplay with a capturing packet sink (inspect honor, watched and inactive factions, at war,
// unlearning a skill). Server packets are written as vmangos-core writes them.
#include "TextTable.h"
#include "core/Text.h"
#include "game/CharacterData.h"
#include "game/CharacterSheet.h"
#include "game/Entities.h"
#include "game/FactionList.h"
#include "game/Gameplay.h"
#include "game/PaperDoll.h"
#include "game/SkillList.h"
#include "game/UnitInfo.h"
#include "net/ByteBuffer.h"
#include "net/CharacterMessages.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;

using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kMe = 0x11;
constexpr u64 kAeryn = 0x21;
constexpr u64 kBrakk = 0x22;

u32 bits(f32 f) {
    u32 b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
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

Bytes createdPlayer(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(game::ObjectType::Player)).u8v(0x40);
    w.f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f);
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

Bytes honorAnswer(u64 player) {
    Bytes out;
    net::ByteWriter w(out);
    w.u64v(player).u8v(6).u32v(2u | (3u << 16)).u16v(5).u16v(0).u16v(60).u16v(0).u16v(11).u16v(0);
    w.u32v(400).u32v(9).u32v(150).u32v(2100).u32v(700).u32v(320).u8v(51);
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
    u32 now = 1000;

    explicit Harness(const Fields& extra = {}) {
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kMe, 0, true, now);
        Fields fields = {{uf::UNIT_FIELD_HEALTH, 100},
                         {uf::UNIT_FIELD_LEVEL, 10},
                         {uf::PLAYER_FIELD_WATCHED_FACTION_INDEX, 0xFFFFFFFFu}};
        fields.insert(fields.end(), extra.begin(), extra.end());
        REQUIRE(store.applyUpdateObject(createdPlayer(kMe, fields), now));
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
};

// Faction.dbc rows of the tests (names and parents as the 1.12 table has them).
game::FactionInfo faction(u32 id, i32 listId, u32 parent, std::string_view name) {
    game::FactionInfo f;
    f.id = id;
    f.listId = listId;
    f.parent = parent;
    f.name = name;
    f.raceMask[0] = 0xFF; // every race starts at 0
    return f;
}

struct Factions {
    std::vector<game::FactionInfo> rows;
    Factions() {
        rows.push_back(faction(469, 11, 0, "Alliance"));
        rows.push_back(faction(72, 19, 469, "Stormwind"));
        rows.push_back(faction(47, 20, 469, "Ironforge"));
        rows.push_back(faction(169, 10, 0, "Steamwheedle Cartel"));
        rows.push_back(faction(21, 1, 169, "Booty Bay"));
        rows.push_back(faction(529, 13, 0, "Argent Dawn"));
        game::FactionInfo cenarion = faction(609, 36, 0, "Cenarion Circle");
        cenarion.raceMask[1] =
            0x28; // tauren and night elves start at Neutral 2000 (the client's last fitting row)
        cenarion.baseValue[1] = 2000;
        rows.push_back(cenarion);
        rows.push_back(faction(910, 54, 0, "brood of Nozdormu")); // a lower-case name: compared without case
    }
    game::FactionList::Source source() const {
        game::FactionList::Source s;
        s.byList = [](const void* context, u32 listId) -> const game::FactionInfo* {
            for (const game::FactionInfo& f : static_cast<const Factions*>(context)->rows) {
                if (f.listId == static_cast<i32>(listId)) {
                    return &f;
                }
            }
            return nullptr;
        };
        s.byId = [](const void* context, u32 id) -> const game::FactionInfo* {
            for (const game::FactionInfo& f : static_cast<const Factions*>(context)->rows) {
                if (f.id == id) {
                    return &f;
                }
            }
            return nullptr;
        };
        s.context = this;
        return s;
    }
};

game::ReputationList reputation(std::vector<std::pair<u32, net::InitializeFactions::Entry>> entries) {
    net::InitializeFactions m;
    m.count = net::kFactionListSize;
    for (const auto& [listId, e] : entries) {
        m.factions[listId] = e;
    }
    game::ReputationList list;
    list.onInitialize(m);
    return list;
}

std::vector<std::string> names(const game::FactionList& list) {
    std::vector<std::string> out;
    for (const game::FactionRow& r : list.rows()) {
        if (r.faction == game::kFactionOther) {
            out.emplace_back("<Other>");
        } else if (r.faction == game::kFactionInactiveHeader) {
            out.emplace_back("<Inactive>");
        } else {
            out.emplace_back(std::string(r.header ? "+" : "") + std::string(r.name));
        }
    }
    return out;
}

game::SkillLineSource skill(u32 id, std::string_view name, u32 category, u32 order,
                            std::string_view categoryName, u32 value, u32 max) {
    game::SkillLineSource s;
    s.skill = id;
    s.name = name;
    s.category = category;
    s.categoryOrder = order;
    s.categoryName = categoryName;
    s.value = value;
    s.max = max;
    s.flags = 0x80;
    return s;
}

std::vector<std::string> names(const game::SkillList& list) {
    std::vector<std::string> out;
    for (const game::SkillLineRow& r : list.rows()) {
        out.emplace_back(std::string(r.header ? "+" : "") + std::string(r.name));
    }
    return out;
}

} // namespace

TEST_CASE("Character: the reputation list as GetFactionInfo lists it") {
    const Factions factions;
    using E = net::InitializeFactions::Entry;
    // A human: Alliance (its header flag 0x08 and visible), its cities (peace forced), Booty Bay without its
    // header's own entry visible, Argent Dawn under no parent, Cenarion Circle inactive, the Brood at Hated
    // and at war.
    const game::ReputationList list = reputation({{11, E{0x09, 0}},
                                                  {19, E{0x11, 3200}},
                                                  {20, E{0x11, 0}},
                                                  {10, E{0x08, 0}},
                                                  {1, E{0x01, -2900}},
                                                  {13, E{0x01, 100}},
                                                  {36, E{0x21, 0}},
                                                  {54, E{0x03, -42000}}});
    game::FactionList rows;
    rows.build(list, factions.source(), 1, 1, 19);
    CHECK(names(rows) == std::vector<std::string>{
                             "+Alliance", "Ironforge", "Stormwind", "+Steamwheedle Cartel", "Booty Bay",
                             "<Other>", "Argent Dawn", "brood of Nozdormu", "<Inactive>", "Cenarion Circle"});
    const game::FactionRow& stormwind = rows.rows()[2];
    CHECK(stormwind.standing == 5); // Friendly: 3200
    CHECK(stormwind.barMin == 3000);
    CHECK(stormwind.barMax == 9000);
    CHECK(stormwind.barValue == 3200);
    CHECK(stormwind.watched);
    CHECK_FALSE(stormwind.canToggleAtWar); // peace forced
    const game::FactionRow& bootyBay = rows.rows()[4];
    CHECK(bootyBay.standing == 3); // Unfriendly: -2900
    CHECK(bootyBay.canToggleAtWar);
    const game::FactionRow& brood = rows.rows()[7];
    CHECK(brood.atWar);
    CHECK(brood.standing == 1);
    CHECK_FALSE(brood.canToggleAtWar); // under -3000: no peace from here
    CHECK(rows.rows()[9].inactive);
    REQUIRE(rows.watched());
    CHECK(rows.watched()->name == "Stormwind");

    // The client's base reputation: the last fitting row (a night elf's Cenarion Circle starts at 2000).
    CHECK(game::clientBaseReputation(factions.rows[6], 4, 1) == 2000);
    CHECK(game::clientBaseReputation(factions.rows[6], 1, 1) == 0);

    // Collapsing a header hides its factions; the selection follows its faction.
    rows.select(6); // Argent Dawn
    rows.collapse(0);
    rows.build(list, factions.source(), 1, 1, 19);
    CHECK(names(rows) == std::vector<std::string>{"+Alliance", "+Steamwheedle Cartel", "Booty Bay", "<Other>",
                                                  "Argent Dawn", "brood of Nozdormu", "<Inactive>",
                                                  "Cenarion Circle"});
    CHECK(rows.rows()[0].collapsed);
    CHECK(rows.selected() == 4);
    // A new header (Cenarion Circle back from inactive empties the Inactive header, which hides; Alliance
    // stays collapsed) ... and a header that appears expands every header again.
    game::ReputationList more = list;
    more.onVisible(54);
    rows.build(more, factions.source(), 1, 1, 19);
    CHECK(rows.rows()[0].collapsed);
    game::ReputationList withNew = reputation({{11, E{0x09, 0}},
                                               {19, E{0x11, 3200}},
                                               {20, E{0x11, 0}},
                                               {1, E{0x01, -2900}},
                                               {13, E{0x01, 100}},
                                               {36, E{0x01, 0}},
                                               {54, E{0x03, -42000}}});
    rows.build(withNew, factions.source(), 1, 1, -1); // the Inactive header empty: not shown
    CHECK(names(rows).back() == "Cenarion Circle");
    CHECK_FALSE(rows.watched());
}

TEST_CASE("Character: the skill list as GetSkillLineInfo lists it") {
    std::vector<game::SkillLineSource> lines;
    lines.push_back(skill(43, "Swords", 6, 5, "Weapon Skills", 52, 75));
    lines.push_back(skill(26, "Arms", 7, 2, "Class Skills", 1, 1));
    lines.back().flags = 0x410; // a proficiency-like class skill: rank 1 of 1
    lines.push_back(skill(186, "Mining", 11, 3, "Professions", 31, 75));
    lines.back().step = 1; // Apprentice
    lines.back().flags = 0xA0;
    lines.push_back(skill(162, "Unarmed", 6, 5, "Weapon Skills", 1, 75));
    lines.push_back(skill(44, "axes", 6, 5, "Weapon Skills", 0, 0)); // not learned, not learnable: left out
    lines.push_back(skill(95, "Defense", 6, 5, "Weapon Skills", 60, 75));
    lines.back().tempBonus = 5;
    lines.back().permBonus = 2;
    lines.push_back(skill(98, "Language: Common", 10, 7, "Languages", 300, 300));
    lines.back().flags = 0xA0; // unlearnable but without a step: no button
    game::SkillList list;
    list.build(lines, 10);
    CHECK(list.changed());
    CHECK(names(list) == std::vector<std::string>{"+Class Skills", "Arms", "+Professions", "Mining",
                                                  "+Weapon Skills", "Defense", "Swords", "Unarmed",
                                                  "+Languages", "Language: Common"});
    const auto row = [&](u32 i) { return list.rows()[i]; };
    CHECK(row(1).maxRank == 1);
    CHECK(row(1).rank == 1);
    CHECK(row(3).abandonable);
    CHECK_FALSE(row(9).abandonable);
    CHECK(row(5).rank == 62); // value + permanent bonus
    CHECK(row(5).maxRank == 77);
    CHECK(row(5).modifier == 5);

    // Collapsing; the selection stays an index (it may land on another line, as 1.12's).
    list.select(6);
    list.collapse(4);
    list.build(lines, 10);
    CHECK_FALSE(list.changed());
    CHECK(names(list) == std::vector<std::string>{"+Class Skills", "Arms", "+Professions", "Mining",
                                                  "+Weapon Skills", "+Languages", "Language: Common"});
    CHECK_FALSE(list.rows()[4].expanded);
    CHECK_FALSE(list.allExpanded());
    CHECK(list.selected() == 6);
    // A skill going up rebuilds the list: every header expands again.
    lines[0].value = 53;
    list.build(lines, 10);
    CHECK(list.changed());
    CHECK(list.allExpanded());
    CHECK(list.rows().size() == 10);
    // "All": every header collapses, then expands.
    list.collapse(-1);
    list.build(lines, 10);
    CHECK(names(list) ==
          std::vector<std::string>{"+Class Skills", "+Professions", "+Weapon Skills", "+Languages"});
    list.expand(-1);
    list.build(lines, 10);
    CHECK(list.rows().size() == 10);
}

TEST_CASE("Character: the skill window's tables") {
    game::CharacterData data;
    data.addSkillInfo({186, 0, 0, 0xA0, 0, 0});
    data.addSkillInfo({98, 0x4D, 0, 0x80, 0, 0});  // Common for the Alliance's races...
    data.addSkillInfo({98, 0xA2, 0, 0xA0, 0, 21}); // ... and for the Horde's (another row)
    REQUIRE(data.skillInfo(98, 1, 1));
    CHECK(data.skillInfo(98, 1, 1)->flags == 0x80);
    CHECK(data.skillInfo(98, 2, 1)->flags == 0xA0);
    CHECK(data.skillInfo(98, 3, 1)->flags == 0x80);
    CHECK_FALSE(data.skillInfo(43, 1, 1));
    data.setClass(2, "PALADIN", true);
    CHECK(data.relicSlot(2));
    CHECK_FALSE(data.relicSlot(1));
    CHECK(game::weaponSkill(2, 7) == 43);   // a one-handed sword
    CHECK(game::weaponSkill(2, 19) == 228); // a wand
    CHECK(game::weaponSkill(4, 7) == 0);    // armor
}

TEST_CASE("Character: honor from the byte-packed fields and the rank names") {
    game::EntityStore store;
    REQUIRE(store.applyUpdateObject(
        createdPlayer(kMe, {{uf::UNIT_FIELD_BYTES_0, 1u | (1u << 8)},
                            {uf::PLAYER_FIELD_SESSION_KILLS, 4u | (2u << 16)},
                            {uf::PLAYER_FIELD_YESTERDAY_KILLS, 17},
                            {uf::PLAYER_FIELD_YESTERDAY_CONTRIBUTION, 250},
                            {uf::PLAYER_FIELD_THIS_WEEK_KILLS, 31},
                            {uf::PLAYER_FIELD_THIS_WEEK_CONTRIBUTION, 900},
                            {uf::PLAYER_FIELD_LAST_WEEK_KILLS, 60},
                            {uf::PLAYER_FIELD_LAST_WEEK_CONTRIBUTION, 3100},
                            {uf::PLAYER_FIELD_LAST_WEEK_RANK, 412},
                            {uf::PLAYER_FIELD_LIFETIME_HONORBALE_KILLS, 777},
                            {uf::PLAYER_FIELD_LIFETIME_DISHONORBALE_KILLS, 3},
                            {uf::PLAYER_FIELD_BYTES, 4u << 24}, // highest rank 4 (Dishonored): read as none
                            {uf::PLAYER_BYTES_3, 7u << 24},     // rank 7: Sergeant
                            {uf::PLAYER_FIELD_BYTES2, 191}}),   // three quarters of the way
        1000));
    const game::HonorStats h = game::honorStats(*store.find(kMe));
    CHECK(h.todayHonorable == 4);
    CHECK(h.todayDishonorable == 2);
    CHECK(h.yesterdayHonorable == 17);
    CHECK(h.yesterdayHonor == 250);
    CHECK(h.thisWeekHonorable == 31);
    CHECK(h.lastWeekHonor == 3100);
    CHECK(h.lastWeekStanding == 412);
    CHECK(h.lifetimeHonorable == 777);
    CHECK(h.highestRank == 0);
    CHECK(h.rank == 7);
    CHECK(h.progress == doctest::Approx(191.0f / 255.0f));
    CHECK(h.alliance);

    char key[40];
    CHECK(game::pvpRankKey(key, 7, true) == "PVP_RANK_7_1");
    CHECK(game::pvpRankKey(key, 18, false) == "PVP_RANK_18_0");
    CHECK(game::pvpRankKey(key, 0, false).empty());
    CHECK(game::pvpRankNumber(7) == 3);
    CHECK(game::pvpRankNumber(5) == 1);
    CHECK(game::pvpRankNumber(1) == -4); // Pariah
    CHECK(game::pvpRankNumber(0) == 0);

    net::InspectHonorStats m;
    REQUIRE(net::parseInspectHonorStats(honorAnswer(kAeryn), m));
    const game::HonorStats theirs = game::honorStats(m, nullptr);
    CHECK(theirs.highestRank == 6); // GetInspectHonorData keeps it
    CHECK(theirs.todayDishonorable == 3);
    CHECK(theirs.lastWeekHonor == 2100);
    CHECK(theirs.progress == doctest::Approx(51.0f / 255.0f));
}

TEST_CASE("Character: the inspected player's honor through Gameplay") {
    Harness h;
    game::CharacterSheet& sheet = h.play.character();
    CHECK_FALSE(sheet.requestInspectHonor()); // nobody inspected
    sheet.setInspected(kAeryn);
    CHECK(sheet.requestInspectHonor());
    CHECK_FALSE(sheet.requestInspectHonor()); // asked already
    Bytes expected;
    net::ByteWriter(expected).u64v(kAeryn);
    REQUIRE(h.sentWith(Opcode::MSG_INSPECT_HONOR_STATS).size() == 1);
    CHECK(h.sentWith(Opcode::MSG_INSPECT_HONOR_STATS)[0] == expected);
    // Another player's answer is ignored; the inspected one's is kept.
    CHECK(h.play.handle(Opcode::MSG_INSPECT_HONOR_STATS, honorAnswer(kBrakk), h.now) ==
          game::Gameplay::Handled::Yes);
    CHECK_FALSE(sheet.inspectHonor());
    CHECK(h.play.handle(Opcode::MSG_INSPECT_HONOR_STATS, honorAnswer(kAeryn), h.now) ==
          game::Gameplay::Handled::Yes);
    REQUIRE(sheet.inspectHonor());
    CHECK(sheet.inspectHonor()->lifetimeHonorable == 400);
    CHECK_FALSE(sheet.requestInspectHonor()); // had already
    // A truncated answer is malformed.
    Bytes cut = honorAnswer(kAeryn);
    cut.pop_back();
    CHECK(h.play.handle(Opcode::MSG_INSPECT_HONOR_STATS, cut, h.now) == game::Gameplay::Handled::Malformed);
    // Another player inspected (or none): the data goes.
    sheet.setInspected(kBrakk);
    CHECK_FALSE(sheet.inspectHonor());
    CHECK(sheet.requestInspectHonor());
    sheet.setInspected(0);
    CHECK_FALSE(sheet.inspectHonor());
}

TEST_CASE("Character: reputation and skill requests through Gameplay") {
    Harness h;
    game::CharacterSheet& sheet = h.play.character();
    net::InitializeFactions init;
    init.count = net::kFactionListSize;
    init.factions[13] = {0x01, 100};  // Argent Dawn
    init.factions[1] = {0x01, -2900}; // Booty Bay, Unfriendly
    h.play.abilities().reputation().onInitialize(init);

    // SetWatchedFactionIndex: only a change goes out.
    CHECK(sheet.watchedFaction() == -1);
    CHECK_FALSE(sheet.setWatchedFaction(-1));
    CHECK(sheet.setWatchedFaction(13));
    REQUIRE(h.sentWith(Opcode::CMSG_SET_WATCHED_FACTION).size() == 1);
    CHECK(h.sentWith(Opcode::CMSG_SET_WATCHED_FACTION)[0] == Bytes{13, 0, 0, 0});

    // SetFactionInactive: sent, and the flag changes here.
    CHECK(sheet.setFactionInactive(13, true));
    CHECK((h.play.abilities().reputation().entry(13).flags & net::kFactionInactive) != 0);
    CHECK(h.sentWith(Opcode::CMSG_SET_FACTION_INACTIVE)[0] == Bytes{13, 0, 0, 0, 1});
    CHECK(sheet.setFactionInactive(13, false));
    CHECK((h.play.abilities().reputation().entry(13).flags & net::kFactionInactive) == 0);

    // FactionToggleAtWar: war from anywhere, peace only from Unfriendly up, nothing in combat.
    CHECK(sheet.toggleAtWar(1, -2900) == game::CharacterSheet::WarResult::Sent);
    CHECK((h.play.abilities().reputation().entry(1).flags & net::kFactionAtWar) != 0);
    CHECK(sheet.toggleAtWar(1, -3500) == game::CharacterSheet::WarResult::Refused);
    REQUIRE(h.store.applyUpdateObject(values(kMe, {{uf::UNIT_FIELD_FLAGS, game::kUnitFlagInCombat}}), h.now));
    CHECK(sheet.toggleAtWar(1, -2900) == game::CharacterSheet::WarResult::InCombat);
    CHECK(h.sentWith(Opcode::CMSG_SET_FACTION_ATWAR).size() == 1);

    // AbandonSkill.
    CHECK(sheet.unlearnSkill(186));
    CHECK(h.sentWith(Opcode::CMSG_UNLEARN_SKILL)[0] == Bytes{186, 0, 0, 0});
}

TEST_CASE("Character: the paper doll's values and tooltips") {
    const std::vector<u8> table = test::textTable({
        {"SPELL_STAT0_NAME", "Strength"},
        {"VD_CHAR_STAT_LABEL", "%s:"},
        {"WARRIOR_STRENGTH_TOOLTIP", "Warrior strength."},
        {"DEFAULT_AGILITY_TOOLTIP", "Default agility."},
        {"ARMOR", "Armor"},
        {"ARMOR_COLON", "Armor:"},
        {"ARMOR_TOOLTIP", "Armor vs. level %d: -%.1f%%"},
        {"MELEE_ATTACK_POWER", "Melee Attack Power"},
        {"MELEE_ATTACK_POWER_TOOLTIP", "+%.1f damage a second."},
        {"NOT_APPLICABLE", "N/A"},
        {"DAMAGE_COLON", "Damage:"},
        {"ATTACK_SPEED_COLON", "Attack Speed (seconds):"},
        {"DAMAGE_PER_SECOND", "Damage per Second:"},
        {"INVTYPE_WEAPONMAINHAND", "Main Hand"},
        {"RESISTANCE2_NAME", "Fire Resistance"},
        {"RESISTANCE_TYPE2", "fire"},
        {"RESISTANCE_POOR", "Poor"},
        {"RESISTANCE_TOOLTIP_SUBTEXT", "Resist %s. Against level %d: %s"},
    });
    text::setTable(table);
    game::EntityStore store;
    REQUIRE(
        store.applyUpdateObject(createdPlayer(kMe, {{uf::UNIT_FIELD_LEVEL, 20},
                                                    {uf::UNIT_FIELD_STAT0, 45},
                                                    {uf::PLAYER_FIELD_POSSTAT0, 7},
                                                    {uf::PLAYER_FIELD_NEGSTAT0, static_cast<u32>(-2)},
                                                    {uf::UNIT_FIELD_STAT0 + 1, 30},
                                                    {uf::UNIT_FIELD_RESISTANCES, 1200},
                                                    {uf::PLAYER_FIELD_RESISTANCEBUFFMODSPOSITIVE, 100},
                                                    {uf::UNIT_FIELD_RESISTANCES + 2, 15},
                                                    {uf::PLAYER_FIELD_RESISTANCEBUFFMODSPOSITIVE + 2, 10},
                                                    {uf::UNIT_FIELD_ATTACK_POWER, 140},
                                                    {uf::UNIT_FIELD_ATTACK_POWER_MODS, 30u},
                                                    {uf::UNIT_FIELD_BASEATTACKTIME, 2600},
                                                    {uf::UNIT_FIELD_MINDAMAGE, bits(41.0f)},
                                                    {uf::UNIT_FIELD_MAXDAMAGE, bits(66.5f)},
                                                    {uf::PLAYER_FIELD_MOD_DAMAGE_DONE_PCT, bits(1.0f)},
                                                    {uf::PLAYER_SKILL_INFO_1_1, 162}, // Unarmed
                                                    {uf::PLAYER_SKILL_INFO_1_1 + 1, 95u | (100u << 16)},
                                                    {uf::PLAYER_SKILL_INFO_1_1 + 2, 5u}}), // a temporary +5
                                1000));
    game::PaperDollInput in;
    in.classFile = "WARRIOR";
    game::PaperDoll doll;
    doll.build(*store.find(kMe), in);
    using S = game::PaperDollStat;
    const game::PaperDollLine& strength = doll.line(S::Strength);
    CHECK(strength.label == "Strength:");
    CHECK(strength.value == "|cffff202045|r"); // a debuff: red even with buffs
    REQUIRE(strength.tooltipLines == 2);
    CHECK(strength.tooltip[0].left ==
          "|cffffffffStrength 45 (40|r|r|cff20ff20+7|r|cffff2020 -2|r|cffffffff)|r");
    CHECK(strength.tooltip[1].left == "Warrior strength.");
    CHECK(doll.line(S::Agility).tooltip[1].left == "Default agility."); // no WARRIOR_AGILITY_TOOLTIP
    CHECK(doll.line(S::Agility).value == "30");

    const game::PaperDollLine& armor = doll.line(S::Armor);
    CHECK(armor.value == "|cff20ff201200|r");
    CHECK(armor.tooltip[0].left == "|cffffffffArmor 1200 (1100|r|r|cff20ff20+100|r|cffffffff)|r");
    CHECK(armor.tooltip[1].left == "Armor vs. level 20: -36.4%"); // 1200 / (85 * 20 + 400)
    CHECK(doll.line(S::AttackPower).value == "|cff20ff20170|r");
    CHECK(doll.line(S::AttackPower).tooltip[1].left == "+12.1 damage a second.");
    CHECK(doll.line(S::Attack).value == "|cff20ff20100|r"); // Unarmed 95 + 5

    const game::PaperDollLine& damage = doll.line(S::Damage);
    CHECK(damage.value == "41 - 67");
    REQUIRE(damage.tooltipLines == 4);
    CHECK(damage.tooltip[0].left == "Main Hand");
    CHECK(damage.tooltip[1].right == "2.60");
    CHECK(damage.tooltip[2].right == "41 - 67");
    CHECK(damage.tooltip[3].right == "20.7"); // (41 + 66.5) / 2 / 2.6

    CHECK(doll.line(S::RangedDamage).value == "N/A"); // nothing in the ranged slot
    CHECK(doll.line(S::RangedDamage).tooltipLines == 0);
    const game::PaperDollLine& fire = doll.line(S::FireResistance);
    CHECK(fire.value == "|cff20ff2015|r");
    CHECK(fire.tooltip[0].left == "Fire Resistance 15 ( |cffffffff5|cff20ff20 +10|r )");
    CHECK(fire.tooltip[1].left == "Resist fire. Against level 20: Poor");
    text::unload();
}
