#include "app/AppOnlyGallery.h"

#include "app/Dialogs.h"
#include "app/HudShared.h"
#include "core/Text.h"
#include "ui/DrawList.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <iterator>

namespace vd {

void appOnlyGalleryPage(DialogContext& c) {
    using ui::rgba;
    ui::DrawList& draw = c.draw;
    const ui::Rect& safe = c.safeArea;
    const f32 u = c.ui.px(1.0f);
    draw.solid({0.0f, 0.0f, c.ui.width(), c.ui.height()}, rgba(14, 16, 22));

    const f32 width = std::min(safe.w - 48.0f * u, 720.0f * u);
    const f32 left = safe.x + (safe.w - width) * 0.5f;
    f32 y = safe.y + 40.0f * u;

    // The title and a paragraph in our font (with its Latin-1 and Latin Extended-A letters in Spanish).
    ui::TextStyle title;
    title.font = ui::FontId::App;
    title.size = 44.0f * u;
    title.color = ui::kWhite;
    title.align = ui::Align::Center;
    y += draw.ascent(title.font, title.size);
    draw.text(left + width * 0.5f, y, text::get("VD_APP_NAME"), title);
    y += draw.lineHeight(title.font, title.size) - draw.ascent(title.font, title.size) + 20.0f * u;

    ui::TextStyle body;
    body.font = ui::FontId::App;
    body.size = 22.0f * u;
    body.color = rgba(214, 220, 230);
    y += draw.wrappedText({left, y, width, 400.0f * u}, text::get("VD_TOUCH_OPTION_CONTROLS_TOOLTIP"), body);
    y += 36.0f * u;

    // Our icons on round buttons, tinted as the touch cluster tints them, with their names.
    struct Glyph {
        u64 sprite;
        text::Key name;
    };
    const Glyph glyphs[] = {{hud::kTouchGlyphJump, "VD_TOUCH_EDIT_PLACE_JUMP"},
                            {hud::kTouchGlyphMount, "VD_TOUCH_EDIT_PLACE_MOUNT"},
                            {hud::kTouchGlyphTarget, "VD_TOUCH_EDIT_PLACE_TARGET"}};
    const f32 radius = 44.0f * u;
    const f32 step = width / static_cast<f32>(std::size(glyphs));
    ui::TextStyle label = body;
    label.size = 18.0f * u;
    label.align = ui::Align::Center;
    for (usize i = 0; i < std::size(glyphs); ++i) {
        const f32 cx = left + step * (static_cast<f32>(i) + 0.5f);
        const f32 cy = y + radius;
        draw.ring(cx, cy, radius, 0.0f, rgba(32, 38, 52, 235));
        draw.ring(cx, cy, radius, 0.92f, rgba(150, 160, 180));
        const f32 g = radius * 0.62f;
        draw.sprite({cx - g, cy - g, 2.0f * g, 2.0f * g}, glyphs[i].sprite, rgba(255, 220, 140));
        draw.text(cx, cy + radius + 12.0f * u + draw.ascent(label.font, label.size),
                  text::get(glyphs[i].name), label);
    }
}

} // namespace vd
