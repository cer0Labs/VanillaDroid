#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "game/AbilityData.h"
#include "game/CombatText.h"
#include "game/GameData.h"

#include <array>
#include <string_view>

namespace vd {

class GameSession;

// Phase 15 (options): Blizzard_CombatText, the floating combat text of 1.12.1 ("Enable Floating Combat Text": off by
// default): what happens to the player scrolls up from the middle of the screen, or down, or in arcs
// (COMBAT_TEXT_FLOAT_MODE), each kind as COMBAT_TEXT_TYPE_INFO colours it and the options show it. The C side's
// COMBAT_TEXT_UPDATE is built here from the game: the combat log about the player (damage, misses, blocks, resists,
// absorbs, heals with the healer's name, power gains), its auras gained and faded (their update fields), entering and
// leaving combat (PLAYER_REGEN), health and mana under 20%, combo points, reputation (ReputationChanged) and reactive
// abilities lit by the player's aura state; phase 17 (honor): honor (SMSG_PVP_CREDIT, game::Honor's
// HonorGained).
//
// The animation as CombatText_OnUpdate: 20 strings (NUM_COMBAT_TEXT_LINES) at 25 units (COMBAT_TEXT_HEIGHT), 1.9 s
// across (COMBAT_TEXT_SCROLLSPEED), fading after 1.3 s, from 384 units over the screen's bottom to 609 (up) or 159
// (down); a new message starts under the lowest moving one (16 units + COMBAT_TEXT_SPACING apart, back to the start past
// COMBAT_TEXT_MAX_OFFSET); damage and misses stagger by up to 20 units; crits grow from 30 to 60 units and shrink back
// in 0.2 s and stay at the start. Main thread; nothing allocates.
class CombatTextScreens {
public:
    void init(const game::GameData* data, const game::AbilityData* abilities);
    // Reads this frame's events (only with the option on: off, nothing is gathered, as the addon unregisters them).
    void update(GameSession& session, const SettingsView& settings, u32 nowMs);
    // Draws the messages over `c`'s whole screen (under the windows).
    void draw(DialogContext& c, const SettingsView& settings, u32 nowMs);
    // A new world, or the option turned off: everything gone.
    void reset();

    // The kinds of COMBAT_TEXT_TYPE_INFO this client sends.
    enum class Kind : u8 {
        Damage, DamageCrit, SpellDamage, Miss, Dodge, Parry, Evade, Immune, Deflect, Block, Absorb, Resist,
        Heal, HealCrit, PeriodicHeal, Mana, Rage, Focus, Energy, AuraStart, AuraStartHarmful, AuraEnd, AuraEndHarmful,
        Faction, SpellActive, HealthLow, ManaLow, EnteringCombat, LeavingCombat, ComboPoints, HonorGained,
    };
    // One message as CombatText_OnEvent words and filters it (`text`: a name, `amount`: the number; `partial`: a
    // partial block / absorb / resist's amount). Shown only when its option is on. Public for the gallery and tests.
    void add(Kind kind, const SettingsView& settings, std::string_view text, i32 amount, i32 partial, u32 nowMs);
    // Whether a kind shows with these settings (COMBAT_TEXT_TYPE_INFO's show / var).
    static bool shown(Kind kind, const SettingsView& settings);

private:
    struct Line {
        std::array<char, 64> text{};
        u32 color = 0;
        u32 startMs = 0;
        f32 startX = 0.0f, startY = 0.0f, endY = 0.0f; // units over the screen's bottom centre
        f32 xDir = 1.0f;
        bool crit = false;
        bool fountain = false;
        bool alive = false;
    };
    void push(std::string_view text, u32 color, bool crit, bool staggered, const SettingsView& settings, u32 nowMs);

    const game::GameData* m_data = nullptr;
    const game::AbilityData* m_abilities = nullptr;
    std::array<Line, 20> m_lines{};
    f32 m_xDir = 1.0f;
    u32 m_random = 0x12345u;

    // What was seen of the player last frame.
    u64 m_player = 0;
    u32 m_combatCursor = 0;
    u32 m_eventCursor = 0;
    bool m_cursorsSet = false;
    bool m_inCombat = false;
    bool m_lowHealth = false;
    bool m_lowMana = false;
    u32 m_comboPoints = 0;
    u32 m_auraState = 0;
    game::AuraWatch m_auras;
};

} // namespace vd
