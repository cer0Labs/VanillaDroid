#include "app/OptionsScreens.h"

#include "app/GameSession.h"
#include "app/TutorialScreens.h" // phase 18 (p3)
#include "core/DevEnv.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vd {
namespace {

namespace uf = net::uf;
constexpr u8 kClassRogue = 4, kClassDruid = 11;

// UIOptionsFrameCameraDropDown: Smart "1", Always "2", Never "0" (cameraSmoothStyle), in that order.
constexpr u32 kCameraStyles[] = {1, 2, 0};
// UIOptionsFrameTargetofTargetDropDown: Raid "1", Party "2", Solo "3", Raid & Party "4", Always "5".
constexpr u32 kTargetOfTargetStates[] = {1, 2, 3, 4, 5};
// UIOptionsFrameCombatTextDropDown: Scroll Up "1", Scroll Down "2", Arc "3".
constexpr u32 kFloatModes[] = {1, 2, 3};

// The fields of each window, for Okay, Cancel and Defaults.
#define VD_INTERFACE_OPTIONS(X)                                                                                        \
    X(cameraSpeed) X(invertCamera) X(deselectOnClick) X(assistAttack) X(autoClearAfk) X(blockTrades) X(autoSelfCast)   \
    X(autoLoot) X(alwaysCompare) X(statusBarText) X(showPlayerNames) X(showGuildNames) X(showPlayerTitles)             \
    X(showNpcNames) X(showOwnName) X(enemyPlates) X(friendPlates) X(showPartyBackground) X(showBuffDurations)          \
    X(hideOutdoorWorldState)                                                                                           \
    X(showNewbieTips) X(showGameTips) X(autoQuestWatch) X(cameraFollowStyle) X(cameraTerrainTilt) X(cameraBobbing)                     \
    X(cameraWaterCollision) X(cameraFollowSpeed) X(cameraDistanceMaxFactor) X(lockActionBars) X(alwaysShowMultiBars)   \
    X(touchControls) X(touchLeftHanded) /* phase 20 */                                                                 \
    X(touchAutoTarget) X(touchStartAttack) X(touchRepeatHeld) X(touchStanceAssist) X(touchFormAssist)                 \
    X(touchDragToFrame) X(touchAimGround) X(touchSpellQueue) X(touchHaptics) X(touchSiteTemplates) /* assists */      \
    X(guildMemberAlert) X(chatBubbles) X(partyChatBubbles) X(showLootSpam) X(hidePartyInterface) X(showPartyPets)      \
    X(showDispellableDebuffs) X(showCastableBuffs) X(showTargetOfTarget) X(targetOfTargetState) X(showCombatText)      \
    X(combatTextFloatMode) X(combatTextLowHealthMana) X(combatTextAuras) X(combatTextAuraFade)                         \
    X(combatTextCombatState) X(combatTextDodgeParryMiss) X(combatTextResistances) X(combatTextReputation)              \
    X(combatTextReactives) X(combatTextFriendlyNames) X(combatTextComboPoints) X(combatTextMana)                       \
    X(combatTextHonorGained) X(showDamage) X(logPeriodicEffects) X(showPetDamage) X(touchPetBar) /* phase 20 (pets) */
#define VD_SOUND_OPTIONS(X)                                                                                            \
    X(soundEffects) X(ambienceSounds) X(errorSpeech) X(musicEnabled) X(soundAtCharacter) X(emoteSounds)                \
    X(musicLooping) X(masterVolume) X(effectsVolume) X(musicVolume) X(ambienceVolume) X(voicesVolume)                  \
    X(interfaceVolume)

std::string_view tooltipOf(const char* key) {
    const text::Key k{std::string_view(key)};
    return text::has(k) ? text::get(k) : std::string_view{};
}

} // namespace

void OptionsScreens::copyInterfaceOptions(const SettingsView& from, SettingsView& to) {
#define VD_COPY(field) to.field = from.field;
    VD_INTERFACE_OPTIONS(VD_COPY)
#undef VD_COPY
}

void OptionsScreens::copySoundOptions(const SettingsView& from, SettingsView& to) {
#define VD_COPY(field) to.field = from.field;
    VD_SOUND_OPTIONS(VD_COPY)
#undef VD_COPY
}

void OptionsScreens::interfaceDefaults(SettingsView& settings) {
    copyInterfaceOptions(SettingsView{}, settings);
    // Phase 16 (pullouts): off until set (RaidFrame_OnLoad), but UIOptionsFrameCheckButtons' default is "1".
    settings.showDispellableDebuffs = true;
    settings.showCastableBuffs = true;
}

void OptionsScreens::soundDefaults(SettingsView& settings) {
    copySoundOptions(SettingsView{}, settings);
}

bool OptionsScreens::targetOfTargetShown(const SettingsView& s, bool inParty, bool inRaid) {
    if (!s.showTargetOfTarget) {
        return false;
    }
    switch (s.targetOfTargetState) {
    case 1: return inRaid;
    case 2: return inParty && !inRaid;
    case 3: return !inParty && !inRaid;
    case 4: return inParty || inRaid;
    default: return true; // 5: always
    }
}

u8 OptionsScreens::multiBars(GameSession* session) const {
    if (session && session->inWorld()) {
        return session->play().actionBarToggles();
    }
    // Development (offline): VD_MULTIBARS=<0..15> shows those extra bars (bit 0 bottom left, 1 bottom right, 2 right,
    // 3 right 2) until the interface options change them.
    static const u32 forced = [] {
        const char* v = VD_DEV_ENV("VD_MULTIBARS");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 0) & 0x0F) : ~0u;
    }();
    return forced != ~0u && !m_localBarsSet ? static_cast<u8>(forced) : m_localBars;
}

void OptionsScreens::prepareHud(GameSession* session, const SettingsView& s, HudFrame& hud) const {
    hud.names.players = s.showPlayerNames;
    hud.names.guilds = s.showPlayerNames && s.showGuildNames;
    hud.names.titles = s.showPlayerNames && s.showPlayerTitles;
    hud.names.npcs = s.showNpcNames;
    hud.names.own = s.showOwnName;
    hud.statusBarText = s.statusBarText;
    hud.showBuffDurations = s.showBuffDurations;
    hud.deselectOnClick = s.deselectOnClick;
    hud.multiBars = multiBars(session);
    hud.alwaysShowMultiBars = s.alwaysShowMultiBars;
}

// --- Rows ------------------------------------------------------------------------------------------------------

void OptionsScreens::addSection(std::string_view label) {
    if (m_rowCount >= kMaxRows) {
        return;
    }
    m_rows[m_rowCount] = {};
    m_rows[m_rowCount].kind = OptionRow::Kind::Section;
    m_rows[m_rowCount].label = label;
    m_bindings[m_rowCount++] = {};
}

OptionRow& OptionsScreens::addCheck(std::string_view label, std::string_view tooltip, bool* flag, bool inverted) {
    static OptionRow overflow;
    if (m_rowCount >= kMaxRows) {
        return overflow;
    }
    OptionRow& row = m_rows[m_rowCount];
    row = {};
    row.kind = OptionRow::Kind::Check;
    row.label = label;
    row.tooltip = tooltip;
    row.checked = inverted ? !*flag : *flag;
    m_bindings[m_rowCount++] = {inverted ? Binding::InvertedBool : Binding::Bool, flag, nullptr, nullptr, nullptr};
    return row;
}

OptionRow& OptionsScreens::addButton(std::string_view label, Binding kind) {
    static OptionRow overflow;
    if (m_rowCount >= kMaxRows) {
        return overflow;
    }
    OptionRow& row = m_rows[m_rowCount];
    row = {};
    row.kind = OptionRow::Kind::Button;
    row.label = label;
    m_bindings[m_rowCount++] = {kind, nullptr, nullptr, nullptr, nullptr};
    return row;
}

OptionRow& OptionsScreens::addServerCheck(std::string_view label, std::string_view tooltip, Binding kind, bool checked) {
    static OptionRow overflow;
    if (m_rowCount >= kMaxRows) {
        return overflow;
    }
    OptionRow& row = m_rows[m_rowCount];
    row = {};
    row.kind = OptionRow::Kind::Check;
    row.label = label;
    row.tooltip = tooltip;
    row.checked = checked;
    m_bindings[m_rowCount++] = {kind, nullptr, nullptr, nullptr, nullptr};
    return row;
}

OptionRow& OptionsScreens::addSlider(std::string_view label, std::string_view tooltip, f32* value, f32 min, f32 max,
                                     f32 step, std::string_view shown) {
    static OptionRow overflow;
    if (m_rowCount >= kMaxRows) {
        return overflow;
    }
    OptionRow& row = m_rows[m_rowCount];
    row = {};
    row.kind = OptionRow::Kind::Slider;
    row.label = label;
    row.tooltip = tooltip;
    row.value = *value;
    row.min = min;
    row.max = max;
    row.step = step;
    row.shown = shown;
    m_bindings[m_rowCount++] = {Binding::Float, nullptr, value, nullptr, nullptr};
    return row;
}

OptionRow& OptionsScreens::addChoice(std::string_view label, std::string_view tooltip, u32* choice,
                                     std::span<const std::string_view> labels, const u32* values) {
    static OptionRow overflow;
    if (m_rowCount >= kMaxRows) {
        return overflow;
    }
    OptionRow& row = m_rows[m_rowCount];
    row = {};
    row.kind = OptionRow::Kind::Choice;
    row.label = label;
    row.tooltip = tooltip;
    row.choices = labels;
    for (u32 i = 0; i < labels.size(); ++i) {
        row.choice = values[i] == *choice ? i : row.choice;
    }
    m_bindings[m_rowCount++] = {Binding::Choice, nullptr, nullptr, choice, values};
    return row;
}

void OptionsScreens::buildInterface(SettingsView& s, u8 playerClass) {
    m_rowCount = 0;
    m_shownCount = 0;
    const auto shown = [this](const char* format, f32 value) -> std::string_view {
        std::array<char, 24>& buffer = m_shown[std::min<u32>(m_shownCount++, static_cast<u32>(m_shown.size() - 1))];
        std::snprintf(buffer.data(), buffer.size(), format, static_cast<double>(value));
        return buffer.data();
    };
    if (m_tab == 0) {
        // --- Basic: Controls, Display, Camera, Help ---
        addSection(text::get("CONTROLS_LABEL"));
        // MOUSE_SENSITIVITY and INVERT_MOUSE move a cursor and a mouse look: their touch counterparts turn the camera
        // under a dragged finger.
        addSlider(text::get("VD_SETTINGS_CAMERA_SPEED"), text::get("VD_OPTIONS_TIP_CAMERA_SPEED"), &s.cameraSpeed, 0.25f,
                  3.0f, 0.05f, shown("%.2fx", s.cameraSpeed));
        addCheck(text::get("VD_SETTINGS_INVERT_CAMERA"), text::get("VD_OPTIONS_TIP_INVERT_CAMERA"), &s.invertCamera);
        addCheck(text::get("GAMEFIELD_DESELECT_TEXT"), tooltipOf("OPTION_TOOLTIP_GAMEFIELD_DESELECT"), &s.deselectOnClick,
                 true);
        addCheck(text::get("ASSIST_ATTACK"), tooltipOf("OPTION_TOOLTIP_ASSIST_ATTACK"), &s.assistAttack);
        addCheck(text::get("CLEAR_AFK"), tooltipOf("OPTION_TOOLTIP_CLEAR_AFK"), &s.autoClearAfk);
        addCheck(text::get("BLOCK_TRADES"), tooltipOf("OPTION_TOOLTIP_BLOCK_TRADES"), &s.blockTrades);
        addCheck(text::get("AUTO_SELF_CAST_TEXT"), tooltipOf("OPTION_TOOLTIP_AUTO_SELF_CAST"), &s.autoSelfCast);
        addCheck(text::get("VD_OPTIONS_AUTO_LOOT"), text::get("VD_OPTIONS_TIP_AUTO_LOOT"), &s.autoLoot); // ours
        addCheck(text::get("VD_OPTIONS_ALWAYS_COMPARE"), text::get("VD_OPTIONS_TIP_ALWAYS_COMPARE"),
                 &s.alwaysCompare); // ours

        addSection(text::get("DISPLAY"));
        addCheck(text::get("STATUS_BAR_TEXT"), tooltipOf("OPTION_TOOLTIP_STATUS_BAR"), &s.statusBarText);
        addCheck(text::get("SHOW_PLAYER_NAMES"), tooltipOf("OPTION_TOOLTIP_SHOW_PLAYER_NAMES"), &s.showPlayerNames);
        OptionRow& guilds =
            addCheck(text::get("SHOW_GUILD_NAMES"), tooltipOf("OPTION_TOOLTIP_SHOW_GUILD_NAMES"), &s.showGuildNames);
        guilds.indent = true;
        guilds.enabled = s.showPlayerNames; // UIOptionsFrame_UpdateDependencies
        OptionRow& titles =
            addCheck(text::get("SHOW_PLAYER_TITLES"), tooltipOf("OPTION_TOOLTIP_SHOW_PLAYER_TITLES"), &s.showPlayerTitles);
        titles.indent = true;
        titles.enabled = s.showPlayerNames;
        addCheck(text::get("SHOW_NPC_NAMES"), tooltipOf("OPTION_TOOLTIP_SHOW_NPC_NAMES"), &s.showNpcNames);
        addCheck(text::get("SHOW_OWN_NAME"), tooltipOf("OPTION_TOOLTIP_SHOW_OWN_NAME"), &s.showOwnName);
        // The V / Shift-V keys' name plates (NAMEPLATES_ON / FRIENDNAMEPLATES_ON): without keys, options here.
        addCheck(text::get("BINDING_NAME_NAMEPLATES"), text::get("VD_OPTIONS_TIP_NAMEPLATES"), &s.enemyPlates);
        addCheck(text::get("BINDING_NAME_FRIENDNAMEPLATES"), text::get("VD_OPTIONS_TIP_FRIENDNAMEPLATES"),
                 &s.friendPlates);
        addServerCheck(text::get("SHOW_CLOAK"), tooltipOf("OPTION_TOOLTIP_SHOW_CLOAK"), Binding::Cloak, m_cloak);
        addServerCheck(text::get("SHOW_HELM"), tooltipOf("OPTION_TOOLTIP_SHOW_HELM"), Binding::Helm, m_helm);
        addCheck(text::get("SHOW_PARTY_BACKGROUND_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_PARTY_BACKGROUND"),
                 &s.showPartyBackground);
        addCheck(text::get("SHOW_BUFF_DURATION_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_BUFF_DURATION"),
                 &s.showBuffDurations);
        addCheck(text::get("HIDE_OUTDOOR_WORLD_STATE_TEXT"), tooltipOf("OPTION_TOOLTIP_HIDE_OUTDOOR_WORLD_STATE"),
                 &s.hideOutdoorWorldState);
        addCheck(text::get("AUTO_QUEST_WATCH_TEXT"), tooltipOf("OPTION_TOOLTIP_AUTO_QUEST_WATCH"), &s.autoQuestWatch);

        addSection(text::get("CAMERA_LABEL"));
        static const std::string_view kStyleKeys[] = {"CAMERA_SMART", "CAMERA_ALWAYS", "CAMERA_NEVER"};
        static std::array<std::string_view, 3> styles;
        for (u32 i = 0; i < styles.size(); ++i) {
            styles[i] = text::get(text::Key(kStyleKeys[i]));
        }
        OptionRow& style = addChoice(text::get("CAMERA_FOLLOWING_STYLE"), {}, &s.cameraFollowStyle, styles, kCameraStyles);
        char key[40];
        std::snprintf(key, sizeof(key), "OPTION_TOOLTIP_CAMERA%u", style.choice + 1); // the choice's own tooltip
        style.tooltip = tooltipOf(key);
        addCheck(text::get("FOLLOW_TERRAIN"), tooltipOf("OPTION_TOOLTIP_FOLLOW_TERRAIN"), &s.cameraTerrainTilt);
        addCheck(text::get("HEAD_BOB"), tooltipOf("OPTION_TOOLTIP_HEAD_BOB"), &s.cameraBobbing);
        OptionRow& follow = addSlider(text::get("AUTO_FOLLOW_SPEED"), tooltipOf("OPTION_TOOLTIP_AUTO_FOLLOW_SPEED"),
                                      &s.cameraFollowSpeed, 90.0f, 270.0f, 10.0f, shown("%.0f", s.cameraFollowSpeed));
        follow.enabled = s.cameraFollowStyle != 0; // Never: UIOptionsFrameSlider2 disabled
        addCheck(text::get("WATER_COLLISION"), tooltipOf("OPTION_TOOLTIP_WATER_COLLISION"), &s.cameraWaterCollision);
        addSlider(text::get("MAX_FOLLOW_DIST"), tooltipOf("OPTION_TOOLTIP_MAX_FOLLOW_DIST"), &s.cameraDistanceMaxFactor,
                  1.0f, 2.0f, 0.1f, shown("%.1f", s.cameraDistanceMaxFactor));

        addSection(text::get("HELP_LABEL"));
        // Phase 18 (p3): UIOptionsFrameCheckButton28 (SHOW_TUTORIALS: TutorialsEnabled) and UIOptionsFrameResetTutorials.
        addServerCheck(text::get("SHOW_TUTORIALS"), tooltipOf("OPTION_TOOLTIP_SHOW_TUTORIALS"), Binding::Tutorials,
                       m_tutorials);
        addButton(text::get("RESET_TUTORIALS"), Binding::Reset);
        addCheck(text::get("SHOW_NEWBIE_TIPS_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_NEWBIE_TIPS"), &s.showNewbieTips);
        // Phase 20 (retouches): UIOptionsFrameCheckButton44, under Show Tutorials.
        addCheck(text::get("SHOW_TIPOFTHEDAY_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_TIPOFTHEDAY"), &s.showGameTips);
        return;
    }
    // --- Advanced: Action Bars, Chat, Raid & Party, Floating Combat Text ---
    addSection(text::get("ACTIONBAR_LABEL"));
    addCheck(text::get("LOCK_ACTIONBAR_TEXT"), tooltipOf("OPTION_TOOLTIP_LOCK_ACTIONBAR"), &s.lockActionBars);
    addServerCheck(text::get("SHOW_MULTIBAR1_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_MULTIBAR1"), Binding::Bar1, m_bars[0]);
    addServerCheck(text::get("SHOW_MULTIBAR2_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_MULTIBAR2"), Binding::Bar2, m_bars[1]);
    addServerCheck(text::get("SHOW_MULTIBAR3_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_MULTIBAR3"), Binding::Bar3, m_bars[2]);
    OptionRow& bar4 =
        addServerCheck(text::get("SHOW_MULTIBAR4_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_MULTIBAR4"), Binding::Bar4, m_bars[3]);
    bar4.indent = true;
    bar4.enabled = m_bars[2]; // the second right bar only with the first (MultiActionBar_Update)
    addCheck(text::get("ALWAYS_SHOW_MULTIBARS_TEXT"), tooltipOf("OPTION_TOOLTIP_ALWAYS_SHOW_MULTIBARS"),
             &s.alwaysShowMultiBars);
    // Phase 20 (docs/MOBILE_CONTROLS.md): the touch cluster or the classic bars, and the left hand.
    addCheck(text::get("VD_TOUCH_OPTION_CONTROLS"), tooltipOf("VD_TOUCH_OPTION_CONTROLS_TOOLTIP"), &s.touchControls);
    addCheck(text::get("VD_TOUCH_PETS_OPTION_PET_BAR"), tooltipOf("VD_TOUCH_PETS_OPTION_PET_BAR_TOOLTIP"), // phase 20 (pets)
             &s.touchPetBar);
    addCheck(text::get("VD_TOUCH_OPTION_LEFT_HANDED"), tooltipOf("VD_TOUCH_OPTION_LEFT_HANDED_TOOLTIP"),
             &s.touchLeftHanded);
    // phase 20 (assists): options here
    // §8: what the touch buttons do for the player (no more than a 1.12 macro), with the cluster on.
    if (s.touchControls) {
        static constexpr struct {
            std::string_view label, tooltip;
            bool SettingsView::*value;
        } kAssists[] = {
            {"VD_TOUCH_ASSIST_AUTO_TARGET", "VD_TOUCH_ASSIST_AUTO_TARGET_TOOLTIP", &SettingsView::touchAutoTarget},
            {"VD_TOUCH_ASSIST_START_ATTACK", "VD_TOUCH_ASSIST_START_ATTACK_TOOLTIP", &SettingsView::touchStartAttack},
            {"VD_TOUCH_ASSIST_REPEAT", "VD_TOUCH_ASSIST_REPEAT_TOOLTIP", &SettingsView::touchRepeatHeld},
            {"VD_TOUCH_ASSIST_STANCES", "VD_TOUCH_ASSIST_STANCES_TOOLTIP", &SettingsView::touchStanceAssist},
            {"VD_TOUCH_ASSIST_FORMS", "VD_TOUCH_ASSIST_FORMS_TOOLTIP", &SettingsView::touchFormAssist},
            {"VD_TOUCH_ASSIST_DRAG_FRAME", "VD_TOUCH_ASSIST_DRAG_FRAME_TOOLTIP", &SettingsView::touchDragToFrame},
            {"VD_TOUCH_ASSIST_AIM_GROUND", "VD_TOUCH_ASSIST_AIM_GROUND_TOOLTIP", &SettingsView::touchAimGround},
            {"VD_TOUCH_ASSIST_QUEUE", "VD_TOUCH_ASSIST_QUEUE_TOOLTIP", &SettingsView::touchSpellQueue},
            {"VD_TOUCH_ASSIST_HAPTICS", "VD_TOUCH_ASSIST_HAPTICS_TOOLTIP", &SettingsView::touchHaptics},
            {"VD_TOUCH_ASSIST_SITE_TEMPLATES", "VD_TOUCH_ASSIST_SITE_TEMPLATES_TOOLTIP",
             &SettingsView::touchSiteTemplates},
        };
        for (const auto& assist : kAssists) {
            addCheck(text::get(text::Key(assist.label)), text::get(text::Key(assist.tooltip)), &(s.*assist.value))
                .indent = true;
        }
    }

    addSection(text::get("CHAT_LABEL"));
    addCheck(text::get("GUILDMEMBER_ALERT"), tooltipOf("OPTION_TOOLTIP_GUILDMEMBER_ALERT"), &s.guildMemberAlert);
    addCheck(text::get("CHAT_BUBBLES_TEXT"), tooltipOf("OPTION_TOOLTIP_CHAT_BUBBLES"), &s.chatBubbles);
    addCheck(text::get("PARTY_CHAT_BUBBLES_TEXT"), tooltipOf("OPTION_TOOLTIP_PARTY_CHAT_BUBBLES"), &s.partyChatBubbles);
    addCheck(text::get("SHOW_LOOT_SPAM"), tooltipOf("OPTION_TOOLTIP_SHOW_LOOT_SPAM"), &s.showLootSpam);

    addSection(text::get("RAID_AND_PARTY"));
    addCheck(text::get("HIDE_PARTY_INTERFACE_TEXT"), tooltipOf("OPTION_TOOLTIP_HIDE_PARTY_INTERFACE"),
             &s.hidePartyInterface);
    addCheck(text::get("SHOW_PARTY_PETS_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_PARTY_PETS"), &s.showPartyPets);
    addCheck(text::get("SHOW_DISPELLABLE_DEBUFFS_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_DISPELLABLE_DEBUFFS"),
             &s.showDispellableDebuffs);
    addCheck(text::get("SHOW_CASTABLE_BUFFS_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_CASTABLE_BUFFS"),
             &s.showCastableBuffs);
    addCheck(text::get("SHOW_TARGET_OF_TARGET_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_TARGET_OF_TARGET"),
             &s.showTargetOfTarget);
    static const std::string_view kTotKeys[] = {"RAID", "PARTY", "SOLO", "RAID_AND_PARTY", "ALWAYS"};
    static std::array<std::string_view, 5> states;
    for (u32 i = 0; i < states.size(); ++i) {
        states[i] = text::get(text::Key(kTotKeys[i]));
    }
    OptionRow& tot = addChoice({}, {}, &s.targetOfTargetState, states, kTargetOfTargetStates);
    char key[48];
    std::snprintf(key, sizeof(key), "OPTION_TOOLTIP_TARGETOFTARGET%u", tot.choice + 1);
    tot.tooltip = tooltipOf(key);
    tot.indent = true;
    tot.enabled = s.showTargetOfTarget;

    addSection(text::get("COMBAT_TEXT_LABEL"));
    addCheck(text::get("SHOW_COMBAT_TEXT_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_COMBAT_TEXT"), &s.showCombatText);
    static const std::string_view kModeKeys[] = {"COMBAT_TEXT_SCROLL_UP", "COMBAT_TEXT_SCROLL_DOWN",
                                                 "COMBAT_TEXT_SCROLL_ARC"};
    static std::array<std::string_view, 3> modes;
    for (u32 i = 0; i < modes.size(); ++i) {
        modes[i] = text::get(text::Key(kModeKeys[i]));
    }
    OptionRow& mode = addChoice(text::get("COMBAT_TEXT_FLOAT_MODE_LABEL"), tooltipOf("OPTION_TOOLTIP_COMBAT_TEXT_MODE"),
                                &s.combatTextFloatMode, modes, kFloatModes);
    mode.indent = true;
    mode.enabled = s.showCombatText;
    const struct {
        const char* label;
        const char* tooltip;
        bool* flag;
    } kCombatText[] = {
        {"COMBAT_TEXT_SHOW_LOW_HEALTH_MANA_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_LOW_HEALTH_MANA",
         &s.combatTextLowHealthMana},
        {"COMBAT_TEXT_SHOW_AURAS_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_AURAS", &s.combatTextAuras},
        {"COMBAT_TEXT_SHOW_AURA_FADE_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_AURA_FADE", &s.combatTextAuraFade},
        {"COMBAT_TEXT_SHOW_COMBAT_STATE_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_COMBAT_STATE",
         &s.combatTextCombatState},
        {"COMBAT_TEXT_SHOW_DODGE_PARRY_MISS_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_DODGE_PARRY_MISS",
         &s.combatTextDodgeParryMiss},
        {"COMBAT_TEXT_SHOW_RESISTANCES_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_RESISTANCES", &s.combatTextResistances},
        {"COMBAT_TEXT_SHOW_REPUTATION_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_REPUTATION", &s.combatTextReputation},
        {"COMBAT_TEXT_SHOW_REACTIVES_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_REACTIVES", &s.combatTextReactives},
        {"COMBAT_TEXT_SHOW_FRIENDLY_NAMES_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_FRIENDLY_NAMES",
         &s.combatTextFriendlyNames},
        {"COMBAT_TEXT_SHOW_COMBO_POINTS_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_COMBO_POINTS",
         &s.combatTextComboPoints},
        {"COMBAT_TEXT_SHOW_MANA_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_MANA", &s.combatTextMana},
        {"COMBAT_TEXT_SHOW_HONOR_GAINED_TEXT", "OPTION_TOOLTIP_COMBAT_TEXT_SHOW_HONOR_GAINED", &s.combatTextHonorGained},
    };
    for (const auto& c : kCombatText) {
        OptionRow& row = addCheck(text::get(text::Key(c.label)), tooltipOf(c.tooltip), c.flag);
        row.indent = true;
        row.enabled = s.showCombatText;
        if (c.flag == &s.combatTextComboPoints && playerClass != kClassRogue && playerClass != kClassDruid) {
            row.enabled = false; // combo points only mean something to rogues and druids
        }
    }
    addCheck(text::get("SHOW_DAMAGE_TEXT"), tooltipOf("OPTION_TOOLTIP_SHOW_DAMAGE"), &s.showDamage);
    OptionRow& periodic =
        addCheck(text::get("LOG_PERIODIC_EFFECTS"), tooltipOf("OPTION_TOOLTIP_LOG_PERIODIC_EFFECTS"), &s.logPeriodicEffects);
    periodic.indent = true;
    periodic.enabled = s.showDamage;
    OptionRow& pet =
        addCheck(text::get("SHOW_PET_MELEE_DAMAGE"), tooltipOf("OPTION_TOOLTIP_SHOW_PET_MELEE_DAMAGE"), &s.showPetDamage);
    pet.indent = true;
    pet.enabled = s.showDamage;
}

void OptionsScreens::buildSound(SettingsView& s) {
    m_rowCount = 0;
    m_shownCount = 0;
    const auto percent = [this](f32 value) -> std::string_view {
        std::array<char, 24>& buffer = m_shown[std::min<u32>(m_shownCount++, static_cast<u32>(m_shown.size() - 1))];
        std::snprintf(buffer.data(), buffer.size(), "%.0f%%", static_cast<double>(value * 100.0f));
        return buffer.data();
    };
    // SoundOptionsFrame: the check buttons at the left, the volumes at the right (two boxes here).
    addSection({});
    addCheck(text::get("ENABLE_ALL_SOUND"), tooltipOf("OPTION_TOOLTIP_ENABLE_ALL_SOUND"), &s.soundEffects);
    OptionRow& ambience = addCheck(text::get("ENABLE_AMBIENCE"), tooltipOf("OPTION_TOOLTIP_ENABLE_AMBIENCE"), &s.ambienceSounds);
    ambience.indent = true;
    ambience.enabled = s.soundEffects; // SoundOptionsFrame_UpdateDependencies
    OptionRow& errors =
        addCheck(text::get("ENABLE_ERROR_SPEECH"), tooltipOf("OPTION_TOOLTIP_ENABLE_ERROR_SPEECH"), &s.errorSpeech);
    errors.indent = true;
    errors.enabled = s.soundEffects;
    addCheck(text::get("ENABLE_MUSIC"), tooltipOf("OPTION_TOOLTIP_ENABLE_MUSIC"), &s.musicEnabled);
    addCheck(text::get("ENABLE_SOUND_AT_CHARACTER"), tooltipOf("OPTION_TOOLTIP_ENABLE_SOUND_AT_CHARACTER"),
             &s.soundAtCharacter);
    addCheck(text::get("ENABLE_EMOTE_SOUNDS"), tooltipOf("OPTION_TOOLTIP_ENABLE_EMOTE_SOUNDS"), &s.emoteSounds);
    addCheck(text::get("ENABLE_MUSIC_LOOPING"), tooltipOf("OPTION_TOOLTIP_ENABLE_MUSIC_LOOPING"), &s.musicLooping);
    addSection({});
    addSlider(text::get("MASTER_VOLUME"), tooltipOf("OPTION_TOOLTIP_MASTER_VOLUME"), &s.masterVolume, 0.0f, 1.0f, 0.05f,
              percent(s.masterVolume));
    addSlider(text::get("SOUND_VOLUME"), tooltipOf("OPTION_TOOLTIP_SOUND_VOLUME"), &s.effectsVolume, 0.0f, 1.0f, 0.05f,
              percent(s.effectsVolume));
    addSlider(text::get("MUSIC_VOLUME"), tooltipOf("OPTION_TOOLTIP_MUSIC_VOLUME"), &s.musicVolume, 0.0f, 1.0f, 0.05f,
              percent(s.musicVolume));
    addSlider(text::get("AMBIENCE_VOLUME"), tooltipOf("OPTION_TOOLTIP_AMBIENCE_VOLUME"), &s.ambienceVolume, 0.0f, 1.0f,
              0.05f, percent(s.ambienceVolume));
    // Ours (phase 9): the voices' and the interface's own volumes.
    addSlider(text::get("VD_AUDIO_VOICES_VOLUME"), {}, &s.voicesVolume, 0.0f, 1.0f, 0.05f, percent(s.voicesVolume));
    addSlider(text::get("VD_AUDIO_INTERFACE_VOLUME"), {}, &s.interfaceVolume, 0.0f, 1.0f, 0.05f,
              percent(s.interfaceVolume));
}

// --- The windows -----------------------------------------------------------------------------------------------

void OptionsScreens::open(Window window, GameSession* session, const SettingsView& settings) {
    m_tooltipRow = -1;
    m_openChoice = -1;
    if (window == Window::Sound) {
        m_soundInitial = settings; // SoundOptionsFrame_Load: each option's initialValue
        return;
    }
    // UIOptionsFrame_Load: the options as they are, the server's too.
    m_pending = settings;
    m_tab = 0;
    const bool online = session && session->inWorld();
    m_helm = !online || session->play().showingHelm();
    m_cloak = !online || session->play().showingCloak();
    const u8 bars = multiBars(session);
    for (u32 i = 0; i < m_bars.size(); ++i) {
        m_bars[i] = (bars & (1u << i)) != 0;
    }
    m_tutorials = !online || session->play().tutorials().enabled(); // phase 18 (p3)
}

void OptionsScreens::resetTutorials(GameSession* session) {
    // Phase 18 (p3): UIOptionsFrameResetTutorials' OnClick: gsTitleOptionOK, ResetTutorials, TutorialFrame_HideAllAlerts,
    // TutorialFrameCheckButton:SetChecked(1).
    m_uiSound(UiSound::TitleOptionOk);
    if (session && session->inWorld()) {
        session->play().tutorials().reset();
    }
    if (m_tutorialScreens) {
        m_tutorialScreens->hideAllAlerts();
        m_tutorialScreens->setDisplayTips(true);
    }
}

void OptionsScreens::apply(const OptionsWindowResult& r) {
    const auto binding = [this](i32 row) -> RowBinding* {
        return row >= 0 && static_cast<u32>(row) < m_rowCount ? &m_bindings[static_cast<u32>(row)] : nullptr;
    };
    if (RowBinding* b = binding(r.toggled)) {
        const bool on = !m_rows[static_cast<u32>(r.toggled)].checked;
        switch (b->kind) {
        case Binding::Bool: *b->flag = on; break;
        case Binding::InvertedBool: *b->flag = !on; break;
        case Binding::Helm: m_helm = on; break;
        case Binding::Cloak: m_cloak = on; break;
        case Binding::Bar1: m_bars[0] = on; break;
        case Binding::Bar2: m_bars[1] = on; break;
        case Binding::Bar3: m_bars[2] = on; break;
        case Binding::Bar4: m_bars[3] = on; break;
        case Binding::Tutorials: m_tutorials = on; break; // phase 18 (p3)
        default: break;
        }
        // OptionsCheckButton's OnClick: igMainMenuOptionCheckBoxOn / Off.
        m_uiSound(on ? UiSound::CheckBoxOn : UiSound::CheckBoxOff);
    }
    if (RowBinding* b = binding(r.slider); b && b->value) {
        *b->value = r.value;
    }
    if (r.choiceOpened >= 0) {
        m_openChoice = m_openChoice == r.choiceOpened ? -1 : r.choiceOpened;
    } else if (r.choice >= 0) {
        if (RowBinding* b = binding(m_openChoice); b && b->choice && b->values) {
            *b->choice = b->values[r.choice];
        }
        m_openChoice = -1;
    }
    if (r.held >= 0) {
        m_tooltipRow = r.held;
    } else if (r.tooltipDismissed || r.toggled >= 0 || r.slider >= 0 || r.choiceOpened >= 0 || r.tab >= 0) {
        m_tooltipRow = -1;
    }
}

bool OptionsScreens::draw(Window window, GameSession* session, DialogContext& c, SettingsView& settings) {
    VD_PROFILE_ZONE("OptionsScreens::draw");
    const bool online = session && session->inWorld();
    OptionsWindowView view;
    if (window == Window::Sound) {
        buildSound(settings); // the sound options act at once (SetCVar on every click and slider move)
        view.name = "soundoptions";
        view.title = text::get("SOUNDOPTIONS_MENU");
    } else {
        u8 playerClass = 0;
        if (const game::Entity* me = online ? session->entities().find(session->playerGuid()) : nullptr) {
            playerClass = me->byteValue(uf::UNIT_FIELD_BYTES_0, 1);
        }
        buildInterface(m_pending, playerClass);
        static std::array<std::string_view, 2> tabs;
        tabs = {text::get("BASIC_LABEL"), text::get("ADVANCED_OPTIONS")};
        view.name = "uioptions";
        view.title = text::get("UIOPTIONS_MENU");
        view.tabs = tabs;
        view.tab = m_tab;
    }
    view.rows = {m_rows.data(), m_rowCount};
    view.tooltipRow = m_tooltipRow;
    view.openChoice = m_openChoice;
    const OptionsWindowResult r = optionsWindow(c, view);
    if (r.tab >= 0 && static_cast<u32>(r.tab) != m_tab) {
        m_tab = static_cast<u32>(r.tab);
        m_openChoice = -1;
        m_uiSound(UiSound::CharacterTab); // igCharacterInfoTab
    }
    apply(r);
    if (r.button >= 0 && static_cast<u32>(r.button) < m_rowCount &&
        m_bindings[static_cast<u32>(r.button)].kind == Binding::Reset) {
        resetTutorials(session); // phase 18 (p3)
    }
    if (r.defaults) {
        if (window == Window::Sound) {
            soundDefaults(settings);
        } else {
            interfaceDefaults(m_pending); // UIOptionsFrame_SetDefaults: the helm and cloak shown, the bars off
            m_helm = m_cloak = true;
            m_bars = {};
            m_tutorials = true; // phase 18 (p3): OptionsFrame_EnableCheckBox(SHOW_TUTORIALS, 1, 1)
        }
    }
    if (r.cancel) {
        if (window == Window::Sound) {
            copySoundOptions(m_soundInitial, settings); // SoundOptionsFrame_Cancel
        }
        return false;
    }
    if (r.okay) {
        if (window == Window::Interface) {
            // UIOptionsFrame_Save: the options, ShowHelm / ShowCloak and SetActionBarToggles.
            copyInterfaceOptions(m_pending, settings);
            const u8 bars = static_cast<u8>((m_bars[0] ? 1u : 0u) | (m_bars[1] ? 2u : 0u) | (m_bars[2] ? 4u : 0u) |
                                            (m_bars[3] ? 8u : 0u));
            if (online) {
                game::Gameplay& play = session->play();
                play.showHelm(m_helm);
                play.showCloak(m_cloak);
                play.setActionBarToggles(bars);
                // Phase 18 (p3): SHOW_TUTORIALS changed: ResetTutorials (and the check button checked) or
                // ClearTutorials, and TutorialFrame_HideAllAlerts.
                if (m_tutorials != play.tutorials().enabled()) {
                    if (m_tutorials) {
                        play.tutorials().reset();
                    } else {
                        play.tutorials().clear();
                    }
                    if (m_tutorialScreens) {
                        m_tutorialScreens->hideAllAlerts();
                        if (m_tutorials) {
                            m_tutorialScreens->setDisplayTips(true);
                        }
                    }
                }
            } else {
                m_localBars = bars;
                m_localBarsSet = true;
            }
        }
        return false;
    }
    return true;
}

} // namespace vd
