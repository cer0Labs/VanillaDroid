#include "app/PortraitGallery.h"

#include "app/CombatWindows.h"
#include "app/Portraits.h"
#include "app/SocialWindows.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "game/Appearance.h"
#include "ui/Widgets.h"
#include "world/Actors.h"

#include <algorithm>
#include <cstdio>

namespace vd {
namespace {

using ui::Rect;
using ui::rgba;

constexpr u64 kTargetingFrame = assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame.blp");
constexpr u64 kTargetOfTarget = assetPathHash("Interface\\TargetingFrame\\UI-TargetofTargetFrame.blp");
constexpr u64 kLevelBackground =
    assetPathHash("Interface\\TargetingFrame\\UI-TargetingFrame-LevelBackground.blp");
constexpr u64 kStatusBar = assetPathHash("Interface\\TargetingFrame\\UI-StatusBar.blp");
constexpr u64 kMonsterPortrait = assetPathHash("Interface\\CharacterFrame\\TemporaryPortrait-Monster.blp");
constexpr u64 kHumanMalePortrait =
    assetPathHash("Interface\\CharacterFrame\\TemporaryPortrait-Male-Human.blp");

// The samples (names as the server would send them).
enum Sample : u32 { kHuman, kOrc, kTauren, kGnome, kWolf, kKobold, kGuard, kCub };
constexpr const char* kNames[] = {"Arthel",      "Ugha",          "Brakk",           "Tinka",
                                  "Timber Wolf", "Kobold Vermin", "Stormwind Guard", "Tiger Cub"};
constexpr u32 kLevels[] = {12, 10, 11, 9, 7, 2, 55, 1};

// A player character: race, gender, a little customization; the human wears a helm and shoulder pads (the
// ItemDisplayInfo of a Stormwind guard's) to show what a portrait carries.
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

// The player frame (mirrored art) or the target frame, as app/Hud lays them out (FrameXML PlayerFrame.xml and
// TargetFrame.xml: 232 x 100 units, a 64-unit portrait, 119 x 12 bars).
void unitFrame(ui::Gui& g, f32 x, f32 y, bool player, TextureHandle portrait, u64 fallback, u32 sample,
               u32 nameColor) {
    const f32 u = g.unit(1.0f);
    const f32 barX = player ? 106.0f : 7.0f;
    g.draw.solid({x + barX * u, y + 22.0f * u, 119.0f * u, 41.0f * u}, rgba(0, 0, 0, 128));
    if (!player) {
        g.draw.sprite({x + 7.0f * u, y + 22.0f * u, 119.0f * u, 19.0f * u}, kLevelBackground, nameColor);
    }
    g.draw.portrait({x + (player ? 42.0f : 126.0f) * u, y + 12.0f * u, 64.0f * u, 64.0f * u}, portrait,
                    fallback);
    g.draw.sprite({x + barX * u, y + 41.0f * u, 119.0f * u * 0.8f, 12.0f * u}, kStatusBar, rgba(0, 255, 0),
                  {0.0f, 0.0f, 0.8f, 1.0f});
    g.draw.sprite({x + barX * u, y + 52.0f * u, 119.0f * u * 0.6f, 12.0f * u}, kStatusBar, rgba(0, 0, 255),
                  {0.0f, 0.0f, 0.6f, 1.0f});
    g.draw.sprite({x, y, 232.0f * u, 100.0f * u}, kTargetingFrame, ui::kWhite,
                  player ? ui::TexCoords{1.0f, 0.0f, 0.09375f, 0.78125f}
                         : ui::TexCoords{0.09375f, 0.0f, 1.0f, 0.78125f});
    ui::TextStyle label = ui::gameFont(12.5f * u, player ? ui::kGold : ui::kWhite);
    label.align = ui::Align::Center;
    ui::label(g, {x + (barX + 4.0f) * u, y + 23.0f * u, 111.0f * u, 17.0f * u}, kNames[sample], label);
    char level[8];
    std::snprintf(level, sizeof(level), "%u", kLevels[sample]);
    ui::label(g, {x + (player ? 40.0f : 166.0f) * u, y + 58.0f * u, 26.0f * u, 16.0f * u}, level, label);
}

} // namespace

void PortraitGallery::spawn() {
    if (m_spawned || !m_actors || !m_actors->isOpen() || !m_actors->appearance().ready()) {
        return;
    }
    m_spawned = true;
    const game::AppearanceDb& db = m_actors->appearance();
    const game::ActorLook looks[kSamples] = {
        db.character(character(1, 0, 1, true)),  // human male
        db.character(character(2, 1, 2, false)), // orc female
        db.character(character(6, 0, 1, false)), // tauren male
        db.character(character(7, 1, 2, false)), // gnome female
        db.creature(165),                        // Timber Wolf
        db.creature(26),                         // a kobold
        db.creature(1364),  // a human NPC: baked skin, helm, shoulders (CreatureDisplayInfoExtra)
        db.creature(16942), // a model without a portrait camera: framed by the baker
    };
    for (u32 i = 0; i < kSamples; ++i) {
        m_samples[i] = m_actors->create();
        m_actors->setLook(m_samples[i], looks[i]);
    }
    VD_LOG_INFO("UI gallery: %u sample units for their portraits", kSamples);
}

void PortraitGallery::draw(DialogContext& c) {
    spawn();
    ui::Gui g{c.ui, c.draw};
    const Rect& safe = c.safeArea;
    const f32 u = g.unit(1.0f);
    const auto portrait = [&](u32 sample) {
        return m_spawned && m_portraits ? m_portraits->actor(m_samples[sample]) : TextureHandle{};
    };

    // Player and target frames along the top (the target's target hanging under the target's right side), a
    // second target frame to the right when there is room.
    unitFrame(g, safe.x, safe.y, true, portrait(kHuman), kHumanMalePortrait, kHuman, ui::kGold);
    const f32 targetX = safe.x + 250.0f * u;
    unitFrame(g, targetX, safe.y, false, portrait(kKobold), kMonsterPortrait, kKobold, rgba(255, 0, 0, 230));
    // The target of target as app/CombatScreens draws it (its art 34 units high instead of the client's 45).
    const Rect tot{targetX + 232.0f * u - 30.0f * u, safe.y + 64.0f * u, 93.0f * u, 34.0f * u};
    g.draw.solid({tot.x + 30.0f * u, tot.y + 6.0f * u, 58.0f * u, 14.0f * u}, rgba(0, 0, 0, 160));
    g.draw.sprite({tot.x + 30.0f * u, tot.y + 16.0f * u, 58.0f * u, 5.0f * u}, kStatusBar, rgba(0, 255, 0));
    const f32 squash = tot.h / (45.0f * u);
    const f32 hole = 35.0f * u * squash;
    g.draw.portrait({tot.x + 23.5f * u - hole * 0.5f, tot.y + 23.5f * u * squash - hole * 0.5f, hole, hole},
                    portrait(kGuard), kMonsterPortrait);
    g.draw.sprite(tot, kTargetOfTarget, ui::kWhite, {0.015625f, 0.0f, 0.7421875f, 0.703125f});
    if (safe.w > 760.0f * u) {
        unitFrame(g, targetX + 330.0f * u, safe.y, false, portrait(kGuard), kMonsterPortrait, kGuard,
                  rgba(0, 255, 0, 230));
    }
    // Under the target, a creature framed by the baker (its model has no portrait camera).
    unitFrame(g, targetX, safe.y + 112.0f * u, false, portrait(kCub), kMonsterPortrait, kCub,
              rgba(255, 255, 0, 230));

    // The pet frame under the player frame, the party under it.
    PetFrameView pet;
    pet.name = kNames[kWolf];
    pet.level = kLevels[kWolf];
    pet.health = 0.9f;
    pet.power = 0.7f;
    pet.powerType = 2;
    pet.happiness = 3;
    pet.portrait = portrait(kWolf);
    const Rect petRect{safe.x + 80.0f * u, safe.y + 60.0f * u, 128.0f * u, 64.0f * u};
    petFrame(g, petRect, pet);
    PartyFrameView party[3];
    const u32 members[3] = {kOrc, kTauren, kGnome};
    const u8 races[3] = {2, 6, 7};
    for (u32 i = 0; i < 3; ++i) {
        party[i].name = kNames[members[i]];
        party[i].level = kLevels[members[i]];
        party[i].health = 1.0f - 0.2f * static_cast<f32>(i);
        party[i].power = 0.5f + 0.2f * static_cast<f32>(i);
        party[i].powerType = i == 0 ? 1 : 0;
        party[i].race = races[i];
        party[i].gender = i == 1 ? 0 : 1;
        party[i].leader = i == 0;
        party[i].portrait = portrait(members[i]);
    }
    partyFrames(
        c, {safe.x + g.px(4.0f), std::max(safe.y + 100.0f * u, petRect.bottom()), 128.0f * u * 1.6f, 0.0f},
        party);
}

} // namespace vd
