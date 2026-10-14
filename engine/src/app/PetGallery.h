#pragma once

#include "app/CombatWindows.h"
#include "app/PetWindows.h"
#include "core/Types.h"
#include "game/CombatFeedback.h"

#include <array>
#include <string_view>

namespace vd {

class ModelFrameGallery;

// Development (phase 15, pets): the pet windows and HUD pieces with sample data of a hunter's level-20 wolf,
// for UiGallery (VD_UI_GALLERY=petbook, petbar, partypets, stable, petpaperdoll, feedback). What the player
// does goes to the log ("UI gallery: ..."). The model frames show the gallery's sample wolf
// (app/ModelFrameGallery) once a map is loaded (`--map Azeroth --pos -9050,440`).
class PetGallery {
public:
    enum class Page : u8 { Book, Bar, PartyPets, Stable, PaperDoll, Feedback, Count };

    void setModels(ModelFrameGallery* models) { m_models = models; }
    void draw(DialogContext& c, Page page, u64 nowMs);

private:
    void book(DialogContext& c);
    void bar(DialogContext& c, u64 nowMs);
    void partyPets(DialogContext& c);
    void stable(DialogContext& c);
    void paperDoll(DialogContext& c);
    void feedback(DialogContext& c, u64 nowMs);

    ModelFrameGallery* m_models = nullptr;
    // The pet's page.
    i32 m_bookSelected = 0;
    std::array<bool, 4> m_autocast{true, false, false, false};
    // The pet bar.
    std::array<u32, 10> m_bar{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}; // sample button per slot (dragging swaps)
    bool m_placing = false;
    u32 m_reaction = 1;
    // The stable.
    i32 m_stableSelected = 0;
    i32 m_stablePicked = -1;
    u32 m_stableSlots = 1;
    ModelFrameState m_stableModel;
    // The Pet tab.
    PetPaperDollState m_paperDoll;
    // The hit indicators (a sample UNIT_COMBAT every second, in turn).
    game::CombatFeedback m_playerFeedback;
    game::CombatFeedback m_petFeedback;
    u64 m_nextFeedbackMs = 0;
    u32 m_feedbackStep = 0;
};

} // namespace vd
