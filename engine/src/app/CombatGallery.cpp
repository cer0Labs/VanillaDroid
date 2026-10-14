#include "app/CombatGallery.h"

#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Text.h"
#include "game/CombatText.h"
#include "game/SpellText.h"
#include "ui/Widgets.h"

#include <array>
#include <cstdio>
#include <iterator>

namespace vd {
namespace {

using ui::Rect;

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

// Phase 15: skills and reputation went to app/CharacterGallery.
constexpr const char* kPageNames[] = {"spellbook",  "trainer",   "talents",      "pet", "actionedit",
                                      "itempicker", "combatlog", "actiontooltip"};

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

// A level-10 human mage's book.
const SpellbookTabView kTabs[] = {{"General", icon("INV_Misc_Book_09")},
                                  {"Arcane", icon("Spell_Holy_MagicalSentry")},
                                  {"Fire", icon("Spell_Fire_FlameBolt")},
                                  {"Frost", icon("Spell_Frost_FrostBolt02")}};
const SpellbookEntryView kGeneral[] = {
    {icon("INV_Sword_04"), "Attack", ""},
    {icon("Spell_Nature_Sleep"), "Perception", ""},
    {icon("INV_Misc_Note_02"), "Diplomacy", "", true},
    {icon("Ability_ShootWand"), "Shoot", ""},
};
const SpellbookEntryView kArcane[] = {
    {icon("Spell_Holy_MagicalSentry"), "Arcane Intellect", "Rank 1"},
    {icon("Spell_Nature_StarFall"), "Arcane Missiles", "Rank 1"},
    {icon("INV_Drink_06"), "Conjure Water", "Rank 2"},
    {icon("INV_Misc_Food_10"), "Conjure Food", "Rank 1"},
    {icon("Spell_Nature_Polymorph"), "Polymorph", "Rank 1", false, 0.4f},
    {icon("Spell_Holy_DispelMagic"), "Dampen Magic", "Rank 1"},
};
const SpellbookEntryView kFire[] = {
    {icon("Spell_Fire_FlameBolt"), "Fireball", "Rank 3"},
    {icon("Spell_Fire_Fireball"), "Fire Blast", "Rank 2", false, 0.6f},
    {icon("Spell_Fire_SelfDestruct"), "Flamestrike", "Rank 1", false, 0.0f, false},
};
const SpellbookEntryView kFrost[] = {
    {icon("Spell_Frost_FrostBolt02"), "Frostbolt", "Rank 2"},
    {icon("Spell_Frost_FrostArmor02"), "Frost Armor", "Rank 2"},
    {icon("Spell_Frost_FrostNova"), "Frost Nova", "Rank 1"},
};

// Fireball (Rank 3), its cost, range and cast time written by game/SpellText as the game writes them.
SpellTooltipView fireballTooltip() {
    static char cost[48], range[48], cast[48];
    game::SpellDetails fireball;
    fireball.powerCost = 65;
    fireball.maxRange = 35.0f;
    fireball.castTimeMs = 2500;
    game::formatSpellCost(cost, sizeof(cost), fireball);
    game::formatSpellRange(range, sizeof(range), fireball);
    game::formatSpellCastTime(cast, sizeof(cast), fireball);
    SpellTooltipView t;
    t.iconHash = icon("Spell_Fire_FlameBolt");
    t.name = "Fireball";
    t.rank = "Rank 3";
    t.cost = cost;
    t.range = range;
    t.castTime = cast;
    t.description =
        "Throws a ball of flame at the target: 53 to 73 Fire damage, then 8 more Fire damage over 6 sec.";
    return t;
}

const TrainerServiceView kServices[] = {
    {icon("Spell_Fire_FlameBolt"), "Fireball", "Rank 4", 1800, TrainerServiceState::Available},
    {icon("Spell_Frost_FrostArmor02"), "Frost Armor", "Rank 2", 900, TrainerServiceState::Available},
    {icon("Spell_Holy_DispelMagic"), "Dampen Magic", "Rank 1", 1000, TrainerServiceState::Available},
    {icon("Spell_Fire_SelfDestruct"), "Flamestrike", "Rank 1", 1400, TrainerServiceState::Unavailable},
    {icon("Spell_Frost_FrostNova"), "Frost Nova", "Rank 2", 3000, TrainerServiceState::Unavailable},
    {icon("Spell_Nature_StarFall"), "Arcane Missiles", "Rank 2", 3000, TrainerServiceState::Unavailable},
    {icon("Spell_Fire_Fireball"), "Fire Blast", "Rank 1", 0, TrainerServiceState::Known},
};

// The Fire tree's first rows.
const TalentButtonView kFireTalents[] = {
    {icon("Spell_Fire_FlameBolt"), 0, 1, 5, 5, false},
    {icon("Spell_Fire_MeteorStorm"), 0, 2, 0, 5, true},
    {icon("Spell_Fire_Incinerate"), 1, 0, 0, 5, true},
    {icon("Spell_Fire_Flare"), 1, 1, 1, 2, true},
    {icon("Spell_Fire_Fireball"), 1, 2, 0, 3, true},
    {icon("Spell_Fire_FlameShock"), 2, 0, 0, 2, false},
    {icon("Spell_Fire_SelfDestruct"), 2, 1, 0, 3, false},
    {icon("Spell_Fire_Fireball02"), 2, 2, 0, 1, false, 4, false},
    {icon("Spell_Fire_SoulBurn"), 3, 0, 0, 2, false},
    {icon("Spell_Fire_MasterOfElements"), 3, 3, 0, 3, false},
    {icon("Spell_Fire_SealOfFire"), 4, 1, 0, 3, false},
    {icon("Spell_Holy_ExcorcismHolyFire"), 4, 2, 0, 1, false, 7, false},
    {icon("Spell_Fire_Immolation"), 5, 2, 0, 5, false},
    {icon("Spell_Fire_SealOfFire"), 6, 1, 0, 1, false, 10, false},
};
const TalentButtonView kArcaneTalents[] = {
    {icon("Spell_Holy_DispelMagic"), 0, 0, 0, 5, true},
    {icon("Spell_Holy_Devotion"), 0, 1, 0, 5, true},
    {icon("Spell_Nature_StarFall"), 0, 2, 0, 5, true},
};

const ItemView kUsable[] = {
    {icon("INV_Drink_06"), "Conjured Fresh Water", 14, 1},
    {icon("INV_Misc_Food_10"), "Conjured Bread", 6, 1},
    {icon("INV_Potion_51"), "Minor Healing Potion", 3, 1},
    {icon("INV_Misc_Rune_01"), "Hearthstone", 1, 1},
};

// A mage fighting a wolf, as game/CombatText writes it (the colors of CombatScreens' log).
constexpr u64 kMe = 0x21;
constexpr u64 kWolf = 0xF130000000000456ull;
constexpr u32 kSampleLines = 16; // phase 13: the last six, the packets added then

game::CombatNames sampleNames() {
    game::CombatNames n;
    n.player = kMe;
    n.unitName = [](const void*, u64 guid) -> std::string_view {
        return guid == kMe ? "Arthel" : guid == kWolf ? "Young Wolf" : std::string_view{};
    };
    n.itemName = [](const void*, u32 entry) -> std::string_view {
        return entry == 2681 ? "Roasted Boar Meat" : entry == 2488 ? "Gladius" : std::string_view{};
    };
    n.enchantName = [](const void*, u32 id) -> std::string_view { return id == 29 ? "Rockbiter 3" : ""; };
    n.spellName = [](const void*, u32 spell) -> std::string_view {
        switch (spell) {
        case 133: return "Fireball";
        case 168: return "Frost Armor";
        case 2136: return "Fire Blast";
        case 774: return "Rejuvenation";
        case 5138: return "Drain Mana"; // phase 13
        case 467: return "Thorns";
        case 1243: return "Power Word: Fortitude";
        case 2550: return "Cooking";
        default: return {};
        }
    };
    return n;
}

game::CombatEvent sampleEvent(game::CombatEvent::Kind kind, u64 attacker, u64 victim, u32 spell, i32 amount,
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

void sampleLog(std::array<std::array<char, 200>, kSampleLines>& out,
               std::array<CombatLogLineView, kSampleLines>& lines) {
    using K = game::CombatEvent::Kind;
    using E = game::CombatEvent;
    const game::CombatNames n = sampleNames();
    const u32 aura = ui::rgba(120, 180, 255), hit = ui::kWhite, hurt = ui::rgba(255, 90, 90);
    u32 i = 0;
    const auto event = [&](const game::CombatEvent& e, u32 color) {
        game::formatCombatEvent(out[i].data(), out[i].size(), e, n);
        lines[i] = {out[i].data(), color};
        ++i;
    };
    game::formatAuraChange(out[i].data(), out[i].size(), kMe, 168, true, false, n);
    lines[i] = {out[i].data(), aura};
    ++i;
    event(sampleEvent(K::Damage, kMe, kWolf, 133, 18, 2), hit);
    event(sampleEvent(K::Damage, kMe, kWolf, 133, 2, 2, E::kPeriodic), hit);
    event(sampleEvent(K::Damage, kWolf, kMe, 0, 3), hurt);
    event(sampleEvent(K::Dodge, kMe, kWolf, 0, 0), ui::rgba(200, 200, 200));
    event(sampleEvent(K::Damage, kMe, kWolf, 2136, 41, 2, E::kCrit), hit);
    // Rested (rest state 1, Exhaustion.dbc's factor 2 and name).
    game::formatExperience(out[i].data(), out[i].size(), {kWolf, false, 45, 23, 1.0f, 1, 2.0f, "Rested"}, n);
    lines[i] = {out[i].data(), ui::rgba(255, 209, 0)};
    ++i;
    event(sampleEvent(K::Heal, kMe, kMe, 774, 12, 0, E::kPeriodic), ui::rgba(90, 255, 90));
    game::formatAuraChange(out[i].data(), out[i].size(), kMe, 168, false, false, n);
    lines[i] = {out[i].data(), aura};
    ++i;
    text::format(out[i], "FACTION_STANDING_INCREASED", "Stormwind", 25);
    lines[i] = {out[i].data(), ui::rgba(128, 128, 255)};
    ++i;
    // Phase 13 (combat): a damage shield, a leech, a dispel and its failure, an item made, an enchantment fading.
    game::CombatEvent shield = sampleEvent(K::Damage, kMe, kWolf, 0, 3, 3, E::kDamageShield);
    event(shield, hit);
    game::CombatEvent leech = sampleEvent(K::Drain, kMe, kWolf, 5138, 40);
    leech.gain = 40;
    event(leech, hit);
    event(sampleEvent(K::Dispelled, kWolf, kMe, 1243, 0), aura);
    event(sampleEvent(K::DispelFailed, kMe, kWolf, 1243, 0), ui::rgba(200, 200, 200));
    game::CombatEvent made = sampleEvent(K::CreateItem, kMe, 0, 2550, 0);
    made.item = 2681;
    event(made, hit);
    game::CombatEvent faded = sampleEvent(K::EnchantFaded, 0, kMe, 0, 29);
    faded.item = 2488;
    event(faded, aura);
}

} // namespace

bool CombatGallery::select(std::string_view name) {
    for (u32 i = 0; i < std::size(kPageNames); ++i) {
        if (name == kPageNames[i]) {
            m_page = static_cast<Page>(i);
            return true;
        }
    }
    return false;
}

bool CombatGallery::next() {
    m_page = static_cast<Page>((static_cast<u32>(m_page) + 1) % static_cast<u32>(Page::Count));
    return m_page == Page::Spellbook;
}

std::string_view CombatGallery::pageName() const {
    return kPageNames[static_cast<u32>(m_page)];
}

void CombatGallery::draw(DialogContext& c) {
    char note[96];
    switch (m_page) {
    case Page::Spellbook: spellbook(c); break;
    case Page::Trainer: trainer(c); break;
    case Page::Talents: talents(c); break;
    case Page::Pet: pet(c); break;
    case Page::ActionEdit: {
        ActionEditView view{icon("Spell_Fire_FlameBolt"), "Fireball", 1, 2, fireballTooltip()};
        const ActionEditResult r = actionEditDialog(c, view);
        if (r.pickSpell) {
            logResult("action edit: spell");
        }
        if (r.pickItem) {
            logResult("action edit: item");
        }
        if (r.clear) {
            logResult("action edit: clear");
        }
        if (r.closed) {
            logResult("action edit: closed");
        }
        break;
    }
    case Page::ItemPicker: {
        bool closed = false;
        const i32 picked = itemPicker(c, kUsable, closed);
        if (picked >= 0) {
            std::snprintf(note, sizeof(note), "item picked %d", picked);
            logResult(note);
        }
        if (closed) {
            logResult("item picker closed");
        }
        break;
    }
    case Page::CombatLog: {
        std::array<std::array<char, 200>, kSampleLines> lineText{};
        std::array<CombatLogLineView, kSampleLines> lines{};
        sampleLog(lineText, lines);
        const CombatLogResult r = combatLogWindow(c, lines, m_onlyMine);
        if (r.toggleOnlyMine) {
            m_onlyMine = !m_onlyMine;
            logResult("combat log: only mine");
        }
        if (r.clear) {
            logResult("combat log: clear");
        }
        if (r.closed) {
            logResult("combat log closed");
        }
        break;
    }
    case Page::ActionTooltip: {
        // Beside a button of the action cluster (bottom right), as a long press shows it.
        std::array<TooltipLineView, 8> lines{};
        SpellTooltipView spell = fireballTooltip();
        static char cooldown[48], left[64];
        game::SpellDetails recovery;
        recovery.recoveryMs = 8000;
        game::formatSpellCooldown(cooldown, sizeof(cooldown), recovery);
        spell.cooldown = cooldown;
        u32 count = spellTooltipLines(spell, lines);
        if (count < lines.size()) {
            lines[count++] = {text::format(left, "ITEM_COOLDOWN_TIME_SEC", 5), {}};
        }
        const std::string_view edit = text::get("VD_ACTIONBAR_EDIT");
        TooltipView view;
        view.iconHash = spell.iconHash;
        view.lines = {lines.data(), count};
        view.actions = {&edit, 1};
        const ui::Rect& safe = c.safeArea;
        const f32 size = c.ui.px(54.0f);
        view.anchor = {safe.right() - c.ui.px(200.0f), safe.bottom() - c.ui.px(160.0f), size, size};
        c.draw.solid(view.anchor, ui::rgba(80, 80, 80, 200));
        const TooltipResult r = itemTooltip(c, view);
        if (r.action == 0) {
            logResult("action tooltip: edit");
        }
        if (r.closed) {
            logResult("action tooltip: closed");
        }
        break;
    }
    case Page::Count: break;
    }
}

void CombatGallery::spellbook(DialogContext& c) {
    SpellbookView view;
    view.tabs = kTabs;
    view.tab = m_tab;
    switch (m_tab) {
    case 0: view.entries = kGeneral; break;
    case 1: view.entries = kArcane; break;
    case 2: view.entries = kFire; break;
    default: view.entries = kFrost; break;
    }
    view.selected = m_selected < static_cast<i32>(view.entries.size()) ? m_selected : -1;
    view.allRanks = m_allRanks;
    if (view.selected >= 0) {
        const SpellbookEntryView& e = view.entries[static_cast<u32>(view.selected)];
        view.tooltip = fireballTooltip();
        view.tooltip.iconHash = e.iconHash;
        view.tooltip.name = e.name;
        view.tooltip.rank = e.passive ? text::get("SPELL_PASSIVE") : e.rank;
    }
    const SpellbookResult r = spellbookWindow(c, view);
    char text[64];
    if (r.tab >= 0) {
        m_tab = static_cast<u32>(r.tab);
        m_selected = -1;
        std::snprintf(text, sizeof(text), "spellbook tab %d", r.tab);
        logResult(text);
    }
    if (r.select >= 0) {
        m_selected = r.select;
        std::snprintf(text, sizeof(text), "spellbook select %d", r.select);
        logResult(text);
    }
    if (r.toggleRanks) {
        m_allRanks = !m_allRanks;
        logResult("spellbook ranks");
    }
    if (r.place) {
        logResult("spellbook place");
    }
    if (r.cast) {
        logResult("spellbook cast");
    }
    if (r.closed) {
        logResult("spellbook closed");
    }
}

void CombatGallery::trainer(DialogContext& c) {
    TrainerServiceView services[std::size(kServices)];
    u32 count = 0;
    for (const TrainerServiceView& s : kServices) {
        const u32 filter = static_cast<u32>(s.state);
        if (m_filters[filter]) {
            services[count++] = s;
        }
    }
    TrainerView view;
    view.npcName = "Khelden Bremen";
    view.greeting = "Back for more lessons, mage? Bring coin and I will show you new spells.";
    view.services = {services, count};
    view.selected = m_service < static_cast<i32>(count) ? m_service : -1;
    view.money = 2350;
    view.showAvailable = m_filters[0];
    view.showUnavailable = m_filters[1];
    view.showKnown = m_filters[2];
    if (view.selected >= 0) {
        const TrainerServiceView& s = services[view.selected];
        view.tooltip = fireballTooltip();
        view.tooltip.iconHash = s.iconHash;
        view.tooltip.name = s.name;
        view.tooltip.rank = s.rank;
        // "Requires: Level 16", as CombatScreens writes it.
        static char requirement[96];
        const std::string_view label = text::get("REQUIRES_LABEL");
        char level[64];
        text::format(level, "TRAINER_REQ_LEVEL_RED", 16);
        std::snprintf(requirement, sizeof(requirement), "%.*s %s", static_cast<int>(label.size()),
                      label.data(), level);
        view.tooltip.requirement = s.state == TrainerServiceState::Unavailable ? requirement : "";
        view.canLearn = s.state == TrainerServiceState::Available && s.cost <= view.money;
    }
    const TrainerResult r = trainerWindow(c, view);
    char text[64];
    if (r.select >= 0) {
        m_service = r.select;
        std::snprintf(text, sizeof(text), "trainer select %d", r.select);
        logResult(text);
    }
    if (r.learn) {
        logResult("trainer learn");
    }
    m_filters[0] ^= r.toggleAvailable;
    m_filters[1] ^= r.toggleUnavailable;
    m_filters[2] ^= r.toggleKnown;
    if (r.closed) {
        logResult("trainer closed");
    }
}

void CombatGallery::talents(DialogContext& c) {
    const TalentTreeView trees[] = {{"Arcane", "MageArcane", 0, kArcaneTalents},
                                    {"Fire", "MageFire", 6, kFireTalents},
                                    {"Frost", "MageFrost", 0, {}}};
    TalentView view;
    view.trees = trees;
    view.tree = m_tree;
    view.selected = m_talent;
    view.freePoints = 2;
    if (m_tree == 1 && m_talent >= 0 && m_talent < static_cast<i32>(std::size(kFireTalents))) {
        const TalentButtonView& t = kFireTalents[m_talent];
        view.tooltip.iconHash = t.iconHash;
        view.tooltip.name = "Improved Fire Blast";
        static char rank[32];
        view.tooltip.rank = text::format(rank, "TOOLTIP_TALENT_RANK", 0, 3);
        view.tooltip.description = "Fire Blast's cooldown is 0.5 sec shorter.";
        view.tooltip.nextTitle = text::get("TOOLTIP_TALENT_NEXT_RANK");
        view.tooltip.nextDescription = "Fire Blast's cooldown is 1 sec shorter.";
        view.canLearn = t.available;
    } else {
        view.selected = -1;
    }
    const TalentResult r = talentWindow(c, view);
    char text[64];
    if (r.tree >= 0) {
        m_tree = static_cast<u32>(r.tree);
        m_talent = -1;
        std::snprintf(text, sizeof(text), "talents tree %d", r.tree);
        logResult(text);
    }
    if (r.select >= 0) {
        m_talent = r.select;
        std::snprintf(text, sizeof(text), "talents select %d", r.select);
        logResult(text);
    }
    if (r.learn) {
        logResult("talents learn");
    }
    if (r.closed) {
        logResult("talents closed");
    }
}

void CombatGallery::pet(DialogContext& c) {
    // The pet frame under where the player frame is, and the bar along the bottom.
    ui::Gui g{c.ui, c.draw};
    const Rect& safe = c.safeArea;
    const f32 u = c.ui.dp() * ui::kUnitDp;
    PetFrameView frame;
    frame.name = "Wolf";
    frame.level = 10;
    frame.health = 0.8f;
    frame.power = 0.55f;
    frame.powerType = 2;
    frame.happiness = 2;
    const PetFrameResult f =
        petFrame(g, {safe.x + 80.0f * u, safe.y + 60.0f * u, 128.0f * u, 64.0f * u}, frame);
    if (f.tapped) {
        logResult("pet frame tapped");
    }
    if (f.held) {
        logResult("pet frame held");
    }
    static constexpr u64 kReactions[] = {icon("Ability_Seal"), icon("Ability_Defend"),
                                         icon("Ability_Racial_BloodRage")};
    const PetSlotView slots[] = {
        {icon("Ability_GhoulFrenzy")},
        {icon("Ability_Tracking"), true},
        {icon("Spell_Nature_TimeStop")},
        {icon("Ability_Racial_Cannibalize"), false, m_autocast[0], true, 0.3f},
        {icon("Ability_Physical_Taunt"), false, m_autocast[1], true},
        {kReactions[m_reaction % 3], true},
    };
    const f32 size = g.px(34.0f);
    const f32 width = 6.0f * size + 5.0f * g.px(4.0f);
    const PetBarResult r =
        petBar(g, c.resources,
               {safe.centerX() - width * 0.5f, safe.bottom() - size - g.px(14.0f), width, size}, slots);
    char text[48];
    if (r.tapped >= 0) {
        if (r.tapped == 5) {
            m_reaction = (m_reaction + 2) % 3;
        }
        std::snprintf(text, sizeof(text), "pet slot %d", r.tapped);
        logResult(text);
    }
    if (r.held == 3 || r.held == 4) {
        m_autocast[r.held - 3] = !m_autocast[r.held - 3];
        std::snprintf(text, sizeof(text), "pet autocast %d", r.held);
        logResult(text);
    }
}

} // namespace vd
