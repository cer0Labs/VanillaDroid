#pragma once

#include "core/Types.h"

#include <array>
#include <string>
#include <string_view>

namespace vd::game {

// Phase 20 (docs/MOBILE_CONTROLS.md): the touch cluster of action buttons. A character keeps up to six templates
// ("plantillas"); each has the places of every element for the two screen profiles and the actions of its twelve
// content slots (nine skills, three consumables) in layers: the base, and one per context (a stance, a form,
// stealth, the hunter's melee range). A layer holds only what changes: an empty slot there shows the base's. This is
// the client's own data (WTF\Account\<ACCOUNT>\<realm>\<character>\touch-layout.txt, a copy in the server's account
// data type 6); the server's 120 action slots stay the 1.12 bars (the drawer, the classic mode). Plain data, no
// allocation once built; formatTouchLayout / parseTouchLayout allocate (load and save only).

// The cluster's elements (§3.1). The first twelve hold actions (the content slots); the others are fixed buttons whose
// action is the client's (jump, the next target...), shown when they have something to do.
enum class TouchPlace : u8 {
    Main,   // the big one: the most pressed skill
    RingA,  // the ring of medium ones around it (§7.1: A over it, C up-left, D left, E down-left, B up-right)
    RingB,
    RingC,
    RingD,
    RingE,
    ColumnF, // the column of small ones, left (once a fight)
    ColumnG,
    ColumnH,
    Consumable1, // the three consumables over the cluster (§6.1)
    Consumable2,
    Consumable3,
    Jump,      // under Main
    Mount,     // right of the jump: mount, travel form (§6.2)
    Reactive,  // right of Main: a reactive ability that just became usable (§6.3)
    Interact,  // down-left of Main: attack, talk, loot, use (the context button, §6.4)
    Target,    // the next enemy (§6.5)
    Templates, // the templates' button (§5.3)
    Drawer,    // the 1.12 bars (§6.9)
    Pet,       // the pet's button (§6.8)
    ClassBar,  // stances, forms, totems (§6.7): where its row starts (square) or its single button (wide)
    Count,
};
constexpr u32 kTouchSkills = 9;  // Main .. ColumnH
constexpr u32 kTouchSlots = 12;  // the skills and the consumables: what a layer holds
constexpr u32 kTouchPlaces = static_cast<u32>(TouchPlace::Count);
constexpr u32 touchIndex(TouchPlace place) {
    return static_cast<u32>(place);
}
constexpr bool isTouchSlot(TouchPlace place) {
    return static_cast<u32>(place) < kTouchSlots;
}

// The layers (§5.2): the forms with a bonus bar in 1.12 (SpellShapeshiftForm.dbc's bonus action bar: the warrior's
// stances, Stealth, Cat and Bear form) and those added for touch (Prowl, Moonkin, Travel / Aquatic / Ghost Wolf,
// Shadowform, the hunter's melee range).
enum class TouchContext : u8 {
    Base,
    BattleStance,
    DefensiveStance,
    BerserkerStance,
    Stealth,
    Cat,
    Prowl,
    Bear,
    Moonkin,
    Travel,
    Shadowform,
    Melee,
    Count,
};
constexpr u32 kTouchContexts = static_cast<u32>(TouchContext::Count);

enum class TouchActionKind : u8 {
    Empty,
    Spell,      // id: a spell; cast at its highest known rank unless kTouchFixedRank
    Item,       // id: an item entry (the first stack in the bags, or worn)
    Macro,      // id: a 1.12 macro (game/Macros)
    Group,      // id: one of the template's groups (§6.6)
    PetAction,  // id: a slot of the pet bar (0..9); phase 20 (pets): a pet spell or command (game/TouchPet.h)
    Consumable, // id: a TouchConsumable, the best one carried (§6.1)
    Special,    // id: a TouchSpecial
};

// The automatic consumables (§6.1).
enum class TouchConsumable : u8 { Health, Mana, Rage, Energy, Bandage, Food, Drink, Count };
// Buttons of the client's own that a slot may hold.
enum class TouchSpecial : u8 { Attack, Interact, Mount, Pet, Forms, Drawer, Count };

constexpr u8 kTouchManual = 0x01;    // put there by the player: the presets never change it
constexpr u8 kTouchHidden = 0x02;    // in a layer: nothing here (the base's action is not inherited)
constexpr u8 kTouchFixedRank = 0x04; // a spell: this very rank (healers' lower ranks)

struct TouchAction {
    TouchActionKind kind = TouchActionKind::Empty;
    u8 flags = 0;
    u16 reserved = 0;
    u32 id = 0;

    bool empty() const { return kind == TouchActionKind::Empty; }
    bool manual() const { return (flags & kTouchManual) != 0; }
    bool hidden() const { return (flags & kTouchHidden) != 0; }
    static TouchAction spell(u32 spellId, u8 flags = 0) { return {TouchActionKind::Spell, flags, 0, spellId}; }
    static TouchAction item(u32 entry, u8 flags = 0) { return {TouchActionKind::Item, flags, 0, entry}; }
    static TouchAction consumable(TouchConsumable c) {
        return {TouchActionKind::Consumable, 0, 0, static_cast<u32>(c)};
    }
    static TouchAction special(TouchSpecial s) { return {TouchActionKind::Special, 0, 0, static_cast<u32>(s)}; }
    friend bool operator==(const TouchAction& a, const TouchAction& b) {
        return a.kind == b.kind && a.flags == b.flags && a.id == b.id;
    }
};

// A group (§6.6): actions of which one is the current (a tap uses it; a hold opens the fan to choose another).
// Automatic ones (`autoKind` != 0) are refilled from the known spells by the class presets (game/ClassPresets).
struct TouchGroup {
    static constexpr u32 kMaxActions = 8;
    std::array<TouchAction, kMaxActions> actions{};
    u8 count = 0;
    u8 current = 0;
    u8 autoKind = 0;
    u8 flags = 0; // kTouchManual: the player's own list
    bool empty() const { return count == 0; }
};
constexpr u32 kTouchGroups = 16;
// The class bar (§6.7) shows the player's forms (warrior stances, druid forms, Shadowform), or these groups when a
// template sets them (the shaman's four totem groups): group index + 1, 0 none.
constexpr u32 kTouchClassBarGroups = 6;

// Where an element goes, in hundredths of the safe area's height (of the height a 16:9 screen of its width would have
// when it is squarer: the cluster keeps the proportions of the reference's 16:9 screen everywhere, as mobile games do,
// docs/MOBILE_CONTROLS.md §12): its center relative to Main's center (x right, y UP), and its diameter
// (0: never shown).
struct TouchPlacement {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 size = 0.0f;
};

// The two screen profiles (app/Dialogs' ScreenProfile): the Fold5's inner screen and tablets, the cover screen and
// 20:9 phones.
enum class TouchProfile : u8 { Square, Wide, Count };
constexpr u32 kTouchProfiles = static_cast<u32>(TouchProfile::Count);

struct TouchGeometry {
    f32 scale = 1.0f;       // the whole cluster (0.7 .. 1.3)
    f32 mainRight = 24.0f;  // from the safe area's right edge to Main's center (hundredths of its height)
    f32 mainBottom = 23.0f; // from its bottom edge
    std::array<TouchPlacement, kTouchPlaces> places{};
};

constexpr u32 kTouchNameBytes = 24;

struct TouchTemplate {
    std::array<char, kTouchNameBytes> name{};
    std::array<TouchGeometry, kTouchProfiles> geometry{};
    std::array<std::array<TouchAction, kTouchSlots>, kTouchContexts> layers{};
    std::array<TouchGroup, kTouchGroups> groups{};
    std::array<u8, kTouchClassBarGroups> classBarGroups{};
    u16 disabledLayers = 0; // bit per TouchContext: that layer is not used (the base shows)
    u8 role = 0;            // the class preset it was made from (game/ClassPresets; 0: none)
    u8 site = 0;            // TouchSite: switched to automatically there (§8)
    // Phase 20 (assists, §5.3 / §8): while this template is in use the party frames stand left of the cluster, near
    // the thumb (the class presets' healing templates).
    bool healerFrames = false;

    std::string_view nameView() const;
    void setName(std::string_view text);
    bool layerEnabled(TouchContext context) const {
        return context == TouchContext::Base || (disabledLayers & (1u << static_cast<u32>(context))) == 0;
    }
};

// Where a template is switched to automatically (§8: off unless the player sets it).
enum class TouchSite : u8 { None, World, Dungeon, Raid, Battleground, Count };

constexpr u32 kMaxTouchTemplates = 6;

// Phase 20 (pets, §6.8): what the hunter's pet families ate. The client cannot know a food's diet (the item query
// leaves out its food type): Feed Pet remembers the foods the server accepted for each family (the last one first) and
// leaves out those it refused (SPELL_FAILED_WRONG_PET_FOOD, _FOOD_LOWLEVEL).
struct TouchPetFood {
    static constexpr u32 kFamilies = 8;
    static constexpr u32 kAccepted = 6;
    static constexpr u32 kRefused = 16;
    struct Family {
        u32 family = 0; // CreatureFamily.dbc (0: unused)
        std::array<u32, kAccepted> accepted{}; // item entries, the last one first (0: none)
        std::array<u32, kRefused> refused{};
    };
    std::array<Family, kFamilies> families{}; // the last used first
};

struct TouchLayout {
    std::array<TouchTemplate, kMaxTouchTemplates> templates{};
    u32 count = 0;
    u32 active = 0;
    TouchAction mount; // §6.2: the mount the player chose (an item or a spell; empty: the fastest)
    // Phase 20 (pets, §6.8): the warlock's pet button summons the demon summoned last (its spell; 0: the first known),
    // and the hunter's Feed Pet remembers what the families ate.
    u32 lastDemon = 0;
    TouchPetFood petFood;

    TouchTemplate& current() { return templates[active < count ? active : 0]; }
    const TouchTemplate& current() const { return templates[active < count ? active : 0]; }
};

// The places of the reference picture (§3.2, square) and its flatter version for short screens (§3.3, wide).
TouchGeometry defaultTouchGeometry(TouchProfile profile);
// A template with the default places and no actions, named `name`.
TouchTemplate makeTouchTemplate(std::string_view name);

// The action slot `slot` (< kTouchSlots) shows in `context`: the layer's own when it has one (hidden: none), else the
// base's.
TouchAction resolveTouchAction(const TouchTemplate& t, TouchContext context, u32 slot);

// The file (§5.5): a first line "VDTOUCH 3" (1 and 2 are still read), then the templates. parseTouchLayout returns
// false (and leaves `out` empty) when the text is not ours: another format (1.12's own layout-cache.txt, which shares
// the server's account data type 6), or nothing.
std::string formatTouchLayout(const TouchLayout& layout);
bool parseTouchLayout(std::string_view text, TouchLayout& out);

// Names in the file (and in the development switches).
std::string_view touchPlaceName(TouchPlace place);
std::string_view touchContextName(TouchContext context);
bool touchPlaceByName(std::string_view name, TouchPlace& out);
bool touchContextByName(std::string_view name, TouchContext& out);

} // namespace vd::game
