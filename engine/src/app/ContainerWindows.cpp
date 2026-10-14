#include "app/ContainerWindows.h"

#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "ui/Widgets.h"

#include <algorithm>

namespace vd {
namespace {

using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::widgetId;

// The backgrounds stream from textures.vdpk (UiResources::texture): four 256-unit textures would take a tenth
// of an atlas page, and only the open frames need them.
constexpr u64 kBagComponents = assetPathHash("Interface\\ContainerFrame\\UI-Bag-Components.blp");
constexpr u64 kBagComponentsBank = assetPathHash("Interface\\ContainerFrame\\UI-Bag-Components-Bank.blp");
constexpr u64 kBagComponentsKeyring =
    assetPathHash("Interface\\ContainerFrame\\UI-Bag-Components-Keyring.blp");
constexpr u64 kBackpackBackground = assetPathHash("Interface\\ContainerFrame\\UI-BackpackBackground.blp");

u64 artTexture(ContainerArt art) {
    switch (art) {
    case ContainerArt::Backpack: return kBackpackBackground;
    case ContainerArt::Bank: return kBagComponentsBank;
    case ContainerArt::Keyring: return kBagComponentsKeyring;
    default: return kBagComponents;
    }
}

u64 cellId(i32 container, u32 slot) {
    return static_cast<u64>(container + 16) * 64u + slot;
}

// A rectangle of the frame (units from its top left) on the screen.
Rect at(const Rect& frame, f32 scale, const Rect& r) {
    return {frame.x + r.x * scale, frame.y + r.y * scale, r.w * scale, r.h * scale};
}

} // namespace

void containerFrames(DialogContext& c, std::span<const ContainerFrameView> frames,
                     std::span<const Rect> rects, f32 scale, ItemDrag& drag, ItemWindowResult& items,
                     ContainerFramesResult& out) {
    VD_PROFILE_ZONE("ContainerWindows::frames");
    Gui g{c.ui, c.draw};
    const u32 count = static_cast<u32>(std::min(frames.size(), rects.size()));
    // The backgrounds first (the portrait under the art, as its BACKGROUND layer): one texture after another
    // keeps the draws together; the frames take the touches that miss their buttons.
    for (u32 i = 0; i < count; ++i) {
        const ContainerFrameView& v = frames[i];
        const Rect& frame = rects[i];
        ui::hit(g, widgetId("container.frame", static_cast<u64>(v.id + 16)), frame);
        const ContainerGeometry geometry = containerGeometry(v.id, static_cast<u32>(v.cells.size()));
        if (v.portrait != 0) {
            const Rect portrait = at(frame, scale, kContainerPortrait);
            if (const TextureHandle icon = c.resources.texture(v.portrait); icon.valid()) {
                g.draw.textureInCircle(portrait, icon, portrait.centerX(), portrait.centerY(),
                                       portrait.w * 0.5f);
            }
        }
        const TextureHandle art = c.resources.texture(artTexture(geometry.art));
        if (!art.valid()) {
            g.draw.solid(frame.inset(scale * 2.0f), rgba(12, 12, 12, 210)); // still on its way
            continue;
        }
        for (u32 p = 0; p < geometry.pieceCount; ++p) {
            const ContainerPiece& piece = geometry.pieces[p];
            const Rect r{frame.x - kContainerArtLeft * scale, frame.y + piece.y * scale, 256.0f * scale,
                         piece.h * scale};
            g.draw.texture(r, art, ui::kWhite, {0.0f, piece.v0, 1.0f, piece.v1});
        }
    }
    for (u32 i = 0; i < count; ++i) {
        const ContainerFrameView& v = frames[i];
        const Rect& frame = rects[i];
        const ContainerGeometry geometry = containerGeometry(v.id, static_cast<u32>(v.cells.size()));
        // The name (GameFontHighlight, 112 x 12 at 47, -10) and the close button (ToggleBag).
        ui::TextStyle name = ui::gameFont(12.0f * scale, ui::kWhite);
        ui::label(g, at(frame, scale, kContainerName), v.name, name);
        if (ui::closeButton(g, widgetId("container.close", static_cast<u64>(v.id + 16)),
                            at(frame, scale, kContainerClose))) {
            out.closed = v.id;
        }
        // PortraitButton: its tooltip (the bag's) on the touch screen's hover, a long press, or a tap.
        const Rect portrait = at(frame, scale, kContainerPortrait);
        const ui::WidgetState state =
            ui::hit(g, widgetId("container.portrait", static_cast<u64>(v.id + 16)), portrait);
        if (ui::tapped(state) || state.longPress) {
            out.portrait = v.id;
            out.portraitRect = portrait;
        }
        if (geometry.art == ContainerArt::Backpack) { // its SmallMoneyFrame
            const f32 size = 12.0f * scale;
            ui::money(g, frame.x + kContainerMoneyRight * scale,
                      frame.y + (kContainerMoneyTop + 7.0f) * scale, v.money, size, ui::Align::Right);
        }
        for (u32 slot = 0; slot < v.cells.size(); ++slot) {
            ItemSlotView cell = v.cells[slot];
            cell.bagArt = true;
            itemCell(c, g, widgetId("container.cell", cellId(v.id, slot)),
                     at(frame, scale, containerItemRect(geometry, slot)), cell, drag, items);
        }
        if (drag.droppedIn(frame)) { // on the frame, off its buttons: into the bags
            items.dropFrom = drag.from;
            items.dropZone = 0;
            drag.taken = true;
        }
    }
}

} // namespace vd
