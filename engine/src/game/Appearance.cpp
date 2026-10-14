#include "game/Appearance.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <cstdio>

namespace vd::game {
namespace {

// Field indices of the 5875 tables (checked against the data, see docs/FORMATS.md).
namespace charsections {
constexpr u32 kRace = 1, kGender = 2, kSection = 3, kVariation = 4, kColor = 5, kTexture = 6;
} // namespace charsections
namespace hairgeosets {
constexpr u32 kRace = 1, kGender = 2, kVariation = 3, kGeoset = 4; // 5 ShowScalp: WoW.exe never reads it
} // namespace hairgeosets
namespace facialhair {
constexpr u32 kRace = 0, kGender = 1, kVariation = 2, kGeoset100 = 6, kGeoset300 = 7, kGeoset200 = 8;
} // namespace facialhair
namespace races {
constexpr u32 kFlags = 1, kMaleDisplay = 4, kFemaleDisplay = 5, kPrefix = 6;
constexpr u32 kBaldUsesFirstHair = 0x8; // a bald male's hair texture is hair style 1's (all but the night elves)
} // namespace races
namespace displays {
constexpr u32 kModel = 1, kExtra = 3, kScale = 4, kTexture = 6;
} // namespace displays
namespace extra {
constexpr u32 kRace = 1, kGender = 2, kSkin = 3, kFace = 4, kHairStyle = 5, kHairColor = 6, kFacialHair = 7,
              kItems = 8, kBakeName = 18;
} // namespace extra
namespace modeldata {
constexpr u32 kPath = 2, kScale = 4;
} // namespace modeldata
namespace items {
constexpr u32 kModel = 1, kModelTexture = 3, kGeosetGroup = 6, kFlags = 9, kHelmetVis = 12, kTextures = 14;
} // namespace items
namespace helmetvis {
constexpr u32 kHair = 1, kFacial1 = 2, kFacial2 = 3, kFacial3 = 4, kEars = 5;
} // namespace helmetvis

// CharSections base sections.
constexpr u8 kSectionSkin = 0;
constexpr u8 kSectionFace = 1;
constexpr u8 kSectionFacialHair = 2;
constexpr u8 kSectionHair = 3;
constexpr u8 kSectionUnderwear = 4;

// CreatureDisplayInfoExtra item columns, in equipment-slot terms.
constexpr EquipSlot kExtraSlots[10] = {kSlotHead,  kSlotShoulders, kSlotShirt, kSlotChest, kSlotWaist,
                                       kSlotLegs,  kSlotFeet,      kSlotWrists, kSlotHands, kSlotTabard};

// ItemDisplayInfo texture columns -> skin regions and their Item\TextureComponents folders.
constexpr SkinRegion kItemRegions[8] = {SkinRegion::ArmUpper,   SkinRegion::ArmLower,   SkinRegion::Hand,
                                        SkinRegion::TorsoUpper, SkinRegion::TorsoLower, SkinRegion::LegUpper,
                                        SkinRegion::LegLower,   SkinRegion::Foot};
constexpr const char* kItemRegionFolders[8] = {"ArmUpperTexture",   "ArmLowerTexture",   "HandTexture",
                                               "TorsoUpperTexture", "TorsoLowerTexture", "LegUpperTexture",
                                               "LegLowerTexture",   "FootTexture"};

// Drawing order of each slot's textures over the skin (later = on top).
u8 layerOrder(EquipSlot slot) {
    switch (slot) {
    case kSlotShirt: return 10;
    case kSlotLegs: return 11;
    case kSlotFeet: return 12;
    case kSlotChest: return 13;
    case kSlotWrists: return 14;
    case kSlotHands: return 15;
    case kSlotTabard: return 16;
    case kSlotWaist: return 17;
    default: return 18;
    }
}

std::string_view stem(std::string_view path) {
    const usize slash = path.find_last_of("\\/");
    if (slash != std::string_view::npos) {
        path.remove_prefix(slash + 1);
    }
    const usize dot = path.find_last_of('.');
    return dot == std::string_view::npos ? path : path.substr(0, dot);
}

std::string_view directoryOf(std::string_view path) {
    const usize slash = path.find_last_of("\\/");
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash + 1);
}

u64 hashOf(std::string_view a, std::string_view b = {}, std::string_view c = {}, std::string_view d = {}) {
    u64 hash = kFnvOffset;
    for (std::string_view part : {a, b, c, d}) {
        for (char ch : part) {
            hash ^= static_cast<u8>(normalizeAssetPathChar(ch));
            hash *= kFnvPrime;
        }
    }
    return hash;
}

bool loadTable(const pack::PackReader& pack, const char* name, db::DbcView& out) {
    const std::optional<pack::PackAsset> asset = pack.find(std::string("DBFilesClient\\") + name + ".dbc");
    if (!asset) {
        VD_LOG_WARN("Appearance: %s missing from db.vdpk", name);
        return false;
    }
    Result<db::DbcView> view = db::DbcView::parse(asset->data, asset->size);
    if (!view) {
        VD_LOG_WARN("Appearance: %s: %s", name, view.error().message.c_str());
        return false;
    }
    out = view.value();
    return true;
}

} // namespace

SkinRect skinRegionRect(SkinRegion region) {
    switch (region) {
    case SkinRegion::ArmUpper: return {0, 0, 128, 64};
    case SkinRegion::ArmLower: return {0, 64, 128, 64};
    case SkinRegion::Hand: return {0, 128, 128, 32};
    case SkinRegion::FaceUpper: return {0, 160, 128, 32};
    case SkinRegion::FaceLower: return {0, 192, 128, 64};
    case SkinRegion::TorsoUpper: return {128, 0, 128, 64};
    case SkinRegion::TorsoLower: return {128, 64, 128, 32};
    case SkinRegion::LegUpper: return {128, 96, 128, 64};
    case SkinRegion::LegLower: return {128, 160, 128, 64};
    case SkinRegion::Foot: return {128, 224, 128, 32};
    default: return {0, 0, 256, 256};
    }
}

Result<void> AppearanceDb::init(const pack::PackReader& db, const pack::PackReader& textures,
                                const pack::PackReader& actors) {
    m_textures = &textures;
    m_actors = &actors;
    bool ok = loadTable(db, "CharSections", m_charSections) && loadTable(db, "CharHairGeosets", m_hairGeosets) &&
              loadTable(db, "CharacterFacialHairStyles", m_facialHair) && loadTable(db, "ChrRaces", m_races) &&
              loadTable(db, "CreatureDisplayInfo", m_displays) &&
              loadTable(db, "CreatureDisplayInfoExtra", m_displayExtra) &&
              loadTable(db, "CreatureModelData", m_modelData) && loadTable(db, "ItemDisplayInfo", m_itemDisplays) &&
              loadTable(db, "HelmetGeosetVisData", m_helmetVis);
    if (!ok || m_itemDisplays.fieldCount() <= items::kTextures + 7 || m_displayExtra.fieldCount() <= extra::kBakeName ||
        m_charSections.fieldCount() <= charsections::kTexture + 2 || m_facialHair.fieldCount() <= facialhair::kGeoset200) {
        return Error{"Appearance tables missing or with an unexpected layout"};
    }
    for (u32 r = 0; r < m_charSections.recordCount(); ++r) {
        const auto field = [&](u32 f) { return static_cast<u8>(m_charSections.getU32(r, f)); };
        m_sections.emplace(sectionKey(field(charsections::kRace), field(charsections::kGender),
                                      field(charsections::kSection), field(charsections::kVariation),
                                      field(charsections::kColor)),
                           r);
    }
    for (u32 r = 0; r < m_hairGeosets.recordCount(); ++r) {
        const u32 key = (m_hairGeosets.getU32(r, hairgeosets::kRace) << 16) |
                        (m_hairGeosets.getU32(r, hairgeosets::kGender) << 8) |
                        m_hairGeosets.getU32(r, hairgeosets::kVariation);
        m_hairGeoset.emplace(key, m_hairGeosets.getU32(r, hairgeosets::kGeoset));
    }
    for (u32 r = 0; r < m_facialHair.recordCount(); ++r) {
        const u32 key = (m_facialHair.getU32(r, facialhair::kRace) << 16) |
                        (m_facialHair.getU32(r, facialhair::kGender) << 8) |
                        m_facialHair.getU32(r, facialhair::kVariation);
        m_facialGeosets.emplace(key, r);
    }
    m_raceRecord.fill(~0u);
    for (u32 r = 0; r < m_races.recordCount(); ++r) {
        const u32 id = m_races.getU32(r, 0);
        if (id < m_raceRecord.size()) {
            m_raceRecord[id] = r;
        }
    }
    auto index = [](const db::DbcView& table, std::unordered_map<u32, u32>& out) {
        out.reserve(table.recordCount());
        for (u32 r = 0; r < table.recordCount(); ++r) {
            out.emplace(table.getU32(r, 0), r);
        }
    };
    index(m_displays, m_displayRecord);
    index(m_displayExtra, m_extraRecord);
    index(m_modelData, m_modelRecord);
    index(m_itemDisplays, m_itemRecord);
    index(m_helmetVis, m_helmetRecord);

    // Dressing material for synthetic crowds: what NPCs wear, and every weapon / shield model.
    for (u32 r = 0; r < m_displayExtra.recordCount(); ++r) {
        for (u32 i = 0; i < 10; ++i) {
            const u32 display = m_displayExtra.getU32(r, extra::kItems + i);
            if (display != 0 && m_itemRecord.count(display)) {
                m_npcItems[kExtraSlots[i]].push_back(display);
            }
        }
    }
    for (std::vector<u32>& list : m_npcItems) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
    for (u32 r = 0; r < m_itemDisplays.recordCount(); ++r) {
        const std::string_view model = m_itemDisplays.getString(r, items::kModel);
        if (model.empty()) {
            continue;
        }
        if (modelInFolder("Weapon", model, {})) {
            m_weapons.push_back(m_itemDisplays.getU32(r, 0));
        } else if (modelInFolder("Shield", model, {})) {
            m_shields.push_back(m_itemDisplays.getU32(r, 0));
        }
    }
    m_ready = true;
    VD_LOG_INFO("Appearance: %u displays, %u items (%zu weapons, %zu shields), %zu skin sections",
                m_displays.recordCount(), m_itemDisplays.recordCount(), m_weapons.size(), m_shields.size(),
                m_sections.size());
    return {};
}

const char* AppearanceDb::sectionTexture(u8 race, u8 gender, u8 section, u8 variation, u8 color, u32 index) const {
    const auto it = m_sections.find(sectionKey(race, gender, section, variation, color));
    if (it == m_sections.end() || index >= 3) {
        return nullptr;
    }
    const std::string_view name = m_charSections.getString(it->second, charsections::kTexture + index);
    return name.empty() ? nullptr : name.data(); // NUL-terminated inside the table's string block
}

u64 AppearanceDb::modelInFolder(std::string_view folder, std::string_view model, std::string_view suffix) const {
    const u64 hash = hashOf("Item\\ObjectComponents\\", folder, "\\", std::string(stem(model)) + std::string(suffix) + ".m2");
    return m_actors && m_actors->find(hash) ? hash : 0;
}

u64 AppearanceDb::textureInFolder(std::string_view folder, std::string_view texture) const {
    if (texture.empty()) {
        return 0;
    }
    const u64 hash = hashOf("Item\\ObjectComponents\\", folder, "\\", std::string(stem(texture)) + ".blp");
    return m_textures && m_textures->find(hash) ? hash : 0;
}

u64 AppearanceDb::itemTexture(std::string_view name, u32 regionIndex, u8 gender) const {
    if (name.empty() || !m_textures) {
        return 0;
    }
    for (const char* suffix : {gender == 0 ? "_M.blp" : "_F.blp", "_U.blp"}) {
        const u64 hash = hashOf("Item\\TextureComponents\\", kItemRegionFolders[regionIndex], "\\",
                                std::string(name) + suffix);
        if (m_textures->find(hash)) {
            return hash;
        }
    }
    return 0;
}

std::string AppearanceDb::modelDirectory(u32 modelData) const {
    const auto it = m_modelRecord.find(modelData);
    return it == m_modelRecord.end() ? std::string{}
                                     : std::string(directoryOf(m_modelData.getString(it->second, modeldata::kPath)));
}

// The bald styles (style 0 of the human, orc, undead, gnome and troll males) have no hair texture in CharSections,
// yet their eyebrows, moustaches and beards are geosets that wear it. WoW.exe writes a hair style's texture only
// when it has one (SetHairTexture 0x478220), and before that SetHairColor (0x4783F0) sets style 1's for a bald male
// of a race with ChrRaces flag 0x8; SetFacialHairStyle (0x478660), last, sets style 1's again when the facial hair
// has geosets and the style lacks a scalp texture. Always the same hair color.
const char* AppearanceDb::hairTexture(u8 race, u8 gender, u8 hairStyle, u8 hairColor, u8 facialHair) const {
    const char* first = sectionTexture(race, gender, kSectionHair, 1, hairColor, 0);
    const char* texture = nullptr;
    const bool flagged = race < m_raceRecord.size() && m_raceRecord[race] != ~0u &&
                         (m_races.getU32(m_raceRecord[race], races::kFlags) & races::kBaldUsesFirstHair) != 0;
    if (gender == 0 && hairStyle == 0 && flagged) {
        texture = first;
    }
    if (const char* own = sectionTexture(race, gender, kSectionHair, hairStyle, hairColor, 0)) {
        texture = own;
    }
    const auto facial = m_facialGeosets.find((u32{race} << 16) | (u32{gender} << 8) | facialHair);
    const bool facialGeosets = facial != m_facialGeosets.end() &&
                               (m_facialHair.getU32(facial->second, facialhair::kGeoset100) != 0 ||
                                m_facialHair.getU32(facial->second, facialhair::kGeoset300) != 0 ||
                                m_facialHair.getU32(facial->second, facialhair::kGeoset200) != 0);
    const bool scalp = sectionTexture(race, gender, kSectionHair, hairStyle, hairColor, 1) &&
                       sectionTexture(race, gender, kSectionHair, hairStyle, hairColor, 2);
    if (facialGeosets && !scalp && first) {
        texture = first;
    }
    return texture;
}

// WoW.exe GetHairGeoset (0x478540): the style's CharHairGeosets geoset, or 1 (the bald scalp, which the body lacks)
// when it is 0 or the style has no row. Always shown (UpdateGeosets 0x477520).
u16 AppearanceDb::hairGeoset(u8 race, u8 gender, u8 hairStyle) const {
    const auto hair = m_hairGeoset.find((u32{race} << 16) | (u32{gender} << 8) | hairStyle);
    return hair != m_hairGeoset.end() && hair->second > 0 ? static_cast<u16>(hair->second) : u16{1};
}

u64 AppearanceDb::raceModel(u8 race, u8 gender) const {
    if (race >= m_raceRecord.size() || m_raceRecord[race] == ~0u) {
        return 0;
    }
    const u32 display = m_races.getU32(m_raceRecord[race], gender == 0 ? races::kMaleDisplay : races::kFemaleDisplay);
    const auto d = m_displayRecord.find(display);
    return d == m_displayRecord.end() ? 0 : displayModel(d->second);
}

u64 AppearanceDb::displayModel(u32 record) const {
    const auto m = m_modelRecord.find(m_displays.getU32(record, displays::kModel));
    if (m == m_modelRecord.end()) {
        return 0;
    }
    const std::string_view path = m_modelData.getString(m->second, modeldata::kPath);
    return hashOf(directoryOf(path), stem(path), ".m2");
}

f32 AppearanceDb::displayScale(u32 displayId) const {
    const auto d = m_displayRecord.find(displayId);
    if (d == m_displayRecord.end()) {
        return 1.0f;
    }
    f32 scale = m_displays.getF32(d->second, displays::kScale);
    const auto m = m_modelRecord.find(m_displays.getU32(d->second, displays::kModel));
    if (m != m_modelRecord.end()) {
        scale *= m_modelData.getF32(m->second, modeldata::kScale);
    }
    return scale > 0.01f && scale < 100.0f ? scale : 1.0f;
}

std::string_view AppearanceDb::itemModel(u32 displayId) const {
    const auto it = m_itemRecord.find(displayId);
    return it == m_itemRecord.end() ? std::string_view() : m_itemDisplays.getString(it->second, items::kModel);
}

void AppearanceDb::addWeapon(ActorLook& look, u32 display, EquipSlot slot, u8 inventoryType, u8 sheath,
                             bool sheathed) const {
    const auto it = m_itemRecord.find(display);
    if (it == m_itemRecord.end()) {
        return;
    }
    const std::string_view model = m_itemDisplays.getString(it->second, items::kModel);
    const std::string_view texture = m_itemDisplays.getString(it->second, items::kModelTexture);
    const char* folder = "Weapon";
    u64 hash = modelInFolder(folder, model, {});
    if (!hash) {
        folder = "Shield";
        hash = modelInFolder(folder, model, {});
    }
    if (!hash) {
        return;
    }
    const bool shield = inventoryType == kInventoryShield || std::string_view(folder) == "Shield";
    // Phase 13: the ranged slot as WoW.exe attaches it (0x47A0C0): bows in the left hand, the "right" ranged weapons
    // (guns, crossbows, wands; thrown ones) in the right; sheathed, only where its item has a sheath type (no 1.12
    // ranged item has one, so they are not shown then).
    const bool ranged = slot == kSlotRanged;
    const bool rightHanded = ranged ? (inventoryType == kInventoryRangedRight || inventoryType == kInventoryThrown)
                                    : slot != kSlotOffHand;
    if (sheath == 0 && !ranged) { // unknown: from the inventory type (2H 17, shields 14, the rest one-handed)
        sheath = shield ? 4 : (inventoryType == 17 ? 1 : 3);
    }
    u32 point = 0;
    if (!sheathed) {
        point = shield ? attach::kShield : (rightHanded ? attach::kHandRight : attach::kHandLeft);
    } else if (shield || sheath == 4) {
        point = attach::kSheathShield;
    } else if (sheath == 3) { // one-handers on the hips
        point = rightHanded ? attach::kHipLeft : attach::kHipRight;
    } else if (sheath == 2) { // staves across the back (0x47A070: 30 / 31)
        point = rightHanded ? attach::kLargeWeaponLeft : attach::kLargeWeaponRight;
    } else if (sheath == 1) { // two-handers across the back
        point = rightHanded ? attach::kSheathMainHand : attach::kSheathOffHand;
    } else {
        return; // a sheathed ranged weapon without a sheath type
    }
    look.attachments.push_back({point, hash, textureInFolder(folder, texture), static_cast<u8>(slot)});
}

void AppearanceDb::dress(ActorLook& look, u8 race, u8 gender, const std::array<u32, kEquipSlots>& equipped,
                         const std::array<u8, kEquipSlots>& types, const std::array<u8, kEquipSlots>& sheaths,
                         bool sheathed, bool rangedDrawn, bool composeTextures) const {
    // Geoset value per group g (shown: g * 100 + value; 0 = none). Group 0 is the hair, handled by the caller.
    std::array<u32, 20> groups{};
    for (u32& g : groups) {
        g = 1;
    }
    groups[7] = 2; // ears
    const std::vector<u16> hairAndFacial = look.geosets;
    groups[1] = groups[2] = groups[3] = 0;
    for (u16 id : hairAndFacial) {
        if (id >= 100 && id < 400) {
            groups[id / 100] = id % 100;
        }
    }
    bool hideHair = false;
    const std::string_view prefix =
        race < m_raceRecord.size() && m_raceRecord[race] != ~0u ? m_races.getString(m_raceRecord[race], races::kPrefix)
                                                                : std::string_view{};
    for (u32 s = 0; s < kEquipSlots; ++s) {
        const u32 display = equipped[s];
        const auto it = display ? m_itemRecord.find(display) : m_itemRecord.end();
        if (it == m_itemRecord.end()) {
            continue;
        }
        const u32 record = it->second;
        const auto slot = static_cast<EquipSlot>(s);
        const u32 g0 = m_itemDisplays.getU32(record, items::kGeosetGroup);
        const u32 g1 = m_itemDisplays.getU32(record, items::kGeosetGroup + 1);
        const u32 g2 = m_itemDisplays.getU32(record, items::kGeosetGroup + 2);
        switch (slot) {
        case kSlotHead: {
            const std::string_view model = m_itemDisplays.getString(record, items::kModel);
            const std::string suffix = "_" + std::string(prefix) + (gender == 0 ? "M" : "F");
            if (const u64 hash = modelInFolder("Head", model, suffix)) {
                look.attachments.push_back({attach::kHelm, hash,
                                            textureInFolder("Head", m_itemDisplays.getString(record, items::kModelTexture))});
            }
            const auto vis = m_helmetRecord.find(m_itemDisplays.getU32(record, items::kHelmetVis + (gender ? 1 : 0)));
            if (vis != m_helmetRecord.end() && race < 32) {
                const auto hides = [&](u32 field) { return ((m_helmetVis.getU32(vis->second, field) >> race) & 1u) != 0; };
                // WoW.exe 0x4799A0: the hair hidden is the bald scalp (geoset 1), the facial hair 101 / 201 / 301.
                hideHair = hides(helmetvis::kHair);
                if (hides(helmetvis::kFacial1)) groups[1] = 1;
                if (hides(helmetvis::kFacial2)) groups[2] = 1;
                if (hides(helmetvis::kFacial3)) groups[3] = 1;
                if (hides(helmetvis::kEars)) groups[7] = 1;
            }
            break;
        }
        case kSlotShoulders: {
            for (u32 side = 0; side < 2; ++side) {
                const std::string_view model = m_itemDisplays.getString(record, items::kModel + side);
                if (const u64 hash = modelInFolder("Shoulder", model, {})) {
                    look.attachments.push_back(
                        {side == 0 ? attach::kShoulderLeft : attach::kShoulderRight, hash,
                         textureInFolder("Shoulder", m_itemDisplays.getString(record, items::kModelTexture + side))});
                }
            }
            break;
        }
        case kSlotShirt:
        case kSlotChest:
            if (g0) groups[8] = 1 + g0;
            if (g1) groups[10] = 1 + g1;
            if (g2) groups[13] = 1 + g2;
            break;
        case kSlotLegs:
            if (g0) groups[11] = 1 + g0;
            if (g1) groups[9] = 1 + g1;
            if (g2) groups[13] = std::max(groups[13], 1 + g2);
            break;
        case kSlotFeet: groups[5] = 1 + g0; break;
        case kSlotHands: groups[4] = 1 + g0; break;
        case kSlotTabard: groups[12] = 1 + std::max(g0, 1u); break;
        case kSlotBack:
            groups[15] = 1 + g0;
            look.textures[2] = textureInFolder("Cape", m_itemDisplays.getString(record, items::kModelTexture));
            break;
        case kSlotMainHand:
        case kSlotOffHand: addWeapon(look, display, slot, types[s], sheaths[s], sheathed || rangedDrawn); break;
        case kSlotRanged: addWeapon(look, display, slot, types[s], sheaths[s], !rangedDrawn); break; // phase 13
        default: break;
        }
        if (composeTextures) {
            for (u32 region = 0; region < 8; ++region) {
                const u64 texture = itemTexture(m_itemDisplays.getString(record, items::kTextures + region), region, gender);
                if (texture) {
                    look.skin.push_back({texture, kItemRegions[region], layerOrder(slot)});
                }
            }
        }
    }
    look.geosets.clear();
    look.geosets.push_back(0);
    for (u16 id : hairAndFacial) {
        if (id > 0 && id < 100) {
            look.geosets.push_back(hideHair ? u16{1} : id);
        }
    }
    for (u32 g = 1; g < groups.size(); ++g) {
        if (groups[g] > 0) {
            look.geosets.push_back(static_cast<u16>(g * 100 + groups[g]));
        }
    }
    std::sort(look.geosets.begin(), look.geosets.end());
}

bool AppearanceDb::guildTabard(u32 displayId) const {
    const auto it = displayId != 0 ? m_itemRecord.find(displayId) : m_itemRecord.end();
    return it != m_itemRecord.end() && (m_itemDisplays.getU32(it->second, items::kFlags) & kGuildTabardDisplayFlag) != 0;
}

void AppearanceDb::applyGuildEmblem(ActorLook& look, const CharacterLook& c) const {
    // Phase 15 (guild): a guild tabard on a member of a guild with a design (WoW.exe 0x5D6485 -> 0x47A610): the
    // background, the border and the emblem take the tabard's own place on the upper and lower torso, in that order.
    if (!c.emblem.designed() || !guildTabard(c.items[kSlotTabard]) || !m_textures) {
        return;
    }
    const u8 order = layerOrder(kSlotTabard);
    std::erase_if(look.skin, [&](const SkinLayer& l) {
        return l.order == order && (l.region == SkinRegion::TorsoUpper || l.region == SkinRegion::TorsoLower);
    });
    for (const bool upper : {true, false}) {
        const EmblemTextures t = emblemTextures(c.emblem, upper);
        const SkinRegion region = upper ? SkinRegion::TorsoUpper : SkinRegion::TorsoLower;
        for (const u64 texture : {t.background, t.border, t.emblem}) {
            if (texture != 0 && m_textures->find(texture)) {
                look.skin.push_back({texture, region, order});
            }
        }
    }
}

ActorLook AppearanceDb::character(const CharacterLook& c, bool sheathed, bool rangedDrawn) const {
    ActorLook look;
    if (!m_ready) {
        return look;
    }
    look.model = raceModel(c.race, c.gender);
    auto layer = [&](const char* texture, SkinRegion region, u8 order) {
        if (texture) {
            look.skin.push_back({assetPathHash(texture), region, order});
        }
    };
    layer(sectionTexture(c.race, c.gender, kSectionSkin, 0, c.skin, 0), SkinRegion::Base, 0);
    if (const char* fur = sectionTexture(c.race, c.gender, kSectionSkin, 0, c.skin, 1)) {
        look.textures[4] = assetPathHash(fur);
    }
    layer(sectionTexture(c.race, c.gender, kSectionFace, c.face, c.skin, 0), SkinRegion::FaceLower, 1);
    layer(sectionTexture(c.race, c.gender, kSectionFace, c.face, c.skin, 1), SkinRegion::FaceUpper, 1);
    layer(sectionTexture(c.race, c.gender, kSectionFacialHair, c.facialHair, c.hairColor, 0), SkinRegion::FaceLower, 2);
    layer(sectionTexture(c.race, c.gender, kSectionFacialHair, c.facialHair, c.hairColor, 1), SkinRegion::FaceUpper, 2);
    layer(sectionTexture(c.race, c.gender, kSectionHair, c.hairStyle, c.hairColor, 1), SkinRegion::FaceLower, 3);
    layer(sectionTexture(c.race, c.gender, kSectionHair, c.hairStyle, c.hairColor, 2), SkinRegion::FaceUpper, 3);
    layer(sectionTexture(c.race, c.gender, kSectionUnderwear, 0, c.skin, 0), SkinRegion::LegUpper, 4);
    layer(sectionTexture(c.race, c.gender, kSectionUnderwear, 0, c.skin, 1), SkinRegion::TorsoUpper, 4);
    if (const char* hair = hairTexture(c.race, c.gender, c.hairStyle, c.hairColor, c.facialHair)) {
        look.textures[3] = assetPathHash(hair);
    }
    // Hair and facial hair geosets, then the equipment.
    look.geosets.push_back(hairGeoset(c.race, c.gender, c.hairStyle));
    if (const auto facial = m_facialGeosets.find((u32{c.race} << 16) | (u32{c.gender} << 8) | c.facialHair);
        facial != m_facialGeosets.end()) {
        look.geosets.push_back(static_cast<u16>(100 + m_facialHair.getU32(facial->second, facialhair::kGeoset100)));
        look.geosets.push_back(static_cast<u16>(300 + m_facialHair.getU32(facial->second, facialhair::kGeoset300)));
        look.geosets.push_back(static_cast<u16>(200 + m_facialHair.getU32(facial->second, facialhair::kGeoset200)));
    }
    dress(look, c.race, c.gender, c.items, c.inventoryTypes, c.sheaths, sheathed, rangedDrawn, true);
    applyGuildEmblem(look, c); // phase 15 (guild)
    std::stable_sort(look.skin.begin(), look.skin.end(),
                     [](const SkinLayer& a, const SkinLayer& b) { return a.order < b.order; });
    return look;
}

ActorLook AppearanceDb::creature(u32 displayId, const std::array<u32, 3>& weapons, const std::array<u8, 3>& weaponTypes,
                                 bool sheathed, bool rangedDrawn) const {
    ActorLook look;
    const auto d = m_ready ? m_displayRecord.find(displayId) : m_displayRecord.end();
    if (d == m_displayRecord.end()) {
        return look;
    }
    const u32 record = d->second;
    look.scale = displayScale(displayId);
    const u32 extraId = m_displays.getU32(record, displays::kExtra);
    const auto e = extraId ? m_extraRecord.find(extraId) : m_extraRecord.end();
    if (e != m_extraRecord.end()) {
        // A humanoid NPC: a race model with a pre-composited ("baked") skin, hair and gear from the extra record. The
        // model is the display's own, as any creature's: ChrRaces' goblin is the creature Goblin.mdx, skinned by
        // texture variations (type 11) a baked NPC has none of, while the 312 goblin NPC displays use
        // Character\Goblin\<gender>\Goblin<gender>.mdx, whose body texture (type 1) is the baked one; a few others
        // put an extra record of one race on another race's model.
        const u64 ownModel = displayModel(record);
        const u32 x = e->second;
        const auto field = [&](u32 f) { return static_cast<u8>(m_displayExtra.getU32(x, f)); };
        CharacterLook c;
        c.race = field(extra::kRace);
        c.gender = field(extra::kGender);
        c.skin = field(extra::kSkin);
        c.face = field(extra::kFace);
        c.hairStyle = field(extra::kHairStyle);
        c.hairColor = field(extra::kHairColor);
        c.facialHair = field(extra::kFacialHair);
        for (u32 i = 0; i < 10; ++i) {
            c.items[kExtraSlots[i]] = m_displayExtra.getU32(x, extra::kItems + i);
        }
        const std::string_view bake = m_displayExtra.getString(x, extra::kBakeName);
        if (bake.empty()) {
            look = character(c, sheathed, rangedDrawn); // composed like a player
            look.model = ownModel != 0 ? ownModel : look.model;
        } else {
            look.model = ownModel != 0 ? ownModel : raceModel(c.race, c.gender);
            look.textures[1] = hashOf("Textures\\BakedNpcTextures\\", bake);
            // As a player's. (WoW.exe never loads a baked NPC's scalp textures, so its facial hair rule may take
            // style 1's texture more often; not followed through the creature path yet.)
            if (const char* hair = hairTexture(c.race, c.gender, c.hairStyle, c.hairColor, c.facialHair)) {
                look.textures[3] = assetPathHash(hair);
            }
            if (const char* fur = sectionTexture(c.race, c.gender, kSectionSkin, 0, c.skin, 1)) {
                look.textures[4] = assetPathHash(fur);
            }
            look.geosets.push_back(hairGeoset(c.race, c.gender, c.hairStyle));
            if (const auto facial = m_facialGeosets.find((u32{c.race} << 16) | (u32{c.gender} << 8) | c.facialHair);
                facial != m_facialGeosets.end()) {
                look.geosets.push_back(static_cast<u16>(100 + m_facialHair.getU32(facial->second, facialhair::kGeoset100)));
                look.geosets.push_back(static_cast<u16>(300 + m_facialHair.getU32(facial->second, facialhair::kGeoset300)));
                look.geosets.push_back(static_cast<u16>(200 + m_facialHair.getU32(facial->second, facialhair::kGeoset200)));
            }
            dress(look, c.race, c.gender, c.items, {}, {}, sheathed, rangedDrawn, false);
        }
        look.scale = displayScale(displayId);
    } else {
        const auto m = m_modelRecord.find(m_displays.getU32(record, displays::kModel));
        if (m == m_modelRecord.end()) {
            return look;
        }
        const std::string_view path = m_modelData.getString(m->second, modeldata::kPath);
        look.model = displayModel(record);
        const std::string_view directory = directoryOf(path);
        for (u32 i = 0; i < 3; ++i) {
            const std::string_view texture = m_displays.getString(record, displays::kTexture + i);
            if (!texture.empty()) {
                look.textures[5 + i] = hashOf(directory, texture, ".blp");
            }
        }
    }
    const EquipSlot weaponSlots[3] = {kSlotMainHand, kSlotOffHand, kSlotRanged};
    for (u32 i = 0; i < 3; ++i) { // phase 13: the ranged one only drawn (sheath state 2)
        if (weapons[i] != 0) {
            addWeapon(look, weapons[i], weaponSlots[i], weaponTypes[i], 0,
                      i == 2 ? !rangedDrawn : sheathed || rangedDrawn);
        }
    }
    return look;
}

u32 AppearanceDb::skinCount(u8 race, u8 gender) const {
    u32 count = 0;
    while (count < 32 && m_sections.count(sectionKey(race, gender, kSectionSkin, 0, static_cast<u8>(count)))) {
        ++count;
    }
    return count;
}

u32 AppearanceDb::faceCount(u8 race, u8 gender, u8 skin) const {
    u32 count = 0;
    while (count < 32 && m_sections.count(sectionKey(race, gender, kSectionFace, static_cast<u8>(count), skin))) {
        ++count;
    }
    return count;
}

u32 AppearanceDb::hairStyleCount(u8 race, u8 gender) const {
    u32 count = 0;
    while (count < 32 && m_hairGeoset.count((u32{race} << 16) | (u32{gender} << 8) | count)) {
        ++count;
    }
    return count;
}

u32 AppearanceDb::hairColorCount(u8 race, u8 gender) const {
    u32 count = 0;
    while (count < 32 && m_sections.count(sectionKey(race, gender, kSectionHair, 1, static_cast<u8>(count)))) {
        ++count;
    }
    return count;
}

u32 AppearanceDb::facialHairCount(u8 race, u8 gender) const {
    u32 count = 0;
    while (count < 32 && m_facialGeosets.count((u32{race} << 16) | (u32{gender} << 8) | count)) {
        ++count;
    }
    return count;
}

const std::vector<u32>& AppearanceDb::npcItems(EquipSlot slot) const {
    return m_npcItems[slot];
}

} // namespace vd::game
