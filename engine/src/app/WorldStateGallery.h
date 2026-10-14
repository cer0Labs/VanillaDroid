#pragma once

#include "app/Dialogs.h"
#include "app/WorldStateScreens.h"
#include "core/Types.h"

#include <memory>
#include <string_view>

namespace vd::game {
class EntityStore;
class Gameplay;
} // namespace vd::game

namespace vd {

class UiResources;

// Development (phase 17, scores): VD_UI_GALLERY=worldstate / score / score2 / victory / bgminimap: the world
// state frames with sample world states and scores, filled by app/WorldStateScreens from a game::Gameplay of
// their own as the game fills them (the client's WorldStateUI.dbc through the game's screens: setSource):
//  - worldstate: Warsong Gulch's two lines (the Horde flag carried: the Alliance line's flag flashing, its
//    tooltip held), Arathi Basin's two under them, and an Eastern Plaguelands capture bar under the minimap's
//    place.
//  - score: WorldStateScoreFrame in Warsong Gulch, 20 players in the middle of the battle (flag captures,
//    returns).
//  - score2: Alterac Valley's 80 players with its seven columns, scrolled down 30 lines.
//  - victory: Warsong Gulch over, the Alliance's banner, Leave Battleground and the closing timer.
//  - bgminimap: BattlefieldMinimap of Warsong Gulch with a raid's dots, two teammates out of it and the
//    carrier of the Horde flag (1.12.1 shows one carrier at most: the packet's), its options menu open.
class WorldStateGallery {
public:
    WorldStateGallery();
    ~WorldStateGallery();
    // A page's name (true when it is one of these).
    bool select(std::string_view page);
    void setSource(const WorldStateScreens* game, UiResources* resources);
    void draw(DialogContext& c, u64 nowMs);

private:
    enum class Page : u8 { WorldState, Score, Score2, Victory, BgMinimap };
    void build(u32 nowMs);
    void drawWorldState(DialogContext& c, u64 nowMs);

    Page m_page = Page::WorldState;
    const WorldStateScreens* m_source = nullptr;
    UiResources* m_resources = nullptr;
    std::unique_ptr<game::EntityStore> m_store;
    std::unique_ptr<game::Gameplay> m_play;
    WorldStateScreens m_screens;
    SettingsView m_settings;
    bool m_built = false;
    // The page's frames' CPU time (logged every 600 frames).
    u32 m_costFrames = 0;
    f32 m_costSum = 0.0f, m_costWorst = 0.0f;
};

} // namespace vd
