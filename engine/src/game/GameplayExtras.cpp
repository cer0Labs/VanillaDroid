// Phase 18 (extras): game::Gameplay's part of the small 1.12 behaviours that were missing (kept apart from
// Gameplay.cpp): B4, the languages of the chat (game/Languages; WoW.exe 0x49A970, 0x5EC720), the mount's
// special animation (0x60DEA0, 0x603FF0), the pet's sounds (0x6040C0), a renamed player's name (0x555600),
// the names of an item set's pieces (0x555120) and the rest start that nothing reads (0x5E470A). Read in
// WoW.exe 1.12.1 (read-only disassembly).
#include "game/Gameplay.h"

#include "game/Languages.h"
#include "net/ExtrasMessages.h"
#include "net/UpdateFields.gen.h"

namespace vd::game {
namespace {

namespace uf = net::uf;
using net::ChatType;
using net::Opcode;

constexpr u32 kSkillSlots = 128; // PLAYER_SKILL_INFO_1_1: three fields a skill

// 0x49A970: the emotes, the system lines and the creatures' emotes are heard as Universal (no garbling, no
// "[language]").
bool universalType(ChatType type) {
    return type == ChatType::Emote || type == ChatType::System || type == ChatType::MonsterEmote ||
           type == ChatType::RaidBossEmote;
}

} // namespace

u32 Gameplay::languageSkill(u32 language) const {
    u32 skill = 0;
    languageSkillOf(language, skill);
    return skill;
}

bool Gameplay::languageSkillOf(u32 language, u32& skill) const {
    skill = 0;
    const Entity* player = me();
    if (!player || !m_languageData || language == 0) {
        return false;
    }
    // The spell that taught it: WoW.exe keeps, by language, the last learned spell whose first effect teaches
    // it (0x4B2641 into 0xB700AC).
    u32 spell = 0;
    for (const u32 id : m_spells.known()) {
        u32 taught = 0;
        if (m_languageData->teaches(id, taught) && taught == language) {
            spell = id;
        }
    }
    if (spell == 0) {
        return false;
    }
    // Its skill line for the player's race and class (0x6DE040), and that skill in the player's fields
    // (0x5EC720: the value, plus the permanent bonus when above 0, plus the temporary one).
    const u32 line = m_languageData->skillLine(spell, player->byteValue(uf::UNIT_FIELD_BYTES_0, 0),
                                               player->byteValue(uf::UNIT_FIELD_BYTES_0, 1));
    if (line == 0) {
        return false;
    }
    for (u32 slot = 0; slot < kSkillSlots; ++slot) {
        const u16 base = static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + slot * 3);
        if ((player->value(base) & 0xFFFFu) != line) {
            continue;
        }
        const u32 bonus = player->value(static_cast<u16>(base + 2)); // MAKE_SKILL_BONUS(temporary, permanent)
        i32 value = static_cast<i32>(player->value(static_cast<u16>(base + 1)) & 0xFFFFu);
        if (value > 0) {
            value += static_cast<i32>(bonus >> 16);
        }
        value += static_cast<i16>(bonus & 0xFFFFu);
        skill = value > 0 ? static_cast<u32>(value) : 0;
        return true;
    }
    return false;
}

void Gameplay::addChatMessage(const net::ChatMessage& m, u32 nowMs) {
    // 0x49A89B: an addon's message (language -1) becomes CHAT_MSG_ADDON, which no chat frame shows.
    if (m.language == static_cast<u32>(net::Language::Addon)) {
        return;
    }
    const u32 language = universalType(m.type) ? 0 : m.language;
    net::ChatMessage heard = m;
    const Entity* player = me();
    // 0x49A9C6..0x49AA7C: a game master understands everything; anyone else hears a language through its
    // skill (0x49B560 with the chat's rules: one space between words).
    if (language != 0 && m_languageData && player &&
        (player->value(uf::PLAYER_FLAGS) & kPlayerFlagGameMaster) == 0) {
        const u32 skill = languageSkill(language);
        if (skill < kLanguageFullSkill) {
            scrambleLanguage(*m_languageData, language, skill, m.text, m_heardText);
            heard.text = m_heardText;
        }
    }
    ChatLine& line = m_chat.add(heard, nowMs);
    // ChatFrame_OnEvent writes "[" .. arg3 .. "] " when the line's language (Languages.dbc's name, none for
    // Universal: 0x49B0D9) is not GetDefaultLanguage (the race's base language, 0x5EC890).
    if (language != 0 && language != playerLanguage() && m_languageData) {
        line.languageName = m_languageData->name(language);
    }
}

u32 Gameplay::knownLanguages(std::span<u32> out) const {
    u32 count = 0;
    if (!m_languageData) {
        return 0;
    }
    for (const auto& [language, name] : m_languageData->names()) {
        u32 skill = 0;
        if (count < out.size() && languageSkillOf(language, skill)) {
            out[count++] = language;
        }
    }
    return count;
}

u32 Gameplay::speakingLanguage() const {
    u32 skill = 0;
    return m_chatLanguage != 0 && languageSkillOf(m_chatLanguage, skill) ? m_chatLanguage : playerLanguage();
}

std::string_view Gameplay::gossipText(u32 language, std::string_view text, std::string& out) const {
    if (language == 0 || !m_languageData) {
        return text;
    }
    const u32 skill = languageSkill(language);
    if (skill >= kLanguageFullSkill) {
        return text;
    }
    scrambleLanguage(*m_languageData, language, skill, text, out, {true, false});
    return out;
}

bool Gameplay::mountSpecial() {
    const Entity* player = me();
    if (!m_inWorld || !player || player->value(uf::UNIT_FIELD_MOUNTDISPLAYID) == 0) {
        return false;
    }
    fx(FxEventKind::MountSpecial, m_player); // 0x60DEF6: the player's own mount rears at once
    return send(Opcode::CMSG_MOUNTSPECIAL_ANIM);
}

bool Gameplay::handlesExtras(Opcode opcode) {
    switch (opcode) {
    case Opcode::SMSG_MOUNTSPECIAL_ANIM:
    case Opcode::SMSG_PET_ACTION_SOUND:
    case Opcode::SMSG_INVALIDATE_PLAYER:
    case Opcode::SMSG_ITEM_NAME_QUERY_RESPONSE:
    case Opcode::SMSG_SET_REST_START: return true;
    default: return false;
    }
}

Gameplay::Handled Gameplay::handleExtras(Opcode opcode, std::span<const u8> p, u32 nowMs) {
    (void)nowMs;
    bool ok = false;
    switch (opcode) {
    case Opcode::SMSG_MOUNTSPECIAL_ANIM: {
        // 0x603FF0: the unit's mount rears; the player's own already did when the key was pressed.
        u64 unit = 0;
        ok = net::parseMountSpecialAnim(p, unit);
        if (ok && unit != m_player) {
            fx(FxEventKind::MountSpecial, unit);
        }
        break;
    }
    case Opcode::SMSG_PET_ACTION_SOUND: {
        // 0x6040C0: the pet's order sound (0x623A40 kind 1: CreatureSoundData's) or its attack sound (kind
        // 2).
        net::PetActionSound m;
        ok = net::parsePetActionSound(p, m);
        if (ok && (m.talk == net::kPetTalkSpecialSpell || m.talk == net::kPetTalkAttack)) {
            fx(FxEventKind::PetTalk, m.pet, m.talk);
        }
        break;
    }
    case Opcode::SMSG_INVALIDATE_PLAYER: {
        u64 guid = 0;
        ok = net::parseInvalidatePlayer(p, guid);
        if (ok) {
            m_queries.invalidatePlayer(guid); // 0x555600: the name cache forgets it
        }
        break;
    }
    case Opcode::SMSG_ITEM_NAME_QUERY_RESPONSE: {
        net::ItemNameQueryResponse m;
        ok = net::parseItemNameQueryResponse(p, m);
        if (ok) {
            m_queries.onItemName(m.entry, m.name); // 0x555120: itemnamecache.wdb
        }
        break;
    }
    case Opcode::SMSG_SET_REST_START: {
        // 0x5E470A keeps (now - time) at 0xB4E33C, which nothing reads: no effect in 1.12.
        u32 time = 0;
        ok = net::parseSetRestStart(p, time);
        break;
    }
    default: return Handled::No;
    }
    return ok ? Handled::Yes : Handled::Malformed;
}

} // namespace vd::game
