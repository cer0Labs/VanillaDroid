#include "app/UnitsGallery.h"

#include "app/HudShared.h"
#include "app/UnitFrames.h"
#include "app/UnitMenuScreens.h"
#include "app/UnitMenuWindows.h"
#include "app/UnitTooltip.h"
#include "core/Log.h"
#include "core/Text.h"
#include "game/CharacterNames.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

namespace uf = net::uf;
using game::ObjectType;
using ui::Rect;

// Faction templates as FactionTemplate.dbc words them (own group, friends, enemies: 1 players, 2 Alliance, 4
// Horde, 8 monsters).
constexpr u32 kAlliancePlayer = 1, kHordePlayer = 2, kStormwind = 11, kMonster = 14, kNeutral = 7;

struct Sample {
    const char* name;
    ObjectType type;
    u32 level;
    u32 faction;
    u32 rank = 0;         // creature_template.rank
    u32 creatureType = 7; // CreatureType.dbc (7 humanoid)
    u8 race = 0, cls = 0, gender = 0;
    u32 unitFlags = 0;
    u32 dynamicFlags = 0;
    u32 healthPercent = 100;
    u32 playerFlags = 0;
    const char* subname = "";
    const char* guild = "";
};

constexpr u32 kPvp = game::kUnitFlagPvp;
// The viewer: a level 12 human warrior.
constexpr Sample kMe{"Arthel", ObjectType::Player, 12, kAlliancePlayer, 0, 7, 1, 1, 0};
// The target frames, in the page's order (the first six have tooltips too).
const Sample kSamples[] = {
    {"Hogger", ObjectType::Unit, 11, kMonster, 1, 7, 0, 0, 0, 0, 0, 62},
    {"Aeryn", ObjectType::Player, 14, kAlliancePlayer, 0, 7, 4, 5, 1, kPvp, 0, 100, 0, "", "Los Errantes"},
    {"Onyxia", ObjectType::Unit, 63, kMonster, 3, 2},
    {"Timber Wolf", ObjectType::Unit, 7, kMonster, 0, 1, 0, 0, 0, game::kUnitFlagSkinnable, 0, 0},
    {"Ugha", ObjectType::Player, 10, kHordePlayer, 0, 7, 2, 1, 0},
    {"Innkeeper Farley", ObjectType::Unit, 30, kStormwind, 0, 7, 0, 0, 0, kPvp, 0, 100, 0, "Innkeeper"},
    {"Gorefang", ObjectType::Unit, 13, kMonster, 4, 1},
    {"Mor'Ladim", ObjectType::Unit, 35, kMonster, 2, 6},
    {"Kobold Vermin", ObjectType::Unit, 2, kMonster, 0, 7, 0, 0, 0, 0, game::kDynFlagTapped, 40},
    {"Brakk", ObjectType::Player, 14, kHordePlayer, 0, 7, 6, 1, 0, kPvp, 0, 85},
    {"Tinka", ObjectType::Player, 9, kAlliancePlayer, 0, 7, 7, 8, 1, kPvp},
    {"Stormwind City Guard", ObjectType::Unit, 55, kStormwind, 0, 7, 0, 0, 0, kPvp},
};
constexpr u32 kSampleCount = sizeof(kSamples) / sizeof(kSamples[0]);

game::Entity makeUnit(u64 guid, const Sample& s) {
    game::Entity e;
    e.guid = guid;
    e.type = s.type;
    e.values.assign(s.type == ObjectType::Player ? uf::PLAYER_END : uf::UNIT_END, 0);
    const u32 maxHealth = 100 + s.level * 20;
    e.values[uf::UNIT_FIELD_LEVEL] = s.level;
    e.values[uf::UNIT_FIELD_FACTIONTEMPLATE] = s.faction;
    e.values[uf::UNIT_FIELD_FLAGS] = s.unitFlags;
    e.values[uf::UNIT_DYNAMIC_FLAGS] = s.dynamicFlags;
    e.values[uf::UNIT_FIELD_MAXHEALTH] = maxHealth;
    e.values[uf::UNIT_FIELD_HEALTH] = maxHealth * s.healthPercent / 100;
    const u8 power = s.cls == 1 ? 1 : 0;
    e.values[uf::UNIT_FIELD_BYTES_0] =
        u32{s.race} | (u32{s.cls} << 8) | (u32{s.gender} << 16) | (u32{power} << 24);
    e.values[static_cast<u16>(uf::UNIT_FIELD_MAXPOWER1 + power)] = power == 1 ? 1000 : maxHealth;
    e.values[static_cast<u16>(uf::UNIT_FIELD_POWER1 + power)] = power == 1 ? 250 : maxHealth * 3 / 4;
    if (s.type == ObjectType::Player) {
        e.values[uf::PLAYER_FLAGS] = s.playerFlags;
    }
    return e;
}

game::UnitNames namesOf(const Sample& s) {
    game::UnitNames names;
    names.name = s.name;
    names.subname = s.subname;
    names.guild = s.guild;
    names.rank = s.rank;
    if (s.type == ObjectType::Player) {
        names.race = game::raceName(s.race);
        names.cls = game::className(s.cls);
    } else if (s.creatureType != game::kCreatureTypeNotSpecified) {
        names.creatureType = game::creatureTypeName(s.creatureType);
    }
    return names;
}

} // namespace

void UnitsGallery::build() {
    m_built = true;
    m_reactions.setTemplate(kAlliancePlayer, 1, 0x3, 0x2, 0xC);
    m_reactions.setTemplate(kHordePlayer, 2, 0x5, 0x4, 0xA);
    m_reactions.setTemplate(kStormwind, 72, 0x2, 0x2, 0xC);
    m_reactions.setTemplate(kMonster, 14, 0x8, 0x0, 0x1);
    m_reactions.setTemplate(kNeutral, 7, 0x0, 0x0, 0x0);
    m_units.clear();
    m_units.push_back(makeUnit(0x11, kMe));
    for (u32 i = 0; i < kSampleCount; ++i) {
        m_units.push_back(makeUnit(0x100 + i, kSamples[i]));
    }
    const game::Entity& me = m_units[0];
    for (u32 i = 0; i < m_tooltips.size(); ++i) {
        game::buildUnitTooltip(me, m_units[1 + i], namesOf(kSamples[i]), &m_reactions, m_tooltips[i]);
    }
}

void UnitsGallery::draw(DialogContext& c, Page page, u64 nowMs) {
    if (!m_built) {
        build();
    }
    ui::Gui g{c.ui, c.draw};
    const Rect& safe = c.safeArea;
    const game::Entity& me = m_units[0];
    const f32 frameW = g.unit(232.0f), frameH = g.unit(100.0f);

    if (page == Page::Menu || page == Page::MenuSelf || page == Page::MenuCreature) {
        // Phase 13: the frames' menus (app/UnitMenuScreens' DropDownList with game/UnitPopup's rules), hanging
        // where ToggleDropDownMenu anchors them: the player frame's (106, 27), the target frame's (120, 10).
        PlayerFrameView player;
        player.name = kMe.name;
        player.level = kMe.level;
        player.health = player.maxHealth = 340;
        player.leader = page != Page::MenuSelf;
        playerFrame(g, {safe.x, safe.y, frameW, frameH}, player, nowMs);
        const u32 sample = page == Page::MenuCreature ? 0u : 1u; // Hogger, or Aeryn of the party
        const Sample& s = kSamples[sample];
        TargetFrameView target;
        target.name = s.name;
        target.level = static_cast<i32>(s.level);
        target.levelColor = ui::kGold;
        target.nameBackground = game::targetNameBackground(sample == 0 ? game::NameColor::Hostile
                                                                       : game::NameColor::FriendlyPlayer);
        target.health = 62;
        target.maxHealth = 100;
        target.portraitFallback = sample == 0 ? hud::kMonsterPortrait : hud::racePortrait(s.race, s.gender);
        target.raidTarget = sample == 0 ? 3 : 8; // the diamond on Hogger, the skull on Aeryn
        const Rect targetRect{safe.x + frameW + g.px(8.0f), safe.y, frameW, frameH};
        if (page != Page::MenuSelf) {
            targetFrame(g, targetRect, target);
        }
        const f32 u = g.unit(1.0f);
        const Rect& owner = page == Page::MenuSelf ? Rect{safe.x, safe.y, frameW, frameH} : targetRect;
        const f32 dx = page == Page::MenuSelf ? 106.0f : 120.0f, dy = page == Page::MenuSelf ? 27.0f : 10.0f;
        UnitMenuScreens::galleryMenu(c, static_cast<u32>(page) - static_cast<u32>(Page::Menu), owner.x + dx * u,
                                     owner.bottom() - dy * u);
        if (page == Page::MenuSelf) {
            char status[96];
            autoFollowStatus(c, text::format(status, "AUTOFOLLOWSTART", "Aeryn"), 1.0f); // AutoFollowStatus
        }
        return;
    }

    if (page == Page::Tooltips) {
        // The tooltips side by side as GameTooltip:SetUnit words them; the last one kept on the target.
        const f32 gap = g.px(10.0f);
        f32 x = safe.x + gap, y = safe.y + gap, rowH = 0.0f;
        for (u32 i = 0; i < m_tooltips.size(); ++i) {
            const std::string_view clear[1] = {text::get("VD_UNIT_CLEAR_TARGET")};
            HudTooltipView view;
            view.lines = m_tooltips[i].lines();
            const bool kept = i + 1 == m_tooltips.size();
            view.interactive = kept;
            if (kept) {
                view.actions = clear;
            }
            const HudTooltipSize size = measureHudTooltip(g, view, g.px(270.0f));
            if (x + size.w > safe.right() - gap) {
                x = safe.x + gap;
                y += rowH + gap;
                rowH = 0.0f;
            }
            const HudTooltipResult r =
                hudTooltip(g, ui::widgetId("gallery.unitTooltip", i), {x, y, size.w, size.h}, view);
            if (r.action >= 0 || r.closed) {
                VD_LOG_INFO("UI gallery: tooltip %u %s", i, r.action >= 0 ? "clear target" : "closed");
            }
            x += size.w + gap;
            rowH = std::max(rowH, size.h);
        }
        return;
    }

    // The player frame resting (leader, master looter, Alliance badge), and fighting.
    PlayerFrameView player;
    player.name = kMe.name;
    player.level = kMe.level;
    player.health = 260;
    player.maxHealth = 340;
    player.powerType = 1;
    player.power = 450;
    player.maxPower = 1000;
    player.race = 1;
    player.status = PlayerStatus::Resting;
    player.statusGlow = true;
    player.leader = true;
    player.masterLooter = true;
    player.pvp = game::PvpBadge::Alliance;
    const Rect resting{safe.x, safe.y, frameW, frameH};
    if (playerFrame(g, resting, player, nowMs).tapped) {
        VD_LOG_INFO("UI gallery: player frame tapped (target yourself)");
    }
    player.status = PlayerStatus::Attacking;
    player.leader = player.masterLooter = false;
    player.pvp = game::PvpBadge::FreeForAll;
    playerFrame(g, {safe.x + frameW + g.px(8.0f), safe.y, frameW, frameH}, player, nowMs);
    // The Alliance badge's tooltip (a tap on it), right of the frames.
    {
        game::TooltipLine lines[2];
        lines[0].left = text::get("FACTION_ALLIANCE");
        lines[1].left = text::get("NEWBIE_TOOLTIP_ALLIANCE");
        lines[1].leftColor = game::kTooltipGold;
        lines[1].wrap = true;
        HudTooltipView view;
        view.lines = lines;
        const HudTooltipSize size = measureHudTooltip(g, view, g.px(270.0f));
        hudTooltip(g, ui::widgetId("gallery.badge"),
                   {safe.x + 2.0f * frameW + g.px(16.0f), safe.y, size.w, size.h}, view);
    }

    // The target frames: every colour, art and level rule.
    const f32 gap = g.px(6.0f);
    const u32 columns = std::max(1u, static_cast<u32>((safe.w + gap) / (frameW + gap)));
    for (u32 i = 0; i < kSampleCount; ++i) {
        const game::Entity& t = m_units[1 + i];
        const Sample& s = kSamples[i];
        TargetFrameView v;
        v.name = s.name;
        v.level = game::shownLevel(me, t, s.rank, &m_reactions);
        v.levelColor = game::levelColor(me, t, &m_reactions);
        const game::NameColor color = game::unitNameColor(me, t, &m_reactions);
        v.nameBackground = game::targetNameBackground(color);
        v.tapped = color == game::NameColor::Tapped;
        v.health = t.value(uf::UNIT_FIELD_HEALTH);
        v.maxHealth = t.value(uf::UNIT_FIELD_MAXHEALTH);
        v.powerType = t.byteValue(uf::UNIT_FIELD_BYTES_0, 3);
        v.power = t.value(static_cast<u16>(uf::UNIT_FIELD_POWER1 + v.powerType));
        v.maxPower = t.value(static_cast<u16>(uf::UNIT_FIELD_MAXPOWER1 + v.powerType));
        v.dead = game::isDead(t);
        v.classification =
            s.type == ObjectType::Player ? game::Classification::Normal : game::classification(s.rank);
        v.portraitFallback =
            s.type == ObjectType::Player ? hud::racePortrait(s.race, s.gender) : hud::kMonsterPortrait;
        v.pvp = game::pvpBadge(t, &m_reactions);
        v.leader = i == 1; // Aeryn leads the group
        v.raidTarget = i < 3 ? static_cast<u8>(8 - i) : 0; // phase 13: the skull, the cross, the square
        const Rect r{safe.x + static_cast<f32>(i % columns) * (frameW + gap),
                     safe.y + frameH + gap + static_cast<f32>(i / columns) * (frameH + gap), frameW, frameH};
        targetFrame(g, r, v);
    }

    // The experience bar, rested, with its text and the rest tooltip by the tick.
    ExperienceBarView xp;
    xp.xp = 3400;
    xp.next = 7600;
    xp.rested = 1800;
    xp.restState = game::kRestStateRested;
    xp.showText = true;
    const Rect bar{safe.x, safe.bottom() - g.unit(13.0f), safe.w, g.unit(13.0f)};
    experienceBar(g, bar, xp);
    char restText[512];
    usize length = text::format(restText, "EXHAUST_TOOLTIP1", text::get("VD_REST_STATE_RESTED"), 200).size();
    length += text::format(std::span<char>(restText + length, sizeof(restText) - length), "EXHAUST_TOOLTIP3",
                           xp.rested * 2)
                  .size();
    game::TooltipLine line;
    line.left = std::string_view(restText, length);
    line.wrap = true;
    HudTooltipView view;
    view.lines = {&line, 1};
    const HudTooltipSize size = measureHudTooltip(g, view, g.px(270.0f));
    const Rect tick = exhaustionTickRect(g, bar, xp);
    hudTooltip(g, ui::widgetId("gallery.rest"),
               besideTooltipAnchor(g, safe, tick, tick.centerX() < safe.centerX(), size), view);
}

} // namespace vd
