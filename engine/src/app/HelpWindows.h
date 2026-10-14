#pragma once

#include "app/Dialogs.h"
#include "core/Text.h"
#include "core/Types.h"
#include "ui/Context.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

// Phase 15 (help): the help windows as pure views (a view-model in, what the player did out): HelpFrame's
// pages (HelpFrame.xml / .lua: Home, the GM categories, the general pages of each category with their
// bullets, the harassment page, the ticket page), TicketStatusFrame (the status line at the top right while a
// ticket is open, or the GM survey offered), Blizzard_GMSurveyUI's GMSurveyFrame and SuggestFrame (bugs and
// suggestions). Interface words by key (core/Text: the client's HELPFRAME_* / GM_TICKET_* / GMSURVEY_*
// strings, VD_HELP_* of locale/*/help.lang).

// HelpFrame_ShowFrame's frames.
enum class HelpPage : u8 { Home, Categories, General, Harassment, OpenTicket };

struct HelpCategoryView {
    std::string_view name; // GMTicketCategory.dbc
    std::string_view
        description; // empty for the table's (GMCategoryButtonTemplate's description stays blank)
};

// A general page (GENERAL_HELPFRAME[category]): the keys of its texts, every part optional (nullptr: none).
struct HelpGeneralView {
    const char* title = nullptr;
    const char* titleText = nullptr;
    const char* bulletTitle1 = nullptr;
    std::span<const char* const> bullets1;
    const char* bulletTitle2 = nullptr;
    std::span<const char* const> bullets2;
    const char* endText = nullptr;
    const char* button = nullptr;  // HelpFrameGeneralButton (OpenTicket, or Auto-Unstuck)
    const char* button2 = nullptr; // HelpFrameGeneralButton2
};

// GENERAL_HELPFRAME[category] (HelpFrame.lua): the general page of a GMTicketCategory id (2, harassment, has
// its own frame: an empty view, as for an unknown id).
HelpGeneralView helpGeneralPage(u32 category);

struct HelpView {
    HelpPage page = HelpPage::Home;
    std::span<const HelpCategoryView> categories; // the GM page
    HelpGeneralView general;                      // the General page
    // The ticket page (HelpFrameOpenTicket): an open ticket edits ("Save Changes" / "Exit"), else a new one.
    bool hasTicket = false;
    std::string_view ticketText;
    bool editing = false;
};

struct HelpResult {
    bool closed = false;       // the X or Cancel
    bool back = false;         // Back (to the categories, or Home)
    bool toCategories = false; // Home's "Issues that GMs can assist with"
    i32 category = -1;         // a category tapped (its index in `categories`)
    bool generalButton = false, generalButton2 = false;
    bool reportVerbal = false, reportPhysical = false; // the harassment page's buttons (OpenTicket)
    bool ticketTapped = false;                         // the ticket's text box
    bool submit = false;                               // Submit / Save Changes
};

HelpResult helpWindow(DialogContext& c, const HelpView& v);

// --- TicketStatusFrame
// -----------------------------------------------------------------------------------------

struct TicketStatusView {
    std::string_view title; // TICKET_STATUS1, or CHOSEN_FOR_GMSURVEY
    std::string_view time;  // the wait's text (empty: none, as for the survey)
    bool flash = false;     // the survey: UIFrameFlash(TicketStatusButton)
};
// Kept between frames: TicketStatusFrame is movable (OnMouseDown StartMoving); here a finger drags it,
// from 1.12's place over the BuffFrame's first row to wherever the player wants it this session.
struct TicketStatusState {
    f32 dx = 0.0f, dy = 0.0f;         // the drag's offset from the frame's place
    f32 grabDx = 0.0f, grabDy = 0.0f; // the offset when the finger went down
};
// The frame (208 x 52 units) at the right end of `area`'s top, moved by `state`; returns true when tapped
// (TicketStatusButton / OnMouseUp; a drag is no tap). `nowMs` drives the flash.
bool ticketStatusFrame(DialogContext& c, const ui::Rect& area, const TicketStatusView& v,
                       TicketStatusState& state, u64 nowMs);

// --- GMSurveyFrame
// ---------------------------------------------------------------------------------------------

struct SurveyQuestionView {
    std::string_view text;
    i32 selected = -1; // 0 (N/A) .. 5
};
struct GmSurveyView {
    std::span<const SurveyQuestionView> questions;
    std::string_view comment;
    bool editing = false;
};
struct GmSurveyResult {
    bool closed = false; // the X or Cancel
    bool submit = false;
    i32 question = -1, answer = -1; // a radio button tapped
    bool commentTapped = false;
};
GmSurveyResult gmSurveyWindow(DialogContext& c, const GmSurveyView& v);

// --- SuggestFrame
// ----------------------------------------------------------------------------------------------

struct SuggestView {
    std::string_view playerInfo; // SuggestFramePlayerInfo: name, race, class, level, location
    std::string_view category;   // the drop-down's choice (BUG_CATEGORY_CHOOSE until one is chosen)
    std::string_view text;
    bool editing = false;
    bool bugEnabled = true, suggestEnabled = true;
};
struct SuggestResult {
    bool closed = false;
    bool categoryMenu = false;
    bool textTapped = false;
    bool sendBug = false, sendSuggestion = false;
};
SuggestResult suggestWindow(DialogContext& c, const SuggestView& v);

} // namespace vd
