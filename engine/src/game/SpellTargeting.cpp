#include "game/SpellTargeting.h"

#include "game/ItemDefs.h"

#include <algorithm>

#include <glm/geometric.hpp>

namespace vd::game {
namespace {

// SpellCastTargetFlags and SpellTarget (vMaNGOS Spells/SpellDefines.h), SpellEffects (SharedDefines).
constexpr u32 kTargetFlagItem = 0x10, kTargetFlagDestLocation = 0x40, kTargetFlagLocked = 0x4000;
constexpr u32 kTargetLocked = 26;
constexpr u32 kEffectOpenLock = 33, kEffectEnchantItem = 53, kEffectEnchantItemTemporary = 54,
              kEffectOpenLockItem = 59, kEffectDisenchant = 99, kEffectFeedPet = 101;
constexpr u32 kEnchantmentCanSoulbound = 0x01; // ENCHANTMENT_CAN_SOULBOUND (SpellItemEnchantment m_flags)
constexpr u32 kPermanentSlot = 0, kTemporarySlot = 1; // PERM_ENCHANTMENT_SLOT, TEMP_ENCHANTMENT_SLOT
constexpr u32 kInvTypeCloak = 16;
constexpr u32 kEnchantCloakMinorAgility =
    13419; // Item::IsFitToSpellRequirements' exception (its class is a weapon's)
constexpr u32 kQualityUncommon = 2, kQualityEpic = 4;

bool isEnchant(u32 effect) {
    return effect == kEffectEnchantItem || effect == kEffectEnchantItemTemporary;
}

// Item::IsFitToSpellRequirements.
bool fitsRequirements(const SpellInfo& spell, const net::ItemQueryInfo& item) {
    if (spell.equippedItemClass != -1) {
        const bool cloakException =
            spell.id == kEnchantCloakMinorAgility && item.inventoryType == kInvTypeCloak;
        if (!cloakException) {
            if (spell.equippedItemClass != static_cast<i32>(item.itemClass)) {
                return false;
            }
            if (spell.equippedItemSubClassMask != 0 && item.subclass < 32 &&
                (spell.equippedItemSubClassMask & (1u << item.subclass)) == 0) {
                return false;
            }
        }
    }
    if (spell.equippedItemInventoryTypeMask != 0 && (spell.targets & kTargetFlagItem) &&
        item.inventoryType < 32 && (spell.equippedItemInventoryTypeMask & (1u << item.inventoryType)) == 0) {
        return false;
    }
    return true;
}

} // namespace

SpellAim spellAim(const SpellInfo& spell) {
    if (spell.targets & kTargetFlagDestLocation) {
        return SpellAim::Location;
    }
    if (spell.targets & kTargetFlagItem) {
        return SpellAim::Item;
    }
    if (spell.targets & kTargetFlagLocked) {
        return SpellAim::Locked;
    }
    for (u32 i = 0; i < 3; ++i) {
        if (spell.effects[i] != 0 &&
            (spell.implicitTargetA[i] == kTargetLocked || spell.implicitTargetB[i] == kTargetLocked)) {
            return SpellAim::Locked;
        }
    }
    return SpellAim::None;
}

f32 spellAreaRadius(const SpellInfo& spell) {
    f32 radius = 0.0f;
    for (u32 i = 0; i < 3; ++i) {
        if (spell.effects[i] != 0) {
            radius = std::max(radius, spell.effectRadius[i]);
        }
    }
    return radius;
}

u32 spellLockType(const SpellInfo& spell) {
    for (u32 i = 0; i < 3; ++i) {
        if (spell.effects[i] == kEffectOpenLock || spell.effects[i] == kEffectOpenLockItem) {
            return static_cast<u32>(std::max(0, spell.effectMisc[i]));
        }
    }
    return 0;
}

PlaceRange placeRange(const SpellInfo& spell, const SpellMods* mods, const glm::vec3& caster,
                      f32 casterBoundingRadius, const glm::vec3& place) {
    constexpr f32 kRangeGive = 1.25f;              // Spell::CheckRange: a player's strict check
    constexpr f32 kDefaultBoundingRadius = 0.389f; // DEFAULT_WORLD_OBJECT_SIZE
    if (spell.maxRange <= 0.0f) {
        return PlaceRange::In;
    }
    const f32 bounds = casterBoundingRadius > 0.0f ? casterBoundingRadius : kDefaultBoundingRadius;
    const f32 distance = glm::length(place - caster) - bounds;
    const f32 maxRange =
        (mods ? mods->apply(modTarget(spell), SpellModOp::Range, spell.maxRange) : spell.maxRange);
    if (distance > maxRange + kRangeGive) {
        return PlaceRange::OutOfRange;
    }
    return spell.minRange > 0.0f && distance < spell.minRange ? PlaceRange::TooClose : PlaceRange::In;
}

ItemFit itemFitsSpell(const SpellInfo& spell, const net::ItemQueryInfo& item, const ItemInstance* instance) {
    bool enchant = false;
    for (const u32 effect : spell.effects) {
        enchant = enchant || isEnchant(effect);
    }
    if (!fitsRequirements(spell, item)) {
        return enchant ? ItemFit::Refused : ItemFit::Unlikely;
    }
    for (const u32 effect : spell.effects) {
        switch (effect) {
        case kEffectEnchantItem:
            if (item.itemLevel < spell.baseLevel) {
                return ItemFit::Unlikely; // SPELL_FAILED_LOWLEVEL
            }
            break;
        case kEffectOpenLock:
        case kEffectOpenLockItem:
            if (item.lockId == 0 || (instance && (instance->flags & kItemDynFlagUnlocked))) {
                return ItemFit::Unlikely;
            }
            break;
        case kEffectDisenchant:
            if ((item.itemClass != kItemClassWeapon && item.itemClass != kItemClassArmor) ||
                item.quality < kQualityUncommon || item.quality > kQualityEpic) {
                return ItemFit::Unlikely; // SPELL_FAILED_CANT_BE_DISENCHANTED
            }
            break;
        case kEffectFeedPet:
            if (item.itemClass != kItemClassConsumable && item.itemClass != kItemClassTradeGoods) {
                return ItemFit::Unlikely; // SPELL_FAILED_WRONG_PET_FOOD
            }
            break;
        default: break;
        }
    }
    return ItemFit::Fits;
}

EnchantQuestion enchantQuestion(const SpellInfo& spell, const ItemInstance& item,
                                const net::ItemQueryInfo& info, bool bindConfirmed, const GameData* data) {
    EnchantQuestion q;
    if (!data) {
        return q;
    }
    for (u32 i = 0; i < 3; ++i) {
        if (!isEnchant(spell.effects[i])) {
            continue;
        }
        const u32 enchantId = static_cast<u32>(std::max(0, spell.effectMisc[i]));
        const std::optional<EnchantmentInfo> enchant = data->enchantment(enchantId);
        if (enchant && (enchant->flags & kEnchantmentCanSoulbound) && !item.soulbound() && !bindConfirmed &&
            info.inventoryType != 0) {
            q.ask = EnchantAsk::Bind;
            return q;
        }
        const u32 slot = spell.effects[i] == kEffectEnchantItem ? kPermanentSlot : kTemporarySlot;
        const u32 old = slot < item.enchantments.size() ? item.enchantments[slot] : 0;
        if (old != 0 && enchant && data->enchantment(old)) {
            q.ask = EnchantAsk::Replace;
            q.oldEnchant = old;
            q.newEnchant = enchantId;
            return q;
        }
    }
    return q;
}

EnchantQuestion tradeEnchantQuestion(const SpellInfo& spell, u32 tradeItemEnchant, const GameData* data) {
    EnchantQuestion q;
    if (!data || tradeItemEnchant == 0 || !data->enchantment(tradeItemEnchant)) {
        return q;
    }
    for (u32 i = 0; i < 3; ++i) {
        const u32 enchantId = static_cast<u32>(std::max(0, spell.effectMisc[i]));
        if (spell.effects[i] == kEffectEnchantItem && data->enchantment(enchantId)) {
            q.ask = EnchantAsk::TradeReplace;
            q.oldEnchant = tradeItemEnchant;
            q.newEnchant = enchantId;
            return q;
        }
    }
    return q;
}

void SpellTargeting::begin(const SpellInfo& spell, SpellAim aim) {
    stop();
    m_spell = spell;
    m_aim = aim;
}

void SpellTargeting::beginFromItem(const SpellInfo& spell, SpellAim aim, ItemPos pos, u64 guid, u32 entry,
                                   u8 spellIndex) {
    begin(spell, aim);
    m_itemPos = pos;
    m_itemGuid = guid;
    m_itemEntry = entry;
    m_itemSpellIndex = spellIndex;
}

void SpellTargeting::ask(const EnchantQuestion& question, u64 item, bool bindConfirmed) {
    m_question = question;
    m_questionItem = item;
    m_bindConfirmed = bindConfirmed;
    ++m_version;
}

void SpellTargeting::clearQuestion() {
    if (m_question.ask != EnchantAsk::None) {
        m_question = EnchantQuestion{};
        m_questionItem = 0;
        ++m_version;
    }
}

void SpellTargeting::stop() {
    m_spell = SpellInfo{};
    m_aim = SpellAim::None;
    m_itemPos = ItemPos{};
    m_itemGuid = 0;
    m_itemEntry = 0;
    m_itemSpellIndex = 0;
    m_question = EnchantQuestion{};
    m_questionItem = 0;
    m_bindConfirmed = false;
    ++m_version;
}

} // namespace vd::game
