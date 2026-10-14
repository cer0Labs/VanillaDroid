#pragma once

#include "app/Dialogs.h"
#include "app/RaidPulloutLayout.h"
#include "core/Types.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

// Phase 16 (pullouts): a raid pullout frame as Blizzard_RaidUI.xml draws it (RaidPulloutFrameTemplate with
// its RaidPulloutButtonTemplates), a pure view like app/Dialogs: a view-model in, what the finger did out; no
// game state. app/RaidPullouts fills it from the raid, app/RaidPulloutGallery with sample data.

// An aura button (RaidBuffButtonTemplate: 12-unit icon, UI-Debuff-Overlays' border of 13).
struct RaidPulloutAuraView {
    u64 iconHash = 0;
    // DebuffTypeColor: the dispel type's, or "none" red (RefreshBuffs leaves buffs that colour too).
    u32 border = 0;
};

// One member (RaidPullout_Update's button): the name in its class colour (RaidPulloutButton_UpdateDead: red
// dead, gray offline), the health bar (green; offline gray and full), the mana bar (ManaBarColor; offline
// gray and full), four auras.
struct RaidPulloutMemberView {
    std::string_view name;
    u32 nameColor = kRaidTitleColor;
    f32 health = 0.0f; // 0..1
    u32 healthColor = kRaidHealthColor;
    f32 power = 0.0f; // 0..1
    u32 powerColor = 0;
    std::array<RaidPulloutAuraView, kRaidPulloutAuras> auras{};
    u32 auraCount = 0;
};

struct RaidPulloutView {
    std::string_view title; // "Group 3" (GROUP .. " " .. N), the class's name
    std::span<const RaidPulloutMemberView> members;
    RaidPulloutGeometry at; // where its frame goes (`members` is the span's size)
    bool background = true; // MenuBackdrop shown (the menu's "Hide Background" hides it)
};

// What the fingers did to it this frame. Touch: the whole frame is one widget, so a drag that starts anywhere
// on it moves it (1.12 drags it by its edges and title; its buttons only click), a tap acts on the member or
// aura under the finger, a long press is the right click.
struct RaidPulloutResult {
    i32 tapped = -1;     // a member's button tapped (RaidPulloutButton_OnClick's left button)
    i32 auraMember = -1; // an aura tapped (RaidBuffButtonTemplate's OnEnter: its tooltip)
    i32 aura = -1;       // 0..3
    ui::Rect auraRect;
    bool held = false;   // a long press anywhere on it: the right click (the pullout's menu)
    bool moving = false; // a finger drags it: the frame follows `fingerDx` / `fingerDy` since it went down
    f32 fingerDx = 0.0f, fingerDy = 0.0f;
    bool dropped = false; // the dragging finger was lifted this frame (RaidPulloutStopMoving)
};
RaidPulloutResult raidPullout(DialogContext& c, u32 index, const RaidPulloutView& v);

} // namespace vd
