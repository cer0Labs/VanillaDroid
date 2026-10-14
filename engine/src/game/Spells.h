#pragma once

#include "core/Types.h"
#include "game/GameData.h"
#include "game/SpellMods.h"
#include "net/GameMessages.h"
#include "net/Messages.h"

#include <array>
#include <span>
#include <vector>

namespace vd::game {

// Spell data for the timing rules (cooldowns, global cooldown): GameData in the game, a table in tests.
struct SpellSource {
    bool (*lookup)(const void* context, u32 spellId, SpellInfo& out) = nullptr;
    const void* context = nullptr;

    bool get(u32 spellId, SpellInfo& out) const { return lookup && lookup(context, spellId, out); }
};
SpellSource spellSource(const GameData& data);

// SPELL_ATTR_COOLDOWN_ON_EVENT: the cooldown starts on SMSG_COOLDOWN_EVENT (an aura fading...), not on
// casting.
constexpr u32 kSpellAttrCooldownOnEvent = 0x02000000;

// The cooldown values an item gives its on-use spell (the item query's, as Player::AddCooldown records them when
// the spell is cast from the item): the item's category (0: the spell's own), its recovery and its category's
// recovery (< 0: the spell's own, Spell.dbc).
struct ItemSpellCooldown {
    u32 category = 0;
    i32 recoveryMs = -1;
    i32 categoryRecoveryMs = -1;
};

// One of the 120 action buttons (action bar pages and the class bars).
struct ActionButton {
    u8 type = net::kActionSpell; // net::kAction*
    u32 action = 0;              // spell id, item entry or macro id
    bool empty() const { return type == net::kActionSpell && action == 0; }
    u32 packed() const { return (action & 0x00FFFFFFu) | (static_cast<u32>(type) << 24); }
    static ActionButton unpack(u32 packed) { return {static_cast<u8>(packed >> 24), packed & 0x00FFFFFFu}; }
};

// A cast in progress (or a channel, which empties instead of filling).
struct CastBar {
    u32 spellId = 0; // 0 = none
    u64 caster = 0;
    u64 target = 0;
    u32 startMs = 0;
    u32 durationMs = 0;
    bool channel = false;

    bool active() const { return spellId != 0; }
    u32 elapsed(u32 nowMs) const { return nowMs - startMs < durationMs ? nowMs - startMs : durationMs; }
    u32 remaining(u32 nowMs) const { return durationMs - elapsed(nowMs); }
    // 0..1: how full the bar is (a channel goes from 1 to 0).
    f32 progress(u32 nowMs) const {
        const f32 done = durationMs ? static_cast<f32>(elapsed(nowMs)) / static_cast<f32>(durationMs) : 1.0f;
        return channel ? 1.0f - done : done;
    }
};

// When a spell can be cast again.
struct CooldownView {
    u32 startMs = 0;
    u32 durationMs = 0;
    // Waits for an event (the cooldown starts when an aura fades...): no end known yet.
    bool untilEvent = false;

    bool active(u32 nowMs) const { return untilEvent || nowMs - startMs < durationMs; }
    u32 remaining(u32 nowMs) const {
        return untilEvent ? 0xFFFFFFFFu : (nowMs - startMs < durationMs ? durationMs - (nowMs - startMs) : 0);
    }
};

// The player's spells: what it knows, its action buttons, its cast bar, cooldowns and the global cooldown;
// plus the cast bars of other units (SMSG_SPELL_START is broadcast). Fed by Gameplay from the spell packets.
class SpellBook {
public:
    SpellBook();
    void setSource(SpellSource source) { m_source = source; }
    // Phase 13 (spells): the player's spell modifiers (SPELLMOD_COOLDOWN on the cooldowns it starts,
    // SPELLMOD_GLOBAL_COOLDOWN on the global cooldown, as WoW.exe applies them); null: none.
    void setMods(const SpellMods* mods) { m_mods = mods; }
    void reset(); // new session

    // Known spells, sorted by id.
    void onInitialSpells(const net::InitialSpells& m, u32 nowMs);
    void onLearned(u32 spellId);
    void onRemoved(u32 spellId);
    void onSuperceded(u32 oldSpellId, u32 newSpellId);
    bool knows(u32 spellId) const;
    std::span<const u32> known() const { return m_known; }
    // Bumped whenever the known spells change (views built from them, like the stance bar, rebuild then).
    u32 knownVersion() const { return m_knownVersion; }

    // Action buttons.
    void onActionButtons(const std::array<u32, net::kActionButtonCount>& buttons);
    ActionButton button(u32 slot) const;
    void setButton(u32 slot, ActionButton button);

    // The player's cast bar. `player`: the player's GUID.
    const CastBar& cast() const { return m_cast; }
    // CMSG_CAST_SPELL sent: starts the global cooldown at once (undone if the server refuses the cast).
    void onCastRequested(u32 spellId, u32 nowMs);
    void onSpellStart(const net::SpellStart& m, u64 player, u32 nowMs);
    // True when it finished the player's cast. `item`: the cast came from an item (SMSG_SPELL_GO's caster is the
    // item): its cooldown values instead of the spell's (potions share the item's category).
    bool onSpellGo(const net::SpellGo& m, u64 player, u32 nowMs, const ItemSpellCooldown* item = nullptr);
    void onSpellFailedOther(const net::SpellFailedOther& m, u64 player);
    void onCastResult(const net::CastResult& m);
    void onSpellDelayed(const net::SpellDelayed& m, u64 player);
    void onChannelStart(const net::ChannelStart& m, u64 player, u32 nowMs);
    void onChannelUpdate(u32 remainingMs, u32 nowMs);
    void clearCast() { m_cast = CastBar{}; }

    // Another unit's cast in progress (target frames); nullptr if none.
    const CastBar* unitCast(u64 guid, u32 nowMs) const;

    // Cooldowns.
    void onSpellCooldown(const net::SpellCooldown& m, u64 player, u32 nowMs);
    void onCooldownEvent(u32 spellId, u32 nowMs);
    void onClearCooldown(u32 spellId);
    // The longest of the spell's own cooldown, its category's and the global cooldown it shares.
    CooldownView cooldown(u32 spellId, u32 nowMs) const;
    u32 remaining(u32 spellId, u32 nowMs) const { return cooldown(spellId, nowMs).remaining(nowMs); }
    // The global cooldown (category 133), for a sweep over every button.
    CooldownView globalCooldown(u32 nowMs) const;
    // The same as cooldown() with the category given (an item's, not the spell's): the spell's own cooldown, the
    // category's (0: none) and the spell's global cooldown.
    CooldownView cooldown(u32 spellId, u32 category, u32 nowMs) const;
    // The same with the spell's data at hand (no lookup: the HUD keeps its buttons' spells).
    CooldownView cooldown(const SpellInfo& spell, u32 category, u32 nowMs) const;
    // An item's cooldown (GetActionCooldown / GetContainerItemCooldown): the longest of its on-use spells', each
    // with the item's category (every healing potion shares one), as onSpellGo recorded them.
    CooldownView itemCooldown(const net::ItemQueryInfo& item, u32 nowMs) const;

    // The on-next-swing spell waiting for the player's next melee swing (Heroic Strike, Maul, Raptor Strike,
    // Cleave): the client's current action (a checked button) from the cast request until it goes off or fails;
    // 0 none. Forgotten after kQueuedSwingMs (the swing never came).
    u32 queuedSwing(u32 nowMs) const;
    static constexpr u32 kQueuedSwingMs = 10000;

private:
    struct Cooldown {
        u32 spellId = 0;
        u32 category = 0;
        u32 startMs = 0;
        u32 spellMs = 0;    // own recovery
        u32 categoryMs = 0; // recovery of the whole category
        bool untilEvent = false;
    };
    struct Gcd {
        u32 category = 0;
        u32 startMs = 0;
        u32 durationMs = 0;
        u32 predictedSpell = 0; // started by onCastRequested and not yet confirmed
    };
    struct UnitCast {
        u64 guid = 0;
        CastBar bar;
    };

    void startCooldown(u32 spellId, u32 category, u32 spellMs, u32 categoryMs, bool untilEvent, u32 nowMs);
    CooldownView cooldownOf(u32 spellId, u32 category, u32 gcdCategory, u32 nowMs) const;
    void startGcd(u32 category, u32 durationMs, u32 nowMs, u32 predictedSpell);
    void prune(u32 nowMs);

    // Phase 13 (spells): a spell's global cooldown with SPELLMOD_GLOBAL_COOLDOWN.
    u32 gcdOf(const SpellInfo& info) const;

    SpellSource m_source;
    const SpellMods* m_mods = nullptr; // phase 13 (spells)
    std::vector<u32> m_known;
    u32 m_knownVersion = 1;
    u32 m_queuedSwing = 0; // on-next-swing spell asked for (queuedSwing)
    u32 m_queuedSwingMs = 0;
    std::array<u32, net::kActionButtonCount> m_buttons{};
    CastBar m_cast;
    std::array<UnitCast, 16> m_unitCasts{};
    std::vector<Cooldown> m_cooldowns;
    std::array<Gcd, 4> m_gcds{};
};

} // namespace vd::game
