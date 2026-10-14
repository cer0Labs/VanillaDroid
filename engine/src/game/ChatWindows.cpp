#include "game/ChatWindows.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vd::game {
namespace {

// WoW.exe's table 0x805FB0 (12 bytes a row: name, on by default, the ADDEDVERSION that brought it). Ours after: a line
// of the combat log store, about others than the player and its pet.
constexpr ChatGroupInfo kGroups[kChatGroups] = {
    {"SYSTEM", true, 0, false, false},
    {"SAY", true, 0, false, false},
    {"YELL", true, 0, false, false},
    {"WHISPER", true, 0, false, false},
    {"PARTY", true, 0, false, false},
    {"GUILD", true, 0, false, false},
    {"CREATURE", true, 0, false, false},
    {"CHANNEL", true, 0, false, false},
    {"SKILL", true, 0, false, false},
    {"LOOT", true, 0, false, false},
    {"COMBAT_MISC_INFO", true, 0, true, false},
    {"COMBAT_SELF_HITS", true, 0, true, false},
    {"COMBAT_SELF_MISSES", true, 0, true, false},
    {"COMBAT_PET_HITS", true, 0, true, false},
    {"COMBAT_PET_MISSES", true, 0, true, false},
    {"COMBAT_PARTY_HITS", false, 0, true, true},
    {"COMBAT_PARTY_MISSES", false, 0, true, true},
    {"COMBAT_FRIENDLYPLAYER_HITS", false, 0, true, true},
    {"COMBAT_FRIENDLYPLAYER_MISSES", false, 0, true, true},
    {"COMBAT_HOSTILEPLAYER_HITS", true, 0, true, false},
    {"COMBAT_HOSTILEPLAYER_MISSES", true, 0, true, false},
    {"COMBAT_CREATURE_VS_SELF_HITS", true, 0, true, false},
    {"COMBAT_CREATURE_VS_SELF_MISSES", true, 0, true, false},
    {"COMBAT_CREATURE_VS_PARTY_HITS", false, 0, true, true},
    {"COMBAT_CREATURE_VS_PARTY_MISSES", false, 0, true, true},
    {"COMBAT_CREATURE_VS_CREATURE_HITS", false, 0, true, true},
    {"COMBAT_CREATURE_VS_CREATURE_MISSES", false, 0, true, true},
    {"COMBAT_FRIENDLY_DEATH", true, 0, true, false},
    {"COMBAT_HOSTILE_DEATH", true, 0, true, false},
    {"COMBAT_XP_GAIN", true, 0, false, false},
    {"SPELL_SELF_DAMAGE", true, 0, true, false},
    {"SPELL_SELF_BUFF", true, 0, true, false},
    {"SPELL_PET_DAMAGE", true, 0, true, false},
    {"SPELL_PET_BUFF", true, 0, true, false},
    {"SPELL_PARTY_DAMAGE", false, 0, true, true},
    {"SPELL_PARTY_BUFF", false, 0, true, true},
    {"SPELL_FRIENDLYPLAYER_DAMAGE", false, 0, true, true},
    {"SPELL_FRIENDLYPLAYER_BUFF", false, 0, true, true},
    {"SPELL_HOSTILEPLAYER_DAMAGE", true, 0, true, false},
    {"SPELL_HOSTILEPLAYER_BUFF", true, 0, true, false},
    {"SPELL_CREATURE_VS_SELF_DAMAGE", true, 0, true, false},
    {"SPELL_CREATURE_VS_SELF_BUFF", true, 0, true, false},
    {"SPELL_CREATURE_VS_PARTY_DAMAGE", false, 0, true, true},
    {"SPELL_CREATURE_VS_PARTY_BUFF", false, 0, true, true},
    {"SPELL_CREATURE_VS_CREATURE_DAMAGE", false, 0, true, true},
    {"SPELL_CREATURE_VS_CREATURE_BUFF", false, 0, true, true},
    {"SPELL_TRADESKILLS", true, 0, true, false},
    {"SPELL_DAMAGESHIELDS_ON_SELF", true, 0, true, false},
    {"SPELL_DAMAGESHIELDS_ON_OTHERS", false, 0, true, true},
    {"SPELL_AURA_GONE_SELF", true, 0, true, false},
    {"SPELL_AURA_GONE_PARTY", false, 0, true, true},
    {"SPELL_AURA_GONE_OTHER", false, 0, true, true},
    {"SPELL_ITEM_ENCHANTMENTS", true, 0, true, false},
    {"SPELL_BREAK_AURA", true, 0, true, false},
    {"SPELL_PERIODIC_SELF_DAMAGE", true, 0, true, false},
    {"SPELL_PERIODIC_SELF_BUFFS", true, 0, true, false},
    {"SPELL_PERIODIC_PARTY_DAMAGE", false, 0, true, true},
    {"SPELL_PERIODIC_PARTY_BUFFS", false, 0, true, true},
    {"SPELL_PERIODIC_FRIENDLYPLAYER_DAMAGE", false, 0, true, true},
    {"SPELL_PERIODIC_FRIENDLYPLAYER_BUFFS", false, 0, true, true},
    {"SPELL_PERIODIC_HOSTILEPLAYER_DAMAGE", true, 0, true, false},
    {"SPELL_PERIODIC_HOSTILEPLAYER_BUFFS", true, 0, true, false},
    {"SPELL_PERIODIC_CREATURE_DAMAGE", true, 0, true, false},
    {"SPELL_PERIODIC_CREATURE_BUFFS", true, 0, true, false},
    {"SPELL_FAILED_LOCALPLAYER", false, 0, false, false},
    {"COMBAT_HONOR_GAIN", true, 0, false, false},
    {"COMBAT_FACTION_CHANGE", true, 1, false, false},
    {"MONEY", true, 2, false, false},
};

// ChatTypeGroup's groups of several CHAT_MSG types: their menus name them by the group (getglobal(value)).
bool severalTypes(u32 group) {
    return group == kGroupSystem || group == kGroupSay || group == kGroupWhisper || group == kGroupParty ||
           group == kGroupGuild || group == kGroupCreature || group == kGroupChannel || group == kGroupLoot;
}

// ChatFrame.lua's ChannelMenuChatTypeGroups, CombatLogMenuChatTypeGroups, SpellLogMenuChatTypeGroups,
// SpellLogOtherMenuChatTypeGroups, PeriodicLogMenuChatTypeGroups, OtherMenuChatTypeGroups.
constexpr u8 kChannelMenu[] = {kGroupSay, kGroupYell, kGroupGuild, kGroupWhisper, kGroupParty};
constexpr u8 kCombatMenu[] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 65, 66};
constexpr u8 kSpellMenu[] = {30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45};
constexpr u8 kSpellOtherMenu[] = {46, 47, 48, 49, 50, 51, 52, 53, 64};
constexpr u8 kPeriodicMenu[] = {54, 55, 56, 57, 58, 59, 60, 61, 62, 63};
constexpr u8 kOtherMenu[] = {kGroupCreature, kGroupSkill, kGroupLoot};

constexpr u8 kDefaultAlpha = 64; // DEFAULT_CHATFRAME_ALPHA 0.25 of 255

bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'a' && a[i] <= 'z' ? static_cast<char>(a[i] - 32) : a[i];
        const char y = b[i] >= 'a' && b[i] <= 'z' ? static_cast<char>(b[i] - 32) : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    return s;
}

// "KEY rest": the key and what follows it.
std::string_view keyOf(std::string_view line, std::string_view& rest) {
    const usize space = line.find(' ');
    rest = space == std::string_view::npos ? std::string_view{} : trim(line.substr(space + 1));
    return line.substr(0, space);
}

u32 number(std::string_view s) {
    char buffer[16] = {};
    const usize n = std::min(s.size(), sizeof(buffer) - 1);
    for (usize i = 0; i < n; ++i) {
        buffer[i] = s[i];
    }
    return static_cast<u32>(std::strtoul(buffer, nullptr, 10));
}

void appendf(std::string& out, const char* format, u32 a) {
    char line[64];
    std::snprintf(line, sizeof(line), format, a);
    out.append(line);
}

} // namespace

const ChatGroupInfo& chatGroupInfo(u32 group) {
    return kGroups[group < kChatGroups ? group : 0];
}

i32 chatGroupIndex(std::string_view name) {
    for (u32 i = 0; i < kChatGroups; ++i) {
        if (name == kGroups[i].name) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

const char* chatGroupLabelKey(u32 group) {
    // Built once: "CHAT_MSG_" + the name for the groups of one type.
    static const std::array<std::string, kChatGroups> keys = [] {
        std::array<std::string, kChatGroups> k;
        for (u32 i = 0; i < kChatGroups; ++i) {
            k[i] = severalTypes(i) ? std::string(kGroups[i].name) : std::string("CHAT_MSG_") + kGroups[i].name;
        }
        return k;
    }();
    return keys[group < kChatGroups ? group : 0].c_str();
}

std::span<const u8> chatMenuGroups(ChatMenu menu) {
    switch (menu) {
    case ChatMenu::Channels: return kChannelMenu;
    case ChatMenu::Combat: return kCombatMenu;
    case ChatMenu::Spell: return kSpellMenu;
    case ChatMenu::SpellOther: return kSpellOtherMenu;
    case ChatMenu::Periodic: return kPeriodicMenu;
    case ChatMenu::Other: return kOtherMenu;
    case ChatMenu::Count: break;
    }
    return {};
}

const char* chatMenuLabelKey(ChatMenu menu) {
    static constexpr const char* kKeys[] = {"CHANNELS",          "COMBAT_MESSAGES",      "SPELL_MESSAGES",
                                            "SPELL_OTHER_MESSAGES", "PERIODIC_MESSAGES", "OTHER_MESSAGES"};
    return kKeys[static_cast<u32>(menu) < std::size(kKeys) ? static_cast<u32>(menu) : 0];
}

bool ChatWindowConfig::hasChannel(std::string_view channel) const {
    for (const std::string& c : channels) {
        if (sameName(c, channel)) {
            return true;
        }
    }
    return false;
}

bool ChatWindowConfig::anyCombat() const {
    for (u32 i = 0; i < kChatGroups; ++i) {
        if (groups[i] && kGroups[i].combat) {
            return true;
        }
    }
    return false;
}

bool ChatWindowConfig::combatOthers() const {
    for (u32 i = 0; i < kChatGroups; ++i) {
        if (groups[i] && kGroups[i].others) {
            return true;
        }
    }
    return false;
}

void ChatWindows::setDefaults() {
    m_windows = {};
    for (ChatWindowConfig& w : m_windows) {
        w.channels.clear();
    }
    // 0x498542: ChatFrame1's first ten groups and its flags; 0x49856F: ChatFrame2's from the table's defaults.
    ChatWindowConfig& general = m_windows[0];
    for (u32 i = 0; i < 10; ++i) {
        general.groups[i] = true;
    }
    general.shown = true;
    general.docked = 1;
    general.locked = true;
    ChatWindowConfig& log = m_windows[1];
    for (u32 i = 10; i < kChatGroups; ++i) {
        log.groups[i] = kGroups[i].byDefault;
    }
    log.shown = true;
    log.docked = 2;
    log.locked = true;
    m_guildRecruitment = "AUTO";
    m_channels.clear();
    m_zoneChannels = 0;
    m_colors.clear();
}

bool ChatWindows::parse(std::string_view text) {
    if (trim(text).empty()) {
        return false;
    }
    setDefaults();
    for (ChatWindowConfig& w : m_windows) {
        w = ChatWindowConfig{};
    }
    u32 addedVersion = 0;
    enum class List : u8 { None, Channels, Colors, Messages, WindowChannels } list = List::None;
    ChatWindowConfig* window = nullptr;
    usize at = 0;
    while (at <= text.size()) {
        const usize end = text.find('\n', at);
        const std::string_view line = trim(text.substr(at, end == std::string_view::npos ? std::string_view::npos
                                                                                       : end - at));
        at = end == std::string_view::npos ? text.size() + 1 : end + 1;
        if (line.empty()) {
            continue;
        }
        if (list != List::None) {
            if (line == "END") {
                list = List::None;
            } else if (list == List::Channels) {
                m_channels.emplace_back(line);
            } else if (list == List::Colors) {
                m_colors.emplace_back(line);
            } else if (list == List::Messages && window) {
                if (const i32 group = chatGroupIndex(line); group >= 0) {
                    window->groups[static_cast<u32>(group)] = true;
                }
            } else if (list == List::WindowChannels && window) {
                window->channels.emplace_back(line);
            }
            continue;
        }
        std::string_view rest;
        const std::string_view key = keyOf(line, rest);
        if (key == "VERSION") {
            continue;
        } else if (key == "ADDEDVERSION") {
            addedVersion = number(rest);
        } else if (key == "OPTION_GUILD_RECRUITMENT_CHANNEL") {
            m_guildRecruitment.assign(rest);
        } else if (key == "CHANNELS") {
            list = window ? List::WindowChannels : List::Channels;
        } else if (key == "COLORS") {
            list = List::Colors;
        } else if (key == "MESSAGES") {
            list = List::Messages;
        } else if (key == "ZONECHANNELS") {
            (window ? window->zoneChannels : m_zoneChannels) = number(rest);
        } else if (key == "WINDOW") {
            const u32 index = number(rest);
            window = index >= 1 && index <= kChatWindowSlots ? &m_windows[index - 1] : nullptr;
        } else if (key == "END") {
            window = nullptr;
        } else if (window && key == "NAME") {
            window->name.assign(rest.substr(0, 31));
        } else if (window && key == "SIZE") {
            window->fontSize = number(rest);
        } else if (window && key == "COLOR") {
            u32 c[4] = {};
            std::string_view more = rest;
            for (u32& v : c) {
                std::string_view next;
                const std::string_view part = keyOf(more, next);
                v = std::min(255u, number(part));
                more = next;
            }
            window->r = static_cast<u8>(c[0]);
            window->g = static_cast<u8>(c[1]);
            window->b = static_cast<u8>(c[2]);
            window->a = static_cast<u8>(c[3]);
        } else if (window && key == "LOCKED") {
            window->locked = number(rest) != 0;
        } else if (window && key == "DOCKED") {
            window->docked = number(rest);
        } else if (window && key == "SHOWN") {
            window->shown = number(rest) != 0;
        }
    }
    // 0x49967C: a file older than the groups it lacks gets those on by default, in ChatFrame1 (the first ten) or
    // ChatFrame2.
    if (addedVersion < kChatCacheVersion) {
        for (u32 i = 0; i < kChatGroups; ++i) {
            if (kGroups[i].byDefault && kGroups[i].addedVersion > addedVersion) {
                m_windows[i < 10 ? 0 : 1].groups[i] = true;
            }
        }
    }
    m_changed = false;
    return true;
}

std::string ChatWindows::format() const {
    std::string out;
    out.reserve(4096);
    appendf(out, "VERSION %u\n\n", kChatCacheVersion);
    appendf(out, "ADDEDVERSION %u\n\n", kChatCacheVersion);
    out.append("OPTION_GUILD_RECRUITMENT_CHANNEL ").append(m_guildRecruitment).append("\n\n");
    out.append("CHANNELS\n");
    for (const std::string& c : m_channels) {
        out.append(c).append("\n");
    }
    out.append("END\n\n");
    appendf(out, "ZONECHANNELS %u\n\n", m_zoneChannels);
    out.append("COLORS\n");
    for (const std::string& c : m_colors) {
        out.append(c).append("\n");
    }
    out.append("END\n\n");
    for (u32 i = 0; i < kChatWindowSlots; ++i) {
        const ChatWindowConfig& w = m_windows[i];
        appendf(out, "WINDOW %u\n", i + 1);
        if (!w.name.empty()) {
            out.append("NAME ").append(w.name).append("\n");
        }
        appendf(out, "SIZE %u\n", w.fontSize);
        char color[64];
        std::snprintf(color, sizeof(color), "COLOR %u %u %u %u\n", w.r, w.g, w.b, w.a);
        out.append(color);
        appendf(out, "LOCKED %u\n", w.locked ? 1u : 0u);
        appendf(out, "DOCKED %u\n", w.docked);
        appendf(out, "SHOWN %u\n\n", w.shown ? 1u : 0u);
        out.append("MESSAGES\n");
        for (u32 g = 0; g < kChatGroups; ++g) {
            if (w.groups[g]) {
                out.append(kGroups[g].name).append("\n");
            }
        }
        out.append("END\n\n");
        out.append("CHANNELS\n");
        for (const std::string& c : w.channels) {
            out.append(c).append("\n");
        }
        out.append("END\n\n");
        appendf(out, "ZONECHANNELS %u\n\n", w.zoneChannels);
        out.append("END\n\n");
    }
    return out;
}

u32 ChatWindows::docked(std::span<u8> out) const {
    // DOCKED_CHAT_FRAMES: the docked windows by their place (ChatFrame1 always first).
    u32 count = 0;
    for (u32 place = 1; place <= kChatWindowSlots && count < out.size(); ++place) {
        for (u32 i = 0; i < kChatWindows && count < out.size(); ++i) {
            if (m_windows[i].docked == place) {
                out[count++] = static_cast<u8>(i);
            }
        }
    }
    return count;
}

u32 ChatWindows::active() const {
    u32 count = 0;
    for (u32 i = 0; i < kChatWindows; ++i) {
        count += m_windows[i].shown || m_windows[i].docked != 0 ? 1u : 0u;
    }
    return count;
}

void ChatWindows::redock() {
    std::array<u8, kChatWindowSlots> order{};
    const u32 count = docked(order);
    for (u32 i = 0; i < count; ++i) {
        m_windows[order[i]].docked = i + 1;
    }
}

i32 ChatWindows::openNew(std::string_view name) {
    for (u32 i = 0; i < kChatWindows; ++i) {
        ChatWindowConfig& w = m_windows[i];
        if ((w.shown || w.docked != 0) && i + 1 != kChatWindows) {
            continue;
        }
        w = ChatWindowConfig{};
        w.name.assign(name.substr(0, 31));
        w.a = kDefaultAlpha;
        for (const u8 g : {kGroupSay, kGroupYell, kGroupGuild, kGroupWhisper, kGroupParty, kGroupChannel}) {
            w.groups[g] = true;
        }
        w.shown = true;
        w.docked = 0;
        redock();
        std::array<u8, kChatWindowSlots> order{};
        w.docked = docked(order) + 1; // FCF_DockFrame: at the end of the dock
        m_changed = true;
        return static_cast<i32>(i);
    }
    return -1;
}

void ChatWindows::close(u32 index) {
    if (index == 0 || index >= kChatWindows) {
        return;
    }
    m_windows[index].shown = false;
    m_windows[index].docked = 0;
    redock();
    m_changed = true;
}

void ChatWindows::toggleGroup(u32 index, u32 group) {
    if (group < kChatGroups) {
        window(index).groups[group] = !window(index).groups[group];
        m_changed = true;
    }
}

void ChatWindows::toggleChannel(u32 index, std::string_view channel) {
    std::vector<std::string>& list = window(index).channels;
    for (usize i = 0; i < list.size(); ++i) {
        if (sameName(list[i], channel)) {
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
            m_changed = true;
            return;
        }
    }
    list.emplace_back(channel);
    m_changed = true;
}

} // namespace vd::game
