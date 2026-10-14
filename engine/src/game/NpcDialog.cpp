#include "game/NpcDialog.h"

namespace vd::game {

bool turnsIn(net::QuestGiverStatus icon) {
    switch (icon) {
    case net::QuestGiverStatus::Incomplete:
    case net::QuestGiverStatus::RewardRep:
    case net::QuestGiverStatus::RewardOld:
    case net::QuestGiverStatus::Reward: return true;
    default: return false;
    }
}

void NpcDialog::setQuests(const net::GossipMessage::Quest* quests, u32 count) {
    m_questCount = count;
    for (u32 i = 0; i < count; ++i) {
        QuestMenuEntry& q = m_quests[i];
        q.questId = quests[i].questId;
        q.icon = static_cast<net::QuestGiverStatus>(quests[i].icon);
        q.level = quests[i].level;
        q.title.assign(quests[i].title);
    }
}

void NpcDialog::onGossipMessage(const net::GossipMessage& m) {
    m_kind = DialogKind::Gossip;
    m_npc = m.source;
    m_textId = m.textId;
    m_greeting.clear();
    m_optionCount = m.optionCount;
    for (u32 i = 0; i < m.optionCount; ++i) {
        GossipOption& o = m_options[i];
        o.index = m.options[i].index;
        o.icon = m.options[i].icon;
        o.coded = m.options[i].coded;
        o.text.assign(m.options[i].text);
    }
    setQuests(m.quests.data(), m.questCount);
}

void NpcDialog::onQuestList(const net::QuestGiverQuestList& m) {
    m_kind = DialogKind::QuestList;
    m_npc = m.questGiver;
    m_textId = 0;
    m_greeting.assign(m.greeting);
    m_optionCount = 0;
    setQuests(m.quests.data(), m.questCount);
}

void NpcDialog::onQuestDetails(const net::QuestDetails& m) {
    m_kind = DialogKind::QuestDetails;
    m_npc = m.questGiver;
    QuestDialog& q = m_quest;
    q.questId = m.questId;
    q.title.assign(m.title);
    q.text.assign(m.details);
    q.objectives.assign(m.objectives);
    q.choiceCount = m.choiceCount;
    q.choices = m.choices;
    q.rewardCount = m.rewardCount;
    q.rewards = m.rewards;
    q.requiredCount = 0;
    q.money = m.money;
    q.requiredMoney = 0;
    q.rewardSpell = m.rewardSpell;
    q.flags = 0;
    q.completable = false;
    q.closeOnCancel = false;
    q.activateAccept = m.activateAccept;
}

void NpcDialog::onRequestItems(const net::QuestRequestItems& m) {
    m_kind = DialogKind::QuestRequestItems;
    m_npc = m.questGiver;
    QuestDialog& q = m_quest;
    q.questId = m.questId;
    q.title.assign(m.title);
    q.text.assign(m.text);
    q.objectives.clear();
    q.choiceCount = 0;
    q.rewardCount = 0;
    q.requiredCount = m.itemCount;
    q.required = m.items;
    q.money = 0;
    q.requiredMoney = m.requiredMoney;
    q.rewardSpell = 0;
    q.flags = 0;
    q.completable = m.completable;
    q.closeOnCancel = m.closeOnCancel;
    q.activateAccept = false;
}

void NpcDialog::onOfferReward(const net::QuestOfferReward& m) {
    m_kind = DialogKind::QuestOfferReward;
    m_npc = m.questGiver;
    QuestDialog& q = m_quest;
    q.questId = m.questId;
    q.title.assign(m.title);
    q.text.assign(m.text);
    q.objectives.clear();
    q.choiceCount = m.choiceCount;
    q.choices = m.choices;
    q.rewardCount = m.rewardCount;
    q.rewards = m.rewards;
    q.requiredCount = 0;
    q.money = m.money;
    q.requiredMoney = 0;
    q.rewardSpell = m.rewardSpell;
    q.flags = m.flags;
    q.completable = true;
    q.closeOnCancel = false;
    q.activateAccept = false;
}

void NpcDialog::close() {
    m_kind = DialogKind::None;
    m_npc = 0;
}

} // namespace vd::game
