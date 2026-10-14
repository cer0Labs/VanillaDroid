#include "app/AuraGallery.h"

#include "app/CombatWindows.h"
#include "app/SocialWindows.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;

constexpr u64 kTargetingFrame = assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame.blp");
constexpr u64 kTargetOfTarget = assetPathHash("Interface\\TargetingFrame\\UI-TargetofTargetFrame.blp");
constexpr u64 kLevelBackground =
    assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame-LevelBackground.blp");
constexpr u64 kStatusBar = assetPathHash("Interface\\TargetingFrame\\UI-StatusBar.blp");
constexpr u64 kMonsterPortrait = assetPathHash("Interface\\CharacterFrame\\TemporaryPortrait-Monster.blp");
constexpr u64 kHumanPortrait = assetPathHash("Interface\\CharacterFrame\\TemporaryPortrait-Male-Human.blp");
constexpr u64 kCircleMask = assetPathHash("Interface\\CharacterFrame\\TempPortraitAlphaMask.blp");

u64 icon(const char* name) {
    char path[96];
    std::snprintf(path, sizeof(path), "Interface\\Icons\\%s.blp", name);
    return assetPathHash(path);
}

// A sample aura (names and texts as Spell.dbc has them; `seconds` counts down from the page's start and
// starts over).
struct Sample {
    const char* icon;
    f32 seconds; // 0: no time shown
    u32 stacks;
    bool harmful;
    u8 dispel;
    bool enchant;
    bool cancelable;
    const char* name;
    const char* text;
    const char* type;
};

// The player: the weapons' enchantments (off hand first), ten buffs, four debuffs.
constexpr Sample kPlayer[] = {
    {"INV_Weapon_ShortBlade_05", 26.0f, 0, false, 0, true, false, "Instant Poison II",
     "Strikes may poison the target for 50 Nature damage (a 20% chance each).", ""},
    {"INV_Sword_04", 1790.0f, 0, false, 0, true, false, "Sharpened (+3 Damage)", "Increases damage by 3.",
     ""},
    {"Spell_Holy_WordFortitude", 1795.0f, 0, false, 0, false, true, "Power Word: Fortitude",
     "Increases Stamina by 32.", ""},
    {"Spell_Holy_MagicalSentry", 1640.0f, 0, false, 0, false, true, "Arcane Intellect",
     "Increases Intellect by 15.", ""},
    {"Ability_Warrior_BattleShout", 95.0f, 0, false, 0, false, true, "Battle Shout",
     "Increases attack power by 55.", ""},
    {"Spell_Holy_Devotion", 0.0f, 0, false, 0, false, true, "Devotion Aura", "Increases armor by 275.", ""},
    {"Spell_Frost_FrostArmor02", 1700.0f, 0, false, 0, false, true, "Frost Armor",
     "Armor raised by 110. Melee attackers may be slowed down.", ""},
    {"Ability_Racial_BloodRage", 14.0f, 0, false, 0, false, true, "Blood Fury",
     "Attack power raised by 25%, healing received halved.", ""},
    {"Ability_Tracking", 0.0f, 0, false, 0, false, true, "Track Beasts",
     "Nearby beasts appear on the minimap.", ""},
    {"Ability_Defend", 4.0f, 0, false, 0, false, true, "Shield Block", "Block chance raised by 75%.", ""},
    {"Spell_Holy_Heal", 12.0f, 0, false, 0, false, true, "Renew", "Restores 45 health every 3 sec.", ""},
    {"INV_Potion_51", 3500.0f, 0, false, 0, false, false, "Elixir of Fortitude",
     "Maximum health raised by 120.", ""},
    {"Spell_Frost_FrostNova", 6.0f, 0, true, 1, false, false, "Frost Nova", "Frozen in place.", "Magic"},
    {"Spell_Shadow_CurseOfSargeras", 22.0f, 0, true, 2, false, false, "Curse of Agony",
     "Takes 18 Shadow damage every 2 sec.", "Curse"},
    {"Spell_Nature_CorrosiveBreath", 10.0f, 3, true, 4, false, false, "Deadly Poison",
     "Takes 9 Nature damage every 3 sec.", "Poison"},
    {"Ability_Gouge", 4.0f, 0, true, 0, false, false, "Gouge", "Incapacitated.", ""},
};
// A hostile target with a target of target: seven debuffs (the rows wrap at 5), three buffs under them.
constexpr Sample kHostile[] = {
    {"Ability_Warrior_BattleShout", 0.0f, 0, false, 0, false, false, "Battle Shout",
     "Increases attack power by 55.", ""},
    {"Spell_Holy_WordFortitude", 0.0f, 0, false, 0, false, false, "Power Word: Fortitude",
     "Increases Stamina by 32.", ""},
    {"Ability_Defend", 0.0f, 0, false, 0, false, false, "Shield Block", "Block chance raised by 75%.", ""},
    {"Spell_Frost_FrostNova", 0.0f, 0, true, 1, false, false, "Frost Nova", "Frozen in place.", "Magic"},
    {"Spell_Shadow_ShadowWordPain", 0.0f, 0, true, 1, false, false, "Shadow Word: Pain",
     "Takes 18 Shadow damage every 3 sec.", "Magic"},
    {"Spell_Shadow_CurseOfSargeras", 0.0f, 0, true, 2, false, false, "Curse of Agony",
     "Takes 18 Shadow damage every 2 sec.", "Curse"},
    {"Spell_Nature_CorrosiveBreath", 0.0f, 5, true, 4, false, false, "Deadly Poison",
     "Takes 9 Nature damage every 3 sec.", "Poison"},
    {"Ability_GhoulFrenzy", 0.0f, 0, true, 3, false, false, "Infected Wound", "Takes more physical damage.",
     "Disease"},
    {"Ability_Gouge", 0.0f, 0, true, 0, false, false, "Gouge", "Incapacitated.", ""},
    {"Spell_Fire_Immolation", 0.0f, 0, true, 1, false, false, "Immolate", "Takes 15 Fire damage every 3 sec.",
     "Magic"},
};
// A friendly target: its buffs first, its debuffs under them.
constexpr Sample kFriendly[] = {
    {"Spell_Holy_WordFortitude", 0.0f, 0, false, 0, false, false, "Power Word: Fortitude",
     "Increases Stamina by 32.", ""},
    {"Spell_Holy_MagicalSentry", 0.0f, 0, false, 0, false, false, "Arcane Intellect",
     "Increases Intellect by 15.", ""},
    {"Spell_Holy_Devotion", 0.0f, 0, false, 0, false, false, "Devotion Aura", "Increases armor by 275.", ""},
    {"Spell_Holy_Heal", 0.0f, 0, false, 0, false, false, "Renew", "Restores 45 health every 3 sec.", ""},
    {"Spell_Shadow_CurseOfSargeras", 0.0f, 0, true, 2, false, false, "Curse of Agony",
     "Takes 18 Shadow damage every 2 sec.", "Curse"},
    {"Spell_Nature_CorrosiveBreath", 0.0f, 2, true, 4, false, false, "Deadly Poison",
     "Takes 9 Nature damage every 3 sec.", "Poison"},
};
// The pet's buffs (the client borders them in the "none" red).
constexpr Sample kPet[] = {
    {"Spell_Holy_WordFortitude", 0.0f, 0, false, 0, false, false, "Power Word: Fortitude",
     "Increases Stamina by 32.", ""},
    {"Ability_Warrior_BattleShout", 0.0f, 0, false, 0, false, false, "Battle Shout",
     "Increases attack power by 55.", ""},
    {"Ability_Racial_BloodRage", 0.0f, 0, false, 0, false, false, "Frenzy", "Attack speed raised by 30%.",
     ""},
};
// A party member's list (its long press).
constexpr Sample kMember[] = {
    {"Spell_Holy_WordFortitude", 0.0f, 0, false, 0, false, false, "Power Word: Fortitude",
     "Increases Stamina by 32.", ""},
    {"Spell_Holy_MagicalSentry", 0.0f, 0, false, 0, false, false, "Arcane Intellect",
     "Increases Intellect by 15.", ""},
    {"Spell_Holy_Devotion", 0.0f, 0, false, 0, false, false, "Devotion Aura", "Increases armor by 275.", ""},
    {"Ability_Warrior_BattleShout", 0.0f, 0, false, 0, false, false, "Battle Shout",
     "Increases attack power by 55.", ""},
    {"Spell_Frost_FrostArmor02", 0.0f, 0, false, 0, false, false, "Frost Armor", "Increases armor by 110.",
     ""},
    {"Spell_Frost_FrostNova", 0.0f, 0, true, 1, false, false, "Frost Nova", "Frozen in place.", "Magic"},
    {"Spell_Nature_CorrosiveBreath", 0.0f, 0, true, 4, false, false, "Deadly Poison",
     "Takes 9 Nature damage every 3 sec.", "Poison"},
    {"Ability_Gouge", 0.0f, 0, true, 0, false, false, "Gouge", "Incapacitated.", ""},
};
struct List {
    const Sample* samples;
    usize count;
};
constexpr List kLists[] = {{kPlayer, std::size(kPlayer)},
                           {kHostile, std::size(kHostile)},
                           {kFriendly, std::size(kFriendly)},
                           {kPet, std::size(kPet)},
                           {kMember, std::size(kMember)}};

// The views of a list (the player's without what was cancelled); `map` gets each view's sample.
u32 views(u32 list, u64 elapsedMs, const std::array<bool, 16>& cancelled, std::span<HudAura> out,
          std::span<u32> map) {
    u32 n = 0;
    for (u32 i = 0; i < kLists[list].count && n < out.size(); ++i) {
        if (list == 0 && i < cancelled.size() && cancelled[i]) {
            continue;
        }
        const Sample& s = kLists[list].samples[i];
        HudAura& a = out[n];
        a = HudAura{};
        a.iconHash = icon(s.icon);
        if (s.seconds > 0.0f) {
            // Counting down, then starting over (flashing under 31 s, white under a minute).
            const f32 t = std::fmod(static_cast<f32>(elapsedMs) / 1000.0f, s.seconds);
            a.remaining = std::max(0.5f, s.seconds - t);
        }
        a.stacks = s.stacks;
        a.harmful = s.harmful;
        a.dispel = s.dispel;
        a.enchant = s.enchant;
        map[n++] = i;
    }
    return n;
}

// The TargetingFrame art (mirrored for the player), its bars and name, as app/Hud draws it.
void unitFrame(ui::Gui& g, const Rect& r, bool player, const char* name, u32 nameColor) {
    const f32 u = g.unit(1.0f);
    const f32 barX = player ? 106.0f : 7.0f;
    g.draw.solid({r.x + barX * u, r.y + 22.0f * u, 119.0f * u, 41.0f * u}, rgba(0, 0, 0, 128));
    if (!player) {
        g.draw.sprite({r.x + 7.0f * u, r.y + 22.0f * u, 119.0f * u, 19.0f * u}, kLevelBackground, nameColor);
    }
    g.draw.portrait({r.x + (player ? 42.0f : 126.0f) * u, r.y + 12.0f * u, 64.0f * u, 64.0f * u}, {},
                    player ? kHumanPortrait : kMonsterPortrait);
    g.draw.sprite({r.x + barX * u, r.y + 41.0f * u, 119.0f * u * 0.7f, 12.0f * u}, kStatusBar,
                  rgba(0, 255, 0), {0.0f, 0.0f, 0.7f, 1.0f});
    g.draw.sprite({r.x + barX * u, r.y + 52.0f * u, 119.0f * u * 0.5f, 12.0f * u}, kStatusBar,
                  rgba(0, 0, 255), {0.0f, 0.0f, 0.5f, 1.0f});
    g.draw.sprite(r, kTargetingFrame, ui::kWhite,
                  player ? ui::TexCoords{1.0f, 0.0f, 0.09375f, 0.78125f}
                         : ui::TexCoords{0.09375f, 0.0f, 1.0f, 0.78125f});
    ui::TextStyle label = ui::gameFont(12.5f * u, player ? ui::kGold : ui::kWhite);
    label.align = ui::Align::Center;
    ui::label(g, {r.x + (barX + 4.0f) * u, r.y + 23.0f * u, 111.0f * u, 17.0f * u}, name, label);
}

} // namespace

void AuraGallery::build(u64 nowMs) {
    m_built = true;
    m_startMs = nowMs;
}

void AuraGallery::draw(DialogContext& c, u64 nowMs) {
    if (!m_built) {
        build(nowMs);
    }
    ui::Gui g{c.ui, c.draw};
    const Rect& safe = c.safeArea;
    const f32 u = g.unit(1.0f);
    const u64 elapsed = nowMs - m_startMs;
    std::array<HudAura, 16> auras{};
    std::array<u32, 16> map{};
    std::array<AuraButtonPlace, 26> places{};
    i32 tapList = -1, tapIndex = -1, holdList = -1, holdIndex = -1;
    Rect tapRect;
    const auto result = [&](u32 list, const AuraButtonsResult& r) {
        // The tooltip hangs under the rows (as app/HudAuras reports the button).
        const Rect anchor = list == 3 ? r.rect : Rect{r.rect.x, r.rect.y, r.rect.w, r.bottom - r.rect.y};
        if (r.tapped >= 0) {
            tapList = static_cast<i32>(list);
            tapIndex = static_cast<i32>(map[static_cast<u32>(r.tapped)]);
            tapRect = anchor;
        }
        if (r.held >= 0) {
            holdList = static_cast<i32>(list);
            holdIndex = static_cast<i32>(map[static_cast<u32>(r.held)]);
            tapRect = anchor;
        }
    };

    // The player frame and the pet frame under it (as app/Hud and app/CombatScreens place them), its buffs on
    // it.
    const Rect playerFrame{safe.x, safe.y, 232.0f * u, 100.0f * u};
    unitFrame(g, playerFrame, true, "Arthel", ui::kGold);
    const Rect petRect{playerFrame.x + 80.0f * u, playerFrame.y + 60.0f * u, 128.0f * u, 64.0f * u};
    PetFrameView pet;
    pet.name = "Wolf";
    pet.level = 12;
    pet.health = 0.8f;
    pet.power = 0.6f;
    pet.powerType = 2;
    pet.happiness = 3;
    petFrame(g, petRect, pet);
    {
        const u32 n = views(3, elapsed, m_cancelled, auras, map);
        const f32 k = petRect.w / 128.0f;
        for (u32 i = 0; i < n; ++i) {
            places[i].aura = i;
            places[i].icon = {petRect.x + (48.0f + static_cast<f32>(i) * 17.0f) * k, petRect.y + 42.0f * k,
                              15.0f * k, 15.0f * k};
            places[i].hit = places[i].icon.inset(-1.0f * k);
        }
        result(3, auraButtons(c.ui, c.draw, c.resources, {auras.data(), n}, {places.data(), n}, k, nowMs,
                              AuraButtonStyle::Small, "gallery.pet", true));
    }

    // Two party members under the pet: debuffs, one glowing (a typed debuff, its auras changed lately).
    std::array<PartyFrameView, 2> party{};
    party[0].name = "Ugha";
    party[0].level = 11;
    party[0].health = 0.45f;
    party[0].power = 0.3f;
    party[0].powerType = 1;
    party[0].race = 2;
    party[0].leader = true;
    party[0].debuffs = {{{icon("Spell_Frost_FrostNova"), 1},
                         {icon("Spell_Nature_CorrosiveBreath"), 4},
                         {icon("Ability_Gouge"), 0},
                         {}}};
    party[0].debuffCount = 3;
    party[0].statusColor = debuffTypeColor(1);
    party[0].statusAlpha = buffFlashAlpha(nowMs);
    party[1].name = "Tinka";
    party[1].level = 9;
    party[1].health = 0.9f;
    party[1].power = 0.7f;
    party[1].race = 7;
    party[1].gender = 1;
    party[1].debuffs[0] = {icon("Ability_GhoulFrenzy"), 3};
    party[1].debuffCount = 1;
    i32 held = -1;
    Rect heldFrame;
    const Rect partyArea{safe.x + g.px(4.0f), petRect.bottom(), g.unit(128.0f) * 1.6f, 0.0f};
    partyFrames(c, partyArea, party, &held, &heldFrame);
    if (held >= 0) {
        m_listMember = held;
        m_listFrame = heldFrame;
        m_tipList = -1;
        VD_LOG_INFO("UI gallery: %s's auras", held == 0 ? "Ugha" : "Tinka");
    }

    // A hostile target with a target of target (debuffs first, rows of 5), a friendly one under it (buffs
    // first).
    const Rect hostile{safe.x + 250.0f * u, safe.y, 232.0f * u, 100.0f * u};
    unitFrame(g, hostile, false, "Defias Pillager", rgba(255, 0, 0, 230));
    const Rect tot{hostile.right() - 30.0f * u, hostile.y + 64.0f * u, 93.0f * u, 34.0f * u};
    g.draw.solid({tot.x + 30.0f * u, tot.y + 6.0f * u, 58.0f * u, 14.0f * u}, rgba(0, 0, 0, 160));
    g.draw.sprite({tot.x + 30.0f * u, tot.y + 16.0f * u, 58.0f * u, 5.0f * u}, kStatusBar, rgba(0, 255, 0));
    g.draw.sprite(tot, kTargetOfTarget, ui::kWhite, {0.015625f, 0.0f, 0.7421875f, 0.703125f});
    f32 bottom = hostile.bottom();
    {
        const u32 n = views(1, elapsed, m_cancelled, auras, map);
        const u32 placed = targetAuraLayout({auras.data(), n}, false, true, hostile, u, places, bottom);
        result(1, auraButtons(c.ui, c.draw, c.resources, {auras.data(), n}, {places.data(), placed}, u, nowMs,
                              AuraButtonStyle::Target, "gallery.hostile"));
    }
    const Rect friendly{hostile.x, bottom + 16.0f * u, 232.0f * u, 100.0f * u};
    unitFrame(g, friendly, false, "Stormwind Guard", rgba(0, 255, 0, 230));
    {
        const u32 n = views(2, elapsed, m_cancelled, auras, map);
        const u32 placed = targetAuraLayout({auras.data(), n}, true, false, friendly, u, places, bottom);
        result(2, auraButtons(c.ui, c.draw, c.resources, {auras.data(), n}, {places.data(), placed}, u, nowMs,
                              AuraButtonStyle::Target, "gallery.friendly"));
    }

    // The minimap's place (a dark disc) and the player's BuffFrame left of it, as app/HudAuras lays it out.
    const f32 clusterLeft = safe.right() - g.px(4.0f) - 192.0f * u, clusterTop = safe.y + g.px(2.0f);
    g.draw.sprite({clusterLeft + 35.0f * u, clusterTop + 22.0f * u, 140.0f * u, 140.0f * u}, kCircleMask,
                  rgba(0, 0, 0, 150));
    {
        const u32 n = views(0, elapsed, m_cancelled, auras, map);
        const u32 placed = buffFrameLayout({auras.data(), n}, safe.right() - g.px(4.0f) - 175.0f * u,
                                           clusterTop + 13.0f * u, u, places);
        result(0, auraButtons(c.ui, c.draw, c.resources, {auras.data(), n}, {places.data(), placed}, u, nowMs,
                              AuraButtonStyle::Player, "gallery.player"));
    }

    // What the finger did: a long press cancels one of the player's cancelable buffs; anything else shows a
    // tooltip.
    if (holdList == 0 && holdIndex >= 0 && kPlayer[holdIndex].cancelable) {
        m_cancelled[static_cast<u32>(holdIndex)] = true;
        VD_LOG_INFO("UI gallery: cancel %s", kPlayer[holdIndex].name);
        if (m_tipList == 0 && m_tipIndex == holdIndex) {
            m_tipList = -1;
        }
    } else if (holdList >= 0) {
        tapList = holdList;
        tapIndex = holdIndex;
    }
    if (tapList >= 0) {
        if (m_tipList == tapList && m_tipIndex == tapIndex) {
            m_tipList = -1; // a second tap closes it
        } else {
            m_tipList = tapList;
            m_tipIndex = tapIndex;
            m_tipAnchor = tapRect;
            m_tipAt = tapList == 0   ? AuraTooltipAnchor::BottomLeft
                      : tapList <= 2 ? AuraTooltipAnchor::BottomRight
                                     : AuraTooltipAnchor::Right;
            m_listMember = tapList == 4 ? m_listMember : -1;
        }
    }
    const auto tappedElsewhere = [&](const Rect& a, const Rect& b) {
        for (const ui::Touch& t : c.ui.touches()) {
            if (t.released && !t.moved && !t.longPressed && !a.contains(t.x, t.y) && !b.contains(t.x, t.y)) {
                return true;
            }
        }
        return false;
    };

    // The party member's list (PartyMemberBuffTooltip).
    if (m_listMember >= 0) {
        if (tappedElsewhere(m_listPanel, m_tipList == 4 ? m_tipPanel : Rect{})) {
            m_listMember = -1;
            m_tipList = m_tipList == 4 ? -1 : m_tipList;
        } else {
            const u32 n = views(4, elapsed, m_cancelled, auras, map);
            const AuraListResult r =
                auraList(c, {{auras.data(), n}, m_listFrame, m_listFrame.w / 120.0f}, nowMs);
            m_listPanel = r.panel;
            if (r.tapped >= 0) {
                m_tipList = 4;
                m_tipIndex = static_cast<i32>(map[static_cast<u32>(r.tapped)]);
                m_tipAnchor = r.rect;
                m_tipAt = AuraTooltipAnchor::Right;
            }
        }
    }
    // The tooltip.
    if (m_tipList >= 0 && tappedElsewhere(m_tipPanel, m_tipAnchor)) {
        m_tipList = -1;
    }
    if (m_tipList >= 0) {
        const Sample& s = kLists[m_tipList].samples[m_tipIndex];
        AuraTooltipView v;
        v.name = s.name;
        v.type = s.type;
        v.description = s.text;
        if (m_tipList == 0 && s.seconds > 0.0f) {
            const f32 t = std::fmod(static_cast<f32>(elapsed) / 1000.0f, s.seconds);
            v.timeLeft = auraTimeRemaining(m_timeLeft, std::max(0.5f, s.seconds - t));
        }
        v.anchor = m_tipAnchor;
        v.at = m_tipAt;
        const AuraTooltipResult r = auraTooltip(c, v);
        m_tipPanel = r.panel;
        if (r.tapped) {
            m_tipList = -1;
        }
    }
}

} // namespace vd
