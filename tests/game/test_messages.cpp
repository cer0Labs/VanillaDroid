// Parity (messages): the mirror timer packets byte for byte as vmangos-core writes them (Server/Packets/Misc
// StartMirrorTimer, PauseMirrorTimer, StopMirrorTimer), truncated ones rejected; the bars' state through
// game::Gameplay (MirrorTimer.lua's frames); and where the client's own lines go (game/Notices): loot, money,
// experience, level-ups, quests, skill-ups, learned spells and reputation, worded by key in English and sent
// to the General chat tab, the combat log or the UIErrorsFrame as the 1.12 interface does.
#include "TestData.h"
#include "TextTable.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/AreaData.h"
#include "game/CombatText.h"
#include "game/Entities.h"
#include "game/GameData.h"
#include "game/Gameplay.h"
#include "game/ItemTooltip.h"
#include "game/MirrorTimers.h"
#include "game/Notices.h"
#include "net/ByteBuffer.h"
#include "net/MirrorTimerMessages.h"
#include "net/TravelMessages.h"
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
using game::NoticeKind;
using game::NoticePlace;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kPlayer = 0x31;
constexpr u64 kMember = 0x32;
constexpr u64 kWolf = 0xF130000000000456ull;
constexpr u32 kLinenCloth = 2589, kBracers = 2970, kBread = 4540;
constexpr u32 kKoboldCamp = 7;

template <typename F> void checkTruncations(const Bytes& payload, F&& parse) {
    for (usize n = 0; n < payload.size(); ++n) {
        CHECK_FALSE(parse(std::span<const u8>(payload.data(), n)));
    }
}

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    std::vector<u32> mask(maxIndex / 32 + 1, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(mask.size()));
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

Bytes createdPlayer(const Fields& fields) {
    return created(kPlayer, game::ObjectType::Player, fields);
}

Bytes values(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

// Player::SendMirrorTimerStart: type, remaining, duration, scale, paused (one byte), spell.
Bytes startTimer(u32 type, u32 remaining, u32 duration, i32 scale, bool paused = false, u32 spell = 0) {
    Bytes out;
    net::ByteWriter(out)
        .u32v(type)
        .u32v(remaining)
        .u32v(duration)
        .u32v(static_cast<u32>(scale))
        .u8v(paused ? 1 : 0)
        .u32v(spell);
    return out;
}

Bytes pauseTimer(u32 type, bool paused) {
    Bytes out;
    net::ByteWriter(out).u32v(type).u8v(paused ? 1 : 0);
    return out;
}

Bytes stopTimer(u32 type) {
    Bytes out;
    net::ByteWriter(out).u32v(type);
    return out;
}

// HandleItemQuerySingleOpcode's layout, the fields these tests need.
Bytes itemQuery(u32 entry, const char* name, u32 quality) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(entry).u32v(7).u32v(5).cstring(name).cstring("").cstring("").cstring("");
    w.u32v(1000).u32v(quality).u32v(0).u32v(40).u32v(10).u32v(0);
    w.u32v(0xFFFFFFFFu).u32v(0xFFFFFFFFu).u32v(5).u32v(0);
    for (u32 i = 0; i < 8; ++i) {
        w.u32v(0);
    }
    w.u32v(20).u32v(0);
    for (u32 i = 0; i < 10; ++i) {
        w.u32v(0).u32v(0);
    }
    for (u32 i = 0; i < 5; ++i) {
        w.f32v(0.0f).f32v(0.0f).u32v(0);
    }
    for (u32 i = 0; i < 7; ++i) {
        w.u32v(0);
    }
    w.u32v(0).u32v(0).f32v(0.0f);
    for (u32 i = 0; i < 5; ++i) {
        w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    }
    w.u32v(0).cstring("").u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    return out;
}

// SMSG_QUEST_QUERY_RESPONSE (QuestHandler), a kill quest.
Bytes questQuery(u32 questId, const char* title) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(questId).u32v(2).u32v(2).u32v(12).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(40);
    w.u32v(0).u32v(0).u32v(0).u32v(0);
    for (u32 i = 0; i < 4 + 6; ++i) {
        w.u32v(0).u32v(0);
    }
    w.u32v(0).f32v(0.0f).f32v(0.0f).u32v(0);
    w.cstring(title).cstring("Objectives").cstring("Details").cstring("");
    w.u32v(257).u32v(10).u32v(0).u32v(0);
    w.u32v(0).u32v(0).u32v(0).u32v(0);
    for (u32 i = 0; i < 2; ++i) {
        w.u32v(0).u32v(0).u32v(0).u32v(0);
    }
    w.cstring("").cstring("").cstring("").cstring("");
    return out;
}

// Player::SendNewItem: player, received (from an NPC), created, showInChat, bag, slot, entry, suffix,
// property, count.
Bytes itemPush(u64 player, u32 entry, u32 count, bool fromNpc = false, bool created = false,
               bool showInChat = true) {
    Bytes out;
    net::ByteWriter(out)
        .u64v(player)
        .u32v(fromNpc ? 1 : 0)
        .u32v(created ? 1 : 0)
        .u32v(showInChat ? 1 : 0)
        .u8v(255)
        .u32v(23)
        .u32v(entry)
        .u32v(0)
        .u32v(0)
        .u32v(count);
    return out;
}

Bytes u32Body(u32 value) {
    Bytes out;
    net::ByteWriter(out).u32v(value);
    return out;
}

// Stub texts for the notices' keys, with the formats of GlobalStrings.lua 1.12 (the game shows the client's own).
const std::vector<std::pair<std::string, std::string>> kEnglish = {
    {"LOOT_ITEM_SELF", "You receive loot: %s."},
    {"LOOT_ITEM_SELF_MULTIPLE", "You receive loot: %sx%d."},
    {"LOOT_ITEM_PUSHED_SELF", "You receive item: %s."},
    {"LOOT_ITEM_PUSHED_SELF_MULTIPLE", "You receive item: %sx%d."},
    {"LOOT_ITEM_CREATED_SELF", "You create: %s."},
    {"LOOT_ITEM_CREATED_SELF_MULTIPLE", "You create: %sx%d."},
    {"LOOT_ITEM", "%s receives loot: %s."},
    {"LOOT_ITEM_MULTIPLE", "%s receives loot: %sx%d."},
    {"CREATED_ITEM", "%s creates: %s."},
    {"CREATED_ITEM_MULTIPLE", "%s creates: %sx%d."},
    {"YOU_LOOT_MONEY", "You loot %s"},
    {"LOOT_MONEY_SPLIT", "Loot share: %s."},
    {"GOLD", "Gold"},
    {"SILVER", "Silver"},
    {"COPPER", "Copper"},
    {"COMBATLOG_XPGAIN_FIRSTPERSON", "%s slain: +%d experience."},
    {"COMBATLOG_XPGAIN_EXHAUSTION1", "%s slain: +%d experience (%s, %s bonus)."},
    {"COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED", "You gain %d experience."},
    {"COMBATLOG_XPLOSS_FIRSTPERSON_UNNAMED", "You lose %d experience."},
    {"UNKNOWNOBJECT", "Unknown"},
    {"LEVEL_UP", "Now level %d!"},
    {"LEVEL_UP_HEALTH", "Health +%d."},
    {"LEVEL_UP_HEALTH_MANA", "Health +%d, mana +%d."},
    {"LEVEL_UP_CHAR_POINTS", "%d new talent point."},
    {"LEVEL_UP_CHAR_POINTS_P1", "%d new talent points."},
    {"LEVEL_UP_STAT", "Your %s increases by %d."},
    {"SPELL_STAT0_NAME", "Strength"},
    {"SPELL_STAT1_NAME", "Agility"},
    {"SPELL_STAT2_NAME", "Stamina"},
    {"SPELL_STAT3_NAME", "Intellect"},
    {"SPELL_STAT4_NAME", "Spirit"},
    {"ERR_QUEST_ACCEPTED_S", "Quest accepted: %s"},
    {"ERR_QUEST_COMPLETE_S", "%s completed."},
    {"ERR_QUEST_REWARD_EXP_I", "Experience gained: %d."},
    {"ERR_QUEST_REWARD_MONEY_S", "Received %s."},
    {"ERR_QUEST_REWARD_ITEM_S", "Received item: %s."},
    {"ERR_SKILL_UP_SI", "Skill %s now %d."},
    {"ERR_LEARN_SPELL_S", "New spell: %s."},
    {"ERR_LEARN_ABILITY_S", "New ability: %s."},
    {"ERR_LEARN_RECIPE_S", "New recipe: %s."},
    {"FACTION_STANDING_INCREASED", "Reputation with %s +%d."},
    {"FACTION_STANDING_DECREASED", "Reputation with %s -%d."},
    {"FACTION_STANDING_CHANGED", "You are now %s with %s."},
    {"FACTION_STANDING_LABEL1", "Hated"},
    {"FACTION_STANDING_LABEL2", "Hostile"},
    {"FACTION_STANDING_LABEL3", "Unfriendly"},
    {"FACTION_STANDING_LABEL4", "Neutral"},
    {"FACTION_STANDING_LABEL5", "Friendly"},
    {"FACTION_STANDING_LABEL6", "Honored"},
    {"FACTION_STANDING_LABEL7", "Revered"},
    {"FACTION_STANDING_LABEL8", "Exalted"},
    {"BREATH_LABEL", "Breath"},
    {"EXHAUSTION_LABEL", "Fatigue"},
    // Phase 12: instances.
    {"INSTANCE_SAVED", "Instance saved"},
    {"INSTANCE_RESET_SUCCESS", "%s has been reset."},
    {"INSTANCE_RESET_FAILED", "%s not reset: players inside."},
    {"INSTANCE_RESET_FAILED_OFFLINE", "%s not reset: players offline."},
    {"INSTANCE_RESET_FAILED_ZONING", "%s not reset: players zoning in."},
    {"RAID_INSTANCE_WARNING_HOURS", "%s resets in %d hour."},
    {"RAID_INSTANCE_WARNING_HOURS_P1", "%s resets in %d hours."},
    {"RAID_INSTANCE_WARNING_MIN", "%s resets in %d minute!"},
    {"RAID_INSTANCE_WARNING_MIN_P1", "%s resets in %d minutes!"},
    {"RAID_INSTANCE_WARNING_MIN_SOON", "%s resets in %d minute: leave now."},
    {"RAID_INSTANCE_WARNING_MIN_SOON_P1", "%s resets in %d minutes: leave now."},
    {"RAID_INSTANCE_WELCOME", "%s: resets in %dd %dh %dm."},
    {"ERR_RAID_GROUP_ONLY", "Raid groups only"},
    {"ERR_RAID_GROUP_FULL", "The instance is full"},
    {"NO_RAID_INSTANCES_SAVED", "No saved instances"},
};

// The specifiers of a format, in order ("%s", "%d", "%.1f", "%2$s"...; "%%" is none).
std::vector<std::string> specifiers(std::string_view format) {
    std::vector<std::string> out;
    for (usize i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            continue;
        }
        usize end = i + 1;
        const std::string_view flags = "0123456789.$-+ #";
        while (end < format.size() && flags.find(format[end]) != std::string_view::npos) {
            ++end;
        }
        if (end < format.size() && format[end] != '%') {
            out.emplace_back(format.substr(i, end + 1 - i));
        }
        i = end;
    }
    return out;
}

struct EnglishTexts {
    std::vector<u8> blob = test::textTable(kEnglish);
    EnglishTexts() { text::setTable(blob); }
    ~EnglishTexts() { text::unload(); }
};

struct Line {
    NoticeKind kind;
    std::string text;
    bool operator==(const Line&) const = default;
};
std::ostream& operator<<(std::ostream& os, const Line& l) {
    return os << static_cast<u32>(l.kind) << ": " << l.text;
}

// Gameplay over its own entity store and the feed of its notices; the player is a level-9 human warrior.
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    game::NoticeFeed feed;
    std::vector<u64> createdGuids;
    u32 now = 1000;
    u32 cursor = 0;

    explicit Harness(const game::GameData* data = nullptr, const game::AbilityData* abilities = nullptr,
                     Fields extra = {}) {
        game::PacketSink sink;
        sink.send = [](void*, Opcode, std::span<const u8>) { return true; };
        play.setSink(sink);
        play.enterWorld(kPlayer, 0, true, now);
        feed.init(data, abilities);
        feed.restart(play);
        cursor = feed.lines().end();
        Fields fields = {{uf::UNIT_FIELD_HEALTH, 100},
                         {uf::UNIT_FIELD_BYTES_0, 1u | (1u << 8)}, // human warrior
                         {uf::UNIT_FIELD_FACTIONTEMPLATE, 1},
                         {uf::UNIT_FIELD_LEVEL, 9}};
        fields.insert(fields.end(), extra.begin(), extra.end());
        apply(createdPlayer(fields));
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
    // The feed's new lines this frame.
    std::vector<Line> lines() {
        game::CombatNames names;
        names.player = kPlayer;
        names.unitName = [](const void*, u64 guid) -> std::string_view {
            return guid == kWolf ? std::string_view("Young Wolf") : std::string_view{};
        };
        feed.update(play, store.find(kPlayer), names, now);
        std::vector<Line> out;
        while (const game::Notice* n = feed.lines().next(cursor)) {
            out.push_back({n->kind, n->text.data()});
        }
        return out;
    }
};

} // namespace

TEST_CASE("Mirror timer packets: as vmangos writes them, truncated ones refused") {
    net::MirrorTimerStart start;
    const Bytes breath = startTimer(net::kMirrorTimerBreath, 45000, 60000, -1, false, 0);
    REQUIRE(breath.size() == 21);
    REQUIRE(net::parseMirrorTimerStart(breath, start));
    CHECK(start.type == 1);
    CHECK(start.remainingMs == 45000);
    CHECK(start.durationMs == 60000);
    CHECK(start.scale == -1);
    CHECK_FALSE(start.paused);
    CHECK(start.spellId == 0);
    const Bytes feign = startTimer(net::kMirrorTimerFeignDeath, 300000, 360000, -1, true, 5384);
    REQUIRE(net::parseMirrorTimerStart(feign, start));
    CHECK(start.type == 2);
    CHECK(start.paused);
    CHECK(start.spellId == 5384);
    CHECK(feign == Bytes{2, 0,    0,    0,    0xE0, 0x93, 0x04, 0,    0x40, 0x7E, 0x05,
                         0, 0xFF, 0xFF, 0xFF, 0xFF, 1,    0x08, 0x15, 0,    0});
    checkTruncations(feign, [](std::span<const u8> p) {
        net::MirrorTimerStart m;
        return net::parseMirrorTimerStart(p, m);
    });

    net::MirrorTimerPause pause;
    const Bytes paused = pauseTimer(net::kMirrorTimerFatigue, true);
    CHECK(paused == Bytes{0, 0, 0, 0, 1});
    REQUIRE(net::parseMirrorTimerPause(paused, pause));
    CHECK(pause.type == 0);
    CHECK(pause.paused);
    checkTruncations(paused, [](std::span<const u8> p) {
        net::MirrorTimerPause m;
        return net::parseMirrorTimerPause(p, m);
    });

    u32 type = 9;
    const Bytes stop = stopTimer(net::kMirrorTimerBreath);
    REQUIRE(net::parseMirrorTimerStop(stop, type));
    CHECK(type == 1);
    checkTruncations(stop, [](std::span<const u8> p) {
        u32 t = 0;
        return net::parseMirrorTimerStop(p, t);
    });

    CHECK(std::string_view(game::mirrorTimerName(0)) == "EXHAUSTION");
    CHECK(std::string_view(game::mirrorTimerLabelKey(1)) == "BREATH_LABEL");
    CHECK(std::string_view(game::mirrorTimerLabelKey(2)) == "FEIGNDEATH_LABEL");
    CHECK(game::mirrorTimerName(3) == nullptr); // the server's environmental timer never reaches the client
}

TEST_CASE("Mirror timers: the client's three frames through Gameplay") {
    Harness h;
    const game::MirrorTimers& timers = h.play.mirrorTimers();
    CHECK_FALSE(timers.any());

    // Under water: the breath bar runs out in real time.
    REQUIRE(h.packet(Opcode::SMSG_START_MIRROR_TIMER,
                     startTimer(net::kMirrorTimerBreath, 60000, 60000, -1)) == game::Gameplay::Handled::Yes);
    const game::MirrorTimer& first = timers.frames()[0];
    REQUIRE(first.active);
    CHECK(first.type == net::kMirrorTimerBreath);
    CHECK(first.seconds(h.now) == doctest::Approx(60.0f));
    CHECK(first.seconds(h.now + 15000) == doctest::Approx(45.0f));
    CHECK(first.fill(h.now + 15000) == doctest::Approx(0.75f));
    CHECK(first.seconds(h.now + 90000) == 0.0f); // drowning: the bar stays empty

    // Deep water too: fatigue takes the second frame.
    REQUIRE(h.packet(Opcode::SMSG_START_MIRROR_TIMER,
                     startTimer(net::kMirrorTimerFatigue, 50000, 60000, -1)) == game::Gameplay::Handled::Yes);
    CHECK(timers.frames()[1].active);
    CHECK(timers.frames()[1].type == net::kMirrorTimerFatigue);

    // Back to the surface: the server resends breath refilling ten times as fast; it keeps its frame.
    h.now += 20000;
    REQUIRE(h.packet(Opcode::SMSG_START_MIRROR_TIMER,
                     startTimer(net::kMirrorTimerBreath, 40000, 60000, 10)) == game::Gameplay::Handled::Yes);
    CHECK(timers.frames()[0].type == net::kMirrorTimerBreath);
    CHECK(timers.frames()[0].seconds(h.now + 1000) == doctest::Approx(50.0f));
    CHECK(timers.frames()[0].seconds(h.now + 5000) == doctest::Approx(60.0f)); // full

    // Full again: stopped. Fatigue stays in the second frame's place (MirrorTimer2 hangs from MirrorTimer1).
    REQUIRE(h.packet(Opcode::SMSG_STOP_MIRROR_TIMER, stopTimer(net::kMirrorTimerBreath)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(timers.frames()[0].active);
    CHECK(timers.frames()[1].active);
    CHECK(timers.find(net::kMirrorTimerBreath) == nullptr);

    // A paused timer stays where it is, then goes on from there.
    h.now += 1000;
    const f32 before = timers.frames()[1].seconds(h.now);
    REQUIRE(h.packet(Opcode::SMSG_PAUSE_MIRROR_TIMER, pauseTimer(net::kMirrorTimerFatigue, true)) ==
            game::Gameplay::Handled::Yes);
    CHECK(timers.frames()[1].paused);
    CHECK(timers.frames()[1].seconds(h.now + 30000) == doctest::Approx(before));
    h.now += 30000;
    REQUIRE(h.packet(Opcode::SMSG_PAUSE_MIRROR_TIMER, pauseTimer(net::kMirrorTimerFatigue, false)) ==
            game::Gameplay::Handled::Yes);
    CHECK(timers.frames()[1].seconds(h.now + 2000) == doctest::Approx(before - 2.0f));
    // A start that says paused (vmangos resends instead of pausing) shows a frozen bar.
    REQUIRE(h.packet(Opcode::SMSG_START_MIRROR_TIMER, startTimer(net::kMirrorTimerFatigue, 30000, 60000, -1,
                                                                 true)) == game::Gameplay::Handled::Yes);
    CHECK(timers.frames()[1].seconds(h.now + 9000) == doctest::Approx(30.0f));

    // Feign death takes the first free frame, with its spell for the label.
    REQUIRE(h.packet(Opcode::SMSG_START_MIRROR_TIMER,
                     startTimer(net::kMirrorTimerFeignDeath, 360000, 360000, -1, false, 5384)) ==
            game::Gameplay::Handled::Yes);
    CHECK(timers.frames()[0].type == net::kMirrorTimerFeignDeath);
    CHECK(timers.frames()[0].spellId == 5384);
    REQUIRE(h.packet(Opcode::SMSG_START_MIRROR_TIMER, startTimer(net::kMirrorTimerBreath, 1000, 60000, -1)) ==
            game::Gameplay::Handled::Yes);
    CHECK(timers.frames()[2].type == net::kMirrorTimerBreath);

    // A type the client has no bar for, a stop of a timer not shown: nothing. Malformed: refused, nothing
    // changes.
    REQUIRE(h.packet(Opcode::SMSG_START_MIRROR_TIMER, startTimer(3, 1000, 1000, -1)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_STOP_MIRROR_TIMER, stopTimer(3)) == game::Gameplay::Handled::Yes);
    CHECK(h.packet(Opcode::SMSG_START_MIRROR_TIMER, Bytes{1, 0, 0}) == game::Gameplay::Handled::Malformed);
    CHECK(h.packet(Opcode::SMSG_STOP_MIRROR_TIMER, Bytes{2, 0}) == game::Gameplay::Handled::Malformed);
    CHECK(h.packet(Opcode::SMSG_PAUSE_MIRROR_TIMER, Bytes{0, 0, 0, 0}) == game::Gameplay::Handled::Malformed);
    CHECK(timers.frames()[0].active);
    CHECK(timers.frames()[1].active);
    CHECK(timers.frames()[2].active);

    // A loading screen (PLAYER_ENTERING_WORLD) hides them all.
    h.play.enterWorld(kPlayer, 1, false, h.now);
    CHECK_FALSE(timers.any());
}

TEST_CASE("Notices: where the default 1.12 interface shows each kind, and its colour") {
    CHECK(game::noticePlace(NoticeKind::System) == NoticePlace::General);
    CHECK(game::noticePlace(NoticeKind::Loot) == NoticePlace::General);
    CHECK(game::noticePlace(NoticeKind::Skill) == NoticePlace::General);
    CHECK(game::noticePlace(NoticeKind::Money) == NoticePlace::General); // ChatTypeGroup LOOT
    CHECK(game::noticePlace(NoticeKind::XpGain) == NoticePlace::CombatLog);
    CHECK(game::noticePlace(NoticeKind::FactionChange) == NoticePlace::CombatLog);
    CHECK(game::noticePlace(NoticeKind::Info) == NoticePlace::ErrorsFrame);
    CHECK(game::noticePlace(NoticeKind::Error) == NoticePlace::ErrorsFrame);
    const auto rgb = [](NoticeKind kind) {
        const game::NoticeColor c = game::noticeColor(kind);
        return (u32{c.r} << 16) | (u32{c.g} << 8) | c.b;
    };
    CHECK(rgb(NoticeKind::System) == 0xFFFF00);
    CHECK(rgb(NoticeKind::Loot) == 0x00AA00);
    CHECK(rgb(NoticeKind::Skill) == 0x5555FF);
    CHECK(rgb(NoticeKind::Money) == 0xFFFF00);
    CHECK(rgb(NoticeKind::XpGain) == 0x6F6FFF);
    CHECK(rgb(NoticeKind::FactionChange) == 0x8080FF);
    CHECK(rgb(NoticeKind::Info) == 0xFFFF00);
    CHECK(rgb(NoticeKind::Error) == 0xFF1A1A);

    const EnglishTexts texts;
    char money[64];
    CHECK(game::moneyText(money, 7) == "7 Copper");
    CHECK(game::moneyText(money, 150) == "1 Silver, 50 Copper");
    CHECK(game::moneyText(money, 10203) == "1 Gold, 2 Silver, 3 Copper");
    CHECK(game::moneyText(money, 50000) == "5 Gold");
    CHECK(game::moneyText(money, 0) == "0 Copper");
    char tiny[8];
    CHECK(game::moneyText(tiny, 10203).size() == 7); // cut to fit, NUL-terminated
}

TEST_CASE("Notices: loot, money and experience go to the chat and the combat log as the client words them") {
    const EnglishTexts texts;
    Harness h;
    CHECK(h.lines().empty());

    // Looted twice: the line waits for the item's name (the client's item cache), then names it as a link.
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kPlayer, kLinenCloth, 2)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_MONEY_NOTIFY, u32Body(10203)) == game::Gameplay::Handled::Yes);
    h.apply(values(kPlayer, {{uf::PLAYER_FIELD_COINAGE, 10203}}));
    CHECK(h.lines().empty()); // the money waits behind the loot: the order holds
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(kLinenCloth, "Linen Cloth", 1)) ==
            game::Gameplay::Handled::Yes);
    std::vector<Line> lines = h.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == Line{NoticeKind::Loot, "You receive loot: |cffffffff|Hitem:2589:0:0:0|h[Linen Cloth]|h|rx2."});
    CHECK(lines[1] == Line{NoticeKind::Money, "Loot share: 1 Gold, 2 Silver, 3 Copper."});

    // Bought (from an NPC), created, a quest reward kept out of the chat, an uncommon item.
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(kBread, "Tough Hunk of Bread", 1)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(kBracers, "Wolfskin Bracers", 2)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kPlayer, kBread, 5, true)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kPlayer, kBread, 1, false, true)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kPlayer, kBracers, 1, true, false, false)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kPlayer, kBracers, 1)) ==
            game::Gameplay::Handled::Yes);
    lines = h.lines();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == Line{NoticeKind::Loot, "You receive item: |cffffffff|Hitem:4540:0:0:0|h[Tough Hunk of Bread]|h|rx5."});
    CHECK(lines[1] == Line{NoticeKind::Loot, "You create: |cffffffff|Hitem:4540:0:0:0|h[Tough Hunk of Bread]|h|r."});
    CHECK(lines[2] == Line{NoticeKind::Loot, "You receive loot: |cff1eff00|Hitem:2970:0:0:0|h[Wolfskin Bracers]|h|r."});

    // A group member's loot (broadcast): their name and the item.
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kMember, kLinenCloth, 1)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.lines().empty()); // waits for the name
    Bytes name;
    net::ByteWriter(name).u64v(kMember).cstring("Aeryn").cstring("").u32v(4).u32v(1).u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_NAME_QUERY_RESPONSE, name) == game::Gameplay::Handled::Yes);
    lines = h.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == Line{NoticeKind::Loot, "Aeryn receives loot: |cffffffff|Hitem:2589:0:0:0|h[Linen Cloth]|h|r."});

    // Alone, the loot window's money: a coinage change after the money was taken.
    Bytes loot;
    net::ByteWriter(loot).u64v(kWolf).u8v(1).u32v(23).u8v(0);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_RESPONSE, loot) == game::Gameplay::Handled::Yes);
    h.play.lootMoney();
    REQUIRE(h.packet(Opcode::SMSG_LOOT_CLEAR_MONEY, {}) == game::Gameplay::Handled::Yes);
    h.apply(values(kPlayer, {{uf::PLAYER_FIELD_COINAGE, 10226}}));
    lines = h.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == Line{NoticeKind::Money, "You loot 23 Copper"});

    // Experience (WoW.exe 0x637EC0): a kill of a unit the client does not know is unnamed; known, it is named
    // (its rested bonus only with Exhaustion.dbc: the test with the real tables); a quest's has no victim;
    // the player as victim is experience lost.
    Bytes xp;
    net::ByteWriter(xp).u64v(kWolf).u32v(90).u8v(0).u32v(45).f32v(1.0f);
    REQUIRE(h.packet(Opcode::SMSG_LOG_XPGAIN, xp) == game::Gameplay::Handled::Yes);
    h.apply(created(kWolf, game::ObjectType::Unit, {{uf::UNIT_FIELD_HEALTH, 0}}));
    REQUIRE(h.packet(Opcode::SMSG_LOG_XPGAIN, xp) == game::Gameplay::Handled::Yes);
    Bytes questXp;
    net::ByteWriter(questXp).u64v(0).u32v(170).u8v(1);
    REQUIRE(h.packet(Opcode::SMSG_LOG_XPGAIN, questXp) == game::Gameplay::Handled::Yes);
    Bytes lost;
    net::ByteWriter(lost).u64v(kPlayer).u32v(static_cast<u32>(-30)).u8v(1);
    REQUIRE(h.packet(Opcode::SMSG_LOG_XPGAIN, lost) == game::Gameplay::Handled::Yes);
    lines = h.lines();
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == Line{NoticeKind::XpGain, "You gain 90 experience."});
    CHECK(lines[1] == Line{NoticeKind::XpGain, "Young Wolf slain: +90 experience."});
    CHECK(lines[2] == Line{NoticeKind::XpGain, "You gain 170 experience."});
    CHECK(lines[3] == Line{NoticeKind::XpGain, "You lose 30 experience."});

    // An item that never gets a name still gets its line after a while.
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kPlayer, 999999, 1)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.lines().empty());
    h.now += game::NoticeFeed::kNameWaitMs;
    lines = h.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == Line{NoticeKind::Loot, "You receive loot: |cffffffff|Hitem:999999:0:0:0|h[...]|h|r."});

    // UIErrorsFrame events (errors, quest progress) and the rest are not the feed's: nothing here.
    Bytes failed;
    net::ByteWriter(failed).u64v(kWolf).u8v(0).u8v(4);
    REQUIRE(h.packet(Opcode::SMSG_LOOT_RESPONSE, failed) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ATTACKSWING_NOTINRANGE, {}) == game::Gameplay::Handled::Yes);
    CHECK(h.lines().empty());
}

TEST_CASE("Notices: a level-up and quests are system lines of the chat") {
    const EnglishTexts texts;
    Harness h;

    // ChatFrame.lua PLAYER_LEVEL_UP: level, health and mana, the talent point of level 10, each stat that
    // rose.
    Bytes level;
    net::ByteWriter l(level);
    l.u32v(10).u32v(17).u32v(22).u32v(0).u32v(0).u32v(0).u32v(0);
    l.u32v(1).u32v(0).u32v(2).u32v(0).u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_LEVELUP_INFO, level) == game::Gameplay::Handled::Yes);
    h.apply(values(kPlayer, {{uf::UNIT_FIELD_LEVEL, 10}})); // the field comes after the packet
    std::vector<Line> lines = h.lines();
    REQUIRE(lines.size() == 6);
    CHECK(lines[0] == Line{NoticeKind::System, "Now level 10!"});
    CHECK(lines[1] == Line{NoticeKind::System, "Health +17, mana +22."});
    CHECK(lines[2] == Line{NoticeKind::System, "1 new talent point."});
    CHECK(lines[3] == Line{NoticeKind::System, "Your Strength increases by 1."});
    CHECK(lines[4] == Line{NoticeKind::System, "Your Stamina increases by 2."});
    CHECK(lines[5] == Line{NoticeKind::System, "Your Spirit increases by 1."});

    // Five levels at once (.levelup 5), no mana: five talent points.
    Bytes jump;
    net::ByteWriter j(jump);
    j.u32v(15).u32v(80);
    for (u32 i = 0; i < 10; ++i) {
        j.u32v(0);
    }
    REQUIRE(h.packet(Opcode::SMSG_LEVELUP_INFO, jump) == game::Gameplay::Handled::Yes);
    lines = h.lines();
    REQUIRE(lines.size() == 3);
    CHECK(lines[1] == Line{NoticeKind::System, "Health +80."});
    CHECK(lines[2] == Line{NoticeKind::System, "5 new talent points."});

    // A quest in the log: accepted, named once its template arrives.
    h.apply(values(kPlayer, {{uf::PLAYER_QUEST_LOG_1_1, kKoboldCamp}}));
    CHECK(h.lines().empty());
    REQUIRE(h.packet(Opcode::SMSG_QUEST_QUERY_RESPONSE, questQuery(kKoboldCamp, "Kobold Camp Cleanup")) ==
            game::Gameplay::Handled::Yes);
    lines = h.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == Line{NoticeKind::System, "Quest accepted: Kobold Camp Cleanup"});

    // Turned in: completed, its experience, money and items (the chosen one comes as a push kept out of the
    // chat).
    Bytes complete;
    net::ByteWriter(complete).u32v(kKoboldCamp).u32v(3).u32v(170).u32v(150).u32v(1).u32v(kBracers).u32v(1);
    REQUIRE(h.packet(Opcode::SMSG_QUESTGIVER_QUEST_COMPLETE, complete) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kPlayer, kBracers, 1, true, false, false)) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.lines().empty()); // the reward's name first
    REQUIRE(h.packet(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, itemQuery(kBracers, "Wolfskin Bracers", 2)) ==
            game::Gameplay::Handled::Yes);
    lines = h.lines();
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == Line{NoticeKind::System, "Kobold Camp Cleanup completed."});
    CHECK(lines[1] == Line{NoticeKind::System, "Experience gained: 170."});
    CHECK(lines[2] == Line{NoticeKind::System, "Received 1 Silver, 50 Copper."});
    CHECK(lines[3] == Line{NoticeKind::System, "Received item: |cff1eff00|Hitem:2970:0:0:0|h[Wolfskin Bracers]|h|r."});

    // A new character starts over: nothing old comes back.
    REQUIRE(h.packet(Opcode::SMSG_LEVELUP_INFO, level) == game::Gameplay::Handled::Yes);
    h.feed.restart(h.play);
    CHECK(h.lines().empty());
}

TEST_CASE("Notices: phase 12, the instance packets as WoW.exe words them") {
    const EnglishTexts texts;
    Harness h;
    game::AreaData areas;
    areas.set({}, {}, {{36, "DeadminesInstance", "Deadmines", 1, 0, 0}, {409, "MoltenCore", "Molten Core", 2, 0, 0}});
    h.play.travel().setData(&areas, nullptr);
    CHECK(h.lines().empty());
    const auto three = [](u32 a, u32 b, u32 c) {
        Bytes out;
        net::ByteWriter(out).u32v(a).u32v(b).u32v(c);
        return out;
    };
    const auto two = [](u32 a, u32 b) {
        Bytes out;
        net::ByteWriter(out).u32v(a).u32v(b);
        return out;
    };
    const auto handled = [&](Opcode op, const Bytes& body) {
        REQUIRE(h.packet(op, body) == game::Gameplay::Handled::Yes);
    };
    handled(Opcode::SMSG_INSTANCE_SAVE_CREATED, u32Body(0));
    // Raid warnings: whole hours and minutes (WoW.exe divides), singular for one; the welcome in days, hours, minutes.
    handled(Opcode::SMSG_RAID_INSTANCE_MESSAGE, three(net::kRaidInstanceWarningHours, 409, 3599));
    handled(Opcode::SMSG_RAID_INSTANCE_MESSAGE, three(net::kRaidInstanceWarningHours, 409, 3600));
    handled(Opcode::SMSG_RAID_INSTANCE_MESSAGE, three(net::kRaidInstanceWarningMin, 409, 900));
    handled(Opcode::SMSG_RAID_INSTANCE_MESSAGE, three(net::kRaidInstanceWarningMinSoon, 409, 60));
    const u32 welcome = 6 * 86400 + 5 * 3600 + 4 * 60 + 3;
    handled(Opcode::SMSG_RAID_INSTANCE_MESSAGE, three(net::kRaidInstanceWelcome, 409, welcome));
    handled(Opcode::SMSG_RAID_INSTANCE_MESSAGE, three(9, 409, 60)); // unknown type: nothing
    handled(Opcode::SMSG_INSTANCE_RESET, u32Body(36));
    handled(Opcode::SMSG_INSTANCE_RESET, u32Body(999)); // a map the client does not know: its number
    handled(Opcode::SMSG_INSTANCE_RESET_FAILED, two(net::kInstanceResetFailedGeneral, 36));
    handled(Opcode::SMSG_INSTANCE_RESET_FAILED, two(net::kInstanceResetFailedOffline, 36));
    handled(Opcode::SMSG_INSTANCE_RESET_FAILED, two(net::kInstanceResetFailedZoning, 36));
    handled(Opcode::SMSG_INSTANCE_RESET_FAILED, two(3, 36)); // "silently"
    handled(Opcode::SMSG_RAID_GROUP_ONLY, two(0, net::kRaidGroupRequired));
    handled(Opcode::SMSG_RAID_GROUP_ONLY, two(60000, net::kRaidGroupRequired)); // the boot: a popup, no line
    handled(Opcode::SMSG_RAID_GROUP_ONLY, two(0, net::kRaidGroupFull));
    // /raidinfo with nothing saved: the chat says so (only for the slash command, not the login's request).
    handled(Opcode::SMSG_RAID_INSTANCE_INFO, u32Body(0));
    h.play.travel().requestRaidInfo(true);
    handled(Opcode::SMSG_RAID_INSTANCE_INFO, u32Body(0));
    const std::vector<Line> lines = h.lines();
    const std::vector<Line> expected = {
        {NoticeKind::System, "Instance saved"},
        {NoticeKind::System, "Molten Core resets in 0 hours."},
        {NoticeKind::System, "Molten Core resets in 1 hour."},
        {NoticeKind::System, "Molten Core resets in 15 minutes!"},
        {NoticeKind::System, "Molten Core resets in 1 minute: leave now."},
        {NoticeKind::System, "Molten Core: resets in 6d 5h 4m."},
        {NoticeKind::System, "Deadmines has been reset."},
        {NoticeKind::System, "999 has been reset."},
        {NoticeKind::System, "Deadmines not reset: players inside."},
        {NoticeKind::System, "Deadmines not reset: players offline."},
        {NoticeKind::System,
         "Deadmines not reset: players zoning in."},
        {NoticeKind::Error, "Raid groups only"},
        {NoticeKind::Error, "The instance is full"},
        {NoticeKind::System, "No saved instances"},
    };
    CHECK(lines == expected);
}

TEST_CASE("Notices: the events behind them") {
    Harness h;
    u32 cursor = h.play.events().end();
    const auto events = [&]() {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = h.play.events().next(cursor)) {
            out.push_back(*e);
        }
        return out;
    };
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kPlayer, kBread, 1, true, false, false)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kMember, kBread, 3, false, true)) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_ITEM_PUSH_RESULT, itemPush(kMember, kBread, 3, false, false, false)) ==
            game::Gameplay::Handled::Yes); // another's, not for the chat: nothing
    REQUIRE(h.packet(Opcode::SMSG_LOOT_MONEY_NOTIFY, u32Body(40)) == game::Gameplay::Handled::Yes);
    Bytes level;
    net::ByteWriter l(level);
    for (u32 i = 0; i < 12; ++i) {
        l.u32v(i == 0 ? 10 : 0);
    }
    REQUIRE(h.packet(Opcode::SMSG_LEVELUP_INFO, level) == game::Gameplay::Handled::Yes);
    const std::vector<game::GameEvent> e = events();
    REQUIRE(e.size() == 4);
    CHECK(e[0].kind == GameEventKind::ItemReceived);
    CHECK(e[0].other == 1); // showInChat off
    CHECK(e[1].kind == GameEventKind::MemberItemReceived);
    CHECK(e[1].guid == kMember);
    CHECK(e[1].id == kBread);
    CHECK(e[1].value[0] == 3);
    CHECK(e[1].value[2] == 1);
    CHECK(e[2].kind == GameEventKind::MoneyLooted);
    CHECK(e[2].value[0] == 40);
    CHECK(e[2].value[1] == 1); // a group's share
    CHECK(e[3].kind == GameEventKind::LevelUp);
    CHECK(e[3].id == 9); // the level before
}

TEST_CASE("Notices: skill-ups, learned spells and reputation (the real tables)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    pack::PackReader db;
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    game::GameData data;
    REQUIRE(data.init(db));
    game::AbilityData abilities;
    REQUIRE(abilities.init(db));
    const EnglishTexts texts;
    constexpr u32 kSwords = 43; // SkillLine.dbc
    const u16 skill = uf::PLAYER_SKILL_INFO_1_1;
    Harness h(&data, &abilities, {{skill, kSwords}, {static_cast<u16>(skill + 1), 10u | (45u << 16)}});
    CHECK(h.lines().empty()); // the skills as they are: where it starts

    // Used: up by one. A level raising the maximum (alone, or with the value of a skill that follows it):
    // nothing.
    h.apply(values(kPlayer, {{static_cast<u16>(skill + 1), 11u | (45u << 16)}}));
    std::vector<Line> lines = h.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == Line{NoticeKind::Skill, "Skill Swords now 11."});
    h.apply(values(kPlayer, {{static_cast<u16>(skill + 1), 11u | (50u << 16)}}));
    h.apply(values(kPlayer, {{static_cast<u16>(skill + 1), 50u | (55u << 16)}}));
    CHECK(h.lines().empty());

    // Learned: a spell, an ability (passive), a recipe; nothing for one the client does not show.
    for (const u32 spell : {133u, 674u, 3275u}) {
        REQUIRE(h.packet(Opcode::SMSG_LEARNED_SPELL, u32Body(spell)) == game::Gameplay::Handled::Yes);
    }
    lines = h.lines();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == Line{NoticeKind::System, "New spell: Fireball."});
    CHECK(lines[1] == Line{NoticeKind::System, "New ability: Dual Wield."});
    CHECK(lines[2] == Line{NoticeKind::System, "New recipe: Linen Bandage."});

    // Reputation with Stormwind: its change, and the new rank when it crosses one.
    i32 listId = -1;
    for (u32 i = 0; i < 64 && listId < 0; ++i) {
        const game::FactionInfo* f = abilities.factionByList(i);
        listId = f && f->name == "Stormwind" ? static_cast<i32>(i) : -1;
    }
    REQUIRE(listId >= 0);
    const game::FactionInfo& stormwind = *abilities.factionByList(static_cast<u32>(listId));
    const i32 base = stormwind.baseReputation(1, 1);
    Bytes init;
    net::ByteWriter w(init);
    w.u32v(64);
    for (u32 i = 0; i < 64; ++i) {
        w.u8v(i == static_cast<u32>(listId) ? net::kFactionVisible : 0).u32v(0);
    }
    REQUIRE(h.packet(Opcode::SMSG_INITIALIZE_FACTIONS, init) == game::Gameplay::Handled::Yes);
    Bytes standing;
    net::ByteWriter(standing).u32v(1).u32v(static_cast<u32>(listId)).u32v(75);
    REQUIRE(h.packet(Opcode::SMSG_SET_FACTION_STANDING, standing) == game::Gameplay::Handled::Yes);
    lines = h.lines();
    REQUIRE(lines.size() >= 1);
    CHECK(lines[0] == Line{NoticeKind::FactionChange, "Reputation with Stormwind +75."});
    CHECK(lines.size() == (game::reputationRank(base) != game::reputationRank(base + 75) ? 2u : 1u));
    // Up to the next rank's first point.
    const u32 rank = game::reputationRank(base + 75);
    const i32 next = game::reputationRankStart(rank + 1) - base;
    Bytes up;
    net::ByteWriter(up).u32v(1).u32v(static_cast<u32>(listId)).u32v(static_cast<u32>(next));
    REQUIRE(h.packet(Opcode::SMSG_SET_FACTION_STANDING, up) == game::Gameplay::Handled::Yes);
    lines = h.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].kind == NoticeKind::FactionChange);
    CHECK(lines[1].text ==
          "You are now " + std::string(game::reputationRankName(rank + 1)) + " with Stormwind.");

    // Experience by the player's rest state (PLAYER_BYTES_2 byte 3) and Exhaustion.dbc: rested (factor 2)
    // names its bonus with the table's name; normal (factor 1) writes the plain line, even with a bonus.
    h.apply(created(kWolf, game::ObjectType::Unit, {{uf::UNIT_FIELD_HEALTH, 0}}));
    Bytes xp;
    net::ByteWriter(xp).u64v(kWolf).u32v(90).u8v(0).u32v(45).f32v(1.0f);
    h.apply(values(kPlayer, {{uf::PLAYER_BYTES_2, 1u << 24}}));
    REQUIRE(h.packet(Opcode::SMSG_LOG_XPGAIN, xp) == game::Gameplay::Handled::Yes);
    lines = h.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] ==
          Line{NoticeKind::XpGain, "Young Wolf slain: +90 experience (+45, Rested bonus)."});
    h.apply(values(kPlayer, {{uf::PLAYER_BYTES_2, 2u << 24}}));
    REQUIRE(h.packet(Opcode::SMSG_LOG_XPGAIN, xp) == game::Gameplay::Handled::Yes);
    lines = h.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == Line{NoticeKind::XpGain, "Young Wolf slain: +90 experience."});
}

TEST_CASE("Notices and mirror timers: every key exists in the client's table, with the stubs' specifiers") {
    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR/ui.vdpk not available: skipping");
        return;
    }
    for (const auto& [key, english] : kEnglish) {
        if (key.rfind("VD_", 0) != 0) {
            CHECK_MESSAGE(text::has(text::Key(key)), key);
            CHECK_MESSAGE(specifiers(text::get(text::Key(key))) == specifiers(english), key);
        }
    }
    CHECK_FALSE(text::has("FEIGNDEATH_LABEL")); // 1.12 has none: the bar names its spell
    text::unload();
}
