#include "game/Quests.h"

#include "net/UpdateFields.gen.h"

namespace vd::game {

void QuestInfo::assign(const net::QuestQueryResponse& m) {
    id = m.questId;
    method = m.method;
    level = m.level;
    zoneOrSort = m.zoneOrSort;
    type = m.type;
    flags = m.flags;
    nextQuestInChain = m.nextQuestInChain;
    money = m.money;
    rewardSpell = m.rewardSpell;
    sourceItemId = m.sourceItemId;
    rewards = m.rewards;
    choices = m.choices;
    title.assign(m.title);
    objectives.assign(m.objectives);
    details.assign(m.details);
    endText.assign(m.endText);
    for (u32 i = 0; i < net::kQuestObjectiveCount; ++i) {
        const net::QuestQueryResponse::Objective& from = m.objectiveList[i];
        Objective& o = objectiveList[i];
        o.creatureOrGo = from.creatureOrGo;
        o.creatureOrGoCount = from.creatureOrGoCount;
        o.itemId = from.itemId;
        o.itemCount = from.itemCount;
        o.text.assign(from.text);
    }
}

QuestLogEntry QuestLog::decode(const Entity& player, u32 slot) {
    // Three fields a slot: quest id; four 6-bit counters + the state byte; the timer.
    const u16 base = static_cast<u16>(net::uf::PLAYER_QUEST_LOG_1_1 + slot * 3);
    QuestLogEntry q;
    q.questId = player.value(base);
    if (q.questId == 0) {
        return q;
    }
    const u32 counters = player.value(static_cast<u16>(base + 1));
    for (u32 i = 0; i < 4; ++i) {
        q.counters[i] = static_cast<u8>((counters >> (i * 6)) & 0x3F);
    }
    q.state = static_cast<u8>(counters >> 24);
    q.timer = player.value(static_cast<u16>(base + 2));
    return q;
}

i32 QuestLog::slotOf(u32 questId) const {
    if (questId == 0) {
        return -1;
    }
    for (u32 slot = 0; slot < kQuestLogSize; ++slot) {
        if (m_slots[slot].questId == questId) {
            return static_cast<i32>(slot);
        }
    }
    return -1;
}

const QuestLogEntry* QuestLog::find(u32 questId) const {
    const i32 slot = slotOf(questId);
    return slot >= 0 ? &m_slots[static_cast<u32>(slot)] : nullptr;
}

u32 QuestLog::count() const {
    u32 n = 0;
    for (const QuestLogEntry& q : m_slots) {
        n += q.questId != 0 ? 1 : 0;
    }
    return n;
}

} // namespace vd::game
