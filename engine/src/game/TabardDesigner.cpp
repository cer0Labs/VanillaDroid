#include "game/TabardDesigner.h"

#include "game/Social.h"
#include "net/GameMessages.h"
#include "net/GuildMessages.h"
#include "net/UpdateFields.gen.h"

#include <iterator>

namespace vd::game {

using net::Opcode;
namespace uf = net::uf;

TabardDesigner::TabardDesigner(const EntityStore& entities, const Social& social)
    : m_entities(entities), m_social(social) {
    m_body.reserve(32);
}

void TabardDesigner::enterWorld(u64 player, u32 nowMs) {
    m_inWorld = true;
    m_player = player;
    m_nowMs = nowMs;
    m_open = false;
    m_pending = false;
}

void TabardDesigner::leaveWorld() {
    m_inWorld = false;
    m_open = false;
    m_pending = false;
}

GuildNotice& TabardDesigner::notice(const char* key) {
    GuildNotice& n = m_notices.push();
    n = GuildNotice{};
    n.key = key;
    n.timeMs = m_nowMs;
    return n;
}

bool TabardDesigner::handles(Opcode opcode) {
    return opcode == Opcode::MSG_TABARDVENDOR_ACTIVATE || opcode == Opcode::MSG_SAVE_GUILD_EMBLEM;
}

bool TabardDesigner::handle(Opcode opcode, std::span<const u8> p, u32 nowMs) {
    m_nowMs = nowMs;
    if (opcode == Opcode::MSG_TABARDVENDOR_ACTIVATE) {
        u64 vendor = 0;
        if (!net::parseGuid(p, vendor)) {
            return false;
        }
        m_vendor = vendor;
        m_open = true;
        m_pending = false;
        initialize();
        return true;
    }
    if (opcode == Opcode::MSG_SAVE_GUILD_EMBLEM) {
        u32 result = 0;
        if (!net::parseU32(p, result)) {
            return false;
        }
        // WoW.exe 0x5E70F0: results 0..4 are ERR_GUILDEMBLEM_* lines, 5 says nothing; the save no longer
        // waits. On success the guild's query comes again (vMaNGOS sends it): the members' tabards change
        // with it.
        static constexpr const char* kKeys[] = {
            "ERR_GUILDEMBLEM_SUCCESS", "ERR_GUILDEMBLEM_INVALID_TABARD_COLORS", "ERR_GUILDEMBLEM_NOGUILD",
            "ERR_GUILDEMBLEM_NOTGUILDMASTER", "ERR_GUILDEMBLEM_NOTENOUGHMONEY"};
        if (result < std::size(kKeys)) {
            notice(kKeys[result]);
        }
        m_pending = false;
        return true;
    }
    return false;
}

u32 TabardDesigner::random(u32 count) {
    m_seed = m_seed * 1664525u + 1013904223u;
    return count > 0 ? (m_seed >> 8) % count : 0;
}

void TabardDesigner::initialize() {
    // InitializeTabardColors (WoW.exe 0x5028E0): the guild's own design when every field is set; otherwise
    // each choice at random within its count.
    const GuildState& g = m_social.guild();
    const GuildEmblem* current = g.id != 0 ? m_social.guildEmblem(g.id) : nullptr;
    if (current && current->designed()) {
        m_design = *current;
        return;
    }
    for (u32 i = 0; i < kEmblemChoiceKinds; ++i) {
        m_design.setChoice(i, static_cast<i32>(random(static_cast<u32>(kEmblemChoiceCounts[i]))));
    }
}

void TabardDesigner::cycle(u32 kind, i32 direction) {
    if (!m_open || kind >= kEmblemChoiceKinds) {
        return;
    }
    // WoW.exe 0x502AC0: (value + count + direction) % count, for a step smaller than the count.
    const i32 count = kEmblemChoiceCounts[kind];
    if ((direction < 0 ? -direction : direction) >= count) {
        return;
    }
    m_design.setChoice(kind, (m_design.choice(kind) + count + direction) % count);
}

bool TabardDesigner::guildMaster() const {
    const GuildState& g = m_social.guild();
    return g.id != 0 && g.known && g.rank == 0;
}

bool TabardDesigner::canSaveNow() const {
    // WoW.exe 0x5F0800: the guild is in the client's cache and no save is pending.
    const GuildState& g = m_social.guild();
    return g.id != 0 && g.known && !m_pending;
}

bool TabardDesigner::save(u32 nowMs) {
    m_nowMs = nowMs;
    if (!m_open || !m_design.designed()) {
        return false;
    }
    // TabardModel:Save (WoW.exe 0x5E03F0): the guild known, the same design is not saved again, a guild
    // master, the money; then the vendor and the five choices, and Accept waits for the answer.
    const GuildState& g = m_social.guild();
    if (g.id == 0 || !g.known) {
        notice("ERR_GUILDEMBLEM_NOGUILD");
        return false;
    }
    if (const GuildEmblem* current = m_social.guildEmblem(g.id); current && *current == m_design) {
        notice("ERR_GUILDEMBLEM_SAME");
        return false;
    }
    if (g.rank != 0) {
        notice("ERR_GUILDEMBLEM_NOTGUILDMASTER");
        return false;
    }
    const Entity* me = m_entities.find(m_player);
    if (me && me->value(uf::PLAYER_FIELD_COINAGE) < kTabardCreationCost) {
        notice("ERR_NOT_ENOUGH_MONEY");
        return false;
    }
    net::GuildEmblemChoice choice;
    choice.emblemStyle = m_design.emblemStyle;
    choice.emblemColor = m_design.emblemColor;
    choice.borderStyle = m_design.borderStyle;
    choice.borderColor = m_design.borderColor;
    choice.backgroundColor = m_design.backgroundColor;
    m_body.clear();
    net::writeSaveGuildEmblem(m_body, m_vendor, choice);
    if (!m_inWorld || !m_sink(Opcode::MSG_SAVE_GUILD_EMBLEM, m_body)) {
        return false;
    }
    m_pending = true;
    return true;
}

void TabardDesigner::close() {
    m_open = false;
}

} // namespace vd::game
