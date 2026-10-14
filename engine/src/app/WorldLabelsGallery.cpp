#include "app/WorldLabelsGallery.h"

#include "app/HudShared.h"
#include "app/UnitMenuWindows.h"
#include "app/WorldLabels.h"
#include "core/Text.h"
#include "game/UnitText.h"

namespace vd {
namespace {

using ui::rgba;

constexpr u32 kFriendly = rgba(0, 255, 0);
constexpr u32 kHostile = rgba(255, 0, 0);
constexpr u32 kNeutral = rgba(255, 255, 0);
constexpr u32 kFriendlyPlayer = rgba(64, 144, 255);

} // namespace

void WorldLabelsGallery::draw(DialogContext& c) {
    const ui::Rect& area = c.safeArea;
    const f32 dp = c.ui.dp();
    const f32 unit = dp * hud::kUnitDp;
    char pet[64];
    // Names over heads (the label's foot is the point over the head), with bubbles over some of them.
    struct Sample {
        f32 fx;
        UnitLabelView label;
        std::string_view bubble;
        u32 bubbleColor;
        f32 alpha;
    };
    Sample names[4];
    names[0] = {0.14f,
                {},
                "Welcome to the Lion's Pride Inn! Rest your feet a while, the road to Westfall is long.",
                rgba(255, 255, 255),
                1.0f};
    names[0].label.name = "Innkeeper Farley";
    names[0].label.title = "Innkeeper";
    names[0].label.color = kFriendly;
    names[1] = {0.38f, {}, "You no take candle!", rgba(255, 64, 64), 1.0f};
    names[1].label.name = "Kobold Tunneler";
    names[1].label.color = kHostile;
    names[1].label.health = 0.45f;
    names[1].label.barColor = kHostile;
    names[2] = {0.62f, {}, "pull the next one, I'm ready", rgba(170, 170, 255), 1.0f};
    names[2].label.name = "Arthel";
    names[2].label.title = "Los Lunes";
    names[2].label.color = kFriendlyPlayer;
    names[3] = {0.84f, {}, "brb", rgba(255, 255, 255), 0.45f};
    names[3].label.name = "Wolf";
    names[3].label.title = text::format(pet, "UNITNAME_TITLE_PET", "Brakk");
    names[3].label.color = kFriendly;
    const f32 nameY = area.y + area.h * 0.42f;
    for (Sample& s : names) {
        s.label.x = area.x + area.w * s.fx;
        s.label.y = nameY;
        const f32 top = drawUnitLabel(c.draw, dp, s.label);
        const ChatBubbleLayout layout = chatBubbleLayout(c.draw, unit, s.label.x, top - dp, s.bubble, area);
        drawChatBubble(c.draw, unit, layout, s.bubble, s.bubbleColor, s.alpha);
    }
    // Name plates (V hostile and neutral, Shift-V friendly).
    UnitLabelView plates[4];
    plates[0].name = "Defias Thug";
    plates[0].barColor = kHostile;
    plates[0].health = 0.6f;
    plates[0].level = 10;
    plates[0].levelColor = game::difficultyColor(game::levelDifficulty(10, 9));
    plates[1].name = "Young Wolf";
    plates[1].barColor = kNeutral;
    plates[1].level = 3;
    plates[1].levelColor = game::difficultyColor(game::levelDifficulty(3, 9));
    plates[1].targeted = true; // the target's glow
    plates[2].name = "Stormwind City Guard";
    plates[2].barColor = kFriendly;
    plates[2].level = 0; // 55 against a level 9 player: the skull
    plates[2].levelColor = rgba(255, 26, 26);
    plates[3].name = "Aeryn";
    plates[3].barColor = kFriendlyPlayer;
    plates[3].health = 0.25f;
    plates[3].level = 9;
    plates[3].levelColor = game::difficultyColor(game::levelDifficulty(9, 9));
    for (u32 i = 0; i < 4; ++i) {
        plates[i].plate = true;
        plates[i].x = area.x + area.w * (0.14f + 0.24f * static_cast<f32>(i));
        plates[i].y = area.y + area.h * 0.8f;
        const f32 top = drawUnitLabel(c.draw, dp, plates[i]);
        if (i < 2) { // phase 13: raid target icons over the plates (the skull, the cross), as app/HudWorld
            const f32 size = 26.0f * dp;
            drawRaidTargetIcon(c.draw, {plates[i].x - size * 0.5f, top - size - 2.0f * dp, size, size},
                               static_cast<u8>(8 - i));
        }
    }
}

} // namespace vd
