#pragma once

#include "core/Types.h"
#include "game/CombatText.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"

#include <array>
#include <span>
#include <string_view>

namespace vd::game {

class AbilityData;
class GameData;
class Gameplay;
struct Entity;

// The lines the 1.12 client writes itself about the game (not the chat the server sends): loot, money,
// experience, reputation, learned spells, level-ups, skill-ups, quests accepted and turned in. Each has the
// ChatTypeInfo type the client gives it, which decides where the default interface shows it and in which
// colour.
enum class NoticeKind : u8 {
    System,        // CHAT_MSG_SYSTEM, PLAYER_LEVEL_UP: learned spells, level-ups, quests
    Loot,          // CHAT_MSG_LOOT: items looted, received, created (also the group's rolls)
    Skill,         // CHAT_MSG_SKILL: skill-ups
    Money,         // CHAT_MSG_MONEY: money looted
    XpGain,        // CHAT_MSG_COMBAT_XP_GAIN
    FactionChange, // CHAT_MSG_COMBAT_FACTION_CHANGE
    Info,          // UI_INFO_MESSAGE (UIErrorsFrame)
    Error,         // UI_ERROR_MESSAGE (UIErrorsFrame)
    HonorGain,     // phase 17 (honor): CHAT_MSG_COMBAT_HONOR_GAIN (combat log; 224 202 10, WoW.exe 0x805DD5)
};

// Where the default interface shows a kind. The 1.12 chat windows' defaults (WTF chat-cache.txt as the client
// writes it): ChatFrame1 "General" listens to SYSTEM, SAY, YELL, WHISPER, PARTY, GUILD, CREATURE, CHANNEL,
// SKILL and LOOT; ChatFrame2 "Combat Log" to the combat messages, COMBAT_XP_GAIN, COMBAT_HONOR_GAIN,
// COMBAT_FACTION_CHANGE and MONEY. UIErrorsFrame.lua shows UI_INFO_MESSAGE and UI_ERROR_MESSAGE.
enum class NoticePlace : u8 { General, CombatLog, ErrorsFrame };
NoticePlace noticePlace(NoticeKind kind);

struct NoticeColor {
    u8 r = 255, g = 255, b = 255;
};
// ChatTypeInfo's default colours (chat-cache.txt COLORS: SYSTEM 255 255 0, LOOT 0 170 0, SKILL 85 85 255,
// MONEY 255 255 0, COMBAT_XP_GAIN 111 111 255, COMBAT_FACTION_CHANGE 128 128 255) and UIErrorsFrame.lua's
// (info 1, 1, 0; error 1, 0.1, 0.1).
NoticeColor noticeColor(NoticeKind kind);

struct Notice {
    NoticeKind kind = NoticeKind::System;
    // NUL-terminated; items as the client links them (|cff..|Hitem:..|h[Name]|h|r). Phase 16 (alerts): 512 as WoW.exe's
    // buffer of RAID_MEMBERS_AFK (0x4BB310), the longest line (a raid's names).
    std::array<char, 512> text{};
};

// "4 Silver, 85 Copper": money as the client's chat lines write it (WoW.exe "%d %s" and ", %d %s" with GOLD,
// SILVER, COPPER), only the coins there are. Returns what was written into `out` (NUL-terminated).
std::string_view moneyText(std::span<char> out, u32 copper);

// Turns the game events of a Gameplay into the client's own lines (see NoticeKind), in order. A line that
// names an item or a quest the client does not know yet waits for its query (up to kNameWaitMs, then "..."),
// as the client waits for its item cache; the lines behind it wait too, so the order holds. Skill-ups come
// from the player's skill fields (a value that rises while its maximum stays: not the maxima a level or a
// trainer raises). Read the lines with lines() and your own cursor (an EventRing). Main thread; fixed storage
// (no allocation per frame).
class NoticeFeed {
public:
    static constexpr u32 kNameWaitMs = 5000;
    using Ring = EventRing<Notice, 64>;

    // `data` (Spell.dbc: what kind of spell was learned) and `abilities` (skill line and faction names) may
    // be null: those lines are then skipped.
    void init(const GameData* data, const AbilityData* abilities);
    // A new character in the world: its events from now on, its skills read again as they are.
    void restart(const Gameplay& play);
    // Once a frame. `player`: the player's entity (skills, race and class), may be null; `names`: units and
    // spells as the combat log names them.
    void update(Gameplay& play, const Entity* player, const CombatNames& names, u32 nowMs);
    const Ring& lines() const { return m_lines; }

private:
    struct Waiting {
        GameEvent event;
        u32 sinceMs = 0;
    };
    static bool wanted(GameEventKind kind);
    bool ready(Gameplay& play, const GameEvent& e) const;
    void emit(Gameplay& play, const GameEvent& e, const Entity* player, const CombatNames& names);
    void levelUp(Gameplay& play, const GameEvent& e);
    void questRewarded(Gameplay& play, const GameEvent& e);
    void instanceLine(Gameplay& play, const GameEvent& e); // phase 12
    // Phase 16 (alerts): the ready check's and the meeting stones' lines (game/RaidAlertLines.cpp).
    bool raidAlertReady(Gameplay& play, const GameEvent& e) const;
    void raidAlertLine(Gameplay& play, const GameEvent& e);
    // Phase 17 (honor): honor gained (COMBATLOG_HONORGAIN...) and the insignia taken (game/PvpLines.cpp).
    bool pvpReady(Gameplay& play, const GameEvent& e) const;
    void pvpLine(Gameplay& play, const GameEvent& e, const Entity* player, const CombatNames& names);
    // Phase 17 (queue): the battleground queue's lines (game/BattlefieldLines.cpp).
    void battlefieldLine(Gameplay& play, const GameEvent& e);
    // Phase 17 (scores): ERR_BG_PLAYER_JOINED_SS / ERR_BG_PLAYER_LEFT_S (game/BattlefieldScore.cpp).
    void battlegroundPlayerLine(Gameplay& play, const GameEvent& e);
    void skillUps(const Entity& player);
    std::span<char> push(NoticeKind kind);
    // "|cffffffff|Hitem:2589:0:0:0|h[Linen Cloth]|h|r": an item as the client links it ("..." while unknown; a tap
    // on it in the chat shows its tooltip).
    std::string_view itemLink(Gameplay& play, u32 entry, std::span<char> out) const;

    const GameData* m_data = nullptr;
    const AbilityData* m_abilities = nullptr;
    Ring m_lines;
    u32 m_cursor = 0;
    std::array<Waiting, 32> m_waiting{};
    u32 m_waitFirst = 0, m_waitCount = 0;
    // The player's skills by field slot (PLAYER_SKILL_INFO_1_1 + 3 * slot): id, then value | max << 16.
    static constexpr u32 kSkillSlots = 128;
    std::array<u32, kSkillSlots> m_skillIds{};
    std::array<u32, kSkillSlots> m_skillValues{};
    bool m_skillsKnown = false;
};

} // namespace vd::game
