#pragma once

#include "app/HudActionSlot.h"
#include "app/MinimapView.h"
#include "app/PlayerController.h"
#include "app/ScreenEdges.h"
#include "app/StatusFlash.h"
#include "app/TouchBarView.h" // phase 20
#include "app/Units.h"
#include "app/WorldTap.h"
#include "core/Types.h"
#include "game/Entities.h"
#include "game/Reactions.h"
#include "platform/Input.h"
#include "ui/Context.h"
#include "ui/DrawList.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace vd {

class FlyCamera;
class GameSession;
class UiResources;
struct PlayerFrameView;   // app/UnitFrames.h
struct TargetFrameView;   // app/UnitFrames.h
struct ExperienceBarView; // app/UnitFrames.h
class ChatBubbles;

// What the big button next to the action bar does with the current target. Phase 20 (smart, docs/MOBILE_CONTROLS.md
// §6.4): Resurrect (a dead friendly player: the class's spell) and Skin (a looted skinnable corpse), with the spell's
// icon (HudFrame::contextActionIcon).
enum class HudContextAction : u8 { None, Attack, StopAttack, Talk, Loot, Use, Resurrect, Skin };

struct HudCastBar {
    bool visible = false;
    std::string_view name;
    f32 progress = 0.0f; // 0..1
    bool channel = false;  // drains instead of filling
    bool failed = false;   // red, INTERRUPTED
    bool succeeded = false;
};

struct HudAura {
    u64 iconHash = 0;
    f32 remaining = 0.0f; // seconds (0: unknown or permanent)
    u32 stacks = 0;
    bool harmful = false;
    // Parity (auras, app/AuraScreens): the debuff's dispel type (Spell.dbc Dispel: 1 magic, 2 curse, 3 disease,
    // 4 poison; its border's DebuffTypeColor), and a weapon's temporary enchantment (TempEnchant: the weapon's icon).
    u8 dispel = 0;
    bool enchant = false;
};

// What the player asked for this frame through the HUD.
struct HudOutput {
    i32 action = -1;      // action bar slot pressed
    i32 actionHold = -1;  // action bar slot held (a long press: edit it)
    bool pageNext = false; // the action bar's page button, tapped
    bool pageHome = false; // ...held: back to the first page
    bool context = false; // the big button, tapped
    bool contextHold = false; // the big button, held (a long press: stop attacking)
    bool menu = false;
    bool targetFrame = false; // the target frame, tapped (phase 13: its unit tooltip stays, the client's hover)
    MinimapResult minimap;    // parity pass: what was done on the minimap cluster (app/MinimapScreens applies it)
    // Parity pass, unit frames (app/UnitScreens applies them).
    bool playerFrame = false;     // the player frame, tapped: target yourself (PlayerFrame_OnClick)
    bool playerFrameHold = false; // ...held: the player's menu (PlayerFrameDropDown)
    bool playerBadge = false;     // its PvP badge, tapped: what it means (PlayerPVPIconHitArea)
    bool targetFrameHold = false; // the target frame, held: its menu (phase 13: TargetFrameDropDown)
    u64 unitHold = 0;             // a unit in the world held still: selected, its tooltip stays (0: none)
    u64 objectHold = 0; // phase 16 (alerts): a game object held still (no unit there): its tooltip (the hover)
    bool experienceBar = false;   // the experience bar, tapped: its text and rest state (the client's hover)
    // Parity (world feedback): a quick tap on the unit already selected, the touch version of the client's
    // right click on it: its GUID (what the context button does with it, when it acts on that unit).
    u64 unitTapped = 0;
    u64 unitSelected = 0; // ours (NPC voices): a quick tap that selected a unit (the client's left click on it)
    // ... and a quick tap on a game object the client lets the player use (app/WorldTap.h): its GUID.
    u64 objectTapped = 0;
    // Parity (auras, app/AuraScreens applies them): an aura button tapped (its tooltip) or held (a buff: cancel it),
    // as an index into playerAuras, or targetAuras when auraOfTarget; auraRect: the button (the tooltip's anchor).
    i32 auraTapped = -1;
    i32 auraHeld = -1;
    bool auraOfTarget = false;
    ui::Rect auraRect;
    // Parity pass (action bars): the bottom left bar's buttons (MultiBarBottomLeft) and the stance bar's, tapped and
    // held like `action` / `actionHold`; where the held button is (its tooltip goes beside it).
    i32 extraAction = -1;
    i32 extraActionHold = -1;
    u8 extraBar = 0; // phase 15 (options): which extra bar they are on (0 bottom left, 1 bottom right, 2 right, 3 right 2)
    i32 stance = -1;
    i32 stanceHold = -1;
    ui::Rect holdRect;
    // Phase 20 (docs/MOBILE_CONTROLS.md): the touch cluster's (when HudFrame::touch is set).
    TouchBarOutput touch;
    // Phase 13 (spells): while a spell waits for a place (HudFrame::groundTargeting), a quick tap on the world
    // places its circle there instead of selecting (app/SpellTargetingScreens); where (logical pixels).
    bool groundTapped = false;
    f32 groundTapX = 0.0f, groundTapY = 0.0f;
};

// What the HUD reads each frame.
struct HudFrame {
    f32 width = 0.0f;  // logical pixels (as the player sees the screen)
    f32 height = 0.0f;
    f32 dp = 1.0f;     // logical pixels per dp (screen density x the player's UI scale)
    ui::Rect safeArea; // logical pixels
    ScreenEdges edges; // phase 20 (look): beyond it where no display cutout reaches (the experience bar)
    u64 nowMs = 0;
    f32 dt = 0.0f;
    GameSession* session = nullptr;     // online: the server's view of the world
    PlayerController* player = nullptr; // the character, if there is one
    FlyCamera* freeCamera = nullptr;    // world touches go to it instead of the character
    glm::mat4 viewProj{1.0f};           // camera-relative, as the renderer's (without the pre-rotation)
    glm::dvec3 camera{0.0};
    std::span<const Units::UnitView> units; // drawn last frame
    const game::Reactions* reactions = nullptr;
    std::string_view map;              // directory of the map shown (Map.dbc), for the minimap
    glm::dvec3 position{0.0};          // the player (or the camera, without a character)
    f32 facing = 0.0f;                 // radians, 0 = north (+X)
    std::span<const HudActionSlot> actions; // the action bar page shown (12 slots, empty ones without icon)
    u32 actionPage = 0;  // shown on the page button (0-based)
    u32 actionPages = 0; // 0: no page button
    bool actionPlacing = false; // a spell is being placed: empty slots take taps too
    HudContextAction context = HudContextAction::None;
    bool contextInRange = true; // false: shown red (walk closer first)
    // Settings (the game menu's OPTIONS).
    f32 cameraSpeed = 1.0f;
    bool invertCamera = false;
    f32 fps = 0.0f; // shown by the minimap when > 0
    HudCastBar cast;
    std::span<const HudAura> playerAuras;
    std::span<const HudAura> targetAuras;
    // Phase 8: the zone's name on the minimap's bar (MinimapZoneText), in its PvP color.
    std::string_view zoneName;
    u32 zoneColor = 0;
    // Parity: the player's and the target's portraits (app/Portraits); invalid: the race's TemporaryPortrait
    // art.
    TextureHandle playerPortrait;
    TextureHandle targetPortrait;
    // Parity pass: the minimap cluster (app/MinimapScreens fills it; nullptr: none), whether the player stands in a
    // building's interior (the minimap's indoor zoom) and the game clock (half-minutes since midnight, < 0 unknown).
    const MinimapView* minimap = nullptr;
    bool indoors = false;
    // Phase 15: the building the player is inside (its WMO, 0: none) and its model -> world rows and scale.
    u64 interiorWmo = 0;
    const f32* interiorTransform = nullptr;
    f32 interiorScale = 1.0f;
    f32 gameTime = -1.0f;
    // Parity pass: the player and target frames and the experience bar as app/UnitScreens describes them
    // (null: the offline stand-in player frame, no target frame, no bar).
    const PlayerFrameView* playerFrameView = nullptr;
    const TargetFrameView* targetFrameView = nullptr;
    const ExperienceBarView* experienceView = nullptr;
    // Parity (world feedback): chat bubbles over the speakers (app/ChatBubbles; nullptr: none); the name
    // plates of the client's V / Shift-V keys (hostile and neutral units / friendly ones, within 20 yards);
    // the context button's subject: its name and the client's cursor over it (an Interface\Cursor texture, 0:
    // none).
    ui::Rect chatRect; // parity (chat): the chat frame of the last frame: a tap there does not reach the world
    ui::Rect questWatchRect; // the watched quests over the chat, the same
    const ChatBubbles* bubbles = nullptr;
    bool enemyPlates = false;
    bool friendPlates = false;
    std::string_view contextName;
    u64 contextIcon = 0;
    std::span<const WorldObjectView> objects; // the game objects drawn last frame (tapping them)
    // Parity (auras): the target's auras as TargetFrame lays them out: a friendly target's buffs before its debuffs,
    // the debuff rows wrapping at 5 instead of 6 while the target of target shows.
    bool targetFriendly = false;
    bool targetOfTarget = false;
    // Parity pass (action bars): the bottom left bar (MultiBarBottomLeft, slots 60..71; empty: hidden; drawn where
    // it fits: the inner screen), whether the player wants it (SHOW_MULTIBAR1_TEXT), and the stance / shapeshift
    // bar's buttons (ShapeshiftBarFrame; empty: none).
    std::span<const HudActionSlot> extraActions;
    bool showExtraBar = true;
    std::span<const HudActionSlot> stances;
    // Phase 13 (spells): a spell waits for a place: quick taps on the world go to HudOutput::groundTapped. And
    // where a line from the camera meets the ground (terrain or the map's collision), for the circle under the
    // finger: the game's (null offline without a map).
    bool groundTargeting = false;
    using GroundPick = std::optional<glm::dvec3> (*)(const void* context, const glm::dvec3& from,
                                                     const glm::vec3& direction, f32 maxDistance);
    GroundPick groundPick = nullptr;
    const void* groundContext = nullptr;
    // Phase 13 (combat): UIParent.isOutOfControl (PLAYER_CONTROL_LOST, not on a taxi): OutOfControlFrame flashes.
    bool outOfControl = false;
    // Ours: a window covers the unit frames' band (app/UiPanels: UiPanelPlaces::overFrames, the wide screens): the
    // player and target frames are not drawn (their tops would show above it).
    bool unitFramesCovered = false;
    // Phase 15 (options): what the interface options change on the HUD (app/OptionsScreens::prepareHud). The names over
    // the units by kind (UnitNamePlayer, UnitNamePlayerGuild, UnitNamePlayerPVPTitle, UnitNameNPC, UnitNameOwn: WoW.exe
    // 0x6070A0 shows a name by the unit's kind, and always the one under the cursor: here the target, the touch's
    // hover); the bars' numbers always (statusBarText; else while the frame is touched); the buff durations; a tap on
    // the ground clearing the target (deselectOnClick); a spell waiting for a unit (a tap on one takes it); and the
    // extra action bars (SHOW_MULTI_ACTIONBAR_1..4: bits 0..3, the bottom left, bottom right, right and right 2 bars,
    // their slots in extraActions and moreBars; ALWAYS_SHOW_MULTIBARS shows their empty buttons).
    struct NameOptions {
        bool players = true;
        bool guilds = true;
        bool titles = true;
        bool npcs = false;
        bool own = false;
    };
    NameOptions names;
    bool statusBarText = false;
    bool showBuffDurations = false;
    bool deselectOnClick = true;
    u8 multiBars = 1;
    bool alwaysShowMultiBars = false;
    std::array<std::span<const HudActionSlot>, 3> moreBars; // the bottom right, right and right 2 bars
    // Phase 20 (docs/MOBILE_CONTROLS.md): the touch cluster instead of the 1.12 bars (null: the classic bars), and the
    // logical pixels of a millimetre on this screen (its places are kept in mm; 0: from the density).
    const TouchBarView* touch = nullptr;
    f32 mm = 0.0f;
    // Phase 20 (smart, §6.4 / §6.5): the context button's own icon when its action casts a spell (Resurrect, Skin), and
    // the next enemy's rule: a unit under a control the damage would break (Polymorph, Sap, Freezing Trap, Blind...:
    // an aura with AURA_INTERRUPT_DAMAGE_CANCELS) is skipped (null: none skipped).
    u64 contextActionIcon = 0;
    using UnitCheck = bool (*)(const void* context, u64 guid);
    UnitCheck breakableControl = nullptr;
    const void* breakableContext = nullptr;
    bool targetFanDemo = false; // VD_TOUCH_SMART_DEMO: the Target button's fan with sample units, without a server
};

// How a text over a unit lives (WoW.exe's world text types, PlayerName.cpp's table 0xCE8828).
enum class HudWorldText : u8 {
    Damage, // damage, heals, misses (types 0-3): rises and fades over 1.5 s
    Notice, // experience (type 4, "XP: 45") and honor (5, "HK: Private"): fades in for 0.5 s, stays to 2 s,
            // fades out to 4.5 s, in place
};

// The in-game touch interface (phase 7): unit frames, the movement stick, camera drag and pinch, buttons. Built every
// frame into a draw list for the renderer; fingers that land on no widget drive the stick and the camera.
class Hud {
public:
    bool init(UiResources* resources, u32 maxQuads);
    bool ready() const { return m_resources != nullptr; }

    void touchEvent(TouchPhase phase, i32 id, f32 x, f32 y) { m_ui.touchEvent(phase, id, x, y); }
    // One frame: begin(), then drawGame() for the in-game HUD (skipped behind the login screen), then windows drawn
    // on context() / drawList() (they go on top and take their fingers), then end().
    void begin(const HudFrame& frame);
    void drawGame(const HudFrame& frame);
    void end();
    void update(const HudFrame& frame) {
        begin(frame);
        drawGame(frame);
        end();
    }
    ui::Context& context() { return m_ui; }
    // Ours (quest marks): whether the unit's name or plate showed in the last frame drawn.
    bool nameShown(u64 guid) const {
        for (u32 i = 0; i < m_labelTopCount; ++i) {
            if (m_labelTops[i].guid == guid) {
                return true;
            }
        }
        return false;
    }
    ui::DrawList& drawList() { return m_draw; }

    std::span<const UiVertex> vertices() const { return m_draw.vertices(); }
    std::span<const UiDrawCmd> commands() const { return m_draw.commands(); }
    const HudOutput& output() const { return m_output; }
    // Floating combat text over a unit (damage, heals, misses: rises and fades over 1.5 s; `kind` Notice:
    // experience and honor).
    void combatText(u64 guid, std::string_view text, u32 color, bool big, u64 nowMs,
                    HudWorldText kind = HudWorldText::Damage);
    // Where the unit frames and the action bar cluster were drawn this frame (overlays anchor to them: the pet
    // frame and bar, combo points, the target's cast bar).
    const ui::Rect& playerFrameRect() const { return m_playerFrameRect; }
    const ui::Rect& targetFrameRect() const { return m_targetFrameRect; }
    const ui::Rect& actionBarRect() const { return m_actionBarRect; }
    const ui::Rect& experienceBarRect() const { return m_experienceBarRect; } // parity: rest tooltip anchor
    const ui::Rect& castBarRect() const { return m_castBarRect; } // phase 18 (p3): its place (its bar, not the border)
    // Parity (auras): the bottom of the target's aura rows (its frame's bottom without auras): its cast bar goes under.
    f32 targetAurasBottom() const { return m_targetAurasBottom; }
    // Phase 17 (scores): where the player's buff and debuff rows were drawn this frame (empty: none), for the world
    // state lines and the battle map to keep clear of them.
    const ui::Rect& playerAurasRect() const { return m_playerAurasRect; }
    // Parity pass: the stance bar's row along the bottom left of the action cluster (the pet bar goes above it)
    // and the bottom left bar (empty rectangles when not drawn).
    const ui::Rect& stanceBarRect() const { return m_stanceBarRect; }
    const ui::Rect& extraBarRect() const { return m_extraBarRect; }
    // Phase 15 (options): the extra bars drawn this frame (bits as HudFrame::multiBars): the others' pages stay among
    // the main bar's (VIEWABLE_ACTION_BAR_PAGES).
    u8 drawnMultiBars() const { return m_drawnMultiBars; }
    // Three fingers touched the world at once (show / hide the development panels); true once.
    bool takeDebugToggle() {
        const bool toggle = m_debugToggle;
        m_debugToggle = false;
        return toggle;
    }

private:
    struct Stick {
        i32 touch = -1;
        f32 originX = 0.0f, originY = 0.0f;
        glm::vec2 value{0.0f};
    };

    void worldControls(const HudFrame& frame, PlayerController::TouchInput& input);
    void drawStick(const HudFrame& frame);
    void playerFrame(const HudFrame& frame, const ui::Rect& area);
    void targetFrame(const HudFrame& frame, const ui::Rect& area);
    void namePlates(const HudFrame& frame);
    void experienceBar(const HudFrame& frame, const ui::Rect& area);
    void actionBar(const HudFrame& frame, PlayerController::TouchInput& input);
    // Phase 20 (app/HudTouchBar.cpp): the touch cluster of frame.touch instead of the bars; its edit mode
    // (app/HudTouchEdit.cpp); one round button; the open fan.
    void touchBar(const HudFrame& frame, PlayerController::TouchInput& input);
    struct TouchFrame; // where the cluster is this frame (HudTouchBar.cpp)
    void touchEdit(const HudFrame& frame, const TouchFrame& at);
    void touchButton(f32 cx, f32 cy, f32 r, const TouchButtonView& button, bool pressed, f32 alpha);
    void touchGesture(const HudFrame& frame, i32 place, const ui::WidgetState& state, const TouchButtonView& button,
                      f32 cx, f32 cy, f32 r);
    void touchFan(const HudFrame& frame, const TouchFrame& at);
    // Phase 20 (smart, §6.5): the Target button's tap, swipes and fan (`scale`: the cluster's pixels per mm).
    void targetButton(const HudFrame& frame, const ui::WidgetState& state, bool enabled, f32 cx, f32 cy, f32 scale);
    void castBar(const HudFrame& frame, const ui::Rect& area);
    // Parity (auras, app/HudAuras.cpp): the BuffFrame left of the minimap and the target's aura rows under its frame.
    void playerAuras(const HudFrame& frame);
    void targetAuras(const HudFrame& frame);
    // Parity pass (action bars): the stance / shapeshift row and the bottom left bar around the action cluster
    // (`k`: the cluster's scale; `bottomLeft`: the left end of its bottom row).
    void stanceBar(const HudFrame& frame, f32 k, bool wide, f32 bottomLeft);
    void extraBar(const HudFrame& frame, f32 k, bool wide);
    void floatingTexts(const HudFrame& frame);
    // An action button: slot art, icon, cooldown shade and seconds, dimmed when unusable, red when out of range.
    ui::WidgetState slotButton(ui::WidgetId id, const ui::Rect& r, const HudActionSlot& slot);
    // The MinimapCluster (app/MinimapView) of frame.minimap.
    void minimap(const HudFrame& frame);
    // Parity: a finger held still on a unit in the world (the touch screen's hover): HudOutput::unitHold.
    void heldUnit(const HudFrame& frame);
    // The unit under a tap (logical pixels), 0 if none.
    u64 pickUnit(const HudFrame& frame, f32 x, f32 y) const;
    // The client's "target nearest enemy" (Tab): the next living unit the player can attack, on screen and within
    // 40 yards, nearest first, after the current target. Phase 20 (smart, §6.5): those on screen first, then those
    // attacking the player or its group, then by distance; units under a control the damage would break skipped
    // (HudFrame::breakableControl), and those attacking the group behind the camera after the rest; `step` -1: the
    // previous one.
    u64 nextEnemy(const HudFrame& frame, i32 step = 1) const;
    // Phase 20 (smart, §6.5): the Target button's fan (held): the nearest enemy, the next party member, the target's
    // target, the pet, the player; what is not there is left out. Returns how many.
    struct TargetOption {
        u64 guid = 0;
        u64 icon = 0;
        std::string_view label;
    };
    u32 targetOptions(const HudFrame& frame, std::array<TargetOption, 5>& out) const;
    // The units nextEnemy chooses among (rank 0: on screen and attacking the group, 1 on screen, 2 attacking it off
    // screen), unsorted. Returns how many.
    struct EnemyCandidate {
        u64 guid = 0;
        f64 distance = 0.0;
        u8 rank = 0;
    };
    u32 enemyCandidates(const HudFrame& frame, std::array<EnemyCandidate, 64>& out) const;
    // Screen position of a world point; false behind the camera.
    bool project(const HudFrame& frame, const glm::dvec3& world, f32& x, f32& y) const;
    // How a unit looks to the player: its name / frame color.
    u32 reactionColor(const HudFrame& frame, const game::Entity& unit) const;
    // Original interface units (the client's 768-high UI) -> logical pixels.
    f32 u(f32 units) const { return units * m_unit; }

    UiResources* m_resources = nullptr;
    ui::Context m_ui;
    ui::DrawList m_draw;
    f32 m_unit = 1.0f;
    Stick m_stick;
    f32 m_pinchDistance = 0.0f;
    MinimapGesture m_minimapGesture; // two fingers on the minimap
    bool m_debugToggle = false;
    bool m_contextHeld = false; // its long press fired: the release is not a tap
    std::array<bool, 12> m_actionHeld{}; // the same for the action slots
    ui::Rect m_playerFrameRect, m_targetFrameRect, m_actionBarRect;
    f32 m_targetAurasBottom = 0.0f;
    ui::Rect m_playerAurasRect; // phase 17 (scores)
    bool m_pageHeld = false;
    // Parity pass (action bars): the long presses of the bottom left bar and the stance bar; where the stance row
    // and the bottom left bar were drawn (empty: not drawn).
    std::array<std::array<bool, 12>, 4> m_extraHeld{}; // phase 15 (options): one row per extra bar
    std::array<bool, 10> m_stanceHeld{};
    ui::Rect m_stanceBarRect, m_extraBarRect;
    u8 m_drawnMultiBars = 0;
    ui::Rect m_bottomRowRect; // the cluster's bottom row with the stance row beside it: the cast bar stays over it
    HudOutput m_output;
    struct FloatingText {
        u64 guid = 0;
        u64 startMs = 0;
        std::array<char, 24> text{};
        u32 color = 0;
        bool big = false;
        f32 drift = 0.0f; // sideways offset, so bursts do not overlap
        HudWorldText kind = HudWorldText::Damage;
    };
    std::array<FloatingText, 32> m_floating{};
    u32 m_floatingNext = 0;
    // Parity: the experience bar's place; the world finger whose hold on a unit was reported (and when it
    // went down).
    ui::Rect m_experienceBarRect;
    ui::Rect m_castBarRect; // phase 18 (p3)
    i32 m_heldTouch = -1;
    u64 m_heldDownMs = 0;
    // Parity (world feedback, app/HudWorld.cpp): the chat bubbles over the units shown (after their names),
    // and the context button's subject (name and cursor icon) next to the button `button`.
    void chatBubbles(const HudFrame& frame);
    void contextLabel(const HudFrame& frame, const ui::Rect& button);
    // The game object under a tap the player may use (logical pixels), 0 if none; the nearest to the camera.
    u64 pickObject(const HudFrame& frame, f32 x, f32 y) const;
    // The top of each unit's label this frame (bubbles stand on it), by GUID; 0 rows unused.
    struct LabelTop {
        u64 guid = 0;
        f32 top = 0.0f;
    };
    std::array<LabelTop, 64> m_labelTops{};
    u32 m_labelTopCount = 0;
    StatusFlash m_statusFlash; // phase 13 (combat): LowHealthFrame and OutOfControlFrame
    // Phase 20: each touch button's finger (fired as it went down, held, dragged out), the fan open (its place, where
    // its ring is), the templates' button's swipe.
    struct TouchPress {
        bool fired = false;
        bool held = false;
        bool dragging = false;
    };
    std::array<TouchPress, game::kTouchPlaces + kTouchClassBarItems> m_touchPress{};
    i32 m_fanPlace = -1;
    f32 m_fanX = 0.0f, m_fanY = 0.0f, m_fanRadius = 0.0f, m_fanItem = 0.0f, m_fanStart = 0.0f;
    i32 m_fanHover = -1;
};

} // namespace vd
