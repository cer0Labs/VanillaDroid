#include "app/DressUpWindows.h"

#include "app/DialogKit.h"
#include "app/UiResources.h"
#include "core/Hash.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "ui/Widgets.h"

#include <algorithm>

namespace vd {
namespace {

using kit::Metrics;
using ui::Gui;
using ui::Rect;
using ui::rgba;
using ui::widgetId;

constexpr u64 backgroundHash(const char* race, char part) {
    u64 hash = assetPathHash("Interface\\DressUpFrame\\DressUpBackground-");
    for (const char* c = race; *c; ++c) {
        hash ^= static_cast<u8>(normalizeAssetPathChar(*c));
        hash *= kFnvPrime;
    }
    hash ^= static_cast<u8>(part);
    hash *= kFnvPrime;
    for (const char c : std::string_view(".blp")) {
        hash ^= static_cast<u8>(c);
        hash *= kFnvPrime;
    }
    return hash;
}
static_assert(backgroundHash("Human", '1') ==
              assetPathHash("Interface\\DressUpFrame\\DressUpBackground-Human1.blp"));

// DressUpTexturePath: ChrRaces' file names, Gnomes with the Dwarf's room and Trolls with the Orc's
// ("HACK!!!").
constexpr const char* kRaceFiles[9] = {"Orc",     "Human",  "Orc",   "Dwarf", "NightElf",
                                       "Scourge", "Tauren", "Dwarf", "Orc"};

// DressUpFrame.xml: the backgrounds' 318 units across (256 + 62) and 255 + 128 down, the lower ones painted
// only in their top 75 rows (the rest is clear over the frame's art, where Reset and Close are:
// Blizzard_AuctionDressUp.xml shows 0.588 of them); the model stands over them.
constexpr f32 kStageW = 318.0f, kStageH = 255.0f + 75.0f, kBottomRows = 75.0f / 128.0f;
// Blizzard_AuctionDressUp.xml: 171 units wide, 282 + 83 tall, the left 0.61 of the textures.
constexpr f32 kAuctionStageW = 171.0f, kAuctionStageH = 365.0f;

// `w` x `h` fitted in `r` keeping its aspect, centered.
Rect fit(const Rect& r, f32 w, f32 h) {
    const f32 k = std::min(r.w / w, r.h / h);
    return {r.x + (r.w - w * k) * 0.5f, r.y + (r.h - h * k) * 0.5f, w * k, h * k};
}

void piece(DialogContext& c, const Rect& r, u64 texture, ui::TexCoords uv) {
    if (const TextureHandle t = texture ? c.resources.texture(texture) : TextureHandle{}; t.valid()) {
        c.draw.texture(r, t, ui::kWhite, uv);
    }
}

} // namespace

u64 dressUpBackground(u8 race, u32 part) {
    const char digit = static_cast<char>('0' + std::clamp<u32>(part, 1, 4));
    return backgroundHash(kRaceFiles[race < 9 ? race : 0], digit);
}

DressUpResult dressUpWindow(DialogContext& c, const DressUpView& v, ModelFrameState& model) {
    VD_PROFILE_ZONE("DressUpWindows::dressUp");
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    const bool wide = c.profile == ScreenProfile::Wide;
    DressUpResult result;
    const kit::Dialog d = kit::beginDialog(c, g, "dressup", wide ? 620.0f : 420.0f, wide ? 420.0f : 600.0f,
                                           v.title, true, true);
    result.closed = d.closed || d.outside;
    // The stage (backgrounds and model) and the description: side by side in a wide place, stacked otherwise.
    const f32 gap = g.px(8.0f);
    const ui::TextStyle note = ui::gameFont(m.small, ui::kGold); // GameFontNormalSmall
    Rect stage = d.body, text = d.body;
    if (d.body.w > d.body.h * 1.15f) {
        stage.w = std::min(d.body.h * kStageW / kStageH, d.body.w * 0.62f);
        text = {stage.right() + gap, d.body.y, d.body.right() - stage.right() - gap, d.body.h};
    } else {
        const f32 h = g.draw.measureWrapped(d.body.w, v.instructions, note.font, note.size);
        text.h = h;
        stage = {d.body.x, d.body.y + h + gap, d.body.w, d.body.h - h - gap};
    }
    g.draw.wrappedText(text, v.instructions, note);
    const Rect bg = fit(stage, kStageW, kStageH);
    const f32 k = bg.w / kStageW;
    g.draw.solid(bg, rgba(0, 0, 0, 255)); // while they load
    piece(c, {bg.x, bg.y, 256.0f * k, 255.0f * k}, v.backgrounds[0], {});
    piece(c, {bg.x + 256.0f * k, bg.y, 62.0f * k, 255.0f * k}, v.backgrounds[1], {});
    piece(c, {bg.x, bg.y + 255.0f * k, 256.0f * k, 75.0f * k}, v.backgrounds[2],
          {0.0f, 0.0f, 1.0f, kBottomRows});
    piece(c, {bg.x + 256.0f * k, bg.y + 255.0f * k, 62.0f * k, 75.0f * k}, v.backgrounds[3],
          {0.0f, 0.0f, 1.0f, kBottomRows});
    result.rotated = modelFrame(c, g, widgetId("dressup.model"), bg, v.model, model);
    // Reset left of Close, at the bottom right (DressUpFrameResetButton anchored to
    // DressUpFrameCancelButton).
    const std::string_view reset = text::get("RESET"), close = text::get("CLOSE");
    const f32 bw = std::min(d.footer.w * 0.48f, std::max(kit::buttonWidth(g, reset, d.footer.h),
                                                         kit::buttonWidth(g, close, d.footer.h)));
    const Rect closeRect{d.footer.right() - bw, d.footer.y, bw, d.footer.h};
    result.reset =
        ui::button(g, widgetId("dressup.reset"), {closeRect.x - bw - gap, d.footer.y, bw, d.footer.h}, reset);
    result.closed = ui::button(g, widgetId("dressup.close"), closeRect, close) || result.closed;
    return result;
}

DressUpResult auctionDressUpWindow(DialogContext& c, const Rect& place, const DressUpView& v,
                                   ModelFrameState& model) {
    VD_PROFILE_ZONE("DressUpWindows::auction");
    Gui g{c.ui, c.draw};
    const Metrics m = kit::metrics(c);
    DressUpResult result;
    ui::hit(g, widgetId("auctionDressUp.panel"), place);
    ui::dialogPanel(g, place);
    const Rect content = ui::dialogContent(g, place);
    const f32 size = g.px(32.0f);
    result.closed =
        ui::closeButton(g, widgetId("auctionDressUp.close"),
                        {place.right() - size - g.unit(4.0f), place.y + g.unit(4.0f), size, size});
    // The model over the left parts of backgrounds 1 and 3, Reset under it.
    const f32 gap = g.px(6.0f);
    const Rect stage{content.x, content.y + size * 0.5f, content.w, content.h - size * 0.5f - m.button - gap};
    const Rect bg = fit(stage, kAuctionStageW, kAuctionStageH);
    const f32 k = bg.w / kAuctionStageW;
    g.draw.solid(bg, rgba(0, 0, 0, 255));
    piece(c, {bg.x, bg.y, bg.w, 282.0f * k}, v.backgrounds[0], {0.0f, 0.0f, 0.61f, 1.0f});
    piece(c, {bg.x, bg.y + 282.0f * k, bg.w, 83.0f * k}, v.backgrounds[2], {0.0f, 0.0f, 0.61f, 0.588f});
    result.rotated = modelFrame(c, g, widgetId("auctionDressUp.model"), bg, v.model, model);
    const std::string_view reset = text::get("RESET");
    const f32 bw = std::min(content.w, kit::buttonWidth(g, reset, m.button));
    result.reset =
        ui::button(g, widgetId("auctionDressUp.reset"),
                   {content.centerX() - bw * 0.5f, content.bottom() - m.button, bw, m.button}, reset);
    return result;
}

} // namespace vd
