#include "game/CharCreate.h"

#include "core/Log.h"
#include "net/ResponseCodes.gen.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <cstdio>

namespace vd::game {
namespace {

// CharSections (DBCfmt "diiiiixxxi"): id, race, gender, base section, variation, color, 3 textures, flags.
namespace charsections {
constexpr u32 kRace = 1, kGender = 2, kSection = 3, kVariation = 4, kColor = 5, kFlags = 9;
constexpr u32 kFlagUnavailable = 0x01; // SECTION_FLAG_UNAVAILABLE: NPC-only skins
} // namespace charsections
// CharacterFacialHairStyles ("iiixxxxxx"): race, gender, variation, geosets.
namespace facialhair {
constexpr u32 kRace = 0, kGender = 1, kVariation = 2;
}
// ChrRaces: id, flags, ..., m_facialHairCustomization[2] (26, 27), m_hairCustomization (28).
namespace races {
constexpr u32 kFlags = 1, kFacialMale = 26, kFacialFemale = 27, kHair = 28;
constexpr u32 kNotPlayable = 0x01;
} // namespace races

constexpr u8 kSectionSkin = 0;
constexpr u8 kSectionFace = 1;
constexpr u8 kSectionFacialHair = 2;
constexpr u8 kSectionHair = 3;

bool loadTable(const pack::PackReader& db, const char* name, db::DbcView& out) {
    char path[96];
    std::snprintf(path, sizeof(path), "DBFilesClient\\%s.dbc", name);
    const std::optional<pack::PackAsset> asset = db.find(path);
    if (!asset) {
        VD_LOG_WARN("Character creation: %s missing", path);
        return false;
    }
    Result<db::DbcView> view = db::DbcView::parse(asset->data, asset->size);
    if (!view) {
        VD_LOG_WARN("Character creation: %s unreadable", path);
        return false;
    }
    out = view.value();
    return true;
}

FeatureKind featureKind(std::string_view token, FeatureKind fallback) {
    struct Entry {
        std::string_view token;
        FeatureKind kind;
    };
    static constexpr Entry kKinds[] = {{"NORMAL", FeatureKind::FacialHair}, {"PIERCINGS", FeatureKind::Piercings},
                                       {"MARKINGS", FeatureKind::Markings},  {"FEATURES", FeatureKind::Features},
                                       {"HAIR", FeatureKind::Hair},          {"TUSKS", FeatureKind::Tusks},
                                       {"EARRINGS", FeatureKind::Earrings},  {"HORNS", FeatureKind::Horns}};
    for (const Entry& e : kKinds) {
        if (e.token == token) {
            return e.kind;
        }
    }
    return fallback;
}

u32 nextRandom(u32& seed) {
    // xorshift32 (never 0).
    u32 x = seed ? seed : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    seed = x;
    return x;
}

// Facial hair is not checked against CharSections for these (Player::ValidateAppearance).
bool facialSectionExcluded(u8 race, u8 gender) {
    return race == 6 || (gender == 1 && race != 4 && race != 5);
}

} // namespace

bool CharCreateData::init(const pack::PackReader& db) {
    db::DbcView sections, facial, base, chrRaces, outfits;
    if (!loadTable(db, "CharSections", sections) || !loadTable(db, "CharacterFacialHairStyles", facial) ||
        !loadTable(db, "CharBaseInfo", base) || !loadTable(db, "ChrRaces", chrRaces) ||
        sections.fieldCount() <= charsections::kFlags || facial.fieldCount() <= facialhair::kVariation ||
        chrRaces.fieldCount() <= races::kHair) {
        return false;
    }
    m_sections.clear();
    m_sections.reserve(sections.recordCount());
    for (u32 r = 0; r < sections.recordCount(); ++r) {
        if (sections.getU32(r, charsections::kFlags) & charsections::kFlagUnavailable) {
            continue;
        }
        const u32 race = sections.getU32(r, charsections::kRace), gender = sections.getU32(r, charsections::kGender);
        const u32 section = sections.getU32(r, charsections::kSection);
        const u32 variation = sections.getU32(r, charsections::kVariation);
        const u32 color = sections.getU32(r, charsections::kColor);
        if (race < 16 && gender < 2 && section < 8 && variation < 256 && color < 256) {
            m_sections.push_back(key(static_cast<u8>(race), static_cast<u8>(gender), static_cast<u8>(section),
                                     static_cast<u8>(variation), static_cast<u8>(color)));
        }
    }
    std::sort(m_sections.begin(), m_sections.end());
    m_facialStyles.clear();
    for (u32 r = 0; r < facial.recordCount(); ++r) {
        const u32 race = facial.getU32(r, facialhair::kRace), gender = facial.getU32(r, facialhair::kGender);
        const u32 style = facial.getU32(r, facialhair::kVariation);
        if (race < 16 && gender < 2 && style < 256) {
            m_facialStyles.push_back((race << 16) | (gender << 8) | style);
        }
    }
    std::sort(m_facialStyles.begin(), m_facialStyles.end());
    m_classes = {};
    for (u32 r = 0; r < base.recordCount(); ++r) {
        // Two one-byte fields per row: race, class.
        const u8 race = base.hasPackedFields() ? base.getU8(r, 0) : static_cast<u8>(base.getU32(r, 0));
        const u8 cls = base.hasPackedFields() ? base.getU8(r, 1) : static_cast<u8>(base.getU32(r, 1));
        if (race < m_classes.size() && cls < 16) {
            m_classes[race] |= static_cast<u16>(1u << cls);
        }
    }
    // 1.12's table still lists the dwarf mage, which vanilla never allowed (it came with Cataclysm).
    m_classes[3] &= static_cast<u16>(~(1u << 8));
    for (auto& kinds : m_facialKinds) {
        kinds = {FeatureKind::FacialHair, FeatureKind::FacialHair};
    }
    m_hairKinds.fill(FeatureKind::HairStyle);
    for (u32 r = 0; r < chrRaces.recordCount(); ++r) {
        const u32 race = chrRaces.getU32(r, 0);
        if (race >= m_facialKinds.size()) {
            continue;
        }
        if (chrRaces.getU32(r, races::kFlags) & races::kNotPlayable) {
            m_classes[race] = 0;
        }
        m_facialKinds[race][0] = featureKind(chrRaces.getString(r, races::kFacialMale), FeatureKind::FacialHair);
        m_facialKinds[race][1] = featureKind(chrRaces.getString(r, races::kFacialFemale), FeatureKind::FacialHair);
        m_hairKinds[race] =
            chrRaces.getString(r, races::kHair) == "HORNS" ? FeatureKind::Horns : FeatureKind::HairStyle;
    }
    // Phase 18 (create): NameGen ("isii": id, name, race, sex), the names GetRandomName learns from, in lower case
    // (0x4752E0 lowers them with _strlwr: ASCII letters only).
    db::DbcView names;
    m_nameGen.clear();
    if (loadTable(db, "NameGen", names) && names.fieldCount() >= 4) {
        m_nameGen.reserve(names.recordCount());
        for (u32 r = 0; r < names.recordCount(); ++r) {
            const u32 race = names.getU32(r, 2), sex = names.getU32(r, 3);
            NameGenRow row;
            row.race = static_cast<u8>(race < 256 ? race : 0);
            row.sex = static_cast<u8>(sex < 256 ? sex : 0);
            row.name.assign(names.getString(r, 1));
            for (char& c : row.name) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            if (!row.name.empty()) {
                m_nameGen.push_back(std::move(row));
            }
        }
    }
    // CharStartOutfit: u32 id, u8 race, class, gender, outfit, then 12 item ids, 12 display ids, 12 InventoryTypes.
    m_outfits.clear();
    if (loadTable(db, "CharStartOutfit", outfits) && outfits.recordSize() >= 152) {
        for (u32 r = 0; r < outfits.recordCount(); ++r) {
            Outfit o;
            o.race = outfits.getU8(r, 4);
            o.cls = outfits.getU8(r, 5);
            o.gender = outfits.getU8(r, 6);
            for (u32 i = 0; i < 12; ++i) {
                const i32 display = outfits.getI32(r, 14 + i);
                const i32 type = outfits.getI32(r, 26 + i);
                o.displays[i] = display > 0 ? static_cast<u32>(display) : 0;
                o.types[i] = type > 0 && type < 256 ? static_cast<u8>(type) : 0;
            }
            m_outfits.push_back(o);
        }
    }
    m_ready = !m_sections.empty() && !m_facialStyles.empty();
    VD_LOG_INFO("Character creation: %zu customizations, %zu facial styles, %zu names", m_sections.size(),
                m_facialStyles.size(), m_nameGen.size());
    return m_ready;
}

bool CharCreateData::has(u8 race, u8 gender, u8 section, u8 variation, u8 color) const {
    return std::binary_search(m_sections.begin(), m_sections.end(), key(race, gender, section, variation, color));
}

bool CharCreateData::hasFacialStyle(u8 race, u8 gender, u8 style) const {
    const u32 k = (u32{race} << 16) | (u32{gender} << 8) | style;
    return std::binary_search(m_facialStyles.begin(), m_facialStyles.end(), k);
}

bool CharCreateData::canBe(u8 race, u8 cls) const {
    return race < m_classes.size() && cls < 16 && (m_classes[race] & (1u << cls)) != 0;
}

u8 CharCreateData::firstClass(u8 race) const {
    for (const u8 cls : kClasses) {
        if (canBe(race, cls)) {
            return cls;
        }
    }
    return 1;
}

FeatureKind CharCreateData::facialHairKind(u8 race, u8 gender) const {
    return race < m_facialKinds.size() ? m_facialKinds[race][gender != 0 ? 1 : 0] : FeatureKind::FacialHair;
}

FeatureKind CharCreateData::hairKind(u8 race) const {
    return race < m_hairKinds.size() ? m_hairKinds[race] : FeatureKind::HairStyle;
}

u8& CharCreateData::field(CharAppearance& a, AppearanceOption option) {
    switch (option) {
    case AppearanceOption::Skin: return a.skin;
    case AppearanceOption::Face: return a.face;
    case AppearanceOption::HairStyle: return a.hairStyle;
    case AppearanceOption::HairColor: return a.hairColor;
    default: return a.facialHair;
    }
}

u8 CharCreateData::field(const CharAppearance& a, AppearanceOption option) {
    return field(const_cast<CharAppearance&>(a), option);
}

u32 CharCreateData::values(const CharAppearance& a, AppearanceOption option, std::array<u8, 32>& out) const {
    u32 count = 0;
    for (u32 v = 0; v < 32; ++v) {
        const u8 value = static_cast<u8>(v);
        bool ok = false;
        switch (option) {
        case AppearanceOption::Skin: ok = has(a.race, a.gender, kSectionSkin, 0, value); break;
        case AppearanceOption::Face: ok = has(a.race, a.gender, kSectionFace, value, a.skin); break;
        case AppearanceOption::HairStyle:
            for (u32 c = 0; c < 32 && !ok; ++c) {
                ok = has(a.race, a.gender, kSectionHair, value, static_cast<u8>(c));
            }
            break;
        case AppearanceOption::HairColor: ok = has(a.race, a.gender, kSectionHair, a.hairStyle, value); break;
        default:
            ok = hasFacialStyle(a.race, a.gender, value) &&
                 (facialSectionExcluded(a.race, a.gender) ||
                  has(a.race, a.gender, kSectionFacialHair, value, a.hairColor));
            break;
        }
        if (ok) {
            out[count++] = value;
        }
    }
    return count;
}

u32 CharCreateData::optionCount(const CharAppearance& a, AppearanceOption option) const {
    std::array<u8, 32> list;
    return values(a, option, list);
}

u32 CharCreateData::optionIndex(const CharAppearance& a, AppearanceOption option) const {
    std::array<u8, 32> list;
    const u32 n = values(a, option, list);
    const u8 current = field(a, option);
    for (u32 i = 0; i < n; ++i) {
        if (list[i] == current) {
            return i;
        }
    }
    return 0;
}

void CharCreateData::step(CharAppearance& a, AppearanceOption option, i32 direction) const {
    std::array<u8, 32> list;
    const u32 n = values(a, option, list);
    if (n == 0) {
        return;
    }
    const u32 index = optionIndex(a, option);
    const i32 next = (static_cast<i32>(index) + (direction < 0 ? -1 : 1) + static_cast<i32>(n)) % static_cast<i32>(n);
    field(a, option) = list[static_cast<u32>(next)];
    fix(a);
}

void CharCreateData::fix(CharAppearance& a) const {
    if (!canBe(a.race, a.cls)) {
        a.cls = firstClass(a.race);
    }
    // In dependency order: the face depends on the skin, the hair color on the style, the facial hair on both.
    for (u32 o = 0; o < static_cast<u32>(AppearanceOption::Count); ++o) {
        const AppearanceOption option = static_cast<AppearanceOption>(o);
        std::array<u8, 32> list;
        const u32 n = values(a, option, list);
        const u8 current = field(a, option);
        if (n > 0 && std::find(list.begin(), list.begin() + n, current) == list.begin() + n) {
            field(a, option) = list[0];
        }
    }
}

bool CharCreateData::valid(const CharAppearance& a) const {
    if (!canBe(a.race, a.cls) || a.gender > 1) {
        return false;
    }
    if (!has(a.race, a.gender, kSectionSkin, 0, a.skin) || !has(a.race, a.gender, kSectionFace, a.face, a.skin) ||
        !has(a.race, a.gender, kSectionHair, a.hairStyle, a.hairColor)) {
        return false;
    }
    if (!facialSectionExcluded(a.race, a.gender) &&
        !has(a.race, a.gender, kSectionFacialHair, a.facialHair, a.hairColor)) {
        return false;
    }
    return hasFacialStyle(a.race, a.gender, a.facialHair);
}

void CharCreateData::randomize(CharAppearance& a, u32& seed) const {
    for (u32 o = 0; o < static_cast<u32>(AppearanceOption::Count); ++o) {
        const AppearanceOption option = static_cast<AppearanceOption>(o);
        std::array<u8, 32> list;
        const u32 n = values(a, option, list);
        if (n > 0) {
            field(a, option) = list[nextRandom(seed) % n];
        }
    }
    fix(a);
}

bool CharCreateData::startOutfit(u8 race, u8 cls, u8 gender, std::array<u32, 19>& displays,
                                 std::array<u8, 19>& types) const {
    displays = {};
    types = {};
    for (const Outfit& o : m_outfits) {
        if (o.race != race || o.cls != cls || o.gender != gender) {
            continue;
        }
        for (u32 i = 0; i < o.displays.size(); ++i) {
            // InventoryType -> equipment slot (Player::FindEquipSlot): what shows on the model.
            i32 slot = -1;
            switch (o.types[i]) {
            case 1: slot = 0; break;            // head
            case 3: slot = 2; break;            // shoulders
            case 4: slot = 3; break;            // shirt
            case 5:
            case 20: slot = 4; break;           // chest, robe
            case 6: slot = 5; break;            // waist
            case 7: slot = 6; break;            // legs
            case 8: slot = 7; break;            // feet
            case 9: slot = 8; break;            // wrists
            case 10: slot = 9; break;           // hands
            case 16: slot = 14; break;          // back
            case 13:
            case 17:
            case 21: slot = 15; break;          // one hand, two hands, main hand
            case 14:
            case 22:
            case 23: slot = 16; break;          // shield, off hand, held
            case 15:
            case 25:
            case 26: slot = 17; break;          // ranged, thrown, wand
            case 19: slot = 18; break;          // tabard
            default: break;                     // food, bags, ammo...
            }
            if (slot >= 0 && o.displays[i] != 0 && displays[static_cast<u32>(slot)] == 0) {
                displays[static_cast<u32>(slot)] = o.displays[i];
                types[static_cast<u32>(slot)] = o.types[i];
            }
        }
        return true;
    }
    return false;
}

u32 CharCreateData::classesOf(u8 race, std::array<u8, 8>& out) const {
    // canBe already leaves the dwarf mage out (init), as 0x4706B0 skips race 3 class 8 by hand.
    u32 count = 0;
    for (const u8 cls : kClasses) {
        if (count < out.size() && canBe(race, cls)) {
            out[count++] = cls;
        }
    }
    return count;
}

void CharCreateData::reset(CharAppearance& a, u32& seed) const {
    a = CharAppearance{};
    a.gender = static_cast<u8>(nextRandom(seed) % 2u);                         // 0x4702A0: the sex first,
    a.race = kRaces[nextRandom(seed) % static_cast<u32>(kRaces.size())]; // then a race of the list
    a.cls = firstClass(a.race);                                          // 0x8380F0 = the first class
    randomize(a, seed);
}

bool CharCreateData::selectRace(CharAppearance& a, u8 race, u32& seed) const {
    if (race == a.race) {
        return false;
    }
    a.race = race;
    a.cls = firstClass(race); // 0x470F50 with index 0
    randomize(a, seed);
    return true;
}

bool CharCreateData::selectGender(CharAppearance& a, u8 gender, u32& seed) const {
    if (gender == a.gender || gender > 1) {
        return false;
    }
    a.gender = gender;
    randomize(a, seed);
    return true;
}

// -------------------------------------------------------------------------------------------------------
// Names.

namespace {

// UTF-8 -> code points; false on invalid input.
bool decode(std::string_view s, std::array<u32, 64>& out, u32& count) {
    count = 0;
    for (usize i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp = 0;
        u32 extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            return false;
        }
        if (extra > 0 && i + extra >= s.size()) {
            return false; // cut inside a sequence
        }
        for (u32 k = 1; k <= extra; ++k) {
            const u8 cc = static_cast<u8>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += extra + 1;
        if (count == out.size()) {
            return false;
        }
        out[count++] = cp;
    }
    return true;
}

void encode(u32 cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// isExtendedLatinCharacter / isCyrillicCharacter of the server (shared/Util.h).
bool isLatin(u32 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xDF) ||
           (c >= 0xE0 && c <= 0xF6) || (c >= 0xF8 && c <= 0xFE) || (c >= 0x100 && c <= 0x12F) || c == 0x1E9E;
}

bool isCyrillic(u32 c) {
    return (c >= 0x410 && c <= 0x44F) || c == 0x401 || c == 0x451;
}

u32 toLower(u32 c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7 && c != 0xDF) || (c >= 0x410 && c <= 0x42F)) {
        return c + 0x20;
    }
    if (c >= 0x100 && c <= 0x12F) {
        return c | 1u; // capital / small pairs
    }
    if (c == 0x401) {
        return 0x451;
    }
    return c;
}

u32 toUpper(u32 c) {
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x430 && c <= 0x44F)) {
        return c - 0x20;
    }
    if (c >= 0x100 && c <= 0x12F) {
        return c & ~1u;
    }
    if (c == 0x451) {
        return 0x401;
    }
    return c;
}

} // namespace

u8 checkCharacterName(std::string_view utf8, std::string& normalized) {
    using net::ResponseCode;
    normalized.clear();
    std::array<u32, 64> cps;
    u32 count = 0;
    if (!decode(utf8, cps, count)) {
        return static_cast<u8>(count == 64 ? ResponseCode::CHAR_NAME_TOO_LONG : ResponseCode::CHAR_NAME_INVALID_CHARACTER);
    }
    if (count == 0) {
        return static_cast<u8>(ResponseCode::CHAR_NAME_NO_NAME);
    }
    bool latin = false, cyrillic = false;
    for (u32 i = 0; i < count; ++i) {
        const u32 c = cps[i];
        if (c == ' ') {
            return static_cast<u8>(ResponseCode::CHAR_NAME_INVALID_SPACE);
        }
        if (c == '\'') {
            return static_cast<u8>(ResponseCode::CHAR_NAME_INVALID_APOSTROPHE);
        }
        if (isLatin(c)) {
            latin = true;
        } else if (isCyrillic(c)) {
            cyrillic = true;
        } else {
            return static_cast<u8>(ResponseCode::CHAR_NAME_INVALID_CHARACTER);
        }
    }
    if (latin && cyrillic) {
        return static_cast<u8>(ResponseCode::CHAR_NAME_MIXED_LANGUAGES);
    }
    if (count < 2) {
        return static_cast<u8>(ResponseCode::CHAR_NAME_TOO_SHORT);
    }
    if (count > 12) {
        return static_cast<u8>(ResponseCode::CHAR_NAME_TOO_LONG);
    }
    for (u32 i = 2; i < count; ++i) {
        if (toLower(cps[i]) == toLower(cps[i - 1]) && toLower(cps[i]) == toLower(cps[i - 2])) {
            return static_cast<u8>(ResponseCode::CHAR_NAME_THREE_CONSECUTIVE);
        }
    }
    for (u32 i = 0; i < count; ++i) {
        encode(i == 0 ? toUpper(cps[i]) : toLower(cps[i]), normalized);
    }
    return static_cast<u8>(ResponseCode::CHAR_NAME_SUCCESS);
}

// -------------------------------------------------------------------------------------------------------
// Phase 18 (create): random names (GetRandomName).

namespace {

constexpr u32 kNameBuffer = 14; // GetRandomName's buffer (0x471990: edx = 0xE), the terminator included
constexpr u32 kShortestName = 4, kLongestName = 10; // 0x4719B5..0x4719BD
constexpr u32 kNameAttempts = 512; // WoW.exe tries forever; a few dozen tries are the most it ever needs

} // namespace

void NameGenerator::select(std::span<const NameGenRow> rows, u8 race, u8 gender) {
    if (race == m_race && gender == m_gender) {
        return;
    }
    m_race = race;
    m_gender = gender;
    m_sequences.clear();
    // 0x475480: the rows from the first one of the race and sex while the race lasts.
    usize first = rows.size();
    for (usize i = 0; i < rows.size(); ++i) {
        if (rows[i].race == race && rows[i].sex == gender) {
            first = i;
            break;
        }
    }
    m_sequences.reserve(512);
    for (usize i = first; i < rows.size() && rows[i].race == race; ++i) {
        // 0x4752E0: "_name" (at most 63 characters), each three characters from the start; the last key holds the
        // last two letters alone (the name ends there).
        std::array<char, 64> s{};
        s[0] = '_';
        const usize n = std::min<usize>(rows[i].name.size(), s.size() - 2);
        for (usize c = 0; c < n; ++c) {
            s[c + 1] = rows[i].name[c];
        }
        const usize length = n + 1;
        for (usize at = 0; at + 1 < length; ++at) {
            Sequence seq;
            for (usize c = 0; c < 3 && at + c < length; ++c) {
                seq.key[c] = s[at + c];
            }
            bool counted = false;
            for (Sequence& other : m_sequences) {
                if (other.key == seq.key) {
                    ++other.count;
                    counted = true;
                    break;
                }
            }
            if (!counted) {
                seq.count = 1;
                m_sequences.push_back(seq);
            }
        }
    }
    // 0x4753FB: sorted by key as C strings (the comparison of bytes, unsigned).
    auto byte = [](char x, char y) { return static_cast<u8>(x) < static_cast<u8>(y); };
    std::sort(m_sequences.begin(), m_sequences.end(), [&](const Sequence& a, const Sequence& b) {
        return std::lexicographical_compare(a.key.begin(), a.key.end(), b.key.begin(), b.key.end(), byte);
    });
}

std::string_view NameGenerator::generate(u32& seed, std::array<char, 16>& out) const {
    if (m_sequences.empty()) {
        return {};
    }
    std::string normalized;
    normalized.reserve(32);
    for (u32 attempt = 0; attempt < kNameAttempts; ++attempt) {
        // 0x475130: the first three characters among the sequences that start a name ('_'), then one letter at a time
        // among those that go on from the last two, by how often they were seen, until a sequence ends the name or
        // the buffer is full.
        std::array<char, kNameBuffer> buf{};
        u32 length = 0;
        while (true) {
            u32 total = 0;
            for (const Sequence& s : m_sequences) {
                const bool next = length == 0 ? s.key[0] == '_'
                                              : s.key[0] == buf[length - 2] && s.key[1] == buf[length - 1];
                total += next ? s.count : 0u;
            }
            if (total == 0) {
                break;
            }
            u32 pick = nextRandom(seed) % total;
            const Sequence* chosen = nullptr;
            for (const Sequence& s : m_sequences) {
                const bool next = length == 0 ? s.key[0] == '_'
                                              : s.key[0] == buf[length - 2] && s.key[1] == buf[length - 1];
                if (!next) {
                    continue;
                }
                if (pick < s.count) {
                    chosen = &s;
                    break;
                }
                pick -= s.count;
            }
            if (!chosen) {
                break;
            }
            if (length == 0) {
                for (u32 c = 0; c < 3 && chosen->key[c] != 0; ++c) {
                    buf[length++] = chosen->key[c];
                }
                continue;
            }
            if (chosen->key[2] == 0) {
                break; // the name ends after these two letters
            }
            buf[length++] = chosen->key[2];
            if (length >= kNameBuffer - 1) {
                break;
            }
        }
        // 0x4752B1: the '_' dropped, the first letter in capitals; 0x4719A1: 4 to 10 letters that pass the checks.
        if (length < 2) {
            continue;
        }
        const u32 letters = length - 1;
        std::string_view name{buf.data() + 1, letters};
        if (letters < kShortestName || letters > kLongestName ||
            checkCharacterName(name, normalized) != static_cast<u8>(net::ResponseCode::CHAR_NAME_SUCCESS) ||
            normalized.size() >= out.size()) {
            continue;
        }
        for (usize c = 0; c < normalized.size(); ++c) {
            out[c] = normalized[c];
        }
        return {out.data(), normalized.size()};
    }
    return {};
}

} // namespace vd::game
