#pragma once

namespace vd {

struct DialogContext;

// Phase 22 (apppack): VD_UI_GALLERY=apponly, what the touch UI can draw with app.vdpk alone (Game does not
// open ui.vdpk for this page: a phone without data yet). Our font (ui::FontId::App) and our icons over plain
// shapes, no client art: the base of the first-run screen (ROADMAP 22.4).
void appOnlyGalleryPage(DialogContext& c);

} // namespace vd
