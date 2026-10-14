// Parity (auras): the aura buttons' layout and animation as FrameXML has them (app/AuraLayout): BuffFrame's
// rows (weapon enchantments, buffs, debuffs), TargetFrame's buff and debuff rows (TargetDebuffButton_Update),
// the flash of auras about to expire (BuffFrame_OnUpdate), DebuffTypeColor and the time texts (with the baked
// texts: VD_PACKS_DIR; those checks skip without them). Positions in interface units (unit = 1 logical
// pixel).
#include "TestData.h"
#include "app/AuraLayout.h"
#include "core/Text.h"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <vector>

namespace {

using namespace vd;

HudAura buff(f32 remaining = 0.0f) {
    HudAura a;
    a.iconHash = 1;
    a.remaining = remaining;
    return a;
}
HudAura debuff(u8 dispel = 0) {
    HudAura a = buff();
    a.harmful = true;
    a.dispel = dispel;
    return a;
}
HudAura enchant() {
    HudAura a = buff(600.0f);
    a.enchant = true;
    return a;
}

// The place laid out for aura `index`, or nullptr.
const AuraButtonPlace* placeOf(std::span<const AuraButtonPlace> places, u32 index) {
    for (const AuraButtonPlace& p : places) {
        if (p.aura == index) {
            return &p;
        }
    }
    return nullptr;
}

struct Texts {
    bool loaded = test::loadTexts();
    ~Texts() { text::unload(); }
};

} // namespace

TEST_CASE("Aura layout: BuffFrame's rows (enchantments, buffs, debuffs)") {
    std::array<AuraButtonPlace, 32> places{};
    // One enchantment, ten buffs, three debuffs; TemporaryEnchantFrame's TOPRIGHT at (1000, 10).
    std::vector<HudAura> auras{enchant()};
    for (u32 i = 0; i < 10; ++i) {
        auras.push_back(buff());
    }
    for (u32 i = 0; i < 3; ++i) {
        auras.push_back(debuff());
    }
    REQUIRE(buffFrameLayout(auras, 1000.0f, 10.0f, 1.0f, places) == 14);
    const auto at = [&](u32 index) {
        const AuraButtonPlace* p = placeOf(places, index);
        REQUIRE(p);
        return p->icon;
    };
    CHECK(at(0).x == doctest::Approx(970.0f)); // TempEnchant1 at the frame's TOPRIGHT
    CHECK(at(0).y == doctest::Approx(10.0f));
    CHECK(at(0).w == doctest::Approx(30.0f));
    // BuffFrame's TOPRIGHT 5 left of the enchantment frame (32 wide a button).
    CHECK(at(1).x == doctest::Approx(1000.0f - 32.0f - 5.0f - 30.0f));
    CHECK(at(2).x == doctest::Approx(1000.0f - 37.0f - 35.0f - 30.0f));
    CHECK(at(8).x == doctest::Approx(1000.0f - 37.0f - 7.0f * 35.0f - 30.0f));
    CHECK(at(8).y == doctest::Approx(10.0f));
    // BuffButton8: under TempEnchant1, 45 down; the next one 35 left.
    CHECK(at(9).x == doctest::Approx(970.0f));
    CHECK(at(9).y == doctest::Approx(55.0f));
    CHECK(at(10).x == doctest::Approx(935.0f));
    // BuffButton16 (the first debuff): the enchantment frame's TOPRIGHT, 90 down.
    CHECK(at(11).x == doctest::Approx(970.0f));
    CHECK(at(11).y == doctest::Approx(100.0f));
    CHECK(at(13).x == doctest::Approx(900.0f));
    // Touch rectangles tile the rows: 35 wide, 45 high (the duration under each button belongs to it).
    const AuraButtonPlace* first = placeOf(places, 1);
    CHECK(first->hit.w == doctest::Approx(35.0f));
    CHECK(first->hit.h == doctest::Approx(45.0f));
    CHECK(first->hit.right() == doctest::Approx(placeOf(places, 0)->hit.x - 2.0f)); // the enchantment gap
    CHECK(placeOf(places, 9)->hit.y == doctest::Approx(first->hit.bottom()));

    // Two enchantments: the buffs start 69 left (TempEnchant2 35 left of TempEnchant1).
    auras.insert(auras.begin(), enchant());
    REQUIRE(buffFrameLayout(auras, 1000.0f, 10.0f, 1.0f, places) == 15);
    CHECK(at(1).x == doctest::Approx(935.0f));
    CHECK(at(2).x == doctest::Approx(1000.0f - 69.0f - 30.0f));
    // None: the buffs at the edge.
    const std::vector<HudAura> plain{buff(), buff(), debuff()};
    REQUIRE(buffFrameLayout(plain, 1000.0f, 10.0f, 1.0f, places) == 3);
    CHECK(at(0).x == doctest::Approx(970.0f));
    CHECK(at(2).y == doctest::Approx(100.0f));

    // At most 16 buffs, 8 debuffs and 2 enchantments show; unit scales everything.
    std::vector<HudAura> many;
    for (u32 i = 0; i < 3; ++i) {
        many.push_back(enchant());
    }
    for (u32 i = 0; i < 20; ++i) {
        many.push_back(buff());
    }
    for (u32 i = 0; i < 10; ++i) {
        many.push_back(debuff());
    }
    std::array<AuraButtonPlace, 40> all{};
    CHECK(buffFrameLayout(many, 1000.0f, 0.0f, 2.0f, all) == 26);
    CHECK_FALSE(placeOf(std::span(all).first(26), 2));  // the third enchantment
    CHECK_FALSE(placeOf(std::span(all).first(26), 19)); // the seventeenth buff
    CHECK(placeOf(std::span(all).first(26), 18)->icon.w == doctest::Approx(60.0f));
    // Room for fewer: only what fits.
    CHECK(buffFrameLayout(many, 1000.0f, 0.0f, 1.0f, std::span(all).first(4)) == 4);
}

TEST_CASE("Aura layout: TargetFrame's buffs and debuffs") {
    std::array<AuraButtonPlace, 21> places{};
    const ui::Rect frame{100.0f, 0.0f, 232.0f, 100.0f}; // the first row 5 in, 32 above the bottom: (105, 68)
    f32 bottom = 0.0f;
    const auto at = [&](u32 index) {
        const AuraButtonPlace* p = placeOf(places, index);
        REQUIRE(p);
        return p->icon;
    };

    // Hostile, no target of target: debuffs first (21 units, 3 apart), the buffs under the second debuff row.
    const std::vector<HudAura> hostile{buff(), debuff(1), debuff(2), buff(), debuff()};
    REQUIRE(targetAuraLayout(hostile, false, false, frame, 1.0f, places, bottom) == 5);
    CHECK(at(1).x == doctest::Approx(105.0f));
    CHECK(at(1).y == doctest::Approx(68.0f));
    CHECK(at(1).w == doctest::Approx(21.0f));
    CHECK(at(2).x == doctest::Approx(129.0f));
    CHECK(at(4).x == doctest::Approx(153.0f));
    // TargetFrameBuff1 under TargetFrameDebuff7 (the second row: 68 + 21 + 2, 17 high, then 2).
    CHECK(at(0).x == doctest::Approx(105.0f));
    CHECK(at(0).y == doctest::Approx(110.0f));
    CHECK(at(3).x == doctest::Approx(129.0f));
    CHECK(bottom == doctest::Approx(131.0f));

    // Friendly: buffs first, the debuffs under them.
    const std::vector<HudAura> friendly{buff(), buff(), debuff()};
    REQUIRE(targetAuraLayout(friendly, true, false, frame, 1.0f, places, bottom) == 3);
    CHECK(at(0).y == doctest::Approx(68.0f));
    CHECK(at(1).x == doctest::Approx(129.0f));
    CHECK(at(2).y == doctest::Approx(91.0f));
    CHECK(bottom == doctest::Approx(112.0f));

    // With a target of target the debuffs wrap after 5; seven of them make every button 17; the buffs go
    // under TargetFrameDebuff6.
    std::vector<HudAura> crowded{buff()};
    for (u32 i = 0; i < 7; ++i) {
        crowded.push_back(debuff());
    }
    REQUIRE(targetAuraLayout(crowded, false, true, frame, 1.0f, places, bottom) == 8);
    CHECK(at(1).w == doctest::Approx(17.0f));
    CHECK(at(5).x == doctest::Approx(105.0f + 4.0f * 20.0f));
    CHECK(at(6).x == doctest::Approx(105.0f)); // TargetFrameDebuff6 starts the second row
    CHECK(at(6).y == doctest::Approx(87.0f));
    CHECK(at(7).x == doctest::Approx(125.0f));
    CHECK(at(0).y == doctest::Approx(106.0f));
    CHECK(at(0).w == doctest::Approx(17.0f));

    // Without it, ten debuffs: the second row holds the other four (and up to ten), Debuff11 follows
    // Debuff10.
    std::vector<HudAura> ten;
    for (u32 i = 0; i < 11; ++i) {
        ten.push_back(debuff());
    }
    REQUIRE(targetAuraLayout(ten, false, false, frame, 1.0f, places, bottom) == 11);
    CHECK(at(5).x == doctest::Approx(105.0f + 5.0f * 20.0f)); // six in the first row
    CHECK(at(6).x == doctest::Approx(105.0f));
    CHECK(at(6).y == doctest::Approx(87.0f));
    CHECK(at(10).x == doctest::Approx(105.0f + 4.0f * 20.0f));
    CHECK(at(10).y == doctest::Approx(87.0f));
    CHECK(bottom == doctest::Approx(104.0f));
    // With a target of target and ten or more debuffs: a third row from Debuff11, the buffs under it.
    std::vector<HudAura> third = ten;
    third.push_back(buff());
    REQUIRE(targetAuraLayout(third, false, true, frame, 1.0f, places, bottom) == 12);
    CHECK(at(10).x == doctest::Approx(105.0f));
    CHECK(at(10).y == doctest::Approx(106.0f));
    CHECK(at(11).y == doctest::Approx(125.0f));

    // A few debuffs with a target of target: 21 units; the buffs under the (empty) second row of 21.
    const std::vector<HudAura> few{debuff(), buff()};
    REQUIRE(targetAuraLayout(few, false, true, frame, 1.0f, places, bottom) == 2);
    CHECK(at(0).w == doctest::Approx(21.0f));
    CHECK(at(1).y == doctest::Approx(68.0f + 23.0f + 23.0f));
    // ...but five buffs with a target of target shrink everything.
    std::vector<HudAura> fiveBuffs{debuff()};
    for (u32 i = 0; i < 7; ++i) {
        fiveBuffs.push_back(buff());
    }
    REQUIRE(targetAuraLayout(fiveBuffs, false, true, frame, 1.0f, places, bottom) == 6); // 5 buffs at most
    CHECK(at(0).w == doctest::Approx(17.0f));
    CHECK_FALSE(placeOf(std::span(places).first(6), 6));

    // No auras: the frame's bottom.
    CHECK(targetAuraLayout({}, false, false, frame, 1.0f, places, bottom) == 0);
    CHECK(bottom == doctest::Approx(100.0f));
    // Weapon enchantments are not the target's.
    const std::vector<HudAura> odd{enchant(), debuff()};
    REQUIRE(targetAuraLayout(odd, false, false, frame, 1.0f, places, bottom) == 1);
    CHECK(places[0].aura == 1);
}

TEST_CASE("Aura layout: the flash of auras about to expire, the debuff colours") {
    // BUFF_ALPHA_VALUE: up from 0.3 to 1 in 0.75 s, back down in 0.75 s.
    CHECK(buffFlashAlpha(0) == doctest::Approx(0.3f));
    CHECK(buffFlashAlpha(375) == doctest::Approx(0.65f));
    CHECK(buffFlashAlpha(750) == doctest::Approx(1.0f));
    CHECK(buffFlashAlpha(1125) == doctest::Approx(0.65f));
    CHECK(buffFlashAlpha(1500) == doctest::Approx(0.3f));
    CHECK(buffFlashAlpha(1500 * 1000 + 750) == doctest::Approx(1.0f));
    // Only under BUFF_WARNING_TIME (31 s), and only with a known duration.
    CHECK(auraButtonAlpha(40.0f, 375) == doctest::Approx(1.0f));
    CHECK(auraButtonAlpha(31.0f, 375) == doctest::Approx(1.0f));
    CHECK(auraButtonAlpha(30.9f, 375) == doctest::Approx(0.65f));
    CHECK(auraButtonAlpha(2.0f, 0) == doctest::Approx(0.3f));
    CHECK(auraButtonAlpha(0.0f, 375) == doctest::Approx(1.0f));
    // The duration turns white in its last minute.
    CHECK(auraDurationColor(59.0f) == ui::rgba(255, 255, 255));
    CHECK(auraDurationColor(60.0f) == ui::rgba(255, 209, 0));

    // DebuffTypeColor.
    CHECK(debuffTypeColor(0) == ui::rgba(204, 0, 0));
    CHECK(debuffTypeColor(1) == ui::rgba(51, 153, 255));
    CHECK(debuffTypeColor(2) == ui::rgba(153, 0, 255));
    CHECK(debuffTypeColor(3) == ui::rgba(153, 102, 0));
    CHECK(debuffTypeColor(4) == ui::rgba(0, 153, 0));
    CHECK(debuffTypeColor(9) == ui::rgba(204, 0, 0)); // Frenzy and the rest: "none"
    CHECK(hasDebuffType(4));
    CHECK_FALSE(hasDebuffType(0));
    CHECK_FALSE(hasDebuffType(5));
}

TEST_CASE("Aura layout: the time texts (SecondsToTimeAbbrev, SPELL_TIME_REMAINING_*)") {
    const Texts texts;
    if (!texts.loaded) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    char out[64];
    const auto abbrev = [&](f32 seconds) { return std::string(secondsToTimeAbbrev(out, seconds)); };
    CHECK(abbrev(0.4f) == "0 s");
    CHECK(abbrev(59.9f) == "59 s");
    CHECK(abbrev(60.0f) == "60 s");
    CHECK(abbrev(61.0f) == "2 m");
    CHECK(abbrev(1795.0f) == "30 m");
    CHECK(abbrev(3601.0f) == "2 h");
    CHECK(abbrev(90000.0f) == "2 d");
    const auto left = [&](f32 seconds) { return std::string(auraTimeRemaining(out, seconds)); };
    CHECK(left(1795.0f) == "30 minutes remaining");
    CHECK(left(61.0f) == "2 minutes remaining");
    CHECK(left(60.0f) == "60 seconds remaining");
    CHECK(left(1.5f) == "1 second remaining");
    CHECK(left(5400.0f) == "2 hours remaining");
    CHECK(left(86401.0f) == "2 days remaining");
}
