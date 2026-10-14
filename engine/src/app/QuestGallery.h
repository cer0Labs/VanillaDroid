#pragma once

#include "app/Dialogs.h"
#include "app/QuestScreens.h"
#include "core/Types.h"

#include <memory>

namespace vd {

namespace game {
class AreaData;
}

// Development (phase 13, VD_UI_GALLERY=log / questwatch, and VD_HUD_DEMO's quest log): the quest log, the
// QuestWatchFrame and the QuestTimerFrame of a level-12 warrior in Elwynn Forest and Westfall, filled by the
// game's own glue (app/QuestScreens) from an offline game::Gameplay fed with the packets and answers a server
// would send: the player's quest log fields (one quest complete, one failed, one timed), the quest templates,
// the item and creature names. Taps work as in the game (headers fold, Track Quest watches, Abandon and Share
// log what they did).
class QuestGallery {
public:
    QuestGallery();
    ~QuestGallery();
    void setAreas(const game::AreaData* areas) { m_areas = areas; }
    // The quest log in the place of `c` (the gallery's whole screen, or a panel's place). Returns true when
    // closed.
    bool drawLog(DialogContext& c, u32 nowMs);
    // The quest timers at the right under where the minimap goes, the watched quests at the left.
    void drawWatch(DialogContext& c, u32 nowMs);
    // VD_HUD_DEMO: the watched quests alone in the game's place (QuestScreens::drawWatch's arguments).
    void drawWatchAt(DialogContext& c, u32 nowMs, const ui::Rect& place);

private:
    struct Session;
    void build(u32 nowMs);
    QuestScreens::Frame frame(u32 nowMs);

    const game::AreaData* m_areas = nullptr;
    std::unique_ptr<Session> m_session;
    QuestScreens m_screens;
};

} // namespace vd
