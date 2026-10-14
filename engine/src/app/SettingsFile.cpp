#include "app/SettingsFile.h"

#include "app/QualityGovernor.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdio>

namespace vd {
namespace {

enum class Kind : u8 { Float, Bool, Uint };

// The file's version: 2 since auto self cast, NPC names and buff durations are on and the camera never follows by
// default (1.12 has them the other way). A file without it wrote the old defaults (every value is written): those four
// keep the values the caller had (the new defaults).
constexpr u32 kSettingsVersion = 2;
constexpr std::string_view kVersionKey = "version";

struct Field {
    const char* key;
    Kind kind;
    usize offset;
    f32 min;
    f32 max;
};

#define VD_SETTING(key, kind, member, min, max) {key, kind, offsetof(SettingsView, member), min, max}
const Field kFields[] = {
    VD_SETTING("ui_scale", Kind::Float, uiScale, 0.6f, 1.6f),
    VD_SETTING("camera_speed", Kind::Float, cameraSpeed, 0.25f, 3.0f),
    VD_SETTING("invert_camera", Kind::Bool, invertCamera, 0.0f, 1.0f),
    VD_SETTING("show_fps", Kind::Bool, showFps, 0.0f, 1.0f),
    VD_SETTING("view_distance", Kind::Float, viewDistance, 200.0f, 1600.0f),
    VD_SETTING("master_volume", Kind::Float, masterVolume, 0.0f, 1.0f),
    VD_SETTING("music_volume", Kind::Float, musicVolume, 0.0f, 1.0f),
    VD_SETTING("ambience_volume", Kind::Float, ambienceVolume, 0.0f, 1.0f),
    VD_SETTING("effects_volume", Kind::Float, effectsVolume, 0.0f, 1.0f),
    VD_SETTING("voices_volume", Kind::Float, voicesVolume, 0.0f, 1.0f),
    VD_SETTING("interface_volume", Kind::Float, interfaceVolume, 0.0f, 1.0f),
    VD_SETTING("minimap_zoom", Kind::Uint, minimapZoom, 0.0f, 5.0f),
    VD_SETTING("minimap_inside_zoom", Kind::Uint, minimapInsideZoom, 0.0f, 5.0f),
    VD_SETTING("chat_bubbles", Kind::Bool, chatBubbles, 0.0f, 1.0f),
    VD_SETTING("party_chat_bubbles", Kind::Bool, partyChatBubbles, 0.0f, 1.0f),
    VD_SETTING("enemy_plates", Kind::Bool, enemyPlates, 0.0f, 1.0f),
    VD_SETTING("friend_plates", Kind::Bool, friendPlates, 0.0f, 1.0f),
    VD_SETTING("graphics_quality", Kind::Uint, graphicsQuality, 0.0f, 4.0f),
    VD_SETTING("render_scale", Kind::Float, renderScale, 0.5f, 1.0f),
    VD_SETTING("environment_detail", Kind::Float, environmentDetail, 0.5f, 1.5f),
    VD_SETTING("spell_detail", Kind::Float, spellDetail, 0.25f, 1.0f),
    VD_SETTING("dynamic_quality", Kind::Bool, dynamicQuality, 0.0f, 1.0f),
    VD_SETTING("limit_fps", Kind::Bool, limitFps, 0.0f, 1.0f),
    VD_SETTING("weather_density", Kind::Uint, weatherDensity, 0.0f, 3.0f), // phase 11
    VD_SETTING("full_screen_glow", Kind::Bool, fullScreenGlow, 0.0f, 1.0f), // phase 11
    VD_SETTING("death_effect", Kind::Bool, deathEffect, 0.0f, 1.0f),
    VD_SETTING("terrain_highlights", Kind::Bool, terrainHighlights, 0.0f, 1.0f), // phase 11
    VD_SETTING("auto_quest_watch", Kind::Bool, autoQuestWatch, 0.0f, 1.0f),      // phase 13
    VD_SETTING("ground_clutter", Kind::Uint, groundClutter, 0.0f, 3.0f),          // phase 14 (grass)
    VD_SETTING("anisotropy", Kind::Uint, anisotropy, 1.0f, 16.0f),                // phase 18 (world)
    VD_SETTING("multisample", Kind::Uint, multisample, 1.0f, 8.0f),
    // Phase 15 (options): UIOptionsFrame and SoundOptionsFrame.
    VD_SETTING("deselect_on_click", Kind::Bool, deselectOnClick, 0.0f, 1.0f),
    VD_SETTING("assist_attack", Kind::Bool, assistAttack, 0.0f, 1.0f),
    VD_SETTING("auto_clear_afk", Kind::Bool, autoClearAfk, 0.0f, 1.0f),
    VD_SETTING("block_trades", Kind::Bool, blockTrades, 0.0f, 1.0f),
    VD_SETTING("auto_self_cast", Kind::Bool, autoSelfCast, 0.0f, 1.0f),
    VD_SETTING("auto_loot", Kind::Bool, autoLoot, 0.0f, 1.0f),
    VD_SETTING("always_compare", Kind::Bool, alwaysCompare, 0.0f, 1.0f),
    VD_SETTING("status_bar_text", Kind::Bool, statusBarText, 0.0f, 1.0f),
    VD_SETTING("show_player_names", Kind::Bool, showPlayerNames, 0.0f, 1.0f),
    VD_SETTING("show_guild_names", Kind::Bool, showGuildNames, 0.0f, 1.0f),
    VD_SETTING("show_player_titles", Kind::Bool, showPlayerTitles, 0.0f, 1.0f),
    VD_SETTING("show_npc_names", Kind::Bool, showNpcNames, 0.0f, 1.0f),
    VD_SETTING("show_own_name", Kind::Bool, showOwnName, 0.0f, 1.0f),
    VD_SETTING("show_party_background", Kind::Bool, showPartyBackground, 0.0f, 1.0f),
    VD_SETTING("show_buff_durations", Kind::Bool, showBuffDurations, 0.0f, 1.0f),
    VD_SETTING("hide_outdoor_world_state", Kind::Bool, hideOutdoorWorldState, 0.0f, 1.0f),
    VD_SETTING("show_newbie_tips", Kind::Bool, showNewbieTips, 0.0f, 1.0f),
    VD_SETTING("show_game_tips", Kind::Bool, showGameTips, 0.0f, 1.0f), // phase 20 (retouches)
    VD_SETTING("game_tip", Kind::Uint, gameTip, 0.0f, 65535.0f),
    VD_SETTING("camera_follow_style", Kind::Uint, cameraFollowStyle, 0.0f, 2.0f),
    VD_SETTING("camera_terrain_tilt", Kind::Bool, cameraTerrainTilt, 0.0f, 1.0f),
    VD_SETTING("camera_bobbing", Kind::Bool, cameraBobbing, 0.0f, 1.0f),
    VD_SETTING("camera_water_collision", Kind::Bool, cameraWaterCollision, 0.0f, 1.0f),
    VD_SETTING("camera_follow_speed", Kind::Float, cameraFollowSpeed, 90.0f, 270.0f),
    VD_SETTING("camera_distance_max_factor", Kind::Float, cameraDistanceMaxFactor, 1.0f, 2.0f),
    VD_SETTING("lock_action_bars", Kind::Bool, lockActionBars, 0.0f, 1.0f),
    VD_SETTING("always_show_multibars", Kind::Bool, alwaysShowMultiBars, 0.0f, 1.0f),
    VD_SETTING("touch_controls", Kind::Bool, touchControls, 0.0f, 1.0f),     // phase 20
    VD_SETTING("touch_pet_bar", Kind::Bool, touchPetBar, 0.0f, 1.0f),        // phase 20 (pets)
    VD_SETTING("touch_left_handed", Kind::Bool, touchLeftHanded, 0.0f, 1.0f), // phase 20
    // phase 20 (assists): settings here
    VD_SETTING("touch_auto_target", Kind::Bool, touchAutoTarget, 0.0f, 1.0f),
    VD_SETTING("touch_start_attack", Kind::Bool, touchStartAttack, 0.0f, 1.0f),
    VD_SETTING("touch_repeat_held", Kind::Bool, touchRepeatHeld, 0.0f, 1.0f),
    VD_SETTING("touch_stance_assist", Kind::Bool, touchStanceAssist, 0.0f, 1.0f),
    VD_SETTING("touch_form_assist", Kind::Bool, touchFormAssist, 0.0f, 1.0f),
    VD_SETTING("touch_drag_to_frame", Kind::Bool, touchDragToFrame, 0.0f, 1.0f),
    VD_SETTING("touch_aim_ground", Kind::Bool, touchAimGround, 0.0f, 1.0f),
    VD_SETTING("touch_spell_queue", Kind::Bool, touchSpellQueue, 0.0f, 1.0f),
    VD_SETTING("touch_haptics", Kind::Bool, touchHaptics, 0.0f, 1.0f),
    VD_SETTING("touch_site_templates", Kind::Bool, touchSiteTemplates, 0.0f, 1.0f),
    VD_SETTING("guild_member_alert", Kind::Bool, guildMemberAlert, 0.0f, 1.0f),
    VD_SETTING("show_loot_spam", Kind::Bool, showLootSpam, 0.0f, 1.0f),
    VD_SETTING("hide_party_interface", Kind::Bool, hidePartyInterface, 0.0f, 1.0f),
    VD_SETTING("show_party_pets", Kind::Bool, showPartyPets, 0.0f, 1.0f),
    VD_SETTING("show_dispellable_debuffs", Kind::Bool, showDispellableDebuffs, 0.0f, 1.0f),
    VD_SETTING("show_castable_buffs", Kind::Bool, showCastableBuffs, 0.0f, 1.0f),
    VD_SETTING("show_target_of_target", Kind::Bool, showTargetOfTarget, 0.0f, 1.0f),
    VD_SETTING("target_of_target_state", Kind::Uint, targetOfTargetState, 1.0f, 5.0f),
    VD_SETTING("show_combat_text", Kind::Bool, showCombatText, 0.0f, 1.0f),
    VD_SETTING("combat_text_float_mode", Kind::Uint, combatTextFloatMode, 1.0f, 3.0f),
    VD_SETTING("combat_text_low_health_mana", Kind::Bool, combatTextLowHealthMana, 0.0f, 1.0f),
    VD_SETTING("combat_text_auras", Kind::Bool, combatTextAuras, 0.0f, 1.0f),
    VD_SETTING("combat_text_aura_fade", Kind::Bool, combatTextAuraFade, 0.0f, 1.0f),
    VD_SETTING("combat_text_combat_state", Kind::Bool, combatTextCombatState, 0.0f, 1.0f),
    VD_SETTING("combat_text_dodge_parry_miss", Kind::Bool, combatTextDodgeParryMiss, 0.0f, 1.0f),
    VD_SETTING("combat_text_resistances", Kind::Bool, combatTextResistances, 0.0f, 1.0f),
    VD_SETTING("combat_text_reputation", Kind::Bool, combatTextReputation, 0.0f, 1.0f),
    VD_SETTING("combat_text_reactives", Kind::Bool, combatTextReactives, 0.0f, 1.0f),
    VD_SETTING("combat_text_friendly_names", Kind::Bool, combatTextFriendlyNames, 0.0f, 1.0f),
    VD_SETTING("combat_text_combo_points", Kind::Bool, combatTextComboPoints, 0.0f, 1.0f),
    VD_SETTING("combat_text_mana", Kind::Bool, combatTextMana, 0.0f, 1.0f),
    VD_SETTING("combat_text_honor_gained", Kind::Bool, combatTextHonorGained, 0.0f, 1.0f),
    VD_SETTING("show_damage", Kind::Bool, showDamage, 0.0f, 1.0f),
    VD_SETTING("log_periodic_effects", Kind::Bool, logPeriodicEffects, 0.0f, 1.0f),
    VD_SETTING("show_pet_damage", Kind::Bool, showPetDamage, 0.0f, 1.0f),
    VD_SETTING("sound_effects", Kind::Bool, soundEffects, 0.0f, 1.0f),
    VD_SETTING("ambience_sounds", Kind::Bool, ambienceSounds, 0.0f, 1.0f),
    VD_SETTING("error_speech", Kind::Bool, errorSpeech, 0.0f, 1.0f),
    VD_SETTING("music_enabled", Kind::Bool, musicEnabled, 0.0f, 1.0f),
    VD_SETTING("sound_at_character", Kind::Bool, soundAtCharacter, 0.0f, 1.0f),
    VD_SETTING("emote_sounds", Kind::Bool, emoteSounds, 0.0f, 1.0f),
    VD_SETTING("music_looping", Kind::Bool, musicLooping, 0.0f, 1.0f),
    // Phase 17 (scores): the battle map.
    VD_SETTING("show_battlefield_minimap", Kind::Bool, showBattlefieldMinimap, 0.0f, 1.0f),
    VD_SETTING("battlefield_minimap_opacity", Kind::Float, battlefieldMinimapOpacity, 0.0f, 1.0f),
    VD_SETTING("battlefield_minimap_locked", Kind::Bool, battlefieldMinimapLocked, 0.0f, 1.0f),
    VD_SETTING("battlefield_minimap_show_players", Kind::Bool, battlefieldMinimapShowPlayers, 0.0f, 1.0f),
};
#undef VD_SETTING

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// Phase 11: each preset's Weather Intensity (weatherDensity 0..3): Low half the particles, Medium three quarters.
constexpr u32 kPresetWeatherDensity[kQualityPresets] = {1, 2, 3, 3};
// Phase 14 (grass): each preset's ground clutter (groundClutter 0..3): none on Low, half on Medium, 1.12's above.
constexpr u32 kPresetGroundClutter[kQualityPresets] = {0, 1, 2, 2};
// Phase 18 (world): anisotropic filtering and multisampling only on Ultra: on a tiled GPU the first costs every
// terrain pixel four times over (PERFORMANCE.md §3.10), the second more tiles for scenes bound by their geometry.
constexpr u32 kPresetAnisotropy[kQualityPresets] = {1, 1, 1, 4};
constexpr u32 kPresetMultisample[kQualityPresets] = {1, 1, 1, 4};

QualitySettings knobs(const SettingsView& s) {
    return {s.renderScale, s.viewDistance, s.environmentDetail, s.spellDetail};
}

} // namespace

std::string formatSettings(const SettingsView& settings) {
    std::string text = "# VanillaDroid settings (written by the game)\n";
    const auto* base = reinterpret_cast<const u8*>(&settings);
    char line[96];
    std::snprintf(line, sizeof(line), "%.*s = %u\n", static_cast<int>(kVersionKey.size()), kVersionKey.data(),
                  kSettingsVersion);
    text += line;
    for (const Field& field : kFields) {
        switch (field.kind) {
        case Kind::Float:
            std::snprintf(line, sizeof(line), "%s = %.3f\n", field.key,
                          static_cast<double>(*reinterpret_cast<const f32*>(base + field.offset)));
            break;
        case Kind::Bool:
            std::snprintf(line, sizeof(line), "%s = %d\n", field.key,
                          *reinterpret_cast<const bool*>(base + field.offset) ? 1 : 0);
            break;
        case Kind::Uint:
            std::snprintf(line, sizeof(line), "%s = %u\n", field.key, *reinterpret_cast<const u32*>(base + field.offset));
            break;
        }
        text += line;
    }
    return text;
}

void parseSettings(std::string_view text, SettingsView& settings) {
    const SettingsView before = settings;
    u32 version = 1;
    auto* base = reinterpret_cast<u8*>(&settings);
    while (!text.empty()) {
        const usize end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        line = trim(line);
        const usize equals = line.find('=');
        if (line.empty() || line.front() == '#' || equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (key == kVersionKey) {
            std::from_chars(value.data(), value.data() + value.size(), version);
            continue;
        }
        for (const Field& field : kFields) {
            if (key != field.key) {
                continue;
            }
            f32 number = 0.0f;
            // std::from_chars for floats is missing from some standard libraries: parse with snprintf's sibling.
            char buffer[32];
            const usize length = std::min(value.size(), sizeof(buffer) - 1);
            std::copy_n(value.data(), length, buffer);
            buffer[length] = '\0';
            if (std::sscanf(buffer, "%f", &number) != 1) {
                break;
            }
            number = std::clamp(number, field.min, field.max);
            switch (field.kind) {
            case Kind::Float: *reinterpret_cast<f32*>(base + field.offset) = number; break;
            case Kind::Bool: *reinterpret_cast<bool*>(base + field.offset) = number >= 0.5f; break;
            case Kind::Uint: *reinterpret_cast<u32*>(base + field.offset) = static_cast<u32>(number + 0.5f); break;
            }
            break;
        }
    }
    if (version < 2) {
        settings.autoSelfCast = before.autoSelfCast;
        settings.showNpcNames = before.showNpcNames;
        settings.showBuffDurations = before.showBuffDurations;
        settings.cameraFollowStyle = before.cameraFollowStyle;
    }
}

void applyQualityPreset(SettingsView& settings, u32 preset) {
    if (preset >= kQualityPresets) {
        settings.graphicsQuality = static_cast<u32>(QualityPreset::Custom);
        return;
    }
    const QualitySettings q = qualityPreset(static_cast<QualityPreset>(preset));
    settings.renderScale = q.renderScale;
    settings.viewDistance = q.viewDistance;
    settings.environmentDetail = q.environmentDetail;
    settings.spellDetail = q.spellDetail;
    settings.weatherDensity = kPresetWeatherDensity[preset];
    settings.groundClutter = kPresetGroundClutter[preset]; // phase 14
    settings.anisotropy = kPresetAnisotropy[preset];       // phase 18 (world)
    settings.multisample = kPresetMultisample[preset];
    settings.graphicsQuality = preset;
}

u32 currentQualityPreset(const SettingsView& settings) {
    const QualityPreset preset = matchingPreset(knobs(settings));
    if (preset != QualityPreset::Custom &&
        settings.weatherDensity != kPresetWeatherDensity[static_cast<u32>(preset)]) {
        return static_cast<u32>(QualityPreset::Custom); // phase 11: the weather is one of the knobs too
    }
    if (preset != QualityPreset::Custom &&
        settings.groundClutter != kPresetGroundClutter[static_cast<u32>(preset)]) {
        return static_cast<u32>(QualityPreset::Custom); // phase 14: and the ground clutter
    }
    if (preset != QualityPreset::Custom && (settings.anisotropy != kPresetAnisotropy[static_cast<u32>(preset)] ||
                                            settings.multisample != kPresetMultisample[static_cast<u32>(preset)])) {
        return static_cast<u32>(QualityPreset::Custom); // phase 18 (world): and the filtering and multisampling
    }
    return static_cast<u32>(preset);
}

} // namespace vd
