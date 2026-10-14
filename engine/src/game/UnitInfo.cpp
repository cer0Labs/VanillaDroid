#include "game/UnitInfo.h"

#include <glm/geometric.hpp>

namespace vd::game {

namespace uf = net::uf;

u64 guidValue(const Entity& e, u16 index) {
    return static_cast<u64>(e.value(index)) | (static_cast<u64>(e.value(static_cast<u16>(index + 1))) << 32);
}

bool isUnit(const Entity& e) {
    return e.type == ObjectType::Unit || e.type == ObjectType::Player;
}

f32 distanceAt(const Entity& a, const Entity& b, u32 nowMs) {
    return glm::length(a.positionAt(nowMs) - b.positionAt(nowMs));
}

bool isDead(const Entity& unit) {
    return isUnit(unit) && unit.value(uf::UNIT_FIELD_HEALTH) == 0;
}

bool inCombat(const Entity& unit) {
    return isUnit(unit) && (unit.value(uf::UNIT_FIELD_FLAGS) & kUnitFlagInCombat) != 0;
}

bool isLootable(const Entity& e) {
    if (e.type == ObjectType::Corpse) {
        return (e.value(uf::CORPSE_FIELD_DYNAMIC_FLAGS) & kCorpseDynFlagLootable) != 0;
    }
    return isUnit(e) && (e.value(uf::UNIT_DYNAMIC_FLAGS) & kDynFlagLootable) != 0;
}

bool isSkinnable(const Entity& unit) {
    return isUnit(unit) && (unit.value(uf::UNIT_FIELD_FLAGS) & kUnitFlagSkinnable) != 0;
}

bool isTappedByOther(const Entity& unit) {
    const u32 flags = isUnit(unit) ? unit.value(uf::UNIT_DYNAMIC_FLAGS) : 0;
    return (flags & kDynFlagTapped) != 0 && (flags & kDynFlagTappedByPlayer) == 0;
}

bool isTappedByMe(const Entity& unit) {
    return isUnit(unit) && (unit.value(uf::UNIT_DYNAMIC_FLAGS) & kDynFlagTappedByPlayer) != 0;
}

u32 npcFlags(const Entity& unit) {
    return unit.type == ObjectType::Unit ? unit.value(uf::UNIT_NPC_FLAGS) : 0;
}

bool isGhost(const Entity& player) {
    return player.type == ObjectType::Player && (player.value(uf::PLAYER_FLAGS) & kPlayerFlagGhost) != 0;
}

bool isPlayerControlled(const Entity& unit) {
    const bool controlled = (unit.value(uf::UNIT_FIELD_FLAGS) & kUnitFlagPlayerControlled) != 0;
    return unit.type == ObjectType::Player || (unit.type == ObjectType::Unit && controlled);
}

bool isPvpFlagged(const Entity& unit) {
    return isUnit(unit) && (unit.value(uf::UNIT_FIELD_FLAGS) & kUnitFlagPvp) != 0;
}

bool isFreeForAllPvp(const Entity& unit) {
    return unit.type == ObjectType::Player && (unit.value(uf::PLAYER_FLAGS) & kPlayerFlagFreeForAllPvp) != 0;
}

bool isResting(const Entity& player) {
    return player.type == ObjectType::Player && (player.value(uf::PLAYER_FLAGS) & kPlayerFlagResting) != 0;
}

u32 gameObjectType(const Entity& go) {
    return go.type == ObjectType::GameObject ? go.value(uf::GAMEOBJECT_TYPE_ID) : 0xFFFFFFFFu;
}

bool isQuestGiver(const Entity& e) {
    if (e.type == ObjectType::Unit) {
        return (e.value(uf::UNIT_NPC_FLAGS) & kNpcFlagQuestGiver) != 0;
    }
    return gameObjectType(e) == kGameObjectQuestGiver;
}

Reaction reactionTo(const Entity& me, const Entity& target, const Reactions* reactions) {
    if (!reactions || !reactions->ready()) {
        return Reaction::Neutral;
    }
    const u16 theirs =
        target.type == ObjectType::GameObject ? uf::GAMEOBJECT_FACTION : uf::UNIT_FIELD_FACTIONTEMPLATE;
    const u32 mine = me.value(uf::UNIT_FIELD_FACTIONTEMPLATE);
    // Phase 13: the player's standing (forced reactions, at war with a reputation faction) between it and others.
    if (const PlayerStanding* standing = reactions->playerStanding(); standing && standing->player != 0) {
        if (me.guid == standing->player) {
            return reactions->playerTowards(mine, target.value(theirs));
        }
        if (target.guid == standing->player) {
            return reactions->towardsPlayer(mine, target.value(theirs));
        }
    }
    return reactions->between(mine, target.value(theirs));
}

NameColor unitNameColor(const Entity& me, const Entity& unit, const Reactions* reactions) {
    if (isPlayerControlled(unit)) { // TargetFrame_CheckFaction: by who may fight whom
        const bool theyMay = mayFight(unit, me, reactions);
        const bool iMay = mayFight(me, unit, reactions);
        if (theyMay) {
            return iMay ? NameColor::Hostile : NameColor::FriendlyPlayer;
        }
        if (iMay) {
            return NameColor::Neutral; // attackable, not hostile: yellow
        }
        return isPvpFlagged(unit) ? NameColor::Friendly : NameColor::FriendlyPlayer;
    }
    if (isTappedByOther(unit)) {
        return NameColor::Tapped;
    }
    switch (reactionTo(me, unit, reactions)) {
    case Reaction::Hostile: return NameColor::Hostile;
    case Reaction::Friendly: return NameColor::Friendly;
    default: return NameColor::Neutral;
    }
}

bool inDuelWith(const Entity& me, const Entity& other) {
    if (me.type != ObjectType::Player || other.type != ObjectType::Player || me.guid == other.guid) {
        return false;
    }
    // Both under the same flag, on opposite sides: the server sets the teams (1 and 2) when the countdown ends
    // and clears both fields when the duel is over (Player::DuelComplete).
    const u64 arbiter = guidValue(me, uf::PLAYER_DUEL_ARBITER);
    const u32 team = me.value(uf::PLAYER_DUEL_TEAM);
    const u32 otherTeam = other.value(uf::PLAYER_DUEL_TEAM);
    return arbiter != 0 && guidValue(other, uf::PLAYER_DUEL_ARBITER) == arbiter && team != 0 && otherTeam != 0 &&
           team != otherTeam;
}

bool mayFight(const Entity& attacker, const Entity& target, const Reactions* reactions) {
    if (!isUnit(attacker) || !isUnit(target) || attacker.guid == target.guid) {
        return false;
    }
    if (inDuelWith(attacker, target)) {
        return true; // a friendly player too, while the duel lasts
    }
    const bool players = isPlayerControlled(attacker) && isPlayerControlled(target);
    if (players && isFreeForAllPvp(attacker) && isFreeForAllPvp(target)) {
        return true;
    }
    const Reaction reaction = reactionTo(attacker, target, reactions);
    if (reaction == Reaction::Friendly) {
        return false;
    }
    if (players) {
        return isPvpFlagged(target); // what a player controls carries its flag (Unit::SetPvP)
    }
    if (isPlayerControlled(target)) {
        return reaction == Reaction::Hostile; // a creature against a player
    }
    return true; // hostile, or neutral (a swing makes it fight back)
}

bool canAttack(const Entity& me, const Entity& target, const Reactions* reactions) {
    if (!isUnit(target) || target.guid == me.guid || isDead(target)) {
        return false;
    }
    constexpr u32 kUntargetable = kUnitFlagSpawning | kUnitFlagNotAttackable1 | kUnitFlagImmuneToPlayer |
                                  kUnitFlagNonAttackable2 | kUnitFlagTaxiFlight | kUnitFlagNotSelectable;
    if (target.value(uf::UNIT_FIELD_FLAGS) & kUntargetable) {
        return false;
    }
    return mayFight(me, target, reactions);
}

u32 inventoryCount(const EntityStore& store, const Entity& player, u32 entry) {
    u32 count = 0;
    forEachInventoryItem(store, player, [&](const InventoryItem& item) {
        if (item.entry == entry) {
            count += std::max<u32>(item.count, 1);
        }
    });
    return count;
}

bool findInventoryItem(const EntityStore& store, const Entity& player, u32 entry, InventoryItem& out) {
    bool found = false;
    forEachInventoryItem(store, player, [&](const InventoryItem& item) {
        if (!found && item.entry == entry) {
            out = item;
            found = true;
        }
    });
    return found;
}

} // namespace vd::game
