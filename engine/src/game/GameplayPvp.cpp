// Phase 17 (honor): what a tap does with the battlegrounds' dead: a spirit guide without a gossip menu, and
// Remove Insignia on an enemy's corpse or on its body before it is released (game::Gameplay's part, kept
// apart from Gameplay.cpp). Read in WoW.exe 1.12.1 (read-only disassembly): the NPC chain of Player_C
// (0x5F0130: bit 0x40 -> 0x5DF950), the corpse's right click (0x5D6BF0..0x5D6CD7), the dead unit's (0x60C01F)
// and the cast they share (0x5F05E0: the spell learned with effect 116, 0x4B263B -> 0xB700E8).
#include "game/Gameplay.h"

#include "game/CharacterSheet.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"

namespace vd::game {
namespace {

namespace uf = net::uf;

constexpr u32 kEffectSkinPlayerCorpse = 116; // SPELL_EFFECT_SKIN_PLAYER_CORPSE ("Remove Insignia")
// CORPSE_FLAG_LOOTABLE: a battleground corpse (Player::CreateCorpse).
constexpr u32 kCorpseFlagLootable = 0x20;

} // namespace

u32 Gameplay::insigniaSpell() const {
    for (const u32 id : m_spells.known()) {
        SpellInfo info;
        if (!m_spellSource.get(id, info)) {
            continue;
        }
        for (const u32 effect : info.effects) {
            if (effect == kEffectSkinPlayerCorpse) {
                return id;
            }
        }
    }
    return 0;
}

Interaction Gameplay::interactSpiritGuide(const Entity& unit) {
    // 0x5DF950: for a ghost the healer is chosen again, which asks the server for its time; alive, nothing.
    return m_areaSpiritHealer.select(unit.guid, m_nowMs) ? Interaction::SpiritHealer : Interaction::None;
}

Interaction Gameplay::removeInsignia(const Entity& target) {
    const Entity* player = me();
    if (!player || isDead(*player)) {
        return Interaction::None;
    }
    net::SpellCastTargets t;
    if (target.type == ObjectType::Corpse) {
        // 0x5D6C96: a corpse with CORPSE_FLAG_LOOTABLE that is not the player's side's (its race: 0x6067D0
        // asks the reaction to the race's faction; the Alliance's races' templates are friendly to each
        // other, the Horde's too). Bones (CORPSE_FLAG_BONES, the insignia taken) have lost the flag.
        if ((target.value(uf::CORPSE_FIELD_FLAGS) & kCorpseFlagLootable) == 0 ||
            allianceRace(target.byteValue(uf::CORPSE_FIELD_BYTES_1, 1)) ==
                allianceRace(player->byteValue(uf::UNIT_FIELD_BYTES_0, 0))) {
            return Interaction::None;
        }
        t.mask = net::kTargetCorpseEnemy;
        t.corpse = target.guid;
    } else {
        // 0x60C01F: a dead player still in its body, skinnable (BattleGround::HandleKillPlayer), the player
        // cannot assist (vMaNGOS takes an unreleased body as a unit: GetAllowedTargetMask).
        if (target.type != ObjectType::Player ||
            (target.value(uf::UNIT_FIELD_FLAGS) & kUnitFlagSkinnable) == 0 ||
            reactionTo(*player, target, m_reactions) == Reaction::Friendly) {
            return Interaction::None;
        }
        t.mask = net::kTargetUnit;
        t.unit = target.guid;
    }
    const u32 spell = insigniaSpell();
    if (spell == 0) {
        return Interaction::None; // never learned: WoW.exe does nothing
    }
    // The server checks the range (Remove Insignia: "Very Short Range", 10 yd) and answers SMSG_CAST_FAILED;
    // on success the bones' loot window opens by itself (Map's corpse removal: SendLoot LOOT_INSIGNIA).
    return cast(spell, t) ? Interaction::Loot : Interaction::None;
}

} // namespace vd::game
