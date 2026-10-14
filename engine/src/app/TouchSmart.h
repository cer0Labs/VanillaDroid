#pragma once

#include "core/Types.h"
#include "game/Consumables.h"
#include "game/GameData.h"
#include "game/Mounts.h"
#include "game/ReactiveSpells.h"
#include "game/SpellText.h"
#include "game/TouchLayout.h"
#include "ui/DrawList.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

class CombatScreens;
class GameSession;
class TouchBarScreens;
struct DialogContext;
struct HudFrame;
struct TouchBarOutput;
struct TouchBarView;
enum class HudContextAction : u8;

namespace game {
class AbilityData;
class Reactions;
} // namespace game

// Phase 20 (smart, docs/MOBILE_CONTROLS.md §6): the touch cluster's buttons that decide what they do: the
// automatic consumables (§6.1: the best potion, stone, food, drink or bandage carried for each button now; a
// hold lists them to fix one), the mount / travel button (§6.2: dismount, the fastest mount, the class's
// travel form where a mount cannot go; a hold chooses a favourite mount), the reactive button (§6.3:
// Overpower, Revenge, Execute, Riposte... when they become usable, glowing where they already are) and, for
// GameScreens and the HUD whatever the action bars, the context button's new subjects (§6.4: resurrect a dead
// friendly player, skin a corpse, gather a herb or a vein) and the next enemy's rule (§6.5: units under a
// control the damage breaks are skipped). app/TouchBarScreens owns it. What it scans (the bags, the known
// spells) is scanned when it changes (the inventory's and the spell book's versions), never every frame. Main
// thread; no allocation per frame (fixed arrays).
class TouchSmart {
public:
    void init(const game::GameData* data, const game::AbilityData* abilities);
    // A character entered the world: what was scanned of the last one forgotten.
    void enterWorld();
    // Once per frame before the cluster's views: the bags scanned when they changed, the player's state;
    // `favorite`: the layout's mount (TouchLayout::mount).
    void update(GameSession& session, const HudFrame& hud, const game::TouchAction& favorite, u32 nowMs);

    // §6.1: the item a consumable button would use now (0: none carried: the button shows consumableIcon
    // dimmed).
    u32 consumableItem(GameSession& session, game::TouchConsumable kind, u32 nowMs);
    u64 consumableIcon(game::TouchConsumable kind) const;

    // §6.2: what the mount button shows and does now (empty: hidden), and its press.
    game::TouchAction mountAction(GameSession& session, u32 nowMs);
    void useMount(GameSession& session, CombatScreens& combat, u32 nowMs);

    // §6.3: a known spell usable now through an aura state (Overpower, Revenge, Execute, Riposte...) that is
    // not among `visible` (the spells on the cluster's buttons shown: those glow there instead); 0 none. The
    // longest cooldown first when several are: reactiveNow() lists them (the button's fan), reactiveGlows()
    // the visible slots that glow.
    u32 reactiveSpell(GameSession& session, std::span<const u32> visible, u32 nowMs);
    std::span<const game::ReactiveEntry> reactiveNow() const {
        return {m_reactiveNow.data(), m_reactiveNowCount};
    }
    bool reactiveGlows(u32 slot) const { return (m_reactiveGlow & (1u << slot)) != 0; }

    // After the cluster's views (TouchBarScreens::prepareHud): what the action bars' rules do not know: a
    // bandage greyed with Recently Bandaged's time, the mount button faint where it cannot be used or lit
    // while mounted, consumables and the mount firing on release (a hold lists them), the reactive glow and
    // fan, igBonusBarOpen when one comes.
    void finishView(CombatScreens& combat, std::span<const game::TouchAction> resolved, TouchBarView& view,
                    u32 nowMs);
    // After the group fans (TouchBarScreens::fillFan): the reactive button's fan when it was opened
    // (`fanPlace`).
    void fillFan(GameSession& session, CombatScreens& combat, i32 fanPlace, TouchBarView& view, u32 nowMs);
    // After the HUD (TouchBarScreens::actOnHud): a consumable slot or the mount button held opens its list
    // (true: no tooltip then); the reactive fan's choice cast. Returns the unit aimed at in `aim`.
    bool actOnHud(GameSession& session, TouchBarScreens& bar, const TouchBarOutput& out,
                  CombatScreens& combat, u64& aim);
    // Over the HUD: the open list (a tap on a row fixes it, "Automatic" lets the button choose again).
    void draw(GameSession& session, TouchBarScreens& bar, DialogContext& c, u32 nowMs);
    // Escape closes the list: true when it was open.
    bool escape();

    // --- GameScreens (the context button) and the HUD (the Target button), the classic bars too ---
    // Before the HUD, in the world: the player's state, the known spells listed when they change, the next
    // enemy's rule (hud.breakableControl).
    void prepareHud(GameSession& session, HudFrame& hud, u32 nowMs);
    // §6.4: what the context button does with the target when it is a dead friendly player (Resurrect: the
    // class's spell, Rebirth in combat) or a looted skinnable corpse (Skin); action None otherwise.
    struct Subject {
        HudContextAction action{};
        u64 guid = 0;
        bool inRange = true;
        u64 icon = 0; // the spell's
        u32 spellId = 0;
    };
    Subject subject(GameSession& session, u32 nowMs);
    // §6.4: the nearest herb or vein a gathering spell of the player opens (0 none), looked for every 250 ms.
    u64 gatherNode(GameSession& session, u32 nowMs);
    // The subject's spell cast on `guid` (the context button tapped on it).
    void useSubject(GameSession& session, u64 guid);

    // Whether `guid` is under a control the damage would break (an aura with AURA_INTERRUPT_DAMAGE_CANCELS).
    bool breakableControl(GameSession& session, u64 guid);

    // Phase 20 (assists): a reactive spell whose stance the stance assist takes first counts as usable in another one
    // (Overpower in Berserker Stance: app/TouchAssists::formReachable).
    using FormReach = bool (*)(const void* context, const game::SpellDetails& spell, u32 form);
    void setFormReach(FormReach reach, const void* context) {
        m_formReach = reach;
        m_formReachContext = context;
    }

    // --- VD_TOUCH_SMART_DEMO (development, without a server, over VD_TOUCH_DEMO's cluster) ---
    // Sample consumables (a bandage under Recently Bandaged), a mount, two reactive abilities (the button's
    // fan: its place `fanPlace` open) and one glowing in its place, the Target button's fan with sample units
    // (hud.targetFanDemo); a consumable or the mount held opens its list with sample rows.
    bool demo() const { return m_demo; }
    void prepareOffline(HudFrame& hud, TouchBarView& view, i32 fanPlace, u32 nowMs);
    void actOffline(const TouchBarOutput& out);
    void drawOffline(DialogContext& c);

private:
    // An item's requirements (the item query): the player's level and skills decide whether it can be used.
    struct Needs {
        u32 level = 0;
        u32 skill = 0;
        u32 skillRank = 0;
    };
    // A spell kept from the known spells (its tables looked up once).
    struct KnownSpell {
        u32 spellId = 0;
        u64 nameHash = 0;
        u32 rank = 0;
        u32 cooldownMs = 0;
        bool notInCombat = false;
        u64 icon = 0;
    };
    // A spell's facts the frames check, cached by id (spellFlags).
    static constexpr u8 kFlagKnown = 0x01;    // looked up
    static constexpr u8 kFlagBreaks = 0x02;   // AURA_INTERRUPT_DAMAGE_CANCELS (Polymorph, Sap, Blind...)
    static constexpr u8 kFlagBandaged = 0x04; // immune to bandages (Recently Bandaged)
    static constexpr u8 kFlagMount = 0x08;    // SPELL_AURA_MOUNTED
    u8 spellFlags(u32 spellId);

    // The player's state, once a frame (both entry points call it).
    void refresh(GameSession& session, const HudFrame& hud, u32 nowMs);
    void scanBags(GameSession& session);
    void scanSpells(GameSession& session);
    void composeTravel();
    bool itemUsable(const Needs& needs) const;
    u32 skillRank(u32 skill) const;
    const game::ConsumableItem* carried(u32 entry) const;
    void chooseTravel(GameSession& session);
    void listRows(GameSession& session, TouchBarScreens& bar);
    // One row of the list as drawn.
    struct ListView {
        u64 icon = 0;
        std::string_view name;
        u32 color = 0xFFFFFFFFu;
        u32 count = 0; // 0: not shown
    };
    // The list over m_listAnchor: the row tapped (-1 none); `closed`: a tap outside it.
    i32 listView(DialogContext& c, std::span<const ListView> rows, i32 checked, bool& closed);

    // A consumable slot or the mount button held: its list (false: not one of them, the tooltip shows
    // instead).
    bool openList(GameSession& session, TouchBarScreens& bar, i32 place, const ui::Rect& anchor);

    const game::GameData* m_data = nullptr;
    const game::AbilityData* m_abilities = nullptr;
    const game::Reactions* m_reactions = nullptr;
    GameSession* m_session = nullptr; // this frame's (the HUD's next-enemy rule calls back)
    u32 m_frame = 0;
    u32 m_nowMs = 0;
    bool m_refreshed = false;

    // The player this frame.
    u64 m_player = 0;
    u32 m_level = 0;
    u32 m_maxHealth = 0, m_maxMana = 0;
    bool m_inCombat = false;
    bool m_indoors = false;
    bool m_swimming = false;
    u32 m_form = 0;
    u32 m_mountAura = 0; // the mount's aura while mounted
    struct Skill {
        u32 id = 0;
        u32 rank = 0;
    };
    std::array<Skill, 64> m_skills{};
    u32 m_skillCount = 0;
    u32 m_skillsReadMs = 0;

    // §6.1: the consumables carried (scanned when the bags change, again while item queries are on their
    // way).
    u32 m_bagVersion = 0xFFFFFFFFu;
    bool m_bagPending = false;
    u32 m_bagScanMs = 0;
    std::array<game::ConsumableItem, 32> m_consumables{};
    std::array<Needs, 32> m_consumableNeeds{};
    u32 m_consumableCount = 0;
    std::array<u32, static_cast<u32>(game::TouchConsumable::Count)> m_choice{}; // the item chosen (0 none)
    std::array<u32, static_cast<u32>(game::TouchConsumable::Count)>
        m_choiceFrame{};                             // the frame it was chosen in
    bool m_bandaged = false;                         // Recently Bandaged on the player
    u32 m_bandagedLeftMs = 0, m_bandagedTotalMs = 0; // ...its time (0: not known)

    // §6.2: the mounts carried, the class mounts and travel forms known, and what the button does this frame.
    static constexpr u32 kMaxTravel = 24;
    std::array<game::TravelOption, 16> m_bagMounts{};
    std::array<Needs, 16> m_bagMountNeeds{};
    u32 m_bagMountCount = 0;
    std::array<game::TravelOption, 8> m_spellTravel{};
    u32 m_spellTravelCount = 0;
    std::array<game::TravelOption, kMaxTravel> m_travel{};
    std::array<Needs, kMaxTravel> m_travelNeeds{};
    u32 m_travelCount = 0;
    game::TouchAction m_favorite;
    game::TravelChoice m_travelChoice;
    u32 m_travelFrame = 0xFFFFFFFFu;

    // §6.3: the known reactive spells (highest ranks) and those usable now.
    u32 m_spellsVersion = 0;
    struct Reactive {
        KnownSpell spell;
        game::SpellInfo info;
        game::SpellDetails details;
    };
    std::array<Reactive, 8> m_reactive{};
    u32 m_reactiveCount = 0;
    std::array<game::ReactiveEntry, 8> m_reactiveNow{};
    u32 m_reactiveNowCount = 0;
    u16 m_reactiveGlow = 0;
    bool m_reactiveShown = false;
    u32 m_reactiveSoundMs = 0;

    // §6.4: the resurrection spells, the skinning spell, the locks the gathering spells open (bit per
    // LockType).
    std::array<KnownSpell, 4> m_resurrect{};
    u32 m_resurrectCount = 0;
    KnownSpell m_skinning;
    u32 m_gatherLocks = 0;
    u32 m_subjectSpell = 0;
    u64 m_gatherNode = 0;
    u32 m_gatherScanMs = 0;

    // Spell facts by id (open addressing; a full table only costs lookups).
    struct SpellFact {
        u32 spellId = 0;
        u8 flags = 0;
    };
    std::array<SpellFact, 512> m_spellFacts{};

    // The hold's list (§6.1 / §6.2): its button, its rows (row 0: "Automatic"), the one in use.
    struct ListRow {
        game::TouchAction action; // empty: Automatic
        u64 icon = 0;
        u32 quality = 1;
    };
    i32 m_listPlace = -1;
    ui::Rect m_listAnchor;
    game::TouchConsumable m_listKind = game::TouchConsumable::Health;
    std::array<ListRow, 13> m_rows{};
    u32 m_rowCount = 0;
    i32 m_rowChecked = 0;
    bool m_demo = false;  // VD_TOUCH_SMART_DEMO
    bool m_trace = false; // VD_TRACE_GAMEPLAY: what the smart buttons choose, to the log
    u32 m_tracedReactive = 0;
    FormReach m_formReach = nullptr; // phase 20 (assists)
    const void* m_formReachContext = nullptr;
};

} // namespace vd
