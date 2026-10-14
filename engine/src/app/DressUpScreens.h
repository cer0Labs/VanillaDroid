#pragma once

#include "app/Dialogs.h"
#include "app/DressUpWindows.h"
#include "app/ModelFrame.h"
#include "app/UiSound.h"
#include "core/Types.h"
#include "game/Appearance.h"

#include <array>

namespace vd {

class GameSession;
class ModelViews;
namespace game {
class AppearanceDb;
class GameData;
} // namespace game

// Phase 15 (items): the game's side of the dressing rooms (app/DressUpWindows): DressUpItemLink from any
// item's tooltip (its "Try On": the desktop's Ctrl + click) opens DressUpFrame on the player
// (DressUpModel:SetUnit) and puts the item on (DressUpModel:TryOn, game/TryOn); while the auction house is
// open it goes to the AuctionDressUpFrame docked beside it instead (Blizzard_AuctionDressUp.lua's
// DressUpItem), which closes with the auction house. Reset dresses the model as the player is now. The model
// frames come from app/ModelViews (ModelViewId::DressUp, AuctionDressUp). Main thread; the looks are rebuilt
// only when what they show changes.
class DressUpScreens {
public:
    void setModelViews(ModelViews* views) { m_views = views; }
    // The tables that turn a character look into a model (world/Actors' AppearanceDb); `data`: the spells
    // (dual wield). Either may be null (no model then, a one-hand weapon always to the main hand).
    void setAppearance(const game::AppearanceDb* appearance) { m_appearance = appearance; }
    void setData(const game::GameData* data) { m_data = data; }
    void setUiSoundSink(UiSoundSink sink) { m_uiSound = sink; }

    // DressUpItemLink(item `entry`).
    void tryOn(GameSession& session, u32 entry);
    // DressUpFrame (UiPanel::DressUp: app/GameScreens shows it by the panel rules).
    bool open() const { return m_rooms[0].open; }
    void close() { closeRoom(m_rooms[0]); }
    // AuctionDressUpFrame: docked right of the auction house while it is open.
    bool auctionOpen() const { return m_rooms[1].open; }
    void closeAuction() { closeRoom(m_rooms[1]); }
    // Once a frame: the auction house's closes with it; looks waiting for item templates are completed.
    void update(GameSession& session);
    // DressUpFrame in the panel place of `c`.
    void draw(GameSession& session, DialogContext& c);
    // AuctionDressUpFrame in `place`.
    void drawAuction(GameSession& session, DialogContext& c, const ui::Rect& place);
    // The width the auction house gives its dressing room (logical pixels) in a place `width` wide.
    static f32 auctionDockWidth(const DialogContext& c, f32 width);

private:
    struct Room {
        bool open = false;
        bool based = false; // the player's look was read (SetUnit: once, when the frame opens; Reset: again)
        game::CharacterLook base;    // SetUnit("player") / Dress
        std::array<u32, 24> tried{}; // TryOn's items since, in order (re-applied over the base)
        u32 triedCount = 0;
        bool dirty = true;       // the model's look must be rebuilt
        bool incomplete = false; // built without some item template: rebuilt as answers arrive
        game::ActorLook look;
        u32 queryVersion = 0;  // the item answers it was built with
        ModelFrameState frame; // kept while closed: the rotation stays as the player left it
    };
    static void closeRoom(Room& room);
    void openRoom(GameSession& session, Room& room);
    void rebuild(GameSession& session, Room& room);
    bool canDualWield(GameSession& session) const;
    DressUpView view(GameSession& session, Room& room, u32 viewId);
    void act(GameSession& session, Room& room, const DressUpResult& r);

    ModelViews* m_views = nullptr;
    const game::AppearanceDb* m_appearance = nullptr;
    const game::GameData* m_data = nullptr;
    UiSoundSink m_uiSound;
    std::array<Room, 2> m_rooms{};   // DressUpFrame, AuctionDressUpFrame
    std::array<bool, 2> m_wasOpen{}; // last frame (OnShow / OnHide sounds)
};

} // namespace vd
