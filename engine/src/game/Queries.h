#pragma once

#include "core/Types.h"
#include "game/GameEvents.h"
#include "game/Quests.h"
#include "net/GameMessages.h"
#include "net/Messages.h"

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vd::game {

// A game object template (SMSG_GAMEOBJECT_QUERY_RESPONSE).
struct GameObjectInfo {
    u32 entry = 0;
    bool found = false; // false: unknown to the server
    u32 type = 0;       // GameobjectTypes (kGameObjectChest...)
    u32 displayId = 0;
    std::string name;
    std::array<u32, 24> data{}; // by type: CHEST data1 = loot id, data8 = quest; GOOBER data1 = quest...
};

// An NPC's greeting text (SMSG_NPC_TEXT_UPDATE): up to 8 variants.
struct NpcText {
    struct Option {
        f32 probability = 0.0f;
        std::string male;   // npc_text text?_0: shown to male characters
        std::string female; // text?_1: shown to female characters
        u32 language = 0;
        std::array<u32, 3> emotes{};
        std::array<u32, 3> emoteDelays{};
    };
    u32 id = 0;
    std::array<Option, 8> options{};

    // The variant to show a player of `gender` (0 male, 1 female): the most probable one with text (the
    // client rolls; this is stable). The other gender's text when this one is empty.
    std::string_view text(u8 gender) const;
    // Phase 18 (extras): that variant's language (Languages.dbc; 0 Universal): the gossip shows it garbled to a player
    // who does not speak it (WoW.exe 0x4E22AB).
    u32 language() const;

private:
    const Option* shown() const;
};

// Phase 16: a player's race, gender and class from its name query (SMSG_NAME_QUERY_RESPONSE). Names that came some
// other way (the group list, the chat) leave it unknown until a query answers.
struct PlayerInfo {
    u8 race = 0, gender = 0, cls = 0;
};

// What the server describes on request (items, player names, creatures, game objects, NPC texts, quests),
// cached for the whole session. A lookup that misses sends the query once and returns nullptr until the
// answer arrives; version() changes with every answer so callers know when to look again. Returned pointers
// stay valid for the whole session (entries are never erased; an answer to a repeated query updates its entry
// in place).
class QueryCache {
public:
    void setSink(PacketSink sink) { m_sink = sink; }
    void setOnline(bool online) { m_online = online; }
    // A new session: queries still unanswered are sent again when asked for.
    void forgetRequests();
    u32 version() const { return m_version; }

    const net::ItemQueryInfo* item(u32 entry);
    const std::string* playerName(u64 guid);
    // Phase 16: asks (CMSG_NAME_QUERY, once) when only the name is known; nullptr until the answer.
    const PlayerInfo* playerInfo(u64 guid);
    const net::CreatureQueryInfo* creature(u32 entry);
    const GameObjectInfo* gameObject(u32 entry, u64 guid = 0);
    const NpcText* npcText(u32 textId, u64 npc);
    const QuestInfo* quest(u32 questId);
    // Phase 18 (extras): an item's name only, as 1.12's tooltip asks for an item set's pieces (0x52DF55:
    // CMSG_ITEM_NAME_QUERY once, itemnamecache.wdb): the template's own when it is known; nullptr until the answer.
    const std::string* itemName(u32 entry);
    // SMSG_INVALIDATE_PLAYER (a rename): a name known is asked for again; the old one stays until the answer.
    void invalidatePlayer(u64 guid);
    // Without asking.
    const net::ItemQueryInfo* findItem(u32 entry) const;
    const std::string* findPlayerName(u64 guid) const;
    const PlayerInfo* findPlayerInfo(u64 guid) const;
    const net::CreatureQueryInfo* findCreature(u32 entry) const;
    const GameObjectInfo* findGameObject(u32 entry) const;
    const NpcText* findNpcText(u32 textId) const;
    const QuestInfo* findQuest(u32 questId) const;

    // Answers.
    void onItem(net::ItemQueryInfo&& info);
    void onName(net::NameQueryInfo&& info);
    void onCreature(net::CreatureQueryInfo&& info);
    void onGameObject(const net::GameObjectQueryInfo& info);
    void onNpcText(const net::NpcTextUpdate& text);
    void onQuest(const net::QuestQueryResponse& quest);
    void onItemName(u32 entry, std::string_view name); // phase 18 (extras): SMSG_ITEM_NAME_QUERY_RESPONSE

private:
    enum class Kind : u8 { Item = 1, Creature = 2, GameObject = 3, NpcText = 4, Quest = 5, ItemName = 6 };
    static u64 key(Kind kind, u32 id) { return (static_cast<u64>(kind) << 56) | id; }
    // True when the query should go out now (first time, online).
    bool ask(Kind kind, u32 id);

    PacketSink m_sink;
    bool m_online = false;
    u32 m_version = 0;
    std::vector<u8> m_body;
    std::unordered_map<u32, net::ItemQueryInfo> m_items;
    std::unordered_map<u64, std::string> m_names;
    std::unordered_map<u64, PlayerInfo> m_playerInfos; // phase 16: only from name query answers
    std::unordered_map<u32, net::CreatureQueryInfo> m_creatures;
    std::unordered_map<u32, GameObjectInfo> m_gameObjects;
    std::unordered_map<u32, NpcText> m_npcTexts;
    std::unordered_map<u32, QuestInfo> m_quests;
    std::unordered_map<u32, std::string> m_itemNames; // phase 18 (extras)
    std::unordered_set<u64> m_pending;      // item / creature / ... requests in flight (key())
    std::unordered_set<u64> m_pendingNames; // player GUIDs asked for
};

} // namespace vd::game
