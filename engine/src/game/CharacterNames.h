#pragma once

#include "core/Types.h"

#include <string_view>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// Names of the playable races and classes in the client's language: ChrRaces.dbc and ChrClasses.dbc (m_name_lang,
// 1.12.1 layout as vMaNGOS' DBCfmt.h; 1.12 has one name for both genders), and of the creature types
// (CreatureType.dbc). Process-wide like core/Text: loaded once
// at startup on the main thread, read-only afterwards (any thread). Empty until loaded, and for ids the tables lack.
bool loadCharacterNames(const pack::PackReader& db);
void unloadCharacterNames();

std::string_view raceName(u8 race);  // ChrRaces id: "Human", "Night Elf"...
std::string_view className(u8 cls); // ChrClasses id: "Warrior", "Mage"...
// Parity: CreatureType id (the creature query's type: 1 "Beast" .. 10 "Not specified"), as unit tooltips name
// it ("Level 10 Beast").
std::string_view creatureTypeName(u32 type);

} // namespace vd::game
