#pragma once

#include "app/Dialogs.h"
#include "app/GuildWindows.h"
#include "app/HelpWindows.h"
#include "app/ModelFrame.h"
#include "app/SocialWindows.h"
#include "core/Types.h"
#include "game/Appearance.h"
#include "game/GuildEmblem.h"

#include <array>
#include <string>
#include <vector>

namespace vd {

class Actors;
class ModelViews;

// Development (phase 15,
// VD_UI_GALLERY=guild|guildstatus|guildmember|guildinfo|guildcontrol|registrar|registrar2|
// petition|petition2|tabard|help|help2|help3|help4|help5|gmsurvey|suggest): the guild and help windows with
// sample data, drawn by the same views (app/GuildWindows, app/HelpWindows): a guild of seven on line (the
// social window's guild tab in both views, the member detail with an officer's rights, the information text,
// the guild master's rank control), the registrar's greeting and purchase page, a charter seen by its
// originator and by a signer, the tabard designer with a human wearing the design (its model needs the
// actors' packs: `--map Azeroth --pos -9050,440`), and HelpFrame's pages with an open ticket waiting
// (TicketStatusFrame at the top right), GMSurveyFrame and SuggestFrame. Taps work as in the game where they
// only change the view (rows, headers, check boxes, the design's arrows, the help pages, the survey's
// answers); what would go to the server goes to the log. Sample names are English, as the server and the
// client's tables have them.
class GuildGallery {
public:
    enum class Page : u8 {
        Roster,
        Status,
        Member,
        Info,
        Control,
        Registrar,
        Purchase,
        Petition,
        PetitionSign,
        Tabard,
        Help,
        HelpCategories,
        HelpGeneral,
        HelpHarassment,
        HelpTicket,
        Survey,
        Suggest,
        Count
    };

    // A page by its VD_UI_GALLERY name (false: not one of these).
    bool select(const char* name);
    // "all": the next page; false once past the last (the gallery goes on to its next page).
    bool next();
    void setSources(Actors* actors, ModelViews* views) {
        m_actors = actors;
        m_views = views;
    }
    void draw(DialogContext& c) { drawPage(c, m_page); }
    // One page in the place of `c` (also app/PanelDemo's help window under VD_HUD_DEMO).
    void drawPage(DialogContext& c, Page page);

private:
    void roster(DialogContext& c, Page page);
    void tabard(DialogContext& c);
    void help(DialogContext& c, Page page);
    void survey(DialogContext& c);
    void suggest(DialogContext& c);
    void ticketStatus(DialogContext& c);

    Page m_page = Page::Roster;
    Page m_lastPage = Page::Count;
    Actors* m_actors = nullptr;
    ModelViews* m_views = nullptr;

    // The guild tab.
    bool m_status = false;
    i32 m_selected = 1;
    std::array<bool, 13> m_flags{};
    std::vector<GuildRowView> m_rows;
    std::array<std::array<char, 48>, 8> m_levelCells{};
    std::array<char, 96> m_totals{};
    std::array<char, 64> m_levelClass{};
    // The tabard designer.
    game::GuildEmblem m_design{10, 3, 2, 3, 5}; // a gold crown on red
    game::GuildEmblem m_lookDesign;
    game::ActorLook m_look;
    bool m_lookBuilt = false;
    ModelFrameState m_model;
    // Help.
    HelpPage m_helpPage = HelpPage::Home;
    u32 m_category = 4;
    std::vector<HelpCategoryView> m_categories;
    std::array<char, 128> m_wait{};
    TicketStatusState m_ticketStatus;
    std::array<SurveyQuestionView, 5> m_questions{};
    std::array<char, 96> m_title{};
    std::array<char, 192> m_info{};
};

} // namespace vd
