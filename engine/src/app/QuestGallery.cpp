#include "app/QuestGallery.h"

#include "app/QuestWindows.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "net/ByteBuffer.h"
#include "net/Messages.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <utility>
#include <vector>

namespace vd {
namespace {

namespace uf = net::uf;

constexpr u64 kPlayer = 0x21;
constexpr u64 kMember = 0x22; // a party member (Aeryn)

constexpr u64 icon(const char* name) {
    u64 hash = assetPathHash("Interface\\Icons\\");
    for (const char* c = name; *c; ++c) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(*c));
        hash *= kFnvPrime;
    }
    for (const char c : std::string_view(".blp")) {
        hash ^= static_cast<u8>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

// Items, creatures and quests of the samples (Elwynn Forest and Westfall, as the server's templates word
// them).
struct SampleItem {
    u32 entry;
    const char* name;
    u32 quality;
    u64 icon;
};
const SampleItem kItems[] = {
    {772, "Large Candle", 1, icon("INV_Misc_Candle_02")},
    {1931, "Huge Gnoll Claw", 1, icon("INV_Misc_MonsterClaw_04")},
    {2547, "Boar Meat", 1, icon("INV_Misc_Food_14")},
    {1917, "Jeweled Dagger", 2, icon("INV_Weapon_ShortBlade_05")},
    {2224, "Militia Dagger", 1, icon("INV_Weapon_ShortBlade_02")},
    {6084, "Stormwind Guard Leggings", 2, icon("INV_Pants_02")},
    {6085, "Footman Tunic", 2, icon("INV_Shirt_05")},
    {2723, "Bottle of Pinot Noir", 1, icon("INV_Drink_05")},
};
struct SampleCreature {
    u32 entry;
    const char* name;
};
const SampleCreature kCreatures[] = {
    {478, "Riverpaw Outrunner"}, {500, "Riverpaw Scout"}, {453, "Riverpaw Mystic"}};

struct SampleQuest {
    u32 id;
    const char* title;
    u32 level;
    i32 zone;
    u32 type;  // QuestInfo
    u32 flags; // QUEST_FLAGS_SHARABLE...
    i32 money;
    u32 spell;
    const char* objectives;
    const char* details;
    std::array<std::pair<u32, u32>, 2> kills; // creature, count
    std::array<std::pair<u32, u32>, 2> items; // item, count
    std::array<std::pair<u32, u32>, 3> choices;
    std::array<std::pair<u32, u32>, 2> rewards;
};
const SampleQuest kQuests[] = {
    {11,
     "Riverpaw Gnoll Bounty",
     10,
     12,
     0,
     0x08,
     900,
     0,
     "Collect 8 Painted Gnoll Armbands for Deputy Rainer, out by the Brackwell Pumpkin Patch.",
     "The gnolls of the Riverpaw clan have crossed the river into Elwynn. Thin their numbers and bring me "
     "proof.",
     {{{478, 10}, {500, 6}}},
     {},
     {},
     {}},
    {60,
     "Kobold Candles",
     9,
     12,
     0,
     0x08,
     0,
     0,
     "Gather 8 Large Candles for William Pestle of Goldshire.",
     "The kobolds in the Jasperlode Mine carry large candles. I need them for my work, $N.",
     {},
     {{{772, 8}, {}}},
     {{{1917, 1}, {2224, 1}}},
     {}},
    {62,
     "The Fargodeep Mine",
     7,
     12,
     0,
     0x08,
     350,
     0,
     "Scout the Fargodeep Mine, then go back to Marshal Dughan.",
     "Marshal Dughan wants you to explore the Fargodeep Mine, south of Goldshire, and report back.",
     {},
     {},
     {},
     {}},
    {176,
     "Wanted: \"Hogger\"",
     11,
     12,
     1,
     0x08,
     0,
     0,
     "Defeat Hogger and take his Huge Gnoll Claw to Marshal Dughan.",
     "A huge gnoll known as Hogger is terrorizing the southwest woods. Bring proof of his death.",
     {},
     {{{1931, 1}, {}}},
     {{{6084, 1}, {6085, 1}}},
     {}},
    {38,
     "Westfall Stew",
     14,
     40,
     0,
     0x08,
     0,
     0,
     "Gather 3 Stringy Vulture Meat and 3 Goretusk Livers for Salma.",
     "Times are hard in Westfall: a hearty stew keeps us going. Bring me what I need.",
     {},
     {{{2547, 3}, {}}},
     {},
     {{{2723, 1}, {}}}},
    {155,
     "The Defias Brotherhood",
     14,
     40,
     81,
     0,
     0,
     0,
     "Follow the Defias Traitor and keep him alive to the hideout.",
     "The traitor will lead you to their hideout, but we have little time before he changes his mind.",
     {},
     {},
     {},
     {}},
    {1665,
     "Bartleby's Mug",
     10,
     -81,
     0,
     0,
     -1000,
     71,
     "Win Bartleby's Mug back for its owner.",
     "Bartleby the drunk will teach you a thing about defense... once you best him.",
     {},
     {},
     {},
     {}},
};

// Values block (Object::BuildValuesUpdate): mask words, the mask, then the values in index order.
void writeValues(net::ByteWriter& w, std::vector<std::pair<u16, u32>> fields) {
    std::sort(fields.begin(), fields.end());
    const u32 words = fields.back().first / 32u + 1u;
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

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

} // namespace

struct QuestGallery::Session {
    game::EntityStore entities;
    game::Gameplay play{entities};
};

QuestGallery::QuestGallery() = default;
QuestGallery::~QuestGallery() = default;

void QuestGallery::build(u32 nowMs) {
    m_session = std::make_unique<Session>();
    game::Gameplay& play = m_session->play;
    game::PacketSink sink;
    sink.send = [](void*, net::Opcode opcode, std::span<const u8>) {
        if (opcode == net::Opcode::CMSG_PUSHQUESTTOPARTY) {
            logResult("quest shared");
        }
        return true;
    };
    play.setSink(sink);
    play.enterWorld(kPlayer, 0, true, nowMs);
    // Names and templates, as their query answers bring them.
    for (const SampleItem& s : kItems) {
        net::ItemQueryInfo item;
        item.entry = s.entry;
        item.found = true;
        item.name = s.name;
        item.quality = s.quality;
        play.queries().onItem(std::move(item));
    }
    for (const SampleCreature& s : kCreatures) {
        net::CreatureQueryInfo creature;
        creature.entry = s.entry;
        creature.found = true;
        creature.name = s.name;
        play.queries().onCreature(std::move(creature));
    }
    for (const SampleQuest& s : kQuests) {
        net::QuestQueryResponse q;
        q.questId = s.id;
        q.method = 2;
        q.level = s.level;
        q.zoneOrSort = s.zone;
        q.type = s.type;
        q.flags = s.flags;
        q.money = s.money;
        q.rewardSpell = s.spell;
        q.title = s.title;
        q.objectives = s.objectives;
        q.details = s.details;
        for (u32 i = 0; i < 2; ++i) {
            q.objectiveList[i].creatureOrGo = s.kills[i].first;
            q.objectiveList[i].creatureOrGoCount = s.kills[i].second;
            q.objectiveList[i].itemId = s.items[i].first;
            q.objectiveList[i].itemCount = s.items[i].second;
            q.rewards[i] = {s.rewards[i].first, s.rewards[i].second, 0};
        }
        for (u32 i = 0; i < 3; ++i) {
            q.choices[i] = {s.choices[i].first, s.choices[i].second, 0};
        }
        play.queries().onQuest(q);
    }
    // The player: level 12, 9 silver, the quest log (Player::SetQuestSlot / SetQuestSlotCounter / State /
    // Timer).
    std::vector<std::pair<u16, u32>> fields = {{uf::UNIT_FIELD_HEALTH, 200},
                                               {uf::UNIT_FIELD_BYTES_0, 1},
                                               {uf::UNIT_FIELD_LEVEL, 12},
                                               {uf::UNIT_FIELD_FACTIONTEMPLATE, 1},
                                               {uf::PLAYER_FIELD_COINAGE, 900}};
    const u32 timer = static_cast<u32>(std::time(nullptr)) + 12 * 60 + 34; // a timed quest: 12 Mins 34 Secs
    const struct {
        u32 quest;
        u32 counters; // four 6-bit counters, the state byte
        u32 timer;
    } slots[] = {{11, 7u | (2u << 6), 0}, {60, 0, 0},      {62, 1u << 24, 0}, {176, 0, 0}, {38, 0, 0},
                 {155, 2u << 24, 0},      {1665, 0, timer}};
    for (u32 slot = 0; slot < std::size(slots); ++slot) {
        const u16 base = static_cast<u16>(uf::PLAYER_QUEST_LOG_1_1 + slot * 3);
        fields.push_back({base, slots[slot].quest});
        fields.push_back({static_cast<u16>(base + 1), slots[slot].counters});
        fields.push_back({static_cast<u16>(base + 2), slots[slot].timer});
    }
    std::vector<u8> update;
    net::ByteWriter w(update);
    w.u32v(1).u8v(0).u8v(2).packedGuid(kPlayer).u8v(static_cast<u8>(game::ObjectType::Player)).u8v(0x40);
    w.f32v(-9459.0f).f32v(42.0f).f32v(57.0f).f32v(0.0f);
    writeValues(w, std::move(fields));
    std::vector<u64> created;
    game::UpdateStats stats;
    stats.createdGuids = &created;
    m_session->entities.applyUpdateObject(update, nowMs, &stats);
    play.afterUpdate(created, nowMs);
    // A party member in sight who is on Riverpaw Gnoll Bounty too (its quest ids are group-only fields):
    // "[1]" before the title, and Share Quest enabled (SMSG_GROUP_LIST: Aeryn, the player leads).
    update.clear();
    w.u32v(1).u8v(0).u8v(2).packedGuid(kMember).u8v(static_cast<u8>(game::ObjectType::Player)).u8v(0x40);
    w.f32v(-9461.0f).f32v(44.0f).f32v(57.0f).f32v(0.0f);
    writeValues(w,
                {{uf::UNIT_FIELD_HEALTH, 180}, {uf::UNIT_FIELD_LEVEL, 11}, {uf::PLAYER_QUEST_LOG_1_1, 11}});
    m_session->entities.applyUpdateObject(update, nowMs, &stats);
    std::vector<u8> group;
    net::ByteWriter(group)
        .u8v(0)
        .u8v(0)
        .u32v(1)
        .cstring("Aeryn")
        .u64v(kMember)
        .u8v(1)
        .u8v(0)
        .u64v(kPlayer)
        .u8v(3)
        .u64v(0)
        .u8v(2)
        .u8v(0);
    play.handle(net::Opcode::SMSG_GROUP_LIST, group, nowMs);

    // Without the client's tables: QuestSort / QuestInfo / AreaTable names, the samples' icons and the spell
    // of Bartleby's Mug.
    game::QuestData& data = m_screens.data();
    data.set({{81, "Warrior"}}, {{1, "Elite"}, {81, "Dungeon"}}, m_areas,
             {{12, "Elwynn Forest"}, {40, "Westfall"}});
    // Two quests watched for good, one for a while (an objective just progressed); the Warrior header folded.
    game::QuestJournal& journal = play.questJournal();
    journal.refresh(play.questLog(), play.queries(), &m_screens.data());
    journal.toggleWatch(11, 2);
    journal.toggleWatch(176, 1);
    journal.autoWatch(60, nowMs);
    journal.collapse(-81);
    journal.select(176);

    QuestScreens::Sources sources;
    sources.itemIcon = [](void*, u32 entry) -> u64 {
        for (const SampleItem& s : kItems) {
            if (s.entry == entry) {
                return s.icon;
            }
        }
        return 0;
    };
    sources.spell = [](void*, u32 spellId) {
        QuestSpellRewardView v;
        if (spellId == 71) {
            v.iconHash = icon("Ability_Warrior_DefensiveStance");
            v.name = "Defensive Stance";
        }
        return v;
    };
    m_screens.setSources(sources);
    m_screens.setMessageSink([](void*, u32, bool, const char* line) { logResult(line); }, nullptr);
    m_screens.restart(play);
}

QuestScreens::Frame QuestGallery::frame(u32 nowMs) {
    return {m_session->play, m_session->entities, kPlayer, static_cast<u64>(std::time(nullptr)), true, nowMs};
}

bool QuestGallery::drawLog(DialogContext& c, u32 nowMs) {
    if (!m_session) {
        build(nowMs);
    }
    c.playerLevel = 12;
    const QuestScreens::Frame f = frame(nowMs);
    m_session->play.update(nowMs);
    m_screens.update(f);
    u32 abandon = 0;
    bool scrolled = false;
    const bool closed = m_screens.drawLog(f, c, abandon, scrolled);
    if (abandon != 0) {
        char line[64];
        std::snprintf(line, sizeof(line), "abandon quest %u", abandon);
        logResult(line);
    }
    if (closed) {
        logResult("quest log closed");
    }
    return closed;
}

void QuestGallery::drawWatchAt(DialogContext& c, u32 nowMs, const ui::Rect& place) {
    if (!m_session) {
        build(nowMs);
    }
    c.playerLevel = 12;
    const QuestScreens::Frame f = frame(nowMs);
    m_session->play.update(nowMs);
    m_screens.update(f);
    ui::Rect bounds;
    if (const u32 tapped = m_screens.drawWatch(f, c, place.x, place.y, place.bottom(), place.w, bounds)) {
        char line[64];
        std::snprintf(line, sizeof(line), "open the log on quest %u", tapped);
        logResult(line);
    }
}

void QuestGallery::drawWatch(DialogContext& c, u32 nowMs) {
    if (!m_session) {
        build(nowMs);
    }
    c.playerLevel = 12;
    const QuestScreens::Frame f = frame(nowMs);
    m_session->play.update(nowMs);
    m_screens.update(f);
    ui::Gui g{c.ui, c.draw};
    const ui::Rect& safe = c.safeArea;
    // Where the game puts them: the timers under the minimap cluster (200 units down) at the right edge, the watch at
    // the left over where the chat would be.
    u32 tapped = m_screens.drawTimers(f, c, safe.right() - g.px(6.0f), safe.y + g.unit(200.0f));
    ui::Rect bounds;
    const u32 watched = m_screens.drawWatch(f, c, safe.x + g.px(6.0f), safe.y + g.unit(100.0f),
                                            safe.bottom() - g.unit(170.0f), safe.w * 0.4f, bounds);
    tapped = watched != 0 ? watched : tapped;
    if (tapped != 0) {
        char line[64];
        std::snprintf(line, sizeof(line), "open the log on quest %u", tapped);
        logResult(line);
    }
}

} // namespace vd
