#pragma once

#include "core/Types.h"
#include "net/GameMessages.h"

#include <array>
#include <span>
#include <string>

namespace vd::game {

// Which NPC window is open.
enum class DialogKind : u8 {
    None,
    Gossip,            // options + quests (SMSG_GOSSIP_MESSAGE)
    QuestList,         // a greeting + quests (SMSG_QUESTGIVER_QUEST_LIST)
    QuestDetails,      // a quest offered: Accept (SMSG_QUESTGIVER_QUEST_DETAILS)
    QuestRequestItems, // a quest being turned in: Continue when completable (SMSG_QUESTGIVER_REQUEST_ITEMS)
    QuestOfferReward,  // the reward: choose one, Complete (SMSG_QUESTGIVER_OFFER_REWARD)
};

struct GossipOption {
    u32 index = 0; // CMSG_GOSSIP_SELECT_OPTION
    // GossipOptionIcon: 0 chat, 1 vendor, 2 taxi, 3 trainer, 4/5 interact, 6 money bag, 7 talk...
    u8 icon = 0;
    bool coded = false; // asks for a code
    std::string text;
};

struct QuestMenuEntry {
    u32 questId = 0;
    net::QuestGiverStatus icon = net::QuestGiverStatus::None; // Available / Incomplete / RewardRep...
    u32 level = 0;                                            // 0 = the player's level
    std::string title;
};

// The quest window (details, request items or offer reward).
struct QuestDialog {
    u32 questId = 0;
    std::string title;
    std::string text;       // details / request-items / offer-reward text (placeholders: see expandText)
    std::string objectives; // details only
    u32 choiceCount = 0;    // rewards to choose one from
    std::array<net::QuestItem, net::kQuestChoiceCount> choices{};
    u32 rewardCount = 0; // rewards given anyway
    std::array<net::QuestItem, net::kQuestRewardCount> rewards{};
    u32 requiredCount = 0; // request items: what must be handed in
    std::array<net::QuestItem, net::kQuestObjectiveCount> required{};
    i32 money = 0;         // reward in copper (negative: required)
    u32 requiredMoney = 0; // request items
    u32 rewardSpell = 0;
    u32 flags = 0;              // offer reward: quest flags
    bool completable = false;   // request items: "Continue" enabled
    bool closeOnCancel = false; // request items: cancelling closes the whole window
    bool activateAccept = true; // details: "Accept" shown
};

// The NPC conversation window as the server drives it: every packet replaces what is shown;
// SMSG_GOSSIP_COMPLETE (or the player) closes it. Strings keep their capacity from one window to the next.
class NpcDialog {
public:
    DialogKind kind() const { return m_kind; }
    bool open() const { return m_kind != DialogKind::None; }
    u64 npc() const { return m_npc; } // the creature, game object, item or player it comes from
    // Gossip: the npc_text to show (Gameplay::npcText); 0xFFFFFF = the default one.
    u32 textId() const { return m_textId; }
    const std::string& greeting() const { return m_greeting; } // QuestList
    std::span<const GossipOption> options() const { return {m_options.data(), m_optionCount}; }
    std::span<const QuestMenuEntry> quests() const { return {m_quests.data(), m_questCount}; }
    const QuestDialog& quest() const { return m_quest; }

    void onGossipMessage(const net::GossipMessage& m);
    void onQuestList(const net::QuestGiverQuestList& m);
    void onQuestDetails(const net::QuestDetails& m);
    void onRequestItems(const net::QuestRequestItems& m);
    void onOfferReward(const net::QuestOfferReward& m);
    void close();

private:
    void setQuests(const net::GossipMessage::Quest* quests, u32 count);

    DialogKind m_kind = DialogKind::None;
    u64 m_npc = 0;
    u32 m_textId = 0;
    std::string m_greeting;
    std::array<GossipOption, net::GossipMessage::kMaxItems> m_options{};
    u32 m_optionCount = 0;
    std::array<QuestMenuEntry, net::GossipMessage::kMaxItems> m_quests{};
    u32 m_questCount = 0;
    QuestDialog m_quest;
};

// Quest menu entries of quests the player already has (or that complete on the spot) are turned in
// (CMSG_QUESTGIVER_COMPLETE_QUEST); the others are offered (CMSG_QUESTGIVER_QUERY_QUEST).
bool turnsIn(net::QuestGiverStatus icon);

} // namespace vd::game
