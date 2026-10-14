#pragma once

#include "app/ChatFrame.h"
#include "app/CombatWindows.h"
#include "app/Dialogs.h"
#include "app/SpellBookFlash.h"
#include "core/Types.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Languages.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace vd {

// Development (phase 18, extras): the small 1.12 behaviours of this branch with sample data, as VD_UI_GALLERY
// pages:
//  - "languages": the chat frame of a human warrior hearing the other languages (B4): an orc's /say and a
//    troll's /yell garbled with LanguageWords.dbc ("[Orcish] ", "[Troll] "), a grunt's yell, a dwarf's
//    Dwarvish, an undead's Gutterspeak, and Common as it is. The lines go through game::Gameplay as
//    SMSG_MESSAGECHAT packets would (the real tables when the game's LanguageData is set; a few sample words
//    otherwise).
//  - "spellbookflash": a warrior's spell book after learning a Fury spell with the book closed: the Fury tab
//    glows (CheckButtonGlow) for 30 s once the book shows; a tap on it stops its glow.
//  - "languagemenu": the chat's "Say:" picker of a dwarf in a party, with the languages it speaks (Dwarvish
//  and
//    Common: ChatMenu's Language submenu), Dwarvish chosen.
// Taps go to the log.
class ExtrasGallery {
public:
    enum class Page : u8 { Languages, SpellBookFlash, LanguageMenu };
    bool select(const char* name);
    void setLanguageData(const game::LanguageData* data) { m_languageData = data; }
    void draw(DialogContext& c, u64 nowMs);

private:
    void buildChat();
    void drawLanguages(DialogContext& c, u64 nowMs);
    void drawSpellBook(DialogContext& c, u64 nowMs);
    void drawLanguageMenu(DialogContext& c);

    Page m_page = Page::Languages;
    bool m_ready = false;
    const game::LanguageData* m_languageData = nullptr;
    game::LanguageData m_sampleLanguages; // without the game's tables
    // The chat page: a session of its own (the player, its skills, the packets).
    std::unique_ptr<game::EntityStore> m_store;
    std::unique_ptr<game::Gameplay> m_play;
    std::vector<std::string> m_texts;
    std::vector<ChatFrameLine> m_lines;
    ChatFrameState m_chatState;
    // The spell book page.
    SpellBookFlash m_flash;
    u32 m_tab = 0;
    // The language menu page.
    std::array<std::string, 2> m_languageChoices{};
    u32 m_speaking = 0; // 0 Dwarvish, 1 Common
};

} // namespace vd
