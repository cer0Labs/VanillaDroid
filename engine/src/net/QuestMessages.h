#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

// Phase 13 (quests): sharing a quest with the party and the living world objects, as vmangos-core 4641790
// reads and writes them for build 5875 (Server/Packets/Quest: PushQuestToParty, QuestPushResult,
// QuestPushResultResponse; Handlers/QuestHandler.cpp HandlePushQuestToParty / HandleQuestPushResult;
// Objects/GameObject.cpp SendGameObjectCustomAnim, SendGameObjectReset; Objects/Object.cpp
// SendObjectSpawnAnim / SendObjectDeSpawnAnim). Same rules as net/GameMessages: parsers return false on a
// malformed packet and never read past its end.
namespace vd::net {

// QuestShareMessages (vMaNGOS QuestDef.h), the u8 of MSG_QUEST_PUSH_RESULT, with the client's line for each.
constexpr u8 kQuestPushSharing = 0;     // ERR_QUEST_PUSH_SUCCESS_S ("Sharing quest with %s...")
constexpr u8 kQuestPushCantTake = 1;    // ERR_QUEST_PUSH_INVALID_S
constexpr u8 kQuestPushAccepted = 2;    // ERR_QUEST_PUSH_ACCEPTED_S
constexpr u8 kQuestPushDeclined = 3;    // ERR_QUEST_PUSH_DECLINED_S
constexpr u8 kQuestPushTooFar = 4;      // ERR_QUEST_PUSH_TOO_FAR_S
constexpr u8 kQuestPushBusy = 5;        // ERR_QUEST_PUSH_BUSY_S
constexpr u8 kQuestPushLogFull = 6;     // ERR_QUEST_PUSH_LOG_FULL_S
constexpr u8 kQuestPushOnQuest = 7;     // ERR_QUEST_PUSH_ONQUEST_S
constexpr u8 kQuestPushAlreadyDone = 8; // ERR_QUEST_PUSH_ALREADY_DONE_S

// CMSG_PUSHQUESTTOPARTY: u32 quest. The server offers it to every party member (each answer comes back as a
// MSG_QUEST_PUSH_RESULT about that member).
void writePushQuestToParty(std::vector<u8>& out, u32 questId); // appended to `out`

// MSG_QUEST_PUSH_RESULT (9 bytes both ways): the server tells the sharer how it went with one member (`guid`:
// that member); the member's client answers a shared quest it declines with the sharer's GUID and
// kQuestPushDeclined (vMaNGOS forwards the u8 to the sharer and forgets the share: without it the member
// stays "busy").
struct QuestPushResult {
    u64 guid = 0;
    u8 result = 0; // kQuestPush*
};
bool parseQuestPushResult(std::span<const u8> payload, QuestPushResult& out);
void writeQuestPushResult(std::vector<u8>& out, u64 guid, u8 result);

// SMSG_GAMEOBJECT_CUSTOM_ANIM (12 bytes): u64 object, u32 custom animation (0..3: the model's
// Custom0..Custom3; the fishing bobber's bite is 0).
struct GameObjectCustomAnim {
    u64 guid = 0;
    u32 anim = 0;
};
bool parseGameObjectCustomAnim(std::span<const u8> payload, GameObjectCustomAnim& out);

// SMSG_GAMEOBJECT_SPAWN_ANIM, SMSG_GAMEOBJECT_DESPAWN_ANIM, SMSG_GAMEOBJECT_RESET_STATE and
// SMSG_GAMEOBJECT_PAGETEXT carry only the object's full GUID (net::parseGuid); SMSG_FISH_NOT_HOOKED and
// SMSG_FISH_ESCAPED are empty.

} // namespace vd::net
