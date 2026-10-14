#include "ui/DrawList.h"
#include "ui/UiAssets.h"
#include "ui/Widgets.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

using namespace vd;
using namespace vd::ui;

// Phase 15 (help): a line broken at the space after a color escape keeps the escape whole ("... will
// |cffffd200NOT|r be able ...", HelpFrame's home text, showed "r be able" on the next line): escapes have no
// width, so the text wraps into as many lines as the same words without them, at every width.
TEST_CASE("Text wrap: color escapes before a space do not move the break (VD_PACKS_DIR)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    const std::filesystem::path pack = dir ? std::filesystem::path(dir) / "ui.vdpk" : std::filesystem::path();
    Assets assets;
    if (!dir || !std::filesystem::exists(pack) || !assets.open(pack)) {
        MESSAGE("VD_PACKS_DIR not set or ui.vdpk missing: skipping");
        return;
    }
    DrawList draw;
    draw.init(&assets, 4096);
    struct Pair {
        const char* escaped;
        const char* plain;
    };
    constexpr Pair kTexts[] = {
        {"A colored word is |cffffd200NOT|r wider than a plain one.  Neither does the code, which is "
         "|cffffd200NOT|r drawn",
         "A colored word is NOT wider than a plain one.  Neither does the code, which is NOT drawn"},
        {"|cff40ff40[Guild]|r |cffffffffBrakk|r: anyone got |cffffffff[Linen Cloth]|r? paying well",
         "[Guild] Brakk: anyone got [Linen Cloth]? paying well"},
    };
    for (const Pair& t : kTexts) {
        for (f32 width = 40.0f; width <= 420.0f; width += 1.0f) {
            INFO(t.plain << " at " << width);
            const f32 escaped = draw.measureWrapped(width, t.escaped, FontId::Friz, 14.0f);
            CHECK(escaped == doctest::Approx(draw.measureWrapped(width, t.plain, FontId::Friz, 14.0f)));
            draw.reset();
            CHECK(draw.wrappedText({0.0f, 0.0f, width, 1000.0f}, t.escaped, gameFont(14.0f)) ==
                  doctest::Approx(draw.wrappedText({0.0f, 0.0f, width, 1000.0f}, t.plain, gameFont(14.0f))));
        }
    }
}
