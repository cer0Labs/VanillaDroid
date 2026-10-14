#include "game/Queries.h"

#include "net/ExtrasMessages.h" // phase 18 (extras)

namespace vd::game {

const NpcText::Option* NpcText::shown() const {
    const Option* best = nullptr;
    for (const Option& o : options) {
        if ((o.male.empty() && o.female.empty()) || (best && o.probability <= best->probability)) {
            continue;
        }
        best = &o;
    }
    return best;
}

u32 NpcText::language() const {
    const Option* best = shown();
    return best ? best->language : 0;
}

std::string_view NpcText::text(u8 gender) const {
    const Option* best = shown();
    if (!best) {
        return {};
    }
    const std::string& own = gender == 1 ? best->female : best->male;
    return own.empty() ? std::string_view(gender == 1 ? best->male : best->female) : std::string_view(own);
}

void QueryCache::forgetRequests() {
    m_pending.clear();
    m_pendingNames.clear();
}

bool QueryCache::ask(Kind kind, u32 id) {
    return m_online && m_sink.send && m_pending.insert(key(kind, id)).second;
}

const net::ItemQueryInfo* QueryCache::item(u32 entry) {
    if (entry == 0) {
        return nullptr;
    }
    if (const auto it = m_items.find(entry); it != m_items.end()) {
        return &it->second;
    }
    if (ask(Kind::Item, entry)) {
        m_body.clear();
        net::writeEntryQuery(m_body, entry, 0);
        m_sink(net::Opcode::CMSG_ITEM_QUERY_SINGLE, m_body);
    }
    return nullptr;
}

const std::string* QueryCache::itemName(u32 entry) {
    if (entry == 0) {
        return nullptr;
    }
    if (const auto it = m_items.find(entry); it != m_items.end() && it->second.found) {
        return &it->second.name;
    }
    if (const auto it = m_itemNames.find(entry); it != m_itemNames.end()) {
        return &it->second;
    }
    if (ask(Kind::ItemName, entry)) {
        m_body.clear();
        net::writeItemNameQuery(m_body, entry);
        m_sink(net::Opcode::CMSG_ITEM_NAME_QUERY, m_body);
    }
    return nullptr;
}

void QueryCache::onItemName(u32 entry, std::string_view name) {
    m_pending.erase(key(Kind::ItemName, entry));
    m_itemNames[entry].assign(name);
    ++m_version;
}

void QueryCache::invalidatePlayer(u64 guid) {
    // WoW.exe 0x555600 drops the name from its cache (0x560170): the next look asks again. Here the old name stays
    // (pointers to it stay valid) and is asked for again at once; the answer replaces it in place.
    if (guid == 0 || m_names.find(guid) == m_names.end()) {
        return;
    }
    m_pendingNames.erase(guid);
    if (m_online && m_sink.send && m_pendingNames.insert(guid).second) {
        m_body.clear();
        net::writeGuid(m_body, guid);
        m_sink(net::Opcode::CMSG_NAME_QUERY, m_body);
    }
}

const std::string* QueryCache::playerName(u64 guid) {
    if (guid == 0) {
        return nullptr;
    }
    if (const auto it = m_names.find(guid); it != m_names.end()) {
        return &it->second;
    }
    if (m_online && m_sink.send && m_pendingNames.insert(guid).second) {
        m_body.clear();
        net::writeGuid(m_body, guid);
        m_sink(net::Opcode::CMSG_NAME_QUERY, m_body);
    }
    return nullptr;
}

const PlayerInfo* QueryCache::playerInfo(u64 guid) {
    if (guid == 0) {
        return nullptr;
    }
    if (const auto it = m_playerInfos.find(guid); it != m_playerInfos.end()) {
        return &it->second;
    }
    if (m_online && m_sink.send && m_pendingNames.insert(guid).second) {
        m_body.clear();
        net::writeGuid(m_body, guid);
        m_sink(net::Opcode::CMSG_NAME_QUERY, m_body);
    }
    return nullptr;
}

const net::CreatureQueryInfo* QueryCache::creature(u32 entry) {
    if (entry == 0) {
        return nullptr;
    }
    if (const auto it = m_creatures.find(entry); it != m_creatures.end()) {
        return &it->second;
    }
    if (ask(Kind::Creature, entry)) {
        m_body.clear();
        net::writeEntryQuery(m_body, entry, 0);
        m_sink(net::Opcode::CMSG_CREATURE_QUERY, m_body);
    }
    return nullptr;
}

const GameObjectInfo* QueryCache::gameObject(u32 entry, u64 guid) {
    if (entry == 0) {
        return nullptr;
    }
    if (const auto it = m_gameObjects.find(entry); it != m_gameObjects.end()) {
        return &it->second;
    }
    if (ask(Kind::GameObject, entry)) {
        m_body.clear();
        net::writeEntryQuery(m_body, entry, guid);
        m_sink(net::Opcode::CMSG_GAMEOBJECT_QUERY, m_body);
    }
    return nullptr;
}

const NpcText* QueryCache::npcText(u32 textId, u64 npc) {
    if (const auto it = m_npcTexts.find(textId); it != m_npcTexts.end()) {
        return &it->second;
    }
    if (ask(Kind::NpcText, textId)) {
        m_body.clear();
        net::writeNpcTextQuery(m_body, textId, npc);
        m_sink(net::Opcode::CMSG_NPC_TEXT_QUERY, m_body);
    }
    return nullptr;
}

const QuestInfo* QueryCache::quest(u32 questId) {
    if (questId == 0) {
        return nullptr;
    }
    if (const auto it = m_quests.find(questId); it != m_quests.end()) {
        return &it->second;
    }
    if (ask(Kind::Quest, questId)) { // an unknown quest gets no answer: asked once a session
        m_body.clear();
        net::writeU32(m_body, questId);
        m_sink(net::Opcode::CMSG_QUEST_QUERY, m_body);
    }
    return nullptr;
}

const net::ItemQueryInfo* QueryCache::findItem(u32 entry) const {
    const auto it = m_items.find(entry);
    return it == m_items.end() ? nullptr : &it->second;
}

const std::string* QueryCache::findPlayerName(u64 guid) const {
    const auto it = m_names.find(guid);
    return it == m_names.end() ? nullptr : &it->second;
}

const PlayerInfo* QueryCache::findPlayerInfo(u64 guid) const {
    const auto it = m_playerInfos.find(guid);
    return it == m_playerInfos.end() ? nullptr : &it->second;
}

const net::CreatureQueryInfo* QueryCache::findCreature(u32 entry) const {
    const auto it = m_creatures.find(entry);
    return it == m_creatures.end() ? nullptr : &it->second;
}

const GameObjectInfo* QueryCache::findGameObject(u32 entry) const {
    const auto it = m_gameObjects.find(entry);
    return it == m_gameObjects.end() ? nullptr : &it->second;
}

const NpcText* QueryCache::findNpcText(u32 textId) const {
    const auto it = m_npcTexts.find(textId);
    return it == m_npcTexts.end() ? nullptr : &it->second;
}

const QuestInfo* QueryCache::findQuest(u32 questId) const {
    const auto it = m_quests.find(questId);
    return it == m_quests.end() ? nullptr : &it->second;
}

void QueryCache::onItem(net::ItemQueryInfo&& info) {
    m_pending.erase(key(Kind::Item, info.entry));
    m_items[info.entry] = std::move(info);
    ++m_version;
}

void QueryCache::onName(net::NameQueryInfo&& info) {
    m_pendingNames.erase(info.guid);
    if (info.cls != 0) { // a real answer (names from the group list or the chat carry no class)
        m_playerInfos[info.guid] = {static_cast<u8>(info.race), static_cast<u8>(info.gender), static_cast<u8>(info.cls)};
    }
    m_names[info.guid] = std::move(info.name);
    ++m_version;
}

void QueryCache::onCreature(net::CreatureQueryInfo&& info) {
    m_pending.erase(key(Kind::Creature, info.entry));
    m_creatures[info.entry] = std::move(info);
    ++m_version;
}

void QueryCache::onGameObject(const net::GameObjectQueryInfo& info) {
    m_pending.erase(key(Kind::GameObject, info.entry));
    GameObjectInfo& go = m_gameObjects[info.entry];
    go.entry = info.entry;
    go.found = info.found;
    go.type = info.type;
    go.displayId = info.displayId;
    go.name.assign(info.name);
    go.data = info.data;
    ++m_version;
}

void QueryCache::onNpcText(const net::NpcTextUpdate& text) {
    m_pending.erase(key(Kind::NpcText, text.textId));
    NpcText& t = m_npcTexts[text.textId];
    t.id = text.textId;
    for (u32 i = 0; i < t.options.size(); ++i) {
        const net::NpcTextUpdate::Option& from = text.options[i];
        NpcText::Option& o = t.options[i];
        o.probability = from.probability;
        o.male.assign(from.maleText);
        o.female.assign(from.femaleText);
        o.language = from.language;
        o.emotes = from.emotes;
        o.emoteDelays = from.emoteDelays;
    }
    ++m_version;
}

void QueryCache::onQuest(const net::QuestQueryResponse& quest) {
    m_pending.erase(key(Kind::Quest, quest.questId));
    m_quests[quest.questId].assign(quest);
    ++m_version;
}

} // namespace vd::game
