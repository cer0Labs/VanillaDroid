#pragma once

#include "core/Types.h"
#include "net/ByteBuffer.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

// Gameplay packets (phase 7): combat, spells, auras, gossip, quests, loot, chat, experience and death, as
// vmangos-core 4641790 reads and writes them for build 5875 (docs: the wire spec of phase 7, from the server
// code).
//
// Parsers fill a caller-owned struct and return false on a malformed packet, never reading past its end.
// Reuse the struct between packets: its vectors keep their capacity, so steady-state parsing allocates
// nothing. Strings are views into the payload, valid only as long as it is (until the next
// NetClient::poll()). Builders append the body of a client packet to a byte vector (the opcode travels
// separately).
namespace vd::net {

// -------------------------------------------------------------------------------------------------------
// Protocol values (the server's enums, build 5875).

// SpellCastTargets flags (Spells/SpellDefines.h TargetFlags).
constexpr u16 kTargetSelf = 0x0000; // no payload: the caster
constexpr u16 kTargetUnit = 0x0002;
constexpr u16 kTargetItem = 0x0010;
constexpr u16 kTargetSourceLocation = 0x0020;
constexpr u16 kTargetDestLocation = 0x0040;
constexpr u16 kTargetUnitEnemy = 0x0080; // no payload of its own
constexpr u16 kTargetUnitAlly = 0x0100;  // no payload of its own
constexpr u16 kTargetCorpseEnemy = 0x0200;
constexpr u16 kTargetUnitDead = 0x0400; // no payload of its own
constexpr u16 kTargetGameObject = 0x0800;
constexpr u16 kTargetTradeItem = 0x1000;
constexpr u16 kTargetString = 0x2000;
constexpr u16 kTargetCorpseAlly = 0x8000;

// HitInfo of SMSG_ATTACKERSTATEUPDATE (Objects/DamageStructs.h, builds > 1.9.4).
constexpr u32 kHitInfoAffectsVictim = 0x00000002;
constexpr u32 kHitInfoLeftSwing = 0x00000004; // off hand
constexpr u32 kHitInfoMiss = 0x00000010;
constexpr u32 kHitInfoAbsorb = 0x00000020;
constexpr u32 kHitInfoResist = 0x00000040;
constexpr u32 kHitInfoCritical = 0x00000080;
constexpr u32 kHitInfoBlock = 0x00000800; // partial block
constexpr u32 kHitInfoBloodSpurt = 0x00002000; // phase 14 (bodyfx): a large blood spurt (a blow of 20 % of its health)
constexpr u32 kHitInfoGlancing = 0x00004000;
constexpr u32 kHitInfoCrushing = 0x00008000;
constexpr u32 kHitInfoNoAction = 0x00010000; // "on next swing" melee spell

// VictimState (Objects/DamageStructs.h).
enum class VictimState : u32 {
    Unaffected = 0,
    Normal = 1,
    Dodge = 2,
    Parry = 3,
    Interrupt = 4,
    Blocks = 5,
    Evades = 6,
    Immune = 7,
    Deflects = 8
};

// SpellMissInfo (Spells/SpellDefines.h).
enum class SpellMiss : u8 {
    None = 0,
    Miss = 1,
    Resist = 2,
    Dodge = 3,
    Parry = 4,
    Block = 5,
    Evade = 6,
    Immune = 7,
    Immune2 = 8,
    Deflect = 9,
    Absorb = 10,
    Reflect = 11
};

// SpellHitType of SMSG_SPELLNONMELEEDAMAGELOG.
constexpr u32 kSpellHitCrit = 0x02;

// Aura types that SMSG_PERIODICAURALOG reports (Spells/SpellAuraDefines.h).
constexpr u32 kAuraPeriodicDamage = 3;
constexpr u32 kAuraPeriodicHeal = 8;
constexpr u32 kAuraObsModHealth = 20;
constexpr u32 kAuraObsModMana = 21;
constexpr u32 kAuraPeriodicEnergize = 24;
constexpr u32 kAuraPeriodicManaLeech = 64;
constexpr u32 kAuraPeriodicDamagePercent = 89;

// SMSG_CAST_RESULT status (SpellCastResultStatus) and the reasons worth naming (SpellCastResult, see
// spellCastResultName() for all of them).
constexpr u8 kCastStatusOk = 0;
constexpr u8 kCastStatusFailed = 2;
constexpr u8 kCastFailedBadTargets = 10;
constexpr u8 kCastFailedInterrupted = 35;
constexpr u8 kCastFailedNotReady = 60;
constexpr u8 kCastFailedNoPower = 77;
constexpr u8 kCastFailedOutOfRange = 89;
constexpr u8 kCastFailedSpellInProgress = 97;
constexpr u8 kCastFailedUnitNotInfront = 124;
// Enum name of a SpellCastResult ("OUT_OF_RANGE"), "UNKNOWN" beyond the table.
const char* spellCastResultName(u8 reason);

// Action button types (Objects/Player.h ActionButtonType): the high byte of a packed button.
constexpr u8 kActionSpell = 0x00;
constexpr u8 kActionMacro = 0x40;
constexpr u8 kActionClientMacro = 0x41;
constexpr u8 kActionItem = 0x80;
constexpr u32 kActionButtonCount = 120; // MAX_ACTION_BUTTONS

// ChatMsg (SharedDefines.h). The underlying u8 holds any value the server sends.
enum class ChatType : u8 {
    Say = 0x00,
    Party = 0x01,
    Raid = 0x02,
    Guild = 0x03,
    Officer = 0x04,
    Yell = 0x05,
    Whisper = 0x06,
    WhisperInform = 0x07,
    Emote = 0x08,
    TextEmote = 0x09,
    System = 0x0A,
    MonsterSay = 0x0B,
    MonsterYell = 0x0C,
    MonsterEmote = 0x0D,
    Channel = 0x0E,
    ChannelJoin = 0x0F,
    ChannelLeave = 0x10,
    ChannelList = 0x11,
    ChannelNotice = 0x12,
    ChannelNoticeUser = 0x13,
    Afk = 0x14,
    Dnd = 0x15,
    Ignored = 0x16,
    Skill = 0x17,
    Loot = 0x18,
    MonsterWhisper = 0x1A,
    BgSystemNeutral = 0x52,  // phase 17 (queue): a battleground's announcements (SendMessageToAll)
    BgSystemAlliance = 0x53, // ... about the Alliance (its flag, its bases)
    BgSystemHorde = 0x54,    // ... about the Horde
    Money = 0x56,
    RaidLeader = 0x57,
    RaidWarning = 0x58,
    RaidBossWhisper = 0x59,
    RaidBossEmote = 0x5A,
    Battleground = 0x5C,
    BattlegroundLeader = 0x5D,
};
constexpr u8 kChatTypeCount = 0x5E; // MAX_CHAT_MSG_TYPE: the server drops anything at or above

// Language (SharedDefines.h).
enum class Language : u32 {
    Universal = 0,
    Orcish = 1,
    Darnassian = 2,
    Taurahe = 3,
    Dwarvish = 6,
    Common = 7,
    Demonic = 8,
    Titan = 9,
    Thalassian = 10,
    Draconic = 11,
    Kalimag = 12,
    Gnomish = 13,
    Troll = 14,
    Gutterspeak = 33,
    Addon = 0xFFFFFFFFu
};

// DialogStatus (QuestDef.h): quest giver marks and the icons of quest menu entries.
enum class QuestGiverStatus : u8 {
    None = 0,
    Unavailable = 1,
    Chat = 2,
    Incomplete = 3,
    RewardRep = 4,
    Available = 5,
    RewardOld = 6,
    Reward = 7
};

// Quest flags (QuestDef.h).
constexpr u32 kQuestFlagHiddenRewards = 0x200;

// LootType as sent (LootMgr.h): 0 is the error form.
constexpr u8 kLootTypeCorpse = 1;
constexpr u8 kLootTypePickpocketing = 2;
constexpr u8 kLootTypeFishing = 3;
constexpr u8 kLootTypeDisenchanting = 4;
// LootSlotType.
constexpr u8 kLootSlotAllowLoot = 0;

// Enum name of an InventoryResult ("INVENTORY_FULL") / LootError ("TOO_FAR"), "UNKNOWN" beyond the table.
const char* inventoryResultName(u8 result);
const char* lootErrorName(u8 error);

// -------------------------------------------------------------------------------------------------------
// Shared structures.

// SpellCastTargets (Spells/SpellCastTargetsInfo.cpp). The server writes one unit/game object/corpse GUID (in
// that priority) and the item GUID; the client sends a GUID for every flag it sets. Mask 0 = self, nothing
// follows.
struct SpellCastTargets {
    u16 mask = kTargetSelf;
    u64 unit = 0;                     // kTargetUnit
    u64 gameObject = 0;               // kTargetGameObject
    u64 corpse = 0;                   // kTargetCorpseEnemy / kTargetCorpseAlly
    u64 item = 0;                     // kTargetItem / kTargetTradeItem
    std::array<f32, 3> source{};      // kTargetSourceLocation
    std::array<f32, 3> destination{}; // kTargetDestLocation
    std::string_view text;            // kTargetString
};
// As the server writes it (SMSG_SPELL_START / SMSG_SPELL_GO).
void readSpellCastTargets(ByteReader& r, SpellCastTargets& out);
// As the server reads it (CMSG_CAST_SPELL / CMSG_USE_ITEM).
void writeSpellCastTargets(ByteWriter& w, const SpellCastTargets& targets);

// -------------------------------------------------------------------------------------------------------
// Melee and combat log (server -> client).

struct AttackStart { // SMSG_ATTACKSTART
    u64 attacker = 0, victim = 0;
};
bool parseAttackStart(std::span<const u8> payload, AttackStart& out);

struct AttackStop { // SMSG_ATTACKSTOP: `dead` = the attacker's own death, or the victim's (SendAttackStop)
    u64 attacker = 0, victim = 0;
    bool dead = false;
};
bool parseAttackStop(std::span<const u8> payload, AttackStop& out);

struct AttackerStateUpdate { // SMSG_ATTACKERSTATEUPDATE
    struct SubDamage {
        u32 school = 0; // index 0..6, not a mask
        i32 damage = 0;
        i32 absorbed = 0;
        i32 resisted = 0;
    };
    static constexpr u32 kMaxSubDamages = 5;
    u32 hitInfo = 0;
    u64 attacker = 0, victim = 0;
    i32 totalDamage = 0;
    u32 subDamageCount = 0; // stored: at most kMaxSubDamages
    std::array<SubDamage, kMaxSubDamages> subDamages{};
    VictimState victimState = VictimState::Normal;
    u32 attackerState = 0;
    u32 meleeSpellId = 0; // 0 = auto attack
    i32 blocked = 0;
};
bool parseAttackerStateUpdate(std::span<const u8> payload, AttackerStateUpdate& out);

struct SpellDamageLog { // SMSG_SPELLNONMELEEDAMAGELOG
    u64 target = 0, attacker = 0;
    u32 spellId = 0;
    u32 damage = 0; // after absorb, resist and block
    u8 school = 0;
    u32 absorbed = 0;
    i32 resisted = 0;
    bool periodic = false;
    u32 blocked = 0;
    u32 hitType = 0; // kSpellHitCrit ...
};
bool parseSpellDamageLog(std::span<const u8> payload, SpellDamageLog& out);

struct SpellHealLog { // SMSG_SPELLHEALLOG
    u64 target = 0, healer = 0;
    u32 spellId = 0;
    u32 amount = 0;
    bool critical = false;
};
bool parseSpellHealLog(std::span<const u8> payload, SpellHealLog& out);

struct SpellEnergizeLog { // SMSG_SPELLENERGIZELOG
    u64 target = 0, caster = 0;
    u32 spellId = 0;
    u32 powerType = 0;
    u32 amount = 0;
};
bool parseSpellEnergizeLog(std::span<const u8> payload, SpellEnergizeLog& out);

struct PeriodicAuraLog { // SMSG_PERIODICAURALOG (one entry: the server always sends count 1)
    u64 target = 0, caster = 0;
    u32 spellId = 0;
    u32 auraType = 0; // kAuraPeriodic*
    u32 amount = 0;
    u32 school = 0;        // damage auras
    u32 absorbed = 0;      // damage auras
    i32 resisted = 0;      // damage auras
    u32 powerType = 0;     // energize / leech auras
    f32 multiplier = 0.0f; // mana leech
};
bool parsePeriodicAuraLog(std::span<const u8> payload, PeriodicAuraLog& out);

struct SpellLogMiss { // SMSG_SPELLLOGMISS
    struct Entry {
        u64 target = 0;
        SpellMiss miss = SpellMiss::None;
    };
    static constexpr u32 kMaxEntries = 32;
    u32 spellId = 0;
    u64 caster = 0;
    u32 count = 0; // stored: at most kMaxEntries (all are validated)
    std::array<Entry, kMaxEntries> entries{};
};
bool parseSpellLogMiss(std::span<const u8> payload, SpellLogMiss& out);

struct SpellImmuneLog { // SMSG_SPELLORDAMAGE_IMMUNE / SMSG_PROCRESIST
    u64 caster = 0, target = 0;
    u32 spellId = 0;
};
bool parseSpellImmuneLog(std::span<const u8> payload, SpellImmuneLog& out);

struct EnvironmentalDamageLog { // SMSG_ENVIRONMENTALDAMAGELOG
    u64 victim = 0;
    u8 type = 0; // EnviromentalDamage: 0 exhausted, 1 drowning, 2 fall, 3 lava, 4 slime, 5 fire
    u32 damage = 0;
    u32 absorbed = 0;
    i32 resisted = 0;
};
bool parseEnvironmentalDamageLog(std::span<const u8> payload, EnvironmentalDamageLog& out);

struct PartyKillLog { // SMSG_PARTYKILLLOG
    u64 killer = 0, victim = 0;
};
bool parsePartyKillLog(std::span<const u8> payload, PartyKillLog& out);

// -------------------------------------------------------------------------------------------------------
// Spellbook, action bar, casting, cooldowns (server -> client).

struct InitialSpells { // SMSG_INITIAL_SPELLS
    struct Cooldown {
        u16 spellId = 0;
        u16 itemId = 0;
        u16 category = 0;
        i32 recoveryMs = 0;         // remaining own cooldown
        i32 categoryRecoveryMs = 0; // remaining category cooldown; bit 31 = permanent ("cooldown on event")
    };
    std::vector<u16> spells;
    std::vector<Cooldown> cooldowns;
};
bool parseInitialSpells(std::span<const u8> payload, InitialSpells& out);

// SMSG_LEARNED_SPELL (u16 id + u16 0) / SMSG_REMOVED_SPELL (u16 id): the spell id.
bool parseLearnedSpell(std::span<const u8> payload, u32& spellId);
bool parseRemovedSpell(std::span<const u8> payload, u32& spellId);
// SMSG_SUPERCEDED_SPELL.
bool parseSupercededSpell(std::span<const u8> payload, u32& oldSpellId, u32& newSpellId);

// SMSG_ACTION_BUTTONS: exactly 120 packed buttons (action = low 24 bits, type = high byte, 0 = empty).
bool parseActionButtons(std::span<const u8> payload, std::array<u32, kActionButtonCount>& out);

struct SpellStart {     // SMSG_SPELL_START
    u64 caster = 0;     // the item when cast from one
    u64 unitCaster = 0; // the casting unit
    u32 spellId = 0;
    u16 castFlags = 0;
    u32 castTimeMs = 0;
    SpellCastTargets targets;
};
bool parseSpellStart(std::span<const u8> payload, SpellStart& out);

struct SpellGo { // SMSG_SPELL_GO
    struct Miss {
        u64 target = 0;
        SpellMiss miss = SpellMiss::None;
    };
    u64 caster = 0, unitCaster = 0;
    u32 spellId = 0;
    u16 castFlags = 0;
    std::vector<u64> hits;
    std::vector<Miss> misses;
    SpellCastTargets targets;
};
bool parseSpellGo(std::span<const u8> payload, SpellGo& out);

struct SpellFailedOther { // SMSG_SPELL_FAILED_OTHER (the caster gets it too)
    u64 caster = 0;
    u32 spellId = 0;
};
bool parseSpellFailedOther(std::span<const u8> payload, SpellFailedOther& out);

struct CastResult { // SMSG_CAST_RESULT
    u32 spellId = 0;
    u8 status = kCastStatusOk;
    u8 reason = 0;    // SpellCastResult, when status == kCastStatusFailed
    u32 argCount = 0; // 0..2 optional arguments (by remaining length)
    std::array<u32, 2> args{};
};
bool parseCastResult(std::span<const u8> payload, CastResult& out);

struct SpellCooldown { // SMSG_SPELL_COOLDOWN: pairs until the end, no count, no flags
    struct Entry {
        u32 spellId = 0;
        u32 cooldownMs = 0; // 0 = start the spell's global cooldown
    };
    u64 caster = 0;
    std::vector<Entry> entries;
};
bool parseSpellCooldown(std::span<const u8> payload, SpellCooldown& out);

// SMSG_COOLDOWN_EVENT (spellId, caster) / SMSG_CLEAR_COOLDOWN (spellId, target).
bool parseCooldownEvent(std::span<const u8> payload, u32& spellId, u64& guid);

struct SpellDelayed { // SMSG_SPELL_DELAYED (cast pushback)
    u64 caster = 0;
    u32 delayMs = 0;
};
bool parseSpellDelayed(std::span<const u8> payload, SpellDelayed& out);

struct ChannelStart { // MSG_CHANNEL_START (server -> caster)
    u32 spellId = 0;
    u32 durationMs = 0;
};
bool parseChannelStart(std::span<const u8> payload, ChannelStart& out);
// MSG_CHANNEL_UPDATE: remaining ms (0 = ended).
bool parseChannelUpdate(std::span<const u8> payload, u32& remainingMs);

// SMSG_UPDATE_AURA_DURATION (own auras only).
bool parseUpdateAuraDuration(std::span<const u8> payload, u8& slot, u32& durationMs);

// -------------------------------------------------------------------------------------------------------
// Gossip and quests (server -> client).

struct GossipMessage { // SMSG_GOSSIP_MESSAGE
    struct Option {
        u32 index = 0; // echo it in CMSG_GOSSIP_SELECT_OPTION
        u8 icon = 0;   // GossipOptionIcon
        bool coded = false;
        std::string_view text;
    };
    struct Quest {
        u32 questId = 0;
        u32 icon = 0; // QuestGiverStatus value
        u32 level = 0;
        std::string_view title;
    };
    static constexpr u32 kMaxItems = 32; // GOSSIP_MAX_MENU_ITEMS: more is malformed
    u64 source = 0;
    u32 textId = 0; // npc_text; 0xFFFFFF = default text
    u32 optionCount = 0;
    std::array<Option, kMaxItems> options{};
    u32 questCount = 0;
    std::array<Quest, kMaxItems> quests{};
};
bool parseGossipMessage(std::span<const u8> payload, GossipMessage& out);

struct NpcTextUpdate { // SMSG_NPC_TEXT_UPDATE: always 8 options
    struct Option {
        f32 probability = 0.0f;
        std::string_view maleText;
        std::string_view femaleText;
        u32 language = 0;
        std::array<u32, 3> emoteDelays{};
        std::array<u32, 3> emotes{};
    };
    u32 textId = 0;
    std::array<Option, 8> options{};
};
bool parseNpcTextUpdate(std::span<const u8> payload, NpcTextUpdate& out);

// SMSG_QUESTGIVER_STATUS (the status is a u32 on the wire).
bool parseQuestGiverStatus(std::span<const u8> payload, u64& guid, u32& status);

struct QuestGiverQuestList { // SMSG_QUESTGIVER_QUEST_LIST
    u64 questGiver = 0;
    std::string_view greeting;
    u32 emoteDelay = 0, emote = 0;
    u32 questCount = 0;
    std::array<GossipMessage::Quest, GossipMessage::kMaxItems> quests{};
};
bool parseQuestGiverQuestList(std::span<const u8> payload, QuestGiverQuestList& out);

struct QuestItem {
    u32 itemId = 0;
    u32 count = 0;
    u32 displayId = 0; // 0 when the server does not know the item
};
constexpr u32 kQuestChoiceCount = 6; // QUEST_REWARD_CHOICES_COUNT
constexpr u32 kQuestRewardCount = 4; // QUEST_REWARDS_COUNT
constexpr u32 kQuestObjectiveCount = 4;

struct QuestDetails {   // SMSG_QUESTGIVER_QUEST_DETAILS
    u64 questGiver = 0; // creature, game object, item, or the player sharing it
    u32 questId = 0;
    std::string_view title, details, objectives;
    bool activateAccept = true;
    u32 choiceCount = 0;
    std::array<QuestItem, kQuestChoiceCount> choices{};
    u32 rewardCount = 0;
    std::array<QuestItem, kQuestRewardCount> rewards{};
    i32 money = 0; // copper; negative = required
    u32 rewardSpell = 0;
    std::array<u32, 4> emotes{}, emoteDelays{};
};
bool parseQuestDetails(std::span<const u8> payload, QuestDetails& out);

struct QuestRequestItems { // SMSG_QUESTGIVER_REQUEST_ITEMS
    u64 questGiver = 0;
    u32 questId = 0;
    std::string_view title, text;
    u32 emoteDelay = 0, emote = 0;
    bool closeOnCancel = false;
    u32 requiredMoney = 0;
    u32 itemCount = 0;
    std::array<QuestItem, kQuestObjectiveCount> items{};
    bool completable = false; // all four completion flags set: "Continue" enabled
};
bool parseQuestRequestItems(std::span<const u8> payload, QuestRequestItems& out);

struct QuestOfferReward { // SMSG_QUESTGIVER_OFFER_REWARD
    u64 questGiver = 0;
    u32 questId = 0;
    std::string_view title, text;
    bool enableNext = true;
    u32 emoteCount = 0;
    std::array<u32, 4> emotes{}, emoteDelays{};
    u32 choiceCount = 0;
    std::array<QuestItem, kQuestChoiceCount> choices{};
    u32 rewardCount = 0;
    std::array<QuestItem, kQuestRewardCount> rewards{};
    i32 money = 0;
    u32 flags = 0;
    u32 rewardSpell = 0;
};
bool parseQuestOfferReward(std::span<const u8> payload, QuestOfferReward& out);

struct QuestComplete { // SMSG_QUESTGIVER_QUEST_COMPLETE (the chosen reward item is not listed)
    u32 questId = 0;
    u32 xp = 0;
    i32 money = 0;
    u32 itemCount = 0;
    std::array<QuestItem, kQuestRewardCount + 1> items{}; // displayId unused (0)
};
bool parseQuestComplete(std::span<const u8> payload, QuestComplete& out);

// SMSG_QUESTGIVER_QUEST_FAILED (quest id + QuestFailedReasons) / SMSG_QUESTGIVER_QUEST_INVALID (reason only).
bool parseQuestGiverQuestFailed(std::span<const u8> payload, u32& questId, u32& reason);
bool parseQuestGiverQuestInvalid(std::span<const u8> payload, u32& reason);

struct QuestQueryResponse { // SMSG_QUEST_QUERY_RESPONSE
    struct Objective {
        u32 creatureOrGo = 0; // creature entry, or game object entry | 0x80000000
        u32 creatureOrGoCount = 0;
        u32 itemId = 0;
        u32 itemCount = 0;
        std::string_view text; // may be empty
    };
    u32 questId = 0;
    u32 method = 0; // 0 autocomplete, 1 disabled, 2 deliver
    u32 level = 0;  // 0 = the player's level
    i32 zoneOrSort = 0;
    u32 type = 0;
    u32 repObjectiveFaction = 0;
    i32 repObjectiveValue = 0;
    u32 nextQuestInChain = 0;
    i32 money = 0;
    u32 moneyMaxLevel = 0;
    u32 rewardSpell = 0;
    u32 sourceItemId = 0;
    u32 flags = 0;
    std::array<QuestItem, kQuestRewardCount> rewards{}; // displayId unused (0)
    std::array<QuestItem, kQuestChoiceCount> choices{};
    u32 pointMapId = 0;
    f32 pointX = 0.0f, pointY = 0.0f;
    u32 pointOpt = 0;
    std::string_view title, objectives, details, endText;
    std::array<Objective, kQuestObjectiveCount> objectiveList{};
};
bool parseQuestQueryResponse(std::span<const u8> payload, QuestQueryResponse& out);

struct QuestUpdateAddKill { // SMSG_QUESTUPDATE_ADD_KILL: `count` is the new total
    u32 questId = 0;
    u32 entry = 0; // creature entry, or game object entry | 0x80000000
    u32 count = 0;
    u32 required = 0;
    u64 source = 0;
};
bool parseQuestUpdateAddKill(std::span<const u8> payload, QuestUpdateAddKill& out);
// SMSG_QUESTUPDATE_ADD_ITEM: item id and how many were added (a delta).
bool parseQuestUpdateAddItem(std::span<const u8> payload, u32& itemId, u32& added);
// SMSG_QUESTUPDATE_COMPLETE / _FAILED / _FAILEDTIMER: the quest id (also SMSG_QUEST_CONFIRM_ACCEPT's first
// field).
bool parseQuestId(std::span<const u8> payload, u32& questId);

struct QuestConfirmAccept { // SMSG_QUEST_CONFIRM_ACCEPT (party-accept quests)
    u32 questId = 0;
    std::string_view title;
    u64 sharer = 0;
};
bool parseQuestConfirmAccept(std::span<const u8> payload, QuestConfirmAccept& out);

// -------------------------------------------------------------------------------------------------------
// Loot and items (server -> client).

struct LootResponse { // SMSG_LOOT_RESPONSE, normal or error form
    struct Item {
        u8 slot = 0; // echo it in CMSG_AUTOSTORE_LOOT_ITEM (not contiguous: quest items come after)
        u32 itemId = 0;
        u32 count = 0;
        u32 displayId = 0;
        u32 randomSuffix = 0;
        u32 randomPropertyId = 0;
        u8 slotType = kLootSlotAllowLoot;
    };
    u64 guid = 0;
    u8 lootType = 0; // 0: error form
    u8 error = 0;    // LootError, error form only
    u32 gold = 0;    // copper
    std::vector<Item> items;
};
bool parseLootResponse(std::span<const u8> payload, LootResponse& out);

// SMSG_LOOT_RELEASE_RESPONSE: the looted object.
bool parseLootReleaseResponse(std::span<const u8> payload, u64& guid);
// SMSG_LOOT_REMOVED: the loot slot.
bool parseLootRemoved(std::span<const u8> payload, u8& slot);
// Bodies that are a single u32: SMSG_LOOT_MONEY_NOTIFY (copper), SMSG_CORPSE_RECLAIM_DELAY (ms),
// SMSG_QUESTGIVER_QUEST_INVALID (reason).
bool parseU32(std::span<const u8> payload, u32& value);

struct ItemPushResult { // SMSG_ITEM_PUSH_RESULT (may be broadcast to the group: check `player`)
    u64 player = 0;
    bool fromNpc = false; // "received": 0 looted, 1 from an NPC
    bool created = false;
    bool showInChat = false;
    u8 bag = 0;      // 255 = backpack
    u32 bagSlot = 0; // 0xFFFFFFFF when merged into an existing stack
    u32 itemId = 0;
    u32 suffixFactor = 0;
    u32 randomPropertyId = 0;
    u32 count = 0;
};
bool parseItemPushResult(std::span<const u8> payload, ItemPushResult& out);

struct InventoryChangeFailure { // SMSG_INVENTORY_CHANGE_FAILURE
    u8 result = 0;              // InventoryResult; 0 = OK (a 1-byte packet)
    u32 requiredLevel = 0;      // result 1 only
    u64 item1 = 0, item2 = 0;
    u8 bagSlot = 0;
};
bool parseInventoryChangeFailure(std::span<const u8> payload, InventoryChangeFailure& out);

struct GameObjectQueryInfo { // SMSG_GAMEOBJECT_QUERY_RESPONSE
    u32 entry = 0;
    bool found = false; // false: the server answered entry | 0x80000000
    u32 type = 0;       // GameobjectTypes
    u32 displayId = 0;
    std::string_view name;
    std::string_view icon;
    std::array<u32, 24> data{}; // meaning depends on the type
};
bool parseGameObjectQueryResponse(std::span<const u8> payload, GameObjectQueryInfo& out);

// -------------------------------------------------------------------------------------------------------
// Experience and death (server -> client).

struct XpGain {      // SMSG_LOG_XPGAIN
    u64 victim = 0;  // 0 for quest / exploration experience
    u32 totalXp = 0; // with the rested bonus
    bool kill = true;
    u32 baseXp = 0; // kills: without the rested bonus (= totalXp otherwise)
    f32 groupBonus = 1.0f;
};
bool parseXpGain(std::span<const u8> payload, XpGain& out);

struct LevelUpInfo { // SMSG_LEVELUP_INFO
    u32 level = 0;
    u32 healthGain = 0;
    std::array<u32, 5> powerGains{}; // [0] mana
    std::array<u32, 5> statGains{};  // strength, agility, stamina, intellect, spirit
};
bool parseLevelUpInfo(std::span<const u8> payload, LevelUpInfo& out);

struct CorpseQuery { // MSG_CORPSE_QUERY (server -> client); unsolicited `found = false` when it turned to
                     // bones
    bool found = false;
    i32 mapId = 0; // the corpse's map, or the entrance's map when it lies in an instance
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    u32 corpseMapId = 0;
};
bool parseCorpseQuery(std::span<const u8> payload, CorpseQuery& out);

struct ResurrectRequest { // SMSG_RESURRECT_REQUEST
    u64 resurrector = 0;
    std::string_view name; // empty when a player casts it
    bool sickness = false;
    bool delayed = false;
};
bool parseResurrectRequest(std::span<const u8> payload, ResurrectRequest& out);

// A body that is a single full GUID: SMSG_SPIRIT_HEALER_CONFIRM.
bool parseGuid(std::span<const u8> payload, u64& guid);
// SMSG_STANDSTATE_UPDATE.
bool parseStandStateUpdate(std::span<const u8> payload, u8& standState);

// -------------------------------------------------------------------------------------------------------
// Chat (server -> client).

struct ChatMessage { // SMSG_MESSAGECHAT (ChatHandler::BuildChatPacket)
    ChatType type = ChatType::Say;
    u32 language = 0;
    u64 sender = 0;              // 0 for MONSTER_EMOTE / MONSTER_WHISPER / RAID_BOSS_* (name only)
    u64 target = 0;              // monster messages: who it is addressed to
    std::string_view senderName; // monster messages only: players' names never travel (name query)
    std::string_view channel;    // CHANNEL
    u32 playerRank = 0;          // CHANNEL
    std::string_view text;
    u8 tag = 0; // 0 none, 1 AFK, 2 DND, 3 GM
};
bool parseChatMessage(std::span<const u8> payload, ChatMessage& out);

// SMSG_NOTIFICATION / SMSG_CHAT_PLAYER_NOT_FOUND: one C string.
bool parseCString(std::span<const u8> payload, std::string_view& text);
// SMSG_AREA_TRIGGER_MESSAGE: u32 length (with the NUL) + string.
bool parseAreaTriggerMessage(std::span<const u8> payload, std::string_view& text);

struct TextEmote { // SMSG_TEXT_EMOTE
    u64 guid = 0;
    u32 textEmote = 0; // EmotesText.dbc
    u32 emoteNum = 0;
    std::string_view targetName; // empty without a target
};
bool parseTextEmote(std::span<const u8> payload, TextEmote& out);

// -------------------------------------------------------------------------------------------------------
// Visuals and sounds (phase 9).

// SMSG_PLAY_SPELL_VISUAL (unit + SpellVisualKit id) and SMSG_PLAY_SPELL_IMPACT (target + kit id): full GUIDs.
bool parseSpellVisualKit(std::span<const u8> payload, u64& guid, u32& kitId);
// SMSG_PLAY_SOUND / SMSG_PLAY_MUSIC: a SoundEntries id.
bool parseSoundId(std::span<const u8> payload, u32& soundId);
// SMSG_PLAY_OBJECT_SOUND (sound, then the object) and SMSG_EMOTE (Emotes.dbc id, then the unit): full GUIDs.
bool parseIdAndGuid(std::span<const u8> payload, u32& id, u64& guid);

// -------------------------------------------------------------------------------------------------------
// Client -> server bodies. Everything not listed is empty (CMSG_ATTACKSTOP, CMSG_LOOT_MONEY,
// CMSG_REPOP_REQUEST, CMSG_QUESTGIVER_CANCEL, MSG_CORPSE_QUERY) or a single full GUID written with
// writeGuid(): CMSG_SET_SELECTION, CMSG_ATTACKSWING, CMSG_GOSSIP_HELLO, CMSG_QUESTGIVER_STATUS_QUERY,
// CMSG_QUESTGIVER_HELLO, CMSG_LOOT, CMSG_LOOT_RELEASE, CMSG_RECLAIM_CORPSE, CMSG_GAMEOBJ_USE,
// CMSG_SPIRIT_HEALER_ACTIVATE.

void writeGuid(std::vector<u8>& out, u64 guid);
// A single u32: CMSG_CANCEL_CAST, CMSG_CANCEL_AURA, CMSG_CANCEL_CHANNELLING, CMSG_QUEST_QUERY,
// CMSG_QUEST_CONFIRM_ACCEPT, CMSG_STANDSTATECHANGE, CMSG_SETSHEATHED (u32 bodies, not u8).
void writeU32(std::vector<u8>& out, u32 value);
// CMSG_CAST_SPELL.
void writeCastSpell(std::vector<u8>& out, u32 spellId, const SpellCastTargets& targets);
// CMSG_USE_ITEM: bag (255 = backpack), slot, index of the item's on-use spell, targets.
void writeUseItem(std::vector<u8>& out, u8 bag, u8 slot, u8 spellIndex, const SpellCastTargets& targets);
// CMSG_SET_ACTION_BUTTON: `packed` = action | type << 24, 0 removes the button.
void writeSetActionButton(std::vector<u8>& out, u8 button, u32 packed);
// CMSG_GOSSIP_SELECT_OPTION: the code is only sent for coded options.
void writeGossipSelectOption(std::vector<u8>& out, u64 source, u32 option, std::string_view code = {});
// CMSG_NPC_TEXT_QUERY.
void writeNpcTextQuery(std::vector<u8>& out, u32 textId, u64 npc);
// GUID + quest id: CMSG_QUESTGIVER_QUERY_QUEST, _ACCEPT_QUEST, _COMPLETE_QUEST, _REQUEST_REWARD.
void writeQuestGiverQuest(std::vector<u8>& out, u64 questGiver, u32 questId);
// CMSG_QUESTGIVER_CHOOSE_REWARD: index into the choice list (0..5).
void writeQuestGiverChooseReward(std::vector<u8>& out, u64 questGiver, u32 questId, u32 choice);
// CMSG_QUESTLOG_REMOVE_QUEST: the quest log slot (0..19). CMSG_AUTOSTORE_LOOT_ITEM: the loot slot.
void writeU8(std::vector<u8>& out, u8 value);
// CMSG_RESURRECT_RESPONSE.
void writeResurrectResponse(std::vector<u8>& out, u64 resurrector, bool accept);
// CMSG_MESSAGECHAT: `target` is written for WHISPER (player name) and CHANNEL (channel name) only.
void writeMessageChat(std::vector<u8>& out, ChatType type, Language language, std::string_view target,
                      std::string_view text);
// CMSG_GAMEOBJECT_QUERY / CMSG_ITEM_QUERY_SINGLE / CMSG_CREATURE_QUERY: entry + GUID.
void writeEntryQuery(std::vector<u8>& out, u32 entry, u64 guid);

} // namespace vd::net
