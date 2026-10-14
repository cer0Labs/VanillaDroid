#pragma once

#include "core/Types.h"
#include "game/Entities.h"

#include <array>
#include <span>

namespace vd::game {

struct SpellDetails;

constexpr u32 kMaxAuras = 48;         // MAX_AURAS: visible aura slots of a unit
constexpr u32 kMaxPositiveAuras = 32; // buffs use slots 0..31, debuffs 32..47

// A visible aura of a unit, from its update fields (UNIT_FIELD_AURA*, Spells/SpellAuras.cpp).
struct AuraInfo {
    static constexpr u8 kCancelable = 0x1; // AFLAG_CANCELABLE: the player may remove it (CMSG_CANCEL_AURA)
    u32 spellId = 0;
    u8 slot = 0;
    u8 stacks = 1; // applications (charges x stack amount), at least 1
    u8 casterLevel = 0;
    u8 flags = 0;       // AFLAG_* nibble: kCancelable, 0x8 / 0x4 / 0x2 = effect 0 / 1 / 2 present
    u32 durationMs = 0; // the player's own auras only (SMSG_UPDATE_AURA_DURATION); 0 = unknown or permanent
    u32 remainingMs = 0;

    bool positive() const { return slot < kMaxPositiveAuras; }
    bool cancelable() const { return (flags & kCancelable) != 0; }
    bool timed() const { return durationMs != 0; }
};

// The unit's visible auras in slot order (buffs first). Returns how many were written to `out`.
u32 readAuras(const Entity& unit, std::span<AuraInfo> out);

// Parity (auras): whether the server removes this aura of the player's on CMSG_CANCEL_AURA (the client's
// CancelPlayerBuff; WorldSession::HandleCancelAuraOpcode): a buff it flagged cancelable (AFLAG_CANCELABLE: positive,
// without SPELL_ATTR_NO_AURA_CANCEL), not passive, shown (no SPELL_ATTR_DO_NOT_DISPLAY, no SPELL_ATTR_EX_NO_AURA_ICON
// without an active icon), and not an area aura somebody else keeps up (a party member's paladin aura: the server
// only lets its caster's own go). `ownSpell`: the player knows the spell (the caster could be itself).
bool canCancelAura(const AuraInfo& aura, const SpellDetails& spell, bool ownSpell);

// Durations of the player's own auras: the server sends them per slot, only when an aura gets a slot or is
// refreshed, and usually just before the field update that shows the aura. A duration is dropped when its
// slot changes aura later than kGraceMs after it arrived.
class AuraDurations {
public:
    static constexpr u32 kGraceMs = 2000;

    void reset();
    void onDuration(u8 slot, u32 durationMs, u32 nowMs);
    // After every update of the player's fields.
    void onPlayerFields(const Entity& player, u32 nowMs);
    // Fills durationMs / remainingMs of the player's auras read by readAuras().
    void apply(std::span<AuraInfo> auras, u32 nowMs) const;

private:
    struct Slot {
        bool valid = false;
        u32 durationMs = 0;
        u32 receivedMs = 0;
    };
    std::array<Slot, kMaxAuras> m_slots{};
    std::array<u32, kMaxAuras> m_seen{}; // spell id in each slot at the last look
};

} // namespace vd::game
