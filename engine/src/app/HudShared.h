#pragma once

// Shared by the Hud translation units (Hud.cpp, HudUnitFrames.cpp, HudActionBar.cpp, HudAuras.cpp,
// HudWorld.cpp): the client's art, metrics and colours of the HUD.

#include "core/Hash.h"
#include "core/Types.h"
#include "game/Entities.h"
#include "net/UpdateFields.gen.h"
#include "ui/DrawList.h"

#include <cstdio>

namespace vd::hud {

namespace uf = net::uf;
using ui::Rect;
using ui::rgba;
using ui::withAlpha;
using game::ObjectType;

constexpr f32 kUnitDp = 0.78f;          // one unit of the client's 768-high interface, in dp
constexpr f32 kStickRadiusDp = 56.0f;   // the stick's travel
constexpr f32 kCameraRadiansPerDp = 0.0085f;
constexpr f32 kPinchStepsPerDp = 0.02f; // zoom: wheel steps per dp of change in the fingers' distance

constexpr u64 kTargetingFrame = assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame.blp");
constexpr u64 kTargetingElite = assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame-Elite.blp");
constexpr u64 kTargetingRare = assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame-Rare.blp");
constexpr u64 kTargetingRareElite = assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame-RareMob.blp");
constexpr u64 kLevelBackground = assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame-LevelBackground.blp");
constexpr u64 kSkull = assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame-Skull.blp");
constexpr u64 kMonsterPortrait = assetPathHash("Interface\\CharacterFrame\\TemporaryPortrait-Monster.blp");
constexpr u64 kCastBorder = assetPathHash("Interface\\CastingBar\\UI-CastingBar-Border.blp");
constexpr u64 kCastSpark = assetPathHash("Interface\\CastingBar\\UI-CastingBar-Spark.blp");
constexpr u64 kCastFlash = assetPathHash("Interface\\CastingBar\\UI-CastingBar-Flash.blp");
constexpr u64 kDebuffOverlays = assetPathHash("Interface\\Buttons\\UI-Debuff-Overlays.blp");
constexpr u64 kActionBorder = assetPathHash("Interface\\Buttons\\UI-ActionButton-Border.blp");
constexpr u64 kIconJump = assetPathHash("Interface\\Icons\\Ability_Rogue_Sprint.blp");
constexpr u64 kIconAttack = assetPathHash("Interface\\Icons\\INV_Sword_04.blp");
constexpr u64 kIconTalk = assetPathHash("Interface\\Icons\\INV_Misc_Note_01.blp");
constexpr u64 kIconLoot = assetPathHash("Interface\\Icons\\INV_Misc_Bag_08.blp");
constexpr u64 kIconUse = assetPathHash("Interface\\Icons\\INV_Gizmo_01.blp");
constexpr u64 kIconNextTarget = assetPathHash("Interface\\Icons\\Ability_Hunter_SniperShot.blp");
constexpr u64 kIconMenu = assetPathHash("Interface\\Icons\\INV_Misc_Gear_01.blp");
constexpr u64 kStatusBar = assetPathHash("Interface\\TargetingFrame\\UI-StatusBar.blp");
constexpr u64 kQuickslot = assetPathHash("Interface\\Buttons\\UI-Quickslot2.blp");
constexpr u64 kQuickslotDown = assetPathHash("Interface\\Buttons\\UI-Quickslot-Depress.blp");
constexpr u64 kButtonHighlight = assetPathHash("Interface\\Buttons\\ButtonHilight-Square.blp");
constexpr u64 kCircleMask = assetPathHash("Interface\\CharacterFrame\\TempPortraitAlphaMask.blp");
// Phase 20: our own glyphs of the touch cluster (art/touch/*.svg, rasterized by the baker: into app.vdpk since phase
// 22, tools/formats/src/bake/AppPackBaker.cpp's kOwnArt).
constexpr u64 kTouchGlyphJump = assetPathHash("Interface\\VanillaDroid\\Touch-Jump.blp");
constexpr u64 kTouchGlyphMount = assetPathHash("Interface\\VanillaDroid\\Touch-Mount.blp");
constexpr u64 kTouchGlyphTarget = assetPathHash("Interface\\VanillaDroid\\Touch-Target.blp");

constexpr const char* kRaceNames[] = {"Human", "Orc", "Dwarf", "NightElf", "Scourge", "Tauren", "Gnome", "Troll"};

// The race's picture (TemporaryPortrait-<Gender>-<Race>): the client draws a live 3D portrait instead.
inline u64 racePortrait(u32 race, u32 gender) {
    char path[96];
    std::snprintf(path, sizeof(path), "Interface\\CharacterFrame\\TemporaryPortrait-%s-%s.blp",
                  gender == 1 ? "Female" : "Male", kRaceNames[(race >= 1 && race <= 8) ? race - 1 : 0]);
    return assetPathHash(path);
}

// Power bar colors (PowerBarColor in the client's UnitFrame.lua): mana, rage, focus, energy, happiness.
constexpr u32 kPowerColors[] = {rgba(0, 0, 255), rgba(255, 0, 0), rgba(255, 128, 64), rgba(255, 255, 0),
                                rgba(0, 255, 255)};


} // namespace vd::hud
