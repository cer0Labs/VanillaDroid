#pragma once

#include "core/Types.h"
#include "game/Abilities.h"
#include "game/AccountData.h" // phase 18 (p3)
#include "game/AreaSpiritHealer.h" // phase 17 (honor)
#include "game/Auras.h"
#include "game/BattlefieldQueue.h"
#include "game/Battlegrounds.h"
#include "game/BattlefieldScore.h"
#include "game/CharacterSheet.h"
#include "game/Chat.h"
#include "game/Combat.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/Exchange.h"
#include "game/GameData.h"
#include "game/GameEvents.h"
#include "game/GuildControl.h"
#include "game/HelpDesk.h"
#include "game/Honor.h" // phase 17 (honor)
#include "game/Inventory.h"
#include "game/Logout.h"
#include "game/Loot.h"
#include "game/MeetingStone.h"
#include "game/Minimap.h"
#include "game/MirrorTimers.h"
#include "game/MoverControl.h"
#include "game/NpcDialog.h"
#include "game/PetStable.h"
#include "game/Petitions.h"
#include "game/PlayerCommands.h"
#include "game/Queries.h"
#include "game/QuestJournal.h"
#include "game/Quests.h"
#include "game/Reactions.h"
#include "game/ReadyCheck.h"
#include "game/Social.h"
#include "game/SpellMods.h"
#include "game/SpellTargeting.h"
#include "game/Spells.h"
#include "game/Summon.h"
#include "game/TabardDesigner.h"
#include "game/TradeSkills.h"
#include "game/Tutorials.h" // phase 18 (p3)
#include "game/Travel.h"
#include "game/WeaponEnchants.h"
#include "game/Weather.h"
#include "game/WorldStateUi.h"
#include "game/WorldStates.h"
#include "net/GameMessages.h"
#include "net/MiscMessages.h"
#include "net/SpellLogMessages.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vd::game {

struct InventoryItem; // game/UnitInfo.h
class LanguageData;   // game/Languages.h (phase 18, extras)

// What interact() did.
enum class Interaction : u8 {
    None,         // nothing to do with it (unknown object, a dead unit without loot, not in the world...)
    Attack,       // started the auto attack
    Loot,         // asked to loot the corpse (the loot window opens on the answer)
    Gossip,       // CMSG_GOSSIP_HELLO: a gossip or quest window will open
    QuestGiver,   // CMSG_QUESTGIVER_HELLO: a quest window will open
    SpiritHealer, // talked to a spirit healer (its gossip offers the resurrection)
    UseObject,    // CMSG_GAMEOBJ_USE: chests open the loot window, quest objects their quest window...
    Trainer,      // CMSG_TRAINER_LIST (a trainer without a gossip menu): the trainer window opens
    Vendor,       // CMSG_LIST_INVENTORY: the vendor window opens on the answer (Inventory::vendor())
    Bank,         // CMSG_BANKER_ACTIVATE: the bank opens on the answer (Inventory::banker())
    FlightMaster, // CMSG_TAXIQUERYAVAILABLENODES: the taxi map (or "new flight path") of a flight master
    Mailbox,      // CMSG_GET_MAIL_LIST: the mailbox opens (Exchange::mail())
    Auctioneer,   // MSG_AUCTION_HELLO: the auction house opens on the answer (Exchange::auction())
    ReadObject,   // phase 13: a plaque or sign (GAMEOBJECT_TYPE_TEXT): its pages open, no packet sent
    Battlemaster, // phase 17 (queue): CMSG_BATTLEMASTER_HELLO (no gossip menu): the list opens on the answer
};

// The player's death: update fields plus the death packets.
struct DeathState {
    bool dead = false;         // health 0, or a ghost
    bool ghost = false;        // released (PLAYER_FLAGS_GHOST)
    bool releaseTimer = false; // the auto-release countdown runs (6 minutes on non-instance maps)
    bool corpseKnown = false;  // MSG_CORPSE_QUERY answered with a corpse
    i32 corpseMap = 0; // where to run to: the corpse's map, or the entrance's when it lies in an instance
    f32 corpseX = 0.0f, corpseY = 0.0f, corpseZ = 0.0f;
    u32 corpseRealMap = 0;
    u32 reclaimAtMs = 0; // client clock from which the corpse can be reclaimed (SMSG_CORPSE_RECLAIM_DELAY)
    u64 resurrector = 0; // a pending resurrection offer (0 = none): answerResurrect()
    std::string resurrectorName; // a creature's name; empty for players (use the name cache)
    bool resurrectSickness = false;
    u64 spiritHealer = 0; // a spirit healer waiting for confirmation: acceptSpiritHealer()
    // Parity (popups): the resurrection offer waits for the corpse delay (SMSG_RESURRECT_REQUEST's
    // `delayed`), and the self-resurrection the DEATH popup offers (PLAYER_SELF_RES_SPELL: Soulstone,
    // Reincarnation, Twisting Nether; 0 none): selfResurrect().
    bool resurrectDelayed = false;
    u32 selfResSpell = 0;
};

// One objective of a quest in the log, counts and names resolved.
struct QuestObjectiveView {
    enum class Kind : u8 { Creature, GameObject, Item };
    Kind kind = Kind::Creature;
    u32 entry = 0; // creature / game object / item entry
    u32 have = 0;  // kills / uses (log fields) or items carried (inventory), capped at `need`
    u32 need = 0;
    std::string_view name; // creature / object / item name; empty until the server answered its query
    std::string_view text; // the quest's own text for it (shown instead of "<name> slain"), may be empty
    bool done() const { return have >= need; }
};

constexpr f32 kCorpseReclaimRadius = 39.0f; // CORPSE_RECLAIM_RADIUS (Objects/Corpse.h)

// The gameplay side of the world session (phase 7): combat, spells, auras, NPC dialogs, quests, loot, chat,
// experience and death. It parses the gameplay packets (handle), keeps their state for the HUD, and sends the
// player's requests through a PacketSink. Main thread only; nothing here allocates per frame once warmed up
// (event rings and windows reuse their storage; caches grow once per new entry).
class Gameplay {
public:
    enum class Handled : u8 { No, Yes, Malformed };

    explicit Gameplay(const EntityStore& entities);

    // Wiring. `data` / `reactions` may be null (no DBCs): cooldowns then come only from the server, targets
    // look neutral. Both must outlive this object.
    void setSink(PacketSink sink);
    void setData(const GameData* data, const Reactions* reactions);
    const Reactions* reactions() const { return m_reactions; } // phase 16 (raidframe): UnitCanCooperate
    // Spell data from elsewhere than GameData (tests, tools).
    void setSpellSource(SpellSource source);

    // Session (GameSession calls these). `newLogin`: SMSG_LOGIN_VERIFY_WORLD (everything starts over); false
    // for SMSG_NEW_WORLD (another map, same character).
    void enterWorld(u64 player, u32 map, bool newLogin, u32 nowMs);
    void leaveWorld();
    bool inWorld() const { return m_inWorld; }
    u64 player() const { return m_player; }
    // Once per frame.
    void update(u32 nowMs);
    // A world packet: consumed when it is a gameplay one.
    Handled handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    // After each SMSG_UPDATE_OBJECT, with the GUIDs it created.
    void afterUpdate(std::span<const u64> created, u32 nowMs);

    // --- Targeting and melee ---
    void setTarget(u64 guid); // CMSG_SET_SELECTION
    // Ours (NPC voices): the player clicked a unit (GameEventKind::UnitClicked: its click voice).
    void unitClicked(u64 guid) { event(GameEventKind::UnitClicked, 0, guid); }
    u64 target() const { return m_target; }
    bool startAttack(u64 guid); // selects it, draws the melee weapons and swings (CMSG_ATTACKSWING)
    void stopAttack();          // CMSG_ATTACKSTOP
    // The player's sheath state (0 put away, 1 melee, 2 ranged) is the client's own, as in WoW.exe (CGUnit_C
    // +0xD40): set here at once and told to the server (CMSG_SETSHEATHED) when it differs, since the server never
    // draws a player's weapons. The client draws them when it attacks or shoots, and puts them away or draws them
    // with its animations (game::sheathAfterAnimation, app/Units).
    void setSheath(u8 state);
    u8 sheath() const;
    const MeleeState& melee() const { return m_melee; }
    const CombatLog::Ring& combatLog() const { return m_combat.events(); }

    // --- Spells and the action bar ---
    const SpellBook& spells() const { return m_spells; }
    // CMSG_CAST_SPELL at `target` (0 or the player: a self cast). False when not sent (not in the world). The
    // server answers a refusal (out of range, not ready...) with a CastFailed event, an unknown spell with
    // nothing; the HUD may grey buttons out with spells().cooldown(). Phase 13 (spells): a spell aimed at a place
    // or an item sends nothing yet: it waits for one (targeting(), the client's targeting cursor) and returns true.
    bool castSpell(u32 spellId, u64 target);
    bool castSpellOnObject(u32 spellId, u64 gameObject);
    bool castSpellAt(u32 spellId, f32 x, f32 y, f32 z); // ground-targeted spells
    // The target a tap on the spell should use: the current target when the spell wants one it can take, the
    // player itself for helpful spells on a hostile or no target, 0 for spells without a unit target.
    u64 spellTarget(u32 spellId) const;
    void cancelCast(); // the player's cast or channel
    void cancelAura(u32 spellId);
    // The action button in `slot` (0..119): casts its spell at spellTarget(), or uses its item.
    bool useAction(u32 slot);
    void setActionButton(u32 slot, ActionButton button); // CMSG_SET_ACTION_BUTTON (empty button = remove)
    // Phase 15 (options): what the server keeps of the interface options (game/GameplayOptions.cpp): the helm and the
    // cloak shown (PLAYER_FLAGS_HIDE_HELM / _HIDE_CLOAK: ShowHelm / ShowCloak send CMSG_TOGGLE_HELM / _CLOAK when the
    // wish differs, the server toggles the flag) and the extra action bars (PLAYER_FIELD_BYTES byte 2, bits 0..3 the
    // bottom left, bottom right, right and right 2 bars: SetActionBarToggles packs its first four arguments into
    // CMSG_SET_ACTIONBAR_TOGGLES). And AUTO_SELF_CAST: a helpful spell without a friendly target (a spell for any unit
    // without a target) goes on the player; off, it is not cast ("No target" / "Invalid target": 1.12's targeting
    // cursor, WoW.exe 0x6E53D7, is not kept on touch). On by default (the game sets the player's option; 1.12's default
    // is off).
    bool showingHelm() const;
    bool showingCloak() const;
    void showHelm(bool show);
    void showCloak(bool show);
    u8 actionBarToggles() const;
    void setActionBarToggles(u8 bars);
    bool autoSelfCast = true;
    // Uses the first stack of `itemEntry` in the bags: its on-use spell at `target` (0 = self), or the quest
    // it starts. False when the item or its template is not known yet (asked for: try again).
    bool useItem(u32 itemEntry, u64 target = 0);
    // Uses the item at `pos` as a tap on it does in the client: its on-use spell, the quest it starts, reading a
    // book or letter, or opening a lockbox (phase 8). Phase 13 (spells): an on-use spell aimed at a place or an
    // item (bombs, poisons, sharpening stones) waits for it (targeting()).
    bool useItemAt(ItemPos pos, u64 target = 0);

    // --- Phase 13 (spells): spells aimed at a place or an item, and the spell modifiers ---
    // The cast waiting for its place or item since castSpell / useAction / useItemAt (inactive: none). One of the
    // target* calls sends it (CMSG_CAST_SPELL, or CMSG_USE_ITEM for an item's spell) and stops waiting;
    // stopTargeting() drops it; casting another spell replaces it. The SpellTargetingStarted / Stopped events say
    // when it starts and ends, EnchantQuestion when 1.12 asks first.
    const SpellTargeting& targeting() const { return m_targeting; }
    enum class TargetResult : u8 {
        Sent,    // the cast went out (targeting stopped)
        Asked,   // an enchanting question first: targeting().question() (confirmEnchant / declineEnchant)
        Refused, // the client refuses it itself (a CastFailed event says why: Invalid target, Out of range): still
                 // waiting for another
        NotSent, // nothing waits for that, or the item is not known yet
    };
    // SpellTargetLocation: the place (the server's own range check first: CastFailed OUT_OF_RANGE / TOO_CLOSE).
    TargetResult targetLocation(f32 x, f32 y, f32 z);
    // SpellTargetItem: the player's item at `pos` (bags, worn).
    TargetResult targetItem(ItemPos pos);
    // The trader's item that will not be traded (TradeFrame's ClickTargetTradeButton(7): TARGET_FLAG_TRADE_ITEM).
    TargetResult targetTradeItem();
    // A locked object in the world (Pick Lock, Disarm Trap).
    TargetResult targetObject(u64 gameObject);
    // The question's answer: BindEnchant (and then maybe REPLACE_ENCHANT), ReplaceEnchant, ReplaceTradeEnchant.
    void confirmEnchant();
    // Its No: nothing is cast, the spell still waits for an item (as the client's cursor stays).
    void declineEnchant();
    void stopTargeting(); // SpellStopTargeting
    // Casts that do not wait (the windows that already know the item): on the player's item at `pos`, on the
    // trader's item that will not be traded.
    bool castSpellOnItem(u32 spellId, ItemPos pos);
    bool castSpellOnTradeItem(u32 spellId);
    // SMSG_SET_FLAT_SPELL_MODIFIER / SMSG_SET_PCT_SPELL_MODIFIER: the costs, ranges, cast times and cooldowns the
    // client shows and counts.
    const SpellMods& spellMods() const { return m_spellMods; }

    // --- Items, vendors, bank, books (phase 8) ---
    Inventory& inventory() { return m_inventory; }
    const Inventory& inventory() const { return m_inventory; }

    // --- Auras ---
    // The unit's visible auras in slot order; the player's own with their durations; a party member out of sight
    // from its last SMSG_PARTY_MEMBER_STATS. Returns the count written.
    u32 auras(u64 guid, std::span<AuraInfo> out) const;
    // Parity (auras): what the weapons' temporary enchantments have left (SMSG_ITEM_ENCHANT_TIME_UPDATE).
    const EnchantTimers& enchantTimers() const { return m_enchantTimers; }

    // --- NPCs ---
    // What a tap on an object does: loot a lootable corpse, attack what can be attacked, talk to NPCs
    // (gossip, quests, spirit healers), use game objects, open vendors and banks without a gossip menu.
    Interaction interact(u64 guid);
    const NpcDialog& dialog() const { return m_dialog; }
    void selectGossipOption(u32 index, std::string_view code = {});
    // A quest of the gossip / quest list menu: offered (details) or turned in (request items / offer reward).
    void selectQuest(u32 questId);
    void acceptQuest();                 // QuestDetails
    void completeQuest();               // QuestRequestItems: "Continue"
    void chooseQuestReward(u32 choice); // QuestOfferReward: index into the choices (0 when there are none)
    void closeDialog();
    void confirmSharedQuest(u32 questId); // GameEventKind::QuestShared
    // "!" / "?" of a quest giver (units with the flag, quest giver objects), None until the server answered.
    net::QuestGiverStatus questGiverStatus(u64 guid) const;
    // The NPC text of a gossip window (queried when missing: nullptr until it arrives).
    const NpcText* npcText(u32 textId);

    // --- Quest log ---
    const QuestLog& questLog() const { return m_questLog; }
    // Quest template (title, texts, objectives); queried when missing: nullptr until it arrives.
    const QuestInfo* quest(u32 questId) { return m_queries.quest(questId); }
    // The objectives of a quest in the log with their progress and names (asking for the names still
    // missing). Returns how many were written (at most 8: four creature / object ones and four items).
    u32 questObjectives(u32 questId, std::span<QuestObjectiveView> out);
    void abandonQuest(u32 questId); // CMSG_QUESTLOG_REMOVE_QUEST
    // Phase 13 (quests): the log's headers, selection, watches (QuestWatchFrame, AUTO_QUEST_WATCH) and timers.
    QuestJournal& questJournal() { return m_journal; }
    const QuestJournal& questJournal() const { return m_journal; }
    // GetQuestLogPushable and a party to share with (QuestLog_Update enables Share Quest so).
    bool canShareQuest(u32 questId) const;
    // CMSG_PUSHQUESTTOPARTY (QuestLogPushQuest): each member's answer comes as a QuestPushResult event. False when not
    // sent (see canShareQuest).
    bool shareQuest(u32 questId);

    // --- Loot ---
    const LootWindow& loot() const { return m_loot; }
    void lootItem(u8 slot); // CMSG_AUTOSTORE_LOOT_ITEM
    void lootMoney();       // CMSG_LOOT_MONEY
    void lootAll();         // the money and every item the player may take
    void closeLoot();       // CMSG_LOOT_RELEASE

    // --- Chat ---
    const ChatLog::Ring& chat() const { return m_chat.lines(); }
    // Say / yell / party / guild / officer / raid / emote in the player's language; whisper to `target` (a
    // name); a channel named `target`. A leading '.' is a GM command for a GM account. False when not sent.
    bool sendChat(net::ChatType type, std::string_view text, std::string_view target = {});
    // Who said it: the monster's name in the line, or the player's (name query: empty until it arrives).
    std::string_view senderName(const ChatLine& line);

    // --- Experience, death ---
    const net::LevelUpInfo& lastLevelUp() const { return m_levelUp; }
    // The last SMSG_QUESTGIVER_QUEST_COMPLETE (the QuestRewarded event's quest, experience, money and items).
    const net::QuestComplete& lastQuestComplete() const { return m_questComplete; }
    // Breath, fatigue and feign death bars (parity: SMSG_START / PAUSE / STOP_MIRROR_TIMER).
    const MirrorTimers& mirrorTimers() const { return m_mirrorTimers; }
    const DeathState& death() const { return m_death; }
    void releaseSpirit(); // CMSG_REPOP_REQUEST
    // CMSG_RECLAIM_CORPSE: a ghost within kCorpseReclaimRadius of its corpse, once the delay is over.
    bool canReclaimCorpse(u32 nowMs) const;
    // Until the corpse can be reclaimed (SMSG_CORPSE_RECLAIM_DELAY), 0 once it can.
    u32 reclaimDelayRemaining(u32 nowMs) const;
    void reclaimCorpse();
    void answerResurrect(bool accept);
    void acceptSpiritHealer(); // CMSG_SPIRIT_HEALER_ACTIVATE
    void selfResurrect();      // parity (popups): CMSG_SELF_RES, when death().selfResSpell is set
    void queryCorpse();        // MSG_CORPSE_QUERY
    u8 standState() const { return m_standState; }
    void setStandState(u8 state); // CMSG_STANDSTATECHANGE: 0 stand, 1 sit, 3 sleep, 8 kneel

    // --- Trainers, talents, pet, reputation, auto repeat (phase 8) ---
    Abilities& abilities() { return m_abilities; }
    const Abilities& abilities() const { return m_abilities; }
    // Phase 13 (professions): the TradeSkillFrame and the CraftFrame (game/TradeSkills); castSpell() of a profession's
    // spell opens them.
    TradeSkills& tradeSkills() { return m_tradeSkills; }
    const TradeSkills& tradeSkills() const { return m_tradeSkills; }
    // Phase 15 (pets): the stable (PetStableFrame) and a pet trainer's untraining offer (game/PetStable).
    PetStable& stable() { return m_stable; }
    const PetStable& stable() const { return m_stable; }
    // Phase 15 (character): the inspected player's honor, the watched and inactive factions, unlearning a skill.
    CharacterSheet& character() { return m_character; }
    const CharacterSheet& character() const { return m_character; }

    // --- Events and caches ---
    using Events = EventRing<GameEvent, 64>;
    const Events& events() const { return m_events; }
    // Visuals and sounds of every unit nearby (phase 9): see FxEventKind.
    using FxEvents = EventRing<FxEvent, 128>;
    const FxEvents& fx() const { return m_fx; }
    QueryCache& queries() { return m_queries; }
    const QueryCache& queries() const { return m_queries; }
    // Groups, channels, friends, who, guild, duels, AFK / DND, text emotes (phase 8).
    Social& social() { return m_social; }
    const Social& social() const { return m_social; }
    // Phase 13 (social): raid target icons, inspect, /played, /pvp, guild leader and disband, ready checks,
    // raid assistants, pet names, the channels' moderation.
    PlayerCommands& commands() { return m_commands; }
    const PlayerCommands& commands() const { return m_commands; }
    // Phase 16 (alerts): the raid's ready check (ReadyCheckFrame, the leader's answers) and the meeting stones' queue.
    ReadyCheck& readyCheck() { return m_readyCheck; }
    const ReadyCheck& readyCheck() const { return m_readyCheck; }
    MeetingStone& meetingStone() { return m_meetingStone; }
    const MeetingStone& meetingStone() const { return m_meetingStone; }
    // Phase 17 (lead): the battleground queues (GetBattlefieldStatus, AcceptBattlefieldPort, LeaveBattlefield) and the
    // world states of the player's map and zone (flags, resources, outdoor PvP).
    Battlegrounds& battlegrounds() { return m_battlegrounds; }
    const Battlegrounds& battlegrounds() const { return m_battlegrounds; }
    WorldStates& worldStates() { return m_worldStates; }
    const WorldStates& worldStates() const { return m_worldStates; }
    // The battlemaster's list (BattlefieldFrame), joining a queue, the queues' sounds, popups and lines.
    BattlefieldQueueing& battlefieldQueueing() { return m_battlefieldQueueing; }
    const BattlefieldQueueing& battlefieldQueueing() const { return m_battlefieldQueueing; }
    // GetNumWorldStateUI / GetWorldStateUIInfo (WorldStateUI.dbc over the world states), and the battleground's score,
    // columns, team positions, flag carrier and joined / left lines.
    WorldStateUi& worldStateUi() { return m_worldStateUi; }
    const WorldStateUi& worldStateUi() const { return m_worldStateUi; }
    BattlefieldScore& battlefieldScore() { return m_battlefieldScore; }
    const BattlefieldScore& battlefieldScore() const { return m_battlefieldScore; }
    // Phase 17 (honor): the battleground graveyards' spirit guide for a ghost (GetAreaSpiritHealerTime,
    // AcceptAreaSpiritHeal, CancelAreaSpiritHeal) and honor, the insignia and the defense channels' messages.
    AreaSpiritHealer& areaSpiritHealer() { return m_areaSpiritHealer; }
    const AreaSpiritHealer& areaSpiritHealer() const { return m_areaSpiritHealer; }
    const Honor& honor() const { return m_honor; }
    // The spell with SPELL_EFFECT_SKIN_PLAYER_CORPSE (116) the player knows ("Remove Insignia", 22027), 0
    // none: what a tap on an enemy's corpse or dead body casts in a battleground (game/GameplayPvp.cpp).
    u32 insigniaSpell() const;
    // Phase 18 (glue).
    // Phase 18 (create).
    // Phase 18 (p3).
    // The tutorials (TutorialFrame's TriggerTutorial / FlagTutorial / ClearTutorials / ResetTutorials) and the account
    // data kept on the server (macros, chat windows: game/AccountData).
    Tutorials& tutorials() { return m_tutorials; }
    const Tutorials& tutorials() const { return m_tutorials; }
    AccountData& accountData() { return m_accountData; }
    const AccountData& accountData() const { return m_accountData; }
    // Phase 18 (extras).
    // B4, the languages (game/Languages: LanguageWords, Languages and the spells that teach them; null: the lines stay
    // as the server sends them). A line in a language the player does not know reaches chat() garbled as WoW.exe
    // garbles it, with ChatLine::languageName for ChatFrame's "[Orcish] ".
    void setLanguageData(const LanguageData* data) { m_languageData = data; }
    const LanguageData* languageData() const { return m_languageData; }
    // The player's skill in a language (0x5EC720): 0 when it does not know it, 300 when it knows it all.
    u32 languageSkill(u32 language) const;
    // The languages the player can speak, in Languages.dbc's order (GetNumLaguages / GetLanguageByIndex, 0x49FB30 /
    // 0x49FBE0: those whose skill it has). Returns how many were written.
    u32 knownLanguages(std::span<u32> out) const;
    // The language the chat speaks in (ChatMenu's Language submenu: the edit box's language; 0 the player's own,
    // GetDefaultLanguage). sendChat falls back to the player's own when it no longer knows that one.
    void setChatLanguage(u32 language) { m_chatLanguage = language; }
    u32 chatLanguage() const { return m_chatLanguage; }
    u32 defaultLanguage() const { return playerLanguage(); } // GetDefaultLanguage: the race's own
    // A gossip text in `language` (an NPC text's variant: NpcText::language) as the player reads it (0x4E22AB: after
    // its $N... are expanded, garbled by the player's skill with what lies between words kept; no game master
    // exception there). `text` itself when understood, else `out` filled.
    std::string_view gossipText(u32 language, std::string_view text, std::string& out) const;
    // The jump key on a mount standing still (0x60DEA0): CMSG_MOUNTSPECIAL_ANIM, and the player's own mount rears
    // (FxEventKind::MountSpecial). False when not sent (not in the world, not mounted).
    bool mountSpecial();
    // Phase 15 (guild): the guild frame's roster, notes, information text and ranks; the registrar and the charter;
    // the tabard designer.
    GuildControl& guildControl() { return m_guildControl; }
    const GuildControl& guildControl() const { return m_guildControl; }
    Petitions& petitions() { return m_petitions; }
    const Petitions& petitions() const { return m_petitions; }
    TabardDesigner& tabards() { return m_tabards; }
    const TabardDesigner& tabards() const { return m_tabards; }
    // Phase 15 (help): GM tickets, the GM survey, bug reports.
    HelpDesk& help() { return m_help; }
    const HelpDesk& help() const { return m_help; }
    // Zones, area triggers, flight masters, innkeepers, far teleports (phase 8).
    Travel& travel() { return m_travel; }
    const Travel& travel() const { return m_travel; }
    // Group loot rolls, mail, the auction house and trade (phase 8).
    Exchange& exchange() { return m_exchange; }
    const Exchange& exchange() const { return m_exchange; }
    // The group's minimap pings (parity pass: MSG_MINIMAP_PING).
    Minimap& minimap() { return m_minimap; }
    const Minimap& minimap() const { return m_minimap; }
    // Parity (popups): logging out (CAMP / QUIT) and summons (CONFIRM_SUMMON).
    Logout& logout() { return m_logout; }
    const Logout& logout() const { return m_logout; }
    Summon& summon() { return m_summon; }
    const Summon& summon() const { return m_summon; }
    // Phase 11: the weather of the player's zone (SMSG_WEATHER).
    const Weather& weather() const { return m_weather; }
    // Phase 13: the player's standing with the factions (forced reactions, at war) for game::Reactions: wire it
    // with Reactions::setPlayerStanding(&standing()).
    const PlayerStanding& standing() const { return m_standing; }
    // Phase 13 (combat): who the client moves (fear, charms, Mind Control, Eyes of the Beast) and the far sight.
    MoverControl& control() { return m_control; }
    const MoverControl& control() const { return m_control; }
    // Phase 14: the cinematic the server started (SMSG_TRIGGER_CINEMATIC: a new character's intro, a camera object's
    // view), its CinematicSequences id, 0 when none plays. The client shows it and tells the server each camera after
    // the first and the end (also when the player stops it): until then the server keeps the player's view point
    // along the cinematic's path. `skipCinematics` (development, tests): ended at once, as before phase 14.
    u32 cinematic() const { return m_cinematic; }
    void nextCinematicCamera(); // CMSG_NEXT_CINEMATIC_CAMERA
    void completeCinematic();   // CMSG_COMPLETE_CINEMATIC
    bool skipCinematics = false;

private:
    const Entity* me() const { return m_entities.find(m_player); }
    bool send(net::Opcode opcode);           // empty body
    bool send(net::Opcode opcode, u64 guid); // a full GUID
    bool sendBody(net::Opcode opcode);       // m_body
    GameEvent& event(GameEventKind kind, u32 id = 0, u64 guid = 0);
    FxEvent& fx(FxEventKind kind, u64 guid, u32 id = 0, u64 other = 0);
    // SpellStart / SpellGo: the target unit and the destination of the cast.
    void fxTargets(FxEvent& e, const net::SpellCastTargets& targets);
    // An event of this kind about `id` among the last few, younger than `withinMs` (packets and field changes
    // may tell the same thing).
    bool recentEvent(GameEventKind kind, u32 id, u32 withinMs) const;
    ExecuteLogSpell executeLogSpell(u32 spellId) const; // phase 13 (combat)
    bool cast(u32 spellId, const net::SpellCastTargets& targets);
    // Phase 13 (spells): sends the waiting cast at `targets` (CMSG_USE_ITEM for an item's spell) and stops waiting.
    bool sendTargeting(const net::SpellCastTargets& targets);
    void startTargeting(const SpellInfo& spell, SpellAim aim, const InventoryItem* item, u8 itemSpellIndex);
    TargetResult refuseTarget(u32 reason); // a CastFailed event, still waiting
    void queryQuestGiverStatus(u64 guid);
    void refreshQuestGiverStatuses();
    void scheduleStatusRefresh(u32 nowMs);
    void updateDeath(const Entity& player);
    void queryItems(std::span<const net::QuestItem> items);
    u32 playerLanguage() const;
    std::string_view serverMessageText(const net::ServerMessage& m); // phase 13: m_serverText
    // Phase 13 (quests): the pages of a readable game object (a TEXT object's or a goober's page in its template).
    bool readObject(u64 guid);
    void questWatchUpdate(u32 questId); // QUEST_WATCH_UPDATE

    const EntityStore& m_entities;
    PacketSink m_sink;
    SpellSource m_spellSource;
    const Reactions* m_reactions = nullptr;
    bool m_inWorld = false;
    u64 m_player = 0;
    u32 m_map = 0;
    u64 m_target = 0;
    u32 m_nowMs = 0;
    std::vector<u8> m_body; // outgoing scratch

    MeleeState m_melee;
    u8 m_sheath = 0;      // the player's own sheath state (read from its field once it is in the world)
    bool m_sheathKnown = false;
    u8 m_sheathField = 0; // the field as last seen: a change the client did not ask for is the server's
    u8 m_sheathAsked = 0; // the last CMSG_SETSHEATHED, and when (not repeated while its field change comes)
    u32 m_sheathAskedAt = 0;
    CombatLog m_combat;
    SpellBook m_spells;
    AuraDurations m_auraDurations;
    NpcDialog m_dialog;
    QuestLog m_questLog;
    QuestJournal m_journal; // phase 13 (quests)
    LootWindow m_loot;
    ChatLog m_chat;
    QueryCache m_queries;
    Events m_events;
    FxEvents m_fx;
    Abilities m_abilities{m_entities, m_events};
    Inventory m_inventory{m_entities, m_queries, m_events}; // after them: it keeps references
    Travel m_travel{m_entities, m_events}; // after m_events: it pushes its events there
    Exchange m_exchange{m_entities, m_queries, m_events, m_inventory}; // phase 8: after what it refers to
    TradeSkills m_tradeSkills{m_entities, m_queries, m_inventory, m_spells, m_abilities}; // phase 13: after them
    PetStable m_stable{m_entities, m_events}; // phase 15 (pets): after m_events
    CharacterSheet m_character{m_entities, m_abilities}; // phase 15 (character): after m_abilities
    DeathState m_death;
    net::LevelUpInfo m_levelUp;
    net::QuestComplete m_questComplete;
    MirrorTimers m_mirrorTimers;
    u8 m_standState = 0;
    std::unordered_map<u64, net::QuestGiverStatus> m_questGiverStatus;
    bool m_statusRefreshPending = false;
    u32 m_statusRefreshAt = 0;
    u32 m_statusPruneAt = 0;
    bool m_moneyRequested = false; // CMSG_LOOT_MONEY sent
    // The money was taken: the share comes as SMSG_LOOT_MONEY_NOTIFY (groups) or a coinage change (solo).
    bool m_expectMoney = false;
    u32 m_expectMoneyMs = 0;
    u32 m_coinage = 0;
    Social m_social{m_entities, m_queries}; // after m_queries: it keeps a reference
    PlayerCommands m_commands{m_entities, m_social, m_events}; // phase 13 (social): after what it refers to
    ReadyCheck m_readyCheck{m_social, m_events};                // phase 16 (alerts): after what it refers to
    MeetingStone m_meetingStone{m_entities, m_social, m_queries, m_events};
    Battlegrounds m_battlegrounds{m_travel, m_events}; // phase 17 (lead): after what it refers to
    WorldStates m_worldStates{m_events};
    // Phase 17 (queue).
    BattlefieldQueueing m_battlefieldQueueing{m_entities, m_social, m_travel, m_battlegrounds, m_events};
    // Phase 17 (scores).
    WorldStateUi m_worldStateUi{m_worldStates, m_social, m_events}; // after what it refers to
    BattlefieldScore m_battlefieldScore{m_entities, m_social, m_queries, m_battlegrounds, m_events};
    // Phase 17 (honor).
    AreaSpiritHealer m_areaSpiritHealer{m_entities, m_events};
    Honor m_honor{m_entities, m_travel, m_social, m_chat, m_events}; // after what it refers to
    // Phase 18 (glue).
    // Phase 18 (create).
    // Phase 18 (p3).
    Tutorials m_tutorials{m_entities, m_events};
    AccountData m_accountData;
    // Phase 18 (extras): the languages, the small packets (game/GameplayExtras.cpp).
    const LanguageData* m_languageData = nullptr;
    std::string m_heardText; // a chat line as the player reads it
    u32 m_chatLanguage = 0;  // setChatLanguage
    bool languageSkillOf(u32 language, u32& skill) const; // 0x5EC720: false when the player has none
    u32 speakingLanguage() const;                          // what sendChat speaks
    static bool handlesExtras(net::Opcode opcode);
    Handled handleExtras(net::Opcode opcode, std::span<const u8> p, u32 nowMs);
    void addChatMessage(const net::ChatMessage& m, u32 nowMs);
    // Taps (game/GameplayPvp.cpp): a spirit guide without a gossip menu (0x5DF950), Remove Insignia on an
    // enemy's corpse or dead body (0x5D6C96, 0x60C01F). None when they do not apply.
    Interaction interactSpiritGuide(const Entity& unit);
    Interaction removeInsignia(const Entity& target);
    GuildControl m_guildControl{m_social};                      // phase 15 (guild): after what it refers to
    Petitions m_petitions{m_entities, m_social, m_inventory};
    TabardDesigner m_tabards{m_entities, m_social};
    HelpDesk m_help; // phase 15 (help)
    Minimap m_minimap; // parity pass
    EnchantTimers m_enchantTimers; // parity (auras)
    Logout m_logout{m_events}; // parity (popups)
    Summon m_summon{m_events};
    Weather m_weather; // phase 11
    PlayerStanding m_standing; // phase 13
    std::string m_serverText;
    // Phase 13 (spells).
    const GameData* m_gameData = nullptr; // also ServerMessages.dbc (13.9)
    SpellMods m_spellMods;
    SpellTargeting m_targeting;
    MoverControl m_control{m_entities, m_events}; // phase 13 (combat)

    // Parsed messages, reused (their vectors keep their capacity).
    net::AttackerStateUpdate m_attackerState;
    net::SpellDamageLog m_spellDamage;
    net::PeriodicAuraLog m_periodic;
    net::SpellLogMiss m_logMiss;
    net::SpellLogExecute m_logExecute; // phase 13 (combat)
    net::InitialSpells m_initialSpells;
    net::SpellStart m_spellStart;
    net::SpellGo m_spellGo;
    net::SpellCooldown m_spellCooldown;
    net::GossipMessage m_gossip;
    net::NpcTextUpdate m_npcTextUpdate;
    net::QuestGiverQuestList m_questList;
    net::QuestDetails m_questDetails;
    net::QuestRequestItems m_requestItems;
    net::QuestOfferReward m_offerReward;
    net::QuestQueryResponse m_questQuery;
    net::LootResponse m_lootResponse;
    net::ChatMessage m_chatMessage;
    net::GameObjectQueryInfo m_gameObjectQuery;
    net::ServerMessage m_serverMessage; // phase 13
    std::vector<net::ForcedReaction> m_forcedReactions;
    u32 m_cinematic = 0; // phase 14
};

} // namespace vd::game
