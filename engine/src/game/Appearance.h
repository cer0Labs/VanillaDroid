#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "db/DbcView.h"
#include "game/GuildEmblem.h"

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// Equipment slots (EQUIPMENT_SLOT_*): also the order of PLAYER_VISIBLE_ITEM_* and of the char enum's items.
enum EquipSlot : u8 {
    kSlotHead = 0,
    kSlotNeck = 1,
    kSlotShoulders = 2,
    kSlotShirt = 3,
    kSlotChest = 4,
    kSlotWaist = 5,
    kSlotLegs = 6,
    kSlotFeet = 7,
    kSlotWrists = 8,
    kSlotHands = 9,
    kSlotBack = 14,
    kSlotMainHand = 15,
    kSlotOffHand = 16,
    kSlotRanged = 17,
    kSlotTabard = 18,
    kEquipSlots = 19,
};

// InventoryType of the items the look depends on.
constexpr u8 kInventoryShield = 14;
constexpr u8 kInventoryRobe = 20;
constexpr u8 kInventoryHoldable = 23;
constexpr u8 kInventoryRanged = 15;      // bows (phase 13)
constexpr u8 kInventoryThrown = 25;
constexpr u8 kInventoryRangedRight = 26; // guns, crossbows, wands

// Attachment ids of the models (M2 attachments).
namespace attach {
constexpr u32 kShield = 0; // also where a mount carries its rider
constexpr u32 kHandRight = 1;
constexpr u32 kHandLeft = 2;
constexpr u32 kShoulderRight = 5;
constexpr u32 kShoulderLeft = 6;
constexpr u32 kHelm = 11;
constexpr u32 kName = 18; // over the head
constexpr u32 kNameMounted = 29; // ours (quest marks): over a rider's head (PlayerNameMounted)
constexpr u32 kSheathMainHand = 26;
constexpr u32 kSheathOffHand = 27;
constexpr u32 kSheathShield = 28;
constexpr u32 kLargeWeaponLeft = 30; // phase 13: staves (sheath type 2) across the back, as WoW.exe's 0x47A070
constexpr u32 kLargeWeaponRight = 31;
constexpr u32 kHipLeft = 32;
constexpr u32 kHipRight = 33;
} // namespace attach

// Regions of the 256x256 character skin atlas (1.12 layout).
enum class SkinRegion : u8 {
    Base, ArmUpper, ArmLower, Hand, FaceUpper, FaceLower, TorsoUpper, TorsoLower, LegUpper, LegLower, Foot, Count
};
struct SkinRect {
    f32 x, y, width, height;
};
SkinRect skinRegionRect(SkinRegion region);
constexpr u32 kSkinSize = 256;

// What a character looks like: race, gender, customization and the display of each equipped item.
struct CharacterLook {
    u8 race = 1;
    u8 gender = 0; // 0 male, 1 female
    u8 skin = 0;
    u8 face = 0;
    u8 hairStyle = 0;
    u8 hairColor = 0;
    u8 facialHair = 0;
    std::array<u32, kEquipSlots> items{};         // ItemDisplayInfo id per equipment slot, 0 = empty
    std::array<u8, kEquipSlots> inventoryTypes{}; // InventoryType of each (0 = unknown)
    std::array<u8, kEquipSlots> sheaths{};        // Sheath type of weapons (item template), 0 = unknown
    // Phase 15 (guild): the guild's tabard design, shown on a guild tabard (not designed: the tabard's own art).
    GuildEmblem emblem;
    bool operator==(const CharacterLook&) const = default;
};

// A model hung on an attachment point: weapons, shields, helmets, shoulder pads.
struct AttachedModel {
    u32 attachment = 0;
    u64 model = 0;   // actors.vdpk asset hash
    u64 texture = 0; // textures.vdpk asset hash for the model's object skin (slot Cape), 0 = none
    u8 slot = 0xFF;  // phase 9: the EquipSlot of a weapon or shield (its enchantment glows on it), 0xFF: none
    bool operator==(const AttachedModel&) const = default;
};

// One texture of a composed skin, drawn over the region in order.
struct SkinLayer {
    u64 texture = 0;
    SkinRegion region = SkinRegion::Base;
    u8 order = 0;
    bool operator==(const SkinLayer&) const = default;
};

// Everything needed to draw an actor, resolved from the client tables.
struct ActorLook {
    u64 model = 0; // actors.vdpk asset hash, 0 = nothing to draw
    f32 scale = 1.0f;
    std::array<u64, 8> textures{}; // per pack::ActorTextureSlot, texture hashes (0 = none)
    std::vector<SkinLayer> skin;   // non-empty: the Body slot is composed from these layers (sorted by order)
    std::vector<u16> geosets;      // visible geoset ids, sorted; empty = every geoset
    std::vector<AttachedModel> attachments;
    bool operator==(const ActorLook&) const = default;
};

// The client tables that describe how creatures and characters look (db.vdpk), indexed at start-up. Lookups are
// cheap but allocate the result: call them when an appearance changes, not every frame.
class AppearanceDb {
public:
    // `textures` (textures.vdpk) and `actors` (actors.vdpk) are probed for the variants of item textures and models
    // (gender, race, folder); all three must stay open.
    Result<void> init(const pack::PackReader& db, const pack::PackReader& textures, const pack::PackReader& actors);
    bool ready() const { return m_ready; }

    // A creature display (CreatureDisplayInfo), including humanoid NPCs dressed by CreatureDisplayInfoExtra.
    // `weapons`: virtual item display ids (main hand, off hand, ranged), `weaponTypes` their InventoryType.
    // Phase 13: `rangedDrawn` (sheath state 2, SHEATH_STATE_RANGED): the ranged weapon in the hands (WoW.exe 0x47A0C0:
    // a bow in the left, a gun, crossbow, wand or thrown weapon in the right), the melee ones sheathed. Sheathed, a
    // ranged weapon shows only where its item has a sheath type, and none has one in 1.12.
    ActorLook creature(u32 displayId, const std::array<u32, 3>& weapons = {},
                       const std::array<u8, 3>& weaponTypes = {}, bool sheathed = true, bool rangedDrawn = false) const;
    // A player character, its skin composed from the customization and the equipped items.
    ActorLook character(const CharacterLook& look, bool sheathed = true, bool rangedDrawn = false) const;
    // Model of a race and gender, 0 if unknown.
    u64 raceModel(u8 race, u8 gender) const;
    // Name plate height over the model origin, from the display's scale (yards, approximate).
    f32 displayScale(u32 displayId) const;

    // Number of customization choices (for random looks): 0 when the combination is unknown.
    u32 skinCount(u8 race, u8 gender) const;
    u32 faceCount(u8 race, u8 gender, u8 skin) const;
    u32 hairStyleCount(u8 race, u8 gender) const;
    u32 hairColorCount(u8 race, u8 gender) const;
    u32 facialHairCount(u8 race, u8 gender) const;
    // Every ItemDisplayInfo id NPCs wear in `slot` (CreatureDisplayInfoExtra), for dressing synthetic crowds.
    const std::vector<u32>& npcItems(EquipSlot slot) const;
    // Displays of weapons and shields (models in the Weapon / Shield folders).
    const std::vector<u32>& weaponDisplays() const { return m_weapons; }
    const std::vector<u32>& shieldDisplays() const { return m_shields; }
    // Phase 9: the model file of an item display ("Sword_1H_Short_A_01.mdx"), empty if unknown (synthetic fights
    // tell a staff from a bow by it).
    std::string_view itemModel(u32 displayId) const;
    // Phase 15 (guild): a tabard display that shows the wearer's guild emblem (ItemDisplayInfo Flags & 1).
    bool guildTabard(u32 displayId) const;

private:
    struct Section {
        u32 record;
    };
    static u64 sectionKey(u8 race, u8 gender, u8 section, u8 variation, u8 color) {
        return (u64{race} << 32) | (u64{gender} << 24) | (u64{section} << 16) | (u64{variation} << 8) | color;
    }
    const char* sectionTexture(u8 race, u8 gender, u8 section, u8 variation, u8 color, u32 index) const;
    // The texture the hair and facial hair geosets wear (the model's texture of type 6) as WoW.exe sets it.
    const char* hairTexture(u8 race, u8 gender, u8 hairStyle, u8 hairColor, u8 facialHair) const;
    // The hair geoset (group 0) as WoW.exe shows it.
    u16 hairGeoset(u8 race, u8 gender, u8 hairStyle) const;
    // The model of a CreatureDisplayInfo record (its CreatureModelData), 0 if unknown.
    u64 displayModel(u32 record) const;
    void dress(ActorLook& look, u8 race, u8 gender, const std::array<u32, kEquipSlots>& items,
               const std::array<u8, kEquipSlots>& types, const std::array<u8, kEquipSlots>& sheaths, bool sheathed,
               bool rangedDrawn, bool composeTextures) const;
    void addWeapon(ActorLook& look, u32 display, EquipSlot slot, u8 inventoryType, u8 sheath, bool sheathed) const;
    void applyGuildEmblem(ActorLook& look, const CharacterLook& c) const; // phase 15 (guild)
    u64 itemTexture(std::string_view name, u32 regionIndex, u8 gender) const;
    u64 modelInFolder(std::string_view folder, std::string_view model, std::string_view suffix) const;
    u64 textureInFolder(std::string_view folder, std::string_view texture) const;
    std::string modelDirectory(u32 modelData) const;

    const pack::PackReader* m_textures = nullptr;
    const pack::PackReader* m_actors = nullptr;
    db::DbcView m_charSections;
    db::DbcView m_hairGeosets;
    db::DbcView m_facialHair;
    db::DbcView m_races;
    db::DbcView m_displays;
    db::DbcView m_displayExtra;
    db::DbcView m_modelData;
    db::DbcView m_itemDisplays;
    db::DbcView m_helmetVis;
    std::unordered_map<u64, u32> m_sections;      // sectionKey -> CharSections record
    std::unordered_map<u32, u32> m_hairGeoset;    // race << 16 | gender << 8 | style -> CharHairGeosets geoset
    std::unordered_map<u32, u32> m_facialGeosets; // race << 16 | gender << 8 | style -> record
    std::unordered_map<u32, u32> m_displayRecord;
    std::unordered_map<u32, u32> m_extraRecord;
    std::unordered_map<u32, u32> m_modelRecord;
    std::unordered_map<u32, u32> m_itemRecord;
    std::unordered_map<u32, u32> m_helmetRecord;
    std::array<u32, 16> m_raceRecord{}; // by race id, ~0u = none
    std::array<std::vector<u32>, kEquipSlots> m_npcItems;
    std::vector<u32> m_weapons;
    std::vector<u32> m_shields;
    bool m_ready = false;
};

} // namespace vd::game
