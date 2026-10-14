#include "app/TouchAssists.h"

#include "app/CombatScreens.h"
#include "app/Dialogs.h"
#include "app/GameSession.h"
#include "app/RaidPulloutSamples.h"
#include "app/SpellTargetingScreens.h"
#include "app/TouchBarScreens.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/AbilityData.h"
#include "game/AreaData.h"
#include "game/Progress.h"
#include "game/SpellTargeting.h"
#include "game/UnitInfo.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vd {

namespace uf = net::uf;
using game::Entity;
using game::TouchAction;
using game::TouchActionKind;
using game::touchIndex;
using game::TouchPlace;

namespace {

constexpr u32 kRefusalBuzzMs = 1500;              // a refusal this soon after a press buzzes
constexpr u32 kAfterCastWaitMs = 3000;            // a spell that starts the attack as it goes off: given this long more
constexpr f32 kMeleeYards = 5.0f;                 // NOMINAL_MELEE_RANGE: the reach the attack starts within
constexpr u32 kInfoColor = ui::rgba(255, 255, 0); // UIErrorsFrame's yellow (an information, not an error)
constexpr u32 kHoverColor = ui::rgba(255, 209, 0);
constexpr u32 kDemoLineMs = 3000;
constexpr u64 kDemoGuid = 0x7100; // VD_TOUCH_ASSIST_DEMO's sample party (and 1: the player)

bool reached(u32 nowMs, u32 untilMs) {
    return static_cast<i32>(nowMs - untilMs) >= 0;
}

// Where the party frames go beside the cluster: left of it (right of it for the left hand), down to its foot.
ui::Rect dockBeside(const ui::Rect& cluster, const ui::Rect& safe, bool mirrored, f32 gap) {
    if (cluster.w <= 0.0f || cluster.h <= 0.0f) {
        return {};
    }
    const f32 bottom = std::min(cluster.bottom(), safe.bottom());
    if (mirrored) {
        return {cluster.right() + gap, safe.y, std::max(0.0f, safe.right() - cluster.right() - gap), bottom - safe.y};
    }
    return {safe.x, safe.y, std::max(0.0f, cluster.x - gap - safe.x), bottom - safe.y};
}

// The stick's band when it is one along the bottom (the square screens: UiPanelPlaces::stickPad) stays the left
// thumb's: the dock ends over it. The wide screens' stick column is far from the frames beside the cluster.
ui::Rect clearOfStick(ui::Rect dock, const ui::Rect& safe, const ui::Rect& stickPad) {
    if (dock.h > 0.0f && stickPad.w > 0.0f && stickPad.y > safe.y + safe.h * 0.5f && stickPad.y < dock.bottom()) {
        dock.h = std::max(0.0f, stickPad.y - dock.y);
    }
    return dock;
}

} // namespace

void TouchAssists::init(const game::GameData* data, const game::AbilityData* abilities) {
    m_data = data;
    m_abilities = abilities;
    const char* trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY");
    m_trace = trace && *trace && *trace != '0';
    if (const char* demo = VD_DEV_ENV("VD_TOUCH_ASSIST_DEMO"); demo && *demo) {
        const std::string_view d(demo);
        m_demo = d == "frame" ? Demo::Frame : d == "dock" ? Demo::Dock : d == "ground" ? Demo::Ground : Demo::None;
        // The player's subgroup of the sample raid (app/RaidPulloutSamples) as its party.
        const std::span<const RaidSampleMember> raid = raidSampleMembers();
        u32 n = 0;
        for (u32 i = 0; i < raid.size() && n < m_demoParty.size(); ++i) {
            const RaidSampleMember& m = raid[i];
            if (i == kRaidSampleSelf || m.subgroup != raid[kRaidSampleSelf].subgroup) {
                continue;
            }
            PartyFrameView& v = m_demoParty[n++];
            v.name = m.name;
            v.level = m.level;
            v.health = m.health;
            v.power = m.power;
            v.powerType = m.powerType;
            v.race = m.race;
            v.gender = m.gender;
            v.dead = m.dead;
            v.online = !m.offline;
        }
    }
}

void TouchAssists::setOptions(const SettingsView& s) {
    m_options.autoTarget = s.touchAutoTarget;
    m_options.startAttack = s.touchStartAttack;
    m_options.repeatHeld = s.touchRepeatHeld;
    m_options.stances = s.touchStanceAssist;
    m_options.druidForms = s.touchFormAssist;
    m_options.dragToFrame = s.touchDragToFrame;
    m_options.aimGround = s.touchAimGround;
    m_options.spellQueue = s.touchSpellQueue;
    m_options.haptics = s.touchHaptics;
    m_options.siteTemplates = s.touchSiteTemplates;
    if (!m_options.spellQueue) {
        m_queue.clear();
    }
}

void TouchAssists::enterWorld(GameSession& session) {
    m_gate.reset();
    m_queue.clear();
    m_formSwitch.clear();
    m_site.reset();
    m_eventCursor = session.play().events().end();
    m_fxCursor = session.play().fx().end();
    m_player = 0;
    m_formsVersion = 0;
    m_formCount = 0;
    m_afterSpell = 0;
    m_pressSpell = 0;
    m_reactiveShown = 0;
    m_aimSpell = {};
    m_aimGround = 0;
    endDrag();
}

const TouchAssists::Facts* TouchAssists::facts(u32 spellId) {
    if (spellId == 0 || !m_data) {
        return nullptr;
    }
    Facts& f = m_facts[spellId % m_facts.size()];
    if (f.spellId != spellId) {
        f = {};
        f.spellId = spellId;
        if (const std::optional<game::SpellInfo> info = m_data->spell(spellId)) {
            f.found = true;
            f.info = *info;
            f.kind = info->targetKind();
            f.ground = game::spellAim(*info) == game::SpellAim::Location;
        }
        f.hasDetails = m_abilities && m_abilities->spellText().details(spellId, f.details);
    }
    return f.found ? &f : nullptr;
}

u32 TouchAssists::spellOfAction(TouchBarScreens& bar, const game::TouchTemplate& t, const TouchAction& action) const {
    if (action.kind == TouchActionKind::Spell) {
        return bar.spellOf(action);
    }
    if (action.kind == TouchActionKind::Group && action.id < game::kTouchGroups && t.groups[action.id].count > 0) {
        const game::TouchGroup& g = t.groups[action.id];
        const TouchAction& current = g.actions[std::min<u32>(g.current, g.count - 1u)];
        return current.kind == TouchActionKind::Spell ? bar.spellOf(current) : 0;
    }
    return 0;
}

bool TouchAssists::hostileTarget(GameSession& session) const {
    const game::Gameplay& play = session.play();
    const Entity* me = session.entities().find(session.playerGuid());
    const Entity* target = play.target() ? session.entities().find(play.target()) : nullptr;
    return me && target && target != me && game::isUnit(*target) && !game::isDead(*target) &&
           game::canAttack(*me, *target, play.reactions());
}

void TouchAssists::haptic(haptics::Pattern pattern) const {
    if (m_options.haptics) {
        haptics::play(pattern);
    }
}

void TouchAssists::message(std::string_view key) const {
    if (!m_links.message) {
        return;
    }
    const std::string_view line = text::get(text::Key(key));
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%.*s", static_cast<int>(line.size()), line.data());
    m_links.message(m_links.context, kInfoColor, buffer);
}

// --- Every frame ------------------------------------------------------------------------------------------------

void TouchAssists::readAnswers(GameSession& session, u32 nowMs) {
    game::Gameplay& play = session.play();
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        if (e->kind != game::GameEventKind::CastFailed) {
            continue;
        }
        m_gate.refused(e->id, static_cast<u32>(e->value[0]), nowMs);
        if (e->id == m_afterSpell) {
            m_afterSpell = 0;
        }
        if (e->id != 0 && e->id == m_pressSpell && nowMs - m_pressMs < kRefusalBuzzMs) {
            haptic(haptics::Pattern::Long); // once a press
            m_pressSpell = 0;
        }
        if (m_trace) {
            VD_LOG_INFO("Touch assist: %u refused (%s)", e->id, net::spellCastResultName(static_cast<u8>(e->value[0])));
        }
    }
    while (const game::FxEvent* e = play.fx().next(m_fxCursor)) {
        if (e->guid != m_player ||
            (e->kind != game::FxEventKind::SpellStart && e->kind != game::FxEventKind::SpellGo)) {
            continue;
        }
        m_gate.answered(e->id);
        if (e->kind != game::FxEventKind::SpellGo || e->id != m_afterSpell) {
            continue;
        }
        // SPELL_ATTR_EX2_INITIATE_COMBAT_POST_CAST: the attack once the spell went off (Judgement, the openers).
        m_afterSpell = 0;
        const bool attacking = play.melee().attacking() && play.melee().target() == m_afterTarget;
        if (!attacking && m_afterTarget == play.target() && hostileTarget(session)) {
            play.startAttack(m_afterTarget);
            if (m_trace) {
                VD_LOG_INFO("Touch assist: %u went off: the auto attack starts", e->id);
            }
        }
    }
}

void TouchAssists::refreshForms(GameSession& session) {
    const game::SpellBook& spells = session.play().spells();
    if (m_formsVersion == spells.knownVersion() || !m_abilities) {
        return;
    }
    m_formsVersion = spells.knownVersion();
    m_formCount = game::shapeshiftForms(spells.known(), m_abilities->spellText(), m_forms);
}

void TouchAssists::update(GameSession& session, TouchBarScreens& bar, CombatScreens& combat, u32 nowMs) {
    VD_PROFILE_ZONE("TouchAssists::update");
    m_hoverShown = false;
    const Entity* me = session.entities().find(session.playerGuid());
    if (!me) {
        return;
    }
    m_player = me->guid;
    game::Gameplay& play = session.play();
    readAnswers(session, nowMs);
    refreshForms(session);
    // The spell waiting for its stance or form.
    if (const game::TouchFormSwitch::Due due = m_formSwitch.poll(me->byteValue(uf::UNIT_FIELD_BYTES_1, 2), nowMs);
        due.spellId != 0) {
        if (m_trace) {
            VD_LOG_INFO("Touch assist: the form came, %u cast", due.spellId);
        }
        castNow(session, combat, due.spellId, nowMs, false);
    }
    // The press waiting for the global cooldown's or the cast's end.
    if (const u32 waiting = m_queue.waiting(); waiting != 0) {
        u32 blocked = 0;
        if (const Facts* f = facts(waiting)) {
            const game::SpellBook& spells = play.spells();
            blocked = std::max(spells.cooldown(f->info, f->info.category, nowMs).remaining(nowMs),
                               spells.cast().active() ? spells.cast().remaining(nowMs) : 0u);
        }
        if (const u32 due = m_queue.due(blocked, nowMs); due != 0) {
            if (m_trace) {
                VD_LOG_INFO("Touch assist: queued %u sent", due);
            }
            castNow(session, combat, due, nowMs, false);
        }
    }
    if (m_afterSpell != 0 && reached(nowMs, m_afterUntilMs)) {
        m_afterSpell = 0;
    }
    // §8 "Plantilla por sitio": the template of the place entered.
    if (m_options.siteTemplates && bar.haveLayout()) {
        const game::AreaData* areas = play.travel().areas();
        const game::MapInfo* map = areas ? areas->map(session.map()) : nullptr;
        const game::TouchSite site = map ? game::touchSiteOf(map->instanceType) : game::TouchSite::None;
        if (const i32 index = m_site.update(site, bar.layout()); index >= 0) {
            bar.selectTemplate(static_cast<u32>(index), nowMs);
            if (m_trace) {
                VD_LOG_INFO("Touch assist: site %u, template %d", static_cast<u32>(site), index);
            }
        }
    } else {
        m_site.reset(); // turned on again: the place's template at once
    }
}

void TouchAssists::finishView(GameSession& session, TouchBarScreens& bar, std::span<const TouchAction> resolved,
                              u32 reactiveSpell, TouchBarView& view, u32 nowMs) {
    VD_PROFILE_ZONE("TouchAssists::finishView");
    (void)nowMs;
    m_aimSpell = {};
    m_aimGround = 0;
    const Entity* me = session.entities().find(session.playerGuid());
    if (!me || !bar.haveLayout()) {
        return;
    }
    game::Gameplay& play = session.play();
    const game::TouchTemplate& t = bar.layout().current();
    const u32 form = me->byteValue(uf::UNIT_FIELD_BYTES_1, 2);
    const u64 targetGuid = play.target();
    const Entity* target = targetGuid ? session.entities().find(targetGuid) : nullptr;
    view.wantsEnemy = m_options.autoTarget && !hostileTarget(session);
    const std::span<const game::ShapeshiftForm> forms{m_forms.data(), m_formCount};
    // The twelve slots, then the reactive button (its spell may need a stance too: Overpower in Berserker Stance).
    const u32 reactivePlace = touchIndex(TouchPlace::Reactive);
    for (u32 s = 0; s <= game::kTouchSlots; ++s) {
        const u32 place = s < game::kTouchSlots ? s : reactivePlace;
        TouchButtonView& b = view.places[place];
        if (!b.shown) {
            continue;
        }
        if (!m_options.repeatHeld) {
            b.repeat = false; // a hold shows its tooltip instead
        }
        const u32 spellId =
            s < game::kTouchSlots ? (s < resolved.size() ? spellOfAction(bar, t, resolved[s]) : 0) : reactiveSpell;
        const Facts* f = facts(spellId);
        if (!f) {
            continue;
        }
        if (s < game::kTouchSlots &&
            (f->ground ? m_options.aimGround
                       : m_options.dragToFrame &&
                             (f->kind == game::SpellTargetKind::Friend || f->kind == game::SpellTargetKind::AnyUnit))) {
            b.aim = true;
            b.aimGround = f->ground;
            m_aimSpell[s] = f->spellId;
            m_aimGround |= static_cast<u16>(f->ground ? 1u << s : 0u);
        }
        // A spell of another stance (or a caster spell in an animal form) is usable: the assist takes the form.
        if (!b.slot.usable && !b.slot.noMana && f->hasDetails && m_abilities &&
            game::touchFormChange(f->details, form, forms, *m_abilities, m_options.stances, m_options.druidForms)
                .needed()) {
            const game::ActionUsable u = game::spellUsable(
                f->info, &f->details, *me, target, game::comboPoints(*me, targetGuid), nullptr, &play.spellMods());
            b.slot.usable = u.usable;
            b.slot.noMana = u.noMana;
        }
    }
    // §8 "Vibración": a reactive ability coming (its button, or its glow where it already is).
    u16 reactive = view.places[touchIndex(TouchPlace::Reactive)].shown ? 0x8000 : 0;
    for (u32 s = 0; s < game::kTouchSlots; ++s) {
        reactive |= static_cast<u16>(view.places[s].shown && view.places[s].glow ? 1u << s : 0u);
    }
    if ((reactive & ~m_reactiveShown) != 0) {
        haptic(haptics::Pattern::Double);
    }
    m_reactiveShown = reactive;
}

// --- The casts ----------------------------------------------------------------------------------------------------

u64 TouchAssists::cast(GameSession& session, CombatScreens& combat, u32 spellId, u32 nowMs, bool repeat) {
    const Facts* f = facts(spellId);
    const Entity* me = session.entities().find(session.playerGuid());
    if (!f || !me || spellId == game::kAttackSpell) {
        return combat.useButton(session, {net::kActionSpell, spellId});
    }
    game::Gameplay& play = session.play();
    if (!repeat) {
        m_pressSpell = spellId;
        m_pressMs = nowMs;
        haptic(haptics::Pattern::Tap);
    }
    // §8 "Objetivo automático": the unit the Target button would choose (1.12 waits for a click on one instead). A
    // press only: a held button does not go from one enemy to the next by itself.
    if (!repeat && m_options.autoTarget && f->kind == game::SpellTargetKind::Enemy && m_enemy != 0 &&
        m_enemy != play.target() && !hostileTarget(session)) {
        if (play.targeting().active()) {
            play.stopTargeting(); // a spell waiting for a unit would take it
        }
        session.setTarget(m_enemy);
        if (m_trace) {
            VD_LOG_INFO("Touch assist: %u without a hostile target: %016llx targeted", spellId,
                        static_cast<unsigned long long>(m_enemy));
        }
    }
    // §8 "Cola de hechizos": the last moment of the global cooldown or of a cast waits for its end.
    if (m_options.spellQueue && !repeat) {
        const game::SpellBook& spells = play.spells();
        const u32 blocked = std::max(spells.cooldown(f->info, f->info.category, nowMs).remaining(nowMs),
                                     spells.cast().active() ? spells.cast().remaining(nowMs) : 0u);
        if (m_queue.offer(spellId, blocked, nowMs)) {
            if (m_trace) {
                VD_LOG_INFO("Touch assist: %u waits %u ms", spellId, blocked);
            }
            return 0;
        }
    }
    // §8 "Asistencia de posturas / formas": the stance first (or out of the animal form), the spell once it shows.
    if (f->hasDetails && m_abilities && (m_options.stances || m_options.druidForms)) {
        const game::TouchFormChange change =
            game::touchFormChange(f->details, me->byteValue(uf::UNIT_FIELD_BYTES_1, 2), {m_forms.data(), m_formCount},
                                  *m_abilities, m_options.stances, m_options.druidForms);
        bool sent = false;
        if (change.spellId != 0) {
            // Not while the stances' shared cooldown runs (the server would refuse it): the spell goes as it is.
            const Facts* stance = facts(change.spellId);
            if (!stance || !play.spells().cooldown(stance->info, stance->info.category, nowMs).active(nowMs)) {
                sent = play.castSpell(change.spellId, 0);
                if (sent && !m_stanceTold) {
                    m_stanceTold = true; // once a session
                    message("VD_TOUCH_ASSIST_STANCE_RAGE");
                }
            }
        } else if (change.leaveSpell != 0) {
            play.cancelAura(change.leaveSpell);
            sent = true;
        }
        if (sent) {
            m_formSwitch.start(spellId, play.target(), change.form, nowMs);
            m_gate.sent(spellId, nowMs);
            if (m_trace) {
                VD_LOG_INFO("Touch assist: %u needs form %u: %s %u first", spellId, change.form,
                            change.spellId != 0 ? "cast" : "left",
                            change.spellId != 0 ? change.spellId : change.leaveSpell);
            }
            return 0;
        }
    }
    return castNow(session, combat, spellId, nowMs, repeat);
}

u64 TouchAssists::castNow(GameSession& session, CombatScreens& combat, u32 spellId, u32 nowMs, bool repeat) {
    (void)repeat;
    game::Gameplay& play = session.play();
    const u64 aim = combat.useButton(session, {net::kActionSpell, spellId});
    m_gate.sent(spellId, nowMs);
    const Facts* f = facts(spellId);
    if (!f || !f->hasDetails) {
        return aim;
    }
    // §8 "Iniciar el ataque".
    const game::AttackStart attack = game::attackStartOf(f->details, m_options.startAttack);
    const u64 target = play.target();
    if (attack == game::AttackStart::None || !hostileTarget(session) ||
        (play.melee().attacking() && play.melee().target() == target)) {
        return aim;
    }
    if (attack == game::AttackStart::AfterCast) {
        m_afterSpell = spellId;
        m_afterTarget = target;
        m_afterUntilMs = nowMs + f->info.castTimeMs + kAfterCastWaitMs;
        return aim;
    }
    // At the cast, within reach (out of it the cast fails and 1.12 would not swing either).
    const Entity* me = session.entities().find(session.playerGuid());
    const Entity* unit = session.entities().find(target);
    game::SpellInfo melee;
    melee.maxRange = kMeleeYards;
    if (me && unit && game::spellRange(melee, *me, unit, nowMs) != game::ActionRange::Out) {
        play.startAttack(target);
        if (m_trace) {
            VD_LOG_INFO("Touch assist: %u starts the auto attack", spellId);
        }
    }
    return aim;
}

bool TouchAssists::repeatReady(GameSession& session, const HudActionSlot& slot, u32 nowMs) const {
    return m_options.repeatHeld && m_gate.ready(nowMs) && slot.cooldownLeft <= 0.0f && slot.usable && slot.inRange &&
           !session.play().spells().cast().active() && m_formSwitch.waiting() == 0 && m_queue.waiting() == 0;
}

// --- Dragging -------------------------------------------------------------------------------------------------------

u64 TouchAssists::unitAt(GameSession& session, f32 x, f32 y, ui::Rect& frame) const {
    if (m_links.unitAt) {
        if (const u64 unit = m_links.unitAt(m_links.context, x, y, frame)) {
            return unit;
        }
    }
    if (session.play().target() != 0 && m_targetFrame.w > 0.0f && m_targetFrame.contains(x, y)) {
        frame = m_targetFrame;
        return session.play().target();
    }
    if (m_playerFrame.w > 0.0f && m_playerFrame.contains(x, y)) {
        frame = m_playerFrame;
        return session.playerGuid();
    }
    return 0;
}

void TouchAssists::endDrag() {
    m_dragPlace = -1;
    m_hoverGuid = 0;
    m_hoverShown = false;
}

void TouchAssists::drag(GameSession& session, const TouchBarOutput& out, u32 nowMs) {
    if (out.dragPlace < 0 || static_cast<u32>(out.dragPlace) >= game::kTouchSlots ||
        m_aimSpell[static_cast<u32>(out.dragPlace)] == 0) {
        endDrag();
        return;
    }
    VD_PROFILE_ZONE("TouchAssists::drag");
    const u32 spellId = m_aimSpell[static_cast<u32>(out.dragPlace)];
    const Facts* f = facts(spellId);
    if (!f) {
        endDrag();
        return;
    }
    m_dragPlace = out.dragPlace;
    game::Gameplay& play = session.play();
    const Entity* me = session.entities().find(session.playerGuid());
    if (f->ground) {
        // §4 "Apuntar arrastrando": the circle where the finger aims; lifted, the spell goes there (as phase 13's
        // confirmation does: Gameplay::targetLocation, the client's range check first).
        if (!m_links.targeting) {
            return;
        }
        if (!out.dragOnButton) {
            m_links.targeting->aimAt(f->info, &play.spellMods(), me, out.dragNx, out.dragNy);
        }
        glm::dvec3 at;
        if (out.dragDropped && m_links.targeting->aimPoint(at)) {
            play.castSpell(spellId, 0); // it waits for its place
            game::Gameplay::TargetResult r = game::Gameplay::TargetResult::NotSent;
            if (play.targeting().active() && play.targeting().aim() == game::SpellAim::Location) {
                r = play.targetLocation(static_cast<f32>(at.x), static_cast<f32>(at.y), static_cast<f32>(at.z));
                if (r == game::Gameplay::TargetResult::Refused) {
                    play.stopTargeting(); // out of range after all: nothing waits on the screen
                }
            }
            m_gate.sent(spellId, nowMs);
            m_pressSpell = spellId;
            m_pressMs = nowMs;
            haptic(haptics::Pattern::Tap);
            if (m_trace) {
                VD_LOG_INFO("Touch assist: %u aimed at (%.1f, %.1f, %.1f): %u", spellId, at.x, at.y, at.z,
                            static_cast<u32>(r));
            }
        }
        if (out.dragDropped) {
            endDrag();
        }
        return;
    }
    // §4 "Arrastrar a un marco": the unit frame under the finger; lifted there, the spell lands on that unit (the
    // target stays).
    ui::Rect frame;
    u64 unit = out.dragOnButton ? 0 : unitAt(session, out.dragX, out.dragY, frame);
    if (unit != 0 && f->kind == game::SpellTargetKind::Friend && me) {
        const Entity* other = session.entities().find(unit);
        if (other && game::reactionTo(*me, *other, play.reactions()) == game::Reaction::Hostile) {
            unit = 0; // a helpful spell does not go on an enemy (a party member out of sight is not known: tried)
        }
    }
    m_hoverGuid = unit;
    m_hover = frame;
    m_hoverShown = unit != 0 && !out.dragDropped;
    if (!out.dragDropped) {
        return;
    }
    if (unit != 0) {
        play.castSpell(spellId, unit);
        m_gate.sent(spellId, nowMs);
        m_pressSpell = spellId;
        m_pressMs = nowMs;
        haptic(haptics::Pattern::Tap);
        if (m_trace) {
            VD_LOG_INFO("Touch assist: %u dragged onto %016llx", spellId, static_cast<unsigned long long>(unit));
        }
    }
    endDrag();
}

void TouchAssists::draw(DialogContext& c) {
    if (!m_hoverShown || m_hover.w <= 0.0f) {
        return;
    }
    // The frame the spell would land on: a gold edge and a faint gold over it.
    const f32 w = std::max(1.0f, c.ui.px(2.0f));
    const ui::Rect r = m_hover.inset(-w);
    c.draw.solid(r, ui::withAlpha(kHoverColor, 0.22f));
    c.draw.solid({r.x, r.y, r.w, w}, kHoverColor);
    c.draw.solid({r.x, r.bottom() - w, r.w, w}, kHoverColor);
    c.draw.solid({r.x, r.y, w, r.h}, kHoverColor);
    c.draw.solid({r.right() - w, r.y, w, r.h}, kHoverColor);
}

ui::Rect TouchAssists::healerDock(TouchBarScreens& bar, const ui::Rect& cluster, const ui::Rect& safe,
                                  const ui::Rect& stickPad) const {
    if (!bar.enabled() || !bar.haveLayout() || !bar.layout().current().healerFrames) {
        return {};
    }
    return clearOfStick(dockBeside(cluster, safe, bar.mirrored(), cluster.h * 0.03f), safe, stickPad);
}

bool TouchAssists::formReachable(const void* self, const game::SpellDetails& spell, u32 form) {
    const TouchAssists& a = *static_cast<const TouchAssists*>(self);
    return a.m_abilities && (a.m_options.stances || a.m_options.druidForms) &&
           game::touchFormChange(spell, form, {a.m_forms.data(), a.m_formCount}, *a.m_abilities, a.m_options.stances,
                                 a.m_options.druidForms)
               .needed();
}

// --- VD_TOUCH_ASSIST_DEMO -------------------------------------------------------------------------------------------

void TouchAssists::prepareOffline(TouchBarScreens& bar, game::TouchTemplate& t, game::TouchContext layer,
                                  TouchBarView& view, u32 nowMs) {
    (void)nowMs;
    m_aimSpell = {};
    m_aimGround = 0;
    m_hoverShown = false;
    if (m_demo == Demo::None) {
        return;
    }
    const game::TouchContext shown = t.layerEnabled(layer) ? layer : game::TouchContext::Base;
    for (u32 s = 0; s < game::kTouchSlots; ++s) {
        TouchButtonView& b = view.places[s];
        const TouchAction a = game::resolveTouchAction(t, shown, s);
        if (!b.shown) {
            continue;
        }
        if (m_demo == Demo::Ground && a.kind == TouchActionKind::Group && a.id < game::kTouchGroups) {
            // The group's ground spell in front (the mage's Blizzard), as if chosen from its fan.
            game::TouchGroup& g = t.groups[a.id];
            for (u32 i = 0; i < g.count; ++i) {
                const Facts* f =
                    g.actions[i].kind == TouchActionKind::Spell ? facts(bar.spellOf(g.actions[i])) : nullptr;
                if (f && f->ground) {
                    g.current = static_cast<u8>(i);
                    b.slot.iconHash = f->info.iconHash;
                    break;
                }
            }
        }
        const Facts* f = facts(spellOfAction(bar, t, a));
        if (f && (f->ground || f->kind == game::SpellTargetKind::Friend || f->kind == game::SpellTargetKind::AnyUnit)) {
            b.aim = true;
            b.aimGround = f->ground;
            b.onPress = false;
            m_aimSpell[s] = f->spellId;
        }
    }
    m_demoDock = m_demo == Demo::Dock || t.healerFrames;
}

void TouchAssists::actOffline(const TouchBarOutput& out, u32 nowMs) {
    if (m_demo == Demo::None) {
        return;
    }
    if (out.dragPlace < 0 || static_cast<u32>(out.dragPlace) >= game::kTouchSlots ||
        m_aimSpell[static_cast<u32>(out.dragPlace)] == 0) {
        endDrag();
        return;
    }
    const Facts* f = facts(m_aimSpell[static_cast<u32>(out.dragPlace)]);
    if (!f) {
        return;
    }
    const std::string_view spell = f->info.name;
    if (f->ground) {
        if (m_links.targeting && !out.dragOnButton) {
            m_links.targeting->aimAt(f->info, nullptr, nullptr, out.dragNx, out.dragNy);
        }
        glm::dvec3 at;
        if (out.dragDropped && m_links.targeting && m_links.targeting->aimPoint(at)) {
            std::snprintf(m_demoLine.data(), m_demoLine.size(), "%.*s -> (%.1f, %.1f, %.1f)",
                          static_cast<int>(spell.size()), spell.data(), at.x, at.y, at.z);
            m_demoLineMs = nowMs;
            VD_LOG_INFO("VD_TOUCH_ASSIST_DEMO: %s", m_demoLine.data());
            haptic(haptics::Pattern::Tap);
        }
        return;
    }
    // The sample party's frames, the player's.
    u64 unit = 0;
    ui::Rect frame;
    std::string_view name;
    for (u32 i = 0; i < m_demoFrames.size() && !out.dragOnButton; ++i) {
        if (m_demoFrames[i].w > 0.0f && m_demoFrames[i].contains(out.dragX, out.dragY)) {
            unit = kDemoGuid + i;
            frame = m_demoFrames[i];
            name = m_demoParty[i % 4].name;
        }
    }
    if (unit == 0 && !out.dragOnButton && m_playerFrame.w > 0.0f && m_playerFrame.contains(out.dragX, out.dragY)) {
        unit = 1;
        frame = m_playerFrame;
        name = "(you)";
    }
    m_hoverGuid = unit;
    m_hover = frame;
    m_hoverShown = unit != 0 && !out.dragDropped;
    if (out.dragDropped && unit != 0) {
        std::snprintf(m_demoLine.data(), m_demoLine.size(), "%.*s -> %.*s", static_cast<int>(spell.size()),
                      spell.data(), static_cast<int>(name.size()), name.data());
        m_demoLineMs = nowMs;
        VD_LOG_INFO("VD_TOUCH_ASSIST_DEMO: %s", m_demoLine.data());
        haptic(haptics::Pattern::Tap);
    }
}

void TouchAssists::drawOffline(DialogContext& c) {
    if (m_demo == Demo::None) {
        return;
    }
    ui::Gui g{c.ui, c.draw};
    if (m_demo != Demo::Ground) {
        // The sample party where the party frames go (under the player frame), or beside the cluster.
        const std::span<const PartyFrameView> party{m_demoParty.data(), m_demoParty.size()};
        ui::Rect area{c.safeArea.x + g.px(4.0f), c.safeArea.y + g.unit(100.0f), g.unit(128.0f) * 1.6f, 0.0f};
        if (m_demoDock) {
            const f32 stickTop = c.safeArea.bottom() - std::max(g.unit(95.0f), c.safeArea.h * 0.23f); // as UiPanels
            const ui::Rect stick =
                c.safeArea.w >= c.safeArea.h * 1.6f
                    ? ui::Rect{}
                    : ui::Rect{c.safeArea.x, stickTop, m_cluster.x - c.safeArea.x, c.safeArea.bottom() - stickTop};
            const ui::Rect dock =
                clearOfStick(dockBeside(m_cluster, c.safeArea, false, m_cluster.h * 0.03f), c.safeArea, stick);
            area.x = std::max(dock.x, dock.right() - area.w);
            area.y = std::max(dock.y, dock.bottom() - partyFramesHeight(c, party));
        }
        partyFrames(c, area, party, nullptr, nullptr, nullptr, nullptr, m_demoFrames.data());
    }
    draw(c);
    if (m_demoLine[0] != '\0' && c.ui.nowMs() - m_demoLineMs < kDemoLineMs) {
        ui::TextStyle style = ui::gameFont(g.px(16.0f), ui::kGold);
        style.outline = std::max(1.0f, g.px(1.0f));
        style.align = ui::Align::Center;
        g.draw.text(c.safeArea.centerX(), c.safeArea.y + g.px(140.0f), m_demoLine.data(), style);
    }
}

// --- The edit mode's rows -------------------------------------------------------------------------------------------

f32 touchAssistRows(ui::Gui& g, const ui::Rect& row, f32 rowHeight, u32 site, bool healerFrames, TouchAssistRows& out) {
    static constexpr std::string_view kSiteKeys[] = {"VD_TOUCH_ASSIST_SITE_NONE", "VD_TOUCH_ASSIST_SITE_WORLD",
                                                     "VD_TOUCH_ASSIST_SITE_DUNGEON", "VD_TOUCH_ASSIST_SITE_RAID",
                                                     "VD_TOUCH_ASSIST_SITE_BATTLEGROUND"};
    static_assert(std::size(kSiteKeys) == static_cast<u32>(game::TouchSite::Count));
    const u32 current = site < std::size(kSiteKeys) ? site : 0u;
    const f32 gap = g.px(6.0f);
    f32 y = row.y;
    // §8: the place this template is switched to on entering (a tap goes to the next one).
    char label[96];
    text::format(label, "VD_TOUCH_ASSIST_SITE", text::get(text::Key(kSiteKeys[current])));
    if (ui::button(g, ui::widgetId("touchassist.site"), {row.x, y, row.w, rowHeight}, label)) {
        out.site = static_cast<i32>((current + 1) % std::size(kSiteKeys));
    }
    y += rowHeight + gap;
    // §5.3: the party frames beside the cluster.
    bool healer = healerFrames;
    if (ui::checkbox(g, ui::widgetId("touchassist.healer"), {row.x, y, row.w, rowHeight},
                     text::get("VD_TOUCH_ASSIST_HEALER_FRAMES"), healer)) {
        out.toggleHealer = true;
    }
    y += rowHeight + gap;
    return y - row.y;
}

} // namespace vd
