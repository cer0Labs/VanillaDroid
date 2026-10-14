// Phase-8 combat log lines (game/CombatText) with the client's texts, the aura watch, skills and combo points
// from the update fields (game/Progress).
#include "TestData.h"
#include "core/Hash.h"
#include "core/Text.h"
#include "game/Combat.h"
#include "game/CombatText.h"
#include "game/Entities.h"
#include "game/Progress.h"
#include "net/ByteBuffer.h"
#include "net/UpdateFields.gen.h"
#include "pack/UiFormat.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;
namespace uf = net::uf;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kMe = 0x21;
constexpr u64 kWolf = 0xF130000000000456ull;
constexpr u64 kKobold = 0xF130000000000789ull;
constexpr u64 kImp = 0xF140000000000042ull;

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

Bytes values(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

// Names as the HUD gives them.
game::CombatNames names() {
    game::CombatNames n;
    n.player = kMe;
    n.pet = kImp;
    n.unitName = [](const void*, u64 guid) -> std::string_view {
        switch (guid) {
        case kMe: return "Arthel";
        case kWolf: return "Young Wolf";
        case kImp: return "Zalgaz";
        default: return {};
        }
    };
    n.spellName = [](const void*, u32 spell) -> std::string_view {
        switch (spell) {
        case 133: return "Fireball";
        case 774: return "Rejuvenation";
        case 772: return "Rend";
        case 168: return "Frost Armor";
        case 3110: return "Firebolt";
        default: return {};
        }
    };
    return n;
}

std::string line(const game::CombatEvent& e, game::CombatLineKind* kind = nullptr) {
    char out[200];
    game::formatCombatEvent(out, sizeof(out), e, names(), kind);
    return out;
}

game::CombatEvent event(game::CombatEvent::Kind kind, u64 attacker, u64 victim, u32 spell, i32 amount,
                        u8 school = 0, u8 flags = 0) {
    game::CombatEvent e;
    e.kind = kind;
    e.attacker = attacker;
    e.victim = victim;
    e.spellId = spell;
    e.amount = amount;
    e.school = school;
    e.flags = flags;
    return e;
}

// A strings table blob as the baker writes it (pack::UiStringsHeader + UiString sorted by hash + texts).
std::vector<u8> buildTable(std::vector<std::pair<std::string, std::string>> texts) {
    std::sort(texts.begin(), texts.end(),
              [](const auto& a, const auto& b) { return stringHash(a.first) < stringHash(b.first); });
    pack::UiStringsHeader header{};
    header.count = static_cast<u32>(texts.size());
    header.textOffset = static_cast<u32>(sizeof(header) + texts.size() * sizeof(pack::UiString));
    std::vector<pack::UiString> entries;
    std::string block;
    for (const auto& [key, text] : texts) {
        entries.push_back({stringHash(key), static_cast<u32>(block.size()), static_cast<u32>(text.size())});
        block += text;
        block.push_back('\0');
    }
    header.textSize = static_cast<u32>(block.size());
    std::vector<u8> blob(header.textOffset + block.size());
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), entries.data(), entries.size() * sizeof(pack::UiString));
    std::memcpy(blob.data() + header.textOffset, block.data(), block.size());
    return blob;
}

// Stub texts for the keys these tests read, with the formats of the enUS client's GlobalStrings.lua and
// locale/enUS/combat.lang (the game shows the client's own); loaded for the test case's life.
struct EnglishTexts {
    std::vector<u8> blob = buildTable({
        {"COMBATHITSELFOTHER", "You hit %s for %d."},
        {"COMBATHITOTHERSELF", "%s hits you for %d."},
        {"COMBATHITOTHEROTHER", "%s hits %s for %d."},
        {"COMBATHITCRITOTHERSELF", "%s crits you for %d."},
        {"COMBATHITSCHOOLSELFOTHER", "Swing on %s: %d %s."},
        {"SPELLLOGSCHOOLSELFOTHER", "Your %s on %s: %d %s."},
        {"SPELLLOGCRITSCHOOLSELFOTHER", "Your %s on %s: %d %s (crit)."},
        {"SPELLLOGSCHOOLOTHEROTHER", "%s's %s on %s: %d %s."},
        {"SPELLLOGSCHOOLOTHERSELF", "%s's %s on you: %d %s."},
        {"SPELLLOGSELFSELF", "Your %s on you: %d."},
        {"PERIODICAURADAMAGESELFOTHER", "%s takes %d %s from your %s."},
        {"PERIODICAURADAMAGEOTHERSELF", "Hurt %d %s by %s's %s."},
        {"VSENVIRONMENTALDAMAGE_FALLING_SELF", "Fall: -%d health."},
        {"VSENVIRONMENTALDAMAGE_LAVA_OTHER", "%s in lava: -%d health."},
        {"GLANCING_TRAILER", " (glancing)"},
        {"ABSORB_TRAILER", " (%d absorbed)"},
        {"RESIST_TRAILER", " (%d resisted)"},
        {"MISSEDSELFOTHER", "You miss %s."},
        {"MISSEDOTHERSELF", "%s misses you."},
        {"VSDODGESELFOTHER", "You attack. %s dodges."},
        {"VSPARRYOTHERSELF", "%s attacks. You parry."},
        {"VSBLOCKOTHEROTHER", "%s attacks. %s blocks."},
        {"SPELLRESISTSELFOTHER", "Your %s: %s resists."},
        {"SPELLIMMUNESELFOTHER", "Your %s: %s immune."},
        {"SPELLLOGABSORBOTHERSELF", "You absorb %s's %s."},
        {"SPELLMISSOTHEROTHER", "%s's %s missed %s."},
        {"SPELLBLOCKEDOTHEROTHER", "%s's %s: %s blocks."},
        {"SPELLINTERRUPTSELFOTHER", "You interrupt %s's %s."},
        {"HEALEDSELFSELF", "Your %s on you: +%d."},
        {"HEALEDCRITSELFOTHER", "Your %s on %s: +%d (crit)."},
        {"HEALEDOTHERSELF", "%s's %s on you: +%d."},
        {"PERIODICAURAHEALSELFSELF", "+%d health from %s."},
        {"PERIODICAURAHEALOTHEROTHER", "%s healed %d by %s's %s."},
        {"POWERGAINSELFSELF", "You gain %d %s from %s."},
        {"SPELLPOWERDRAINSELFOTHER", "Your %s: -%d %s on %s."},
        {"AURAADDEDSELFHELPFUL", "You gain %s."},
        {"AURAADDEDOTHERHARMFUL", "%s is afflicted by %s."},
        {"AURAREMOVEDSELF", "%s fades from you."},
        {"AURAREMOVEDOTHER", "%s fades from %s."},
        {"SELFKILLOTHER", "You have slain %s!"},
        {"ERR_KILLED_BY_S", "%s has slain you."},
        {"UNITDIESSELF", "You die."},
        {"PARTYKILLOTHER", "%s is slain by %s!"},
        {"COMBATLOG_XPGAIN_FIRSTPERSON", "%s slain: +%d experience."},
        {"COMBATLOG_XPGAIN_EXHAUSTION1", "%s slain: +%d experience (%s, %s bonus)."},
        {"COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED", "You gain %d experience."},
        {"COMBATLOG_XPGAIN_EXHAUSTION1_GROUP", "%s slain: +%d experience (%s, %s bonus, +%d group)."},
        {"COMBATLOG_XPGAIN_EXHAUSTION1_RAID", "%s slain: +%d experience (%s, %s bonus, -%d raid)."},
        {"COMBATLOG_XPGAIN_FIRSTPERSON_RAID", "%s slain: +%d experience (-%d raid)."},
        {"COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED_GROUP", "+%d experience (+%d group)."},
        {"COMBATLOG_XPLOSS_FIRSTPERSON_UNNAMED", "You lose %d experience."},
        {"XP", "XP"},
        {"UNKNOWNOBJECT", "Unknown"},
        {"UNKNOWN", "Unknown"},
        {"SPELL_SCHOOL0_CAP", "Physical"},
        {"SPELL_SCHOOL2_CAP", "Fire"},
        {"SPELL_SCHOOL6_CAP", "Arcane"},
        {"MANA", "Mana"},
    });
    EnglishTexts() { text::setTable(blob); }
    ~EnglishTexts() { text::unload(); }
};

} // namespace

TEST_CASE("Combat log lines: the client's texts, by who did it to whom") {
    const EnglishTexts texts;
    using K = game::CombatEvent::Kind;
    using C = game::CombatEvent;
    game::CombatLineKind kind = game::CombatLineKind::Other;
    // Spells (SPELLLOG*): the school named unless physical.
    CHECK(line(event(K::Damage, kMe, kWolf, 133, 25, 2), &kind) == "Your Fireball on Young Wolf: 25 Fire.");
    CHECK(kind == game::CombatLineKind::Outgoing);
    CHECK(line(event(K::Damage, kMe, kWolf, 133, 51, 2, C::kCrit)) ==
          "Your Fireball on Young Wolf: 51 Fire (crit).");
    CHECK(line(event(K::Damage, kMe, kMe, 772, 5)) == "Your Rend on you: 5.");
    CHECK(line(event(K::Damage, kImp, kWolf, 3110, 12, 2), &kind) == "Zalgaz's Firebolt on Young Wolf: 12 Fire.");
    CHECK(kind == game::CombatLineKind::Outgoing); // the pet's
    // Swings (COMBATHIT*).
    CHECK(line(event(K::Damage, kMe, kWolf, 0, 7)) == "You hit Young Wolf for 7.");
    CHECK(line(event(K::Damage, kMe, kWolf, 0, 7, 2)) == "Swing on Young Wolf: 7 Fire.");
    CHECK(line(event(K::Damage, kWolf, kMe, 0, 3), &kind) == "Young Wolf hits you for 3.");
    CHECK(kind == game::CombatLineKind::Incoming);
    CHECK(line(event(K::Damage, kWolf, kMe, 0, 6, 0, C::kCrit)) == "Young Wolf crits you for 6.");
    CHECK(line(event(K::Damage, kKobold, kWolf, 0, 2), &kind) == "Unknown hits Young Wolf for 2.");
    CHECK(kind == game::CombatLineKind::Other);
    // What the hit lost, after the sentence.
    game::CombatEvent glancing = event(K::Damage, kMe, kWolf, 0, 4, 0, C::kGlancing);
    glancing.absorbed = 2;
    CHECK(line(glancing) == "You hit Young Wolf for 4. (glancing) (2 absorbed)");
    game::CombatEvent resisted = event(K::Damage, kWolf, kMe, 133, 20, 2);
    resisted.resisted = 9;
    CHECK(line(resisted) == "Young Wolf's Fireball on you: 20 Fire. (9 resisted)");
    // Over time, the school always named.
    CHECK(line(event(K::Damage, kMe, kWolf, 772, 5, 0, C::kPeriodic)) == "Young Wolf takes 5 Physical from your Rend.");
    CHECK(line(event(K::Damage, kWolf, kMe, 133, 3, 2, C::kPeriodic)) == "Hurt 3 Fire by Young Wolf's Fireball.");
    // The environment.
    CHECK(line(event(K::Damage, 0, kMe, 2, 40, 0, C::kEnvironment)) == "Fall: -40 health.");
    CHECK(line(event(K::Damage, 0, kWolf, 3, 50, 0, C::kEnvironment)) == "Young Wolf in lava: -50 health.");

    // Misses and avoidances: swings (MISSED*, VS*) and spells (SPELL*).
    CHECK(line(event(K::Miss, kMe, kWolf, 0, 0), &kind) == "You miss Young Wolf.");
    CHECK(kind == game::CombatLineKind::Miss);
    CHECK(line(event(K::Miss, kWolf, kMe, 0, 0)) == "Young Wolf misses you.");
    CHECK(line(event(K::Dodge, kMe, kWolf, 0, 0)) == "You attack. Young Wolf dodges.");
    CHECK(line(event(K::Parry, kWolf, kMe, 0, 0)) == "Young Wolf attacks. You parry.");
    CHECK(line(event(K::Block, kImp, kWolf, 0, 0)) == "Zalgaz attacks. Young Wolf blocks.");
    CHECK(line(event(K::Resist, kMe, kWolf, 133, 0)) == "Your Fireball: Young Wolf resists.");
    CHECK(line(event(K::Immune, kMe, kWolf, 133, 0)) == "Your Fireball: Young Wolf immune.");
    CHECK(line(event(K::Absorb, kWolf, kMe, 133, 0)) == "You absorb Young Wolf's Fireball.");
    CHECK(line(event(K::Miss, kImp, kWolf, 3110, 0)) == "Zalgaz's Firebolt missed Young Wolf.");
    CHECK(line(event(K::Block, kMe, kMe, 133, 0)) == "Arthel's Fireball: Arthel blocks."); // no SELFSELF
    CHECK(line(event(K::Interrupt, kMe, kWolf, 133, 0)) == "You interrupt Young Wolf's Fireball.");

    // Heals and power.
    CHECK(line(event(K::Heal, kMe, kMe, 774, 12), &kind) == "Your Rejuvenation on you: +12.");
    CHECK(kind == game::CombatLineKind::Heal);
    CHECK(line(event(K::Heal, kWolf, kMe, 774, 8)) == "Young Wolf's Rejuvenation on you: +8.");
    CHECK(line(event(K::Heal, kMe, kMe, 774, 4, 0, C::kPeriodic)) == "+4 health from Rejuvenation.");
    CHECK(line(event(K::Heal, kImp, kWolf, 774, 4, 0, C::kPeriodic)) ==
          "Young Wolf healed 4 by Zalgaz's Rejuvenation.");
    CHECK(line(event(K::Heal, kMe, kWolf, 774, 9, 0, C::kCrit)) == "Your Rejuvenation on Young Wolf: +9 (crit).");
    CHECK(line(event(K::Energize, kMe, kMe, 774, 10, 0)) == "You gain 10 Mana from Rejuvenation.");
    CHECK(line(event(K::Drain, kMe, kWolf, 772, 15, 0)) == "Your Rend: -15 Mana on Young Wolf.");

    // Unknown names never leave holes; the buffer cuts long lines.
    CHECK(line(event(K::Damage, kKobold, kMe, 999, 1, 6)) == "Unknown's Unknown on you: 1 Arcane.");
    char tiny[12];
    game::formatCombatEvent(tiny, sizeof(tiny), event(K::Damage, kMe, kWolf, 133, 25, 2), names());
    CHECK(std::string(tiny) == "Your Fireba"); // 11 characters and the NUL
}

TEST_CASE("Aura, kill and experience lines") {
    const EnglishTexts texts;
    char out[160];
    game::formatAuraChange(out, sizeof(out), kMe, 168, true, false, names());
    CHECK(std::string(out) == "You gain Frost Armor.");
    game::formatAuraChange(out, sizeof(out), kWolf, 772, true, true, names());
    CHECK(std::string(out) == "Young Wolf is afflicted by Rend.");
    game::formatAuraChange(out, sizeof(out), kMe, 168, false, false, names());
    CHECK(std::string(out) == "Frost Armor fades from you.");
    game::formatAuraChange(out, sizeof(out), kWolf, 772, false, true, names());
    CHECK(std::string(out) == "Rend fades from Young Wolf.");
    game::formatKill(out, sizeof(out), kMe, kWolf, names());
    CHECK(std::string(out) == "You have slain Young Wolf!");
    game::formatKill(out, sizeof(out), kWolf, kMe, names());
    CHECK(std::string(out) == "Young Wolf has slain you.");
    game::formatKill(out, sizeof(out), 0, kMe, names());
    CHECK(std::string(out) == "You die.");
    game::formatKill(out, sizeof(out), kImp, kWolf, names());
    CHECK(std::string(out) == "Young Wolf is slain by Zalgaz!");
    // Experience (WoW.exe 0x637AC0): the rest state's key and name (Exhaustion.dbc: Rested x2, Normal x1).
    game::ExperienceGain xp{kWolf, false, 45, 23, 1.0f, 1, 2.0f, "Rested"};
    game::formatExperience(out, sizeof(out), xp, names());
    CHECK(std::string(out) == "Young Wolf slain: +45 experience (+22, Rested bonus).");
    game::formatExperience(out, sizeof(out), {kWolf, false, 45, 45, 1.0f, 2, 1.0f, "Normal"}, names());
    CHECK(std::string(out) == "Young Wolf slain: +45 experience.");
    game::formatExperience(out, sizeof(out), {kWolf, false, 45, 45}, names()); // no table: the plain line
    CHECK(std::string(out) == "Young Wolf slain: +45 experience.");
    game::formatExperience(out, sizeof(out), {0, false, 120, 120, 1.0f, 1, 2.0f, "Rested"}, names());
    CHECK(std::string(out) == "You gain 120 experience.");
    // A group bonus factor: what the kill gives alone is base / it (truncated); less than what came is a
    // group bonus, more a raid's penalty.
    xp.groupBonus = 0.8f; // 23 / 0.8 = 28: 5 more alone
    game::formatExperience(out, sizeof(out), xp, names());
    CHECK(std::string(out) == "Young Wolf slain: +45 experience (+22, Rested bonus, -5 raid).");
    game::formatExperience(out, sizeof(out), {kWolf, false, 45, 25, 1.25f, 1, 2.0f, "Rested"}, names()); // 20
    CHECK(std::string(out) == "Young Wolf slain: +45 experience (+20, Rested bonus, +5 group).");
    game::formatExperience(out, sizeof(out), {kWolf, false, 60, 60, 0.5f}, names()); // 120 alone
    CHECK(std::string(out) == "Young Wolf slain: +60 experience (-60 raid).");
    game::formatExperience(out, sizeof(out), {0, false, 60, 60, 1.25f}, names()); // 48 alone
    CHECK(std::string(out) == "+60 experience (+12 group).");
    game::formatExperience(out, sizeof(out), {kMe, true, -30, -30}, names());
    CHECK(std::string(out) == "You lose 30 experience.");
    char world[32];
    CHECK(game::formatExperienceWorldText(world, 45) == "XP: 45");
}

TEST_CASE("Combat log lines with the baked tables: every key exists, Spanish reorders its arguments") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    using K = game::CombatEvent::Kind;
    using C = game::CombatEvent;
    // A key the table lacks shows as itself: no line of any kind, form or flag may hold one.
    const auto hasKey = [](const std::string& s) {
        usize run = 0;
        for (const char c : s) {
            run = (c >= 'A' && c <= 'Z') || c == '_' ? run + 1 : 0;
            if (run >= 6) {
                return true;
            }
        }
        return false;
    };
    const u64 units[][2] = {{kMe, kWolf}, {kWolf, kMe}, {kWolf, kImp}, {kMe, kMe}};
    for (u32 k = 0; k <= static_cast<u32>(K::Drain); ++k) {
        for (const auto& who : units) {
            for (const u32 spell : {0u, 133u}) {
                for (const u8 school : {u8{0}, u8{4}}) {
                    for (const u8 flags : {u8{0}, C::kCrit, C::kPeriodic, u8{C::kGlancing | C::kCrushing}}) {
                        game::CombatEvent e =
                            event(static_cast<K>(k), who[0], who[1], spell, 10, school, flags);
                        e.absorbed = e.resisted = e.blocked = 1;
                        const std::string s = line(e);
                        INFO(k << " " << spell << " " << int(flags) << ": " << s);
                        CHECK_FALSE(s.empty());
                        CHECK_FALSE(hasKey(s));
                    }
                }
            }
        }
    }
    for (u32 type = 0; type < 7; ++type) {
        CHECK_FALSE(hasKey(line(event(K::Damage, 0, kMe, type, 10, 0, C::kEnvironment))));
        CHECK_FALSE(hasKey(line(event(K::Damage, 0, kWolf, type, 10, 0, C::kEnvironment))));
    }
    char out[200];
    for (const u64 unit : {kMe, kWolf}) {
        for (const bool gained : {true, false}) {
            for (const bool harmful : {true, false}) {
                game::formatAuraChange(out, sizeof(out), unit, 772, gained, harmful, names());
                CHECK_FALSE(hasKey(out));
            }
        }
    }
    for (const auto& who : units) {
        game::formatKill(out, sizeof(out), who[0], who[1], names());
        CHECK_FALSE(hasKey(out));
    }
    // The client's sentences, by their keys and arguments.
    const auto keyed = [](text::Key key, const auto&... args) {
        char buf[256];
        return std::string(text::format(buf, key, args...));
    };
    game::formatExperience(out, sizeof(out), {kWolf, false, 45, 23, 1.0f, 1, 2.0f, "Rested"}, names());
    CHECK(std::string(out) == keyed("COMBATLOG_XPGAIN_EXHAUSTION1", "Young Wolf", 45, "+22", "Rested"));
    CHECK(line(event(K::Damage, kMe, kWolf, 133, 25, 2)) ==
          keyed("SPELLLOGSCHOOLSELFOTHER", "Fireball", "Young Wolf", 25, text::get("SPELL_SCHOOL2_CAP")));

    // The Spanish table: the caster after the spell (positional arguments).
    REQUIRE(text::load(std::filesystem::path(dir) / "ui.vdpk", "esES"));
    // Phase 21: Blizzard's own wording when ui.vdpk was baked with the esES client (its "Rested bonus" in English
    // included), ours otherwise.
    const bool official = test::esESClientTexts();
    const std::string firebolt = line(event(K::Damage, kImp, kWolf, 3110, 12));
    CHECK(firebolt.starts_with("Firebolt de Zalgaz ")); // the spell before its caster
    CHECK(firebolt == (official ? keyed("SPELLLOGOTHEROTHER", "Zalgaz", "Firebolt", "Young Wolf", 12)
                                : "Firebolt de Zalgaz golpea a Young Wolf y causa 12 p. de daño."));
    const std::string rend = line(event(K::Damage, kWolf, kMe, 772, 3, 0, C::kPeriodic));
    CHECK(rend.find("Rend de Young Wolf") != std::string::npos);
    CHECK(rend == (official ? keyed("PERIODICAURADAMAGEOTHERSELF", 3, text::get("SPELL_SCHOOL0_CAP"), "Young Wolf",
                                    "Rend")
                            : "Sufres 3 p. de daño de Físico por Rend de Young Wolf."));
    CHECK(line(event(K::Dodge, kMe, kWolf, 133, 0)) == "Young Wolf esquiva tu Fireball.");
    game::formatExperience(out, sizeof(out), {kWolf, false, 45, 23, 1.0f, 1, 2.0f, "Rested"}, names());
    CHECK(std::string(out) == (official ? keyed("COMBATLOG_XPGAIN_EXHAUSTION1", "Young Wolf", 45, "+22", "Rested")
                                        : "Young Wolf muere, ganas 45 p. de experiencia (+22 por descanso)."));
    CHECK(std::string_view(out).starts_with("Young Wolf muere, "));
    game::formatKill(out, sizeof(out), kImp, kWolf, names());
    CHECK(std::string(out) == (official ? "¡Zalgaz asesina a Young Wolf!" : "Zalgaz mata a Young Wolf."));
    text::unload();
}

TEST_CASE("Aura watch: gained and faded auras of the units watched") {
    game::EntityStore store;
    REQUIRE(store.applyUpdateObject(
        created(kMe, game::ObjectType::Player, {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_AURA, 168}}),
        1000, nullptr));
    REQUIRE(store.applyUpdateObject(created(kWolf, game::ObjectType::Unit, {{uf::UNIT_FIELD_HEALTH, 40}}),
                                    1000, nullptr));
    game::AuraWatch watch;
    struct Change {
        u64 guid;
        u32 spell;
        bool gained, harmful;
    };
    std::vector<Change> changes;
    const auto record = [&](u64 guid, u32 spell, bool gained, bool harmful) {
        changes.push_back({guid, spell, gained, harmful});
    };
    const u64 units[] = {kMe, kWolf, 0};
    watch.update(store, units, record);
    CHECK(changes.empty()); // what a unit has when first seen is not news

    // Rend on the wolf (a debuff slot), Frost Armor gone from the player.
    REQUIRE(store.applyUpdateObject(values(kWolf, {{static_cast<u16>(uf::UNIT_FIELD_AURA + 32), 772}}), 1100,
                                    nullptr));
    REQUIRE(store.applyUpdateObject(values(kMe, {{uf::UNIT_FIELD_AURA, 0}}), 1100, nullptr));
    watch.update(store, units, record);
    REQUIRE(changes.size() == 2);
    CHECK(changes[0].guid == kMe);
    CHECK(changes[0].spell == 168);
    CHECK_FALSE(changes[0].gained);
    CHECK(changes[1].guid == kWolf);
    CHECK(changes[1].spell == 772);
    CHECK(changes[1].gained);
    CHECK(changes[1].harmful);
    changes.clear();
    watch.update(store, units, record);
    CHECK(changes.empty());
}

TEST_CASE("Skills and combo points from the player's fields") {
    game::EntityStore store;
    // Two skills: Fire (8) 15/50, and Swords (43) 12/50 with +5 / -1 bonuses.
    const u16 skill0 = uf::PLAYER_SKILL_INFO_1_1;
    REQUIRE(store.applyUpdateObject(
        created(kMe, game::ObjectType::Player,
                {{uf::UNIT_FIELD_HEALTH, 100},
                 {skill0, 8},
                 {static_cast<u16>(skill0 + 1), 15u | (50u << 16)},
                 {static_cast<u16>(skill0 + 3), 43u | (1u << 16)},
                 {static_cast<u16>(skill0 + 4), 12u | (50u << 16)},
                 {static_cast<u16>(skill0 + 5), 5u | (0xFFFFu << 16)},
                 {uf::PLAYER_FIELD_COMBO_TARGET, static_cast<u32>(kWolf)},
                 {static_cast<u16>(uf::PLAYER_FIELD_COMBO_TARGET + 1), static_cast<u32>(kWolf >> 32)},
                 {uf::PLAYER_FIELD_BYTES, 3u << 8}}),
        1000, nullptr));
    const game::Entity* me = store.find(kMe);
    REQUIRE(me != nullptr);
    std::array<game::SkillValue, game::kMaxSkills> skills{};
    REQUIRE(game::readSkills(*me, skills) == 2);
    CHECK(skills[0].id == 8);
    CHECK(skills[0].value == 15);
    CHECK(skills[0].max == 50);
    CHECK(skills[0].bonus == 0);
    CHECK(skills[1].id == 43);
    CHECK(skills[1].step == 1);
    CHECK(skills[1].value == 12);
    CHECK(skills[1].bonus == 4); // +5 temporary, -1 permanent
    CHECK(game::comboPoints(*me, kWolf) == 3);
    CHECK(game::comboPoints(*me, kKobold) == 0); // points on another target
    CHECK(game::comboPoints(*me, 0) == 0);
}
