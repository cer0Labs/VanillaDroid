#pragma once

#include "core/Types.h"
#include "net/Opcodes.gen.h"

#include <array>
#include <span>

namespace vd::game {

// Where the game state sends its requests: NetClient::send in the game, a capture buffer in tests.
struct PacketSink {
    bool (*send)(void* context, net::Opcode opcode, std::span<const u8> body) = nullptr;
    void* context = nullptr;

    bool operator()(net::Opcode opcode, std::span<const u8> body) const {
        return send && send(context, opcode, body);
    }
};

// Something the HUD reacts to once: error texts, "You receive loot", quest progress lines, level up... Fields
// that do not apply stay 0. Read them with Gameplay::events() (an EventRing: keep your own cursor).
enum class GameEventKind : u8 {
    // Auto attack. SwingDeadTarget also ends it.
    SwingNotInRange,
    SwingBadFacing,
    SwingNotStanding,
    SwingDeadTarget,
    SwingCantAttack,
    AttackStarted, // guid = the victim of the player's auto attack
    AttackStopped, // guid = the victim (0 if unknown)
    // Spells.
    // id = spell, value[0] = SpellCastResult (net::spellCastResultName), value[1..2] = its arguments.
    CastFailed,
    SpellLearned, // id = spell
    SpellRemoved, // id = spell
    // Experience.
    // SMSG_LOG_XPGAIN. guid = the victim of a kill the client knows as a unit (0: a quest, exploration, an unknown
    // victim), id = 1 for experience lost (the player as victim); value[0] = total (with the rest bonus), value[1] =
    // base (without it; the total when not a kill), value[2] = the group bonus factor (std::bit_cast<f32>; 1 none).
    XpGained,
    // value[0] = new level, value[1] = health gained, value[2] = mana gained (everything: lastLevelUp());
    // id = the level before (the player's level field when the packet came, 0 if unknown).
    LevelUp,
    // Loot and items.
    // id = item entry, value[0] = count, value[1] = 1 from an NPC (a reward), value[2] = 1 created; other = 1
    // when the server keeps it out of the chat (showInChat off: quest rewards, which the quest's lines name).
    ItemReceived,
    // value[0] = copper (the player's share), value[1] = 1 a group's share (SMSG_LOOT_MONEY_NOTIFY), 0 alone.
    MoneyLooted,
    LootError,      // guid = the object, value[0] = LootError (net::lootErrorName)
    InventoryError, // value[0] = InventoryResult (net::inventoryResultName), value[1] = required level
    // Quests.
    QuestAccepted, // id = quest (a new quest-log entry)
    QuestRemoved,  // id = quest (abandoned or turned in)
    // id = quest, guid = the creature / object, value[0] = entry (| 0x80000000: game object),
    // value[1] = count now, value[2] = needed.
    QuestObjectiveProgress,
    // id = item, value[0] = added, value[1] = carried with it (bags + added, up to the need), value[2] = needed by
    // the first quest asking for it (0: no line, the bags already held enough or no quest asks for it).
    QuestItemProgress,
    QuestObjectivesDone, // id = quest: every objective met (log state bit or SMSG_QUESTUPDATE_COMPLETE)
    QuestFailed,         // id = quest
    QuestRewarded,       // id = quest, value[0] = experience, value[1] = money (copper)
    QuestLogFull,
    QuestInvalid,     // value[0] = QuestFailedReasons (SMSG_QUESTGIVER_QUEST_INVALID)
    QuestGiverFailed, // id = quest, value[0] = QuestFailedReasons (SMSG_QUESTGIVER_QUEST_FAILED)
    QuestShared,      // id = quest, guid = who shares it: answer with Gameplay::confirmSharedQuest()
    // Death.
    PlayerDied,
    PlayerReleased,      // a ghost now
    PlayerAlive,         // resurrected
    ResurrectRequest,    // guid = resurrector: answer with Gameplay::answerResurrect()
    SpiritHealerConfirm, // guid = spirit healer: answer with Gameplay::acceptSpiritHealer()
    CorpseLocated,       // Gameplay::death() has the corpse position
    // Others.
    PartyKill, // guid = victim, other = killer
    // Phase 8, trainers, talents, pets and reputation (game/Abilities).
    TrainerBuySucceeded, // id = the trainer's spell, guid = trainer
    TrainerBuyFailed,    // id = the trainer's spell, guid = trainer, value[0] = net::kTrainFail*
    TalentWipeOffer,     // guid = trainer (0: no talent points spent), value[0] = cost (copper)
    PetFeedback,         // guid = pet, value[0] = net::kPetFeedback* (0: the pet ran away, SMSG_PET_BROKEN)
    PetCastFailed,       // id = spell, guid = pet, value[0] = SpellCastResult (net::spellCastResultName)
    PetTameFailed,       // value[0] = PetTameFailureReason
    PetChanged,          // guid = the new pet or charmed unit (0: gone)
    ReputationChanged,   // id = reputation list id, value[0] = change, value[1] = standing now
    FactionVisible,      // id = reputation list id (now listed)
    // Items, vendors, the bank, books (phase 8: game/Inventory).
    VendorOpened,   // guid = the vendor (Inventory::vendor() has its list)
    ItemBought,     // id = item entry (0 if the row is unknown), guid = vendor, value[0] = items received
    BuyFailed,      // id = item entry, guid = vendor, value[0] = BuyResult (net::buyResultName)
    SellFailed,     // guid = vendor, other = the item, value[0] = SellResult (net::sellResultName)
    BankOpened,     // guid = the banker
    BankSlotBought, // value[0] = bank bag slots now
    BankSlotFailed, // value[0] = BuyBankSlotResult (net::bankSlotResultName)
    ReadingOpened,  // other = the item (Inventory::reading())
    ReadItemFailed, // other = the item, value[0] = reason
    DurabilityLoss, // the equipped items lost 10% durability (death)
    // Travel (phase 8, game::Travel).
    ZoneChanged,     // id = zone, value[0] = area (the subzone, or the zone), value[1] = 1 when the zone changed
    AreaExplored,    // id = area, value[0] = experience (SMSG_EXPLORATION_EXPERIENCE)
    AreaTriggerSent, // id = AreaTrigger.dbc (CMSG_AREATRIGGER)
    TaxiPathLearned, // SMSG_NEW_TAXI_PATH: "new flight path"
    TaxiMapOpened,   // guid = flight master (SMSG_SHOWTAXINODES): Travel::taxiMap()
    TaxiReply,       // value[0] = net::TaxiReply (an error unless Ok)
    BinderConfirm,   // guid = innkeeper: answer with Travel::answerBinder()
    PlayerBound,     // id = area: the hearthstone now leads there (SMSG_PLAYERBOUND)
    TransferAborted, // value[0] = TransferAbortReason (net::kTransferAbort*)
    // Instances (phase 12, game::Travel).
    InstanceSaved,         // SMSG_INSTANCE_SAVE_CREATED: the player is now saved to this raid (INSTANCE_SAVED)
    InstanceResetWarning,  // id = map, value[0] = net::kRaidInstance* type, value[1] = seconds until the reset
    InstanceReset,         // id = map (SMSG_INSTANCE_RESET): INSTANCE_RESET_SUCCESS
    InstanceResetFailed,   // id = map, value[0] = net::kInstanceResetFailed* reason
    RaidGroupOnly,         // value[0] = net::kRaidGroup* error: an entry refused, or the boot countdown stopped
    InstanceBootStarted,   // value[0] = ms until the player is sent to its bind point (INSTANCE_BOOT_START)
    InstanceBootStopped,   // INSTANCE_BOOT_STOP
    SavedInstancesUpdated, // UPDATE_INSTANCE_INFO: Travel::savedInstances(); value[0] = 1: it answers /raidinfo
    // Group loot, mail, the auction house and trade (phase 8, game::Exchange).
    LootRollStarted, // guid = the looted object, id = item, value[0] = loot slot, value[1] = countdown (ms)
    // guid = the looted object, other = who rolled, id = item, value[0] = net::LootRollKind, value[1] = the
    // dice (NeedRoll / GreedRoll), value[2] = loot slot.
    LootRolled,
    // guid = the looted object, other = winner, id = item, value[0] = the winning roll, value[1] =
    // net::kRollNeed / kRollGreed, value[2] = loot slot.
    LootRollWon,
    LootAllPassed, // guid = the looted object, id = item, value[2] = loot slot
    MailListed,    // the mailbox's messages arrived (Exchange::mail())
    // id = message (0 for a send), value[0] = net::kMailAction*, value[1] = net::kMailOk / kMailErr*,
    // value[2] = InventoryResult (kMailErrEquipError).
    MailResult,
    NewMail,       // SMSG_RECEIVED_MAIL: the minimap shows the letter
    AuctionOpened, // guid = the auctioneer (Exchange::auction())
    // id = auction, value[0] = net::kAuctionAction*, value[1] = net::kAuctionOk / kAuctionErr*, value[2] =
    // InventoryResult (kAuctionErrInventory).
    AuctionResult,
    AuctionOutbid,  // id = item entry, value[0] = the new bid
    AuctionWon,     // id = item entry
    AuctionBid,     // id = item entry, value[0] = the bid: someone bid on one of the player's auctions
    AuctionSold,    // id = item entry, value[0] = the price
    AuctionRemoved, // id = item entry: an auction the player bid on was cancelled (the money comes by mail)
    TradeRequested, // guid = who wants to trade: answer with Trade::acceptRequest() / declineRequest()
    TradeOpened,    // guid = the partner
    TradeClosed,    // guid = the partner, value[0] = net::kTrade* (cancelled, complete or why it failed)
    // Parity: a group member's item (SMSG_ITEM_PUSH_RESULT is broadcast to the group for group loot):
    // guid = the member, id = item entry, value[0] = count, value[2] = 1 created.
    MemberItemReceived,
    // Parity (popups): logging out (game::Logout) and summons (game::Summon).
    LogoutRefused,   // value[0] = the server's reason (net::kLogoutRefused*): ERR_LOGOUT_FAILED
    LogoutStarted,   // the 20 s countdown runs (PLAYER_CAMPING; value[0] = 1: PLAYER_QUITING)
    LogoutInstant,   // at once (resting, a taxi, a GM): SMSG_LOGOUT_COMPLETE follows; value[0] = 1 quit
    LogoutCancelled, // SMSG_LOGOUT_CANCEL_ACK (LOGOUT_CANCEL)
    SummonRequested, // guid = summoner, id = zone (AreaTable), value[0] = ms until it expires: Summon::accept
    // Phase 13 (lead: lost messages).
    // value[0] = UnitMountResult (SMSG_MOUNTRESULT) or UnitDismountResult (SMSG_DISMOUNTRESULT), value[1] = 1 for a
    // dismount: the client's ERR_MOUNT_* / ERR_DISMOUNT_* line. Never sent for the successes.
    MountFailed,
    // Phase 13 (spells): a cast waiting for a place or an item (Gameplay::targeting(), the client's SpellIsTargeting).
    SpellTargetingStarted, // id = spell, value[0] = game::SpellAim
    SpellTargetingStopped, // id = spell: cast, dropped or replaced (CURRENT_SPELL_CAST_CHANGED)
    // id = spell, guid = the item (0: the trade slot), value[0] = game::EnchantAsk (BIND_ENCHANT, REPLACE_ENCHANT,
    // TRADE_REPLACE_ENCHANT), value[1] / value[2] = the old / new enchantment (SpellItemEnchantment).
    EnchantQuestion,
    // Phase 13 (social): the player's menus and slash commands (game::PlayerCommands).
    TimePlayed,        // value[0] = seconds played in total, value[1] = at this level (TIME_PLAYED_MSG)
    PetNameInvalid,    // SMSG_PET_NAME_INVALID: the rename was refused (ERR_INVALID_PETNAME)
    PvpToggled,        // value[0] = 1 on, 0 off: the PvP wish the player's /pvp asked for (ERR_PVP_TOGGLE_ON/OFF)
    RaidTargetChanged, // RAID_TARGET_UPDATE: id = icon (1..8; 0: the whole list came), guid = its unit (0: off)
    // Phase 13 (quests): sharing quests, the watch list, fishing and readable objects.
    QuestPushResult,  // guid = the party member, value[0] = net::kQuestPush* (MSG_QUEST_PUSH_RESULT): ERR_QUEST_PUSH_*
    QuestWatchUpdate, // id = quest: an objective progressed (QUEST_WATCH_UPDATE; AUTO_QUEST_WATCH watches it)
    FishNotHooked,    // SMSG_FISH_NOT_HOOKED: ERR_FISH_NOT_HOOKED
    FishEscaped,      // SMSG_FISH_ESCAPED: ERR_FISH_ESCAPED
    // Phase 13 (combat): PLAYER_CONTROL_LOST / _GAINED (SMSG_CLIENT_CONTROL_UPDATE for the player: feared, confused,
    // charmed, possessed; game::MoverControl). UIParent closes the windows and flashes OutOfControlFrame.
    ControlLost,
    ControlGained,
    // Phase 13 (combat): SMSG_FEIGN_DEATH_RESISTED: the UIErrorsFrame's ERR_FEIGN_DEATH_RESISTED ("Resisted").
    FeignDeathResisted,
    // Phase 15 (pets): the stable and a pet trainer's untraining offer (game::PetStable).
    StableOpened,    // guid = the stable master: its list came (PET_STABLE_SHOW; also after each change)
    StableUpdated,   // PET_STABLE_UPDATE
    StableClosed,    // PET_STABLE_CLOSED
    StableFailed,    // value[0] = net::kStableErr*: ERR_NOT_ENOUGH_MONEY (the other failures say nothing)
    PetUnlearnOffer, // guid = the pet, value[0] = cost (copper): CONFIRM_PET_UNLEARN
    // Phase 15 (items): SMSG_OPEN_CONTAINER (BAG_OPEN): guid = the bag just put in a bag slot (the player: the
    // backpack); its ContainerFrame opens.
    ContainerOpened,
    // Phase 15 (options): guid = who asked to trade while BLOCK_TRADES refused it (CMSG_BUSY_TRADE): ERR_TRADE_BLOCKED_S.
    TradeBlocked,
    // Phase 16 (alerts): the ready check (game::ReadyCheck) and the meeting stones' queue (game::MeetingStone).
    ReadyCheck,          // READY_CHECK: guid = the leader asking; ReadyCheckFrame shows (ReadyCheck::prompting())
    ReadyCheckNotice,    // value[0] = game::ReadyCheckLine, guid = whom it names, value[1] = ReadyCheck::afk() count
    MeetingStoneChanged, // MEETINGSTONE_CHANGED: id = the queue's area (0: none), value[0] = status, value[1] = before
    MeetingStoneNotice,  // value[0] = game::MeetingStoneLine, id = its area, guid = the player it names
    MeetingStoneJoined,  // guid = the player: queued at a stone ("HARDCODED Meeting Stone Join" on it)
    // Phase 17 (lead): the battleground queues (game::Battlegrounds) and the world states (game::WorldStates).
    BattlefieldStatus, // UPDATE_BATTLEFIELD_STATUS: id = the queue slot, value[0] = BattlefieldState, value[1] = before
    WorldStates,       // UPDATE_WORLD_STATES: id = the state that changed (0: a whole new set), value[0] = its value
    // game::BattlefieldQueueing: the battlemaster's list and BattlefieldFrame_UpdateStatus' sounds and lines.
    BattlefieldsShow,       // BATTLEFIELDS_SHOW: a list came (id = its map)
    BattlefieldsClosed,     // BATTLEFIELDS_CLOSED: the player walked away from where it came
    BattlefieldQueueJoined, // a queue newly "queued" (id = its slot): PVPENTERQUEUE, the minimap's shine
    BattlefieldConfirm,     // a queue in "confirm" (id = its slot): PVPTHROUGHQUEUE, its popup again
    BattlefieldNotice,      // value[0] = game::BattlefieldLine, id = its map, value[1] = seconds
    // Phase 17 (scores).
    BattlefieldScore,   // UPDATE_BATTLEFIELD_SCORE (0x1A4): the score's lines (their names known), sorted and filtered
    BattlegroundPlayer, // SMSG_BATTLEGROUND_PLAYER_JOINED (value[0] 1) / _LEFT (0): guid = the player
    // The area spirit healer (game::AreaSpiritHealer), honor and the insignia (game::Honor).
    AreaSpiritHealerInRange,    // AREA_SPIRIT_HEALER_IN_RANGE: guid = the spirit guide, value[0] = ms left
    AreaSpiritHealerOutOfRange, // AREA_SPIRIT_HEALER_OUT_OF_RANGE (Waiting to Resurrect cancelled, left)
    // SMSG_PVP_CREDIT: CHAT_MSG_COMBAT_HONOR_GAIN, COMBAT_TEXT_UPDATE "HONOR_GAINED" and "HK: <rank>" over
    // the player. guid = the victim (0: honor awarded), id = its rank, value[0] = honor, value[1] =
    // game::HonorCredit, value[2] = the victim's side (0 Horde, 1 Alliance).
    HonorGained,
    PlayerSkinned, // PLAYER_SKINNED: an enemy took the insignia (DEATH_CORPSE_SKINNED); value[0] = free repop
    // Phase 18 (glue).
    // Phase 18 (create).
    // Phase 18 (p3).
    // TUTORIAL_TRIGGER: id = the tutorial (game::Tutorial, 0-based: TUTORIAL<id + 1>), value[0] = Tutorials::session()
    TutorialTriggered,
    // Ours (NPC voices): guid = a unit the player clicked: selected by a tap (the left click: 0x4925D0), or tapped
    // again while the context button has nothing to do with it (OnRightClick 0x60BEA0). Its click voice (app/GameAudio:
    // CGUnit::PlayClickVoice 0x60C270).
    UnitClicked,
};

struct GameEvent {
    GameEventKind kind = GameEventKind::SwingNotInRange;
    u32 timeMs = 0;
    u64 guid = 0;
    u64 other = 0;
    u32 id = 0;
    std::array<i32, 3> value{};
};

// What every client nearby sees or hears (phase 9: animations, spell visuals, sounds), for any unit the server
// tells about, not only the player. Read them with Gameplay::fx() (an EventRing: keep your own cursor; several
// readers — sounds, visuals, animations — follow it independently). Hits, misses and heals are in the combat log
// ring (Gameplay::combatLog()); casts of other units in progress in SpellBook::unitCast(); channels of other
// units in their UNIT_CHANNEL_SPELL / UNIT_FIELD_CHANNEL_OBJECT update fields.
enum class FxEventKind : u8 {
    // guid = caster, other = target unit (0: none or a location), id = spell, value[0] = cast time (ms),
    // position = the destination of ground-targeted spells (hasPosition). SMSG_SPELL_START.
    SpellStart,
    // guid = caster, other = target unit (the explicit one, else the first hit; 0: none), id = spell,
    // value[0] = units hit, value[1] = units missed, position as SpellStart. SMSG_SPELL_GO.
    SpellGo,
    // guid = caster, other = a unit the spell hit: one per hit target of SMSG_SPELL_GO, after its SpellGo.
    SpellHit,
    // guid = caster, id = spell (SMSG_SPELL_FAILED_OTHER, which the caster gets too): the cast was interrupted
    // or failed; its precast visual stops.
    SpellInterrupted,
    ChannelStart, // guid = the player, id = spell, value[0] = duration (ms) — MSG_CHANNEL_START is only for it
    ChannelEnd,   // guid = the player, id = spell (MSG_CHANNEL_UPDATE with 0 left)
    SpellVisual,  // guid = the unit, id = SpellVisualKit.dbc (SMSG_PLAY_SPELL_VISUAL)
    SpellImpact,  // guid = the unit, id = SpellVisualKit.dbc (SMSG_PLAY_SPELL_IMPACT)
    Sound,        // id = SoundEntries.dbc (SMSG_PLAY_SOUND)
    Music,        // id = SoundEntries.dbc (SMSG_PLAY_MUSIC)
    ObjectSound,  // guid = the object, id = SoundEntries.dbc (SMSG_PLAY_OBJECT_SOUND)
    Emote,        // guid = the unit, id = Emotes.dbc (SMSG_EMOTE): play its animation once
    TextEmote,    // guid = the unit, id = EmotesText.dbc, value[0] = emote number (SMSG_TEXT_EMOTE)
    AttackStart,  // guid = attacker, other = victim (SMSG_ATTACKSTART, any unit)
    AttackStop,   // guid = attacker, other = victim, value[0] = 1 when one of them died (SMSG_ATTACKSTOP)
    // Phase 13 (lead): guid = the creature, id = net::kAiReaction* (SMSG_AI_REACTION): alert = it noticed the
    // player (its alert sound), hostile = it attacks (its aggro sound).
    AiReaction,
    // Phase 13 (quests): living world objects (app/ServerObjects plays them on the object's model).
    ObjectCustomAnim,  // guid = game object, id = 0..3: its Custom0..3 once (SMSG_GAMEOBJECT_CUSTOM_ANIM)
    ObjectSpawnAnim,   // guid = game object (SMSG_GAMEOBJECT_SPAWN_ANIM)
    ObjectDespawnAnim, // guid = game object (SMSG_GAMEOBJECT_DESPAWN_ANIM): it goes away after it
    ObjectResetState,  // guid = game object (SMSG_GAMEOBJECT_RESET_STATE): back to its state's loop
    // Phase 13 (combat): a chain spell's jumps (SMSG_SPELL_UPDATE_CHAIN_TARGETS: Chain Lightning, Chain Heal), one
    // event per unit in order: guid = caster, other = the unit, id = spell, value[0] = its index, value[1] = how
    // many (the beams between them: SpellChainEffects, ROADMAP F14.4).
    ChainTarget,
    // Phase 18 (extras): guid = a mounted unit whose mount rears: its MountSpecial (AnimationData 94) once
    // (SMSG_MOUNTSPECIAL_ANIM for another player, 0x603FF0; Gameplay::mountSpecial() for the player, 0x60DEA0).
    MountSpecial,
    // guid = the player's pet, id = net::kPetTalk*: its CreatureSoundData order (special spell) or attack sound
    // (SMSG_PET_ACTION_SOUND, 0x6040C0 -> 0x623A40).
    PetTalk,
};

struct FxEvent {
    FxEventKind kind = FxEventKind::SpellStart;
    bool hasPosition = false;
    u32 timeMs = 0;
    u64 guid = 0;
    u64 other = 0;
    u32 id = 0;
    std::array<i32, 2> value{};
    std::array<f32, 3> position{}; // world coordinates (server = world)
};

} // namespace vd::game
