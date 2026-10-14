#pragma once

#include "app/TouchAssists.h" // phase 20 (assists)
#include "app/TouchBarView.h"
#include "app/TouchEditScreens.h"
#include "app/TouchPetScreens.h" // phase 20 (pets)
#include "app/TouchSmart.h"
#include "core/Types.h"
#include "game/ActionBar.h"
#include "game/ClassPresets.h"
#include "game/Entities.h"
#include "game/SpellLines.h"
#include "game/Spells.h"
#include "game/TouchContext.h"
#include "game/TouchLayout.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace vd {

class CombatScreens;
class GameSession;
class UiResources;
struct DialogContext;
struct HudFrame;
struct HudOutput;

namespace game {
class AbilityData;
class GameData;
} // namespace game

// Phase 20 (docs/MOBILE_CONTROLS.md): the touch cluster's glue (like GameScreens for the rest of the HUD). Keeps the
// character's touch layout (its file WTF\Account\<ACCOUNT>\<realm>\<character>\touch-layout.txt; a new character's is
// made from its class's presets, else from its server bars), decides the layer from the player's state, fills the
// HUD's TouchBarView (the buttons' states as the action bars' through CombatScreens) and applies what the player did.
// GameScreens owns it. Main thread; no allocation per frame once warmed up (the file is formatted on a save only).
class TouchBarScreens {
public:
    void init(const game::GameData* data, const game::AbilityData* abilities, UiResources* resources);
    using FileWriter = void (*)(void* context, const std::filesystem::path& path, std::string text);
    void setStorage(std::filesystem::path folder, FileWriter writer, void* context);
    // The player's choice: the touch cluster (on) or the classic 1.12 bars; the cluster for the left hand.
    void setOptions(bool enabled, bool leftHanded) {
        m_enabled = enabled;
        m_mirrored = leftHanded;
    }
    bool enabled() const { return m_enabled; }

    // A character entered the world: its layout read from its file (or made once its spells are known).
    void enterWorld(GameSession& session);
    // It left (or the session ended): what changed is saved.
    void leaveWorld();
    // Before the HUD draws: hud.touch set when the cluster is on (after CombatScreens::prepareActionBar).
    void prepareHud(GameSession& session, HudFrame& hud, CombatScreens& combat, u32 nowMs);
    // Without a world: VD_TOUCH_DEMO=<class>[:<layer>] shows a sample cluster (warrior, rogue; layer as in the file),
    // its fan opening (actOffline: the HUD's output) with sample actions.
    void prepareOffline(HudFrame& hud, u32 nowMs);
    void actOffline(const HudOutput& out);
    // After the HUD: what the player did with the cluster. Returns the unit an action was aimed at (the player turns
    // to it), 0 none.
    u64 actOnHud(GameSession& session, const HudOutput& out, CombatScreens& combat, u32 nowMs);
    // Over the HUD: the windows of the templates, the edit mode and the drawer (TouchEditScreens).
    void draw(GameSession& session, CombatScreens& combat, DialogContext& c, u32 nowMs);
    // Escape closes the edit mode, the template list or the drawer: true when one was up.
    bool escape() { return m_pets.escape() || m_smart.escape() || m_edit.escape(); } // phase 20 (smart, pets): lists first

    // --- For the edit mode and the presets ---
    game::TouchLayout& layout() { return m_layout; }
    bool haveLayout() const { return m_haveLayout; }
    game::TouchContext context() const { return m_context; }
    game::TouchProfile profile() const { return m_profile; }
    // The layout changed (a place moved, an action put, a template chosen): saved a moment later.
    void changed();
    void selectTemplate(u32 index, u32 nowMs);
    TouchEditScreens& edit() { return m_edit; }
    TouchSmart& smart() { return m_smart; }
    TouchPetScreens& pets() { return m_pets; } // phase 20 (pets)
    const game::SpellLines& lines() const { return m_lines; }
    // The spell a spell action casts now: its highest known rank, unless it keeps its own (kTouchFixedRank).
    u32 spellOf(const game::TouchAction& action) const;
    // What the class presets are built from (the player's class, race, level, spells, weapons).
    game::PresetInput presetInput(GameSession& session) const;
    // The 1.12 bars' first page (and each bonus bar on its layer) into a template: a character without a class
    // preset, and VD_TOUCH_DEMO.
    void fillFromBars(GameSession& session, game::TouchTemplate& t) const;
    // phase 20 (smart): public API here
    // Without a world: VD_TOUCH_SMART_DEMO's lists (a consumable or the mount held), over the HUD.
    void drawOffline(DialogContext& c) {
        m_smart.drawOffline(c);
        m_assists.drawOffline(c); // phase 20 (assists): VD_TOUCH_ASSIST_DEMO's sample party and drag
        m_pets.drawOffline(c); // phase 20 (pets): VD_TOUCH_PETS_DEMO's panel and list
    }
    // phase 20 (edit): public API here
    // The view of `action` as the cluster shows it (cache < CombatScreens::kTouchViews), and what content slot `slot`
    // uses this frame (the layer's or the base's): the edit mode and the tooltips.
    void viewOf(GameSession& session, CombatScreens& combat, const game::TouchAction& action, u32 cache, u32 nowMs,
                TouchButtonView& out) {
        actionView(session, combat, action, cache, nowMs, out);
    }
    const game::TouchAction& resolved(u32 slot) const { return m_resolved[slot < game::kTouchSlots ? slot : 0]; }
    const game::GameData* gameData() const { return m_data; }
    const game::AbilityData* abilityData() const { return m_abilities; }
    bool mirrored() const { return m_mirrored; }
    // A layout used as it is: the server's copy (account data type 6, §5.5) or VD_TOUCH_EDIT_DEMO's. The text: false
    // (nothing changes) when it is not a touch layout (1.12's own layout-cache.txt shares the type).
    bool adoptLayout(std::string_view text);
    void adoptLayout(const game::TouchLayout& layout);
    // After each save of the file (app/AccountDataFiles: the account data's 30 s timer), and a save now (the edit
    // mode's Done).
    using SaveHook = void (*)(void* context);
    void setSaveHook(SaveHook hook, void* context) {
        m_saveHook = hook;
        m_saveHookContext = context;
    }
    void saveNow() {
        if (m_dirty) {
            save();
        }
    }
    // Without a world: VD_TOUCH_EDIT_DEMO's windows (after the HUD; `cluster`: Hud::actionBarRect()).
    void drawOffline(DialogContext& c, ui::TextInput& input, const ui::Rect& cluster, u32 nowMs);
    // phase 20 (classes): public API here
    // phase 20 (assists): public API here
    // The assists (§4 / §8): their options, the frames and screens they reach (GameScreens wires them).
    TouchAssists& assists() { return m_assists; }
    // Where the party frames go while the template in use has healerFrames (empty: their own place).
    ui::Rect healerDock(const ui::Rect& cluster, const ui::Rect& safe, const ui::Rect& stickPad) {
        return m_assists.healerDock(*this, cluster, safe, stickPad);
    }

private:
    // The view of an action in content slot / cache `cache` (< CombatScreens::kTouchViews).
    void actionView(GameSession& session, CombatScreens& combat, const game::TouchAction& action, u32 cache, u32 nowMs,
                    TouchButtonView& out, u32 depth = 0);
    // Uses an action; returns the unit it was aimed at.
    u64 execute(GameSession& session, CombatScreens& combat, const game::TouchAction& action, u32 nowMs,
                u32 depth = 0, bool repeat = false); // phase 20 (assists): `repeat`, a held button again
    // The group an action stands for (a Group action), null if none.
    game::TouchGroup* groupOf(const game::TouchAction& action);
    void buildLayout(GameSession& session);
    void save();
    void fillFan(GameSession& session, CombatScreens& combat, u32 nowMs);
    void fillClassBar(GameSession& session, CombatScreens& combat, u32 nowMs);
    // A spell's facts the buttons need (looked up when the spell changes, not every frame).
    struct SpellFacts {
        u32 spellId = 0;
        bool hostile = false;   // its target is an enemy: it fires as the finger goes down, repeats when held
        bool toggle = false;    // Attack, Auto Shot, Shoot, next-swing spells, forms: never repeated
    };
    const SpellFacts& facts(u32 cache, u32 spellId);

    const game::GameData* m_data = nullptr;
    const game::AbilityData* m_abilities = nullptr;
    UiResources* m_resources = nullptr;
    TouchSmart m_smart;
    TouchEditScreens m_edit;
    bool m_enabled = true;
    bool m_mirrored = false;
    bool m_trace = false;

    // The character's layout and its file.
    game::TouchLayout m_layout;
    bool m_haveLayout = false;
    bool m_needBuild = false;
    std::filesystem::path m_folder;
    std::filesystem::path m_file;
    FileWriter m_writer = nullptr;
    void* m_writerContext = nullptr;
    bool m_dirty = false;
    u32 m_dirtySinceMs = 0;
    u32 m_nowMs = 0;

    // This frame.
    game::TouchContext m_context = game::TouchContext::Base;
    game::TouchProfile m_profile = game::TouchProfile::Square;
    bool m_hunterMelee = false;
    game::SpellLines m_lines;
    u32 m_presetVersion = 0; // the known spells' version the presets were refreshed for
    std::array<game::TouchAction, game::kTouchSlots> m_resolved{};
    game::TouchAction m_mountAction;
    u32 m_reactiveSpell = 0;
    std::array<u16, game::kTouchContexts> m_newSlots{}; // "New" (§4): per layer, a bit per slot
    std::array<SpellFacts, 64> m_facts{};
    TouchBarView m_view;
    i32 m_fanPlace = -1; // the fan the HUD opened (its place, or kTouchClassBarFan + i)
    u32 m_lastRepeatMs = 0;
    u64 m_templateShownMs = 0;
    // phase 20 (smart): members here
    // phase 20 (pets): members here
    TouchPetScreens m_pets;
    // phase 20 (edit): members here
    SaveHook m_saveHook = nullptr;
    void* m_saveHookContext = nullptr;
    TouchAssists m_assists; // phase 20 (assists)
    // phase 20 (classes): members here
    // Phase 20 (classes): the presets refilled when the weapons change (§7.1: Backstab with a dagger, Shield Bash
    // with a shield), as when a spell is learned; and VD_TOUCH_DEMO=<class>:<level>[:...] (app/TouchClassesDemo.cpp):
    // the class presets' cluster for a sample of the spells known at that level, without a server.
    void refreshPresetsOnWeapons(GameSession& session);
    bool prepareClassesDemo(HudFrame& hud, u32 nowMs);
    void fillClassesDemoView(u32 nowMs);
    u64 m_presetWeapons = 0;
    bool m_classesDemoParsed = false;
    bool m_classesDemo = false;
    game::TouchContext m_classesDemoLayer = game::TouchContext::Base;
    game::TouchLayout m_classesLayout;
    std::vector<u32> m_classesKnown;
    std::array<u16, game::kTouchContexts> m_classesNew{};
    std::array<game::ShapeshiftForm, game::kMaxShapeshiftForms> m_classesForms{};
    u32 m_classesFormCount = 0;
    // VD_TOUCH_DEMO.
    std::array<char, 32> m_demo{};
    game::TouchTemplate m_demoTemplate;
};

} // namespace vd
