#pragma once

#include "app/Dialogs.h"
#include "app/Hud.h"
#include "app/TargetingWindows.h"
#include "core/Types.h"
#include "game/GameData.h"
#include "game/Inventory.h"
#include "game/SpellTargeting.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace vd {

class GameSession;

// Phase 13 (spells): the touch side of game::Gameplay::targeting(), a spell waiting for a place or an item
// (the desktop's targeting cursor: SpellIsTargeting).
//
// A place (Blizzard, Flamestrike, Rain of Fire, Hurricane, Volley, Flare, bombs): a circle on the ground as
// wide as the spell's area (SpellRadius.dbc with SPELLMOD_RADIUS), drawn by the game like the selection
// circle (1.12 only changes the cursor: the circle is this client's), starting a few yards in front of the
// player. A quick tap on the world moves it there, a finger that lands on it drags it over the ground; it
// turns red beyond the spell's range (SpellRange.dbc with SPELLMOD_RANGE, the server's rule). A tap on the
// circle or the bar's Cast button casts: a confirmation, not the release of the drag, so a finger meant for
// the camera or a misplaced drop never spends the mana and the cooldown, and the place can be adjusted (the
// camera and the stick keep working meanwhile). Cancel, and any window that opens, drop it.
//
// An item (poisons, sharpening stones, enchanting, Disenchant, Feed Pet, Pick Lock): the bags open (and the
// character window when the spell wants a weapon or armor), the items it suits lit and the rest dimmed
// (game::itemFitsSpell); a tap on one casts (or asks first: app/PopupScreens shows BIND_ENCHANT /
// REPLACE_ENCHANT); the trader's "will not be traded" slot takes it too, and a locked object in the world for
// Pick Lock. GameScreens owns it and calls it at fixed points of its frame; main thread, nothing allocates.
class SpellTargetingScreens {
public:
    void init(const game::GameData* data);

    // --- Before the HUD draws ---
    // Follows the targeting; the camera, the projection and the ground picker of this frame; the HUD's quick
    // taps on the world come here while a place is chosen (hud.groundTargeting).
    void prepareHud(GameSession& session, HudFrame& hud, u32 nowMs);

    // --- After the HUD drew ---
    // A tap on a game object: Pick Lock's target. True when it took the tap (no other use of it).
    bool actOnObject(GameSession& session, u64 gameObject);
    // The circle's drag widget and the world taps that move it (under the windows: call before the panels).
    void drawWorld(GameSession& session, DialogContext& c, const HudOutput& out);
    // The bar with Cast and Cancel (over the panels).
    // `actionBar`: the HUD's action cluster (the bar goes over it).
    void drawBar(GameSession& session, DialogContext& c, const ui::Rect& actionBar);
    // The windows shown this frame (a bit each; GameScreens' mask): one that was not there when the place
    // began to be chosen drops it.
    void windowsShown(GameSession& session, u64 mask);

    // The ground circle for the game to draw (the selection circle's texture and pass); false when none.
    struct Circle {
        glm::dvec3 center{0.0};
        f32 radius = 0.0f;
        bool acceptable = true; // in range: Spell-Shadow-Acceptable; else Spell-Shadow-Unacceptable (WoW.exe 0x482154)
    };
    bool circle(Circle& out) const;

    // --- An item to choose (InventoryScreens, ExchangeScreens) ---
    bool pickingItems(const GameSession& session) const;
    // How a cell looks: lit (the spell suits it) or dimmed.
    enum class Mark : u8 { None, Lit, Dim };
    Mark mark(const GameSession& session, const game::ItemInstance* item,
              const net::ItemQueryInfo* info) const;
    // A tap on the player's item at `pos` / on the trader's item that will not be traded.
    void pickItem(GameSession& session, game::ItemPos pos);
    void pickTradeItem(GameSession& session);
    // The windows the item choice wants open this frame (GameScreens opens them, and closes the ones it
    // opened when the choice ends).
    bool wantsBags() const { return m_wantsBags; }
    bool wantsCharacter() const { return m_wantsCharacter; }
    // Whether the choice began this frame / ended (read once).
    bool takeItemChoiceStarted();
    bool takeItemChoiceEnded();

    // Development (VD_TARGETING_DEMO=<spell id>): the place of that spell chosen offline (with --walk), its
    // casts only logged.
    bool demo() const { return m_demoSpell.id != 0; }

    // Phase 20 (assists, docs/MOBILE_CONTROLS.md §4 "Apuntar arrastrando"): a ground spell dragged out of its touch
    // button: its circle where the finger aims, around the player within the spell's range, in the camera's frame
    // (up on the screen is away from the camera). `nx` / `ny`: the finger's offset as a share of the aiming pad
    // (TouchBarOutput::dragNx / dragNy); `mods` the player's spell modifiers and `me` the player (null offline: the
    // HUD's position). Shown in the frames that call it (after prepareHud); aimPoint() is where it aims then (false:
    // not aiming this frame).
    void aimAt(const game::SpellInfo& spell, const game::SpellMods* mods, const game::Entity* me, f32 nx, f32 ny);
    bool aimPoint(glm::dvec3& out) const;

private:
    bool groundActive(const GameSession& session) const;
    const game::SpellInfo* groundSpell(const GameSession& session) const;
    bool project(const glm::dvec3& world, f32& x, f32& y) const;
    // Where the line from the camera through the screen point meets the ground.
    bool pickGround(f32 x, f32 y, glm::dvec3& out) const;
    bool groundBelow(const glm::dvec3& at, glm::dvec3& out) const;
    void placeDefault(const GameSession& session);
    void updateRange(const GameSession& session);
    void confirm(GameSession& session);
    void cancel(GameSession& session);

    const game::GameData* m_data = nullptr;
    bool m_trace = false;
    u32 m_version = 0; // of the targeting last seen
    u32 m_nowMs = 0;
    game::SpellInfo m_demoSpell; // VD_TARGETING_DEMO
    bool m_demoActive = false;
    u32 m_demoRearmAt = 0;

    // This frame's view of the world (prepareHud).
    glm::mat4 m_viewProj{1.0f};
    glm::dvec3 m_camera{0.0};
    f32 m_width = 1.0f, m_height = 1.0f;
    glm::dvec3 m_position{0.0}; // the player
    HudFrame::GroundPick m_pick = nullptr;
    const void* m_pickContext = nullptr;

    // The place.
    bool m_placed = false;
    bool m_groundShown = false; // a place is being chosen this frame
    glm::dvec3 m_center{0.0};
    f32 m_radius = 0.0f;
    f32 m_maxRange = 0.0f; // with SPELLMOD_RANGE
    bool m_inRange = true;
    u64 m_windowsAtStart = 0;
    bool m_windowsKnown = false;

    // Phase 20 (assists): the touch cluster's ground aim (aimAt), shown in the frame it was set in.
    u32 m_frame = 0;
    u32 m_aimFrame = 0xFFFFFFFFu;
    glm::dvec3 m_aimCenter{0.0};
    f32 m_aimRadius = 0.0f;
    bool m_aimInRange = true;

    // The item choice.
    bool m_wantsBags = false;
    bool m_wantsCharacter = false;
    bool m_itemChoice = false;
    bool m_itemChoiceStarted = false;
    bool m_itemChoiceEnded = false;
};

} // namespace vd
