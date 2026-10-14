#include "app/GlueScene.h"

#include "net/Messages.h"

namespace vd {

GlueBackground glueBackgroundOfRace(u8 race) {
    // ChrRaces ids: 1 human, 2 orc, 3 dwarf, 4 night elf, 5 undead, 6 tauren, 7 gnome, 8 troll.
    switch (race) {
    case 1: return GlueBackground::Human;
    case 3:
    case 7: return GlueBackground::Dwarf;
    case 4: return GlueBackground::NightElf;
    case 5: return GlueBackground::Scourge;
    case 6: return GlueBackground::Tauren;
    default: return GlueBackground::Orc;
    }
}

game::CharacterLook lookOfCharacter(const net::CharacterInfo& character) {
    game::CharacterLook look;
    look.race = character.race;
    look.gender = character.gender;
    look.skin = character.skin;
    look.face = character.face;
    look.hairStyle = character.hairStyle;
    look.hairColor = character.hairColor;
    look.facialHair = character.facialHair;
    // The char enum's items are in equipment slot order (then the first bag, not worn).
    for (u32 slot = 0; slot < game::kEquipSlots; ++slot) {
        look.items[slot] = character.equipmentDisplay[slot];
        look.inventoryTypes[slot] = character.equipmentType[slot];
    }
    return look;
}

} // namespace vd
