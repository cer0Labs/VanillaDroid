#include "app/UnitScreens.h"

#include "app/GameSession.h"
#include "app/HudShared.h"
#include "core/DevEnv.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/CharacterNames.h"
#include "game/SocialText.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

namespace uf = net::uf;
using game::Entity;

constexpr u32 kTooltipMs = 4000; // a unit's tooltip after a tap selected it; the badge's and the bar's
constexpr f32 kTooltipWidthDp = 270.0f;

bool before(u32 nowMs, u32 untilMs) {
    return static_cast<i32>(untilMs - nowMs) > 0;
}

std::string_view nameOf(GameSession& session, u64 guid) {
    const std::string* name = guid ? session.playerName(guid) : nullptr;
    return name ? std::string_view(*name) : std::string_view{};
}

} // namespace

UnitScreens::UnitScreens() {
    m_demo = VD_DEV_ENV("VD_HUD_DEMO") != nullptr;
}

void UnitScreens::reset() {
    m_tipGuid = 0;
    m_tipKept = false;
    m_lastTarget = 0;
    m_badgeUntilMs = m_restUntilMs = 0;
    m_combatGlow = false;
}

void UnitScreens::showTooltip(u64 guid, u32 nowMs, bool kept) {
    m_tipGuid = guid;
    m_tipUntilMs = nowMs + kTooltipMs;
    m_tipKept = kept;
}

void UnitScreens::demoViews(HudFrame& hud) {
    m_player = {};
    m_player.name = "Arthel";
    m_player.level = 12;
    m_player.health = 260;
    m_player.maxHealth = 340;
    m_player.powerType = 1;
    m_player.power = 450;
    m_player.maxPower = 1000;
    m_player.status = PlayerStatus::Resting;
    m_player.statusGlow = true;
    m_player.leader = true;
    m_player.pvp = game::PvpBadge::Alliance;
    hud.playerFrameView = &m_player;
    m_target = {};
    m_target.name = "Hogger";
    m_target.level = 11;
    m_target.levelColor = game::difficultyColor(game::levelDifficulty(11, 12));
    m_target.nameBackground = game::targetNameBackground(game::NameColor::Hostile);
    m_target.health = 62;
    m_target.maxHealth = 100;
    m_target.classification = game::Classification::Elite;
    m_target.portraitFallback = hud::kMonsterPortrait;
    m_target.raidTarget = 8; // phase 13: the skull on him (TargetRaidTargetIcon)
    hud.targetFrameView = &m_target;
    m_experience = {3400, 7600, 1800, game::kRestStateRested, false}; // tapped: VD_UI_GALLERY=units
    hud.experienceView = &m_experience;
    if (m_tooltip.empty()) { // Hogger's tooltip, worded by the game's rules
        game::Reactions reactions;
        reactions.setTemplate(1, 1, 0x3, 0x2, 0xC);
        reactions.setTemplate(14, 14, 0x8, 0x0, 0x1);
        game::Entity me, hogger;
        me.guid = 1;
        me.type = game::ObjectType::Player;
        me.values.assign(uf::PLAYER_END, 0);
        me.values[uf::UNIT_FIELD_LEVEL] = 12;
        me.values[uf::UNIT_FIELD_FACTIONTEMPLATE] = 1;
        hogger.guid = 2;
        hogger.type = game::ObjectType::Unit;
        hogger.values.assign(uf::UNIT_END, 0);
        hogger.values[uf::UNIT_FIELD_LEVEL] = 11;
        hogger.values[uf::UNIT_FIELD_FACTIONTEMPLATE] = 14;
        hogger.values[uf::UNIT_FIELD_HEALTH] = 62;
        game::UnitNames names;
        names.name = "Hogger";
        names.rank = 1;
        names.creatureType = game::creatureTypeName(7);
        game::buildUnitTooltip(me, hogger, names, &reactions, m_tooltip);
    }
}

void UnitScreens::prepareHud(GameSession& session, HudFrame& hud, u32 nowMs) {
    VD_PROFILE_ZONE("UnitScreens::prepareHud");
    if (!session.inWorld()) {
        if (m_demo) {
            demoViews(hud);
        }
        return;
    }
    const u64 self = session.playerGuid();
    if (self != m_playerGuid) {
        m_playerGuid = self;
        reset();
    }
    game::Gameplay& play = session.play();
    const game::EntityStore& entities = session.entities();
    const Entity* me = entities.find(self);
    if (!me) {
        return;
    }
    const game::Reactions* reactions = hud.reactions;
    m_reactions = reactions;
    const game::GroupState& group = play.social().group();

    // PlayerFrame.
    m_player = {};
    m_player.name = nameOf(session, self);
    m_player.level = me->value(uf::UNIT_FIELD_LEVEL);
    m_player.health = me->value(uf::UNIT_FIELD_HEALTH);
    m_player.maxHealth = std::max(1u, me->value(uf::UNIT_FIELD_MAXHEALTH));
    m_player.race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
    m_player.gender = me->byteValue(uf::UNIT_FIELD_BYTES_0, 2);
    m_player.powerType = std::min<u8>(me->byteValue(uf::UNIT_FIELD_BYTES_0, 3), 4);
    m_player.power = me->value(static_cast<u16>(uf::UNIT_FIELD_POWER1 + m_player.powerType));
    m_player.maxPower = me->value(static_cast<u16>(uf::UNIT_FIELD_MAXPOWER1 + m_player.powerType));
    m_player.portrait = hud.playerPortrait;
    // PlayerFrame_UpdateStatus: resting first, then the auto attack (PLAYER_ENTER_COMBAT), then the hate list
    // (PLAYER_REGEN_DISABLED), whose branch leaves the glow as it was; out of combat everything goes off.
    const bool attacking = play.melee().attacking();
    const bool inCombat = game::inCombat(*me);
    if (game::isResting(*me)) {
        m_player.status = PlayerStatus::Resting;
        m_player.statusGlow = true;
        m_combatGlow = false;
    } else if (attacking) {
        m_player.status = PlayerStatus::Attacking;
        m_player.statusGlow = true;
        m_combatGlow = true;
    } else if (inCombat) {
        m_player.status = PlayerStatus::InCombat;
        m_player.statusGlow = m_combatGlow;
    } else {
        m_combatGlow = false;
    }
    m_player.leader = group.active && group.leader == self;
    m_player.masterLooter = group.active &&
                            group.lootMethod == static_cast<u8>(net::LootMethod::MasterLoot) &&
                            group.looter == self;
    m_player.pvp = game::pvpBadge(*me, reactions);
    hud.playerFrameView = &m_player;

    // TargetFrame.
    const u64 target = play.target();
    const Entity* t = target ? entities.find(target) : nullptr;
    if (t && game::isUnit(*t)) {
        const bool player = t->type == game::ObjectType::Player;
        const net::CreatureQueryInfo* creature =
            player ? nullptr : session.creature(t->value(uf::OBJECT_FIELD_ENTRY));
        const u32 rank = creature ? creature->rank : 0;
        m_target = {};
        m_target.name = player ? nameOf(session, target) : creature ? std::string_view(creature->name) : "";
        if (m_target.name.empty()) {
            m_target.name = "...";
        }
        m_target.level = game::shownLevel(*me, *t, rank, reactions);
        m_target.levelColor = game::levelColor(*me, *t, reactions);
        const game::NameColor color = game::unitNameColor(*me, *t, reactions);
        m_target.nameBackground = game::targetNameBackground(color);
        m_target.tapped = color == game::NameColor::Tapped;
        m_target.health = t->value(uf::UNIT_FIELD_HEALTH);
        m_target.maxHealth = std::max(1u, t->value(uf::UNIT_FIELD_MAXHEALTH));
        m_target.powerType = std::min<u8>(t->byteValue(uf::UNIT_FIELD_BYTES_0, 3), 4);
        m_target.power = t->value(static_cast<u16>(uf::UNIT_FIELD_POWER1 + m_target.powerType));
        m_target.maxPower = t->value(static_cast<u16>(uf::UNIT_FIELD_MAXPOWER1 + m_target.powerType));
        m_target.dead = game::isDead(*t);
        m_target.classification = player ? game::Classification::Normal : game::classification(rank);
        m_target.portrait = hud.targetPortrait;
        m_target.portraitFallback = player ? hud::racePortrait(t->byteValue(uf::UNIT_FIELD_BYTES_0, 0),
                                                               t->byteValue(uf::UNIT_FIELD_BYTES_0, 2))
                                           : hud::kMonsterPortrait;
        m_target.pvp = game::pvpBadge(*t, reactions);
        m_target.leader =
            group.active && group.leader == target && (target == self || play.social().inGroupWith(target));
        m_target.raidTarget = play.commands().raidTarget(target); // phase 13: TargetFrame_UpdateRaidTargetIcon
        hud.targetFrameView = &m_target;
    }

    // MainMenuExpBar.
    m_experience.xp = me->value(uf::PLAYER_XP);
    m_experience.next = me->value(uf::PLAYER_NEXT_LEVEL_XP);
    m_experience.rested = me->value(uf::PLAYER_REST_STATE_EXPERIENCE);
    m_experience.restState = me->byteValue(uf::PLAYER_BYTES_2, 3);
    m_experience.showText = before(nowMs, m_restUntilMs);
    hud.experienceView = &m_experience;

    // A new target: its tooltip for a while (the client's shows while the mouse is over the unit).
    if (target != m_lastTarget) {
        m_lastTarget = target;
        if (target != 0) {
            showTooltip(target, nowMs, false);
        } else {
            m_tipGuid = 0;
        }
    }
}

void UnitScreens::actOnHud(GameSession& session, const HudOutput& out, u32 nowMs) {
    if (!session.inWorld()) {
        return;
    }
    if (out.playerFrame) {
        session.setTarget(session.playerGuid()); // TargetUnit("player")
    }
    if (out.playerBadge) {
        m_badgeUntilMs = before(nowMs, m_badgeUntilMs) ? nowMs : nowMs + kTooltipMs;
    }
    if (out.experienceBar) {
        m_restUntilMs = before(nowMs, m_restUntilMs) ? nowMs : nowMs + kTooltipMs;
    }
    if (out.targetFrame && session.target() != 0) {
        // Phase 13: the target frame's mouse-over (its long press is its menu): its tooltip stays, or goes.
        if (m_tipGuid == session.target() && m_tipKept) {
            m_tipGuid = 0;
            m_tipKept = false;
        } else {
            showTooltip(session.target(), nowMs, true);
        }
    }
    if (out.unitHold != 0) {
        session.setTarget(out.unitHold);
        m_lastTarget = out.unitHold; // selected by the hold: its tooltip stays, not for a while
        showTooltip(out.unitHold, nowMs, true);
    }
}

void UnitScreens::fillTooltip(GameSession& session, u64 guid) {
    const Entity* me = session.entities().find(session.playerGuid());
    const Entity* unit = session.entities().find(guid);
    m_tooltip.clear();
    if (!me || !unit) {
        return;
    }
    game::UnitNames names;
    if (unit->type == game::ObjectType::Player) {
        names.name = nameOf(session, guid);
        names.race = game::raceName(unit->byteValue(uf::UNIT_FIELD_BYTES_0, 0));
        names.cls = game::className(unit->byteValue(uf::UNIT_FIELD_BYTES_0, 1));
        if (const u32 guildId = unit->value(uf::PLAYER_GUILDID); guildId != 0) {
            names.guild = session.play().social().guildName(guildId); // asked for when missing
        }
    } else if (const net::CreatureQueryInfo* creature =
                   session.creature(unit->value(uf::OBJECT_FIELD_ENTRY))) {
        names.name = creature->name;
        names.subname = creature->subname;
        names.rank = creature->rank;
        if (creature->type != 0 && creature->type != game::kCreatureTypeNotSpecified) {
            names.creatureType = game::creatureTypeName(creature->type);
        }
        u64 owner = game::guidValue(*unit, uf::UNIT_FIELD_CHARMEDBY);
        owner = owner ? owner : game::guidValue(*unit, uf::UNIT_FIELD_SUMMONEDBY);
        names.owner = nameOf(session, owner);
    }
    game::buildUnitTooltip(*me, *unit, names, m_reactions, m_tooltip);
}

void UnitScreens::drawUnitTooltip(DialogContext& c, const Hud& hud, bool kept, bool clearButton, bool& clear,
                                  bool& closed) {
    ui::Gui g{c.ui, c.draw};
    const ui::Rect& safe = c.safeArea;
    std::string_view actions[1] = {text::get("VD_UNIT_CLEAR_TARGET")};
    HudTooltipView view;
    view.lines = m_tooltip.lines();
    view.interactive = kept;
    if (clearButton) {
        view.actions = actions; // the touch screen's Escape
    }
    const HudTooltipSize size = measureHudTooltip(g, view, g.px(kTooltipWidthDp));
    const ui::Rect r = defaultTooltipAnchor(g, safe, hud.actionBarRect(), safe.y + g.unit(200.0f), size);
    const HudTooltipResult result = hudTooltip(g, ui::widgetId("unitTooltip"), r, view);
    clear = result.action == 0;
    closed = result.closed;
}

void UnitScreens::drawOverlay(GameSession& session, DialogContext& c, const Hud& hud, u32 nowMs) {
    VD_PROFILE_ZONE("UnitScreens::overlay");
    bool clear = false, closed = false;
    if (!session.inWorld()) {
        if (m_demo && !m_tooltip.empty()) {
            drawUnitTooltip(c, hud, false, false, clear, closed);
        }
        if (!m_demo) {
            return;
        }
    }
    ui::Gui g{c.ui, c.draw};
    const ui::Rect& safe = c.safeArea;
    const f32 maxWidth = g.px(kTooltipWidthDp);

    // The unit's tooltip: kept until tapped; else for a while, out of the cast bar's way.
    if (m_tipGuid != 0 && session.inWorld()) {
        const bool expired = !m_tipKept && !before(nowMs, m_tipUntilMs);
        if (expired || !session.entities().find(m_tipGuid)) {
            m_tipGuid = 0;
            m_tipKept = false;
        } else if (m_tipKept || !session.play().spells().cast().active()) {
            fillTooltip(session, m_tipGuid);
            drawUnitTooltip(c, hud, m_tipKept, m_tipKept && m_tipGuid == session.target(), clear, closed);
            if (clear) {
                session.setTarget(0);
                m_tipGuid = 0;
            } else if (closed) {
                m_tipGuid = 0;
                m_tipKept = false;
            }
        }
    }

    // The PvP badge (PlayerPVPIconHitArea: GameTooltip_AddNewbieTip), beside it.
    if (m_player.pvp != game::PvpBadge::None && before(nowMs, m_badgeUntilMs)) {
        game::TooltipLine lines[2];
        const bool ffa = m_player.pvp == game::PvpBadge::FreeForAll;
        const bool horde = m_player.pvp == game::PvpBadge::Horde;
        lines[0].left = text::get(ffa ? "PVPFFA" : horde ? "FACTION_HORDE" : "FACTION_ALLIANCE");
        lines[1].left = text::get(ffa     ? "NEWBIE_TOOLTIP_PVPFFA"
                                  : horde ? "NEWBIE_TOOLTIP_HORDE"
                                          : "NEWBIE_TOOLTIP_ALLIANCE");
        lines[1].leftColor = game::kTooltipGold; // NORMAL_FONT_COLOR
        lines[1].wrap = true;
        HudTooltipView view;
        view.lines = std::span<const game::TooltipLine>(lines, m_newbieTips ? 2 : 1);
        const HudTooltipSize size = measureHudTooltip(g, view, maxWidth);
        const ui::Rect badge = playerBadgeRect(g, hud.playerFrameRect());
        hudTooltip(g, ui::widgetId("badgeTooltip"), besideTooltipAnchor(g, safe, badge, true, size), view);
    }

    // The rest state (ExhaustionToolTipText): by the tick, or at the default anchor while it is hidden.
    if (m_experience.showText && m_experience.next > 0) {
        const bool rested = m_experience.restState == game::kRestStateRested;
        char restText[512];
        const std::string_view state = text::get(rested ? "VD_REST_STATE_RESTED" : "VD_REST_STATE_NORMAL");
        usize length = text::format(restText, "EXHAUST_TOOLTIP1", state, rested ? 200 : 100).size();
        if (rested && m_experience.rested > 0 && length < sizeof(restText)) {
            // EXHAUST_TOOLTIP3: the monster experience still doubled (the bonus and what it doubles).
            length += text::format(std::span<char>(restText + length, sizeof(restText) - length),
                                   "EXHAUST_TOOLTIP3", m_experience.rested * 2)
                          .size();
        }
        game::TooltipLine line;
        line.left = std::string_view(restText, length);
        line.wrap = true;
        HudTooltipView view;
        view.lines = {&line, 1};
        const HudTooltipSize size = measureHudTooltip(g, view, maxWidth);
        const ui::Rect tick = exhaustionTickRect(g, hud.experienceBarRect(), m_experience);
        const ui::Rect r =
            tick.w > 0.0f ? besideTooltipAnchor(g, safe, tick, tick.centerX() < safe.centerX(), size)
                          : defaultTooltipAnchor(g, safe, hud.actionBarRect(), safe.y + g.unit(200.0f), size);
        hudTooltip(g, ui::widgetId("restTooltip"), r, view);
    }
}

} // namespace vd
