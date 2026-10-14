#pragma once

#include "core/Types.h"
#include "game/GameData.h"
#include "game/Inventory.h"
#include "game/SpellMods.h"
#include "net/Messages.h"

#include <glm/vec3.hpp>

namespace vd::game {

// Phase 13 (spells): spells that wait for a place or an item after the tap that casts them, as the 1.12
// client does it (SpellIsTargeting / SpellTargetItem / SpellStopTargeting in its Lua API, the targeting
// cursor on the desktop).

// What a spell waits for. From Spell.dbc, as the server judges a client's targets
// (Spell::ValidateExplicitTargetMask: the Targets mask the cast must carry; SpellMgr's GetAllowedTargetMask:
// what its effects' implicit targets accept).
enum class SpellAim : u8 {
    None,     // cast at once: a unit, the caster, around it
    Location, // TARGET_FLAG_DEST_LOCATION: a place (Blizzard, Flamestrike, Rain of Fire, Hurricane, Volley,
              // Flare, Distract, Far Sight, Eagle Eye, engineering bombs)
    Item,   // TARGET_FLAG_ITEM: an item (poisons, sharpening stones, oils, enchanting, Disenchant, Feed Pet)
    Locked, // TARGET_FLAG_LOCKED or a TARGET_LOCKED effect: a locked object, or a locked item (Pick Lock)
};
SpellAim spellAim(const SpellInfo& spell);
// The ground circle's yards for a place: the widest radius of its effects (SpellRadius.dbc), 0 when none.
f32 spellAreaRadius(const SpellInfo& spell);
// The spell opens locks of this LockType (OPEN_LOCK effects' misc value: 1 lockpicking), 0 none.
u32 spellLockType(const SpellInfo& spell);

// Whether a place is within the spell's reach, as Spell::CheckRange measures a destination: the 3D distance
// from the caster less its bounding radius, against the maximum range with SPELLMOD_RANGE (`mods` may be
// null) and the strict check's 1.25 yards, and the minimum range.
enum class PlaceRange : u8 { In, OutOfRange, TooClose };
PlaceRange placeRange(const SpellInfo& spell, const SpellMods* mods, const glm::vec3& caster,
                      f32 casterBoundingRadius, const glm::vec3& place);

// How an item suits the waiting spell, for the windows that offer items to pick (lit, or dimmed).
enum class ItemFit : u8 {
    Fits,    // what the spell wants
    Refused, // the client itself refuses it (enchantments: "Invalid target", SPELL_FAILED_BAD_TARGETS)
    Unlikely // dimmed: the server will most likely refuse it (it judges with data the client lacks), sent
             // anyway
};
// Item::IsFitToSpellRequirements (EquippedItemClass, the subclass and inventory type masks), checked by
// WoW.exe itself for enchantments; an enchantment's level (Spell::CheckItems: SPELL_FAILED_LOWLEVEL); a
// locked item for OPEN_LOCK; armor and weapons of uncommon or better quality for Disenchant (the server needs
// their DisenchantID); food and trade goods for Feed Pet (the server checks the food type, which the 1.12
// item query does not send).
ItemFit itemFitsSpell(const SpellInfo& spell, const net::ItemQueryInfo& item, const ItemInstance* instance);

// The question 1.12 asks before an enchantment lands (WoW.exe's item target check, read from its disassembly;
// the popups are UIParent.lua's BIND_ENCHANT, REPLACE_ENCHANT, TRADE_REPLACE_ENCHANT):
// - Bind: an ENCHANT_ITEM / _TEMPORARY effect whose SpellItemEnchantment has ENCHANTMENT_CAN_SOULBOUND, on an
//   equippable item that is not soulbound yet (asked first; BindEnchant asks the next question);
// - Replace: the item already has an enchantment in the slot the effect writes (permanent / temporary),
//   named old and new ("Do you want to replace %s with %s?");
// - TradeReplace: a permanent enchantment on the trader's item that already has one.
enum class EnchantAsk : u8 { None, Bind, Replace, TradeReplace };
struct EnchantQuestion {
    EnchantAsk ask = EnchantAsk::None;
    u32 oldEnchant = 0; // SpellItemEnchantment ids (Replace / TradeReplace)
    u32 newEnchant = 0;
};
// `bindConfirmed`: BIND_ENCHANT was answered (only the replacement is left to ask).
EnchantQuestion enchantQuestion(const SpellInfo& spell, const ItemInstance& item,
                                const net::ItemQueryInfo& info, bool bindConfirmed, const GameData* data);
EnchantQuestion tradeEnchantQuestion(const SpellInfo& spell, u32 tradeItemEnchant, const GameData* data);

// The cast waiting for its target (Gameplay::targeting()): its spell, what it waits for, and when it comes
// from using an item (a poison, a bomb) the item it is cast with.
class SpellTargeting {
public:
    bool active() const { return m_spell.id != 0; }
    u32 spellId() const { return m_spell.id; }
    const SpellInfo& spell() const { return m_spell; }
    SpellAim aim() const { return m_aim; }
    // Cast by using an item: the item's place, GUID, entry and on-use spell index (CMSG_USE_ITEM).
    bool fromItem() const { return m_itemGuid != 0; }
    ItemPos itemPos() const { return m_itemPos; }
    u64 itemGuid() const { return m_itemGuid; }
    u32 itemEntry() const { return m_itemEntry; }
    u8 itemSpellIndex() const { return m_itemSpellIndex; }
    // An enchanting question waits for its answer (Gameplay::confirmEnchant), about this item (0: the trade
    // slot).
    const EnchantQuestion& question() const { return m_question; }
    u64 questionItem() const { return m_questionItem; }
    bool bindConfirmed() const { return m_bindConfirmed; }
    // Bumped whenever it starts, stops or asks (the windows follow it).
    u32 version() const { return m_version; }

    void begin(const SpellInfo& spell, SpellAim aim);
    void beginFromItem(const SpellInfo& spell, SpellAim aim, ItemPos pos, u64 guid, u32 entry, u8 spellIndex);
    void ask(const EnchantQuestion& question, u64 item, bool bindConfirmed);
    void clearQuestion();
    void stop();

private:
    SpellInfo m_spell;
    SpellAim m_aim = SpellAim::None;
    ItemPos m_itemPos;
    u64 m_itemGuid = 0;
    u32 m_itemEntry = 0;
    u8 m_itemSpellIndex = 0;
    EnchantQuestion m_question;
    u64 m_questionItem = 0;
    bool m_bindConfirmed = false;
    u32 m_version = 1;
};

} // namespace vd::game
