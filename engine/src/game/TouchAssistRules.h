#pragma once

#include "core/Types.h"
#include "game/TouchLayout.h"

#include <glm/vec2.hpp>

#include <span>

namespace vd::game {

class AbilityData;
struct ShapeshiftForm;
struct SpellDetails;

// Phase 20 (assists, docs/MOBILE_CONTROLS.md §4 and §8): the rules and state machines of the touch cluster's assists,
// which app/TouchAssists runs against the game. Each one does at most what a 1.12 macro could (P3): it chooses the
// target the Target button would, starts the auto attack, takes a stance first, sends again what a held button asks,
// waits for the global cooldown's last moment, casts a helpful spell on a unit frame or a ground spell where the finger
// aims. Plain data and functions; no allocation.

// The player's choices (the interface options' touch section), with §8's defaults.
struct TouchAssistOptions {
    bool autoTarget = true;     // a hostile spell without a hostile target takes the Target button's unit
    bool startAttack = true;    // Heroic Strike, Raptor Strike and the seals start the auto attack
    bool repeatHeld = true;     // a held button fires again whenever it can
    bool stances = true;        // warrior: a spell of another stance takes that stance first
    bool druidForms = false;    // druid: a caster spell in an animal form leaves the form first
    bool dragToFrame = true;    // a helpful spell dragged onto a unit frame lands on that unit
    bool aimGround = true;      // a ground spell dragged out of its button aims around the player
    bool spellQueue = false;    // a press in the last 200 ms of the global cooldown or a cast waits for its end
    bool haptics = true;        // vibration (a cast, a reactive ability, a refusal)
    bool siteTemplates = false; // the template of the place entered (the world, a dungeon, a raid, a battleground)
};

// --- §8 "Iniciar el ataque" ------------------------------------------------------------------------------------

// When casting `spell` at a hostile target starts the auto attack (CMSG_ATTACKSWING). 1.12's client does it itself
// for SPELL_ATTR_EX_INITIATES_COMBAT as it casts (Sinister Strike, Mortal Strike, Claw, Maul, Cleave...) and for
// SPELL_ATTR_EX2_INITIATE_COMBAT_POST_CAST once SMSG_SPELL_GO comes (Judgement, Ambush, Cheap Shot, Garrote, Pounce,
// Ravage: vMaNGOS' SpellDefines.h names both); the assist (`assist`) adds the "attack macro" everybody used then: the
// on-next-swing spells that lack the flag (Heroic Strike, Raptor Strike) and the paladin's seals (SpellFamilyFlags
// of IsSealSpell).
enum class AttackStart : u8 { None, AtCast, AfterCast };
AttackStart attackStartOf(const SpellDetails& spell, bool assist);

// --- §8 "Repetir manteniendo" ----------------------------------------------------------------------------------

constexpr u32 kTouchRepeatEveryMs = 250; // a held button asks at most this often
constexpr u32 kTouchInFlightMs = 600;    // ...never while its last request waits for an answer (a lost one: this long)
constexpr u32 kTouchRefusedMs = 1000;    // ...nor this long after a refusal (no rage yet, out of range...)
constexpr u32 kTouchNotReadyMs = 250;    // ...or this long after "not ready yet" (the cooldown ends any moment)

// Whether a held button may send its spell again: one request on its way at a time (sent, and the server has neither
// started it, cast it nor refused it: SMSG_SPELL_START / SMSG_SPELL_GO / SMSG_CAST_RESULT), at most one every
// kTouchRepeatEveryMs, and none for a while after a refusal (the server would refuse it again). Presses are always
// sent (the player asked); they count as requests too.
class TouchRepeatGate {
public:
    bool ready(u32 nowMs) const;
    void sent(u32 spellId, u32 nowMs);
    // The player's cast of `spellId` started or went off.
    void answered(u32 spellId);
    // The server (or the client's own check) refused `spellId` (SpellCastResult `reason`).
    void refused(u32 spellId, u32 reason, u32 nowMs);
    u32 waiting() const { return m_spell; } // the request on its way (0: none)
    void reset() { *this = {}; }

private:
    u32 m_spell = 0;
    u32 m_sentMs = 0;
    bool m_sentAny = false;
    u32 m_blockedUntilMs = 0;
    bool m_blocked = false;
};

// --- §8 "Cola de hechizos" -------------------------------------------------------------------------------------

constexpr u32 kTouchQueueWindowMs = 200; // a press this close to the end of the global cooldown or a cast waits
constexpr u32 kTouchQueueGraceMs = 400;  // ...and is forgotten this long after it should have gone

// A press within the last kTouchQueueWindowMs of the global cooldown or of the player's cast is kept and sent the
// moment it ends, instead of the server's "not ready yet" (off by default: on a LAN the delay is short anyway). One
// press waits at a time: the last one.
class TouchSpellQueue {
public:
    // A press of `spellId` whose way is blocked `blockedMs` more (the global cooldown's or the cast's time left, 0:
    // free; a spell waiting for its own cooldown is not offered). True when it waits.
    bool offer(u32 spellId, u32 blockedMs, u32 nowMs);
    // Every frame: the spell to send now (`blockedMs` as above; 0: none).
    u32 due(u32 blockedMs, u32 nowMs);
    u32 waiting() const { return m_spell; }
    void clear() { m_spell = 0; }

private:
    u32 m_spell = 0;
    u32 m_untilMs = 0;
};

// --- §8 "Asistencia de posturas / formas" ----------------------------------------------------------------------

constexpr u32 kTouchFormWaitMs = 1500; // a spell waits this long for its form, then is forgotten

// What `spell` needs before it can be cast from the form `current` (UNIT_FIELD_BYTES_1 byte 2): a warrior's stance
// among the player's `forms` (with `stances`: Overpower from Berserker Stance takes Battle Stance), or leaving a
// druid's animal form (with `druidForms`: Healing Touch in Cat Form cancels Cat Form). None when it can be cast now
// or no form of the player's would do (AbilityData::castableInForm decides, as the action bars).
struct TouchFormChange {
    u32 form = 0;       // the form wanted (0: none, the caster's own)
    u32 spellId = 0;    // the spell that takes it (0 when leaving)
    u32 leaveSpell = 0; // the form spell whose aura is cancelled (CMSG_CANCEL_AURA), 0 none
    bool needed() const { return spellId != 0 || leaveSpell != 0; }
};
TouchFormChange touchFormChange(const SpellDetails& spell, u32 current, std::span<const ShapeshiftForm> forms,
                                const AbilityData& data, bool stances, bool druidForms);

// The spell waiting for its form: sent once the server shows the form, within kTouchFormWaitMs, else forgotten.
class TouchFormSwitch {
public:
    void start(u32 spellId, u64 target, u32 form, u32 nowMs);
    struct Due {
        u32 spellId = 0;
        u64 target = 0;
    };
    // Every frame with the player's form now: the spell to cast (0 none).
    Due poll(u32 form, u32 nowMs);
    u32 waiting() const { return m_spell; }
    u32 form() const { return m_form; }
    void clear() { m_spell = 0; }

private:
    u32 m_spell = 0;
    u64 m_target = 0;
    u32 m_form = 0;
    u32 m_untilMs = 0;
};

// --- §8 "Plantilla por sitio" ----------------------------------------------------------------------------------

// The place a map is (Map.dbc's instance type: 0 the world, 1 a dungeon, 2 a raid, 3 a battleground).
TouchSite touchSiteOf(u32 instanceType);
// The first template of `layout` set for `site` (-1: none).
i32 touchTemplateForSite(const TouchLayout& layout, TouchSite site);
// The template to switch to when the player enters another site (-1: none: the same site, an unknown one, or no
// template for it). The player's own choice inside a site stays until the next one.
class TouchSiteSwitch {
public:
    i32 update(TouchSite site, const TouchLayout& layout);
    void reset() { m_site = TouchSite::None; }

private:
    TouchSite m_site = TouchSite::None;
};

// --- §4 "Apuntar arrastrando" ----------------------------------------------------------------------------------

// Where a ground spell dragged out of its button lands, around the player as the camera sees it (MOBA style): the
// finger's offset from the button as a share of the aiming pad (`nx` right, `ny` down on the screen, |n| <= 1: 0 at
// the button's rim, 1 at the pad's edge), `forward` the camera's horizontal direction (normalized, world x / y),
// `right` the screen's right on the ground (normalized), `range` yards. Returns the offset from the player (world
// x / y, yards).
glm::vec2 touchAimOffset(f32 nx, f32 ny, glm::vec2 forward, glm::vec2 right, f32 range);

} // namespace vd::game
