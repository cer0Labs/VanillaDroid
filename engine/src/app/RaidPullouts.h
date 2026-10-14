#pragma once

#include "app/Dialogs.h"
#include "app/RaidPulloutWindows.h"
#include "app/UnitMenuWindows.h"
#include "core/Types.h"
#include "game/Auras.h"
#include "game/RaidAuras.h"
#include "game/Social.h"

#include <array>
#include <span>

namespace vd {

class AuraScreens;
class GameSession;
class Hud;
class InventoryScreens;
namespace game {
class AbilityData;
class GameData;
} // namespace game

// Phase 16: the raid's pullout frames (Blizzard_RaidUI.xml RaidPulloutFrameTemplate /
// RaidPulloutButtonTemplate, Blizzard_RaidUI.lua RaidPullout_*): small frames out of the raid window listing
// one subgroup or one class with their names, health and mana bars and auras, that stay on the screen while
// the raid lasts. The raid window (RaidGroupFrame) opens them when a group's label or a class button is
// dragged out of it. Main thread; fixed storage (no allocation per frame once warmed up).
//
// As 1.12: one frame per filter (RaidPullout_GetFrame), each the raid's members of that subgroup or class in
// raid index order (RAID_SUBGROUP_LISTS), rebuilt every frame from game::Social (RAID_ROSTER_UPDATE,
// UNIT_HEALTH, UNIT_AURA: in sight the units' fields, out of sight SMSG_PARTY_MEMBER_STATS), hidden when it
// lists nobody; not saved between sessions (the client makes them with CreateFrame). Touch: a tap on a member
// is RaidPulloutButton_OnClick's left button (the spell waiting for a unit, else TargetUnit; an item carried
// and let go on it, DropItemOnUnit), a tap on an aura its tooltip (the mouse-over), a long press anywhere its
// right click (RaidPulloutDropDown: Show Buffs, Show Debuffs, Hide Background, Close), and a drag anywhere
// moves it (it stays on the screen, off the stick, the action buttons and the unit frames). Development:
// VD_RAID_DEMO=1 (or `all`: every group and two classes) shows them over the --walk HUD with
// app/RaidPulloutSamples' raid of 40; VD_RAID_PULLOUTS=g1,g3,c5 opens those in a real raid.
class RaidPullouts {
public:
    // The tables for the auras' icons and filters (either may be null: no auras), the tooltips of the auras
    // tapped (app/AuraScreens) and the items dropped on members (app/InventoryScreens); null: not offered.
    // `sink`: the UIErrorsFrame (a trade that cannot start).
    using MessageSink = void (*)(void* context, u32 color, bool big, const char* line);
    void init(const game::GameData* data, const game::AbilityData* abilities, AuraScreens* auras,
              InventoryScreens* inventory, MessageSink sink, void* sinkContext);
    // UIOptionsFrame's raid options: SHOW_DISPELLABLE_DEBUFFS and SHOW_CASTABLE_BUFFS (RefreshBuffs'
    // filters).
    void setFilters(bool dispellableDebuffs, bool castableBuffs);
    // The rectangles of this frame the pullouts keep clear of (at most 10; empty ones are ignored).
    void setKeepClear(std::span<const ui::Rect> rects);
    // ...those of the HUD: the stick's pad (UiPanelPlaces::stickPad), the action buttons, the stance and
    // bottom left bars, the unit frames, the micro menu (`microMenu`: its strip) and the minimap cluster.
    void keepClearOfHud(const Hud& hud, const ui::Rect& stickPad, const ui::Rect& microMenu,
                        const DialogContext& c);

    // RaidPullout_GenerateGroupFrame(group): a pullout listing subgroup 0..7 with its top centre at (x, y)
    // (where the label was dropped). False when one already shows that subgroup (RaidPullout_GetFrame) or
    // nobody is in it.
    bool openGroup(GameSession& session, u8 subgroup, f32 x, f32 y);
    // RaidPullout_GenerateClassFrame(class, fileName): the same for a class (ChrClasses id).
    bool openClass(GameSession& session, u8 cls, f32 x, f32 y);
    // Every frame in the world, over the HUD and under the panels: the open pullouts and their taps. Out of a
    // raid they all close (the frames have nobody to list).
    void draw(GameSession& session, DialogContext& c, u32 nowMs);
    // After the panels: the open pullout's menu over them (RaidPulloutDropDown, modal while open), and an
    // item carried and let go on a member.
    void drawOverlay(GameSession& session, DialogContext& c, u32 nowMs);
    // Every frame closes (another character in the world, or none).
    void closeAll();

    // Phase 20 (assists, docs/MOBILE_CONTROLS.md §4): the member whose row of a pullout is under (x, y) as last drawn
    // (the top frame first), 0 none; `row` its rectangle. A helpful spell dragged from the touch cluster lands on it.
    u64 unitAt(f32 x, f32 y, ui::Rect& row) const;

    // Development and tests.
    u32 shownCount() const;
    bool menuOpen() const { return m_menuFrame >= 0; }
    bool demo() const { return m_demo != 0; }

private:
    struct Frame {
        bool shown = false;
        bool byClass = false;
        u8 filter = 0;          // subgroup 0..7, or ChrClasses id
        bool showBuffs = false; // the menu's Show Buffs (RaidPullout_Generate*: nil when it opens)
        bool background = true; // MenuBackdrop: its own state, kept when the frame is used again
        bool place = false;     // to be placed (just opened, or dropped)
        f32 x = 0.0f, y = 0.0f; // the frame's top left, logical pixels
        f32 unit = 1.0f;        // logical pixels per interface unit this frame
        bool dragging = false;  // a finger moves it from (dragX, dragY)
        f32 dragX = 0.0f, dragY = 0.0f;
        u32 count = 0;
        std::array<u8, kMaxRaidPulloutMembers> members{}; // indices into the roster
    };
    // A member's auras of one kind as the frames show them, kept while its auras and the filters do not
    // change.
    struct AuraCache {
        u64 guid = 0;
        u64 signature = 0;
        u32 count = 0;
        std::array<RaidPulloutAuraView, kRaidPulloutAuras> views{};
        std::array<u32, kRaidPulloutAuras> spells{};
        std::array<u8, kRaidPulloutAuras> slots{};
    };
    // What the filters need of an aura's spell, looked up once.
    struct SpellMemo {
        u32 spellId = 0;
        u64 iconHash = 0;
        u32 dispel = 0;
        bool listed = false;
        game::RaidAuraSpell spell;
    };
    static constexpr u32 kSpellMemoSize = 512;

    bool open(GameSession& session, bool byClass, u8 filter, f32 x, f32 y);
    void openListed(GameSession& session, const DialogContext& c, const char* list); // development
    i32 frameFor(bool byClass, u8 filter);  // RaidPullout_GetFrame: -1 when that filter shows already
    u32 gatherRoster(GameSession& session); // the raid (or the demo's), its count
    void listMembers(Frame& f) const;       // RaidPullout_Update's RAID_SUBGROUP_LISTS[filterID]
    void memberView(GameSession& session, u32 index, bool buffs, u32 nowMs, RaidPulloutMemberView& out);
    void memberAuras(GameSession& session, u32 index, bool buffs, RaidPulloutMemberView& out);
    const SpellMemo* spellMemo(u32 spellId);
    void updateDispelTypes(GameSession& session);
    std::string_view title(const Frame& f, std::span<char> out) const;
    RaidPulloutGeometry geometry(const Frame& f) const;
    void raise(u32 frame); // toplevel: a frame touched comes on top of the others
    void act(GameSession& session, Frame& f, u32 frameIndex, const RaidPulloutResult& r);
    void message(u32 color, std::string_view line);

    const game::GameData* m_data = nullptr;
    const game::AbilityData* m_abilities = nullptr;
    AuraScreens* m_auras = nullptr;
    InventoryScreens* m_inventory = nullptr;
    MessageSink m_message = nullptr;
    void* m_messageContext = nullptr;
    bool m_dispellableOnly = false;
    bool m_castableOnly = false;
    u8 m_demo = 0;                     // VD_RAID_DEMO: 1 a few frames, 2 every group and two classes
    bool m_autoOpened = false;         // the demo's (or VD_RAID_PULLOUTS') frames opened
    std::array<char, 64> m_autoList{}; // VD_RAID_PULLOUTS: frames to open in a raid ("g1,g3,c5")
    u32 m_autoRosterCount = 0;         // ...once the raid kept this size for a while (bots join one by one)
    u32 m_autoSinceMs = 0;
    bool m_trace = false;              // VD_TRACE_GAMEPLAY

    std::array<Frame, kMaxRaidPullouts> m_frames{};
    u32 m_frameCount = 0; // NUM_RAID_PULLOUT_FRAMES: frames made so far (a hidden one is used again first)
    std::array<u8, kMaxRaidPullouts> m_order{}; // drawing order of the frames made (the last on top)
    static constexpr u32 kMaxKeepClear = 10;
    std::array<ui::Rect, kMaxKeepClear + kMaxRaidPullouts>
        m_keepClear{}; // the HUD's, then (demo) the frames placed
    u32 m_keepClearCount = 0;
    ui::Rect m_safe; // the last frame's safe area and interface unit (where a frame opens)
    f32 m_baseUnit = 1.0f;

    std::array<game::PartyMemberView, kMaxRaidPulloutMembers> m_roster{};
    u32 m_rosterCount = 0;
    std::array<std::array<AuraCache, 2>, kMaxRaidPulloutMembers> m_auraCache{};
    std::array<game::AuraInfo, game::kMaxAuras> m_auraScratch{};
    std::array<RaidPulloutMemberView, kMaxRaidPulloutMembers> m_views{}; // the frame being drawn
    std::array<SpellMemo, kSpellMemoSize> m_memo{};
    u32 m_memoCount = 0;
    u32 m_dispelTypes = 0;  // the player's (game::raidDispelTypes of its spells)
    u32 m_knownVersion = 0; // the spell book's version they were taken from (0: never)

    // The menu of one frame (RaidPulloutDropDown), hanging at the frame's top right.
    i32 m_menuFrame = -1;
    std::array<DropDownItemView, 4> m_menuItems{};
};

} // namespace vd
