#include "app/ExtrasGallery.h"

#include "app/SocialWindows.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Text.h"
#include "game/SocialText.h"
#include "net/ByteBuffer.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

namespace vd {
namespace {

using net::ChatType;
namespace uf = net::uf;

constexpr u64 kPlayer = 0x20;
constexpr u32 kNow = 1000;

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

constexpr u64 icon(const char* name) {
    u64 hash = assetPathHash("Interface\\Icons\\");
    for (const char* c = name; *c; ++c) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(*c));
        hash *= kFnvPrime;
    }
    for (const char c : std::string_view(".blp")) {
        hash ^= static_cast<u8>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

// What the human hears around Tarren Mill and in Orgrimmar's shadow (sample lines; the languages are
// Languages.dbc's).
struct Said {
    ChatType type;
    u32 language;
    u64 sender;
    const char* name;
    const char* text;
};
constexpr Said kSaid[] = {
    {ChatType::Say, 1, 0x21, "Grukk", "Lok'tar ogar! For the Horde!"},
    {ChatType::Yell, 14, 0x22, "Zuljin", "You be dead soon, mon!"},
    {ChatType::MonsterYell, 1, 0xF130000000000D3Bull, "Orgrimmar Grunt", "Strength and honor!"},
    {ChatType::Say, 6, 0x23, "Thorgrim", "Ye'll need a stronger ale than that, lad."},
    {ChatType::Say, 33, 0x24, "Mordant", "The living have no place here."},
    {ChatType::Say, 7, 0x25, "Aeryn", "Did anyone else hear that?"},
};

// SMSG_MESSAGECHAT as ChatHandler::BuildChatPacket writes it (say and yell: the sender twice; a creature: its
// name).
std::vector<u8> chatPacket(const Said& s) {
    std::vector<u8> out;
    net::ByteWriter w(out);
    w.u8v(static_cast<u8>(s.type)).u32v(s.language).u64v(s.sender);
    if (s.type == ChatType::MonsterYell) {
        w.u32v(static_cast<u32>(std::strlen(s.name) + 1)).cstring(s.name).u64v(0);
    } else {
        w.u64v(s.sender);
    }
    w.u32v(static_cast<u32>(std::strlen(s.text) + 1)).cstring(s.text).u8v(0);
    return out;
}

// SMSG_UPDATE_OBJECT creating the player: a human warrior who knows Common (skill 98 at 300 / 300).
std::vector<u8> playerCreate() {
    std::vector<std::pair<u16, u32>> fields = {
        {uf::UNIT_FIELD_HEALTH, 100},
        {uf::UNIT_FIELD_BYTES_0, 1 | (1u << 8)},
        {uf::UNIT_FIELD_LEVEL, 10},
        {uf::PLAYER_SKILL_INFO_1_1, 98},
        {static_cast<u16>(uf::PLAYER_SKILL_INFO_1_1 + 1), 300 | (300u << 16)}};
    std::sort(fields.begin(), fields.end());
    std::vector<u8> out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(kPlayer).u8v(static_cast<u8>(game::ObjectType::Player)).u8v(0x40);
    w.f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f);
    std::vector<u32> mask(fields.back().first / 32 + 1, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(mask.size()));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
    return out;
}

// A warrior's book: General, Arms, Fury, Protection.
const SpellbookEntryView kGeneral[] = {{icon("INV_Sword_04"), "Attack", ""},
                                       {icon("Spell_Holy_HolyBolt"), "The Human Spirit", "", true}};
const SpellbookEntryView kArms[] = {{icon("Ability_Warrior_Charge"), "Charge", "Rank 1"},
                                    {icon("Ability_Rogue_Ambush"), "Heroic Strike", "Rank 2"},
                                    {icon("Ability_Gouge"), "Rend", "Rank 2"}};
const SpellbookEntryView kFury[] = {{icon("Ability_Warrior_BattleShout"), "Battle Shout", "Rank 2"},
                                    {icon("Ability_ThunderClap"), "Thunder Clap", "Rank 1"},
                                    {icon("Ability_Warrior_Cleave"), "Cleave", "Rank 1"}};
const SpellbookEntryView kProtection[] = {{icon("Ability_Warrior_DefensiveStance"), "Defensive Stance", ""}};

} // namespace

bool ExtrasGallery::select(const char* name) {
    static constexpr const char* kNames[] = {"languages", "spellbookflash", "languagemenu"};
    for (u32 i = 0; i < std::size(kNames); ++i) {
        if (std::strcmp(name, kNames[i]) == 0) {
            m_page = static_cast<Page>(i);
            m_ready = false;
            return true;
        }
    }
    return false;
}

void ExtrasGallery::draw(DialogContext& c, u64 nowMs) {
    switch (m_page) {
    case Page::Languages: drawLanguages(c, nowMs); break;
    case Page::SpellBookFlash: drawSpellBook(c, nowMs); break;
    case Page::LanguageMenu: drawLanguageMenu(c); break;
    }
}

void ExtrasGallery::buildChat() {
    const game::LanguageData* languages = m_languageData;
    if (!languages || !languages->ready()) {
        // Without the game's tables: a few words of each.
        static const std::pair<u32, std::string_view> kNames[] = {
            {1, "Orcish"}, {6, "Dwarvish"}, {7, "Common"}, {14, "Troll"}, {33, "Gutterspeak"}};
        static const game::LanguageData::Word kWords[] = {
            {1, "Ko"},   {1, "Lok"},   {1, "Ogar"},   {1, "Kazum"},  {1, "Thrakk"},
            {6, "Ok"},   {6, "Ruk"},   {6, "Gral"},   {6, "Modan"},  {6, "Dunkel"},
            {14, "Ju"},  {14, "Mon"},  {14, "Wuze"},  {14, "Fadah"}, {33, "Ras"},
            {33, "Lok"}, {33, "Thra"}, {33, "Morth"}, {33, "Gorbam"}};
        static const game::LanguageData::Teacher kTeachers[] = {{668, 7, 98, 0, 0}};
        m_sampleLanguages.set(kNames, kWords, kTeachers);
        languages = &m_sampleLanguages;
    }
    m_store = std::make_unique<game::EntityStore>();
    m_play = std::make_unique<game::Gameplay>(*m_store);
    m_play->setSink({[](void*, net::Opcode, std::span<const u8>) { return true; }, nullptr});
    m_play->setLanguageData(languages);
    m_play->enterWorld(kPlayer, 0, true, kNow);
    m_store->applyUpdateObject(playerCreate(), kNow);
    std::vector<u8> spells;
    net::ByteWriter(spells).u8v(0).u16v(1).u16v(668).u16v(0).u16v(0); // SMSG_INITIAL_SPELLS: Language Common
    m_play->handle(net::Opcode::SMSG_INITIAL_SPELLS, spells, kNow);
    u32 cursor = m_play->chat().end();
    m_texts.clear();
    m_lines.clear();
    for (const Said& s : kSaid) {
        m_play->handle(net::Opcode::SMSG_MESSAGECHAT, chatPacket(s), kNow);
        const game::ChatLine* line = m_play->chat().next(cursor);
        if (!line) {
            continue;
        }
        std::string& text = m_texts.emplace_back();
        game::appendChatLine(text, *line, s.name, 0);
        VD_LOG_INFO("UI gallery: languages: %s", text.c_str());
    }
    for (u32 i = 0; i < m_texts.size(); ++i) {
        const ChatType type = kSaid[i].type;
        const u32 color = type == ChatType::Say ? ui::rgba(255, 255, 255) : ui::rgba(255, 64, 64);
        m_lines.push_back({m_texts[i], color, i + 1, {}});
    }
}

void ExtrasGallery::drawLanguages(DialogContext& c, u64 nowMs) {
    (void)nowMs;
    if (!m_ready) {
        m_ready = true;
        buildChat();
    }
    const ChatFrameTab tabs[] = {{text::get("GENERAL")}, {text::get("COMBAT_LOG")}};
    ChatFrameView v;
    v.lines = m_lines;
    v.tabs = tabs;
    v.expanded = true;
    v.backgroundAlpha = 1.0f;
    v.header = text::get("CHAT_MSG_SAY");
    v.headerColor = ui::rgba(255, 255, 255);
    v.hint = text::get("VD_SOCIAL_CHAT_HINT");
    const ChatFrameResult r = chatFrame(c, chatFrameRect(c, true, {}, {}, false), v, m_chatState);
    if (r.line >= 0) {
        char line[48];
        std::snprintf(line, sizeof(line), "languages: line %d", r.line);
        logResult(line);
    }
}

void ExtrasGallery::drawSpellBook(DialogContext& c, u64 nowMs) {
    if (!m_ready) {
        m_ready = true;
        m_flash = {};
        m_tab = 0;
        m_flash.learned(2,
                        false); // Cleave learned at the trainer with the book closed: LEARNED_SPELL_IN_TAB 3
        m_flash.shown(nowMs);   // then the book opens
    }
    static const SpellbookTabView kTabs[] = {{"General", icon("INV_Misc_Book_09")},
                                             {"Arms", icon("Ability_Rogue_Eviscerate")},
                                             {"Fury", icon("Ability_Warrior_InnerRage")},
                                             {"Protection", icon("INV_Shield_06")}};
    std::array<SpellbookTabView, std::size(kTabs)> tabs{};
    for (u32 i = 0; i < tabs.size(); ++i) {
        tabs[i] = kTabs[i];
        tabs[i].flash = m_flash.alpha(i, nowMs);
    }
    SpellbookView view;
    view.tabs = tabs;
    view.tab = m_tab;
    switch (m_tab) {
    case 0: view.entries = kGeneral; break;
    case 1: view.entries = kArms; break;
    case 2: view.entries = kFury; break;
    default: view.entries = kProtection; break;
    }
    const SpellbookResult r = spellbookWindow(c, view);
    if (r.tab >= 0) {
        m_flash.clicked(static_cast<u32>(r.tab)); // SpellBookSkillLineTab_OnClick
        m_tab = static_cast<u32>(r.tab);
        char line[48];
        std::snprintf(line, sizeof(line), "spellbook tab %d", r.tab);
        logResult(line);
    }
    if (r.closed) {
        m_flash.hidden();
        logResult("spellbook closed");
    }
}

void ExtrasGallery::drawLanguageMenu(DialogContext& c) {
    // As SocialScreens builds it (Modal::Channels): the chat types, the channels, the last whisper, then the
    // languages when there is more than one (Languages.dbc's names; Dwarvish 6 comes before Common 7).
    static constexpr std::string_view kLanguages[] = {"Dwarvish", "Common"};
    char line[96];
    for (u32 i = 0; i < m_languageChoices.size(); ++i) {
        m_languageChoices[i].assign(text::format(line, "VD_EXTRAS_LANGUAGE_CHOICE", kLanguages[i]));
    }
    char channel[64], tell[96];
    std::snprintf(channel, sizeof(channel), "1. General");
    std::string_view whisper = text::format(tell, "CHAT_WHISPER_SEND", "Aeryn");
    while (!whisper.empty() && (whisper.back() == ' ' || whisper.back() == ':')) {
        whisper.remove_suffix(1);
    }
    const ChoiceView choices[] = {
        {text::get("CHAT_MSG_SAY"), ui::rgba(255, 255, 255), true, true},
        {text::get("CHAT_MSG_YELL"), ui::rgba(255, 64, 64), false, true},
        {text::get("CHAT_MSG_PARTY"), ui::rgba(170, 170, 255), false, true},
        {channel, ui::rgba(255, 192, 192), false, true},
        {whisper, ui::rgba(255, 128, 255), false, true},
        {m_languageChoices[0], ui::kGold, m_speaking == 0, true},
        {m_languageChoices[1], ui::kGold, m_speaking == 1, true},
    };
    const i32 picked = choiceList(c, "channels", text::get("VD_SOCIAL_CHAT_TO"), choices);
    if (picked >= 5) {
        m_speaking = static_cast<u32>(picked - 5);
        logResult(m_speaking == 0 ? "language: Dwarvish" : "language: Common");
    } else if (picked >= 0) {
        logResult("chat type picked");
    }
}

} // namespace vd
