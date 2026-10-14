#include "app/PetGallery.h"

#include "app/DialogKit.h"
#include "app/ModelFrameGallery.h"
#include "app/SocialWindows.h"
#include "app/UnitFrames.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

// A hunter's wolf at level 20: its book (WoW.exe's order: by name, then rank) and its bar.
constexpr u64 kBite = assetPathHash("Interface\\Icons\\Ability_Racial_Cannibalize.blp");
constexpr u64 kDash = assetPathHash("Interface\\Icons\\Ability_Druid_Dash.blp");
constexpr u64 kHowl = assetPathHash("Interface\\Icons\\Ability_Hunter_Pet_Wolf.blp");
constexpr u64 kGrowl = assetPathHash("Interface\\Icons\\Ability_Physical_Taunt.blp");
constexpr u64 kStamina = assetPathHash("Interface\\Icons\\Spell_Nature_UnyeildingStamina.blp");
constexpr u64 kArmor = assetPathHash("Interface\\Icons\\Spell_Nature_SpiritArmor.blp");
constexpr u64 kFireResist = assetPathHash("Interface\\Icons\\Spell_Fire_FireArmor.blp");
constexpr u64 kAttack = assetPathHash("Interface\\Icons\\Ability_GhoulFrenzy.blp");
constexpr u64 kFollow = assetPathHash("Interface\\Icons\\Ability_Tracking.blp");
constexpr u64 kStay = assetPathHash("Interface\\Icons\\Spell_Nature_TimeStop.blp");
constexpr u64 kAggressive = assetPathHash("Interface\\Icons\\Ability_Racial_BloodRage.blp");
constexpr u64 kDefensive = assetPathHash("Interface\\Icons\\Ability_Defend.blp");
constexpr u64 kPassive = assetPathHash("Interface\\Icons\\Ability_Seal.blp");
constexpr u64 kWolfIcon = assetPathHash("Interface\\Icons\\Ability_Hunter_Pet_Wolf.blp");
constexpr u64 kCatIcon = assetPathHash("Interface\\Icons\\Ability_Hunter_Pet_Cat.blp");

struct BookSample {
    u64 icon;
    const char* name;
    const char* rank;
    bool passive;
    bool autocastable;
};
// By name, as Abilities::buildPetBook sorts the pet's book.
constexpr BookSample kBook[] = {
    {kBite, "Bite", "Rank 3", false, true},
    {kDash, "Dash", "Rank 1", false, true},
    {kFireResist, "Fire Resistance", "Rank 1", true, false},
    {kHowl, "Furious Howl", "Rank 1", false, true},
    {kStamina, "Great Stamina", "Rank 2", true, false},
    {kGrowl, "Growl", "Rank 3", false, true},
    {kArmor, "Natural Armor", "Rank 1", true, false},
};

} // namespace

void PetGallery::draw(DialogContext& c, Page page, u64 nowMs) {
    VD_PROFILE_ZONE("PetGallery::draw");
    switch (page) {
    case Page::Book: book(c); break;
    case Page::Bar: bar(c, nowMs); break;
    case Page::PartyPets: partyPets(c); break;
    case Page::Stable: stable(c); break;
    case Page::PaperDoll: paperDoll(c); break;
    case Page::Feedback: feedback(c, nowMs); break;
    case Page::Count: break;
    }
}

void PetGallery::book(DialogContext& c) {
    // SpellBookFrame with BOOKTYPE_PET: the wolf's spells, Bite's autocast on; the tabs along the bottom.
    std::array<SpellbookEntryView, std::size(kBook)> entries{};
    u32 autocastIndex = 0;
    for (u32 i = 0; i < std::size(kBook); ++i) {
        SpellbookEntryView& e = entries[i];
        e.iconHash = kBook[i].icon;
        e.name = kBook[i].name;
        e.rank = kBook[i].rank;
        e.passive = kBook[i].passive;
        e.autocastable = kBook[i].autocastable;
        if (e.autocastable) {
            e.autocast = m_autocast[autocastIndex++ % m_autocast.size()];
        }
        e.cooldown = i == 0 ? 0.4f : 0.0f;
    }
    const std::string_view books[] = {text::get("SPELLBOOK"), text::get("PET_TYPE_PET")};
    SpellbookView v;
    v.entries = entries;
    v.selected = m_bookSelected;
    v.books = books;
    v.book = 1;
    v.petBook = true;
    v.title = text::get("PET_TYPE_PET");
    if (m_bookSelected >= 0) {
        v.tooltip.iconHash = kBook[m_bookSelected].icon;
        v.tooltip.name = kBook[m_bookSelected].name;
        v.tooltip.rank = kBook[m_bookSelected].rank;
        v.tooltip.cost = "35 Focus";
        v.tooltip.range = "Melee Range";
        v.tooltip.castTime = "Instant";
        v.tooltip.cooldown = "10 sec cooldown";
        v.tooltip.description = "Bites the target for 31 to 37 damage.";
    }
    const SpellbookResult r = spellbookWindow(c, v);
    char line[64];
    if (r.select >= 0) {
        m_bookSelected = r.select;
    }
    if (r.autocast >= 0 && kBook[r.autocast].autocastable) {
        u32 index = 0;
        for (i32 i = 0; i < r.autocast; ++i) {
            index += kBook[i].autocastable ? 1u : 0u;
        }
        m_autocast[index % m_autocast.size()] = !m_autocast[index % m_autocast.size()];
        std::snprintf(line, sizeof(line), "pet book autocast %s", kBook[r.autocast].name);
        logResult(line);
    }
    if (r.place) {
        logResult("pet book: put on bar");
    }
    if (r.cast) {
        logResult("pet book: cast");
    }
    if (r.book >= 0) {
        std::snprintf(line, sizeof(line), "pet book: tab %d", r.book);
        logResult(line);
    }
}

void PetGallery::bar(DialogContext& c, u64 nowMs) {
    // PetActionBarFrame's ten buttons (Attack, Follow, Stay, Bite, Growl, two empty, the three reactions);
    // "Put on Bar" lights the empty slots (PET_BAR_SHOWGRID). A finger dragged between two buttons swaps
    // them.
    ui::Gui g{c.ui, c.draw};
    const Rect& safe = c.safeArea;
    const f32 u = c.ui.dp() * ui::kUnitDp;
    PetFrameView frame;
    frame.name = "Wolf";
    frame.level = 20;
    frame.health = 0.8f;
    frame.power = 0.55f;
    frame.powerType = 2;
    frame.happiness = 3;
    petFrame(g, {safe.x + 80.0f * u, safe.y + 60.0f * u, 128.0f * u, 64.0f * u}, frame);
    static constexpr u64 kButtons[10] = {kAttack, kFollow, kStay,       kBite,      kGrowl,
                                         0,       0,       kAggressive, kDefensive, kPassive};
    std::array<PetSlotView, 10> slots{};
    std::array<u32, 10> slotOf{};
    u32 count = 0;
    for (u32 slot = 0; slot < 10; ++slot) {
        const u32 button = m_bar[slot];
        PetSlotView v;
        v.iconHash = kButtons[button];
        v.active = button == 1 || button == 7 + m_reaction;
        v.autocastable = button == 3 || button == 4;
        v.autocast = button == 3;
        v.cooldown = button == 3 ? 0.35f : 0.0f;
        v.target = m_placing && (button >= 3 && button <= 6);
        if (v.iconHash == 0 && !v.target) {
            continue;
        }
        slots[count] = v;
        slotOf[count++] = slot;
    }
    const f32 size = g.px(34.0f), gap = g.px(4.0f);
    const f32 width = static_cast<f32>(count) * size + static_cast<f32>(count - 1) * gap;
    const PetBarResult r = petBar(
        g, c.resources, {safe.centerX() - width * 0.5f, safe.bottom() - size - g.px(14.0f), width, size},
        {slots.data(), count});
    char text[64];
    if (r.tapped >= 0) {
        const u32 button = m_bar[slotOf[static_cast<u32>(r.tapped)]];
        if (button >= 7) {
            m_reaction = button - 7;
        }
        if (m_placing) {
            m_placing = false;
        }
        std::snprintf(text, sizeof(text), "pet slot %u", slotOf[static_cast<u32>(r.tapped)]);
        logResult(text);
    }
    if (r.dragFrom >= 0 && r.dragTo >= 0) {
        std::swap(m_bar[slotOf[static_cast<u32>(r.dragFrom)]], m_bar[slotOf[static_cast<u32>(r.dragTo)]]);
        std::snprintf(text, sizeof(text), "pet slots %d <-> %d", r.dragFrom, r.dragTo);
        logResult(text);
    }
    // A button standing for the pet book's "Put on Bar".
    const Rect toggle{safe.centerX() - g.px(110.0f), safe.bottom() - size - g.px(70.0f), g.px(220.0f),
                      g.px(40.0f)};
    if (ui::button(g, ui::widgetId("petGallery.placing"), toggle,
                   text::get(m_placing ? "CANCEL" : "VD_COMBAT_PLACE_ON_BAR"))) {
        m_placing = !m_placing;
    }
    (void)nowMs;
}

void PetGallery::partyPets(DialogContext& c) {
    // Three members, a hunter's and a warlock's with their pets (PartyMemberPetFrame under each), one
    // without.
    ui::Gui g{c.ui, c.draw};
    std::array<PartyFrameView, 3> members{};
    members[0].name = "Aeryn";
    members[0].level = 20;
    members[0].health = 0.9f;
    members[0].power = 0.7f;
    members[0].race = 4;
    members[0].gender = 1;
    members[0].pet = true;
    members[0].petHealth = 0.65f;
    members[0].petDebuffCount = 1;
    members[0].petDebuffs[0] = {assetPathHash("Interface\\Icons\\Ability_Gouge.blp"), 0};
    members[1].name = "Brakk";
    members[1].level = 19;
    members[1].health = 0.45f;
    members[1].power = 0.3f;
    members[1].race = 3;
    members[1].leader = true;
    members[2].name = "Galen";
    members[2].level = 21;
    members[2].health = 1.0f;
    members[2].power = 0.9f;
    members[2].race = 1;
    members[2].pet = true;
    members[2].petHealth = 0.2f;
    members[2].petTargeted = true;
    const Rect area{c.safeArea.x + g.px(4.0f), c.safeArea.y + g.unit(110.0f), g.unit(128.0f) * 1.6f, 0.0f};
    i32 tappedPet = -1;
    const i32 tapped = partyFrames(c, area, members, nullptr, nullptr, nullptr, &tappedPet);
    char line[48];
    if (tapped >= 0) {
        std::snprintf(line, sizeof(line), "party frame %d", tapped);
        logResult(line);
    }
    if (tappedPet >= 0) {
        std::snprintf(line, sizeof(line), "party pet %d", tappedPet);
        logResult(line);
    }
    // The bottom of the frames (where the chat starts).
    const f32 h = partyFramesHeight(c, members);
    g.draw.solid({area.x, area.y + h, area.w, g.px(2.0f)}, rgba(255, 0, 0, 160));
}

void PetGallery::stable(DialogContext& c) {
    // The current pet (Fang, a wolf), a stabled cat, the second slot not bought (its price, the money).
    // The level and the diet through the client's templates (the families and loyalty levels are the DBC's).
    StableView v;
    char level[96], sub[3][48], part[2][32], diet[64];
    const bool catSelected = m_stableSelected == 1;
    const std::string_view lvl20 = text::format(part[0], "UNIT_LEVEL_TEMPLATE", 20);
    const std::string_view lvl18 = text::format(part[1], "UNIT_LEVEL_TEMPLATE", 18);
    const std::string_view lvl = catSelected ? lvl18 : lvl20;
    std::snprintf(level, sizeof(level), "%s %.*s %s", catSelected ? "Shadow" : "Fang",
                  static_cast<int>(lvl.size()), lvl.data(), catSelected ? "Cat" : "Wolf");
    v.levelText = level;
    v.loyaltyText = catSelected ? "(Loyalty Level 2) Unruly" : "(Loyalty Level 3) Submissive";
    v.dietText = text::format(diet, "PET_DIET_TEMPLATE", catSelected ? "Meat, Fish" : "Meat");
    v.showModel = m_stableSelected >= 0;
    if (m_models && v.showModel) {
        v.model =
            m_models->sample(ModelViewId::Stable, ModelFrameGallery::Sample::Wolf, m_stableModel.facing);
    }
    v.current.iconHash = kWolfIcon;
    v.current.tooltip = "Fang";
    std::snprintf(sub[0], sizeof(sub[0]), "%.*s Wolf", static_cast<int>(lvl20.size()), lvl20.data());
    v.current.tooltipSub = sub[0];
    v.current.checked = m_stableSelected == 0;
    v.current.picked = m_stablePicked == 0;
    v.stabled[0].iconHash = kCatIcon;
    v.stabled[0].tooltip = "Shadow";
    std::snprintf(sub[1], sizeof(sub[1]), "%.*s Cat", static_cast<int>(lvl18.size()), lvl18.data());
    v.stabled[0].tooltipSub = sub[1];
    v.stabled[0].checked = m_stableSelected == 1;
    v.stabled[0].picked = m_stablePicked == 1;
    v.stabled[1].purchased = m_stableSlots >= 2;
    v.stabled[1].tooltip = text::get("EMPTY_STABLE_SLOT");
    v.showPurchase = m_stableSlots < 2;
    v.nextSlotCost = 50000;
    v.money = 72345;
    v.canPurchase = v.money >= v.nextSlotCost;
    const StableResult r = stableWindow(c, v, m_stableModel);
    char line[64];
    if (r.pickup >= 0) {
        m_stablePicked = r.pickup;
        std::snprintf(line, sizeof(line), "stable pickup %d", r.pickup);
        logResult(line);
    }
    if (r.click >= 0 || r.drop >= 0) {
        const i32 at = r.drop >= 0 ? r.drop : r.click;
        if (m_stablePicked >= 0 && m_stablePicked != at) {
            std::snprintf(line, sizeof(line), "stable move %d -> %d", m_stablePicked, at);
        } else {
            m_stableSelected = at <= 1 ? at : -1;
            std::snprintf(line, sizeof(line), "stable select %d", at);
        }
        m_stablePicked = -1;
        logResult(line);
    }
    if (r.purchase) {
        logResult("stable purchase");
    }
}

void PetGallery::paperDoll(DialogContext& c) {
    // PetPaperDollFrame in a character window's body: the wolf's model, attributes, resistances, loyalty,
    // training points, diet and experience.
    ui::Gui g{c.ui, c.draw};
    const bool wide = c.profile == ScreenProfile::Wide;
    const kit::Dialog d = kit::beginDialog(c, g, "petGallery.sheet", wide ? 880.0f : 660.0f,
                                           wide ? 420.0f : 580.0f, "Arthel", true, false);
    PetPaperDollView v;
    char part[32], level[64], diet[64], attackPower[96], armor[256];
    const std::string_view lvl = text::format(part, "UNIT_LEVEL_TEMPLATE", 20);
    std::snprintf(level, sizeof(level), "%.*s Wolf", static_cast<int>(lvl.size()), lvl.data());
    v.name = "Fang";
    v.levelText = level;
    v.loyaltyText = "(Loyalty Level 3) Submissive";
    v.hunter = true;
    v.dietText = text::format(diet, "PET_DIET_TEMPLATE", "Meat");
    v.trainingText = "12";
    if (m_models) {
        v.model = m_models->sample(ModelViewId::PetPaperDoll, ModelFrameGallery::Sample::Wolf,
                                   m_paperDoll.model.facing);
    }
    static constexpr const char* kStatLabels[5] = {
        "Strength:", "Agility:", "Stamina:", "Intellect:", "Spirit:"};
    static constexpr const char* kStatValues[5] = {"58", "41", "52", "22", "35"};
    for (u32 i = 0; i < 5; ++i) {
        v.stats[i] = {kStatLabels[i], kStatValues[i], ui::kWhite, "Strength 58",
                      text::get("DEFAULT_STRENGTH_TOOLTIP")};
    }
    v.combat[0] = {text::get("ATTACK_COLON"), "100", ui::kWhite, text::get("ATTACK_TOOLTIP"), {}};
    v.combat[1] = {text::get("ATTACK_POWER_COLON"), "182", ui::kGreen,
                   "Melee Attack Power 182 (162|cff20ff20+20|r)",
                   text::format(attackPower, "MELEE_ATTACK_POWER_TOOLTIP", 13.0f)};
    v.combat[2] = {text::get("DAMAGE_COLON"), "31 - 37", ui::kWhite, text::get("INVTYPE_WEAPONMAINHAND"),
                   "Attack Speed (seconds): 2.00\nDamage: 31 - 37\nDamage per Second: 17.0"};
    v.combat[3] = {text::get("DEFENSE_COLON"), "100", ui::kWhite, text::get("DEFENSE_TOOLTIP"), {}};
    v.combat[4] = {text::get("ARMOR_COLON"), "820", ui::kWhite, "Armor 820",
                   text::format(armor, "ARMOR_TOOLTIP", 20, 28.2f)};
    v.resistances[0] = {"0", ui::kWhite, text::get("RESISTANCE6_NAME")};
    v.resistances[1] = {"30", ui::kGreen, text::get("RESISTANCE2_NAME")};
    v.resistances[2] = {"0", ui::kWhite, text::get("RESISTANCE3_NAME")};
    v.resistances[3] = {"0", ui::kWhite, text::get("RESISTANCE4_NAME")};
    v.resistances[4] = {"-5", ui::kRed, text::get("RESISTANCE5_NAME")};
    v.xp = 2600;
    v.nextXp = 7400;
    petPaperDoll(c, d.body, v, m_paperDoll);
}

void PetGallery::feedback(DialogContext& c, u64 nowMs) {
    // The player frame in a raid ("Group 3") and its pet's frame, with a sample UNIT_COMBAT every second on
    // each: hits, a crit, a glancing blow, a spell, a heal, misses, an immunity, an absorb.
    ui::Gui g{c.ui, c.draw};
    const Rect& safe = c.safeArea;
    const f32 frameW = g.unit(232.0f), frameH = g.unit(100.0f);
    const Rect player{safe.x, safe.y + g.unit(30.0f), frameW, frameH};
    PlayerFrameView me;
    me.name = "Arthel";
    me.level = 20;
    me.health = 640;
    me.maxHealth = 820;
    me.power = 300;
    me.maxPower = 420;
    playerFrame(g, player, me, nowMs);
    groupIndicator(g, player, 3);
    const f32 s = player.w / 232.0f;
    const Rect pet{player.x + 80.0f * s, player.y + 60.0f * s, 128.0f * s, 64.0f * s};
    PetFrameView petView;
    petView.name = "Fang";
    petView.level = 20;
    petView.health = 0.7f;
    petView.power = 0.4f;
    petView.powerType = 2;
    petView.happiness = 3;
    petFrame(g, pet, petView);
    using E = game::UnitCombatEvent;
    using F = game::UnitCombatFlag;
    static constexpr game::UnitCombat kSamples[] = {
        {0, E::Wound, F::None, 24, 0}, {0, E::Wound, F::Critical, 61, 0}, {0, E::Wound, F::Glancing, 12, 0},
        {0, E::Wound, F::None, 38, 4}, {0, E::Heal, F::None, 120, 0},     {0, E::Heal, F::Critical, 210, 0},
        {0, E::Dodge, F::None, 0, 0},  {0, E::Parry, F::None, 0, 0},      {0, E::Wound, F::Absorb, 0, 0},
        {0, E::Wound, F::Block, 0, 0}, {0, E::Immune, F::None, 0, 0},     {0, E::Miss, F::None, 0, 0},
    };
    const u32 now = static_cast<u32>(nowMs);
    if (nowMs >= m_nextFeedbackMs) {
        m_nextFeedbackMs = nowMs + 1300;
        m_playerFeedback.onCombatEvent(kSamples[m_feedbackStep % std::size(kSamples)], now);
        m_petFeedback.onCombatEvent(kSamples[(m_feedbackStep + 5) % std::size(kSamples)], now);
        ++m_feedbackStep;
    }
    hitIndicator(g, player.x + 73.0f * s, player.y + 42.0f * s, s, m_playerFeedback.view(now));
    const f32 ps = pet.w / 128.0f;
    hitIndicator(g, pet.x + 28.0f * ps, pet.y + 27.0f * ps, ps, m_petFeedback.view(now));
}

} // namespace vd
