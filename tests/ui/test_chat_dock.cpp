// The chat frames' timing and tabs (parity, ui/ChatDock): lines fading after ChatFrameTemplate's two minutes,
// UIFrameFlash on a tab, the bottom button's blink, the tab strip's fades and FCF_DockUpdate's widths; and
// where the draw list lays out a link of a wrapped line (ui/DrawList::wrappedSpan, with the baked ui.vdpk).
#include "core/Hash.h"
#include "ui/ChatDock.h"
#include "ui/DrawList.h"
#include "ui/UiAssets.h"
#include "ui/Widgets.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <string>

using namespace vd;
using namespace vd::ui;

TEST_CASE("Chat dock: a line shows two minutes, then fades out over three seconds") {
    const u64 shown = 10000;
    CHECK(chatLineAlpha(shown, shown) == 1.0f);
    CHECK(chatLineAlpha(shown, shown + kChatVisibleMs) == 1.0f);
    CHECK(chatLineAlpha(shown, shown + kChatVisibleMs + kChatFadeMs / 2) == doctest::Approx(0.5f));
    CHECK(chatLineAlpha(shown, shown + kChatVisibleMs + kChatFadeMs) == 0.0f);
    CHECK(chatLineAlpha(shown, shown + 10 * kChatVisibleMs) == 0.0f);
    // Brought back (shown again later): two more minutes from then.
    const u64 revealed = shown + 5 * kChatVisibleMs;
    CHECK(chatLineAlpha(revealed, revealed + 1000) == 1.0f);
    // A shorter time (development) works the same way.
    CHECK(chatLineAlpha(0, 6000, 5000, 2000) == doctest::Approx(0.5f));
    CHECK(chatLineAlpha(0, 4000, 5000, 2000) == 1.0f);
}

TEST_CASE("Chat dock: a tab's flash fades in, holds, fades out and stops after a minute") {
    const FlashTiming t; // FCF_FlashTab's: 0.25 s in, 0.5 s held, 0.25 s out, 0.5 s hidden, 60 s
    CHECK(flashAlpha(0, t) == 0.0f);
    CHECK(flashAlpha(125, t) == doctest::Approx(0.5f));
    CHECK(flashAlpha(250, t) == 1.0f);
    CHECK(flashAlpha(700, t) == 1.0f);
    CHECK(flashAlpha(875, t) == doctest::Approx(0.5f));
    CHECK(flashAlpha(1100, t) == 0.0f);
    CHECK(flashAlpha(1500 + 300, t) == 1.0f); // the next cycle
    CHECK(!flashOver(60000, t));
    CHECK(flashOver(60001, t));
    CHECK(flashAlpha(60300, t) == 0.0f);
    // The bottom button blinks every half second.
    CHECK(bottomButtonLit(0));
    CHECK(!bottomButtonLit(kChatButtonFlashMs));
    CHECK(bottomButtonLit(2 * kChatButtonFlashMs + 10));
}

TEST_CASE("Chat dock: fades step towards their target at CHAT_FRAME_FADE_TIME") {
    CHECK(fadeTowards(0.0f, 1.0f, 75.0f) == doctest::Approx(0.5f));
    CHECK(fadeTowards(0.5f, 1.0f, 1000.0f) == 1.0f);
    CHECK(fadeTowards(1.0f, 0.0f, 15.0f) == doctest::Approx(0.9f));
    CHECK(fadeTowards(0.2f, 0.0f, 1000.0f) == 0.0f);
    CHECK(fadeTowards(0.3f, 0.3f, 16.0f) == 0.3f);
    CHECK(fadeTowards(0.0f, 1.0f, -5.0f) == 0.0f); // a clock going back moves nothing
}

TEST_CASE("Chat dock: docked tabs keep their widths while they fit, then share the frame") {
    std::array<f32, 3> fits{80.0f, 100.0f, 90.0f};
    dockTabWidths(fits, 400.0f);
    CHECK(fits == std::array<f32, 3>{80.0f, 100.0f, 90.0f});
    // Too wide: the narrow one keeps its width, the others take the average of what is left.
    std::array<f32, 3> wide{60.0f, 200.0f, 150.0f};
    dockTabWidths(wide, 300.0f);
    CHECK(wide[0] == 60.0f);
    CHECK(wide[1] == doctest::Approx(120.0f));
    CHECK(wide[2] == doctest::Approx(120.0f));
    CHECK(wide[0] + wide[1] + wide[2] == doctest::Approx(300.0f));
    // All wider than the average: all equal.
    std::array<f32, 4> all{200.0f, 210.0f, 220.0f, 230.0f};
    dockTabWidths(all, 400.0f);
    for (const f32 w : all) {
        CHECK(w == doctest::Approx(100.0f));
    }
    std::array<f32, 0> none{};
    dockTabWidths(none, 100.0f); // nothing to do
}

TEST_CASE("Chat dock: a link of a wrapped line is found where wrappedText draws it") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    const std::filesystem::path pack = dir ? std::filesystem::path(dir) / "ui.vdpk" : std::filesystem::path();
    Assets assets;
    if (!dir || !std::filesystem::exists(pack) || !assets.open(pack)) {
        MESSAGE("VD_PACKS_DIR not set or ui.vdpk missing: skipping");
        return;
    }
    // The chat's new art is in the atlas.
    for (const char* path :
         {"Interface\\ChatFrame\\ChatFrameTab.blp", "Interface\\ChatFrame\\UI-ChatIcon-BlinkHilight.blp",
          "Interface\\PaperDollInfoFrame\\UI-Character-Tab-Highlight.blp",
          "Interface\\Buttons\\UI-Common-MouseHilight.blp",
          "Interface\\ChatFrame\\UI-ChatIcon-ScrollUp-Up.blp",
          "Interface\\ChatFrame\\UI-ChatIcon-ScrollDown-Up.blp",
          "Interface\\ChatFrame\\UI-ChatInputBorder-Left.blp",
          "Interface\\ChatFrame\\UI-ChatInputBorder-Right.blp"}) {
        INFO(path);
        CHECK(assets.sprite(assetPathHash(path)).has_value());
    }
    DrawList draw;
    draw.init(&assets, 4096);
    const f32 size = 14.0f;
    const f32 line = draw.lineHeight(FontId::Arial, size);
    const std::string prefix = "[2. Trade] |Hplayer:Galen|h[Galen]|h: WTS ";
    const std::string link = "|cff1eff00|Hitem:2132:0:0:0|h[Militia Shortsword]|h|r";
    const std::string text = prefix + link + " 1g";
    const usize first = prefix.size() + 10, last = prefix.size() + link.size() - 2; // the |H..|h span
    std::array<Rect, 4> out{};
    // Wide: one line, starting where the text before it ends.
    REQUIRE(draw.wrappedSpan(1000.0f, text, FontId::Arial, size, first, last, out) == 1);
    CHECK(out[0].x == doctest::Approx(draw.measure(prefix, FontId::Arial, size)));
    CHECK(out[0].w ==
          doctest::Approx(draw.measure("[Militia Shortsword]", FontId::Arial, size)).epsilon(0.02));
    CHECK(out[0].y == 0.0f);
    CHECK(out[0].h == doctest::Approx(line));
    // Narrow: the lines it breaks into, each inside the box, below the first.
    const f32 width = draw.measure("[2. Trade] [Galen]: WTS [Militia", FontId::Arial, size);
    const u32 n = draw.wrappedSpan(width, text, FontId::Arial, size, first, last, out);
    REQUIRE(n >= 1);
    REQUIRE(n <= 2);
    const f32 height = draw.measureWrapped(width, text, FontId::Arial, size);
    for (u32 i = 0; i < n; ++i) {
        CHECK(out[i].x >= 0.0f);
        CHECK(out[i].right() <= width + 0.5f);
        CHECK(out[i].bottom() <= height + 0.5f);
    }
    CHECK(out[n - 1].y > 0.0f); // the link is not all on the first line
    // A span outside the text, or empty: nothing.
    CHECK(draw.wrappedSpan(1000.0f, text, FontId::Arial, size, text.size(), text.size() + 4, out) == 0);
    CHECK(draw.wrappedSpan(1000.0f, text, FontId::Arial, size, 5, 5, out) == 0);
}
