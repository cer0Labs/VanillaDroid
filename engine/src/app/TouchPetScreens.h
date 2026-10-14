#pragma once

#include "app/TouchPetWindows.h"
#include "core/Types.h"
#include "game/TouchLayout.h"
#include "game/TouchPet.h"
#include "ui/DrawList.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

class CombatScreens;
class GameSession;
class TouchBarScreens;
struct DialogContext;
struct TouchBarOutput;
struct TouchBarView;

namespace game {
class AbilityData;
class GameData;
} // namespace game

// Phase 20 (pets, docs/MOBILE_CONTROLS.md §6.8, §5.2 rows 1-3): the touch cluster's pet button and the states
// that take the cluster over.
// - The pet button: Call Pet / Revive Pet for a hunter without its pet (dead: as vMaNGOS answers Call Pet, or
//   seen dying), the last demon for a warlock (a hold: the fan of the demons known), and with a pet out a tap
//   that sends it at the target or calls it back, a hold that opens its panel (commands, reactions, its
//   spells with autocast, Mend Pet and the other spells on it, Feed Pet's list of foods, the dismiss /
//   abandon confirmations, the demons).
// - Another unit controlled (Mind Control, Eyes of the Beast, Eye of Kilrogg: the active mover is not the
//   player): that unit's bar in the cluster, an Exit in the reactive place.
// - No control (feared, stunned, confused, charmed): the cluster dimmed but what breaks the control
//   (Spell.dbc's immunities, as vMaNGOS' Spell::CheckCasterAuras) and the pet's button.
// - Dead or a ghost: the skills hidden; Release Spirit, Resurrect at the corpse, the soulstone or
//   Reincarnation, a resurrection offered (what the popups send; they stay).
// app/TouchBarScreens owns it and calls it at fixed points of its frame. Main thread; fixed arrays (nothing
// allocates per frame); the known spells are scanned when they change, the bags when the food list opens or
// they change. Development:
// VD_TOUCH_PETS_DEMO=pet|panel|feed|callpet|revive|demons|controlled|feared|dead|ghost (over VD_TOUCH_DEMO's
// cluster, without a server).
class TouchPetScreens {
public:
    void init(const game::GameData* data, const game::AbilityData* abilities);
    // A character entered the world: what was known of the last one's pet forgotten.
    void enterWorld(GameSession& session);
    // Once per frame before the views: the events (the pet's death, Call Pet's answer, a food refused), the
    // known spells when they change, the demon summoned last and the food eaten (both kept in the layout:
    // bar.changed()).
    void update(GameSession& session, TouchBarScreens& bar, u32 nowMs);
    // The pet button's view (TouchBarView::places[Pet]).
    void fillPetButton(GameSession& session, CombatScreens& combat, TouchBarScreens& bar, TouchBarView& view,
                       u32 nowMs);
    // After the views (§5.2 rows 1-3 over them): the controlled unit's bar (its PetActions in `resolved`) and
    // the Exit, no control, death. True when the reactive place is taken (the bar forgets its reactive
    // spell).
    bool takeOver(GameSession& session, CombatScreens& combat, TouchBarScreens& bar,
                  std::span<game::TouchAction> resolved, TouchBarView& view, u32 nowMs);
    // After the groups' fans: the demons' fan when the pet button's was opened (`fanPlace`).
    void fillFan(GameSession& session, CombatScreens& combat, i32 fanPlace, TouchBarView& view, u32 nowMs);
    // After the HUD: the pet button's tap, hold and fan, the Exit and the death buttons. True when a hold of
    // the pet button opened its panel (no tooltip then); `aim`: the unit an action was aimed at.
    bool actOnHud(GameSession& session, TouchBarScreens& bar, const TouchBarOutput& out,
                  CombatScreens& combat, u32 nowMs, u64& aim);
    // Over the HUD: the pet's panel, Feed Pet's list.
    void draw(GameSession& session, TouchBarScreens& bar, CombatScreens& combat, DialogContext& c, u32 nowMs);
    // Escape closes the panel or the list: true when one was open.
    bool escape();

    // A PetAction (game/TouchPet.h) fires as the finger goes down (§4): the Attack, a spell for an enemy.
    bool firesOnPress(GameSession& session, u32 id) const;

    // The edit mode's Pet tab (app/TouchEditScreens): the pet's spells (kept by spell: they follow the pet
    // that knows them), its commands and reactions, as PetActions (game/TouchPet.h). Returns how many were
    // written.
    struct PickerEntry {
        game::TouchAction action;
        u64 icon = 0;
        std::string_view name;
    };
    u32 pickerEntries(GameSession& session, CombatScreens& combat, std::span<PickerEntry> out) const;

    // --- VD_TOUCH_PETS_DEMO (development, without a server, over VD_TOUCH_DEMO's cluster) ------------------
    bool demo() const { return m_demo != Demo::None; }
    void prepareOffline(TouchBarView& view, i32 fanPlace, u32 nowMs);
    void actOffline(const TouchBarOutput& out);
    void drawOffline(DialogContext& c);

private:
    // What a button of the panel does.
    enum class PanelKind : u8 { Command, Reaction, PetSlot, OwnerSpell, Feed, Dismiss, Demon };
    struct PanelEntry {
        PanelKind kind = PanelKind::Command;
        u32 value = 0; // the command, the reaction, the bar slot, the spell
    };
    enum class Demo : u8 { None, Pet, Panel, Feed, CallPet, Revive, Demons, Controlled, Feared, Dead, Ghost };
    enum class Takeover : u8 { None, Controlled, NoControl, Dead };

    void readEvents(GameSession& session, TouchBarScreens& bar, u32 nowMs);
    void watchPet(GameSession& session, TouchBarScreens& bar, u32 nowMs);
    // The pet button's choice this frame.
    game::PetButtonChoice choose(GameSession& session, const game::TouchLayout& layout) const;
    void buildPanel(GameSession& session, CombatScreens& combat, u32 nowMs);
    void usePanel(GameSession& session, TouchBarScreens& bar, CombatScreens& combat, u32 index, bool held,
                  u64& aim);
    void buildFood(GameSession& session, const game::TouchLayout& layout);
    void feed(GameSession& session, u32 row, u32 nowMs);
    bool takeOverControlled(GameSession& session, CombatScreens& combat,
                            std::span<game::TouchAction> resolved, TouchBarView& view, u32 nowMs);
    void takeOverNoControl(GameSession& session, TouchBarScreens& bar,
                           std::span<const game::TouchAction> resolved, TouchBarView& view);
    bool takeOverDead(GameSession& session, std::span<game::TouchAction> resolved, TouchBarView& view,
                      u32 nowMs);
    const game::SpellImmunity& immunityOf(u32 spellId);
    u32 petFamily(GameSession& session) const;

    const game::GameData* m_data = nullptr;
    const game::AbilityData* m_abilities = nullptr;
    bool m_trace = false;
    u32 m_eventCursor = 0;

    // The player's pet spells (scanned when the known spells change) and the pet as last seen.
    game::PetSpellBook m_spells;
    u32 m_spellsVersion = 0xFFFFFFFFu;
    u64 m_petGuid = 0;
    bool m_petWasDead = false;
    bool m_knownDead = false; // the hunter's pet died and went away (or the server said PETTAME_DEAD)
    game::PetButtonChoice m_choice;
    ui::Rect m_petRect; // the pet button, where its windows open

    // The panel (a hold of the pet button with a pet out).
    static constexpr u32 kMaxPanel = 28;
    bool m_panelOpen = false;
    std::array<TouchPetPanelButton, kMaxPanel> m_panel{};
    std::array<PanelEntry, kMaxPanel> m_panelEntries{};
    u32 m_panelCount = 0;
    std::array<char, 64> m_subtitle{};

    // Feed Pet's list (rebuilt when it opens and while the bags change under it) and the food being eaten.
    static constexpr u32 kMaxFood = 24;
    bool m_foodOpen = false;
    u32 m_foodKey = 0;
    std::array<game::PetFoodItem, kMaxFood> m_foods{};
    std::array<TouchPetFoodRow, kMaxFood> m_foodRows{};
    u32 m_foodCount = 0;
    u32 m_feedEntry = 0, m_feedCount = 0, m_feedFamily = 0, m_feedMs = 0;

    // §5.2: what took the cluster over this frame.
    Takeover m_takeover = Takeover::None;
    std::array<i32, game::kTouchSkills> m_controlled{};
    u64 m_controlledKey = 0;
    u32 m_exitSpell = 0;
    u64 m_controlKey = 0; // the player's control auras the list below was made for
    std::array<game::ControlAura, 8> m_controls{};
    u32 m_controlCount = 0;
    bool m_charmed = false; // charmed or possessed by another: nothing works
    struct Immunity {
        u32 spellId = 0;
        game::SpellImmunity immunity;
    };
    std::array<Immunity, 128> m_immunities{};
    game::DeathButtons m_death;
    u32 m_deathSinceMs =
        0; // the body has lain there since (a tap on Release in the first moments is ignored)
    bool m_wasDead = false;
    u32 m_reclaimTotalMs = 0;
    u64 m_iconRelease = 0, m_iconResurrect = 0, m_iconExit = 0, m_iconDismiss = 0, m_iconCallPet = 0;

    // VD_TOUCH_PETS_DEMO.
    Demo m_demo = Demo::None;
    bool m_demoPanel = false;
    bool m_demoFood = false;
    bool m_demoWarlock = false;
};

} // namespace vd
