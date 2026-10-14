#pragma once

#include "core/Types.h"
#include "db/DbcView.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// What a new character looks like (CMSG_CHAR_CREATE's fields; ChrRaces / ChrClasses ids, 0 male).
struct CharAppearance {
    u8 race = 1, cls = 1, gender = 0;
    u8 skin = 0, face = 0, hairStyle = 0, hairColor = 0, facialHair = 0;
    bool operator==(const CharAppearance&) const = default;
};

// Phase 18 (create): a row of NameGen.dbc (the names GetRandomName learns from), in lower case as WoW.exe keeps them.
struct NameGenRow {
    u8 race = 0;
    u8 sex = 0; // 0 male
    std::string name;
};

// The customization choices of the creation screen, in the order they depend on each other.
enum class AppearanceOption : u8 { Skin, Face, HairStyle, HairColor, FacialHair, Count };

// The facial hair choice's name in the original screen (ChrRaces m_facialHairCustomization / m_hairCustomization).
enum class FeatureKind : u8 { FacialHair, Piercings, Markings, Features, Hair, Tusks, Earrings, HairStyle, Horns };

// Character creation (phase 8): which races and classes go together and which customizations exist, from the
// client tables (CharBaseInfo, ChrRaces, CharSections, CharacterFacialHairStyles in db.vdpk), with exactly the
// rules vmangos-core checks (Player::ValidateAppearance: an available CharSections entry for the skin, the face
// in that skin color, the hair style in its color and — except for tauren and most females — the facial hair in
// the hair color, plus a CharacterFacialHairStyles row). Built once; queries do not allocate.
class CharCreateData {
public:
    // The playable races as the original screen lists them: the Alliance's, then the Horde's.
    static constexpr std::array<u8, 8> kRaces = {1, 3, 4, 7, 2, 5, 6, 8};
    static constexpr std::array<u8, 9> kClasses = {1, 2, 3, 4, 5, 7, 8, 9, 11};

    bool init(const pack::PackReader& db);
    bool ready() const { return m_ready; }

    static bool alliance(u8 race) { return race == 1 || race == 3 || race == 4 || race == 7; }
    // CharBaseInfo: the race can be this class.
    bool canBe(u8 race, u8 cls) const;
    // The first class the race can be (a race change keeps the class when it can).
    u8 firstClass(u8 race) const;
    FeatureKind facialHairKind(u8 race, u8 gender) const;
    FeatureKind hairKind(u8 race) const;

    // How many values an option has for this appearance, and which of them it holds (0-based).
    u32 optionCount(const CharAppearance& a, AppearanceOption option) const;
    u32 optionIndex(const CharAppearance& a, AppearanceOption option) const;
    // Moves an option to the next (+1) or previous (-1) value, wrapping, then fixes the options after it.
    void step(CharAppearance& a, AppearanceOption option, i32 direction) const;
    // Keeps what is still valid after a change of race or gender; the first valid value otherwise.
    void fix(CharAppearance& a) const;
    // What the server accepts (Player::ValidateAppearance) and the race / class pair.
    bool valid(const CharAppearance& a) const;
    // A random look for a.race / a.gender (`seed` advances).
    void randomize(CharAppearance& a, u32& seed) const;
    // What a new character of this race, class and gender starts wearing (CharStartOutfit): ItemDisplayInfo ids and
    // InventoryTypes by equipment slot (EQUIPMENT_SLOT_*, 19), for the creation preview. False without a row.
    bool startOutfit(u8 race, u8 cls, u8 gender, std::array<u32, 19>& displays, std::array<u8, 19>& types) const;

    // Phase 18 (create): the rules of WoW.exe's CharacterCreate C side (CGlueMgr's customization, the Lua API of
    // GlueXML CharacterCreate.lua).
    // GetClassesForRace (0x4706B0 builds the list): the classes the race can be in CharBaseInfo's order (by class id),
    // without the dwarf mage, which the client skips by hand. Returns how many were written.
    u32 classesOf(u8 race, std::array<u8, 8>& out) const;
    // ResetCharCustomize (0x470900, CharacterCreate_OnShow: "randomly selects a combination"): a random gender and a
    // random race of the eight (0x4702A0), a random look for them and the race's first class.
    void reset(CharAppearance& a, u32& seed) const;
    // SetSelectedRace (0x470CF0): another race takes its first class and a random look for the gender (0x4703C0);
    // the race already chosen changes nothing. Returns whether it changed.
    bool selectRace(CharAppearance& a, u8 race, u32& seed) const;
    // SetSelectedSex (0x470E30): the other gender gets a random look, the class stays; the same one changes nothing.
    bool selectGender(CharAppearance& a, u8 gender, u32& seed) const;
    // NameGen.dbc's rows in the table's order (sorted by race, then sex), for NameGenerator.
    std::span<const NameGenRow> nameGen() const { return m_nameGen; }

private:
    static u32 key(u8 race, u8 gender, u8 section, u8 variation, u8 color) {
        return (u32{race} << 24) | (u32{gender} << 20) | (u32{section} << 16) | (u32{variation} << 8) | color;
    }
    bool has(u8 race, u8 gender, u8 section, u8 variation, u8 color) const;
    bool hasFacialStyle(u8 race, u8 gender, u8 style) const;
    // The values an option can take now, ascending, into `out` (fixed: 32 at most). Returns the count.
    u32 values(const CharAppearance& a, AppearanceOption option, std::array<u8, 32>& out) const;
    static u8& field(CharAppearance& a, AppearanceOption option);
    static u8 field(const CharAppearance& a, AppearanceOption option);

    struct Outfit {
        u8 race = 0, cls = 0, gender = 0;
        std::array<u32, 12> displays{};
        std::array<u8, 12> types{};
    };
    std::vector<Outfit> m_outfits;
    std::vector<NameGenRow> m_nameGen; // phase 18 (create)
    std::vector<u32> m_sections;     // key() of every available CharSections entry, sorted
    std::vector<u32> m_facialStyles; // race << 16 | gender << 8 | style of CharacterFacialHairStyles, sorted
    std::array<u16, 16> m_classes{}; // CharBaseInfo: bit `class` set for each race
    std::array<std::array<FeatureKind, 2>, 16> m_facialKinds{};
    std::array<FeatureKind, 16> m_hairKinds{};
    bool m_ready = false;
};

// Client-side checks of a new character's name, as the original creation screen does before sending it (the
// server checks again): 2..12 letters, Latin (with accents) or Cyrillic but not both, no three equal letters in a
// row. Returns a ResponseCode (net/ResponseCodes.gen.h): CHAR_NAME_SUCCESS, or the CHAR_NAME_* that fails;
// `normalized` receives the name with its first letter capitalized and the rest in lower case, as the server
// stores it.
u8 checkCharacterName(std::string_view utf8, std::string& normalized);

// Phase 18 (create): GetRandomName, the "Randomize" button under CharacterCreate's name (enUS only: GlueLocalization.lua
// shows CharacterCreateRandomName in LocalizeFrames). WoW.exe (0x471980) draws names letter by letter from how often
// each three letters follow each other in NameGen.dbc's names of the race (counted by 0x4752E0 over "_name" in lower
// case, the last two letters of a name ending it; drawn by weight by 0x475130 into 14 bytes, the '_' dropped and the
// first letter capitalized) until one of 4 to 10 letters passes the name checks. The names it learns from are taken
// as 0x475480 takes them: from the first row of the race and sex to the last row of the race, so a male's names come
// from the race's male and female names and a female's from the female ones alone (NameGen is sorted by race, then
// sex). The sequences are counted again only when the race or the gender changes; nothing allocates after that.
class NameGenerator {
public:
    // The race and gender (0 male) the names are for.
    void select(std::span<const NameGenRow> rows, u8 race, u8 gender);
    // A new name into `out` (`seed` advances); empty without names for the race.
    std::string_view generate(u32& seed, std::array<char, 16>& out) const;
    u32 sequenceCount() const { return static_cast<u32>(m_sequences.size()); }

private:
    struct Sequence {
        std::array<char, 4> key{}; // three letters, or the last two of a name (it ends after them)
        u32 count = 0;
    };
    std::vector<Sequence> m_sequences; // sorted by key
    u8 m_race = 0, m_gender = 0xFF;
};

} // namespace vd::game
