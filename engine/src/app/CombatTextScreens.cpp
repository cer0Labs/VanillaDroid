#include "app/CombatTextScreens.h"

#include "app/GameSession.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/Progress.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vd {
namespace {

namespace uf = net::uf;
using Kind = CombatTextScreens::Kind;

// Blizzard_CombatText.lua's constants.
constexpr f32 kScrollSeconds = 1.9f;  // COMBAT_TEXT_SCROLLSPEED
constexpr f32 kFadeSeconds = 1.3f;    // COMBAT_TEXT_FADEOUT_TIME
constexpr f32 kHeight = 25.0f;        // COMBAT_TEXT_HEIGHT
constexpr f32 kCritMax = 60.0f;       // COMBAT_TEXT_CRIT_MAXHEIGHT
constexpr f32 kCritMin = 30.0f;       // COMBAT_TEXT_CRIT_MINHEIGHT
constexpr f32 kCritScale = 0.05f;     // COMBAT_TEXT_CRIT_SCALE_TIME
constexpr f32 kCritShrink = 0.2f;     // COMBAT_TEXT_CRIT_SHRINKTIME
constexpr f32 kStagger = 20.0f;       // COMBAT_TEXT_STAGGER_RANGE
constexpr f32 kSpacing = 10.0f;       // COMBAT_TEXT_SPACING
constexpr f32 kMaxOffset = 130.0f;    // COMBAT_TEXT_MAX_OFFSET
constexpr f32 kLowThreshold = 0.2f;   // COMBAT_TEXT_LOW_HEALTH_THRESHOLD / _LOW_MANA_THRESHOLD
constexpr f32 kStartY = 384.0f, kUpY = 609.0f, kDownY = 159.0f; // COMBAT_TEXT_LOCATIONS
constexpr f32 kFountainRadius = 150.0f;                           // CombatText_FountainScroll

// COMBAT_TEXT_TYPE_INFO's colours.
constexpr u32 kRed = ui::rgba(255, 26, 26);        // 1, 0.1, 0.1
constexpr u32 kGreen = ui::rgba(26, 255, 26);      // 0.1, 1, 0.1
constexpr u32 kBlue = ui::rgba(26, 26, 255);       // 0.1, 0.1, 1
constexpr u32 kPurple = ui::rgba(201, 77, 217);    // 0.79, 0.3, 0.85
constexpr u32 kGold = ui::rgba(255, 209, 0);       // 1, 0.82, 0

u32 colorOf(Kind kind) {
    switch (kind) {
    case Kind::SpellDamage: return kPurple;
    case Kind::Heal:
    case Kind::HealCrit:
    case Kind::PeriodicHeal:
    case Kind::AuraStart:
    case Kind::AuraEnd: return kGreen;
    case Kind::Mana:
    case Kind::Rage:
    case Kind::Focus:
    case Kind::Energy:
    case Kind::Faction:
    case Kind::ComboPoints:
    case Kind::HonorGained: return kBlue;
    case Kind::SpellActive: return kGold;
    default: return kRed;
    }
}

} // namespace

void CombatTextScreens::init(const game::GameData* data, const game::AbilityData* abilities) {
    m_data = data;
    m_abilities = abilities;
}

void CombatTextScreens::reset() {
    for (Line& l : m_lines) {
        l.alive = false;
    }
    m_cursorsSet = false;
    m_player = 0;
    m_auras.reset();
}

bool CombatTextScreens::shown(Kind kind, const SettingsView& s) {
    switch (kind) {
    case Kind::Damage:
    case Kind::DamageCrit:
    case Kind::SpellDamage:
    case Kind::Heal:
    case Kind::HealCrit:
    case Kind::PeriodicHeal: return true; // show = 1
    case Kind::Miss:
    case Kind::Dodge:
    case Kind::Parry:
    case Kind::Evade:
    case Kind::Immune:
    case Kind::Deflect: return s.combatTextDodgeParryMiss;
    case Kind::Block:
    case Kind::Absorb:
    case Kind::Resist: return s.combatTextResistances;
    case Kind::Mana:
    case Kind::Rage:
    case Kind::Focus:
    case Kind::Energy: return s.combatTextMana;
    case Kind::AuraStart:
    case Kind::AuraStartHarmful: return s.combatTextAuras;
    case Kind::AuraEnd:
    case Kind::AuraEndHarmful: return s.combatTextAuraFade;
    case Kind::Faction: return s.combatTextReputation;
    case Kind::SpellActive: return s.combatTextReactives;
    case Kind::HealthLow:
    case Kind::ManaLow: return s.combatTextLowHealthMana;
    case Kind::EnteringCombat:
    case Kind::LeavingCombat: return s.combatTextCombatState;
    case Kind::ComboPoints: return s.combatTextComboPoints;
    case Kind::HonorGained: return s.combatTextHonorGained;
    }
    return false;
}

void CombatTextScreens::add(Kind kind, const SettingsView& s, std::string_view name, i32 amount, i32 partial,
                            u32 nowMs) {
    if (!s.showCombatText || !shown(kind, s)) {
        return;
    }
    char text[64];
    char trailer[48];
    bool crit = false;
    bool staggered = false;
    const auto number = [&](const char* format, i32 value) {
        std::snprintf(text, sizeof(text), format, value);
        return std::string_view(text);
    };
    std::string_view message;
    switch (kind) {
    case Kind::DamageCrit: crit = true; [[fallthrough]];
    case Kind::Damage: staggered = kind == Kind::Damage; [[fallthrough]];
    case Kind::SpellDamage: message = number("-%d", amount); break;
    case Kind::Miss: staggered = true; message = text::get("MISS"); break;
    case Kind::Dodge: staggered = true; message = text::get("DODGE"); break;
    case Kind::Parry: staggered = true; message = text::get("PARRY"); break;
    case Kind::Evade: staggered = true; message = text::get("EVADE"); break;
    case Kind::Immune: message = text::get("IMMUNE"); break;
    case Kind::Deflect: message = text::get("DEFLECT"); break;
    case Kind::Block:
    case Kind::Absorb:
    case Kind::Resist: {
        const char* whole = kind == Kind::Block ? "BLOCK" : kind == Kind::Absorb ? "ABSORB" : "RESIST";
        const char* part = kind == Kind::Block ? "BLOCK_TRAILER" : kind == Kind::Absorb ? "ABSORB_TRAILER" : "RESIST_TRAILER";
        if (partial > 0) { // data.." "..format(BLOCK_TRAILER, arg3): the damage taken and what was stopped
            const std::string_view tail = text::format(trailer, text::Key(std::string_view(part)), partial);
            std::snprintf(text, sizeof(text), "%d %.*s", amount, static_cast<int>(tail.size()), tail.data());
            message = text;
        } else {
            message = text::get(text::Key(std::string_view(whole)));
        }
        break;
    }
    case Kind::HealCrit: crit = true; [[fallthrough]];
    case Kind::Heal:
    case Kind::PeriodicHeal:
        // COMBAT_TEXT_SHOW_FRIENDLY_NAMES: "+N [Healer]" for another's direct heal.
        if (s.combatTextFriendlyNames && kind != Kind::PeriodicHeal && !name.empty()) {
            std::snprintf(text, sizeof(text), "+%d [%.*s]", amount, static_cast<int>(name.size()), name.data());
            message = text;
        } else {
            message = number("+%d", amount);
        }
        break;
    case Kind::Mana:
    case Kind::Rage:
    case Kind::Focus:
    case Kind::Energy: {
        const std::string_view power = text::get(kind == Kind::Mana ? "MANA" : kind == Kind::Rage ? "RAGE"
                                                 : kind == Kind::Focus ? "FOCUS" : "ENERGY");
        std::snprintf(text, sizeof(text), amount > 0 ? "+%d %.*s" : "%d %.*s", amount, static_cast<int>(power.size()),
                      power.data());
        message = text;
        break;
    }
    case Kind::AuraStart:
    case Kind::AuraStartHarmful:
    case Kind::SpellActive:
        crit = kind == Kind::SpellActive;
        std::snprintf(text, sizeof(text), "<%.*s>", static_cast<int>(name.size()), name.data());
        message = text;
        break;
    case Kind::AuraEnd:
    case Kind::AuraEndHarmful: message = text::format(text, "AURA_END", name); break;
    case Kind::Faction:
        std::snprintf(text, sizeof(text), amount > 0 ? "(%.*s +%d)" : "(%.*s %d)", static_cast<int>(name.size()),
                      name.data(), amount);
        message = text;
        break;
    case Kind::HealthLow: message = text::get("HEALTH_LOW"); break;
    case Kind::ManaLow: message = text::get("MANA_LOW"); break;
    case Kind::EnteringCombat: message = text::get("ENTERING_COMBAT"); break;
    case Kind::LeavingCombat: message = text::get("LEAVING_COMBAT"); break;
    case Kind::ComboPoints:
        crit = amount >= 5; // MAX_COMBO_POINTS: shown as a crit
        message = text::format(text, amount == 1 ? "COMBAT_TEXT_COMBO_POINTS" : "COMBAT_TEXT_COMBO_POINTS_P1", amount);
        break;
    case Kind::HonorGained: {
        char value[16];
        std::snprintf(value, sizeof(value), amount > 0 ? "+%d" : "%d", amount);
        message = text::format(text, "COMBAT_TEXT_HONOR_GAINED", std::string_view(value));
        break;
    }
    }
    if (!message.empty()) {
        push(message, colorOf(kind), crit, staggered, s, nowMs);
    }
}

void CombatTextScreens::push(std::string_view text, u32 color, bool crit, bool staggered, const SettingsView& s,
                             u32 nowMs) {
    // CombatText_GetAvailableString: a free one, else the oldest.
    Line* line = &m_lines[0];
    for (Line& l : m_lines) {
        if (!l.alive) {
            line = &l;
            break;
        }
        if (l.startMs < line->startMs) {
            line = &l;
        }
    }
    const bool down = s.combatTextFloatMode == 2;
    const bool fountain = s.combatTextFloatMode == 3;
    // CombatText_AddMessage: under the lowest message still moving (over it, scrolling down), back at the start when
    // that is too far.
    f32 start = kStartY;
    for (const Line& l : m_lines) {
        if (!l.alive || &l == line || l.crit) {
            continue;
        }
        const f32 t = static_cast<f32>(nowMs - l.startMs) / 1000.0f;
        const f32 y = l.fountain ? l.startY + kFountainRadius * std::sin(1.5707963f * t / kScrollSeconds)
                                 : l.startY + (l.endY - kStartY) * t / kScrollSeconds;
        if (!down && start >= y - 16.0f - kSpacing) {
            start = y - 16.0f - kSpacing;
        } else if (down && start <= y + 16.0f + kSpacing) {
            start = y + 16.0f + kSpacing;
        }
    }
    if ((!down && start < kStartY - kMaxOffset) || (down && start > kStartY + kMaxOffset)) {
        start = kStartY;
    }
    m_xDir = m_xDir < 0.0f ? 1.0f : -1.0f;
    m_random = m_random * 1664525u + 1013904223u;
    const f32 stagger = staggered ? static_cast<f32>((m_random >> 8) % 21u) - kStagger * 0.5f : 0.0f;
    usize n = std::min(text.size(), line->text.size() - 1);
    std::copy_n(text.data(), n, line->text.data());
    line->text[n] = '\0';
    line->color = color;
    line->startMs = nowMs;
    line->startX = stagger;
    line->startY = crit ? kStartY : start;
    line->endY = crit ? kStartY : down ? kDownY : kUpY;
    line->xDir = m_xDir;
    line->crit = crit;
    line->fountain = fountain && !crit;
    line->alive = true;
}

void CombatTextScreens::update(GameSession& session, const SettingsView& s, u32 nowMs) {
    VD_PROFILE_ZONE("CombatTextScreens::update");
    if (!s.showCombatText || !session.inWorld()) {
        if (m_cursorsSet) {
            reset();
        }
        return;
    }
    game::Gameplay& play = session.play();
    const game::Entity* me = session.entities().find(session.playerGuid());
    if (!me) {
        return;
    }
    if (!m_cursorsSet || m_player != me->guid) {
        // From now on (the option just turned on, another character): nothing old comes back.
        m_cursorsSet = true;
        m_player = me->guid;
        m_combatCursor = play.combatLog().end();
        m_eventCursor = play.events().end();
        m_inCombat = (me->value(uf::UNIT_FIELD_FLAGS) & game::kUnitFlagInCombat) != 0;
        m_comboPoints = game::comboPoints(*me, play.target());
        m_auraState = me->value(uf::UNIT_FIELD_AURASTATE);
        m_auras.reset();
        const u64 units[] = {me->guid};
        m_auras.update(session.entities(), units, [](u64, u32, bool, bool) {});
    }
    // The combat log about the player (COMBAT_TEXT_UPDATE).
    while (const game::CombatEvent* e = play.combatLog().next(m_combatCursor)) {
        if (e->victim != me->guid || (e->flags & game::CombatEvent::kEnvironment) != 0) {
            continue;
        }
        using K = game::CombatEvent::Kind;
        const bool crit = (e->flags & game::CombatEvent::kCrit) != 0;
        switch (e->kind) {
        case K::Damage: {
            const Kind kind = e->spellId != 0 ? Kind::SpellDamage : crit ? Kind::DamageCrit : Kind::Damage;
            if (e->resisted > 0 && s.combatTextResistances) {
                add(Kind::Resist, s, {}, e->amount, static_cast<i32>(e->resisted), nowMs);
            } else if (e->blocked > 0 && s.combatTextResistances) {
                add(Kind::Block, s, {}, e->amount, static_cast<i32>(e->blocked), nowMs);
            } else if (e->absorbed > 0 && s.combatTextResistances) {
                add(Kind::Absorb, s, {}, e->amount, static_cast<i32>(e->absorbed), nowMs);
            } else {
                add(kind, s, {}, e->amount, 0, nowMs);
            }
            break;
        }
        case K::Miss: add(Kind::Miss, s, {}, 0, 0, nowMs); break;
        case K::Dodge: add(Kind::Dodge, s, {}, 0, 0, nowMs); break;
        case K::Parry: add(Kind::Parry, s, {}, 0, 0, nowMs); break;
        case K::Evade: add(Kind::Evade, s, {}, 0, 0, nowMs); break;
        case K::Immune: add(Kind::Immune, s, {}, 0, 0, nowMs); break;
        case K::Deflect: add(Kind::Deflect, s, {}, 0, 0, nowMs); break;
        case K::Block: add(Kind::Block, s, {}, 0, 0, nowMs); break;
        case K::Absorb: add(Kind::Absorb, s, {}, 0, 0, nowMs); break;
        case K::Resist: add(Kind::Resist, s, {}, 0, 0, nowMs); break;
        case K::Heal: {
            std::string_view healer;
            if (e->attacker != 0 && e->attacker != me->guid) {
                if (const std::string* n = session.playerName(e->attacker)) {
                    healer = *n;
                }
            }
            const bool periodic = (e->flags & game::CombatEvent::kPeriodic) != 0;
            add(periodic ? Kind::PeriodicHeal : crit ? Kind::HealCrit : Kind::Heal, s, healer, e->amount, 0, nowMs);
            break;
        }
        case K::Energize: {
            static constexpr Kind kPowers[] = {Kind::Mana, Kind::Rage, Kind::Focus, Kind::Energy};
            if (e->school < 4) {
                add(kPowers[e->school], s, {}, e->school == 1 ? e->amount / 10 : e->amount, 0, nowMs);
            }
            break;
        }
        default: break;
        }
    }
    // Reputation (COMBAT_TEXT_UPDATE "FACTION"); phase 17 (honor): every SMSG_PVP_CREDIT the client words
    // ("HONOR_GAINED", 0x626960: honor awarded, honorable and dishonorable kills).
    while (const game::GameEvent* e = play.events().next(m_eventCursor)) {
        if (e->kind == game::GameEventKind::ReputationChanged && m_abilities) {
            if (const game::FactionInfo* faction = m_abilities->factionByList(e->id)) {
                add(Kind::Faction, s, faction->name, e->value[0], 0, nowMs);
            }
        } else if (e->kind == game::GameEventKind::HonorGained) {
            add(Kind::HonorGained, s, {}, e->value[0], 0, nowMs);
        }
    }
    // Auras gained and faded.
    const u64 units[] = {me->guid};
    m_auras.update(session.entities(), units, [&](u64, u32 spellId, bool gained, bool harmful) {
        const std::optional<game::SpellInfo> spell = m_data ? m_data->spell(spellId) : std::nullopt;
        if (!spell || spell->name.empty()) {
            return;
        }
        const Kind kind = gained ? (harmful ? Kind::AuraStartHarmful : Kind::AuraStart)
                                 : (harmful ? Kind::AuraEndHarmful : Kind::AuraEnd);
        add(kind, s, spell->name, 0, 0, nowMs);
    });
    // PLAYER_REGEN_DISABLED / _ENABLED.
    const bool inCombat = (me->value(uf::UNIT_FIELD_FLAGS) & game::kUnitFlagInCombat) != 0;
    if (inCombat != m_inCombat) {
        m_inCombat = inCombat;
        add(inCombat ? Kind::EnteringCombat : Kind::LeavingCombat, s, {}, 0, 0, nowMs);
    }
    // UNIT_HEALTH / UNIT_MANA: once under 20% (again only after going back over it); mana for mana users only.
    const u32 maxHealth = me->value(uf::UNIT_FIELD_MAXHEALTH);
    const bool lowHealth = maxHealth > 0 && me->value(uf::UNIT_FIELD_HEALTH) <= static_cast<u32>(kLowThreshold * maxHealth);
    if (lowHealth && !m_lowHealth) {
        add(Kind::HealthLow, s, {}, 0, 0, nowMs);
    }
    m_lowHealth = lowHealth;
    const u32 maxMana = me->value(uf::UNIT_FIELD_MAXPOWER1);
    const bool lowMana = me->byteValue(uf::UNIT_FIELD_BYTES_0, 3) == 0 && maxMana > 0 &&
                         me->value(uf::UNIT_FIELD_POWER1) <= static_cast<u32>(kLowThreshold * maxMana);
    if (lowMana && !m_lowMana) {
        add(Kind::ManaLow, s, {}, 0, 0, nowMs);
    }
    m_lowMana = lowMana;
    // PLAYER_COMBO_POINTS: each new one.
    const u32 combo = game::comboPoints(*me, play.target());
    if (combo > m_comboPoints) {
        add(Kind::ComboPoints, s, {}, static_cast<i32>(combo), 0, nowMs);
    }
    m_comboPoints = combo;
    // SPELL_ACTIVE: a known spell the player's aura state just made usable (Revenge, Execute's caster states...).
    const u32 auraState = me->value(uf::UNIT_FIELD_AURASTATE);
    const u32 gainedStates = auraState & ~m_auraState;
    m_auraState = auraState;
    if (gainedStates != 0 && s.combatTextReactives && m_abilities && m_data) {
        for (const u32 id : play.spells().known()) {
            game::SpellDetails details;
            if (m_abilities->spellText().details(id, details) && details.casterAuraState != 0 &&
                details.casterAuraState <= 32 && (gainedStates & (1u << (details.casterAuraState - 1))) != 0) {
                if (const std::optional<game::SpellInfo> spell = m_data->spell(id)) {
                    add(Kind::SpellActive, s, spell->name, 0, 0, nowMs);
                    break; // its highest rank is enough
                }
            }
        }
    }
}

void CombatTextScreens::draw(DialogContext& c, const SettingsView& s, u32 nowMs) {
    if (!s.showCombatText) {
        return;
    }
    VD_PROFILE_ZONE("CombatTextScreens::draw");
    ui::Gui g{c.ui, c.draw};
    const f32 unit = g.unit(1.0f);
    const ui::Rect& screen = c.safeArea;
    // UIParent is 768 units high: positions keep their share of the screen, sizes stay in interface units.
    const f32 place = screen.h / 768.0f;
    for (Line& l : m_lines) {
        if (!l.alive) {
            continue;
        }
        const f32 t = static_cast<f32>(nowMs - l.startMs) / 1000.0f;
        if (t >= kScrollSeconds) {
            l.alive = false;
            continue;
        }
        f32 x = 0.0f, y = 0.0f;
        if (l.fountain) { // CombatText_FountainScroll
            x = l.xDir * (l.startX + kFountainRadius * std::cos(1.5707963f * t / kScrollSeconds)) - l.xDir * kFountainRadius;
            y = l.startY + kFountainRadius * std::sin(1.5707963f * t / kScrollSeconds);
        } else { // CombatText_StandardScroll
            x = l.startX;
            y = l.startY + (l.endY - kStartY) * t / kScrollSeconds;
        }
        f32 height = kHeight;
        if (l.crit) { // grows to 60 units in 0.05 s, back to 30 by 0.2 s
            height = t <= kCritScale ? kCritMin + (kCritMax - kCritMin) * t / kCritScale
                     : t <= kCritShrink ? kCritMax - (kCritMax - kCritMin) * (t - kCritScale) / (kCritShrink - kCritScale)
                                        : kCritMin;
        }
        const f32 alpha = t >= kFadeSeconds ? std::max(0.0f, 1.0f - (t - kFadeSeconds) / (kScrollSeconds - kFadeSeconds))
                                            : 1.0f;
        ui::TextStyle style = ui::gameFont(height * unit, ui::withAlpha(l.color, alpha));
        style.outline = std::max(1.0f, unit * 1.5f);
        style.shadow = 0.0f;
        style.align = ui::Align::Center;
        // SetPoint("TOP", UIParent, "BOTTOM", x, y): the text's top `y` units over the screen's bottom.
        const f32 top = screen.bottom() - y * place;
        g.draw.text(screen.centerX() + x * place, top + g.draw.ascent(style.font, style.size), l.text.data(), style);
    }
}

} // namespace vd
