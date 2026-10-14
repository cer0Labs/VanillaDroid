// Phase 9: the combat animations of the server's units (Units' part): what they hold, and the swings, shots,
// reactions and emotes of Gameplay's combat log and fx rings played as actions over their movement.
#include "app/GameSession.h"
#include "app/Units.h"
#include "core/Clock.h"
#include "core/Profile.h"
#include "game/Appearance.h"
#include "game/Gameplay.h"
#include "game/SpellVisualData.h"
#include "net/GameMessages.h"
#include "net/UpdateFields.gen.h"
#include "world/Actors.h"
#include "world/UnitBodyEffects.h"

namespace vd {
namespace {

namespace uf = net::uf;
namespace animation = anim::animation;

constexpr u32 kVisibleItemStride = uf::PLAYER_VISIBLE_ITEM_2_0 - uf::PLAYER_VISIBLE_ITEM_1_0;
constexpr u32 kSheathMelee = 1, kSheathRanged = 2; // UNIT_FIELD_BYTES_2 byte 0 (SheathState)
constexpr u32 kCombatStanceMs = 5000;              // the ready stance after the last swing or hit
constexpr u32 kRangedStanceMs = 3500;              // the ranged one after the last shot
constexpr u32 kSwingMs = 600;                      // a swing is not cut by a wound before this
constexpr u32 kEmoteEchoMs = 600; // a text emote right after its SMSG_EMOTE plays nothing more

bool later(u32 a, u32 b) {
    return static_cast<i32>(a - b) > 0;
}

// The ranged weapon a shot uses when the unit's own is unknown: from the spell's weapon mask.
game::WeaponKind rangedOfMask(u32 mask) {
    if (mask == 0x8) {
        return game::WeaponKind::Gun;
    }
    if (mask == 0x10000) {
        return game::WeaponKind::Thrown;
    }
    if (mask == 0x80000) {
        return game::WeaponKind::Wand;
    }
    return game::WeaponKind::Bow;
}

} // namespace

game::UnitGear Units::gearOf(GameSession& session, const game::Entity& e, u8 sheath) const {
    game::UnitGear gear;
    gear.sheathed = sheath != kSheathMelee && sheath != kSheathRanged;
    if (e.type == game::ObjectType::Player) {
        // Its visible items: the item answers say the class (asked for when unknown; the look waits for them
        // too).
        const auto kindOf = [&](u32 slot) {
            const u32 entry =
                e.value(static_cast<u16>(uf::PLAYER_VISIBLE_ITEM_1_0 + slot * kVisibleItemStride));
            const net::ItemQueryInfo* item = entry != 0 ? session.item(entry) : nullptr;
            return item ? game::weaponKind(item->itemClass, item->subclass, item->inventoryType)
                        : game::WeaponKind::None;
        };
        gear.mainHand = kindOf(game::kSlotMainHand);
        gear.offHand = kindOf(game::kSlotOffHand);
        gear.ranged = kindOf(game::kSlotRanged);
        return gear;
    }
    // Creatures: UNIT_VIRTUAL_ITEM_INFO, two fields per slot (class, subclass, material, inventory type).
    std::array<game::WeaponKind, 3> kinds{};
    for (u32 i = 0; i < 3; ++i) {
        const u16 info = static_cast<u16>(uf::UNIT_VIRTUAL_ITEM_INFO + i * 2);
        if (e.value(static_cast<u16>(uf::UNIT_VIRTUAL_ITEM_SLOT_DISPLAY + i)) != 0) {
            kinds[i] = game::weaponKind(e.byteValue(info, 0), e.byteValue(info, 1), e.byteValue(info, 3));
        }
    }
    gear.mainHand = kinds[0];
    gear.offHand = kinds[1];
    gear.ranged = kinds[2];
    return gear;
}

bool Units::emoteShown(const Actors& actors, const Unit& unit, u16 animation) {
    // Ours (seated animations): SMSG_EMOTE's handler (0x5E66B0) and PlayEmote (0x5FCD20): not on a unit asleep or
    // swimming, nor channeling, nor again over itself.
    constexpr u8 kStandStateSleep = 3;
    return unit.standState != kStandStateSleep && !unit.swimming && !unit.channeling &&
           (unit.actor == ~0u || actors.action(unit.actor) != animation);
}

void Units::playAction(Actors& actors, Unit& unit, u16 animation) {
    if (animation != game::kNoAnimation && unit.actor != ~0u) {
        actors.playAction(unit.actor, animation);
    }
}

void Units::readCombat(const game::Gameplay& play, Actors& actors, u64 player, u32 nowMs) {
    VD_PROFILE_ZONE("Units::readCombat");
    const Clock::time_point start = Clock::now();
    struct Timer {
        const Clock::time_point& start;
        f32& out;
        ~Timer() { out = static_cast<f32>(secondsBetween(start, Clock::now()) * 1000.0); }
    } timer{start, m_combatStats.readMs};
    if (player != m_cursorsPlayer) {
        // Another world (or character): from now on.
        m_cursorsPlayer = player;
        m_fxCursor = play.fx().end();
        m_combatCursor = play.combatLog().end();
        return;
    }
    const auto find = [&](u64 guid) -> Unit* {
        const auto it = guid != 0 ? m_units.find(guid) : m_units.end();
        return it == m_units.end() || it->second.actor == ~0u ? nullptr : &it->second;
    };

    // Melee swings and the victims' reactions; spell damage wounds (unless the spell's impact animates).
    while (const game::CombatEvent* e = play.combatLog().next(m_combatCursor)) {
        if ((e->flags & game::CombatEvent::kEnvironment) != 0 || e->logOnly()) {
            continue; // phase 13: dispels, enchantments, items made... move nobody
        }
        Unit* attacker = find(e->attacker);
        Unit* victim = find(e->victim);
        const bool melee = e->swing(); // phase 13: not a damage shield's hit either
        if (melee && attacker) {
            playAction(actors, *attacker,
                       game::swingAnimation(attacker->gear, (e->flags & game::CombatEvent::kOffHand) != 0));
            attacker->swingUntilMs = nowMs + kSwingMs;
            attacker->combatUntilMs = nowMs + kCombatStanceMs;
            ++m_combatStats.swings;
        }
        if (!victim || e->kind == game::CombatEvent::Kind::Heal ||
            e->kind == game::CombatEvent::Kind::Energize) {
            continue;
        }
        if (!melee && m_visualData) {
            const game::SpellVisualInfo* visual = m_visualData->spellVisual(e->spellId);
            const game::VisualKit* impact = visual ? m_visualData->kit(visual->impactKit) : nullptr;
            if (impact && impact->animation != game::kNoKitAnimation) {
                continue; // SpellVisuals plays the impact's own animation when it lands
            }
        }
        // Phase 14 (bodyfx): a melee blow that landed bleeds (WoW.exe 0x624530: HITINFO_AFFECTS_VICTIM, damage, the
        // victim wounded or interrupted), more with HITINFO_BLOOD_SPURT.
        if (melee && m_bodyFx && m_effects && e->amount > 0 && (e->hitInfo & net::kHitInfoAffectsVictim) &&
            (e->kind == game::CombatEvent::Kind::Damage || e->kind == game::CombatEvent::Kind::Interrupt)) {
            m_bodyFx->bleed(*m_effects, actors, victim->actor, victim->feet, victim->yaw,
                            attacker ? &attacker->feet : nullptr, (e->hitInfo & net::kHitInfoBloodSpurt) != 0);
        }
        const u16 reaction = game::reactionAnimation(*e, victim->gear);
        const bool wound = reaction == animation::kCombatWound || reaction == animation::kCombatCritical;
        if (reaction != game::kNoAnimation && !(wound && later(victim->swingUntilMs, nowMs))) {
            playAction(actors, *victim, reaction);
            ++m_combatStats.reactions;
        }
        victim->combatUntilMs = nowMs + kCombatStanceMs;
    }

    // Auto attack on and off, ranged shots, emotes.
    while (const game::FxEvent* e = play.fx().next(m_fxCursor)) {
        Unit* unit = find(e->guid);
        if (!unit) {
            continue;
        }
        switch (e->kind) {
        case game::FxEventKind::AttackStart:
            unit->attacking = true;
            unit->combatUntilMs = nowMs + kCombatStanceMs;
            break;
        case game::FxEventKind::AttackStop: unit->attacking = false; break;
        case game::FxEventKind::SpellGo: {
            // A shot (Auto Shot, Shoot, Throw): the ranged weapon's attack, unless the spell's cast animates.
            const u32 mask = m_visualData ? m_visualData->rangedWeaponMask(e->id) : 0;
            if (mask == 0) {
                break;
            }
            const game::SpellVisualInfo* visual = m_visualData->spellVisual(e->id);
            const game::VisualKit* cast = visual ? m_visualData->kit(visual->castKit) : nullptr;
            if (cast && cast->animation != game::kNoKitAnimation) {
                break;
            }
            const game::WeaponKind ranged =
                unit->gear.ranged != game::WeaponKind::None ? unit->gear.ranged : rangedOfMask(mask);
            playAction(actors, *unit, game::rangedAttackAnimation(ranged));
            unit->rangedUntilMs = nowMs + kRangedStanceMs;
            unit->combatUntilMs = nowMs + kCombatStanceMs;
            ++m_combatStats.swings;
            break;
        }
        case game::FxEventKind::Emote:
            if (m_visualData && m_visualData->emoteOneShot(e->id) &&
                emoteShown(actors, *unit, m_visualData->emoteAnimation(e->id))) {
                playAction(actors, *unit, m_visualData->emoteAnimation(e->id));
                unit->emoteMs = nowMs;
                ++m_combatStats.emotes;
            }
            break;
        case game::FxEventKind::TextEmote:
            // The server sends the emote's SMSG_EMOTE first; a text emote without one still animates.
            if (m_visualData && !(unit->emoteMs != 0 && !later(nowMs, unit->emoteMs + kEmoteEchoMs))) {
                const u32 emote = m_visualData->textEmote(e->id);
                if (m_visualData->emoteOneShot(emote) && emoteShown(actors, *unit, m_visualData->emoteAnimation(emote))) {
                    playAction(actors, *unit, m_visualData->emoteAnimation(emote));
                    ++m_combatStats.emotes;
                }
            }
            break;
        case game::FxEventKind::MountSpecial: // phase 18 (extras): the mount rears (0x608D50: AnimationData 94)
            if (unit->mount != ~0u) {
                actors.playAction(unit->mount, 94);
            }
            break;
        default: break;
        }
    }
}

} // namespace vd
