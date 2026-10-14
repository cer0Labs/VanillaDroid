#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/GameEvents.h"
#include "net/GameMessages.h"

#include <array>
#include <string>

namespace vd::game {

constexpr u32 kQuestLogSize = 20;        // MAX_QUEST_LOG_SIZE
constexpr u8 kQuestStateComplete = 0x01; // every objective met
constexpr u8 kQuestStateFailed = 0x02;
constexpr u32 kQuestObjectiveGameObject = 0x80000000u; // objective entries of game objects carry this bit

// A quest template (SMSG_QUEST_QUERY_RESPONSE), cached for the session.
struct QuestInfo {
    struct Objective {
        u32 creatureOrGo = 0; // creature entry, or game object entry | kQuestObjectiveGameObject; 0 = none
        u32 creatureOrGoCount = 0;
        u32 itemId = 0;
        u32 itemCount = 0;
        std::string text; // replaces the creature's name in "X slain: 2/5" when not empty
    };
    u32 id = 0;
    u32 method = 0;     // 0 autocomplete, 2 deliver
    u32 level = 0;      // 0 = the player's level
    i32 zoneOrSort = 0; // > 0 AreaTable id, < 0 QuestSort id
    u32 type = 0;       // QuestInfo.dbc: 1 elite, 41 PvP, 62 raid, 81 dungeon
    u32 flags = 0;
    u32 nextQuestInChain = 0;
    i32 money = 0;
    u32 rewardSpell = 0;
    u32 sourceItemId = 0;
    std::array<net::QuestItem, net::kQuestRewardCount> rewards{};
    std::array<net::QuestItem, net::kQuestChoiceCount> choices{};
    std::string title, objectives, details, endText;
    std::array<Objective, net::kQuestObjectiveCount> objectiveList{};

    void assign(const net::QuestQueryResponse& m);
};

// One slot of the quest log, decoded from PLAYER_QUEST_LOG_<slot+1>_* (Objects/Player.h).
struct QuestLogEntry {
    u32 questId = 0; // 0 = empty slot
    std::array<u8, 4>
        counters{}; // kills / uses of the creature and game object objectives, by objective index
    u8 state = 0;   // kQuestStateComplete / kQuestStateFailed
    u32 timer = 0;  // unix time when a timed quest runs out (0 = untimed)

    bool complete() const { return (state & kQuestStateComplete) != 0; }
    bool failed() const { return (state & kQuestStateFailed) != 0; }
    bool operator==(const QuestLogEntry&) const = default;
};

// The player's quest log, decoded from its update fields. Item objectives are not in the fields reliably (not
// restored at login, never decremented): count those from the inventory (Gameplay::questObjectives does).
class QuestLog {
public:
    // Decodes the player's fields and reports what changed as emit(GameEventKind, questId): QuestAccepted,
    // QuestRemoved, QuestObjectivesDone, QuestFailed (the first call after reset() only records a baseline).
    // True if anything changed.
    template <typename Emit> bool refresh(const Entity& player, Emit&& emit);
    // The first decode after reset() is still to come.
    bool initialized() const { return m_initialized; }
    void reset() {
        m_initialized = false;
        m_slots.fill(QuestLogEntry{});
    }

    const std::array<QuestLogEntry, kQuestLogSize>& slots() const { return m_slots; }
    // The slot holding `questId`, -1 if none.
    i32 slotOf(u32 questId) const;
    const QuestLogEntry* find(u32 questId) const;
    u32 count() const;

private:
    static QuestLogEntry decode(const Entity& player, u32 slot);

    std::array<QuestLogEntry, kQuestLogSize> m_slots{};
    bool m_initialized = false;
};

// -------------------------------------------------------------------------------------------------------

template <typename Emit> bool QuestLog::refresh(const Entity& player, Emit&& emit) {
    std::array<QuestLogEntry, kQuestLogSize> next;
    for (u32 slot = 0; slot < kQuestLogSize; ++slot) {
        next[slot] = decode(player, slot);
    }
    if (!m_initialized) {
        m_slots = next;
        m_initialized = true;
        return true;
    }
    if (next == m_slots) {
        return false;
    }
    auto findIn = [](const std::array<QuestLogEntry, kQuestLogSize>& slots,
                     u32 questId) -> const QuestLogEntry* {
        for (const QuestLogEntry& q : slots) {
            if (q.questId == questId) {
                return &q;
            }
        }
        return nullptr;
    };
    // By quest id, not slot: CMSG_QUESTLOG_SWAP_QUEST moves quests between slots.
    for (const QuestLogEntry& before : m_slots) {
        if (before.questId != 0 && !findIn(next, before.questId)) {
            emit(GameEventKind::QuestRemoved, before.questId);
        }
    }
    for (const QuestLogEntry& now : next) {
        if (now.questId == 0) {
            continue;
        }
        const QuestLogEntry* before = findIn(m_slots, now.questId);
        if (!before) {
            emit(GameEventKind::QuestAccepted, now.questId);
            if (now.complete()) {
                emit(GameEventKind::QuestObjectivesDone,
                     now.questId); // nothing to do (items already at hand...)
            }
            continue;
        }
        if (now.complete() && !before->complete()) {
            emit(GameEventKind::QuestObjectivesDone, now.questId);
        }
        if (now.failed() && !before->failed()) {
            emit(GameEventKind::QuestFailed, now.questId);
        }
    }
    m_slots = next;
    return true;
}

} // namespace vd::game
