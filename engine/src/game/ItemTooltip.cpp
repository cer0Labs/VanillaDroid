#include "game/ItemTooltip.h"

#include "game/Inventory.h"
#include "game/ItemData.h"
#include "game/ItemDefs.h"
#include "game/Queries.h"
#include "game/Spells.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vd::game {
namespace {

constexpr u32 kPlayableClasses = 0x5DF; // warrior..druid (classes 1-5, 7-9, 11)
constexpr u32 kPlayableRaces = 0xFF;    // human..troll

// SpellSchools 1..6 as the client names damage and resistances ("Fire").
constexpr text::Key kSchoolNames[7] = {"",                  "SPELL_SCHOOL1_CAP", "SPELL_SCHOOL2_CAP", "SPELL_SCHOOL3_CAP",
                                       "SPELL_SCHOOL4_CAP", "SPELL_SCHOOL5_CAP", "SPELL_SCHOOL6_CAP"};

std::string_view schoolName(u32 school) {
    return school >= 1 && school < 7 ? text::get(kSchoolNames[school]) : std::string_view{};
}

// The stat line of an ItemModType ("%c%d Agility"), nullptr for the ones the tooltip does not show.
const char* statKey(u32 type) {
    switch (type) {
    case kItemModMana: return "ITEM_MOD_MANA";
    case kItemModHealth: return "ITEM_MOD_HEALTH";
    case kItemModAgility: return "ITEM_MOD_AGILITY";
    case kItemModStrength: return "ITEM_MOD_STRENGTH";
    case kItemModIntellect: return "ITEM_MOD_INTELLECT";
    case kItemModSpirit: return "ITEM_MOD_SPIRIT";
    case kItemModStamina: return "ITEM_MOD_STAMINA";
    default: return nullptr;
    }
}

// "Warrior, Mage" for the playable bits of `mask` (ids from 1), names by `name(id)`; empty when every playable one is
// in it.
template <typename Name> void listNames(u32 mask, u32 playable, Name name, char* out, usize capacity) {
    out[0] = '\0';
    if ((mask & playable) == playable) {
        return;
    }
    usize length = 0;
    for (u32 id = 1; id <= 32; ++id) {
        if ((playable & (1u << (id - 1))) == 0 || (mask & (1u << (id - 1))) == 0) {
            continue;
        }
        const std::string_view text = name(id);
        if (text.empty()) {
            continue;
        }
        const int n = std::snprintf(out + length, capacity - length, "%s%.*s", length ? ", " : "",
                                    static_cast<int>(text.size()), text.data());
        length = n < 0 ? length : std::min(capacity - 1, length + static_cast<usize>(n));
    }
}

// The subclass the tooltip shows on the right of the slot ("Sword", "Plate", "Arrow"): weapons, armor but its
// miscellaneous kind (rings, necks), projectiles.
std::string_view subclassText(const net::ItemQueryInfo& item, const TooltipContext& c) {
    const bool shown = item.itemClass == kItemClassWeapon || item.itemClass == kItemClassProjectile ||
                       (item.itemClass == kItemClassArmor && item.subclass != kArmorMisc);
    return shown && c.data ? c.data->subclassName(item.itemClass, item.subclass) : std::string_view{};
}

// A time left as the client writes it: the largest unit that fits (days, hours, minutes, seconds), rounded up.
// `keys`: the texts for a day, days, an hour, hours, minutes and seconds; `name` goes before the number when given
// (an enchantment's "%s (%d min)").
TooltipLine& addTimeLeft(ItemTooltip& out, u32 color, u32 seconds, const text::Key (&keys)[6], std::string_view name) {
    const auto add = [&](const text::Key& key, u32 value) -> TooltipLine& {
        return name.empty() ? out.addText(color, key, value) : out.addText(color, key, name, value);
    };
    if (seconds >= 86400) {
        const u32 days = (seconds + 86399) / 86400;
        return add(days > 1 ? keys[1] : keys[0], days);
    }
    if (seconds >= 3600) {
        const u32 hours = (seconds + 3599) / 3600;
        return add(hours > 1 ? keys[3] : keys[2], hours);
    }
    if (seconds >= 60) {
        return add(keys[4], (seconds + 59) / 60);
    }
    return add(keys[5], seconds);
}

constexpr text::Key kEnchantTimeLeft[6] = {"ITEM_ENCHANT_TIME_LEFT_DAYS",  "ITEM_ENCHANT_TIME_LEFT_DAYS_P1",
                                           "ITEM_ENCHANT_TIME_LEFT_HOURS", "ITEM_ENCHANT_TIME_LEFT_HOURS_P1",
                                           "ITEM_ENCHANT_TIME_LEFT_MIN",   "ITEM_ENCHANT_TIME_LEFT_SEC"};
constexpr text::Key kDurationLeft[6] = {"ITEM_DURATION_DAYS",  "ITEM_DURATION_DAYS", "ITEM_DURATION_HOURS",
                                        "ITEM_DURATION_HOURS", "ITEM_DURATION_MIN",  "ITEM_DURATION_SEC"};

bool isWeaponLike(const net::ItemQueryInfo& item) {
    return item.itemClass == kItemClassWeapon ||
           (item.damages[0].max > 0.0f && item.itemClass != kItemClassProjectile && item.delayMs > 0);
}

} // namespace

u32 itemQualityColor(u32 quality) {
    constexpr u32 kColors[] = {tooltipColor(157, 157, 157), tooltipColor(255, 255, 255), tooltipColor(30, 255, 0),
                               tooltipColor(0, 112, 221),   tooltipColor(163, 53, 238),  tooltipColor(255, 128, 0),
                               tooltipColor(230, 204, 128)};
    return kColors[std::min<u32>(quality, 6)];
}

std::string_view inventoryTypeName(u32 inventoryType) {
    // INVTYPE_* of GlobalStrings.lua; the 1.12 client has none for ammo, thrown weapons and quivers (ours).
    const auto key = [](u32 type) -> const char* {
        switch (type) {
        case kInvTypeHead: return "INVTYPE_HEAD";
        case kInvTypeNeck: return "INVTYPE_NECK";
        case kInvTypeShoulders: return "INVTYPE_SHOULDER";
        case kInvTypeBody: return "INVTYPE_BODY";
        case kInvTypeChest: return "INVTYPE_CHEST";
        case kInvTypeRobe: return "INVTYPE_ROBE";
        case kInvTypeWaist: return "INVTYPE_WAIST";
        case kInvTypeLegs: return "INVTYPE_LEGS";
        case kInvTypeFeet: return "INVTYPE_FEET";
        case kInvTypeWrists: return "INVTYPE_WRIST";
        case kInvTypeHands: return "INVTYPE_HAND";
        case kInvTypeFinger: return "INVTYPE_FINGER";
        case kInvTypeTrinket: return "INVTYPE_TRINKET";
        case kInvTypeWeapon: return "INVTYPE_WEAPON";
        case kInvTypeShield: return "INVTYPE_SHIELD";
        case kInvTypeRanged:
        case kInvTypeRangedRight: return "INVTYPE_RANGED";
        case kInvTypeCloak: return "INVTYPE_CLOAK";
        case kInvType2HWeapon: return "INVTYPE_2HWEAPON";
        case kInvTypeBag: return "INVTYPE_BAG";
        case kInvTypeTabard: return "INVTYPE_TABARD";
        case kInvTypeWeaponMainHand: return "INVTYPE_WEAPONMAINHAND";
        case kInvTypeWeaponOffHand: return "INVTYPE_WEAPONOFFHAND";
        case kInvTypeHoldable: return "INVTYPE_HOLDABLE";
        case kInvTypeAmmo: return "VD_INV_INVTYPE_AMMO";
        case kInvTypeThrown: return "VD_INV_INVTYPE_THROWN";
        case kInvTypeQuiver: return "VD_INV_INVTYPE_QUIVER";
        case kInvTypeRelic: return "INVTYPE_RELIC";
        default: return nullptr;
        }
    };
    const char* name = key(inventoryType);
    return name ? text::get(name) : std::string_view{};
}

std::string_view reputationRankName(u32 rank) {
    constexpr text::Key kRanks[8] = {"FACTION_STANDING_LABEL1", "FACTION_STANDING_LABEL2", "FACTION_STANDING_LABEL3",
                                     "FACTION_STANDING_LABEL4", "FACTION_STANDING_LABEL5", "FACTION_STANDING_LABEL6",
                                     "FACTION_STANDING_LABEL7", "FACTION_STANDING_LABEL8"};
    return rank < 8 ? text::get(kRanks[rank]) : std::string_view{};
}

// -------------------------------------------------------------------------------------------------------

void ItemTooltip::clear() {
    m_count = 0;
    m_used = 0;
    m_quality = 1;
}

std::string_view ItemTooltip::store(std::string_view text) {
    const u32 length = static_cast<u32>(std::min<usize>(text.size(), m_text.size() - m_used));
    std::memcpy(m_text.data() + m_used, text.data(), length);
    const std::string_view stored{m_text.data() + m_used, length};
    m_used += length;
    return stored;
}

TooltipLine& ItemTooltip::add(std::string_view text, u32 color) {
    if (m_count >= kMaxLines) {
        m_overflow = TooltipLine{};
        return m_overflow;
    }
    TooltipLine& line = m_lines[m_count++];
    line = TooltipLine{};
    line.left = store(text);
    line.leftColor = color;
    return line;
}

void ItemTooltip::right(TooltipLine& line, std::string_view text, u32 color) {
    line.right = store(text);
    line.rightColor = color;
}

// -------------------------------------------------------------------------------------------------------

void buildItemTooltip(const net::ItemQueryInfo& item, const ItemInstance* instance, const TooltipContext& c,
                      ItemTooltip& out) {
    out.clear();
    out.setQuality(item.quality);

    // Name, with the random suffix of "of the Bear" items.
    std::string_view suffix;
    if (c.data && instance && instance->randomPropertyId != 0) {
        suffix = c.data->randomSuffix(instance->randomPropertyId);
    }
    if (suffix.empty()) {
        out.add(item.name, itemQualityColor(item.quality));
    } else {
        out.addText(itemQualityColor(item.quality), "ITEM_SUFFIX_TEMPLATE", item.name, suffix);
    }

    // Binding, uniqueness, special kinds.
    if (instance && instance->soulbound()) {
        out.addText(kTooltipWhite, "ITEM_SOULBOUND");
    } else {
        switch (item.bonding) {
        case kBindOnPickup: out.addText(kTooltipWhite, "ITEM_BIND_ON_PICKUP"); break;
        case kBindOnEquip: out.addText(kTooltipWhite, "ITEM_BIND_ON_EQUIP"); break;
        case kBindOnUse: out.addText(kTooltipWhite, "ITEM_BIND_ON_USE"); break;
        case kBindQuest: out.addText(kTooltipWhite, "ITEM_BIND_QUEST"); break;
        default: break;
        }
    }
    if (item.maxCount == 1) {
        out.addText(kTooltipWhite, "ITEM_UNIQUE");
    } else if (item.maxCount > 1) {
        out.addText(kTooltipWhite, "ITEM_UNIQUE_MULTIPLE", item.maxCount);
    }
    if (item.flags & kItemFlagConjured) {
        out.addText(kTooltipWhite, "ITEM_CONJURED");
    }
    if (item.lockId != 0 && !(instance && (instance->flags & kItemDynFlagUnlocked))) {
        out.addText(kTooltipRed, "LOCKED");
    }
    if (item.startQuest != 0) {
        out.addText(kTooltipWhite, "ITEM_STARTS_QUEST");
    }

    // Slot and type, or the bag's size ("16 Slot Bag", the kind from ItemSubClass.dbc).
    const std::string_view slot = inventoryTypeName(item.inventoryType);
    const std::string_view type = subclassText(item, c);
    const bool usable = !c.inventory || c.inventory->proficient(item.itemClass, item.subclass);
    if (item.containerSlots > 0) {
        std::string_view kind;
        if (c.data && (item.itemClass == kItemClassContainer || item.itemClass == kItemClassQuiver)) {
            kind = c.data->subclassName(item.itemClass, item.subclass);
        }
        out.addText(kTooltipWhite, "CONTAINER_SLOTS", item.containerSlots,
                    kind.empty() ? text::get("INVTYPE_BAG") : kind);
    } else if (!slot.empty() || !type.empty()) {
        TooltipLine& line = out.add(slot.empty() ? type : slot);
        if (!slot.empty() && !type.empty()) {
            out.right(line, type, usable ? kTooltipWhite : kTooltipRed);
        } else if (slot.empty()) {
            line.leftColor = usable ? kTooltipWhite : kTooltipRed;
        }
    }

    // Weapons: damage and speed, extra schools, damage per second. Ammo: damage per second added.
    if (item.itemClass == kItemClassProjectile && item.damages[0].max > 0.0f) {
        const f32 perSecond = (item.damages[0].min + item.damages[0].max) * 0.5f;
        const std::string_view school = schoolName(item.damages[0].school);
        if (school.empty()) {
            out.addText(kTooltipWhite, "AMMO_DAMAGE_TEMPLATE", perSecond);
        } else {
            out.addText(kTooltipWhite, "AMMO_SCHOOL_DAMAGE_TEMPLATE", perSecond, school);
        }
    } else if (isWeaponLike(item) && item.damages[0].max > 0.0f) {
        const net::ItemQueryInfo::Damage& d = item.damages[0];
        const std::string_view school = schoolName(d.school);
        const i32 lo = static_cast<i32>(d.min), hi = static_cast<i32>(d.max);
        TooltipLine& line = school.empty() ? out.addText(kTooltipWhite, "DAMAGE_TEMPLATE", lo, hi)
                                           : out.addText(kTooltipWhite, "DAMAGE_TEMPLATE_WITH_SCHOOL", lo, hi, school);
        f32 perSecond = 0.0f;
        const f32 speed = static_cast<f32>(item.delayMs) / 1000.0f;
        if (speed > 0.0f) {
            char buffer[64];
            out.right(line, text::format(buffer, "VD_INV_WEAPON_SPEED", speed));
        }
        for (u32 i = 0; i < item.damages.size(); ++i) {
            const net::ItemQueryInfo::Damage& extra = item.damages[i];
            if (extra.max <= 0.0f) {
                continue;
            }
            perSecond += (extra.min + extra.max) * 0.5f;
            if (i == 0) {
                continue;
            }
            const std::string_view s = schoolName(extra.school);
            const i32 min = static_cast<i32>(extra.min), max = static_cast<i32>(extra.max);
            if (min == max) {
                out.addText(kTooltipWhite, "PLUS_SINGLE_DAMAGE_TEMPLATE", min);
            } else if (s.empty()) {
                out.addText(kTooltipWhite, "PLUS_DAMAGE_TEMPLATE", min, max);
            } else {
                out.addText(kTooltipWhite, "PLUS_DAMAGE_TEMPLATE_WITH_SCHOOL", min, max, s);
            }
        }
        if (speed > 0.0f) {
            out.addText(kTooltipWhite, "DPS_TEMPLATE", perSecond / speed);
        }
    }

    // Armor, block, stats, resistances (one line for all five schools when they are equal, as the client).
    if (item.armor > 0) {
        out.addText(kTooltipWhite, "ARMOR_TEMPLATE", item.armor);
    }
    if (item.block > 0) {
        out.addText(kTooltipWhite, "SHIELD_BLOCK_TEMPLATE", item.block);
    }
    for (const net::ItemQueryInfo::Stat& stat : item.stats) {
        const char* key = statKey(stat.type);
        if (stat.value != 0 && key) {
            out.addText(kTooltipWhite, key, stat.value > 0 ? '+' : '-', std::abs(stat.value));
        }
    }
    const i32 fire = item.resistances[1];
    bool allEqual = fire != 0;
    for (u32 i = 2; i < item.resistances.size(); ++i) {
        allEqual = allEqual && item.resistances[i] == fire;
    }
    for (u32 i = 0; i < item.resistances.size(); ++i) {
        const i32 value = item.resistances[i];
        if (value == 0 || (allEqual && i > 1)) {
            continue;
        }
        if (allEqual && i == 1) {
            out.addText(kTooltipWhite, "ITEM_RESIST_ALL", value > 0 ? '+' : '-', std::abs(value));
        } else {
            out.addText(kTooltipWhite, "ITEM_RESIST_SINGLE", value > 0 ? '+' : '-', std::abs(value), schoolName(i + 1));
        }
    }

    // Enchantments: the random property's as stats, the permanent and temporary ones in green.
    if (c.data && instance) {
        for (u32 slotIndex = kEnchantProperty0; slotIndex < kEnchantmentSlots; ++slotIndex) {
            const std::string_view text = c.data->enchantmentText(instance->enchantments[slotIndex]);
            if (!text.empty()) {
                out.add(text);
            }
        }
        const std::string_view permanent = c.data->enchantmentText(instance->enchantments[kEnchantPermanent]);
        if (!permanent.empty()) {
            out.add(permanent, kTooltipGreen);
        }
        const std::string_view temporary = c.data->enchantmentText(instance->enchantments[kEnchantTemporary]);
        if (!temporary.empty()) {
            const u32 seconds = instance->enchantmentDurations[kEnchantTemporary] / 1000;
            if (seconds > 0) {
                addTimeLeft(out, kTooltipGreen, seconds, kEnchantTimeLeft, temporary);
            } else {
                out.add(temporary, kTooltipGreen);
            }
        }
    }

    // Durability, duration.
    const u32 maxDurability = instance && instance->maxDurability ? instance->maxDurability : item.maxDurability;
    if (maxDurability > 0) {
        const u32 durability = instance ? instance->durability : maxDurability;
        out.addText(durability == 0 ? kTooltipRed : kTooltipWhite, "DURABILITY_TEMPLATE", durability, maxDurability);
    }
    if (instance && instance->durationS > 0) {
        addTimeLeft(out, kTooltipWhite, instance->durationS, kDurationLeft, {});
    }

    // Who may use it (names from ChrClasses / ChrRaces.dbc), and what it needs.
    char names[160];
    if (c.data) {
        listNames(item.allowableClass, kPlayableClasses, [&](u32 id) { return c.data->className(id); }, names,
                  sizeof(names));
        if (names[0] != '\0') {
            const bool mine = c.cls == 0 || (item.allowableClass & (1u << (c.cls - 1))) != 0;
            out.addText(mine ? kTooltipWhite : kTooltipRed, "ITEM_CLASSES_ALLOWED", std::string_view(names));
        }
        listNames(item.allowableRace, kPlayableRaces, [&](u32 id) { return c.data->raceName(id); }, names,
                  sizeof(names));
        if (names[0] != '\0') {
            const bool mine = c.race == 0 || (item.allowableRace & (1u << (c.race - 1))) != 0;
            out.addText(mine ? kTooltipWhite : kTooltipRed, "ITEM_RACES_ALLOWED", std::string_view(names));
        }
    }
    if (item.requiredLevel > 1) {
        out.addText(c.level != 0 && c.level < item.requiredLevel ? kTooltipRed : kTooltipWhite, "ITEM_MIN_LEVEL",
                    item.requiredLevel);
    }
    if (item.requiredSkill != 0 && c.data) {
        const std::string_view skill = c.data->skillName(item.requiredSkill);
        if (!skill.empty()) {
            out.addText(kTooltipWhite, "ITEM_MIN_SKILL", skill, item.requiredSkillRank);
        }
    }
    if (item.requiredSpell != 0 && c.data) {
        const std::string_view spell = c.data->spellName(item.requiredSpell);
        if (!spell.empty()) {
            const bool known = !c.spells || c.spells->knows(item.requiredSpell);
            out.addText(known ? kTooltipWhite : kTooltipRed, "ITEM_REQ_SKILL", spell);
        }
    }
    if (item.requiredReputationFaction != 0 && c.data) {
        const std::string_view faction = c.data->factionName(item.requiredReputationFaction);
        if (!faction.empty()) {
            out.addText(kTooltipWhite, "ITEM_REQ_REPUTATION", faction, reputationRankName(item.requiredReputationRank));
        }
    }
    if (item.requiredHonorRank != 0) {
        // The rank's title on the player's side: PVP_RANK_<rank>_<0 Horde, 1 Alliance>.
        char key[32];
        const bool horde = c.data && c.data->hordeRace(c.race);
        std::snprintf(key, sizeof(key), "PVP_RANK_%u_%u", item.requiredHonorRank, horde ? 0u : 1u);
        out.addText(kTooltipWhite, "ITEM_REQ_SKILL", text::get(std::string_view(key)));
    }

    // Spells: "Use:", "Equip:", "Chance on hit:" with the spell's text; charges.
    for (u32 i = 0; i < item.spells.size(); ++i) {
        const net::ItemQueryInfo::Spell& spell = item.spells[i];
        if (spell.id == 0 || spell.trigger > net::kItemSpellTriggerChanceOnHit) {
            continue;
        }
        const text::Key trigger = spell.trigger == net::kItemSpellTriggerOnUse     ? "ITEM_SPELL_TRIGGER_ONUSE"
                                  : spell.trigger == net::kItemSpellTriggerOnEquip ? "ITEM_SPELL_TRIGGER_ONEQUIP"
                                                                                   : "ITEM_SPELL_TRIGGER_ONPROC";
        std::string& text = out.scratch();
        bool known = c.data && c.data->spellDescription(spell.id, text);
        if (!known && c.data) {
            text.assign(c.data->spellName(spell.id));
            known = !text.empty();
        }
        if (!known) {
            continue;
        }
        text.insert(0, 1, ' ');
        text.insert(0, text::get(trigger));
        out.add(text, kTooltipGreen).wrap = true;
        const i32 charges = instance ? instance->spellCharges[i] : spell.charges;
        if (charges != 0) {
            const i32 left = std::abs(charges);
            out.addText(kTooltipWhite, left == 1 ? "ITEM_SPELL_CHARGES" : "ITEM_SPELL_CHARGES_P1", left);
        }
    }
    if (item.pageText != 0 || (instance && instance->itemTextId != 0)) {
        out.addText(kTooltipGreen, "VD_INV_TAP_TO_READ");
    } else if (item.flags & kItemFlagLootable) {
        out.addText(kTooltipGreen, "VD_INV_TAP_TO_OPEN");
    }

    // The set: its name with the pieces worn, each piece, each bonus.
    if (item.itemSet != 0 && c.data) {
        if (const std::optional<ItemSetInfo> set = c.data->itemSet(item.itemSet)) {
            u32 worn = 0;
            std::array<bool, 17> wornPiece{};
            for (u32 i = 0; i < set->itemCount; ++i) {
                for (u8 s = 0; c.inventory && s < kEquipmentSlotEnd; ++s) {
                    const ItemInstance* equipped = c.inventory->equipped(s);
                    if (equipped && equipped->entry == set->items[i]) {
                        wornPiece[i] = true;
                    }
                }
                worn += wornPiece[i] ? 1 : 0;
            }
            out.addText(kTooltipGold, "ITEM_SET_NAME", set->name, worn, set->itemCount).gapBefore = true;
            for (u32 i = 0; i < set->itemCount; ++i) {
                // Phase 18 (extras): the piece's name alone, as 1.12 asks for it (CMSG_ITEM_NAME_QUERY, 0x52DF55).
                const std::string* piece = c.queries ? c.queries->itemName(set->items[i]) : nullptr;
                if (piece) {
                    std::string& indented = out.scratch();
                    indented.assign("  ");
                    indented += *piece;
                    out.add(indented, wornPiece[i] ? kTooltipSetPiece : kTooltipGray);
                }
            }
            for (u32 i = 0; i < set->spells.size(); ++i) {
                if (set->spells[i] == 0) {
                    continue;
                }
                std::string& text = out.scratch();
                if (!c.data->spellDescription(set->spells[i], text)) {
                    text.assign(c.data->spellName(set->spells[i]));
                }
                const bool active = worn >= set->thresholds[i];
                TooltipLine& line = active ? out.addText(kTooltipGreen, "ITEM_SET_BONUS", text)
                                           : out.addText(kTooltipGray, "ITEM_SET_BONUS_GRAY", set->thresholds[i], text);
                line.wrap = true;
                line.gapBefore = i == 0;
            }
        }
    }

    // Flavour text, creator, price.
    if (!item.description.empty()) {
        out.addText(kTooltipGold, "VD_INV_ITEM_DESCRIPTION", item.description).wrap = true;
    }
    if (instance && instance->creator != 0 && c.queries) {
        if (const std::string* name = c.queries->playerName(instance->creator)) {
            out.addText(kTooltipGreen, "ITEM_CREATED_BY", *name); // with its own color codes
        }
    }
    if (c.showSellPrice && item.sellPrice > 0) {
        out.addText(kTooltipWhite, "VD_INV_SELL_PRICE").money =
            item.sellPrice * (instance ? std::max<u32>(instance->count, 1) : 1);
    }
}

} // namespace vd::game
