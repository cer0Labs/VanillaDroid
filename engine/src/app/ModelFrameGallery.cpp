#include "app/ModelFrameGallery.h"

#include "core/Profile.h"
#include "ui/Widgets.h"
#include "world/Actors.h"

#include <algorithm>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;

// A player character (app/PortraitGallery's samples): race, gender, a little customization; armored = the helm and
// shoulder pads of a Stormwind guard.
game::CharacterLook character(u8 race, u8 gender, u8 variant, bool armored) {
    game::CharacterLook look;
    look.race = race;
    look.gender = gender;
    look.skin = variant;
    look.face = variant;
    look.hairStyle = static_cast<u8>(variant + 1);
    look.hairColor = variant;
    look.facialHair = variant;
    if (armored) {
        look.items[game::kSlotHead] = 8806;
        look.inventoryTypes[game::kSlotHead] = 1; // INVTYPE_HEAD
        look.items[game::kSlotShoulders] = 11690;
        look.inventoryTypes[game::kSlotShoulders] = 3; // INVTYPE_SHOULDERS
    }
    return look;
}

} // namespace

const game::ActorLook* ModelFrameGallery::look(Sample sample) {
    if (!m_ready) {
        if (!m_actors || !m_actors->isOpen() || !m_actors->appearance().ready()) {
            return nullptr;
        }
        const game::AppearanceDb& db = m_actors->appearance();
        m_looks[static_cast<usize>(Sample::Human)] = db.character(character(1, 0, 1, true));
        m_looks[static_cast<usize>(Sample::Orc)] = db.character(character(2, 1, 2, false));
        m_looks[static_cast<usize>(Sample::Wolf)] = db.creature(165); // Timber Wolf
        m_ready = true;
    }
    return sample < Sample::Count ? &m_looks[static_cast<usize>(sample)] : nullptr;
}

TextureHandle ModelFrameGallery::sample(ModelViewId id, Sample which, f32 facing) {
    const game::ActorLook* l = look(which);
    return l && m_views ? m_views->show(id, *l, facing) : TextureHandle{};
}

void ModelFrameGallery::draw(DialogContext& c) {
    VD_PROFILE_ZONE("ModelFrameGallery::draw");
    ui::Gui g{c.ui, c.draw};
    const kit::Dialog d = kit::beginDialog(c, g, "gallery.modelframe", 620.0f, 420.0f, "", false, false);
    // Two frames side by side over the window's art: the human (CharacterModelFrame) and the wolf (a pet's model).
    const f32 gap = g.px(8.0f);
    const Rect left{d.body.x, d.body.y, (d.body.w - gap) * 0.5f, d.body.h};
    const Rect right{left.right() + gap, d.body.y, left.w, d.body.h};
    g.draw.solid(left, rgba(40, 30, 20, 255));
    g.draw.solid(right, rgba(20, 30, 40, 255));
    modelFrame(c, g, ui::widgetId("gallery.model", 0), left,
               sample(ModelViewId::Character, Sample::Human, m_frames[0].facing), m_frames[0]);
    modelFrame(c, g, ui::widgetId("gallery.model", 1), right,
               sample(ModelViewId::PetPaperDoll, Sample::Wolf, m_frames[1].facing), m_frames[1]);
    // The third, small, at the screen's bottom right over the world (or the gallery's backdrop).
    const Rect& safe = c.safeArea;
    const f32 h = std::min(g.px(220.0f), safe.h * 0.3f);
    const Rect small{safe.right() - h * 0.8f - g.px(8.0f), safe.bottom() - h - g.px(8.0f), h * 0.8f, h};
    modelFrame(c, g, ui::widgetId("gallery.model", 2), small,
               sample(ModelViewId::Gallery, Sample::Orc, m_frames[2].facing), m_frames[2]);
}

} // namespace vd
