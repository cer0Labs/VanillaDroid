#pragma once

#include "app/SocialWindows.h"
#include "app/TouchBarView.h"
#include "core/Types.h"
#include "game/ActionBar.h"
#include "game/GameData.h"
#include "game/SpellText.h"
#include "game/TouchAssistRules.h"
#include "game/TouchLayout.h"
#include "platform/Haptics.h"
#include "ui/DrawList.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

class CombatScreens;
class GameSession;
class SpellTargetingScreens;
class TouchBarScreens;
struct DialogContext;
struct SettingsView;

namespace game {
class AbilityData;
} // namespace game
namespace ui {
struct Gui;
} // namespace ui

// Phase 20 (assists, docs/MOBILE_CONTROLS.md §4 and §8): the touch cluster's assists, each an option of the interface
// options' touch section (SettingsView::touch*), none doing more than a 1.12 macro could (P3):
// - Drag to a frame: a helpful spell's button dragged out of its circle lights the unit frame under the finger (the
//   player's, the target's, the target of target's, the party's and their pets', the raid pullouts', the pet's) and,
//   lifted there, casts the spell on that unit (CMSG_CAST_SPELL with the unit) without changing the target.
// - Aim on the ground: a ground spell's button dragged out aims around the player within its range (MOBA style: the
//   pad round the button is the range, up on the screen is away from the camera); the phase 13 circle shows there and
//   lifting casts there (Gameplay::targetLocation). Back on the button cancels both. A tap keeps phase 13's flow.
// - Automatic target: a hostile spell pressed without a hostile target first targets the unit the Target button would
//   (Hud::nextEnemy, TouchBarOutput::enemy).
// - Start the attack: the auto attack as 1.12's client starts it for SPELL_ATTR_EX_INITIATES_COMBAT /
//   _EX2_INITIATE_COMBAT_POST_CAST spells (our client did not), and the "attack macro" for Heroic Strike, Raptor
//   Strike and the seals (game::attackStartOf), at a hostile target not attacked yet.
// - Stances (warrior) and forms (druid, off by default): a spell of another stance takes it first and is cast once the
//   server shows it (within 1.5 s); a caster spell in an animal form leaves the form first. The first stance change of
//   a session says in a message line that the rage above what Tactical Mastery keeps is lost.
// - Repeat while held, robust: one request on its way at a time, none for a while after a refusal (game::
//   TouchRepeatGate); off: a held button shows its tooltip.
// - Spell queue (off by default): a press in the last 200 ms of the global cooldown or of a cast goes as it ends.
// - Haptics: a short tick on a cast, a double one when a reactive ability comes, a long one on a refusal
//   (platform/Haptics).
// - Template by site (off by default): entering the world, a dungeon, a raid or a battleground switches to the template
//   set for it (TouchTemplate::site, chosen in the edit mode's Places panel).
// - Healer frames: a template with TouchTemplate::healerFrames (the class presets' healing ones) brings the party
//   frames beside the cluster (healerDock).
// TouchBarScreens owns it and calls it at fixed points of its frame; main thread; fixed arrays, nothing allocates per
// frame. Spell.dbc is read when a button's spell changes (a small cache by spell id), the known forms when the spell
// book changes. Development: VD_TOUCH_ASSIST_DEMO=frame|dock|ground (with VD_TOUCH_DEMO=<class>:<level> and --walk):
// sample party frames to drag a helpful spell onto (dock: beside the cluster), a ground spell's aim, offline;
// VD_TRACE_GAMEPLAY=1: what the assists do, in the log; VD_TRACE_HAPTICS=1: each vibration.
class TouchAssists {
public:
    void init(const game::GameData* data, const game::AbilityData* abilities);
    // What the assists reach in the rest of the game (GameScreens wires it once): the unit frames under a point (the
    // raid pullouts', the party's, the pet's and target of target's; the player's and the target's come with
    // setHudFrames), the message lines (UIErrorsFrame), the ground circle.
    struct Links {
        using UnitAt = u64 (*)(void* context, f32 x, f32 y, ui::Rect& frame);
        UnitAt unitAt = nullptr;
        using Message = void (*)(void* context, u32 color, const char* line);
        Message message = nullptr;
        void* context = nullptr;
        SpellTargetingScreens* targeting = nullptr;
    };
    void link(const Links& links) { m_links = links; }
    void setOptions(const SettingsView& settings);
    const game::TouchAssistOptions& options() const { return m_options; }
    // The HUD's player and target frames and its touch cluster as drawn (Hud::playerFrameRect, targetFrameRect,
    // actionBarRect).
    void setHudFrames(const ui::Rect& player, const ui::Rect& target, const ui::Rect& cluster) {
        m_playerFrame = player;
        m_targetFrame = target;
        m_cluster = cluster;
    }
    // A character entered the world: what waited for the last one is forgotten.
    void enterWorld(GameSession& session);

    // --- TouchBarScreens' frame ---
    // Before the views: the server's answers (casts started, refused, gone off), the spell waiting for its form or the
    // global cooldown's end, the site's template.
    void update(GameSession& session, TouchBarScreens& bar, CombatScreens& combat, u32 nowMs);
    // After the views: which buttons aim (a helpful spell, a ground spell) and repeat, whether a press needs the
    // Target button's unit, a spell another stance makes usable shown usable, the reactive ability's double tick.
    void finishView(GameSession& session, TouchBarScreens& bar, std::span<const game::TouchAction> resolved,
                    u32 reactiveSpell, TouchBarView& view, u32 nowMs);
    // After the HUD, before its presses are used: the unit the Target button would choose (TouchBarOutput::enemy).
    void beginActions(const TouchBarOutput& out) { m_enemy = out.enemy; }
    // A spell of the cluster asked for (a press, a fan's choice, the reactive button; `repeat`: a held button again):
    // the assists around CombatScreens::useButton. Returns the unit aimed at (the player turns to it), 0 none.
    u64 cast(GameSession& session, CombatScreens& combat, u32 spellId, u32 nowMs, bool repeat = false);
    // Whether a held button may fire again now (its view's state; the option).
    bool repeatReady(GameSession& session, const HudActionSlot& slot, u32 nowMs) const;
    // A button dragged to aim (TouchBarOutput's drag): the frame or the ground under the finger, the cast when lifted.
    void drag(GameSession& session, const TouchBarOutput& out, u32 nowMs);
    // Over the HUD: the unit frame under a dragged helpful spell, lit.
    void draw(DialogContext& c);
    // Where the party frames go while the template in use has healerFrames: left of the cluster `cluster` (right of
    // it for the left hand), at its foot, over the movement stick's band when it is one at the bottom (`stickPad`,
    // UiPanelPlaces': the square screens); empty: their own place.
    ui::Rect healerDock(TouchBarScreens& bar, const ui::Rect& cluster, const ui::Rect& safe,
                        const ui::Rect& stickPad) const;
    // For TouchSmart's reactive button: the stance assist makes `spell` castable from `form` (Overpower in Berserker
    // Stance shows).
    static bool formReachable(const void* self, const game::SpellDetails& spell, u32 form);

    // --- Without a world: VD_TOUCH_ASSIST_DEMO ---
    bool demo() const { return m_demo != Demo::None; }
    void prepareOffline(TouchBarScreens& bar, game::TouchTemplate& t, game::TouchContext layer, TouchBarView& view,
                        u32 nowMs);
    void actOffline(const TouchBarOutput& out, u32 nowMs);
    void drawOffline(DialogContext& c);

private:
    // A spell's facts the assists read, looked up when a button's spell changes.
    struct Facts {
        u32 spellId = 0;
        bool found = false;
        game::SpellInfo info;
        game::SpellDetails details;
        bool hasDetails = false;
        game::SpellTargetKind kind = game::SpellTargetKind::None;
        bool ground = false; // waits for a place (game::SpellAim::Location)
    };
    const Facts* facts(u32 spellId);
    // The spell an action of the cluster casts now (a group's current one), 0 none.
    u32 spellOfAction(TouchBarScreens& bar, const game::TouchTemplate& t, const game::TouchAction& action) const;
    // Sends it now (the automatic target, the cast, the attack it starts).
    u64 castNow(GameSession& session, CombatScreens& combat, u32 spellId, u32 nowMs, bool repeat);
    // The target is a living unit the player can attack.
    bool hostileTarget(GameSession& session) const;
    // The unit frame under (x, y) and its rectangle (0: none).
    u64 unitAt(GameSession& session, f32 x, f32 y, ui::Rect& frame) const;
    void readAnswers(GameSession& session, u32 nowMs);
    void refreshForms(GameSession& session);
    void haptic(haptics::Pattern pattern) const;
    void message(std::string_view key) const;
    void endDrag();

    const game::GameData* m_data = nullptr;
    const game::AbilityData* m_abilities = nullptr;
    Links m_links;
    game::TouchAssistOptions m_options;
    bool m_trace = false;
    ui::Rect m_playerFrame, m_targetFrame, m_cluster;

    std::array<Facts, 32> m_facts{};
    // The player's forms (the stance assist's choices), from the known spells.
    std::array<game::ShapeshiftForm, game::kMaxShapeshiftForms> m_forms{};
    u32 m_formCount = 0;
    u32 m_formsVersion = 0;

    // The state machines.
    game::TouchRepeatGate m_gate;
    game::TouchSpellQueue m_queue;
    game::TouchFormSwitch m_formSwitch;
    game::TouchSiteSwitch m_site;
    bool m_stanceTold = false; // the rage message said this session
    u32 m_eventCursor = 0, m_fxCursor = 0;
    u64 m_player = 0;
    u64 m_enemy = 0;      // this frame's (TouchBarOutput::enemy)
    u32 m_pressSpell = 0; // the last spell a press sent, and when: its refusal buzzes
    u32 m_pressMs = 0;
    u32 m_afterSpell = 0; // a spell that starts the attack once it goes off (AttackStart::AfterCast)
    u64 m_afterTarget = 0;
    u32 m_afterUntilMs = 0;
    u16 m_reactiveShown = 0; // the reactive ability's button and glows last frame (a bit per place)

    // The buttons that aim this frame: their spell (0: none) and kind.
    std::array<u32, game::kTouchSlots> m_aimSpell{};
    u16 m_aimGround = 0;
    // The drag in progress: the frame under the finger (lit by draw()).
    i32 m_dragPlace = -1;
    u64 m_hoverGuid = 0;
    ui::Rect m_hover;
    bool m_hoverShown = false;

    // VD_TOUCH_ASSIST_DEMO.
    enum class Demo : u8 { None, Frame, Dock, Ground };
    Demo m_demo = Demo::None;
    bool m_demoDock = false;
    std::array<PartyFrameView, 4> m_demoParty{};
    std::array<ui::Rect, 8> m_demoFrames{};
    std::array<char, 64> m_demoLine{};
    u32 m_demoLineMs = 0;
};

// Phase 20 (assists): the edit mode's Places panel (app/TouchEditWindows) gets the template's site (§8 "Plantilla por
// sitio": a button that goes round None / World / Dungeon / Raid / Battleground) and its healer frames (§5.3) from
// here, `row` wide, from its top; returns the height used.
struct TouchAssistRows {
    i32 site = -1;             // the site chosen (TouchSite)
    bool toggleHealer = false; // the healer frames' box tapped
};
f32 touchAssistRows(ui::Gui& g, const ui::Rect& row, f32 rowHeight, u32 site, bool healerFrames, TouchAssistRows& out);

} // namespace vd
