#include "game/CharacterNames.h"

#include "core/Log.h"
#include "db/DbcView.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace vd::game {
namespace {

// The client's tables (vMaNGOS DBCfmt.h, build 5875): ChrRaces "niixiixxiiiiixixissssssssxxxx" (m_name_lang at 17),
// ChrClasses "nxxixssssssssxxix" (m_name_lang at 5). The first of the 8 locale columns is the client's language.
constexpr u32 kRaceNameField = 17;
constexpr u32 kRaceFieldCount = 29;
constexpr u32 kClassNameField = 5;
constexpr u32 kClassFieldCount = 17;
// CreatureType "nxxxxxxxxxx": the id, the 8 locale names (and their flags), the type's flags.
constexpr u32 kCreatureTypeNameField = 1;
constexpr u32 kCreatureTypeFieldCount = 11;
constexpr u32 kMaxId = 16;   // 1.12: races 1..8, classes 1..11
constexpr u32 kMaxBytes = 48; // a name in any of the client's languages

// Fixed storage (no std::string: nothing to construct, nothing allocated).
struct Name {
    std::array<char, kMaxBytes> bytes{};
    u32 length = 0;
};
std::array<Name, kMaxId> g_races{};
std::array<Name, kMaxId> g_classes{};
std::array<Name, kMaxId> g_creatureTypes{};

bool loadTable(const pack::PackReader& db, const char* name, u32 fieldCount, u32 nameField,
               std::array<Name, kMaxId>& out) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    Result<db::DbcView> view = asset ? db::DbcView::parse(asset->data, asset->size)
                                     : Result<db::DbcView>(Error{"missing from db.vdpk"});
    if (!view || view.value().fieldCount() != fieldCount) {
        VD_LOG_WARN("%s.dbc: %s", name, view ? "unexpected layout" : view.error().message.c_str());
        return false;
    }
    for (u32 r = 0; r < view.value().recordCount(); ++r) {
        const u32 id = view.value().getU32(r, 0);
        if (id < kMaxId) {
            const std::string_view text = view.value().getString(r, nameField);
            Name& n = out[id];
            n.length = static_cast<u32>(std::min<usize>(text.size(), n.bytes.size()));
            std::memcpy(n.bytes.data(), text.data(), n.length);
        }
    }
    return true;
}

std::string_view nameOf(const std::array<Name, kMaxId>& names, u8 id) {
    return id < kMaxId ? std::string_view(names[id].bytes.data(), names[id].length) : std::string_view{};
}

} // namespace

bool loadCharacterNames(const pack::PackReader& db) {
    unloadCharacterNames();
    const bool races = loadTable(db, "ChrRaces", kRaceFieldCount, kRaceNameField, g_races);
    const bool classes = loadTable(db, "ChrClasses", kClassFieldCount, kClassNameField, g_classes);
    const bool types =
        loadTable(db, "CreatureType", kCreatureTypeFieldCount, kCreatureTypeNameField, g_creatureTypes);
    return races && classes && types;
}

void unloadCharacterNames() {
    g_races = {};
    g_classes = {};
    g_creatureTypes = {};
}

std::string_view raceName(u8 race) {
    return nameOf(g_races, race);
}

std::string_view className(u8 cls) {
    return nameOf(g_classes, cls);
}

std::string_view creatureTypeName(u32 type) {
    return type < kMaxId ? nameOf(g_creatureTypes, static_cast<u8>(type)) : std::string_view{};
}

} // namespace vd::game
